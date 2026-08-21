#!/usr/bin/env python3
"""Build a bootable GameCube El Torito ISO9660 mini-DVD image.

macOS' hdiutil creates the ISO/Joliet tree and boot catalog. The GameCube
Linux Team generic boot header supplies the disc header and apploader which
loads the no-emulation DOL from that catalog.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile


MINI_DVD_BYTES = 1_459_978_240
ISO_SECTOR_BYTES = 2048
EL_TORITO_BOOT_RECORD_SECTOR = 17
EL_TORITO_CATALOG_POINTER = 71
EL_TORITO_DEFAULT_ENTRY = 0x20
EL_TORITO_SECTOR_COUNT = EL_TORITO_DEFAULT_ENTRY + 6


def hardlink_tree(source, destination):
    for root, dirs, files in os.walk(source):
        relative = os.path.relpath(root, source)
        target_root = destination if relative == "." else os.path.join(destination, relative)
        os.makedirs(target_root, exist_ok=True)
        dirs[:] = [name for name in dirs if not name.startswith("._")]
        for name in files:
            if name.startswith("._") or name.endswith((".bak", ".tmp")):
                continue
            src = os.path.join(root, name)
            dst = os.path.join(target_root, name)
            try:
                os.link(src, dst)
            except OSError:
                shutil.copy2(src, dst)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, help="prepared game data root")
    parser.add_argument("--dol", required=True, help="GameCube reVC.dol")
    parser.add_argument("--out", required=True, help="output .iso")
    parser.add_argument("--gbi", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "iso", "gbi.hdr"),
        help="cubeboot-tools generic GameCube boot header")
    args = parser.parse_args()

    for label, path in (("root", args.root), ("DOL", args.dol), ("GBI header", args.gbi)):
        if not os.path.exists(path):
            sys.exit(f"{label} not found: {path}")
    if shutil.which("hdiutil") is None:
        sys.exit("hdiutil is required on macOS")
    if os.path.getsize(args.gbi) != 32768:
        sys.exit("gbi.hdr must occupy the 16-sector ISO system area")

    parent = os.path.dirname(os.path.abspath(args.root))
    work = tempfile.mkdtemp(prefix=".revc-iso-", dir=parent)
    try:
        hardlink_tree(os.path.abspath(args.root), work)
        boot_dol = os.path.join(work, "revc.dol")
        shutil.copy2(args.dol, boot_dol)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        command = [
            "hdiutil", "makehybrid", "-ov", "-o", os.path.abspath(args.out),
            "-iso", "-joliet", "-iso-volume-name", "REVC",
            "-joliet-volume-name", "REVC", "-no-emul-boot",
            "-eltorito-boot", boot_dol, work,
        ]
        result = subprocess.run(command)
        if result.returncode:
            sys.exit("hdiutil failed to build the ISO")

        # hdiutil hard-codes the no-emulation boot count to four 512-byte
        # sectors. Cubeboot uses that field as the DOL length and rejects any
        # real application as truncated. Patch it to the complete DOL size;
        # the field is outside the catalog validation-entry checksum.
        dol_sectors = (os.path.getsize(args.dol) + 511) // 512
        if dol_sectors > 0xFFFF:
            sys.exit("DOL is too large for the El Torito sector-count field")
        with open(args.gbi, "rb") as source, open(args.out, "r+b") as image:
            image.seek(EL_TORITO_BOOT_RECORD_SECTOR * ISO_SECTOR_BYTES +
                       EL_TORITO_CATALOG_POINTER)
            catalog_sector_data = image.read(4)
            if len(catalog_sector_data) != 4:
                sys.exit("ISO validation failed: boot catalog pointer missing")
            catalog_sector = struct.unpack("<I", catalog_sector_data)[0]
            image.seek(catalog_sector * ISO_SECTOR_BYTES + EL_TORITO_SECTOR_COUNT)
            image.write(struct.pack("<H", dol_sectors))
            image.seek(0)
            image.write(source.read())
        size = os.path.getsize(args.out)
        with open(args.out, "rb") as image:
            image.seek(0x8001)
            if image.read(5) != b"CD001":
                sys.exit("ISO validation failed: primary volume descriptor missing")
            image.seek(catalog_sector * ISO_SECTOR_BYTES + EL_TORITO_SECTOR_COUNT)
            if struct.unpack("<H", image.read(2))[0] != dol_sectors:
                sys.exit("ISO validation failed: boot DOL length is wrong")
        if size > MINI_DVD_BYTES:
            sys.exit(f"ISO is {size - MINI_DVD_BYTES} bytes over mini-DVD capacity")
        print(f"PASS: {args.out} ({size} bytes, "
              f"{MINI_DVD_BYTES - size} bytes free)")
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
