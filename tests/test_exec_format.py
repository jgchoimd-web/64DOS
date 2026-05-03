import struct
import unittest


class ExecFormatError(ValueError):
    pass


def parse_exec_header(data):
    if len(data) < 16:
        raise ExecFormatError("exec header parse failed: truncated file")
    magic = data[0:4]
    if magic != b"64EX":
        raise ExecFormatError("exec header parse failed: invalid magic")
    version = struct.unpack_from("<H", data, 4)[0]
    if version != 1:
        raise ExecFormatError(f"exec header parse failed: unsupported version {version}")
    entry = struct.unpack_from("<I", data, 8)[0]
    image_size = struct.unpack_from("<I", data, 12)[0]
    if entry >= image_size:
        raise ExecFormatError(
            f"exec header parse failed: entry point {entry} out of range for size {image_size}"
        )
    return {"version": version, "entry": entry, "size": image_size}


class ExecHeaderTests(unittest.TestCase):
    def test_valid_header(self):
        data = struct.pack("<4sHHII", b"64EX", 1, 0, 0, 16)
        parsed = parse_exec_header(data)
        self.assertEqual(parsed["version"], 1)
        self.assertEqual(parsed["entry"], 0)
        self.assertEqual(parsed["size"], 16)

    def test_version_mismatch(self):
        data = struct.pack("<4sHHII", b"64EX", 2, 0, 0, 16)
        with self.assertRaisesRegex(ExecFormatError, "unsupported version 2"):
            parse_exec_header(data)

    def test_entry_out_of_range(self):
        data = struct.pack("<4sHHII", b"64EX", 1, 0, 17, 16)
        with self.assertRaisesRegex(ExecFormatError, "entry point 17 out of range for size 16"):
            parse_exec_header(data)

    def test_truncated_file(self):
        with self.assertRaisesRegex(ExecFormatError, "truncated file"):
            parse_exec_header(b"64EX")


if __name__ == "__main__":
    unittest.main()
