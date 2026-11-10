"""The ability registry's checksum: tools/cefjson.py has to agree with the DLL.

CEF refuses CEF_abilities.json at launch when its "checksum" disagrees with
FNV-1a over nlohmann's compact dump of the abilities array, and the tools that
edit the registry for tests (tools/fill_gen1.py, tools/batch5.py) recompute it
in Python through cefjson. This holds the two to one answer, both ways round:

  R1  a registry CEF wrote: cefjson reproduces nlohmann's compact dump byte for
      byte, and so the checksum (fill_gen1.py's self-test)
  R2  a registry cefjson edited: the DLL's code accepts the checksum cefjson
      wrote into it (what fill_gen1.py --write hands CEF)

The fixture carries what makes the serialisation hard: floats that came from
32-bit values, a uint64 at its maximum, CJK and a 4-byte character, quotes,
backslashes, a tab and control characters. The C++ half is the DLL's own code
(src/RegistryChecksum.h) through build/release/registry_checksum.exe. Run after
a release build:

    python tests/registry_checksum_test.py

Writes only into a temp directory.
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
TOOL = os.path.join(REPO, 'build', 'release', 'registry_checksum.exe')
FIXTURE = os.path.join(REPO, 'tests', 'fixtures', 'abilities_registry.json')
sys.path.insert(0, os.path.join(REPO, 'tools'))
import cefjson  # noqa: E402

failures = []


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what)
    if not ok:
        failures.append(what)


def first_difference(a, b):
    # ascii(): the fixture holds a 4-byte character, and a cp932 console cannot
    # print it - the diagnostic must not be what crashes.
    n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
    return 'at %d: cefjson %s / nlohmann %s' % (n, ascii(a[max(0, n - 20):n + 20]),
                                                 ascii(b[max(0, n - 20):n + 20]))


def run_tool(src, tmp):
    """The DLL's code on `src`: (checksum, the rewritten file, compact dump)."""
    written = os.path.join(tmp, 'written.json')
    compact = os.path.join(tmp, 'compact.json')
    r = subprocess.run([TOOL, src, written, compact], capture_output=True)
    if r.returncode != 0:
        raise SystemExit('registry_checksum failed: ' + r.stderr.decode('utf-8', 'replace'))
    with io.open(compact, encoding='utf-8', newline='') as f:
        return r.stdout.decode('ascii').strip(), written, f.read()


def main():
    if not os.path.exists(TOOL):
        print('missing', TOOL, '- run build.cmd release first')
        return 1
    tmp = tempfile.mkdtemp(prefix='cef_regsum_')
    try:
        print('R1  a registry CEF wrote')
        cpp_sum, written, cpp_compact = run_tool(FIXTURE, tmp)
        doc = cefjson.load(written)
        arr = doc['abilities']
        py_compact = cefjson.dumps(arr)
        check(py_compact == cpp_compact, 'cefjson.dumps is nlohmann\'s compact dump, byte for byte')
        if py_compact != cpp_compact:
            print('       ' + first_difference(py_compact, cpp_compact))
        check(cefjson.checksum(arr) == cpp_sum, 'cefjson.checksum is the DLL\'s checksum')
        check(doc['checksum'] == cpp_sum, 'the written file carries that checksum')

        print('R2  a registry cefjson edited')
        # What fill_gen1.py adds (a tombstone), plus a live entry in a script
        # that is not ASCII.
        arr.append({
            'ability': '000BFF:CostumeFW_Abilities.esp',
            'content': '',
            'recipeGeneration': cefjson.Raw('0'),
            'tombstone': True,
            'effects': [],
        })
        arr.append({
            'ability': '000803:CostumeFW_Abilities.esp',
            'content': '000D65:衣装\U0001F600 "2".esp',
            'recipeGeneration': cefjson.Raw('3'),
            'tombstone': False,
            'effects': [],
        })
        doc['checksum'] = cefjson.checksum(arr)
        edited = os.path.join(tmp, 'edited.json')
        with io.open(edited, 'w', encoding='utf-8', newline='') as f:
            f.write(cefjson.pretty(doc) + '\n')
        cpp_sum2, _, cpp_compact2 = run_tool(edited, tmp)
        check(cpp_sum2 == doc['checksum'], 'the DLL accepts the checksum cefjson wrote')
        check(cefjson.dumps(arr) == cpp_compact2, 'and reads the same array back')
        if cefjson.dumps(arr) != cpp_compact2:
            print('       ' + first_difference(cefjson.dumps(arr), cpp_compact2))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print('registry_checksum_test: %d failure(s)' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
