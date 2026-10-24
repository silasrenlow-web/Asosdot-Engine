#!/usr/bin/env python3
"""Package the export templates built in bin/ into an Asosdot .tpz, for the editor's Install from File."""

from __future__ import annotations

import argparse
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# What scons builds, and the name Godot's exporters look for in the templates folder.
TEMPLATES = {
    "asosdot.windows.template_release.x86_64.exe": "windows_release_x86_64.exe",
    "asosdot.windows.template_release.x86_64.console.exe": "windows_release_x86_64_console.exe",
    "asosdot.windows.template_debug.x86_64.exe": "windows_debug_x86_64.exe",
    "asosdot.windows.template_debug.x86_64.console.exe": "windows_debug_x86_64_console.exe",
    "asosdot.windows.template_release.arm64.exe": "windows_release_arm64.exe",
    "asosdot.windows.template_release.arm64.console.exe": "windows_release_arm64_console.exe",
    "asosdot.windows.template_debug.arm64.exe": "windows_debug_arm64.exe",
    "asosdot.windows.template_debug.arm64.console.exe": "windows_debug_arm64_console.exe",
    "asosdot.linuxbsd.template_release.x86_64": "linux_release.x86_64",
    "asosdot.linuxbsd.template_debug.x86_64": "linux_debug.x86_64",
    "asosdot.linuxbsd.template_release.arm64": "linux_release.arm64",
    "asosdot.linuxbsd.template_debug.arm64": "linux_debug.arm64",
    "asosdot.web.template_release.wasm32.zip": "web_release.zip",
    "asosdot.web.template_debug.wasm32.zip": "web_debug.zip",
    "asosdot.web.template_release.wasm32.nothreads.zip": "web_nothreads_release.zip",
    "asosdot.web.template_debug.wasm32.nothreads.zip": "web_nothreads_debug.zip",
    "macos.zip": "macos.zip",
    "ios.zip": "ios.zip",
    "android_release.apk": "android_release.apk",
    "android_debug.apk": "android_debug.apk",
    "android_source.zip": "android_source.zip",
}


def templates_version() -> str:
    """The folder the editor reads templates from: "asosdot." + Asosdot's version, as in core/version.h."""
    text = (ROOT / "version.py").read_text(encoding="utf-8")
    fields = dict(re.findall(r'^(asosdot_\w+)\s*=\s*"?([^"\n]+)"?', text, re.M))
    version = f"{fields['asosdot_major']}.{fields['asosdot_minor']}"
    if int(fields["asosdot_patch"]):
        version += f".{fields['asosdot_patch']}"
    return f"asosdot.{version}.{fields['asosdot_status']}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, default=ROOT / "bin")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    version = templates_version()
    output = args.output or args.bin / f"Asosdot-Export-Templates-{version.removeprefix('asosdot.')}.tpz"
    found = [(args.bin / built, name) for built, name in TEMPLATES.items() if (args.bin / built).is_file()]
    if not found:
        raise SystemExit(f"No export templates found in {args.bin}. Build them first, for example:\n  scons platform=windows target=template_release arch=x86_64 d3d12=yes")
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as tpz:
        tpz.writestr("templates/version.txt", version)
        for path, name in found:
            tpz.write(path, f"templates/{name}")
            print(f"  {name}  ({path.stat().st_size / 1e6:.1f} MB)")
    print(f"{output}  (version {version})")


if __name__ == "__main__":
    main()
