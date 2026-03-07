#!/usr/bin/env python3
"""Generate a gzipped newc-format cpio archive from a directory.

Supports injecting synthetic char device nodes without root privileges,
which is required on macOS where mknod needs superuser.

Usage:
    mkcpio.py <source_dir> <output.cpio.gz> [dev_spec ...]

Each dev_spec is: path,mode_oct,major,minor
Example:
    mkcpio.py rootfs/ out.cpio.gz dev/console,0600,5,1 dev/null,0666,1,3
"""
import gzip
import os
import stat
import sys
import time


class CpioWriter:
    def __init__(self):
        self.buf = bytearray()
        self.ino_counter = 1

    def _pad4(self):
        while len(self.buf) % 4:
            self.buf += b"\x00"

    def add(self, name, mode, uid=0, gid=0, nlink=1, mtime=0,
            data=b"", rdev_major=0, rdev_minor=0):
        namesize = len(name) + 1
        filesize = len(data)
        hdr = "070701"
        hdr += "%08X" % self.ino_counter
        hdr += "%08X" % mode
        hdr += "%08X" % uid
        hdr += "%08X" % gid
        hdr += "%08X" % nlink
        hdr += "%08X" % mtime
        hdr += "%08X" % filesize
        hdr += "%08X" % 0   # devmajor
        hdr += "%08X" % 0   # devminor
        hdr += "%08X" % rdev_major
        hdr += "%08X" % rdev_minor
        hdr += "%08X" % namesize
        hdr += "%08X" % 0   # check
        self.buf += hdr.encode() + name.encode() + b"\x00"
        self._pad4()
        if data:
            self.buf += data
            self._pad4()
        self.ino_counter += 1

    def finish(self):
        self.add("TRAILER!!!", 0)
        while len(self.buf) % 512:
            self.buf += b"\x00"
        return bytes(self.buf)


def scan_dir(cpio, base_dir):
    mtime = int(time.time())
    for dirpath, dirnames, filenames in os.walk(base_dir):
        reldir = os.path.relpath(dirpath, base_dir)

        for name in sorted(dirnames):
            rel = os.path.join(reldir, name) if reldir != "." else name
            full = os.path.join(dirpath, name)
            st = os.stat(full)
            cpio.add(rel, stat.S_IFDIR | (st.st_mode & 0o777),
                     nlink=2, mtime=mtime)

        for name in sorted(filenames):
            rel = os.path.join(reldir, name) if reldir != "." else name
            full = os.path.join(dirpath, name)
            st = os.stat(full)
            with open(full, "rb") as f:
                data = f.read()
            cpio.add(rel, stat.S_IFREG | (st.st_mode & 0o777),
                     mtime=mtime, data=data)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    src_dir = sys.argv[1]
    out_path = sys.argv[2]
    dev_specs = sys.argv[3:]

    cpio = CpioWriter()
    cpio.add(".", stat.S_IFDIR | 0o755, nlink=2)
    scan_dir(cpio, src_dir)

    for spec in dev_specs:
        parts = spec.split(",")
        path, mode_oct, major, minor = parts[0], int(parts[1], 8), int(parts[2]), int(parts[3])
        cpio.add(path, stat.S_IFCHR | mode_oct, rdev_major=major, rdev_minor=minor)

    result = cpio.finish()
    with gzip.open(out_path, "wb") as f:
        f.write(result)
    print(f"  Packed {cpio.ino_counter - 1} entries, {len(result)} bytes -> {out_path}")


if __name__ == "__main__":
    main()
