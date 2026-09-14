"""v1.6.4 step 5 - the carrier namespace is per TOKEN, not per biped slot.

Drives the real Sync() through nifcarrier_cli, so the thing under test is the
code the DLL runs, not a re-implementation of it:

  T1  a pre-1.6.4 manifest + a slot-keyed carriers.json: the entry moves to
      "Box55", keeps its revision, and keeps using the files that shipped
  T2  two boxes on slot 55: two namespaces, neither overwriting the other, and
      an unchanged re-run that skips both
  T3  a hand-edited carrier key that could escape the mod folder: refused

Run after a release build:

    python tests/nifcarrier/carrier_namespace_test.py

Needs build/release/nifcarrier_cli.exe (CEF_BUILD_NIFCARRIER_CLI, on by
default). Writes only into a temp directory.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CLI = os.path.join(REPO, 'build', 'release', 'nifcarrier_cli.exe')
EMPTY = os.path.join(REPO, 'package_assets', 'meshes', 'CostumeFW', 'boxtoken.nif')

GEN0 = {'slot': 55, 'token': '000811:CostumeFW.esp', 'carrierKey': 'Box55', 'contents': []}
POOL1 = {'slot': 55, 'token': '000800:CostumeFW_BoxPool1.esp', 'carrierKey': 'BP01_000800',
         'contents': []}

root = ''
failures = []


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what)
    if not ok:
        failures.append(what)


def fresh():
    """An empty CEF mod folder to sync into."""
    global root
    if root:
        shutil.rmtree(root, ignore_errors=True)
    root = tempfile.mkdtemp(prefix='cef_carrier_ns_')
    os.makedirs(carrier_dir())
    os.makedirs(os.path.join(carrier_dir(), 'XML'))
    os.makedirs(os.path.join(root, 'data'))


def carrier_dir():
    return os.path.join(root, 'out', 'meshes', 'CostumeFW')


def carriers():
    with open(os.path.join(carrier_dir(), 'carriers.json'), encoding='utf-8') as f:
        return json.load(f)


def artifact(name):
    return os.path.exists(os.path.join(carrier_dir(), name))


def sync(boxes, version=2):
    path = os.path.join(root, 'manifest.json')
    manifest = {'version': version, 'boxes': boxes}
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(manifest, f)
    done = subprocess.run(
        [CLI, 'sync', path, '--data', os.path.join(root, 'data'),
         '--out', os.path.join(root, 'out'), '--empty', EMPTY],
        capture_output=True, text=True)
    return done.returncode, done.stdout


print('T1 - a 1.6.3 carriers.json migrates off the slot key')
fresh()
# What 1.6.3 left behind: an entry keyed by the slot, on revision 3.
shutil.copy(EMPTY, os.path.join(carrier_dir(), 'Box55_carrier.nif'))
shutil.copy(EMPTY, os.path.join(carrier_dir(), 'Box55_carrier_r3.nif'))
with open(os.path.join(carrier_dir(), 'carriers.json'), 'w', encoding='utf-8') as f:
    f.write('{ "55": { "rev": 3, "file": "CostumeFW/Box55_carrier_r3.nif" } }')
rc, log = sync([{'slot': 55, 'token': '000811:CostumeFW.esp', 'contents': []}], version=1)
cj = carriers()
check(rc == 0, 'sync succeeded (rc=%d)' % rc)
check('55' not in cj, 'the bare slot key is gone')
check('Box55' in cj, 'the entry is keyed Box55')
check(cj.get('Box55', {}).get('rev') == 4, 'the revision carried over and advanced (3 -> 4)')
check(cj.get('Box55', {}).get('file', '').startswith('CostumeFW/Box55_carrier_r'),
      'still pointed at the files that shipped')
check(artifact('Box55_carrier.nif') and artifact('Box55_carrier.hash'),
      'generation 0 artifacts keep the names their ARMA already names')

print('T2 - two boxes on slot 55 build in separate namespaces')
fresh()
rc, log = sync([GEN0, POOL1])
cj = carriers()
check(rc == 0, 'sync succeeded (rc=%d)' % rc)
check('Box55' in cj and 'BP01_000800' in cj, 'both boxes have their own entry')
check(cj['Box55']['file'] != cj['BP01_000800']['file'], 'they point at different carriers')
check(artifact('Box55_carrier.nif') and artifact('BP01_000800_carrier.nif'),
      'both carriers exist')
check(artifact('Box55_carrier.hash') and artifact('BP01_000800_carrier.hash'),
      'each has its own hash - one box syncing cannot skip the other')
check(artifact('Box55_carrier_r7.nif') and artifact('BP01_000800_carrier_r7.nif'),
      'each has its own revision pool')
rc, log = sync([GEN0, POOL1])
again = carriers()
check(again['Box55']['rev'] == cj['Box55']['rev'] and
      again['BP01_000800']['rev'] == cj['BP01_000800']['rev'],
      'an unchanged second run bumps no revision')

print('T3 - a carrier key that could escape the mod folder is refused')
fresh()
rc, log = sync([{'slot': 55, 'token': '000811:CostumeFW.esp',
                 'carrierKey': '..\\..\\evil', 'contents': []}])
check(rc != 0, 'sync reports failure (rc=%d)' % rc)
check('refusing carrier key' in log, 'and says why')
check(not artifact('evil_carrier.nif') and
      not os.path.exists(os.path.join(root, 'evil_carrier.nif')),
      'nothing was written outside the carrier folder')

print('T4 - a box cannot take a stem another pipeline owns')
for reserved in ('Persist', 'Pub01', 'NpcPersist03', 'persist'):
    fresh()
    rc, log = sync([{'slot': 55, 'token': '000811:CostumeFW.esp',
                     'carrierKey': reserved, 'contents': []}])
    check(rc != 0 and not artifact(reserved + '_carrier.nif'),
          '"%s" is refused, not built over' % reserved)

shutil.rmtree(root, ignore_errors=True)
print()
if failures:
    print('carrier namespace: %d FAILURE(S)' % len(failures))
    sys.exit(1)
print('carrier namespace: all checks passed')
