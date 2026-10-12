# Renderers for the arm64 line

vkd3d-proton (Direct3D 12) and DXVK (DXGI, Direct3D 11 and 10) on Vulkan, at the commits pinned in
`inputs-arm64.json`, with the patches below. `build-renderers-arm64.yml` cross-compiles them for x86-64 Windows with
llvm-mingw 20250910 (clang 21, the toolchain of the tested builds; clang 23 refuses dxil-spirv's dxbc-spirv), marks every DLL as a Wine builtin (Wine 11.18 ignores DLLs in WINEDLLPATH that are
not), and packages `renderers/vkd3d/wine/aarch64-windows` and `renderers/dxvk/wine/aarch64-windows`, the overlays
Highball's .vkd3d and .dxvk modes put first in WINEDLLPATH. The DLLs run under FEX like the games that load them.

Each series is `git format-patch` output against its pinned commit. Applied in order they rebuild, file for file,
the trees the Microsoft Flight Simulator 2024 runs of 2026-10-11 and 12 were built from (checked on the M4).

## vkd3d-proton (v3.0.1)

- `0001` vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR is optional (winevulkan returns NULL for it on MoltenVK).
- `0002` missing transform feedback queries, single texel alignment and robustness2 are warnings, not a failed
  device.
- `0003` the NV memory decompression shaders count regions in an array, not a uvec3 (Metal refuses an atomic on a
  vector element).
- `0004` null descriptors emulated with dummy resources on devices without nullDescriptor. Known limit: the dummy
  resources are shared and writable, so a write through one null UAV is visible through another.
- `0005` tessellation on MoltenVK (driver ID check): one Location per varying, no Component decorations, which
  SPIRV-Cross cannot translate in tessellation stages.
- `0006` SV_ShadingRate dropped on devices without primitive fragment shading rate.

## DXVK (master of 2026-10-09, 1cad6cd)

- `0001` a texture that cannot be shared is created unshared instead of calling vkGetMemoryWin32HandleKHR, which was
  never loaded (Microsoft Flight Simulator 2024 crashed at its first boot video).
- `0002` `dxgi.maxVideoMemoryBudget` (MB) caps the budget QueryVideoMemoryInfo reports, which on unified memory was
  about 80% of RAM and let games fill more than the GPU may wire.
- `0003` `DXVK_RELAX_FEATURES=1` turns missing required features into warnings, and geometry shaders are optional
  (Apple GPUs have none). From Michael-Liendo's msfs2024-apple-silicon work (highball-db#6).
