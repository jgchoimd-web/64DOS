import importlib.util
import struct
import tempfile
from pathlib import Path
import unittest

from fs_contract import Fat12ImageFs, FsContractError
from test_fat12_image import make_stage1

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("mkfloppy", ROOT / "scripts" / "mkfloppy.py")
mkfloppy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mkfloppy)


class Fat12ContractTests(unittest.TestCase):
    def _build_image(self):
        td = tempfile.TemporaryDirectory()
        work = Path(td.name)
        root = work / "root"
        root.mkdir()
        (root / "AUTOEXEC.BAT").write_text("ECHO TEST\n", encoding="ascii")
        (root / "EMPTY.TXT").write_bytes(b"")
        (root / "HELLO.TXT").write_bytes(b"hello world\n")

        stage1 = work / "stage1.bin"
        stage2 = work / "stage2.bin"
        kernel = work / "kernel64.bin"
        image = work / "64dos.img"
        make_stage1(stage1)
        stage2.write_bytes(b"S2")
        kernel.write_bytes(bytes(range(64)))
        mkfloppy.build_image(stage1, stage2, kernel, root, image)
        return td, image

    def test_mount_read_and_list_contract(self):
        td, image = self._build_image()
        with td:
            fs = Fat12ImageFs()
            fs.mount(image)
            names = fs.list_dir()
            self.assertIn("KERNEL64.BIN", names)
            self.assertIn("AUTOEXEC.BAT", names)
            self.assertEqual(fs.read_file("HELLO.TXT"), b"hello world\n")
            self.assertEqual(fs.read_file("EMPTY.TXT"), b"")

    def test_read_file_error_message_is_stable(self):
        td, image = self._build_image()
        with td:
            fs = Fat12ImageFs()
            fs.mount(image)
            with self.assertRaisesRegex(FsContractError, "file not found: MISSING.TXT"):
                fs.read_file("missing.txt")


if __name__ == "__main__":
    unittest.main()
