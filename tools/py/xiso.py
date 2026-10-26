#!/usr/bin/env python3
"""List or extract files from an original Xbox disc image (XDVDFS / "XISO").

Usage:
    python tools/py/xiso.py <image> --list
    python tools/py/xiso.py <image> --extract default.xbe [--out orig/<name>]
    python tools/py/xiso.py <image> --extract-all --out orig/<name>

Handles plain XISO images (filesystem at offset 0) and images that carry the
game partition at a fixed offset (Redump-style dumps, where the filesystem
starts after the video partition). The offset is detected by probing for the
"MICROSOFT*XBOX*MEDIA" volume descriptor.
"""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ATTR_DIRECTORY = 0x10
KNOWN_BASES = (0, 0x18300000, 0xFD90000, 0x2080000)


def find_base(f, scan_limit=2 << 30):
    """Return the byte offset at which the XDVDFS starts."""
    for base in KNOWN_BASES:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    # Fall back to a sector scan of the first part of the image.
    pos = 0
    while pos < scan_limit:
        f.seek(pos)
        chunk = f.read(1 << 20)
        if not chunk:
            break
        idx = chunk.find(MAGIC)
        if idx != -1 and (pos + idx) % SECTOR == 0:
            return pos + idx - 32 * SECTOR
        pos += len(chunk)
    raise ValueError("no XDVDFS volume descriptor found")


class Xiso:
    def __init__(self, path):
        self.path = Path(path)
        self.f = self.path.open("rb")
        self.base = find_base(self.f)
        self.f.seek(self.base + 32 * SECTOR + 20)
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))
        self.entries = []  # (path, start_sector, size, is_dir)
        self._walk(self.root_sector, self.root_size, "")

    def close(self):
        self.f.close()

    def _read_dir(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def _walk(self, sector, size, prefix):
        if size == 0:
            return
        data = self._read_dir(sector, size)
        stack = [0]
        seen = set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue  # padding
            name = data[off + 14 : off + 14 + nlen].decode("ascii", "replace")
            full = prefix + name
            is_dir = bool(attrs & ATTR_DIRECTORY)
            self.entries.append((full, start, fsize, is_dir))
            if is_dir:
                self._walk(start, fsize, full + "/")
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)

    def read(self, start, size):
        self.f.seek(self.base + start * SECTOR)
        return self.f.read(size)

    def find(self, name):
        wanted = name.lower()
        for full, start, size, is_dir in self.entries:
            if not is_dir and full.lower() == wanted:
                return full, start, size
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", metavar="FILE")
    ap.add_argument("--extract-all", action="store_true")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    x = Xiso(args.image)
    try:
        print("%s: filesystem at offset 0x%X, root dir sector %d size %d, %d entries"
              % (args.image.name, x.base, x.root_sector, x.root_size, len(x.entries)))
        if args.list:
            for full, start, size, is_dir in sorted(x.entries):
                print("%s %12s  %s" % ("DIR " if is_dir else "    ", "" if is_dir else size, full))
        if args.extract:
            hit = x.find(args.extract)
            if not hit:
                sys.exit("not found: %s" % args.extract)
            full, start, size = hit
            out = (args.out or Path("orig") / args.image.stem) / Path(full).name
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(x.read(start, size))
            print("extracted %s (%d bytes) -> %s" % (full, size, out))
        if args.extract_all:
            root = args.out or Path("orig") / args.image.stem
            total = 0
            for full, start, size, is_dir in x.entries:
                if is_dir:
                    continue
                out = root / full
                out.parent.mkdir(parents=True, exist_ok=True)
                with out.open("wb") as fh:
                    remaining = size
                    x.f.seek(x.base + start * SECTOR)
                    while remaining:
                        chunk = x.f.read(min(remaining, 8 << 20))
                        if not chunk:
                            break
                        fh.write(chunk)
                        remaining -= len(chunk)
                total += size
            print("extracted %d files, %d bytes -> %s" % (sum(1 for e in x.entries if not e[3]), total, root))
    finally:
        x.close()


if __name__ == "__main__":
    main()
