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
  python batch5.py unbreak        put back what an 8-8x item moved or edited
                                  outside the settings (registry, ini)

The 8-8x items are the review's T5 (CEF_ADVERSARIAL_REVIEW_2026-09-23, part 2
section T). 8-83/84/85 break CEF_abilities.json and 8-88 the ini, which
restore.ps1 does not cover (the ini lives in the mod folder), so those keep
their own way back: a file moved away keeps its name + .batch5aside, a file
edited in place leaves its original as + .batch5orig, and `unbreak` puts both
back. 8-91 edits the settings like the 5-x items, and restore.ps1 undoes it.
"""
import hashlib
import io
import json
import os
import shutil
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

REGISTRY = r'K:\Mo2_SkyrimSE1170\overwrite\SKSE\Plugins\CEF_abilities.json'
# What CEF checks before it will call a missing registry a first run
# (AbilityPool.cpp LoadRegistryEntries: .bak1 and the 1.6.3 copy), plus .bak2,
# which rotates with them. All of them gone is "lost with its backups" (8-85).
REGISTRY_BACKUPS = [
    REGISTRY + '.bak1',
    REGISTRY + '.bak2',
    r'K:\Mo2_SkyrimSE1170\overwrite\SKSE\Plugins\CEF_abilities.pre164.json',
]
# The ini the game reads is the mod folder's - nothing in overwrite shadows it.
INI = r'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW\SKSE\Plugins\CostumeExpansionFW.ini'
ASIDE = '.batch5aside'  # a file moved out of the way
ORIG = '.batch5orig'    # the untouched copy of a file edited in place
RUN = '.batch5run'      # what CEF wrote where a moved file was, kept by unbreak


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


def set_aside(path):
    if os.path.exists(path + ASIDE):
        raise SystemExit('%s is already set aside - run "batch5.py unbreak" first' % path)
    if os.path.exists(path):
        os.replace(path, path + ASIDE)
        print('  moved aside: %s' % os.path.basename(path))
    else:
        print('  not there  : %s' % os.path.basename(path))


def keep_original(path):
    if os.path.exists(path + ORIG):
        raise SystemExit('%s already has a %s copy - run "batch5.py unbreak" first'
                         % (path, ORIG))
    shutil.copy2(path, path + ORIG)


def unbreak():
    put_back = 0
    for path in [REGISTRY] + REGISTRY_BACKUPS + [INI]:
        if os.path.exists(path + ASIDE):
            # CEF may have written a file of its own there during the run (the
            # 8-85 audit writes a registry, and every write rotates a .bak1 in).
            # Keep it for a look rather than lose it.
            if os.path.exists(path):
                os.replace(path, path + RUN)
                print("  kept the run's file as %s" % os.path.basename(path + RUN))
            os.replace(path + ASIDE, path)
            print('  put back   : %s' % os.path.basename(path))
            put_back += 1
        if os.path.exists(path + ORIG):
            os.replace(path + ORIG, path)
            print('  restored   : %s' % os.path.basename(path))
            put_back += 1
    print(('%d file(s) put back' % put_back) if put_back else 'nothing to put back')
    return 0


@item('8-83', 'C16: the registry\'s "schema" becomes the string "2" -> refused, no CTD')
def i_8_83(d):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import cefjson
    keep_original(REGISTRY)
    doc = cefjson.load(REGISTRY)
    doc['schema'] = '2'
    with io.open(REGISTRY, 'w', encoding='utf-8', newline='') as f:
        f.write(cefjson.pretty(doc))
    print('  schema -> "2" (a string); the abilities and their checksum are untouched')
    print('  expect: no CTD, "abilities: DISABLED - ... has no schema number this build')
    print('          can read". Undo with "batch5.py unbreak"')


@item('8-84', 'C7: the registry is missing but its backups are not -> no empty start')
def i_8_84(d):
    set_aside(REGISTRY)
    print('  expect: "... is missing but its backup is not, so this is not a first run".')
    print('          "cef abilities restore" + restart fixes it. Then "batch5.py unbreak"')


@item('8-85', 'C7: the registry AND its backups are gone -> the save audit reserves')
def i_8_85(d):
    for path in [REGISTRY] + REGISTRY_BACKUPS:
        set_aside(path)
    print('  load a save wearing an enchanted box. expect: "this save holds pool ability')
    print('  ... that the registry does not know - reserved" and DO NOT SAVE.')
    print('  DO NOT SAVE. Quit, then "batch5.py unbreak"')


@item('8-88', 'M3: "bEnabled=0 ; comment" in the ini -> CEF is disabled')
def i_8_88(d):
    keep_original(INI)
    with io.open(INI, encoding='utf-8', newline='') as f:
        text = f.read()
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)
    hit = [i for i, l in enumerate(lines) if l.strip().lower().startswith('benabled')]
    if len(hit) != 1:
        os.remove(INI + ORIG)
        raise SystemExit('expected one bEnabled line in %s, found %d' % (INI, len(hit)))
    lines[hit[0]] = 'bEnabled=0 ; batch5 8-88 - an inline comment must not void this'
    with io.open(INI, 'w', encoding='utf-8', newline='') as f:
        f.write(eol.join(lines))
    print('  ini line %d -> %s' % (hit[0] + 1, lines[hit[0]]))
    print('  expect: "CostumeExpansionFW is DISABLED via external config - doing nothing".')
    print('          Undo with "batch5.py unbreak"')


@item('8-91', 'W11: one more box, on a publish token -> quarantined and left alone')
def i_8_91(d):
    token = '000800:CostumeFW_NPC.esp'
    if any(b.get('token') == token for b in d['boxes']):
        raise SystemExit('a box already holds %s - run restore.ps1 first' % token)
    # Shaped like the boxes CEF writes today: the first one's keys, minus the
    # preset link, with this item's values.
    box = {k: v for k, v in d['boxes'][0].items() if k not in ('preset', 'presetName')}
    box.update({
        'boxId': 'B5B5B5B5B5B50891',
        'contents': [],
        'enabled': True,
        'label': 'batch5 8-91 (publish token)',
        'token': token,
        'wear': False,
    })
    d['boxes'].append(box)
    print('  added "%s" on %s' % (box['label'], token))
    print('  expect: quarantined (foreign plugin); toggling the master switch leaves')
    print('          the NPC wearing CFW_PubToken01 alone ("cef tokens")')


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]

    if cmd == 'unbreak':
        return unbreak()

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
