"""x86-16 -> C static recompiler for 16-bit Windows (NE) programs.

usage: recomp16.py GAME.EXE ghidra_functions.tsv native.txt outdir module

Code addresses are written seg:off as one number (seg << 16 | off), seg =
NE segment number.  Every function becomes `void <module>_<seg>_<off>(void)`
on the shared register file (cpu.h).  Memory operands are linear =
segment base (cpu.sbase[] for es/ss/ds, the fixed code base for cs) + 16-bit
offset.  Selectors: NE segment i = (i << 3) | 7; imported functions are far
pointers API_SEL:index, the runtime dispatches them to native code.

The instruction emitter is recomp.py's Gen; this file overrides what is
different in 16-bit code: addressing, stack width, string ops, far/near
calls and returns, NE relocations, switch tables in the code segment.
"""
import sys, os, re, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone import x86 as X
import recomp
from recomp import Gen, flag_use_def, ALL, CF, ZF, SF, OF, PF, AF, MASK, SBIT, SEGS, cond_of
from x86an import JCC
import ne as NEmod
import winspec

API_SEL = 0xf7          # selector of the import thunk "segment"
NORET_API = {'FatalExit', 'FatalAppExit'}
# OS fixups (floating point emulator calls int 34h-3Dh -> real x87 code)
OSFIX = {1: (0xFE32, 0x4000), 2: (0x0632, 0x8000), 3: (0x0E32, 0xC000), 4: (0x1632, 0), 5: (0x5C32, None), 6: (0xA23D, None)}
AH = {113: 3, 114: 8}   # __AHSHIFT, __AHINCR


def SEL(i):
    return (i << 3) | 7


class Prog16:
    def __init__(self, path):
        self.ne = n = NEmod.NE(path)
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True
        self.img = {}          # seg -> bytearray (relocations applied)
        self.apis = []         # index -> (module, ordinal, name)
        self.api_index = {}
        self.far_imports = {}  # (seg, off of the far pointer) -> api index
        self.code_segs = [s.idx for s in n.segs if not s.is_data and s.data]
        for s in n.segs:
            b = bytearray(s.data) + bytearray(max(0, s.alloc - len(s.data)))
            for st, add, offs, t in s.relocs:
                for o in offs:
                    self._apply(s.idx, b, st, add, o, t)
            self.img[s.idx] = b
        self.icache = {}

    def api(self, mod, o):
        k = (mod, o)
        if k not in self.api_index:
            self.api_index[k] = len(self.apis)
            self.apis.append((mod, o, winspec.lookup(mod, o)[0]))
        return self.api_index[k]

    def _apply(self, seg, b, st, add, o, t):
        def put16(off, v):
            if add:
                v = (struct.unpack_from('<H', b, off)[0] + v) & 0xffff
            struct.pack_into('<H', b, off, v)
        if t[0] == 'os':
            # the code is real x87 code; these fixups would turn it into
            # emulator calls (int 34h-3Dh) on a machine without an FPU
            return
        if t[0] in ('ord', 'name'):
            mod = self.ne.modules[t[1] - 1]
            if mod == 'KERNEL' and t[2] in AH:
                v = AH[t[2]]
                if st == NEmod.SRC_FARPTR:
                    put16(o, v); struct.pack_into('<H', b, o + 2, 0)
                else:
                    put16(o, v)
                return
            if mod == 'KERNEL' and t[2] == 178:     # __WINFLAGS: enhanced mode, 386, FPU
                put16(o, 0x0429)
                return
            idx = self.api(mod, t[2])
            if st == NEmod.SRC_FARPTR:
                struct.pack_into('<HH', b, o, idx, API_SEL)
                self.far_imports[(seg, o)] = idx
            elif st == NEmod.SRC_SEGMENT:
                put16(o, API_SEL)
            elif st == NEmod.SRC_OFFSET:
                put16(o, idx)
            else:
                raise ValueError('import reloc type %d' % st)
            return
        tg = self.ne.target(t)
        ts, to = tg
        if st == NEmod.SRC_SEGMENT:
            put16(o, SEL(ts))
        elif st == NEmod.SRC_FARPTR:
            struct.pack_into('<HH', b, o, to, SEL(ts))
        elif st == NEmod.SRC_OFFSET:
            put16(o, to)
        elif st == NEmod.SRC_LOBYTE:
            b[o] = to & 0xff
        else:
            raise ValueError('reloc type %d' % st)

    def seg_of_sel(self, sel):
        if sel & 7 == 7 and 1 <= (sel >> 3) <= len(self.ne.segs):
            return sel >> 3
        return None

    def in_code(self, a):
        s, o = a >> 16, a & 0xffff
        return s in self.code_segs and o < len(self.ne.segs[s - 1].data)

    def insn(self, a):
        i = self.icache.get(a)
        if i is None and a not in self.icache:
            s, o = a >> 16, a & 0xffff
            b = self.img.get(s)
            i = None
            if b is not None and o < len(self.ne.segs[s - 1].data):
                try:
                    i = next(self.md.disasm(bytes(b[o:o + 16]), a, 1))
                except StopIteration:
                    i = None
            self.icache[a] = i
        return i

    def rd16(self, a):
        return struct.unpack_from('<H', self.img[a >> 16], a & 0xffff)[0]


def target(ins):
    """near branch target (wraps inside the segment)"""
    return (ins.address & 0xffff0000) | (ins.operands[0].imm & 0xffff)


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.blocks = {}
        self.succ = {}
        self.tailcalls = {}
        self.tables = {}
        self.multi = False
        self.calls = []


def far_target(p, ins):
    """lcall/ljmp seg:off -> ('code', addr) / ('api', idx) / None"""
    sel = ins.operands[0].imm & 0xffff
    off = ins.operands[1].imm & 0xffff
    if sel == API_SEL:
        return ('api', off)
    s = p.seg_of_sel(sel)
    if s and s in p.code_segs:
        return ('code', (s << 16) | off)
    return None


def switch_table(p, f_insns, ins, inherited=None, entry=0):
    """jmp word ptr cs:[reg + disp] -> list of targets, or None"""
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM or op.size != 2 or not op.mem.segment or ins.reg_name(op.mem.segment) != 'cs':
        return None
    if op.mem.index:
        return None
    seg = ins.address >> 16
    tab = op.mem.disp & 0xffff
    reg = ins.reg_name(op.mem.base) if op.mem.base else None
    # bound from a preceding "cmp r, N ; ja/jbe" in the last instructions
    n = None
    for prev in reversed(f_insns[-12:]):
        if prev.mnemonic == 'cmp' and prev.operands[1].type == X.X86_OP_IMM and prev.operands[0].type == X.X86_OP_REG:
            n = (prev.operands[1].imm & 0xffff) + 1
            break
    if n is None and inherited:
        n = inherited
    lo = entry & 0xffff
    out = []
    limit = n if n else 64
    segdata = p.ne.segs[seg - 1].data
    end = 0x10000          # unbounded tables end where the code they jump to begins
    for k in range(limit):
        o = tab + 2 * k
        if o + 2 > len(segdata) or (n is None and o >= end):
            break
        t = p.rd16((seg << 16) | o)
        if t >= len(segdata) or (tab <= t < o + 2) or (n is None and t < lo):
            break
        if t > tab:
            end = min(end, t)
        out.append((seg << 16) | t)
    return out or None


def analyze(p, entries, noreturn_api):
    funcs = {}
    todo = list(dict.fromkeys(e for e in entries if p.in_code(e)))
    known = set(todo)
    # pass 1: find all function entries
    while todo:
        e = todo.pop()
        for c in explore(p, e, known, noreturn_api, collect=True):
            if c not in known and p.in_code(c):
                known.add(c)
                todo.append(c)
    # pass 2: build functions with the final entry set
    for e in sorted(known):
        f = Func(e)
        explore(p, e, known, noreturn_api, collect=False, f=f)
        funcs[e] = f
    return funcs


def explore(p, entry, known, noreturn_api, collect, f=None):
    """walk one function; returns call targets (collect) or fills f"""
    calls = []
    seen = set()
    work = [entry]
    starts = {entry}
    blocks = {}
    bound = {}
    while work:
        b = work.pop()
        if b in seen:
            continue
        seen.add(b)
        a = b
        insns = []
        succ = []
        while True:
            ins = p.insn(a)
            if ins is None:
                break
            insns.append(ins)
            m = ins.mnemonic
            nxt = a + ins.size
            if m in JCC:
                t = target(ins)
                succ = [t, nxt]
                for prev in reversed(insns[-3:-1]):
                    if prev.mnemonic == 'cmp' and prev.operands[1].type == X.X86_OP_IMM and prev.operands[0].type == X.X86_OP_REG:
                        k = (prev.operands[1].imm & 0xffff) + 1
                        for sx in succ:
                            bound.setdefault(sx, k)
                        break
                break
            if m == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = target(ins)
                    if t in known and t != entry and not collect:
                        f.tailcalls[a] = t
                        succ = []
                    else:
                        succ = [t]
                else:
                    tab = switch_table(p, insns, ins, bound.get(b), entry)
                    if tab:
                        succ = list(dict.fromkeys(tab))
                        if f:
                            f.tables[a] = tab
                    else:
                        succ = []
                break
            if m == 'ljmp':
                if ins.operands[0].type == X.X86_OP_IMM:
                    ft = far_target(p, ins)
                    if ft and ft[0] == 'code':
                        calls.append(ft[1])
                        if f:
                            f.tailcalls[a] = ft[1]
                succ = []
                break
            if m in ('ret', 'retf', 'iret', 'hlt'):
                break
            if m == 'call' and ins.operands[0].type == X.X86_OP_IMM:
                calls.append(target(ins))
            if m == 'lcall' and ins.operands[0].type == X.X86_OP_IMM:
                ft = far_target(p, ins)
                if ft and ft[0] == 'code':
                    calls.append(ft[1])
                elif ft and ft[0] == 'api' and p.apis[ft[1]][2] in noreturn_api:
                    break
            a = nxt
            if a in starts and a != b:
                succ = [a]
                break
            if m == 'jmp' and False:
                pass
        blocks[b] = (insns, succ)
        for s in succ:
            if p.in_code(s) and (s >> 16) == (entry >> 16):
                starts.add(s)
                if s not in seen:
                    work.append(s)
    if f is not None:
        # split blocks so that every block start is unique and no block overlaps another start
        allstarts = set(blocks)
        for b, (insns, succ) in blocks.items():
            cur = b
            cur_l = []
            for ins in insns:
                if ins.address != b and ins.address in allstarts:
                    f.blocks[cur] = cur_l
                    f.succ[cur] = [ins.address]
                    cur = None
                    break
                cur_l.append(ins.address)
            if cur is not None:
                f.blocks[cur] = cur_l
                f.succ[cur] = [s for s in succ if (s >> 16) == (entry >> 16)]
        f.calls = calls
    return calls


# tools/trace16.txt: instruction addresses (seg:off) where registers are logged
TRACE16 = set()
_tp16 = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trace16.txt')
if os.path.exists(_tp16):
    for _l in open(_tp16):
        _l = _l.split('#')[0].strip()
        if _l:
            _s, _o = _l.split(':')
            TRACE16.add((int(_s, 16) << 16) | int(_o, 16))


# tools/patch16.txt: "seg:off C statement" - C code run before that instruction
# (game fixes / enhancements that need a few registers changed)
PATCH16 = {}
_pp16 = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'patch16.txt')
if os.path.exists(_pp16):
    for _l in open(_pp16):
        _l = _l.strip()
        if _l and not _l.startswith('#'):
            _a, _c = _l.split(None, 1)
            _s, _o = _a.split(':')
            PATCH16.setdefault((int(_s, 16) << 16) | int(_o, 16), []).append(_c)


class Gen16(Gen):
    def __init__(self, p, funcs, native):
        super().__init__(p, funcs, native, set())
        self.seg = 0

    # ---------- registers / addressing ----------
    def reg_rd(self, name):
        if name == 'cs':
            return '0x%xu' % SEL(self.seg)
        return super().reg_rd(name)

    def segbase(self, sn):
        if sn == 'cs':
            return 'rt_seg_base[%d]' % self.seg
        return 'cpu.sbase[%d]' % SEGS[sn]

    def offset(self, ins, mem):
        parts = []
        wide = False
        for r in (mem.base, mem.index):
            if r and ins.reg_name(r).startswith('e'):
                wide = True
        if mem.base:
            parts.append(self.reg_rd(ins.reg_name(mem.base)))
        if mem.index:
            ix = self.reg_rd(ins.reg_name(mem.index))
            parts.append('%s*%d' % (ix, mem.scale) if mem.scale != 1 else ix)
        d = mem.disp & (0xffffffff if wide else 0xffff)
        if d or not parts:
            parts.append('0x%xu' % d)
        return ('(u32)(%s)' if wide else '(u16)(%s)') % '+'.join(parts)

    def addr(self, ins, mem):
        sn = ins.reg_name(mem.segment) if mem.segment else None
        if not sn:
            b = ins.reg_name(mem.base) if mem.base else None
            sn = 'ss' if b in ('bp', 'sp', 'ebp', 'esp') else 'ds'
        return '(u32)(%s+%s)' % (self.segbase(sn), self.offset(ins, mem))

    def vpfx(self, ins, op):
        return ''

    # ---------- per instruction ----------
    def insn(self, f, ins, live_after, next_addr):
        mn = ins.mnemonic
        ops = ins.operands
        a = ins.address
        if mn == 'lea':
            return self.wr(ins, ops[0], self.offset(ins, ops[1].mem))
        if mn in ('cbw', 'cwde', 'cwd', 'cdq'):
            # capstone names 98 / 99 cwde / cdq in 16-bit mode too: the operand size prefix decides
            o32 = 0x66 in ins.prefix
            if mn in ('cbw', 'cwde'):
                return 'eax = (u32)(s32)(s16)eax;' if o32 else 'eax = (eax & 0xffff0000u) | ((u32)(s16)(s8)eax & 0xffff);'
            return "edx = ((s32)eax < 0) ? 0xffffffffu : 0;" if o32 else 'edx = (edx & 0xffff0000u) | (((s16)eax < 0) ? 0xffffu : 0);'
        if mn == 'push':
            o = ops[0]
            if o.size == 4 and not (o.type == X.X86_OP_REG and ins.reg_name(o.reg) in SEGS):
                return '{ u32 v = %s; PUSH32(v); }' % self.rd(ins, o, 4)
            return '{ u32 v = %s; PUSH16(v); }' % self.rd(ins, o, 2)
        if mn == 'pop':
            o = ops[0]
            if o.size == 4 and not (o.type == X.X86_OP_REG and ins.reg_name(o.reg) in SEGS):
                return '{ u32 v = POP32(); %s }' % self.wr(ins, o, 'v')
            if o.type == X.X86_OP_REG and ins.reg_name(o.reg) == 'sp':
                return 'esp = RD16(SSB + (u16)esp);'
            return '{ u32 v = POP16(); %s }' % self.wr(ins, o, 'v')
        if mn in ('pushaw', 'pusha'):
            return ('{ u32 t = esp; PUSH16(eax); PUSH16(ecx); PUSH16(edx); PUSH16(ebx); PUSH16(t);'
                    ' PUSH16(ebp); PUSH16(esi); PUSH16(edi); }')
        if mn in ('popaw', 'popa'):
            return ('{ edi = (edi & 0xffff0000u) | POP16(); esi = (esi & 0xffff0000u) | POP16(); ebp = (ebp & 0xffff0000u) | POP16();'
                    ' (void)POP16(); ebx = (ebx & 0xffff0000u) | POP16(); edx = (edx & 0xffff0000u) | POP16();'
                    ' ecx = (ecx & 0xffff0000u) | POP16(); eax = (eax & 0xffff0000u) | POP16(); }')
        if mn in ('pushal', 'pushad'):
            return ('{ u32 t = esp; PUSH32(eax); PUSH32(ecx); PUSH32(edx); PUSH32(ebx); PUSH32(t);'
                    ' PUSH32(ebp); PUSH32(esi); PUSH32(edi); }')
        if mn in ('popal', 'popad'):
            return ('{ edi = POP32(); esi = POP32(); ebp = POP32(); (void)POP32(); ebx = POP32(); edx = POP32();'
                    ' ecx = POP32(); eax = POP32(); }')
        if mn == 'pushf':
            return 'PUSH16(PACK_FLAGS());'
        if mn == 'pushfd':
            return 'PUSH32(PACK_FLAGS());'
        if mn == 'popf':
            return '{ u32 v = POP16(); UNPACK_FLAGS(v); }'
        if mn == 'popfd':
            return '{ u32 v = POP32(); UNPACK_FLAGS(v); }'
        if mn == 'leave':
            return 'esp = (u16)ebp; ebp = (ebp & 0xffff0000u) | POP16();'
        if mn == 'enter':
            sz = ops[0].imm
            if ops[1].imm != 0:
                return 'rt_unimpl(0x%x, "enter level");' % a
            return 'PUSH16(ebp); ebp = (ebp & 0xffff0000u) | (u16)esp; esp = (u16)(esp - %d);' % sz
        if mn == 'xlatb':
            sn = ins.reg_name(ops[0].mem.segment) if ops and ops[0].mem.segment else 'ds'
            return 'eax = (eax & 0xffffff00u) | RD8(%s + (u16)(ebx + (u8)eax));' % self.segbase(sn)
        if mn in ('lds', 'les', 'lss', 'lfs', 'lgs'):
            seg = mn[1:]
            ad = self.addr(ins, ops[1].mem)
            if ops[0].size == 4:
                return '{ u32 ad = %s; %s %s }' % (ad, self.wr(ins, ops[0], 'RD32(ad)'), self.reg_wr(seg, 'RD16(ad + 4)'))
            return '{ u32 ad = %s; u32 o = RD16(ad); %s %s }' % (ad, self.reg_wr(seg, 'RD16(ad + 2)'), self.wr(ins, ops[0], 'o'))
        if mn == 'int':
            n = ops[0].imm & 0xff
            return 'SYNC_OUT; rt_int(0x%x, 0x%x); SYNC_IN;' % (n, a)
        if mn in ('cwde',) and False:
            pass
        m2 = mn
        for pre in ('rep ', 'repe ', 'repne '):
            if m2.startswith(pre):
                m2 = m2[len(pre):]
        if m2 in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                  'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd', 'insb', 'insw', 'outsb', 'outsw'):
            return self.string16(ins, m2)
        if mn in ('bt', 'bts', 'btr', 'btc') and ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
            return 'rt_unimpl(0x%x, "bt mem,reg");' % a
        return super().insn(f, ins, live_after, next_addr)

    def string16(self, ins, m):
        p0 = 0
        a32 = False
        for bt in ins.bytes:
            if bt in (0xf2, 0xf3):
                p0 = bt
            elif bt == 0x67:
                a32 = True
            elif bt not in (0xf0, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0x66):
                break
        base = m[:-1]
        sz = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
        bits = sz * 8
        if a32:
            return 'rt_unimpl(0x%x, "a32 string op");' % ins.address
        sn = 'ds'
        for op in ins.operands:
            if op.type == X.X86_OP_MEM and op.mem.segment and op.mem.base in (X.X86_REG_SI, X.X86_REG_ESI):
                sn = ins.reg_name(op.mem.segment)
        src = '%s + (u16)esi' % self.segbase(sn)
        dst = 'cpu.sbase[0] + (u16)edi'
        step = 'DSTEP(%d)' % sz
        isi = 'esi = (esi & 0xffff0000u) | (u16)(esi + %s);' % step
        idi = 'edi = (edi & 0xffff0000u) | (u16)(edi + %s);' % step
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        rep = p0 != 0
        if base == 'movs':
            if rep:
                return 'REP_MOVS16(%d, %s);' % (sz, self.segbase(sn))
            body = 'WR%d(%s, RD%d(%s)); %s %s' % (bits, dst, bits, src, isi, idi)
        elif base == 'stos':
            if rep:
                return 'REP_STOS16(%d);' % sz
            body = 'WR%d(%s, %s); %s' % (bits, dst, self.reg_rd(acc), idi)
        elif base == 'lods':
            body = '%s %s' % (self.reg_wr(acc, 'RD%d(%s)' % (bits, src)), isi)
        elif base == 'scas':
            body = ('{ u32 a = %s, b = RD%d(%s); u32 r = (a - b) & %s; cf = a < b; zf = r == 0; sf = (r & %s) != 0; '
                    'of = (((a ^ b) & (a ^ r) & %s) != 0); pf = PARITY(r); af = (((a ^ b ^ r) & 0x10) != 0); %s }'
                    % (self.reg_rd(acc), bits, dst, MASK[sz], SBIT[sz], SBIT[sz], idi))
        elif base == 'cmps':
            body = ('{ u32 a = RD%d(%s), b = RD%d(%s); u32 r = (a - b) & %s; cf = a < b; zf = r == 0; sf = (r & %s) != 0; '
                    'of = (((a ^ b) & (a ^ r) & %s) != 0); pf = PARITY(r); af = (((a ^ b ^ r) & 0x10) != 0); %s %s }'
                    % (bits, src, bits, dst, MASK[sz], SBIT[sz], SBIT[sz], isi, idi))
        else:
            return 'rt_unimpl(0x%x, "%s");' % (ins.address, m)
        if not rep:
            return body
        cx = '(u16)ecx'
        dec = 'ecx = (ecx & 0xffff0000u) | (u16)(ecx - 1);'
        if base in ('scas', 'cmps'):
            stop = 'if (!zf) break;' if p0 == 0xf3 else 'if (zf) break;'
            return 'while (%s) { %s %s %s }' % (cx, body, dec, stop)
        return 'while (%s) { %s %s }' % (cx, body, dec)

    # ---------- calls ----------
    def api_call(self, idx):
        mod, o, name = self.p.apis[idx]
        return 'api_%s_%d' % (mod, o)

    def fname16(self, t):
        if t in self.native:
            return self.native[t]
        if t in self.funcs:
            return fn_name(t)
        return None

    def call_code(self, t, ret_off, far):
        push = ('PUSH16(0x%xu); ' % SEL(self.seg) if far else '') + 'PUSH16(0x%xu); ' % ret_off
        name = self.fname16(t)
        if name:
            return push + 'SYNC_OUT; %s(); SYNC_IN;' % name
        return push + 'SYNC_OUT; rt_call_far(0x%xu, 0x%xu, 0x%xu); SYNC_IN;' % (SEL(t >> 16), t & 0xffff, 0)

    # ---------- flags across calls ----------
    def callee_of(self, ins):
        m = ins.mnemonic
        if m == 'call' and ins.operands[0].type == X.X86_OP_IMM:
            return target(ins)
        if m == 'lcall' and ins.operands[0].type == X.X86_OP_IMM:
            ft = far_target(self.p, ins)
            if ft and ft[0] == 'code':
                return ft[1]
        return None

    def interproc(self):
        p = self.p
        for it in range(8):
            changed = False
            for e, f in self.funcs.items():
                la = self.liveness(f)
                for b, l in f.blocks.items():
                    for a in l:
                        ins = p.insn(a)
                        t = self.callee_of(ins)
                        if t is None or t not in self.funcs:
                            continue
                        need = self.live_at(f, la, a + ins.size)
                        if need & ~self.rlive[t]:
                            self.rlive[t] |= need
                            changed = True
            if not changed:
                break

    def gen_func(self, f):
        p = self.p
        self.seg = f.entry >> 16
        la = self.liveness(f)
        labels = set()
        out = ['void %s(void)' % fn_name(f.entry), '{', '    LOCALS;']
        order = sorted(f.blocks)
        if order[0] != f.entry:
            out.append('    goto L_%x;' % f.entry)
        for b in order:
            l = f.blocks[b]
            out.append('L_%x:' % b)
            if not l:
                out.append('    rt_bad_code(0x%x); SYNC_OUT; return;' % b)
                continue
            for a in l:
                ins = p.insn(a)
                m = ins.mnemonic
                nxt = a + ins.size
                ro = nxt & 0xffff
                L = la.get(a, ALL)
                cmt = ' /* %x:%04x %s %s */' % (a >> 16, a & 0xffff, m, ins.op_str.replace('*/', '* /'))
                if a in TRACE16:
                    out.append('    SYNC_OUT; rt_trace_at(0x%xu);' % a)
                for pc in PATCH16.get(a, ()):
                    out.append('    %s /* patch16 */' % pc)
                if m in JCC:
                    t = target(ins)
                    if m == 'jcxz':
                        c = '!(u16)ecx'
                    elif m == 'jecxz':
                        c = '!ecx'
                    elif m.startswith('loop'):
                        out.append('    ecx = (ecx & 0xffff0000u) | (u16)(ecx - 1);')
                        c = {'loop': '(u16)ecx != 0', 'loope': '((u16)ecx != 0 && zf)', 'loopne': '((u16)ecx != 0 && !zf)'}[m]
                    else:
                        c = cond_of(m, 'j')[0]
                    tj = ('POLL; ' if t <= a else '') + ('goto L_%x;' % t if t in f.blocks else
                                                        'SYNC_OUT; rt_jump(0x%xu, 0x%x); return;' % (t, a))
                    out.append('    if (%s) { %s }%s' % (c, tj, cmt))
                    out.append('    goto L_%x;' % nxt if nxt in f.blocks else '    rt_fell_off(0x%x); SYNC_OUT; return;' % a)
                    continue
                if m == 'jmp':
                    op = ins.operands[0]
                    if a in f.tailcalls:
                        out.append('    SYNC_OUT; %s(); return; /* tail */%s' % (self.fname16(f.tailcalls[a]), cmt))
                    elif op.type == X.X86_OP_IMM:
                        t = target(ins)
                        out.append('    %s%s' % ('POLL; ' if t <= a else '', 'goto L_%x;' % t) + cmt)
                    elif a in f.tables:
                        out.append('    { u32 t = %s; switch (t) {%s' % (self.rd(ins, op, 2), cmt))
                        for t in sorted(set(f.tables[a])):
                            out.append('    case 0x%xu: goto L_%x;' % (t & 0xffff, t))
                        out.append('    default: SYNC_OUT; rt_jump(0x%xu | t, 0x%x); return; } }' % (self.seg << 16, a))
                    else:
                        out.append('    { u32 t = %s; SYNC_OUT; rt_jump(0x%xu | t, 0x%x); return; }%s' % (self.rd(ins, op, 2), self.seg << 16, a, cmt))
                    continue
                if m == 'ljmp':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        ft = far_target(p, ins)
                        if ft and ft[0] == 'code' and self.fname16(ft[1]):
                            out.append('    SYNC_OUT; %s(); return; /* far tail */%s' % (self.fname16(ft[1]), cmt))
                        elif ft and ft[0] == 'api':
                            out.append('    SYNC_OUT; %s(); return; /* api tail */%s' % (self.api_call(ft[1]), cmt))
                        else:
                            out.append('    SYNC_OUT; rt_jump_far(0x%xu, 0x%xu, 0x%x); return;%s' % (op.imm & 0xffff, ins.operands[1].imm & 0xffff, a, cmt))
                    else:
                        ad = self.addr(ins, op.mem)
                        out.append('    { u32 ad = %s; SYNC_OUT; rt_jump_far(RD16(ad + 2), RD16(ad), 0x%x); return; }%s' % (ad, a, cmt))
                    continue
                if m in ('ret', 'retf', 'iret'):
                    n = ins.operands[0].imm if ins.operands else 0
                    k = {'ret': 2, 'retf': 4, 'iret': 6}[m] + n
                    out.append('    esp = (u16)(esp + %d); SYNC_OUT; return;%s' % (k, cmt))
                    continue
                if m == 'call':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        out.append('    ' + self.call_code(target(ins), ro, False) + cmt)
                    else:
                        out.append('    { u32 t = %s; PUSH16(0x%xu); SYNC_OUT; rt_call_far(0x%xu, t, 0x%x); SYNC_IN; }%s'
                                   % (self.rd(ins, op, 2), ro, SEL(self.seg), a, cmt))
                    continue
                if m == 'lcall':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        ft = far_target(p, ins)
                        if ft and ft[0] == 'api':
                            out.append('    PUSH16(0x%xu); PUSH16(0x%xu); SYNC_OUT; %s(); SYNC_IN;%s'
                                       % (SEL(self.seg), ro, self.api_call(ft[1]), cmt))
                            if p.apis[ft[1]][2] in NORET_API:
                                out.append('    rt_noreturn(0x%x); return;' % a)
                        elif ft:
                            out.append('    ' + self.call_code(ft[1], ro, True) + cmt)
                        else:
                            out.append('    PUSH16(0x%xu); PUSH16(0x%xu); SYNC_OUT; rt_call_far(0x%xu, 0x%xu, 0x%x); SYNC_IN;%s'
                                       % (SEL(self.seg), ro, op.imm & 0xffff, ins.operands[1].imm & 0xffff, a, cmt))
                    else:
                        ad = self.addr(ins, op.mem)
                        out.append('    { u32 ad = %s; u32 o = RD16(ad), s = RD16(ad + 2); PUSH16(0x%xu); PUSH16(0x%xu); SYNC_OUT; rt_call_far(s, o, 0x%x); SYNC_IN; }%s'
                                   % (ad, SEL(self.seg), ro, a, cmt))
                    continue
                if m in ('hlt', 'ud2'):
                    out.append('    rt_unimpl(0x%x, "%s");' % (a, m))
                    continue
                try:
                    c = self.insn(f, ins, L, nxt)
                except Exception as ex:
                    c = 'rt_unimpl(0x%x, "gen error %s: %s");' % (a, ins.mnemonic, str(ex).replace('"', "'"))
                    self.unimpl['generr ' + m] += 1
                out.append('    %s%s' % (c, cmt))
            last = p.insn(l[-1])
            lm = last.mnemonic
            ended = lm in JCC or lm in ('jmp', 'ljmp', 'ret', 'retf', 'iret')
            if lm == 'lcall' and last.operands[0].type == X.X86_OP_IMM:
                ft = far_target(p, last)
                if ft and ft[0] == 'api' and p.apis[ft[1]][2] in NORET_API:
                    ended = True
            if not ended:
                s = f.succ.get(b, [])
                if s and s[0] in f.blocks:
                    out.append('    goto L_%x;' % s[0])
                else:
                    out.append('    rt_fell_off(0x%x); SYNC_OUT; return;' % l[-1])
        out.append('}')
        return out


MOD = 'rc'


def fn_name(a):
    return '%s_%02x_%04x' % (MOD, a >> 16, a & 0xffff)


def main():
    global MOD
    exe, gfuncs, native_txt, outdir, mod = sys.argv[1:6]
    MOD = mod
    p = Prog16(exe)
    n = p.ne
    entries = [(n.cs << 16) | n.ip]
    entries += [(s << 16) | o for s, o, fl in n.entries.values()]
    if os.path.exists(gfuncs):
        for l in open(gfuncs):
            w = l.split('\t')[0]
            if ':' in w:
                sg, of = w.split(':')
                entries.append(((((int(sg, 16) - 0x1000) >> 3) + 1) << 16) | int(of, 16))
    # far code pointers stored in data segments (CRT init tables, callbacks):
    # a SEGMENT fixup with the offset word before it; only into code segments
    # that hold functions (segment 17 is far constant data in a code segment)
    fseg = set(e >> 16 for e in entries)
    for s in n.segs:
        if not s.is_data:
            continue
        for st, add, offs, t in s.relocs:
            if t[0] not in ('int', 'mov') or st not in (NEmod.SRC_SEGMENT, NEmod.SRC_FARPTR):
                continue
            tg = n.target(t)
            for o in offs:
                if st == NEmod.SRC_FARPTR:
                    ts, to = tg
                elif o >= 2:
                    ts, to = tg[0], struct.unpack_from('<H', s.data, o - 2)[0]
                else:
                    continue
                if ts in fseg and p.in_code((ts << 16) | to):
                    entries.append((ts << 16) | to)
    # far function pointers built in code (window procedures, callbacks):
    # "mov ax, seg F" (SEGMENT fixup) near an immediate offset whose target
    # starts with a far-function prologue
    PRO = (b'\x8c\xd8\x90\x45', b'\x1e\x58\x90\x45', b'\x45\x55\x8b\xec', b'\x8c\xd0\x90\x45')
    for s in n.segs:
        if s.is_data:
            continue
        for st, add, offs, t in s.relocs:
            if t[0] not in ('int', 'mov') or st != NEmod.SRC_SEGMENT:
                continue
            ts = n.target(t)[0]
            if ts not in fseg:
                continue
            tdata = n.segs[ts - 1].data
            for o in offs:
                for k in range(max(0, o - 16), min(len(s.data) - 1, o + 16)):
                    v = struct.unpack_from('<H', s.data, k)[0]
                    if v + 4 > len(tdata):
                        continue
                    ok = tdata[v:v + 4] in PRO
                    # "enter n, 0" right after a far / near return
                    if not ok and tdata[v] == 0xc8 and tdata[v + 3] == 0 and v >= 3 and (
                            tdata[v - 1] in (0xcb, 0xc3, 0x90) or tdata[v - 3] in (0xca, 0xc2)):
                        ok = True
                    if ok:
                        entries.append((ts << 16) | v)
    xf = native_txt.replace('native_', 'extra_')
    if os.path.exists(xf):
        for l in open(xf):
            l = l.split('#')[0].strip()
            if l:
                sg, of = l.split(':')
                entries.append((int(sg, 16) << 16) | int(of, 16))
    native = {}
    if os.path.exists(native_txt):
        for line in open(native_txt):
            line = line.split('#')[0].strip()
            if line:
                w = line.split()
                sg, of = w[0].split(':')
                native[(int(sg, 16) << 16) | int(of, 16)] = w[1]
    funcs = analyze(p, entries, NORET_API)
    for a in native:
        funcs.pop(a, None)
    g = Gen16(p, funcs, native)
    g.interproc()
    os.makedirs(outdir, exist_ok=True)
    files = []
    groups = collections.OrderedDict()
    for e in sorted(funcs):
        groups.setdefault((e >> 16, (e & 0xffff) >> 13), []).append(e)
    for (sg, part), chunk in groups.items():
        fn = os.path.join(outdir, 'gen_%02x_%d.c' % (sg, part))
        body = []
        for e in chunk:
            body += g.gen_func(funcs[e])
            body.append('')
        btxt = '\n'.join(body)
        names = sorted(set(re.findall(r'\b(' + MOD + r'_[0-9a-f]{2}_[0-9a-f]{4}|api_\w+)\(\);', btxt)) |
                       set(nm for nm in native.values() if (nm + '();') in btxt))
        lines = ['/* generated by tools/recomp16.py - do not edit */', '#include "cpu.h"', '']
        lines += ['void %s(void);' % nm for nm in names]
        txt = '\n'.join(lines) + '\n\n' + btxt + '\n'
        if not os.path.exists(fn) or open(fn).read() != txt:
            open(fn, 'w').write(txt)
        files.append(fn)
    for fn in os.listdir(outdir):
        full = os.path.join(outdir, fn)
        if fn.startswith('gen_') and fn.endswith('.c') and full not in files:
            os.remove(full)
    # function table, API table, segment images
    tab = ['/* generated */', '#include "cpu.h"']
    allf = sorted(set(funcs) | set(native))
    for e in allf:
        tab.append('void %s(void);' % native.get(e, fn_name(e)))
    for i, (m, o, nm) in enumerate(p.apis):
        tab.append('void api_%s_%d(void); /* %s */' % (m, o, nm))
    tab.append('const FuncEnt %s_functab[] = {' % mod)
    for e in allf:
        tab.append('    {0x%xu, %s},' % (e, native.get(e, fn_name(e))))
    tab.append('};')
    tab.append('const int %s_nfuncs = %d;' % (mod, len(allf)))
    tab.append('const ApiEnt %s_apitab[] = {' % mod)
    for i, (m, o, nm) in enumerate(p.apis):
        tab.append('    {"%s", %d, "%s", api_%s_%d},' % (m, o, nm, m, o))
    tab.append('};')
    tab.append('const int %s_napis = %d;' % (mod, len(p.apis)))
    open(os.path.join(outdir, 'functab.c'), 'w').write('\n'.join(tab) + '\n')
    # relocated segment images for the runtime loader
    with open(os.path.join(outdir, 'segs.bin'), 'wb') as fo:
        fo.write(struct.pack('<HHHHHH', len(n.segs), n.autodata, n.cs, n.ip, n.stack, n.heap))
        for s in n.segs:
            b = bytes(p.img[s.idx][:len(s.data)])
            fo.write(struct.pack('<HHII', s.idx, s.flags, s.alloc if s.alloc else 0x10000, len(b)))
            fo.write(b)
    # api list for writing natives
    with open(os.path.join(outdir, 'apis.txt'), 'w') as fo:
        for m, o, nm in p.apis:
            fo.write('%s %d %s %s\n' % (m, o, nm, ' '.join(winspec.lookup(m, o)[1] or [])))
    print('functions', len(funcs), 'native', len(native), 'files', len(files), 'apis', len(p.apis))
    print('unimplemented:', dict(g.unimpl.most_common()))


if __name__ == '__main__':
    main()
