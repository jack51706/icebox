#include "callstack.hpp"
#include "core.hpp"

#define FDP_MODULE "main"
#include "log.hpp"
#include "os.hpp"
#include "utils/pe.hpp"
#include "utils/sanitizer.hpp"

#include "plugin/syscall_tracer.hpp"

#include <chrono>
#include <thread>

namespace
{
    bool test_core(core::Core& core, pe::Pe& pe)
    {
        LOG(INFO, "drivers:");
        core.os->driver_list([&](driver_t drv)
        {
            const auto name = core.os->driver_name(drv);
            const auto span = core.os->driver_span(drv);
            LOG(INFO, "    driver: {:#x} {} {:#x} {:#x}", drv.id, name ? name->data() : "<noname>", span ? span->addr : 0, span ? span->size : 0);
            return WALK_NEXT;
        });

        const auto pc = core.os->proc_current();
        LOG(INFO, "current process: {:#x} dtb: {:#x} {}", pc->id, pc->dtb.val, core.os->proc_name(*pc)->data());

        const auto tc = core.os->thread_current();
        LOG(INFO, "current thread: {:#x}", tc->id);

        LOG(INFO, "processes:");
        core.os->proc_list([&](proc_t proc)
        {
            const auto procname = core.os->proc_name(proc);
            LOG(INFO, "proc: {:#x} {}", proc.id, procname ? procname->data() : "<noname>");
            return WALK_NEXT;
        });

        const char proc_target[] = "notepad.exe";
        LOG(INFO, "searching {}", proc_target);
        const auto target = core.os->proc_find(proc_target);
        if(!target)
            return false;

        LOG(INFO, "{}: {:#x} dtb: {:#x} {}", proc_target, target->id, target->dtb.val, core.os->proc_name(*target)->data());
        core.os->proc_join(*target, os::JOIN_ANY_MODE);
        core.os->proc_join(*target, os::JOIN_USER_MODE);

        std::vector<uint8_t> buffer;
        size_t modcount = 0;
        core.os->mod_list(*target, [&](mod_t)
        {
            ++modcount;
            return WALK_NEXT;
        });
        size_t modi = 0;
        core.os->mod_list(*target, [&](mod_t mod)
        {
            const auto name = core.os->mod_name(*target, mod);
            const auto span = core.os->mod_span(*target, mod);
            if(!name || !span)
                return WALK_NEXT;

            LOG(INFO, "module[{:>2}/{:<2}] {}: {:#x} {:#x}", modi, modcount, name->data(), span->addr, span->size);
            ++modi;

            const auto debug_dir = pe.get_directory_entry(core, target->dtb, *span, pe::pe_directory_entries_e::IMAGE_DIRECTORY_ENTRY_DEBUG);
            buffer.resize(debug_dir->size);
            auto ok = core.mem.read_virtual(&buffer[0], target->dtb, debug_dir->addr, debug_dir->size);
            if(!ok)
                return WALK_NEXT;

            const auto codeview = pe.parse_debug_dir(&buffer[0], span->addr, *debug_dir);
            buffer.resize(codeview->size);
            ok = core.mem.read_virtual(&buffer[0], target->dtb, codeview->addr, codeview->size);
            if(!ok)
                FAIL(WALK_NEXT, "Unable to read IMAGE_CODEVIEW (RSDS)");

            ok = core.sym.insert(sanitizer::sanitize_filename(*name).data(), *span, &buffer[0], buffer.size());
            if(!ok)
                return WALK_NEXT;

            return WALK_NEXT;
        });

        core.os->thread_list(*target, [&](thread_t thread)
        {
            const auto rip = core.os->thread_pc(*target, thread);
            if(!rip)
                return WALK_NEXT;

            const auto name = core.sym.find(*rip);
            LOG(INFO, "thread: {:#x} {:#x}{}", thread.id, *rip, name ? (" " + name->module + "!" + name->symbol + "+" + std::to_string(name->offset)).data() : "");
            return WALK_NEXT;
        });

        // check breakpoints
        {
            const auto ptr = core.sym.symbol("nt", "SwapContext");
            const auto bp  = core.state.set_breakpoint(*ptr, [&]
            {
                const auto rip = core.regs.read(FDP_RIP_REGISTER);
                if(!rip)
                    return;

                const auto proc     = core.os->proc_current();
                const auto pid      = core.os->proc_id(*proc);
                const auto thread   = core.os->thread_current();
                const auto tid      = core.os->thread_id(*proc, *thread);
                const auto procname = proc ? core.os->proc_name(*proc) : ext::nullopt;
                const auto sym      = core.sym.find(rip);
                LOG(INFO, "BREAK! rip: {:#x} {} {} pid:{} tid:{}",
                    rip, sym ? sym::to_string(*sym).data() : "", procname ? procname->data() : "", pid, tid);
            });
            for(size_t i = 0; i < 16; ++i)
            {
                core.state.resume();
                core.state.wait();
            }
        }

        // test callstack
        do
        {
            const auto callstack = callstack::make_callstack_nt(core, pe);
            const auto cs_depth  = 40;
            const auto pdb_name  = "ntdll";
            const auto func_name = "RtlAllocateHeap";
            const auto func_addr = core.sym.symbol(pdb_name, func_name);
            LOG(INFO, "{} = {:#x}", func_name, func_addr ? *func_addr : 0);

            const auto bp = core.state.set_breakpoint(*func_addr, *target, [&]
            {
                const auto rip = core.regs.read(FDP_RIP_REGISTER);
                const auto rsp = core.regs.read(FDP_RSP_REGISTER);
                const auto rbp = core.regs.read(FDP_RBP_REGISTER);

                int k = 0;
                callstack->get_callstack(*target, {rip, rsp, rbp}, [&](callstack::callstep_t callstep)
                {
                    auto cursor = core.sym.find(callstep.addr);
                    if(!cursor)
                        cursor = sym::Cursor{"_", "_", callstep.addr};

                    LOG(INFO, "{:>2} - {}", k, sym::to_string(*cursor).data());
                    k++;
                    if(k >= cs_depth)
                        return WALK_STOP;

                    return WALK_NEXT;
                });
                LOG(INFO, "");
            });
            for(size_t i = 0; i < 3; ++i)
            {
                core.state.resume();
                core.state.wait();
            }
        } while(0);

        // test syscall plugin
        {
            syscall_tracer::SyscallPlugin syscall_plugin(core, pe);
            syscall_plugin.setup(*target);

            LOG(INFO, "Everything is set up ! Please trigger some syscalls");

            const auto n = 100;
            for(size_t i = 0; i < n; ++i)
            {
                core.state.resume();
                core.state.wait();
            }

            syscall_plugin.generate("output.json");
        }

        return true;
    }
}

int main(int argc, char* argv[])
{
    loguru::g_preamble_uptime = false;
    loguru::g_preamble_date   = false;
    loguru::g_preamble_thread = false;
    loguru::g_preamble_file   = false;
    loguru::init(argc, argv);
    if(argc != 2)
        FAIL(-1, "usage: fdp_exec <name>");

    const auto name = std::string{argv[1]};
    LOG(INFO, "starting on {}", name.data());

    core::Core core;
    auto ok = core::setup(core, name);
    if(!ok)
        FAIL(-1, "unable to start core at {}", name.data());

    pe::Pe pe;
    ok = pe.setup(core);
    if(!ok)
        FAIL(-1, "unable to retrieve PE format informations from pdb");

    // core.state.resume();
    core.state.pause();
    const auto valid = test_core(core, pe);
    core.state.resume();
    return !valid;
}
