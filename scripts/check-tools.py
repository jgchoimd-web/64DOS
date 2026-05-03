#!/usr/bin/env python3
"""Check external tools used by the 64DOS build and smoke tests."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import sys

TOOL_GROUPS = {
    "build": {
        "nasm": {"env": "NASM", "candidates": ["nasm"]},
        "x86-64 C compiler": {"env": "CC", "candidates": ["gcc", "cc", "clang"]},
        "GNU linker": {"env": "LD", "candidates": ["ld", "x86_64-elf-ld"]},
        "objcopy": {"env": "OBJCOPY", "candidates": ["objcopy", "x86_64-elf-objcopy"]},
    },
    "smoke": {
        "QEMU x86_64": {"env": "QEMU", "candidates": ["qemu-system-x86_64"]},
    },
    "compat": {
        "mtools": {"env": None, "candidates": ["mcopy"]},
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


def resolve_tool(env_name: str | None, candidates: list[str]) -> str | None:
    if env_name and (override := os.environ.get(env_name)):
        try:
            cmd = shlex.split(override)[0]
        except ValueError:
            return None
        return cmd if shutil.which(cmd) else None

    return next((tool for tool in candidates if shutil.which(tool)), None)


def main() -> int:
    args = parse_args()

    missing: list[str] = []
    for group in selected_groups(args.profile):
        print(f"[{group}]")
        for label, config in TOOL_GROUPS[group].items():
            found = resolve_tool(config["env"], config["candidates"])
            if found:
                print(f"{label}: {found}")
            else:
                env_hint = f" via ${config['env']}" if config["env"] else ""
                missing.append(f"{label}{env_hint} ({', '.join(config['candidates'])})")

    if missing:
        print("Missing required tools:", file=sys.stderr)
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
