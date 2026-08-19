#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "vk_present.h"
#include "movie_player.h"
#include "dm_replace.h"
#include "hitch.h"
#include <stdio.h>
#include <string.h>

typedef int(WINAPI *PFN_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT,
                                          CONST VOID *, CONST BITMAPINFO *, UINT);
typedef BOOL(WINAPI *PFN_EnumDisplaySettingsA)(LPCSTR, DWORD, DEVMODEA *);
typedef LONG(WINAPI *PFN_ChangeDisplaySettingsA)(DEVMODEA *, DWORD);
typedef BOOL(WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI *PFN_GetClientRect)(HWND, LPRECT);
typedef BOOL(WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL(WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
typedef int(WINAPI *PFN_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
typedef int(WINAPI *PFN_MessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT);

static PFN_SetDIBitsToDevice real_SetDIBitsToDevice;
static void *g_iat_SetDIBitsToDevice;
static PFN_EnumDisplaySettingsA real_EnumDisplaySettingsA;
static void *g_iat_EnumDisplaySettingsA;
static PFN_ChangeDisplaySettingsA real_ChangeDisplaySettingsA;
static void *g_iat_ChangeDisplaySettingsA;
static PFN_SetWindowPos real_SetWindowPos;
static void *g_iat_SetWindowPos;
static PFN_GetClientRect real_GetClientRect;
static void *g_iat_GetClientRect;
static PFN_ScreenToClient real_ScreenToClient;
static void *g_iat_ScreenToClient;
static PFN_ClientToScreen real_ClientToScreen;
static void *g_iat_ClientToScreen;
static PFN_MessageBoxA real_MessageBoxA;
static PFN_MessageBoxW real_MessageBoxW;
static BYTE *g_mba_tramp;
static BYTE g_mba_saved[16];
static void *g_mba_target;
static BYTE *g_mbw_tramp;
static BYTE g_mbw_saved[16];
static void *g_mbw_target;
static volatile LONG g_eds_calls;
static volatile LONG g_cds_calls;
static volatile LONG g_swp_calls;
static volatile LONG g_cvm_suppress_mb;

typedef int(__cdecl *PFN_ConfigureVideoMode)(int force, int w, int h);
static PFN_ConfigureVideoMode real_ConfigureVideoMode;
static BYTE *g_cvm_tramp;
static BYTE g_cvm_saved[16];
static SIZE_T g_cvm_steal;
static void *g_cvm_target;

/* Celtic_Kings cutscene entry points (winegstreamer → Vulkan player). */
static BYTE *g_movie_path_tramp;
static BYTE g_movie_path_saved[16];
static SIZE_T g_movie_path_steal;
static void *g_movie_path_site;
typedef int(__cdecl *PFN_PlayMoviePath)(const char *path);
static PFN_PlayMoviePath real_PlayMoviePath;

static BYTE *g_splash_pm_tramp;
static BYTE g_splash_pm_saved[16];
static SIZE_T g_splash_pm_steal;
static void *g_splash_pm_site;
typedef unsigned(__attribute__((thiscall)) *PFN_SplashPlayMovie)(void *self, const char *path);
static PFN_SplashPlayMovie real_SplashPlayMovie;

/* tpw CMovieWnd::PlayMovie(void) — silence BGM, leave native/quartz playback. */
static BYTE *g_moviewnd_tramp;
static BYTE g_moviewnd_saved[16];
static SIZE_T g_moviewnd_steal;
static void *g_moviewnd_site;
typedef unsigned(__attribute__((thiscall)) *PFN_MovieWndPlay)(void *self);
static PFN_MovieWndPlay real_MovieWndPlay;

static volatile LONG g_sdi_calls;

static void log_ini_resolution_prefs(void)
{
    char path[MAX_PATH];
    char line[256];
    FILE *f;
    int res_idx = -1;
    path_join(path, sizeof(path), "vxSettings.ini");
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "Resolution=%d", &res_idx) == 1)
                break;
        }
        fclose(f);
    }
    log_msg("res-trace vxSettings Resolution=%d", res_idx);
    path_join(path, sizeof(path), "Data\\CONST.INI");
    f = fopen(path, "r");
    if (!f) {
        path_join(path, sizeof(path), "Data/CONST.INI");
        f = fopen(path, "r");
    }
    if (f) {
        int in_res = 0;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '[')
                in_res = (strstr(line, "[Resolutions]") != NULL);
            else if (in_res) {
                int n = 0, x = 0, y = 0;
                if (sscanf(line, "Res%d_x = %d", &n, &x) == 2 ||
                    sscanf(line, "Res%d_x=%d", &n, &x) == 2) {
                    log_msg("res-trace CONST Res%d_x=%d", n, x);
                    (void)y;
                } else if (sscanf(line, "Res%d_y = %d", &n, &y) == 2 ||
                           sscanf(line, "Res%d_y=%d", &n, &y) == 2) {
                    log_msg("res-trace CONST Res%d_y=%d", n, y);
                }
            }
        }
        fclose(f);
    }
}

static int g_eds_real_count = -1;

/* CONST Res3=1536×864 etc. are often missing from Wine EDS — FindMode needs an exact match. */
static BOOL eds_fill_injected(DEVMODEA *dm, int inj_idx)
{
    static const struct {
        WORD w, h;
    } extras[] = {{1536, 864}, {1600, 900}, {1440, 1080}, {1920, 1080},
                  {1280, 720},  {1024, 768}, {800, 600},   {640, 480}};
    if (!dm || inj_idx < 0 || inj_idx >= (int)(sizeof(extras) / sizeof(extras[0])))
        return FALSE;
    memset(dm, 0, sizeof(*dm));
    dm->dmSize = (WORD)sizeof(*dm);
    dm->dmPelsWidth = extras[inj_idx].w;
    dm->dmPelsHeight = extras[inj_idx].h;
    dm->dmBitsPerPel = 16;
    dm->dmDisplayFrequency = 60;
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return TRUE;
}

static BOOL WINAPI hook_EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA *dm)
{
    BOOL ok;
    LONG n = InterlockedIncrement(&g_eds_calls);
    DWORD real_bpp = 0;

    if (mode == 0)
        g_eds_real_count = -1;

    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS) {
        ok = real_EnumDisplaySettingsA(device, mode, dm);
        if (ok && dm) {
            if (dm->dmBitsPerPel == 24 || dm->dmBitsPerPel == 32)
                dm->dmBitsPerPel = 16;
            if (dm->dmDisplayFrequency > 60) {
                dm->dmDisplayFrequency = 60;
                dm->dmFields |= DM_DISPLAYFREQUENCY;
            }
        }
        return ok;
    }

    ok = real_EnumDisplaySettingsA(device, mode, dm);
    /*
     * FindMode requires dmBitsPerPel == 16 + exact w/h. Spoof 32→16 and append
     * CONST resolutions Wine omits (esp. 1536×864) so post-splash CVM can succeed.
     */
    if (ok && dm) {
        if (g_eds_real_count < (int)mode + 1)
            g_eds_real_count = (int)mode + 1;
        real_bpp = dm->dmBitsPerPel;
        if (dm->dmBitsPerPel == 24 || dm->dmBitsPerPel == 32)
            dm->dmBitsPerPel = 16;
        if (dm->dmDisplayFrequency > 60) {
            dm->dmDisplayFrequency = 60;
            dm->dmFields |= DM_DISPLAYFREQUENCY;
        }
        if (n <= 48 || mode < 16 ||
            (dm->dmPelsWidth == 1920 && dm->dmPelsHeight == 1080) ||
            (dm->dmPelsWidth == 1536 && dm->dmPelsHeight == 864) ||
            (dm->dmPelsWidth == 640 && dm->dmPelsHeight == 480)) {
            log_msg("EnumDisplaySettingsA #%ld mode=%ld %lux%lu bpp=%lu->%lu hz=%lu", (long)n,
                    (long)mode, (unsigned long)dm->dmPelsWidth, (unsigned long)dm->dmPelsHeight,
                    (unsigned long)real_bpp, (unsigned long)dm->dmBitsPerPel,
                    (unsigned long)dm->dmDisplayFrequency);
        }
        return TRUE;
    }

    if (g_eds_real_count < 0)
        g_eds_real_count = (int)mode;
    {
        int inj = (int)mode - g_eds_real_count;
        if (eds_fill_injected(dm, inj)) {
            log_msg("EnumDisplaySettingsA #%ld mode=%ld INJECT %lux%lu bpp=16", (long)n, (long)mode,
                    (unsigned long)dm->dmPelsWidth, (unsigned long)dm->dmPelsHeight);
            /* #region agent log */
            {
                char js[160];
                snprintf(js, sizeof(js),
                         "{\"n\":%ld,\"mode\":%ld,\"inj\":%d,\"w\":%lu,\"h\":%lu,\"real_n\":%d}",
                         (long)n, (long)mode, inj, (unsigned long)dm->dmPelsWidth,
                         (unsigned long)dm->dmPelsHeight, g_eds_real_count);
                hooks_agent("H99", "hooks.c:EDS", "eds-inject", js);
            }
            /* #endregion */
            return TRUE;
        }
    }
    /* #region agent log */
    if (n <= 8 || (mode >= (DWORD)g_eds_real_count && mode < (DWORD)g_eds_real_count + 12u)) {
        char js[120];
        snprintf(js, sizeof(js), "{\"n\":%ld,\"mode\":%ld,\"real_n\":%d,\"end\":1}", (long)n,
                 (long)mode, g_eds_real_count);
        hooks_agent("H99", "hooks.c:EDS", "eds-end", js);
    }
    /* #endregion */
    return FALSE;
}

static int ck_is_cutscene_res(int w, int h)
{
    return (w == 640 && h == 480);
}

static int cds_current_is_force(void)
{
    DEVMODEA cur;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = (WORD)sizeof(cur);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur))
        return 0;
    return (int)cur.dmPelsWidth == CK_FORCE_W && (int)cur.dmPelsHeight == CK_FORCE_H;
}

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    LONG rc;
    LONG raw_rc;
    LONG n = InterlockedIncrement(&g_cds_calls);
    DEVMODEA local;
    DEVMODEA *use;
    DWORD asked_w = dm ? dm->dmPelsWidth : 0;
    DWORD asked_h = dm ? dm->dmPelsHeight : 0;
    DWORD asked_bpp = dm ? dm->dmBitsPerPel : 0;
    int cutscene = dm && ck_is_cutscene_res((int)asked_w, (int)asked_h);
    int salvaged = 0;
    int skipped = 0;

    (void)asked_bpp;

    /* No CK_FORCE_RES: do not let the game shrink/resize the HWND via CDS.
     * Under gamescope, CDS→1024×768 while the Vulkan surface stays ~1920 leaves the
     * menu window offset with a black pillar (screenshot: ~25% black left).
     * Soft DIB still follows game BMI; present scales it into the stable swapchain. */
    if (!g_force_res) {
        if (cutscene || (asked_w > 0 && asked_h > 0)) {
            log_msg("ChangeDisplaySettingsA #%ld nop asked %lux%lu (force-res off, keep surface)",
                    (long)n, (unsigned long)asked_w, (unsigned long)asked_h);
            /* #region agent log */
            {
                char js[180];
                snprintf(js, sizeof(js),
                         "{\"n\":%ld,\"rc\":0,\"asked_w\":%lu,\"asked_h\":%lu,\"force_res\":0,"
                         "\"cds_nop\":1,\"cutscene\":%d}",
                         (long)n, (unsigned long)asked_w, (unsigned long)asked_h, cutscene ? 1 : 0);
                hooks_agent("H97", "hooks.c:ChangeDisplaySettingsA", "cds-result", js);
            }
            /* #endregion */
            return DISP_CHANGE_SUCCESSFUL;
        }
        rc = real_ChangeDisplaySettingsA(dm, flags);
        log_msg("ChangeDisplaySettingsA #%ld passthrough => %ld (force-res off)", (long)n, (long)rc);
        return rc;
    }

    memset(&local, 0, sizeof(local));
    local.dmSize = (WORD)sizeof(local);
    /* CK_FORCE_RES=1: always apply 1920x1080×32. */
    local.dmPelsWidth = CK_FORCE_W;
    local.dmPelsHeight = CK_FORCE_H;
    local.dmBitsPerPel = 32;
    local.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    local.dmDisplayFrequency = 60;
    use = &local;
    (void)cutscene;
    log_msg("ChangeDisplaySettingsA #%ld FORCE %lux%lu (asked %lux%lu) flags=0x%lx", (long)n,
            (unsigned long)use->dmPelsWidth, (unsigned long)use->dmPelsHeight,
            (unsigned long)asked_w, (unsigned long)asked_h, (unsigned long)flags);
    /*
     * Runtime (I2/tpw session 764ba7): Wine CDS→force often returns -2 (BADMODE) when
     * already at 1920x1080; returning -2 → AV. If current mode is already force res,
     * skip/salvage as SUCCESS.
     */
    if (cds_current_is_force()) {
        rc = DISP_CHANGE_SUCCESSFUL;
        raw_rc = rc;
        skipped = 1;
        log_msg("ChangeDisplaySettingsA #%ld skip (already %dx%d)", (long)n, CK_FORCE_W,
                CK_FORCE_H);
    } else {
        rc = real_ChangeDisplaySettingsA(use, flags);
        raw_rc = rc;
        if (rc != DISP_CHANGE_SUCCESSFUL && cds_current_is_force()) {
            salvaged = 1;
            rc = DISP_CHANGE_SUCCESSFUL;
            log_msg("ChangeDisplaySettingsA #%ld salvage raw=%ld → 0 (already force)", (long)n,
                    (long)raw_rc);
        } else {
            log_msg("ChangeDisplaySettingsA #%ld => %ld", (long)n, (long)rc);
        }
    }
    /* #region agent log — H70 */
    {
        char js[280];
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"rc\":%ld,\"raw_rc\":%ld,\"asked_w\":%lu,\"asked_h\":%lu,"
                 "\"force_w\":%lu,\"force_h\":%lu,\"flags\":%lu,\"skipped\":%d,\"salvaged\":%d}",
                 (long)n, (long)rc, (long)raw_rc, (unsigned long)asked_w, (unsigned long)asked_h,
                 (unsigned long)use->dmPelsWidth, (unsigned long)use->dmPelsHeight,
                 (unsigned long)flags, skipped, salvaged);
        hooks_agent("H70", "hooks.c:ChangeDisplaySettingsA", "cds-result", js);
    }
    /* #endregion */
    return rc;
}

static BOOL WINAPI hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    BOOL ok;
    LONG n = InterlockedIncrement(&g_swp_calls);
    int fx = x, fy = y, fcx = cx, fcy = cy;
    UINT fflags = flags;
    int do_force = 0;
    if (g_force_res && (flags & SWP_NOSIZE) == 0 && (cx >= 640 || cy >= 480)) {
        fcx = CK_FORCE_W;
        fcy = CK_FORCE_H;
        if ((flags & SWP_NOMOVE) == 0) {
            fx = 0;
            fy = 0;
        }
        do_force = 1;
    }
    if (n <= 40 || do_force || ck_is_cutscene_res(cx, cy) || (cx >= 640 && cy >= 480)) {
        log_msg("SetWindowPos #%ld hwnd=%p %dx%d -> %dx%d force=%d flags=0x%x", (long)n,
                (void *)hwnd, cx, cy, fcx, fcy, do_force, (unsigned)fflags);
        /* #region agent log */
        if (cx >= 640 || cy >= 480) {
            char js[160];
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"cx\":%d,\"cy\":%d,\"fcx\":%d,\"fcy\":%d,\"force\":%d,\"flags\":%u}",
                     (long)n, cx, cy, fcx, fcy, do_force, (unsigned)fflags);
            hooks_agent("H101", "hooks.c:SetWindowPos", "swp", js);
        }
        /* #endregion */
    }
    ok = real_SetWindowPos(hwnd, after, fx, fy, fcx, fcy, fflags);
    return ok;
}

static BOOL WINAPI hook_GetClientRect(HWND hwnd, LPRECT rc)
{
    BOOL ok = real_GetClientRect(hwnd, rc);
    int sw, sh;
    if (ok && rc && vk_present_scale_active() && hwnd == vk_present_hwnd() &&
        vk_present_soft_size(&sw, &sh)) {
        rc->left = 0;
        rc->top = 0;
        rc->right = sw;
        rc->bottom = sh;
    }
    return ok;
}

static BOOL WINAPI hook_ScreenToClient(HWND hwnd, LPPOINT pt)
{
    BOOL ok;
    RECT real;
    int sw, sh;
    ok = real_ScreenToClient(hwnd, pt);
    if (!ok || !pt || !vk_present_scale_active() || hwnd != vk_present_hwnd() ||
        !vk_present_soft_size(&sw, &sh))
        return ok;
    if (!real_GetClientRect(hwnd, &real))
        return ok;
    {
        int cw = real.right - real.left;
        int ch = real.bottom - real.top;
        if (cw > 0 && ch > 0) {
            pt->x = (LONG)((long long)pt->x * sw / cw);
            pt->y = (LONG)((long long)pt->y * sh / ch);
        }
    }
    return ok;
}

static BOOL WINAPI hook_ClientToScreen(HWND hwnd, LPPOINT pt)
{
    RECT real;
    int sw, sh;
    if (pt && vk_present_scale_active() && hwnd == vk_present_hwnd() &&
        vk_present_soft_size(&sw, &sh) && real_GetClientRect(hwnd, &real)) {
        int cw = real.right - real.left;
        int ch = real.bottom - real.top;
        if (cw > 0 && ch > 0 && sw > 0 && sh > 0) {
            pt->x = (LONG)((long long)pt->x * cw / sw);
            pt->y = (LONG)((long long)pt->y * ch / sh);
        }
    }
    return real_ClientToScreen(hwnd, pt);
}

static int __cdecl hook_PlayMoviePath(const char *path)
{
    LONGLONG t0 = hitch_qpc_now();
    hitch_mark("movie");
    log_msg("movie: hook PlayMoviePath '%s'", path ? path : "(null)");
    dm_replace_silence_music();
    if (ck_movie_play(path)) {
        hitch_note_ms("F7", "hooks.c:movie", "movie-play", "movie", hitch_qpc_ms_since(t0), "{}");
        return 1;
    }
    if (real_PlayMoviePath)
        return real_PlayMoviePath(path);
    return 0;
}

static unsigned __attribute__((thiscall)) hook_SplashPlayMovie(void *self, const char *path)
{
    LONGLONG t0 = hitch_qpc_now();
    hitch_mark("movie-splash");
    log_msg("movie: hook SplashPlayMovie '%s'", path ? path : "(null)");
    /* #region agent log */
    hooks_agent("H80", "hooks.c:SplashPlayMovie", "movie-splash-enter",
                path ? "{\"has_path\":1}" : "{\"has_path\":0}");
    /* #endregion */
    /* RE menu owns the boot UI — skip retail splash race with embedded MainMenu. */
    if (env_on("CK_NATIVE_MENU", 0)) {
        log_msg("movie: SplashPlayMovie skipped (CK_NATIVE_MENU)");
        /* #region agent log */
        hooks_agent("H-E", "hooks.c:SplashPlayMovie", "movie-splash-skip-native-menu", "{}");
        /* #endregion */
        (void)self;
        (void)path;
        (void)t0;
        return 1;
    }
    dm_replace_silence_music();
    if (ck_movie_play(path)) {
        hitch_note_ms("F7", "hooks.c:movie", "movie-splash", "movie", hitch_qpc_ms_since(t0),
                      "{}");
        /* #region agent log */
        hooks_agent("H80", "hooks.c:SplashPlayMovie", "movie-splash-ok", "{\"via\":\"ck\"}");
        /* #endregion */
        return 1;
    }
    log_msg("movie: SplashPlayMovie skip (unresolved/fail) '%s'", path ? path : "(null)");
    vk_present_restore_game_soft();
    /* #region agent log */
    hooks_agent("H80", "hooks.c:SplashPlayMovie", "movie-splash-skip", "{\"via\":\"missing\"}");
    /* #endregion */
    (void)self;
    (void)real_SplashPlayMovie;
    return 1;
}

/* tpw CMovieWnd::PlayMovie(void) — no path arg; mute DM BGM then native play. */
static unsigned __attribute__((thiscall)) hook_MovieWndPlay(void *self)
{
    log_msg("movie: hook CMovieWnd::PlayMovie (tpw)");
    /* #region agent log — H72 */
    hooks_agent("H72", "hooks.c:MovieWndPlay", "movie-wnd-enter", "{}");
    /* #endregion */
    dm_replace_silence_music();
    if (real_MovieWndPlay)
        return real_MovieWndPlay(self);
    return 0;
}

static int WINAPI hook_MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type)
{
    /* gamescope/Wine: post-intro Video Mode MB often AVs inside MessageBoxW/A. */
    (void)hwnd;
    (void)type;
    log_msg("MessageBoxA SUPPRESSED: cap='%.40s' text='%.60s'", caption ? caption : "",
            text ? text : "");
    /* #region agent log */
    hooks_agent("H96", "hooks.c:MessageBoxA", "msgbox-suppress",
                "{\"ansi\":1}");
    /* #endregion */
    return IDOK;
}

static int WINAPI hook_MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
    (void)hwnd;
    (void)text;
    (void)caption;
    (void)type;
    log_msg("MessageBoxW SUPPRESSED");
    /* #region agent log */
    hooks_agent("H96", "hooks.c:MessageBoxW", "msgbox-suppress", "{\"ansi\":0}");
    /* #endregion */
    return IDOK;
}

static int __cdecl hook_ConfigureVideoMode(int force, int w, int h)
{
    int ow = w, oh = h;
    int cutscene = ck_is_cutscene_res(w, h);
    int soft_w = 0, soft_h = 0;
    int rc;
    LONGLONG t0 = hitch_qpc_now();

    hitch_mark("CVM");
    (void)vk_present_soft_size(&soft_w, &soft_h);

    /*
     * Match original tpw ConfigureVideoMode:
     *  - force=0: skip FindMode, still programs soft rect / letterbox / SetMode; returns 0xFFFF
     *  - force=1: FindMode+CDS required; 0 = OK, 0xFFFF = fail (menu then falls back to 1024)
     * CDS is nop'd when force_res=0 so FindMode can succeed without resizing HWND/swapchain.
     * Cutscene 640×480: skip native so soft globals are not forced to 640.
     * Soft width > scanline table size AVs on I2; clamp only if expand did not raise max.
     */
    if (cutscene) {
        log_msg("ConfigureVideoMode CUTSCENE-SKIP (asked %dx%d force=%d)", ow, oh, force);
        hitch_note_ms("F7", "hooks.c:CVM", "cvm-cutscene-skip", "CVM", hitch_qpc_ms_since(t0),
                      "{}");
        /* #region agent log */
        {
            char js[120];
            snprintf(js, sizeof(js), "{\"asked_w\":%d,\"asked_h\":%d,\"force\":%d,\"cut\":1}", ow, oh,
                     force);
            hooks_agent("H98", "hooks.c:CVM", "cvm-cutscene", js);
        }
        /* #endregion */
        return 0; /* force=1 success code — do not trigger 1024 fallback */
    }

    if (w > g_scan_soft_max) {
        int nw = g_scan_soft_max;
        int nh = (oh > 0 && ow > 0) ? (int)((long long)oh * nw / ow) : ((nw * 9) / 16);
        if (nw == 1600 && ow == 1920 && oh == 1080)
            nh = 900;
        if (nw == 1920 && ow == 1920 && oh == 1080)
            nh = 1080;
        if (nh < 600)
            nh = 600;
        log_msg("ConfigureVideoMode CLAMP soft %dx%d -> %dx%d (scan soft_max=%d)", ow, oh, nw, nh,
                g_scan_soft_max);
        /* #region agent log */
        {
            char js[180];
            snprintf(js, sizeof(js),
                     "{\"asked_w\":%d,\"asked_h\":%d,\"clamp_w\":%d,\"clamp_h\":%d,\"soft_max\":%d,"
                     "\"force\":%d}",
                     ow, oh, nw, nh, g_scan_soft_max, force);
            hooks_agent("H103", "hooks.c:CVM", "cvm-clamp-soft", js);
        }
        /* #endregion */
        w = nw;
        h = nh;
    }

    if (!real_ConfigureVideoMode) {
        log_msg("ConfigureVideoMode MISSING tramp asked %dx%d", ow, oh);
        return 0xFFFF;
    }

    InterlockedExchange(&g_cvm_suppress_mb, 1);
    if (g_force_res && !cutscene && force) {
        rc = real_ConfigureVideoMode(force, w, h);
        log_msg("ConfigureVideoMode NATIVE-FORCE soft %dx%d (asked %dx%d force=%d) => %d", w, h, ow,
                oh, force, rc);
    } else {
        rc = real_ConfigureVideoMode(force, w, h);
        log_msg("ConfigureVideoMode NATIVE %dx%d force=%d => %d (force_res=%d)", w, h, force, rc,
                g_force_res);
    }
    InterlockedExchange(&g_cvm_suppress_mb, 0);

    hitch_note_ms("F7", "hooks.c:CVM", "cvm-native", "CVM", hitch_qpc_ms_since(t0), "{}");
    /* #region agent log */
    {
        char js[200];
        snprintf(js, sizeof(js),
                 "{\"asked_w\":%d,\"asked_h\":%d,\"use_w\":%d,\"use_h\":%d,\"force\":%d,\"rc\":%d,"
                 "\"force_res\":%d,\"native\":1}",
                 ow, oh, w, h, force, rc, g_force_res);
        hooks_agent("H98", "hooks.c:CVM", "cvm-native", js);
    }
    /* #endregion */
    return rc;
}

static int WINAPI hook_SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc,
                                        int ySrc, UINT start, UINT lines, CONST VOID *bits,
                                        CONST BITMAPINFO *bmi, UINT usage)
{
    LONG n = InterlockedIncrement(&g_sdi_calls);
    static LONG s_frame_seq;
    LONG seq;
    LARGE_INTEGER qpc;
    char js[240];

    if (ck_movie_active())
        return (int)h; /* cutscene owns Vulkan present */
    hitch_set_main_tid(GetCurrentThreadId());
    {
        double gap = hitch_ms_since_last_present();
        double sleep_ms = 0.0;
        long sleep_n = 0, gtc_n = 0;
        char gtc_callers[640];
        hooks_hitch_take_gap_stats(&sleep_ms, &sleep_n, &gtc_n);
        hooks_hitch_dump_gtc_callers(gtc_callers, sizeof(gtc_callers));
        /* #region agent log */
        if (gap >= 40.0) {
            static LONG s_gap_n;
            LONG gn = InterlockedIncrement(&s_gap_n);
            if (gn <= 40 || (gn % 15) == 0 || gap >= 80.0) {
                char js[1100];
                char mark[48];
                LONG cL = 0, cT = 0, cR = 0, cB = 0;
                void *ra = __builtin_return_address(0);
                /* #region agent log */
                hitch_get_mark(mark, sizeof(mark));
                mm_read_cam(&cL, &cT, &cR, &cB);
                snprintf(js, sizeof(js),
                         "{\"gap_ms\":%.3f,\"sleep_ms\":%.3f,\"sleep_n\":%ld,\"gtc_n\":%ld,"
                         "\"gtc_callers\":[%s],\"wh\":[%lu,%lu],\"tid\":%lu,\"ret\":\"0x%lX\","
                         "\"n\":%ld,\"mark\":\"%s\",\"cam\":[%ld,%ld,%ld,%ld]}",
                         gap, sleep_ms, sleep_n, gtc_n, gtc_callers,
                         (unsigned long)w, (unsigned long)h,
                         (unsigned long)GetCurrentThreadId(), (unsigned long)(ULONG_PTR)ra,
                         (long)gn, mark[0] ? mark : "", (long)cL, (long)cT, (long)cR, (long)cB);
                hooks_agent("H-LOC", "hooks_video.c:SetDIBits", "dib-gap", js);
                /* #endregion */
            }
        }
        /* #endregion */
    }
    hooks_hitch_rebind_modules();
    /* Log first sizes so we learn retail blit geometry. */
    if (!g_proxy_min && (n <= 40 || (n % 100) == 0)) {
        int bpp = (bmi && bits) ? bmi->bmiHeader.biBitCount : -1;
        int biw = bmi ? bmi->bmiHeader.biWidth : -1;
        int bih = bmi ? bmi->bmiHeader.biHeight : -1;
        log_msg("SetDIBitsToDevice #%ld dest=(%d,%d) wh=%lux%lu src=(%d,%d) start=%u lines=%u "
                "bpp=%d bi=%dx%d clamps=%ld",
                (long)n, xDest, yDest, (unsigned long)w, (unsigned long)h, xSrc, ySrc, start, lines,
                bpp, biw, bih, (long)g_scan_clamps);
    }
    /* Always tick pan beacon (vk_present path alone was too rare — H-F). */
    dm_replace_beacon_tick();
    if (bmi && bits) {
        /* #region agent log */
        seq = InterlockedIncrement(&s_frame_seq);
        if (seq <= 120 || (seq % 30) == 0) {
            QueryPerformanceCounter(&qpc);
            snprintf(js, sizeof(js),
                     "{\"seq\":%ld,\"phase\":\"dib-before-present\",\"wh\":[%lu,%lu],\"qpc\":%llu,"
                     "\"path\":\"frame-timeline\"}",
                     (long)seq, (unsigned long)w, (unsigned long)h,
                     (unsigned long long)qpc.QuadPart);
            hooks_agent("H-FRAME", "hooks_video.c:SetDIBits", "frame-phase", js);
        }
        /* #endregion */
        /* Option B: present via Wine Vulkan; skip GDI on success.
         * Camera pan runs inside vk_present on successful swap. */
        if (vk_present_try(hdc, xDest, yDest, w, h, xSrc, ySrc, start, lines, bits, bmi, usage)) {
            return (int)h; /* GDI success ≈ lines copied */
        }
    }
    return real_SetDIBitsToDevice(hdc, xDest, yDest, w, h, xSrc, ySrc, start, lines, bits, bmi,
                                  usage);
}


void hooks_video_install(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    HMODULE user = GetModuleHandleA("user32.dll");

        if (gdi)
            real_SetDIBitsToDevice =
                (PFN_SetDIBitsToDevice)GetProcAddress(gdi, "SetDIBitsToDevice");
        if (game && real_SetDIBitsToDevice) {
            if (patch_iat_entry(game, "GDI32.dll", "SetDIBitsToDevice",
                                (void *)hook_SetDIBitsToDevice, (void **)&real_SetDIBitsToDevice,
                                &g_iat_SetDIBitsToDevice))
                log_msg("IAT hooked gdi32!SetDIBitsToDevice");
            else
                log_msg("WARN: SetDIBitsToDevice IAT hook failed");
        }
        if (user) {
            real_EnumDisplaySettingsA =
                (PFN_EnumDisplaySettingsA)GetProcAddress(user, "EnumDisplaySettingsA");
            real_ChangeDisplaySettingsA =
                (PFN_ChangeDisplaySettingsA)GetProcAddress(user, "ChangeDisplaySettingsA");
            real_SetWindowPos = (PFN_SetWindowPos)GetProcAddress(user, "SetWindowPos");
            real_GetClientRect = (PFN_GetClientRect)GetProcAddress(user, "GetClientRect");
            real_ScreenToClient = (PFN_ScreenToClient)GetProcAddress(user, "ScreenToClient");
            real_ClientToScreen = (PFN_ClientToScreen)GetProcAddress(user, "ClientToScreen");
            real_MessageBoxA = (PFN_MessageBoxA)GetProcAddress(user, "MessageBoxA");
            real_MessageBoxW = (PFN_MessageBoxW)GetProcAddress(user, "MessageBoxW");
        }
        if (game && real_EnumDisplaySettingsA) {
            if (patch_iat_entry(game, "USER32.dll", "EnumDisplaySettingsA",
                                (void *)hook_EnumDisplaySettingsA,
                                (void **)&real_EnumDisplaySettingsA, &g_iat_EnumDisplaySettingsA))
                log_msg("IAT hooked user32!EnumDisplaySettingsA");
            else
                log_msg("WARN: EnumDisplaySettingsA IAT hook failed");
        }
        if (game && real_ChangeDisplaySettingsA) {
            if (patch_iat_entry(game, "USER32.dll", "ChangeDisplaySettingsA",
                                (void *)hook_ChangeDisplaySettingsA,
                                (void **)&real_ChangeDisplaySettingsA,
                                &g_iat_ChangeDisplaySettingsA))
                log_msg("IAT hooked user32!ChangeDisplaySettingsA");
            else
                log_msg("WARN: ChangeDisplaySettingsA IAT hook failed");
        }
        if (game && real_SetWindowPos) {
            if (patch_iat_entry(game, "USER32.dll", "SetWindowPos", (void *)hook_SetWindowPos,
                                (void **)&real_SetWindowPos, &g_iat_SetWindowPos))
                log_msg("IAT hooked user32!SetWindowPos");
            else
                log_msg("WARN: SetWindowPos IAT hook failed");
        }
        if (game && real_GetClientRect) {
            if (patch_iat_entry(game, "USER32.dll", "GetClientRect", (void *)hook_GetClientRect,
                                (void **)&real_GetClientRect, &g_iat_GetClientRect))
                log_msg("IAT hooked user32!GetClientRect (vk scale)");
            else
                log_msg("WARN: GetClientRect IAT hook failed");
        }
        if (game && real_ScreenToClient) {
            if (patch_iat_entry(game, "USER32.dll", "ScreenToClient", (void *)hook_ScreenToClient,
                                (void **)&real_ScreenToClient, &g_iat_ScreenToClient))
                log_msg("IAT hooked user32!ScreenToClient (vk scale)");
            else
                log_msg("WARN: ScreenToClient IAT hook failed");
        }
        if (game && real_ClientToScreen) {
            if (patch_iat_entry(game, "USER32.dll", "ClientToScreen", (void *)hook_ClientToScreen,
                                (void **)&real_ClientToScreen, &g_iat_ClientToScreen))
                log_msg("IAT hooked user32!ClientToScreen (vk scale)");
            else
                log_msg("WARN: ClientToScreen IAT hook failed");
        }
        /* Inline-hook MessageBox exports — IAT alone missed Wine/CVM path. */
        if (real_MessageBoxA && !g_mba_tramp) {
            g_mba_target = (void *)real_MessageBoxA;
            if (install_inline_hook(g_mba_target, (void *)hook_MessageBoxA, 5, &g_mba_tramp,
                                    g_mba_saved)) {
                real_MessageBoxA = (PFN_MessageBoxA)g_mba_tramp;
                log_msg("inline hooked user32!MessageBoxA (CVM suppress)");
            } else {
                log_msg("WARN: MessageBoxA inline hook failed");
            }
        }
        if (real_MessageBoxW && !g_mbw_tramp) {
            g_mbw_target = (void *)real_MessageBoxW;
            if (install_inline_hook(g_mbw_target, (void *)hook_MessageBoxW, 5, &g_mbw_tramp,
                                    g_mbw_saved)) {
                real_MessageBoxW = (PFN_MessageBoxW)g_mbw_tramp;
                log_msg("inline hooked user32!MessageBoxW (CVM suppress)");
            } else {
                log_msg("WARN: MessageBoxW inline hook failed");
            }
        }
        log_ini_resolution_prefs();
        {
            int cx = GetSystemMetrics(SM_CXSCREEN);
            int cy = GetSystemMetrics(SM_CYSCREEN);
            log_msg("res-trace desktop %dx%d", cx, cy);
        }
        /* Optional: force display mode at boot (CK_FORCE_RES=1). */
        if (g_force_res && real_ChangeDisplaySettingsA) {
            DEVMODEA dm;
            LONG rc;
            memset(&dm, 0, sizeof(dm));
            dm.dmSize = (WORD)sizeof(dm);
            dm.dmPelsWidth = CK_FORCE_W;
            dm.dmPelsHeight = CK_FORCE_H;
            dm.dmBitsPerPel = 32;
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
            rc = real_ChangeDisplaySettingsA(&dm, 0);
            log_msg("boot Force ChangeDisplaySettings %dx%d => %ld", CK_FORCE_W, CK_FORCE_H,
                    (long)rc);
        } else {
            log_msg("boot Force ChangeDisplaySettings skipped (CK_FORCE_RES off)");
        }
        /* ConfigureVideoMode: CK1 @ 0x6BFF90, I2/tpw @ 0x6EE690 — both sub esp,0x814 */
        {
            static const BYTE sig_cvm[] = {0x81, 0xEC, 0x14, 0x08, 0x00, 0x00};
            static const ULONG_PTR cvm_cands[] = {0x006EE690u, 0x006BFF90u};
            HMODULE game_mod = GetModuleHandleA(NULL);
            BYTE *cand = NULL;
            size_t ci;
            for (ci = 0; ci < sizeof(cvm_cands) / sizeof(cvm_cands[0]); ++ci) {
                BYTE *p = (BYTE *)cvm_cands[ci];
                if (game_mod && mem_eq(p, sig_cvm, sizeof(sig_cvm))) {
                    cand = p;
                    break;
                }
            }
            if (cand) {
                g_cvm_target = cand;
                g_cvm_steal = 6;
                if (install_inline_hook(g_cvm_target, (void *)hook_ConfigureVideoMode, g_cvm_steal,
                                        &g_cvm_tramp, g_cvm_saved)) {
                    real_ConfigureVideoMode = (PFN_ConfigureVideoMode)g_cvm_tramp;
                    log_msg("inline hooked ConfigureVideoMode @ %p (force %dx%d)", g_cvm_target,
                            CK_FORCE_W, CK_FORCE_H);
                    install_scanline_oob_guards();
                    /* #region agent log */
                    {
                        char js[80];
                        snprintf(js, sizeof(js), "{\"addr\":%lu}", (unsigned long)(ULONG_PTR)cand);
                        hooks_agent("H96", "hooks.c:CVM", "cvm-hooked", js);
                    }
                    /* #endregion */
                } else {
                    log_msg("WARN: ConfigureVideoMode hook failed");
                }
            } else {
                log_msg("WARN: ConfigureVideoMode signature mismatch (tried 0x6EE690, 0x6BFF90)");
            }
        }

}

void hooks_video_movies_install(void)
{
    /* Cutscenes: Celtic_Kings @ 45F520/45E950; tpw Splash @ 45F910, CMovieWnd @ 4600F0. */
    {
        static const BYTE sig_path[6] = {0x8B, 0x0D, 0x90, 0x21, 0x8C, 0x00};
        static const BYTE sig_splash[6] = {0x81, 0xEC, 0x38, 0x02, 0x00, 0x00};
        void *path_site = (void *)(ULONG_PTR)0x0045F520;
        void *splash_ck = (void *)(ULONG_PTR)0x0045E950;
        void *splash_tpw = (void *)(ULONG_PTR)0x0045F910;
        void *moviewnd_tpw = (void *)(ULONG_PTR)0x004600F0;
        void *splash_site = NULL;

        if (mem_eq(path_site, sig_path, sizeof(sig_path))) {
            g_movie_path_site = path_site;
            g_movie_path_steal = 6;
            if (install_inline_hook(path_site, (void *)hook_PlayMoviePath, 6, &g_movie_path_tramp,
                                    g_movie_path_saved)) {
                real_PlayMoviePath = (PFN_PlayMoviePath)g_movie_path_tramp;
                log_msg("movie: hooked PlayMoviePath @ %p", path_site);
            } else
                log_msg("movie: WARN PlayMoviePath hook failed");
        } else
            log_msg("movie: PlayMoviePath sig mismatch (skip)");

        if (mem_eq(splash_ck, sig_splash, sizeof(sig_splash)))
            splash_site = splash_ck;
        else if (mem_eq(splash_tpw, sig_splash, sizeof(sig_splash)))
            splash_site = splash_tpw;

        if (splash_site) {
            g_splash_pm_site = splash_site;
            g_splash_pm_steal = 6;
            if (install_inline_hook(splash_site, (void *)hook_SplashPlayMovie, 6, &g_splash_pm_tramp,
                                    g_splash_pm_saved)) {
                real_SplashPlayMovie = (PFN_SplashPlayMovie)g_splash_pm_tramp;
                log_msg("movie: hooked SplashPlayMovie @ %p", splash_site);
            } else
                log_msg("movie: WARN SplashPlayMovie hook failed");
        } else
            log_msg("movie: SplashPlayMovie sig mismatch (skip)");

        if (mem_eq(moviewnd_tpw, sig_splash, sizeof(sig_splash))) {
            g_moviewnd_site = moviewnd_tpw;
            g_moviewnd_steal = 6;
            if (install_inline_hook(moviewnd_tpw, (void *)hook_MovieWndPlay, 6, &g_moviewnd_tramp,
                                    g_moviewnd_saved)) {
                real_MovieWndPlay = (PFN_MovieWndPlay)g_moviewnd_tramp;
                log_msg("movie: hooked CMovieWnd::PlayMovie @ %p (tpw)", moviewnd_tpw);
            } else
                log_msg("movie: WARN CMovieWnd::PlayMovie hook failed");
        }
    }
}

void hooks_video_remove(void)
{
    restore_iat_slot(g_iat_SetDIBitsToDevice, (void *)real_SetDIBitsToDevice);
    restore_iat_slot(g_iat_EnumDisplaySettingsA, (void *)real_EnumDisplaySettingsA);
    restore_iat_slot(g_iat_ChangeDisplaySettingsA, (void *)real_ChangeDisplaySettingsA);
    restore_iat_slot(g_iat_SetWindowPos, (void *)real_SetWindowPos);
    restore_iat_slot(g_iat_GetClientRect, (void *)real_GetClientRect);
    restore_iat_slot(g_iat_ScreenToClient, (void *)real_ScreenToClient);
    restore_iat_slot(g_iat_ClientToScreen, (void *)real_ClientToScreen);
    if (g_mba_target && g_mba_tramp)
        remove_inline_hook(g_mba_target, 5, g_mba_saved, g_mba_tramp);
    if (g_mbw_target && g_mbw_tramp)
        remove_inline_hook(g_mbw_target, 5, g_mbw_saved, g_mbw_tramp);
    remove_inline_hook(g_cvm_target, g_cvm_steal, g_cvm_saved, g_cvm_tramp);
    g_cvm_tramp = NULL;
    remove_inline_hook(g_movie_path_site, g_movie_path_steal, g_movie_path_saved, g_movie_path_tramp);
    g_movie_path_tramp = NULL;
    remove_inline_hook(g_splash_pm_site, g_splash_pm_steal, g_splash_pm_saved, g_splash_pm_tramp);
    g_splash_pm_tramp = NULL;
    remove_inline_hook(g_moviewnd_site, g_moviewnd_steal, g_moviewnd_saved, g_moviewnd_tramp);
    g_moviewnd_tramp = NULL;

}
