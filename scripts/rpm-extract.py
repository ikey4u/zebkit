#!/usr/bin/env python3
# Unpack RPMs into a directory, without rpm or cpio installed:
#   scripts/rpm-extract.py <dest> <rpm>...
# For scripts/glibc-test.sh, which builds a CentOS 7 root from them.
import lzma, gzip, os, struct, sys, stat

def header_end(data, off):
    # magic(3) ver(1) reserved(4) nindex(4) hsize(4)
    nindex, hsize = struct.unpack(">II", data[off + 8:off + 16])
    return off + 16 + nindex * 16 + hsize

def payload(data):
    off = header_end(data, 96)
    off = (off + 7) & ~7
    off = header_end(data, off)
    body = data[off:]
    if body[:6] == b"\xfd7zXZ\x00":
        return lzma.decompress(body)
    if body[:2] == b"\x1f\x8b":
        return gzip.decompress(body)
    raise SystemExit("unknown payload compression")

def extract(cpio, dest):
    off = 0
    while True:
        h = cpio[off:off + 110]
        assert h[:6] in (b"070701", b"070702"), h[:6]
        f = [int(h[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        mode, size, namesize = f[1], f[6], f[11]
        name = cpio[off + 110:off + 110 + namesize - 1].decode()
        off = (off + 110 + namesize + 3) & ~3
        data = cpio[off:off + size]
        off = (off + size + 3) & ~3
        if name == "TRAILER!!!":
            return
        path = os.path.join(dest, name.lstrip("./"))
        if stat.S_ISDIR(mode):
            os.makedirs(path, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.lexists(path):
            os.remove(path)
        if stat.S_ISLNK(mode):
            os.symlink(data.decode(), path)
        elif stat.S_ISREG(mode):
            with open(path, "wb") as out:
                out.write(data)
            os.chmod(path, mode & 0o755 | 0o600)

dest = sys.argv[1]
for rpm in sys.argv[2:]:
    with open(rpm, "rb") as fh:
        extract(payload(fh.read()), dest)
