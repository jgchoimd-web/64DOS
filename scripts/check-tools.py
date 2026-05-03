#!/usr/bin/env python3
"""Check external tools used by the 64DOS build and smoke tests."""

from __future__ import annotations

import argparse
import shutil
import sys

TOOL_GROUPS = {
    "build": {
        "nasm": ["nasm"],
        "x86-64 C compiler": ["gcc", "cc", "clang"],
        "GNU linker": ["ld", "x86_64-elf-ld"],
        "objcopy": ["objcopy", "x86_64-elf-objcopy"],
    },
    "smoke": {
        "QEMU x86_64": ["qemu-system-x86_64"],
    },
    "compat": {
        "mtools": ["mcopy"],
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile",
        choices=("all", "build", "smoke", "compat"),
        default="all",
        help=(
            "Select which tool set to validate. "
            "'all' checks build + smoke + compat (default)."
        ),
    )
    return parser.parse_args()


def selected_groups(profile: str) -> list[str]:
    if profile == "all":
        return ["build", "smoke", "compat"]
    return [profile]


def main() -> int:
    args = parse_args()

    missing: list[str] = []
    for group in selected_groups(args.profile):
        print(f"[{group}]")
        for label, candidates in TOOL_GROUPS[group].items():
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
