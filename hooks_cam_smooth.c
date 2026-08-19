#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "vk_present.h"
#include "hitch.h"

#include <stdio.h>

/* Avoid vk_terrain.h (pulls vulkan.h); only need GPU-draw gate for CamLight. */
int vk_terrain_ready(void);
int vk_terrain_draw_enabled(void);

/*
 * Keyboard pan: CenterOn only, present-dt integrated, soft step cap.
 *
 * scroll-freq-1: dual-drive (present + retail timer 50Hz).
 * scroll-freq-2: paced to ScrollRefresh + stepcap 8 → too slow.
 * scroll-freq-3: paced + ScrollStepMax → d=72..128, hard jerk; eat_n=0
 *                while tmr_n=580 (timer still moved cam).
 *
 * scroll-freq-4:
 *  - ALWAYS eat retail scroll timer while CK_CAM_SMOOTH (ASI owns cam)
 *  - per-present time integration (no ScrollRefresh bucket)
 *  - visual step softcap 16 (smooth); speed from factor band (~1802)
 */

enum {
    CK_SCROLL_HANDLER = 0x005924D0u,
    CK_SCROLL_SPEED = 0x008DDD18u,
    CK_STEP_ALIGN = 0x0076AD70u,
    CK_SCROLL_REFRESH = 0x0076AD64u,
    CK_SPD_MIN = 0x0076AD58u,
    CK_SPD_MAX = 0x0076AD5Cu,
    CK_SCROLL_DIR = 0x008DDD2Cu,
    CK_SPEED_FACTOR = 0x009AFFFCu,
    CK_CAM_PTR = 0x007CA604u,
    CK_CAM_CENTER = 0x0047C050u,
    CK_SCROLL_BLIT = 0x008E2C80u,
    CK_CAM_LEFT = 0x4C8u,
    CK_CAM_TOP = 0x4CCu,
    CK_CAM_RIGHT = 0x4D0u,
    CK_CAM_BOTTOM = 0x4D4u,
    CK_MSG_KEYDOWN = 0x201u,
    CK_MSG_KEYUP = 0x202u,
    CK_MSG_SCROLL_TMR = 0x19A87453u
};

#define CK_DT_CLAMP 0.025f
#define CK_PAN_VISUAL_MAX 16
#define CK_PAN_MAX_ACC 24.0f

typedef int (*PFN_ScrollMsg)(void *msg);
typedef void(__attribute__((thiscall)) *PFN_CamCenter)(void *cam, int x, int y, int flags);
typedef void(__attribute__((thiscall)) *PFN_ScrollBlit)(void *self, int dx, int dy);

static PFN_ScrollMsg real_ScrollMsg;
static BYTE *g_sc_tramp;
static BYTE g_sc_saved[16];
static SIZE_T g_sc_steal = 6;
static void *g_sc_target;
static volatile LONG g_pan_n, g_block_n, g_clamp_n, g_tmr_eat_n;
static int g_cam_pan = 1;
static int g_cam_light = 1; /* GPU terrain: nudge LTRB, skip retail CamCenter */
static float g_speed_mul = 1.0f;
static LARGE_INTEGER g_qpc_freq;
static LONGLONG g_last_qpc;
static LONGLONG g_last_tmr_qpc;
static int g_qpc_ok;
static float g_acc_x, g_acc_y;
static int g_light_acc; /* px scrolled on light path since last full CamCenter */
static int g_was_panning;
static volatile LONG g_sync_n;

static DWORD read_u32(DWORD va)
{
    if (IsBadReadPtr((void *)(ULONG_PTR)va, 4))
        return 0;
    return *(DWORD *)(ULONG_PTR)va;
}

static int is_arrow_vk(DWORD vk)
{
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
}

static int retail_align(void)
{
    int a = (int)read_u32(CK_STEP_ALIGN);
    return a > 0 ? a : 4;
}

static float retail_scroll_speed(void)
{
    DWORD factor = read_u32(CK_SPEED_FACTOR);
    DWORD mn = read_u32(CK_SPD_MIN);
    DWORD mx = read_u32(CK_SPD_MAX);
    DWORD rt;
    float spd;

    if (mn == 0)
        mn = 512;
    if (mx <= mn)
        mx = mn + 1536;
    if (factor > 0 && factor <= 200) {
        spd = (float)mn + (float)(mx - mn) * (float)factor / 100.0f;
        return spd;
    }
    rt = read_u32(CK_SCROLL_SPEED);
    if (rt >= 200)
        return (float)rt;
    return 1802.0f;
}

static void clamp_acc(float *a)
{
    if (*a > CK_PAN_MAX_ACC)
        *a = CK_PAN_MAX_ACC;
    else if (*a < -CK_PAN_MAX_ACC)
        *a = -CK_PAN_MAX_ACC;
}

static int take_aligned(float *acc, int align, int max_step)
{
    int v, sign, mag, out;

    if (align < 1)
        align = 4;
    v = (int)*acc;
    if (v == 0)
        return 0;
    sign = v < 0 ? -1 : 1;
    mag = v < 0 ? -v : v;
    mag -= mag % align;
    if (mag == 0)
        return 0;
    if (mag > max_step) {
        mag = max_step - (max_step % align);
        if (mag < align)
            mag = align;
    }
    out = sign * mag;
    *acc -= (float)out;
    return out;
}

static int cam_nudge_ltrb(int dx, int dy)
{
    DWORD cam;
    LONG *pl, *pt, *pr, *pb;

    cam = read_u32(CK_CAM_PTR);
    if (!cam || IsBadReadPtr((void *)(ULONG_PTR)(cam + CK_CAM_BOTTOM), 4))
        return 0;
    pl = (LONG *)(ULONG_PTR)(cam + CK_CAM_LEFT);
    pt = (LONG *)(ULONG_PTR)(cam + CK_CAM_TOP);
    pr = (LONG *)(ULONG_PTR)(cam + CK_CAM_RIGHT);
    pb = (LONG *)(ULONG_PTR)(cam + CK_CAM_BOTTOM);
    *pl += dx;
    *pt += dy;
    *pr += dx;
    *pb += dy;
    return 1;
}

static int cam_light_ok(void)
{
    return g_cam_light && vk_terrain_ready() && vk_terrain_draw_enabled();
}

/* force_full: 1 = always retail CamCenter (+ScrollBlit). dx=dy=0 refreshes visibility
 * at the current camera without moving (pan-end sync for MapObj soft/GPU spawn). */
static void cam_scroll_by(int dx, int dy, DWORD *soft_ptr, int *blit_ok, double *center_ms,
                          double *blit_ms, int *light_used, int force_full)
{
    DWORD cam, soft;
    LONG L, T, R, B;
    PFN_CamCenter center;
    void *blit;
    void **vt;
    PFN_ScrollBlit sblit;
    LONGLONG t0 = 0, t1 = 0;
    int use_light;

    if (soft_ptr)
        *soft_ptr = 0;
    if (blit_ok)
        *blit_ok = 0;
    if (center_ms)
        *center_ms = 0.0;
    if (blit_ms)
        *blit_ms = 0.0;
    if (light_used)
        *light_used = 0;
    if (!force_full && !dx && !dy)
        return;

    /* H-WALL: pure light pan skipped CamCenter → soft MapObj / CreateVisible for walls
     * never ran. Mid-scroll full CamCenter every ~192px caused FPS dips (~20–30ms).
     * Keep light for all continuous pans; pan-end force_full sync restores walls. */
    use_light = !force_full && cam_light_ok() && (dx || dy);
    if (use_light) {
        hitch_mark("CamLight");
        if (g_qpc_ok)
            t0 = hitch_qpc_now();
        if (cam_nudge_ltrb(dx, dy)) {
            g_light_acc += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (center_ms && g_qpc_ok && t0)
                *center_ms = hitch_qpc_ms_since(t0);
            if (light_used)
                *light_used = 1;
            return;
        }
    }

    cam = read_u32(CK_CAM_PTR);
    if (!cam)
        return;
    mm_read_cam(&L, &T, &R, &B);
    center = (PFN_CamCenter)(ULONG_PTR)CK_CAM_CENTER;
    hitch_mark(force_full ? "CamSync" : "CamCenter");
    if (g_qpc_ok)
        t0 = hitch_qpc_now();
    center((void *)(ULONG_PTR)cam, (int)((L + R) / 2 + dx), (int)((T + B) / 2 + dy), 0);
    if (center_ms && g_qpc_ok && t0)
        *center_ms = hitch_qpc_ms_since(t0);
    g_light_acc = 0;

    soft = read_u32(CK_SCROLL_BLIT);
    if (soft_ptr)
        *soft_ptr = soft;
    if (!dx && !dy)
        return; /* refresh-only: skip ScrollBlit */
    if (!soft || IsBadReadPtr((void *)(ULONG_PTR)soft, 4))
        return;
    blit = (void *)(ULONG_PTR)soft;
    vt = *(void ***)blit;
    if (!vt || IsBadReadPtr(vt, 0xa0))
        return;
    sblit = (PFN_ScrollBlit)vt[0x9c / 4];
    if (!sblit)
        return;
    hitch_mark("ScrollBlit");
    if (g_qpc_ok)
        t1 = hitch_qpc_now();
    sblit(blit, dx, dy);
    if (blit_ms && g_qpc_ok && t1)
        *blit_ms = hitch_qpc_ms_since(t1);
    if (blit_ok)
        *blit_ok = 1;
}

static DWORD read_arrow_dir(void)
{
    DWORD d = 0;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)
        d |= 8u;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
        d |= 4u;
    if (GetAsyncKeyState(VK_UP) & 0x8000)
        d |= 2u;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)
        d |= 1u;
    return d;
}

static int hook_ScrollMsg(void *msg)
{
    DWORD id = 0, vk = 0;
    LONG n;
    char js[480];

    if (!g_cam_pan || !msg || IsBadReadPtr(msg, 0x14))
        return real_ScrollMsg(msg);

    id = *(DWORD *)((BYTE *)msg + 0xc);
    if (id == CK_MSG_KEYDOWN || id == CK_MSG_KEYUP) {
        vk = (*(DWORD *)((BYTE *)msg + 0x10)) >> 16;
        if (is_arrow_vk(vk)) {
            InterlockedIncrement(&g_block_n);
            /* #region agent log */
            n = g_block_n;
            if (n <= 30 || (n % 40) == 0) {
                snprintf(js, sizeof(js),
                         "{\"n\":%ld,\"id\":%lu,\"vk\":%lu,\"blocked\":1,"
                         "\"runId\":\"scroll-freq-4\"}",
                         (long)n, (unsigned long)id, (unsigned long)vk);
                hooks_agent("H-S1", "hooks_cam_smooth.c:ScrollMsg", "scroll-key-block", js);
            }
            /* #endregion */
            return 1;
        }
    }

    /* Always eat retail scroll timer — ASI is sole camera mover (freq-3: eat_n=0
     * while tmr_n=580 caused dual-drive jerks). */
    if (id == CK_MSG_SCROLL_TMR) {
        LARGE_INTEGER now;
        double dt_ms = -1.0;
        n = InterlockedIncrement(&g_tmr_eat_n);
        if (g_qpc_ok && QueryPerformanceCounter(&now)) {
            if (g_last_tmr_qpc)
                dt_ms = (double)(now.QuadPart - g_last_tmr_qpc) * 1000.0 /
                        (double)g_qpc_freq.QuadPart;
            g_last_tmr_qpc = now.QuadPart;
        }
        /* #region agent log */
        if (n <= 60 || (n % 40) == 0) {
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"ate\":1,\"dt_ms\":%.3f,\"hz\":%.1f,"
                     "\"ScrollRefresh\":%lu,\"pan_n\":%ld,\"dir_bits\":%lu,"
                     "\"runId\":\"scroll-freq-4\"}",
                     (long)n, dt_ms, dt_ms > 0.1 ? 1000.0 / dt_ms : 0.0,
                     (unsigned long)read_u32(CK_SCROLL_REFRESH), (long)g_pan_n,
                     (unsigned long)read_u32(CK_SCROLL_DIR));
            hooks_agent("H-S4", "hooks_cam_smooth.c:ScrollMsg", "scroll-timer-eat", js);
        }
        /* #endregion */
        return 1;
    }

    return real_ScrollMsg(msg);
}

void cam_smooth_on_frame(void)
{
}

void cam_smooth_on_present(void)
{
    LARGE_INTEGER now;
    float raw_dt, dt, speed, move;
    DWORD dir;
    int dx, dy, align, max_step, clamped;
    LONG L0, T0, R0, B0, L1, T1, R1, B1;
    LONG n;
    DWORD soft_ptr = 0;
    int blit_ok = 0;
    int light_used = 0;
    double center_ms = 0.0, blit_ms = 0.0;
    char js[800];

    if (!g_cam_pan)
        return;
    if (!read_u32(CK_CAM_PTR))
        return;
    if (!g_qpc_ok || !QueryPerformanceCounter(&now))
        return;

    vk_present_set_pan_shift(0, 0);

    if (!g_last_qpc) {
        g_last_qpc = now.QuadPart;
        return;
    }

    raw_dt = (float)(now.QuadPart - g_last_qpc) / (float)g_qpc_freq.QuadPart;
    g_last_qpc = now.QuadPart;
    if (raw_dt <= 0.0f)
        return;

    clamped = 0;
    dt = raw_dt;
    if (dt > CK_DT_CLAMP) {
        dt = CK_DT_CLAMP;
        clamped = 1;
        InterlockedIncrement(&g_clamp_n);
    }

    dir = read_arrow_dir();
    if (!dir) {
        /* Edge scroll: retail dir flags (timer eaten, so we honor bits here). */
        dir = read_u32(CK_SCROLL_DIR) & 0xfu;
    }
    if (!dir) {
        g_acc_x = g_acc_y = 0.0f;
        /* After a light-pan streak, one CamCenter at current pos restores MapObj visibility
         * (walls/props that enter view via soft CreateVisible). */
        if (g_was_panning && cam_light_ok()) {
            double sync_ms = 0.0;
            int lu = 0;
            LONG sn;
            int acc_before = g_light_acc;
            g_was_panning = 0;
            /* Skip sync if we barely moved — avoids hitch on tiny taps. */
            if (acc_before < 8) {
                g_light_acc = 0;
                return;
            }
            cam_scroll_by(0, 0, &soft_ptr, &blit_ok, &sync_ms, &blit_ms, &lu, 1);
            sn = InterlockedIncrement(&g_sync_n);
            /* #region agent log */
            if (sn <= 40 || (sn % 20) == 0 || sync_ms >= 8.0) {
                char data[192];
                snprintf(data, sizeof(data),
                         "{\"n\":%ld,\"why\":\"pan-end\",\"center_ms\":%.3f,\"light_acc\":%d,"
                         "\"runId\":\"cam-light-2\"}",
                         (long)sn, sync_ms, acc_before);
                hooks_agent("H-WALL", "hooks_cam_smooth.c:on_present", "cam-sync", data);
            }
            /* #endregion */
        }
        return;
    }

    speed = retail_scroll_speed() * g_speed_mul;
    move = speed * dt;

    if (dir & 8u)
        g_acc_x -= move;
    if (dir & 4u)
        g_acc_x += move;
    if (dir & 2u)
        g_acc_y -= move;
    if (dir & 1u)
        g_acc_y += move;
    if ((dir & 12u) == 12u)
        g_acc_x = 0.0f;
    if ((dir & 3u) == 3u)
        g_acc_y = 0.0f;

    clamp_acc(&g_acc_x);
    clamp_acc(&g_acc_y);

    align = retail_align();
    max_step = CK_PAN_VISUAL_MAX;
    if (max_step < align)
        max_step = align;
    dx = take_aligned(&g_acc_x, align, max_step);
    dy = take_aligned(&g_acc_y, align, max_step);
    if (!dx && !dy) {
        /* #region agent log */
        n = InterlockedIncrement(&g_pan_n);
        if (n <= 40 || (n % 30) == 0) {
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"dir\":%lu,\"d\":[0,0],\"acc\":[%.2f,%.2f],\"raw_dt\":%.4f,"
                     "\"why\":\"acc_below_align\"}",
                     (long)n, (unsigned long)dir, (double)g_acc_x, (double)g_acc_y,
                     (double)raw_dt);
            hooks_agent("H-CAM", "hooks_cam_smooth.c:on_present", "cam-pan", js);
        }
        /* #endregion */
        return;
    }

    mm_read_cam(&L0, &T0, &R0, &B0);
    cam_scroll_by(dx, dy, &soft_ptr, &blit_ok, &center_ms, &blit_ms, &light_used, 0);
    mm_read_cam(&L1, &T1, &R1, &B1);
    g_was_panning = 1;

    n = InterlockedIncrement(&g_pan_n);
    /* #region agent log */
    if (n <= 200 || (n % 10) == 0 || center_ms >= 2.0 || blit_ms >= 2.0 || !light_used) {
        LARGE_INTEGER qpc;
        float expect_px = speed * dt;
        QueryPerformanceCounter(&qpc);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"dir\":%lu,\"raw_dt\":%.4f,\"dt\":%.4f,\"clamped\":%d,"
                 "\"present_hz\":%.1f,\"speed\":%.0f,\"expect_px\":%.1f,"
                 "\"align\":%d,\"max\":%d,\"d\":[%d,%d],\"got\":[%ld,%ld],"
                 "\"acc\":[%.2f,%.2f],\"center_ms\":%.3f,\"blit_ms\":%.3f,\"soft\":%lu,"
                 "\"blit_ok\":%d,\"light\":%d,\"light_acc\":%d,\"clamp_n\":%ld,\"eat_n\":%ld,"
                 "\"block_n\":%ld,\"qpc\":%llu}",
                 (long)n, (unsigned long)dir, (double)raw_dt, (double)dt, clamped,
                 raw_dt > 0.0001f ? (1.0 / (double)raw_dt) : 0.0, (double)speed,
                 (double)expect_px, align, max_step, dx, dy, (long)(L1 - L0),
                 (long)(T1 - T0), (double)g_acc_x, (double)g_acc_y, center_ms, blit_ms,
                 (unsigned long)soft_ptr, blit_ok, light_used, g_light_acc, (long)g_clamp_n,
                 (long)g_tmr_eat_n, (long)g_block_n, (unsigned long long)qpc.QuadPart);
        hooks_agent("H-CAM", "hooks_cam_smooth.c:on_present", "cam-pan", js);
        if (!light_used && center_ms + blit_ms >= 8.0) {
            char extra[240];
            /* #region agent log */
            snprintf(extra, sizeof(extra),
                     "{\"center_ms\":%.3f,\"blit_ms\":%.3f,\"d\":[%d,%d],\"dir\":%lu,"
                     "\"cam\":[%ld,%ld,%ld,%ld],\"light\":0,\"light_acc\":%d}",
                     center_ms, blit_ms, dx, dy, (unsigned long)dir, (long)L1, (long)T1,
                     (long)R1, (long)B1, g_light_acc);
            hitch_note_ms("H-CAM", "hooks_cam_smooth.c:on_present", "cam-slow", "CamCenter",
                          center_ms + blit_ms, extra);
            if (center_ms >= 16.0)
                hooks_agent("H-LOC", "hooks_cam_smooth.c:on_present", "cam-center-slow", extra);
            /* #endregion */
        }
    }
    /* #endregion */
}

void hooks_cam_smooth_install(void)
{
    char buf[32];
    DWORD n;

    if (!looks_like_tpw()) {
        log_msg("cam_pan: skip (not tpw)");
        return;
    }
    g_cam_pan = env_on("CK_CAM_SMOOTH", 1);
    g_cam_light = env_on("CK_CAM_LIGHT", 1);
    g_speed_mul = 1.0f;
    n = GetEnvironmentVariableA("CK_CAM_SPEED", buf, (DWORD)sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        float m = (float)atof(buf);
        if (m >= 0.25f && m <= 3.0f)
            g_speed_mul = m;
    }
    g_qpc_ok = QueryPerformanceFrequency(&g_qpc_freq) && g_qpc_freq.QuadPart > 0;
    g_last_qpc = 0;
    g_last_tmr_qpc = 0;
    g_acc_x = g_acc_y = 0.0f;
    g_light_acc = 0;
    g_was_panning = 0;
    g_clamp_n = 0;
    g_tmr_eat_n = 0;
    g_block_n = 0;
    g_pan_n = 0;

    g_sc_target = (void *)(ULONG_PTR)CK_SCROLL_HANDLER;
    if (install_inline_hook(g_sc_target, (void *)hook_ScrollMsg, g_sc_steal, &g_sc_tramp,
                            g_sc_saved)) {
        real_ScrollMsg = (PFN_ScrollMsg)g_sc_tramp;
        log_msg("cam_pan: present-dt softcap=%d eat-timer speed*=%.2f light=%d", CK_PAN_VISUAL_MAX,
                (double)g_speed_mul, g_cam_light);
        /* #region agent log */
        hooks_agent("H-CAM", "hooks_cam_smooth.c:install", "cam-install-ok",
                    "{\"tpwCached\":1,\"afterTerrainHooks\":1}");
        /* #endregion */
    } else {
        log_msg("cam_pan: FAILED block hook");
        /* #region agent log */
        hooks_agent("H-CAM", "hooks_cam_smooth.c:install", "cam-install-fail", "{}");
        /* #endregion */
    }
}

void hooks_cam_smooth_remove(void)
{
    if (g_sc_target && g_sc_tramp)
        remove_inline_hook(g_sc_target, g_sc_steal, g_sc_saved, g_sc_tramp);
    g_sc_tramp = NULL;
    real_ScrollMsg = NULL;
}
