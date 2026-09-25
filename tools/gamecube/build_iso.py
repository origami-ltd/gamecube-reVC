#!/usr/bin/env python3
"""Build a bootable GameCube El Torito ISO9660 mini-DVD image.

xorriso creates the ISO/Joliet tree with a controlled physical file order.
The GameCube Linux Team header supplies the apploader for the El Torito DOL.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


MINI_DVD_BYTES = 1_459_978_240
ISO_SECTOR_BYTES = 2048
EL_TORITO_BOOT_RECORD_SECTOR = 17
EL_TORITO_CATALOG_POINTER = 71
EL_TORITO_DEFAULT_ENTRY = 0x20
EL_TORITO_SECTOR_COUNT = EL_TORITO_DEFAULT_ENTRY + 6


def iso_files(path):
    files = {}
    with open(path, 'rb') as image:
        image.seek(16*ISO_SECTOR_BYTES)
        pvd = image.read(ISO_SECTOR_BYTES)
        if pvd[:7] != b'\x01CD001\x01':
            raise ValueError('Invalid ISO9660 primary volume descriptor')
        pending = [('', struct.unpack_from('<I', pvd, 158)[0],
                    struct.unpack_from('<I', pvd, 166)[0])]
        while pending:
            parent, sector, size = pending.pop()
            image.seek(sector*ISO_SECTOR_BYTES)
            data = image.read(size)
            cursor = 0
            while cursor < size:
                length = data[cursor]
                if length == 0:
                    cursor = (cursor//ISO_SECTOR_BYTES+1)*ISO_SECTOR_BYTES
                    continue
                record = data[cursor:cursor+length]
                name = record[33:33+record[32]]
                cursor += length
                if name in (b'\0', b'\1'):
                    continue
                name = name.decode('ascii').split(';')[0].lower()
                full = parent+'/'+name
                lba, count = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
                if record[25] & 2:
                    pending.append((full, lba, count))
                else:
                    files[full] = {'sector': lba, 'bytes': count}
    return files


def disc_order(path):
    name = path.as_posix().lower()
    if name == 'revc.dol':
        return (0, name)
    if name == 'disc_pad.bin':
        return (2, name)
    if name.startswith('audio/') and name.endswith('.ogg'):
        return (3, name)
    if name in {'audio/city.wav', 'audio/beachamb.wav', 'audio/water.wav',
                'audio/hotel.wav', 'audio/police.wav', 'audio/sfx.adp'}:
        return (4, name)
    if name in {'models/gta3.dir', 'models/gta3.img'}:
        return (5, name)
    return (1, name)


def verify_files(image_path, work):
    layout = iso_files(image_path)
    with open(image_path, 'rb') as image:
        for source in Path(work).rglob('*'):
            if not source.is_file() or source.name == 'disc_pad.bin':
                continue
            name = '/'+source.relative_to(work).as_posix().lower()
            entry = layout.get(name)
            if entry is None or entry['bytes'] != source.stat().st_size:
                raise ValueError(f'ISO file missing or truncated: {name}')
            expected, actual = hashlib.sha256(), hashlib.sha256()
            image.seek(entry['sector']*ISO_SECTOR_BYTES)
            with source.open('rb') as src:
                while chunk := src.read(1024*1024):
                    expected.update(chunk)
                    actual.update(image.read(len(chunk)))
            if expected.digest() != actual.digest():
                raise ValueError(f'ISO payload mismatch: {name}')
            entry['sha256'] = actual.hexdigest()
    return layout


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


def dolphin_uses_image(image):
    """Do not replace a disc image while Dolphin is reading it."""
    try:
        commands = subprocess.run(
            ["ps", "-axo", "command="], check=True,
            stdout=subprocess.PIPE, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return False
    image = os.path.abspath(image)
    return any("/Dolphin " in command and image in command
               for command in commands.splitlines())


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
    if shutil.which("xorriso") is None:
        sys.exit("xorriso is required (brew install xorriso)")
    if os.path.getsize(args.gbi) != 32768:
        sys.exit("gbi.hdr must occupy the 16-sector ISO system area")

    output = os.path.abspath(args.out)
    if dolphin_uses_image(output):
        sys.exit(f"refusing to rebuild an ISO used by Dolphin: {output}")

    parent = os.path.dirname(os.path.abspath(args.root))
    work = tempfile.mkdtemp(prefix=".revc-iso-", dir=parent)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    fd, image_path = tempfile.mkstemp(
        prefix=f".{os.path.basename(output)}.", suffix=".building.iso",
        dir=os.path.dirname(output))
    os.close(fd)
    os.unlink(image_path)
    try:
        hardlink_tree(os.path.abspath(args.root), work)
        boot_dol = os.path.join(work, "revc.dol")
        # hardlink_tree deliberately shares unchanged assets with the staging
        # tree, but the boot DOL is replaced on every build.  Unlink it first so
        # copy2 cannot overwrite the staging tree's hardlink in place.
        if os.path.exists(boot_dol):
            os.unlink(boot_dol)
        shutil.copy2(args.dol, boot_dol)
        padding = Path(work)/'disc_pad.bin'
        if padding.exists():
            sys.exit('disc_pad.bin is reserved for the disc layout')
        padding.write_bytes(b'\0'*ISO_SECTOR_BYTES)
        ordered = sorted((p.relative_to(work) for p in Path(work).rglob('*') if p.is_file()), key=disc_order)
        with tempfile.NamedTemporaryFile(mode='w', suffix='.weights') as weights:
            for rank, path in enumerate(ordered):
                weights.write(f'{len(ordered)-rank} /{path.as_posix()}\n')
            weights.flush()
            command = [
                'xorriso', '-as', 'mkisofs', '-iso-level', '2', '-J', '-joliet-long',
                '-allow-lowercase', '-allow-multidot', '-V', 'REVC', '-no-pad',
                '-b', 'revc.dol', '-c', 'boot.catalog', '-no-emul-boot',
                '--sort-weight-list', weights.name, '-o', image_path, work,
            ]
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
            payload_size = os.path.getsize(image_path)
            spare = MINI_DVD_BYTES - 32768 - payload_size
            if spare < 0:
                sys.exit(f'ISO exceeds the mini-DVD data budget by {-spare} bytes')
            with padding.open('r+b') as pad:
                pad.truncate(ISO_SECTOR_BYTES + spare//ISO_SECTOR_BYTES*ISO_SECTOR_BYTES)
            os.unlink(image_path)
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL)

        # Cubeboot reads the complete DOL length from the El Torito entry.
        dol_sectors = (os.path.getsize(args.dol) + 511) // 512
        if dol_sectors > 0xFFFF:
            sys.exit("DOL is too large for the El Torito sector-count field")
        with open(args.gbi, "rb") as source, open(image_path, "r+b") as image:
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
        payload_size = os.path.getsize(image_path)
        if payload_size > MINI_DVD_BYTES:
            sys.exit(f"ISO is {payload_size - MINI_DVD_BYTES} bytes over mini-DVD capacity")
        layout = verify_files(image_path, work)
        archive = layout['/models/gta3.img']
        archive_end = archive['sector']*ISO_SECTOR_BYTES + archive['bytes']
        if MINI_DVD_BYTES - archive_end > 64*1024:
            sys.exit('ISO layout failed: gta3.img is not at the outer end of the disc')
        with open(image_path, "r+b") as image:
            image.truncate(MINI_DVD_BYTES)
        size = os.path.getsize(image_path)
        with open(image_path, "rb") as image:
            image.seek(0x8001)
            if image.read(5) != b"CD001":
                sys.exit("ISO validation failed: primary volume descriptor missing")
            image.seek(catalog_sector * ISO_SECTOR_BYTES + EL_TORITO_SECTOR_COUNT)
            if struct.unpack("<H", image.read(2))[0] != dol_sectors:
                sys.exit("ISO validation failed: boot DOL length is wrong")
            image.seek(MINI_DVD_BYTES - 1)
            if image.read(1) != b"\0":
                sys.exit("ISO validation failed: final disc byte is not readable padding")
        if size != MINI_DVD_BYTES:
            sys.exit(f"ISO validation failed: {size} != {MINI_DVD_BYTES} bytes")
        # The canonical ISO changes only after the new image has passed every
        # validation.  Existing readers retain the old inode instead of seeing
        # hdiutil truncate and rewrite the file underneath them.
        os.replace(image_path, output)
        Path(output+'.layout.json').write_text(json.dumps({
            'iso': output, 'bytes': size, 'files': layout,
            'order': ['/'+p.as_posix() for p in ordered],
        }, indent=2)+'\n')
        print(f"PASS: {output} ({size} bytes, "
              f"{MINI_DVD_BYTES - payload_size} bytes padding)")
    finally:
        if os.path.exists(image_path):
            os.unlink(image_path)
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
