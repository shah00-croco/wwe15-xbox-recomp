# Native renderer for WWE 2K15 — working notes

This document records how we investigate and build a native renderer for the
WWE 2K15 recompilation. Each step lists what we looked at, the commands we used,
what we found, and what is still open. Someone else should be able to repeat
every step from these notes.

Words used here:

- **Confirmed**: we checked it directly, in the disassembly, the SDK source, or
  a log.
- **Hypothesis**: our best guess, not checked yet.

## 1. The goal in simple terms

Today the game runs like this:

1. The recompiled game code calls the Xbox 360 Direct3D library. This library
   is part of the game executable (it is linked statically).
2. Direct3D writes GPU commands (called PM4 packets) into a ring buffer in
   memory.
3. ReXGlue's `xenos` GPU plugin reads those packets and emulates the Xbox 360
   GPU ("Xenos") on Vulkan or Direct3D 12. It also translates every shader when
   it is first used.

A native renderer skips steps 2 and 3. We replace the game's Direct3D
functions with our own C++ functions. These call a modern API (Direct3D 12 or
Vulkan) directly. The shaders are converted ahead of time, not while the game
runs.

Unleashed Recompiled (the PC port of Sonic Unleashed) did exactly this:

- `UnleashedRecomp/gpu/video.cpp` replaces 42 of the game's D3D functions with
  `GUEST_FUNCTION_HOOK(sub_XXXXXXXX, HostFunction)`. Examples are
  `CreateDevice`, `SetTexture`, `DrawIndexedPrimitive` and `Present`.
- When the game creates the device, Unleashed fills the device's render-state
  function table with its own functions.
- The tool XenosRecomp converts the Xbox 360 shaders to HLSL. DXC then compiles
  that HLSL to DXIL (for D3D12) and SPIR-V (for Vulkan).
- The project draws through the `plume` graphics layer.

Why do this? It is faster, because there is no packet parsing, no EDRAM
emulation and no shader compiling during play. It also gives cleaner output, and
makes it easy to add higher resolutions and PC features. The cost is large: we
must understand how this game uses D3D, and write a renderer for it by hand.

## 2. Step 1 — Is the current boot stable? (2026-11-02)

**What we did:** We checked the game process that was started at 08:11 with
`--mnk_mode=true`. We also searched its rotating logs for fatal errors and took
a screenshot.

```powershell
Get-Process -Id <pid>      # Responding = True after 80 minutes
```

```bash
grep -c FATAL out/build/win-amd64-release/logs/wwe2k15_005*.log   # 0 in every file
```

**Results:**

- **Confirmed:** After 80 minutes there was no fatal error. The attract-mode
  demo match was still running at about 88 FPS.
- **Confirmed:** The logs grow by about 100 MB every 10 minutes. Almost all of
  it is the `Texture fetch constant ... has "invalid" type!` warning.
- **Confirmed:** The log also contains 27 errors of
  `Resolve region is empty`, each followed by
  `PM4_DRAW_INDX_2(3, 8, 2): Failed in backend (... edram_mode=6)`.
- **Hypothesis:** In the screenshot the wrestlers look very dark, almost like
  shadows. This may be a rendering bug, or it may be the game's lighting. We
  have not compared it with real hardware.

## 3. Step 2 — Can ReXGlue replace functions and the GPU backend?

**What we read:**

- Wiki pages: *Function Overrides*, *Mid-ASM Hooks*, *Runtime Architecture
  Overview*, *ReXApp*.
- SDK source (v0.10.0, `c94f5eb`) at `D:\narutoxbox\.tools\rexglue-sdk`:
  - `include/rex/hook.h`
  - `include/rex/runtime.h`
  - `include/rex/rex_app.h`
  - `include/rex/system/interfaces/graphics.h`
  - `include/rex/system/gpu_plugin.h`
  - `src/codegen/config.cpp`
  - `src/codegen/function_graph.cpp`
- Generated header: `generated/default/wwe2k15_pch.h`.

**Results:**

1. **Whole-function replacement — confirmed in the SDK source and on the
   wiki.**
   - Every generated function is a *weak* alias of a strong `__imp__sub_XXXXXXXX`
     symbol. See `DEFINE_REX_FUNC` in `wwe2k15_pch.h`.
   - If our code defines `sub_XXXXXXXX` with `REX_HOOK(sub_X, fn)` or
     `REX_HOOK_RAW(sub_X)`, the linker uses ours. All callers then reach our
     function, including indirect calls through the function table.
   - `__imp__sub_XXXXXXXX` still points to the original, so a hook can call the
     original.
   - This works like Unleashed's `GUEST_FUNCTION_HOOK`.
   - **Not verified yet:** whether this override works on Windows with our
     toolchain (clang and lld-link, COFF weak externals). The SDK uses these
     macros for kernel exports, but neither the WWE project nor the Naruto
     project overrides a game function yet. The first test build must check
     this.
2. **Mid-function hooks — confirmed in the SDK source and on the wiki.** The
   manifest accepts `[[midasm_hook]]` entries (`src/codegen/config.cpp`, line
   310). Each entry takes `address`, `name`, `registers`, `return`,
   `jump_address`, the conditional forms and `after_instruction`. Codegen
   inserts a call to our function at that instruction.
3. **Replacing the GPU backend — confirmed in the SDK source. The wiki only
   says "swap graphics backends" in `OnPreSetup`.**
   - `RuntimeConfig::graphics` accepts any `IGraphicsSystem`. The comment in
     `runtime.h` shows `REX_GRAPHICS_BACKEND(MyCustomGraphicsSystem)`.
   - `ReXApp` also has a documented **"Detached overlay mode ('bring your own
     renderer')"**. If `OnPreSetup` clears `config.graphics`, the app returns
     its own `ui::ImmediateDrawer` from `OnCreateImmediateDrawer()`. ImGui and
     the overlay then draw through our renderer.
   - The comment in `rex_app.h` even names "the guest D3D device-creation hook"
     as the place where the GPU device may come up.
   - In that mode we own presentation, vsync and letterboxing.
   - The *Runtime Architecture Overview* page marks `IGraphicsSystem` as
     "in-flux", so this interface may change in later SDK versions.

**Decision:** The SDK already supports this approach. We do not need to
change the SDK.

## 4. Step 3 — How does WWE 2K15 reach the GPU?

**Tools:**

- `callgraph.py` reads `generated/default/*.cpp` (read-only) and builds a static
  call graph of 81,642 functions. `callers.py` walks it upwards.
- `dis.py` disassembles the private section dump `.tools/dump/sections.bin`
  with Capstone 5.0.9.
- `pm4scan.py` finds code that builds PM4 type-3 headers: `lis rX,0xC0xx`
  followed by `ori`/`addi`, where bits 8–14 are a known PM4 opcode.
- `tables.py` and `stride.py` find tables of function pointers in the data
  sections.
- `xexlibs.py` prints the XEX static-library list (optional header
  `0x000200FF`).

These scripts are in the session scratchpad. They should be copied to
`tools/renderer-research/` if this work continues.

### 4.1 Library version

**Confirmed** with `xexlibs.py assets/default.xex`: the game links `D3D9`,
`D3DX9` and `XGRAPHC` from XDK **2.0.21256.0**. Function addresses from
Unleashed Recompiled (Sonic Unleashed) cannot be reused, because it is a
different game and a different link.

### 4.2 Where the D3D code is

**Confirmed:** Every PM4 packet header built in code is in
`0x82B9B4B8–0x82BBD4D0` (98 header builds in 51 functions).

**Hypothesis:** The D3D library lies roughly between `0x82B9B000` and
`0x82BC8000`. We have not found its exact edges.

### 4.3 The device object

Most D3D functions take the device in `r3`. **Confirmed** from disassembly:

| Offset | Meaning | Evidence |
| --- | --- | --- |
| `+0x00 … +0x28` | 64-bit "dirty" masks: which state must be sent to the GPU | `82BA3D08` reads them and calls the upload helpers. The state setters `OR` bits into `+0x10`. |
| `+0x30` | ring-buffer write pointer | `82BA3740` writes packets at `0x30(r31)` and stores the new end back |
| `+0x38` | ring-buffer limit | compared with `+0x30` before writing |
| `+0x780`, `+0x1780` | vertex and pixel shader float-constant shadows | uploaded with register bases `0x4000` / `0x4400` in `82BA3D08` |
| `+0x2920 …` | render-state register shadows | uploaded with register bases `0x2180`, `0x2200` |

**Confirmed:** `sub_82BA9690` makes room in the ring buffer. 70 functions call
it, and all of them are in `0x82B9B4B8–0x82BBD4D0`.

**Hypothesis:** The global at `0x838D8418` holds the device pointer.
`sub_82AA6CC8` loads `r3` from it before it draws. We have not counted its
other users yet.

### 4.4 Key functions found

| Address | What it does | Confidence and evidence |
| --- | --- | --- |
| `82BA4F40` | **CreateDevice** (outer) | Hypothesis: it calls `82BB3588`, which calls `VdInitializeEngines` (through `82BB2F58`), `VdInitializeRingBuffer`/`VdEnableRingBufferRPtrWriteBack` (through `82BA9A10`, which also builds `PM4_ME_INIT`) and `VdRetrainEDRAM`. Called from game code `82A9ABA8` and `82B19F00`. |
| `82BB3130` | device reset or re-init | Hypothesis: it calls `VdShutdownEngines`, `VdSetGraphicsInterruptCallback` and the ring init. |
| `82BA5FA8` | **Swap / Present** (inside) | Confirmed: it is the only caller of `VdSwap` and `VdGetSystemCommandBuffer`. Callers: `82B15408`, `82BA23C0`, `82BA6710`, `82BBCE98`. |
| `82BA41C8` | **draw (indexed or not)** | Confirmed: it builds `0xC0012201` = PM4 `DRAW_INDX`, 2 dwords, predicate bit set. |
| `82BA3D08` / `82BA3CF8` | **begin / end inline-vertex draw** (like `DrawPrimitiveUP`) | Confirmed: `82BA3D08` flushes dirty state, writes `DRAW_INDX` and saves the ring pointer at `+0x3614`. `82BA3CF8` restores `+0x30` from `+0x3614`. Each has the same 6 outside callers. |
| `82BA4DC8` | draw during device init | Confirmed: it builds `DRAW_INDX` and is called only from `82BB3588`. |
| `82B9F708`, `82B9F860`, `82BA38A0`, `82BA6828`, `82BB3FD8`, `82BBB1D8` | built-in helper draws (clear, resolve or blit) | Confirmed: they build `DRAW_INDX_2` (vertices inside the packet), most with `IM_LOAD_IMMEDIATE` (shader code inside the packet). Which one is Clear and which is Resolve is not checked yet. |
| `82BB0D40` | loads a shader into GPU instruction memory | Confirmed: it is the only builder of `PM4_IM_LOAD`, called during the state flush. |
| `82BA3740` | sends three `INDIRECT_BUFFER` packets from device fields `+0x3770…+0x3B78` | Confirmed packet type. Its purpose is a hypothesis: shader or constant buffers. |
| `0x835BDA30` (`.data`) | **render-state function table** | Confirmed: 91/111 entries of 12 bytes, each `{getter, setter, flags}`. Setters are small functions near `0x82B9B7A0–0x82B9BF60`. They update a register shadow and set a dirty bit at `+0x10` (for example `82B9BEC8` / `82B9BF08`). This matches Unleashed's `setRenderStateFunctions`. The exact first entry is not fixed yet. |
| `0x835BDE7C` (`.data`) | **sampler-state function table** (hypothesis) | 20 entries of 12 bytes pointing to `0x82B9D040–0x82B9D890`. This matches Unleashed's sampler table, which also has a 12-byte stride. |

### 4.5 The game's own render layer — an important finding

Only 78 D3D functions are called from game code at all, and each by only a few
call sites.

**Confirmed:** `sub_82AA7EB0` is a `switch` over **105 command types**: it does
`cmplwi r11,0x68`, then a jump table at `82AA7EDC`. It calls into D3D, and it is
reached from `82AA87A0`, a function with no static callers that also calls
Present. So the game records its own render commands and plays them back on a
render thread.

**Hypothesis:** That thread entry is `82AA87A0`. `82AA6CC8` is the case for
"draw indexed": it reads primitive type and counts from a command record at
`+0x10…+0x1C` and calls `82BA41C8`.

**Why this matters:** We have two possible levels to hook:

- **(a) The D3D level**, like Unleashed. This is general, but on this XDK most
  state is set *inline* in game code (it writes device fields and dirty bits
  directly). So we must also take over the render-state table and the dirty-flag
  flush.
- **(b) The game's 105 render commands.** This is closer to the meaning of
  each draw, but specific to Yuke's engine. Each command must be understood.

## 5. Step 4 — Shaders

**Tools:**

- XenosRecomp source and README, read through the GitHub API.
- `shaderscan.py` searches the game files for the shader container magic
  that XenosRecomp uses (`flags & 0xFFFFFF00 == 0x102A1100`).
- A small parser for the SDK shader cache `out/cache/shaders/shareable/545408B6.xsh`.

**Results:**

- **Confirmed:** XenosRecomp is under the **MIT** licence. It converts Xbox 360
  shader containers to HLSL, and DXC then compiles that to DXIL or SPIR-V. Its
  README warns:
  - "Do not expect the recompiler to work out of the box."
  - Some features are not implemented: dynamic register indexing, integer
    constants, 1D textures, mini vertex fetch, several texture-fetch options,
    memory export and point size.
  - It contains Sonic-Unleashed-specific code under `UNLEASHED_RECOMP`.
- **Confirmed:** Unleashed Recompiled is **GPL-3.0**, so copying its renderer
  code would make our project GPL-3.0 as well. `plume` is **MIT**. The ReXGlue
  SDK is BSD-3-Clause.
- **Confirmed:** None of the game files contains a plain shader container: 0
  hits in the 7.7 GB, without `.bk2` videos, and 0 in the executable. The
  `.pac` archives are either `PACH` containers (`_startup.pac`) or start with
  `0F F5 12 ED`.
  - **Hypothesis:** `0F F5 12 ED` is an XMemCompress (LZX) stream, so the
    shaders are compressed inside the archives.
- **Confirmed:** The SDK shader cache recorded **127 shaders (61 vertex, 66
  pixel)** after the title screen and demo match. These entries hold only raw
  microcode, without the constant table and interpolator data that XenosRecomp
  needs.

**Options for getting the shaders:**

1. Write a reader for `.pac` and XMemCompress (LZX), then scan the
   decompressed data with XenosRecomp. This finds all shaders, but we must
   reverse-engineer Yuke's archive format.
2. Hook the D3D *create shader* functions and save each shader container the
   game passes in. This is easy, but it only finds shaders the game actually
   uses during the play-through.
3. Use the SDK's own Xenos translators (`src/graphics/pipeline/shader/
   dxbc_translator*.cpp`, `spirv_translator*.cpp`, BSD-licensed) offline on
   the microcode. They already handle this game, because it renders today. But
   they emit code that expects Xenia's emulated register layout, not a
   clean native pipeline.

## 6. Step 5 — Xbox-only GPU features

| Feature | Used by WWE 2K15? | Evidence |
| --- | --- | --- |
| EDRAM render targets | Yes, always on Xbox 360 | All rendering goes through EDRAM. `VdRetrainEDRAM` is called at device init. |
| Resolves (EDRAM → texture copies) | **Confirmed** | The log shows draws with `edram_mode=6` (copy mode) and 27 `Resolve region is empty` errors. |
| Predicated tiling | **Hypothesis: yes** | The draw packets have the predicate bit set (`0xC0012201`). `82BA3D08` also writes bin-mask packets (`0xC0006000`). |
| Tiled textures | Hypothesis: yes | This is the normal Xbox 360 texture layout. We have not checked the fetch constants yet. |
| Memory export (memexport) | Unknown | We must parse the 127 cached shaders for export instructions. |
| Shader instruction memory loads (`IM_LOAD`) | Confirmed | `82BB0D40` |

**What must be written by hand (first estimate):**

- A host renderer on `plume` (MIT), or on SDK-free D3D12 or Vulkan.
- Hooks for:
  - CreateDevice and Present
  - Creating, locking and unlocking textures, vertex buffers and index buffers
  - Shader creation
  - The draws
  - Resolve and Clear
  - Render targets and depth
  - Viewport and scissor
- A replacement for the render-state table and the dirty-flag flush.
- Untiling and format conversion for textures.
- Emulation of resolves with render-to-texture copies.
- The shader pipeline: extraction, conversion and a pipeline cache.

**Rough size:** Unleashed's `video.cpp` alone is 7,882 lines. Expect weeks of
focused work before the first native frame, and much longer before every game
mode renders correctly.

## 7. Open questions

1. ~~Does overriding a weak function work in our Windows build?~~ Yes — see
   section 9.
2. Where exactly do the render-state and sampler tables start, and which
   function copies them into the device?
3. Is `82AA87A0` the render thread, and what do the 105 command types mean?
4. Which helper draw is Clear and which is Resolve?
5. What is the `.pac` format, and is `0F F5 12 ED` really XMemCompress?
6. Do any shaders use memexport?
7. Are the dark wrestlers an emulation bug? Probably not: in the Phase 2 run
   (section 9) the wrestlers in a different demo match are lit normally, so
   the earlier dark picture was probably that scene's lighting. Not compared
   with real hardware.

## 8. Proposed plan (needs the user's approval before any code is written)

- **Phase 2 — proof of the hook mechanism** (1 day):
  - Add one logging-only `REX_HOOK_RAW` around `sub_82BA5FA8` (Present). It
    calls the original and counts frames.
  - Build it and check the log.
  - This proves that weak overrides work on Windows without changing any
    rendering.
- **Phase 3 — tracing** (a few days):
  - Logging hooks on CreateDevice, the draw functions, the render-state
    setters and the 105-case command switch.
  - Record one frame of the title screen and one frame of the demo match, so
    we know which commands and states are really used.
- **Phase 4 — shader capture**:
  - Hook shader creation and dump the containers to `out/shader-dump/`. This
    output is private and git-ignored, because it is game data.
  - Run XenosRecomp on them and record how many convert.
- **Phase 5 — first native frame**:
  - Detached mode (`config.graphics = nullptr`) and a plume device.
  - Draw only the title screen's 2D elements.

Each phase needs the user's approval for its file changes, builds and runs.

## 9. Step 6 — Phase 2: proof that hooks work (2026-11-02)

**What we changed:**

- New file `src/renderer_hooks.cpp`. It holds one hook,
  `REX_HOOK_RAW(sub_82BA5FA8)`, on D3D's present function. The hook counts
  frames, logs frame 1 and every 600th frame, and then calls the original
  `__imp__sub_82BA5FA8`. Rendering does not change.
- `CMakeLists.txt`: added `src/renderer_hooks.cpp` to `WWE2K15_SOURCES`.

**Build and run:**

```powershell
.\build-windows.ps1 -SdkPrefix D:\narutoxbox\.tools\rexglue-sdk\out\install\win-amd64-vulkan-test `
    -ToolsRoot D:\narutoxbox\.tools -Jobs 8
.\run-windows.ps1 -VulkanDevice 1 -Keyboard
```

**Results:**

- **Confirmed:** The build compiled the new file and linked with no
  duplicate-symbol error (`out/build-phase2.log`).
- **Confirmed:** Our function replaces the generated one, and the game calls it.
  The log (`logs/wwe2k15_006.log`) shows:

  ```
  [10:31:55.102] [info] [gpu] [t28992] [native-renderer] present hook: frame 1
  [10:32:05.177] [info] [gpu] [t28992] [native-renderer] present hook: frame 600
  [10:32:15.177] [info] [gpu] [t28992] [native-renderer] present hook: frame 1200
  ...
  [10:33:21.343] [info] [gpu] [t28992] [native-renderer] present hook: frame 4200
  ```

  So weak-function overrides work with clang and lld-link on Windows.
- **Confirmed:** The game presents exactly 600 frames per 10 seconds, which is
  **60 presents per second**, always from the same guest thread (`t28992`).
  The FPS counter in the overlay (about 80–120) measures the host, not the
  game.
- **Confirmed:** The demo match still renders as before, from a screenshot at
  about 60 seconds.
- **Confirmed:** `run-windows.ps1` works. It waits until the game exits.

**Decision:** The hook mechanism is proven. The next phase (tracing) can use
the same pattern on more functions.

**Update — a crash after about 4.5 minutes (not explained yet):**

- **Confirmed:** The Phase 2 run ended at 10:36:21 with exit code
  `-1073741819` (`0xC0000005`, access violation). The log shows:

  ```
  [10:36:13.475] [info] [gpu] [t28992] [native-renderer] present hook: frame 9000
  [10:36:21.985] [error] [sys] [t38992] Unhandled guest access violation: read of guest 0x000001A4 (host 0x00000001000001A4) on thread 0xF8000714
  ```

- The read at guest address `0x1A4` looks like a field at offset `0x1A4` read
  through a null pointer.
- It happened on host thread `t38992`. The present hook runs on `t28992`.
- The log does not name the guest instruction address.
- **Hypothesis:** The hook did not cause it. The hook only counts and then
  calls the original, and it runs on a different thread. But this is the first
  crash we have seen: the earlier run (without the hook) lasted more than 80
  minutes. So the hook is not ruled out.
- **Open question 8:** What is the guest PC of this crash? Next time, run under
  a debugger, or turn on more logging, to get a backtrace. Also run the same
  build with the hook removed for the same time, and compare.

## 10. Step 7 — Phase 3: tracing (2026-11-02)

### 10.1 Finding the state tables precisely

**What we did:** We disassembled the code that references the tables
(`lisaddi.py` found `0x835BDE80` built at `82BB29AC`/`82BB29B8`) and read the
loops in `sub_82BB2938`.

**Confirmed:**

- **Render-state table: `0x835BD9C0`, 101 entries of 12 bytes,
  `{getter, setter, default}`.** The loop at `82BB2960`:
  - copies the setter to device `+0x40 + i*4`;
  - copies the getter to device `+0x224 + i*4`;
  - then calls the setter with the default value.

  The D3D render-state value is `i * 4`. Entries 0–9 point to `82A1E600`,
  which is a single `blr` (an empty stub), so the first real state is index 10
  = `0x28`. This matches the XDK numbering that Unleashed Recompiled uses
  (`D3DRS_ZENABLE = 40`, `D3DRS_ZFUNC = 44`, ...).
- **Sampler-state table: `0x835BDE80`, 20 entries of 12 bytes,
  `{getter, setter, default}`.**
  - The setters are copied to device `+0x1D4`, the getters to `+0x3B8`.
  - Each setter is called as `setter(device, sampler, value)` for each of 26
    samplers.
- All 91 real render-state setters and 20 sampler setters are distinct
  functions, and all exist in the generated code.
- The function registry (`wwe2k15_register.cpp`) maps each of them to the weak
  name (`sub_...`), so the game's indirect calls reach our hooks.
- **The command player reads its command type from `**r4`.** At `82AA7EC4`:
  `lwz r11,0(r4); lwz r11,0(r11); cmplwi r11,0x68`. A normal function hook can
  therefore see every command, and no manifest change is needed.

The table dump is produced by `statetables.py` (scratchpad).

### 10.2 The trace hooks

- `src/renderer_hooks.cpp` now has hooks on:
  - present
  - CreateDevice
  - the command player `82AA7EB0`
  - the 9 draw builders, plus end-vertices `82BA3CF8`
  - all 91 render-state setters
  - all 20 sampler-state setters
- Every hook logs and then calls the original.
- Tracing is off unless these cvars are given:
  - `--wwe2k15_trace_file=<path>`
  - `--wwe2k15_trace_start_frame=<n>` (default 600)
  - `--wwe2k15_trace_frames=<n>` (default 2)
- The log gets one summary line per traced frame.

Run:

```powershell
.\run-windows.ps1 -VulkanDevice 1 -Keyboard -GameArguments @(
  '--wwe2k15_trace_file=D:\wwe2k15-recomp\out\trace\title-1800.txt',
  '--wwe2k15_trace_start_frame=1800', '--wwe2k15_trace_frames=2')
```

### 10.3 Results

- **Confirmed — CreateDevice arguments:**

  ```
  CreateDevice(r3=00000000 r4=00000001 r5=00000000 r6=00000001 r7=AFFD5874 r8=AFFD58F4) -> 00000000
  ```

  This is exactly the D3D9 signature `CreateDevice(Adapter 0, D3DDEVTYPE_HAL,
  hFocusWindow 0, BehaviorFlags 1, &PresentParameters, &pDevice)`, and it
  returns `D3D_OK`. `sub_82BA4F40` is now **confirmed** as CreateDevice.
- **Confirmed — frames 1800–1801** (a 2D screen just before a fade to black,
  shortly before the demo match). Each frame had:
  - 256 game render commands. Command records lie 16 bytes apart in one buffer
    (`0xAF3C0000`, `0xAF3C0010`, ...). The most common types are 80 (66 per
    frame), 21, 3, 4, 9, 8, 43 and 19.
  - 18 draws: 15 `BEGIN_VERTICES` and 3 `DRAW2` (`82BA6828`).
  - No `DRAW 82BA41C8` (indexed draws) at all.
  - The `BEGIN_VERTICES` calls use `r4=0x0D`, `r5=4`, `r6=0x0C` or `0x14`.
    **Hypothesis:** That is primitive type 13 (Xbox 360 `D3DPT_QUADLIST`),
    4 vertices, a stride of 12 or 20 bytes. These are 2D sprites drawn with
    inline vertices.
  - 768 render-state calls, but only **11 different states**: `0x28`, `0x2C`,
    `0x30`, `0x38`, `0x3C`, `0x48`, `0x4C`, `0x50`, `0x60`, `0x64`, `0x68`.
    These are the depth, cull, blend and alpha-test states. The game sets them
    again for every 2D element.
  - 18 sampler-state calls (types 0, 1, 4, 5, 6).
- **Not captured:** a frame of the 3D demo match. The second run (trace at
  frame 6000) crashed before it reached that frame. See 10.4.

### 10.4 The crash reproduces

- **Confirmed:** Run 2 crashed at 10:44:45, about 102 seconds after start
  (after frame 5400). The error was the same as in Phase 2:
  `Unhandled guest access violation: read of guest 0x000001A4 ... on thread
  0xF8000714`.
- So far:
  - **Hooked builds:** 2 of 3 runs crashed, at 267 s and at 102 s. The third
    (run 1) was stopped by us at about 2 minutes.
  - **Builds without hooks:** 0 of 2 runs crashed (more than 80 minutes, and
    more than 90 seconds).
- **Hypothesis:** The hooks, or the launch through `run-windows.ps1`, change
  timing enough to expose a race in the game or the runtime. All hooked runs
  were started with `run-windows.ps1`, and the older runs started the exe
  directly. A null pointer at offset `0x1A4` is not the hook code: our hooks
  never read guest offset `0x1A4`.
- **Next test (needs approval):**
  - Build once with `src/renderer_hooks.cpp` removed (only a relink is needed).
  - Run that build through `run-windows.ps1` for 10 minutes.
  - Compare. Then get the guest PC from a debug build or `lldb`.

### 10.5 Control test: the same build without hooks (2026-11-02)

**What we did:**

1. Removed `src/renderer_hooks.cpp` from `WWE2K15_SOURCES` for this test only,
   and relinked (`out/build-control.log`).
2. Started the game with exactly the same command as the crashing runs:
   `.\run-windows.ps1 -VulkanDevice 1 -Keyboard`.
3. Watched the process for 10 minutes.
4. Put the line back in `CMakeLists.txt` and relinked again
   (`out/build-restore.log`). The current exe has the hooks again.

**Result:**

- **Confirmed:** The build without hooks ran for **606 seconds with no crash**.
  The process was still alive, and the logs written after 10:50 contain no
  `access violation` or `FATAL`.
- The launcher is therefore not the cause.

**Score so far:**

| Build | Runs | Crashed |
| --- | --- | --- |
| With hooks (Phase 2: present only; Phase 3: all trace hooks) | 3 | 2 (at 267 s and 102 s); the third was stopped by us at about 2 minutes |
| Without hooks | 3 | 0 (80 min, more than 90 s, 10 min) |

**Hypothesis:** The hooks are involved. Even the Phase 2 build, which had only
the present hook (a counter plus a log line every 600 frames), crashed.

- This is not proven. The crash is intermittent, and there are few runs.
- Possible causes, none checked yet:
  - The hook changes timing on the render thread and exposes an existing race.
  - Replacing the function changes something the generated code depends on.

**Next step:** Find the guest instruction that crashes, using `lldb`
(`D:\narutoxbox\.tools\llvm\bin\lldb.exe`) or a build with debug info. Also
try a hook build with no logging at all.

## 11. Step 8 — The character-select crash: found with lldb (2026-11-02)

**New information from the user:** The game crashes when you choose a
character in 1 vs 1 mode. So the earlier crashes came from the user playing,
not from the hooks. In the control run (10.5) nobody reached character select.
**The hooks are most likely innocent.** The score table in 10.5 mixed
"someone played" with "has hooks".

### 11.1 Getting lldb to run

- LLVM's `lldb.exe` (20.1.8, in `D:\narutoxbox\.tools\llvm\bin`) needs
  `python310.dll`. Only Python 3.9 and 3.11 were installed, so it exited with
  code 53.
- With the user's approval we downloaded the official embeddable package
  `https://www.python.org/ftp/python/3.10.11/python-3.10.11-embed-amd64.zip`
  (8,629,277 bytes, SHA-256
  `608619F8619075629C9C69F361352A0DA6ED7E62F83A0E19C63E0EA32EB7629D`).
- We extracted it to `D:\wwe2k15-recomp\.tools\python310`. The
  `python310.dll` signature is valid (Python Software Foundation).
- Nothing was installed system-wide: no PATH or registry change.
- Start lldb with these variables for the command only:

```powershell
$py = 'D:\wwe2k15-recomp\.tools\python310'
$env:PATH = "$py;D:\narutoxbox\.tools\llvm\bin;$env:PATH"
$env:PYTHONHOME = $py
$env:PYTHONPATH = "$py\python310.zip;$py;D:\narutoxbox\.tools\llvm\lib\site-packages"
lldb -b -o "process launch -w <build dir>" -k "bt 60" -k "register read" `
     -k "image list wwe2k15.exe" -k "disassemble -s `$pc -c 16" -k "process kill" `
     -- <build dir>\wwe2k15.exe <same arguments as run-windows.ps1>
```

- On Windows, lldb gives first-chance exceptions to the program. The runtime's
  normal memory traps (MMIO, write watches) therefore do not stop it. lldb
  stopped only on the real, unhandled crash. Full output:
  `out/lldb-crash.txt`.

### 11.2 Turning host addresses into game functions

The Release exe has no debug symbols. But the generated table
`PPCFuncMappings[]` (`{u32 guest; host function pointer}`, 81,915 entries) is
stored in the exe. The script `hostmap.py` (scratchpad):

1. Finds this table in the file.
2. Sorts it by host address.
3. Maps each backtrace address to `sub_XXXXXXXX + offset`.

### 11.3 Result

**Confirmed from the lldb output:**

```
stop reason = Exception 0xc0000005 ... Access violation reading location 0x1000001a4
->  movl (%rdx,%rcx), %ecx    ; guest load, rcx = 0x1A4
    bswapl %eax
    movq %rax, 0x58(%rdi)     ; store to ctx.r11
r12 = 0x82BD5ACC              ; guest LR at the crash
```

Backtrace, mapped to guest functions (innermost first):

1. `sub_82BD5A80`
2. `sub_82BDBE00`
3. `sub_82BD39C0`
4. `sub_82BD3C58`
5. `sub_82BC1EA0`
6. `sub_82BC3990`
7. `sub_82BBF718`
8. `sub_82BBFEF0`
9. `sub_82AC6D20`
10. `sub_82A9D268`
11. `sub_82895F88`
12. `sub_82B2A748`
13. `sub_82B30210`
14. `sub_82B2D220`
15. `sub_822672F8` (the thread entry)

What happened, step by step:

- The crash is `lwz r11,420(r31)` at `0x82BD5AD4` in `sub_82BD5A80`, with
  guest `r31 = 0`.
- The same function had already read `420(r31)` successfully at
  `0x82BD5AA0`.
- `LR = 0x82BD5ACC` shows that the last call was `bl 0x82BD5820`. So that
  callee returned with `r31` destroyed. In the PowerPC ABI, `r31` is
  non-volatile: every function must preserve it.
- `sub_82BD5820` is a JPEG marker reader. It scans for `0xFF` bytes, stores the
  marker to `+0x1A4` (libjpeg's `unread_marker`), and calls the data source
  and the error handler through function pointers. So the
  `sub_82BD....` code looks like **libjpeg**. This is a hypothesis based on
  the code shape. The game probably decodes the wrestler images on the
  character select screen.

### 11.4 Root cause: setjmp/longjmp are not configured

libjpeg reports fatal errors with `longjmp`.

- **Confirmed `setjmp` = `0x833AF8F0`:**
  - It checks a hook pointer at `0x8397B180`.
  - It saves `lr`, `cr`, `r13`–`r31`, `f14`–`f31` and VMX registers to the
    buffer.
  - It is called from `sub_82BC1EA0`, which is **frame 5 of the crash
    backtrace**.
- **Confirmed `longjmp` = `0x833AF5D0`:**
  - `r6 = r4 ? r4 : 1`.
  - It reloads `f14`–`f31`, `r13`–`r31` (`ld r13,0x98(r7)` … `ld r31,0x128(r7)`),
    the VMX registers, `lr` from `+0x134`, `r1` from `+0x90` and `cr` from
    `+0x130`.
  - Then `mr r3,r6; blr`.
  - It is called from `sub_82BD33A0` (16 callers, probably libjpeg's error
    exit) and others.
- These were found with `jmpscan.py` / `jmpscan2.py` (scratchpad). The scripts
  look for functions with 18 `std`/`ld` of `r14`–`r31` through one base
  register.

The ReXGlue SDK handles non-local jumps only when the manifest names these
functions. With `setjmp_address` / `longjmp_address` set, codegen replaces the
calls with `ppc_setjmp` / `ppc_longjmp` (host `setjmp`/`longjmp`, keyed by the
guest buffer address). See `src/codegen/builders/context.cpp`, line 178.

`wwe2k15_manifest.toml` names neither. The recompiled guest `longjmp` therefore
restores the guest registers that `setjmp` saved, but its final `blr` only
returns to its *host* caller, not to the `setjmp` site. The JPEG code then
continues with registers from the `setjmp` moment. In this crash, `r31` was 0.

- **Confirmed:** the crash mechanism, the addresses and the missing manifest
  keys.
- **Hypothesis:** That a JPEG error happens on character select at all. It may
  come from a file the decoder cannot read, or from a different emulation bug
  that the `longjmp` then turns into a crash.

### 11.5 Proposed fix (needs approval)

```toml
[entrypoint]
# ...existing keys...
setjmp_address = 0x833AF8F0   # saves lr, cr, r13-r31, f14-f31, VMX (disassembly, section 11.4)
longjmp_address = 0x833AF5D0  # restores them and returns r4 (or 1)
```

**Wiki versus installed SDK:** The wiki page *rexglue CLI Configuration File*
shows these as top-level keys, as in the older single-binary config. In the
installed v0.10.0, `manifest.cpp` passes the `[entrypoint]` table to
`RecompilerConfig::LoadFromTable`, so the keys must go under `[entrypoint]`.

After the edit, codegen must run again and the generated code must be
recompiled. This is a full rebuild.

### 11.6 The fix applied (2026-11-02)

- `wwe2k15_manifest.toml`, `[entrypoint]`: added
  `setjmp_address = 0x833AF8F0` and `longjmp_address = 0x833AF5D0`, with a
  comment that points to this section. We checked it: no BOM, it parses, and
  it still has 314 boundaries.
- Rebuild: `build-windows.ps1` (log `out/build-setjmp.log`). CMake ran codegen
  again by itself. Because the partitions did not change, only the affected
  generated files were recompiled (8 build steps), not all of them.
- **Confirmed in the new generated code:**
  - `ppc_setjmp` in `sub_82BC1EA0`, `sub_82BC2C10` and `sub_82BD65B8`.
  - `ppc_longjmp` in `sub_82BC00F0`, `sub_82BC01F8`, `sub_82BD33A0` and
    `sub_82BD33F0`.
  - These are exactly the callers that the static call graph found for the two
    functions.
- **Confirmed:** The build links and boots, and presents frames under lldb.
- **Not verified yet:** whether character select in 1 vs 1 now works. The user
  is testing it under lldb (output: `out/lldb-crash2.txt`).

### 11.7 Result of the fix, and the slow-down report (2026-11-02)

- **Confirmed by the user:** With the fix, they chose both characters in 1 vs 1
  and reached the match loading screen. Before the fix, this crashed. The run
  (`wwe2k15_011`, under lldb) was still alive at 12:50, with no
  `access violation` in its logs.
- **User report:** The game "stuck for a longer time then continue" at
  character select and on the loading screen.

**What the log shows** (present-hook lines, one every 600 frames; read-only):

| Time | Seconds per 600 frames | Frames per second | Main log content |
| --- | --- | --- | --- |
| 12:37–12:38 | 10 | 60 | normal |
| 12:38:20–12:43:10 | about 46 | about 13 | about 50,000 `Texture fetch constant ... "invalid" type!` warnings per minute |
| 12:43:10–12:43:40 | 10 | 60 | warnings stop |
| 12:44:05–12:46:40 | about 47 | about 13 | warnings again, about 48,000 per minute |
| 12:46:40–12:47:13 | 10 | 60 | warnings stop |
| 12:47:13–12:49:12 | 120 | 5 | loading: no warnings, new graphics pipelines |

So the game does not freeze. It slows down, and the slow stretches match the
warning flood exactly.

**Why the warnings are expensive** (SDK source, confirmed):

- `src/core/logging.cpp`, line 152: on Windows every log line also goes to an
  `msvc_sink` (OutputDebugString), and this sink stays for the whole run.
  - **Hypothesis:** While a debugger is attached, each OutputDebugString is a
    debug event that pauses the game until lldb handles it. That is about 65
    events per frame.
- `include/rex/logging/types.h`, line 83: `flush_level = info`, so every
  warning is also flushed to the file at once.
- The warning itself comes from `TextureCache::BindingInfoFromFetchConstant`
  (`src/graphics/pipeline/texture/cache.cpp`, line 914). The texture binding is
  skipped and a line is logged for each such fetch, in every draw.
- **Hypothesis** for the loading-screen slow-down: under lldb, every
  first-chance exception from the runtime's memory traps (texture and buffer
  write watches) also goes to the debugger first.

**Next tests (each needs approval):**

1. The same path (1 vs 1, choose two characters, load) **without lldb**, with
   default logging. If the slow-downs mostly disappear, the debugger was the
   main cause.
2. If they remain: the same path with `--log_level=error`, to remove the
   warning flood. Note that the present-hook frame lines are `info`, so they
   would be hidden too.
3. A second run with the same `--cache_root`, to see whether pipeline creation
   on the loading screen gets faster once the pipelines are cached.
4. Only as an experiment: `--gpu_allow_invalid_fetch_constants=true`. It may
   change what is drawn.

**How the lldb run ended** (`out/lldb-crash2.txt`, log `wwe2k15_011`):

- After frame 15,600 (12:53:15), the present hook logged nothing more, so the
  game drew fewer than 600 frames in 10 minutes on the loading screen. During
  that time the log shows only new graphics pipelines (25 after 12:53).
- At 13:03:51 the log says `Window closing, shutting down...` and
  `Title terminated; hard-exiting process.` So the window was closed.
- lldb then reported `Exception 0x80000003` (a breakpoint exception) on thread
  `Vulkan Pipelines` with an invalid frame (`0xffffffffffffffff`).
  `register read` failed because the process was already gone.
- **Hypothesis:** This happened during the hard exit, not during play. No
  `access violation` appears in this run.
- So under lldb the loading screen was extremely slow (below 1 frame per
  second). Test 1 above (without lldb) is needed to know the real speed.

### 11.8 Test without lldb (run `wwe2k15_012`, 2026-11-02 13:05–13:09)

- Started with `run-windows.ps1 -VulkanDevice 1 -Keyboard`, without a
  debugger. Keys were sent with a small SendInput script (`keys.ps1`,
  scratchpad). The script checks that the game window is in front before it
  sends anything; the first try hit the wrong window, and the check now
  prevents that.
- The user has already accepted the agreements in an earlier session, so Start
  led directly to the menus.
- The window was closed at 13:09:03 (`Window closing, shutting down...`). The
  user plays through AnyDesk at the same time.

**Frame rate from the present hook (confirmed), and warning rate:**

| Time | Frames per second | `invalid fetch constant` warnings per second |
| --- | --- | --- |
| 13:05:20–13:06:00 | 60 | – |
| 13:06:10–13:06:52 | 55 → **20** | about 940–1,250 |
| 13:06:52–13:07:35 | 46 → 60 | none after 13:07:00 |
| 13:07:35–13:08:46 | **23–29** | about 360–720 |

**Conclusions:**

- **Confirmed:** Without lldb the slow-downs are smaller (20–29 fps instead
  of 5–13), but they are still real. The game itself presents fewer frames.
  This is measured inside the game, so AnyDesk cannot cause it. AnyDesk can add
  extra delay to what the user sees and to the input on top of this.
- **Confirmed:** The slow stretches again match the warning flood exactly.
- **Hypothesis:** The cost is the logging itself:
  - every warning is written to the debug-output sink (`msvc_sink`);
  - every warning is flushed to the file (`flush_level = info`).

  The skipped texture binding behind each warning may also cost a little.

**Next test (needs approval):** Silence only the GPU warnings and keep a frame
counter:

- `out/build/win-amd64-release/wwe2k15.toml` with `[log.levels] gpu = "error"`.
  `rex_app.cpp`, line 166, reads per-category levels from `<exe dir>/wwe2k15.toml`.
- Move the present-hook frame line to the `core` category, so it stays visible.

### 11.9 Flag test and the `PhysicalHeap::Alloc` error bursts (2026-11-02)

**Test of `--gpu_allow_invalid_fetch_constants=true`** (run `wwe2k15_013`,
no debugger, keys sent with `keys.ps1`):

- **Confirmed:** 0 `invalid fetch constant` warnings. In run `012` without the
  flag there were thousands.
- **Confirmed:** The screenshots (title, demo matches, loading tips) show no
  visible glitches. We did not compare the same frame pixel by pixel.
- **Confirmed:** The slow stretches remain *without* any warnings:

  | Time | Frames per second |
  | --- | --- |
  | 14:12:48–14:13:40 | 22–24 |
  | 14:14:21–14:15:09 | 24–26 |
  | 14:15:51–14:16:34 | 14 |
  | 14:16:34–14:19:48 | 19 → a steady **30** (20.0–20.8 s per 600 frames) |

- **Conclusion:** The warning flood was **not** the main cause of the
  slow-down. Hypothesis 11.7 (the logging cost) is therefore mostly wrong. It
  may still have added a little.
- **Hypothesis:** A steady 30 frames per second in a match is the game's own
  frame cap for matches. We have not compared with real hardware. The dips
  to 14–26 fps happen around demo transitions and loading.

**The `PhysicalHeap::Alloc unable to alloc physical memory` bursts** (the user
saw them in the log overlay):

- We added logging hooks on the allocation wrappers `sub_82261418` /
  `sub_822614A0`. Both call `MmAllocatePhysicalMemoryEx`. We also hooked their
  callers `sub_82261530` and `sub_8225F848`. The hooks are in
  `src/renderer_hooks.cpp`, run `014`.
- **Confirmed:** The failures come from `sub_8225E870` (and a similar function
  near `0x8225EB2C`/`0x8225EC14`). Disassembly shows it is a **free-memory
  check**:
  1. It allocates 1 MB blocks until one fails (at most 512), then 64 KB
     blocks until one fails (at most 16), and frees them all.
  2. Starting at `0x1E000000` (480 MB), it tries one block and steps down by
     1 MB per try until a block succeeds. Then it frees it.
  3. It stores the largest block and the total in the caller's structure
     (`stw r30,0(r23)`, `stw r10,4(r23)`).
- So hundreds of failed allocations are **by design**. Log of one check:

  ```
  [14:20:34.182] 82261418 failed #1:   r3=00100000 ... lr=8225E8FC   (end of the 1 MB loop)
  [14:20:34.248] 82261418 failed #7:   r3=1E000000 ... lr=8225E9DC   (480 MB try)
  [14:20:34.259] 82261418 failed #250: r3=0ED00000 ... lr=8225E9DC
  ```

- The SDK logs two `[error]` lines for every failed try (`xmemory.cpp`, lines
  1412 and 1906). That is about 1,000 lines per check.
- One check took about 0.1–0.2 s here. That is a small hitch, not the long
  slow-downs.
- **Possible fix (needs approval):** Replace `sub_8225E870` with a hook that
  computes the same two numbers directly from the guest physical heap, without
  hundreds of failing allocations. We must first check what the game uses the
  numbers for (its caller is `sub_8225EE08`).

### 11.10 What limits the speed during a dip (2026-11-02, run `wwe2k15_015`)

**Method:** `threads.ps1` (scratchpad) samples the CPU time of each thread of
the game process for 8 seconds, and the Windows counter
`\GPU Engine(*engtype_3D)\Utilization Percentage` for the process. Thread ids
are matched to log lines (`[tNNNN]`):

- `t35116` writes the present-hook lines, so it is the game's render thread.
- `t9212` writes `VulkanTextureCache` lines, so it is the GPU command
  processor.
- `t28772` runs the memory checks, so it is the game's main thread.

**Sample during a dip** (14:28:30, demo match, 600 frames took 29.8 s, about
**20 fps**):

| Thread | CPU (100% = one full core) |
| --- | --- |
| `t35116` game render thread | **90%** |
| `t9212` GPU command processor | **71%** |
| `t43644` (unknown) | 42% |
| `t28772` game main thread | 25% |
| whole process | about 320% (the PC has 12 logical cores) |
| GPU 3D engine, this process | **about 3%** |

**Conclusions:**

- **Confirmed:** During the dip the graphics card is almost idle. The limit is
  the CPU, mainly one thread: the game's render thread is close to a full
  core.
- **Hypothesis A:** The render thread does real work: recompiled game code plus
  the Xbox D3D library (state setup, command building). This would be the
  part a native renderer removes.
- **Hypothesis B:** The render thread spends its time *waiting* in a busy
  loop, for example a spin on the GPU fence or the ring read pointer. Busy
  waiting also shows as about 90% CPU. The command-processor thread at 71%
  makes this possible: the render thread may wait for it.
- **Not yet done:** a sampling profile of `t35116` to tell A from B, for
  example by attaching lldb a number of times and mapping the stacks with
  `hostmap.py`.

### 11.11 Profile of the render thread during a dip (2026-11-02, run `wwe2k15_016`)

**Method:**

- `sample.py` (scratchpad) uses lldb's Python API, with the portable Python
  3.10. It attaches once, stops the process 40 times (every 0.25 s), records
  the stack of the render thread, and detaches.
- The render thread is `t41520`, the thread that writes the present-hook
  lines.
- `profmap.py` maps each frame to a guest function through the
  `PPCFuncMappings` table in the exe.
- We sampled during a slow stretch of the demo match (14:31:29, about 600
  frames in 12–14+ s). The game was closed right after.
- **Caveat:** While lldb is attached, the runtime's memory-trap exceptions are
  slower, so the sampled stretch is slower than normal play. The proportions
  are still useful.

**Result (40 samples):**

| Where the render thread was | Samples |
| --- | --- |
| Game code (recomplied, incl. Xbox D3D) | 36 |
| Waiting in Windows (`ntdll`) | 4 |
| Inside D3D present `sub_82BA5FA8` (via `sub_82B15408` ← `sub_82A9B078`) | **17 (about 42%)** |
| of which leaf in `sub_82BA4738` | 12 |
| of which leaf in `sub_82BA8208` | 6 |
| The game's render-command player `sub_82AA86C8` / `sub_82AA7EB0` | 9 |

**Confirmed by disassembly: `sub_82BA4738` is a busy-wait for the GPU.**

- It contains a loop of PowerPC priority hints (`mr r1,r1`, `mr r31,r31`,
  `mr r2,r2`). These are used only in spin loops.
- It compares a value the GPU writes back (`lwz r8,0(r11)` with
  `r11 = *(device+0x2B10)`) with the expected value, and reads a timer
  (`sub_82261D78`) for a timeout.
- `sub_82BA8208` calls it, together with the ring-space function
  `sub_82BA9690`.

So about 40% of the render thread's time is D3D **waiting for the emulated GPU
to finish earlier work**. During that time the graphics card itself is about
3% busy (11.10), and the GPU command processor thread uses about 71% of a
core.

**Conclusion (hypothesis, to be confirmed):** The dips are limited by the
emulated GPU's command processing on the CPU, not by the graphics card and not
mostly by the game logic.

- This is exactly the part a native renderer removes.
- A shorter-term step is to profile the command-processor thread in the same
  way, to see what it spends time on. It may be texture uploads, EDRAM or
  resolve handling, pipeline lookups, or the CPU vertex-shader run for
  unclipped draws (`execute_unclipped_draw_vs_on_cpu_for_psi_render_backend`,
  default `true`).

### 11.12 Profile of the GPU-emulation thread (2026-11-02, run `wwe2k15_017`)

**Method:** The same as 11.11 (`sample.py`, 40 samples, 0.25 s apart) on the
command-processor thread `t33076`. This is the thread that writes the
`VulkanTextureCache` lines. We sampled during a slow stretch of the demo. The
game was closed right after.

**Result** (`symcp.py`; `rexgpu-xenos.dll` has no PDB, so its own functions
show only as the nearest export `rex_gpu_create`):

| Where the thread was | Samples |
| --- | --- |
| Waiting in Windows (`ntdll`, called from the GPU plugin) | 17 (about 42%) |
| NVIDIA Vulkan driver (`nvoglv64.dll`), partly under `VulkanPresenter::RefreshGuestOutputImpl` | 7 (about 18%) |
| GPU plugin code (emulation work, function names not available) | 15 (about 38%) |
| Runtime | 1 |

**Reading:**

- **Confirmed:** The emulation thread is *not* saturated. It waits about 40% of
  the time.
- **Confirmed** from the SDK source:
  - `--vsync` (default `true`, `src/graphics/command_processor.cpp`, line 39)
    drives an emulated 60 Hz vblank (`graphics_system.cpp`, line 164).
  - The command processor serves the game's `WAIT_REG_MEM` with
    `Sleep(wait / 0x100 ms)` (`command_processor.cpp`, line 1059).
- **Hypothesis:** The frame rates we measured (60, 30, about 20–24) look like
  60, 60/2 and 60/3. The game paces itself on the emulated vblank. When the
  chain "game render thread → command processor → Vulkan → present" misses a
  vblank, the frame waits for the next one. So the cause would be the *latency*
  of the emulated GPU path, not raw throughput. Millisecond sleeps make this
  worse. Not proven; finding out would need plugin symbols, a RelWithDebInfo
  build of the SDK.

**Decision for the project:** The user wants native rendering on the GPU,
not more tuning of the emulator. The measurements show that the cost lies in the
emulated GPU path (packets, command processor, waits, presenter). A native
renderer removes that path completely. So we return to the native-renderer
plan (section 8, Phase 4: shader capture). The diagnostic hooks for physical
allocations in `src/renderer_hooks.cpp` (11.9) can be removed once they are no
longer needed.

## 12. Phase 4 — shader capture (2026-11-02)

### 12.1 Where the shaders are

- D3D does not check the container magic `0x102A11xx`: no `lis`, `cmpwi` or
  `cmplwi` with `0x102A` exists in the code (`immscan.py`, `cmpscan.py`).
- The texture pair `sub_82B9AFA8` / `sub_82B9B0C8` turned out to be
  CreateTexture-like: a 0x34-byte object plus `sub_82B9A7E0`, which builds the
  texture header.
- **Confirmed:** The state flush `sub_82BB0D40` (the only `IM_LOAD` builder):
  - reads the current vertex shader from device `+0x3310` and the pixel shader
    from `+0x330C`;
  - takes the code address from the shader object's physical base `+0x18`,
    plus an offset found through `+0x40` / `+0x28`.

  So the D3D shader objects point to the raw code only. The game files hold
  the full containers compressed in `.pac` archives (section 5).

### 12.2 Collecting the containers from memory

**Code:** `src/renderer_hooks.cpp`, `ScanGuestMemoryForShaders`, called from
the present hook every `--wwe2k15_shader_dump_interval` frames (default 600)
when `--wwe2k15_shader_dump_dir` is set. It:

1. walks the committed, readable regions of the physical heap (through the
   unwatched physical view) and of the virtual heaps `0x40000000`,
   `0x80000000` and `0x90000000`;
2. applies XenosRecomp's own validity test: big-endian flags `0x102A11xx`,
   `virtualSize + physicalSize` inside the region, fields `0x1C` and `0x20`
   equal to 0;
3. saves each unique container (FNV-1a 64-bit hash) as `<hash>.bin`.

The scan is guarded with SEH.

**Run** (`wwe2k15_018`, title and demo, about 3 minutes, game closed after):

```
.\run-windows.ps1 -VulkanDevice 1 -Keyboard -GameArguments @(
  '--gpu_allow_invalid_fetch_constants=true',
  '--wwe2k15_shader_dump_dir=D:\wwe2k15-recomp\out\shader-dump',
  '--wwe2k15_shader_dump_interval=600')
[14:46:36.826] shader scan: 1796 containers in memory, 1366 new, 1366 saved in total
[14:46:46.995] shader scan: 1796 containers in memory, 10 new, 1376 saved in total
...
[14:49:32.562] shader scan: 1796 containers in memory, 83 new, 2055 saved in total
```

**Confirmed results:**

- There are always 1,796 containers in memory. The first scan saved 1,366
  unique ones: 1,095 have flags `0x102A1100` and 960 have `0x102A1101`
  (counted over all 2,055 files).
- All 689 files saved by later scans are near-copies of first-scan
  containers. They are the same size, the first 0x20 bytes are identical, and
  they differ in at most 64 bytes, **all inside the physical (shader code)
  part**.
- **Hypothesis:** D3D patches vertex-fetch instructions into the code when a
  vertex layout is bound. So the true number of distinct shaders seen is
  about 1,366.
- The dump (`out/shader-dump`, 4.1 MB) is game data. It is git-ignored and must
  never be shared.

### 12.3 XenosRecomp

- Cloned to `.tools/XenosRecomp` at
  `990d03b28a27b50277ee5d8d942e1c5f873869d1` (2025-08-03), with its pinned
  submodules: fmt, smol-v, xxHash, zstd and dxc-bin. This is 441 MB.
- **Confirmed:** All DXC binaries in `thirdparty/dxc-bin/bin` (`dxc.exe`,
  `dxcompiler.dll`, `dxil.dll` for x64, x86 and arm64) have valid Microsoft
  Corporation signatures.
- Usage: `XenosRecomp <input path> <output path> <shader common header>`.
- **Not done:** Building XenosRecomp was blocked by the session's permission
  check, because it compiles and later runs code downloaded from the internet.
  It needs the user's explicit approval. The build script is ready (MSVC +
  Ninja, Release, output in `.tools/XenosRecomp/build`).

### 12.4 Building XenosRecomp (approved by the user)

- The build script (scratchpad `build-xenosrecomp.ps1`) does three things:
  1. enters the VS 2022 developer shell;
  2. adds the local Windows SDK (`INCLUDE`/`LIB`, as in `build-windows.ps1`);
  3. configures with CMake + Ninja, Release.
- Problems on the way:
  - **MSVC:** `kernel32.lib` was not found without the local SDK. After that,
    XenosRecomp did not compile with `cl`, because it uses clang built-ins
    (`'__builtin_bswap32': identifier not found`).
  - **Fix:** build with **`clang-cl`** from `D:\narutoxbox\.tools\llvm`.
  - **PowerShell:** CMake deprecation warnings on stderr stopped a script that
    used `$ErrorActionPreference = 'Stop'`. The script now checks exit codes
    instead.
- Result: `.tools/XenosRecomp/build/XenosRecomp/XenosRecomp.exe` (714 KB).
  Log: `out/build-xenosrecomp.log`.

### 12.5 Converting the 1,366 shaders

**Method:** `xenosrun.py` (scratchpad) runs XenosRecomp once per shader in
single-file mode (container → HLSL), with 8 shaders in parallel. Directory mode
was not used, because it builds one cache file and its error checks are
`assert`, which is off in Release. Then each HLSL file is compiled with the
Microsoft-signed `dxc.exe` from `thirdparty/dxc-bin`, with XenosRecomp's own
arguments (`dxc_compiler.cpp`):

- DXIL: `-T ps_6_0`/`vs_6_0 -E main` (or `-T lib_6_3` when the shader uses
  `g_SpecConstants`, as XenosRecomp does, so that it can be linked at runtime)
  `-HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug`
- SPIR-V: the same profile plus `-spirv -fvk-use-dx-layout` (vertex shaders
  also `-fvk-invert-y`).

The first run used the plain profile for every shader. That caused 385 false
`External function used in non-library profile: g_SpecConstants` errors, so it
was rerun with `lib_6_3`.

Outputs are in `out/xenos-hlsl/` (HLSL, DXIL, SPIR-V and `results.csv`). This
is derived game data and stays private.

**Results (second run, confirmed):**

| Result | Shaders | Share |
| --- | --- | --- |
| HLSL + DXIL + SPIR-V all compile | **493** | 36% |
| SPIR-V only (DXIL validation fails) | 204 | 15% |
| DXIL only | 2 | 0.1% |
| HLSL produced, but both compiles fail | 479 | 35% |
| XenosRecomp itself crashes (`0xC0000005`, a few `0xC0000409`) | 188 | 14% |
| **SPIR-V usable in total** | **697** | 51% |

By type: pixel shaders 357 ok / 12 SPIR-V only / 253 fail; vertex shaders
136 ok / 192 SPIR-V only / 2 DXIL only / 226 fail.

The numbers changed slightly between the two runs: 186 → 188 crashes, and the
vertex/pixel split moved. So some XenosRecomp crashes are not deterministic.

Most common errors:

- `use of undeclared identifier 'iNormal0'` (155), `'iPosition0'` (72),
  `'oNormal0'` (26): the vertex inputs and outputs that XenosRecomp declares
  follow Sonic Unleashed's fixed layouts, not WWE's vertex declarations.
- `'s16_Texture2DDescriptorIndex'` (53), `'i0'`, `'c0'`, `'i31'`, `'r32'`:
  sampler, integer-constant and register names that XenosRecomp did not
  declare. Integer constants are listed as unimplemented in its README.
- `extraneous closing brace`, `expected unqualified-id`: broken control flow.
  The README warns that control flow is not well tested. Even the first test
  shader had code after `return`.
- DXIL `validation errors` (204), for example `Instructions should not read
  uninitialized value`. The SPIR-V path accepts these.

**Important:** "compiles" does not mean "renders correctly". None of these
shaders has been run or compared with the emulator's output yet.

### 12.6 What this means for the native renderer

- XenosRecomp is a useful base, but not usable unchanged for WWE 2K15, as its
  README says. The main gaps are clear from the error list:
  1. vertex input/output mapping from the game's vertex declarations, instead
     of Unleashed's fixed names;
  2. integer constants and missing register and sampler declarations;
  3. control-flow reconstruction;
  4. the crashes (188 shaders) in the recompiler itself.
- **Alternative:** The ReXGlue SDK's own Xenos → SPIR-V/DXBC translators
  (`src/graphics/pipeline/shader/spirv_translator*.cpp`, BSD-3) already handle
  *all* of this game's shaders today, because the game renders. They emit code
  for Xenia's emulated register layout, so they would need a different binding
  model for a native renderer, but their translation itself is proven.
- **Decision needed (user):** improve XenosRecomp for WWE, or base the
  native-renderer shaders on the SDK's translator.

## 13. Comparing the two shader converters (2026-11-02)

### 13.1 Method

- **SDK translator:** One run with the **D3D12** backend and
  `--dump_shaders=D:\wwe2k15-recomp\out\sdk-shaders` (run `wwe2k15_019`,
  title and demo, about 2.5 minutes, closed after).
  - In this SDK only the D3D12 pipeline cache writes translated shaders
    (`d3d12/pipeline_cache.cpp`, line 1224). The Vulkan backend dumps only the
    original code (`Shader::DumpUcode`).
  - **Confirmed:** The D3D12 backend renders the title and the demo match
    correctly. This is the first D3D12 run of this game.
- **Matching:** The SDK stores the original code with each 32-bit word in PC
  byte order. After swapping the words back, every pixel shader matched a
  saved container exactly (93 of 93). 70 of 81 vertex shaders matched by
  their first 48 bytes; the rest of the code differs because of the
  vertex-fetch patching (12.2). 11 vertex shaders had no saved container.
- Each matched shader was then given its XenosRecomp result from
  `results.csv` (12.5).

### 13.2 Results on the shaders the game actually used (174)

| | SDK translator | XenosRecomp (+ DXC) |
| --- | --- | --- |
| Pixel shaders | 93 / 93 translated | 72 / 93 compile for DXIL + SPIR-V, 21 fail |
| Vertex shaders | 81 / 81 translated | 48 / 70 compile for both, 2 DXIL only, 20 fail (11 not matched) |
| Total | **174 / 174 (100%)**, no errors (`Translated 174 shaders from the storage in 556 milliseconds`) | **120 / 163 (74%)** for both APIs |
| Crashes | none | none among the used shaders (all 188 crashes were in unused shaders) |
| Proven to render correctly | yes: the game renders with it today | not yet run at all |

Of the 1,366 containers in memory, the GPU used only 174 in this run. The
rest are loaded but not drawn on the title screen and in the demo. Menus,
character select and the other match types will use more.

### 13.3 What each one produces

**XenosRecomp:**

- Output is readable **HLSL**, built for a *native* renderer (Unleashed
  Recompiled):
  - textures and samplers through descriptor indices;
  - alpha test and similar states as specialization constants;
  - normal vertex inputs and render-target outputs.
- It needs WWE-specific changes:
  - vertex input and output names from the game's vertex declarations (the
    largest error group);
  - integer constants and the missing register declarations;
  - control flow.
- MIT licence.

**SDK translator** (`src/graphics/pipeline/shader/`, BSD-3):

- Output is DXBC/DXIL or SPIR-V **binary**, built for the *emulator's* data
  model:
  - vertex data is read in the shader from an emulated copy of Xbox memory
    ("shared memory"), with byte swapping;
  - Xenos float constants come as large arrays;
  - "system constants" describe the emulated state.
- Pixel outputs either go to normal render targets (the `RTV` path) or emulate
  EDRAM inside the shader (the `ROV`/`FSI` path; this run used `d3d12_rov`).
- Proven on 100% of the shaders seen.

### 13.4 Recommendation

- The speed problem measured in 11.10–11.12 is the **command path**:
  - D3D building PM4 packets;
  - the command processor parsing them;
  - waiting for vblank/fences.

  It is not the shaders. A native renderer that takes the game's D3D calls
  directly (sections 3–4, Unleashed-style hooks) removes that path whichever
  converter it uses.
- **Hypothesis:** The fastest working route is to start the native renderer
  with the **SDK translator** (RTV path), feeding it the guest memory as its
  vertex and constant buffers. It is proven on every shader. Later, swap in
  improved XenosRecomp shaders where native vertex inputs give a clear speed or
  quality gain.
- The other route: fix XenosRecomp first (about 25% of the *used* shaders,
  and about 64% of all saved ones). That is more work before the first native
  frame, but the result is cleaner.

## 14. Phase 5 plan — first native frame, with the SDK translator (decided 2026-11-02)

**User decision:** Use the SDK translator first (section 13.4).

### 14.1 What we checked

- **Confirmed:** The installed SDK ships the translator headers
  (`include/rex/graphics/pipeline/shader/spirv_translator.h`, `translator.h`,
  `shader.h`). But `rexgpu-xenos.dll` exports only `rex_gpu_abi_version`,
  `rex_gpu_create` and the two GPU-preference symbols, so the translator
  classes cannot be called from our code.
- **Confirmed:** The SPIR-V translator is these SDK sources
  (`src/graphics/CMakeLists.txt`, lines 7–41):
  - `pipeline/shader/{shader, translator, translator_disasm, interpreter, spirv, spirv_builder}.cpp`
  - `pipeline/shader/spirv_translator{,_alu,_fetch,_memexport,_rb}.cpp`
  - `format/ucode.cpp`

  They need only the installed headers, `rexruntime` (logging, memory,
  strings) and glslang's `SPIRV.lib`, which the SDK install also ships.
  BSD-3 licence.
- **Decision:** Compile those sources into our project from the SDK checkout at
  `c94f5eb`. Copy nothing into the repository; CMake points at the SDK source
  folder. The exe does not load the `xenos` plugin in native mode, so its
  cvars and symbols do not clash.

### 14.2 How the native renderer gets its data

The D3D device object already keeps a shadow of the Xenos GPU registers, with
dirty bits (section 4.3):

- render-state registers at `+0x2920…`;
- shader constants at `+0x780` / `+0x1780`;
- current shaders at `+0x3310` / `+0x330C`.

The state flush `sub_82BA3D08` turns them into PM4 packets. A native renderer
can hook the draw functions (section 4.4), read the shadow **as register
values**, decode them with the SDK's own register definitions (`registers.h`,
`xenos.h`), and draw with Vulkan directly. No PM4 packets and no command
processor are involved. Texture and vertex data come from guest memory, as in
the emulator.

### 14.3 The hard part: GPU synchronisation

D3D busy-waits for values that the GPU writes back (`sub_82BA4738`, 11.11).
Without a command processor nobody writes them, and the game would hang at
present. So the native mode must either:

- (a) keep a tiny "sync-only" consumer that walks the ring buffer and performs
  only the fence and event writes (cheap), or
- (b) hook the wait and fence functions so they complete at once.

We will decide this during milestone M2, with disassembly evidence.

### 14.4 Milestones (each one needs approval before code, build or run)

| | Goal | Done when |
| --- | --- | --- |
| **M1** | Build the SDK SPIR-V translator into a small tool in `tools/`, built from the SDK source, and translate the 174 used shaders offline (ucode from `out/sdk-shaders`). | Count of successful translations; outputs stay in `out/`. |
| **M2** | Native mode skeleton: `--wwe2k15_renderer=native` clears `config.graphics`, uses ReXApp's detached mode, creates our own Vulkan device and swapchain, and clears the window once per guest present. GPU sync solved as in 14.3. | The game runs past the title screen in native mode (black or clear-colour frames), with no hang. |
| **M3** | First real draws: the title screen's 2D quads (`BEGIN_VERTICES`, primitive type 13, inline vertices; section 10.3) with their textures (untiled from guest memory) and blend state from the shadow registers. | A screenshot of the title screen drawn natively. |
| **M4** | Indexed 3D draws, render targets and resolves (the demo match). | The demo match drawn natively, with a speed comparison against the emulator. |

The emulator (`xenos` plugin) stays the default until native mode is better.
The two modes are switched with one option.

### 14.5 M1 result — offline SPIR-V translation with the SDK translator (2026-11-02)

**New files (source only):**

- `tools/shader-translate/CMakeLists.txt`: a standalone CMake project that the
  game build does not use. It compiles these SDK sources from
  `REXGLUE_SDK_SOURCE_DIR` (checkout `c94f5eb`):
  - `graphics/{flags, registers}.cpp`
  - `graphics/util/draw.cpp`
  - `graphics/format/ucode.cpp`
  - `graphics/pipeline/shader/{shader, interpreter, translator, translator_disasm, spirv, spirv_builder, spirv_translator{,_alu,_fetch,_memexport,_rb}}.cpp`

  It links `rex::runtime`, `rex::renderdoc`, `rex::SPIRV`, the glslang
  libraries and `rex::xxhash`, and includes `thirdparty/glslang` from the
  SDK checkout for `SPIRV/SpvBuilder.h`.
- `tools/shader-translate/main.cpp`: reads SDK-style microcode files
  (`shader_<hash>.ucode.bin.vert/.frag`, host byte order). For each one it:
  1. calls `Shader::AnalyzeUcode`;
  2. takes the default modification with the maximum register count;
  3. calls `SpirvShaderTranslator::TranslateAnalyzedShader`, set up for the
     plain render-target path (`edram_fragment_shader_interlock = false`, no
     native 2x MSAA, all features on);
  4. writes `<hash>.vert.spv` / `.frag.spv` and `results.csv`.

  Each translation is guarded by SEH, so one crashing shader does not stop the
  batch.

**Build problems on the way:**

| Missing | Fix |
| --- | --- |
| `renderdoc_app.h` | link `rex::renderdoc` |
| `FLAGS_dump_shaders_storage_` | add `flags.cpp` |
| `draw_util::kD3D10StandardSamplePositions2x/4x` | add `util/draw.cpp` |
| `reg::RB_COLOR_INFO::rt_register_indices` | add `registers.cpp` |

The build script is in the scratchpad (`build-shader-translate.ps1`: VS dev
shell, local Windows SDK, clang + Ninja, Release). Output:
`out/build/shader-translate/wwe2k15-shader-translate.exe`.

**Extracting the code from the saved containers (confirmed):**

- At the container's `shaderOffset` (header word `+0x18`) are two big-endian
  words: the code's offset and its size inside the physical part.
- Code extracted with this rule is byte-identical to the SDK's own microcode
  dump for all used pixel shaders (95 containers cover all 93).
- Flags `0x102A1100` mean a pixel shader and `0x102A1101` a vertex shader.
- Vertex shaders cannot match exactly, because of the runtime vertex-fetch
  patching (12.2).
- The extracted code of the 1,366 first-scan containers is in `out/ucode-all/`
  (private).

**Results (confirmed):**

| Input | Translated to SPIR-V | Translator crashed |
| --- | --- | --- |
| The 174 shaders the game used (SDK dump, `out/sdk-shaders`) | **174 / 174** (93 pixel, 81 vertex) | 0 |
| All 1,366 saved containers (`out/ucode-all`) | **1,352 / 1,366 (99%)** (772 pixel, 580 vertex) | 14 |

- All 174 outputs start with the SPIR-V magic number `0x07230203`. We have no
  SPIR-V validator (`spirv-val`) yet, and no output has been run on the GPU.
- The 14 crashes are all shaders that XenosRecomp also fails on: 6 crash
  XenosRecomp and 8 fail in DXC. None of them was used in the test runs.
  **Hypothesis:** These are unusual or not real shaders, or the extraction
  rule does not fit them.
- For comparison, XenosRecomp + DXC compiled 493 of the same 1,366 for both
  APIs (12.5). Every one of those 493 also translates with the SDK.

**Conclusion:** M1 is done. The SDK translator works outside the GPU plugin, in
our own code, on 100% of the used shaders. The next milestone is M2 (14.4):
native mode skeleton and GPU synchronisation.

### 14.6 M2 — native mode skeleton (2026-11-02)

**What D3D waits for (confirmed by disassembly):**

- `sub_82BA8208` is D3D's wait-for-fence. `device+0x2B1C` is the last fence
  the CPU issued, and `*(device+0x2B10)` is the fence the GPU has completed,
  written back to memory.
- It loops on `sub_82BA4738` (the busy-wait, 11.11) until
  `issued - target < issued - completed`.
- The ring-space wait `sub_82BA9690` (4.3) depends on the read pointer
  written back by the GPU.
- In the emulator, these write-backs come from the PM4 packets that the
  command processor executes.

**Decision (better than options a/b in 14.3):**

- The SDK has backend-independent base classes, `rex::graphics::GraphicsSystem`
  and `CommandProcessor` (`src/graphics/{graphics_system,command_processor}.cpp`).
  They already do the ring buffer, read-pointer write-back, fences and events,
  interrupts, vblank (`MarkVblank`) and `VdSwap`.
- A backend only adds `CreateProvider`, `CreateCommandProcessor`,
  `SetupContext`, `ShutdownContext`, `LoadShader`, `IssueDraw`, `IssueCopy` and
  `IssueSwap`.
- So native mode subclasses them. Synchronisation stays exactly as in the
  emulator, but draws and resolves are not emulated.
- `rexruntime.dll` already exports `rex::ui::vulkan::VulkanProvider` and
  `VulkanPresenter`, so the window and swapchain code is reused.

**New and changed files:**

- **`src/native/native_graphics_system.cpp`** (new):
  - `NativeGraphicsSystem` creates `VulkanProvider::Create(false, with_presentation)`.
  - `NativeCommandProcessor`: `LoadShader` keeps a `Shader` per microcode
    hash; `IssueDraw` / `IssueCopy` only count; `IssueSwap` logs every 600
    swaps.
  - The plugin exports `rex_gpu_abi_version` / `rex_gpu_create` (Vulkan only).
- **`CMakeLists.txt`:** a new optional target `rexgpu-native` (SHARED). It
  compiles these SDK sources from `REXGLUE_SDK_SOURCE_DIR` (cache variable,
  default from the environment variable of the same name):
  - `flags`, `register_file`, `registers`, `sampler_info`, `graphics_system`,
    `command_processor`, `util/draw`, `format/ucode`, `pipeline/shader/shader`

  It is added to `GPU_PLUGINS`, so the SDK helper copies it next to the exe.
  Without the variable, only `xenos` is built as before.
- It is a separate plugin DLL, not code in the exe, because `flags.cpp` defines
  the same GPU options (`vsync`, ...) as the xenos plugin. Only one plugin is
  ever loaded, so they never clash in the shared option registry.
- **`run-windows.ps1`:** new `-GpuPlugin xenos|native` (default `xenos`). The
  first test added `--gpu_plugin=native` *after* the script's own
  `--gpu_plugin=xenos`, and the option ended up empty
  (`GPU plugin '' not found at ...rexgpu-.dll`). So the script now passes it
  only once.

**Build:**

```powershell
$env:REXGLUE_SDK_SOURCE_DIR = 'D:/narutoxbox/.tools/rexglue-sdk'
.\build-windows.ps1 -SdkPrefix ...\win-amd64-vulkan-test -ToolsRoot D:\narutoxbox\.tools -Jobs 8
# [11/12] Linking CXX shared library rexgpu-native.dll
# [12/12] Linking CXX executable wwe2k15.exe
```

It linked on the first try.

**Run:** `.\run-windows.ps1 -VulkanDevice 1 -Keyboard -GpuPlugin native`
(run `wwe2k15_021`, about 100 s, closed after).

```
[15:34:23.952] [native] swap 1: 1280x720 front buffer 115D5000, 50 draws and 0 copies ..., 4 shaders loaded
[15:35:34.734] [native] swap 4200: ... 1514627 draws and 0 copies since the last report, 115 shaders loaded
[15:36:05.818] [native] swap 6000: ... 1409324 draws and 0 copies since the last report, 117 shaders loaded
```

**Results:**

- **Confirmed:** No hang. The game ran for 100 s in native mode. D3D's fence
  and ring waits are satisfied, and the game kept going through the title and
  into the demo match, as the draw counts show.
- **Confirmed:** 600 guest frames every 10.0–10.4 s (about 58–60 fps)
  throughout, *including the demo match*. With the emulator the same stretch
  ran at 20–30 fps (11.8).
  - This is not yet a fair speed comparison, because native mode does no GPU
    work so far.
  - But it shows that the game's own CPU side can keep 60 fps here.
- **Confirmed:** About 2,350 draws per frame during the demo (1.4 million per
  600 frames). 117 distinct shaders were loaded.
- **Confirmed:** No GPU errors in the log, only the known memory-check bursts
  (11.9) and missing `cache:`/`UPDATE:` devices.
- **Open:** `IssueCopy` was never called (0 copies). In the emulator, the
  resolves showed up as `edram_mode=6` copy draws (6). We need to check how the
  base processor detects copy mode, before resolves are needed in M4.
- **Not verified:** what is on screen. The screenshot showed another window
  in front, not the game. Native mode does not present anything yet, so the
  game window would be black.

M2 is done: native mode runs without the Xenos draw emulation, with correct
synchronisation. Next is M3: draw the title screen's 2D quads natively.

### 14.7 M3, step 1 — native presentation and the title screen's draw list (2026-11-02)

**Code (`src/native/native_graphics_system.cpp`):**

- `IssueSwap` fills the presenter's guest output image through Vulkan
  (`VulkanPresenter::RefreshGuestOutput`):
  - one command buffer on graphics queue 0;
  - barrier `UNDEFINED → TRANSFER_DST`, then `vkCmdClearColorImage`, then a
    barrier to `kGuestOutputInternalLayout` (`SHADER_READ_ONLY_OPTIMAL`) with
    `kGuestOutputInternalStageMask/AccessMask`, as `presenter.h` requires;
  - submit with a fence and wait (synchronous, for testing only).

  The colour pulses with the frame count.
- `LoadShader` now also calls `AnalyzeUcode`, which gives texture and vertex
  bindings. It needs `pipeline/shader/translator.cpp` and
  `translator_disasm.cpp` in the plugin (`CMakeLists.txt`).
- New options `--wwe2k15_native_trace_frame=N` and
  `--wwe2k15_native_trace_file=...` write every draw of guest frame N. For each
  draw: the primitive, the count, the EDRAM mode, render target 0's format,
  base and pitch, MSAA, `RB_BLENDCONTROL0`, alpha test, depth, the shader
  hashes, and each texture fetch (format, dimension, tiled, size, address,
  endianness) and vertex fetch (address, size, stride).
- `IssueDraw` now also counts copy-mode draws (`RB_MODECONTROL.edram_mode == kCopy`).
  This answers the open point in 14.6: resolves arrive as draws in copy mode,
  and `IssueCopy` is only what a backend calls from there.

**Run** (`-GpuPlugin native`, trace of frame 1800, game closed after):

```
[native] swap 1: 1280x720 front buffer 115D5000, 50 draws (1 copy mode) ...
[native] Vulkan presentation objects created
[native] swap 1800: ... 26400 draws (1200 copy mode) since the last report, 11 shaders loaded
[native] draw trace of frame 1800 written
```

- **Confirmed:** The game window shows our Vulkan test frame (the pulsing
  blue colour), with the SDK's FPS overlay drawn on top. Native presentation
  works.

**Frame 1800 (title-screen area) needs these features** (44 draws,
`out/trace/native-1800.txt`):

| What | Count / detail |
| --- | --- |
| Point lists (`kPointList`, prim 1) | 24 draws: point sprites, expanded in the vertex shader (stride 20, 2 attributes) |
| Quad lists (`kQuadList`, prim 13) | 15 draws: 2D quads (stride 12/20) |
| Rectangle lists (prim 8) | 5 draws, 2 of them in copy mode (resolves) |
| Render targets | colour format 0 (`k_8_8_8_8`) at EDRAM bases 0 and 1536 (pitch 1280 and 1040); one 2x MSAA target with a depth-only pass |
| Resolves | 2 per frame: one EDRAM surface (base 1536, pitch 1040) and the back buffer (base 0, pitch 1280) to the front buffer |
| Textures | `k_DXT4_5` (BC3) linear 512x512 and others, `k_8_8_8_8` **tiled** 1024x512 (probably a resolved render target read back as a texture), `k_8` linear, a 1x1 `k_16_16_16_16` dummy |
| States | alpha blending (`RB_BLENDCONTROL0 = 07060706`, source alpha / inverse source alpha), alpha test on, depth off for 2D; depth on for one 3D pass |

**What M3 must build** (in order):

1. The SDK SPIR-V translator inside the plugin (as in M1), plus a Vulkan
   pipeline layout matching what the translator expects (descriptor sets and
   constants, taken from the SDK's Vulkan backend).
2. A vertex and constant data path from guest memory, including point-sprite
   and quad-list expansion.
3. Render targets as real Vulkan images (no EDRAM emulation), and resolves as
   image copies into textures and the front buffer.
4. Textures: BC3 upload (no decode needed), untiling for tiled `k_8_8_8_8`,
   and endian swapping.
5. Presenting the front buffer instead of the test colour.

### 14.8 M3, step 2 — scope check before writing the draw path (2026-11-02)

**What the SDK translator's shaders need** (`spirv_translator.h`, lines 215–365):

- **Descriptor set 0:** "shared memory", a storage buffer with the whole 512 MB
  of Xbox physical memory, plus EDRAM. Vertex data is fetched from it inside
  the shader, with endian swapping.
- **Descriptor set 1:** five constant buffers:
  - `SystemConstants` (about 60 fields: viewport/NDC transform, point-sprite
    sizes, alpha-test reference, texture swizzles and signs, render-target
    formats and blend data, ...);
  - packed float constants for the vertex and pixel shader;
  - bool/loop constants;
  - fetch constants.
- **Descriptor sets 2–3:** vertex and pixel textures and samplers, in the
  translator's own binding order.

**What feeding them takes in the SDK's Vulkan backend** (lines of C++):
`command_processor` 6,827, `render_target_cache` 6,155, `pipeline_cache` 3,702,
`texture_cache` 3,261, `shared_memory` 364 + 578, `primitive_processor`
234 + 1,567, `texture/cache` 1,120, `render_target/cache` 1,388. That is
**about 25,700 lines**.

**Confirmed:** The emulator's Vulkan path already uses host render targets,
not EDRAM emulation in shaders. Its pipeline cache file is
`out/cache/shaders/shareable/545408B6.fbo.vk.xpso` (`fbo` path;
`render_target_cache.cpp`, line 209, chooses `fsi` only on request).

**Consequence:** "SDK translator first" means the native renderer would rebuild
most of the emulator's Vulkan backend: shared memory, constant packing,
texture untiling and caching, render targets, resolves and primitive
conversion. The result would have almost the same architecture as the code it
replaces. The slowness measured in 11.10–11.12 (busy-waits on the emulated
GPU, a half-idle command processor, an idle graphics card) points to latency
in that pipeline, not to a part that a rebuild would remove by itself.

**Options:**

- **A. Continue as planned:** draw from the register state with the SDK
  translator. This means reimplementing large parts of the 25,700 lines. The
  speed gain is uncertain.
- **B. A real Unleashed-style native renderer:**
  - D3D-level hooks (sections 4 and 10);
  - XenosRecomp shaders fixed for WWE (vertex declarations, integer
    constants, control flow);
  - textures converted once;
  - a simple render-target model;
  - no shared-memory or write-watch emulation.

  This needs the most reverse engineering, but it is the only option that
  removes the emulation model. Weeks to months.
- **C. Shorten the latency in the existing path first:**
  - the command processor serves the game's waits with
    `Sleep(wait / 0x100 ms)` (`command_processor.cpp`, line 1059);
  - the game busy-waits on fences (11.11).

  Small, measurable changes could raise the frame rate soon. This is
  independent of A and B.

**Decision needed from the user.** Nothing from step 2 has been written yet.

## 15. Option C — finding the latency in the emulated GPU path (2026-11-02)

**User decision:** Option C (14.8) first.

### 15.1 Windows timer resolution — tested, not the cause

- **Confirmed:** The SDK never raises the Windows timer resolution. There is no
  `timeBeginPeriod` or `NtSetTimerResolution` in `src/` or `include/`. The
  vblank worker (`graphics_system.cpp`, line 169) and the `WAIT_REG_MEM`
  handler (`command_processor.cpp`, line 1059) sleep in 1 ms steps.
- **Confirmed:** Measured on this PC, `Sleep(1)` takes on average 15.50 ms
  (7.90–23.19) by default, and 1.77 ms (1.07–3.14) after
  `timeBeginPeriod(1)`.
- **Test:** a temporary option `--wwe2k15_timer_resolution_ms` called
  `timeBeginPeriod` in `OnPreSetup`. A/B over the same 170 s attract
  sequence (`abrun.ps1` in the scratchpad: emulator, Vulkan, texture flag):

  | Run | Menus | Demo match |
  | --- | --- | --- |
  | A, default timer (`wwe2k15_023`) | 58–60 fps | 20–22 fps |
  | B, 1 ms timer (`wwe2k15_024`) | 57–60 fps | 19 fps |

- **Conclusion:** No improvement, so the hypothesis is rejected. The option
  was removed again (`src/main.cpp`, `src/wwe2k15_app.h` are back as before).
  Possibly a library in the process (for example SDL) already raises the
  resolution. We did not check this.

### 15.2 What the GPU thread sleeps in

- In the GPU-thread profile (11.12), 10 of 40 samples are
  `ntdll!NtDelayExecution` ← `KernelBase!SleepEx` ←
  `rexgpu-xenos.dll+0x1b13a`. We resolved this by the nearest exports of the
  system DLLs.
- The plugin code just before it is `cmpl $0xff, %r13d`. That is the
  `if (wait >= 0x100)` check in the `WAIT_REG_MEM` handler, so this time is
  the command processor sleeping in a `WAIT_REG_MEM`.
- 3 more samples are in a `msvcp140` mutex or condition-variable wait.

### 15.3 Which D3D waits run during the dips

**Method:** counting hooks on the ten D3D functions that build `WAIT_REG_MEM`
(section 4.2), in `src/renderer_hooks.cpp` (`COUNT_WAIT_BUILDER`). They log the
calls and the last caller every 600 frames. Run `wwe2k15_025`:

| Function | Menus (per 600 frames) | Demo dip (per 600 frames) |
| --- | --- | --- |
| `sub_82BA0270` / `sub_82BA03E0`, called from `sub_82BA0750` | 0–109 | **33,540–36,000** |
| `sub_82BA1218`, called from `sub_82BA2370` | 600–1,200 | **30,909–32,843** |
| `sub_82BA5AE8`, `sub_82BA89A8`, `sub_82BAFCD0` | about 600 each | about 600–1,100 |
| `sub_82BA8CF8` | 1,800 | 1,800–2,800 |

**What they are (confirmed by disassembly):**

- **`sub_82BA0270`** is a cache-coherency flush for a memory range:
  - it writes `COHER_SIZE_HOST`, `COHER_BASE_HOST` and `COHER_STATUS_HOST`
    (registers `0xA2F`–`0xA31`);
  - then a `WAIT_REG_MEM` (`0xC0043C00`) on `0xA31`: equal, mask
    `0x80000000`, `wait=8`.

  In the SDK this poll calls `MakeCoherent()`, which only clears the status
  register (`command_processor.cpp`, line 574), and `wait < 0x100` only
  yields. So these finish immediately and are cheap.
- **`sub_82BA1218`** (called through `sub_82BA2370`, 4 game call sites) fills
  the device's copy-register shadow (`+0x2A18…+0x2A24`) and ends with the same
  coherency wait. **Hypothesis, strong:** this is D3D's **Resolve**.

**So the demo match does about 50–55 resolves per frame, against 1–2 in the
menus.** On real hardware a resolve is a cheap EDRAM copy. In the emulator each
one copies into emulated guest memory, and textures made from that memory
must then be uploaded again. This matches the busy command processor, the idle
graphics card (11.10) and D3D busy-waiting on fences (11.11).

### 15.4 Texture-cache limits — tested, not the cause

The run `wwe2k15_026` used larger limits:
`--texture_cache_memory_limit_render_to_texture=512` (default 24),
`--texture_cache_memory_limit_soft=1536` (384) and
`--texture_cache_memory_limit_hard=2048` (768).

The demo match ran at 21–23 fps, against 20–22 fps with the defaults. No real
change.

### 15.5 Next step

To speed up resolves inside the emulator, we must see where the command
processor spends its time per resolve. The installed `rexgpu-xenos.dll` has no
PDB, so the next measurement would be a profile against a GPU plugin built
from the same SDK sources *with debug symbols*, the way `rexgpu-native` is
built.

For the native renderer (option B), this confirms the main design point: the
resolves must become plain GPU image copies (or render-to-texture) that never
pass through guest memory.

### 15.6 Profile with debug symbols (2026-11-02, run `wwe2k15_027`)

- New optional target **`rexgpu-xenosprof`** (`CMakeLists.txt`, environment
  `WWE2K15_PROFILING_PLUGIN=1`): the SDK's Vulkan GPU emulation built from the
  SDK sources with `-msse4.1 -g -gcodeview` and `/DEBUG`, giving
  `rexgpu-xenosprof.pdb` (15 MB). Entry file
  `src/native/xenos_profiling_plugin.cpp`. Run with
  `run-windows.ps1 -GpuPlugin xenosprof`.
- lldb's Python API did not load the PDB (even with
  `LLDB_USE_NATIVE_PDB_READER=1`), but **`llvm-symbolizer --obj=<dll>`
  does**. `pdbprof.py` (scratchpad) aggregates the samples with it.
- **Caveat:** The user was playing during this run. The 60 samples of the
  command-processor thread were taken right after they chose both wrestlers
  and skipped both entrances, during a long black stall (no present for more
  than 15 s after frame 4800). The attached debugger made that stall longer.
  The sampling script then closed the game, which ended the user's session.
- **Result** (inclusive, of 52 samples on the command processor):
  - `VulkanCommandProcessor::IssueDraw`: 30
  - `VulkanPipelineCache::ConfigurePipeline` / `EnsurePipelineCreated`:
    12 / 10, with **10 leaf samples in `nvgpucomp64.dll`** (the NVIDIA shader
    compiler)
  - `WAIT_REG_MEM`: 5
  - `SharedMemory::RequestRange`: 5
  - `EndSubmission`: 5
- **Hypothesis:** The black screen on the match-loading screen is mostly
  **synchronous pipeline (shader) compilation** for the new match's shaders,
  since `EnsurePipelineCreated` blocks the command processor.
  - The log agrees: `Creating graphics pipeline state ...` lines minutes apart
    (16:51:35, 16:53:18) during the stall.
  - A second run with the same `--cache_root` should then be much faster.
    That test is still open, as is a profile of the demo dips without a
    player.

## 16. Option B plan — a true native renderer (decided 2026-11-02)

**User decision:** Option B (14.8): an Unleashed-style renderer that takes the
game's D3D work directly and draws it on the GPU, with no Xenos emulation
model (no shared-memory copy of Xbox memory, no EDRAM, no write watches).

### 16.1 What we already have

- **Synchronisation** (M2, 14.6): the `rexgpu-native` plugin keeps the SDK's
  backend-independent command processor. It drains the ring buffer, so D3D's
  fences, vblank interrupts and swaps work. The game runs at about 60 fps
  with no GPU work. Native mode keeps this and only adds drawing.
- **Presentation** (14.7): our own Vulkan frame reaches the window through
  `VulkanPresenter::RefreshGuestOutput`.
- **Hooks:** game functions can be replaced (section 9).
- **The D3D map** (sections 4, 10, 15):
  - CreateDevice `82BA4F40`, Present `82BA5FA8`;
  - draws `82BA41C8` (indexed or not) and `82BA3D08`/`82BA3CF8`
    (inline vertices);
  - Resolve `82BA2370` → `82BA1218` (hypothesis, strong);
  - the render-state and sampler-state setter tables (`0x835BD9C0`,
    `0x835BDE80`);
  - the device shadow of GPU registers, the shaders and the constants
    (`+0x780`, `+0x1780`, `+0x2920…`, `+0x3310`, `+0x330C`).
- **Shaders:** all 1,366 containers in memory (Phase 4) and XenosRecomp
  builds. 493 compile for both APIs today; 120 of the 163 matched *used*
  shaders compile.

### 16.2 How the renderer gets its data

At every hooked draw, read the device object: the current shaders, the
shader constants, the render-state register shadow, the textures (fetch
constants) and the vertex streams. Decode the register values with the SDK's
`registers.h`/`xenos.h`. These are headers only; no emulator code is involved.
Resources (textures, vertex and index buffers) are converted once, when the
game creates or unlocks them, not on every draw. Resolves become GPU image
copies between our own render targets and textures; the result never goes
back into guest memory, unless the game later reads it with the CPU.

### 16.3 Milestones (each needs approval before code, build or run)

| | Goal | Done when |
| --- | --- | --- |
| **B1** | Map the remaining D3D API: creating textures, vertex/index buffers, shaders and vertex declarations, Lock/Unlock, and the inline "Set" paths (which device fields change). Logging hooks plus disassembly, with call counts for the title and the demo. | A table of functions and device fields with evidence; no rendering changes |
| **B2** | XenosRecomp for WWE: vertex inputs from the game's vertex declarations (the largest error group), integer constants, missing declarations, control-flow fixes. Work on the 174 used shaders first. | At least 95% of the used shaders compile to SPIR-V |
| **B3** | Renderer core in `rexgpu-native`: Vulkan device objects, a frame and submission ring, a resource table keyed by guest address, texture conversion (untiling, DXT pass-through, endian swap), buffer upload, a shader table keyed by container hash. | Resources from the title screen are created and converted (logged) |
| **B4** | Title screen: hook the draw paths, build pipelines from the device state, handle point sprites and quad lists, alpha blend and alpha test, our own render targets, and resolves as copies. | A screenshot of the title screen drawn natively |
| **B5** | Demo match (3D): depth and stencil, MSAA, the roughly 50 resolves per frame, multiple render targets. | The demo match drawn natively, with a speed comparison against the emulator |
| **B6** | Menus and character select, then a playable match. Switchable in Charge. | The user plays a match in native mode |

**Expected size:** weeks to months. Each milestone ends in something testable
and written up here. The emulator stays the default until native mode is
better.

**Licences:** XenosRecomp (MIT) is used as a tool. Its output stays private
game-derived data, built on the player's machine. Nothing is copied from
Unleashed Recompiled (GPL-3.0); only its approach is followed. ReXGlue is
BSD-3.

## 17. B1 — mapping the game's D3D usage (started 2026-11-02)

### 17.1 Method

- `src/d3d_api_trace.inc` (generated with `apisurface.py`, range
  `0x82B9A000–0x82BC8000`) lists the **89** functions in the D3D range that
  game code calls directly and that are not already hooked. There are 126 in
  total.
- `src/renderer_hooks.cpp` hooks each one with `API_COUNT`. Every 600 frames
  it logs the call count, plus `r3`–`r6` and the caller of the last call.
- Run `wwe2k15_028`: emulator, 170 s, title and demo, no input.
  `out/trace/b1-api.txt`.
- The functions were then classified from their disassembly together with
  the logged arguments. `r3` is always the device (`AECF5280`) for device
  functions.

### 17.2 Results — the hot path (calls in 170 s)

| Function | Calls | What it is | Evidence | Confidence |
| --- | --- | --- | --- | --- |
| `82BA2E40` | 12.5 M | **SetVertexShaderConstantF**(dev, start, data, count) | destination `dev + (0x78 + start) * 16` = the `+0x780` shadow; vector copy loop | confirmed |
| `82BA2F18` | 8.4 M | **SetPixelShaderConstantF** | destination `dev + (0x178 + start) * 16` = `+0x1780` | confirmed |
| `82BA2FF0` / `82BA3050` | 2.4 M / 80 k | **Set{Vertex,Pixel}ShaderConstantB** | sets bits in dword `0x9E0`/`0x9E4` (`+0x2780`/`+0x2790`), dirty bit in `+0x20` | confirmed (VS/PS order is a hypothesis) |
| `82B9DBB0` | 1.7 M | **SetStreamSource**(dev, stream, vb, offset, stride) | reads `vb+0x18/+0x1C`, writes vertex fetch constant `dev + (0xDE + 17 - stream) * 8` | confirmed |
| `82B9B328` | 1.6 M | **SetTexture**(dev, sampler, texture) | copies the texture header (`tex+0x1C…+0x30`, fetch-constant words) into the sampler's fetch-constant shadow | confirmed |
| `82BA33A0` / `82BA3198` | 631 k each | **SetVertexShader** / **SetPixelShader** | store the shader to `+0x3310` / `+0x330C`, set a dirty bit, fence the old shader (`+0x2B1C`) | confirmed |
| `82BA35B8` | 768 k | **SetVertexDeclaration** | `stw r4,0x2FD0(r3)`, dirty bit `0x80000` | hypothesis |
| `82B9E870` | 125 k | **SetIndices** | stores to `+0x3220`, copies `ib+0x1C` to `+0x2888` | hypothesis |
| `82B9E3B0` / `82B9DB60` / `82B9DA68` | 422 k / 314 k / 423 k | **SetViewport** / **GetViewport** / viewport-derived update | read and write the floats at `+0x32E0…+0x32F4` | SetViewport/GetViewport confirmed |
| `82B9DD58` | 1.7 M | draw preparation that fences the current index buffer (`+0x320C`) | args (ib, prim 13, count 52) | hypothesis |
| `82BA2370` | 150 k | **Resolve** (15.3) | | strong hypothesis |
| `82BA7AA0` | 82 k | **Clear** | float and colour arguments | hypothesis |
| `82B9F490` | 62 k | resource **AddRef** | `lwarx`/`stwcx.` increment on `obj+4` | confirmed |
| `82B9F228`, `82B9F638`, `82BA0750` | 145–147 k each | resource lock / block-until-idle family (`82BA0750` issues the coherency flush, 15.3) | | hypothesis |

### 17.3 Results — resource creation (menus and loading only)

| Function | Calls | What it is |
| --- | --- | --- |
| `82B9AFA8` | 1,002 | **CreateTexture** (0x34-byte object, header via `82B9A7E0`; 12.1) |
| `82B9B0C8` | 143 | the second texture creator (cube or volume, hypothesis) |
| `82B9B200` | 3,303 | texture level query (reads the format from `tex+0x1C`; hypothesis) |
| `82B9A0E0` | 7,785 | texture-header helper used inside Resolve (`lr 82BA140C`) |
| `82B9AE18`, `82B9AD58`, `82B9AD48` | 21, 6,186, 9 | wrappers into `82B9A6A0` (resource header setup; hypothesis) |

**Main finding for the design:** every `Set*` call the renderer needs is a
real, hookable function on this XDK (constants, streams, textures, shaders,
declaration, viewport, indices). Only the render states use the per-device
setter tables (10.1). So the renderer can track state as it is set, instead of
decoding register shadows at every draw.

### 17.4 Still open in B1

- Creating vertex and index buffers and shaders, and Lock/Unlock: not yet
  identified among the remaining low-count functions.
- The vertex-declaration layout (`82BA35B8`'s object), and how the declaration
  is turned into vertex-fetch patching (12.2).
- Confirming SetIndices, Clear and the lock family with arguments over a few
  frames.

### 17.5 B1 completed — creation and locking (2026-11-02)

From the same run (`out/trace/b1-api.txt`) and disassembly:

| Function | Calls | What it is | Evidence | Confidence |
| --- | --- | --- | --- | --- |
| `82B9F570` | 906 | **CreateVertexBuffer**(length, usage, ...) | allocates a 0x20-byte object (`8225F848`), resource type bits `...0001` | confirmed |
| `82B9F648` | 829 | **CreateIndexBuffer**(length, usage, format) | 0x20-byte object, type `...0002`, format bit `r5 << 29` (32-bit indices) | confirmed |
| `82B9F638` / `82B9F6F8` | 36,000 / 829 | vertex / index buffer **Lock** | pass `obj+0x18` (the data address) to `82B9F228`, the wait-until-not-busy function | confirmed |
| `82BA0750` (+`82BA03E0`) | 145 k | the "unlock" side: a GPU cache flush (`COHER_*`, 15.3) over the resource range | | hypothesis |
| `82BA39D0` / `82BA3BB8` | 399 / 290 | **CreateVertexShader** / **CreatePixelShader** from a **compiled shader container** in memory (`r3`; reads `+0`, `+4`, `+8` as in the container header) | the optional hook pointers at `0x838F5984` / `0x838F5988` differ between VS and PS | confirmed for the container argument; the VS/PS order is a hypothesis |
| `82BA36C0` | 29 | **CreateVertexDeclaration**(elements) | counts 12-byte `D3DVERTEXELEMENT9` entries until stream `0xFF`, allocates `count * 12 + 0x38` | confirmed |
| `82BA3858` | 1,633 | shader usage-mask query (ORs two 64-bit masks from the shader object) | | hypothesis |
| `82BA4C10` | 21 | device state reset (unbinds streams and indices) | | hypothesis |

Because the game creates its shaders from containers in memory, a native
renderer can identify each shader **at creation** (by the container's hash)
and look up the precompiled XenosRecomp output for it. No microcode matching
is needed.

**B1 is complete** for what B2–B4 need. Still to learn when needed: texture
LockRect and level queries (`82B9B200`, `82B9AD58` family) and Clear's exact
arguments.

## 18. B2 — XenosRecomp for WWE 2K15 (2026-11-02)

**Patch:** `tools/xenosrecomp/wwe2k15.patch` (our changes only, MIT like
XenosRecomp), against `.tools/XenosRecomp` at
`990d03b28a27b50277ee5d8d942e1c5f873869d1`. Apply it with
`git -C .tools/XenosRecomp apply ../../tools/xenosrecomp/wwe2k15.patch`.

**Changes (each found from the error list of the used shaders):**

1. **Interpolators:**
   - Added `Color2`, `Color3`, `Normal0`, `Tangent0`, `Binormal0`,
     `Position0` and `Fog0` to `INTERPOLATORS`. They are appended at the end,
     so the existing Vulkan locations do not move.
   - A scan of all converted HLSL for undeclared `i…`/`o…` names found exactly
     these: `Position0` 213 shaders, `Normal0` 221, `Tangent0` 159, `Color3`
     89, `Color2` 40, `Binormal0` 4.
2. **Temporary registers:** `r0`–`r127` instead of `r0`–`r31`.
3. **SPIR-V shared constants:**
   - Texture and sampler indices now have room for 32 fetch constants:
     `dimension * 128 + register * 4`, samplers at `384 + register * 4`.
   - The other shared values moved up: `g_Booleans` 512, `g_SwappedTexcoords`
     516, `g_HalfPixelOffset` 520, `g_AlphaThreshold` 528.
   - Integer constants are at `1024 + register * 16`.
   - **Our renderer must use this layout.**
4. **Unnamed registers:** a post-pass (`addUnnamedRegisterFallbacks`)
   declares every `cN`, `iN` and `sN_…DescriptorIndex` that a shader uses but
   whose name is not in its constant table:
   - on SPIR-V, as raw loads from the vertex/pixel constants and the shared
     constants;
   - on DXIL, as zero placeholders, because our renderer is Vulkan.
5. **Vertex texture fetch:**
   - `shader_common.h` samples through `XENOS_SAMPLE`, which becomes
     `SampleLevel(…, 0)` in vertex shaders.
   - XenosRecomp now writes `#define XENOS_VERTEX_SHADER` before the header
     for vertex shaders. Vulkan forbids implicit-LOD sampling there; this
     affected 28 used shaders.
6. **Vertex inputs:** usages that are not in `USAGE_LOCATIONS` get
   `vk::location(16 + n)`. Vulkan needs a location on every input once one has
   one.

**Results (`xenosrun.py`, same DXC arguments as before):**

| Set | SPIR-V before | SPIR-V after | DXIL after |
| --- | --- | --- | --- |
| Shaders the game used (218 containers, incl. runtime-patched variants; `out/shader-used`) | 189 after the first patch steps; 153 compiled for both before B2 | **218 / 218 (100%)** | 211 (7 fail only DXIL's "uninitialized value" validation) |
| All saved containers (1,366) | 697 (51%) | **928 (68%)** | 683 |

The remaining failures are all in shaders that were not used in the test
runs:

- 144 XenosRecomp crashes;
- broken control flow (`expected '}'`, ...);
- `short-circuiting logical operator` on vectors;
- out-of-range swizzles;
- a few more interpolators and 1D textures.

They will be fixed when menus or matches start using those shaders.

**Not verified yet:** "compiles" is not "correct". The first real check is
drawing with these shaders in B4.

**B2 target met:** 100% of the used shaders compile to SPIR-V (the target was
95%).

## 19. B3 — renderer core (in progress, 2026-11-02)

### 19.1 Where the renderer lives

- **Decision:** The renderer lives in the **game exe**, next to the D3D hooks,
  in the new file `src/native/native_renderer.cpp`.
- It reaches Vulkan through the running graphics system:
  `rex::Runtime::instance()->graphics_system()` → `provider()` / `presenter()`.
  `rexruntime.dll` exports `VulkanProvider` and `VulkanPresenter`.
- The `rexgpu-native` plugin keeps doing only synchronisation and the swap
  (14.6).
- The creation functions are hooked by `native_renderer.cpp` and were removed
  from the generic counter list `src/d3d_api_trace.inc`, which now has 83
  entries, to avoid duplicate hook symbols.

### 19.2 Resource tracking (confirmed working)

Hooks (each calls the original first, then records the result by its guest
object address):

| Function | Recorded |
| --- | --- |
| CreateTexture `82B9AFA8` | the texture fetch constant from the header (`obj+0x1C…+0x30`, 6 words) |
| CreateVertexBuffer `82B9F570` / CreateIndexBuffer `82B9F648` | data address `obj+0x18`, length `r3` |
| CreateVertexShader `82BA39D0` / CreatePixelShader `82BA3BB8` | FNV-1a 64 hash of the container (`virtualSize + physicalSize` bytes) |
| CreateVertexDeclaration `82BA36C0` | the `D3DVERTEXELEMENT9` entries up to the `0xFF` end marker |

**Run** `wwe2k15_029` (emulator, 120 s, title and demo):

```
[native] shader container AEE1508C size 240 hash EBE17097BAA01443
[native] shader container AEE1518C size 320 hash 141EFABE342F8A7B
[native] resources: 1002 textures, 912 vertex buffers, 835 index buffers, 399 vertex shaders,
         290 pixel shaders, 29 declarations (created so far); live textures by format: ...
```

- **Confirmed:** Both logged shader hashes are names of dumped containers
  (`out/shader-dump/EBE17097BAA01443.bin`, `141EFABE342F8A7B.bin`). The
  renderer can map each shader object to its precompiled XenosRecomp output
  at creation.
- **Texture formats in use** (live, by Xenos format number):
  - `k_DXT1` (18): 510 linear, plus 10 cube or volume
  - `k_DXT4_5` (20): 349
  - `k_8_8_8_8` (6): 53 tiled, 23 tiled cube, 1 linear
  - `k_8` (2): 16 linear, 2 tiled
  - `k_DXT2_3` (19): 9
  - formats 15, 23, 25, 26 and 36: a few each, mostly tiled render-target
    textures

  So most textures are block-compressed (BC1/BC3) and can be uploaded without
  decoding. Only the tiled ones need untiling (the SDK header
  `rex/graphics/pipeline/texture/util.h` declares `GetTiledOffset2D`).

### 19.3 Next in B3

1. Texture conversion: endian swap, untiling and Vulkan upload, done once per
   texture and again only after Lock/Unlock.
2. Vertex and index buffer upload (endian swap per declaration element).
3. Shader table: load the SPIR-V for each container hash from a private
   cache (built from the dumps with the B2 XenosRecomp).
4. Vulkan objects: a per-frame command buffer ring, pipeline layout for
   XenosRecomp's push-constant and shared-constant layout (B2 offsets).

### 19.3 Shaders must run on the SDK's Vulkan device (2026-11-02)

**Found (SDK source, `src/ui/vulkan/vulkan_device.cpp`):** The SDK's
`VulkanDevice` enables only a few Vulkan 1.2 features
(`samplerMirrorClampToEdge`, `uniformBufferStandardLayout`,
`scalarBlockLayout`). It does **not** enable `bufferDeviceAddress`,
`shaderInt64` or runtime descriptor arrays.

XenosRecomp's SPIR-V needs all three. It reads constants through 64-bit
buffer device addresses in push constants (`vk::RawBufferLoad`), and textures
through unbounded descriptor arrays. Unleashed creates its own device with
those features.

**Decision:** We do not change the SDK. Instead the patch gives XenosRecomp a
Vulkan 1.0 friendly binding model for SPIR-V (`tools/xenosrecomp/wwe2k15.patch`):

- **Set 0:**
  - `binding 0` `ByteAddressBuffer g_VertexShaderConstantsBuffer`
  - `binding 1` `g_PixelShaderConstantsBuffer`
  - `binding 2` `g_SharedConstantsBuffer` (same byte layout as in 18:
    booleans 512, swapped texcoords 516, half-pixel offset 520, alpha
    threshold 528, integer constants `1024 + 16·n`)
- **Set 1:**
  - `binding 0` `Texture2D[32]`, `binding 1` `Texture3D[32]`,
    `binding 2` `TextureCube[32]`, `binding 3` `SamplerState[32]`
  - **one slot per texture fetch constant**
- A post-pass (`convertToStorageBufferBindings`) rewrites the generated reads:
  - `vk::RawBufferLoad<float4>(…ShaderConstants + X, 0x10)` →
    `asfloat(g_…ShaderConstantsBuffer.Load4(X))`
  - the `int4` loads → `asint(…Load4(X))`
  - texture and sampler descriptor indices → the fetch-constant number itself
- The generated code indexes the arrays with compile-time constants, which is
  core Vulkan.
- `shader_common.h` used `1ull << semanticIndex` for a 32-bit flag word. That
  needed the `Int64` capability in 118 shaders; it is now `1u`.

**Result (confirmed):**

- All **218/218** used shaders still compile to SPIR-V (211 to DXIL).
- Across them the only SPIR-V capabilities are `OpCapability Shader` (218)
  and `OpCapability ImageQuery` (116).
- So they can run on the SDK device unchanged. The SPIR-V files are
  `out/xenos-hlsl-used/<container hash>.spv` (private).

**What the renderer must provide per draw:**

- set 0: the vertex constants (256 `float4` from the device shadow `+0x780`),
  the pixel constants (`+0x1780`) and the shared constants;
- set 1: the textures and samplers from the 32 texture fetch constants;
- vertex inputs at the locations of `USAGE_LOCATIONS` (and 16+ for the rest);
- the specialization constant 0 (`SPEC_CONSTANT_ALPHA_TEST` and others).

### 19.4 Vulkan core on the game side (2026-11-02)

**Found (SDK source, `src/ui/vulkan/vulkan_presenter.cpp`, lines 340/1499):**
The presenter's guest output image is created with
`TRANSFER_SRC | SAMPLED | COLOR_ATTACHMENT | STORAGE`, with **no
`TRANSFER_DST`**. The M3 test frame (14.7) cleared it with
`vkCmdClearColorImage`. That was not valid Vulkan, even though NVIDIA
accepted it. The image must be rendered to.

**New code:**

- `src/native/native_vulkan.cpp`: `VulkanCore`, enabled with
  `--wwe2k15_native_renderer=true` (only together with `--gpu_plugin=native`).
  - It reaches the SDK device and presenter through
    `rex::Runtime::instance()->graphics_system()`.
  - It owns a 1280x720 `R8G8B8A8_UNORM` back buffer (dedicated allocation
    through `vk_ui::util::CreateDedicatedAllocationImage`), a command pool and
    buffer, and a fence.
  - At every guest Present (called from the Present hook in
    `renderer_hooks.cpp`) it records the back buffer content, then a render
    pass into the guest output image: initial layout `COLOR_ATTACHMENT`, final
    layout `kGuestOutputInternalLayout`, plus an external dependency to
    `kGuestOutputInternalStageMask/AccessMask`. In that pass a full-screen
    triangle samples the back buffer. It submits inside
    `RefreshGuestOutput`.
  - B3 back-buffer content: a slowly pulsing clear colour. B4 replaces this
    with the game's draws.
- `src/native/shaders/present_blit.hlsl` → `present_blit_spirv.h`: SPIR-V
  words, compiled with the Microsoft-signed `dxc.exe` from the XenosRecomp
  checkout.
- `rexgpu-native` no longer presents; its invalid test frame is gone.
- `CMakeLists.txt`: the game links `rex::renderdoc`, for the
  `renderdoc_app.h` include of the SDK's Vulkan UI headers.

**Run** (`run-windows.ps1 -GpuPlugin native -GameArguments
'--wwe2k15_native_renderer=true'`, 40 s, closed after):

```
[native-renderer] present hook: frame 1
[native] renderer core ready: back buffer 1280x720
[native-renderer] present hook: frame 600     (10.0 s later)
[native-renderer] present hook: frame 1800
```

- **Confirmed:** The window shows the core's back buffer (screenshot: the
  dark pulsing clear colour). The game kept presenting at 60 fps, with no
  errors in the log.
- **Not yet:** textures, buffers, pipelines and draws (B3 remainder and B4).

## 20. B4 — first native frames (2026-11-02)

### 20.1 Draw-state fields (confirmed with a state dump)

`--wwe2k15_native_dump_draws=N` logs the first N inline-vertex draws
(`native_renderer.cpp`). Run with the emulator, 35 s:

```
inline draw: prim 13 count 4 stride 20 data B1E70520 decl AECFBD70
  [s0 off0 type002A23B9 usage0.0] [s0 off12 type002C23A5 usage5.0]
  vs AF20E400=B4AE0C55F46DFB37 ps AF210420=7AB203A69BD4AA3F
  tex slot 0: fmt 20 256x256 tiled 0 base 1556D000 endian 1
  first vertex: 44760000 43EC0000 3F800000 00000000 00000000
```

| Field | Location | What it holds |
| --- | --- | --- |
| vertex declaration | device `+0x2FD0` | elements with Xbox `D3DDECLTYPE`; the low 6 bits are the Xenos vertex format (`0x39` = 57 `k_32_32_32_FLOAT`, `0x25` = 37 `k_32_32_FLOAT`) |
| shaders | `+0x3310` / `+0x330C` | map through the registry to the container hashes |
| texture fetch constants | `+0x480 + slot * 0x18` | 6 words per slot, as `SetTexture` writes them |
| vertex data | | screen-pixel positions, big-endian |

### 20.2 The draw path

- **Inline-vertex draws:** `BeginVertices` `82BA3D08` and `EndVertices`
  `82BA3CF8` are now hooked in `native_renderer.cpp`; they were removed from
  the trace hooks. At EndVertices the game side builds a `NativeDrawState`
  and calls `NativeDraw`:
  - the vertices;
  - the declaration;
  - the shader hashes;
  - 256 vertex and 256 pixel `float4` constants from `+0x780` / `+0x1780`;
  - the 32 texture fetch constants.
- **The renamed struct:** it was first called `DrawState`. `WinUser.h`
  defines `DrawState` as `DrawStateA`, so the two files mangled it differently
  and the link failed. It is now `NativeDrawState` (`native_draw.h`).
- **`native_vulkan.cpp`, rewritten as the renderer core:**
  - **Frame:** one command buffer, with a render pass on the back buffer (clear
    to black) that the first draw opens. Present ends it, draws the back buffer
    to the presenter and submits.
  - **Upload ring:** 32 MB, host-visible and persistently mapped, flushed
    before submit. It holds the vertices (big-endian words swapped), the
    generated quad-list indices (`0,1,2, 0,2,3`) and the three constant blocks
    (vertex, pixel and shared, the shared one zeroed for now).
  - **Descriptors:** set 0 is static, with three dynamic storage buffers. Set 1
    is allocated per draw from a per-frame pool: 32 2D / 3D / cube images and
    32 samplers, with white 1×1 placeholders.
  - **Pipelines:** keyed by shader hashes, declaration, stride and topology.
    SPIR-V is loaded from `out/native-shaders/<hash>.spv`. The vertex
    attributes come from the declaration: XenosRecomp locations, formats
    `R32/R32G32/R32G32B32/R32G32B32A32_SFLOAT` and `B8G8R8A8_UNORM`. Blending
    is fixed standard alpha, there is no culling, and specialization constant
    0 is 0.
  - **Textures:** cached by fetch constant. Linear `k_DXT1/2_3/4_5` →
    `BC1/BC2/BC3`, `k_8_8_8_8` → `R8G8B8A8`, `k_8` → `R8`, with the endian swap
    from the fetch constant. Rows follow the fetch pitch, padded to 256 bytes.
    The upload is synchronous through a staging buffer. Tiled textures are
    still placeholders.
- **Shader cache:** `out/native-shaders` holds all 2,055 saved containers
  converted with the current XenosRecomp (1,198 SPIR-V; private game-derived
  data).

### 20.3 Result

**Run:** `run-windows.ps1 -GpuPlugin native -GameArguments
'--wwe2k15_native_renderer=true'`, 32 s, closed after.

```
[native] renderer core ready: back buffer 1280x720, shaders from ...\native-shaders
[native] frame 600: 8 draws, 8 drawn, skipped 0 (primitive) 0 (pipeline), 2 pipelines,
         8 textures (0 tiled placeholders), 0 shaders missing
[native] frame 1200: 15 draws, 15 drawn, ..., 5 pipelines, 33 textures (1 tiled placeholders),
         0 shaders missing
```

- **Confirmed (screenshots at 20 s and 32 s):** The **title screen is drawn by
  the native renderer**. The stadium lights, bokeh circles, the John Cena
  render and "PRESS START BUTTON" all come from the game's own D3D calls,
  XenosRecomp shaders and our textures, with no Xenos GPU emulation. The game
  ran at 60 presents per second.
- **Wrong so far (compared with the emulator's title screen):**
  1. **Red and blue look swapped** (the wrestler is bluish). Hypothesis: the
     fetch constant's component swizzle (`swizzle` in dword 3) is not applied
     yet.
  2. **The WWE 2K15 logo is missing.** Hypothesis: it is the one tiled
     texture (placeholder), or it is drawn through the indexed-draw path
     (`82BA41C8`), which is not connected yet.
  3. **The diagonal panel is too bright.** Blend states are fixed instead of
     read from the device (`RB_BLENDCONTROL`), and alpha test is off.

### 20.4 Register shadow blocks in the device (found 2026-11-03, not used yet)

The state flush (`sub_82BA3D08`) uploads these Xenos register blocks from the
D3D device object. Register `R` in a block lives at
`device + offset + (R - first) * 4`:

| First register | Device offset | Contains (examples) |
| --- | --- | --- |
| `0x2000` | `+0x2880` | `RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO` |
| `0x2100` | `+0x28CC` | `RB_COLOR_MASK` (`0x2104`), `RB_BLEND_*`, `RB_ALPHA_REF` (`0x210E`), viewport scale/offset |
| `0x2180` | `+0x2920` | `SQ_PROGRAM_CNTL`, `SQ_INTERPOLATOR_CNTL` |
| `0x2200` | `+0x2934` | `RB_DEPTHCONTROL`, `RB_BLENDCONTROL0` (`+0x2938`), `RB_COLORCONTROL` (`+0x293C`), `PA_SU_SC_MODE_CNTL` (`+0x2948`), `RB_MODECONTROL` (`+0x2954`) |
| `0x2280` | `+0x2964` | |
| `0x2300` | `+0x29B8` | |
| `0x2380` | `+0x2A50` | |

Evidence: the `ZENABLE`/`ZWRITEENABLE` setters flip bits 1/2 of `+0x2934`,
which are `RB_DEPTHCONTROL.z_enable` / `z_write_enable` (10.1).

**Next step:** decode blending, alpha test and depth for each draw from these
offsets, apply the texture swizzle (`xe_gpu_texture_fetch_t::swizzle`) in the
image view, and connect the vertex/index-buffer draw path `82BA41C8`.

### 20.7 Gamma ramp at present (2026-11-03)

**Symptom:** native frames were brighter and flatter than the emulator's
(loading screen at 60 s: heart red `236,28,35` against `202,11,15`, dark
background `27,33,35` against `11,15,16`). The loading screen's tight crop
("HATE" cut off on the right, the logo low) is not a bug: the emulator frames
it the same way at 55 s and 60 s.

**Cause:** the game writes a gamma ramp through the `DC_LUT_*` registers, and
the display controller applies it at scan-out. The emulator applies it in its
swap pass (the 256-entry table for an 8_8_8_8 front buffer, PWL for
2_10_10_10). The native present copied the back buffer unchanged.

**Fix:**
- The SDK `CommandProcessor` that rexgpu-native reuses already records the
  `DC_LUT_*` writes (`gamma_ramp_256_entry_table_`). The accessor is
  protected, so the plugin exports `wwe2k15_native_gamma_ramp(IGraphicsSystem*)`
  and the game side finds it with `GetProcAddress` on `rexgpu-native.dll`.
- At each present, `WriteGammaRamp` copies the 256 `DC_LUT_30_COLOR` words
  (blue bits 0–9, green 10–19, red 20–29) into the upload ring. The blit
  shader (`present_blit.hlsl`, binding 2, a dynamic storage buffer) looks each
  channel up as `ramp[round(c * 255)]`. Before the command processor exists
  it uses an identity ramp.
- The blit shader is compiled with XenosRecomp's DXC
  (`.tools/XenosRecomp/thirdparty/dxc-bin`). The Windows SDK's DXC has no
  SPIR-V output.

**Result:** the title screen at 45 s matches the emulator to within about
3/255 per channel, and the loading screen's flat areas match exactly. Only the
256-entry table is used; the PWL ramp (2_10_10_10 front buffers) is not.

**Still open on the loading screen:** a dark seam, one or two pixels high, at
about guest row 619 across the picture (`out/gamma-test/shot-60s.png`). It is
probably where two quads meet, with a half-pixel or clamping error.

### 20.8 Samplers from the fetch constant (2026-11-03)

**Symptom:** a dark line one or two pixels high across the loading-screen
picture (about guest row 619).

**Finding:** `--wwe2k15_native_dump_frame=3300` (new: it logs every inline
draw of one frame with all vertices, the target, the viewport and each
texture's sampler fields) showed the picture is two DXT5 tiles (format 20,
linear) drawn as quads. The 1024×512 tile is at game rows −16…496 and the
1024×256 tile at 496…704 (v 0…0.8125). Both use clamp mode 1, mirrored repeat,
with linear filtering. The core sampled every texture with REPEAT, so the last
row of each tile was blended with its first row.

**Fix:** `GetSampler` builds (and caches) a sampler from each fetch constant:
- **Addressing:** repeat stays repeat; mirrored repeat is mirrored repeat;
  clamp to edge, and the halfway clamps, become clamp to edge; the mirror-once
  clamps become mirrored repeat; the border clamps become clamp to border, with
  transparent black or opaque white from `border_color`.
- **Filtering:** `mag_filter`, `min_filter` and `mip_filter`, with point as
  nearest and everything else as linear. Mip filter "base map" limits
  sampling to level 0.
- **Not handled:** anisotropy, LOD bias and the shader's own filter overrides.

**Result:** the seam is gone (`out/sampler-test/shot-60s.png`). Row by row at
x=600 the native shot now matches the emulator (213 throughout). The title
screen is unchanged apart from where the animation happened to be.

**Also seen in the dump:** the post-processing passes (targets 320×180,
160×90, and the 1280×720 copy) draw with `PA_CL_VTE_CNTL = 0x400`: no
viewport scale or offset, positions in pixels (−0.5…319.5). They look right
on screen, but the core's viewport for VTE-off draws (the whole target) has
not been checked against this.

### 20.9 B5 starts: vertex inputs and per-draw cost in the demo (2026-11-03)

**Runs:** emulator `out/demo-emu/` and native `out/demo-native*/`, shots at
70–145 s, no input. Both reach the demo match at about 100 s.

**Symptom:** natively, the demo showed only the "DEMO / Press START" overlay.
`frame 4800: 1214 draws, 51 drawn, ... 1163 (pipeline)`: every 3D draw was
skipped because pipeline creation gave up on the vertex declaration:
- `usage 3.0 type 002A2191`: NORMAL0 in Xenos format 17 (k_11_11_10), signed
  and normalized.
- `usage 5.0 type 002C235F`: TEXCOORD0 in k_16_16_FLOAT.
- `usage 1.1 type 002C83A4`: BLENDWEIGHT1, float. It is not in XenosRecomp's
  location table (made for Sonic Unleashed). Such usages get locations 16 and
  up in each shader's own order.

**Fix 1: vertex inputs** (`native_vulkan.cpp`):
- **Locations from the shader:** each SPIR-V module's stage inputs are read
  back (`ReflectInputs`). DXC keeps the names `in.var.<SEMANTIC><index>`, so
  every declared element gets the location of its semantic in that shader.
  This replaces the copy of the Unleashed table. Elements the shader does not
  read are left out.
- **Formats:** `VertexFormat` decodes the whole D3DDECLTYPE:
  - bits 0–5 are the Xenos format, bit 8 is signed, bit 9 is integer (not
    normalized);
  - D3DCOLOR (`0x182886`) is the only BGRA one;
  - k_8_8_8_8, k_2_10_10_10, k_16_16 and k_16_16_16_16 map to the matching
    UNORM, SNORM, USCALED or SSCALED formats; the half and float formats map
    directly.
  
  The decoding of bits 8 and 9 comes from the known type values and is not
  yet checked against Xbox headers. Inputs the shader declares as `uint4`
  (NORMAL, TANGENT, BINORMAL, BLENDINDICES) get UINT formats with the raw
  bits.
- **Component order:** after the 32-bit swap on upload, the first component
  is in the low bits. That is how Xenos fetches it (the SDK's
  `spirv_translator_fetch.cpp`), so 16-bit pairs need no extra swap.
- **Packed normals:**
  - XenosRecomp unpacks normals in the shader (`tfetchR11G11B10`, behind
    `SPEC_CONSTANT_R11G11B10_NORMAL`, which pipelines now set). It reads x
    and y as 11 bits and z as 10, the k_10_11_11 layout.
  - This game's normals are k_11_11_10: x is 10 bits, then y and z are 11
    each.
  - So `RepackNormal` converts them on upload: x is doubled to 11 bits and
    z halved to 10.

**Result 1:** every draw gets a pipeline (`1589 draws, 1589 drawn`), but
the demo ran at about 6 fps. It started about 30 s late, and the scene was
still almost black.

**Fix 2: per-draw cost.** New frame reports (every 600 frames, or every
5 s when frames are slow) give upload sizes and timings:
```
frame 5089 uploads: 12711 KB of vertices from 327 buffers, ring 32371 KB, 0 ring overflows
frame 4518 times: draws 137.1 ms (texture creation 0.0 ms, 0 textures), fence wait 20.7 ms, frame 174.7 ms
```
About 100 µs per draw, all on the CPU in the core:
- `GetTexture` sampled the contents of every bound texture on every draw:
  64 bytes per 4 KB page, so 256 FNV samples for a 1 MB texture, in each
  slot. Lookups are now cached for the frame (`frame_textures_`). A texture
  the CPU rewrites in the middle of a frame is only noticed at the next
  frame.
- The normal repack read back from the upload ring, which is write-combined
  memory. It now repacks while swapping and never reads the ring.

**Result 2:** `draws 9.8 ms, fence wait 3.1 ms, frame 33.0 ms`. The demo
starts on time, at 100 s, as in the emulator. The title screen is unchanged
(mean difference from the emulator 3.0, before 3.1).

**Still wrong:** the 3D picture is almost black. Only pieces of the crowd,
barricade and floor show, at the bottom left
(`out/demo-native5/shot-115s.png`). The demo frame uses 15 render targets
and about 50 resolves. The main scene target is `surface_info 0A010280`:
pitch 640, MSAA bits 1 (2×), with a depth buffer (`depth_info 000102E0`).
The core still has no depth buffer, treats MSAA targets as single-sampled,
and sizes targets from the pitch with a height of 1024. These are the next
B5 items.

**Upload volume:** every indexed draw copies its vertex buffer, from the
start up to the highest index it uses. That is 11–12 MB from about 320
buffers per frame. With about 15 MB of constants, the 32 MB ring is about
85% full. A per-buffer cache, the design in section 16.2, is still to do
(done in 20.15).


### 20.10 B5: depth, tiled MSAA targets, shader constants, resolves (2026-11-03)

**Depth buffers.**
- Every render target has a `D32_SFLOAT_S8_UINT` depth image. Render passes
  have it as attachment 1.
- The game uses reversed depth: `PA_CL_VPORT_ZSCALE` -1, `ZOFFSET` 1,
  compare `GREATER_EQUAL` (`RB_DEPTHCONTROL 00700766` and similar), cleared
  to 0.
- `DepthStencilState` builds the pipeline state from `RB_DEPTHCONTROL` and
  `RB_STENCILREFMASK(_BF)` (registers 0x210C/0x210D). Xenos compare
  functions and stencil ops are numbered like `VkCompareOp`/`VkStencilOp`.
- Viewport min/max depth come from `ZOFFSET`/`ZSCALE` when
  `PA_CL_VTE_CNTL` bits 4–5 are set.

**Clears (82BA7AA0)** take r4 count, r5 rects, r6 flags, r7 color (ARGB),
f1 z and r8 stencil. Flags seen in the demo: 0x01, 0x0F, 0x10, 0x1F, 0x3F, so
targets are 0x0F, `ZBUFFER` 0x10 and `STENCIL` 0x20. The first guess
(stencil 0x10) was wrong: 0x10 appears on its own.

**Resolves (82BA2370)** take r4 flags, r5 rect, r6 destination, r10 clear
color pointer and f1 clear z. Flags: `DEPTHSTENCIL` 0x4,
`CLEARRENDERTARGET` 0x100, `CLEARDEPTHSTENCIL` 0x200.
- A depth resolve (flags 0x14, to a `k_24_8_FLOAT` texture at F0E2C000)
  copies the depth aspect through a 2048×1024×4 buffer into an
  `R32_SFLOAT` image.
- The clear-after-resolve is done with `vkCmdClearAttachments` on the
  resolved rect.

**Predicated tiling.** The main scene targets are `05020140` (4×, pitch
320) and `0A010280` (2×, pitch 640), with a 1280×720 viewport. The game
issues each draw once and the GPU replays it per EDRAM tile. Anti-aliased
targets are now made at least 1280 wide, so one draw covers the whole
screen. The scene resolve to F123C000 is a full 1280×720 image. Rendering is
still 1× (no real MSAA).

**Boolean and loop constants.** These were always zero, so lighting
branches and the skinning bone loops never ran. That is why the scene was
black and the wrestlers were spikes.
- `SetVertexShaderConstantB` (82BA2FF0) writes bit `n & 31` of dword
  `0x9E0 + (n >> 5)`: vertex booleans at device +0x2780, pixel booleans at
  +0x2790.
- XenosRecomp tests bit n for vertex shader bn and bit 16 + n for pixel
  shader bn, in `g_Booleans` (shared constants offset 512).
- Loop constants are read from +0x27A0 (32 dwords, count in bits 0–7, start
  8–15, step 16–23) and written as `int4(count, start, step, 0)` at
  1024 + 16 × id. The address is an inference (it follows the booleans as
  on the GPU). It was not checked against the game's `SetShaderConstantI`.

**Screen-space draws.** Post-processing quads draw with the viewport
transform off (`PA_CL_VTE_CNTL 0x400`). Their vertices are pixel
coordinates (logged: `-0.5 … 255.5` with w 1 for a 256×256 target), which
Vulkan clips away. A plain
viewport cannot fix this.
- XenosRecomp now emits
  `oPos.xy = oPos.xy * g_PositionScale + g_HalfPixelOffset * oPos.w`, with
  `g_PositionScale` at shared constants offset 536
  (`tools/xenosrecomp/wwe2k15.patch`).
- The 324 compiled vertex shaders in `out/native-shaders` were patched and
  recompiled with the same DXC arguments (scratchpad
  `position_scale.py`); all compiled.
- The renderer sets scale (1, 1) and offset (0, 0) normally. With VTE bits
  0–3 clear it sets scale (2/width, -2/height) and offset (-1, 1) for a
  full-target viewport. The y sign is flipped back by `-fvk-invert-y`.

**Resolve addresses.**
- A resolve takes its destination from the texture object, which holds the
  CPU address, for example FF5A0000.
- A draw's fetch constant holds the physical address. For
  0xE0000000–0xFFFFFFFF that is 4 KB higher: 1F5A1000.
- So no resolved render target was ever found when sampled. Post
  processing read black, and so did the crowd impostor sprites. Both are now
  keyed by physical page (`PhysicalPage`).

**Debugging aid.** `--wwe2k15_native_dump_resolves=<frame>` writes every
resolve and the back buffer of that frame as BMPs to `native-dumps/frame-N`,
and logs each draw (target, shaders, viewport, VTE, depth, blend, booleans,
loops; vertices for screen-space draws).

**Result (confirmed, `out/demo-native11`).**
- The demo match renders natively at 90–94 FPS, as the emulator does: the
  lit and skinned wrestlers, ring, ropes, crowd and arena, with post
  processing.
- Compared with `out/demo-emu/shot-115s.png`:
  - crowd faces look like masks (the impostor atlas is not quite right);
  - skin is too orange;
  - the ring-post logo is missing;
  - the picture is a little brighter and bluer.
- Likely causes: render-target formats (`k_16_16`, `k_16_16_16_16` and
  `k_2_10_10_10` targets are still RGBA8) and no real MSAA. The buffer
  cache (section 16.2) is also still to do.

### 20.11 B5: render-target formats (2026-11-03)

**Formats.** `TargetFormat` gives each EDRAM color format its own Vulkan
format, in place of RGBA8 for all:

| Xenos `ColorRenderTargetFormat` | Vulkan |
|---|---|
| `k_8_8_8_8`, `k_8_8_8_8_GAMMA` | `R8G8B8A8_UNORM` (gamma stays UNORM, as before) |
| `k_2_10_10_10`, `_AS_10_10_10_10` | `A2B10G10R10_UNORM_PACK32` |
| `k_16_16`, `k_16_16_FLOAT` | `R16G16_SFLOAT` |
| `k_16_16_16_16`, `k_16_16_16_16_FLOAT`, `k_2_10_10_10_FLOAT(_AS_16_16_16_16)` | `R16G16B16A16_SFLOAT` |
| `k_32_FLOAT`, `k_32_32_FLOAT` | `R32_SFLOAT`, `R32G32_SFLOAT` |

- Render passes are made per format (`GetPasses`), and pipelines are keyed
  by format.
- Half floats hold the -32…32 range of the fixed 16-bit formats directly.
  The 7e3 float format is approximated by them.
- In the demo the targets are `08000230/00040000` (`k_16_16`, resolved to
  FF6FE000) and `08000230/00050000` (`k_16_16_16_16`, resolved to FF7FE000
  and FF9FE000). These are 512×512 per-wrestler atlases.

**Resolving the fixed formats.** The GPU resolves `k_16_16` and
`k_16_16_16_16` to unsigned data: 0…1 of the -32…32 values. This is the
SDK's note on `IsColorResolveFormatBitwiseEquivalent`. The renderer does
that with a converting `vkCmdBlitImage` (half float to `R16G16(B16A16)_UNORM`,
which clamps). Other formats are copied with `vkCmdCopyImage`.

What didn't work, in order:
- **SNORM targets with the output scaled by 1/32, copied bit for bit and
  read as UNORM.** This is how the SDK's full-range emulation stores the
  targets, but the reads came out at v/64.
- **Scaling with blend constants.** Vulkan clamps a fragment output to
  [-1, 1] for SNORM attachments *before* blending, so values above 1 were
  lost.

**Exponent bias.** The pixel shader now multiplies `oC0` by `g_OutputScale`
(shared constants offset 544) after the alpha test. The renderer sets it to
2^`RB_COLOR_INFO.color_exp_bias`. The bias is 0 in the demo.
- XenosRecomp emits this (`tools/xenosrecomp/wwe2k15.patch`).
- The 426 compiled pixel shaders in `out/native-shaders` were patched and
  recompiled (scratchpad `output_scale.py`). The other 149 are empty and
  never write a color.

**Resolve swizzle.** A resolved texture is read through a view with the
destination's fetch swizzle, for example `k_16_16`'s `B48` (X, Y, 1, 1).
- ARGB textures (swizzle `60A`, red read from Z) get red and blue swapped by
  the resolve on the GPU (`RB_COPY_DEST_INFO.copy_dest_swap`), so the two
  swaps cancel.
- The copy keeps the target's order, so `ResolveSwizzle` swaps X and Z back
  (`60A` becomes identity, `A0A` becomes X, Y, Z, 1).
- Applying `60A` as is turned skin and clothes blue.
- The sampling fetch constants match the resolve destinations (same format,
  sign, exponent, swizzle and endianness), as the debug log shows (`samples
  resolved`).

**Dumps** decode the 10-bit, 16-bit UNORM/SNORM/float and 32-bit float
formats (`DumpTexel`).

**Result (`out/demo-native18`).**
- The demo renders at 92 FPS with the same hues as the emulator: skin
  tones, purple posts, red and blue crowd.
- Still different from `out/demo-emu/shot-115s.png`:
  - skin is a little too orange and patchy (fixed in 20.12);
  - crowd faces look like masks (fixed in 20.12);
  - the ring-post logo is missing (fixed by 20.12, see 20.14).

**Not checked.**
- Format support for `R16G16_UNORM` as a blit destination and for the
  half-float targets as blended attachments. Desktop GPUs support both;
  nothing checks at runtime.
- The red/blue swap rule is inferred from the swizzle, because the game's
  D3D call doesn't expose `RB_COPY_DEST_INFO`.


### 20.12 Crowd faces: 16-bit vertex components (2026-11-03)

**Symptom.** Crowd members had mask-like faces: heads showed the shirt
colour, torsos showed skin, and the jeans were fine.

**Crowd impostors.** Each crowd member is one draw (vs `D0A57FBD5FED26A7`,
ps `A78AAAD35075DD6A`, about 225 vertices) into target `01000050`, resolved
to a 64×64 texture (F6DDD000 to F6E29000).
- The pixel shader is diffuse × color0.
- The diffuse texture is a per-member 256×256 linear DXT1 (16F95000 to
  17079000, 0xC000 apart).
- Decoding the raw dumps (`tex-<base>-dxt1-WxH-endianN.bin`, scratchpad
  `dxt1.py`) showed the textures are correct: face bottom left, shirt top
  right, skin top left, jeans bottom right.
- So the texture coordinates were wrong. The pattern (top right ↔ bottom
  left swapped, the diagonal corners unchanged) means **u and v were swapped**.

**Cause.** `TEXCOORD0` is `FLOAT16_2` (`D3DDECLTYPE 002C235F`, Xenos format
31, `k_16_16_FLOAT`). After the 8in32 swap of the word, the first component
is in the high 16 bits, but Vulkan's `R16G16_SFLOAT` reads x from the low 16
bits.

**Fix.** `CreatePipeline` records every element with two 16-bit components
per word in `Pipeline::swapped_halves`:
- `k_16_16` (25) and `k_16_16_FLOAT` (31): one word.
- `k_16_16_16_16` (26) and `k_16_16_16_16_FLOAT` (32): two words.

The upload swaps the halves of those words after the endian swap, as it
already repacks `k_2_10_10_10` normals (`repacked_normals`).

**Also added while looking.**
- The vertex fetch constant's endian mode (low 2 bits of the size word) is
  now honoured per stream (`NativeDrawState::Stream::endian`). Every stream
  in the demo is 8in32, so this changed nothing yet.
- Unsupported texture formats are logged once per format. Only format 36
  (one 16×16 texture) and resolve destinations appeared.

**Result (`out/demo-native19`).** Crowd faces, shirts and skin are where
they belong. The wrestlers' skin is smooth too, so its patchiness had the
same cause.

### 20.13 Real MSAA on anti-aliased targets (2026-11-03)

The game draws the demo's main scene into `0A010280` (2×). The 4× target
`05020140` (section 20.10) was not created in this run. Until now both were
drawn single-sampled.

**What it does now.**
- A target whose `RB_SURFACE_INFO` asks for 2× or 4× gets multisampled
  colour and depth images (`RenderTarget::msaa_image`, `msaa_depth`) with
  that sample count. It keeps its single-sampled `image` and `depth`.
- Its render passes (`CreateMsaaRenderPass`, render pass 2) draw into the
  multisampled images and resolve them at the end of every pass:
  - colour by averaging the samples, into `image`;
  - depth and stencil from sample 0 (`VK_RESOLVE_MODE_SAMPLE_ZERO_BIT`),
    into `depth`.
- The render area is always the whole target, so the resolves overwrite
  all of `image` and `depth`. Everything that reads a target (the game's
  resolves, `CopyColor`, `CopyDepth`, the dumps) still reads the
  single-sampled images, unchanged.
- Pipelines and render passes are keyed by sample count as well as format.
- `--wwe2k15_native_msaa=false` turns it off. It is also off if the device
  has no `vkCreateRenderPass2`.

**Result (`out/demo-native20`).** The scene target is created with
`samples 2`. The picture is the same as before, at the same frame rate
(68 FPS against 69 at the same point of the demo, RTX 3050 Laptop). No
Vulkan errors appeared. The game blurs and composites the scene afterwards,
so the smoother edges are hard to see in screenshots.

**Not done.**
- Alpha to coverage (`RB_COLORCONTROL` alpha-to-mask) is not mapped
  (done in 20.14).
- The Xenos sample positions are not reproduced; Vulkan's standard
  positions are used.
- Each pass on the target resolves the whole 1280×1024 image, even if the
  pass drew a small part. That cost wasn't measurable here.
- Sample count support is not queried. Desktop GPUs support 2× and 4× for
  these formats.

### 20.14 Alpha to coverage and the ring-post logo (2026-11-03)

**Alpha to coverage.** `RB_COLORCONTROL.alpha_to_mask_enable` (bit 4) now
turns on `alphaToCoverageEnable` in the pipeline. The bit is part of the
pipeline key.
- Xenos dithers the coverage with per-pixel offsets (bits 24–31). These are
  not reproduced; Vulkan's own alpha-to-coverage pattern is used.
- **The demo never sets the bit.** Every pipeline's `RB_COLORCONTROL` is
  `87000004`–`8700000F`. Hair, fences and crowd cut-outs use the plain
  alpha test, so this changes nothing in the demo. It is there for scenes
  that do use it.

**Ring-post logo.** The post pads' WrestleMania lettering and gold stripe,
plain purple in `out/demo-native18`, now render as in the emulator
(`out/demo-native21`). The 16-bit texture-coordinate fix (20.12) brought
them back, so no other change was needed.

**Result (`out/demo-native21`).** The run is clean at 70 FPS, with MSAA on
and no Vulkan errors.

### 20.15 Vertex stream cache (2026-11-03)

**Before.** Every indexed draw copied its vertex buffers out of guest memory
into a `std::vector`, then byte-swapped them byte by byte into the upload
ring. That was about 11.5 MB from about 318 buffers per frame, and the ring
was about 26 MB of 32 MB full.

**Now.**
- `NativeOnIndexedDraw` no longer copies. `NativeDrawState::Stream` points
  at the guest bytes (`guest`, `guest_size`), which `NativeDraw` reads in
  place. It is synchronous, under `g_core_mutex`. Inline (`DrawPrimitiveUP`)
  vertices still come in `data`.
- `ConvertStream` does the conversion: a plain `bswap32` for 8in32 streams,
  then the repacked normals and swapped 16-bit halves of section 20.12.
- `UseCachedStream` keeps converted guest ranges in a 32 MB host-visible
  buffer (`stream_cache_buffer_`). The key is the address, size, stride,
  endian mode, and the pipeline's repacked elements for that stream. A
  range is reused while the XXH3 hash of its guest bytes is unchanged.
  Each draw hashes again, so a buffer the game rewrites is always seen.
- **Changed contents** are converted in place, unless a draw earlier in the
  same frame already used the range. The GPU hasn't read it yet in that
  case, so the draw uses the ring instead. Only one frame is in flight, and
  `BeginFrameIfNeeded` waits for it, so last frame's ranges are free to
  overwrite.
- **Ranges that change 3 frames in a row** stop being cached and go through
  the ring, which saves hashing them.
- **When the cache fills up**, it is emptied at the start of the next frame.
- `--wwe2k15_native_stream_cache=false` turns it off.

**Result.** These figures are from the demo match, frames 3600 and later.

| | Cache off | Cache on |
| --- | --- | --- |
| Vertices converted per frame | 11.1 MB | 0.55 MB (10.5 MB from the cache) |
| Upload ring used | 26 MB | 15 MB |
| Draw CPU time per frame, match windows | 10.5–12.2 ms | 8.3–9.1 ms |
| Frames in the same stretch | 2256 | 2592 |

- The cache held about 5.6 MB in about 630 ranges, with no flushes.
- The game caps the demo at about 30 game frames per second either way. The
  saving is CPU time on the render thread.
- Each window's average is in the log line `average over N frames: draws`.

**Notes.**
- At 128 MB the cache made the NVIDIA overlay's FPS counter disappear from
  screenshots. At 32 MB it is still missing, so memory was not the cause.
  The counter belongs to the NVIDIA overlay (`VK_LAYER_NV_present`), not to
  the game or the SDK, so this was not investigated further.
- **Blank crowd signs** (plain white, black or yellow) appear with and
  without the cache. The log shows two 512×512 textures in formats 25
  (`k_16_16`) and 26 (`k_16_16_16_16`) that `CreateTexture` does not
  support yet. They are the likely cause. Not fixed yet.

### 20.16 16-bit textures and the loop constants (2026-11-03)

**Loop constant address: checked.**
- `SetVertexShaderConstantI` (82BA30B0) writes `i0`–`i15` to device
  +0x27A0. `SetPixelShaderConstantI` (82BA3108) writes them to +0x27E0, which
  are loops 16–31.
- Each constant is stored as `count | start << 8 | step << 16`. These come
  from the low bytes of the int4's x, y and z.
- Then both functions set bit 56 of the dirty mask at +0x20.
- This matches what `NativeOnIndexedDraw` already reads: 32 dwords from
  +0x27A0. The inference in 20.10 was right, and the code didn't change.

**Texture formats 25 and 26.**
- `CreateTexture` now loads `k_16_16` as `R16G16` and `k_16_16_16_16` as
  `R16G16B16A16`. Both are UNORM, or SNORM when sign X is signed. Both go
  through the usual tiling and endian path.
- The demo binds two of them when the match starts:

| Address | Format | Size | Signs | Swizzle | Endian | Tiled |
| --- | --- | --- | --- | --- | --- | --- |
| 1F6FF000 | 25 | 512×512 | unsigned | X, Y, 1, 1 | 8in32 | yes |
| 1F7FF000 | 26 | 512×512 | unsigned | XYZW | 8in16 | yes |

- **Not checked:** whether format 25 with 8in32 needs its 16-bit halves
  swapped, like the vertex components in 20.12. The texture is loaded
  without that swap, which is how the SDK reads it.
- **Picture:** no change that stands out. The crowd sign at 100 s still
  shows mostly white, with part of its blue text. So these two textures
  were not the cause of the blank signs. What they hold in the scene is
  unknown.
- **Next unsupported format in the log:** 36 (`k_32_FLOAT`), a 16×16
  texture at 10AFF000 during the loading screen.
