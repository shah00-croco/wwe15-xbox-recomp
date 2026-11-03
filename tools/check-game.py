#!/usr/bin/env python3
"""Check that assets/ holds the game version this project was recompiled from.

The function boundaries in wwe2k15_manifest.toml match one exact default.xex,
so a different region or revision would build but not run. Read-only.
Usage: python3 tools/check-game.py   (Windows: py -3 tools\\check-game.py)
"""

import hashlib
from pathlib import Path
import sys

ASSETS = Path(__file__).resolve().parent.parent / "assets"
# Title ID 545408B6, media ID 77BB6C49, executable version 4.
XEX_SHA256 = "1aaf88a63a57ad3435e5db6b228809d06929a07bd8cb7296cfba0d5a903e0503"
REQUIRED = (
    "pac/_startup.pac",
    "sound/Comm.pck",
    "sound/Game_Sound_M.pck",
    "plist360.arc",
    "plist360.def",
)


def main():
    xex = ASSETS / "default.xex"
    if not xex.is_file():
        print(f"Missing {xex}. Extract your own copy of the game into assets/.")
        return 1
    digest = hashlib.sha256(xex.read_bytes()).hexdigest()
    if digest != XEX_SHA256:
        print(f"default.xex SHA-256 is {digest},\nexpected {XEX_SHA256}.")
        print("This is a different game version; the manifest will not match it.")
        return 1
    missing = [name for name in REQUIRED if not (ASSETS / name).is_file()]
    for name in missing:
        print(f"Missing assets/{name}")
    if missing:
        print("Copy the complete game folder into assets/, keeping its layout.")
        return 1
    print("Game files OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
