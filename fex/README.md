# FEX for the arm64 line

`fexunixlib_darwin.cpp` is the Unix helper FEX's Windows DLLs load from the ntdll.so directory: the
same call table as FEX's Linux helper, with BSD calls, and TSO through `thread_set_x86_64_compat`.

## Patch series for a FEX 2610 build (`patches/`)

`build-arm64` uses Hangover 11.16's FEX 2608 DLLs by default. Dispatched with `fex_build: source`, it
builds `libarm64ecfex.dll` and `libwow64fex.dll` from FEX 2610 (`inputs-arm64.json`, `fex-source`)
with this series applied, for x86-64 programs that see what real x86-64 Windows shows them. Each
patch was measured with `tests/x64-fidelity`, whose expectations hold on real x86-64 Windows, on an
M1 Pro (2026-10-10 and 2026-10-11). The Wine half is `patches-arm64/0022` to `0028`.

- `0001` floating-point exceptions: unmasked SSE and x87 exceptions trap (FPCR trap enables armed
  from MXCSR in JIT code only), MXCSR status flags live in FPSR as on ARM64EC Windows, x87 status
  flags from SoftFloat, x87 #MF before the next waiting instruction.
- `0002` integer divide errors: DIV and IDIV by zero raise STATUS_INTEGER_DIVIDE_BY_ZERO and a
  quotient overflow STATUS_INTEGER_OVERFLOW at the instruction (.NET, Mono and Java rely on them).
- `0003` the x64 state an ARM64 context has no room for (x87 words, EFLAGS, DAZ, YMM upper halves)
  travels through ntdll; exception handlers run with DF and TF clear.
- `0004` FEX's own C++ code (compiler, linker) no longer leaves the inexact flag in the guest's
  MXCSR: before, the first call of every API set MXCSR.PE.
- `0005` a REP MOVS or STOS interrupted by a fault resumes where it stopped, not from the start,
  which corrupted overlapping copies (memmove) on commit-on-demand memory.
- `0006` `GetX87State` for ntdll's `RtlCaptureContext`, `setjmp` and `GetThreadContext`.
- `0007` MXCSR.DAZ kept in FPCR bit 19, where the ARM64EC context conversion reads it.
- `0008` FUCOM and FUCOMI raise invalid only for a signalling NaN.
- `0009` one MXCSR for x64 code and the native CRT: rounding and denormal control set by
  `_controlfp` show in MXCSR, masks are the emulator's (`GetX64Mxcsr`, `SetX64Mxcsr`).
- `0010` exact FPREM and FPREM1 with the quotient bits in C0, C3 and C1 and partial reduction in C2
  (2^100 mod 3 gave 0 before; real x86 confirms every case of the test).
- `0011` MXCSR_MASK 0xFFFF in FXSAVE and XSAVE.
- `0012` no FPCR write when nothing changes (writing it costs about 4 ns on Apple cores): calls
  from x64 code into native code cost what they cost with FEX 2610 unpatched.
- `0013` self-modifying code across threads: a block whose guest code changed before its page was
  protected is compiled again. A thread kept running code another thread had patched in about 1 of
  300 patches, for good.
