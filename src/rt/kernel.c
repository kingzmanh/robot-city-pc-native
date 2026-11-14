/* KERNEL: task start, global memory + selectors, resources, files and the
 * DOS services (DOS3Call / int 21h) on Windows files.
 *
 * Drives: C:\ = game\hd, D:\ = game\cd (both Robot City CDs merged).  The
 * game runs from the CD as installed (D:\RC\ROBOTCTY.EXE).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"

int api_log_on;

/* ---------------- memory ---------------- */
typedef struct { u32 lin, size; u16 sel; int nsel; int used; } Blk;
#define NBLK 8192
static Blk blk[NBLK];
static int nblk;
static u32 heap_top = HEAP_LIN;
static u32 low_top = LOW_LIN;

u32 lowmem_alloc(u32 size)
{
    u32 a = low_top;
    low_top = (low_top + size + 15) & ~15u;
    if (low_top > SEG_LIN(0)) rt_fatal("low memory full");
    memset(MEM + a, 0, size);
    return a;
}

static Blk *blk_of(u16 sel)
{
    for (int i = 0; i < nblk; i++)
        if (blk[i].used && blk[i].sel == (sel | 7)) return &blk[i];
    return NULL;
}

static void map_sels(Blk *b)
{
    for (int k = 0; k < b->nsel; k++) {
        u32 rest = b->size > (u32)k * 0x10000 ? b->size - k * 0x10000 : 1;
        sel_set((u16)(b->sel + 8 * k), b->lin + k * 0x10000, (rest > 0x10000 ? 0x10000 : rest) - 1);
    }
}

static u32 lin_alloc(u32 size)
{
    /* reuse a freed block of the same rounded size, else bump */
    u32 rs = (size + 0xfff) & ~0xfffu;
    for (int i = 0; i < nblk; i++)
        if (!blk[i].used && blk[i].lin && ((blk[i].size + 0xfff) & ~0xfffu) == rs) {
            u32 l = blk[i].lin;
            blk[i].lin = 0;
            return l;
        }
    u32 a = heap_top;
    heap_top += rs;
    if (heap_top > GUEST_SIZE) rt_fatal("guest heap full (%u bytes asked)", size);
    return a;
}

u32 gmem_alloc(u32 size, int flags)
{
    if (!size) size = 1;
    int i;
    for (i = 0; i < nblk; i++) if (!blk[i].used && !blk[i].lin) break;
    if (i == nblk) { if (nblk == NBLK) rt_fatal("too many global blocks"); nblk++; }
    Blk *b = &blk[i];
    b->size = size;
    b->lin = lin_alloc(size);
    b->nsel = (size + 0xffff) >> 16;
    b->sel = sel_alloc(b->nsel);
    b->used = 1;
    map_sels(b);
    memset(MEM + b->lin, 0, (size + 0xfff) & ~0xfffu);
    (void)flags;
    return b->sel;
}

u32 gmem_realloc(u16 sel, u32 size)
{
    Blk *b = blk_of(sel);
    if (!b) return 0;
    int nsel = (size + 0xffff) >> 16;
    if (((size + 0xfff) & ~0xfffu) <= ((b->size + 0xfff) & ~0xfffu) && nsel <= b->nsel) {
        b->size = size;
        map_sels(b);
        return b->sel;
    }
    u32 nl = lin_alloc(size);
    memset(MEM + nl, 0, (size + 0xfff) & ~0xfffu);
    memcpy(MEM + nl, MEM + b->lin, b->size < size ? b->size : size);
    /* the old space goes to a free record */
    for (int i = 0; i < nblk; i++) if (!blk[i].used && !blk[i].lin) { blk[i].lin = b->lin; blk[i].size = b->size; break; }
    b->lin = nl;
    if (nsel > b->nsel) {
        for (int k = 0; k < b->nsel; k++) sel_free((u16)(b->sel + 8 * k));
        b->sel = sel_alloc(nsel);   /* keeps the old selector only if it fits */
        b->nsel = nsel;
        rt_log("GlobalReAlloc: block moved to new selector %04x (old %04x)\n", b->sel, sel);
    }
    b->size = size;
    map_sels(b);
    return b->sel;
}

void gmem_free(u16 sel)
{
    Blk *b = blk_of(sel);
    if (!b) return;
    for (int k = 0; k < b->nsel; k++) sel_free((u16)(b->sel + 8 * k));
    b->used = 0;   /* lin stays recorded for reuse */
}

u32 gmem_size(u16 sel)
{
    Blk *b = blk_of(sel);
    return b ? b->size : 0;
}

u32 gstrdup16(const char *s)
{
    u16 sel = gmem_alloc(strlen(s) + 1, 0);
    strcpy((char *)PTR(sel_base[SEL_IDX(sel)]), s);
    return (u32)sel << 16;
}

const char *gstr(u32 fp)
{
    if (!fp) return NULL;
    if ((fp >> 16) == 0) return NULL;
    return (const char *)FARPTR(fp);
}

u32 w16_GlobalAlloc(u16 flags, u32 size) { return gmem_alloc(size, flags); }
u32 w16_GlobalReAlloc(u16 h, u32 size, u16 flags) { (void)flags; return gmem_realloc(h, size); }
u32 w16_GlobalFree(u16 h) { gmem_free(h); return 0; }
u32 w16_GlobalLock(u16 h) { return h ? ((u32)(h | 7) << 16) : 0; }
u32 w16_GlobalUnlock(u16 h) { (void)h; return 0; }
u32 w16_GlobalSize(u16 h) { return gmem_size(h); }
u32 w16_LockSegment(u16 s) { return s; }
u32 w16_SetErrorMode(u16 m) { (void)m; return 0; }
u32 w16_GetVersion(void) { return 0x06160a03u; }
u32 w16_GetDOSEnvironment(void) { return (u32)RD16(sel_base[SEL_IDX(g_psp)] + 0x2c) << 16; }
u32 w16_WaitEvent(u16 t) { (void)t; win_pump(1); return 1; }
u32 w16_FatalExit(u16 c) { rt_fatal("FatalExit(%d)", c); return 0; }
u32 w16_FatalAppExit(u16 a, u32 s) { (void)a; rt_fatal("FatalAppExit: %s", gstr(s) ? gstr(s) : ""); return 0; }
u32 w16_WinExec(u32 cmd, u16 show) { (void)show; rt_log("WinExec(%s) ignored\n", gstr(cmd)); return 2; }

u32 w16_GetModuleFileName(u16 h, u32 buf, s16 n)
{
    (void)h;
    const char *p = GAME_EXE_DOS;
    int l = (int)strlen(p);
    if (l >= n) l = n - 1;
    memcpy(FARPTR(buf), p, l);
    ((char *)FARPTR(buf))[l] = 0;
    return l;
}

void w16_InitTask(void)
{
    cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | 1;
    cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffff0000u) | 0x81;
    cpu.r[R_ECX] = (cpu.r[R_ECX] & 0xffff0000u) | (RD16(sel_base[SEL_IDX(g_dgroup)] + 0x0a));
    cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | 1;   /* SW_SHOWNORMAL */
    cpu.r[R_ESI] &= 0xffff0000u;
    cpu.r[R_EDI] = (cpu.r[R_EDI] & 0xffff0000u) | g_hinst;
    rt_load_seg(S_ES, g_psp);
}

void w16___fpMath(void)
{
    /* WIN87EM: BX = function; 0 = init (AX = 0: ok) */
    u32 f = cpu.r[R_EBX] & 0xffff;
    cpu.cf = 0;
    if (f == 0 || f == 1) { cpu.r[R_EAX] &= 0xffff0000u; return; }
    if (f == 2) { cpu.r[R_EAX] &= 0xffff0000u; return; }    /* set error handler */
    if (f == 3) { FPU_INIT(); return; }
    rt_log("__fpMath(%u)\n", f);
    cpu.r[R_EAX] &= 0xffff0000u;
}

/* ---------------- resources ---------------- */
typedef struct { int type_id; char type_name[16]; int id; char name[32]; u32 off, len; u16 sel; } Res;
static Res res[512];
static int nres;
static int res_loaded;

static void res_load(void)
{
    if (res_loaded) return;
    res_loaded = 1;
    char p[700];
    snprintf(p, sizeof p, "%s\\hd\\" GAME_EXE_PATH, rt_root);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return; }
    fclose(f);
#define W(o) (d[o] | (d[(o) + 1] << 8))
    u32 ne = W(0x3c);
    u32 rs = ne + W(ne + 0x24);
    int sh = W(rs);
    u32 q = rs + 2;
    while (W(q)) {
        int t = W(q), c = W(q + 2);
        q += 8;
        for (int k = 0; k < c && nres < 512; k++, q += 12) {
            Res *r = &res[nres++];
            r->off = (u32)W(q) << sh;
            r->len = (u32)W(q + 2) << sh;
            int rid = W(q + 6);
            r->type_id = (t & 0x8000) ? (t & 0x7fff) : -1;
            if (!(t & 0x8000)) { int l = d[rs + t]; memcpy(r->type_name, d + rs + t + 1, l < 15 ? l : 15); }
            r->id = (rid & 0x8000) ? (rid & 0x7fff) : -1;
            if (!(rid & 0x8000)) { int l = d[rs + rid]; memcpy(r->name, d + rs + rid + 1, l < 31 ? l : 31); }
            /* keep the bytes in guest memory */
            u16 sel = gmem_alloc(r->len, 0);
            memcpy(PTR(sel_base[SEL_IDX(sel)]), d + r->off, r->len);
            r->sel = sel;
        }
    }
    free(d);
    rt_log("resources: %d\n", nres);
}

static int res_match(u32 want, int id, const char *name)
{
    if ((want >> 16) == 0) return id == (int)(want & 0xffff);
    const char *s = gstr(want);
    if (s[0] == '#') return id == atoi(s + 1);
    return id < 0 && !_stricmp(s, name);
}

u32 w16_FindResource(u16 h, u32 name, u32 type)
{
    (void)h;
    res_load();
    for (int i = 0; i < nres; i++)
        if (res_match(type, res[i].type_id, res[i].type_name) && res_match(name, res[i].id, res[i].name)) return i + 1;
    rt_log("FindResource: not found (name %08x type %08x %s)\n", name, type, (type >> 16) ? gstr(type) : "");
    return 0;
}
u32 w16_LoadResource(u16 h, u16 r) { (void)h; return (r && r <= nres) ? res[r - 1].sel : 0; }
u32 w16_LockResource(u16 h) { return h ? ((u32)h << 16) : 0; }
u32 w16_FreeResource(u16 h) { (void)h; return 0; }
u32 w16_SizeofResource(u16 h, u16 r) { (void)h; return (r && r <= nres) ? res[r - 1].len : 0; }

/* ---------------- paths and files ---------------- */
static char cur_drive = 'C';
static char cur_dir[260] = "\\RC";

void path_to_host(const char *g, char *out, size_t n)
{
    char full[600];
    char drv = cur_drive;
    if (g[0] && g[1] == ':') { drv = (char)toupper((u8)g[0]); g += 2; }
    if (g[0] == '\\' || g[0] == '/') snprintf(full, sizeof full, "%s", g);
    else snprintf(full, sizeof full, "%s\\%s", drv == cur_drive ? cur_dir : "", g);
    for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
    snprintf(out, n, "%s\\%s%s", rt_root, drv == 'C' ? "hd" : "cd", full);
}

#define NFH 64
static HANDLE fh[NFH];

static int fh_new(HANDLE h)
{
    for (int i = 5; i < NFH; i++) if (!fh[i]) { fh[i] = h; return i; }
    CloseHandle(h);
    return -1;
}

static HANDLE fh_get(int i) { return (i >= 0 && i < NFH) ? fh[i] : NULL; }

static int file_debug = -1;
static int dos_open(const char *gpath, int mode, int create)
{
    char hp[700];
    path_to_host(gpath, hp, sizeof hp);
    DWORD acc = (mode & 3) == 0 ? GENERIC_READ : (mode & 3) == 1 ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    HANDLE h = CreateFileA(hp, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, create ? CREATE_ALWAYS : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (file_debug < 0) file_debug = getenv(ENV("FILEDEBUG")) != NULL;
    if (file_debug) rt_log("open %s -> %s %s\n", gpath, hp, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    if (h == INVALID_HANDLE_VALUE) return -(int)(GetLastError() == ERROR_PATH_NOT_FOUND ? 3 : 2);
    return fh_new(h);
}

static void dos_close(int i)
{
    HANDLE h = fh_get(i);
    if (h) { CloseHandle(h); fh[i] = NULL; }
}

static s32 dos_read(int i, u32 lin, u32 n)
{
    HANDLE h = fh_get(i);
    DWORD got = 0;
    if (!h || !ReadFile(h, MEM + lin, n, &got, NULL)) return -1;
    return got;
}

static s32 dos_write(int i, u32 lin, u32 n)
{
    HANDLE h = fh_get(i);
    DWORD put = 0;
    if (!h) return -1;
    if (!n) { SetEndOfFile(h); return 0; }
    if (!WriteFile(h, MEM + lin, n, &put, NULL)) return -1;
    return put;
}

static s32 dos_seek(int i, s32 off, int how)
{
    HANDLE h = fh_get(i);
    if (!h) return -1;
    LONG hi = 0;
    DWORD r = SetFilePointer(h, off, &hi, how);
    if (r == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) return -1;
    return (s32)r;
}

u32 w16_OpenFile(u32 name, u32 ofs, u16 mode)
{
    const char *n = gstr(name);
    u8 *of = FARPTR(ofs);
    if (of) {
        memset(of, 0, 136);
        of[0] = 136;
        snprintf((char *)of + 8, 128, "%s", n);
    }
    if (mode & 0x4000) {   /* OF_EXIST */
        char hp[700];
        path_to_host(n, hp, sizeof hp);
        DWORD a = GetFileAttributesA(hp);
        if (file_debug > 0) rt_log("OpenFile exist %s -> %s\n", n, a == INVALID_FILE_ATTRIBUTES ? "no" : "yes");
        return a == INVALID_FILE_ATTRIBUTES ? 0xffff : 1;
    }
    int h = dos_open(n, mode & 3, (mode & 0x1000) != 0);
    if (h < 0) { if (of) of[2] = (u8)-h; return 0xffff; }
    return h;
}

u32 w16__lclose(u16 h) { dos_close(h); return 0; }
u32 w16__llseek(u16 h, u32 off, u16 how) { return (u32)dos_seek(h, (s32)off, how); }

/* huge reads cross 64 KB selector tiles: the block is linear in MEM */
u32 w16__hread(u16 h, u32 buf, u32 n) { return (u32)dos_read(h, FAR2LIN(buf), n); }
u32 w16__hwrite(u16 h, u32 buf, u32 n) { return (u32)dos_write(h, FAR2LIN(buf), n); }

/* ---------------- DOS (int 21h / DOS3Call) ---------------- */
#define AX (cpu.r[R_EAX] & 0xffff)
#define AH ((cpu.r[R_EAX] >> 8) & 0xff)
#define AL (cpu.r[R_EAX] & 0xff)
#define BX (cpu.r[R_EBX] & 0xffff)
#define CX (cpu.r[R_ECX] & 0xffff)
#define DX (cpu.r[R_EDX] & 0xffff)
static void setr(int r, u32 v) { cpu.r[r] = (cpu.r[r] & 0xffff0000u) | (v & 0xffff); }
static void ok(void) { cpu.cf = 0; }
static void err(int code) { cpu.cf = 1; setr(R_EAX, code); }
static u32 dta_lin;
static HANDLE find_h;
static char find_attr;

static int fill_dta(WIN32_FIND_DATAA *fd)
{
    if (!dta_lin) return 0;
    WR8(dta_lin + 0x15, (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x20);
    FILETIME lt;
    WORD dd = 0, tt = 0;
    FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lt);
    FileTimeToDosDateTime(&lt, &dd, &tt);
    WR16(dta_lin + 0x16, tt);
    WR16(dta_lin + 0x18, dd);
    WR32(dta_lin + 0x1a, fd->nFileSizeLow);
    const char *n = fd->cAlternateFileName[0] ? fd->cAlternateFileName : fd->cFileName;
    char up[14] = { 0 };
    for (int i = 0; i < 12 && n[i]; i++) up[i] = (char)toupper((u8)n[i]);
    memcpy(MEM + dta_lin + 0x1e, up, 13);
    return 1;
}

static int find_next_match(WIN32_FIND_DATAA *fd)
{
    do {
        int dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dir && !(find_attr & 0x10)) continue;
        if (!strcmp(fd->cFileName, ".") || !strcmp(fd->cFileName, "..")) { if (!(find_attr & 0x10)) continue; }
        return 1;
    } while (FindNextFileA(find_h, fd));
    return 0;
}

void w16_DOS3Call(void) { dos_int21(); }

void dos_int21(void)
{
    u32 dsdx = cpu.sbase[S_DS] + DX;
    switch (AH) {
    case 0x0e: if (DX < 26) cur_drive = 'A' + DX; setr(R_EAX, (AX & 0xff00) | 26); return;
    case 0x19: setr(R_EAX, (AX & 0xff00) | (cur_drive - 'A')); return;
    case 0x1a: dta_lin = dsdx; return;
    case 0x2a: { SYSTEMTIME t; GetLocalTime(&t); setr(R_ECX, t.wYear); setr(R_EDX, (t.wMonth << 8) | t.wDay); setr(R_EAX, (AX & 0xff00) | t.wDayOfWeek); return; }
    case 0x2c: { SYSTEMTIME t; GetLocalTime(&t); setr(R_ECX, (t.wHour << 8) | t.wMinute); setr(R_EDX, (t.wSecond << 8) | (t.wMilliseconds / 10)); return; }
    case 0x2f: rt_load_seg(S_ES, 0); setr(R_EBX, 0); return;
    case 0x30: setr(R_EAX, 0x1606); setr(R_EBX, 0); ok(); return;
    case 0x25: case 0x35: return;
    case 0x33: setr(R_EDX, AX & 0xff00); return;
    case 0x36: setr(R_EAX, 64); setr(R_EBX, 0x7fff); setr(R_ECX, 512); setr(R_EDX, 0x7fff); return;
    case 0x39: { char hp[700]; path_to_host((char *)MEM + dsdx, hp, sizeof hp); CreateDirectoryA(hp, NULL); ok(); return; }
    case 0x3b: {
        const char *p = (char *)MEM + dsdx;
        if (p[0] && p[1] == ':') { cur_drive = (char)toupper((u8)p[0]); p += 2; }
        if (p[0] == '\\') snprintf(cur_dir, sizeof cur_dir, "%s", p);
        else { char t[260]; snprintf(t, sizeof t, "%s\\%s", cur_dir, p); snprintf(cur_dir, sizeof cur_dir, "%s", t); }
        size_t l = strlen(cur_dir);
        if (l > 1 && cur_dir[l - 1] == '\\') cur_dir[l - 1] = 0;
        ok();
        return;
    }
    case 0x3c: { int h = dos_open((char *)MEM + dsdx, 2, 1); if (h < 0) err(-h); else { setr(R_EAX, h); ok(); } return; }
    case 0x3d: { int h = dos_open((char *)MEM + dsdx, AL & 3, 0); if (h < 0) err(-h); else { setr(R_EAX, h); ok(); } return; }
    case 0x3e: dos_close(BX); ok(); return;
    case 0x3f: { s32 n = dos_read(BX, dsdx, CX); if (n < 0) err(6); else { setr(R_EAX, n); ok(); } return; }
    case 0x40: {
        if (BX == 1 || BX == 2) { rt_log("stdout: %.*s\n", CX, (char *)MEM + dsdx); setr(R_EAX, CX); ok(); return; }
        s32 n = dos_write(BX, dsdx, CX); if (n < 0) err(6); else { setr(R_EAX, n); ok(); } return;
    }
    case 0x41: { char hp[700]; path_to_host((char *)MEM + dsdx, hp, sizeof hp); if (DeleteFileA(hp)) ok(); else err(2); return; }
    case 0x42: {
        s32 r = dos_seek(BX, (s32)((CX << 16) | DX), AL);
        if (r < 0) err(6); else { setr(R_EAX, r); setr(R_EDX, (u32)r >> 16); ok(); }
        return;
    }
    case 0x43: {
        char hp[700];
        path_to_host((char *)MEM + dsdx, hp, sizeof hp);
        DWORD a = GetFileAttributesA(hp);
        if (a == INVALID_FILE_ATTRIBUTES) { err(2); return; }
        setr(R_ECX, (a & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x20);
        ok();
        return;
    }
    case 0x44:
        if (AL == 0) { setr(R_EDX, BX < 5 ? 0x80d3 : 0x0002); ok(); return; }
        if (AL == 8) { setr(R_EAX, 1); ok(); return; }
        if (AL == 9) { setr(R_EDX, 0x1000); ok(); return; }   /* remote (CD) */
        err(1);
        return;
    case 0x45: { HANDLE h = fh_get(BX), d; if (h && DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) { setr(R_EAX, fh_new(d)); ok(); } else err(6); return; }
    case 0x47: {
        const char *d = cur_dir[0] == '\\' ? cur_dir + 1 : cur_dir;
        strcpy((char *)MEM + cpu.sbase[S_DS] + (cpu.r[R_ESI] & 0xffff), d);
        ok();
        return;
    }
    case 0x4e: {
        char hp[700];
        if (find_h) { FindClose(find_h); find_h = NULL; }
        path_to_host((char *)MEM + dsdx, hp, sizeof hp);
        find_attr = (char)CX;
        WIN32_FIND_DATAA fd;
        find_h = FindFirstFileA(hp, &fd);
        if (find_h == INVALID_HANDLE_VALUE) { find_h = NULL; err(18); return; }
        if (!find_next_match(&fd)) { err(18); return; }
        fill_dta(&fd);
        ok();
        return;
    }
    case 0x4f: {
        WIN32_FIND_DATAA fd;
        if (!find_h || !FindNextFileA(find_h, &fd) || !find_next_match(&fd)) { err(18); return; }
        fill_dta(&fd);
        ok();
        return;
    }
    case 0x4c: rt_log("DOS exit %d\n", AL); TerminateProcess(GetCurrentProcess(), AL); return;
    case 0x56: {
        char a[700], b[700];
        path_to_host((char *)MEM + dsdx, a, sizeof a);
        path_to_host((char *)MEM + cpu.sbase[S_ES] + (cpu.r[R_EDI] & 0xffff), b, sizeof b);
        if (MoveFileA(a, b)) ok(); else err(5);
        return;
    }
    case 0x57: {
        HANDLE h = fh_get(BX);
        if (!h) { err(6); return; }
        if (AL == 0) {
            FILETIME ft, lt; WORD dd = 0, tt = 0;
            GetFileTime(h, NULL, NULL, &ft);
            FileTimeToLocalFileTime(&ft, &lt);
            FileTimeToDosDateTime(&lt, &dd, &tt);
            setr(R_ECX, tt); setr(R_EDX, dd);
        }
        ok();
        return;
    }
    case 0x59: setr(R_EAX, 0); ok(); return;
    case 0x67: ok(); return;
    }
    rt_log("int 21h ah=%02x not handled (ax=%04x)\n", AH, AX);
    err(1);
}
