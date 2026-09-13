"""Generate CostumeFW_Abilities.esp - the fixed ability pool for 1.6.3.

A plugin full of empty constant-effect abilities. CEF fills each one at
kDataLoaded from its own registry, before any save is read, and hands it out
when the content that owns it is worn. See
cef_documents/active/ENCHANTMENT_ABILITY_POOL_DESIGN.md.

WHY A GENERATOR AND NOT A ONE-OFF FILE
    The pool is append-only for the life of the mod. A save records which
    ability a content was given, so an existing record can never be renumbered
    or reused - only new ones added at the end (design 5.2). A generator makes
    that guarantee mechanical: run it with a larger count and every existing
    record comes out byte-identical, because nothing about record i depends on
    the total. Editing a binary by hand does not give you that.

WHY NOT ESL
    The light form-ID range is 0x800-0xFFF, about 2,048 slots, and those are
    consumed for the lifetime of an install rather than held - an ID is never
    reused once a save might point at it. More importantly the flag cannot be
    removed later without changing how every form in the file is addressed,
    which would break every save that had already used one. A full plugin
    costs a load-order slot and settles the question permanently.

FORMAT NOTES (Skyrim SE)
    Record header is 24 bytes: sig, dataSize (fields only), flags, formID,
    timestamp+vcs, formVersion, unknown.
    Subrecord header is 6: sig, uint16 size.
    A record's own formID carries the plugin's self index in the high byte,
    which is the number of masters. One master (Skyrim.esm) means 0x01xxxxxx.
    GRUP size INCLUDES its own 24-byte header.

Usage:
    python tools/make_ability_pool.py [--count 1024] [--out <path>]
"""

import argparse
import struct
import sys

MASTERS = ["Skyrim.esm"]
SELF_INDEX = len(MASTERS)  # our own records live at this mod index
FIRST_LOCAL = 0x800
FORM_VERSION = 44
HEDR_VERSION = 1.71

# SPIT enums
SPELL_TYPE_ABILITY = 4
CAST_TYPE_CONSTANT_EFFECT = 0
DELIVERY_SELF = 0


def subrecord(sig: bytes, data: bytes) -> bytes:
    assert len(data) <= 0xFFFF, f"{sig!r} too long for a uint16 size"
    return sig + struct.pack("<H", len(data)) + data


def record(sig: bytes, form_id: int, fields: bytes, flags: int = 0) -> bytes:
    header = struct.pack(
        "<4sIIIHHHH",
        sig,
        len(fields),
        flags,
        form_id,
        0,  # timestamp
        0,  # vcs info
        FORM_VERSION,
        0,
    )
    return header + fields


def zstring(text: str) -> bytes:
    return text.encode("cp1252") + b"\x00"


def tes4(num_records: int, next_object_id: int) -> bytes:
    fields = subrecord(b"HEDR", struct.pack("<fiI", HEDR_VERSION, num_records, next_object_id))
    fields += subrecord(b"CNAM", zstring("HeavyMoon"))
    fields += subrecord(
        b"SNAM",
        zstring(
            "Costume Expansion FW - ability pool. Empty constant-effect "
            "abilities, filled at runtime. Do not remove once used: saves "
            "refer to these form IDs."
        ),
    )
    for master in MASTERS:
        fields += subrecord(b"MAST", zstring(master))
        fields += subrecord(b"DATA", struct.pack("<Q", 0))
    # flags 0: not ESM, and deliberately NOT ESL (0x200).
    return record(b"TES4", 0, fields, flags=0)


def ability(index: int) -> bytes:
    form_id = (SELF_INDEX << 24) | (FIRST_LOCAL + index)
    fields = subrecord(b"EDID", zstring(f"CFW_Ability_{index:04d}"))
    fields += subrecord(b"OBND", b"\x00" * 12)
    fields += subrecord(
        b"SPIT",
        struct.pack(
            "<IIIfIIffI",
            0,                          # base cost
            0,                          # flags (no manual cost calc)
            SPELL_TYPE_ABILITY,
            0.0,                        # charge time
            CAST_TYPE_CONSTANT_EFFECT,
            DELIVERY_SELF,
            0.0,                        # cast duration
            0.0,                        # range
            0,                          # half cost perk
        ),
    )
    # No FULL: an unnamed ability has nothing to show anywhere, and 1,024
    # names would be 1,024 strings for no reader.
    # No effects: that is the whole point - CEF supplies them at kDataLoaded.
    return record(b"SPEL", form_id, fields)


def group(label: bytes, records: bytes) -> bytes:
    size = 24 + len(records)
    header = struct.pack("<4sI4siHHHH", b"GRUP", size, label, 0, 0, 0, 0, 0)
    return header + records


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=1024)
    ap.add_argument("--out", default="package_assets/CostumeFW_Abilities.esp")
    args = ap.parse_args()

    if args.count < 1 or args.count > 0xFFFFFF - FIRST_LOCAL:
        print(f"count out of range: {args.count}", file=sys.stderr)
        return 1

    body = b"".join(ability(i) for i in range(args.count))
    out = tes4(args.count, FIRST_LOCAL + args.count) + group(b"SPEL", body)

    with open(args.out, "wb") as fh:
        fh.write(out)

    last = FIRST_LOCAL + args.count - 1
    print(f"wrote {args.out}: {args.count} abilities, {len(out)} bytes")
    print(f"  local form IDs {FIRST_LOCAL:06X}-{last:06X}")
    print(f"  editor IDs     CFW_Ability_0000-CFW_Ability_{args.count - 1:04d}")
    print(f"  masters        {', '.join(MASTERS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
