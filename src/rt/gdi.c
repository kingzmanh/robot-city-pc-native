/* GDI + WinG: an 8-bit 640x480 screen with a palette, memory DCs,
 * compatible bitmaps and WinG DIB sections in guest memory.
 *
 * Robot City draws with WinG (identity palette, 8-bit DIBs blitted with
 * WinGStretchBlt) and a few GDI calls (BitBlt between memory DCs,
 * Rectangle, palettes).  Pixels are palette indices everywhere; whatever
 * reaches the window DC lands in screen8, which is converted with the
 * realized palette and presented when the game pumps messages.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

u8 screen8[640 * 480];
u32 screen_pal[256];
int screen_dirty;

enum { O_FREE, O_DC, O_BMP, O_PAL, O_BRUSH, O_RGN, O_PEN, O_FONT };
typedef struct {
    int kind;
    /* DC */
    int screen;              /* the window DC */
    u16 bmp, pal, brush, pen, rgn_unused;
    /* bitmap */
    int w, h, stride, topdown;
    u32 lin;                 /* bits in guest memory (WinG) */
    u8 *host;                /* or host memory (compatible bitmaps) */
    u32 ctab[256];
    /* palette */
    u8 pe[256][4];
    int npe;
    /* brush */
    u32 color;
    int style;
    /* region */
    int rl, rt, rr, rb;
    int stock;
} GObj;
#define NOBJ 2048
#define HBASE 0x2000
static GObj obj[NOBJ];
static u16 win_dc;
static u16 stock[20];

static u16 new_obj(int kind)
{
    for (int i = 1; i < NOBJ; i++)
        if (obj[i].kind == O_FREE) {
            memset(&obj[i], 0, sizeof obj[i]);
            obj[i].kind = kind;
            return (u16)(HBASE + i * 4);
        }
    rt_fatal("out of GDI objects");
    return 0;
}

static GObj *O(u16 h)
{
    if (h < HBASE) return NULL;
    int i = (h - HBASE) / 4;
    if (i <= 0 || i >= NOBJ || obj[i].kind == O_FREE) return NULL;
    return &obj[i];
}

static void init_stock(void)
{
    if (stock[0]) return;
    static const u32 brush_col[6] = { 0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0x000000, 0 };
    for (int i = 0; i <= 5; i++) {
        stock[i] = new_obj(O_BRUSH);
        O(stock[i])->color = brush_col[i];
        O(stock[i])->style = i == 5 ? 1 : 0;   /* NULL_BRUSH = hollow */
        O(stock[i])->stock = 1;
    }
    for (int i = 6; i <= 8; i++) { stock[i] = new_obj(O_PEN); O(stock[i])->stock = 1; O(stock[i])->color = i == 6 ? 0xffffff : 0; O(stock[i])->style = i == 8; }
    for (int i = 10; i <= 14; i++) { stock[i] = new_obj(O_FONT); O(stock[i])->stock = 1; }
    stock[15] = new_obj(O_PAL);   /* DEFAULT_PALETTE: the 20 static colours */
    GObj *p = O(stock[15]);
    p->stock = 1;
    static const u8 sys20[20][3] = {
        {0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},{192,220,192},{166,202,240},
        {255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255} };
    p->npe = 256;
    for (int i = 0; i < 256; i++) {
        int k = i < 10 ? i : i >= 246 ? i - 236 : -1;
        if (k >= 0) { p->pe[i][0] = sys20[k][0]; p->pe[i][1] = sys20[k][1]; p->pe[i][2] = sys20[k][2]; }
    }
    for (int i = 0; i < 256; i++) screen_pal[i] = p->pe[i][0] | (p->pe[i][1] << 8) | (p->pe[i][2] << 16);
    stock[16] = new_obj(O_FONT); O(stock[16])->stock = 1;
    stock[17] = new_obj(O_FONT); O(stock[17])->stock = 1;
}

u16 gdi_window_dc(void)
{
    init_stock();
    if (!win_dc) {
        win_dc = new_obj(O_DC);
        GObj *d = O(win_dc);
        d->screen = 1;
        d->pal = stock[15];
        d->brush = stock[0];
        d->pen = stock[7];
    }
    return win_dc;
}

/* ---------------- pixels ---------------- */
typedef struct { u8 *base; int w, h, stride; int topdown; } Surf;

static int surf_of(u16 dc, Surf *s)
{
    GObj *d = O(dc);
    if (!d || d->kind != O_DC) return 0;
    if (d->screen) { s->base = screen8; s->w = 640; s->h = 480; s->stride = 640; s->topdown = 1; return 1; }
    GObj *b = O(d->bmp);
    if (!b || b->kind != O_BMP) return 0;
    s->base = b->host ? b->host : (u8 *)PTR(b->lin);
    s->w = b->w; s->h = b->h; s->stride = b->stride; s->topdown = b->topdown;
    return 1;
}

static inline u8 *row(Surf *s, int y) { return s->base + (size_t)(s->topdown ? y : s->h - 1 - y) * s->stride; }

static int nearest(u32 rgb, u16 dc)
{
    if ((rgb >> 24) == 1) return rgb & 0xff;       /* PALETTEINDEX */
    GObj *d = O(dc);
    GObj *p = d ? O(d->pal) : NULL;
    if (!p) p = O(stock[15]);
    int r = rgb & 0xff, g = (rgb >> 8) & 0xff, b = (rgb >> 16) & 0xff, best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = p->pe[i][0] - r, dg = p->pe[i][1] - g, db = p->pe[i][2] - b;
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}

static void fill(Surf *s, int x0, int y0, int x1, int y1, int c)
{
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w; if (y1 > s->h) y1 = s->h;
    for (int y = y0; y < y1; y++) memset(row(s, y) + x0, c, x1 > x0 ? x1 - x0 : 0);
}

static void blit(u16 ddc, int xd, int yd, int wd, int hd, u16 sdc, int xs, int ys, int ws, int hs, u32 rop)
{
    Surf d, s;
    if (!surf_of(ddc, &d)) return;
    if (rop == 0x00000042 || rop == 0x00ff0062 || rop == 0x00f00021 || !sdc) {
        int c = rop == 0x00ff0062 ? 255 : 0;
        if (rop == 0x00f00021) { GObj *dd = O(ddc); GObj *br = dd ? O(dd->brush) : NULL; c = br ? nearest(br->color, ddc) : 0; }
        fill(&d, xd, yd, xd + wd, yd + hd, c);
        if (O(ddc)->screen) screen_dirty = 1;
        return;
    }
    if (!surf_of(sdc, &s)) return;
    if (rop != 0x00cc0020) {
        static int n;
        if (n++ < 10) rt_log("blit: rop %06x treated as SRCCOPY\n", rop);
    }
    if (wd == ws && hd == hs) {
        /* clip */
        if (xd < 0) { xs -= xd; wd += xd; xd = 0; }
        if (yd < 0) { ys -= yd; hd += yd; yd = 0; }
        if (xs < 0) { xd -= xs; wd += xs; xs = 0; }
        if (ys < 0) { yd -= ys; hd += ys; ys = 0; }
        if (xd + wd > d.w) wd = d.w - xd;
        if (yd + hd > d.h) hd = d.h - yd;
        if (xs + wd > s.w) wd = s.w - xs;
        if (ys + hd > s.h) hd = s.h - ys;
        if (wd <= 0 || hd <= 0) return;
        for (int y = 0; y < hd; y++) memmove(row(&d, yd + y) + xd, row(&s, ys + y) + xs, wd);
    } else {
        if (wd <= 0 || hd <= 0 || ws == 0 || hs == 0) return;
        for (int y = 0; y < hd; y++) {
            int dy = yd + y;
            if (dy < 0 || dy >= d.h) continue;
            int sy = ys + (int)((long long)y * hs / hd);
            if (sy < 0 || sy >= s.h) continue;
            u8 *dr = row(&d, dy), *sr = row(&s, sy);
            for (int x = 0; x < wd; x++) {
                int dx = xd + x;
                if (dx < 0 || dx >= d.w) continue;
                int sx = xs + (int)((long long)x * ws / wd);
                if (sx < 0 || sx >= s.w) continue;
                dr[dx] = sr[sx];
            }
        }
    }
    if (O(ddc)->screen) screen_dirty = 1;
}

void screen_present(void)
{
    if (!screen_dirty) return;
    screen_dirty = 0;
    present_frame(screen8, 640, screen_pal);
}

/* ---------------- GDI natives ---------------- */
u32 w16_GetDeviceCaps(u16 dc, s16 idx)
{
    (void)dc;
    switch (idx) {
    case 2: return 0x300;          /* TECHNOLOGY: raster display */
    case 8: return 640;            /* HORZRES */
    case 10: return 480;           /* VERTRES */
    case 12: return 8;             /* BITSPIXEL */
    case 14: return 1;             /* PLANES */
    case 24: return 20;            /* NUMCOLORS */
    case 38: return 0x0001 | 0x0008 | 0x0080 | 0x0100 | 0x0200 | 0x0800;   /* RASTERCAPS: bitblt, bitmap64, dibtodev, palette, stretch */
    case 88: case 90: return 96;   /* LOGPIXELSX/Y */
    case 104: return 256;          /* SIZEPALETTE */
    case 106: return 20;           /* NUMRESERVED */
    case 108: return 18;           /* COLORRES */
    }
    return 0;
}

u32 w16_GetStockObject(u16 i) { init_stock(); return i < 20 ? stock[i] : 0; }

u32 w16_CreateCompatibleDC(u16 dc)
{
    (void)dc;
    init_stock();
    u16 h = new_obj(O_DC);
    O(h)->pal = stock[15];
    O(h)->brush = stock[0];
    O(h)->pen = stock[7];
    return h;
}

u32 w16_DeleteDC(u16 dc)
{
    GObj *d = O(dc);
    if (!d || d->kind != O_DC || d->screen) return 0;
    d->kind = O_FREE;
    return 1;
}

u32 w16_CreateCompatibleBitmap(u16 dc, u16 w, u16 h)
{
    (void)dc;
    u16 b = new_obj(O_BMP);
    GObj *o = O(b);
    o->w = w; o->h = h; o->stride = (w + 3) & ~3; o->topdown = 1;
    o->host = calloc((size_t)o->stride * (h ? h : 1), 1);
    return b;
}

u32 w16_SelectObject(u16 dc, u16 h)
{
    GObj *d = O(dc), *o = O(h);
    if (!d || !o) return 0;
    u16 old = 0;
    switch (o->kind) {
    case O_BMP: old = d->bmp; d->bmp = h; break;
    case O_BRUSH: old = d->brush; d->brush = h; break;
    case O_PEN: old = d->pen; d->pen = h; break;
    case O_PAL: old = d->pal; d->pal = h; break;
    case O_RGN: return 2;   /* SIMPLEREGION */
    default: return stock[13];
    }
    return old ? old : (o->kind == O_BMP ? 0 : stock[0]);
}

u32 w16_DeleteObject(u16 h)
{
    GObj *o = O(h);
    if (!o || o->stock || o->kind == O_DC) return 0;
    if (o->kind == O_BMP && o->host) free(o->host);
    if (o->kind == O_BMP && o->lin) {
        /* WinG bits: their selectors were given out by gmem; leave the memory */
    }
    o->kind = O_FREE;
    return 1;
}

u32 w16_UnrealizeObject(u16 h) { (void)h; return 1; }
u32 w16_CreateSolidBrush(u32 c) { u16 b = new_obj(O_BRUSH); O(b)->color = c; return b; }
u32 w16_CreateBrushIndirect(u32 lb)
{
    u32 a = FAR2LIN(lb);
    u16 b = new_obj(O_BRUSH);
    O(b)->style = RD16(a);
    O(b)->color = RD32(a + 2);
    return b;
}

u32 w16_CreateRectRgn(s16 l, s16 t, s16 r, s16 b)
{
    u16 h = new_obj(O_RGN);
    GObj *o = O(h);
    o->rl = l; o->rt = t; o->rr = r; o->rb = b;
    return h;
}

u32 w16_Rectangle(u16 dc, s16 l, s16 t, s16 r, s16 b)
{
    Surf s;
    if (!surf_of(dc, &s)) return 0;
    GObj *d = O(dc);
    GObj *br = O(d->brush), *pen = O(d->pen);
    if (br && br->style != 1) fill(&s, l + 1, t + 1, r - 1, b - 1, nearest(br->color, dc));
    if (pen && pen->style != 1) {
        int c = nearest(pen->color, dc);
        fill(&s, l, t, r, t + 1, c); fill(&s, l, b - 1, r, b, c);
        fill(&s, l, t, l + 1, b, c); fill(&s, r - 1, t, r, b, c);
    }
    if (d->screen) screen_dirty = 1;
    return 1;
}

u32 w16_FrameRgn(u16 dc, u16 rgn, u16 brush, u16 w, u16 h)
{
    Surf s;
    GObj *g = O(rgn), *br = O(brush);
    if (!surf_of(dc, &s) || !g) return 0;
    int c = br ? nearest(br->color, dc) : 0;
    fill(&s, g->rl, g->rt, g->rr, g->rt + h, c);
    fill(&s, g->rl, g->rb - h, g->rr, g->rb, c);
    fill(&s, g->rl, g->rt, g->rl + w, g->rb, c);
    fill(&s, g->rr - w, g->rt, g->rr, g->rb, c);
    if (O(dc)->screen) screen_dirty = 1;
    return 1;
}

u32 w16_BitBlt(u16 dd, s16 x, s16 y, s16 w, s16 h, u16 sd, s16 xs, s16 ys, u32 rop)
{
    blit(dd, x, y, w, h, sd, xs, ys, w, h, rop);
    return 1;
}

/* ---------------- palettes ---------------- */
static void realize_screen(GObj *p)
{
    for (int i = 0; i < 256 && i < p->npe; i++)
        screen_pal[i] = p->pe[i][0] | (p->pe[i][1] << 8) | (p->pe[i][2] << 16);
    screen_dirty = 1;
}

u32 w16_CreatePalette(u32 lp)
{
    u32 a = FAR2LIN(lp);
    u16 h = new_obj(O_PAL);
    GObj *p = O(h);
    p->npe = RD16(a + 2);
    if (p->npe > 256) p->npe = 256;
    memcpy(p->pe, MEM + a + 4, p->npe * 4);
    return h;
}

u32 w16_SetPaletteEntries(u16 h, u16 start, u16 n, u32 e)
{
    GObj *p = O(h);
    if (!p || p->kind != O_PAL) return 0;
    for (int i = 0; i < n && start + i < 256; i++) memcpy(p->pe[start + i], MEM + FAR2LIN(e) + i * 4, 4);
    if (start + n > p->npe) p->npe = start + n > 256 ? 256 : start + n;
    return n;
}

static u16 screen_pal_h;
u32 w16_AnimatePalette(u16 h, u16 start, u16 n, u32 e)
{
    GObj *p = O(h);
    if (!p || p->kind != O_PAL) return 0;
    for (int i = 0; i < n && start + i < 256; i++) {
        u8 *src = MEM + FAR2LIN(e) + i * 4;
        if (p->pe[start + i][3] & 1) memcpy(p->pe[start + i], src, 3);   /* PC_RESERVED entries only */
        else memcpy(p->pe[start + i], src, 3);
    }
    if (h == screen_pal_h) realize_screen(p);
    return 1;
}

u32 w16_SelectPalette(u16 dc, u16 pal, u16 bg)
{
    (void)bg;
    GObj *d = O(dc);
    if (!d) return 0;
    u16 old = d->pal;
    d->pal = pal;
    return old;
}

u32 w16_RealizePalette(u16 dc)
{
    GObj *d = O(dc), *p = d ? O(d->pal) : NULL;
    if (!p) return 0;
    if (d->screen) { screen_pal_h = d->pal; realize_screen(p); }
    return p->npe;
}

u32 w16_SetSystemPaletteUse(u16 dc, u16 use) { (void)dc; (void)use; return 1; }

u32 w16_GetDIBits(u16 dc, u16 bmp, u16 start, u16 lines, u32 bits, u32 bmi, u16 usage)
{
    (void)usage;
    GObj *b = O(bmp);
    if (!b || b->kind != O_BMP) return 0;
    u32 hi = FAR2LIN(bmi);
    if (!bits) {   /* fill in the header only */
        WR32(hi + 4, b->w); WR32(hi + 8, b->h); WR16(hi + 12, 1); WR16(hi + 14, 8); WR32(hi + 16, 0);
        WR32(hi + 20, ((b->w + 3) & ~3) * b->h);
        return b->h;
    }
    s32 bh = (s32)RD32(hi + 8);
    int td = bh < 0;
    int stride = (b->w + 3) & ~3;
    u32 dst = FAR2LIN(bits);
    Surf s = { b->host ? b->host : (u8 *)PTR(b->lin), b->w, b->h, b->stride, b->topdown };
    int n = 0;
    for (int i = 0; i < lines && start + i < b->h; i++) {
        int sy = b->h - 1 - (start + i);         /* scan line i of a bottom-up DIB */
        int dline = td ? (b->h - 1 - (start + i)) : i;
        memcpy(MEM + dst + (u32)dline * stride, row(&s, sy), b->w);
        n++;
    }
    /* colour table from the DC's palette */
    GObj *d = O(dc), *p = d ? O(d->pal) : NULL;
    u32 ct = hi + RD32(hi);
    for (int i = 0; i < 256 && p; i++) { WR8(ct + i * 4, p->pe[i][2]); WR8(ct + i * 4 + 1, p->pe[i][1]); WR8(ct + i * 4 + 2, p->pe[i][0]); WR8(ct + i * 4 + 3, 0); }
    return n;
}

/* ---------------- WinG ---------------- */
u32 w16_WinGCreateDC(void) { return w16_CreateCompatibleDC(0); }

u32 w16_WinGCreateBitmap(u16 dc, u32 bmi, u32 ppbits)
{
    (void)dc;
    u32 hi = FAR2LIN(bmi);
    s32 w = (s32)RD32(hi + 4), h = (s32)RD32(hi + 8);
    int bpp = RD16(hi + 14);
    if (bpp != 8) rt_log("WinGCreateBitmap: %d bpp\n", bpp);
    u16 b = new_obj(O_BMP);
    GObj *o = O(b);
    o->w = w; o->h = h < 0 ? -h : h; o->topdown = h < 0;
    o->stride = (w + 3) & ~3;
    u32 size = (u32)o->stride * o->h;
    u16 sel = gmem_alloc(size, 0);
    o->lin = sel_base[SEL_IDX(sel)];
    for (int i = 0; i < 256; i++) o->ctab[i] = RD32(hi + RD32(hi) + i * 4);
    if (ppbits) WR32(FAR2LIN(ppbits), (u32)sel << 16);
    rt_log("WinGCreateBitmap %dx%d %s -> %04x bits %04x:0000\n", o->w, o->h, o->topdown ? "top-down" : "bottom-up", b, sel);
    return b;
}

u32 w16_WinGSetDIBColorTable(u16 dc, u16 start, u16 n, u32 rgb)
{
    GObj *d = O(dc), *b = d ? O(d->bmp) : NULL;
    if (!b) return 0;
    for (int i = 0; i < n && start + i < 256; i++) b->ctab[start + i] = RD32(FAR2LIN(rgb) + i * 4);
    return n;
}

u32 w16_WinGStretchBlt(u16 dd, u16 xd, u16 yd, u16 wd, u16 hd, u16 sd, u16 xs, u16 ys, u16 ws, u16 hs)
{
    blit(dd, (s16)xd, (s16)yd, (s16)wd, (s16)hd, sd, (s16)xs, (s16)ys, (s16)ws, (s16)hs, 0xcc0020);
    return 1;
}
