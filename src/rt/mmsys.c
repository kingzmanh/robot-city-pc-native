/* MMSYSTEM: waveOut on the Windows waveOut device, timeGetTime.
 *
 * The game's buffers (WAVEHDR in guest memory) are copied to host headers
 * when written; wave_poll() (run whenever the game pumps messages) marks
 * finished ones done and delivers the game's callback (window message or
 * callback function) like Windows 3.1 did.  RC_MUTE=1: no device, buffers
 * finish at their real playing time.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define HWO 0x0700
typedef struct { u32 ghdr; WAVEHDR h; u8 *buf; u32 done_at; int used; } Pend;
#define NP 64
static Pend pend[NP];
static HWAVEOUT hwo;
static int open_, mute = -1;
static u32 cb, cb_inst, cb_flags;
static u32 avg_bps = 22050;
static u32 mute_clock;            /* muted: when the device finishes everything queued */
static u32 bytes_done;            /* muted position */
static u32 open_time;

u32 w16_timeGetTime(void) { return timeGetTime(); }
u32 w16_waveOutGetNumDevs(void) { return 1; }
u32 w16_waveOutGetID(u16 h, u32 id) { (void)h; if (id) WR16(FAR2LIN(id), 0); return 0; }
u32 w16_waveOutSetVolume(u16 dev, u32 v) { (void)dev; if (open_ && hwo && !mute) waveOutSetVolume(hwo, v); return 0; }

u32 w16_waveOutGetDevCaps(u16 dev, u32 caps, u16 size)
{
    (void)dev;
    u8 c[48] = { 0 };
    c[0] = 1; c[2] = 1; c[4] = 0x00; c[5] = 0x03;
    memcpy(c + 6, "Wave Out", 9);
    c[38] = 0xff; c[39] = 0x0f;          /* dwFormats: all 1/2/4 x 8/16 x mono/stereo */
    c[42] = 2;                            /* wChannels */
    c[44] = 0x0c;                         /* dwSupport: volume, L/R volume */
    memcpy(FARPTR(caps), c, size < 48 ? size : 48);
    return 0;
}

u32 w16_waveOutOpen(u32 phwo, u16 dev, u32 fmt, u32 callback, u32 inst, u32 flags)
{
    (void)dev;
    if (mute < 0) mute = getenv(ENV("MUTE")) != NULL;
    u32 a = FAR2LIN(fmt);
    WAVEFORMATEX wf = { 0 };
    wf.wFormatTag = RD16(a);
    wf.nChannels = RD16(a + 2);
    wf.nSamplesPerSec = RD32(a + 4);
    wf.nAvgBytesPerSec = RD32(a + 8);
    wf.nBlockAlign = RD16(a + 12);
    wf.wBitsPerSample = wf.wFormatTag == 1 ? RD16(a + 14) : 8;
    rt_log("waveOutOpen %u Hz %u ch %u bit flags %x cb %08x\n", wf.nSamplesPerSec, wf.nChannels, wf.wBitsPerSample, flags, callback);
    if (flags & 1) return 0;               /* WAVE_FORMAT_QUERY */
    if (open_) return 4;                   /* MMSYSERR_ALLOCATED */
    if (!mute && waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        rt_log("waveOutOpen: host device failed, running muted\n");
        mute = 1;
    }
    avg_bps = wf.nAvgBytesPerSec ? wf.nAvgBytesPerSec : 22050;
    open_ = 1;
    cb = callback; cb_inst = inst; cb_flags = flags & 0x70000;
    open_time = timeGetTime();
    mute_clock = 0;
    bytes_done = 0;
    if (phwo) WR16(FAR2LIN(phwo), HWO);
    return 0;
}

static void deliver(u32 msg, u32 p1)
{
    if (cb_flags == 0x10000) msgq_post(cb & 0xffff, msg, HWO, p1);              /* CALLBACK_WINDOW */
    else if (cb_flags == 0x30000 && cb) {                                       /* CALLBACK_FUNCTION */
        u16 w[8] = { HWO, (u16)msg, (u16)(cb_inst >> 16), (u16)cb_inst, (u16)(p1 >> 16), (u16)p1, 0, 0 };
        guest_call16(cb, 8, w);
    }
}

static void finish(Pend *p)
{
    u32 a = FAR2LIN(p->ghdr);
    WR32(a + 16, (RD32(a + 16) & ~0x10u) | 1u);   /* ~INQUEUE, DONE */
    u32 g = p->ghdr;
    if (!mute) waveOutUnprepareHeader(hwo, &p->h, sizeof p->h);
    free(p->buf);
    p->used = 0;
    deliver(0x3bd, g);                          /* MM_WOM_DONE */
}

void wave_poll(void)
{
    if (!open_) return;
    static int busy;
    if (busy) return;
    busy = 1;
    u32 t = timeGetTime();
    /* in queue order */
    for (;;) {
        Pend *first = NULL;
        for (int i = 0; i < NP; i++)
            if (pend[i].used && (!first || (s32)(pend[i].done_at - first->done_at) < 0)) first = &pend[i];
        if (!first) break;
        int done = mute ? (s32)(t - first->done_at) >= 0 : (first->h.dwFlags & WHDR_DONE) != 0;
        if (!done) break;
        if (mute) bytes_done += first->h.dwBufferLength;
        finish(first);
    }
    busy = 0;
}

u32 w16_waveOutPrepareHeader(u16 h, u32 hdr, u16 size) { (void)h; (void)size; u32 a = FAR2LIN(hdr); WR32(a + 16, RD32(a + 16) | 2); return 0; }
u32 w16_waveOutUnprepareHeader(u16 h, u32 hdr, u16 size) { (void)h; (void)size; u32 a = FAR2LIN(hdr); WR32(a + 16, RD32(a + 16) & ~2u); return 0; }

static u32 seq;
u32 w16_waveOutWrite(u16 h, u32 hdr, u16 size)
{
    (void)h; (void)size;
    if (!open_) return 5;
    u32 a = FAR2LIN(hdr);
    u32 len = RD32(a + 4);
    int i;
    for (i = 0; i < NP; i++) if (!pend[i].used) break;
    if (i == NP) { rt_log("waveOutWrite: too many buffers\n"); return 1; }
    Pend *p = &pend[i];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->ghdr = hdr;
    p->buf = malloc(len ? len : 1);
    memcpy(p->buf, FARPTR(RD32(a)), len);
    WR32(a + 16, (RD32(a + 16) & ~1u) | 0x10);   /* INQUEUE */
    u32 t = timeGetTime();
    if (mute) {
        if ((s32)(mute_clock - t) < 0) mute_clock = t;
        mute_clock += (u32)((u64)len * 1000 / avg_bps);
        p->done_at = mute_clock;
    } else {
        p->done_at = t + (seq++);    /* order key */
        p->h.lpData = (LPSTR)p->buf;
        p->h.dwBufferLength = len;
        waveOutPrepareHeader(hwo, &p->h, sizeof p->h);
        waveOutWrite(hwo, &p->h, sizeof p->h);
    }
    if (mute) p->h.dwBufferLength = len;
    return 0;
}

u32 w16_waveOutReset(u16 h)
{
    (void)h;
    if (!open_) return 0;
    if (!mute) waveOutReset(hwo);
    for (int i = 0; i < NP; i++) if (pend[i].used) { pend[i].h.dwFlags |= WHDR_DONE; pend[i].done_at = 0; }
    mute_clock = 0;
    for (int i = 0; i < NP; i++) if (pend[i].used) finish(&pend[i]);
    bytes_done = 0;
    return 0;
}

u32 w16_waveOutClose(u16 h)
{
    (void)h;
    if (!open_) return 0;
    w16_waveOutReset(h);
    if (!mute) waveOutClose(hwo);
    open_ = 0;
    deliver(0x3bc, 0);   /* MM_WOM_CLOSE */
    return 0;
}

u32 w16_waveOutGetPosition(u16 h, u32 mmt, u16 size)
{
    (void)h; (void)size;
    u32 a = FAR2LIN(mmt);
    u32 bytes;
    if (mute) {
        bytes = bytes_done;
        /* plus the part of the playing buffer */
        u32 t = timeGetTime();
        for (int i = 0; i < NP; i++)
            if (pend[i].used) {
                u32 dur = (u32)((u64)pend[i].h.dwBufferLength * 1000 / avg_bps);
                u32 start = pend[i].done_at - dur;
                if ((s32)(t - start) > 0 && (s32)(pend[i].done_at - t) > 0) bytes += (u32)((u64)(t - start) * avg_bps / 1000);
            }
    } else {
        MMTIME m = { TIME_BYTES };
        waveOutGetPosition(hwo, &m, sizeof m);
        bytes = m.u.cb;
    }
    u16 type = RD16(a);
    if (type == TIME_SAMPLES) WR32(a + 2, bytes / (avg_bps / 22050 ? avg_bps / 22050 : 1));
    else { WR16(a, TIME_BYTES); WR32(a + 2, bytes); }
    return 0;
}
