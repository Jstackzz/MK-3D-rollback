#!/usr/bin/env python3
"""Identify an original Xbox executable (XBE).

Usage:
    python tools/py/xbe_info.py <default.xbe> [--json out.json]

Prints the certificate (title id, title name, region, version, disc number),
the header (base address, entry point and kernel thunk after removing the
retail/debug XOR), the sections with their flags, and the XDK library
versions the game was linked against. The XDK build numbers are what the
recompilers key their symbol signatures on, so they decide which toolchain
layout applies to this exact build.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ENTRY_XOR = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_XOR = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}
REGIONS = {1: "North America", 2: "Japan", 4: "Rest of world", 0x80000000: "Manufacturing"}
SECTION_FLAGS = [(1, "writable"), (2, "preload"), (4, "executable"), (8, "inserted"), (0x10, "head_ro"), (0x20, "tail_ro")]


def cstring(data, off, limit=256):
    end = data.find(bytes([0]), off, off + limit)
    return data[off:end if end != -1 else off + limit].decode("ascii", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    data = args.xbe.read_bytes()
    if data[:4] != b"XBEH":
        sys.exit("not an XBE (magic %r)" % data[:4])
    base = struct.unpack_from("<I", data, 0x104)[0]

    def va(addr):  # header virtual address -> file offset
        return addr - base

    hdr = {}
    (hdr["base"], hdr["headers_size"], hdr["image_size"], hdr["image_header_size"], hdr["timestamp"],
     cert_addr, nsections, sect_addr, hdr["init_flags"], entry_x, hdr["tls"]) = struct.unpack_from("<11I", data, 0x104)
    thunk_x = struct.unpack_from("<I", data, 0x158)[0]
    nlibs, libs_addr = struct.unpack_from("<II", data, 0x160)
    debug_path_addr, debug_file_addr = struct.unpack_from("<II", data, 0x14C)

    kind = "retail" if (entry_x ^ ENTRY_XOR["retail"]) >= base and (entry_x ^ ENTRY_XOR["retail"]) < base + hdr["image_size"] else "debug"
    hdr["kind"] = kind
    hdr["entry_point"] = entry_x ^ ENTRY_XOR[kind]
    hdr["kernel_thunk"] = thunk_x ^ THUNK_XOR[kind]
    hdr["debug_path"] = cstring(data, va(debug_path_addr)) if debug_path_addr else ""
    hdr["debug_file"] = cstring(data, va(debug_file_addr)) if debug_file_addr else ""

    c = va(cert_addr)
    cert = {}
    (cert["size"], cert["timestamp"], cert["title_id"]) = struct.unpack_from("<III", data, c)
    cert["title_name"] = data[c + 0x0C : c + 0x0C + 80].decode("utf-16-le", "replace").split(chr(0))[0]
    cert["alt_title_ids"] = [t for t in struct.unpack_from("<16I", data, c + 0x5C) if t]
    (cert["allowed_media"], cert["region"], cert["ratings"], cert["disc_number"], cert["version"]) = struct.unpack_from("<5I", data, c + 0x9C)
    cert["region_name"] = ", ".join(n for bit, n in REGIONS.items() if cert["region"] & bit) or "none"
    cert["title_id_hex"] = "%08X" % cert["title_id"]
    pub = cert["title_id"] >> 16
    cert["publisher_code"] = bytes([pub >> 8, pub & 0xFF]).decode("ascii", "replace")
    cert["title_number"] = cert["title_id"] & 0xFFFF

    sections = []
    for i in range(nsections):
        s = va(sect_addr) + i * 0x38
        flags, vaddr, vsize, raw, rawsize, name_addr = struct.unpack_from("<6I", data, s)
        sections.append({
            "name": cstring(data, va(name_addr), 32),
            "flags": [n for bit, n in SECTION_FLAGS if flags & bit],
            "vaddr": "0x%08X" % vaddr, "vsize": vsize, "raw": "0x%X" % raw, "rawsize": rawsize,
        })

    libs = []
    for i in range(nlibs):
        l = va(libs_addr) + i * 16
        name = data[l : l + 8].rstrip(bytes([0])).decode("ascii", "replace")
        major, minor, build, flags = struct.unpack_from("<4H", data, l + 8)
        libs.append({"name": name, "version": "%d.%d.%d" % (major, minor, build), "build": build,
                     "qfe": flags & 0x1FFF, "approved": (flags >> 13) & 3, "debug": bool(flags & 0x8000)})

    info = {
        "file": args.xbe.name, "size": len(data),
        "sha1": hashlib.sha1(data).hexdigest(), "md5": hashlib.md5(data).hexdigest(),
        "header": {k: ("0x%08X" % v if isinstance(v, int) and k not in ("headers_size", "image_size", "image_header_size", "timestamp", "init_flags") else v) for k, v in hdr.items()},
        "certificate": cert, "sections": sections, "libraries": libs,
    }

    print("%s: %s XBE, %d bytes, sha1 %s" % (info["file"], kind, len(data), info["sha1"]))
    print("  title   %s  [%s]  publisher %s #%d" % (cert["title_name"], cert["title_id_hex"], cert["publisher_code"], cert["title_number"]))
    print("  region  %s (0x%X)  version 0x%X  disc %d  media 0x%X" % (cert["region_name"], cert["region"], cert["version"], cert["disc_number"], cert["allowed_media"]))
    print("  base 0x%08X  entry 0x%08X  kernel thunk 0x%08X  image %d bytes  sections %d" % (base, hdr["entry_point"], hdr["kernel_thunk"], hdr["image_size"], nsections))
    if hdr["debug_path"]:
        print("  debug path %s" % hdr["debug_path"])
    print("  XDK libraries:")
    for l in libs:
        print("    %-8s %-12s%s" % (l["name"], l["version"], "  (debug)" if l["debug"] else ""))
    print("  sections:")
    for s in sections:
        print("    %-10s %s +%-8d raw %-10s %s" % (s["name"], s["vaddr"], s["vsize"], s["raw"], ",".join(s["flags"])))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(info, indent=2))
        print("  wrote %s" % args.json)


if __name__ == "__main__":
    main()
