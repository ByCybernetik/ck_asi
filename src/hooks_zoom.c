#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "tip_font.h"
#include "vk_present.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/*
 * BuildZoomMap @ 0x457d80 (cdecl, 4 args). Size math from disasm:
 *   lod = ceil_log2((cam.right-cam.left+1)/1024)
 *   W0  = (mgr.b0 - mgr.a8 + 1) >> lod     // with mgr=[0,0,32767,32767] → 1024
 *   H0  = ((b4*181>>8) - (ac*181>>8) + 1) >> lod  → 724
 *   temp bmp = (W0*2)×(H0*2) → FlatMiniMap → Distort → Shrink2x → final W0×H0
 *   aspect W0/H0 = 256/181 ≈ 1.414  (not soft 16:9)
 */
typedef void (*PFN_BuildZoom)(void *a0, void *a1, void *a2, void *a3);
static PFN_BuildZoom real_BuildZoom;
static BYTE *g_bz_tramp;
static BYTE g_bz_saved[16];
static SIZE_T g_bz_steal = 10;
static void *g_bz_target;
static volatile LONG g_bz_n;

static void try_patch_zoom_create_vt(void);

static void hook_BuildZoom(void *a0, void *a1, void *a2, void *a3)
{
    LONG n = InterlockedIncrement(&g_bz_n);
    unsigned a8, ac, b0, b4;
    int soft_w = 0, soft_h = 0;
    short bw = 0, bh = 0;
    void *mgr;
    void *zbmp;
    LONG cL = 0, cT = 0, cR = 0, cB = 0;
    char js[320];

    mm_read_mgr(&a8, &ac, &b0, &b4);
    mm_read_cam(&cL, &cT, &cR, &cB);
    (void)vk_present_soft_size(&soft_w, &soft_h);
    real_BuildZoom(a0, a1, a2, a3);
    mgr = *(void **)(ULONG_PTR)0x008C7B30u;
    zbmp = NULL;
    if (mgr && (ULONG_PTR)mgr > 0x10000u && !IsBadReadPtr((BYTE *)mgr + 0x34, 4)) {
        zbmp = *(void **)((BYTE *)mgr + 0x34);
        if (zbmp && !IsBadReadPtr(zbmp, 12)) {
            bw = *(short *)((BYTE *)zbmp + 8);
            bh = *(short *)((BYTE *)zbmp + 10);
        }
    }
    {
        unsigned wspan = (b0 >= a8) ? (b0 - a8 + 1u) : 0u;
        unsigned h_top = (ac * 181u) >> 8;
        unsigned h_bot = (b4 * 181u) >> 8;
        unsigned hspan = (h_bot >= h_top) ? (h_bot - h_top + 1u) : 0u;
        log_msg("BuildZoom #%ld mgr=[%u,%u,%u,%u] span=%ux%u foreshort=%u soft=%dx%d "
                "cam=[%ld..%ld,%ld..%ld] out_bmp=%dx%d aspect=%.3f soft_aspect=%.3f",
                (long)n, a8, ac, b0, b4, wspan, (b4 >= ac) ? (b4 - ac + 1u) : 0u, hspan, soft_w,
                soft_h, (long)cL, (long)cR, (long)cT, (long)cB, (int)bw, (int)bh,
                bh > 0 ? (double)bw / (double)bh : 0.0,
                soft_h > 0 ? (double)soft_w / (double)soft_h : 0.0);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"mgr\":[%u,%u,%u,%u],\"wspan\":%u,\"hspan_f\":%u,\"soft\":[%d,%d],"
                 "\"cam\":[%ld,%ld,%ld,%ld],\"out\":[%d,%d],"
                 "\"aspects\":{\"bmp\":%.4f,\"soft\":%.4f,\"play\":%.4f,\"stretch_if_fill\":%.4f}}",
                 (long)n, a8, ac, b0, b4, wspan, hspan, soft_w, soft_h, (long)cL, (long)cT,
                 (long)cR, (long)cB, (int)bw, (int)bh,
                 bh > 0 ? (double)bw / (double)bh : 0.0,
                 soft_h > 0 ? (double)soft_w / (double)soft_h : 0.0,
                 (soft_h > 160) ? (double)soft_w / (double)(soft_h - 80 - 54) : 0.0,
                 (bh > 0 && soft_h > 160)
                     ? ((double)soft_w / (double)(soft_h - 80 - 54)) / ((double)bw / (double)bh)
                     : 0.0);
        hooks_agent("H-T", "hooks.c:BuildZoom", "zoom-build", js);
    }
    /* After BuildZoom, retail @ 0x58074e calls zoom-ctrl vtable[0](softRect, bmp)
     * which stretches 1023×716 into 16:9 playfield. Patch vtable before Create. */
    try_patch_zoom_create_vt();
}

/*
 * Zoom controller @ [0x7A7D34] vtable[0](this, LT_packed, RB_packed, bmp):
 * binds BuildZoom bitmap into soft rect from [0x8DD084+0x10]. Without letterbox,
 * dest aspect ≈ soft playfield 1920/946 while bmp is 256/181 → oval icons (H-Y).
 */
typedef void *(__attribute__((thiscall)) *PFN_ZoomCreate)(void *self, DWORD lt, DWORD rb,
                                                          void *bmp);
static PFN_ZoomCreate real_ZoomCreate;
static volatile LONG g_zoom_vt_patched;
static volatile LONG g_zoom_lb_active;
static volatile DWORD g_zoom_end_tick; /* ZoomEndMode: suppress re-arm from late lb blits */
static volatile DWORD g_zoom_arm_tick; /* when pillars were armed (measure visible latency) */
static volatile LONG g_zoom_session;   /* increments on Create/Show (new zoom instance) */
static volatile LONG g_zoom_draw_seen; /* first letterboxed blit observed for this session */
static volatile LONG g_zoom_closed;    /* sticky: 1 after EndMode until live flag says open */
static void *g_zoom_mode_obj;          /* ZoomEndMode `this` — flag at this-0xA8 */
static short g_zoom_lb_play[4]; /* L,T,R,B playfield (pre-letterbox) */
static short g_zoom_lb_map[4];  /* L,T,R,B letterboxed map */
static volatile LONG g_tip_on;
static int g_tip_x0, g_tip_y0, g_tip_x1, g_tip_y1;
static int g_tip_draw_x, g_tip_draw_y;
static char g_tip_str[256];
static DWORD g_tip_tick;

enum { CK_ZOOM_END_SUPPRESS_MS = 48 }; /* late post-close lb is ~2ms; 500ms ate reopen blits */

/* Retail zoom-mode flag at (ZoomEndMode this)-0xA8. 0 after close; non-0 while map mode. */
static int zoom_mode_flag_on(void)
{
    BYTE *p;
    if (!g_zoom_mode_obj)
        return -1;
    p = (BYTE *)g_zoom_mode_obj - 0xA8;
    if (IsBadReadPtr(p, 4))
        return -1;
    return *(DWORD *)p != 0;
}

/* Arm pillar fill when letterbox gutters exist. Cleared by ZoomEndMode/Hide. */
static void ck_zoom_letterbox_arm_if_geometry(void)
{
    if (g_zoom_lb_map[2] <= g_zoom_lb_map[0])
        return;
    if (!(g_zoom_lb_map[0] > g_zoom_lb_play[0] || g_zoom_lb_map[2] < g_zoom_lb_play[2]))
        return;
    g_zoom_end_tick = 0;
    InterlockedExchange(&g_zoom_closed, 0);
    InterlockedExchange(&g_zoom_draw_seen, 0);
    InterlockedExchange(&g_zoom_lb_active, 1);
    g_zoom_arm_tick = GetTickCount();
}

int ck_zoom_letterbox_get(int *play_l, int *play_t, int *play_r, int *play_b, int *map_l,
                          int *map_t, int *map_r, int *map_b)
{
    /* H-Z: do NOT require mgr+0x34 — retail frees the zoom bmp right after ZoomCreate.
     * H-Z15: require active and retail zoom-mode flag (when known) — no fill outside map. */
    if (!g_zoom_lb_active || g_zoom_closed)
        return 0;
    if (zoom_mode_flag_on() == 0)
        return 0;
    if (play_l)
        *play_l = g_zoom_lb_play[0];
    if (play_t)
        *play_t = g_zoom_lb_play[1];
    if (play_r)
        *play_r = g_zoom_lb_play[2];
    if (play_b)
        *play_b = g_zoom_lb_play[3];
    if (map_l)
        *map_l = g_zoom_lb_map[0];
    if (map_t)
        *map_t = g_zoom_lb_map[1];
    if (map_r)
        *map_r = g_zoom_lb_map[2];
    if (map_b)
        *map_b = g_zoom_lb_map[3];
    return 1;
}

void ck_zoom_letterbox_clear(void)
{
    InterlockedExchange(&g_zoom_lb_active, 0);
    InterlockedExchange(&g_zoom_draw_seen, 0);
    InterlockedExchange(&g_tip_on, 0);
    g_tip_str[0] = 0;
}

void ck_zoom_letterbox_note_blit(int x, int y, int w, int h)
{
    int mL, mT, mR, mB;
    int map_w, map_h;
    int play_w, play_h;

    if (w < 8 || h < 8)
        return;
    if (g_zoom_lb_map[2] <= g_zoom_lb_map[0])
        return;
    mL = g_zoom_lb_map[0];
    mT = g_zoom_lb_map[1];
    mR = g_zoom_lb_map[2];
    mB = g_zoom_lb_map[3];
    map_w = mR - mL + 1;
    map_h = mB - mT + 1;
    play_w = g_zoom_lb_play[2] - g_zoom_lb_play[0] + 1;
    play_h = g_zoom_lb_play[3] - g_zoom_lb_play[1] + 1;
    if (map_w < 8 || map_h < 8)
        return;

    /* Full playfield while closed: keep closed; do NOT clear end_tick (H-Z15 —
     * clearing it allowed stale lb blits to re-arm fill outside the map). */
    if (g_zoom_closed && play_w > 8 && play_h > 8 && w * 10 >= play_w * 9 &&
        h * 10 >= play_h * 8 && x <= g_zoom_lb_play[0] + 8) {
        /* #region agent log */
        {
            static volatile LONG s_full_n;
            if (InterlockedIncrement(&s_full_n) <= 12) {
                char js[96];
                snprintf(js, sizeof(js), "{\"blit\":[%d,%d,%d,%d],\"closed\":1}", x, y, w, h);
                hooks_agent("H-Z15", "hooks.c:note_blit", "zoom-full-closed", js);
            }
        }
        /* #endregion */
        return;
    }

    /* Letterboxed ZoomMap blit — arm only while retail zoom-mode flag is on (H-Z15). */
    if (h * 10 >= map_h * 8 && w * 10 >= map_w * 8 && w * 10 <= map_w * 12 &&
        x >= mL - 8 && x <= mL + 8 && y >= mT - 8 && y <= mT + 8) {
        DWORD now = GetTickCount();
        int flag = zoom_mode_flag_on();

        if (g_zoom_end_tick && now - g_zoom_end_tick < (DWORD)CK_ZOOM_END_SUPPRESS_MS) {
            /* #region agent log */
            {
                static volatile LONG s_sup_n;
                if (InterlockedIncrement(&s_sup_n) <= 16) {
                    char js[128];
                    snprintf(js, sizeof(js),
                             "{\"blit\":[%d,%d,%d,%d],\"suppressed\":1,\"dt\":%lu,\"flag\":%d}", x, y,
                             w, h, (unsigned long)(now - g_zoom_end_tick), flag);
                    hooks_agent("H-Z15", "hooks.c:note_blit", "zoom-lb-suppress", js);
                }
            }
            /* #endregion */
            return;
        }

        /* After EndMode, flag is 0 — refuse stale lb blits that look like map draws. */
        if (g_zoom_closed && flag == 0) {
            /* #region agent log */
            {
                static volatile LONG s_rej_n;
                if (InterlockedIncrement(&s_rej_n) <= 16) {
                    char js[112];
                    snprintf(js, sizeof(js),
                             "{\"blit\":[%d,%d,%d,%d],\"rejected\":\"flag0\",\"closed\":1}", x, y, w,
                             h);
                    hooks_agent("H-Z15", "hooks.c:note_blit", "zoom-lb-reject", js);
                }
            }
            /* #endregion */
            return;
        }

        {
            LONG was = g_zoom_lb_active;
            long dt_end = g_zoom_end_tick ? (long)(now - g_zoom_end_tick) : -1L;
            g_zoom_end_tick = 0;
            InterlockedExchange(&g_zoom_closed, 0);
            InterlockedExchange(&g_zoom_lb_active, 1);
            if (!was)
                g_zoom_arm_tick = now;
            /* #region agent log */
            if (!g_zoom_draw_seen || !was) {
                InterlockedExchange(&g_zoom_draw_seen, 1);
                {
                    char js[176];
                    snprintf(js, sizeof(js),
                             "{\"blit\":[%d,%d,%d,%d],\"was\":%ld,\"active\":1,\"arm\":\"blit\","
                             "\"session\":%ld,\"dt_end\":%ld,\"flag\":%d}",
                             x, y, w, h, (long)was, (long)g_zoom_session, dt_end, flag);
                    hooks_agent("H-Z15", "hooks.c:note_blit", "zoom-lb-blit", js);
                }
            }
            /* #endregion */
        }
    }
}

static void zoom_letterbox_rect(short bw, short bh, short *L, short *T, short *R, short *B)
{
    int rw, rh, dw, dh, ox, oy;
    if (!L || !T || !R || !B || bw <= 0 || bh <= 0)
        return;
    rw = (int)*R - (int)*L + 1;
    rh = (int)*B - (int)*T + 1;
    if (rw < 8 || rh < 8)
        return;
    /* Prefer playfield band if rect covers full soft (top bar + bottom chrome). */
    if (*T <= 4 && rh >= 1000) {
        *T = 80;
        if (*B > 1025)
            *B = 1025;
        rh = (int)*B - (int)*T + 1;
    }
    if ((long)bw * rh <= (long)bh * rw) {
        dh = rh;
        dw = (int)((long)bw * rh / bh);
    } else {
        dw = rw;
        dh = (int)((long)bh * rw / bw);
    }
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;
    ox = (int)*L + (rw - dw) / 2;
    oy = (int)*T + (rh - dh) / 2;
    *L = (short)ox;
    *T = (short)oy;
    *R = (short)(ox + dw - 1);
    *B = (short)(oy + dh - 1);
}

static void *__attribute__((thiscall)) hook_ZoomCreate(void *self, DWORD lt, DWORD rb, void *bmp)
{
    short L = (short)(lt & 0xffff);
    short T = (short)((lt >> 16) & 0xffff);
    short R = (short)(rb & 0xffff);
    short B = (short)((rb >> 16) & 0xffff);
    short oL = L, oT = T, oR = R, oB = B;
    short bw = 0, bh = 0;
    DWORD nlt, nrb;
    char js[360];
    void *ret;

    if (bmp && !IsBadReadPtr(bmp, 12)) {
        bw = *(short *)((BYTE *)bmp + 8);
        bh = *(short *)((BYTE *)bmp + 10);
    }
    zoom_letterbox_rect(bw, bh, &L, &T, &R, &B);
    nlt = (DWORD)(unsigned short)L | ((DWORD)(unsigned short)T << 16);
    nrb = (DWORD)(unsigned short)R | ((DWORD)(unsigned short)B << 16);
    {
        long ow = (long)oR - (long)oL + 1, oh = (long)oB - (long)oT + 1;
        long nw = (long)R - (long)L + 1, nh = (long)B - (long)T + 1;
        log_msg("ZoomCreate bmp=%dx%d orig=(%d,%d)-(%d,%d) %ldx%ld -> (%d,%d)-(%d,%d) %ldx%ld "
                "aspect %.3f->%.3f",
                (int)bw, (int)bh, (int)oL, (int)oT, (int)oR, (int)oB, ow, oh, (int)L, (int)T,
                (int)R, (int)B, nw, nh, oh > 0 ? (double)ow / (double)oh : 0.0,
                nh > 0 ? (double)nw / (double)nh : 0.0);
        snprintf(js, sizeof(js),
                 "{\"bmp\":[%d,%d],\"orig\":[%d,%d,%d,%d],\"owh\":[%ld,%ld],"
                 "\"dst\":[%d,%d,%d,%d],\"dwh\":[%ld,%ld]}",
                 (int)bw, (int)bh, (int)oL, (int)oT, (int)oR, (int)oB, ow, oh, (int)L, (int)T,
                 (int)R, (int)B, nw, nh);
        /* #region agent log */
        hooks_agent("H-Y", "hooks.c:ZoomCreate", "zoom-create", js);
        /* #endregion */
    }
    g_zoom_lb_play[0] = oL;
    g_zoom_lb_play[1] = oT;
    g_zoom_lb_play[2] = oR;
    g_zoom_lb_play[3] = oB;
    g_zoom_lb_map[0] = L;
    g_zoom_lb_map[1] = T;
    g_zoom_lb_map[2] = R;
    g_zoom_lb_map[3] = B;
    /* H-Z14: Create runs at mission enter (before map open) — store geometry only.
     * Arming here painted side pillars over normal play until first map use. */
    InterlockedIncrement(&g_zoom_session);
    InterlockedExchange(&g_zoom_lb_active, 0);
    InterlockedExchange(&g_zoom_draw_seen, 0);
    /* #region agent log */
    {
        char js2[160];
        snprintf(js2, sizeof(js2),
                 "{\"active\":0,\"play\":[%d,%d,%d,%d],\"map\":[%d,%d,%d,%d],\"arm\":\"create-geom\"}",
                 (int)oL, (int)oT, (int)oR, (int)oB, (int)L, (int)T, (int)R, (int)B);
        hooks_agent("H-Z14", "hooks.c:ZoomCreate", "zoom-lb-active", js2);
    }
    /* #endregion */
    ret = real_ZoomCreate(self, nlt, nrb, bmp);
    return ret;
}

static void try_patch_zoom_create_vt(void)
{
    void *ctrl;
    void **vt;
    DWORD oldprot;

    ctrl = *(void **)(ULONG_PTR)0x007A7D34u;
    if (!ctrl || (ULONG_PTR)ctrl < 0x10000u || IsBadReadPtr(ctrl, 4))
        return;
    vt = *(void ***)ctrl;
    if (!vt || IsBadReadPtr(vt, 4))
        return;
    if (vt[0] == (void *)hook_ZoomCreate)
        return;
    if (!VirtualProtect(vt, sizeof(void *), PAGE_READWRITE, &oldprot))
        return;
    real_ZoomCreate = (PFN_ZoomCreate)vt[0];
    vt[0] = (void *)hook_ZoomCreate;
    VirtualProtect(vt, sizeof(void *), oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), vt, sizeof(void *));
    if (InterlockedExchange(&g_zoom_vt_patched, 1) == 0)
        log_msg("patched ZoomCreate vtable[0] @ %p -> %p (was %p)", (void *)vt,
                (void *)hook_ZoomCreate, (void *)real_ZoomCreate);
}

/* Mid-fn entry @ 0x580730: patch vtable before Create (BuildZoom may be skipped). */
static BYTE *g_zbind_tramp;
static BYTE g_zbind_saved[16];
static SIZE_T g_zbind_steal = 8;
static void *g_zbind_target;

static void zoom_bind_pre(void)
{
    try_patch_zoom_create_vt();
}

static void __attribute__((naked)) hook_ZoomBindEntry(void)
{
    __asm__ __volatile__(
        "pushal\n\t"
        "call %P0\n\t"
        "popal\n\t"
        "jmp *%1"
        :
        : "i"(zoom_bind_pre), "m"(g_zbind_tramp));
}

/* HideZoomMap @ 0x456a20 / ShowZoomMap @ 0x456a30 / Toggle @ 0x456a10 */
typedef unsigned (*PFN_ZoomCmd)(void);
static PFN_ZoomCmd real_HideZoomMap;
static PFN_ZoomCmd real_ShowZoomMap;
static PFN_ZoomCmd real_ToggleZoomMap;
static BYTE *g_hz_tramp, *g_sz_tramp, *g_tz_tramp;
static BYTE g_hz_saved[16], g_sz_saved[16], g_tz_saved[16];
static SIZE_T g_hz_steal = 6, g_sz_steal = 6, g_tz_steal = 6;
static void *g_hz_target, *g_sz_target, *g_tz_target;

/*
 * Zoom mode end @ 0x456990 (thiscall): clears object flag at this-0xA8 and
 * tears down UI tied to [7A7D38]. Retail close path (HideZoomMap script often
 * unused). Clear pillar fill here (H-Z9).
 */
typedef void(__attribute__((thiscall)) *PFN_ZoomEndMode)(void *self);
static PFN_ZoomEndMode real_ZoomEndMode;
static BYTE *g_zem_tramp;
static BYTE g_zem_saved[16];
static SIZE_T g_zem_steal = 9;
static void *g_zem_target;

static void __attribute__((thiscall)) hook_ZoomEndMode(void *self)
{
    g_zoom_mode_obj = self;
    g_zoom_end_tick = GetTickCount();
    InterlockedExchange(&g_zoom_closed, 1);
    ck_zoom_letterbox_clear();
    /* #region agent log */
    {
        char js[96];
        snprintf(js, sizeof(js), "{\"self\":%lu,\"flag_before\":%d}",
                 (unsigned long)(ULONG_PTR)self, zoom_mode_flag_on());
        hooks_agent("H-Z15", "hooks.c:ZoomEndMode", "zoom-end-mode", js);
    }
    /* #endregion */
    real_ZoomEndMode(self);
}

static int zoom_lb_geometry_ok(void)
{
    return g_zoom_lb_map[0] > g_zoom_lb_play[0] || g_zoom_lb_map[2] < g_zoom_lb_play[2];
}

static unsigned hook_HideZoomMap(void)
{
    unsigned r = real_HideZoomMap();
    g_zoom_end_tick = GetTickCount();
    InterlockedExchange(&g_zoom_closed, 1);
    ck_zoom_letterbox_clear();
    /* #region agent log */
    hooks_agent("H-Z", "hooks.c:HideZoom", "zoom-hide-clear", "{}");
    /* #endregion */
    return r;
}

static unsigned hook_ShowZoomMap(void)
{
    unsigned r = real_ShowZoomMap();
    /* Reopen without Create: arm from stored letterbox geometry (H-Z12). */
    if (zoom_lb_geometry_ok()) {
        InterlockedIncrement(&g_zoom_session);
        ck_zoom_letterbox_arm_if_geometry();
    }
    /* #region agent log */
    {
        char js[80];
        snprintf(js, sizeof(js), "{\"active\":%ld,\"arm\":\"show\",\"session\":%ld}",
                 (long)g_zoom_lb_active, (long)g_zoom_session);
        hooks_agent("H-Z12", "hooks.c:ShowZoom", "zoom-show", js);
    }
    /* #endregion */
    return r;
}

static unsigned hook_ToggleZoomMap(void)
{
    LONG was = g_zoom_lb_active;
    unsigned r = real_ToggleZoomMap();
    /* ShowZoom is unused on reopen (no zoom-show logs); Toggle may open the map. */
    if (!was && zoom_lb_geometry_ok()) {
        InterlockedIncrement(&g_zoom_session);
        ck_zoom_letterbox_arm_if_geometry();
    }
    /* #region agent log */
    {
        char js[96];
        snprintf(js, sizeof(js), "{\"was\":%ld,\"active\":%ld,\"arm\":\"toggle\"}", (long)was,
                 (long)g_zoom_lb_active);
        hooks_agent("H-Z13", "hooks.c:ToggleZoom", "zoom-toggle", js);
    }
    /* #endregion */
    return r;
}

/*
 * AttachZoomMap @ 0x661fd0 (thiscall, bmp*): sizes ZoomMap window from child chrome +
 * CBitmap wh, centers on main wnd. If final widget aspect != bmp aspect, UI stretch
 * into soft playfield makes circles oval (H-U).
 */
typedef void(__attribute__((thiscall)) *PFN_AttachZoom)(void *self, void *bmp);
static PFN_AttachZoom real_AttachZoom;
static BYTE *g_az_tramp;
static BYTE g_az_saved[16];
static SIZE_T g_az_steal = 8;
static void *g_az_target;
static volatile LONG g_az_n;

static void __attribute__((thiscall)) hook_AttachZoom(void *self, void *bmp)
{
    LONG n = InterlockedIncrement(&g_az_n);
    short bw = 0, bh = 0;
    short pL = 0, pT = 0, pR = 0, pB = 0;
    short cL = 0, cT = 0, cR = 0, cB = 0;
    int soft_w = 0, soft_h = 0;
    void *child = NULL;
    char js[420];

    try_patch_zoom_create_vt();
    if (bmp && !IsBadReadPtr(bmp, 12)) {
        bw = *(short *)((BYTE *)bmp + 8);
        bh = *(short *)((BYTE *)bmp + 10);
    }
    if (self && !IsBadReadPtr((BYTE *)self + 0x14, 4)) {
        pL = *(short *)((BYTE *)self + 0x10);
        pT = *(short *)((BYTE *)self + 0x12);
        pR = *(short *)((BYTE *)self + 0x14);
        pB = *(short *)((BYTE *)self + 0x16);
    }
    real_AttachZoom(self, bmp);
    if (self && !IsBadReadPtr((BYTE *)self + 0x14, 4)) {
        pL = *(short *)((BYTE *)self + 0x10);
        pT = *(short *)((BYTE *)self + 0x12);
        pR = *(short *)((BYTE *)self + 0x14);
        pB = *(short *)((BYTE *)self + 0x16);
    }
    /* Child 0x1002 — same FindChild as retail (vtable+0x7c). */
    if (self && !IsBadReadPtr(self, 4)) {
        void *vt = *(void **)self;
        typedef void *(__attribute__((thiscall)) *PFN_Find)(void *, unsigned, void *);
        if (vt && !IsBadReadPtr((BYTE *)vt + 0x7c, 4)) {
            PFN_Find find = *(PFN_Find *)((BYTE *)vt + 0x7c);
            void *tag = *(void **)(ULONG_PTR)0x00754F10u;
            if (find)
                child = find(self, 0x1002u, tag);
        }
    }
    if (child && !IsBadReadPtr((BYTE *)child + 0x14, 4)) {
        cL = *(short *)((BYTE *)child + 0x10);
        cT = *(short *)((BYTE *)child + 0x12);
        cR = *(short *)((BYTE *)child + 0x14);
        cB = *(short *)((BYTE *)child + 0x16);
    }
    (void)vk_present_soft_size(&soft_w, &soft_h);
    {
        long pw = (long)pR - (long)pL + 1;
        long ph = (long)pB - (long)pT + 1;
        long cw = (long)cR - (long)cL + 1;
        long ch = (long)cB - (long)cT + 1;
        double bmp_a = bh > 0 ? (double)bw / (double)bh : 0.0;
        double wid_a = ph > 0 ? (double)pw / (double)ph : 0.0;
        double play_a = (soft_h > 160) ? (double)soft_w / (double)(soft_h - 80 - 54) : 0.0;
        log_msg("AttachZoom #%ld bmp=%dx%d parent=(%d,%d)-(%d,%d) %ldx%ld aspect=%.3f "
                "child=%ldx%ld soft=%dx%d play_aspect=%.3f bmp_aspect=%.3f stretch=%.3f",
                (long)n, (int)bw, (int)bh, (int)pL, (int)pT, (int)pR, (int)pB, pw, ph, wid_a, cw,
                ch, soft_w, soft_h, play_a, bmp_a, (bmp_a > 0.01) ? (wid_a / bmp_a) : 0.0);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"bmp\":[%d,%d],\"parent\":[%d,%d,%d,%d],\"pwh\":[%ld,%ld],"
                 "\"child\":[%d,%d,%d,%d],\"cwh\":[%ld,%ld],\"soft\":[%d,%d],"
                 "\"aspects\":{\"bmp\":%.4f,\"widget\":%.4f,\"play\":%.4f,\"stretch\":%.4f}}",
                 (long)n, (int)bw, (int)bh, (int)pL, (int)pT, (int)pR, (int)pB, pw, ph, (int)cL,
                 (int)cT, (int)cR, (int)cB, cw, ch, soft_w, soft_h, bmp_a, wid_a, play_a,
                 (bmp_a > 0.01) ? (wid_a / bmp_a) : 0.0);
        /* #region agent log */
        hooks_agent("H-U", "hooks.c:AttachZoom", "zoom-attach", js);
        /* #endregion */
    }
}

/*
 * Retail bitmap-font DrawText @ 0x4025e0 (cdecl):
 *   (surf, font, x, y, str, maxlen, flags) → layout [vt+0x3c], glyph blit [vt+0x34].
 * Callers include CUIText @ 0x472bf6 (HelpText / UI tips). H-TA..H-TE: measure X under letterbox.
 */
typedef int (*PFN_DrawTextBmp)(void *surf, void *font, int x, int y, const char *str, int maxlen,
                               int flags);
static PFN_DrawTextBmp real_DrawTextBmp;
static BYTE *g_dt_tramp;
static BYTE g_dt_saved[16];
/* steal=5 only: prolog is mov eax,0x4000; call alloca — copying the relative call
 * into the trampoline relocates it and crashes (H-CRASH). */
static SIZE_T g_dt_steal = 5;
static void *g_dt_target;
static volatile LONG g_dt_n;

/*
 * H-MENU-B: TextW paint @ 0x445C10 (thiscall, ret 0xC = 3 stack args).
 * Prolog is two 4-byte MOVs — steal MUST be 8 (steal=5 crashed mid-insn).
 * Tips/version/copyright use this; ImgButton200 labels do NOT (baked via CTextImage).
 */
typedef void(__attribute__((thiscall)) *PFN_TextW_Paint)(void *self, void *a0, void *a1, void *a2);
static PFN_TextW_Paint real_TextW_Paint;
static BYTE *g_tw_tramp;
static BYTE g_tw_saved[16];
static SIZE_T g_tw_steal = 8;
static void *g_tw_target;
static volatile LONG g_tw_n;

/*
 * H-TEXTW-A: TextW glyph blit @ 0x444770 (thiscall, ret 0x8 = dest + extra).
 * Prolog mov eax,0x7028; call alloca — steal=5 (same as DrawTextBmp).
 * Dest is CMemoryDC (vt0=0x41d650, path=20). Paint @ 0x445C10 only layouts.
 */
typedef void(__attribute__((thiscall)) *PFN_TextW_Blit)(void *self, void *dest, void *a1);
static PFN_TextW_Blit real_TextW_Blit;
static BYTE *g_twb_tramp;
static BYTE g_twb_saved[16];
static SIZE_T g_twb_steal = 5;
static void *g_twb_target;
static volatile LONG g_twb_n;

/*
 * H-BAKE-A/C: CTextImage measure/bake entry @ 0x416350 (thiscall, ret 0).
 * Called from ImageButton load @ 0x440667 via vt+0x44 after Text=/Font=/FontColor=.
 * Uses CBMPFont layout vt+0x3c; centers label into ButtonImage frames.
 * steal=6: sub esp,0x44; push ebx; mov ebx,ecx.
 */
typedef int(__attribute__((thiscall)) *PFN_CTextImage_Bake)(void *self);
static PFN_CTextImage_Bake real_CTextImage_Bake;
static BYTE *g_tib_tramp;
static BYTE g_tib_saved[16];
static SIZE_T g_tib_steal = 6;
static void *g_tib_target;
static volatile LONG g_tib_n;

/*
 * H-BAKE-E: CTextImage paint/raster @ 0x415fe0 (thiscall, ret 0x1c = 7 stack args).
 * Glyph loop via font vt+0x34; layouts via vt+0x3c. steal=5: push ebx; mov ebx,[esp+0x10].
 */
typedef void(__attribute__((thiscall)) *PFN_CTextImage_Paint)(void *self, void *a0, void *a1,
                                                              void *a2, void *a3, void *a4,
                                                              void *a5, void *a6);
static PFN_CTextImage_Paint real_CTextImage_Paint;
static BYTE *g_tipaint_tramp;
static BYTE g_tipaint_saved[16];
static SIZE_T g_tipaint_steal = 5;
static void *g_tipaint_target;
static volatile LONG g_tipaint_n;

/*
 * H-BAKE-B: CBMPFont glyph blit @ 0x403030 (thiscall, ret 0x14 = 5 stack args).
 * Used by DrawTextBmp and by CTextImage paint @ 0x415fe0.
 * steal=6: sub esp,0x18; push edi.
 */
typedef int(__attribute__((thiscall)) *PFN_CBMPFont_Glyph)(void *self, void *a0, void *a1, void *a2,
                                                           void *a3, void *a4);
static PFN_CBMPFont_Glyph real_CBMPFont_Glyph;
static BYTE *g_fg_tramp;
static BYTE g_fg_saved[16];
static SIZE_T g_fg_steal = 6;
static void *g_fg_target;
static volatile LONG g_fg_n;

/*
 * H-MISS-A: CUIText (ANSI Type=Text) paint @ 0x4497c0 (thiscall, ret 0x8).
 * Counterpart of TextW_Paint; still CBMPFont. steal=6: push esi; push edi; mov esi,ecx; push 0x10.
 */
typedef void(__attribute__((thiscall)) *PFN_CUIText_Paint)(void *self, void *dest, void *a1);
static PFN_CUIText_Paint real_CUIText_Paint;
static BYTE *g_ct_tramp;
static BYTE g_ct_saved[16];
static SIZE_T g_ct_steal = 6;
static void *g_ct_target;
static volatile LONG g_ct_n;

/*
 * H-MISS-A: CFont::DrawString @ 0x449810 (thiscall, ret 0x10). Glyph loop ret 0x449a8d.
 * steal=6: sub esp,0x50; push esi; mov esi,ecx.
 */
typedef void(__attribute__((thiscall)) *PFN_CFont_Draw)(void *self, void *a0, void *a1, void *a2,
                                                        void *a3);
static PFN_CFont_Draw real_CFont_Draw;
static BYTE *g_cfd_tramp;
static BYTE g_cfd_saved[16];
static SIZE_T g_cfd_steal = 6;
static void *g_cfd_target;
static volatile LONG g_cfd_n;

/*
 * H-LIST-A: cdecl DrawWide @ 0x439440 (19 args, add esp,0x4c). Glyph loop ret 0x4397ed.
 * steal=7: mov eax,[esp+0xc]; sub esp,0x60.
 */
typedef void(__attribute__((cdecl)) *PFN_DrawWide)(unsigned a0, unsigned a1, unsigned a2,
                                                   unsigned a3, unsigned a4, unsigned a5,
                                                   unsigned a6, unsigned a7, unsigned a8,
                                                   unsigned a9, unsigned a10, unsigned a11,
                                                   unsigned a12, unsigned a13, unsigned a14,
                                                   unsigned a15, unsigned a16, unsigned a17,
                                                   unsigned a18);
static PFN_DrawWide real_DrawWide;
static BYTE *g_dw_tramp;
static BYTE g_dw_saved[16];
static SIZE_T g_dw_steal = 7;
static void *g_dw_target;
static volatile LONG g_dw_n;

/*
 * H-LIST-B: cdecl DrawAnsi @ 0x4398a0 (~15 args, caller add esp,0x3c). steal=10.
 */
typedef void(__attribute__((cdecl)) *PFN_DrawAnsi)(unsigned a0, unsigned a1, unsigned a2,
                                                   unsigned a3, unsigned a4, unsigned a5,
                                                   unsigned a6, unsigned a7, unsigned a8,
                                                   unsigned a9, unsigned a10, unsigned a11,
                                                   unsigned a12, unsigned a13, unsigned a14);
static PFN_DrawAnsi real_DrawAnsi;
static BYTE *g_da_tramp;
static BYTE g_da_saved[16];
static SIZE_T g_da_steal = 10;
static void *g_da_target;
static volatile LONG g_da_n;

static const char *ck_ptr_str(void *p, int maxn)
{
    int i;
    if (!p || IsBadReadPtr(p, 2))
        return NULL;
    for (i = 0; i < maxn; ++i) {
        unsigned char c = ((unsigned char *)p)[i];
        if (c == 0)
            return i > 0 ? (const char *)p : NULL;
        if (c < 32 && c != '\t' && c != '\n' && c != '\r')
            return NULL;
    }
    return (const char *)p;
}

/* FT text hooks (CK_FT_DRAWTEXT). Map editor uses same path as game. */
static int ft_drawtext_enabled(void)
{
    static int s_ft_on = -1;
    if (s_ft_on < 0)
        s_ft_on = env_on("CK_FT_DRAWTEXT", 1);
    return s_ft_on;
}

static void __attribute__((thiscall)) hook_CUIText_Paint(void *self, void *dest, void *a1)
{
    LONG n = InterlockedIncrement(&g_ct_n);
    int ok = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, r = 255, g = 255, b = 255, shadow = 1, pt = 10;
    unsigned flags = 0;
    const char *str = NULL;
    int ft_on = ft_drawtext_enabled();

    /* Retail CUIText @ 0x4494a8: ANSI text ptr at +0xaa (NOT heuristic +0x7e — that
     * false-hit «Иps» and skipped native, blanking MapTools tree). */
    if (ft_on && self && !IsBadReadPtr(self, 0xc0)) {
        int dok = 0, dpath = 0, dw = 0, dh = 0;
        unsigned long dvt0 = 0;
        void *surf = dest;
        void *sp;

        flags = *(unsigned *)((BYTE *)self + 0x28);
        x0 = (int)*(short *)((BYTE *)self + 0x10);
        y0 = (int)*(short *)((BYTE *)self + 0x12);
        x1 = (int)*(short *)((BYTE *)self + 0x14);
        y1 = (int)*(short *)((BYTE *)self + 0x16);
        sp = *(void **)((BYTE *)self + 0xaa);
        str = ck_ptr_str(sp, 200);
        if (surf)
            dok = tip_font_probe_surf(surf, &dpath, &dw, &dh, &dvt0);
        if ((!dok || dpath < 10 || dpath == 2) && a1) {
            int p2 = 0, w2 = 0, h2 = 0;
            unsigned long v2 = 0;
            if (tip_font_probe_surf(a1, &p2, &w2, &h2, &v2) && p2 >= 10 && p2 != 2) {
                surf = a1;
                dok = 1;
                dpath = p2;
                dw = w2;
                dh = h2;
                dvt0 = v2;
            }
        }
        if (!(flags & 0x200u) && dok && str && str[0]) {
            unsigned col = *(unsigned *)((BYTE *)self + 0xb8);
            unsigned cc = *(unsigned *)((BYTE *)self + 0xb0);
            if (flags & 0x10u) {
                void **dvt = *(void ***)surf;
                if (dvt && !IsBadReadPtr(dvt, 0x2c)) {
                    typedef void(__attribute__((thiscall)) * PFN_Conv)(void *dc, void *src,
                                                                       void *dst);
                    PFN_Conv conv = (PFN_Conv)dvt[0x28 / 4];
                    if (conv)
                        conv(surf, (BYTE *)self + 0xb0, (BYTE *)self + 0xb8);
                    col = *(unsigned *)((BYTE *)self + 0xb8);
                }
            }
            if (col == 0xffffffffu || col == 0) {
                r = (int)((BYTE *)self)[0xb2];
                g = (int)((BYTE *)self)[0xb1];
                b = (int)((BYTE *)self)[0xb0];
                if (r + g + b == 0) {
                    r = (int)((cc >> 16) & 255u);
                    g = (int)((cc >> 8) & 255u);
                    b = (int)(cc & 255u);
                }
            } else {
                unsigned f = col & 0x7FFFu;
                r = (int)(((f >> 10) & 31u) * 255u / 31u);
                g = (int)(((f >> 5) & 31u) * 255u / 31u);
                b = (int)((f & 31u) * 255u / 31u);
            }
            shadow = (r + g + b) >= 200 ? 1 : 0;
            {
                char env[32];
                DWORD nenv = GetEnvironmentVariableA("CK_FT_CTEXT_PT", env, (DWORD)sizeof(env));
                if (nenv > 0 && nenv < sizeof(env)) {
                    int v = atoi(env);
                    if (v >= 8 && v <= 24)
                        pt = v;
                }
            }
            ok = tip_font_draw_cbitmap_bold_pt(surf, NULL, x0, y0, str, -1, r, g, b, shadow, pt);
            if (ok && tip_font_present_active())
                tip_font_present_queue_str_surf(surf, x0, y0, str, -1, 0, r, g, b, shadow, pt, 1);
        }
        /* #region agent log */
        {
            static volatile LONG s_ct;
            LONG cn = InterlockedIncrement(&s_ct);
            int editor = (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0);
            if (cn <= 40 || (editor && cn <= 80)) {
                char esc[96];
                int i = 0;
                FILE *df;
                esc[0] = 0;
                if (str) {
                    for (i = 0; i < 40 && str[i]; ++i) {
                        unsigned char c = (unsigned char)str[i];
                        esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
                    }
                    esc[i] = 0;
                }
                df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
                if (df) {
                    fprintf(df,
                            "{\"sessionId\":\"764ba7\",\"runId\":\"cuitext-ft-fix\","
                            "\"hypothesisId\":\"H-CT\",\"location\":\"hooks_zoom.c:CUIText\","
                            "\"message\":\"cuitext-aa\","
                            "\"data\":{\"n\":%ld,\"ok\":%d,\"editor\":%d,\"xy\":[%d,%d],"
                            "\"off\":170,\"str\":\"%s\"},\"timestamp\":%lu}\n",
                            (long)cn, ok, editor, x0, y0, esc, (unsigned long)GetTickCount());
                    fclose(df);
                }
            }
        }
        /* #endregion */
        if (ok)
            return;
    }

    real_CUIText_Paint(self, dest, a1);
}

static void __attribute__((thiscall)) hook_CFont_Draw(void *self, void *a0, void *a1, void *a2,
                                                      void *a3)
{
    LONG n = InterlockedIncrement(&g_cfd_n);
    /* #region agent log */
    if (n <= 80) {
        char esc[96];
        char js[640];
        int i = 0;
        const char *str = NULL;
        void *dest = NULL;
        int dok = 0, dpath = 0, dw = 0, dh = 0;
        unsigned long dvt0 = 0;
        esc[0] = 0;
        if (self && !IsBadReadPtr(self, 0xcc))
            dest = *(void **)((BYTE *)self + 0xc8);
        if (a0 && !IsBadReadPtr(a0, 0x24))
            str = ck_ptr_str(*(void **)((BYTE *)a0 + 0x1c), 200);
        if (str) {
            for (i = 0; i < 40 && str[i]; ++i) {
                unsigned char c = (unsigned char)str[i];
                esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
            }
            esc[i] = 0;
        }
        if (dest)
            dok = tip_font_probe_surf(dest, &dpath, &dw, &dh, &dvt0);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"str\":\"%s\",\"a\":[%ld,%ld,%ld],\"dok\":%d,\"dpath\":%d,"
                 "\"dwh\":[%d,%d],\"dvt0\":%lu}",
                 (long)n, esc, (long)(LONG_PTR)a1, (long)(LONG_PTR)a2, (long)(LONG_PTR)a3, dok,
                 dpath, dw, dh, dvt0);
        hooks_agent("H-MISS-A", "hooks_zoom.c:CFont_Draw", "cfont-draw", js);
    }
    /* #endregion */
    real_CFont_Draw(self, a0, a1, a2, a3);
}

static int ck_arg_wstr(unsigned a, char *esc, int esclen)
{
    const WCHAR *ws = (const WCHAR *)(ULONG_PTR)a;
    int i, n = 0;
    if (!a || a < 0x10000u || IsBadReadPtr(ws, 4))
        return 0;
    for (n = 0; n < 80; ++n) {
        WCHAR c;
        if (IsBadReadPtr(ws + n, 2))
            return 0;
        c = ws[n];
        if (c == 0)
            break;
        if (c < 32 && c != 9 && c != 10 && c != 13)
            return 0;
    }
    if (n < 1)
        return 0;
    if (esc && esclen > 1) {
        int lim = n < 40 ? n : 40;
        if (lim >= esclen)
            lim = esclen - 1;
        for (i = 0; i < lim; ++i) {
            WCHAR c = ws[i];
            esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
        }
        esc[i] = 0;
    }
    return n;
}

static int ck_arg_dest(unsigned a, int *path, int *w, int *h, unsigned long *vt0, int *ox, int *oy)
{
    void *p = (void *)(ULONG_PTR)a;
    if (!a || a < 0x10000u || IsBadReadPtr(p, 0x20))
        return 0;
    if (!tip_font_probe_surf(p, path, w, h, vt0))
        return 0;
    *ox = (int)*(short *)((BYTE *)p + 0x14);
    *oy = (int)*(short *)((BYTE *)p + 0x16);
    return 1;
}

static void ck_rgb555(unsigned col, int *r, int *g, int *b)
{
    unsigned f = col & 0x7FFFu;
    *r = (int)(((f >> 10) & 31u) * 255u / 31u);
    *g = (int)(((f >> 5) & 31u) * 255u / 31u);
    *b = (int)((f & 31u) * 255u / 31u);
}

static void __attribute__((cdecl)) hook_DrawWide(unsigned a0, unsigned a1, unsigned a2, unsigned a3,
                                                unsigned a4, unsigned a5, unsigned a6, unsigned a7,
                                                unsigned a8, unsigned a9, unsigned a10,
                                                unsigned a11, unsigned a12, unsigned a13,
                                                unsigned a14, unsigned a15, unsigned a16,
                                                unsigned a17, unsigned a18)
{
    LONG n = InterlockedIncrement(&g_dw_n);
    int ok = 0, filled = 0, x0, y0, x1, y1, nch, r = 0, g = 0, b = 0, shadow = 0, pt = 10;
    int dok = 0, dpath = 0, dw = 0, dh = 0, ox = 0, oy = 0, measure;
    int dy_nudge = -2, draw_y = 0, cell_h = 0, tw = 0, th = 0;
    unsigned long dvt0 = 0;
    const WCHAR *ws = (const WCHAR *)(ULONG_PTR)a4;
    void *dest = (void *)(ULONG_PTR)a6;
    char esc[96];
    int ft_on = ft_drawtext_enabled();

    x0 = (short)(a0 & 0xffff);
    y0 = (short)(a0 >> 16);
    x1 = (short)(a1 & 0xffff);
    y1 = (short)(a1 >> 16);
    nch = (int)a5;
    measure = (a7 == 0xffffffffu && a8 == 0xffffffffu);
    esc[0] = 0;
    if (nch < 0)
        nch = ck_arg_wstr(a4, NULL, 0);
    if (ft_on && !measure && dest && nch > 0 && nch < 400 &&
        ck_arg_wstr(a4, esc, (int)sizeof(esc)) > 0) {
        dok = ck_arg_dest(a6, &dpath, &dw, &dh, &dvt0, &ox, &oy);
        /* Glyph dest is CMemoryDC path=20. path=2 is false-positive soft DIB. */
        if (dok && dpath >= 20) {
            int max_w;
            unsigned col = a7;
            if (col == 0xffffffffu)
                col = a8;
            if (col > 0xFFFFu) {
                r = (int)((col >> 16) & 255u);
                g = (int)((col >> 8) & 255u);
                b = (int)(col & 255u);
            } else
                ck_rgb555(col, &r, &g, &b);
            shadow = (r + g + b) >= 200 ? 1 : 0;
            {
                char env[32];
                DWORD nenv = GetEnvironmentVariableA("CK_FT_CTEXT_PT", env, (DWORD)sizeof(env));
                if (nenv > 0 && nenv < sizeof(env)) {
                    int v = atoi(env);
                    if (v >= 8 && v <= 24)
                        pt = v;
                }
            }
            max_w = x1 - x0 + 1;
            if (max_w < 8)
                max_w = 0;
            cell_h = y1 - y0 + 1;
            (void)tip_font_measure_wstr_pt(ws, nch, pt, &tw, &th);
            (void)tw;
            /* H-LIST-Y: FT baseline = dy+ascender+1 sits ink low in 17px cells
             * (gap above, descenders near bar bottom). Optical raise -2; CK_FT_LIST_DY. */
            {
                char env[32];
                DWORD nenv = GetEnvironmentVariableA("CK_FT_LIST_DY", env, (DWORD)sizeof(env));
                if (nenv > 0 && nenv < sizeof(env))
                    dy_nudge = atoi(env);
            }
            draw_y = y0 + dy_nudge;
            /* H-LIST-SEL: retail DrawWide fills with a8 (RGB555) before glyphs when
             * a8!=-1 (selected row). Skipping native removed the white bar. */
            if (a8 != 0xffffffffu) {
                void **dvt = *(void ***)dest;
                if (dvt && !IsBadReadPtr(dvt, 0x40)) {
                    typedef void(__attribute__((thiscall)) * PFN_Fill)(void *dc, void *rect,
                                                                      unsigned col);
                    PFN_Fill fill = (PFN_Fill)dvt[0x3c / 4];
                    short rc[4];
                    rc[0] = (short)x0;
                    rc[1] = (short)y0;
                    rc[2] = (short)x1;
                    rc[3] = (short)y1;
                    if (fill) {
                        fill(dest, rc, a8);
                        filled = 1;
                    }
                }
            }
            ok = tip_font_draw_cbitmap_wstr_pt(dest, x0, draw_y, ws, nch, max_w, r, g, b, shadow,
                                              pt);
            if (ok && tip_font_present_active())
                tip_font_present_queue_wstr_surf(dest, x0, draw_y, ws, nch, max_w, r, g, b, shadow,
                                                 pt, 1);
        }
    }
    /* #region agent log */
    if (n <= 80 || (!measure && n <= 200) || filled) {
        char js[960];
        void *ret = __builtin_return_address(0);
        if (!esc[0])
            ck_arg_wstr(a4, esc, (int)sizeof(esc));
        if (!dok)
            dok = ck_arg_dest(a6, &dpath, &dw, &dh, &dvt0, &ox, &oy);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"ok\":%d,\"meas\":%d,\"fill\":%d,\"xy\":[%d,%d],\"rb\":[%d,%d],"
                 "\"draw_y\":%d,\"dy\":%d,\"cell_h\":%d,\"th\":%d,\"nch\":%d,\"dpath\":%d,"
                 "\"dwh\":[%d,%d],\"oxy\":[%d,%d],\"a6\":%lu,\"a7\":%u,\"a8\":%u,"
                 "\"rgb\":[%d,%d,%d],\"shadow\":%d,\"ret\":%lu,\"face\":\"%s\",\"str\":\"%s\"}",
                 (long)n, ok, measure, filled, x0, y0, x1, y1, draw_y, dy_nudge, cell_h, th, nch,
                 dpath, dw, dh, ox, oy, (unsigned long)(ULONG_PTR)a6, a7, a8, r, g, b, shadow,
                 (unsigned long)(ULONG_PTR)ret, tip_font_ui_style(), esc);
        hooks_agent(ok ? (filled ? "H-LIST-SEL" : "H-LIST-FT") : "H-LIST-A",
                    "hooks_zoom.c:DrawWide", "drawwide", js);
    }
    /* #endregion */
    if (ok)
        return;
    real_DrawWide(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17,
                  a18);
}

static void __attribute__((cdecl)) hook_DrawAnsi(unsigned a0, unsigned a1, unsigned a2, unsigned a3,
                                                unsigned a4, unsigned a5, unsigned a6, unsigned a7,
                                                unsigned a8, unsigned a9, unsigned a10,
                                                unsigned a11, unsigned a12, unsigned a13,
                                                unsigned a14)
{
    LONG n = InterlockedIncrement(&g_da_n);
    /* #region agent log */
    if (n <= 80) {
        char esc[96];
        char js[800];
        unsigned args[15];
        int i, si = -1, di = -1, dok = 0, dpath = 0, dw = 0, dh = 0, ox = 0, oy = 0;
        unsigned long dvt0 = 0;
        void *ret = __builtin_return_address(0);
        const char *str = NULL;
        args[0] = a0;
        args[1] = a1;
        args[2] = a2;
        args[3] = a3;
        args[4] = a4;
        args[5] = a5;
        args[6] = a6;
        args[7] = a7;
        args[8] = a8;
        args[9] = a9;
        args[10] = a10;
        args[11] = a11;
        args[12] = a12;
        args[13] = a13;
        args[14] = a14;
        esc[0] = 0;
        for (i = 0; i < 15; ++i) {
            str = ck_ptr_str((void *)(ULONG_PTR)args[i], 200);
            if (str) {
                int k;
                si = i;
                for (k = 0; k < 40 && str[k]; ++k) {
                    unsigned char c = (unsigned char)str[k];
                    esc[k] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
                }
                esc[k] = 0;
                break;
            }
        }
        for (i = 0; i < 15; ++i) {
            int p = 0, w = 0, h = 0, x = 0, y = 0;
            unsigned long v = 0;
            if (ck_arg_dest(args[i], &p, &w, &h, &v, &x, &y)) {
                di = i;
                dok = 1;
                dpath = p;
                dw = w;
                dh = h;
                dvt0 = v;
                ox = x;
                oy = y;
                break;
            }
        }
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"lt\":[%d,%d],\"rb\":[%d,%d],\"si\":%d,\"di\":%d,\"dok\":%d,"
                 "\"dpath\":%d,\"dwh\":[%d,%d],\"oxy\":[%d,%d],\"ret\":%lu,\"str\":\"%s\"}",
                 (long)n, (short)(a0 & 0xffff), (short)(a0 >> 16), (short)(a1 & 0xffff),
                 (short)(a1 >> 16), si, di, dok, dpath, dw, dh, ox, oy,
                 (unsigned long)(ULONG_PTR)ret, esc);
        hooks_agent("H-LIST-B", "hooks_zoom.c:DrawAnsi", "drawansi", js);
    }
    /* #endregion */
    real_DrawAnsi(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

static void __attribute__((thiscall)) hook_TextW_Paint(void *self, void *a0, void *a1, void *a2)
{
    LONG n = InterlockedIncrement(&g_tw_n);
    /* #region agent log */
    if (n <= 150 && self && !IsBadReadPtr(self, 0x160)) {
        char esc[96];
        char js[900];
        int i = 0;
        void *font = *(void **)((BYTE *)self + 0x58);
        void *wstr = *(void **)((BYTE *)self + 0x98);
        unsigned nch = *(unsigned *)((BYTE *)self + 0xa4);
        void *parent = *(void **)((BYTE *)self + 0x8);
        void *dc = NULL;
        int x0 = (int)*(short *)((BYTE *)self + 0x10);
        int y0 = (int)*(short *)((BYTE *)self + 0x12);
        int x1 = (int)*(short *)((BYTE *)self + 0x14);
        int y1 = (int)*(short *)((BYTE *)self + 0x16);
        int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        unsigned long fvt0 = 0, fvt34 = 0, fvt3c = 0;
        unsigned long pvt0 = 0, dcvt0 = 0, a1vt0 = 0, a2vt0 = 0;
        unsigned col_d8 = 0, col_dc = 0, flags28 = 0;
        int pok = 0, ppath = 0, pw = 0, ph = 0;
        int a1ok = 0, a1path = 0, a1w = 0, a1h = 0;
        esc[0] = 0;
        if (a0 && !IsBadReadPtr(a0, 8)) {
            rx0 = (int)*(short *)a0;
            ry0 = (int)*((short *)a0 + 1);
            rx1 = (int)*((short *)a0 + 2);
            ry1 = (int)*((short *)a0 + 3);
        }
        if (font && !IsBadReadPtr(font, 4)) {
            void **fvt = *(void ***)font;
            if (fvt && !IsBadReadPtr(fvt, 0x40)) {
                fvt0 = (unsigned long)(ULONG_PTR)fvt[0];
                fvt34 = (unsigned long)(ULONG_PTR)fvt[0x34 / 4];
                fvt3c = (unsigned long)(ULONG_PTR)fvt[0x3c / 4];
            }
        }
        if (parent && !IsBadReadPtr(parent, 0x34)) {
            void **pvt = *(void ***)parent;
            if (pvt && !IsBadReadPtr(pvt, 4))
                pvt0 = (unsigned long)(ULONG_PTR)pvt[0];
            dc = *(void **)((BYTE *)parent + 0x30);
        }
        if (dc && !IsBadReadPtr(dc, 4)) {
            void **dvt = *(void ***)dc;
            if (dvt && !IsBadReadPtr(dvt, 4))
                dcvt0 = (unsigned long)(ULONG_PTR)dvt[0];
            pok = tip_font_probe_surf(dc, &ppath, &pw, &ph, &dcvt0);
        }
        if (a1 && !IsBadReadPtr(a1, 4)) {
            void **vt = *(void ***)a1;
            if (vt && !IsBadReadPtr(vt, 4))
                a1vt0 = (unsigned long)(ULONG_PTR)vt[0];
            a1ok = tip_font_probe_surf(a1, &a1path, &a1w, &a1h, &a1vt0);
        }
        if (a2 && !IsBadReadPtr(a2, 4)) {
            void **vt = *(void ***)a2;
            if (vt && !IsBadReadPtr(vt, 4))
                a2vt0 = (unsigned long)(ULONG_PTR)vt[0];
        }
        col_d8 = *(unsigned *)((BYTE *)self + 0xd8);
        col_dc = *(unsigned *)((BYTE *)self + 0xdc);
        flags28 = *(unsigned *)((BYTE *)self + 0x28);
        if (wstr && nch > 0 && nch < 200 && !IsBadReadPtr(wstr, nch * 2)) {
            const WCHAR *w = (const WCHAR *)wstr;
            unsigned lim = nch < 40 ? nch : 40;
            for (i = 0; i < (int)lim; ++i) {
                WCHAR c = w[i];
                esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
            }
            esc[i] = 0;
        }
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"xy\":[%d,%d,%d,%d],\"wh\":[%d,%d],\"rect\":[%d,%d,%d,%d],"
                 "\"nch\":%u,\"parent\":%lu,\"pvt0\":%lu,\"dc\":%lu,\"dcvt0\":%lu,"
                 "\"pok\":%d,\"ppath\":%d,\"pwh\":[%d,%d],\"a1ok\":%d,\"a1vt0\":%lu,"
                 "\"a1path\":%d,\"a1wh\":[%d,%d],\"a2vt0\":%lu,\"col\":[%u,%u],\"fl\":%u,"
                 "\"font\":%lu,\"fvt\":[%lu,%lu,%lu],\"str\":\"%s\",\"ret\":%lu}",
                 (long)n, x0, y0, x1, y1, x1 - x0 + 1, y1 - y0 + 1, rx0, ry0, rx1, ry1, nch,
                 (unsigned long)(ULONG_PTR)parent, pvt0, (unsigned long)(ULONG_PTR)dc, dcvt0, pok,
                 ppath, pw, ph, a1ok, a1vt0, a1path, a1w, a1h, a2vt0, col_d8, col_dc, flags28,
                 (unsigned long)(ULONG_PTR)font, fvt0, fvt34, fvt3c, esc,
                 (unsigned long)(ULONG_PTR)__builtin_return_address(0));
        hooks_agent("H-TEXTW-B", "hooks_zoom.c:TextW_Paint", "textw-paint", js);
    }
    /* #endregion */
    real_TextW_Paint(self, a0, a1, a2);
}

static void __attribute__((thiscall)) hook_TextW_Blit(void *self, void *dest, void *a1)
{
    LONG n = InterlockedIncrement(&g_twb_n);
    int ok = 0;
    int r = 255, g = 255, b = 255, shadow = 1, pt = 10;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, max_w = 0;
    int orig_x = 0, orig_y = 0;
    int tw = 0, th = 0, b0 = 0, b4 = 0, b8 = 0, fcenter = 0, fright = 0, span = 0;
    int dy_nudge = 0;
    unsigned nch = 0, flags = 0, cc = 0, col = 0;
    const WCHAR *ws = NULL;
    int ft_on = ft_drawtext_enabled();

    if (ft_on && self && !IsBadReadPtr(self, 0x100)) {
        void *surf = dest;
        int dok = 0, dpath = 0, dw = 0, dh = 0;
        unsigned long dvt0 = 0;

        flags = *(unsigned *)((BYTE *)self + 0x28);
        ws = *(const WCHAR **)((BYTE *)self + 0x98);
        nch = *(unsigned *)((BYTE *)self + 0xa4);
        x0 = (int)*(short *)((BYTE *)self + 0x10);
        y0 = (int)*(short *)((BYTE *)self + 0x12);
        x1 = (int)*(short *)((BYTE *)self + 0x14);
        y1 = (int)*(short *)((BYTE *)self + 0x16);
        if (x1 < x0 || y1 < y0) {
            x0 = (int)*(short *)((BYTE *)self + 0xfc);
            y0 = (int)*(short *)((BYTE *)self + 0xfe);
            x1 = (int)*(short *)((BYTE *)self + 0x100);
            y1 = (int)*(short *)((BYTE *)self + 0x102);
        }
        orig_x = x0;
        orig_y = y0;
        cc = *(unsigned *)((BYTE *)self + 0xcc);
        if (surf)
            dok = tip_font_probe_surf(surf, &dpath, &dw, &dh, &dvt0);
        /* Glyph loop dest is CMemoryDC (vt0=0x41d650, path>=20). If arg0 is not, try a1. */
        if ((!dok || dpath < 10 || dpath == 2) && a1) {
            int p2 = 0, w2 = 0, h2 = 0;
            unsigned long v2 = 0;
            if (tip_font_probe_surf(a1, &p2, &w2, &h2, &v2) && p2 >= 10 && p2 != 2) {
                surf = a1;
                dok = 1;
                dpath = p2;
                dw = w2;
                dh = h2;
                dvt0 = v2;
            }
        }
        (void)dok;
        (void)dw;
        (void)dh;
        (void)dvt0;
        if (!(flags & 0x200u) && surf && ws && nch > 0 && nch < 2000u &&
            !IsBadReadPtr(ws, nch * 2)) {
            if (flags & 0x10u) {
                void **dvt = *(void ***)surf;
                if (dvt && !IsBadReadPtr(dvt, 0x2c)) {
                    typedef void(__attribute__((thiscall)) * PFN_Conv)(void *dc, void *src,
                                                                       void *dst);
                    PFN_Conv conv = (PFN_Conv)dvt[0x28 / 4];
                    if (conv) {
                        conv(surf, (BYTE *)self + 0xcc, (BYTE *)self + 0xd8);
                        conv(surf, (BYTE *)self + 0xd0, (BYTE *)self + 0xdc);
                        conv(surf, (BYTE *)self + 0xd4, (BYTE *)self + 0xe0);
                    }
                }
            }
            col = *(unsigned *)((BYTE *)self + 0xd8);
            if (col == 0xffffffffu || col == 0) {
                r = (int)((BYTE *)self)[0xce];
                g = (int)((BYTE *)self)[0xcd];
                b = (int)((BYTE *)self)[0xcc];
            } else {
                unsigned f = col & 0x7FFFu;
                r = (int)(((f >> 10) & 31u) * 255u / 31u);
                g = (int)(((f >> 5) & 31u) * 255u / 31u);
                b = (int)((f & 31u) * 255u / 31u);
            }
            /* H-SCALE-FAT: Bold+shadow on Tips looked like an upscaled bitmap (logs: pt=11
             * th=14, glyph ~10px — not 2x). No FT drop-shadow on TextW; outline was retail. */
            shadow = 0;
            max_w = x1 - x0 + 1;
            if (max_w < 8)
                max_w = 0;
            {
                char env[32];
                DWORD nenv = GetEnvironmentVariableA("CK_FT_CTEXT_PT", env, (DWORD)sizeof(env));
                pt = 10; /* Bold UI default; was 11 — optically large vs APF */
                if (nenv > 0 && nenv < sizeof(env)) {
                    int v = atoi(env);
                    if (v >= 8 && v <= 24)
                        pt = v;
                }
            }
            {
                int dx, dy, box_w, box_h;
                /* Retail 444770: ebx = x0 - [+0xb4]; if (flags&0x3000000)==0x3000000
                 * center: +(b0-line_w)/2; else if (flags&0x2000000)==0x2000000 right.
                 * H-TIP-A REJECTED: +0x3c is not HAlign (log ha=[0,0,1] → x=1048). */
                b0 = *(int *)((BYTE *)self + 0xb0);
                b4 = *(int *)((BYTE *)self + 0xb4);
                b8 = *(int *)((BYTE *)self + 0xb8);
                fcenter = ((flags & 0x3000000u) == 0x3000000u);
                fright = !fcenter && ((flags & 0x2000000u) == 0x2000000u);
                box_w = x1 - x0 + 1;
                box_h = y1 - y0 + 1;
                dx = x0;
                dy = y0;
                if (b4 > -512 && b4 < 512)
                    dx = x0 - b4;
                if (tip_font_measure_wstr_pt(ws, (int)nch, pt, &tw, &th) && tw > 0) {
                    span = b0;
                    if (span <= tw || span > 2000)
                        span = box_w;
                    if ((fcenter || fright) && span > tw) {
                        if (fcenter)
                            dx += (span - tw) / 2;
                        else
                            dx += (span - tw);
                        max_w = 0;
                    }
                }
                /* H-COMBO-Y: closed combo value ("Средняя") is TextW 126x18, not DrawWide.
                 * Same FT low-ink issue; optical -2. CK_FT_TEXTW_DY overrides. */
                if (box_h > 0 && box_h <= 22 && nch > 0 && nch < 64u) {
                    dy_nudge = -2;
                    {
                        char env[32];
                        DWORD nenv =
                            GetEnvironmentVariableA("CK_FT_TEXTW_DY", env, (DWORD)sizeof(env));
                        if (nenv > 0 && nenv < sizeof(env))
                            dy_nudge = atoi(env);
                    }
                    dy += dy_nudge;
                }
                ok = tip_font_draw_cbitmap_wstr_pt(surf, dx, dy, ws, (int)nch, max_w, r, g, b,
                                                   shadow, pt);
                if (ok && tip_font_present_active())
                    tip_font_present_queue_wstr_surf(surf, dx, dy, ws, (int)nch, max_w, r, g, b,
                                                     shadow, pt, 1);
                x0 = dx;
                y0 = dy;
            }
        }
    }

            /* #region agent log */
            /* Throttle: empty FAIL spam was ~4k fopen/session and drowned H-CAM. */
            if (n <= 40 || (ok && (n % 100) == 0) || (!ok && nch > 0 && (n % 50) == 0)) {
                char esc[96];
                char js[800];
                int i = 0;
        unsigned long dvt0 = 0;
        int dok = 0, dpath = 0, dw = 0, dh = 0;
        esc[0] = 0;
        if (ws && nch > 0 && nch < 200 && !IsBadReadPtr(ws, nch * 2)) {
            unsigned lim = nch < 40 ? nch : 40;
            for (i = 0; i < (int)lim; ++i) {
                WCHAR c = ws[i];
                esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
            }
            esc[i] = 0;
        }
        if (dest)
            dok = tip_font_probe_surf(dest, &dpath, &dw, &dh, &dvt0);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"ok\":%d,\"xy\":[%d,%d],\"orig\":[%d,%d],\"wh\":[%d,%d],\"nch\":%u,"
                 "\"pt\":%d,\"tw\":%d,\"th\":%d,\"b0\":%d,\"b4\":%d,\"b8\":%d,\"span\":%d,"
                 "\"fc\":%d,\"fr\":%d,\"dy\":%d,\"rgb\":[%d,%d,%d],\"shadow\":%d,\"cc\":%u,"
                 "\"col\":%u,\"fl\":%u,\"dok\":%d,\"dpath\":%d,\"dwh\":[%d,%d],\"dvt0\":%lu,"
                 "\"str\":\"%s\"}",
                 (long)n, ok, x0, y0, orig_x, orig_y, x1 - orig_x + 1, y1 - orig_y + 1, nch, pt, tw,
                 th, b0, b4, b8, span, fcenter, fright, dy_nudge, r, g, b, shadow, cc, col, flags,
                 dok, dpath, dw, dh, dvt0, esc);
        hooks_agent(ok ? (dy_nudge ? "H-COMBO-Y" : "H-A") : "H-TEXTW-FAIL",
                    "hooks_zoom.c:TextW_Blit", "textw-ft", js);
    }
    /* #endregion */

    if (ok)
        return;
    real_TextW_Blit(self, dest, a1);
}

static int __attribute__((thiscall)) hook_CTextImage_Bake(void *self)
{
    LONG n = InterlockedIncrement(&g_tib_n);
    /* #region agent log */
    if (n <= 80 && self && !IsBadReadPtr(self, 0x90)) {
        char esc[96];
        char js[640];
        int i = 0;
        void *img = *(void **)((BYTE *)self + 0x4);
        void *font = *(void **)((BYTE *)self + 0x8);
        const char *str = *(const char **)((BYTE *)self + 0x84);
        unsigned long fvt0 = 0, fvt34 = 0, fvt3c = 0;
        esc[0] = 0;
        if (font && !IsBadReadPtr(font, 4)) {
            void **fvt = *(void ***)font;
            if (fvt && !IsBadReadPtr(fvt, 0x40)) {
                fvt0 = (unsigned long)(ULONG_PTR)fvt[0];
                fvt34 = (unsigned long)(ULONG_PTR)fvt[0x34 / 4];
                fvt3c = (unsigned long)(ULONG_PTR)fvt[0x3c / 4];
            }
        }
        if (str && !IsBadReadPtr(str, 1)) {
            for (i = 0; i < 40 && str[i]; ++i) {
                unsigned char c = (unsigned char)str[i];
                esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
            }
            esc[i] = 0;
        }
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"self\":%lu,\"img\":%lu,\"font\":%lu,\"fvt\":[%lu,%lu,%lu],"
                 "\"str\":\"%s\",\"ret\":%lu}",
                 (long)n, (unsigned long)(ULONG_PTR)self, (unsigned long)(ULONG_PTR)img,
                 (unsigned long)(ULONG_PTR)font, fvt0, fvt34, fvt3c, esc,
                 (unsigned long)(ULONG_PTR)__builtin_return_address(0));
        hooks_agent("H-BAKE-A", "hooks_zoom.c:CTextImage_Bake", "ctextimage-bake", js);
    }
    /* #endregion */
    return real_CTextImage_Bake(self);
}

static void __attribute__((thiscall)) hook_CTextImage_Paint(void *self, void *a0, void *a1,
                                                            void *a2, void *a3, void *a4,
                                                            void *a5, void *a6)
{
    LONG n = InterlockedIncrement(&g_tipaint_n);
    int ok = 0;
    int tx = 0, ty = 0, r = 0, g = 0, b = 0, shadow = 0;
    const char *str = NULL;
    void *font = NULL;
    /* #region agent log */
    if (n <= 100 && self && !IsBadReadPtr(self, 0x90)) {
        char esc[96];
        char js[640];
        int i = 0;
        font = *(void **)((BYTE *)self + 0x8);
        str = *(const char **)((BYTE *)self + 0x84);
        esc[0] = 0;
        if (str && !IsBadReadPtr(str, 1)) {
            for (i = 0; i < 40 && str[i]; ++i) {
                unsigned char c = (unsigned char)str[i];
                esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
            }
            esc[i] = 0;
        }
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"self\":%lu,\"font\":%lu,\"str\":\"%s\",\"ret\":%lu}",
                 (long)n, (unsigned long)(ULONG_PTR)self, (unsigned long)(ULONG_PTR)font, esc,
                 (unsigned long)(ULONG_PTR)__builtin_return_address(0));
        hooks_agent("H-BAKE-E", "hooks_zoom.c:CTextImage_Paint", "ctextimage-paint", js);
    }
    /* #endregion */

    /*
     * Retail: paint ButtonImage child (vt+0xc), then APF glyphs via font.
     * Replace glyph stage with FreeType into the same surface (a0).
     */
    if (ft_drawtext_enabled() && self && !IsBadReadPtr(self, 0x90) && a0) {
        void *child = *(void **)((BYTE *)self + 0x4);
        typedef int(__attribute__((thiscall)) * PFN_GetI)(void *s);
        typedef void(__attribute__((thiscall)) * PFN_Paint7)(void *s, void *p0, void *p1, void *p2,
                                                             void *p3, void *p4, void *p5, void *p6);
        int idx = 0;
        unsigned col = 0;
        short ox = 0, oy = 0;

        font = *(void **)((BYTE *)self + 0x8);
        str = *(const char **)((BYTE *)self + 0x84);

        if (str && !IsBadReadPtr(str, 1) && str[0]) {
            void **vt = *(void ***)self;
            typedef int(__attribute__((thiscall)) * PFN_GetDim)(void *s);
            int btn_w = 0, btn_h = 0, tw = 0, th = 0;
            int old_tx, old_ty, nudge = -4, ctext_pt = 11, geo_ty = 0, origin_y = 0;
            int ink_top = 0, asc = 0, desc = 0, line_h = 0;

            if (vt && !IsBadReadPtr(vt, 0x38)) {
                PFN_GetI get34 = (PFN_GetI)vt[0x34 / 4];
                PFN_GetI get2c = (PFN_GetI)vt[0x2c / 4];
                PFN_GetI get28 = (PFN_GetI)vt[0x28 / 4];
                PFN_GetDim get_w = (PFN_GetDim)vt[0x10 / 4];
                PFN_GetDim get_h = (PFN_GetDim)vt[0x14 / 4];
                int va = get34 ? get34(self) : 0;
                int vb = get2c ? get2c(self) : 0;
                int vc = get28 ? get28(self) : 0;
                idx = va * vb + vc;
                if (idx < 0 || idx > 9)
                    idx = 0;
                if (get_w)
                    btn_w = get_w(self);
                if (get_h)
                    btn_h = get_h(self);
            }
            ox = *(short *)((BYTE *)self + 0x0c + idx * 4);
            oy = *(short *)((BYTE *)self + 0x0e + idx * 4);
            /* Retail oy centers APF glyph box; FT metrics differ → looks shifted down.
             * Re-center with measured FreeType size inside button (same formula as bake). */
            /*
             * Menu buttons: slightly smaller than tip 12pt (APF was ~14 but denser).
             * CK_FT_CTEXT_PT overrides (8..24).
             */
            {
                char env[32];
                DWORD nenv = GetEnvironmentVariableA("CK_FT_CTEXT_PT", env, (DWORD)sizeof(env));
                if (nenv > 0 && nenv < sizeof(env)) {
                    int v = atoi(env);
                    if (v >= 8 && v <= 24)
                        ctext_pt = v;
                }
            }
            old_tx = (int)ox - (int)(INT_PTR)a5 + (int)(INT_PTR)a1;
            old_ty = (int)oy - (int)(INT_PTR)a6 + (int)(INT_PTR)a2;
            origin_y = (int)(INT_PTR)a2 - (int)(INT_PTR)a6;
            tx = old_tx;
            ty = old_ty;
            geo_ty = old_ty;
            /* H-BTN-A REJECTED: geo (btn_h-th)/2 only dY=+2 vs APF, user: text too low.
             * dy is line-box top; ink sits below (ink_top). Optical -4 recenters in the
             * stone oval (thicker bottom bevel). CK_FT_CTEXT_DY overrides. */
            if (tip_font_measure_pt(str, -1, ctext_pt, &tw, &th) && btn_w > 0 && tw > 0) {
                tx = (int)(INT_PTR)a1 - (int)(INT_PTR)a5 + (btn_w - tw) / 2;
                ink_top = tip_font_last_ink_top();
                tip_font_line_box_pt(ctext_pt, &asc, &desc, &line_h);
                if (btn_h > 0 && th > 0) {
                    geo_ty = origin_y + (btn_h - th) / 2;
                    ty = geo_ty;
                }
            }
            {
                char env[32];
                DWORD nenv = GetEnvironmentVariableA("CK_FT_CTEXT_DY", env, (DWORD)sizeof(env));
                if (nenv > 0 && nenv < sizeof(env))
                    nudge = atoi(env);
                ty += nudge;
            }
            col = *(unsigned *)((BYTE *)self + 0x5c + idx * 4);
            if (col == 0xffffffffu)
                col = *(unsigned *)((BYTE *)self + 0x34 + idx * 4);
            if (col == 0xffffffffu)
                col = 0;
            {
                unsigned f = col & 0x7FFFu;
                r = (int)(((f >> 10) & 31u) * 255u / 31u);
                g = (int)(((f >> 5) & 31u) * 255u / 31u);
                b = (int)((f & 31u) * 255u / 31u);
            }
            /* Dark labels (menu FontColor=0,0,0): no FT shadow. Light text: tip shadow. */
            shadow = (r + g + b) >= 200 ? 1 : 0;
            ok = tip_font_draw_cbitmap_rgb_pt(a0, font, tx, ty, str, -1, r, g, b, shadow, ctext_pt);
            /* #region agent log */
            if (n <= 80 || !ok) {
                char esc[96];
                char js[800];
                int i = 0;
                esc[0] = 0;
                if (str) {
                    for (i = 0; i < 40 && str[i]; ++i) {
                        unsigned char c = (unsigned char)str[i];
                        esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
                    }
                    esc[i] = 0;
                }
                snprintf(js, sizeof(js),
                         "{\"n\":%ld,\"ok\":%d,\"xy\":[%d,%d],\"old_xy\":[%d,%d],\"geo_y\":%d,"
                         "\"origin_y\":%d,\"dY\":%d,\"nudge\":%d,\"ink_top\":%d,\"asc\":%d,"
                         "\"desc\":%d,\"lh\":%d,\"pt\":%d,\"face\":\"%s\",\"btn\":[%d,%d],"
                         "\"ft\":[%d,%d],\"rgb\":[%d,%d,%d],\"shadow\":%d,\"idx\":%d,"
                         "\"col\":%u,\"str\":\"%s\"}",
                         (long)n, ok, tx, ty, old_tx, old_ty, geo_ty, origin_y, ty - old_ty, nudge,
                         ink_top, asc, desc, line_h, ctext_pt, tip_font_ctext_style(), btn_w, btn_h,
                         tw, th, r, g, b, shadow, idx, col, esc);
                hooks_agent(ok ? "H-BTN-C" : "H-BAKE-FAIL", "hooks_zoom.c:CTextImage_Paint",
                            "ctext-ft", js);
            }
            /* #endregion */
            if (ok) {
                /* Child chrome first (same as retail), then FT text already in a0. */
                if (child && !IsBadReadPtr(child, 4)) {
                    void **cvt = *(void ***)child;
                    if (cvt && !IsBadReadPtr(cvt, 0x10)) {
                        PFN_Paint7 cp = (PFN_Paint7)cvt[0x0c / 4];
                        if (cp)
                            cp(child, a0, a1, a2, a3, a4, a5, a6);
                    }
                }
                /* Re-draw FT after child so text stays on top of button art. */
                tip_font_draw_cbitmap_rgb_pt(a0, font, tx, ty, str, -1, r, g, b, shadow, ctext_pt);
                if (tip_font_present_active())
                    tip_font_present_queue_str_surf(a0, tx, ty, str, -1, 0, r, g, b, shadow,
                                                    ctext_pt, 0);
                return;
            }
        }
    }

    real_CTextImage_Paint(self, a0, a1, a2, a3, a4, a5, a6);
}

static int __attribute__((thiscall)) hook_CBMPFont_Glyph(void *self, void *a0, void *a1, void *a2,
                                                         void *a3, void *a4)
{
    LONG n = InterlockedIncrement(&g_fg_n);
    /* #region agent log */
    if (n <= 120 || (n % 200) == 0) {
        char js[640];
        void *ret = __builtin_return_address(0);
        unsigned long fvt0 = 0, dest_vt0 = 0;
        int from_bake = 0, from_textw = 0, from_cfont = 0, from_dwide = 0;
        int dok = 0, dpath = 0, dw = 0, dh = 0;
        ULONG_PTR r = (ULONG_PTR)ret;
        /* 0x415fe0..0x41618b = CTextImage paint; 0x444770..0x444ad3 = TextW glyph loop. */
        if (r >= 0x00415fe0u && r < 0x00416190u)
            from_bake = 1;
        if (r >= 0x00444770u && r < 0x00444ae0u)
            from_textw = 1;
        if (r >= 0x00449810u && r < 0x00449ac0u)
            from_cfont = 1;
        if (r >= 0x00439440u && r < 0x004398a0u)
            from_dwide = 1;
        if (self && !IsBadReadPtr(self, 4)) {
            void **fvt = *(void ***)self;
            if (fvt && !IsBadReadPtr(fvt, 4))
                fvt0 = (unsigned long)(ULONG_PTR)fvt[0];
        }
        if (a0)
            dok = tip_font_probe_surf(a0, &dpath, &dw, &dh, &dest_vt0);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"from_ctext\":%d,\"from_textw\":%d,\"from_cfont\":%d,"
                 "\"from_dwide\":%d,\"ch\":%lu,"
                 "\"xy\":[%ld,%ld],\"dest\":%lu,\"dvt0\":%lu,\"dok\":%d,\"dpath\":%d,\"dwh\":[%d,%d],"
                 "\"fvt0\":%lu,\"ret\":%lu}",
                 (long)n, from_bake, from_textw, from_cfont, from_dwide,
                 (unsigned long)(ULONG_PTR)a1, (long)(LONG_PTR)a2, (long)(LONG_PTR)a3,
                 (unsigned long)(ULONG_PTR)a0, dest_vt0, dok, dpath, dw, dh, fvt0,
                 (unsigned long)(ULONG_PTR)ret);
        hooks_agent(from_dwide ? "H-LIST-E"
                               : (from_cfont ? "H-MISS-A"
                                             : (from_textw ? "H-TEXTW-A"
                                                           : (from_bake ? "H-BAKE-B" : "H-TEXTW-E"))),
                    "hooks_zoom.c:CBMPFont_Glyph", "font-glyph", js);
    }
    /* #endregion */
    return real_CBMPFont_Glyph(self, a0, a1, a2, a3, a4);
}

int ck_zoom_tip_overlay_get(int *x0, int *y0, int *x1, int *y1)
{
    if (!g_tip_on || !g_zoom_lb_active || g_zoom_closed)
        return 0;
    if (GetTickCount() - g_tip_tick > 800u)
        return 0;
    if (x0)
        *x0 = g_tip_x0;
    if (y0)
        *y0 = g_tip_y0;
    if (x1)
        *x1 = g_tip_x1;
    if (y1)
        *y1 = g_tip_y1;
    return 1;
}

int ck_zoom_tip_text_get(char *out, int out_n, int *x, int *y)
{
    int i;
    if (!g_tip_on || !g_zoom_lb_active || g_zoom_closed || !g_tip_str[0])
        return 0;
    if (GetTickCount() - g_tip_tick > 800u)
        return 0;
    if (out && out_n > 0) {
        for (i = 0; i < out_n - 1 && g_tip_str[i]; ++i)
            out[i] = g_tip_str[i];
        out[i] = 0;
    }
    if (x)
        *x = g_tip_draw_x;
    if (y)
        *y = g_tip_draw_y;
    return 1;
}

/*
 * H-GDI2: soft tip restore REJECTED (junk strip). Shift X + capture string; skip
 * native blit so soft stays clean; present redraws tip via GDI on fog.
 */
static int hook_DrawTextBmp(void *surf, void *font, int x, int y, const char *str, int maxlen,
                            int flags)
{
    int pL = 0, pT = 0, pR = 0, pB = 0, mL = 0, mT = 0, mR = 0, mB = 0;
    int lb = ck_zoom_letterbox_get(&pL, &pT, &pR, &pB, &mL, &mT, &mR, &mB);
    int shifted = 0, dx = 0;
    LONG n;
    void *retaddr = __builtin_return_address(0);

    if (lb && mL > pL && x >= 0 && x < 4000 && y >= 0 && y < 2000) {
        int tip_y0 = mB - 80;
        if (tip_y0 < mT)
            tip_y0 = mT;
        if (y >= tip_y0 && y <= mB + 8 && x >= mL - 48 && x <= mL + 120) {
            int tw = 200;
            int i;
            dx = mL - pL;
            x -= dx;
            shifted = 1;
            if (str && !IsBadReadPtr(str, 1)) {
                for (i = 0; i < (int)sizeof(g_tip_str) - 1 && str[i]; ++i)
                    g_tip_str[i] = str[i];
                g_tip_str[i] = 0;
                tw = i * 8 + 48;
                if (tw < 80)
                    tw = 80;
                if (tw > 1600)
                    tw = 1600;
            } else
                g_tip_str[0] = 0;
            g_tip_draw_x = x;
            g_tip_draw_y = y;
            g_tip_x0 = x - 4;
            g_tip_y0 = y - 4;
            g_tip_x1 = x + tw;
            g_tip_y1 = y + 28;
            g_tip_tick = GetTickCount();
            InterlockedExchange(&g_tip_on, 1);
        }
    }

    n = InterlockedIncrement(&g_dt_n);

    /* H-FTALL: DrawText arg0 is CMemoryDC → FreeType into its CBitmap.
     * Shifted ZoomMap tip: skip native (junk); FT paints memdc; present tip stays backup. */
    {
        static int s_ft_on = -1;
        int ok = 0;
        int menuish = 0;
        const char *s0 = "";
        if (str && !IsBadReadPtr(str, 1))
            s0 = str;
        /* Rough menu detect (EN keys); tip_font logs RU via CP1251 heuristic. */
        {
            static const char *const keys[] = {"Tutorial", "Adventure", "Single", "Multi",
                                               "Load",     "Editor",    "Options", "Credits",
                                               "Quit",     "Change",    "Tips",    "Next", NULL};
            int ki, j;
            for (ki = 0; keys[ki]; ++ki) {
                const char *k = keys[ki];
                for (j = 0; s0[j]; ++j) {
                    int m = 0;
                    while (k[m] && s0[j + m] &&
                           ((s0[j + m] | 32) == (k[m] | 32) || s0[j + m] == k[m]))
                        m++;
                    if (!k[m]) {
                        menuish = 1;
                        break;
                    }
                }
                if (menuish)
                    break;
            }
        }
        if (s_ft_on < 0)
            s_ft_on = env_on("CK_FT_DRAWTEXT", 1);
        /* Force re-read via helper so editor map tools use retail fonts. */
        s_ft_on = ft_drawtext_enabled() ? 1 : 0;
        if (s_ft_on) {
            /* #region agent log */
            {
                static volatile LONG s_probe;
                LONG pn = InterlockedIncrement(&s_probe);
                if ((pn <= 80 || menuish) && surf && !IsBadReadPtr(surf, 0x20)) {
                    char js[480];
                    void **vt = *(void ***)surf;
                    void *bmp = *(void **)((BYTE *)surf + 0x18);
                    int bw = 0, bh = 0, bb = 0, bp = 0;
                    if (bmp && !IsBadReadPtr(bmp, 0x10)) {
                        bw = *(short *)((BYTE *)bmp + 8);
                        bh = *(short *)((BYTE *)bmp + 0xa);
                        bb = *(short *)((BYTE *)bmp + 0xc);
                        bp = *(short *)((BYTE *)bmp + 0xe);
                    }
                    snprintf(js, sizeof(js),
                             "{\"n\":%ld,\"xy\":[%d,%d],\"dc8\":[%d,%d],\"bmp\":[%d,%d,%d,%d],"
                             "\"vt0\":%lu,\"flags\":%d,\"shifted\":%d,\"menu\":%d,\"ret\":%lu}",
                             (long)pn, x, y, (int)*(short *)((BYTE *)surf + 8),
                             (int)*(short *)((BYTE *)surf + 0xa), bw, bh, bb, bp,
                             vt ? (unsigned long)(ULONG_PTR)vt[0] : 0ul, flags, shifted, menuish,
                             (unsigned long)(ULONG_PTR)retaddr);
                    hooks_agent(menuish ? "H-MENU-C" : "H-FTALL", "hooks_zoom.c:DrawTextBmp",
                                "ft-probe", js);
                }
            }
            /* #endregion */
            /* Map/game tips: always soft FT. Present queue blinked when HUD
             * batches replaced sticky live without the tip. */
            ok = tip_font_draw_cbitmap(surf, font, x, y, str, maxlen, flags);
        }
        /* #region agent log */
        if (n <= 120 || menuish || shifted || lb) {
            char esc[96];
            char js[640];
            json_escape(esc, sizeof(esc), s0);
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"xy\":[%d,%d],\"flags\":%d,\"ok\":%d,\"ft\":%d,\"menu\":%d,"
                     "\"lb\":%d,\"shifted\":%d,\"ret\":%lu,\"str\":\"%s\"}",
                     (long)n, x, y, flags, ok, s_ft_on, menuish, lb, shifted,
                     (unsigned long)(ULONG_PTR)retaddr, esc);
            hooks_agent(menuish ? (ok ? "H-MENU-D" : "H-MENU-C") : "H-NAT",
                        "hooks_zoom.c:DrawTextBmp", "text-draw-xy", js);
        }
        /* #endregion */
        if (shifted)
            return 0; /* never native tip under letterbox */
        if (ok)
            return 1;
        /* #region agent log */
        if (menuish) {
            char esc[96];
            char js[320];
            json_escape(esc, sizeof(esc), s0);
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"flags\":%d,\"ret\":%lu,\"str\":\"%s\",\"native\":1}", (long)n,
                     flags, (unsigned long)(ULONG_PTR)retaddr, esc);
            hooks_agent("H-MENU-C", "hooks_zoom.c:DrawTextBmp", "ft-fallback-native", js);
        }
        /* #endregion */
    }
    return real_DrawTextBmp(surf, font, x, y, str, maxlen, flags);
}


void hooks_zoom_install(void)
{
    g_bz_target = (void *)(ULONG_PTR)0x00457D80u;
    if (*(BYTE *)g_bz_target == 0x8B) {
        if (install_inline_hook(g_bz_target, (void *)hook_BuildZoom, g_bz_steal, &g_bz_tramp,
                                g_bz_saved)) {
            real_BuildZoom = (PFN_BuildZoom)g_bz_tramp;
            log_msg("inline hooked BuildZoomMap @ %p", g_bz_target);
        } else
            log_msg("WARN: BuildZoomMap hook failed");
    } else
        log_msg("WARN: BuildZoomMap sig mismatch");

    g_az_target = (void *)(ULONG_PTR)0x00661FD0u;
    if (*(BYTE *)g_az_target == 0x83) {
        if (install_inline_hook(g_az_target, (void *)hook_AttachZoom, g_az_steal, &g_az_tramp,
                                g_az_saved)) {
            real_AttachZoom = (PFN_AttachZoom)g_az_tramp;
            log_msg("inline hooked AttachZoomMap @ %p", g_az_target);
        } else
            log_msg("WARN: AttachZoomMap hook failed");
    } else
        log_msg("WARN: AttachZoomMap sig mismatch");

    /* Before ZoomCreate @ 0x58074e (also when BuildZoom skipped). */
    g_zbind_target = (void *)(ULONG_PTR)0x00580730u;
    if (*(BYTE *)g_zbind_target == 0xA1) {
        if (install_inline_hook(g_zbind_target, (void *)hook_ZoomBindEntry, g_zbind_steal,
                                &g_zbind_tramp, g_zbind_saved)) {
            log_msg("inline hooked ZoomBind entry @ %p", g_zbind_target);
        } else
            log_msg("WARN: ZoomBind entry hook failed");
    } else
        log_msg("WARN: ZoomBind entry sig mismatch");

    g_hz_target = (void *)(ULONG_PTR)0x00456A20u;
    g_sz_target = (void *)(ULONG_PTR)0x00456A30u;
    g_tz_target = (void *)(ULONG_PTR)0x00456A10u;
    if (*(BYTE *)g_hz_target == 0x8B) {
        if (install_inline_hook(g_hz_target, (void *)hook_HideZoomMap, g_hz_steal, &g_hz_tramp,
                                g_hz_saved)) {
            real_HideZoomMap = (PFN_ZoomCmd)g_hz_tramp;
            log_msg("inline hooked HideZoomMap @ %p", g_hz_target);
        } else
            log_msg("WARN: HideZoomMap hook failed");
    }
    if (*(BYTE *)g_sz_target == 0x8B) {
        if (install_inline_hook(g_sz_target, (void *)hook_ShowZoomMap, g_sz_steal, &g_sz_tramp,
                                g_sz_saved)) {
            real_ShowZoomMap = (PFN_ZoomCmd)g_sz_tramp;
            log_msg("inline hooked ShowZoomMap @ %p", g_sz_target);
        } else
            log_msg("WARN: ShowZoomMap hook failed");
    }
    if (*(BYTE *)g_tz_target == 0x8B) {
        if (install_inline_hook(g_tz_target, (void *)hook_ToggleZoomMap, g_tz_steal, &g_tz_tramp,
                                g_tz_saved)) {
            real_ToggleZoomMap = (PFN_ZoomCmd)g_tz_tramp;
            log_msg("inline hooked ToggleZoomMap @ %p", g_tz_target);
        } else
            log_msg("WARN: ToggleZoomMap hook failed");
    }

    /* Zoom mode end (clears this-0xA8 flag) — real close path. */
    g_zem_target = (void *)(ULONG_PTR)0x00456990u;
    if (*(BYTE *)g_zem_target == 0x56) {
        if (install_inline_hook(g_zem_target, (void *)hook_ZoomEndMode, g_zem_steal, &g_zem_tramp,
                                g_zem_saved)) {
            real_ZoomEndMode = (PFN_ZoomEndMode)g_zem_tramp;
            log_msg("inline hooked ZoomEndMode @ %p", g_zem_target);
        } else
            log_msg("WARN: ZoomEndMode hook failed");
    } else
        log_msg("WARN: ZoomEndMode sig mismatch");

    /* Bitmap font DrawText — locate HelpText X under ZoomMap letterbox. */
    g_dt_target = (void *)(ULONG_PTR)0x004025E0u;
    if (*(BYTE *)g_dt_target == 0xB8) {
        if (install_inline_hook(g_dt_target, (void *)hook_DrawTextBmp, g_dt_steal, &g_dt_tramp,
                                g_dt_saved)) {
            real_DrawTextBmp = (PFN_DrawTextBmp)g_dt_tramp;
            log_msg("inline hooked DrawTextBmp @ %p", g_dt_target);
        } else
            log_msg("WARN: DrawTextBmp hook failed");
    } else
        log_msg("WARN: DrawTextBmp sig mismatch");

    /* TextW paint — tips/version (steal=8, full 2×MOV prolog). */
    g_tw_target = (void *)(ULONG_PTR)0x00445C10u;
    if (*(DWORD *)g_tw_target == 0x0c24448bu) { /* mov eax,[esp+0xc] */
        if (install_inline_hook(g_tw_target, (void *)hook_TextW_Paint, g_tw_steal, &g_tw_tramp,
                                g_tw_saved)) {
            real_TextW_Paint = (PFN_TextW_Paint)g_tw_tramp;
            log_msg("inline hooked TextW_Paint @ %p steal=%u", g_tw_target, (unsigned)g_tw_steal);
        } else
            log_msg("WARN: TextW_Paint hook failed");
    } else
        log_msg("WARN: TextW_Paint sig mismatch");

    /* TextW glyph blit — dest is CMemoryDC (steal=5, alloca prolog). */
    g_twb_target = (void *)(ULONG_PTR)0x00444770u;
    if (*(BYTE *)g_twb_target == 0xB8) {
        if (install_inline_hook(g_twb_target, (void *)hook_TextW_Blit, g_twb_steal, &g_twb_tramp,
                                g_twb_saved)) {
            real_TextW_Blit = (PFN_TextW_Blit)g_twb_tramp;
            log_msg("inline hooked TextW_Blit @ %p steal=%u", g_twb_target, (unsigned)g_twb_steal);
        } else
            log_msg("WARN: TextW_Blit hook failed");
    } else
        log_msg("WARN: TextW_Blit sig mismatch");

    /* CTextImage bake — ImageButton Text= labels (steal=6). */
    g_tib_target = (void *)(ULONG_PTR)0x00416350u;
    if (*(BYTE *)g_tib_target == 0x83 && *((BYTE *)g_tib_target + 1) == 0xec) {
        if (install_inline_hook(g_tib_target, (void *)hook_CTextImage_Bake, g_tib_steal,
                                &g_tib_tramp, g_tib_saved)) {
            real_CTextImage_Bake = (PFN_CTextImage_Bake)g_tib_tramp;
            log_msg("inline hooked CTextImage_Bake @ %p steal=%u", g_tib_target,
                    (unsigned)g_tib_steal);
        } else
            log_msg("WARN: CTextImage_Bake hook failed");
    } else
        log_msg("WARN: CTextImage_Bake sig mismatch");

    /* CTextImage paint — rasters label glyphs into button frame (steal=5). */
    g_tipaint_target = (void *)(ULONG_PTR)0x00415FE0u;
    if (*(BYTE *)g_tipaint_target == 0x53) {
        if (install_inline_hook(g_tipaint_target, (void *)hook_CTextImage_Paint, g_tipaint_steal,
                                &g_tipaint_tramp, g_tipaint_saved)) {
            real_CTextImage_Paint = (PFN_CTextImage_Paint)g_tipaint_tramp;
            log_msg("inline hooked CTextImage_Paint @ %p steal=%u", g_tipaint_target,
                    (unsigned)g_tipaint_steal);
        } else
            log_msg("WARN: CTextImage_Paint hook failed");
    } else
        log_msg("WARN: CTextImage_Paint sig mismatch");

    /* CBMPFont glyph — shared by DrawTextBmp + CTextImage paint (steal=6). */
    g_fg_target = (void *)(ULONG_PTR)0x00403030u;
    if (*(BYTE *)g_fg_target == 0x83 && *((BYTE *)g_fg_target + 1) == 0xec) {
        if (install_inline_hook(g_fg_target, (void *)hook_CBMPFont_Glyph, g_fg_steal, &g_fg_tramp,
                                g_fg_saved)) {
            real_CBMPFont_Glyph = (PFN_CBMPFont_Glyph)g_fg_tramp;
            log_msg("inline hooked CBMPFont_Glyph @ %p steal=%u", g_fg_target,
                    (unsigned)g_fg_steal);
        } else
            log_msg("WARN: CBMPFont_Glyph hook failed");
    } else
            log_msg("WARN: CBMPFont_Glyph sig mismatch");

    /* CUIText paint — ANSI Type=Text still on APF (steal=6). */
    g_ct_target = (void *)(ULONG_PTR)0x004497C0u;
    if (*(BYTE *)g_ct_target == 0x56) {
        if (install_inline_hook(g_ct_target, (void *)hook_CUIText_Paint, g_ct_steal, &g_ct_tramp,
                                g_ct_saved)) {
            real_CUIText_Paint = (PFN_CUIText_Paint)g_ct_tramp;
            log_msg("inline hooked CUIText_Paint @ %p steal=%u", g_ct_target, (unsigned)g_ct_steal);
        } else
            log_msg("WARN: CUIText_Paint hook failed");
    } else
        log_msg("WARN: CUIText_Paint sig mismatch");

    /* CFont::DrawString — remaining APF glyph loop (steal=6). */
    g_cfd_target = (void *)(ULONG_PTR)0x00449810u;
    if (*(BYTE *)g_cfd_target == 0x83 && *((BYTE *)g_cfd_target + 1) == 0xec) {
        if (install_inline_hook(g_cfd_target, (void *)hook_CFont_Draw, g_cfd_steal, &g_cfd_tramp,
                                g_cfd_saved)) {
            real_CFont_Draw = (PFN_CFont_Draw)g_cfd_tramp;
            log_msg("inline hooked CFont_Draw @ %p steal=%u", g_cfd_target, (unsigned)g_cfd_steal);
        } else
            log_msg("WARN: CFont_Draw hook failed");
    } else
        log_msg("WARN: CFont_Draw sig mismatch");

    /* DrawWide — tree/combo UTF-16 blit (steal=7). */
    g_dw_target = (void *)(ULONG_PTR)0x00439440u;
    if (*(BYTE *)g_dw_target == 0x8b && *((BYTE *)g_dw_target + 1) == 0x44) {
        if (install_inline_hook(g_dw_target, (void *)hook_DrawWide, g_dw_steal, &g_dw_tramp,
                                g_dw_saved)) {
            real_DrawWide = (PFN_DrawWide)g_dw_tramp;
            log_msg("inline hooked DrawWide @ %p steal=%u", g_dw_target, (unsigned)g_dw_steal);
        } else
            log_msg("WARN: DrawWide hook failed");
    } else
        log_msg("WARN: DrawWide sig mismatch");

    /* DrawAnsi — ANSI twin of DrawWide (steal=10). */
    g_da_target = (void *)(ULONG_PTR)0x004398A0u;
    if (*(BYTE *)g_da_target == 0x8b && *((BYTE *)g_da_target + 1) == 0x44) {
        if (install_inline_hook(g_da_target, (void *)hook_DrawAnsi, g_da_steal, &g_da_tramp,
                                g_da_saved)) {
            real_DrawAnsi = (PFN_DrawAnsi)g_da_tramp;
            log_msg("inline hooked DrawAnsi @ %p steal=%u", g_da_target, (unsigned)g_da_steal);
        } else
            log_msg("WARN: DrawAnsi hook failed");
    } else
        log_msg("WARN: DrawAnsi sig mismatch");

}

void hooks_zoom_remove(void)
{
    remove_inline_hook(g_bz_target, g_bz_steal, g_bz_saved, g_bz_tramp);
    g_bz_tramp = NULL;
    remove_inline_hook(g_az_target, g_az_steal, g_az_saved, g_az_tramp);
    g_az_tramp = NULL;
    remove_inline_hook(g_zbind_target, g_zbind_steal, g_zbind_saved, g_zbind_tramp);
    g_zbind_tramp = NULL;
    remove_inline_hook(g_hz_target, g_hz_steal, g_hz_saved, g_hz_tramp);
    g_hz_tramp = NULL;
    remove_inline_hook(g_sz_target, g_sz_steal, g_sz_saved, g_sz_tramp);
    g_sz_tramp = NULL;
    remove_inline_hook(g_tz_target, g_tz_steal, g_tz_saved, g_tz_tramp);
    g_tz_tramp = NULL;
    remove_inline_hook(g_zem_target, g_zem_steal, g_zem_saved, g_zem_tramp);
    g_zem_tramp = NULL;
    remove_inline_hook(g_dt_target, g_dt_steal, g_dt_saved, g_dt_tramp);
    g_dt_tramp = NULL;
    remove_inline_hook(g_tw_target, g_tw_steal, g_tw_saved, g_tw_tramp);
    g_tw_tramp = NULL;
    remove_inline_hook(g_twb_target, g_twb_steal, g_twb_saved, g_twb_tramp);
    g_twb_tramp = NULL;
    remove_inline_hook(g_tib_target, g_tib_steal, g_tib_saved, g_tib_tramp);
    g_tib_tramp = NULL;
    remove_inline_hook(g_tipaint_target, g_tipaint_steal, g_tipaint_saved, g_tipaint_tramp);
    g_tipaint_tramp = NULL;
    remove_inline_hook(g_fg_target, g_fg_steal, g_fg_saved, g_fg_tramp);
    g_fg_tramp = NULL;
    remove_inline_hook(g_ct_target, g_ct_steal, g_ct_saved, g_ct_tramp);
    g_ct_tramp = NULL;
    remove_inline_hook(g_cfd_target, g_cfd_steal, g_cfd_saved, g_cfd_tramp);
    g_cfd_tramp = NULL;
    remove_inline_hook(g_dw_target, g_dw_steal, g_dw_saved, g_dw_tramp);
    g_dw_tramp = NULL;
    remove_inline_hook(g_da_target, g_da_steal, g_da_saved, g_da_tramp);
    g_da_tramp = NULL;

}
