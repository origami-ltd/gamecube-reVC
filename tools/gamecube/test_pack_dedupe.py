#!/usr/bin/env python3
import os
import struct
import subprocess
import sys
import tempfile
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 2048


class PackDedupeTest(unittest.TestCase):
    def test_img_extents_are_reused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source_img = os.path.join(tmp, "in.img")
            source_dir = os.path.join(tmp, "in.dir")
            output_img = os.path.join(tmp, "out.img")
            output_dir = os.path.join(tmp, "out.dir")
            payloads = [b"A" * SECTOR, b"A" * SECTOR, b"B" * SECTOR]
            with open(source_img, "wb") as image:
                image.write(b"".join(payloads))
            with open(source_dir, "wb") as directory:
                for index, name in enumerate((b"a.txd", b"b.txd", b"c.dff")):
                    directory.write(struct.pack("<II24s", index, 1,
                                                name.ljust(24, b"\0")))

            subprocess.run([
                sys.executable, os.path.join(HERE, "repack_img.py"),
                "--copy-only", source_img, source_dir, output_img, output_dir,
            ], check=True)

            self.assertEqual(os.path.getsize(output_img), 2 * SECTOR)
            records = []
            with open(output_dir, "rb") as directory:
                data = directory.read()
            for offset in range(0, len(data), 32):
                records.append(struct.unpack_from("<II24s", data, offset)[:2])
            self.assertEqual(records, [(0, 1), (0, 1), (1, 1)])
            with open(output_img, "rb") as image:
                for record, expected in zip(records, payloads):
                    image.seek(record[0] * SECTOR)
                    self.assertEqual(image.read(record[1] * SECTOR), expected)

    def test_random_samples_share_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw_path = os.path.join(tmp, "sfx.raw")
            sdt_path = os.path.join(tmp, "sfx.sdt")
            pak_path = os.path.join(tmp, "sfx.pak")
            samples = [struct.pack("<h", index % 32768) for index in range(524)]
            samples[1] = samples[0]
            repeated = struct.pack("<h", 123) * 2048
            samples.extend((repeated, repeated))
            offset = 0
            with open(raw_path, "wb") as raw, open(sdt_path, "wb") as index:
                for sample in samples:
                    raw.write(sample)
                    index.write(struct.pack("<IIIII", offset, len(sample), 8000, 0, 0))
                    offset += len(sample)

            command = [sys.executable, os.path.join(HERE, "pack_sfx.py"),
                       raw_path, sdt_path, pak_path]
            subprocess.run(command + ["--verify"], check=True)
            subprocess.run(command + ["--check"], check=True)

            with open(pak_path, "rb") as packed:
                magic, count, data_start = struct.unpack(">8sII", packed.read(16))
                table = [struct.unpack(">II", packed.read(8)) for _ in range(count)]
            self.assertEqual(magic, b"GCSFXP2\0")
            self.assertEqual(table[0][0], data_start)
            self.assertNotEqual(table[0][0], table[1][0])
            self.assertEqual(table[524], table[525])


if __name__ == "__main__":
    unittest.main()
