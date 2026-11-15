/* PC native runtime - entry point and core (Win16).
 *
 * Robot City's ROBOTCTY.EXE (16-bit Windows 3.1, NE) is recompiled to C
 * (src/gen/rc).  Here: guest memory and selectors, the NE loader (code and
 * data segments come from the player's CD copy at run time, relocations
 * applied like the Windows loader does), calls between recompiled code and
 * the native API replacements (kernel.c, user.c, gdi.c, mmsys.c).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

CPU cpu;
u8 *MEM;
u8 rt_parity_tab[256];
FILE *rt_logf;
char rt_root[512];
char rt_exe_dir[512];
volatile int rt_irq_pending;
u32 rt_tail_ret;
u32 rt_hr_n = 1, rt_hr_k = 0, rt_hr_rowtab;
u8 vga_planes[4][65536];
int vga_rd_plane;
u8 vga_wr_mask;
u8 *vga_capture;
u32 rt_seg_base[64];
u32 sel_base[NSEL], sel_limit[NSEL];
u8 sel_used[NSEL];
u16 g_dgroup, g_hinst, g_psp;

extern const FuncEnt rc_functab[];
extern const int rc_nfuncs;
extern const ApiEnt rc_apitab[];
extern const int rc_napis;

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (rt_logf) { vfprintf(rt_logf, fmt, ap); fflush(rt_logf); }
    va_end(ap);
}

int rt_closing;
void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s\n", buf);
    rt_log("regs: ax=%04x cx=%04x dx=%04x bx=%04x sp=%04x bp=%04x si=%04x di=%04x ds=%04x es=%04x ss=%04x\n",
           cpu.r[0] & 0xffff, cpu.r[1] & 0xffff, cpu.r[2] & 0xffff, cpu.r[3] & 0xffff, cpu.r[4] & 0xffff,
           cpu.r[5] & 0xffff, cpu.r[6] & 0xffff, cpu.r[7] & 0xffff, cpu.sreg[S_DS], cpu.sreg[S_ES], cpu.sreg[S_SS]);
    for (int i = 0; i < 16; i++) rt_log(" %04x", RD16(STK(i * 2)));
    rt_log("\n");
    if (rt_closing) { if (rt_logf) fflush(rt_logf); TerminateProcess(GetCurrentProcess(), 0); }
    if (!getenv(ENV("HIDDEN"))) MessageBoxA(NULL, buf, GAME_TITLE, MB_ICONERROR);
    if (rt_logf) fflush(rt_logf);
    TerminateProcess(GetCurrentProcess(), 1);
}

void rt_bad_code(u32 at) { rt_fatal("execution reached undecodable code at %x:%04x", at >> 16, at & 0xffff); }
void rt_fell_off(u32 at) { rt_fatal("fell off the end of a block at %x:%04x", at >> 16, at & 0xffff); }
void rt_noreturn(u32 at) { rt_fatal("noreturn call returned at %x:%04x", at >> 16, at & 0xffff); }
void rt_divide_error(u32 at) { rt_fatal("divide error at %x:%04x", at >> 16, at & 0xffff); }
u32 rt_in8(u32 port, u32 at) { rt_log("in port %x at %x\n", port, at); return 0xff; }
u32 rt_in16(u32 port, u32 at) { return rt_in8(port, at) | 0xff00; }
u32 rt_in32(u32 port, u32 at) { return rt_in8(port, at) | 0xffffff00; }
void rt_out8(u32 port, u32 v, u32 at) { rt_log("out port %x,%x at %x\n", port, v, at); }
void rt_out16(u32 port, u32 v, u32 at) { rt_out8(port, v, at); }
void rt_out32(u32 port, u32 v, u32 at) { rt_out8(port, v, at); }
void rt_trace_enter(u32 f) { (void)f; }
void rt_trace_leave(u32 f) { (void)f; }
void rt_magic_chain(u32 ra) { rt_fatal("retf to %08x", ra); }
void rt_call(u32 t, u32 at) { rt_fatal("rt_call %x at %x", t, at); }
void irq_poll(void) { rt_irq_pending = 0; }

static int unimpl_count;
void rt_unimpl(u32 at, const char *what)
{
    if (unimpl_count++ < 200) rt_log("unimplemented instruction at %x:%04x: %s\n", at >> 16, at & 0xffff, what);
}

/* ---- selectors ---- */
void sel_set(u16 sel, u32 base, u32 limit)
{
    int i = SEL_IDX(sel);
    sel_base[i] = base;
    sel_limit[i] = limit;
    sel_used[i] = 1;
}

u16 sel_alloc(int n)
{
    for (int i = 32; i + n <= NSEL; i++) {
        int k = 0;
        while (k < n && !sel_used[i + k]) k++;
        if (k == n) {
            for (k = 0; k < n; k++) sel_used[i + k] = 1;
            return MKSEL(i);
        }
        i += k;
    }
    rt_fatal("out of selectors");
    return 0;
}

void sel_free(u16 sel) { sel_used[SEL_IDX(sel)] = 0; }

void rt_load_seg(int idx, u16 sel)
{
    cpu.sreg[idx] = sel;
    cpu.sbase[idx] = sel_base[SEL_IDX(sel)];
}

u32 rt_sel_limit(u16 sel) { return sel_limit[SEL_IDX(sel)]; }

/* ---- x87 ---- */
long double rt_fround(long double v)
{
    switch ((cpu.fcw >> 10) & 3) {
    case 0: return nearbyintl(v);
    case 1: return floorl(v);
    case 2: return ceill(v);
    default: return truncl(v);
    }
}

s64 rt_fist(long double v, int bits)
{
    long double r = rt_fround(v);
    long double lim = bits == 16 ? 32768.0L : bits == 32 ? 2147483648.0L : 9223372036854775808.0L;
    if (!(r >= -lim && r < lim)) return bits == 16 ? (s64)(s16)0x8000 : bits == 32 ? (s64)(s32)0x80000000 : INT64_MIN;
    return (s64)r;
}

void rt_fpu_save(u32 a)
{
    /* 16-bit real/protected format: 14-byte environment + 80 bytes */
    memset(MEM + a, 0, 94);
    WR16(a, cpu.fcw);
    WR16(a + 2, FPU_SW());
    for (int i = 0; i < 8; i++) WRF80(a + 14 + i * 10, ST(i));
    FPU_INIT();
}

void rt_fpu_restore(u32 a)
{
    cpu.fcw = RD16(a);
    u16 sw = RD16(a + 2);
    cpu.fsw = sw & ~0x3800;
    cpu.ftop = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) ST(i) = RDF80(a + 14 + i * 10);
}

/* ---- rep string ops (16-bit offsets wrap inside the segment) ---- */
void rt_rep_movs16(int sz, u32 sb)
{
    u32 n = (u16)cpu.r[R_ECX];
    u32 si = (u16)cpu.r[R_ESI], di = (u16)cpu.r[R_EDI];
    u32 db = cpu.sbase[S_ES];
    if (!cpu.df && si + n * sz <= 0x10000 && di + n * sz <= 0x10000) {
        u32 s = sb + si, d = db + di, bytes = n * sz;
        if (d - s >= bytes || d < s) memmove(MEM + d, MEM + s, bytes);
        else for (u32 k = 0; k < bytes; k++) MEM[d + k] = MEM[s + k];
        si += bytes; di += bytes;
    } else {
        int st = cpu.df ? -sz : sz;
        while (n--) {
            memcpy(MEM + db + di, MEM + sb + si, sz);
            si = (u16)(si + st); di = (u16)(di + st);
        }
    }
    cpu.r[R_ECX] &= 0xffff0000u;
    cpu.r[R_ESI] = (cpu.r[R_ESI] & 0xffff0000u) | (u16)si;
    cpu.r[R_EDI] = (cpu.r[R_EDI] & 0xffff0000u) | (u16)di;
}

void rt_rep_stos16(int sz)
{
    u32 n = (u16)cpu.r[R_ECX];
    u32 di = (u16)cpu.r[R_EDI];
    u32 db = cpu.sbase[S_ES];
    u32 v = cpu.r[R_EAX];
    if (!cpu.df && di + n * sz <= 0x10000 && sz == 1) {
        memset(MEM + db + di, (u8)v, n);
        di += n;
    } else {
        int st = cpu.df ? -sz : sz;
        while (n--) {
            memcpy(MEM + db + di, &v, sz);
            di = (u16)(di + st);
        }
    }
    cpu.r[R_ECX] &= 0xffff0000u;
    cpu.r[R_EDI] = (cpu.r[R_EDI] & 0xffff0000u) | (u16)di;
}

/* ---- code lookup and dispatch ---- */
void (*rt_lookup(u32 addr))(void)
{
    int lo = 0, hi = rc_nfuncs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (rc_functab[m].addr == addr) return rc_functab[m].fn;
        if (rc_functab[m].addr < addr) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

static int api_trace;
void rt_call_far(u32 sel, u32 off, u32 at)
{
    sel &= 0xffff; off &= 0xffff;
    if (sel == API_SEL) {
        if ((int)off < rc_napis) {
            if (api_trace) rt_log("api %s.%s (indirect)\n", rc_apitab[off].mod, rc_apitab[off].name);
            rc_apitab[off].fn();
            return;
        }
        rt_fatal("bad api index %x at %x", off, at);
    }
    int idx = SEL_IDX(sel);
    if (idx >= 1 && idx < 64 && rt_seg_base[idx]) {
        void (*f)(void) = rt_lookup((u32)idx << 16 | off);
        if (f) { f(); return; }
    }
    rt_fatal("call to unknown code %04x:%04x from %x:%04x (add %x:%04x to tools/extra_rc.txt)", sel, off, at >> 16, at & 0xffff, idx, off);
}

void rt_jump(u32 t, u32 at)
{
    /* computed jump: tail-call a known function, else stop */
    void (*f)(void) = rt_lookup(t);
    if (f) { f(); return; }
    rt_fatal("jump to unknown code %x:%04x from %x:%04x", t >> 16, t & 0xffff, at >> 16, at & 0xffff);
}

void rt_jump_far(u32 sel, u32 off, u32 at)
{
    /* a far jump to a function entry is a tail call; to anything else it is
       the CRT's "return through a saved address" (_setargv / _setenvp move
       the stack and jump back to the caller): the C caller continues there */
    sel &= 0xffff; off &= 0xffff;
    if (sel == MAGIC_SEL) return;
    int idx = SEL_IDX(sel);
    if (sel == API_SEL || (idx >= 1 && idx < 64 && rt_lookup((u32)idx << 16 | off))) { rt_call_far(sel, off, at); return; }
    static int n;
    if (n++ < 20) rt_log("far jump to %04x:%04x at %x:%04x taken as a return\n", sel, off, at >> 16, at & 0xffff);
}

/* calls from native code into the guest (window procedures, callbacks):
   pascal, words pushed in the order given, then a far return address */
static int guest_depth;
u32 guest_call16(u32 fp, int nwords, const u16 *words)
{
    u32 sp = cpu.r[R_ESP];
    u16 ds = cpu.sreg[S_DS], es = cpu.sreg[S_ES];
    u32 bp = cpu.r[R_EBP], si = cpu.r[R_ESI], di = cpu.r[R_EDI], bx = cpu.r[R_EBX], cx = cpu.r[R_ECX];
    for (int i = 0; i < nwords; i++) { cpu.r[R_ESP] = (u16)(cpu.r[R_ESP] - 2); WR16(STK(0), words[i]); }
    cpu.r[R_ESP] = (u16)(cpu.r[R_ESP] - 2); WR16(STK(0), MAGIC_SEL);
    cpu.r[R_ESP] = (u16)(cpu.r[R_ESP] - 2); WR16(STK(0), 0xfff0);
    /* exported-function prologues load DS from AX (mov ax, ds / mov ds, ax style) */
    cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | g_dgroup;
    rt_load_seg(S_DS, g_dgroup);
    guest_depth++;
    rt_call_far(fp >> 16, fp & 0xffff, 0);
    guest_depth--;
    u32 r = (cpu.r[R_EAX] & 0xffff) | ((cpu.r[R_EDX] & 0xffff) << 16);
    if ((u16)cpu.r[R_ESP] != (u16)sp) {
        static int n;
        if (n++ < 10) rt_log("guest_call16 %04x:%04x: sp %04x after, %04x before\n", fp >> 16, fp & 0xffff, cpu.r[R_ESP] & 0xffff, sp & 0xffff);
        cpu.r[R_ESP] = sp;
    }
    rt_load_seg(S_DS, ds);
    rt_load_seg(S_ES, es);
    cpu.r[R_EBP] = bp; cpu.r[R_ESI] = si; cpu.r[R_EDI] = di; cpu.r[R_EBX] = bx; cpu.r[R_ECX] = cx;
    return r;
}

/* ---- interrupts ---- */
void rt_int(u32 n, u32 at)
{
    if (n == 0x21) { dos_int21(); return; }
    if (n == 0x2f && (cpu.r[R_EAX] & 0xffff) == 0x1600) { cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | 0x0a03; return; }
    static int k;
    if (k++ < 20) rt_log("int %02x ax=%04x at %x:%04x ignored\n", n, cpu.r[R_EAX] & 0xffff, at >> 16, at & 0xffff);
}

/* ---- the NE loader ---- */
static u8 *exe;
static size_t exe_len;
static u16 RW(u32 o) { return exe[o] | (exe[o + 1] << 8); }

static int api_find(const char *mod, int ord)
{
    for (int i = 0; i < rc_napis; i++)
        if (rc_apitab[i].ord == ord && !strcmp(rc_apitab[i].mod, mod)) return i;
    return -1;
}

static void pstr(u32 o, char *out)
{
    int n = exe[o];
    memcpy(out, exe + o + 1, n);
    out[n] = 0;
}

typedef struct { u16 seg, off; } Entry;

static void ne_load(const char *host_path)
{
    FILE *f = fopen(host_path, "rb");
    if (!f) rt_fatal("cannot open %s", host_path);
    fseek(f, 0, SEEK_END);
    exe_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    exe = malloc(exe_len);
    if (fread(exe, 1, exe_len, f) != exe_len) rt_fatal("cannot read %s", host_path);
    fclose(f);
    u32 ne = RW(0x3c);
    if (exe[ne] != 'N' || exe[ne + 1] != 'E') rt_fatal("%s is not a 16-bit Windows program", host_path);
    int nseg = RW(ne + 0x1c), nmod = RW(ne + 0x1e);
    u32 segtab = ne + RW(ne + 0x22), modtab = ne + RW(ne + 0x28), imptab = ne + RW(ne + 0x2a);
    u32 entry = ne + RW(ne + 0x04), entry_len = RW(ne + 0x06);
    int align = RW(ne + 0x32) ? RW(ne + 0x32) : 9;
    int autodata = RW(ne + 0x0e);
    u16 heap = RW(ne + 0x10), stack = RW(ne + 0x12);
    char mods[16][16];
    for (int i = 0; i < nmod && i < 16; i++) pstr(imptab + RW(modtab + 2 * i), mods[i]);
    /* entry table (movable references) */
    static Entry ents[512];
    {
        u32 p = entry;
        int ord = 1;
        while (p < entry + entry_len) {
            int cnt = exe[p], ind = exe[p + 1];
            p += 2;
            if (!cnt) break;
            for (int k = 0; k < cnt; k++, ord++) {
                if (ind == 0) continue;
                if (ind == 0xff) { if (ord < 512) { ents[ord].seg = exe[p + 3]; ents[ord].off = RW(p + 4); } p += 6; }
                else { if (ord < 512) { ents[ord].seg = ind; ents[ord].off = RW(p + 1); } p += 3; }
            }
        }
    }
    /* segments */
    for (int i = 1; i <= nseg; i++) {
        u32 so = segtab + 8 * (i - 1);
        u32 off = (u32)RW(so) << align, flen = RW(so + 2), flags = RW(so + 4), alloc = RW(so + 6);
        if (off && !flen) flen = 0x10000;
        if (!alloc) alloc = off ? flen : 0x10000;
        if (alloc < flen) alloc = flen;
        u32 lin = SEG_LIN(i);
        u32 size = alloc;
        if (i == autodata) size = alloc + stack + heap;
        if (size > 0x10000) size = 0x10000;
        if (off) memcpy(MEM + lin, exe + off, flen);
        rt_seg_base[i] = lin;
        sel_set(MKSEL(i), lin, size - 1);
        if (!(flags & 1)) continue;
        (void)flags;
    }
    for (int i = 1; i <= nseg; i++) {
        u32 so = segtab + 8 * (i - 1);
        u32 off = (u32)RW(so) << align, flen = RW(so + 2), flags = RW(so + 4);
        if (off && !flen) flen = 0x10000;
        if (!off || !(flags & 0x100)) continue;
        u32 p = off + flen;
        int n = RW(p);
        p += 2;
        u8 *b = MEM + SEG_LIN(i);
        for (int k = 0; k < n; k++, p += 8) {
            int st = exe[p], fl = exe[p + 1];
            u16 so2 = RW(p + 2), a = RW(p + 4), c = RW(p + 6);
            int typ = fl & 3, add = fl & 4;
            u16 vlo = 0, vhi = 0;
            int has_hi = 0;
            if (typ == 3) continue;   /* OS fixup: the code is real x87 code, these would make it emulator calls */
            if (typ == 0) {
                int ts = a, to = c;
                if (a == 0xff) { ts = ents[c].seg; to = ents[c].off; }
                if (st == 2) vlo = MKSEL(ts);
                else if (st == 3) { vlo = to; vhi = MKSEL(ts); has_hi = 1; }
                else vlo = to;
            } else if (typ == 1 || typ == 2) {
                char nm[64];
                const char *mod = mods[a - 1];
                int ord = c;
                if (typ == 2) { pstr(imptab + c, nm); ord = -1; rt_log("import by name %s.%s unsupported\n", mod, nm); }
                if (!strcmp(mod, "KERNEL") && (ord == 113 || ord == 114 || ord == 178)) {
                    vlo = ord == 113 ? 3 : ord == 114 ? 8 : 0x0429;   /* __WINFLAGS: pmode, 486, enhanced, 80x87 */
                    if (st == 3) has_hi = 1;
                } else {
                    int idx = api_find(mod, ord);
                    if (idx < 0) { rt_log("unresolved import %s.%d\n", mod, ord); idx = 0xffff; }
                    if (st == 3) { vlo = idx; vhi = API_SEL; has_hi = 1; }
                    else if (st == 2) vlo = API_SEL;
                    else vlo = idx;
                }
            }
            /* chained (non-additive) or single additive site */
            u32 o = so2;
            for (int guard = 0; guard < 4096; guard++) {
                u16 next = b[o] | (b[o + 1] << 8);
                if (st == 0) { b[o] = add ? b[o] + (u8)vlo : (u8)vlo; break; }
                u16 v = add ? (u16)(next + vlo) : vlo;
                if (st == 2 && 1) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
                else if (st == 3) { b[o] = (u8)vlo; b[o + 1] = (u8)(vlo >> 8); b[o + 2] = (u8)vhi; b[o + 3] = (u8)(vhi >> 8); }
                else { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
                (void)has_hi;
                if (add || next == 0xffff) break;
                o = next;
            }
        }
    }
    g_dgroup = MKSEL(autodata);
    g_hinst = g_dgroup;
    u32 dsz = RW(segtab + 8 * (autodata - 1) + 6);
    /* instance data at DS:0 (pStackTop / pStackMin / pStackBot) */
    u32 dl = SEG_LIN(autodata);
    u32 sp = dsz + stack;
    if (sp > 0xfffe) sp = 0xfffe;
    WR16(dl + 0x0a, dsz);
    WR16(dl + 0x0c, sp);
    WR16(dl + 0x0e, sp);
    /* PSP + environment */
    u32 env = lowmem_alloc(0x100);
    {
        static const char e[] = "PATH=C:\\WINDOWS\0windir=C:\\WINDOWS\0\0\1\0" GAME_EXE_DOS "\0";
        memcpy(MEM + env, e, sizeof e);
    }
    u16 envsel = sel_alloc(1);
    sel_set(envsel, env, 0xff);
    u32 psp = lowmem_alloc(0x100);
    WR16(psp, 0x20cd);
    WR16(psp + 0x2c, envsel);
    WR8(psp + 0x80, 0);
    WR8(psp + 0x81, 0x0d);
    g_psp = sel_alloc(1);
    sel_set(g_psp, psp, 0xff);
    /* registers at the entry point */
    memset(&cpu, 0, sizeof cpu);
    FPU_INIT();
    rt_load_seg(S_DS, g_dgroup);
    rt_load_seg(S_SS, g_dgroup);
    rt_load_seg(S_ES, g_psp);
    rt_load_seg(S_CS, MKSEL(RW(ne + 0x16)));
    cpu.r[R_ESP] = sp;
    cpu.r[R_EBX] = stack;
    cpu.r[R_ECX] = heap;
    cpu.r[R_EDI] = g_hinst;
    cpu.r[R_ESI] = 0;
    cpu.r[R_EAX] = 0;
    rt_log("NE: %d segments, DGROUP %d (%u bytes + stack %u + heap %u) sp %04x, entry %d:%04x\n",
           nseg, autodata, dsz, stack, heap, sp, RW(ne + 0x16), RW(ne + 0x14));
}

/* test runs (RC_QUIT): end the process even when the game no longer pumps */
static DWORD WINAPI watchdog(void *p)
{
    u32 ms = (u32)(uintptr_t)p;
    Sleep(ms + 3000);
    rt_log("watchdog: no clean exit 3 s after the quit time\n");
    if (rt_logf) fflush(rt_logf);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

int main(int argc, char **argv)
{
    if (getenv(ENV("QUIT"))) CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)atoi(getenv(ENV("QUIT"))), 0, NULL);
    GetModuleFileNameA(NULL, rt_exe_dir, sizeof rt_exe_dir);
    char *s = strrchr(rt_exe_dir, '\\');
    if (s) *s = 0;
    snprintf(rt_root, sizeof rt_root, "%s\\game", rt_exe_dir);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(rt_root, sizeof rt_root, "%s", argv[++i]);
    if (getenv(ENV("ROOT"))) snprintf(rt_root, sizeof rt_root, "%s", getenv(ENV("ROOT")));
    char logp[700];
    snprintf(logp, sizeof logp, "%s\\" GAME_SHORT ".log", rt_exe_dir);
    rt_logf = fopen(logp, "w");
    rt_log(GAME_TITLE " - PC native runtime\nroot: %s\n", rt_root);
    api_trace = getenv(ENV("TRACEAPI")) != NULL;
    { extern int api_log_on; api_log_on = api_trace; }
    if (getenv(ENV("HIDDEN"))) SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    timeBeginPeriod(1);
    MEM = VirtualAlloc(NULL, GUEST_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!MEM) rt_fatal("cannot allocate guest memory");
    for (int i = 0; i < 256; i++) {
        int b = i, p = 1;
        while (b) { p ^= b & 1; b >>= 1; }
        rt_parity_tab[i] = (u8)p;
    }
    sel_used[0] = 1;
    sel_set(API_SEL, 0, 0);
    char exep[700];
    snprintf(exep, sizeof exep, "%s\\hd\\" GAME_EXE_PATH, rt_root);
    ne_load(exep);
    settings_load();
    rt_log("start\n");
    void (*entry)(void) = rt_lookup(((u32)SEL_IDX(cpu.sreg[S_CS]) << 16) | RW(RW(0x3c) + 0x14));
    if (!entry) rt_fatal("no code at the entry point");
    entry();
    rt_log("game returned from its entry point\n");
    return 0;
}

/* tools/trace16.txt: registers at listed instructions */
void rt_trace_at(u32 at)
{
    { void rt_trace_event(void); if (at == 0x6d361) rt_trace_event(); }
    { void rt_probe_cursor(void); if (at == 0x599da) { rt_probe_cursor(); return; } }
    rt_log("@%x:%04x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x ds=%04x es=%04x\n", at >> 16, at & 0xffff,
           cpu.r[0] & 0xffff, cpu.r[3] & 0xffff, cpu.r[1] & 0xffff, cpu.r[2] & 0xffff, cpu.r[6] & 0xffff, cpu.r[7] & 0xffff,
           cpu.r[5] & 0xffff, cpu.sreg[S_DS], cpu.sreg[S_ES]);
}
void rt_trace_event(void)
{
    u32 fp = ((cpu.r[R_EDX] & 0xffff) << 16) | (cpu.r[R_EAX] & 0xffff);
    if (!fp) return;
    u32 a = FAR2LIN(fp);
    rt_log("   event %04x:%04x type %u id %u  caller ret %04x:%04x\n", fp >> 16, fp & 0xffff, RD16(a + 0x18), RD16(a + 0x1c),
           RD16(STK(4)), RD16(STK(2)));
}

/* probe: cursor animation step 5:99da(hotspot far ptr) */
void rt_probe_cursor(void)
{
    u32 off = RD16(STK(4)), sel = RD16(STK(6));
    u32 ds = cpu.sbase[S_DS];
    u32 k = RD8(ds + 0x2f24);
    u32 base = sel_base[SEL_IDX(sel)] + off + k;
    u32 tp = RD32(sel_base[SEL_IDX(sel)] + off + 0x1c);
    u8 *tb = (u8 *)PTR(FAR2LIN(tp));
    static int n2;
    if (n2++ < 3) rt_log("cursor anim table %08x: %u %u %u %u %u %u %u %u\n", tp, tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6], tb[7]);
    static int n;
    if (n++ < 40) rt_log("cursor anim %04x:%04x k=%u count=%u b16=%u loop=%u frame=%u\n", sel, off, k,
                         RD8(base + 0x15), RD8(base + 0x16), RD8(base + 0x17), RD16(ds + 0x2d44));
}

/* patch16 at 5:99da (cursor animation step, cdecl far: hotspot far ptr at sp+4):
   frame 0 of the arrow cursors is empty; with Video > Steady cursor its entry
   in the hotspot's frame table is cleared so the animation never shows it */
extern int opt_steadycursor;
void rt_cursor_steady(u32 sp)
{
    if (!opt_steadycursor) return;
    u32 ss = cpu.sbase[S_SS];
    u32 hp = RD16(ss + (u16)(sp + 4)) | ((u32)RD16(ss + (u16)(sp + 6)) << 16);
    if (!hp) return;
    u32 h = FAR2LIN(hp);
    u32 k = RD8(sel_base[SEL_IDX(RD16(cpu.sbase[S_DS] + 0x1f76))] + 0x2f24);
    if (k) return;
    u32 count = RD8(h + 0x15);
    u32 tp = RD32(h + 0x1c);
    if (count < 2 || count == 255 || !tp) return;
    u32 t = FAR2LIN(tp);
    /* the cursors grow from an empty frame to their full shape and loop
       (a pulse that reads as flicker): keep only the last, fully drawn frame
       enabled - the game's own frame-skip logic then shows just that one */
    int last = -1, n = 0;
    for (u32 i = 0; i < count; i++)
        if (RD8(t + i)) { last = (int)i; n++; }
    if (n <= 1) return;
    for (u32 i = 0; i < count; i++)
        if ((int)i != last) WR8(t + i, 0);
}

/* probe (RC_TEXTDEBUG): 5:7018 text box print(textbox far, string far, flags) */
void rt_text_probe(u32 sp)
{
    if (!getenv(ENV("TEXTDEBUG"))) return;
    u32 ss = cpu.sbase[S_SS];
    u32 tb = RD16(ss + (u16)(sp + 4)) | ((u32)RD16(ss + (u16)(sp + 6)) << 16);
    u32 st = RD16(ss + (u16)(sp + 8)) | ((u32)RD16(ss + (u16)(sp + 10)) << 16);
    if (!tb || !st) return;
    const char *s = (const char *)PTR(FAR2LIN(st));
    u32 t = FAR2LIN(tb);
    rt_log("text tb %08x \"%.40s\" w:", tb, s);
    for (int i = 0; i < 0x40; i += 2) rt_log(" %d", (s16)RD16(t + i));
    rt_log("\n");
}
