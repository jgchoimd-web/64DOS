#!/usr/bin/env python3
"""Validate the structure of a generated 64DOS floppy image."""

import argparse
import os
import struct
import sys

IMAGE_SIZE = 1_474_560
BYTES_PER_SECTOR = 512
RESERVED_SECTORS = 32
FAT_COUNT = 2
ROOT_ENTRIES = 224
SECTORS_PER_FAT = 9
ROOT_SECTORS = 14
ROOT_LBA = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
DATA_LBA = ROOT_LBA + ROOT_SECTORS
MANIFEST_LBA = 31


def rd16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def rd32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def root_entries(image):
    root = ROOT_LBA * BYTES_PER_SECTOR
    for i in range(ROOT_ENTRIES):
        off = root + i * 32
        entry = image[off : off + 32]
        if entry[0] == 0x00:
            break
        if entry[0] == 0xE5:
            continue
        yield entry


def entry_name(entry):
    stem = entry[0:8].decode("ascii").rstrip()
    ext = entry[8:11].decode("ascii").rstrip()
    return f"{stem}.{ext}" if ext else stem


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    args = parser.parse_args()

    data = open(args.image, "rb").read()
    errors = []

    if len(data) != IMAGE_SIZE:
        errors.append(f"size is {len(data)}, expected {IMAGE_SIZE}")
    if data[510:512] != b"\x55\xaa":
        errors.append("missing boot signature")
    if data[3:11] != b"64DOS1  ":
        errors.append("unexpected OEM label")

    expected_bpb = {
        "bytes/sector": (11, 2, 512),
        "sectors/cluster": (13, 1, 1),
        "reserved sectors": (14, 2, 32),
        "FAT count": (16, 1, 2),
        "root entries": (17, 2, 224),
        "total sectors": (19, 2, 2880),
        "media": (21, 1, 0xF0),
        "sectors/FAT": (22, 2, 9),
        "sectors/track": (24, 2, 18),
        "heads": (26, 2, 2),
    }
    for label, (off, width, expected) in expected_bpb.items():
        actual = data[off] if width == 1 else rd16(data, off)
        if actual != expected:
            errors.append(f"BPB {label} is {actual}, expected {expected}")

    manifest = data[MANIFEST_LBA * BYTES_PER_SECTOR : (MANIFEST_LBA + 1) * BYTES_PER_SECTOR]
    if manifest[0:4] != b"64MF":
        errors.append("manifest magic missing")
    image_sectors = rd32(manifest, 8)
    kernel_lba = rd32(manifest, 12)
    kernel_sectors = rd32(manifest, 16)
    kernel_size = rd32(manifest, 20)
    image_base = rd32(manifest, 24)
    kernel_load = rd32(manifest, 28)
    if image_sectors != 2880:
        errors.append(f"manifest image sectors is {image_sectors}")
    if image_base != 0x100000:
        errors.append(f"manifest image base is {image_base:#x}")
    if kernel_load != 0x300000:
        errors.append(f"manifest kernel load is {kernel_load:#x}")

    files = {entry_name(e): e for e in root_entries(data)}
    if "KERNEL64.BIN" not in files:
        errors.append("KERNEL64.BIN missing from root")
    else:
        e = files["KERNEL64.BIN"]
        first_cluster = rd16(e, 26)
        expected_lba = DATA_LBA + first_cluster - 2
        if kernel_lba != expected_lba:
            errors.append(f"manifest kernel LBA {kernel_lba}, expected {expected_lba}")
        if rd32(e, 28) != kernel_size:
            errors.append("manifest kernel size does not match root entry")
        if kernel_sectors == 0:
            errors.append("manifest kernel sector count is zero")
    if "AUTOEXEC.BAT" not in files:
        errors.append("AUTOEXEC.BAT missing from root")
    if "README.TXT" not in files:
        errors.append("README.TXT missing from root")

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"{args.image}: valid 64DOS FAT12 image ({os.path.getsize(args.image)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

