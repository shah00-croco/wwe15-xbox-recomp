# WWE 2K15 (Xbox 360) - ReXGlue recompilation

A work-in-progress static recompilation of the Xbox 360 version of WWE 2K15
with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (v0.10.0,
revision c94f5eb), laid out like the Naruto Storm 3 Full Burst project.

This repository holds only the manifest, source and scripts. It contains no
game files and no code generated from them: bring your own copy of the game.

## Game version

The function boundaries in `wwe2k15_manifest.toml` match one exact executable:

| Field | Value |
| --- | --- |
| `default.xex` SHA-256 | `1aaf88a63a57ad3435e5db6b228809d06929a07bd8cb7296cfba0d5a903e0503` |
| Title ID | `545408B6` |
| Media ID | `77BB6C49` |
| Executable version | `0.0.0.4` |
| Disc format | XGD3 (game partition at `0x02080000`) |

## Setup (Windows x64)

1. Extract your own disc image into `assets/` with
   [extract-xiso](https://github.com/XboxDev/extract-xiso):
   `extract-xiso -x <your.iso>` writes a folder named after the image; rename
   it to `assets`. Then check it: `py -3 tools\check-game.py`.
2. Build the ReXGlue SDK and install it (see the SDK wiki, Getting Started).
3. From an x64 Developer PowerShell:

   ```powershell
   .\build-windows.ps1 -SdkPrefix <sdk>\out\install\win-amd64
   ```

   Pass `-ToolsRoot <folder>` to use a portable LLVM, CMake and Windows SDK
   laid out like the Naruto project's `.tools`.
4. Run: `.\run-windows.ps1 -Backend vulkan [-VulkanDevice N]`. Shader caches go
   to `out/cache` unless `-CacheRoot` says otherwise.

   Add `-Keyboard` to map the keyboard to a controller (Enter/X = Start,
   Space = A, Backspace = B, WASD = left stick, arrows = right stick).

## Status (2026-11-02, Vulkan, RTX 3050 Laptop)

Boots to the title screen ("PRESS START BUTTON") and plays the attract-mode
demo match; it ran for more than 90 seconds without a crash. Menus, input
handling in game and matches are **not verified**.

The manifest holds 314 function boundaries. Earlier boots stopped with
`[FATAL] Call to invalid or unregistered function at guest address ...`
(0x83455738, 0x82AA8B00, 0x830FF0C0): small thunks and leaves that analysis
had merged into the preceding function. Each manifest entry was checked by
disassembly; the per-entry evidence is in `reports/*.json`.

Known noise in the log, not yet investigated:

- About 460 `PhysicalHeap::Alloc unable to alloc physical memory` errors in
  the first second, apparently the game probing memory sizes (unconfirmed).
- `Texture fetch constant ... has "invalid" type!` warnings while drawing.
- `NtCreateFile` on `UPDATE:\plist360.arc` fails: no title-update device.
