#!/usr/bin/env python3
"""Create a reproducible 1.44 MB 64DOS FAT12 floppy image."""

import argparse
import math
import os
import struct
from pathlib import Path

IMAGE_SIZE = 1_474_560
BYTES_PER_SECTOR = 512
TOTAL_SECTORS = 2880
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 32
FAT_COUNT = 2
ROOT_ENTRIES = 224
SECTORS_PER_FAT = 9
SECTORS_PER_TRACK = 18
HEADS = 2
ROOT_SECTORS = (ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
ROOT_LBA = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
DATA_LBA = ROOT_LBA + ROOT_SECTORS
MAX_STAGE2_BYTES = 30 * BYTES_PER_SECTOR

IMAGE_BASE = 0x100000
KERNEL_LOAD = 0x300000
BOOTINFO_ADDR = 0x5000

MANIFEST_MAGIC = b"64MF"


def dos_name(filename):
    name = Path(filename).name.upper()
    if "." in name:
        stem, ext = name.rsplit(".", 1)
    else:
        stem, ext = name, ""
    if not stem or len(stem) > 8 or len(ext) > 3:
        raise ValueError(f"{filename!r} is not an 8.3 filename")
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$~!#%&-{}()@'`"
    for ch in stem + ext:
        if ch not in allowed:
            raise ValueError(f"{filename!r} contains unsupported FAT12 character {ch!r}")
    return stem.ljust(8) + ext.ljust(3)


def set_fat12_entry(fat, cluster, value):
    offset = cluster + cluster // 2
    value &= 0x0FFF
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def write_root_entry(root, index, name, first_cluster, size):
    off = index * 32
    entry = bytearray(32)
    entry[0:11] = dos_name(name).encode("ascii")
    entry[11] = 0x20
    struct.pack_into("<H", entry, 26, first_cluster)
    struct.pack_into("<I", entry, 28, size)
    root[off : off + 32] = entry


def read_root_files(root_dir, kernel_path):
    files = [("KERNEL64.BIN", Path(kernel_path).read_bytes())]
    for path in sorted(Path(root_dir).iterdir()):
        if not path.is_file():
            continue
        name = path.name.upper()
        if name == "KERNEL64.BIN":
            raise ValueError("image_root must not contain KERNEL64.BIN; it is supplied by --kernel")
        files.append((name, path.read_bytes()))
    return files


def validate_stage1(stage1):
    if len(stage1) != BYTES_PER_SECTOR:
        raise ValueError("stage1 must be exactly one sector")
    if stage1[510:512] != b"\x55\xaa":
        raise ValueError("stage1 is missing BIOS boot signature")
    checks = {
        "bytes/sector": (11, "<H", BYTES_PER_SECTOR),
        "sectors/cluster": (13, "B", SECTORS_PER_CLUSTER),
        "reserved sectors": (14, "<H", RESERVED_SECTORS),
        "FAT count": (16, "B", FAT_COUNT),
        "root entries": (17, "<H", ROOT_ENTRIES),
        "total sectors": (19, "<H", TOTAL_SECTORS),
        "sectors/FAT": (22, "<H", SECTORS_PER_FAT),
        "sectors/track": (24, "<H", SECTORS_PER_TRACK),
        "heads": (26, "<H", HEADS),
    }
    for label, (offset, fmt, expected) in checks.items():
        actual = struct.unpack_from(fmt, stage1, offset)[0]
        if actual != expected:
            raise ValueError(f"stage1 BPB {label} is {actual}, expected {expected}")


def build_image(stage1_path, stage2_path, kernel_path, root_dir, output_path):
    stage1 = Path(stage1_path).read_bytes()
    stage2 = Path(stage2_path).read_bytes()
    validate_stage1(stage1)
    if len(stage2) > MAX_STAGE2_BYTES:
        raise ValueError(f"stage2 is {len(stage2)} bytes; limit is {MAX_STAGE2_BYTES}")

    files = read_root_files(root_dir, kernel_path)
    if len(files) > ROOT_ENTRIES:
        raise ValueError("too many files for FAT12 root directory")

    image = bytearray(IMAGE_SIZE)
    image[0:BYTES_PER_SECTOR] = stage1
    image[BYTES_PER_SECTOR : BYTES_PER_SECTOR + len(stage2)] = stage2

    fat = bytearray(SECTORS_PER_FAT * BYTES_PER_SECTOR)
    set_fat12_entry(fat, 0, 0xFF0)
    set_fat12_entry(fat, 1, 0xFFF)

    root = bytearray(ROOT_SECTORS * BYTES_PER_SECTOR)
    next_cluster = 2
    kernel_lba = 0
    kernel_sectors = 0
    kernel_size = len(files[0][1])

    for index, (name, data) in enumerate(files):
        clusters = math.ceil(len(data) / (BYTES_PER_SECTOR * SECTORS_PER_CLUSTER))
        first_cluster = 0
        if clusters:
            first_cluster = next_cluster
            last_cluster = first_cluster + clusters - 1
            if DATA_LBA + (last_cluster - 2) >= TOTAL_SECTORS:
                raise ValueError("files do not fit in 1.44 MB image")

            for cluster in range(first_cluster, last_cluster + 1):
                if cluster == last_cluster:
                    set_fat12_entry(fat, cluster, 0xFFF)
                else:
                    set_fat12_entry(fat, cluster, cluster + 1)

            data_offset = (DATA_LBA + (first_cluster - 2)) * BYTES_PER_SECTOR
            image[data_offset : data_offset + len(data)] = data
        write_root_entry(root, index, name, first_cluster, len(data))

        if index == 0:
            kernel_lba = DATA_LBA + (first_cluster - 2)
            kernel_sectors = clusters

        next_cluster += clusters

    fat_start = RESERVED_SECTORS * BYTES_PER_SECTOR
    image[fat_start : fat_start + len(fat)] = fat
    image[fat_start + len(fat) : fat_start + 2 * len(fat)] = fat

    root_start = ROOT_LBA * BYTES_PER_SECTOR
    image[root_start : root_start + len(root)] = root

    manifest = bytearray(BYTES_PER_SECTOR)
    struct.pack_into(
        "<4sHHIIIIII",
        manifest,
        0,
        MANIFEST_MAGIC,
        1,
        0,
        TOTAL_SECTORS,
        kernel_lba,
        kernel_sectors,
        kernel_size,
        IMAGE_BASE,
        KERNEL_LOAD,
    )
    struct.pack_into("<I", manifest, 32, BOOTINFO_ADDR)
    manifest_start = 31 * BYTES_PER_SECTOR
    image[manifest_start : manifest_start + BYTES_PER_SECTOR] = manifest

    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", required=True)
    parser.add_argument("--stage2", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    build_image(args.stage1, args.stage2, args.kernel, args.root, args.output)


if __name__ == "__main__":
    main()
