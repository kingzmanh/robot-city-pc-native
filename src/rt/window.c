/* USER: the game's window, its message queue, input and painting.
 *
 * The game registers one window class and creates one 640x480 window; its
 * window procedure (guest code) gets the messages Windows 3.1 would send.
 * The real window is ours: resizable, the 640x480 picture scaled into it at
 * 4:3 through the Direct3D presenter (present.c).
 *
 * Test switches: RC_HIDDEN=1 (no window, no dialogs), RC_KEYS="ms:vk;..."
 * (key presses), RC_CLICK="x,y,ms;..." (left clicks at game coordinates),
 * RC_SHOT="ms;ms..." (frames -> shots\), RC_QUIT=ms (close after that time).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

int present_init(HWND h);
u32 g_hwnd;
int quit_requested;
static HWND hwnd;
static int hidden, cursor_shown = 1, in_menu;
static u32 t0;
static int gx = 320, gy = 240;   /* mouse in game coordinates */
static u8 keys[256];
static int d3d_ok;

u32 now_ms(void) { return timeGetTime(); }
u32 win_time(void) { return now_ms() - t0; }

/* ---------------- guest windows ---------------- */
#define GHWND 0x0100
typedef struct { char name[64]; u32 proc; u16 style, hbr, hcursor; } WClass;
static WClass classes[8];
static int nclasses;
static u32 g_proc;
static int g_w = 640, g_h = 480, g_x, g_y;
static int invalid;           /* the window needs WM_PAINT */
static int inv_l, inv_t, inv_r, inv_b;   /* the update region (its bounding box) */

static void inv_add(int l, int t, int r, int b)
{
    if (l < 0) l = 0; if (t < 0) t = 0; if (r > 640) r = 640; if (b > 480) b = 480;
    if (r <= l || b <= t) return;
    if (!invalid) { inv_l = l; inv_t = t; inv_r = r; inv_b = b; }
    else {
        if (l < inv_l) inv_l = l; if (t < inv_t) inv_t = t;
        if (r > inv_r) inv_r = r; if (b > inv_b) inv_b = b;
    }
    invalid = 1;
}
static int created, destroyed;

/* ---------------- message queue ---------------- */
typedef struct { u32 hwnd, msg, wp, lp, time; s16 x, y; } GMsg;
#define QN 1024
static GMsg q[QN];
static int qh, qt;
static int quit_posted, quit_code;

void msgq_post(u32 h, u32 msg, u32 wp, u32 lp)
{
    int n = (qt + 1) % QN;
    if (n == qh) return;
    /* coalesce mouse moves */
    if (msg == WM_MOUSEMOVE && qh != qt) {
        int last = (qt + QN - 1) % QN;
        if (q[last].msg == WM_MOUSEMOVE) { q[last].wp = wp; q[last].lp = lp; q[last].x = gx; q[last].y = gy; return; }
    }
    q[qt].hwnd = h; q[qt].msg = msg; q[qt].wp = wp; q[qt].lp = lp; q[qt].time = now_ms();
    q[qt].x = (s16)gx; q[qt].y = (s16)gy;
    qt = n;
}

void msgq_quit(int code) { quit_posted = 1; quit_code = code; }

static u32 send(u32 msg, u32 wp, u32 lp)
{
    if (!g_proc || destroyed) return 0;
    u16 w[5] = { (u16)GHWND, (u16)msg, (u16)wp, (u16)(lp >> 16), (u16)lp };
    return guest_call16(g_proc, 5, w);
}

/* ---------------- host window ---------------- */
static u32 shots[64];
static int nshots, shot_next;
typedef struct { u32 t; int x, y, kind; u32 vk; } Script;
static Script script[256];
static int nscript, script_next;
static u32 quit_at;

static void script_init(void)
{
    const char *s = getenv(ENV("SHOT"));
    while (s && *s && nshots < 64) { shots[nshots++] = (u32)atoi(s); s = strchr(s, ';'); if (s) s++; }
    s = getenv(ENV("CLICK"));
    while (s && *s && nscript < 256) {
        Script *e = &script[nscript++];
        e->kind = 0;
        sscanf(s, "%d,%d,%u", &e->x, &e->y, &e->t);
        s = strchr(s, ';'); if (s) s++;
    }
    s = getenv(ENV("KEYS"));
    while (s && *s && nscript < 256) {
        Script *e = &script[nscript++];
        e->kind = 1;
        sscanf(s, "%u:%u", &e->t, &e->vk);
        s = strchr(s, ';'); if (s) s++;
    }
    s = getenv(ENV("MOVE"));    /* RC_MOVE="x,y,ms;..." mouse moves (no button) */
    while (s && *s && nscript < 256) {
        Script *e = &script[nscript++];
        e->kind = 3;
        sscanf(s, "%d,%d,%u", &e->x, &e->y, &e->t);
        s = strchr(s, ';'); if (s) s++;
    }
    /* time order */
    for (int i = 0; i < nscript; i++)
        for (int j = i + 1; j < nscript; j++)
            if (script[j].t < script[i].t) { Script t = script[i]; script[i] = script[j]; script[j] = t; }
    if (getenv(ENV("QUIT"))) quit_at = (u32)atoi(getenv(ENV("QUIT")));
}

static void save_shot(u32 t);
static int have_frame;
int dlg_find_button(int n, int *x, int *y);
int dlg_option_at(int x, int y);
static void press_for_player(int bx, int by, int with_up);
static void script_poll(void)
{
    u32 t = win_time();
    {   /* RC_TRACEFROM=ms: API trace from that time on */
        static int tf = -2;
        extern int api_log_on;
        if (tf == -2) tf = getenv(ENV("TRACEFROM")) ? atoi(getenv(ENV("TRACEFROM"))) : -1;
        if (tf >= 0 && t >= (u32)tf) { api_log_on = 1; tf = -1; }
    }
    while (have_frame && shot_next < nshots && t >= shots[shot_next]) { save_shot(shots[shot_next]); shot_next++; }
    while (script_next < nscript && t >= script[script_next].t) {
        Script *e = &script[script_next++];
        int dbx, dby, dn;
        if (e->kind == 0 && (dn = dlg_option_at(e->x, e->y)) && dlg_find_button(dn, &dbx, &dby)) {
            rt_log("script click %d,%d at %u = dialogue option %d\n", e->x, e->y, t, dn);
            press_for_player(dbx, dby, 1);
        } else if (e->kind == 1 && ((e->vk >= '1' && e->vk <= '9') || (e->vk >= 0x61 && e->vk <= 0x69)) &&
                   dlg_find_button(e->vk >= 0x61 ? (int)e->vk - 0x60 : (int)e->vk - '0', &dbx, &dby)) {
            rt_log("script key %u at %u = dialogue option\n", e->vk, t);
            press_for_player(dbx, dby, 1);
        } else if (e->kind == 0) {
            gx = e->x; gy = e->y;
            u32 lp = (u32)(u16)e->x | ((u32)(u16)e->y << 16);
            msgq_post(GHWND, WM_MOUSEMOVE, 0, lp);
            msgq_post(GHWND, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            /* the button comes up 150 ms later, like a real click */
            if (nscript < 256) {
                int k = nscript++;
                while (k > script_next && script[k - 1].t > t + 150) { script[k] = script[k - 1]; k--; }
                script[k].t = t + 150; script[k].kind = 2; script[k].x = e->x; script[k].y = e->y; script[k].vk = 0;
            }
            rt_log("script click %d,%d at %u\n", e->x, e->y, t);
        } else if (e->kind == 2) {
            msgq_post(GHWND, WM_LBUTTONUP, 0, (u32)(u16)e->x | ((u32)(u16)e->y << 16));
        } else if (e->kind == 3) {
            gx = e->x; gy = e->y;
            msgq_post(GHWND, WM_MOUSEMOVE, 0, (u32)(u16)e->x | ((u32)(u16)e->y << 16));
        } else {
            msgq_post(GHWND, WM_KEYDOWN, e->vk, 1);
            msgq_post(GHWND, WM_KEYUP, e->vk, 0xc0000001u);
            rt_log("script key %u at %u\n", e->vk, t);
        }
    }
    if (quit_at && t >= quit_at) {
        rt_log("quit time reached\n");
        if (rt_logf) fflush(rt_logf);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

static u32 *frame;
static int frames;

/* dialogue menus (dialog_input.c): a click on an option's text or its
   number key presses the option's button - the game reads the cursor
   position while it handles the press, so it sees the button's position */
int dlg_find_button(int n, int *x, int *y);
int dlg_option_at(int x, int y);
static int ovr_on, ovr_x, ovr_y, ovr_up_pending;
static u32 ovr_until;

static void post_at(u32 msg, u32 wp, int x, int y)
{
    int sx = gx, sy = gy;
    gx = x; gy = y;
    msgq_post(GHWND, msg, wp, (u32)(u16)x | ((u32)(u16)y << 16));
    gx = sx; gy = sy;
}

static void press_for_player(int bx, int by, int with_up)
{
    ovr_on = 1; ovr_x = bx; ovr_y = by;
    ovr_until = now_ms() + (with_up ? 400 : 10000);
    post_at(WM_MOUSEMOVE, 0, bx, by);
    post_at(WM_LBUTTONDOWN, MK_LBUTTON, bx, by);
    if (with_up) post_at(WM_LBUTTONUP, 0, bx, by);
    rt_log("dialogue: pressed the option button at %d,%d\n", bx, by);
}

/* Video > Zoom to the game area: the game draws only into this box of its
   640x480 screen (a white frame line on black around it) */
extern int opt_zoom;
#define ZX 64
#define ZY 48
#define ZW 512
#define ZH 384
static u32 *zframe;

/* Video > Widescreen fill: the 4:3 picture stays sharp in the middle; the
   sides show the same picture zoomed, blurred and darkened (no stretching) */
extern int opt_widefill, opt_aspect;
static u32 *wframe;
static int last_ext, last_bw;      /* side columns added, base picture width (mouse) */

static int wide_ext(int bw, int bh)
{
    if (!opt_widefill || opt_aspect == 1 || !hwnd) return 0;
    RECT r;
    GetClientRect(hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return 0;
    double a = (double)r.right / r.bottom;
    if (a > 21.0 / 9.0) a = 21.0 / 9.0;
    int e = (int)((a * bh - bw) / 2.0 + 0.5);
    return e >= 4 ? e : 0;
}

static const u32 *widen(const u32 *p, int *w, int h)
{
    int bw = *w, e = wide_ext(bw, h);
    last_ext = e; last_bw = bw;
    if (!e) return p;
    int W = bw + 2 * e;
    static int cap;
    if (W * h > cap) { free(wframe); cap = W * h; wframe = malloc((size_t)cap * 4); }
    /* the picture averaged down to 32 x 24 cells */
    enum { SW = 32, SH = 24 };
    float sm[SH][SW][3];
    for (int cy = 0; cy < SH; cy++)
        for (int cx = 0; cx < SW; cx++) {
            int x0 = cx * bw / SW, x1 = (cx + 1) * bw / SW, y0 = cy * h / SH, y1 = (cy + 1) * h / SH;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) { u32 c = p[y * bw + x]; r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++; }
            if (!n) n = 1;
            sm[cy][cx][0] = (float)r / n; sm[cy][cx][1] = (float)g / n; sm[cy][cx][2] = (float)b / n;
        }
    /* two 3x3 box blur passes over the cells: no stripes from small details */
    for (int pass = 0; pass < 2; pass++) {
        static float t[SH][SW][3];
        for (int cy = 0; cy < SH; cy++)
            for (int cx = 0; cx < SW; cx++)
                for (int ch = 0; ch < 3; ch++) {
                    float sum = 0;
                    int n = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int yy = cy + dy, xx = cx + dx;
                            if (yy < 0 || yy >= SH || xx < 0 || xx >= SW) continue;
                            sum += sm[yy][xx][ch];
                            n++;
                        }
                    t[cy][cx][ch] = sum / n;
                }
        memcpy(sm, t, sizeof sm);
    }
    /* sides: the picture zoomed to cover the wide frame, sampled smoothly, darkened */
    double z = (double)W / bw;
    for (int y = 0; y < h; y++) {
        u32 *row = wframe + (size_t)y * W;
        memcpy(row + e, p + (size_t)y * bw, (size_t)bw * 4);
        double v = ((y - h / 2.0) / z + h / 2.0) / h * SH - 0.5;
        if (v < 0) v = 0; if (v > SH - 1) v = SH - 1;
        int v0 = (int)v, v1 = v0 + 1 < SH ? v0 + 1 : v0;
        float fv = (float)(v - v0);
        for (int x = 0; x < W; x++) {
            if (x == e) { x = e + bw - 1; continue; }
            double u = ((x - W / 2.0) / z + bw / 2.0) / bw * SW - 0.5;
            if (u < 0) u = 0; if (u > SW - 1) u = SW - 1;
            int u0 = (int)u, u1 = u0 + 1 < SW ? u0 + 1 : u0;
            float fu = (float)(u - u0);
            /* darker towards the outer edges */
            float d = x < e ? (float)x / e : (float)(W - 1 - x) / e;
            float k = 0.25f + 0.25f * d;
            u32 c = 0;
            for (int ch = 0; ch < 3; ch++) {
                float a = sm[v0][u0][ch] * (1 - fu) + sm[v0][u1][ch] * fu;
                float b2 = sm[v1][u0][ch] * (1 - fu) + sm[v1][u1][ch] * fu;
                int val = (int)((a * (1 - fv) + b2 * fv) * k);
                c |= (u32)(val > 255 ? 255 : val) << (16 - 8 * ch);
            }
            row[x] = c;
        }
    }
    *w = W;
    return wframe;
}

/* the picture shown: the whole screen or the zoomed game area (+ wide sides) */
static const u32 *shown(int *w, int *h)
{
    if (!opt_zoom) { *w = 640; *h = 480; return widen(frame, w, 480); }
    if (!zframe) zframe = calloc(ZW * ZH, 4);
    for (int y = 0; y < ZH; y++) memcpy(zframe + y * ZW, frame + (ZY + y) * 640 + ZX, ZW * 4);
    *w = ZW; *h = ZH;
    return widen(zframe, w, ZH);
}

static int present_cur(void)
{
    int w, h;
    const u32 *p = shown(&w, &h);
    return present_rgb(p, w, h);
}

static void paint_gdi(HDC dc)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h, sw, sh;
    present_dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    const u32 *p = shown(&sw, &sh);
    BITMAPINFOHEADER bmi = { sizeof bmi, sw, -sh, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, sw, sh, p, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* the zoom option changed: show the last picture again */
void win_refresh(void)
{
    if (hwnd && frame) InvalidateRect(hwnd, NULL, FALSE);
}

static void save_shot(u32 t)
{
    {
        int nz = 0; u32 ps = 0;
        for (int i = 0; i < 640 * 480; i++) nz += screen8[i] != 0;
        for (int i = 0; i < 256; i++) ps += (screen_pal[i] & 0xff) + ((screen_pal[i] >> 8) & 0xff) + ((screen_pal[i] >> 16) & 0xff);
        rt_log("shot %u: screen8 nonzero %d, palette sum %u\n", t, nz, ps);
        if (getenv(ENV("DUMPSCREEN"))) {
            char rp[760];
            snprintf(rp, sizeof rp, "%s\\shots\\screen_%06u.raw", rt_exe_dir, t);
            FILE *f = fopen(rp, "wb");
            if (f) { fwrite(screen8, 1, 640 * 480, f); fwrite(screen_pal, 4, 256, f); fclose(f); }
            void gdi_dump(const char *dir, u32 t);
            char dd[700];
            snprintf(dd, sizeof dd, "%s\\shots", rt_exe_dir);
            gdi_dump(dd, t);
        }
    }
    char dir[700], p[760];
    snprintf(dir, sizeof dir, "%s\\shots", rt_exe_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(p, sizeof p, "%s\\shot_%06u.bmp", dir, t);
    if (getenv(ENV("SHOTOUT")) && d3d_ok) {
        present_cur();
        present_shot(p);
        rt_log("shot %s (presented)\n", p);
        return;
    }
    FILE *f = fopen(p, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh2 = { 0x4d42, sizeof fh2 + 40 + 640 * 480 * 4, 0, 0, sizeof fh2 + 40 };
    BITMAPINFOHEADER ih = { 40, 640, -480, 1, 32, 0, 640 * 480 * 4, 0, 0, 0, 0 };
    fwrite(&fh2, sizeof fh2, 1, f);
    fwrite(&ih, 40, 1, f);
    fwrite(frame, 4, 640 * 480, f);
    fclose(f);
    rt_log("shot %s\n", p);
}

/* 640x480 8-bit with a palette (0x00BBGGRR entries) */
void present_frame(const u8 *pix, int pitch, const u32 *pal)
{
    if (!frame) frame = calloc(640 * 480, 4);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            u32 c = pal[pix[y * pitch + x]];
            frame[y * 640 + x] = ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
        }
    have_frame = 1;
    frames++;
    u32 t = win_time();
    if (getenv(ENV("CURSORCHECK"))) {   /* test: pink cursor pixels in each presented frame */
        int n = 0;
        for (int i = 0; i < 640 * 480; i++) {
            u32 c = frame[i];
            if (((c >> 16) & 255) > 120 && ((c >> 8) & 255) < 40 && (c & 255) > 50 && ((c >> 16) & 255) > (c & 255)) n++;
        }
        /* white pixels (the arrow cursors) within 24 px of the mouse */
        int wn = 0;
        for (int y = gy - 24; y <= gy + 24; y++)
            for (int x = gx - 24; x <= gx + 24; x++) {
                if (x < 0 || y < 0 || x >= 640 || y >= 480) continue;
                u32 c = frame[y * 640 + x];
                if ((c & 255) > 235 && ((c >> 8) & 255) > 235 && ((c >> 16) & 255) > 235) wn++;
            }
        extern int gdi_last_w, gdi_last_h;
        rt_log("frame %u cursorpx %d white %d at %d,%d last %dx%d\n", t, n, wn, gx, gy, gdi_last_w, gdi_last_h);
        int f0, f1;   /* RC_FRAMEDUMP=from,to (ms): every presented frame -> shots */
        if (getenv(ENV("FRAMEDUMP")) && sscanf(getenv(ENV("FRAMEDUMP")), "%d,%d", &f0, &f1) == 2 && (int)t >= f0 && (int)t <= f1)
            save_shot(t);
    }
    if (hwnd && !hidden && !IsIconic(hwnd)) {
        if (!(d3d_ok && present_cur())) {
            HDC dc = GetDC(hwnd);
            paint_gdi(dc);
            ReleaseDC(hwnd, dc);
        }
    }
    static u32 last_fps;
    static int last_frames;
    if (t - last_fps >= 5000) {
        rt_log("fps %.1f\n", (frames - last_frames) * 1000.0 / (t - last_fps));
        present_fps((frames - last_frames) * 1000.0 / (t - last_fps));
        last_fps = t;
        last_frames = frames;
    }
}

static int map_mouse(LPARAM lp)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    present_dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    int ox, oy;
    /* the wide sides are not part of the game: map onto the picture in the middle */
    if (last_ext && last_bw) {
        int W = last_bw + 2 * last_ext;
        int px = (mx - x) * W / (w ? w : 1) - last_ext;
        x = x + last_ext * w / W;
        w = last_bw * w / W;
        mx = x + px * w / last_bw;
    }
    if (opt_zoom) {
        ox = ZX + (mx - x) * ZW / (w ? w : 1);
        oy = ZY + (my - y) * ZH / (h ? h : 1);
        if (ox < ZX) ox = ZX; if (ox > ZX + ZW - 1) ox = ZX + ZW - 1;
        if (oy < ZY) oy = ZY; if (oy > ZY + ZH - 1) oy = ZY + ZH - 1;
    } else {
        ox = (mx - x) * 640 / (w ? w : 1);
        oy = (my - y) * 480 / (h ? h : 1);
    }
    if (ox < 0) ox = 0; if (ox > 639) ox = 639;
    if (oy < 0) oy = 0; if (oy > 479) oy = 479;
    gx = ox; gy = oy;
    return 1;
}

static u32 mk_keys(WPARAM wp)
{
    u32 k = 0;
    if (wp & MK_LBUTTON) k |= 1;
    if (wp & MK_RBUTTON) k |= 2;
    if (wp & MK_SHIFT) k |= 4;
    if (wp & MK_CONTROL) k |= 8;
    if (wp & MK_MBUTTON) k |= 0x10;
    return k;
}

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (have_frame && !(d3d_ok && present_cur())) paint_gdi(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: msgq_post(GHWND, WM_CLOSE, 0, 0); quit_requested = 1; return 0;
    case WM_ACTIVATEAPP: if (!hidden) msgq_post(GHWND, WM_ACTIVATEAPP, wp ? 1 : 0, 0); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { present_set_fullscreen(!opt_fullscreen); return 0; }
        if (wp == VK_F4) break;
        return 0;
    case WM_SYSCHAR: return 0;
    case WM_COMMAND: if (present_menu_command(LOWORD(wp))) { InvalidateRect(h, NULL, FALSE); return 0; } break;
    case WM_KEYDOWN:
        if (wp == VK_F9) { present_menu_command(1111); InvalidateRect(h, NULL, FALSE); return 0; }   /* Widescreen fill on / off */
        {   /* number keys (top row / keypad) choose a dialogue option */
            int n = (wp >= '1' && wp <= '9') ? (int)wp - '0' : (wp >= VK_NUMPAD1 && wp <= VK_NUMPAD9) ? (int)wp - VK_NUMPAD0 : 0;
            int bx, by;
            if (n && !(lp & (1 << 30)) && dlg_find_button(n, &bx, &by)) { press_for_player(bx, by, 1); return 0; }
        }
        keys[wp & 0xff] = 0x80; msgq_post(GHWND, WM_KEYDOWN, (u32)wp, (u32)lp); return 0;
    case WM_KEYUP: keys[wp & 0xff] = 0; msgq_post(GHWND, WM_KEYUP, (u32)wp, (u32)lp); return 0;
    case WM_CHAR: msgq_post(GHWND, WM_CHAR, (u32)wp, (u32)lp); return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
        map_mouse(lp);
        if (m == WM_LBUTTONDOWN) {
            int o = dlg_option_at(gx, gy), bx, by;
            if (o && dlg_find_button(o, &bx, &by)) { SetCapture(h); press_for_player(bx, by, 0); ovr_up_pending = 1; return 0; }
        }
        if (m == WM_LBUTTONUP && ovr_up_pending) {
            ReleaseCapture();
            ovr_up_pending = 0;
            post_at(WM_LBUTTONUP, 0, ovr_x, ovr_y);
            ovr_until = now_ms() + 400;
            return 0;
        }
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) SetCapture(h);
        if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) ReleaseCapture();
        msgq_post(GHWND, m, mk_keys(wp), (u32)(u16)gx | ((u32)(u16)gy << 16));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(cursor_shown || in_menu ? LoadCursorA(NULL, IDC_ARROW) : NULL); return TRUE; }
        break;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ENTERMENULOOP: in_menu = 1; break;
    case WM_EXITMENULOOP: in_menu = 0; break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

void win_create_host(const char *title, int w, int h)
{
    (void)w; (void)h;
    if (hwnd) return;
    hidden = getenv(ENV("HIDDEN")) != NULL;
    WNDCLASSA wc = { 0 };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = host_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = GAME_ID "PC";
    RegisterClassA(&wc);
    RECT wa;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int k = 1;
    while (640 * (k + 1) <= wa.right - wa.left - 40 && 480 * (k + 1) <= wa.bottom - wa.top - 80) k++;
    RECT r = { 0, 0, 640 * k, 480 * k };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    hwnd = CreateWindowExA(0, wc.lpszClassName, title && *title ? title : GAME_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    rt_log("window %dx%d (x%d)%s\n", 640 * k, 480 * k, k, hidden ? " hidden" : "");
    d3d_ok = present_init(hwnd);
    int ww, wh;
    const char *ws = getenv(ENV("WINSIZE"));
    if (ws && sscanf(ws, "%dx%d", &ww, &wh) == 2) {
        RECT c = { 0, 0, ww, wh };
        AdjustWindowRect(&c, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), GetMenu(hwnd) != NULL);
        SetWindowPos(hwnd, NULL, 0, 0, c.right - c.left, c.bottom - c.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (!hidden) { ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); }
}

/* wait: 1 = sleep a little when idle, 0 = no wait, -1 = input only (no
   presenting: the game asks for the cursor position in the middle of
   redrawing the scene and its cursor - showing that half-drawn screen made
   the cursor flash) */
int win_pump(int wait)
{
    MSG m;
    if (wait > 0 && qh == qt && !quit_posted) MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    script_poll();
    wave_poll();
    /* show the screen once the game has stopped drawing for a moment: it
       blits the scene and then its cursor over it; presenting in between
       made the cursor flash.  A screen that keeps changing is still shown
       at least every 50 ms. */
    (void)wait;
    if (screen_dirty) {
        extern u32 gdi_last_t;
        static u32 dirty_since;
        u32 now = timeGetTime();
        if (!dirty_since) dirty_since = now;
        extern int gdi_cursor_pending; extern u32 gdi_pending_t;
        int wait_cursor = gdi_cursor_pending && now - gdi_pending_t < 80;
        if (!wait_cursor && (now - gdi_last_t >= 6 || now - dirty_since >= 50)) { screen_present(); dirty_since = 0; }
    }
    return !quit_posted;
}

void win_get_cursor(int *x, int *y) { *x = gx; *y = gy; }

/* ---------------- USER natives ---------------- */
static void write_msg(u32 fp, GMsg *m)
{
    u32 a = FAR2LIN(fp);
    WR16(a, m->hwnd); WR16(a + 2, m->msg); WR16(a + 4, m->wp); WR32(a + 6, m->lp);
    WR32(a + 10, m->time); WR16(a + 14, m->x); WR16(a + 16, m->y);
}

int msgq_get(u32 msg_fp, u32 h, u32 lo, u32 hi, int remove)
{
    (void)h;
    if (qh != qt) {
        for (int i = qh; i != qt; i = (i + 1) % QN) {
            if ((lo || hi) && (q[i].msg < lo || q[i].msg > hi)) continue;
            GMsg m = q[i];
            if (remove) {
                for (int j = i; j != qh; j = (j + QN - 1) % QN) q[j] = q[(j + QN - 1) % QN];
                qh = (qh + 1) % QN;
            }
            write_msg(msg_fp, &m);
            return 1;
        }
    }
    if (quit_posted) {
        GMsg m = { 0, WM_QUIT, (u32)quit_code, 0, now_ms(), 0, 0 };
        write_msg(msg_fp, &m);
        return 1;
    }
    if (invalid && created && (!lo || (WM_PAINT >= lo && WM_PAINT <= hi))) {
        GMsg m = { GHWND, WM_PAINT, 0, 0, now_ms(), 0, 0 };
        write_msg(msg_fp, &m);
        return 1;
    }
    return 0;
}

u32 w16_PeekMessage(u32 msg, u16 h, u16 lo, u16 hi, u16 flags)
{
    win_pump(0);
    int r = msgq_get(msg, h, lo, hi, flags & 1);
    if (!r) {
        /* a polling loop: give the host some breathing room */
        static u32 idle;
        if (++idle % 64 == 0) Sleep(1);
    }
    return r;
}

u32 w16_TranslateMessage(u32 msg) { (void)msg; return 0; }
u32 w16_DispatchMessage(u32 msg)
{
    u32 a = FAR2LIN(msg);
    u32 m = RD16(a + 2), wp = RD16(a + 4), lp = RD32(a + 6);
    if (m == WM_QUIT) return 0;
    if (getenv(ENV("CLICKDEBUG")) && (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP)) {
        u32 dg = sel_base[SEL_IDX(g_dgroup)];
        u16 sel16 = RD16(dg + 0x2016), s18 = RD16(dg + 0x2018);
        u32 pane = RD32(sel_base[SEL_IDX(sel16)] + 0x197a);
        u32 hot = RD32(sel_base[SEL_IDX(s18)] + 0x1b24);
        u32 fl = pane ? RD32(FAR2LIN(pane) + 0x58) : 0;
        rt_log("click msg %x at %d,%d: pane %08x flags %08x captured hotspot %08x\n", m, gx, gy, pane, fl, hot);
        if (pane) {
            u32 pl = FAR2LIN(pane);
            rt_log("  pane rect %d,%d - %d,%d\n", (s16)RD16(pl + 0x2c), (s16)RD16(pl + 0x2e), (s16)RD16(pl + 0x30), (s16)RD16(pl + 0x32));
            /* its hotspots: list from pane+0x18, next = first dword */
            u32 hp = RD32(FAR2LIN(RD32(pl + 0x18)));
            for (int i = 0; hp && i < 60; i++) {
                u32 h = FAR2LIN(hp);
                rt_log("  hs %08x flags %08x click %d,%d-%d,%d cur %d,%d-%d,%d id %u\n", hp, RD32(h + 0x30),
                       (s16)RD16(h + 0x1c), (s16)RD16(h + 0x1e), (s16)RD16(h + 0x20), (s16)RD16(h + 0x22),
                       (s16)RD16(h + 0x24), (s16)RD16(h + 0x26), (s16)RD16(h + 0x28), (s16)RD16(h + 0x2a), RD16(h + 0x38));
                hp = RD32(h);
            }
        }
    }
    return send(m, wp, lp);
}

static void destroy(void)
{
    if (destroyed) return;
    send(WM_DESTROY, 0, 0);
    destroyed = 1;
}

u32 w16_DefWindowProc(u16 h, u16 msg, u16 wp, u32 lp)
{
    (void)h; (void)wp; (void)lp;
    switch (msg) {
    case WM_CLOSE: destroy(); return 0;
    case WM_PAINT: invalid = 0; return 0;
    case WM_ERASEBKGND: return 1;
    }
    return 0;
}

u32 w16_PostQuitMessage(u16 code) { msgq_quit(code); return 0; }
u32 w16_InitApp(u16 h) { (void)h; return 1; }
u32 w16_GetTickCount(void) { void rt_idle_tick(void); rt_idle_tick(); return now_ms(); }
u32 w16_GetMessageTime(void) { return now_ms(); }
u32 w16_GetMessagePos(void) { return (u32)(u16)gx | ((u32)(u16)gy << 16); }

u32 w16_GetCursorPos(u32 pt)
{
    win_pump(-1);
    if (getenv(ENV("LOOPDEBUG"))) {   /* who polls the cursor: two frames up the BP chain */
        static u32 n;
        if ((n++ & 4095) == 0) {
            u32 ss = cpu.sbase[S_SS], bp = (u16)cpu.r[R_EBP];
            u32 r1 = RD16(ss + (u16)(bp + 2)) | ((u32)RD16(ss + (u16)(bp + 4)) << 16);
            u32 bp2 = RD16(ss + bp);
            u32 r2 = RD16(ss + (u16)(bp2 + 2)) | ((u32)RD16(ss + (u16)(bp2 + 4)) << 16);
            u32 bp3 = RD16(ss + bp2);
            u32 r3 = RD16(ss + (u16)(bp3 + 2)) | ((u32)RD16(ss + (u16)(bp3 + 4)) << 16);
            extern u32 rt_api_ret;
            rt_log("cursorpoll %u: api ret %08x  frames %08x %08x %08x\n", now_ms(), rt_api_ret, r1, r2, r3);
        }
    }
    u32 a = FAR2LIN(pt);
    if (ovr_on && (s32)(now_ms() - ovr_until) < 0) { WR16(a, ovr_x); WR16(a + 2, ovr_y); return 1; }
    ovr_on = 0;
    WR16(a, gx); WR16(a + 2, gy);
    { extern int api_log_on; if (api_log_on) rt_log("  cursor %d,%d\n", gx, gy); }
    return 1;
}
u32 w16_ScreenToClient(u16 h, u32 pt) { (void)h; (void)pt; return 0; }

u32 w16_RegisterClass(u32 wc)
{
    u32 a = FAR2LIN(wc);
    if (nclasses >= 8) return 0;
    WClass *c = &classes[nclasses++];
    c->style = RD16(a);
    c->proc = RD32(a + 2);
    c->hcursor = RD16(a + 14);
    c->hbr = RD16(a + 16);
    snprintf(c->name, sizeof c->name, "%s", gstr(RD32(a + 22)) ? gstr(RD32(a + 22)) : "");
    rt_log("RegisterClass %s proc %04x:%04x style %x\n", c->name, c->proc >> 16, c->proc & 0xffff, c->style);
    return 0xc000 + nclasses;
}

u32 w16_CreateWindow(u32 cls, u32 title, u32 style, s16 x, s16 y, s16 w, s16 h, u16 parent, u16 menu, u16 inst, u32 param)
{
    (void)parent; (void)menu; (void)inst; (void)param;
    const char *cn = (cls >> 16) ? gstr(cls) : "";
    for (int i = 0; i < nclasses; i++)
        if (!_stricmp(classes[i].name, cn)) g_proc = classes[i].proc;
    if (!g_proc && nclasses) g_proc = classes[0].proc;
    g_x = x; g_y = y; g_w = w; g_h = h;
    rt_log("CreateWindow %s \"%s\" style %08x %d,%d %dx%d\n", cn, gstr(title) ? gstr(title) : "", style, x, y, w, h);
    t0 = now_ms();
    script_init();
    win_create_host(gstr(title), w, h);
    g_hwnd = GHWND;
    /* WM_CREATE with a CREATESTRUCT */
    static u32 cs_lin;
    if (!cs_lin) cs_lin = lowmem_alloc(64);
    static u16 cs_sel;
    if (!cs_sel) { cs_sel = sel_alloc(1); sel_set(cs_sel, cs_lin, 63); }
    WR32(cs_lin, param); WR16(cs_lin + 4, inst); WR16(cs_lin + 6, menu); WR16(cs_lin + 8, parent);
    WR16(cs_lin + 10, h); WR16(cs_lin + 12, w); WR16(cs_lin + 14, y); WR16(cs_lin + 16, x);
    WR32(cs_lin + 18, style); WR32(cs_lin + 22, title); WR32(cs_lin + 26, cls);
    created = 1;
    u32 r = send(WM_CREATE, 0, (u32)cs_sel << 16);
    if ((s32)r == -1 || (r & 0xffff) == 0xffff) { rt_log("WM_CREATE failed\n"); return 0; }
    send(WM_SIZE, 0, (u32)(u16)w | ((u32)(u16)h << 16));
    send(WM_MOVE, 0, (u32)(u16)x | ((u32)(u16)y << 16));
    return GHWND;
}

u32 w16_ShowWindow(u16 h, u16 cmd)
{
    (void)h;
    rt_log("ShowWindow %d\n", cmd);
    send(WM_SHOWWINDOW, 1, 0);
    send(WM_ACTIVATEAPP, 1, 0);
    send(WM_ACTIVATE, 1, 0);
    send(WM_SETFOCUS, 0, 0);
    inv_add(0, 0, 640, 480);
    return 0;
}

u32 w16_UpdateWindow(u16 h)
{
    (void)h;
    if (invalid && created) send(WM_PAINT, 0, 0);
    return 0;
}

u32 w16_InvalidateRect(u16 h, u32 r, u16 erase)
{
    (void)h; (void)erase;
    extern u32 rt_api_ret;
    if (getenv(ENV("BLITLOG"))) rt_log("InvalidateRect %08x from %04x:%04x t %u\n", r, rt_api_ret >> 16, rt_api_ret & 0xffff, timeGetTime());
    if (r) {
        u32 a = FAR2LIN(r);
        inv_add((s16)RD16(a), (s16)RD16(a + 2), (s16)RD16(a + 4), (s16)RD16(a + 6));
    } else inv_add(0, 0, 640, 480);
    return 0;
}
u32 w16_ValidateRect(u16 h, u32 r)
{
    (void)h;
    if (!r) { invalid = 0; return 0; }
    u32 a = FAR2LIN(r);
    /* a rectangle covering the whole update region validates it */
    if ((s16)RD16(a) <= inv_l && (s16)RD16(a + 2) <= inv_t && (s16)RD16(a + 4) >= inv_r && (s16)RD16(a + 6) >= inv_b) invalid = 0;
    return 0;
}

u16 gdi_window_dc(void);
u32 w16_BeginPaint(u16 h, u32 ps)
{
    (void)h;
    u32 a = FAR2LIN(ps);
    u16 gdi_paint_dc(int l, int t, int r, int b);
    u16 dc = invalid ? gdi_paint_dc(inv_l, inv_t, inv_r, inv_b) : gdi_paint_dc(0, 0, 0, 0);
    memset(MEM + a, 0, 32);
    WR16(a, dc);
    WR16(a + 2, 0);
    if (invalid) { WR16(a + 4, inv_l); WR16(a + 6, inv_t); WR16(a + 8, inv_r); WR16(a + 10, inv_b); }
    else { WR16(a + 4, 0); WR16(a + 6, 0); WR16(a + 8, 0); WR16(a + 10, 0); }
    invalid = 0;
    return dc;
}
u32 w16_EndPaint(u16 h, u32 ps) { (void)h; (void)ps; return 0; }
u32 w16_GetDC(u16 h) { (void)h; return gdi_window_dc(); }
u32 w16_ReleaseDC(u16 h, u16 dc) { (void)h; (void)dc; return 1; }

static int show_count = 0;
u32 w16_ShowCursor(u16 show)
{
    show_count += show ? 1 : -1;
    cursor_shown = show_count >= 0;
    if (hwnd) { POINT p; GetCursorPos(&p); SetCursorPos(p.x, p.y); }
    return (u32)(u16)show_count;
}
u32 w16_LoadCursor(u16 inst, u32 name) { (void)inst; return 0x7000 | (name & 0xfff); }
u32 w16_SetCursor(u16 c) { static u16 cur; u16 o = cur; cur = c; return o; }

u32 w16_MessageBox(u16 h, u32 text, u32 cap, u16 type)
{
    (void)h;
    const char *t = gstr(text) ? gstr(text) : "", *c = gstr(cap) ? gstr(cap) : "";
    rt_log("MessageBox \"%s\": %s\n", c, t);
    if (hidden) return 1;
    return MessageBoxA(hwnd, t, c, type & 0xff);
}

/* rectangles (RECT16: left, top, right, bottom as s16) */
#define RL(a, i) ((s16)RD16((a) + 2 * (i)))
u32 w16_SetRect(u32 r, s16 l, s16 t, s16 rr, s16 b) { u32 a = FAR2LIN(r); WR16(a, l); WR16(a + 2, t); WR16(a + 4, rr); WR16(a + 6, b); return 1; }
u32 w16_CopyRect(u32 d, u32 s) { memcpy(FARPTR(d), FARPTR(s), 8); return 1; }
u32 w16_OffsetRect(u32 r, s16 dx, s16 dy)
{
    u32 a = FAR2LIN(r);
    WR16(a, RL(a, 0) + dx); WR16(a + 2, RL(a, 1) + dy); WR16(a + 4, RL(a, 2) + dx); WR16(a + 6, RL(a, 3) + dy);
    return 1;
}
u32 w16_IntersectRect(u32 d, u32 s1, u32 s2)
{
    u32 a = FAR2LIN(s1), b = FAR2LIN(s2), o = FAR2LIN(d);
    s16 l = RL(a, 0) > RL(b, 0) ? RL(a, 0) : RL(b, 0), t = RL(a, 1) > RL(b, 1) ? RL(a, 1) : RL(b, 1);
    s16 r = RL(a, 2) < RL(b, 2) ? RL(a, 2) : RL(b, 2), bo = RL(a, 3) < RL(b, 3) ? RL(a, 3) : RL(b, 3);
    if (l >= r || t >= bo) { WR32(o, 0); WR32(o + 4, 0); return 0; }
    WR16(o, l); WR16(o + 2, t); WR16(o + 4, r); WR16(o + 6, bo);
    return 1;
}
u32 w16_UnionRect(u32 d, u32 s1, u32 s2)
{
    u32 a = FAR2LIN(s1), b = FAR2LIN(s2), o = FAR2LIN(d);
    int ea = RL(a, 0) >= RL(a, 2) || RL(a, 1) >= RL(a, 3), eb = RL(b, 0) >= RL(b, 2) || RL(b, 1) >= RL(b, 3);
    if (ea && eb) { WR32(o, 0); WR32(o + 4, 0); return 0; }
    if (ea) { memmove(MEM + o, MEM + b, 8); return 1; }
    if (eb) { memmove(MEM + o, MEM + a, 8); return 1; }
    s16 l = RL(a, 0) < RL(b, 0) ? RL(a, 0) : RL(b, 0), t = RL(a, 1) < RL(b, 1) ? RL(a, 1) : RL(b, 1);
    s16 r = RL(a, 2) > RL(b, 2) ? RL(a, 2) : RL(b, 2), bo = RL(a, 3) > RL(b, 3) ? RL(a, 3) : RL(b, 3);
    WR16(o, l); WR16(o + 2, t); WR16(o + 4, r); WR16(o + 6, bo);
    return 1;
}
