/* Dialogue menus (DARLA and the other robots): the options are pictures of
 * text ("1.  Beverage" ...) and the game only takes the small numbered
 * buttons under them (hotspots 301..309 of the main pane).  Here: clicking an
 * option's text or pressing its number key (top row or keypad) presses that
 * button for the player.
 *
 * Game data used (found with the click handler 6:8d4c / pane hit test 3:a768):
 *   DS:0x2000 = selector of the far data holding the pane root at 0x47b6
 *   root +0x2c -> list head -> panes (first dword = next, list ends with a
 *   node whose next is 0); pane +0x2c/+0x2e = origin on the screen,
 *   pane +0x18 -> list head -> hotspots (next = first dword);
 *   hotspot +0x30 flags (0x100001 = enabled + clickable), +0x1c..+0x22 the
 *   rectangle relative to the pane, +0x38 id.
 */
#include <stdlib.h>
#include "rt.h"

static u32 data_base(void)
{
    u32 dg = sel_base[SEL_IDX(g_dgroup)];
    u16 sel = RD16(dg + 0x2000);
    return sel_base[SEL_IDX(sel)];
}

static int valid_fp(u32 fp)
{
    u16 s = fp >> 16;
    return fp && SEL_IDX(s) < NSEL && sel_used[SEL_IDX(s)] && (fp & 0xffff) < 0xfff0;
}

/* button n (1..9) of a dialogue menu: its centre on the 640x480 screen */
int dlg_find_button(int n, int *x, int *y)
{
    /* off unless RC_DLGINPUT is set: the game's own way is the numbered buttons */
    static int on = -1;
    if (on < 0) on = getenv(ENV("DLGINPUT")) != NULL;
    if (!on || !g_dgroup) return 0;
    u32 root = RD32(data_base() + 0x47b6);
    if (!valid_fp(root)) return 0;
    u32 head = RD32(FAR2LIN(root) + 0x2c);
    if (!valid_fp(head)) return 0;
    u32 pane = RD32(FAR2LIN(head));
    for (int i = 0; i < 64 && valid_fp(pane); i++) {
        u32 p = FAR2LIN(pane);
        u32 next = RD32(p);
        if (!next) break;
        int px = (s16)RD16(p + 0x2c), py = (s16)RD16(p + 0x2e);
        u32 hh = RD32(p + 0x18);
        u32 hs = valid_fp(hh) ? RD32(FAR2LIN(hh)) : 0;
        for (int k = 0; k < 400 && valid_fp(hs); k++) {
            u32 h = FAR2LIN(hs);
            if ((RD32(h + 0x30) & 0x100001u) == 0x100001u && RD16(h + 0x38) == (u16)(300 + n)) {
                *x = px + ((s16)RD16(h + 0x1c) + (s16)RD16(h + 0x20)) / 2;
                *y = py + ((s16)RD16(h + 0x1e) + (s16)RD16(h + 0x22)) / 2;
                return 1;
            }
            hs = RD32(h);
        }
        pane = next;
    }
    return 0;
}

/* the option text: light cyan on the menu panel */
static int ink(int x, int y)
{
    if (x < 0 || y < 0 || x >= 640 || y >= 480) return 0;
    u32 c = screen_pal[screen8[y * 640 + x]];      /* 0x00BBGGRR */
    int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
    return b > 190 && g > 160 && r > 100 && r < 200 && b > r + 30;
}

static int row_ink(int y, int x0, int x1)
{
    for (int x = x0; x <= x1; x++) if (ink(x, y)) return 1;
    return 0;
}

/* which option's text is at (x, y): 1.. or 0.  Only while button 1 exists
   (a dialogue menu is up); the text sits above the buttons. */
int dlg_option_at(int x, int y)
{
    int bx, by;
    if (!dlg_find_button(1, &bx, &by)) return 0;
    int top = by - 230, bottom = by - 12;          /* the text panel above the buttons */
    if (top < 0) top = 0;
    if (y < top || y > bottom) return 0;
    /* left edge of the text */
    int L = -1;
    for (int cx = bx - 80; cx < bx + 10 && L < 0; cx++)
        for (int cy = top; cy <= bottom; cy++) if (ink(cx, cy)) { L = cx; break; }
    if (L < 0 || x < L - 4) return 0;
    /* text lines = runs of rows with ink near the left edge */
    int lines_top[40], lines_opt[40], nl = 0, last_bottom = -1;
    int cy = top;
    while (cy <= bottom && nl < 40) {
        if (!row_ink(cy, L, L + 60)) { cy++; continue; }
        int y0 = cy;
        while (cy <= bottom && row_ink(cy, L, L + 60)) cy++;
        int y1 = cy - 1;
        last_bottom = y1;
        int digit = 0, gap = 0;
        for (int yy = y0; yy <= y1; yy++) {
            if (row_ink(yy, L, L + 5)) digit = 1;
            if (row_ink(yy, L + 10, L + 17)) gap = 1;   /* a heading's letters run on */
        }
        lines_top[nl] = y0;
        lines_opt[nl] = digit && !gap;
        nl++;
    }
    if (y > last_bottom + 4) return 0;
    /* the option whose first line is the last one starting above the click */
    int opt = 0, n = 0, hit = 0;
    for (int i = 0; i < nl; i++) {
        if (lines_opt[i]) n++;
        if (lines_top[i] <= y + 2) { if (lines_opt[i]) opt = n; else if (!opt) hit = 0; }
    }
    (void)hit;
    return opt;
}
