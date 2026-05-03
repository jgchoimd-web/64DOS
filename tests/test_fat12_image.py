import importlib.util
import struct
import tempfile
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("mkfloppy", ROOT / "scripts" / "mkfloppy.py")
mkfloppy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mkfloppy)


def make_stage1(path):
    data = bytearray(512)
    data[0:3] = b"\xeb\x3c\x90"
    data[3:11] = b"64DOS1  "
    struct.pack_into("<H", data, 11, 512)
    data[13] = 1
    struct.pack_into("<H", data, 14, 32)
    data[16] = 2
    struct.pack_into("<H", data, 17, 224)
    struct.pack_into("<H", data, 19, 2880)
    data[21] = 0xF0
    struct.pack_into("<H", data, 22, 9)
    struct.pack_into("<H", data, 24, 18)
    struct.pack_into("<H", data, 26, 2)
    data[510:512] = b"\x55\xaa"
    path.write_bytes(data)


def rd16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def rd32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def fat12_next(fat, cluster):
    off = cluster + cluster // 2
    value = fat[off] | (fat[off + 1] << 8)
    if cluster & 1:
        return value >> 4
    return value & 0x0FFF


class Fat12ImageTests(unittest.TestCase):
    def test_builder_creates_kernel_and_root_files(self):
        with tempfile.TemporaryDirectory() as td:
            work = Path(td)
            root = work / "root"
            root.mkdir()
            (root / "AUTOEXEC.BAT").write_text("ECHO TEST\n", encoding="ascii")
            (root / "README.TXT").write_text("hello\n", encoding="ascii")

            stage1 = work / "stage1.bin"
            stage2 = work / "stage2.bin"
            kernel = work / "kernel64.bin"
            image = work / "64dos.img"
            make_stage1(stage1)
            stage2.write_bytes(b"S2")
            kernel.write_bytes(bytes(range(256)) * 3)

            mkfloppy.build_image(stage1, stage2, kernel, root, image)
            data = image.read_bytes()

            self.assertEqual(len(data), mkfloppy.IMAGE_SIZE)
            self.assertEqual(data[510:512], b"\x55\xaa")
            self.assertEqual(data[31 * 512 : 31 * 512 + 4], b"64MF")

            manifest = data[31 * 512 : 32 * 512]
            kernel_lba = rd32(manifest, 12)
            kernel_size = rd32(manifest, 20)
            self.assertEqual(kernel_size, 768)

            root_off = mkfloppy.ROOT_LBA * 512
            names = []
            entries = {}
            for i in range(224):
                e = data[root_off + i * 32 : root_off + (i + 1) * 32]
                if e[0] == 0:
                    break
                stem = e[0:8].decode("ascii").rstrip()
                ext = e[8:11].decode("ascii").rstrip()
                name = f"{stem}.{ext}" if ext else stem
                names.append(name)
                entries[name] = e

            self.assertEqual(names[:3], ["KERNEL64.BIN", "AUTOEXEC.BAT", "README.TXT"])
            first_cluster = rd16(entries["KERNEL64.BIN"], 26)
            self.assertEqual(kernel_lba, mkfloppy.DATA_LBA + first_cluster - 2)
            self.assertEqual(data[kernel_lba * 512 : kernel_lba * 512 + 4], b"\x00\x01\x02\x03")

            fat = data[mkfloppy.RESERVED_SECTORS * 512 : (mkfloppy.RESERVED_SECTORS + 9) * 512]
            self.assertEqual(fat12_next(fat, first_cluster), first_cluster + 1)
            self.assertEqual(fat12_next(fat, first_cluster + 1), 0xFFF)

    def test_rejects_non_83_names(self):
        with self.assertRaises(ValueError):
            mkfloppy.dos_name("too-long-name.txt")

    def test_zero_length_file_uses_cluster_zero(self):
        with tempfile.TemporaryDirectory() as td:
            work = Path(td)
            root = work / "root"
            root.mkdir()
            (root / "EMPTY.TXT").write_bytes(b"")

            stage1 = work / "stage1.bin"
            stage2 = work / "stage2.bin"
            kernel = work / "kernel64.bin"
            image = work / "64dos.img"
            make_stage1(stage1)
            stage2.write_bytes(b"S2")
            kernel.write_bytes(b"abc")

            mkfloppy.build_image(stage1, stage2, kernel, root, image)
            data = image.read_bytes()
            root_off = mkfloppy.ROOT_LBA * 512

            empty = data[root_off + 32 : root_off + 64]
            self.assertEqual(rd32(empty, 28), 0)
            self.assertEqual(rd16(empty, 26), 0)


if __name__ == "__main__":
    unittest.main()
