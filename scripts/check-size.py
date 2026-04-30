#!/usr/bin/env python3
"""Fail if the final floppy image is not exactly 1.44 MB."""

import argparse
import os
import sys

IMAGE_SIZE = 1_474_560


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    args = parser.parse_args()

    size = os.path.getsize(args.image)
    if size != IMAGE_SIZE:
        print(f"{args.image}: {size} bytes, expected {IMAGE_SIZE}", file=sys.stderr)
        return 1
    print(f"{args.image}: {size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

