/* Internal interface of the PC runtime that replaces Windows 3.1 (KERNEL,
 * USER, GDI, MMSYSTEM, WinG) under the recompiled 16-bit game code
 * (ROBOTCTY.EXE, src/gen/rc). */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include "cpu.h"
#include "game.h"

#define ENV(x) GAME_ENV x

/* guest linear memory (MEM) */
#define GUEST_SIZE      0x04000000u      /* 64 MB */
#define SEG_LIN(i)      (0x00100000u + (u32)(i) * 0x10000u)   /* NE segment i */
#define HEAP_LIN        0x00800000u      /* GlobalAlloc */
#define LOW_LIN         0x00010000u      /* PSP, environment, strings */

/* selectors */
#define NSEL 8192
#define SEL_IDX(s) ((u16)(s) >> 3)
#define MKSEL(i) ((u16)(((i) << 3) | 7))
#define API_SEL 0xf7
#define MAGIC_SEL 0xfff7          /* return address of calls from natives into guest code */
extern u32 sel_base[NSEL], sel_limit[NSEL];
extern u8 sel_used[NSEL];
u16 sel_alloc(int n);             /* n consecutive selectors (__AHINCR = 8) */
void sel_free(u16 sel);
void sel_set(u16 sel, u32 base, u32 limit);
static inline u32 FAR2LIN(u32 fp) { return sel_base[SEL_IDX(fp >> 16)] + (fp & 0xffff); }
static inline void *FARPTR(u32 fp) { return fp ? PTR(FAR2LIN(fp)) : NULL; }
extern u16 g_dgroup, g_hinst, g_psp;

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
extern FILE *rt_logf;
extern char rt_root[512];      /* folder holding game\hd (C:) and game\cd (D:) */
extern char rt_exe_dir[512];

/* calls from native code into guest code: pascal, args left to right as words
   (a dword arg = two words: high first) */
u32 guest_call16(u32 fp, int nwords, const u16 *words);
void (*rt_lookup(u32 addr))(void);

/* natives (api_*.c): helpers on the guest stack */
#define SP16 ((u16)cpu.r[R_ESP])
#define STK(o) (cpu.sbase[2] + (u16)(SP16 + (o)))
static inline void set_ret(u32 v) { cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | (v & 0xffff); cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | (v >> 16); }

/* memory */
u32 gmem_alloc(u32 size, int flags);    /* -> selector (0 fail) */
u32 gmem_realloc(u16 sel, u32 size);
void gmem_free(u16 sel);
u32 gmem_size(u16 sel);
u32 lowmem_alloc(u32 size);              /* linear, never freed */
u32 gstrdup16(const char *s);            /* far pointer to a copy */

/* paths: guest DOS path -> host path */
void path_to_host(const char *g, char *out, size_t n);
const char *gstr(u32 fp);   /* far pointer -> C string (NULL for 0) */

/* DOS services (kernel.c) */
void dos_int21(void);

/* window (window.c) */
extern u32 g_hwnd;
extern int quit_requested;
void win_create_host(const char *title, int w, int h);
int win_pump(int wait);
void msgq_post(u32 hwnd, u32 msg, u32 wp, u32 lp);
int msgq_get(u32 msg_fp, u32 hwnd, u32 lo, u32 hi, int remove);
void msgq_quit(int code);
void win_cursor_show(int show);
void win_get_cursor(int *x, int *y);
u32 now_ms(void);

/* screen (gdi.c) */
extern u8 screen8[640 * 480];
extern u32 screen_pal[256];
extern int screen_dirty;
void screen_present(void);
void present_frame(const u8 *pix, int pitch, const u32 *pal);
int present_rgb(const u32 *pix, int sw, int sh);
void present_shot(const char *path);
void settings_load(void);
void present_set_fullscreen(int on);
int present_menu_command(int id);
void present_dest_rect(int cw, int ch, int *dx, int *dy, int *dw, int *dh);
void present_fps(double fps);
extern int opt_fullscreen;

typedef struct { const u32 *pix; int w, h; int x, y, rw, rh; int valid; } HiresLayer;
extern HiresLayer hires_layer;
u32 win_time(void);

/* audio (mmsys.c) */
void wave_poll(void);
