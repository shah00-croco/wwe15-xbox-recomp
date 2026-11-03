# WWE 2K15 native renderer: handoff (2026-11-03, evening)

**Project:** `D:\wwe2k15-recomp`, a ReXGlue recompilation of WWE 2K15 (Xbox 360). The SDK is at `D:\narutoxbox\.tools\rexglue-sdk`, commit `c94f5eb`. Nothing is committed. Game data and anything made from it stay private under `out/`, `assets/` and `.tools/`.

**Goal:** a native renderer, Unleashed-style ("option B"). The game's own D3D calls are hooked and drawn directly with Vulkan, with no Xbox GPU emulation.

**Status:** B1–B3 are done and B4 is mostly done. **The title screen now matches the emulator**: logo, stadium lights, colours, wrestler and "PRESS START BUTTON". Comparison screenshots are `out/texcache-final/shot-45s.png` (native) and `out/emulator-reference/shot-45s.png` (emulator).

The full research log is in [native-renderer.md](native-renderer.md). Sections 17–20 cover B1–B4. This session's findings (below) are not written into it yet.

## Build and run

```bash
$env:REXGLUE_SDK_SOURCE_DIR='D:/narutoxbox/.tools/rexglue-sdk'
```

```bash
.\build-windows.ps1 -SdkPrefix D:\narutoxbox\.tools\rexglue-sdk\out\install\win-amd64-vulkan-test -ToolsRoot D:\narutoxbox\.tools -Jobs 8
```

```bash
.\run-windows.ps1 -VulkanDevice 1 -Keyboard -GpuPlugin native -GameArguments '--wwe2k15_native_renderer=true'
```

- **Emulator mode (default):** run without `-GpuPlugin` to get the normal emulator.
- **Several game flags:** `-GameArguments` is a `string[]`, so pass each flag as its own element (`-GameArguments 'a','b'`). One string with spaces becomes one argument.
- **Profiling build:** set `WWE2K15_PROFILING_PLUGIN=1` to also build `rexgpu-xenosprof`, an emulator plugin with debug symbols for profiling. Read its symbols with `llvm-symbolizer`. The Release `wwe2k15.exe` has no symbols.
- **Logs:** `out/build/win-amd64-release/logs/wwe2k15_NNN.log`. Native lines start with `[native]`. Every 600 frames it logs the draws, pipelines, textures, render targets and resolves.
- **Heap errors are not ours:** `PhysicalHeap::Alloc unable to alloc` errors show up in every run, including before this work.

## Done in this session (2026-11-03)

1. **Texture swizzle:** the image view applies `fetch.swizzle` (same mapping as the SDK's `GuestToHostSwizzle`), and the swizzle is part of the cache key. This fixed red and blue being swapped.
2. **Render state from the registers:** blending (`RB_BLENDCONTROL0`), the colour write mask (`RB_COLOR_MASK`) and alpha test (`RB_COLORCONTROL` and `RB_ALPHA_REF`, through XenosRecomp's `SPEC_CONSTANT_ALPHA_TEST` and `g_AlphaThreshold` at shared-constants offset 528).
   - Alpha test only supports alpha ≥ ref (`kGreater` is treated the same). Never skips the draw.
   - The title screen uses `07060706` (standard alpha) and alpha test "always".
3. **Tiled textures:** untiled on upload with a copy of the SDK's `GetTiledOffset2D`. The game target doesn't link `pipeline/texture/util.cpp`.
   - Small textures (16 pixels or less) whose base level is packed into the mip tail are still placeholders.
4. **Render targets, Resolve and Clear:**
   - Each EDRAM colour surface (`RB_COLOR_INFO` base and format plus `RB_SURFACE_INFO` pitch) is its own RGBA8 Vulkan image, 1024 rows high. The back buffer is base 0 with pitch 1280.
   - Render passes start on demand and switch when the target changes. A target's first pass clears it; later passes load its contents.
   - The viewport comes from `PA_CL_VPORT_*` when `PA_CL_VTE_CNTL` bits 0–3 are set.
   - Resolve (`82BA2370`) is a GPU `vkCmdCopyImage` into a texture keyed by the destination texture's base address. `GetTexture` checks resolved textures first.
   - `vkCmdCopyImage` is loaded through `vkGetDeviceProcAddr`, because the SDK's function table doesn't include it.
   - Clear (`82BA7AA0`) clears colour only, with `vkCmdClearAttachments`.
5. **Indexed draw path (`82BA41C8`):** connected (streams 0–3, 16- and 32-bit indices, base vertex), **but never run**: the title screen makes no indexed draws.
6. **Texture cache fix (this brought the logo back):** the cache entry is keyed by address, format, size and swizzle, and stores a sampled content hash (64 bytes from every 4 KB of the base level).
   - If the contents change, the texture is uploaded again. The game reuses memory for new textures of the same size and format, so without this the logo showed a stale image.
   - Replaced images go to `retired_` and are freed at the next `BeginFrameIfNeeded`, after the frame fence.
   - On the title screen the cache stays at 33 textures.

## Confirmed device layout (Xbox D3D, XDK 21256)

| What | Where |
| --- | --- |
| VS / PS float constants | device `+0x780` / `+0x1780` |
| Texture fetch constants | `+0x480 + slot*0x18` |
| Vertex fetch constant for stream s | `+0x778 - s*8` (word 0: physical address; word 1: size in bytes, low 2 bits endian) |
| Stream stride (dwords, byte) | `+0x3268 + s` |
| Stream buffer object | `+0x3224 + s*4` |
| Current index buffer object | `+0x320C` (data address at `+0x18`; 32-bit indices if bit 31 of word 0 is set) |
| Current VS / PS | `+0x3310` / `+0x330C` |
| Vertex declaration | `+0x2FD0` |
| `RB_SURFACE_INFO` / `RB_COLOR_INFO` / `RB_DEPTH_INFO` | `+0x2880` / `+0x2884` / `+0x2888` |
| `RB_COLOR_MASK`, `RB_ALPHA_REF` | `+0x28DC`, `+0x2904` |
| `PA_CL_VPORT_XSCALE…YOFFSET` (floats) | `+0x2908…+0x2914` |
| `RB_DEPTHCONTROL` / `RB_BLENDCONTROL0` / `RB_COLORCONTROL` | `+0x2934` / `+0x2938` / `+0x293C` |
| `PA_CL_VTE_CNTL` | `+0x294C` |
| Copy registers (`RB_COPY_CONTROL…DEST_INFO`) | `+0x2A18…+0x2A24` |
| Inline draws | `82BA3D08` begin(dev, prim, count, stride) / `82BA3CF8` end |
| Indexed draw | `82BA41C8(dev, prim, baseVertex, startIndex, indexCount)` |
| Resolve | `82BA2370(dev, flags, rect, destTexture)`; the texture header (fetch constant) is at `destTexture+0x1C` |
| Clear | `82BA7AA0(dev, count, rects, flags, color)`; flags bits 0–3 = colour targets |
| Present | `82BA5FA8` |

**Title screen per frame:**
- 15 inline quads.
- Target A: EDRAM base 1536, pitch 1040, viewport 1024×512. It's resolved into the tiled 1024×512 texture at `1F929000`, with rect `(0,0,528,512)`.
- The back buffer is resolved to the front buffer at `115D5000`.
- The emulator also shows 24 untextured single-point draws (`prim=1`) that the native path doesn't receive. They don't affect what's on screen.

## Key files

- **`src/native/native_renderer.cpp`:** the game-side hooks: resource tracking, `ReadDeviceState`, inline draws, `NativeOnIndexedDraw`, `NativeOnResolve`, `NativeOnClear`, and once-per-combination logging of render targets and resolves (`FirstTime`).
- **`src/native/native_vulkan.cpp`:** the Vulkan core:
  - render targets (`GetTarget`, `BindTarget`, `EndPass`, `SetDrawViewport`), `Resolve`, `Clear`;
  - texture cache (`GetTexture`, `SampleContents`, `CreateTexture` with untiling, `Retire`/`FreeRetired`);
  - pipelines keyed by shaders, declaration, stream strides, topology and render state.
- **`src/native/native_draw.h`:** `NativeDrawState` (4 streams, indices, base vertex, render state, target and viewport registers) and the entry points.
- **`src/renderer_hooks.cpp`:** the `API_COUNT` hook forwards Resolve and Clear to the native side, and `TRACE_DRAW` forwards `82BA41C8`. Tracing: `--wwe2k15_trace_file`, `--wwe2k15_trace_start_frame` and `--wwe2k15_trace_frames`.
- **Draw dump:** `--wwe2k15_native_dump_draws=N` logs the state of the first N inline draws.

## Next steps

1. **Loading-tips screen looks zoomed in** (`out/texhash-test/shot-60s.png`): the text is cut off on the right and the logo sits low. It shows up after the title screen, about 60 s in. Compare with the emulator; the viewport or render-target size is the likely suspect.
2. **Write this session's findings into `native-renderer.md`** (as section 20.5 onwards).
3. **Indexed draws:** test them in menus or the demo match. Only 32-bit float and 8888 vertex formats are converted; formats with 16-bit fields need different byte swapping.
4. **B5, the 3D demo match:** a depth buffer (depth is only logged now), about 50 resolves per frame, MSAA, and blend constants.
5. **B6:** menus and a playable match, plus a Charge option.

## Watch-outs

- **Windows `DrawState` macro:** `WinUser.h` defines `DrawState`, which is why the struct is called `NativeDrawState`.
- **Presenter image:** the presenter's output image has no `TRANSFER_DST`, so it must be drawn into, never cleared or copied into.
- **Vulkan device features:** the SDK's device lacks `bufferDeviceAddress` and `shaderInt64`. That's the reason for the binding-model patch in `tools/xenosrecomp/wwe2k15.patch`.
- **Guest memory reads:** reading guest memory past a texture can hit unmapped pages and crash. That happened with an over-estimated hash size.
- **Image layout:** render-target images stay in `SHADER_READ_ONLY_OPTIMAL` between passes.
- **Test script:** `shot-test.ps1` in the old session's scratchpad. It runs the game, captures with `PrintWindow` (works in the background), then closes the game. Warn the user before they play during a test.
