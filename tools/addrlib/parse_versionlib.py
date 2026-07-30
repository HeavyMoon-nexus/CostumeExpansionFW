"""Address Library (meh321) v2 'versionlib' bin parser - ID -> RVA lookup.

Usage:
  python parse_versionlib.py <versionlib-X-X-X-X.bin> <id> [<id> ...]
  python parse_versionlib.py <bin> --near 0x14A3311   # find the ID containing an RVA

Verified against crash-log ground truth 2026-07-30 (1.6.1170): six anchors
(40447 PlayerCharacter::Update, 69162, 70299 NiNode::GetObjectByName, 71212,
106350, 106353) all matched the return-address arithmetic exactly.
"""
import struct, sys

class R:
    def __init__(self, data): self.d = data; self.o = 0
    def u8(self):  v = self.d[self.o]; self.o += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.d, self.o)[0]; self.o += 2; return v
    def u32(self): v = struct.unpack_from("<I", self.d, self.o)[0]; self.o += 4; return v
    def u64(self): v = struct.unpack_from("<Q", self.d, self.o)[0]; self.o += 8; return v

def load(path):
    with open(path, "rb") as f:
        r = R(f.read())
    fmt = r.u32()
    ver = [r.u32() for _ in range(4)]
    nlen = r.u32()
    name = r.d[r.o:r.o+nlen].decode(); r.o += nlen
    ptr_size = r.u32()
    count = r.u32()
    print(f"# format={fmt} version={'.'.join(map(str, ver))} module={name!r} entries={count}",
          file=sys.stderr)
    pid = pofs = 0
    table = {}
    for _ in range(count):
        t = r.u8()
        lo, hi = t & 0xF, t >> 4
        if   lo == 0: iid = r.u64()
        elif lo == 1: iid = pid + 1
        elif lo == 2: iid = pid + r.u8()
        elif lo == 3: iid = pid - r.u8()
        elif lo == 4: iid = pid + r.u16()
        elif lo == 5: iid = pid - r.u16()
        elif lo == 6: iid = r.u16()
        elif lo == 7: iid = r.u32()
        else: raise ValueError(f"id type {lo}")
        tmp = (pofs // ptr_size) if (hi & 8) else pofs
        h = hi & 7
        if   h == 0: ofs = r.u64()
        elif h == 1: ofs = tmp + 1
        elif h == 2: ofs = tmp + r.u8()
        elif h == 3: ofs = tmp - r.u8()
        elif h == 4: ofs = tmp + r.u16()
        elif h == 5: ofs = tmp - r.u16()
        elif h == 6: ofs = r.u16()
        elif h == 7: ofs = r.u32()
        else: raise ValueError(f"ofs type {h}")
        if hi & 8: ofs *= ptr_size
        pid, pofs = iid, ofs
        table[iid] = ofs
    return table

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 1
    table = load(sys.argv[1])
    if sys.argv[2] == "--near":
        target = int(sys.argv[3], 0)
        below = [(o, i) for i, o in table.items() if o <= target]
        above = [(o, i) for i, o in table.items() if o > target]
        below.sort(); above.sort()
        for o, i in below[-3:]:
            print(f"id {i:6d} -> RVA 0x{o:X}  (target-0x{target - o:X})")
        if above:
            o, i = above[0]
            print(f"id {i:6d} -> RVA 0x{o:X}  (next above)")
        return 0
    for arg in sys.argv[2:]:
        iid = int(arg, 0)
        if iid in table:
            print(f"id {iid:6d} -> RVA 0x{table[iid]:X}")
        else:
            print(f"id {iid:6d} -> NOT IN DB")
    return 0

if __name__ == "__main__":
    sys.exit(main())
