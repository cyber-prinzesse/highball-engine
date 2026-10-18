# FEX for the arm64 line

`fexunixlib_darwin.cpp` is the Unix helper FEX's Windows DLLs load from the ntdll.so directory: the
same call table as FEX's Linux helper, with BSD calls, and TSO through `thread_set_x86_64_compat`.

## Patch series for a FEX 2610 build (`patches/`)

`build-arm64` builds `libarm64ecfex.dll` and `libwow64fex.dll` from FEX 2610 (`inputs-arm64.json`,
`fex-source`) with this series applied, for x86-64 programs that see what real x86-64 Windows shows
them. That is the default since 2026-10-11. Dispatched with `fex_build: hangover`, it uses Hangover
11.16's FEX 2608 DLLs instead, which have none of this. Each patch was measured with
`tests/x64-fidelity`, whose expectations hold on real x86-64 Windows, on an M1 Pro (2026-10-10 and
2026-10-11). The Wine half is `patches-arm64/0022` to `0029`.

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
- `0014` the same string-instruction resume as `0005` for 32-bit programs (the WoW64 module).
- `0015` MXCSR and the x87 status word through the 32-bit context. Without it `0001` hangs a 32-bit
  program that unmasks an SSE exception and masks it again in its handler (the trap re-armed from a
  copy the context never reached). 32-bit SSE exceptions arrive as STATUS_FLOAT_MULTIPLE_TRAPS, as
  32-bit Windows reports them.

- `0016` EFLAGS in contexts: only the architectural bits. The NZCV storage bytes leaked into bits
  24-31 once `0003` passed all of EFLAGS (an int3 context showed 0xa0000e97 where Windows shows
  0xe97).
- `0017` the x86 debug registers DR0-DR3, DR6 and DR7, per thread. Instruction breakpoints end a
  block before the instruction and fault there (#DB, RIP at the instruction); RF from the context
  the thread resumes with lets it run once, and fault contexts report RF set, as Windows does. Data
  breakpoints are checked on every guest memory access; a thread with one runs one instruction per
  block so the trap comes right after the access, and REP string instructions run one element per
  execution (RIP at the instruction, RCX the count left). A thread with breakpoints compiles into a
  code buffer of its own. No cost without breakpoints; about three times slower while a data
  breakpoint is set. The registers cross ntdll in contexts, like the rest of the x64 state: the
  ARM64EC module takes them from a context it is entered with and reports them in the contexts it
  builds, and exports `KeepX64DebugRegisters`, what x86-64 Windows keeps of registers a context sets
  (ntdll keeps a thread's registers while it is outside the emulator, `patches-arm64/0029`); the
  WoW64 module takes them from the 32-bit context. Registers read back as Windows reports them for a
  64-bit and for a 32-bit program, which differ (drtest), from this thread or from another process
  (drdbg).

- `0018` hardware TSO in every thread on macOS. TSOHandlerConfig turned Apple's TSO mode on once, in
  ProcessInit, and then told the JIT the hardware orders memory, so the JIT emits no ordering of its own.
  `thread_set_x86_64_compat` changes only the calling thread on macOS (Linux's prctl carries over to new
  threads), so every thread but the first ran x86-64 code with Arm's weak ordering, in every program on the
  arm64 line. ThreadInit (ARM64EC) and BTCpuThreadInit (WoW64) now turn it on for each thread before it runs
  guest code. Measured on the M4 (2026-10-12) with the message-passing test that became
  `tests/x64-fidelity/ordertest`, 6 pairs of 20,000,000 rounds: 2,938 to 2,841,851 reorderings per pair
  before, none after, 27 s instead of 21. `ordertest` at its defaults on the CI engine (M4, 2026-10-12):
  without 0018 all four pairs fail in 64-bit (356,151 to 1,227,594 reorderings) and in 32-bit through
  WoW64 (427,248 to 3,036,629); with it all pass in both. Found while Microsoft Flight Simulator 2024
  crashed in a game thread during loading.

- `0019` stores into the data of a page that holds translated code no longer throw the page's
  translations away. Blocks record the guest instruction bytes they were decoded from; on a write fault
  in an RWX region the ARM64EC module decodes the faulting JIT store (STR, STUR, STR register, STLR,
  STLUR, STP and STNP, general and SIMD registers) and, when it changes no instruction byte, writes it
  for the guest through a second, writable mapping of the page (`AliasWrite` in
  `fexunixlib_darwin.cpp`: `mach_vm_remap`), steps past it and keeps the page protected, so another
  thread's write into code on that page still faults. Stores that change nothing are skipped. Stores
  into code, undecoded stores and more than 2,000 replays into a page within 100 ms
  (`HB_FEX_SMC_BURST`) take the old path; `HB_FEX_SMC_REPLAY=0` turns it off. `smcdata.exe` on the M4:
  data beside a translated function 2.6 times faster, 0 stale calls with code rewritten beside a
  thread writing data (opening the page for the store instead ran stale code 1,960 times in 2,000).
  The DLL and the helper go together: the helper's call table gains `AliasWrite`, and Wine's unix
  call dispatch has no bounds check.
- `0020` translations a write fault invalidates are kept and put back when the same instruction bytes
  return. Copy protections decrypt a function, run it and encrypt it again; Microsoft Flight Simulator
  2024's main thread rewrote translated instruction bytes about 380 times a second and its stutter
  followed that thread's translation time (2026-10-18). A block records an XXH3 hash of its bytes; a
  write fault retires the page's blocks into a preallocated table of their code buffer (16,384 entries,
  about 2 MB, no allocation in the fault path); a lookup miss checks it first, protects the block's
  pages again, hashes the bytes and maps the old host code back when they match. Any other invalidation
  (protection change, free, unmap) purges the retired blocks on its pages, and blocks compiled for
  single steps, breakpoints, an instruction limit or with an instruction that failed to decode are
  never kept. `HB_FEX_SMC_RETIRE=0` turns it off. `smcretire.exe` on the M4: decrypt, run, encrypt 41
  against 155 us a round, two versions in turn 38 against 171 us, 0 stale calls in every case (beside a
  data-writing thread, with eight versions, with a protection change between rounds); `smcpatch` 64
  and 32-bit, `drtest` 64 and 32-bit, `fibertest`, `ctxtest`, `memtest`, `kattest`, `tsctest` and the
  string resume pass.
- `0021` makes `0019` and `0020` opt-in: `HB_FEX_SMC_REPLAY=1` turns the replay on, `HB_FEX_SMC_RETIRE=1`
  retirement. With both on, Microsoft Flight Simulator 2024 hung 45 s into every flight on the M4 (three of three,
  2026-10-18: its main thread ran into an unmapped page and Wine's unwinder recursed until the stack ran out, which
  `patches-arm64/0042` stops); either one alone flew. Diagnosis switches for the search: `HB_FEX_SMC_REINSTATE=0`
  (retire, never reinstate), `=2` (reinstate only where no page protection changes), `HB_FEX_SMC_SAME=0` (write
  stores that change nothing), `HB_FEX_SMC_VERIFY=1` (read every replayed store back).
- `0022` exports `GuestProtection`, which Wine's ntdll (`patches-arm64/0043`) asks for the protection a program set
  on a page: FEX makes read-write-execute pages that hold translated code read-execute so writes fault into its SMC
  tracking, and the program saw that (VirtualQuery and VirtualProtect's old protection read 0x20 for a page it made
  0x40). A program that saved a page's protection, changed it and restored the saved value locked its own page and
  faulted on its next write (`smcprot.exe`, 100 of 100). Copy protections guard their pages this way.

32-bit programs were measured with the engine built by CI (it has the i386 half): x87test32 120/128,
the same eight x87 gaps as the 64-bit build; the string resume and the code patching probe pass.
