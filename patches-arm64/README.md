# Patch series, arm64 line

Applied in lexical order on top of upstream Wine (`inputs-arm64.json`), by `build-arm64.yml`. The
Intel line's series in `patches/` is for CrossOver's tree and does not apply here; what the arm64
line needs of it is carried over by number.

Measured on an M4 (macOS 27.0, 2026-10-04): with these three patches, Hangover's FEX DLLs and the
Darwin helper in `fex/`, the native arm64 Wine boots a prefix in 2.4 s with both halves populated,
and 32-bit and 64-bit x86 programs run within 20 percent of Rosetta on a compute loop. Graphics are
not covered yet: DXMT needs the Mac driver's Metal glue, which upstream Wine does not carry (the
CrossOver tree does, as `macdrv_functions`), so the arm64 line has no renderer overlay until
CrossOver 27's sources are published or the glue is ported (about 500 lines).

- `0003-ntdll-winedllpath-prepend.patch`: WINEDLLPATH_PREPEND, the same file as the Intel line's
  0003. Highball's renderer overlays and shims all go through it.

- `0018-loader-arm64-macos-pagezero-reserve.patch`: arm64 macOS has no preloader and a `__PAGEZERO`
  of 4 GB or more. With Apple's cross-architecture entitlement the kernel lets the process map over
  it, so the loader hands Wine that range as its reserved low memory, where 32-bit and 64-bit
  Windows programs expect their address space. Without it `map_fixed_area` fails for every image
  below 4 GB. Only effective in a loader linked with `-pagezero_size 0x170000000 -image_base
  0x170000000 -x86_64_layout_emulation` (Xcode 26.4 or later) and signed with the entitlement, which
  is Highball's `WineLoader.app`; the unsigned loader this build produces is for development.

- `0019-ntdll-arm64-macos-image-pages.patch`: macOS on arm64 refuses writable-and-executable
  memory outside `MAP_JIT`, refuses to execute a file-backed mapping that is not signed, and has
  16 KB pages where x86 images are laid out in 4 KB ones, so code and data share a host page.
  `map_file_into_view` reads writable image sections into anonymous memory instead of mapping the
  file, requests for RWX memory are granted as RW and the page is flipped to RX on the first
  execute fault and back on the first write fault (`virtual_handle_fault`), tracked in a small
  range table that `delete_view` clears. `MAP_JIT` was tried first and does not fit: a thread's JIT
  write-protect state cannot be flipped from inside a signal handler, the flip is lost on return.
  Upstream: Brendan Shanks (CodeWeavers) announced macOS ARM64 merge requests on wine-devel on
  2026-08-07; none had landed by 2026-10-04. These two patches are Highball's own and go the day his
  land.

The rest makes x86-64 programs see what real x86-64 Windows shows them (2026-10-11, M1 Pro, macOS
27.0). Each finding comes from a check in `tests/x64-fidelity`, whose expectations hold on real
x86-64 Windows (the `x64-fidelity` workflow). 0022 to 0024 come from the FEX and arm64 spikes of
2026-10-09 and 2026-10-10. With Hangover's FEX DLLs (the default) every one of them is harmless:
the state they carry is simply absent, and the hooks that ask the emulator fall back when its
exports are missing. With the FEX build of `fex/patches/` (`fex_build: source`) they are its other
half.

- `0022-ntdll-arm64-macos-fp-trap-sigill.patch`: Apple silicon implements the FPCR trap enables,
  and macOS reports a trapped floating-point exception as SIGILL with ILL_ILLTRP instead of
  SIGFPE. The SIGILL handler now tells it apart by the syndrome (EC 0x2c) and raises the matching
  STATUS_FLOAT_* code at the instruction, and clears the trap enables in the context the
  dispatcher runs with. The x64 half of unmasked SSE and x87 exceptions (FEX patch 0001).

- `0023-loader-arm64-macos-4k-pages.patch`: every Wine process re-execs itself with
  `posix_spawnattr_set_4k_page_size_np` (Apple's cross-architecture entitlement, which Highball's
  signed loader carries), so it runs with 4 KB pages: Windows programs assume 4 KB protection
  granularity (guard pages, VirtualProtect on part of a 16 KB page, write watches, self-modifying
  code detection). With 16 KB pages six memory checks fail. Falls back to host pages with a
  message when the attribute is refused; `HB_NO_4K_PAGES=1` turns it off.

- `0024-ntdll-arm64-macos-4k-pages-initial-process.patch`: the first process of a session never
  went through 0023's exec, only its children did. It now re-execs too while it still has 16 KB
  pages.

- `0025-ntdll-arm64-macos-id-registers.patch`: FEX decides which x86 extensions it can implement
  from the host's ID registers, which ntdll publishes in the registry (`CP 40xx`). macOS traps user
  reads of those registers, so `get_core_id_regs_arm64` was a stub and FEX saw a plain ARMv8.0
  CPU: no SSE4.2, AES-NI, PCLMULQDQ or SHA for x86 programs, and no LSE atomics for FEX itself,
  on Macs that have all of them. The registers are now built field by field from the
  `hw.optional.arm.FEAT_*` sysctls. On an M1 Pro: all advertised, and kattest's known-answer
  checks (CRC32C, AES-NI, PCLMULQDQ, SHA-1 and SHA-256 blocks, VAES, VPCLMULQDQ) all match.

- `0026-ntdll-aarch64-xstate-layout.patch`: `GetEnabledXStateFeatures` returned 0 on aarch64
  hosts although the emulator reports AVX. The XState layout (x87, SSE, AVX, standard form) now
  matches the emulator's CPUID leaf 0xD, so contexts can carry the YMM upper halves.

- `0027-ntdll-arm64ec-x64-state-through-context.patch`: the ARM64EC register mapping has no room
  for the x87 control, status and tag words, the EFLAGS bits NZCV cannot hold (PF, AF, DF, TF),
  MXCSR's DAZ or the YMM upper halves, so every exception handler, `GetThreadContext` and continue
  lost them: handlers saw a zero x87 control word, a continued thread came back with its x87 stack
  scrambled and DF from wherever the JIT left it. The conversions now carry them in registers
  ARM64EC code never uses (X13, X14, a marker in X28, V16-V31) and the dispatchers use an x64
  context with room for the AVX state. `RtlCaptureContext`, `setjmp` and `GetThreadContext` on the
  current thread take the x87 state from the emulator (`GetX87State`): before, a C++ exception
  unwound by `RtlUnwindEx` reset the x87 control word to 0x27f, which drops mingw-w64 programs
  from 64-bit to 53-bit precision.

- `0028-ntdll-arm64ec-x64-information-through-emulator.patch`: the CRT's `_controlfp` is native
  code and reads and sets MXCSR through `__os_arm64x_get_x64_information` and
  `__os_arm64x_set_x64_information`. The exception masks now come from and go to the emulator
  (`GetX64Mxcsr`, `SetX64Mxcsr`), which arms the trap enables for x64 code only, and native code
  keeps running with them off; the x87 control word comes from `GetX87State` instead of a fixed
  0x27f. With FEX patch 0009, `_controlfp` and `LDMXCSR` see each other's rounding mode, masks and
  denormal control, and an exception unmasked with `_controlfp` traps, as on x86-64 Windows.
