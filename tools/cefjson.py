"""Serialize exactly the way nlohmann::json::dump() does, for CEF's checksum.

The registry's `checksum` is FNV-1a over the COMPACT dump of the abilities
array, so anything that edits the file has to reproduce that dump byte for byte
or CEF rejects the whole registry. Two things make json.dumps insufficient:

  * floats - nlohmann writes a shortest-round-trip form ("250.0",
    "3256.831787109375") that Python's repr does not always match. So floats are
    parsed as their ORIGINAL TEXT (parse_float=Raw) and written back verbatim;
    exact by construction for anything already in the file.
  * non-ASCII - nlohmann emits raw UTF-8, json.dumps escapes by default.

Keys are sorted because nlohmann's default json is std::map-backed.
"""
import json


class Raw(str):
    """A number kept as the exact text the file carried."""


def load(path):
    import io
    return json.load(io.open(path, encoding='utf-8'), parse_float=Raw, parse_int=Raw)


def _esc(s):
    # json.dumps on a lone string gives us the escaping rules; strip the quotes.
    return json.dumps(s, ensure_ascii=False)


def dumps(v):
    if isinstance(v, Raw):
        return str(v)
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if v is None:
        return 'null'
    if isinstance(v, str):
        return _esc(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, list):
        return '[' + ','.join(dumps(x) for x in v) + ']'
    if isinstance(v, dict):
        return '{' + ','.join('%s:%s' % (_esc(k), dumps(v[k]))
                              for k in sorted(v)) + '}'
    raise TypeError(type(v))


def pretty(v, indent=0):
    """dump(2), the form CEF writes the file in."""
    pad = ' ' * indent
    pad2 = ' ' * (indent + 2)
    if isinstance(v, list):
        if not v:
            return '[]'
        inner = (',\n').join(pad2 + pretty(x, indent + 2) for x in v)
        return '[\n' + inner + '\n' + pad + ']'
    if isinstance(v, dict):
        if not v:
            return '{}'
        inner = (',\n').join('%s%s: %s' % (pad2, _esc(k), pretty(v[k], indent + 2))
                             for k in sorted(v))
        return '{\n' + inner + '\n' + pad + '}'
    return dumps(v)


def fnv1a64(text):
    h = 1469598103934665603
    for c in text.encode('utf-8'):
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def checksum(arr):
    return '%016x' % fnv1a64(dumps(arr))
