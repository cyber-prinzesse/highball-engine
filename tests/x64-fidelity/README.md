# x86-64 fidelity tests

Nine standalone Windows x64 programs (three of them also built as 32-bit ones) that check what an x86-64 program can observe about the CPU and the
Windows exception and memory model. They are the regression tests for the arm64 engine line (Wine ARM64EC +
FEX), and run unchanged on real x86-64 Windows, which is the reference: a check that fails on real Windows is
a wrong check, not a finding.

| program | area |
|---|---|
| `ctxtest` | exceptions and CONTEXT: the record and context a vectored handler receives for int3, ud2, divide errors, access violations, DEP, privileged instructions and single step; continuing with a modified context (GPRs, EFlags incl. PF/AF/DF, MXCSR, x87 control word and stack, XMM, YMM); DF in handlers; nested and repeated exceptions; SEH unwinding of nonvolatile registers; RtlCaptureContext and RtlRestoreContext; Get/SetThreadContext on a suspended thread and, one class at a time, on the current thread; RF in fault contexts and reserved EFLAGS bits; a thread checking its own x87/MXCSR/DF/YMM state while another thread suspends and resumes it |
| `cpuidtest` | CPUID, XGETBV and XSAVE consistency: advertised extensions execute and have their XCR0 state, leaf 0xD sizes, XSAVE/XSAVEOPT/XSAVEC/XRSTOR/FXSAVE/FXRSTOR layout and round trips, XRSTOR init semantics, GetEnabledXStateFeatures and IsProcessorFeaturePresent against CPUID, AVX state under preemption and suspension |
| `x87test` | FXAM on every class, full and abridged tag words, TOP, FNSAVE/FRSTOR/FNSTENV/FLDENV, precision and rounding control, masked exception flags and stack faults, FPREM/FPREM1 quotient bits, FCOM/FUCOM/FCOMI, FIST range, MMX aliasing, MXCSR (round trip, sticky flags, DAZ, FTZ, RC) |
| `fibertest` | nonvolatile registers (rbx rbp rsi rdi r12-r15, xmm6-15), stack canaries, FLS and GetCurrentFiber across fiber switches, a fiber migrating between threads, exceptions between switches; MXCSR/x87 control word per fiber recorded |
| `memtest` | VirtualQuery/VirtualProtect/VirtualFree at 4 KB granularity, every protection, PAGE_GUARD, page-split accesses, generated and self-modifying code (documented flush path, RWX without a flush, inline, WriteProcessMemory, another thread), write watch, stack growth |
| `kattest` | known-answer tests for CRC32 (SSE4.2), AES-NI, PCLMULQDQ, SHA-NI (full SHA-1 and SHA-256 blocks), VAES and VPCLMULQDQ against portable C, for every one of them CPUID advertises |
| `tsctest` | fenced RDTSC monotonic, happens-before across threads and while migrating, RDTSCP, CPUID's TSC frequency against the measured rate |
| `drtest` | the hardware debug registers DR0-DR3, DR6 and DR7 through Get/SetThreadContext, exception contexts and NtContinue: what reads back (64-bit and 32-bit programs differ), instruction breakpoints (a fault before the instruction, Dr6 B bits, RF on resume and after a fault, per thread, set by handle on a thread created suspended and on one already running), data breakpoints (write and read/write, every length, range edges, unaligned addresses, rep stosb and repe cmpsb traps with Rcx left, another thread's and the kernel's writes); also built as `drtest32.exe` |
| `drdbg` | the debug registers of a thread in another process, as a debugger or a trainer reaches them: set at a debug event (one raised by native code, OutputDebugString, and an x64 exception), read back, Dr6 at each breakpoint event, continuing with RF and DBG_CONTINUE; set on a thread suspended while it runs x64 code and on one blocked in a wait, read back before and after the thread ran (DR7 bit 10 only shows once it ran), a second breakpoint added to the ones read back, cleared; set on a process created suspended before its first instruction; also built as `drdbg32.exe` |

Output: one line per check, `PASS <id>`, `FAIL <id>: <detail>` or `INFO <id>: <value>`, then
`SUMMARY <test> pass=N fail=M`; the exit code is the number of failures.

Build with llvm-mingw 20260922: `./build.sh <llvm-mingw>/bin out`. x87test is also built as a 32-bit
program (`x87test32.exe`), which Windows x64 runs through WoW64, as the engine does with FEX's WoW64 DLL. The workflow `x64-fidelity.yml` builds
them once and runs the same files on Windows Server 2022 and 2025 (x86-64) and on Windows 11 ARM64 (Microsoft's
x64 emulation, for comparison).
