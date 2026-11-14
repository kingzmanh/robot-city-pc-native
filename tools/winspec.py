"""ordinal -> (name, pascal arg types, return) from Wine 16-bit .spec files in re/winspec"""
import os, re
D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 're', 'winspec')
FILES = {'KERNEL': 'krnl386.exe16.spec', 'USER': 'user.exe16.spec', 'GDI': 'gdi.exe16.spec',
         'MMSYSTEM': 'mmsystem.dll16.spec', 'WING': 'wing.dll16.spec', 'WIN87EM': 'win87em.dll16.spec'}
_cache = {}
def table(mod):
    if mod not in _cache:
        t = {}
        for l in open(os.path.join(D, FILES[mod])):
            m = re.match(r'\s*(\d+)\s+(\w+)\s+(.*)', l)
            if not m: continue
            o, kind, rest = int(m.group(1)), m.group(2), m.group(3)
            rest = re.sub(r'-\w+\s+', '', rest)
            m2 = re.match(r'(\w+)\s*\(([^)]*)\)', rest)
            if m2:
                t[o] = (m2.group(1), m2.group(2).split(), kind)
            else:
                t[o] = (rest.split()[0] if rest.split() else '?', None, kind)
        _cache[mod] = t
    return _cache[mod]
def lookup(mod, o):
    return table(mod).get(o, ('%s_%d' % (mod, o), None, '?'))
