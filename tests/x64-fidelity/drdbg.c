/* drdbg: the debug registers of a thread in another process, as a debugger reads and sets them.
 *
 * drtest covers a program's own threads. A debugger, a trainer or a crash handler in another process reaches a
 * thread's DR0-DR3, DR6 and DR7 through Get/SetThreadContext with CONTEXT_DEBUG_REGISTERS too, either at a debug
 * event (the debuggee stopped by its debug port) or on a thread it suspended. The parent runs a copy of itself as the
 * child, three ways:
 *   event    debugged (DEBUG_ONLY_THIS_PROCESS). At the child's OutputDebugString the debugger sets an instruction
 *            and a data breakpoint, reads them back and continues with DBG_CONTINUE; at each EXCEPTION_SINGLE_STEP it
 *            reads Dr6 and continues past the instruction breakpoint with RF; at an illegal instruction (ud2) it skips
 *            it and moves the instruction breakpoint to another function.
 *   suspend  not debugged. The parent suspends a child thread running x64 code, sets an instruction breakpoint, reads
 *            it back, later reads it back again and adds a data breakpoint to it (as debuggers do), then clears them;
 *            then the same on a thread blocked in a wait. The child counts the hits with a vectored handler.
 *   created  created suspended: the parent sets an instruction breakpoint on the main thread before it ever ran.
 * Every check holds on x86-64 Windows (the x64-fidelity workflow); drdbg32 is the same program as a 32-bit one. */
#include "fid.h"
#include <winternl.h>

#define RF 0x10000

#ifdef _WIN64
#define CTX_IP(c) ((c)->Rip)
#define WOW64 0
#define PEB_IMAGE_BASE 0x10
#else
#define CTX_IP(c) ((c)->Eip)
#define WOW64 1
#define PEB_IMAGE_BASE 0x08
#endif

/* DR7: L0 with an instruction breakpoint in DR0, L1 with a 4-byte write breakpoint in DR1 */
#define DR7_EXEC0   0x1
#define DR7_WRITE1  (0x4 | (1u << 20) | (3u << 22))
/* How DR7 reads back (measured on Windows Server 2022 and 2025): as it was set until the thread runs with it; then a
 * 64-bit thread's has bit 10 set, as the processor gives it back. WoW64 reads it through 0xffff0155, never bit 10. */
#define DR7_RAN(v) ((v) | (WOW64 ? 0 : 0x400))

struct shared
{
    volatile LONG ready, stop;
    volatile uint64_t target, target2, watch;
    volatile DWORD main_tid, wait_tid;
    volatile LONG main_hits, wait_hits, watch_hits, other_hits, raced_hits, spins;
    volatile uint64_t last_dr6, other_ip, other_dr6, other_dr7;
    volatile LONG faults;
    volatile uint64_t fault_ip, fault_addr, fault_sp, fault_dr7, fault_eflags;
};

static struct shared *sh;
static volatile LONG watch_var;

static __attribute__((noinline)) void target_fn(void) { __asm__ volatile("nop\n\tnop\n\tnop"); }
static __attribute__((noinline)) void target_fn2(void) { __asm__ volatile("nop\n\tnop\n\tnop\n\tnop"); }

/* ---- the child ---- */

static LONG CALLBACK child_veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    DWORD tid = GetCurrentThreadId();

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    {
        /* recorded for the parent's report; the child still dies of it */
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && InterlockedIncrement(&sh->faults) == 1)
        {
            sh->fault_ip = CTX_IP(c);
            sh->fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
#ifdef _WIN64
            sh->fault_sp = c->Rsp;
#else
            sh->fault_sp = c->Esp;
#endif
            sh->fault_dr7 = c->Dr7;
            sh->fault_eflags = c->EFlags;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    sh->last_dr6 = c->Dr6;
    if (CTX_IP(c) == (uintptr_t)target_fn)
    {
        if (tid == sh->wait_tid) InterlockedIncrement(&sh->wait_hits);
        else if (tid == sh->main_tid) InterlockedIncrement(&sh->main_hits);
        else InterlockedIncrement(&sh->other_hits);
        c->EFlags |= RF;
    }
    else if (c->Dr6 & 2) InterlockedIncrement(&sh->watch_hits);
    /* a trap whose context shows registers the parent changed meanwhile (see suspend.raced) */
    else if (!(c->Dr6 & 0xf) && !(c->Dr7 & 0xff)) InterlockedIncrement(&sh->raced_hits);
    else if (InterlockedIncrement(&sh->other_hits) == 1)
    {
        sh->other_ip = CTX_IP(c);
        sh->other_dr6 = c->Dr6;
        sh->other_dr7 = c->Dr7;
    }
    /* continue without the debug registers: the parent may have changed them since the exception, and NtContinue would
     * put back the ones this context holds (on Windows too) */
    c->ContextFlags &= ~0x10;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI child_wait(void *ev)
{
    WaitForSingleObject(ev, 20000);
    target_fn();
    return 0;
}

static int child(const char *mode, const char *name)
{
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    LONG i = 0;

    if (!map || !(sh = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*sh)))) return 100;
    sh->target = (uintptr_t)target_fn;
    sh->target2 = (uintptr_t)target_fn2;
    sh->watch = (uintptr_t)&watch_var;
    sh->main_tid = GetCurrentThreadId();

    if (!strcmp(mode, "event"))
    {
        sh->ready = 1;
        OutputDebugStringA("drdbg ready");   /* the debugger sets the registers here */
        target_fn();                         /* instruction breakpoint */
        watch_var = 1;                       /* data breakpoint */
        __asm__ volatile("ud2");             /* the debugger skips it and moves DR0 to target_fn2 */
        target_fn();
        target_fn2();
        return 0;
    }
    AddVectoredExceptionHandler(1, child_veh);
    if (!strcmp(mode, "created"))
    {
        target_fn();   /* DR0 set before this thread ever ran */
        return 0;
    }
    if (!strcmp(mode, "suspend"))
    {
        char evname[64];
        HANDLE ev, t;

        sprintf(evname, "%s-go", name);
        ev = CreateEventA(NULL, TRUE, FALSE, evname);
        t = CreateThread(NULL, 0, child_wait, ev, 0, (DWORD *)&sh->wait_tid);
        sh->ready = 1;
        while (!sh->stop)
        {
            target_fn();
            watch_var = i++;
            InterlockedIncrement(&sh->spins);
        }
        WaitForSingleObject(t, 20000);
        return 0;
    }
    return 101;
}

/* ---- the parent ---- */

static char g_exe[MAX_PATH], g_name[64];

static HANDLE start_child(const char *mode, DWORD flags, PROCESS_INFORMATION *pi)
{
    char cmd[MAX_PATH + 128];
    STARTUPINFOA si = { sizeof(si) };
    HANDLE map;

    sprintf(g_name, "Local\\drdbg-%lu-%s", GetCurrentProcessId(), mode);
    map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*sh), g_name);
    sh = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*sh));
    memset((void *)sh, 0, sizeof(*sh));
    sprintf(cmd, "\"%s\" child %s %s", g_exe, mode, g_name);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, pi)) return NULL;
    return map;
}

static BOOL get_dr(HANDLE th, CONTEXT *c, DWORD flags)
{
    memset(c, 0, sizeof(*c));
    c->ContextFlags = flags;
    return GetThreadContext(th, c);
}

static BOOL set_dr(HANDLE th, uint64_t dr0, uint64_t dr1, uint64_t dr7)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = dr0;
    c.Dr1 = dr1;
    c.Dr7 = dr7;
    return SetThreadContext(th, &c);
}

static int wait_for(volatile LONG *v, LONG want, DWORD ms)
{
    DWORD start = GetTickCount();
    while (*v < want) { if (GetTickCount() - start > ms) return 0; Sleep(1); }
    return 1;
}

static void test_event(void)
{
    PROCESS_INFORMATION pi;
    DEBUG_EVENT ev;
    HANDLE map, thread = NULL;
    CONTEXT c;
    int done = 0, events = 0, exec_hits = 0, exec2_hits = 0, data_hits = 0, other_steps = 0, ud2 = 0, ready = 0;
    uint64_t exec_dr6 = 0, data_dr6 = 0, exec_addr = 0, exec_rip = 0;

    if (!(map = start_child("event", DEBUG_ONLY_THIS_PROCESS, &pi)))
    {
        fid_check(0, "event.start", "CreateProcess failed, error %lu", GetLastError());
        return;
    }
    while (!done && events++ < 1000)
    {
        DWORD status = DBG_CONTINUE;

        if (!WaitForDebugEvent(&ev, 20000))
        {
            fid_check(0, "event.loop", "no debug event within 20 s (after %d)", events);
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            thread = ev.u.CreateProcessInfo.hThread;
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case OUTPUT_DEBUG_STRING_EVENT:
            if (!sh->ready || ready) break;
            ready = 1;
            /* the child's thread, stopped in OutputDebugString */
            get_dr(thread, &c, CONTEXT_DEBUG_REGISTERS);
            fid_check(!c.Dr0 && !c.Dr1 && !c.Dr2 && !c.Dr3 && !c.Dr7, "event.clear",
                      "Dr0 %#llx Dr1 %#llx Dr7 %#llx before any was set", (unsigned long long)c.Dr0,
                      (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
            fid_check(set_dr(thread, sh->target, sh->watch, DR7_EXEC0 | DR7_WRITE1), "event.set",
                      "SetThreadContext failed, error %lu", GetLastError());
            get_dr(thread, &c, CONTEXT_DEBUG_REGISTERS);
            fid_check(c.Dr0 == sh->target && c.Dr1 == sh->watch && c.Dr7 == (DR7_EXEC0 | DR7_WRITE1),
                      "event.readback", "Dr0 %#llx Dr1 %#llx Dr7 %#llx (want %#llx %#llx %#llx)",
                      (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7,
                      (unsigned long long)sh->target, (unsigned long long)sh->watch,
                      (unsigned long long)(DR7_EXEC0 | DR7_WRITE1));
            break;
        case EXCEPTION_DEBUG_EVENT:
        {
            EXCEPTION_RECORD *r = &ev.u.Exception.ExceptionRecord;

            HANDLE t;

            /* the loader's breakpoint (a 32-bit one may come as STATUS_WX86_BREAKPOINT) */
            if (r->ExceptionCode == EXCEPTION_BREAKPOINT || r->ExceptionCode == 0x4000001f) break;
            if (!(t = OpenThread(THREAD_ALL_ACCESS, FALSE, ev.dwThreadId))) { status = DBG_EXCEPTION_NOT_HANDLED; break; }
            if (r->ExceptionCode == EXCEPTION_SINGLE_STEP)
            {
                get_dr(t, &c, CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS);
                if (CTX_IP(&c) == sh->target || CTX_IP(&c) == sh->target2)
                {
                    if (CTX_IP(&c) == sh->target2) exec2_hits++;
                    else if (!exec_hits++) { exec_dr6 = c.Dr6; exec_addr = (uintptr_t)r->ExceptionAddress; exec_rip = CTX_IP(&c); }
                    c.ContextFlags = CONTEXT_CONTROL;
                    c.EFlags |= RF;   /* run the instruction once without its breakpoint */
                    SetThreadContext(t, &c);
                }
                else if (c.Dr6 & 2) { if (!data_hits++) data_dr6 = c.Dr6; }
                else other_steps++;
                CloseHandle(t);
                break;
            }
            if (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && ev.u.Exception.dwFirstChance && !ud2++)
            {
                /* an x64 exception: skip the ud2 and move the instruction breakpoint, keeping the data one */
                get_dr(t, &c, CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS);
                CTX_IP(&c) += 2;
                c.Dr0 = sh->target2;
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(t, &c);
                CloseHandle(t);
                break;
            }
            CloseHandle(t);
            status = DBG_EXCEPTION_NOT_HANDLED;
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            done = 1;
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    WaitForSingleObject(pi.hProcess, 10000);
    fid_check(ready, "event.ready", "the child's OutputDebugString never came");
    fid_check(exec_hits == 1 && exec_addr == sh->target && exec_rip == sh->target, "event.exec",
              "instruction breakpoint: %d hits, ExceptionAddress %#llx Rip %#llx (want 1 at %#llx)", exec_hits,
              (unsigned long long)exec_addr, (unsigned long long)exec_rip, (unsigned long long)sh->target);
    fid_check(exec_dr6 == 0xffff0ff1, "event.exec.dr6", "Dr6 %#llx at the event (want 0xffff0ff1, B0)", (unsigned long long)exec_dr6);
    fid_check(data_hits == 1, "event.data", "data breakpoint: %d hits", data_hits);
    fid_check(data_dr6 == 0xffff0ff2, "event.data.dr6", "Dr6 %#llx at the event (want 0xffff0ff2, B1)", (unsigned long long)data_dr6);
    fid_check(ud2 == 1 && exec2_hits == 1, "event.moved", "after the ud2 event: %d ud2, %d hits on the moved breakpoint (want 1)", ud2, exec2_hits);
    fid_check(other_steps == 0, "event.other", "%d single steps nobody asked for", other_steps);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    UnmapViewOfFile((void *)sh);
    CloseHandle(map);
}

static void test_suspend(void)
{
    PROCESS_INFORMATION pi;
    HANDLE map, main_t, wait_t, ev;
    char evname[80];
    CONTEXT c;
    LONG hits, watch;
    DWORD exit_code = 0;

    if (!(map = start_child("suspend", 0, &pi)))
    {
        fid_check(0, "suspend.start", "CreateProcess failed, error %lu", GetLastError());
        return;
    }
    if (!wait_for(&sh->ready, 1, 10000) || !wait_for(&sh->spins, 1000, 10000))
    {
        fid_check(0, "suspend.ready", "the child did not start");
        TerminateProcess(pi.hProcess, 1);
        return;
    }
    main_t = OpenThread(THREAD_ALL_ACCESS, FALSE, sh->main_tid);
    wait_t = OpenThread(THREAD_ALL_ACCESS, FALSE, sh->wait_tid);
    sprintf(evname, "%s-go", g_name);

    /* a thread running x64 code */
    SuspendThread(main_t);
    get_dr(main_t, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(!c.Dr0 && !c.Dr1 && !c.Dr7, "suspend.clear", "Dr0 %#llx Dr1 %#llx Dr7 %#llx before any was set",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    fid_check(set_dr(main_t, sh->target, 0, DR7_EXEC0), "suspend.set", "SetThreadContext failed, error %lu", GetLastError());
    get_dr(main_t, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(c.Dr0 == sh->target && c.Dr7 == DR7_EXEC0, "suspend.readback", "Dr0 %#llx Dr7 %#llx",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    ResumeThread(main_t);
    fid_check(wait_for(&sh->main_hits, 3, 10000), "suspend.exec", "%ld hits on the instruction breakpoint after 10 s", sh->main_hits);

    /* read back while it runs into its breakpoint, add a data breakpoint to what was read (as a debugger does) */
    SuspendThread(main_t);
    get_dr(main_t, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(c.Dr0 == sh->target && c.Dr7 == DR7_RAN(DR7_EXEC0), "suspend.readback.running", "Dr0 %#llx Dr7 %#llx",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    c.Dr1 = sh->watch;
    c.Dr7 |= DR7_WRITE1;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SetThreadContext(main_t, &c);
    hits = sh->main_hits;
    ResumeThread(main_t);
    fid_check(wait_for(&sh->watch_hits, 3, 10000) && wait_for(&sh->main_hits, hits + 3, 10000), "suspend.both",
              "%ld data hits and %ld more instruction hits after adding the data breakpoint", sh->watch_hits, sh->main_hits - hits);

    /* clear them: no hit after that */
    SuspendThread(main_t);
    set_dr(main_t, 0, 0, 0);
    get_dr(main_t, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(!c.Dr0 && !c.Dr1 && !c.Dr7, "suspend.cleared", "Dr0 %#llx Dr1 %#llx Dr7 %#llx after clearing",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    ResumeThread(main_t);
    wait_for(&sh->spins, sh->spins + 1000, 5000);   /* the thread takes the cleared registers before running on */
    hits = sh->main_hits;
    watch = sh->watch_hits;
    wait_for(&sh->spins, sh->spins + 100000, 5000);
    fid_check(sh->main_hits == hits && sh->watch_hits == watch, "suspend.cleared.quiet",
              "%ld instruction and %ld data hits after clearing", sh->main_hits - hits, sh->watch_hits - watch);

    /* a thread blocked in a wait */
    SuspendThread(wait_t);
    get_dr(wait_t, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(!c.Dr0 && !c.Dr7, "suspend.wait.clear", "Dr0 %#llx Dr7 %#llx (registers are per thread)",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    set_dr(wait_t, sh->target, 0, DR7_EXEC0);
    get_dr(wait_t, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(c.Dr0 == sh->target && c.Dr7 == DR7_EXEC0, "suspend.wait.readback", "Dr0 %#llx Dr7 %#llx",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    ResumeThread(wait_t);
    ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, evname);
    SetEvent(ev);
    WaitForSingleObject(wait_t, 10000);
    fid_check(sh->wait_hits == 1, "suspend.wait.hit", "%ld hits in the thread that waited (want 1)", sh->wait_hits);

    sh->stop = 1;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    fid_check(sh->other_hits == 0 && exit_code == 0, "suspend.other",
              "%ld unexpected single steps (the first at %#llx with Dr6 %#llx Dr7 %#llx; target %#llx), exit code %lu",
              sh->other_hits, (unsigned long long)sh->other_ip, (unsigned long long)sh->other_dr6,
              (unsigned long long)sh->other_dr7, (unsigned long long)sh->target, exit_code);
    fid_info("suspend.dr6", "last Dr6 in the child's handler %#llx", (unsigned long long)sh->last_dr6);
    if (sh->faults)
        fid_info("suspend.fault", "the child faulted at %#llx on %#llx (stack %#llx, Dr7 %#llx, EFlags %#llx; target_fn %#llx, "
                 "target_fn2 %#llx, watch %#llx)", (unsigned long long)sh->fault_ip, (unsigned long long)sh->fault_addr,
                 (unsigned long long)sh->fault_sp, (unsigned long long)sh->fault_dr7, (unsigned long long)sh->fault_eflags,
                 (unsigned long long)sh->target, (unsigned long long)sh->target2, (unsigned long long)sh->watch);
    /* A trap whose exception context already shows the registers the parent cleared or changed while it was being
     * dispatched: a race, not an error, which Windows Server 2025 showed once (run 38144192216). The arm64 engine's
     * 32-bit programs show it too: Wine's wow64 reads the 32-bit context back from the thread's CPU area when it
     * dispatches the exception, after another process's SetThreadContext may have written it. */
    fid_info("suspend.raced", "%ld single steps reporting registers changed after the trap", sh->raced_hits);
    CloseHandle(ev);
    CloseHandle(main_t);
    CloseHandle(wait_t);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    UnmapViewOfFile((void *)sh);
    CloseHandle(map);
}

static void test_created(void)
{
    typedef LONG (WINAPI *NtQueryInformationProcess_t)(HANDLE, int, void *, ULONG, ULONG *);
    NtQueryInformationProcess_t pNtQueryInformationProcess =
        (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION pbi;
    PROCESS_INFORMATION pi;
    uintptr_t base = 0, target;
    HANDLE map;
    CONTEXT c;
    DWORD exit_code = 0;

    if (!(map = start_child("created", CREATE_SUSPENDED, &pi)))
    {
        fid_check(0, "created.start", "CreateProcess failed, error %lu", GetLastError());
        return;
    }
    /* the same image in the child: target_fn at the same offset from its base */
    pNtQueryInformationProcess(pi.hProcess, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), NULL);
    ReadProcessMemory(pi.hProcess, (char *)pbi.PebBaseAddress + PEB_IMAGE_BASE, &base, sizeof(base), NULL);
    target = base + ((uintptr_t)target_fn - (uintptr_t)GetModuleHandleA(NULL));
    fid_check(set_dr(pi.hThread, target, 0, DR7_EXEC0), "created.set", "SetThreadContext failed, error %lu", GetLastError());
    get_dr(pi.hThread, &c, CONTEXT_DEBUG_REGISTERS);
    fid_check(c.Dr0 == target && c.Dr7 == DR7_EXEC0, "created.readback", "Dr0 %#llx Dr7 %#llx (want %#llx)",
              (unsigned long long)c.Dr0, (unsigned long long)c.Dr7, (unsigned long long)target);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    fid_check(sh->target == target && sh->main_hits == 1 && exit_code == 0, "created.hit",
              "%ld hits at %#llx (child's target_fn %#llx), exit code %lu", sh->main_hits,
              (unsigned long long)target, (unsigned long long)sh->target, exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    UnmapViewOfFile((void *)sh);
    CloseHandle(map);
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "child")) return child(argv[2], argv[3]);

    fid_name = WOW64 ? "drdbg32" : "drdbg";
    setvbuf(stdout, NULL, _IONBF, 0);
    fid_environment();
    GetModuleFileNameA(NULL, g_exe, sizeof(g_exe));
    test_event();
    test_suspend();
    test_created();
    return fid_summary();
}
