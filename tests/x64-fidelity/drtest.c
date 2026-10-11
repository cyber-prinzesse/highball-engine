/* drtest: the x86-64 hardware debug registers as a Windows program sees them.
 *
 * DR0-DR3 hold linear addresses, DR7 enables them (L/G bits 0-7, R/W and LEN fields from bit 16) and DR6 reports
 * which one hit (B0-B3) or a single step (BS). A program reaches them with CONTEXT_DEBUG_REGISTERS through
 * Get/SetThreadContext, the context an exception handler receives, and NtContinue. An instruction breakpoint is a
 * fault: EXCEPTION_SINGLE_STEP before the instruction runs, Rip and ExceptionAddress at it. A data breakpoint is a
 * trap: after the access, Rip at the next instruction. The processor only ignores an instruction breakpoint when
 * the resume flag (RF, EFlags bit 16) is set for that instruction, and clears RF after it (Intel SDM vol. 3 ch. 18).
 * Windows x64 behaviour not stated by the SDM (what the kernel does to RF and Dr6, which Dr7 bits survive a round
 * trip, whether a new thread inherits them) is recorded as INFO and compared across machines. Expectations are the
 * ones Wine's conformance tests verified on Windows (dlls/ntdll/tests/exception.c test_debug_registers) plus the
 * SDM; every FAIL on real x86-64 Windows is a wrong check.
 *
 * Windows 11 ARM64's x64 emulation ignores the registers (reads back 0, never fires); the arm64 engine's reference is
 * x86-64 Windows. */
#include "fid.h"

#define RF 0x10000
#define TF 0x100

/* the same program as a 32-bit one (drtest32.exe, through WoW64 on x86-64 Windows and on the arm64 engine) */
#ifdef _WIN64
#define CTX_IP(c) ((c)->Rip)
#define CTX_CX(c) ((c)->Rcx)
#else
#define CTX_IP(c) ((c)->Eip)
#define CTX_CX(c) ((c)->Ecx)
#endif

/* What Get/SetThreadContext give back differs between a 64-bit program and a 32-bit one (measured on Windows Server
 * 2022 and 2025). x64: the kernel keeps the registers only while DR7 has L0-L3, LE or GE set, reports all of them as 0
 * otherwise, and reads DR7 back with bit 10 set; GE survives. WoW64: an address set with nothing enabled gets LE, the
 * addresses always read back, and DR7 reads back through the 32-bit mask 0xffff0155 (no bit 10, no GE). */
#ifdef _WIN64
#define WOW64 0
#else
#define WOW64 1
#endif

enum { MODE_RF = 0, MODE_PLAIN, MODE_DR7_ZERO, MODE_MOVE_DR0, MODE_KEEP_DR6, MODE_RECORD_ONLY };

static volatile uint8_t *g_fix_page;
static struct
{
    int hits, cap_reached, av_hits;
    uint64_t av_eflags;
    DWORD code, flags, params;
    uint64_t addr, rip, rcx, dr0, dr1, dr6, dr7, eflags, first_dr6, tid;
    uint64_t entry_dr6;    /* what GetThreadContext says inside the handler */
} g;
static volatile int g_mode = MODE_RF;
static volatile uint64_t g_move_to;

static void get_dr(HANDLE th, CONTEXT *c)
{
    memset(c, 0xcc, sizeof(*c));
    c->ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, c)) memset(c, 0xee, sizeof(*c));
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

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    CONTEXT live;

    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_fix_page)
    {
        DWORD old;
        g.av_hits++;
        g.av_eflags = c->EFlags;
        VirtualProtect((void *)g_fix_page, 4096, PAGE_READWRITE, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_SINGLE_STEP && r->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!g.hits) g.first_dr6 = c->Dr6;
    g.hits++;
    g.code = r->ExceptionCode;
    g.flags = c->ContextFlags;
    g.params = r->NumberParameters;
    g.addr = (uint64_t)r->ExceptionAddress;
    g.rip = CTX_IP(c);
    g.rcx = CTX_CX(c);
    g.dr0 = c->Dr0;
    g.dr1 = c->Dr1;
    g.dr6 = c->Dr6;
    g.dr7 = c->Dr7;
    g.eflags = c->EFlags;
    g.tid = GetCurrentThreadId();
    get_dr(GetCurrentThread(), &live);
    g.entry_dr6 = live.Dr6;

    if (r->ExceptionCode == EXCEPTION_BREAKPOINT) { CTX_IP(c)++; return EXCEPTION_CONTINUE_EXECUTION; }
    if (c->EFlags & TF) { c->EFlags &= ~TF; return EXCEPTION_CONTINUE_EXECUTION; }

    switch (g_mode)
    {
    case MODE_RF:       c->Dr6 = 0; c->EFlags |= RF; break;
    case MODE_PLAIN:    c->Dr6 = 0; if (g.hits >= 8) { g.cap_reached = 1; c->Dr7 = 0; } break;
    case MODE_DR7_ZERO: c->Dr6 = 0; c->Dr7 = 0; break;
    case MODE_MOVE_DR0: c->Dr6 = 0; c->Dr0 = g_move_to; c->EFlags |= RF; break;
    case MODE_KEEP_DR6: c->EFlags |= RF; break;          /* leave Dr6 as the kernel gave it */
    case MODE_RECORD_ONLY: break;                        /* data breakpoints: nothing to do, the access is done */
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void arm(int mode) { memset(&g, 0, sizeof(g)); g_mode = mode; g_move_to = 0; }

/* the breakpoint targets live in one RWX page, at the same places in both programs:
 *   code+0:  nop; nop; ret                              (three one-byte instructions)
 *   code+16: mov ecx, 5; L: dec ecx; jnz L; ret        (dec at +21)
 *   code+32: store 7 to the 4 bytes at the argument     (the store at STORE4, the ret after it at RET4)
 *   code+48: load the byte at the argument              (the ret after it at RET1R)
 *   code+64: rep stosb; ret                             (rdi, rcx and rax set by stos_thunk)
 *   code+80: store the second argument, 8 bytes, at the first in one access (the ret at RET8)
 *   code+96: store 1 to the byte at the argument
 *   code+112: repe cmpsb; ret                           (rsi, rdi, rcx set by cmps_thunk) */
#ifdef _WIN64
enum { STORE4 = 32, RET4 = 38, RET1R = 50, RET8 = 86 };
static const uint8_t loop[] = { 0xb9, 0x05, 0x00, 0x00, 0x00, 0xff, 0xc9, 0x75, 0xfc, 0xc3 };   /* dec ecx is ff c9 */
static const uint8_t write4[] = { 0xc7, 0x01, 0x07, 0x00, 0x00, 0x00, 0xc3 };                  /* mov dword [rcx], 7 */
static const uint8_t read1[] = { 0x8a, 0x01, 0xc3 };                                           /* mov al, [rcx] */
static const uint8_t write8[] = { 0x48, 0x89, 0xd0, 0x48, 0x89, 0x01, 0xc3 };                  /* mov rax, rdx; mov [rcx], rax */
static const uint8_t write1[] = { 0xc6, 0x01, 0x01, 0xc3 };                                     /* mov byte [rcx], 1 */
#else
enum { STORE4 = 36, RET4 = 42, RET1R = 54, RET8 = 94 };
static const uint8_t loop[] = { 0xb9, 0x05, 0x00, 0x00, 0x00, 0x49, 0x75, 0xfd, 0xc3 };         /* dec ecx is 49 */
static const uint8_t write4[] = { 0x8b, 0x4c, 0x24, 0x04, 0xc7, 0x01, 0x07, 0x00, 0x00, 0x00, 0xc3 };
static const uint8_t read1[] = { 0x8b, 0x4c, 0x24, 0x04, 0x8a, 0x01, 0xc3 };
/* mov ecx, [esp+4]; movq xmm0, [esp+8]; movq [ecx], xmm0: one 8-byte store */
static const uint8_t write8[] = { 0x8b, 0x4c, 0x24, 0x04, 0xf3, 0x0f, 0x7e, 0x44, 0x24, 0x08, 0x66, 0x0f, 0xd6, 0x01, 0xc3 };
static const uint8_t write1[] = { 0x8b, 0x4c, 0x24, 0x04, 0xc6, 0x01, 0x01, 0xc3 };
#endif
static uint8_t *code;
static int (*fn_nops)(void);
static int (*fn_loop)(void);
static void (*fn_write4)(void *);
static int (*fn_read1)(void *);
static void (*fn_write8)(void *, uint64_t);
static void (*fn_write1)(void *);

static void make_code(void)
{
    static const uint8_t nops[] = { 0x90, 0x90, 0xc3 };
    static const uint8_t stos[] = { 0xf3, 0xaa, 0xc3 };
    static const uint8_t cmps[] = { 0xf3, 0xa6, 0xc3 };

    code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memset(code, 0xcc, 4096);
    memcpy(code + 0, nops, sizeof(nops));
    memcpy(code + 16, loop, sizeof(loop));
    memcpy(code + 32, write4, sizeof(write4));
    memcpy(code + 48, read1, sizeof(read1));
    memcpy(code + 64, stos, sizeof(stos));
    memcpy(code + 80, write8, sizeof(write8));
    memcpy(code + 96, write1, sizeof(write1));
    memcpy(code + 112, cmps, sizeof(cmps));
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    fn_nops = (void *)(code + 0);
    fn_loop = (void *)(code + 16);
    fn_write4 = (void *)(code + 32);
    fn_read1 = (void *)(code + 48);
    fn_write8 = (void *)(code + 80);
    fn_write1 = (void *)(code + 96);
}

/* rep stosb needs rdi, rcx, rax: a tiny thunk written in C with inline asm */
static void stos_thunk(void *dst, uintptr_t n, uintptr_t value)
{
    void (*f)(void) = (void *)(code + 64);
#ifdef _WIN64
    __asm__ volatile("call *%3" : "+D"(dst), "+c"(n), "+a"(value) : "r"(f) : "memory", "cc", "rdx", "r8", "r9", "r10", "r11");
#else
    __asm__ volatile("call *%3" : "+D"(dst), "+c"(n), "+a"(value) : "r"(f) : "memory", "cc", "edx");
#endif
}

/* repe cmpsb over n bytes; returns rcx left and the flags after it */
static uintptr_t cmps_thunk(const void *a, const void *b, uintptr_t n, uintptr_t *flags)
{
    void (*f)(void) = (void *)(code + 112);
    uintptr_t fl;
#ifdef _WIN64
    __asm__ volatile("call *%4; pushf; pop %3" : "+S"(a), "+D"(b), "+c"(n), "=r"(fl) : "r"(f) : "memory", "cc", "rax", "rdx", "r8", "r9", "r10", "r11");
#else
    __asm__ volatile("call *%4; pushf; pop %3" : "+S"(a), "+D"(b), "+c"(n), "=r"(fl) : "r"(f) : "memory", "cc", "eax", "edx");
#endif
    *flags = fl;
    return n;
}

static DWORD WINAPI wait_loop(void *p)
{
    HANDLE ev = p;
    WaitForSingleObject(ev, 10000);
    fn_nops();
    return 0;
}

/* DR7 field helpers: n = register, rw: 0 execute, 1 write, 3 read/write; len: 0 1 byte, 1 2 bytes, 3 4 bytes, 2 8 bytes */
static uint64_t dr7(int n, int rw, int len) { return (1ULL << (2 * n)) | ((uint64_t)rw << (16 + 4 * n)) | ((uint64_t)len << (18 + 4 * n)); }

static DWORD WINAPI other_thread(void *p)
{
    int i;
    for (i = 0; i < 3; i++) fn_nops();
    return 0;
}

static DWORD WINAPI wait_then_run(void *p)
{
    WaitForSingleObject((HANDLE)p, 10000);
    fn_nops();
    return 0;
}

static DWORD WINAPI report_dr(void *p)
{
    CONTEXT *c = p;
    get_dr(GetCurrentThread(), c);
    return 0;
}

static DWORD WINAPI write_other(void *p)
{
    fn_write4(p);
    return 0;
}

static HANDLE run_thread(LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags)
{
    return CreateThread(NULL, 0, fn, arg, flags, NULL);
}

int main(void)
{
    HANDLE self = GetCurrentThread();
    CONTEXT c;
    uintptr_t f;
    DECLSPEC_ALIGN(16) static uint8_t buf[64];
    BOOL r;

    fid_name = "drtest";
    setvbuf(stdout, NULL, _IONBF, 0);
    fid_environment();
    make_code();
    AddVectoredExceptionHandler(1, veh);

    /* ---- round trip through Get/SetThreadContext on the current thread (Wine's test_debug_registers values) ---- */
    r = set_dr(self, 0x42424240, 0x126bb070, 0xffff0115);
    fid_check(r, "rt.set", "SetThreadContext(CONTEXT_DEBUG_REGISTERS) failed, error %lu", GetLastError());
    get_dr(self, &c);
    fid_check(c.Dr0 == 0x42424240 && c.Dr1 == 0x126bb070, "rt.dr0dr1", "Dr0 %#llx Dr1 %#llx", (unsigned long long)c.Dr0, (unsigned long long)c.Dr1);
    fid_check((c.Dr7 & ~0xdc00ULL) == 0xffff0115, "rt.dr7", "Dr7 %#llx (want ffff0115 outside bits 10-12, 14, 15)", (unsigned long long)c.Dr7);
    fid_check(c.Dr7 == (WOW64 ? 0xffff0115 : 0xffff0515), "rt.dr7.value", "%#llx after writing 0xffff0115 (Windows x64: 0xffff0515, bit 10 reads as 1; WoW64: 0xffff0115)", (unsigned long long)c.Dr7);
    set_dr(self, 0, 0, 0);
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr6 = 0xf;
    SetThreadContext(self, &c);
    get_dr(self, &c);
    fid_check(c.Dr6 == 0, "rt.dr6.dropped", "Dr6 %#llx after writing 0xf (Windows keeps no Dr6 a context sets, it reads 0)", (unsigned long long)c.Dr6);
    c.Dr6 = 0;
    SetThreadContext(self, &c);
    get_dr(self, &c);
    fid_check(c.Dr0 == 0 && c.Dr1 == 0 && c.Dr7 == 0, "rt.cleared", "Dr0 %#llx Dr1 %#llx Dr7 %#llx", (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    /* addresses without an enable bit in DR7, then every DR7 bit */
    set_dr(self, 0x42424240, 0x126bb070, 0);
    get_dr(self, &c);
    if (WOW64)
        fid_check(c.Dr0 == 0x42424240 && c.Dr1 == 0x126bb070 && c.Dr7 == 0x100, "rt.inactive", "Dr0 %#llx Dr1 %#llx Dr7 %#llx after writing addresses with DR7 0 (WoW64 keeps them and sets LE)", (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    else
        fid_check(c.Dr0 == 0 && c.Dr1 == 0 && c.Dr7 == 0, "rt.inactive", "Dr0 %#llx Dr1 %#llx Dr7 %#llx after writing addresses with DR7 0 (x64 Windows reports none while DR7 keeps nothing)", (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    set_dr(self, 0, 0, 0xffffffffffffffffULL);
    get_dr(self, &c);
    fid_check(c.Dr7 == (WOW64 ? 0xffff0155 : 0xffff0755), "rt.dr7.allbits", "Dr7 %#llx after writing all ones (Windows keeps x64 0xffff0755, WoW64 0xffff0155)", (unsigned long long)c.Dr7);
    set_dr(self, 0, 0, 0xffff0355);
    get_dr(self, &c);
    fid_check(c.Dr7 == (WOW64 ? 0xffff0155 : 0xffff0755), "rt.dr7.ge", "Dr7 %#llx after writing 0xffff0355 (LE and GE; x64 keeps GE, WoW64 does not)", (unsigned long long)c.Dr7);
    set_dr(self, 0, 0, 0x200);
    get_dr(self, &c);
    fid_check(c.Dr7 == (WOW64 ? 0 : 0x600), "rt.ge.alone", "Dr7 %#llx after writing GE alone (x64: kept, 0x600; WoW64: dropped, 0)", (unsigned long long)c.Dr7);
    /* when LE shows up: an address with only L0, LE alone, an address with LE alone */
    set_dr(self, 0x42424240, 0, 1);
    get_dr(self, &c);
    fid_check(c.Dr0 == 0x42424240 && c.Dr7 == (WOW64 ? 0x1 : 0x401), "rt.le.l0", "Dr0 %#llx Dr7 %#llx after writing Dr0 and Dr7 1", (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    set_dr(self, 0, 0, 0x100);
    get_dr(self, &c);
    fid_check(c.Dr0 == 0 && c.Dr7 == (WOW64 ? 0x100 : 0x500), "rt.le.alone", "Dr0 %#llx Dr7 %#llx after writing Dr7 0x100 and no address (LE alone keeps the registers)", (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    set_dr(self, 0x42424240, 0, 0x100);
    get_dr(self, &c);
    fid_check(c.Dr0 == 0x42424240 && c.Dr7 == (WOW64 ? 0x100 : 0x500), "rt.le.addr", "Dr0 %#llx Dr7 %#llx after writing Dr0 and Dr7 0x100", (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
    set_dr(self, 0, 0x126bb070, 0);
    get_dr(self, &c);
    fid_check(WOW64 ? (c.Dr1 == 0x126bb070 && c.Dr7 == 0x100) : (c.Dr1 == 0 && c.Dr7 == 0), "rt.le.dr1", "Dr1 %#llx Dr7 %#llx after writing Dr1 and Dr7 0 (x64: none kept; WoW64: kept with LE)", (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    set_dr(self, 0, 0, 0);
    {
        /* a new thread starts with clear registers while the creator has one set */
        CONTEXT tc;
        HANDLE t;
        set_dr(self, (uint64_t)code, 0, dr7(0, 0, 0));
        t = run_thread(report_dr, &tc, 0);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        set_dr(self, 0, 0, 0);
        fid_check(tc.Dr0 == 0 && tc.Dr7 == 0, "rt.newthread", "Dr0 %#llx Dr7 %#llx in a thread created while the creator had DR0/DR7 set (want 0, 0)", (unsigned long long)tc.Dr0, (unsigned long long)tc.Dr7);
    }

    /* ---- references: int3 and a trap-flag step ---- */
    arm(MODE_RECORD_ONLY);
    __asm__ volatile("int3");
    fid_check(g.hits == 1 && g.code == EXCEPTION_BREAKPOINT, "ref.int3", "hits %d code %#lx", g.hits, (unsigned long)g.code);
    arm(MODE_RECORD_ONLY);
    __asm__ volatile("pushf; pop %0" : "=r"(f));
    __asm__ volatile("push %0; popf; nop; nop" :: "r"(f | TF) : "cc", "memory");
    fid_check(g.hits >= 1 && g.code == EXCEPTION_SINGLE_STEP, "ref.tf", "hits %d code %#lx", g.hits, (unsigned long)g.code);
    fid_check(g.dr6 == 0, "ref.tf.dr6", "Dr6 %#llx in the single-step context (Windows reports 0, no BS)", (unsigned long long)g.dr6);
    fid_info("ref.tf.ctxflags", "ContextFlags %#lx in the single-step context", (unsigned long)g.flags);

    /* ---- instruction breakpoints ---- */
    arm(MODE_RF);
    set_dr(self, (uint64_t)code, 0, dr7(0, 0, 0));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "exec.fires", "hits %d (want 1)", g.hits);
    fid_check(g.code == EXCEPTION_SINGLE_STEP, "exec.code", "%#lx want 80000004", (unsigned long)g.code);
    fid_check(g.rip == (uint64_t)code && g.addr == (uint64_t)code, "exec.fault.rip", "Rip %#llx ExceptionAddress %#llx code %#llx", (unsigned long long)g.rip, (unsigned long long)g.addr, (unsigned long long)code);
    fid_check((g.dr6 & 0xf) == 1 && !(g.dr6 & 0x4000), "exec.dr6.b0", "Dr6 %#llx want B0 set, BS clear", (unsigned long long)g.dr6);
    fid_check(g.params == 0, "exec.params", "NumberParameters %lu", (unsigned long)g.params);
    fid_check((g.flags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS, "exec.ctxflags", "ContextFlags %#lx lacks CONTEXT_DEBUG_REGISTERS", (unsigned long)g.flags);
    fid_check(g.dr0 == (uint64_t)code && (g.dr7 & 1), "exec.ctx.dr", "handler context Dr0 %#llx Dr7 %#llx", (unsigned long long)g.dr0, (unsigned long long)g.dr7);
    fid_check(!(g.eflags & RF), "exec.ctx.rf", "EFlags %#llx: RF set in an instruction-breakpoint context (x86 leaves it clear there)", (unsigned long long)g.eflags);
    fid_check(!(g.eflags & TF), "exec.ctx.tf", "TF set in an instruction-breakpoint context");
    fid_check(g.dr6 == 0xffff0ff1, "exec.dr6.value", "Dr6 %#llx in the context (Windows: 0xffff0ff1, B0 with the bits x86 reads as 1)", (unsigned long long)g.dr6);
    fid_info("exec.dr6.live", "Dr6 %#llx from GetThreadContext inside the handler (Windows: 0xffff0ff0)", (unsigned long long)g.entry_dr6);

    /* every execution: the loop body runs five times with the breakpoint on its dec */
    arm(MODE_RF);
    set_dr(self, (uint64_t)code + 21, 0, dr7(0, 0, 0));
    fn_loop();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 5 && g.rip == (uint64_t)code + 21, "exec.loop", "hits %d last Rip %#llx (want 5 at %#llx)", g.hits, (unsigned long long)g.rip, (unsigned long long)code + 21);

    /* continuing without RF: the instruction faults again. What Windows does on a plain continue is recorded. */
    arm(MODE_PLAIN);
    set_dr(self, (uint64_t)code, 0, dr7(0, 0, 0));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 8 && g.cap_reached, "exec.plain", "continue with the context unchanged: %d hits%s (Windows sets no RF for the handler: the fault repeats until the handler disables the breakpoint)", g.hits, g.cap_reached ? ", cap reached" : "");

    /* enabled through DR7 but at another address: no hit. DR0 set and DR7 clear: no hit. */
    arm(MODE_RF);
    set_dr(self, (uint64_t)code + 2, 0, dr7(0, 0, 0));
    __asm__ volatile("" ::: "memory");
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && g.rip == (uint64_t)code + 2, "exec.middle", "breakpoint on the third instruction: hits %d Rip %#llx", g.hits, (unsigned long long)g.rip);
    arm(MODE_RF);
    set_dr(self, (uint64_t)code, 0, 0);
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "exec.disabled", "DR0 set with DR7 clear: hits %d", g.hits);

    /* DR1, the global enable bit, LEN other than 0, two registers on one address */
    arm(MODE_RF);
    set_dr(self, 0, (uint64_t)code + 1, dr7(1, 0, 0));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && g.rip == (uint64_t)code + 1 && (g.dr6 & 0xf) == 2, "exec.dr1", "hits %d Rip %#llx Dr6 %#llx", g.hits, (unsigned long long)g.rip, (unsigned long long)g.dr6);
    arm(MODE_RF);
    set_dr(self, (uint64_t)code, 0, 2 /* G0 */);
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "exec.g0", "enabled with G0 instead of L0: hits %d (Windows honours only the local enable bits)", g.hits);
    arm(MODE_RF);
    set_dr(self, (uint64_t)code, 0, dr7(0, 0, 3));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "exec.len4", "instruction breakpoint with LEN=4 bytes: %d hits (undefined by the SDM, none on Windows's AMD EPYC)", g.hits);
    arm(MODE_RF);
    set_dr(self, (uint64_t)code, (uint64_t)code, dr7(0, 0, 0) | dr7(1, 0, 0));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && (g.dr6 & 0xf) == 3, "exec.two.same", "DR0 and DR1 on one address: hits %d Dr6 %#llx (want 1 hit, B0|B1)", g.hits, (unsigned long long)g.dr6);

    /* Dr6 the kernel hands over: is it only this hit, or sticky from the previous one when the handler left it? */
    arm(MODE_KEEP_DR6);
    set_dr(self, (uint64_t)code, (uint64_t)code + 2, dr7(0, 0, 0) | dr7(1, 0, 0));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 2, "exec.sticky.hits", "hits %d", g.hits);
    fid_check((g.dr6 & 0xf) == 2, "exec.sticky", "first context Dr6 %#llx, second %#llx with the handler leaving Dr6 alone (Windows reports each hit's own bit, Dr6 is not sticky)", (unsigned long long)g.first_dr6, (unsigned long long)g.dr6);
    get_dr(self, &c);
    fid_check(c.Dr6 == 0, "exec.sticky.after", "Dr6 %#llx from GetThreadContext after the hits (want 0)", (unsigned long long)c.Dr6);
    c.Dr6 = 0;
    SetThreadContext(self, &c);

    /* the handler's context is applied on continue: Dr7 cleared there stops the next hit, Dr0 moved there hits the next instruction */
    arm(MODE_DR7_ZERO);
    set_dr(self, (uint64_t)code, 0, dr7(0, 0, 0));
    fn_nops();
    fn_nops();
    get_dr(self, &c);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "exec.ctx.dr7", "handler wrote Dr7=0 in the context: hits %d over two calls (want 1)", g.hits);
    fid_check(c.Dr7 == (WOW64 ? 0x100 : 0), "exec.ctx.dr7.readback", "Dr7 %#llx after the handler cleared it (WoW64 keeps Dr0 with LE)", (unsigned long long)c.Dr7);
    arm(MODE_MOVE_DR0);
    g_move_to = (uint64_t)code + 1;
    set_dr(self, (uint64_t)code, 0, dr7(0, 0, 0));
    fn_nops();
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 2 && g.rip == (uint64_t)code + 1, "exec.ctx.dr0", "handler moved Dr0 to code+1: hits %d last Rip %#llx", g.hits, (unsigned long long)g.rip);

    /* an instruction breakpoint on an instruction that then faults: the fault's context has RF set, so continuing
     * from the fault does not take the breakpoint a second time (one #DB, one access violation) */
    {
        uint8_t *ro = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        arm(MODE_RF);
        g_fix_page = ro;
        set_dr(self, (uint64_t)code + STORE4, 0, dr7(0, 0, 0));
        fn_write4(ro + 8);
        set_dr(self, 0, 0, 0);
        g_fix_page = NULL;
        fid_check(g.hits == 1 && g.av_hits == 1 && *(uint32_t *)(ro + 8) == 7, "exec.fault.rf", "%d breakpoint hits, %d access violations (want 1 and 1: the fault's RF covers the retry)", g.hits, g.av_hits);
        fid_info("exec.fault.rf.eflags", "EFlags %#llx in the access violation's context", (unsigned long long)g.av_eflags);
        VirtualFree(ro, 0, MEM_RELEASE);
    }

    /* per thread */
    arm(MODE_RF);
    set_dr(self, (uint64_t)code, 0, dr7(0, 0, 0));
    {
        HANDLE t = run_thread(other_thread, NULL, 0);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "exec.thread.other", "another thread with clear registers ran the code: hits %d", g.hits);
    arm(MODE_RF);
    {
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE t = run_thread(wait_then_run, ev, CREATE_SUSPENDED);
        DWORD tid = GetThreadId(t);
        r = set_dr(t, (uint64_t)code, 0, dr7(0, 0, 0));
        get_dr(t, &c);
        ResumeThread(t);
        SetEvent(ev);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        CloseHandle(ev);
        fid_check(r && c.Dr0 == (uint64_t)code && (c.Dr7 & 1), "exec.thread.set", "set %d, read back by handle Dr0 %#llx Dr7 %#llx", r, (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
        fid_check(g.hits == 1 && g.rip == (uint64_t)code && g.tid == tid, "exec.thread.hit", "hits %d Rip %#llx in thread %llu (want thread %lu)", g.hits, (unsigned long long)g.rip, (unsigned long long)g.tid, tid);
    }
    /* a thread that has started (blocked in a wait), suspended, given a breakpoint by handle, resumed */
    arm(MODE_RF);
    {
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE t = run_thread(wait_loop, ev, 0);
        DWORD tid = GetThreadId(t);
        Sleep(50);
        SuspendThread(t);
        r = set_dr(t, (uint64_t)code, 0, dr7(0, 0, 0));
        get_dr(t, &c);
        ResumeThread(t);
        SetEvent(ev);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        CloseHandle(ev);
        fid_check(r && c.Dr0 == (uint64_t)code && (c.Dr7 & 1), "exec.thread.started.set", "set %d, read back by handle Dr0 %#llx Dr7 %#llx", r, (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);
        fid_check(g.hits == 1 && g.rip == (uint64_t)code && g.tid == tid, "exec.thread.started.hit", "hits %d Rip %#llx in thread %llu (want thread %lu)", g.hits, (unsigned long long)g.rip, (unsigned long long)g.tid, tid);
    }
    get_dr(self, &c);
    fid_check(c.Dr0 == 0 && c.Dr7 == 0, "exec.thread.self", "the main thread's registers after setting another thread's: Dr0 %#llx Dr7 %#llx", (unsigned long long)c.Dr0, (unsigned long long)c.Dr7);

    /* ---- data breakpoints: a 4-byte write breakpoint on buf+4 ---- */
    memset(buf, 0, sizeof(buf));
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
    fn_write4(buf + 4);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && (g.dr6 & 0xf) == 1, "data.write", "hits %d Dr6 %#llx", g.hits, (unsigned long long)g.dr6);
    fid_check(g.code == EXCEPTION_SINGLE_STEP, "data.code", "%#lx", (unsigned long)g.code);
    fid_check(g.rip == (uint64_t)code + RET4 && g.addr == (uint64_t)code + RET4, "data.trap.rip", "Rip %#llx ExceptionAddress %#llx (want the ret after the store, %#llx)", (unsigned long long)g.rip, (unsigned long long)g.addr, (unsigned long long)code + RET4);
    fid_check(*(uint32_t *)(buf + 4) == 7, "data.write.done", "the store completed before the trap: %u", *(uint32_t *)(buf + 4));
    fid_info("data.ctx.eflags", "EFlags %#llx in the data-breakpoint context", (unsigned long long)g.eflags);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
    fn_read1(buf + 4);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "data.write.read", "a read of a write breakpoint: hits %d", g.hits);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 3, 3));
    fn_read1(buf + 4);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && g.rip == (uint64_t)code + RET1R, "data.rw.read", "a read of a read/write breakpoint: hits %d Rip %#llx (want %#llx)", g.hits, (unsigned long long)g.rip, (unsigned long long)code + RET1R);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
    fn_write4(buf + 8);
    fn_write1(buf + 3);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "data.outside", "writes next to the range: hits %d", g.hits);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
    fn_write1(buf + 7);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "data.lastbyte", "a byte write to the range's last byte: hits %d", g.hits);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
    fn_write8(buf, 0x1122334455667788ULL);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && g.rip == (uint64_t)code + RET8, "data.overlap", "an 8-byte write at buf covering the range: hits %d Rip %#llx", g.hits, (unsigned long long)g.rip);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 16, 0, dr7(0, 1, 0));
    fn_write1(buf + 16);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "data.len1", "1-byte breakpoint, 1-byte write: hits %d", g.hits);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 16, 0, dr7(0, 1, 2));
    fn_write1(buf + 23);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "data.len8", "8-byte breakpoint at buf+16, byte write at buf+23: hits %d", g.hits);
    /* unaligned: a 4-byte breakpoint at buf+5. The SDM asks for aligned addresses; x86 masks the low bits. */
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 5, 0, dr7(0, 1, 3));
    fn_write1(buf + 4);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "data.unaligned.below", "4-byte breakpoint at buf+5, byte write at buf+4: %d hits (the processor masks the address to buf+4)", g.hits);
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 5, 0, dr7(0, 1, 3));
    fn_write1(buf + 8);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "data.unaligned.above", "4-byte breakpoint at buf+5, byte write at buf+8: %d hits (masked to buf+4..7)", g.hits);
    /* two registers, the second one hit */
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 32, (uint64_t)buf + 40, dr7(0, 1, 3) | dr7(1, 1, 3));
    fn_write4(buf + 40);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1 && (g.dr6 & 0xf) == 2, "data.dr1", "hits %d Dr6 %#llx (want B1)", g.hits, (unsigned long long)g.dr6);
    /* a string instruction: rep stosb over 32 bytes with a byte breakpoint in the middle */
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 16, 0, dr7(0, 1, 0));
    stos_thunk(buf, 32, 0x55);
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 1, "data.string", "rep stosb over the range: hits %d", g.hits);
    fid_check(g.rip == (uint64_t)code + 64 && g.rcx == 15, "data.string.rip", "Rip %+lld from the rep stosb, Rcx %llu (x86 traps after the iteration that wrote the byte: Rip at the rep, Rcx the 15 bytes left)", (long long)(g.rip - ((uint64_t)code + 64)), (unsigned long long)g.rcx);
    fid_check(buf[31] == 0x55, "data.string.done", "the string instruction completed after the trap: buf[31] %#x", buf[31]);
    /* repe cmpsb over two equal 32-byte buffers with a read breakpoint on the second one's byte 16: the trap comes
     * after that element with Rip at the instruction and Rcx the 15 elements left, then the compare completes */
    {
        static uint8_t ca[32], cb[32];
        uintptr_t left, fl;
        memset(ca, 0x33, sizeof(ca));
        memset(cb, 0x33, sizeof(cb));
        arm(MODE_RECORD_ONLY);
        set_dr(self, (uint64_t)cb + 16, 0, dr7(0, 3, 0));
        left = cmps_thunk(ca, cb, 32, &fl);
        set_dr(self, 0, 0, 0);
        fid_check(g.hits == 1 && g.rip == (uint64_t)code + 112 && g.rcx == 15, "data.cmps", "hits %d Rip %+lld from the repe cmpsb Rcx %llu (want 1 hit at +0 with 15 left)", g.hits, (long long)(g.rip - ((uint64_t)code + 112)), (unsigned long long)g.rcx);
        fid_check(left == 0 && (fl & 0x40), "data.cmps.done", "after the trap the compare finished: Rcx %llu, ZF %d", (unsigned long long)left, !!(fl & 0x40));
    }
    /* another thread writing the range with its own registers clear */
    arm(MODE_RECORD_ONLY);
    set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
    {
        HANDLE t = run_thread(write_other, buf + 4, 0);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
    set_dr(self, 0, 0, 0);
    fid_check(g.hits == 0, "data.thread.other", "another thread wrote the range: hits %d", g.hits);
    /* the kernel's write into a watched buffer (ReadFile) is not a program access: recorded */
    arm(MODE_RECORD_ONLY);
    {
        char path[MAX_PATH], name[MAX_PATH];
        HANDLE h;
        DWORD n = 0;
        GetTempPathA(sizeof(path), path);
        GetTempFileNameA(path, "drt", 0, name);
        h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            WriteFile(h, "abcdefgh", 8, &n, NULL);
            SetFilePointer(h, 0, NULL, FILE_BEGIN);
            set_dr(self, (uint64_t)buf + 4, 0, dr7(0, 1, 3));
            ReadFile(h, buf, 8, &n, NULL);
            set_dr(self, 0, 0, 0);
            CloseHandle(h);
            fid_check(g.hits == 0 && n == 8, "data.kernel", "ReadFile into the watched range: %d hits, %lu bytes read (a kernel write is not a program access)", g.hits, n);
        }
    }

    get_dr(self, &c);
    fid_check(c.Dr0 == 0 && c.Dr1 == 0 && c.Dr7 == 0, "end.cleared", "Dr0 %#llx Dr1 %#llx Dr7 %#llx", (unsigned long long)c.Dr0, (unsigned long long)c.Dr1, (unsigned long long)c.Dr7);
    return fid_summary();
}
