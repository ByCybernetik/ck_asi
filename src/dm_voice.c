#include "dm_replace_internal.h"

/* Camera / beacon / live voice playback */
/* Game 0x47E4A0 thiscall: transform world frustum → view AABB (same space as unit x/y). */
typedef void(__attribute__((thiscall)) *CkXformRectFn)(void *cam, LONG *out, LONG left, LONG top,
                                                         LONG right, LONG bottom);

static int cam_frustum_xform(LONG *oL, LONG *oT, LONG *oR, LONG *oB)
{
    DWORD cam;
    LONG left, top, right, bottom, out[4];
    CkXformRectFn fn;

    if (IsBadReadPtr((void *)(ULONG_PTR)CK_CAM_PTR_VA, 4))
        return 0;
    cam = *(DWORD *)(ULONG_PTR)CK_CAM_PTR_VA;
    if (!cam || IsBadReadPtr((void *)(ULONG_PTR)(cam + CK_CAM_BOTTOM_OFF), 4))
        return 0;
    if (IsBadCodePtr((FARPROC)(ULONG_PTR)CK_XFORM_RECT_VA))
        return 0;
    left = *(LONG *)(ULONG_PTR)(cam + CK_CAM_LEFT_OFF);
    top = *(LONG *)(ULONG_PTR)(cam + CK_CAM_TOP_OFF);
    right = *(LONG *)(ULONG_PTR)(cam + CK_CAM_RIGHT_OFF);
    bottom = *(LONG *)(ULONG_PTR)(cam + CK_CAM_BOTTOM_OFF);
    fn = (CkXformRectFn)(ULONG_PTR)CK_XFORM_RECT_VA;
    memset(out, 0, sizeof(out));
    fn((void *)(ULONG_PTR)cam, out, left, top, right, bottom);
    if (oL)
        *oL = out[0];
    if (oT)
        *oT = out[1];
    if (oR)
        *oR = out[2];
    if (oB)
        *oB = out[3];
    return (out[3] > out[1]) ? 1 : 0;
}

static DWORD g_beacon_path_id;
static LPDIRECTSOUNDBUFFER g_beacon_L;
static LPDIRECTSOUNDBUFFER g_beacon_R;
static LONG g_beacon_last_game_pan;
static LONG g_beacon_last_applied;
static LONG g_beacon_last_yatten;
static int g_beacon_enabled = -1;
static volatile LONG g_beacon_sx;
static volatile LONG g_beacon_sy;
static volatile int g_beacon_sx_valid;
static volatile int g_beacon_y_valid;
static volatile LONG g_beacon_ref_span;
static int beacon_want(void)
{
    const char *e;
    if (g_beacon_enabled < 0) {
        e = getenv("CK_AUDIO_BEACON");
        /* Opt-in only: looping Hastatus test. Unit voices use one-shot spatial. */
        g_beacon_enabled = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
    }
    return g_beacon_enabled;
}

/* X → stereo L/R; Y → loudness (top=farther/quieter, bottom=closer/louder). */
static LONG mouse_spatial_soft(int *out_x, int *out_y, int *out_sw, int *out_sh, LONG *out_yatten)
{
    POINT pt;
    HWND hwnd;
    int sw = 0, sh = 0;
    LONG pan, yatten;

    if (out_x)
        *out_x = -1;
    if (out_y)
        *out_y = -1;
    if (out_sw)
        *out_sw = 0;
    if (out_sh)
        *out_sh = 0;
    if (out_yatten)
        *out_yatten = 0;
    if (!vk_present_soft_size(&sw, &sh) || sw <= 1 || sh <= 1)
        return 0;
    hwnd = vk_present_hwnd();
    if (!GetCursorPos(&pt))
        return 0;
    if (hwnd)
        ScreenToClient(hwnd, &pt);
    if (pt.x < 0)
        pt.x = 0;
    if (pt.x >= sw)
        pt.x = sw - 1;
    if (pt.y < 0)
        pt.y = 0;
    if (pt.y >= sh)
        pt.y = sh - 1;
    if (out_x)
        *out_x = (int)pt.x;
    if (out_y)
        *out_y = (int)pt.y;
    if (out_sw)
        *out_sw = sw;
    if (out_sh)
        *out_sh = sh;
    pan = (LONG)(((LONGLONG)pt.x * 20000LL) / (LONGLONG)sw - 10000LL);
    /* Client y=0 is top → farther/quieter. Keep mild: -1800..0 so mid-screen stays audible (H-G). */
    yatten = -(LONG)(((LONGLONG)(sh - 1 - pt.y) * 1800LL) / (LONGLONG)(sh - 1));
    if (yatten < -1800)
        yatten = -1800;
    if (yatten > 0)
        yatten = 0;
    if (out_yatten)
        *out_yatten = yatten;
    return path_clamp_pan(pan);
}

static LONG pan_from_mouse_soft(int *out_x, int *out_sw)
{
    int my = 0, sh = 0;
    LONG yatten = 0;
    return mouse_spatial_soft(out_x, &my, out_sw, &sh, &yatten);
}

static HANDLE g_beacon_timer;
static volatile LONG g_beacon_timer_run;
static volatile LONG g_beacon_tick_n;
static volatile LONG g_beacon_apply_n;

static void beacon_timer_stop(void)
{
    InterlockedExchange(&g_beacon_timer_run, 0);
    if (g_beacon_timer) {
        WaitForSingleObject(g_beacon_timer, 1000);
        CloseHandle(g_beacon_timer);
        g_beacon_timer = NULL;
    }
}

void beacon_stop(void)
{
    beacon_timer_stop();
    if (g_beacon_L) {
        IDirectSoundBuffer_Stop(g_beacon_L);
        IDirectSoundBuffer_Release(g_beacon_L);
        g_beacon_L = NULL;
    }
    if (g_beacon_R) {
        IDirectSoundBuffer_Stop(g_beacon_R);
        IDirectSoundBuffer_Release(g_beacon_R);
        g_beacon_R = NULL;
    }
    g_beacon_path_id = 0;
    g_beacon_sx_valid = 0;
    g_beacon_y_valid = 0;
    g_beacon_ref_span = 0;
}

static int beacon_mode_mouse(void)
{
    const char *e = getenv("CK_BEACON_MODE");
    /* Default: unit (game SetPan). CK_BEACON_MODE=mouse for cursor test. */
    return (e && (e[0] == 'm' || e[0] == 'M')) ? 1 : 0;
}

/*
 * Live frustum: world sx vs raw L/R; world sy vs LIVE transformed view T/B (0x47E4A0).
 * H-P: raw top/bottom ≠ sy space. H-Q: delta-tracking vt/vb drifted on orbit — xform each tick.
 */
static LONG unit_pan_from_camera(LONG sx, LONG sy, LONG ref_span, LONG *oleft, LONG *oright,
                                 LONG *otop, LONG *obottom, LONG *opre, LONG *oatten, int *ok,
                                 LONG *ospan, LONG *odatten, LONG *olateral_q, LONG *oyatten,
                                 LONG *ovt, LONG *ovb)
{
    DWORD cam;
    LONG left, right, top, bottom, span, mid, pre, ds, atten, yatten;
    LONG vt = 0, vb = 0, vl = 0, vr = 0, vyspan;
    double dx, dist, half, lateral, zoom, t;
    int xok;

    if (ok)
        *ok = 0;
    if (ospan)
        *ospan = 0;
    if (odatten)
        *odatten = 0;
    if (olateral_q)
        *olateral_q = 0;
    if (oyatten)
        *oyatten = 0;
    if (ovt)
        *ovt = 0;
    if (ovb)
        *ovb = 0;
    if (IsBadReadPtr((void *)(ULONG_PTR)CK_CAM_PTR_VA, 4))
        return 0;
    cam = *(DWORD *)(ULONG_PTR)CK_CAM_PTR_VA;
    if (!cam || IsBadReadPtr((void *)(ULONG_PTR)(cam + CK_CAM_BOTTOM_OFF), 4))
        return 0;
    left = *(LONG *)(ULONG_PTR)(cam + CK_CAM_LEFT_OFF);
    top = *(LONG *)(ULONG_PTR)(cam + CK_CAM_TOP_OFF);
    right = *(LONG *)(ULONG_PTR)(cam + CK_CAM_RIGHT_OFF);
    bottom = *(LONG *)(ULONG_PTR)(cam + CK_CAM_BOTTOM_OFF);
    span = right - left;
    if (span <= 0)
        return 0;
    mid = left + span / 2;
    pre = (LONG)(((LONGLONG)(mid - sx) * 20000LL) / (LONGLONG)span);
    if (pre < -10000)
        pre = -10000;
    if (pre > 10000)
        pre = 10000;
    ds = -pre;

    dx = (double)sx - (double)mid;
    dist = fabs(dx);
    half = (double)span * 0.5;
    if (half < 1.0)
        half = 1.0;
    lateral = dist / half;
    if (ref_span <= 0)
        ref_span = span;
    zoom = (double)span / (double)ref_span;
    t = (zoom - 1.0) * 1.1 + lateral * 0.75;
    if (t < 0.0)
        t = 0.0;
    if (t > 2.0)
        t = 2.0;
    atten = (LONG)(-(double)CK_DATTEN_MAX * (t / 2.0) * (t / 2.0));
    if (atten < -CK_DATTEN_MAX)
        atten = -CK_DATTEN_MAX;
    if (atten > 0)
        atten = 0;

    /* Vertical: live xform AABB vs sy (same space as on-screen test). */
    yatten = 0;
    xok = cam_frustum_xform(&vl, &vt, &vr, &vb);
    if (!xok && g_last_onscreen_vb > g_last_onscreen_vt) {
        /* Fallback: last hook bounds if thiscall fails on worker tick. */
        vt = g_last_onscreen_vt;
        vb = g_last_onscreen_vb;
        xok = 1;
    }
    if (xok) {
        vyspan = vb - vt;
        if (vyspan > 0) {
            /*
             * vyspan~1336: mapping fade to the on-screen AABB mutes after a tiny
             * camera nudge. Keep full volume while the unit is still in view (vert<=1),
             * then fade over extra screen-heights off-screen (not pan-law on both ears).
             */
            {
                double midy = 0.5 * (double)(vt + vb);
                double yhalf = 0.5 * (double)vyspan;
                double vert, off;
                if (yhalf < 1.0)
                    yhalf = 1.0;
                vert = fabs((double)sy - midy) / yhalf;
                if (vert <= 1.0) {
                    off = 0.0;
                    yatten = 0;
                } else {
                    off = vert - 1.0;
                    if (off > 4.0)
                        off = 4.0;
                    /* 1 screen off ~−400; 2 ~−1600; 3 ~−3600; cap −3500 */
                    yatten = -(LONG)(400.0 * off * off + 0.5);
                    if (yatten < -3500)
                        yatten = -3500;
                    if (yatten > 0)
                        yatten = 0;
                }
            }
        }
    }

    if (oleft)
        *oleft = left;
    if (oright)
        *oright = right;
    if (otop)
        *otop = top;
    if (obottom)
        *obottom = bottom;
    if (opre)
        *opre = pre;
    if (oatten)
        *oatten = atten + yatten;
    if (ospan)
        *ospan = span;
    if (odatten)
        *odatten = atten;
    if (olateral_q)
        *olateral_q = (LONG)(lateral * 1000.0);
    if (oyatten)
        *oyatten = yatten;
    if (ovt)
        *ovt = vt;
    if (ovb)
        *ovb = vb;
    if (ok)
        *ok = 1;
    return path_clamp_pan(ds);
}

static void beacon_apply_spatial(int force_log)
{
    LONG mpan, yatten = 0;
    int mx = 0, my = 0, sw = 0, sh = 0, lg = 0, rg = 0;
    LONG vl, vr;
    LONG uleft = 0, uright = 0, utop = 0, ubottom = 0, upre = 0;
    LONG uspan = 0, udatten = 0, ulat_q = 0, uyatten = 0, uvt = 0, uvb = 0;
    int uok = 0;
    int mouse = beacon_mode_mouse();

    if (!g_beacon_L || !g_beacon_R || !beacon_want())
        return;
    if (mouse) {
        mpan = mouse_spatial_soft(&mx, &my, &sw, &sh, &yatten);
    } else {
        (void)vk_present_soft_size(&sw, &sh);
        mx = -1;
        my = -1;
        if (g_beacon_sx_valid) {
            mpan = unit_pan_from_camera(g_beacon_sx, g_beacon_sy, g_beacon_ref_span, &uleft, &uright,
                                        &utop, &ubottom, &upre, &yatten, &uok, &uspan, &udatten,
                                        &ulat_q, &uyatten, &uvt, &uvb);
            if (uok) {
                g_beacon_last_game_pan = mpan;
                if (g_beacon_ref_span <= 0 && uspan > 0)
                    g_beacon_ref_span = uspan;
            } else {
                mpan = path_clamp_pan(g_beacon_last_game_pan);
                yatten = 0;
            }
        } else {
            mpan = path_clamp_pan(g_beacon_last_game_pan);
            yatten = 0;
        }
    }
    if (!force_log && mpan == g_beacon_last_applied && yatten == g_beacon_last_yatten)
        return;
    pan_gains_q15(mpan, &lg, &rg);
    vl = path_clamp_vol(q15_to_dsvol(lg) + yatten);
    vr = path_clamp_vol(q15_to_dsvol(rg) + yatten);
    IDirectSoundBuffer_SetVolume(g_beacon_L, vl);
    IDirectSoundBuffer_SetVolume(g_beacon_R, vr);
    g_beacon_last_applied = mpan;
    g_beacon_last_yatten = yatten;
    InterlockedIncrement(&g_beacon_apply_n);
}

static void spatial_tick_live(void);
static void music_tick(void);

void dm_replace_beacon_tick(void)
{
    CkPerf *perf;
    InterlockedIncrement(&g_beacon_tick_n);
    if (g_beacon_L && g_beacon_R)
        beacon_apply_spatial(0);
    spatial_tick_live();
    live_cs_enter();
    perf = g_active_perf;
    if (perf)
        ck_perf_addref(perf);
    live_cs_leave();
    if (perf) {
        notif_tick(perf);
        ck_perf_release(perf);
    }
    music_tick();
}

static DWORD WINAPI beacon_timer_proc(void *arg)
{
    HMODULE module = (HMODULE)arg;
    while (InterlockedCompareExchange(&g_beacon_timer_run, 1, 1) == 1) {
        dm_replace_beacon_tick();
        Sleep(16);
    }
    dm_worker_exit(module, 0);
    return 0;
}

static void beacon_timer_start(void)
{
    HMODULE module;
    if (g_beacon_timer)
        return;
    module = dm_pin_module((const void *)beacon_timer_proc);
    if (!module)
        return;
    InterlockedExchange(&g_beacon_timer_run, 1);
    g_beacon_timer = CreateThread(NULL, 0, beacon_timer_proc, module, 0, NULL);
    if (!g_beacon_timer) {
        InterlockedExchange(&g_beacon_timer_run, 0);
        FreeLibrary(module);
    }
}

static LPDIRECTSOUNDBUFFER beacon_make_half(LPDIRECTSOUND ds, const WAVEFORMATEX *wfx, BYTE *pcm,
                                            DWORD bytes, LONG base_vol, int loop)
{
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS |
                   DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_STATIC;
    desc.dwBufferBytes = bytes;
    desc.lpwfxFormat = (WAVEFORMATEX *)wfx;
    hr = IDirectSound_CreateSoundBuffer(ds, &desc, &buf, NULL);
    if (FAILED(hr) || !buf || !ds_buf_write_all(buf, pcm, bytes)) {
        if (buf)
            IDirectSoundBuffer_Release(buf);
        return NULL;
    }
    path_apply_buf_vol(buf, base_vol);
    if (FAILED(IDirectSoundBuffer_Play(buf, 0, 0, loop ? DSBPLAY_LOOPING : 0))) {
        IDirectSoundBuffer_Release(buf);
        return NULL;
    }
    return buf;
}

void beacon_start_from_seg(CkPerf *perf, CkSegment *seg, CkPath *apath)
{
    WAVEFORMATEX wfx;
    BYTE *left = NULL, *right = NULL;
    DWORD stereo_bytes;
    LONG base_vol;

    if (!beacon_want() || !perf || !perf->ds || !seg || !seg->pcm || !seg->pcm_bytes)
        return;
    if (seg->fmt.wBitsPerSample != 16)
        return;

    beacon_stop();
    stereo_wfx_from(&seg->fmt, &wfx);
    if (seg->fmt.nChannels == 1)
        stereo_bytes = (seg->pcm_bytes / 2u) * 4u;
    else
        stereo_bytes = seg->pcm_bytes;

    left = (BYTE *)malloc(stereo_bytes);
    right = (BYTE *)malloc(stereo_bytes);
    if (!left || !right) {
        free(left);
        free(right);
        return;
    }
    if (pcm_render_one_channel(seg->pcm, &seg->fmt, seg->pcm_bytes, left, stereo_bytes, 0) !=
            stereo_bytes ||
        pcm_render_one_channel(seg->pcm, &seg->fmt, seg->pcm_bytes, right, stereo_bytes, 1) !=
            stereo_bytes) {
        free(left);
        free(right);
        return;
    }

    base_vol = apath ? InterlockedCompareExchange(&apath->vol, 0, 0) : 0;
    g_beacon_L = beacon_make_half(perf->ds, &wfx, left, stereo_bytes, base_vol, 1);
    g_beacon_R = beacon_make_half(perf->ds, &wfx, right, stereo_bytes, base_vol, 1);
    free(left);
    free(right);
    if (!g_beacon_L || !g_beacon_R) {
        beacon_stop();
        return;
    }

    g_beacon_path_id = apath ? apath->id : 0;
    g_beacon_last_game_pan =
        apath ? InterlockedCompareExchange(&apath->pan, 0, 0) : 0;
    g_beacon_last_applied = 0x7fffffff;
    g_beacon_last_yatten = 0x7fffffff;
    g_beacon_tick_n = 0;
    g_beacon_apply_n = 0;
    /* Lock world X/Y (SetPan only once — H-J). Live vt/vb via 0x47E4A0 each tick (H-Q). */
    if (g_last_onscreen_ms != 0) {
        g_beacon_sx = g_last_onscreen_sx;
        g_beacon_sy = g_last_onscreen_sy;
        g_beacon_sx_valid = 1;
        g_beacon_y_valid = 1;
    } else {
        g_beacon_sx_valid = 0;
        g_beacon_y_valid = 0;
    }
    g_beacon_ref_span = 0;
    beacon_apply_spatial(1);
    beacon_timer_start();
    log_msg("dm-replace: RHASTATUS beacon unit-cam path=%lu pan=%ld sx=%ld sy=%ld sxv=%d",
            (unsigned long)g_beacon_path_id, (long)g_beacon_last_applied, (long)g_beacon_sx,
            (long)g_beacon_sy, g_beacon_sx_valid);
}

void beacon_update_pan(LONG game_pan, DWORD path_id)
{
    if (g_beacon_L && g_beacon_path_id && path_id != g_beacon_path_id)
        return;
    g_beacon_last_game_pan = game_pan;
    beacon_apply_spatial(0);
}

void live_cs_enter(void)
{
    if (g_live_cs_ok)
        EnterCriticalSection(&g_live_cs);
}

void live_cs_leave(void)
{
    if (g_live_cs_ok)
        LeaveCriticalSection(&g_live_cs);
}

void live_free_slot(int i)
{
    LPDIRECTSOUNDBUFFER b, bR;
    CkSegment *seg;

    live_cs_enter();
    if (i < 0 || i >= s_live_n) {
        live_cs_leave();
        return;
    }
    b = s_live[i].b;
    bR = s_live[i].bR;
    seg = s_live[i].seg;
    s_live[i].b = NULL;
    s_live[i].bR = NULL;
    s_live[i].spatial = 0;
    s_live[i] = s_live[s_live_n - 1];
    memset(&s_live[s_live_n - 1], 0, sizeof(s_live[0]));
    s_live_n--;
    live_cs_leave();
    if (b) {
        IDirectSoundBuffer_Stop(b);
        IDirectSoundBuffer_Release(b);
    }
    if (bR) {
        IDirectSoundBuffer_Stop(bR);
        IDirectSoundBuffer_Release(bR);
    }
    if (seg)
        ck_segment_release(seg);
}

void spatial_apply_live(CkLiveBuf *lb, int force)
{
    LONG mpan, yatten = 0;
    LONG uleft = 0, uright = 0, utop = 0, ubottom = 0, upre = 0;
    LONG uspan = 0, udatten = 0, ulat_q = 0, uyatten = 0, uvt = 0, uvb = 0;
    int uok = 0, lg = 0, rg = 0;
    LONG vl, vr;
    LPDIRECTSOUNDBUFFER b, bR;

    if (!lb || !lb->spatial)
        return;
    b = lb->b;
    bR = lb->bR;
    if (!b || !bR)
        return;
    /* Ambient often has no world pan (SetPan=0). sx=0 is map origin — mute/hard-left (764ba7). */
    if (!lb->xy_ok) {
        mpan = 0;
        yatten = 0;
        uok = 1;
    } else {
        mpan = unit_pan_from_camera(lb->sx, lb->sy, lb->ref_span, &uleft, &uright, &utop, &ubottom,
                                    &upre, &yatten, &uok, &uspan, &udatten, &ulat_q, &uyatten, &uvt,
                                    &uvb);
        if (!lb->y_ok)
            yatten = 0;
        if (!uok) {
            mpan = lb->last_pan;
            yatten = lb->y_ok ? lb->last_yatten : 0;
            if (mpan == 0x7fffffff)
                mpan = 0;
            if (yatten == 0x7fffffff)
                yatten = 0;
        } else {
            if (lb->ref_span <= 0 && uspan > 0)
                lb->ref_span = uspan;
        }
    }
    if (!force && mpan == lb->last_pan && yatten == lb->last_yatten)
        return;
    pan_gains_q15(mpan, &lg, &rg);
    vl = path_clamp_vol(q15_to_dsvol(lg) + yatten + lb->base_vol);
    vr = path_clamp_vol(q15_to_dsvol(rg) + yatten + lb->base_vol);
    b = lb->b;
    bR = lb->bR;
    if (!b || !bR)
        return;
    IDirectSoundBuffer_SetVolume(b, vl);
    IDirectSoundBuffer_SetVolume(bR, vr);
    lb->last_pan = mpan;
    lb->last_yatten = yatten;
}

static void spatial_tick_live(void)
{
    int i;
    DWORD now = GetTickCount();
    live_cs_enter();
    for (i = 0; i < s_live_n;) {
        if ((LONG)(now - s_live[i].done_tick) >= 0) {
            live_free_slot(i);
            continue;
        }
        if (s_live[i].spatial)
            spatial_apply_live(&s_live[i], 0);
        i++;
    }
    live_cs_leave();
}

/* Dual L/R + camera spatial. loop=1 for long ambient beds; voices/buildings stay one-shot. */
int spatial_voice_play(CkPerf *perf, CkSegment *seg, CkPath *apath, int loop)
{
    WAVEFORMATEX wfx;
    BYTE *left = NULL, *right = NULL;
    DWORD stereo_bytes, dur_ms, play_bytes;
    LONG base_vol;
    LPDIRECTSOUNDBUFFER bL = NULL, bR = NULL;
    CkLiveBuf *lb;

    if (!perf || !perf->ds || !seg || !seg->pcm || !seg->pcm_bytes)
        return 0;
    if (seg->fmt.wBitsPerSample != 16)
        return 0;
    stereo_wfx_from(&seg->fmt, &wfx);
    if (seg->fmt.nChannels == 1)
        stereo_bytes = (seg->pcm_bytes / 2u) * 4u;
    else
        stereo_bytes = seg->pcm_bytes;
    left = (BYTE *)malloc(stereo_bytes);
    right = (BYTE *)malloc(stereo_bytes);
    if (!left || !right) {
        free(left);
        free(right);
        return 0;
    }
    if (pcm_render_one_channel(seg->pcm, &seg->fmt, seg->pcm_bytes, left, stereo_bytes, 0) !=
            stereo_bytes ||
        pcm_render_one_channel(seg->pcm, &seg->fmt, seg->pcm_bytes, right, stereo_bytes, 1) !=
            stereo_bytes) {
        free(left);
        free(right);
        return 0;
    }

    base_vol = apath ? InterlockedCompareExchange(&apath->vol, 0, 0) : 0;
    bL = beacon_make_half(perf->ds, &wfx, left, stereo_bytes, base_vol, loop);
    bR = beacon_make_half(perf->ds, &wfx, right, stereo_bytes, base_vol, loop);
    free(left);
    free(right);
    if (!bL || !bR) {
        if (bL)
            IDirectSoundBuffer_Release(bL);
        if (bR)
            IDirectSoundBuffer_Release(bR);
        return 0;
    }

    play_bytes = seg->pcm_bytes;
    dur_ms = seg->fmt.nAvgBytesPerSec
                 ? (DWORD)((ULONGLONG)play_bytes * 1000ull / seg->fmt.nAvgBytesPerSec)
                 : 1000u;
    if (dur_ms < 50u)
        dur_ms = 50u;
    dur_ms += 400u;

    live_cs_enter();
    if (s_live_n >= (int)(sizeof(s_live) / sizeof(s_live[0])))
        live_free_slot(0);
    lb = &s_live[s_live_n];
    memset(lb, 0, sizeof(*lb));
    lb->b = bL;
    lb->bR = bR;
    lb->done_tick = loop ? (GetTickCount() + 3600000u) : (GetTickCount() + dur_ms);
    lb->path_id = apath ? apath->id : 0;
    lb->seg = seg;
    ck_segment_addref(seg);
    lb->spatial = 1;
    lb->looping = loop ? 1 : 0;
    lb->base_vol = base_vol;
    lb->y_ok = 0;
    lb->xy_ok = 0;
    lb->last_pan = 0x7fffffff;
    lb->last_yatten = 0x7fffffff;
    /*
     * Ambient: game SetPan(path)=0 (764ba7). Do not glue to last unit/building or
     * world origin (0,0) — that hard-pans / mutes. Center until we have emitter XY.
     */
    if (!path_is_ambient_sfx(seg->path) && g_last_onscreen_ms != 0) {
        lb->sx = g_last_onscreen_sx;
        lb->xy_ok = 1;
        if (g_last_onscreen_sy_ok) {
            lb->sy = g_last_onscreen_sy;
            lb->y_ok = 1;
        } else {
            lb->sy = 0;
        }
    } else {
        lb->sx = 0;
        lb->sy = 0;
    }
    lb->ref_span = 0;
    s_live_n++;
    spatial_apply_live(lb, 1);
    live_cs_leave();
    return 1;
}

/*
 * Game 0x6A9F18: on-screen sources get pan=0. Replace with frustum-X pan.
 * esi=world X, ecx=camera, eax=&pan. World Y is arg [esp+0x2C] (not edi).
 */
static void __attribute__((used, noinline)) ck_fill_onscreen_pan(LONG *out, LONG sx, LONG left,
                                                                  LONG right, LONG sy_arg,
                                                                  LONG edi_sy)
{
    LONG span, mid, pan;
    int y_ok;

    if (!out || IsBadWritePtr(out, sizeof(LONG)))
        return;
    span = right - left;
    if (span <= 0) {
        *out = 0;
        return;
    }
    mid = left + span / 2;
    pan = (LONG)(((LONGLONG)(mid - sx) * 20000LL) / (LONGLONG)span);
    if (pan < -10000)
        pan = -10000;
    if (pan > 10000)
        pan = 10000;
    *out = pan;
    /*
     * World Y is arg1 at [esp+0x2C] in the pan fn (0x6A9DD0). edi at 0x6A9F18 is
     * often already the Y-distance residue (sy==vb / negative) — do not use it.
     */
    y_ok = (sy_arg > 500 && sy_arg < 50000);
    g_last_onscreen_sx = sx;
    g_last_onscreen_ms = GetTickCount();
    if (y_ok) {
        g_last_onscreen_sy = sy_arg;
        g_last_onscreen_sy_ok = 1;
        g_beacon_sy = sy_arg;
        g_beacon_y_valid = 1;
    }
    g_beacon_sx = sx;
    g_beacon_sx_valid = 1;
    (void)edi_sy;
}

void install_onscreen_pan_fix(void)
{
    BYTE *site = (BYTE *)(ULONG_PTR)0x006A9F18u;
    static BYTE *cave;
    BYTE *p;
    DWORD oldprot;
    INT32 rel;
    INT32 call_rel;

    if (IsBadReadPtr(site, 6) || site[0] != 0xC7 || site[1] != 0x00) {
        log_msg("dm-replace: onscreen pan site mismatch (skip)");
        return;
    }
    cave = (BYTE *)VirtualAlloc(NULL, 160, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave)
        return;
    /*
     * eax=&pan, esi=sx, ecx=camera. World Y is pan-fn arg1 [esp+0x2C], NOT edi
     * (edi clobbered by Y-distance before 0x6A9F18). cdecl:
     * ck_fill(out, sx, left, right, sy_arg, edi_sy)
     */
    p = cave;
    *p++ = 0x51; /* push ecx */
    *p++ = 0x52; /* push edx */
    *p++ = 0x53; /* push ebx */
    *p++ = 0x50; /* push eax — &pan */
    *p++ = 0x57; /* push edi — edi_sy (log vs stack Y) */
    *p++ = 0xFF;
    *p++ = 0x74;
    *p++ = 0x24;
    *p++ = 0x44; /* push dword [esp+0x44] — world Y arg */
    *p++ = 0xFF;
    *p++ = 0xB1;
    {
        DWORD off = 0x4d0;
        memcpy(p, &off, 4);
        p += 4;
    }
    *p++ = 0xFF;
    *p++ = 0xB1;
    {
        DWORD off = 0x4c8;
        memcpy(p, &off, 4);
        p += 4;
    }
    *p++ = 0x56; /* push esi — sx */
    *p++ = 0xFF;
    *p++ = 0x74;
    *p++ = 0x24;
    *p++ = 0x14; /* push dword [esp+0x14] — saved &pan */
    *p++ = 0xE8;
    call_rel = (INT32)((BYTE *)ck_fill_onscreen_pan - (p + 4));
    memcpy(p, &call_rel, 4);
    p += 4;
    *p++ = 0x83;
    *p++ = 0xC4;
    *p++ = 0x18; /* add esp, 24 — 6 args */
    *p++ = 0x58; /* pop eax */
    *p++ = 0x5B; /* pop ebx */
    *p++ = 0x5A; /* pop edx */
    *p++ = 0x59; /* pop ecx */
    *p++ = 0xC3;

    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    site[0] = 0xE8;
    rel = (INT32)(cave - (site + 5));
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    VirtualProtect(site, 6, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    log_msg("dm-replace: onscreen pan fix @ 0x6A9F18 (frustum-X + view-Y, eax preserved)");
}

void path_propagate_volume(CkPath *path)
{
    int i;
    LONG vol;

    if (!path)
        return;
    vol = path_clamp_vol(InterlockedCompareExchange(&path->vol, 0, 0));
    if (path->ctrl)
        IDirectSoundBuffer_SetVolume(path->ctrl, vol);
    {
        CkPerf *perf;
        live_cs_enter();
        perf = g_active_perf;
        if (perf)
            ck_perf_addref(perf);
        live_cs_leave();
        if (perf) {
        EnterCriticalSection(&perf->lock);
        if (path->id == perf->music_path_id) {
            if (perf->music_wasapi)
                audio_wasapi_set_volume(vol);
            else if (perf->music_buf)
                IDirectSoundBuffer_SetVolume(perf->music_buf, vol);
        }
        LeaveCriticalSection(&perf->lock);
            ck_perf_release(perf);
        }
    }
    live_cs_enter();
    for (i = 0; i < s_live_n; i++) {
        if (s_live[i].path_id != path->id)
            continue;
        s_live[i].base_vol = vol;
        if (s_live[i].spatial) {
            spatial_apply_live(&s_live[i], 1);
            continue;
        }
        if (s_live[i].b)
            IDirectSoundBuffer_SetVolume(s_live[i].b, vol);
    }
    live_cs_leave();
}

/* Game 0x40bbc0: GetObjectInPath(PATH_BUFFER) → IDirectSoundBuffer::SetPan — must hit SFX. */
void live_apply_softpan(CkLiveBuf *lb, LONG pan)
{
    CkSegment *seg;
    BYTE *stereo = NULL;
    DWORD stereo_bytes, n, play_pos = 0, write_pos = 0;
    DWORD now;
    int still_playing, looping;

    if (!lb || !lb->b || !lb->seg || !lb->seg->pcm)
        return;
    pan = path_clamp_pan(pan);
    /* Skip no-op rewrites (Stop/Lock/Play is audible on Wine). */
    if (lb->last_pan != 0x7fffffff && lb->last_pan == pan)
        return;

    seg = lb->seg;
    if (seg->fmt.wBitsPerSample != 16) {
        IDirectSoundBuffer_SetPan(lb->b, pan);
        lb->last_pan = pan;
        return;
    }
    if (seg->fmt.nChannels == 1)
        stereo_bytes = (seg->pcm_bytes / 2u) * 4u;
    else
        stereo_bytes = seg->pcm_bytes;
    stereo = (BYTE *)malloc(stereo_bytes);
    if (!stereo)
        return;
    n = pcm_render_panned_stereo(seg->pcm, &seg->fmt, seg->pcm_bytes, stereo, stereo_bytes, pan);
    if (n != stereo_bytes) {
        free(stereo);
        return;
    }

    /*
     * H36: Wine GetStatus lies on STATIC buffers — do not trust PLAYING/LOOPING.
     * Use done_tick + looping flag set at play time.
     */
    now = GetTickCount();
    still_playing = (LONG)(now - lb->done_tick) < 0;
    looping = lb->looping != 0;
    IDirectSoundBuffer_GetCurrentPosition(lb->b, &play_pos, &write_pos);
    IDirectSoundBuffer_Stop(lb->b);
    if (ds_buf_write_all(lb->b, stereo, n)) {
        if (n)
            IDirectSoundBuffer_SetCurrentPosition(lb->b, play_pos % n);
        path_apply_buf_vol(lb->b, lb->base_vol);
        if (still_playing)
            IDirectSoundBuffer_Play(lb->b, 0, 0, looping ? DSBPLAY_LOOPING : 0);
        lb->last_pan = pan;
    }
    free(stereo);
}

void path_propagate_pan(CkPath *path)
{
    int i;
    LONG pan;

    if (!path)
        return;
    pan = path_clamp_pan(InterlockedCompareExchange(&path->pan, 0, 0));
    if (path->ctrl)
        IDirectSoundBuffer_SetPan(path->ctrl, pan);
    live_cs_enter();
    for (i = 0; i < s_live_n; i++) {
        if (s_live[i].path_id != path->id)
            continue;
        if (s_live[i].spatial) {
            /* Camera frustum owns L/R; refresh from frozen sx/sy. */
            spatial_apply_live(&s_live[i], 0);
            continue;
        }
        live_apply_softpan(&s_live[i], pan);
    }
    live_cs_leave();
}

/* -------- in-game BGM playlist (CVXMusicPlayer::PlayRandomMusic parity) -------- */
enum { MUSIC_POOL_MAX = 64 };

static char g_music_last[200];
static DWORD g_music_done_tick;
static int g_music_looping;
static int g_music_auto; /* non-menu one-shot → advance randomly */
static LONG g_music_vol;
static int g_music_rand_seeded;

static int music_fill_pool(char names[][80], int maxn);

static int music_path_is_menu(const char *path)
{
    char t[260];
    size_t i;
    if (!path || !path[0])
        return 0;
    for (i = 0; path[i] && i + 1 < sizeof(t); ++i) {
        char c = path[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "_MENU") != NULL;
}

static void music_note_started(const char *path, int loop, DWORD dur_ms, LONG vol, int enable_auto)
{
    g_music_vol = vol;
    g_music_looping = loop ? 1 : 0;
    if (path && path[0]) {
        strncpy(g_music_last, path, sizeof(g_music_last) - 1);
        g_music_last[sizeof(g_music_last) - 1] = '\0';
    } else {
        g_music_last[0] = '\0';
    }
    /*
     * Mission BGM advances via SEGEND → game GetObject/PlaySegmentEx.
     * Do not also auto-advance in music_tick (duplicate decode froze the render thread).
     */
    g_music_auto = enable_auto && (!loop && path && path[0] && !music_path_is_menu(path)) ? 1 : 0;
    if (g_music_auto)
        g_music_done_tick = GetTickCount() + (dur_ms > 50u ? dur_ms : 50u);
    else
        g_music_done_tick = 0;
}

void music_prefetch_siblings(const char *current_rel)
{
    char pool[MUSIC_POOL_MAX][80];
    char rel[260];
    int n, i, start;

    n = music_fill_pool(pool, MUSIC_POOL_MAX);
    if (n < 1)
        return;
    if (!g_music_rand_seeded) {
        srand((unsigned)(GetTickCount() ^ GetCurrentThreadId()));
        g_music_rand_seeded = 1;
    }
    start = n > 1 ? (rand() % n) : 0;
    for (i = 0; i < n; ++i) {
        const char *fname = pool[(start + i) % n];
        snprintf(rel, sizeof(rel), "music\\%s", fname);
        if (current_rel && current_rel[0]) {
            char tcur[260], ttry[260];
            size_t j;
            for (j = 0; current_rel[j] && j + 1 < sizeof(tcur); ++j) {
                char c = current_rel[j];
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                if (c == '/')
                    c = '\\';
                tcur[j] = c;
            }
            tcur[j] = '\0';
            for (j = 0; rel[j] && j + 1 < sizeof(ttry); ++j) {
                char c = rel[j];
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                if (c == '/')
                    c = '\\';
                ttry[j] = c;
            }
            ttry[j] = '\0';
            if (strcmp(tcur, ttry) == 0)
                continue;
        }
        music_prefetch_full_async(rel);
    }
}

static void music_clear_state(void)
{
    g_music_auto = 0;
    g_music_looping = 0;
    g_music_done_tick = 0;
}

static int music_fill_pool(char names[][80], int maxn)
{
    const char *root;
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    root = dm_game_dir();
    if (!root || !root[0] || maxn < 1)
        return 0;
    snprintf(pat, sizeof(pat), "%smusic\\*.ogg", root);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(pat, sizeof(pat), "%smusic/*.ogg", root);
        h = FindFirstFileA(pat, &fd);
    }
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char *name = fd.cFileName;
        if (!name || !name[0] || name[0] == '_')
            continue; /* original skips _menu.ogg etc. */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        strncpy(names[n], name, 79);
        names[n][79] = '\0';
        n++;
        if (n >= maxn)
            break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int music_name_matches_last(const char *fname)
{
    char tlast[200], tf[100];
    size_t i;
    const char *base;
    if (!fname || !g_music_last[0])
        return 0;
    for (i = 0; g_music_last[i] && i + 1 < sizeof(tlast); ++i) {
        char c = g_music_last[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        tlast[i] = c;
    }
    tlast[i] = '\0';
    for (i = 0; fname[i] && i + 1 < sizeof(tf); ++i) {
        char c = fname[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        tf[i] = c;
    }
    tf[i] = '\0';
    base = strrchr(tlast, '\\');
    base = base ? base + 1 : tlast;
    return strcmp(base, tf) == 0;
}

static int music_play_rel(CkPerf *perf, const char *fname)
{
    char rel[260];
    BYTE *raw = NULL, *pcm = NULL;
    DWORD raw_len = 0, pcm_len = 0, dur_ms;
    WAVEFORMATEX fmt;
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    HRESULT hr;
    int cached = 0;

    if (!perf || !perf->ds || !fname || !fname[0])
        return 0;
    snprintf(rel, sizeof(rel), "music\\%s", fname);
    memset(&fmt, 0, sizeof(fmt));
    if (music_cache_acquire(rel, &fmt, &pcm, &pcm_len)) {
        cached = 1;
    } else {
        LONGLONG t0 = hitch_qpc_now();
        if (FAILED(load_wav_file_or_pak(rel, &raw, &raw_len)) || !raw || !raw_len)
            return 0;
        /* Full track — STATIC DS buffer cannot grow after head+prefetch. */
        if (!decode_to_pcm(raw, raw_len, &fmt, &pcm, &pcm_len, OGG_DECODE_FULL) || !pcm ||
            !pcm_len) {
            free(raw);
            return 0;
        }
        free(raw);
        pcm = music_cache_intern(rel, &fmt, pcm, pcm_len);
        cached = music_cache_acquire(rel, &fmt, &pcm, &pcm_len);
        log_msg("dm-replace: random music decode '%s' ms=%.1f cached=%d pcm=%lu", rel,
                hitch_qpc_ms_since(t0), cached, (unsigned long)pcm_len);
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS |
                   DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_STATIC;
    desc.dwBufferBytes = pcm_len;
    desc.lpwfxFormat = &fmt;
    hr = IDirectSound_CreateSoundBuffer(perf->ds, &desc, &buf, NULL);
    if (SUCCEEDED(hr) && buf) {
        if (!ds_buf_write_all(buf, pcm, pcm_len))
            hr = E_FAIL;
        else {
            path_apply_buf_vol(buf, g_music_vol);
            hr = IDirectSoundBuffer_Play(buf, 0, 0, 0);
        }
    }
    if (cached)
        music_cache_release(rel, pcm);
    /* Shared music-cache buffers must not be freed. */
    if (!cached && pcm)
        free(pcm);
    if (FAILED(hr) || !buf) {
        if (buf)
            IDirectSoundBuffer_Release(buf);
        return 0;
    }
    EnterCriticalSection(&perf->lock);
    if (perf->music_buf) {
        IDirectSoundBuffer_Stop(perf->music_buf);
        IDirectSoundBuffer_Release(perf->music_buf);
    }
    if (perf->music_seg)
        ck_segment_release(perf->music_seg);
    perf->music_buf = buf;
    perf->music_seg = NULL;
    perf->music_path_id = 0;
    LeaveCriticalSection(&perf->lock);
    dur_ms = fmt.nAvgBytesPerSec ? (DWORD)((ULONGLONG)pcm_len * 1000ull / fmt.nAvgBytesPerSec) : 1000u;
    music_note_started(rel, 0, dur_ms, g_music_vol, 1);
    music_prefetch_siblings(rel);
    log_msg("dm-replace: random music '%s' (%lu ms)%s", rel, (unsigned long)dur_ms,
            cached ? " [cache]" : "");
    return 1;
}

static void music_play_random_next(CkPerf *perf)
{
    char pool[MUSIC_POOL_MAX][80];
    int n, pick, tries;
    if (!perf)
        return;
    n = music_fill_pool(pool, MUSIC_POOL_MAX);
    if (n < 1)
        return;
    if (!g_music_rand_seeded) {
        srand((unsigned)(GetTickCount() ^ GetCurrentThreadId()));
        g_music_rand_seeded = 1;
    }
    pick = rand() % n;
    for (tries = 0; tries < n; ++tries) {
        int i = (pick + tries) % n;
        if (n == 1 || !music_name_matches_last(pool[i])) {
            music_play_rel(perf, pool[i]);
            return;
        }
    }
    music_play_rel(perf, pool[pick]);
}

static void music_tick(void)
{
    CkPerf *p;
    int should_advance = 0;
    if (!g_music_auto || g_music_looping)
        return;
    live_cs_enter();
    p = g_active_perf;
    if (p)
        ck_perf_addref(p);
    live_cs_leave();
    if (!p)
        return;
    EnterCriticalSection(&p->lock);
    should_advance = p->music_buf != NULL &&
                     (LONG)(GetTickCount() - g_music_done_tick) >= 0;
    LeaveCriticalSection(&p->lock);
    if (should_advance)
        music_play_random_next(p);
    ck_perf_release(p);
}

/* H59: Unload/movie must tear down looping BGM — music lives outside s_live. */
void stop_music_buf(void)
{
    CkPerf *p;
    LPDIRECTSOUNDBUFFER buf;
    CkSegment *seg;
    music_clear_state();
    live_cs_enter();
    p = g_active_perf;
    if (p)
        ck_perf_addref(p);
    live_cs_leave();
    if (!p)
        return;
    if (p->music_wasapi)
        audio_wasapi_stop();
    EnterCriticalSection(&p->lock);
    buf = p->music_buf;
    seg = p->music_seg;
    p->music_buf = NULL;
    p->music_seg = NULL;
    p->music_path_id = 0;
    LeaveCriticalSection(&p->lock);
    if (buf) {
        IDirectSoundBuffer_Stop(buf);
        IDirectSoundBuffer_Release(buf);
    }
    if (seg)
        ck_segment_release(seg);
    ck_perf_release(p);
}

void dm_replace_silence_music(void)
{
    stop_music_buf();
}

/* H55: Unload / menu teardown — stop DS buffers for one segment. */
void stop_live_for_segment(CkSegment *seg)
{
    int i;
    if (!seg)
        return;
    live_cs_enter();
    for (i = 0; i < s_live_n;) {
        if (s_live[i].seg == seg)
            live_free_slot(i);
        else
            i++;
    }
    live_cs_leave();
}

/* H55/H56: mission→menu or CloseDown — silence all secondary SFX. */
void stop_all_live_sfx(void)
{
    live_cs_enter();
    while (s_live_n > 0)
        live_free_slot(0);
    live_cs_leave();
}

int segment_is_playing(CkSegment *seg)
{
    int i, playing = 0;
    CkPerf *perf;
    if (!seg)
        return 0;
    live_cs_enter();
    perf = g_active_perf;
    if (perf)
        ck_perf_addref(perf);
    live_cs_leave();
    if (perf) {
        EnterCriticalSection(&perf->lock);
        if (perf->music_wasapi)
            playing = (perf->music_seg == seg) && audio_wasapi_is_playing();
        else
            playing = perf->music_buf && perf->music_seg == seg;
        LeaveCriticalSection(&perf->lock);
        ck_perf_release(perf);
    }
    if (playing)
        return 1;
    live_cs_enter();
    for (i = 0; i < s_live_n; ++i) {
        if (s_live[i].seg == seg) {
            playing = 1;
            break;
        }
    }
    live_cs_leave();
    return playing;
}

/* Stop prior SFX on this audiopath — native channel re-triggers (H47). */
void path_stop_live_sfx(DWORD path_id)
{
    int i;
    if (!path_id)
        return;
    live_cs_enter();
    for (i = 0; i < s_live_n;) {
        if (s_live[i].path_id == path_id)
            live_free_slot(i);
        else
            i++;
    }
    live_cs_leave();
}

typedef struct {
    LPDIRECTSOUNDBUFFER buf;
    CkSegment *seg;
    LONG start_vol;
    HMODULE module;
} CkMusicFade;

static DWORD WINAPI music_fade_release_proc(void *arg)
{
    CkMusicFade *fade = (CkMusicFade *)arg;
    int step;
    for (step = 1; step <= 16; ++step) {
        LONG vol = fade->start_vol + (DSBVOLUME_MIN - fade->start_vol) * step / 16;
        IDirectSoundBuffer_SetVolume(fade->buf, path_clamp_vol(vol));
        Sleep(16);
    }
    IDirectSoundBuffer_Stop(fade->buf);
    IDirectSoundBuffer_Release(fade->buf);
    if (fade->seg)
        ck_segment_release(fade->seg);
    {
        HMODULE module = fade->module;
        free(fade);
        dm_worker_exit(module, 0);
    }
    return 0;
}

static void music_release_old(LPDIRECTSOUNDBUFFER buf, CkSegment *seg, int fade, LONG start_vol)
{
    CkMusicFade *ctx;
    HANDLE thread;
    if (!buf) {
        if (seg)
            ck_segment_release(seg);
        return;
    }
    if (!fade) {
        IDirectSoundBuffer_Stop(buf);
        IDirectSoundBuffer_Release(buf);
        if (seg)
            ck_segment_release(seg);
        return;
    }
    ctx = (CkMusicFade *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        IDirectSoundBuffer_Stop(buf);
        IDirectSoundBuffer_Release(buf);
        if (seg)
            ck_segment_release(seg);
        return;
    }
    ctx->buf = buf;
    ctx->seg = seg;
    ctx->start_vol = path_clamp_vol(start_vol);
    ctx->module = dm_pin_module((const void *)music_fade_release_proc);
    if (!ctx->module) {
        IDirectSoundBuffer_Stop(buf);
        IDirectSoundBuffer_Release(buf);
        if (seg)
            ck_segment_release(seg);
        free(ctx);
        return;
    }
    thread = CreateThread(NULL, 0, music_fade_release_proc, ctx, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    IDirectSoundBuffer_Stop(buf);
    IDirectSoundBuffer_Release(buf);
    if (seg)
        ck_segment_release(seg);
    FreeLibrary(ctx->module);
    free(ctx);
}

typedef struct {
    CkPerf *perf;
    CkSegment *seg;
    DWORD flags;
    LONG pvol;
    HMODULE module;
} MusicPlayJob;

static HRESULT play_pcm_music_start(CkPerf *perf, CkSegment *seg, CkPath *apath, DWORD flags,
                                    LONG pvol, LPDIRECTSOUNDBUFFER buf, int loop_play)
{
    LPDIRECTSOUNDBUFFER old_buf;
    CkSegment *old_seg;
    HRESULT hr;
    int fade_transition = (flags & CK_PLAY_FADE_TRANSITION) != 0;

    if (!buf)
        return E_FAIL;
    IDirectSoundBuffer_SetCurrentPosition(buf, 0);
    path_apply_buf_vol(buf, pvol);
    hr = IDirectSoundBuffer_Play(buf, 0, 0, loop_play ? DSBPLAY_LOOPING : 0);
    if (FAILED(hr)) {
        IDirectSoundBuffer_Release(buf);
        return hr;
    }
    EnterCriticalSection(&perf->lock);
    old_buf = perf->music_buf;
    old_seg = perf->music_seg;
    perf->music_buf = buf;
    perf->music_seg = seg;
    ck_segment_addref(seg);
    perf->music_path_id = apath ? apath->id : 0;
    LeaveCriticalSection(&perf->lock);
    music_release_old(old_buf, old_seg, fade_transition, pvol);
    {
        DWORD md = seg->fmt.nAvgBytesPerSec
                       ? (DWORD)((ULONGLONG)seg->pcm_bytes * 1000ull / seg->fmt.nAvgBytesPerSec)
                       : 1000u;
        music_note_started(seg->path, loop_play, md, pvol, 0);
        if (!loop_play && !music_path_is_menu(seg->path))
            music_prefetch_siblings(seg->path);
    }
    return S_OK;
}

static HRESULT play_pcm_music_wasapi(CkPerf *perf, CkSegment *seg, CkPath *apath, DWORD flags, LONG pvol)
{
    LPDIRECTSOUNDBUFFER old_buf;
    CkSegment *old_seg;
    BYTE *raw = NULL;
    DWORD raw_len = 0;
    DWORD dur_ms = 0;
    int fade_transition = (flags & CK_PLAY_FADE_TRANSITION) != 0;
    int loop_play = seg->repeats == (DWORD)-1;

    if (FAILED(load_wav_file_or_pak(seg->path, &raw, &raw_len)) || !raw || raw_len < 4) {
        free(raw);
        return E_NOTIMPL;
    }
    if (memcmp(raw, "OggS", 4) != 0) {
        free(raw);
        return E_NOTIMPL;
    }
    if (!audio_wasapi_play_ogg(raw, raw_len, loop_play, pvol, &dur_ms)) {
        free(raw);
        return E_NOTIMPL;
    }
    free(raw);

    EnterCriticalSection(&perf->lock);
    old_buf = perf->music_buf;
    old_seg = perf->music_seg;
    perf->music_buf = NULL;
    perf->music_seg = seg;
    ck_segment_addref(seg);
    perf->music_path_id = apath ? apath->id : 0;
    LeaveCriticalSection(&perf->lock);
    music_release_old(old_buf, old_seg, fade_transition, pvol);

    if (dur_ms < 1000u)
        dur_ms = seg->fmt.nAvgBytesPerSec
                     ? (DWORD)((ULONGLONG)seg->pcm_bytes * 1000ull / seg->fmt.nAvgBytesPerSec)
                     : 180000u;
    music_note_started(seg->path, loop_play, dur_ms, pvol, 0);
    return S_OK;
}

static HRESULT play_pcm_music_sync(CkPerf *perf, CkSegment *seg, DWORD flags, LONG pvol)
{
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    HRESULT hr;
    int loop_play = seg->repeats == (DWORD)-1;

    if (music_cache_try_acquire_ds_buf(seg->path, &buf) && buf)
        return play_pcm_music_start(perf, seg, NULL, flags, pvol, buf, loop_play);

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS |
                   DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_STATIC;
    desc.dwBufferBytes = seg->pcm_bytes;
    desc.lpwfxFormat = &seg->fmt;
    hr = IDirectSound_CreateSoundBuffer(perf->ds, &desc, &buf, NULL);
    if (FAILED(hr) || !buf)
        return hr;
    if (!ds_buf_write_all(buf, seg->pcm, seg->pcm_bytes)) {
        IDirectSoundBuffer_Release(buf);
        return E_FAIL;
    }
    return play_pcm_music_start(perf, seg, NULL, flags, pvol, buf, loop_play);
}

static DWORD WINAPI music_async_play_proc(void *arg)
{
    MusicPlayJob *job = (MusicPlayJob *)arg;
    play_pcm_music_sync(job->perf, job->seg, job->flags, job->pvol);
    ck_segment_release(job->seg);
    ck_perf_release(job->perf);
    {
        HMODULE module = job->module;
        free(job);
        dm_worker_exit(module, 0);
    }
    return 0;
}

static HRESULT music_play_async(CkPerf *perf, CkSegment *seg, CkPath *apath, DWORD flags, LONG pvol)
{
    MusicPlayJob *job;
    HANDLE th;

    job = (MusicPlayJob *)calloc(1, sizeof(*job));
    if (!job)
        return E_OUTOFMEMORY;
    ck_perf_addref(perf);
    ck_segment_addref(seg);
    job->perf = perf;
    job->seg = seg;
    job->flags = flags;
    job->pvol = pvol;
    job->module = dm_pin_module((const void *)music_async_play_proc);
    if (!job->module) {
        ck_segment_release(seg);
        ck_perf_release(perf);
        free(job);
        return E_OUTOFMEMORY;
    }
    th = CreateThread(NULL, 0, music_async_play_proc, job, 0, NULL);
    if (!th) {
        FreeLibrary(job->module);
        ck_segment_release(seg);
        ck_perf_release(perf);
        free(job);
        return play_pcm_music_sync(perf, seg, flags, pvol);
    }
    CloseHandle(th);
    (void)apath;
    return S_OK;
}

HRESULT play_pcm(CkPerf *perf, CkSegment *seg, CkPath *apath, DWORD flags)
{
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    LPVOID p1 = NULL, p2 = NULL;
    DWORD n1 = 0, n2 = 0;
    HRESULT hr;
    DWORD play_bytes;
    DWORD dur_ms;
    DWORD now;
    int i;
    int is_music;
    int loop_play;
    LONG pvol;
    LONG ppan = 0;
    int use_softpan = 0;
    int fade_transition = (flags & CK_PLAY_FADE_TRANSITION) != 0;

    if (!perf || !perf->ds || !seg)
        return E_FAIL;

    if (apath && !InterlockedCompareExchange((LONG *)&apath->active, 0, 0))
        return S_FALSE;

    /* flags=0 → default/music path; flags=0x80 → secondary SFX channel (DIRECTMUSIC.md) */
    is_music = !(flags & 0x80u) && path_is_music(seg->path);
    /* H-M1: do not force-loop all music — only SetRepeats(-1). */
    loop_play = seg->repeats == (DWORD)-1;
    pvol = apath ? InterlockedCompareExchange(&apath->vol, 0, 0) : 0;

    if (is_music) {
        if (perf->music_wasapi) {
            HRESULT whr = play_pcm_music_wasapi(perf, seg, apath, flags, pvol);
            if (whr != E_NOTIMPL)
                return whr;
            perf->music_wasapi = 0;
            log_msg("dm-replace: WASAPI play failed, falling back to DirectSound");
        }
        {
        LPDIRECTSOUNDBUFFER cached = NULL;
        music_cache_build_ds_for_path(perf->ds, seg->path);
        if (music_cache_try_acquire_ds_buf(seg->path, &cached) && cached)
            return play_pcm_music_start(perf, seg, apath, flags, pvol, cached, loop_play);
        return music_play_async(perf, seg, apath, flags, pvol);
        }
    }

    if (!seg->pcm || !seg->pcm_bytes)
        return E_FAIL;

    if (!is_music && apath && apath->id)
        path_stop_live_sfx(apath->id);
    now = GetTickCount();
    /* H36: Wine GetStatus lies on STATIC buffers → free only by done_tick + margin */
    live_cs_enter();
    for (i = 0; i < s_live_n;) {
        if ((LONG)(now - s_live[i].done_tick) >= 0) {
            live_free_slot(i);
        } else
            i++;
    }
    live_cs_leave();

    play_bytes = seg->pcm_bytes;
    dur_ms = seg->fmt.nAvgBytesPerSec
                 ? (DWORD)((ULONGLONG)play_bytes * 1000ull / seg->fmt.nAvgBytesPerSec)
                 : 1000u;
    if (dur_ms < 50u)
        dur_ms = 50u;
    if (!is_music)
        dur_ms += 400u; /* sfx tail margin — do not recycle early (H36) */

    /* Opt-in looping Hastatus test only (CK_AUDIO_BEACON=1). */
    if (!is_music && path_is_rhastatus(seg->path) && beacon_want()) {
        beacon_start_from_seg(perf, seg, apath);
        return (g_beacon_L && g_beacon_R) ? S_OK : E_FAIL;
    }

    /*
     * Voices/buildings/fight/death/effects: one-shot spatial.
     * Ambient + walk may loop (beds / army march).
     */
    if (!is_music && path_want_spatial(seg->path) &&
        (!loop_play || path_is_ambient_sfx(seg->path) || path_is_walk_sfx(seg->path) ||
         path_is_fight_sfx(seg->path) || path_is_death_sfx(seg->path) ||
         path_is_effects_sfx(seg->path))) {
        int loop_spat =
            loop_play && (path_is_ambient_sfx(seg->path) || path_is_walk_sfx(seg->path));
        int spat_ok = spatial_voice_play(perf, seg, apath, loop_spat);
        if (spat_ok)
            return S_OK;
    }

    {
        WAVEFORMATEX play_wfx = seg->fmt;
        BYTE *render = NULL;
        DWORD render_bytes = play_bytes;
        ppan = (!is_music && apath)
                   ? InterlockedCompareExchange(&apath->pan, 0, 0)
                   : 0;
        use_softpan = !is_music && seg->fmt.wBitsPerSample == 16 &&
                      (seg->fmt.nChannels == 1 || seg->fmt.nChannels == 2);

        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS |
                       DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_STATIC;
        if (use_softpan) {
            stereo_wfx_from(&seg->fmt, &play_wfx);
            if (seg->fmt.nChannels == 1)
                render_bytes = (seg->pcm_bytes / 2u) * 4u;
            else
                render_bytes = seg->pcm_bytes;
            render = (BYTE *)malloc(render_bytes);
            if (!render)
                return E_OUTOFMEMORY;
            if (pcm_render_panned_stereo(seg->pcm, &seg->fmt, seg->pcm_bytes, render, render_bytes,
                                         ppan) != render_bytes) {
                free(render);
                return E_FAIL;
            }
            desc.dwBufferBytes = render_bytes;
            desc.lpwfxFormat = &play_wfx;
            play_bytes = render_bytes;
        } else {
            desc.dwFlags |= DSBCAPS_CTRLPAN;
            desc.dwBufferBytes = play_bytes;
            desc.lpwfxFormat = &seg->fmt;
        }

        hr = IDirectSound_CreateSoundBuffer(perf->ds, &desc, &buf, NULL);
        if (SUCCEEDED(hr) && buf) {
            if (render) {
                if (!ds_buf_write_all(buf, render, render_bytes))
                    hr = E_FAIL;
            } else {
                hr = IDirectSoundBuffer_Lock(buf, 0, play_bytes, &p1, &n1, &p2, &n2, 0);
                if (SUCCEEDED(hr)) {
                    memcpy(p1, seg->pcm, n1);
                    if (p2 && n2)
                        memcpy(p2, seg->pcm + n1, n2);
                    IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
                }
            }
            if (SUCCEEDED(hr)) {
                DWORD play_flags = loop_play ? DSBPLAY_LOOPING : 0;
                path_apply_buf_vol(buf, pvol);
                if (!use_softpan && !is_music && apath)
                    path_apply_buf_pan(buf, InterlockedCompareExchange(&apath->pan, 0, 0));
                hr = IDirectSoundBuffer_Play(buf, 0, 0, play_flags);
            }
        }
        free(render);
    }

    if (SUCCEEDED(hr) && buf) {
        if (is_music) {
            /* Unreachable — music returns earlier. */
            IDirectSoundBuffer_Release(buf);
        } else {
            live_cs_enter();
            if (s_live_n >= (int)(sizeof(s_live) / sizeof(s_live[0])))
                live_free_slot(0);
            memset(&s_live[s_live_n], 0, sizeof(s_live[0]));
            s_live[s_live_n].b = buf;
            s_live[s_live_n].bR = NULL;
            s_live[s_live_n].done_tick =
                loop_play ? (GetTickCount() + 3600000u) : (GetTickCount() + dur_ms);
            s_live[s_live_n].path_id = apath ? apath->id : 0;
            s_live[s_live_n].seg = seg;
            ck_segment_addref(seg);
            s_live[s_live_n].spatial = 0;
            s_live[s_live_n].looping = loop_play ? 1 : 0;
            s_live[s_live_n].base_vol = pvol;
            s_live[s_live_n].last_pan = use_softpan ? ppan : 0x7fffffff;
            s_live_n++;
            buf = NULL;
            live_cs_leave();
        }
    } else if (buf) {
        IDirectSoundBuffer_Release(buf);
        buf = NULL;
    }

    return hr;
}

