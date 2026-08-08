"""Scan a UT99 .u file to locate the import & export tables empirically.

We KNOW from the header: nameCount=7625 (names parse cleanly inline), exportCount,
exportCount validated by generation data right before the name table. But the
table offsets are unclear, so we scan every byte offset and test whether a run of
`exportCount` entries looks like FObjectExport {Class, Super, Package, Name,
Flags, SerialSize, SerialOffset} — small name indices (< nameCount) and serial
offsets within the file.
"""

import struct
import sys
import zlib


def parse_names(data, name_count, name_off):
    names = []
    pos = name_off
    for _ in range(name_count):
        if pos >= len(data):
            break
        ln = data[pos]
        if ln <= 0:
            return names, pos
        nm = data[pos + 1:pos + 1 + ln - 1].decode("latin-1")
        names.append(nm)
        pos += 1 + ln + 4
    return names, pos


def find_export_table(data, exp_count, name_count, file_size):
    """Try 20/24/28-byte entries; collect candidate offsets where every entry's
    ObjectNameIndex is a valid name index and SerialOffset+SerialSize <= filesize."""
    results = []
    for esize in (20, 24, 28, 16):
        nbytes = exp_count * esize
        for start in range(0, file_size - nbytes):
            ok = True
            bad_off = None
            for i in range(exp_count):
                off = start + i * esize
                (cls, sup, pkg, nm, flags) = struct.unpack_from("<iiiiI", data, off)
                if not (0 <= nm < name_count):
                    ok = False
                    break
            if ok:
                # validate serial offsets for a sample
                sample_ok = True
                for i in range(0, exp_count, max(1, exp_count // 50)):
                    off = start + i * esize
                    (cls, sup, pkg, nm, flags, ssize, soff) = struct.unpack_from("<iiiiIii", data, off)
                    if not (0 <= soff <= file_size):
                        sample_ok = False
                        break
                results.append((start, esize, sample_ok))
    return results


def main(path):
    with open(path, "rb") as f:
        data = f.read()
    name_count, name_off = struct.unpack_from("<II", data, 12)
    exp_count, exp_off = struct.unpack_from("<II", data, 20)
    imp_count, imp_off = struct.unpack_from("<II", data, 28)
    names, _ = parse_names(data, name_count, name_off)
    print(f"names parsed: {len(names)}")

    print(f"header says: exports={exp_count}@{exp_off} imports={imp_count}@{imp_off}")
    for start, esize, sample_ok in find_export_table(data, exp_count, name_count, len(data)):
        print(f"  candidate export table @ {start} entry_size={esize} serial_ok={sample_ok}")


if __name__ == "__main__":
    main(sys.argv[1])
