#!/usr/bin/env python3
"""Check external tools used by the 64DOS build and test workflows."""

from __future__ import annotations

import argparse
import shutil
import sys

TOOL_GROUPS = {
    "build": {
        "nasm": ["nasm"],
        "x86-64 C compiler": ["gcc", "cc", "clang"],
        "GNU linker": ["ld"],
        "objcopy": ["objcopy"],
        "python3": ["python3"],
    },
    "test": {
        "python3": ["python3"],
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
        choices=("build", "test", "smoke", "compat", "all"),
        default="build",
        help="tool profile to validate (default: build)",
    )
    return parser.parse_args()


def selected_tools(profile: str) -> dict[str, list[str]]:
    if profile == "all":
        merged: dict[str, list[str]] = {}
        for group in ("build", "test", "smoke", "compat"):
            merged.update(TOOL_GROUPS[group])
        return merged
    return TOOL_GROUPS[profile]


def main() -> int:
    args = parse_args()
    tools = selected_tools(args.profile)

    missing: list[str] = []
    print(f"Checking tool profile: {args.profile}")
    for label, candidates in tools.items():
        found = next((tool for tool in candidates if shutil.which(tool)), None)
        if found:
            print(f"  OK   {label}: {found}")
        else:
            missing.append(f"{label} ({', '.join(candidates)})")
            print(f"  MISS {label}: {', '.join(candidates)}")

    if missing:
        print("\nMissing required tools:", file=sys.stderr)
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        return 1

    print("All required tools are available.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
