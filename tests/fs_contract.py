import struct
from pathlib import Path


class FsContractError(ValueError):
    pass


class Fat12ImageFs:
    """Small FAT12 reader used by tests to enforce mount/read/dir contract."""

    BYTES_PER_SECTOR = 512
    RESERVED_SECTORS = 32
    FAT_COUNT = 2
    SECTORS_PER_FAT = 9
    ROOT_ENTRIES = 224
    ROOT_LBA = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
    ROOT_SECTORS = (ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
    DATA_LBA = ROOT_LBA + ROOT_SECTORS

    def __init__(self):
        self._image = None
        self._fat = None
        self._entries = None

    def mount(self, image_path):
        data = Path(image_path).read_bytes()
        if len(data) < self.BYTES_PER_SECTOR:
            raise FsContractError("FAT12 mount failed: image too small")
        if data[510:512] != b"\x55\xaa":
            raise FsContractError("FAT12 mount failed: missing boot signature")
        self._image = data
        fat_start = self.RESERVED_SECTORS * self.BYTES_PER_SECTOR
        fat_len = self.SECTORS_PER_FAT * self.BYTES_PER_SECTOR
        self._fat = data[fat_start : fat_start + fat_len]
        self._entries = self._read_root_entries()

    def list_dir(self):
        self._ensure_mounted()
        return sorted(self._entries)

    def read_file(self, name):
        self._ensure_mounted()
        upper = name.upper()
        if upper not in self._entries:
            raise FsContractError(f"file not found: {upper}")
        entry = self._entries[upper]
        first_cluster = struct.unpack_from("<H", entry, 26)[0]
        size = struct.unpack_from("<I", entry, 28)[0]
        if size == 0:
            return b""
        out = bytearray()
        cluster = first_cluster
        while 2 <= cluster < 0xFF8 and len(out) < size:
            lba = self.DATA_LBA + cluster - 2
            off = lba * self.BYTES_PER_SECTOR
            out.extend(self._image[off : off + self.BYTES_PER_SECTOR])
            cluster = self._fat12_next(cluster)
        return bytes(out[:size])

    def _read_root_entries(self):
        root = self.ROOT_LBA * self.BYTES_PER_SECTOR
        entries = {}
        for i in range(self.ROOT_ENTRIES):
            off = root + i * 32
            e = self._image[off : off + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                continue
            stem = e[0:8].decode("ascii").rstrip()
            ext = e[8:11].decode("ascii").rstrip()
            n = f"{stem}.{ext}" if ext else stem
            entries[n] = e
        return entries

    def _fat12_next(self, cluster):
        off = cluster + cluster // 2
        value = self._fat[off] | (self._fat[off + 1] << 8)
        return (value >> 4) if (cluster & 1) else (value & 0x0FFF)

    def _ensure_mounted(self):
        if self._image is None:
            raise FsContractError("filesystem is not mounted")
