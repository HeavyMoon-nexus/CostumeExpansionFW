#!/usr/bin/env python3
"""Read the Created Objects table out of a Skyrim SE save.

Written because ReSaver does not surface it, and the question "did the
enchantment CEF created actually reach the save?" has no other honest answer.
It settled that question on 2026-09-12, twice. First negative: eight
consecutive saves carried a byte-identical 31-byte record holding one
pre-existing enchantment, so the one the spike made was never written. Then
positive, once the spike held a reference on it - the record grew to 77 bytes
and the new enchantment was in it with both of its effects and their
magnitudes intact.

    python tools/readsave.py                    # newest save in the MO2 profile
    python tools/readsave.py <file.ess> ...     # specific saves
    python tools/readsave.py --last 8           # the newest N

Parses only as far as it must, and validates each layout assumption instead of
trusting a remembered one. Two things are easy to get wrong here and both cost
an hour the first time:

  * Created Objects is global data type 4. Type 3 is Global Variables, which
    in a heavily modded save is ~27KB and looks convincingly like a big table
    of somethings.
  * The offsets in the FileLocationTable are relative to the START OF THE FILE,
    not to the decompressed body - so every one of them needs the header and
    screenshot size subtracted before it indexes the body.
"""
import argparse
import datetime
import glob
import os
import struct
import sys

DEFAULT_SAVES = r"K:\Mo2_SkyrimSE1170\profiles\Default\saves\*.ess"

GLOBAL_DATA_TYPES = {
    0: "Misc Stats",
    1: "Player Location",
    2: "TES",
    3: "Global Variables",
    4: "Created Objects",
    5: "Effects",
    6: "Weather",
    7: "Audio",
    8: "SkyCells",
}


def lz4_block_decompress(src, want=None):
    """LZ4 block format - what Skyrim SE compresses the save body with."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        mlen = token & 0x0F
        if mlen == 15:
            while True:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(out) - offset
        if start < 0:
            raise ValueError("bad match offset")
        for k in range(mlen):
            out.append(out[start + k])
    if want is not None and len(out) != want:
        print(f"  ! decompressed {len(out)} bytes, header said {want}")
    return bytes(out)


def _wstr(buf, o):
    (n,) = struct.unpack_from("<H", buf, o)
    return buf[o + 2:o + 2 + n], o + 2 + n


def body_and_delta(path):
    """The decompressed save body, and what to subtract from a FileLocationTable
    offset to index into it."""
    d = open(path, "rb").read()
    if d[:13] != b"TESV_SAVEGAME":
        raise ValueError("not a Skyrim save")
    o = 13
    (hdr_size,) = struct.unpack_from("<I", d, o)
    o += 4
    h = o
    (ver,) = struct.unpack_from("<I", d, h); h += 4
    h += 4                                   # saveNumber
    _, h = _wstr(d, h)                       # playerName
    h += 4                                   # playerLevel
    _, h = _wstr(d, h)                       # playerLocation
    _, h = _wstr(d, h)                       # gameDate
    _, h = _wstr(d, h)                       # playerRaceEditorId
    h += 2 + 4 + 4 + 8                       # sex, curExp, lvlUpExp, filetime
    (sw,) = struct.unpack_from("<I", d, h); h += 4
    (sh,) = struct.unpack_from("<I", d, h); h += 4
    comp = 0
    if ver >= 12:
        (comp,) = struct.unpack_from("<H", d, h)
    delta = o + hdr_size + sw * sh * (4 if ver >= 12 else 3)
    p = delta
    if comp == 0:
        return d[p:], delta
    (unc,) = struct.unpack_from("<I", d, p); p += 4
    (clen,) = struct.unpack_from("<I", d, p); p += 4
    raw = d[p:p + clen]
    if comp == 2:
        return lz4_block_decompress(raw, unc), delta
    import zlib
    return zlib.decompress(raw), delta


def global_data(path):
    """{type: bytes} for globalDataTable1 (types 0-8)."""
    b, delta = body_and_delta(path)
    o = 1                                        # formVersion
    (plugin_info_size,) = struct.unpack_from("<I", b, o)
    o += 4 + plugin_info_size                    # covers the ESL list too
    table = struct.unpack_from("<25I", b, o)
    start, count = table[2] - delta, table[6]
    if not (0 <= start < len(b)):
        raise ValueError(f"globalDataTable1 offset {start} outside body {len(b)}")
    out = {}
    o = start
    for _ in range(count):
        ty, ln = struct.unpack_from("<II", b, o)
        out[ty] = b[o + 8:o + 8 + ln]
        o += 8 + ln
    return out


def _vsval(buf, o):
    b0 = buf[o]
    n = b0 & 0x3
    if n == 0:
        return b0 >> 2, o + 1
    if n == 1:
        return (b0 | (buf[o + 1] << 8)) >> 2, o + 2
    return (b0 | (buf[o + 1] << 8) | (buf[o + 2] << 16)) >> 2, o + 3


def _refid(buf, o):
    """3 bytes big-endian: top 2 bits are the kind, low 22 the value.
    kind 2 = created (the real form id is 0xFF000000 | value)."""
    v = (buf[o] << 16) | (buf[o + 1] << 8) | buf[o + 2]
    return v >> 22, v & 0x3FFFFF, o + 3


def parse_created(co):
    """Created Objects: four lists of magic items, each carrying its refCount
    and its effects (MGEF, magnitude, area, duration, cost).

    Note what is NOT here: conditions. A created enchantment comes back from a
    save unconditional, which is the whole of the C5 risk, established from the
    file format rather than from a measurement."""
    o = 0
    out = {}
    for label in ("weapon", "armor", "potion", "poison"):
        n, o = _vsval(co, o)
        items = []
        for _ in range(n):
            kind, value, o = _refid(co, o)
            (refcount,) = struct.unpack_from("<I", co, o); o += 4
            ne, o = _vsval(co, o)
            effects = []
            for _ in range(ne):
                # 19 bytes: MGEF ref, magnitude, area, duration, cost. The cost
                # is PER EFFECT (RE::Effect carries one), not per item - reading
                # it as one trailing field per item parses a single-effect
                # enchantment correctly and then overruns on the next one.
                ekind, evalue, o = _refid(co, o)
                (mag,) = struct.unpack_from("<f", co, o); o += 4
                area, dur = struct.unpack_from("<II", co, o); o += 8
                (cost,) = struct.unpack_from("<f", co, o); o += 4
                effects.append((ekind, evalue, mag, area, dur, cost))
            items.append({"kind": kind, "value": value, "refcount": refcount,
                          "effects": effects})
        out[label] = items
    return out, o


def formid(kind, value):
    return f"FF{value:06X}" if kind == 2 else f"kind{kind}:{value:06X}"


def report(path, verbose):
    stamp = datetime.datetime.fromtimestamp(os.path.getmtime(path))
    print(f"\n{stamp:%Y-%m-%d %H:%M:%S}  {os.path.basename(path)}")
    try:
        tables = global_data(path)
    except Exception as ex:
        print(f"  ! {ex}")
        return
    if verbose:
        for ty in sorted(tables):
            print(f"    type {ty}  {GLOBAL_DATA_TYPES.get(ty, '?'):<17} "
                  f"{len(tables[ty]):>8} bytes")
    co = tables.get(4)
    if co is None:
        print("  ! no Created Objects record")
        return
    try:
        parsed, used = parse_created(co)
    except Exception as ex:
        print(f"  Created Objects {len(co)} bytes - parse failed: {ex}")
        print(f"  hex: {co[:96].hex(' ')}")
        return
    counts = " ".join(f"{k}={len(v)}" for k, v in parsed.items())
    warn = "" if used == len(co) else f"  ! consumed {used}/{len(co)}"
    print(f"  Created Objects {len(co)} bytes: {counts}{warn}")
    for label in ("weapon", "armor"):
        for it in parsed[label]:
            print(f"    {label:<7} {formid(it['kind'], it['value'])}  "
                  f"refCount={it['refcount']}  effects={len(it['effects'])}")
            for ekind, evalue, mag, area, dur, cost in it["effects"]:
                print(f"      mgef {formid(ekind, evalue)}  mag={mag:g} "
                      f"area={area} dur={dur} cost={cost:.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("saves", nargs="*", help="save files (default: newest in the MO2 profile)")
    ap.add_argument("--last", type=int, default=1, help="how many of the newest to read")
    ap.add_argument("--all-tables", action="store_true", help="list every global data record")
    args = ap.parse_args()
    paths = args.saves or sorted(glob.glob(DEFAULT_SAVES), key=os.path.getmtime)[-args.last:]
    if not paths:
        print(f"no saves found: {DEFAULT_SAVES}", file=sys.stderr)
        return 1
    for p in paths:
        report(p, args.all_tables)
    return 0


if __name__ == "__main__":
    sys.exit(main())
