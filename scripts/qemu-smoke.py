#!/usr/bin/env python3
"""Boot a 64DOS floppy image in QEMU and assert serial output."""

import argparse
import shutil
import subprocess
import sys


EXPECTED = [
    "64DOS stage1",
    "64DOS stage2",
    "64DOS BIOS x86-64",
    "A: mounted read-only",
    "64DOS AUTOEXEC",
    "64DOS v0.1",
    "README.TXT",
    "A:\\>",
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--timeout", type=float, default=12.0)
    args = parser.parse_args()

    if not shutil.which(args.qemu):
        print(f"{args.qemu} not found", file=sys.stderr)
        return 1

    cmd = [
        args.qemu,
        "-drive",
        f"file={args.image},format=raw,if=floppy",
        "-boot",
        "a",
        "-serial",
        "stdio",
        "-display",
        "none",
        "-no-reboot",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()

    missing = [text for text in EXPECTED if text not in out]
    if missing:
        print(out)
        print("Missing expected output:", file=sys.stderr)
        for text in missing:
            print(f"  - {text}", file=sys.stderr)
        return 1

    print(out)
    print("QEMU smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
