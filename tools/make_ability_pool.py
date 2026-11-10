"""Generate a CostumeFW ability pool plugin - generation 1 or a successor.

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

GENERATION 1 IS NOT ESL, AND EVERY LATER ONE IS
    Generation 1 shipped in 1.6.3 as a full plugin, for a reason that was true
    of a SINGLE pool: the light form-ID range is 0x800-0xFFF, about 2,048 ids,
    they are consumed for the lifetime of an install rather than held, and the
    flag cannot be removed later without changing how every form in the file is
    addressed - which would break every save that had already used one. Paying
    one load-order slot settled it permanently.

    That argument does not survive there being a generation 2. Capacity now
    comes from ADDING a plugin, so an ESL generation costs nothing in the
    load order and the range limit stops mattering: run out and you ship the
    next one. So generation 1 keeps its flags exactly as they shipped - it can
    never be regenerated, saves point into it - and generation 2 and up are
    ESL. --esl is therefore required from generation 2 on and refused for
    generation 1.

    This is the ABILITY pool's reasoning and nothing else's. The BOX token pool
    is ESL from its first generation; see ENCHANTMENT_ABILITY_POOL_DESIGN.md
    §2.2, which says in as many words not to carry this rationale over to it.

FORMAT NOTES (Skyrim SE)
    Record header is 24 bytes: sig, dataSize (fields only), flags, formID,
    timestamp+vcs, formVersion, unknown.
    Subrecord header is 6: sig, uint16 size.
    A record's own formID carries the plugin's self index in the high byte,
    which is the number of masters. One master (Skyrim.esm) means 0x01xxxxxx.
    GRUP size INCLUDES its own 24-byte header.

Usage:
    python tools/make_ability_pool.py                      # generation 1, as shipped
    python tools/make_ability_pool.py --generation 2 --esl # the next one
"""

import argparse
import os
import struct
import sys

MASTERS = ["Skyrim.esm"]
# Our own records live at this mod index. FIXED AT 1, for the ESL generations
# too: a light plugin's records are addressed FE<index>xxx at RUNTIME, but the
# file on disk still carries the self index its master count implies, and the
# pool has exactly one master whatever its flags say.
SELF_INDEX = len(MASTERS)
FIRST_LOCAL = 0x800
LAST_LOCAL = 0xFFF  # the light form-ID range; generation 2 and up must fit it
ESL_FLAG = 0x200
# What each generation ships with. Generation 1 is a historical fact rather
# than a choice; the default for a new one is the whole light range.
GEN1_COUNT = 1024
DEFAULT_COUNT = 2048
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


def plugin_name(generation: int) -> str:
    """The one spelling of a generation's file name.

    Generation 1 shipped as CostumeFW_Abilities.esp and the name is published
    identity now, so "Abilities1.esp" is not another way to write it - it is a
    file that does not exist. Must stay in lockstep with
    tokenid::AbilityPoolPluginName (src/TokenIdentity.cpp).
    """
    return "CostumeFW_Abilities.esp" if generation == 1 else f"CostumeFW_Abilities{generation}.esp"


def tes4(num_records: int, next_object_id: int, esl: bool) -> bytes:
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
    # Not ESM either way. ESL from generation 2 on - see the module docstring.
    return record(b"TES4", 0, fields, flags=ESL_FLAG if esl else 0)


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
    ap.add_argument("--generation", type=int, default=1)
    ap.add_argument("--count", type=int, default=None,
                    help=f"default {GEN1_COUNT} for generation 1, {DEFAULT_COUNT} after")
    ap.add_argument("--esl", action="store_true", help="required from generation 2 on")
    ap.add_argument("--out", default=None, help="default: package_assets/<generation's name>")
    args = ap.parse_args()

    if args.generation < 1:
        print(f"generation must be >= 1: {args.generation}", file=sys.stderr)
        return 1

    # Generation 1 is not regenerable (design 5.2, and §7.5 of the 1.6.4 plan).
    # Its records are named by every save that has ever allocated one, and the
    # only way this script could improve it is a way that breaks them. The guard
    # is here rather than in a comment because the default arguments still
    # produce it, and "I ran it to see what it did" would overwrite the file.
    if args.generation == 1 and not os.environ.get("CEF_ALLOW_GEN1_REGEN"):
        print("generation 1 shipped in 1.6.3 and must not be regenerated: saves name its "
              "form IDs, and a rebuild that differs in any byte is a different plugin.\n"
              "Set CEF_ALLOW_GEN1_REGEN=1 if you really mean to reproduce it byte for byte.",
              file=sys.stderr)
        return 1

    esl = args.esl
    if args.generation >= 2 and not esl:
        print(f"generation {args.generation} must be ESL (pass --esl): capacity comes from "
              "adding a plugin now, so a later pool costs no load-order slot", file=sys.stderr)
        return 1
    if args.generation == 1 and esl:
        print("generation 1 is not ESL and cannot become one: the flag changes how every form "
              "in the file is addressed, and saves already point at them", file=sys.stderr)
        return 1

    count = args.count if args.count is not None else (
        GEN1_COUNT if args.generation == 1 else DEFAULT_COUNT)
    if count < 1:
        print(f"count out of range: {count}", file=sys.stderr)
        return 1
    if esl and FIRST_LOCAL + count - 1 > LAST_LOCAL:
        print(f"{count} abilities do not fit the light form-ID range "
              f"{FIRST_LOCAL:03X}-{LAST_LOCAL:03X} - ship another generation instead",
              file=sys.stderr)
        return 1
    if count > 0xFFFFFF - FIRST_LOCAL:
        print(f"count out of range: {count}", file=sys.stderr)
        return 1

    name = plugin_name(args.generation)
    out_path = args.out if args.out else os.path.join("package_assets", name)

    body = b"".join(ability(i) for i in range(count))
    out = tes4(count, FIRST_LOCAL + count, esl) + group(b"SPEL", body)

    with open(out_path, "wb") as fh:
        fh.write(out)

    last = FIRST_LOCAL + count - 1
    print(f"wrote {out_path}: {count} abilities, {len(out)} bytes")
    print(f"  generation     {args.generation} ({name})")
    print(f"  local form IDs {FIRST_LOCAL:06X}-{last:06X}")
    print(f"  editor IDs     CFW_Ability_0000-CFW_Ability_{count - 1:04d}")
    print(f"  masters        {', '.join(MASTERS)}")
    print(f"  ESL flag       {'yes' if esl else 'no'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
