"""NE (16-bit Windows 3.x) executable parser.

Gives segments (with their bytes), relocation records, the entry table,
imported modules, resources and the resident/non-resident name tables.

usage: python tools/ne.py FILE.EXE [imports|segs|entries|relocs|res]
"""
import struct, sys, collections

# relocation source types
SRC_LOBYTE, SRC_SEGMENT, SRC_FARPTR, SRC_OFFSET = 0, 2, 3, 5
SRC_FARPTR48, SRC_OFFSET32 = 11, 13
# relocation target types
REL_INTERNAL, REL_IMPORTORD, REL_IMPORTNAME, REL_OSFIXUP = 0, 1, 2, 3


class Seg:
    def __init__(self, idx, off, flen, flags, alloc):
        self.idx = idx            # 1-based
        self.off = off            # file offset (0 = no data)
        self.flen = flen
        self.flags = flags
        self.alloc = alloc
        self.data = b''
        self.relocs = []          # (srctype, additive, [offsets], target)

    @property
    def is_data(self):
        return bool(self.flags & 1)

    @property
    def size(self):
        return max(self.alloc, len(self.data))


class NE:
    def __init__(self, path):
        d = self.raw = open(path, 'rb').read()
        ne = self.ne = struct.unpack_from('<H', d, 0x3c)[0]
        assert d[ne:ne + 2] == b'NE', 'not an NE file'
        h = lambda o, f='<H': struct.unpack_from(f, d, ne + o)[0]
        self.flags = h(0x0c)
        self.autodata = h(0x0e)
        self.heap = h(0x10)
        self.stack = h(0x12)
        self.ip, self.cs = struct.unpack_from('<HH', d, ne + 0x14)
        self.sp, self.ss = struct.unpack_from('<HH', d, ne + 0x18)
        nseg, nmod = h(0x1c), h(0x1e)
        nonres_size = h(0x20)
        segtab, rsrc, resn, modtab, imptab = (ne + h(o) for o in (0x22, 0x24, 0x26, 0x28, 0x2a))
        nonres = h(0x2c, '<I')
        entry = ne + h(0x04)
        entry_len = h(0x06)
        self.align = h(0x32) or 9
        self.segs = []
        for i in range(nseg):
            off, flen, flags, alloc = struct.unpack_from('<HHHH', d, segtab + 8 * i)
            s = Seg(i + 1, off << self.align, flen or (0x10000 if off else 0), flags, alloc or 0x10000)
            if s.off:
                s.data = d[s.off:s.off + s.flen]
                if flags & 0x100:      # relocation info follows
                    p = s.off + s.flen
                    n = struct.unpack_from('<H', d, p)[0]
                    p += 2
                    for k in range(n):
                        st, fl, so, a, b = struct.unpack_from('<BBHHH', d, p + 8 * k)
                        s.relocs.append(self._reloc(s, st, fl, so, a, b))
            self.segs.append(s)
        # imported module names
        self.modules = []
        for i in range(nmod):
            o = struct.unpack_from('<H', d, modtab + 2 * i)[0]
            self.modules.append(self._pstr(imptab + o))
        self.imptab = imptab
        # names
        self.resident = self._names(resn)
        self.nonresident = self._names(nonres) if nonres else {}
        self.name = min(self.resident.items(), key=lambda kv: kv[1])[0] if self.resident else ''
        # entry table: ordinal -> (seg, off, flags)
        self.entries = {}
        p, ordn = entry, 1
        while p < entry + entry_len:
            cnt, ind = d[p], d[p + 1]
            p += 2
            if cnt == 0:
                break
            for k in range(cnt):
                if ind == 0:
                    pass
                elif ind == 0xff:     # movable: flags, int3f, seg, off
                    fl, _, sg, of = struct.unpack_from('<BHBH', d, p)
                    self.entries[ordn] = (sg, of, fl)
                    p += 6
                else:                 # fixed segment
                    fl, of = struct.unpack_from('<BH', d, p)
                    self.entries[ordn] = (ind, of, fl)
                    p += 3
                ordn += 1
        # resources
        self.resources = []   # (type, id, file offset, length)
        sh = struct.unpack_from('<H', d, rsrc)[0]
        p = rsrc + 2
        while True:
            t, c = struct.unpack_from('<HH', d, p)
            if t == 0:
                break
            p += 8
            for k in range(c):
                o, l, f, rid = struct.unpack_from('<HHHH', d, p)
                p += 12
                tn = t & 0x7fff if t & 0x8000 else self._pstr(rsrc + t)
                rn = rid & 0x7fff if rid & 0x8000 else self._pstr(rsrc + rid)
                self.resources.append((tn, rn, o << sh, l << sh))

    def _pstr(self, p):
        n = self.raw[p]
        return self.raw[p + 1:p + 1 + n].decode('latin1')

    def _names(self, p):
        r = {}
        while self.raw[p]:
            s = self._pstr(p)
            p += 1 + len(s)
            r[s] = struct.unpack_from('<H', self.raw, p)[0]
            p += 2
        return r

    def _reloc(self, seg, st, fl, so, a, b):
        typ = fl & 3
        add = bool(fl & 4)
        if typ == REL_INTERNAL:
            tgt = ('int', a, b) if a != 0xff else ('mov', b)     # movable: entry ordinal b
        elif typ == REL_IMPORTORD:
            tgt = ('ord', a, b)
        elif typ == REL_IMPORTNAME:
            tgt = ('name', a, self._pstr(self.imptab + b))
        else:
            tgt = ('os', a, b)
        # chained offsets (non-additive): each patch site holds the next offset, 0xffff ends
        offs = []
        if add:
            offs = [so]
        else:
            o = so
            seen = set()
            while o != 0xffff and o not in seen and o + 2 <= len(seg.data):
                seen.add(o)
                offs.append(o)
                if st == SRC_LOBYTE:
                    break
                o = struct.unpack_from('<H', seg.data, o)[0]
        return (st, add, offs, tgt)

    def target(self, tgt):
        """resolve an internal target to (seg, off)"""
        if tgt[0] == 'int':
            return tgt[1], tgt[2]
        if tgt[0] == 'mov':
            s, o, _ = self.entries[tgt[1]]
            return s, o
        return None

    def import_name(self, tgt):
        mod = self.modules[tgt[1] - 1]
        return mod, tgt[2]


def main():
    ne = NE(sys.argv[1])
    what = sys.argv[2] if len(sys.argv) > 2 else 'imports'
    if what == 'segs':
        for s in ne.segs:
            print('seg %2d %s flen %5d alloc %5d flags %04x relocs %d' % (
                s.idx, 'DATA' if s.is_data else 'CODE', s.flen, s.alloc, s.flags, len(s.relocs)))
        print('entry %d:%04x  stack %d:%04x size %d heap %d autodata %d' % (ne.cs, ne.ip, ne.ss, ne.sp, ne.stack, ne.heap, ne.autodata))
    elif what == 'imports':
        c = collections.Counter()
        for s in ne.segs:
            for st, add, offs, t in s.relocs:
                if t[0] in ('ord', 'name'):
                    c[(ne.modules[t[1] - 1], t[2])] += len(offs)
        for (m, o), n in sorted(c.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))):
            print('%-9s %-6s %d' % (m, o, n))
        print(len(c), 'imports')
    elif what == 'entries':
        names = {v: k for k, v in list(ne.resident.items()) + list(ne.nonresident.items())}
        for o, (s, of, fl) in sorted(ne.entries.items()):
            print('%4d %2d:%04x %02x %s' % (o, s, of, fl, names.get(o, '')))
    elif what == 'relocs':
        c = collections.Counter()
        for s in ne.segs:
            for st, add, offs, t in s.relocs:
                c[(st, t[0], add)] += len(offs)
        for k, v in sorted(c.items()):
            print(k, v)
    elif what == 'res':
        for r in ne.resources:
            print(r)


if __name__ == '__main__':
    main()
