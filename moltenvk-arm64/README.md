# MoltenVK for the arm64 line

MoltenVK v1.4.2 (Vulkan on Metal) with the patches below, and SPIRV-Cross (MoltenVK's SPIR-V to Metal Shading
Language translator) at the revision MoltenVK 1.4.2 pins, with Highball's patches. `build-moltenvk-arm64.yml` builds
them for arm64 and packages `lib/libMoltenVK.dylib`, the library the engine's winevulkan opens. The Intel line's
MoltenVK is built separately, for x86-64, by the app repository's `Scripts/build-moltenvk.sh`.

Each series is `git format-patch` output against its pinned commit. Applied in order they rebuild, file for file, the
MoltenVK tree of the gin19g build that the Microsoft Flight Simulator 2024 runs of 2026-10-12 used (checked on the M4
against that tree), and the SPIRV-Cross commits that build carried.

## MoltenVK (v1.4.2, db66022)

- `0001` a descriptor set is bound again after MoltenVK's own clears, copies and fills replaced its argument buffer at
  the same Metal index. Without it the next draw or dispatch read the helper's buffer as the set: GPU hangs and
  garbage in Microsoft Flight Simulator 2024 through vkd3d-proton.
- `0002` destroyed Metal objects are released 3 seconds late. The GPU may still run work submitted before the
  destroy; Windows and Linux keep such memory until the GPU is done, and D3D12 games rely on it. A time limit is
  crude: releasing when the submitted work completes would be exact.
- `0003` command buffer error messages name the VkDevice (DXVK's and vkd3d-proton's both label theirs "Queue 0-0").
- `0004` `HB_MVK_TEXTURE_2D_AS_ARRAY=1` creates shader-readable 2D views as one-layer 2D array textures and converts
  shaders with SPIRV-Cross's `texture_2D_as_2D_array` (SPIRV-Cross `0004`). Off unless set.
- `0005` placement heaps go into the device's residency set. Images placed in a heap had their own texture added,
  but the textures MoltenVK makes from the heap later (block-texel views of compressed images, 2D views of 3D images)
  did not, and shader validation reported them used while not resident in three compute pipelines of Microsoft
  Flight Simulator 2024.
- `0006` an imported MTLTexture stays the memory of its dedicated image. initExternalMemory(), written for export,
  replaced it with a new texture, so memory shared between two VkDevices as an MTLTexture never showed the other
  side's content. Needed by patches-arm64/0036.
- `0007` textures shared with another VkDevice are not compressed (allowGPUOptimizedContents NO). The two devices'
  queues are not ordered, and a read racing a write of a compressed texture showed whole tiles of magenta in the
  game's videos. Measured: magenta blocks in 2 of 25 boot screenshots before, 0 of 216 after (one per second).
- `0008` an image bound to imported MTLTexture memory retains the texture. It used the memory's texture without a
  reference of its own and released it on destroy, so a texture shared between two VkDevices was freed while the
  other side still used it (crashes in vkDestroyImage/vkDestroyBuffer and a hung game once videos were shared). Ships
  with 0006 and patches-arm64/0036.

## SPIRV-Cross (6c09849, MoltenVK 1.4.2's pin)

- `0001` alias casts keep the base variable's `__restrict` (Metal refused casts that dropped it, about 55 of the
  game's failing shaders), `raster_order_group` stays off arrays of buffer pointers, and an OpCopyObject of an access
  chain keeps its access chain flag.
- `0002` `__restrict` goes on the element pointer of buffer arrays passed by reference, so the parameter binds to the
  argument buffer member (hull shaders failed with "no matching function for call to 'patch_main'").
- `0003` a flattened input array element widened to the previous stage's vector size (a float3 read as a float4) is
  swizzled back when copied into the array.
- `0004` `Options::texture_2D_as_2D_array` declares non-multisampled 2D textures as 2D arrays and reads layer 0, the
  same shape as `texture_1D_as_2D`, for D3D12 shaders that read a Texture2DArray descriptor through a Texture2D
  declaration (the game's compute passes).
