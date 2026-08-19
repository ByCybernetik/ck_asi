/*
 * Cutscene player: winegstreamer decode via FilterGraph RenderFile.
 * Primary: hook quartz IAT StretchDIBits → Vulkan present.
 * Fallback: visible IVideoWindow (GDI) if hook/present fails.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "movie_player.h"
#include "vk_present.h"
#include "dm_replace.h"


static volatile LONG g_movie_active;
static volatile LONG g_sdib_capture; /* 1 only during StretchDIBits→Vulkan path */
static HMODULE g_wg_mod;

/* Latest frame from StretchDIBits (streaming thread → main). */
static CRITICAL_SECTION g_frame_cs;
static int g_frame_cs_init;
static BYTE *g_frame_bgra;
static int g_frame_w, g_frame_h;
static volatile LONG g_frame_seq;
static volatile LONG g_frame_cb_count;
static volatile LONG g_sdib_hook_hits;

typedef int(WINAPI *StretchDIBits_fn)(HDC, int, int, int, int, int, int, int, int, const VOID *,
                                      const BITMAPINFO *, UINT, DWORD);

static StretchDIBits_fn g_real_StretchDIBits;
static void *g_sdib_iat_slot;
static int g_sdib_hooked;

int ck_movie_active(void)
{
    return g_movie_active != 0;
}

static void ensure_frame_cs(void)
{
    if (!g_frame_cs_init) {
        InitializeCriticalSection(&g_frame_cs);
        g_frame_cs_init = 1;
    }
}

static int file_exists_a(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void movie_basename(const char *path, char *leaf, size_t leaf_sz)
{
    const char *s = path;
    const char *a = strrchr(path, '\\');
    const char *b = strrchr(path, '/');
    if (a && (!b || a > b))
        s = a + 1;
    else if (b)
        s = b + 1;
    strncpy(leaf, s, leaf_sz - 1);
    leaf[leaf_sz - 1] = 0;
}

/* Intro3 on I2 ships as Movies\webm\Intro3.webm only (no .avi). */
static void movie_webm_name(const char *leaf, char *out, size_t out_sz)
{
    char base[MAX_PATH];
    char *dot;
    strncpy(base, leaf, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    dot = strrchr(base, '.');
    if (dot)
        *dot = 0;
    snprintf(out, out_sz, "%s.webm", base);
}

static int resolve_movie_path(const char *in, char *out, size_t out_sz)
{
    char tmp[MAX_PATH];
    char leaf[MAX_PATH];
    char webm[MAX_PATH];
    const char *p;
    size_t n;
    static const char *const dirs[] = {"Movies\\avi\\", "Movies\\", "movies\\", "Movies\\webm\\",
                                       "movies\\webm\\"};
    size_t i;

    if (!in || !out || out_sz < 8)
        return 0;
    while (*in == ' ' || *in == '\t')
        in++;

    n = 0;
    for (p = in; *p && n + 1 < sizeof(tmp); ++p)
        tmp[n++] = (*p == '/') ? '\\' : *p;
    tmp[n] = 0;

    if (file_exists_a(tmp)) {
        strncpy(out, tmp, out_sz - 1);
        out[out_sz - 1] = 0;
        return 1;
    }

    /* Absolute/relative miss: always use basename (not the whole Z:\...\Movies\...). */
    movie_basename(tmp, leaf, sizeof(leaf));
    if (!leaf[0])
        return 0;
    movie_webm_name(leaf, webm, sizeof(webm));

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        snprintf(out, out_sz, "%s%s", dirs[i], leaf);
        if (file_exists_a(out))
            return 1;
        snprintf(out, out_sz, "%s%s", dirs[i], webm);
        if (file_exists_a(out))
            return 1;
    }

    /* Same dir as requested absolute path, .webm sibling. */
    {
        char *slash = strrchr(tmp, '\\');
        if (slash) {
            size_t dir_len = (size_t)(slash - tmp + 1);
            if (dir_len + strlen(webm) + 1 < out_sz) {
                memcpy(out, tmp, dir_len);
                memcpy(out + dir_len, webm, strlen(webm) + 1);
                if (file_exists_a(out))
                    return 1;
            }
            if (dir_len + 5 + strlen(webm) + 1 < out_sz) {
                memcpy(out, tmp, dir_len);
                memcpy(out + dir_len, "webm\\", 5);
                memcpy(out + dir_len + 5, webm, strlen(webm) + 1);
                if (file_exists_a(out))
                    return 1;
            }
        }
    }

    strncpy(out, tmp, out_sz - 1);
    out[out_sz - 1] = 0;
    return 0;
}

static HWND find_game_hwnd(void)
{
    HWND h = vk_present_hwnd();
    if (h)
        return h;
    h = GetForegroundWindow();
    if (h)
        return h;
    return GetActiveWindow();
}

static int user_wants_skip(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return 1;
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN))
            return 1;
        if (msg.message == WM_LBUTTONDOWN)
            return 1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        return 1;
    return 0;
}

static BOOL patch_iat_entry(HMODULE mod, const char *dll, const char *func, void *hook,
                            void **orig_out, void **slot_out)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD imp_rva;

    if (!base || base == (BYTE *)(ULONG_PTR)-1)
        return FALSE;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return FALSE;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return FALSE;
    imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva)
        return FALSE;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp_rva); imp->Name; ++imp) {
        const char *name = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *oft;
        IMAGE_THUNK_DATA *ft;
        DWORD oldprot;

        if (lstrcmpiA(name, dll) != 0)
            continue;
        oft = (IMAGE_THUNK_DATA *)(base +
                                  (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            const char *fname;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))
                continue;
            fname = (const char *)(base + oft->u1.AddressOfData + 2);
            if (lstrcmpA(fname, func) != 0)
                continue;
            if (orig_out && !*orig_out)
                *orig_out = (void *)(ULONG_PTR)ft->u1.Function;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &oldprot))
                return FALSE;
            ft->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void *), oldprot, &oldprot);
            if (slot_out)
                *slot_out = &ft->u1.Function;
            return TRUE;
        }
    }
    return FALSE;
}

static void store_frame_bgra(const BYTE *src, int w, int h, int bpp, int top_down)
{
    int stride, y, x;
    size_t need;
    BYTE *dst;

    if (!src || w <= 0 || h <= 0)
        return;
    if (bpp != 24 && bpp != 32 && bpp != 16)
        return;
    stride = ((w * bpp + 31) / 32) * 4;
    need = (size_t)w * (size_t)h * 4u;

    ensure_frame_cs();
    EnterCriticalSection(&g_frame_cs);
    if (!g_frame_bgra || g_frame_w != w || g_frame_h != h) {
        free(g_frame_bgra);
        g_frame_bgra = (BYTE *)malloc(need);
        g_frame_w = w;
        g_frame_h = h;
    }
    dst = g_frame_bgra;
    if (dst) {
        for (y = 0; y < h; ++y) {
            int src_y = top_down ? y : (h - 1 - y);
            const BYTE *srow = src + (size_t)src_y * (size_t)stride;
            BYTE *row = dst + (size_t)y * (size_t)w * 4u;
            if (bpp == 32) {
                for (x = 0; x < w; ++x) {
                    const BYTE *s = srow + (size_t)x * 4u;
                    row[x * 4 + 0] = s[0];
                    row[x * 4 + 1] = s[1];
                    row[x * 4 + 2] = s[2];
                    row[x * 4 + 3] = 255;
                }
            } else if (bpp == 24) {
                for (x = 0; x < w; ++x) {
                    const BYTE *s = srow + (size_t)x * 3u;
                    row[x * 4 + 0] = s[0];
                    row[x * 4 + 1] = s[1];
                    row[x * 4 + 2] = s[2];
                    row[x * 4 + 3] = 255;
                }
            } else { /* RGB565 */
                for (x = 0; x < w; ++x) {
                    WORD p = *(const WORD *)(srow + (size_t)x * 2u);
                    row[x * 4 + 0] = (BYTE)((p & 0x001F) << 3);
                    row[x * 4 + 1] = (BYTE)((p & 0x07E0) >> 3);
                    row[x * 4 + 2] = (BYTE)((p & 0xF800) >> 8);
                    row[x * 4 + 3] = 255;
                }
            }
        }
        InterlockedIncrement(&g_frame_seq);
        InterlockedIncrement(&g_frame_cb_count);
    }
    LeaveCriticalSection(&g_frame_cs);
}

static int WINAPI hook_StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight,
                                     int xSrc, int ySrc, int SrcWidth, int SrcHeight,
                                     const VOID *lpBits, const BITMAPINFO *lpbmi, UINT iUsage,
                                     DWORD rop)
{
    InterlockedIncrement(&g_sdib_hook_hits);

    if (g_sdib_capture && lpBits && lpbmi) {
        const BITMAPINFOHEADER *bih = &lpbmi->bmiHeader;
        int w = bih->biWidth;
        int h = bih->biHeight;
        int top_down = 0;
        int bpp = bih->biBitCount;

        if (h < 0) {
            h = -h;
            top_down = 1;
        }
        if (SrcWidth > 0 && SrcWidth < w)
            w = SrcWidth;
        if (SrcHeight > 0 && SrcHeight < h)
            h = SrcHeight;
        (void)xSrc;
        (void)ySrc;
        store_frame_bgra((const BYTE *)lpBits, w, h, bpp, top_down);
        /* Skip GDI blit so VR child does not cover Vulkan swapchain. */
        return DestHeight;
    }

    if (g_real_StretchDIBits)
        return g_real_StretchDIBits(hdc, xDest, yDest, DestWidth, DestHeight, xSrc, ySrc, SrcWidth,
                                    SrcHeight, lpBits, lpbmi, iUsage, rop);
    return 0;
}

static int ensure_sdib_hook(void)
{
    HMODULE quartz;
    void *orig = NULL;

    if (g_sdib_hooked)
        return 1;

    quartz = GetModuleHandleA("quartz.dll");
    if (!quartz)
        quartz = LoadLibraryA("quartz.dll");
    if (!quartz) {
        log_msg("movie: quartz.dll not loaded for StretchDIBits hook");
        return 0;
    }

    if (!patch_iat_entry(quartz, "GDI32.dll", "StretchDIBits", (void *)hook_StretchDIBits, &orig,
                         &g_sdib_iat_slot) &&
        !patch_iat_entry(quartz, "gdi32.dll", "StretchDIBits", (void *)hook_StretchDIBits, &orig,
                         &g_sdib_iat_slot)) {
        log_msg("movie: failed to IAT-hook quartz!StretchDIBits");
        return 0;
    }

    g_real_StretchDIBits = (StretchDIBits_fn)orig;
    if (!g_real_StretchDIBits)
        g_real_StretchDIBits =
            (StretchDIBits_fn)GetProcAddress(GetModuleHandleA("gdi32.dll"), "StretchDIBits");
    g_sdib_hooked = 1;
    log_msg("movie: hooked quartz IAT StretchDIBits → Vulkan path");
    return 1;
}

static int present_latest_frame(HWND hwnd)
{
    BYTE *copy = NULL;
    int w, h;
    size_t nbytes;

    ensure_frame_cs();
    EnterCriticalSection(&g_frame_cs);
    w = g_frame_w;
    h = g_frame_h;
    if (g_frame_bgra && w > 0 && h > 0) {
        nbytes = (size_t)w * (size_t)h * 4u;
        copy = (BYTE *)malloc(nbytes);
        if (copy)
            memcpy(copy, g_frame_bgra, nbytes);
    }
    LeaveCriticalSection(&g_frame_cs);
    if (!copy)
        return 0;
    {
        int ok = vk_present_movie_frame(hwnd, copy, w, h);
        free(copy);
        return ok;
    }
}

static void free_frame_buf(void)
{
    ensure_frame_cs();
    EnterCriticalSection(&g_frame_cs);
    free(g_frame_bgra);
    g_frame_bgra = NULL;
    g_frame_w = g_frame_h = 0;
    LeaveCriticalSection(&g_frame_cs);
}

static int play_pump(IMediaControl *control, IMediaEvent *event, HWND hwnd, int vulkan_frames)
{
    int frames = 0;
    LONG last_seq = 0;

    for (;;) {
        LONG ev = 0, p1 = 0, p2 = 0;

        if (user_wants_skip()) {
            log_msg("movie: skip requested after %d frames", frames);
            break;
        }
        if (event) {
            while (SUCCEEDED(IMediaEvent_GetEvent(event, &ev, &p1, &p2, 0))) {
                IMediaEvent_FreeEventParams(event, ev, p1, p2);
                if (ev == EC_COMPLETE || ev == EC_USERABORT || ev == EC_ERRORABORT)
                    return frames;
            }
        }
        if (vulkan_frames) {
            LONG seq = g_frame_seq;
            if (seq != last_seq) {
                if (present_latest_frame(hwnd))
                    frames++;
                last_seq = seq;
            }
        }
        Sleep(8);
    }
    return frames;
}

/* Primary: RenderFile + StretchDIBits capture → Vulkan (swapchain kept). */
static int play_vulkan_sdib(const char *path_a, HWND hwnd)
{
    HRESULT hr;
    IGraphBuilder *graph = NULL;
    IMediaControl *control = NULL;
    IMediaEvent *event = NULL;
    IVideoWindow *vwin = NULL;
    WCHAR path_w[MAX_PATH];
    int frames = 0;
    int ok = 0;

    if (!ensure_sdib_hook())
        return 0;

    if (!g_wg_mod)
        g_wg_mod = LoadLibraryA("winegstreamer.dll");

    MultiByteToWideChar(CP_ACP, 0, path_a, -1, path_w, MAX_PATH);
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return 0;

    hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder,
                          (void **)&graph);
    if (FAILED(hr))
        return 0;
    hr = IGraphBuilder_RenderFile(graph, path_w, NULL);
    if (FAILED(hr)) {
        log_msg("movie: RenderFile sdib failed 0x%lx", (unsigned long)hr);
        IGraphBuilder_Release(graph);
        return 0;
    }
    IGraphBuilder_QueryInterface(graph, &IID_IMediaControl, (void **)&control);
    IGraphBuilder_QueryInterface(graph, &IID_IMediaEvent, (void **)&event);
    IGraphBuilder_QueryInterface(graph, &IID_IVideoWindow, (void **)&vwin);
    if (!control)
        goto done;

    /* Keep VR window alive for StretchDIBits, but off-screen so it cannot cover Vulkan. */
    if (vwin) {
        IVideoWindow_put_AutoShow(vwin, OAFALSE);
        IVideoWindow_put_Visible(vwin, OAFALSE);
        IVideoWindow_put_Owner(vwin, (OAHWND)(ULONG_PTR)hwnd);
        IVideoWindow_SetWindowPosition(vwin, -32000, -32000, 16, 16);
    }

    InterlockedExchange(&g_frame_seq, 0);
    InterlockedExchange(&g_frame_cb_count, 0);
    InterlockedExchange(&g_sdib_hook_hits, 0);
    InterlockedExchange(&g_sdib_capture, 1);

    hr = IMediaControl_Run(control);
    if (FAILED(hr)) {
        InterlockedExchange(&g_sdib_capture, 0);
        log_msg("movie: sdib Run failed 0x%lx", (unsigned long)hr);
        goto done;
    }

    log_msg("movie: playing %s via StretchDIBits → Vulkan", path_a);

    frames = play_pump(control, event, hwnd, 1);
    IMediaControl_Stop(control);
    InterlockedExchange(&g_sdib_capture, 0);
    if (vwin) {
        IVideoWindow_put_Visible(vwin, OAFALSE);
        IVideoWindow_put_Owner(vwin, (OAHWND)0);
    }

    ok = (frames > 0) ? 1 : 0;
    log_msg("movie: sdib-vk done frames=%d cbs=%ld hits=%ld", frames, (long)g_frame_cb_count,
            (long)g_sdib_hook_hits);

done:
    free_frame_buf();
    if (vwin)
        IVideoWindow_Release(vwin);
    if (event)
        IMediaEvent_Release(event);
    if (control)
        IMediaControl_Release(control);
    if (graph)
        IGraphBuilder_Release(graph);
    return ok;
}

static void movie_agent(const char *hid, const char *msg, const char *data_json)
{
    (void)hid;
    (void)msg;
    (void)data_json;
}

static int path_is_webm(const char *p)
{
    size_t n;
    if (!p)
        return 0;
    n = strlen(p);
    return n >= 5 && _stricmp(p + n - 5, ".webm") == 0;
}

static void path_absolutize(char *path, size_t path_sz)
{
    char full[MAX_PATH];
    DWORD n;
    if (!path || !path[0])
        return;
    n = GetFullPathNameA(path, MAX_PATH, full, NULL);
    if (n > 0 && n < MAX_PATH) {
        strncpy(path, full, path_sz - 1);
        path[path_sz - 1] = 0;
    }
}

/* Fallback / webm primary: visible IVideoWindow (winegstreamer VP9 does not StretchDIBits). */
static int play_vwin_fallback(const char *path_a, HWND hwnd)
{
    HRESULT hr;
    IGraphBuilder *graph = NULL;
    IMediaControl *control = NULL;
    IMediaEvent *event = NULL;
    IVideoWindow *vwin = NULL;
    WCHAR path_w[MAX_PATH];
    int ok = 0;
    DWORD t0 = GetTickCount();

    MultiByteToWideChar(CP_ACP, 0, path_a, -1, path_w, MAX_PATH);
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return 0;

    hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder,
                          (void **)&graph);
    if (FAILED(hr))
        return 0;
    hr = IGraphBuilder_RenderFile(graph, path_w, NULL);
    if (FAILED(hr)) {
        log_msg("movie: RenderFile VWin failed 0x%lx path=%s", (unsigned long)hr, path_a);
        /* #region agent log */
        {
            char js[240];
            snprintf(js, sizeof(js), "{\"hr\":%ld,\"webm\":%d}", (long)hr, path_is_webm(path_a));
            movie_agent("H87", "movie-vwin-render-fail", js);
        }
        /* #endregion */
        IGraphBuilder_Release(graph);
        return 0;
    }
    IGraphBuilder_QueryInterface(graph, &IID_IMediaControl, (void **)&control);
    IGraphBuilder_QueryInterface(graph, &IID_IMediaEvent, (void **)&event);
    IGraphBuilder_QueryInterface(graph, &IID_IVideoWindow, (void **)&vwin);
    if (!control)
        goto done;

    vk_present_movie_begin(hwnd);
    if (vwin && hwnd) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        IVideoWindow_put_Owner(vwin, (OAHWND)(ULONG_PTR)hwnd);
        IVideoWindow_put_MessageDrain(vwin, (OAHWND)(ULONG_PTR)hwnd);
        IVideoWindow_put_WindowStyle(vwin, WS_CHILD | WS_CLIPSIBLINGS);
        IVideoWindow_SetWindowPosition(vwin, 0, 0, rc.right - rc.left, rc.bottom - rc.top);
        IVideoWindow_put_AutoShow(vwin, OATRUE);
        IVideoWindow_put_Visible(vwin, OATRUE);
    }

    hr = IMediaControl_Run(control);
    if (FAILED(hr)) {
        log_msg("movie: VWin Run failed 0x%lx", (unsigned long)hr);
        vk_present_movie_end();
        goto done;
    }
    log_msg("movie: IVideoWindow play %s", path_a);
    play_pump(control, event, hwnd, 0);
    IMediaControl_Stop(control);
    if (vwin) {
        IVideoWindow_put_Visible(vwin, OAFALSE);
        IVideoWindow_put_Owner(vwin, (OAHWND)0);
    }
    vk_present_movie_end();
    ok = 1;
    /* #region agent log */
    {
        char js[280];
        snprintf(js, sizeof(js), "{\"ok\":1,\"webm\":%d,\"ms\":%lu,\"path\":\"%.160s\"}",
                 path_is_webm(path_a), (unsigned long)(GetTickCount() - t0), path_a);
        movie_agent("H87", "movie-vwin-ok", js);
    }
    /* #endregion */

done:
    if (vwin)
        IVideoWindow_Release(vwin);
    if (event)
        IMediaEvent_Release(event);
    if (control)
        IMediaControl_Release(control);
    if (graph)
        IGraphBuilder_Release(graph);
    return ok;
}

static int play_with_filtergraph(const char *path_a, HWND hwnd)
{
    char abs[MAX_PATH];

    strncpy(abs, path_a, sizeof(abs) - 1);
    abs[sizeof(abs) - 1] = 0;
    path_absolutize(abs, sizeof(abs));

    /* VP9/webm: own libav decode daemon → Vulkan (not Wine ddraw / StretchDIBits). */
    if (path_is_webm(abs)) {
        if (play_webm_own_vulkan(abs, hwnd))
            return 1;
        log_msg("movie: webm own-decode failed for %s", abs);
        return 0;
    }

    if (play_vulkan_sdib(abs, hwnd))
        return 1;
    log_msg("movie: StretchDIBits→Vulkan failed — IVideoWindow fallback");
    return play_vwin_fallback(abs, hwnd);
}

int ck_movie_play(const char *path)
{
    char resolved[MAX_PATH];
    HWND hwnd;
    int rc;

    if (!path || !path[0])
        return 0;

    InterlockedExchange(&g_movie_active, 1);
    hwnd = find_game_hwnd();

    if (!resolve_movie_path(path, resolved, sizeof(resolved))) {
        log_msg("movie: path resolve failed for '%s'", path);
        InterlockedExchange(&g_movie_active, 0);
        return 0;
    }
    log_msg("movie: resolve '%s' -> '%s' hwnd=%p", path, resolved, (void *)hwnd);
    dm_replace_silence_music();

    rc = play_with_filtergraph(resolved, hwnd);
    if (!rc) {
        char leaf[MAX_PATH];
        const char *p = strrchr(resolved, '\\');
        strncpy(leaf, p ? p + 1 : resolved, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = 0;
        if (_strnicmp(resolved, "Movies\\avi\\", 11) != 0) {
            snprintf(resolved, sizeof(resolved), "Movies\\avi\\%s", leaf);
            if (file_exists_a(resolved)) {
                log_msg("movie: retry avi fallback %s", resolved);
                rc = play_with_filtergraph(resolved, hwnd);
            }
        }
    }

    InterlockedExchange(&g_movie_active, 0);
    vk_present_restore_game_soft();
    return rc;
}

int ck_movie_resolve(const char *in, char *out, size_t out_sz)
{
    if (!resolve_movie_path(in, out, out_sz))
        return 0;
    path_absolutize(out, out_sz);
    return file_exists_a(out);
}

int ck_movie_is_webm_path(const char *path)
{
    return path_is_webm(path);
}
