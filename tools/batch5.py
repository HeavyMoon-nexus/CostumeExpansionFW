#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""batch5 - stage one destructive v1.6.4 test at a time, and prove CEF did not
write the settings back.

Every item here is undone by restore.ps1; nothing else edits the live file, so
the md5 recorded by `break` is the exact bytes CEF must still find on disk when
the game exits.  That is the whole point of 5-1 / 5-2 / 5-3: a no-touch path
that writes nothing.

  python batch5.py break 5-2      stage the item (records md5 + size)
  python batch5.py verify         after the game run: file unchanged?
  python batch5.py stamp          just record the current md5 (for 5-1, 5-3,
                                  5-10, 5-11 - the items broken outside JSON)
"""
import hashlib
import io
import json
import os
import sys

SETTINGS = r'K:\Mo2_SkyrimSE1170\overwrite\SKSE\Plugins\CEF_settings.json'
# Beside the file it describes, not in the repo - the same place fill_gen1.py
# keeps its .before-4-3 copy.
STAMP = SETTINGS + '.batch5stamp'

# A box with one content: Recovery has something to show, and a rename/stat
# attempt has something to land on.
VICTIM = '000803:CostumeFW.esp'   # "Costume Box 45"
TWIN = '000801:CostumeFW.esp'     # "Costume Box 44", for the casing-duplicate
EMPTY = '000A15:CostumeFW.esp'    # "Costume Box 43", 0 contents


def load():
    with io.open(SETTINGS, encoding='utf-8-sig') as f:
        return json.load(f)


def save(d):
    # Byte-for-byte CEF's own writer (UTF-8, no BOM, indent 2, sorted keys,
    # no trailing newline) - verified by round-tripping the live file, so a
    # later difference is CEF's write and never our reformat.
    out = json.dumps(d, ensure_ascii=False, indent=2, sort_keys=True)
    with io.open(SETTINGS, 'w', encoding='utf-8', newline='') as f:
        f.write(out)


def digest():
    with open(SETTINGS, 'rb') as f:
        data = f.read()
    return hashlib.md5(data).hexdigest(), len(data)


def find(d, token):
    for b in d['boxes']:
        if b['token'] == token:
            return b
    raise SystemExit('no box holds token %s - did restore.ps1 run?' % token)


def retoken(d, victim, new, what):
    b = find(d, victim)
    b['token'] = new
    print('  %s  "%s"  %s -> %s' % (what, b.get('label'), victim, new))


ITEMS = {}


def item(name, desc):
    def deco(fn):
        ITEMS[name] = (desc, fn)
        return fn
    return deco


@item('5-2a', 'B12 as written: every token unresolvable -> ALL QUARANTINED, no G3')
def i_5_2a(d):
    # Distinct ids on purpose: a repeat would hit the duplicate-token DROP and
    # confuse which rule fired.
    for i, b in enumerate(d['boxes']):
        b['token'] = '000F%02X:CostumeFW.esp' % (i + 1)
    print('  %d token(s) pointed at 000F01.. (no such form)' % len(d['boxes']))
    print('  expect: unresolved-form quarantine on every box, boxesDropped = 0,')
    print('          so G3 does NOT fire. The file must still be untouched.')


@item('5-2b', 'B12 as intended: blank every token -> G3 fires (the only trigger)')
def i_5_2b(d):
    # LoadBoxes counts a row as DROPPED in exactly three places: empty token,
    # duplicate boxId, duplicate token. G3 needs dropped >= seen, so every row
    # has to be refused - and only the empty token refuses the FIRST row too
    # (a duplicate always leaves the original standing). Blanking them is the
    # one hand-edit that reaches the breaker.
    for b in d['boxes']:
        b['token'] = ''
    print('  %d token(s) blanked' % len(d['boxes']))
    print('  expect: "settings: REFUSED every box ... (%d of %d)"'
          % (len(d['boxes']), len(d['boxes'])))
    print('  note: the drops themselves are SILENT - the G3 error is the only line')


@item('5-3', 'B15: exactly one box left, to be run against the 1.6.3 core esp')
def i_5_3(d):
    keep = find(d, VICTIM)
    dropped = len(d['boxes']) - 1
    d['boxes'] = [keep]
    print('  kept "%s" only (%d box definitions removed)'
          % (keep.get('label'), dropped))
    print('  NOW ALSO: copy CostumeFW_163_nomarker.esp over the mod folder esp')


@item('5-4', 'B13: plugin name lowercased -> heals to canonical casing')
def i_5_4(d):
    retoken(d, VICTIM, '000803:costumefw.esp', 'lowercased')


@item('5-5', 'B16: the same token twice, differing only in casing -> 2nd dropped')
def i_5_5(d):
    # VICTIM, not EMPTY: the dropped box has to CARRY something for "its contents
    # are still in Recovery" to be checkable, and EMPTY holds nothing. VICTIM sits
    # later in the array than TWIN, so TWIN is the row that survives.
    retoken(d, VICTIM, TWIN.replace('CostumeFW', 'costumefw'),
            'casing twin of %s' % TWIN)


@item('5-6', 'B14: token points at a QUST -> not-armo, quarantined')
def i_5_6(d):
    retoken(d, VICTIM, '000802:CostumeFW.esp', 'CFW_MCMQuest (QUST)')


@item('5-7', 'B22: token points at nothing -> unresolved-form')
def i_5_7(d):
    retoken(d, VICTIM, '000999:CostumeFW.esp', 'no such form')


@item('5-8', 'B18: token points at a publish token -> foreign-plugin')
def i_5_8(d):
    retoken(d, VICTIM, '000800:CostumeFW_NPC.esp', 'CFW_PubToken01 (foreign)')


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]

    if cmd == 'verify':
        if not os.path.exists(STAMP):
            raise SystemExit('no stamp.json - run break/stamp first')
        with io.open(STAMP, encoding='utf-8') as f:
            old = json.load(f)
        md5, size = digest()
        same = (md5 == old['md5'] and size == old['size'])
        print('staged for : %s' % old.get('item'))
        print('expected   : %s  %d bytes' % (old['md5'], old['size']))
        print('on disk now: %s  %d bytes' % (md5, size))
        print('')
        print('UNCHANGED - CEF wrote nothing' if same
              else 'CHANGED - CEF wrote the settings back (FAIL for 5-1/5-2/5-3)')
        return 0 if same else 1

    if cmd == 'stamp':
        md5, size = digest()
        item_name = sys.argv[2] if len(sys.argv) > 2 else '(manual)'
        with io.open(STAMP, 'w', encoding='utf-8') as f:
            f.write(json.dumps({'item': item_name, 'md5': md5, 'size': size},
                               ensure_ascii=False, indent=1))
        print('%s  %s  %d bytes' % (item_name, md5, size))
        return 0

    if cmd != 'break':
        raise SystemExit(__doc__)

    name = sys.argv[2] if len(sys.argv) > 2 else ''
    if name not in ITEMS:
        print('items:')
        for k in sorted(ITEMS):
            print('  %-5s %s' % (k, ITEMS[k][0]))
        return 2

    desc, fn = ITEMS[name]
    before_md5, before_size = digest()
    print('%s  %s' % (name, desc))
    print('before: %s  %d bytes' % (before_md5, before_size))
    d = load()
    fn(d)
    save(d)
    md5, size = digest()
    with io.open(STAMP, 'w', encoding='utf-8') as f:
        f.write(json.dumps({'item': name, 'md5': md5, 'size': size},
                           ensure_ascii=False, indent=1))
    print('after : %s  %d bytes   <- CEF must leave these bytes alone' % (md5, size))
    return 0


if __name__ == '__main__':
    sys.exit(main())
