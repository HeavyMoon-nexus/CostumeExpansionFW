"""Make CEF's ability registry look like generation 1 is FULL, for test 4-3.

Doing it honestly would need 1024 real allocations. Instead every FREE local id
in generation 1 gets a tombstoned entry - what an exhausted pool looks like from
the allocator's side - while the REAL entries are left untouched, so "existing
abilities still work" stays a meaningful half of the test.

CEF refuses the whole registry if `checksum` (FNV-1a over the compact dump of
the abilities array) disagrees, so this reproduces nlohmann's serialization via
cefjson. It self-tests first: re-checksum the UNTOUCHED array and compare with
what the file already carries. A mismatch means the serialization is off and the
script writes nothing, rather than handing CEF a file it rejects for a reason we
would then have to guess at.

Usage:  python fill_gen1.py            (self-test + dry run)
        python fill_gen1.py --write    (apply; keeps a .before-4-3 copy)
        python fill_gen1.py --restore  (put the .before-4-3 copy back)
"""
import argparse
import io
import os
import shutil
import sys

import cefjson

REG = r'K:\Mo2_SkyrimSE1170\overwrite\SKSE\plugins\CEF_abilities.json'
BAK = REG + '.before-4-3'
GEN1_PLUGIN = 'CostumeFW_Abilities.esp'
GEN1_FIRST = 0x800
GEN1_COUNT = 1024  # 0x800-0xBFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--restore', action='store_true')
    args = ap.parse_args()

    if args.restore:
        if not os.path.exists(BAK):
            print('no backup at', BAK)
            return 1
        shutil.copy2(BAK, REG)
        print('restored', REG, 'from .before-4-3')
        return 0

    doc = cefjson.load(REG)
    arr = doc['abilities']
    stored = doc.get('checksum', '')
    mine = cefjson.checksum(arr)

    print('schema          :', doc.get('schema'))
    print('entries         :', len(arr))
    print('checksum stored :', stored)
    print('checksum mine   :', mine)
    if mine != stored:
        print('\nSELF-TEST FAILED - serialization does not match nlohmann. Nothing written.')
        return 1
    print('self-test       : OK')

    if str(doc.get('schema')) != '2':
        print('\nschema is %r, not 2 - run a real allocation (4-1) first.' % doc.get('schema'))
        return 1

    used = {str(e.get('ability')) for e in arr}
    live = sum(1 for e in arr if not e.get('tombstone'))
    added = 0
    for i in range(GEN1_COUNT):
        colon = '%06X:%s' % (GEN1_FIRST + i, GEN1_PLUGIN)
        if colon in used:
            continue
        arr.append({
            'ability': colon,
            'content': '',
            'recipeGeneration': cefjson.Raw('0'),
            'tombstone': True,
            'effects': [],
        })
        added += 1

    doc['abilities'] = arr
    doc['checksum'] = cefjson.checksum(arr)
    print('live (non-tombstone) entries kept :', live)
    print('tombstones added                  :', added)
    print('entries now                       :', len(arr))
    print('new checksum                      :', doc['checksum'])

    if not args.write:
        print('\n(dry run - pass --write to apply)')
        return 0

    shutil.copy2(REG, BAK)
    io.open(REG, 'w', encoding='utf-8', newline='').write(cefjson.pretty(doc) + '\n')
    # Read back and re-verify: the file CEF will actually see.
    back = cefjson.load(REG)
    ok = cefjson.checksum(back['abilities']) == back['checksum']
    print('\nwritten. original kept as', os.path.basename(BAK))
    print('read-back checksum verify :', 'OK' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
