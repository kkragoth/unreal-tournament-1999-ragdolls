"""Diagnostic: parse a UT99 .u package header + name table empirically.

UT99 (UE1) name table at NameOffset is stored inline as a sequence of:
    int8  length (includes trailing NUL)
    bytes name (length bytes, NUL-terminated)
    int32 flags
"""

import struct
import sys


def parse_package(path):
    with open(path, "rb") as f:
        data = f.read()

    sig, = struct.unpack_from("<I", data, 0)
    print(f"signature=0x{sig:08X} (expect 0x9E2A83C1)")
    if sig != 0x9E2A83C1:
        raise SystemExit("not a UE1 package")

    # Candidate field at 0x04: package version (int16) + licensee (int16), or int32.
    ver, lic = struct.unpack_from("<Hh", data, 4)
    print(f"version={ver} licensee={lic}")
    flags, = struct.unpack_from("<I", data, 8)
    name_count, name_off = struct.unpack_from("<II", data, 12)
    exp_count, exp_off = struct.unpack_from("<II", data, 20)
    imp_count, imp_off = struct.unpack_from("<II", data, 28)
    print(f"flags=0x{flags:08X} names={name_count}@{name_off} exports={exp_count}@{exp_off} imports={imp_count}@{imp_off}")

    # ---- Name table (inline, [len][name\0][flags]) ----
    names = []
    pos = name_off
    ok = True
    for i in range(name_count):
        if pos >= len(data):
            ok = False
            break
        ln = data[pos]
        if ln <= 0:
            ok = False
            break
        nm = data[pos + 1:pos + 1 + ln - 1].decode("latin-1")
        flags_i = struct.unpack_from("<I", data, pos + 1 + ln)[0] if pos + 1 + ln + 4 <= len(data) else 0
        names.append((nm, flags_i))
        pos += 1 + ln + 4
    print(f"parsed {len(names)} names, next pos={pos} (import table at {imp_off})")
    print(f"  first 10: {[n for n, f in names[:10]]}")
    print(f"  name table bytes = {pos - name_off}")

    # Import table position must follow the name table.
    print(f"  delta import_off - name_table_end = {imp_off - pos}")

    return data, names, imp_off, exp_off, imp_count, exp_count


if __name__ == "__main__":
    parse_package(sys.argv[1])
