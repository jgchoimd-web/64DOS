#!/usr/bin/env python3
"""Check external tools used by the 64DOS build and smoke tests."""

import shutil
import sys


TOOLS = {
    "nasm": ["nasm"],
    "x86-64 C compiler": ["gcc", "cc", "clang"],
    "GNU linker": ["ld"],
    "objcopy": ["objcopy"],
    "QEMU x86_64": ["qemu-system-x86_64"],
    "mtools": ["mcopy"],
}


def main():
    missing = []
    for label, candidates in TOOLS.items():
        found = next((tool for tool in candidates if shutil.which(tool)), None)
        if found:
            print(f"{label}: {found}")
        else:
            missing.append(f"{label} ({', '.join(candidates)})")
    if missing:
        print("Missing required tools:", file=sys.stderr)
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

