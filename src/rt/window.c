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
    /* time order */
    for (int i = 0; i < nscript; i++)
        for (int j = i + 1; j < nscript; j++)
            if (script[j].t < script[i].t) { Script t = script[i]; script[i] = script[j]; script[j] = t; }
    if (getenv(ENV("QUIT"))) quit_at = (u32)atoi(getenv(ENV("QUIT")));
}

static void save_shot(u32 t);
static int have_frame;
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
        if (e->kind == 0) {
            gx = e->x; gy = e->y;
            u32 lp = (u32)(u16)e->x | ((u32)(u16)e->y << 16);
            msgq_post(GHWND, WM_MOUSEMOVE, 0, lp);
            msgq_post(GHWND, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            msgq_post(GHWND, WM_LBUTTONUP, 0, lp);
            rt_log("script click %d,%d at %u\n", e->x, e->y, t);
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

static void paint_gdi(HDC dc)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    present_dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    BITMAPINFOHEADER bmi = { sizeof bmi, 640, -480, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, 640, 480, frame, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
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
        }
    }
    char dir[700], p[760];
    snprintf(dir, sizeof dir, "%s\\shots", rt_exe_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(p, sizeof p, "%s\\shot_%06u.bmp", dir, t);
    if (getenv(ENV("SHOTOUT")) && d3d_ok) {
        present_rgb(frame, 640, 480);
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
    if (hwnd && !hidden && !IsIconic(hwnd)) {
        if (!(d3d_ok && present_rgb(frame, 640, 480))) {
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
    int ox = (mx - x) * 640 / (w ? w : 1), oy = (my - y) * 480 / (h ? h : 1);
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
        if (have_frame && !(d3d_ok && present_rgb(frame, 640, 480))) paint_gdi(dc);
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
    case WM_KEYDOWN: keys[wp & 0xff] = 0x80; msgq_post(GHWND, WM_KEYDOWN, (u32)wp, (u32)lp); return 0;
    case WM_KEYUP: keys[wp & 0xff] = 0; msgq_post(GHWND, WM_KEYUP, (u32)wp, (u32)lp); return 0;
    case WM_CHAR: msgq_post(GHWND, WM_CHAR, (u32)wp, (u32)lp); return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
        map_mouse(lp);
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

int win_pump(int wait)
{
    MSG m;
    if (wait && qh == qt && !quit_posted) MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    script_poll();
    wave_poll();
    screen_present();
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
u32 w16_GetTickCount(void) { return now_ms(); }
u32 w16_GetMessageTime(void) { return now_ms(); }
u32 w16_GetMessagePos(void) { return (u32)(u16)gx | ((u32)(u16)gy << 16); }

u32 w16_GetCursorPos(u32 pt)
{
    win_pump(0);
    u32 a = FAR2LIN(pt);
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
    invalid = 1;
    return 0;
}

u32 w16_UpdateWindow(u16 h)
{
    (void)h;
    if (invalid && created) send(WM_PAINT, 0, 0);
    return 0;
}

u32 w16_InvalidateRect(u16 h, u32 r, u16 erase) { (void)h; (void)r; (void)erase; invalid = 1; return 0; }
u32 w16_ValidateRect(u16 h, u32 r) { (void)h; (void)r; invalid = 0; return 0; }

u16 gdi_window_dc(void);
u32 w16_BeginPaint(u16 h, u32 ps)
{
    (void)h;
    u32 a = FAR2LIN(ps);
    u16 dc = gdi_window_dc();
    memset(MEM + a, 0, 32);
    WR16(a, dc);
    WR16(a + 2, 0);
    WR16(a + 4, 0); WR16(a + 6, 0); WR16(a + 8, 640); WR16(a + 10, 480);
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
