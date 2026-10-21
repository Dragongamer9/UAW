"""Round-trip tests for megtool using synthetic MEG v1 archives.

Usage: python3 test_meg.py /path/to/megtool
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

MEGTOOL = sys.argv[1]


def norm(name: str) -> str:
    return name.replace("/", "\\").upper()


def build_meg(files: dict[str, bytes]) -> bytes:
    names = [norm(n) for n in files]
    datas = list(files.values())
    header = struct.pack("<II", len(names), len(names))
    name_table = b"".join(struct.pack("<H", len(n)) + n.encode("ascii") for n in names)
    data_start = len(header) + len(name_table) + 20 * len(names)

    records, offset = [], data_start
    for i, (n, d) in enumerate(zip(names, datas)):
        records.append((zlib.crc32(n.encode("ascii")), len(d), offset, i))
        offset += len(d)
    records.sort()  # engine expects table sorted by CRC
    file_table = b"".join(
        struct.pack("<IIIII", crc, idx, size, start, name_idx)
        for idx, (crc, size, start, name_idx) in enumerate(records)
    )
    return header + name_table + file_table + b"".join(datas)


def run(*args, ok=True):
    r = subprocess.run([MEGTOOL, *args], capture_output=True)
    if ok and r.returncode != 0:
        raise AssertionError(f"megtool {args} failed: {r.stderr.decode()}")
    return r


def main():
    files = {
        "Data/XML/GameObjectFiles.xml": b"<?xml version=\"1.0\"?><Files/>",
        "Data/Art/Models/NM_Walker.alo": bytes(range(256)) * 40,
        "Data/Scripts/AI/Empty.lua": b"",
        "Data/Text/MasterTextFile_English.dat": os.urandom(5000),
    }
    with tempfile.TemporaryDirectory() as tmp:
        meg = os.path.join(tmp, "test.meg")
        with open(meg, "wb") as f:
            f.write(build_meg(files))

        out = run("list", meg).stdout.decode()
        assert "4 files" in out, out
        assert run("verify", meg).returncode == 0

        # cat, with lowercase forward-slash lookup
        got = run("cat", meg, "data/xml/gameobjectfiles.xml").stdout
        assert got == files["Data/XML/GameObjectFiles.xml"]

        dst = os.path.join(tmp, "out")
        run("extract", meg, dst)
        for name, data in files.items():
            p = os.path.join(dst, *norm(name).split("\\"))
            with open(p, "rb") as f:
                assert f.read() == data, name

        # Path traversal must be refused.
        evil = os.path.join(tmp, "evil.meg")
        with open(evil, "wb") as f:
            f.write(build_meg({"..\\..\\ESCAPE.TXT": b"x", "OK.TXT": b"y"}))
        r = run("extract", evil, os.path.join(tmp, "evil_out"))
        assert b"unsafe" in r.stderr
        assert not os.path.exists(os.path.join(tmp, "ESCAPE.TXT"))

        # Malformed and v2 archives must fail cleanly, not crash.
        for blob in (b"\xff\xff\xff\xff" + b"\0" * 40, b"\x10\0\0\0\x10\0\0\0", b""):
            bad = os.path.join(tmp, "bad.meg")
            with open(bad, "wb") as f:
                f.write(blob)
            r = run("list", bad, ok=False)
            assert r.returncode == 1 and b"error:" in r.stderr, r

    print("all MEG tests passed")


if __name__ == "__main__":
    main()
