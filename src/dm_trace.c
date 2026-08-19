#include "dm_trace.h"
#include "hooks_internal.h"
#include "log.h"

#include <windows.h>
#include <objbase.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/*
 * Native DirectMusic vtable tracer — gather a replace contract.
 * Never call game IStream::Read (custom thiscall / non-stdcall).
 * Paths: scrape from stream object (+8 etc), not via COM Read.
 */


static const char *g_dm_run_id = "dm";

static void dm_agent(const char *hid, const char *loc, const char *msg, const char *data_json)
{
    (void)hid;
    (void)loc;
    (void)msg;
    (void)data_json;
}

static const GUID CLSID_DMPerformance = {
    0xd2ac2881, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}};
static const GUID CLSID_DMLoader = {
    0xd2ac2892, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}};
static const GUID k_CLSID_DirectSound = {
    0x47d4d946, 0x62e8, 0x11cf, {0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID GUID_PerfAutoDownload = {
    0xfb09565b, 0x3631, 0x11d2, {0xbc, 0xb8, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb}};

enum {
    PERF_VT_COUNT = 53,
    PERF_IDX_PLAYSEG = 4,      /* +0x10 PlaySegment */
    PERF_IDX_FREEPMSG = 17,    /* +0x44 */
    PERF_IDX_SETNOTIF = 20,    /* +0x50 */
    PERF_IDX_GETNOTIF = 21,    /* +0x54 */
    PERF_IDX_ADDNOTIF = 22,    /* +0x58 */
    PERF_IDX_SETGLOBAL = 34,   /* +0x88 */
    PERF_IDX_INITAUDIO = 44,   /* +0xB0 */
    PERF_IDX_PLAYSEGEX = 45,   /* +0xB4 */
    PERF_IDX_CREATESTD = 49,   /* +0xC4 */
    /* Game notification thread reads these offsets (RE @ 0x40C860): */
    NOTIF_OFF_TYPE = 0x28,
    NOTIF_OFF_PUNK = 0x34,
    NOTIF_OFF_GUID = 0x38,
    NOTIF_OFF_OPT = 0x48,
    LDR_VT_COUNT = 15,
    LDR_IDX_GETOBJECT = 3, /* +0x0C */
    SEG_VT_COUNT = 31,
    SEG_IDX_SETREPEATS = 6, /* +0x18 */
    SEG_IDX_DOWNLOAD = 29,  /* +0x74 */
    SEG_IDX_UNLOAD = 30,    /* +0x78 */
};

typedef HRESULT(STDMETHODCALLTYPE *PFN_InitAudio)(void *This, void **ppDM, void **ppDS, HWND hwnd,
                                                   DWORD pathType, DWORD pchannels, DWORD flags,
                                                   void *params);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PlaySegmentEx)(void *This, void *source, WCHAR *name,
                                                     void *transition, DWORD flags, LONGLONG start,
                                                     void **ppState, void *from, void *audiopath);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PlaySegment)(void *This, void *seg, DWORD flags, LONGLONG start,
                                                    void **ppState);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateStdPath)(void *This, DWORD type, DWORD pchannels,
                                                     DWORD flags, void **ppPath);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetGlobalParam)(void *This, REFGUID rguidType, void *pData,
                                                      DWORD dwSize);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GetObject)(void *This, void *pDesc, REFIID riid, void **ppv);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Download)(void *This, void *pAudioPath);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetRepeats)(void *This, DWORD n);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GetNotif)(void *This, void **ppMsg);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FreePMsg)(void *This, void *pMsg);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetNotifHandle)(void *This, HANDLE h, LONGLONG rt);
typedef HRESULT(STDMETHODCALLTYPE *PFN_AddNotifType)(void *This, REFGUID g);

static PFN_InitAudio real_InitAudio;
static PFN_PlaySegmentEx real_PlaySegmentEx;
static PFN_PlaySegment real_PlaySegment;
static PFN_CreateStdPath real_CreateStdPath;
static PFN_SetGlobalParam real_SetGlobalParam;
static PFN_GetObject real_GetObject;
static PFN_Download real_Download;
static PFN_Download real_Unload;
static PFN_SetRepeats real_SetRepeats;
static PFN_GetNotif real_GetNotif;
static PFN_FreePMsg real_FreePMsg;
static PFN_SetNotifHandle real_SetNotifHandle;
static PFN_AddNotifType real_AddNotifType;

static void **g_perf_vt_template;
static void **g_ldr_vt_template;
static void **g_seg_vt_template;
static void **g_seg_vt_orig;
static volatile LONG g_play_n;
static volatile LONG g_play_old_n;
static volatile LONG g_getobj_n;
static volatile LONG g_dl_n;
static volatile LONG g_ul_n;
static volatile LONG g_init_n;
static volatile LONG g_path_n;
static volatile LONG g_rep_n;
static volatile LONG g_notif_n;
static volatile LONG g_notif_hit_n;
static volatile LONG g_free_notif_n;

static const GUID k_GUID_NOTIFICATION_SEGMENT = {
    0xd2ac2899, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}};

/* #region agent log — parse native DMUS_NOTIFICATION_PMSG (game layout) */
static int path_is_selection(const char *path)
{
    return path && (strstr(path, "selection/") || strstr(path, "selection\\"));
}

static void notif_log_fields(const char *tag, const char *hid, void *msg, const char *ctx_file)
{
    BYTE *m;
    void *punk;
    DWORD typ, opt;
    GUID g;
    char js[360];

    if (!msg)
        return;
    m = (BYTE *)msg;
    typ = *(DWORD *)(m + NOTIF_OFF_TYPE);
    punk = *(void **)(m + NOTIF_OFF_PUNK);
    opt = *(DWORD *)(m + NOTIF_OFF_OPT);
    memcpy(&g, m + NOTIF_OFF_GUID, sizeof(g));
    snprintf(js, sizeof(js),
             "{\"tag\":\"%.16s\",\"ctx\":\"%.48s\",\"type\":%lu,\"opt\":%lu,"
             "\"punk\":%lu,\"guid\":\"%08lX\",\"st4\":%lu,\"st8\":%lu}",
             tag ? tag : "?", ctx_file ? ctx_file : "",
             (unsigned long)typ, (unsigned long)opt, (unsigned long)(ULONG_PTR)punk,
             (unsigned long)g.Data1,
             (unsigned long)(punk ? (ULONG_PTR) * (void **)((BYTE *)punk + 4) : 0),
             (unsigned long)(punk ? (ULONG_PTR) * (DWORD *)((BYTE *)punk + 8) : 0));
    dm_agent(hid ? hid : "N1", "dm_trace.c:notif", "notif-fields", js);
}
/* #endregion */

enum {
    DMUS_OBJ_CLASS = 0x001,
    DMUS_OBJ_FILENAME = 0x008,
    DMUS_OBJ_FULLPATH = 0x010,
    DMUS_OBJ_URL = 0x020,
    DMUS_OBJ_MEMORY = 0x100,
    DMUS_OBJ_STREAM = 0x800,
    SEG_PATH_N = 256
};

static struct {
    void *seg;
    char path[200];
    DWORD valid;
    WORD ch;
    DWORD rate;
    WORD bits;
    int fmt_ok; /* 1=probed, 0=unknown */
} g_seg_paths[SEG_PATH_N];
static int g_seg_path_i;

static int path_looks_audio(const char *p); /* fwd */

/* Stereo check: DM pch != PCM channels. Probe source WAVE/OGG + DS primary. */
static int open_probe_file(const char *path, HANDLE *out)
{
    char try[512];
    char cwd[260];
    const char *slash;
    size_t i, j;
    HANDLE h;

    if (!path || !path[0] || !out)
        return 0;
    *out = INVALID_HANDLE_VALUE;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        *out = h;
        return 1;
    }

    if (GetCurrentDirectoryA(sizeof(cwd), cwd)) {
        snprintf(try, sizeof(try), "%s\\%s", cwd, path);
        h = CreateFileA(try, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            *out = h;
            return 1;
        }
    }

    /* assets/sounds/foo.wav → assets_unpacked/SOUNDS/FOO.WAV (host via Z:) */
    {
        const char *rel = path;
        const char *pfx[] = {"assets/sounds/", "assets\\sounds\\", "Assets/Sounds/",
                             "Assets\\Sounds\\", NULL};
        for (i = 0; pfx[i]; ++i) {
            size_t n = strlen(pfx[i]);
            if (_strnicmp(path, pfx[i], (unsigned)n) == 0) {
                rel = path + n;
                break;
            }
        }
        /* build UPPER SOUNDS\REL */
        j = 0;
        try[j++] = 'S';
        try[j++] = 'O';
        try[j++] = 'U';
        try[j++] = 'N';
        try[j++] = 'D';
        try[j++] = 'S';
        try[j++] = '\\';
        for (i = 0; rel[i] && j + 2 < sizeof(try); ++i) {
            char c = rel[i];
            if (c == '/')
                c = '\\';
            try[j++] = (char)toupper((unsigned char)c);
        }
        try[j] = '\0';
        {
            static const char *bases[] = {
                "assets_unpacked\\",
                "Z:\\home\\cybernetik\\Games\\Imperivm\\Imperivm\\assets_unpacked\\",
                "/home/cybernetik/Games/Imperivm/Imperivm/assets_unpacked/",
                NULL};
            char full[640];
            for (i = 0; bases[i]; ++i) {
                snprintf(full, sizeof(full), "%s%s", bases[i], try);
                h = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    *out = h;
                    return 1;
                }
            }
        }
    }

    /* music ogg next to exe or cwd */
    slash = strrchr(path, '/');
    if (!slash)
        slash = strrchr(path, '\\');
    if (slash && (_stricmp(slash + 1, "game10.ogg") == 0 || path_looks_audio(path))) {
        snprintf(try, sizeof(try), "music\\%s", slash ? slash + 1 : path);
        h = CreateFileA(try, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            *out = h;
            return 1;
        }
    }
    return 0;
}

static int parse_wav_fmt(const BYTE *buf, DWORD n, WORD *ch, DWORD *rate, WORD *bits)
{
    DWORD off = 12;
    if (n < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
        return 0;
    while (off + 8 <= n) {
        DWORD sz;
        memcpy(&sz, buf + off + 4, 4);
        if (!memcmp(buf + off, "fmt ", 4) && off + 8 + 16 <= n) {
            WORD tag, nch, nb;
            DWORD sr;
            memcpy(&tag, buf + off + 8, 2);
            memcpy(&nch, buf + off + 10, 2);
            memcpy(&sr, buf + off + 12, 4);
            memcpy(&nb, buf + off + 22, 2);
            (void)tag;
            *ch = nch;
            *rate = sr;
            *bits = nb;
            return nch > 0 ? 1 : 0;
        }
        off += 8 + sz + (sz & 1);
    }
    return 0;
}

static int parse_ogg_ident(const BYTE *buf, DWORD n, WORD *ch, DWORD *rate, WORD *bits)
{
    DWORD i;
    for (i = 0; i + 16 < n; ++i) {
        if (buf[i] == 1 && i + 7 < n && !memcmp(buf + i + 1, "vorbis", 6)) {
            DWORD sr;
            *ch = buf[i + 11];
            memcpy(&sr, buf + i + 12, 4);
            *rate = sr;
            *bits = 16; /* decoded PCM width we care about */
            return *ch > 0 ? 1 : 0;
        }
    }
    return 0;
}

static int probe_audio_fmt(const char *path, WORD *ch, DWORD *rate, WORD *bits)
{
    HANDLE h;
    BYTE buf[8192];
    DWORD rd = 0;
    const char *ext;

    *ch = 0;
    *rate = 0;
    *bits = 0;
    if (!open_probe_file(path, &h))
        return 0;
    if (!ReadFile(h, buf, sizeof(buf), &rd, NULL) || rd < 16) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    ext = strrchr(path, '.');
    if (ext && _stricmp(ext, ".ogg") == 0)
        return parse_ogg_ident(buf, rd, ch, rate, bits);
    return parse_wav_fmt(buf, rd, ch, rate, bits);
}

static void ds_log_primary_format(void *pDS)
{
    IDirectSound *ds = (IDirectSound *)pDS;
    IDirectSoundBuffer *prim = NULL;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;
    HRESULT hr;
    char js[320];
    int owned = 0;

    /* Game InitAudio often passes ppDS=NULL; probe default device primary anyway. */
    if (!ds) {
        hr = DirectSoundCreate(NULL, &ds, NULL);
        if (FAILED(hr) || !ds) {
            snprintf(js, sizeof(js), "{\"hrCreateDS\":%ld}", (long)hr);
            dm_agent("S1", "dm_trace.c:ds_primary", "ds-primary-fail", js);
            return;
        }
        owned = 1;
    }
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    hr = IDirectSound_CreateSoundBuffer(ds, &desc, &prim, NULL);
    if (FAILED(hr) || !prim) {
        snprintf(js, sizeof(js), "{\"hrCreate\":%ld,\"owned\":%d}", (long)hr, owned);
        dm_agent("S1", "dm_trace.c:ds_primary", "ds-primary-fail", js);
        if (owned)
            IDirectSound_Release(ds);
        return;
    }
    memset(&wfx, 0, sizeof(wfx));
    wfx.cbSize = sizeof(wfx);
    hr = IDirectSoundBuffer_GetFormat(prim, &wfx, sizeof(wfx), NULL);
    snprintf(js, sizeof(js),
             "{\"hr\":%ld,\"ch\":%u,\"rate\":%lu,\"bits\":%u,\"tag\":%u,\"align\":%u,\"avg\":%lu,"
             "\"owned\":%d,\"stereo\":%d}",
             (long)hr, (unsigned)wfx.nChannels, (unsigned long)wfx.nSamplesPerSec,
             (unsigned)wfx.wBitsPerSample, (unsigned)wfx.wFormatTag, (unsigned)wfx.nBlockAlign,
             (unsigned long)wfx.nAvgBytesPerSec, owned, wfx.nChannels >= 2 ? 1 : 0);
    dm_agent("S1", "dm_trace.c:ds_primary", "ds-primary-fmt", js);
    log_msg("dm-trace: DS primary fmt hr=0x%08lx ch=%u rate=%lu bits=%u stereo=%d owned=%d",
            (unsigned long)hr, (unsigned)wfx.nChannels, (unsigned long)wfx.nSamplesPerSec,
            (unsigned)wfx.wBitsPerSample, wfx.nChannels >= 2 ? 1 : 0, owned);
    IDirectSoundBuffer_Release(prim);
    if (owned)
        IDirectSound_Release(ds);
}

static void guid_str(const GUID *g, char *out, size_t n)
{
    if (!g || n < 40) {
        if (n)
            out[0] = '\0';
        return;
    }
    snprintf(out, n, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned)g->Data1,
             (unsigned)g->Data2, (unsigned)g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
             g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static void json_esc(const char *in, char *out, size_t n)
{
    size_t i = 0, j = 0;
    if (!out || !n)
        return;
    out[0] = '\0';
    if (!in)
        return;
    for (; in[i] && j + 2 < n; ++i) {
        char c = in[i];
        if (c == '\\' || c == '"') {
            if (j + 3 >= n)
                break;
            out[j++] = '\\';
            out[j++] = c;
        } else if ((unsigned char)c < 32) {
            out[j++] = '?';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static int path_looks_audio(const char *p)
{
    size_t n, i;
    const char *ext;
    if (!p || (unsigned char)p[0] < 32)
        return 0;
    n = strlen(p);
    if (n < 5 || n > 240)
        return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < 32 || c == 0x7f)
            return 0;
    }
    ext = strrchr(p, '.');
    if (!ext)
        return 0;
    return lstrcmpiA(ext, ".wav") == 0 || lstrcmpiA(ext, ".ogg") == 0 || lstrcmpiA(ext, ".sgt") == 0;
}

static int scrape_stream_path(void *stream, char *out, size_t outn)
{
    static const int offs[] = {8, 12, 16, 20, 24, 32, 64, 128, -1};
    int i;
    if (!stream || !out || outn < 8)
        return 0;
    for (i = 0; offs[i] >= 0; ++i) {
        const char *p = (const char *)stream + offs[i];
        if (IsBadReadPtr(p, 64))
            continue;
        if (path_looks_audio(p)) {
            size_t j;
            for (j = 0; j + 1 < outn && p[j] && (unsigned char)p[j] >= 32; ++j)
                out[j] = p[j];
            out[j] = '\0';
            return 1;
        }
    }
    return 0;
}

static void seg_path_put(void *seg, const char *path, DWORD valid, WORD ch, DWORD rate, WORD bits,
                         int fmt_ok)
{
    int i;
    if (!seg)
        return;
    for (i = 0; i < SEG_PATH_N; ++i) {
        if (g_seg_paths[i].seg == seg) {
            if (path && path[0])
                strncpy(g_seg_paths[i].path, path, sizeof(g_seg_paths[i].path) - 1);
            g_seg_paths[i].valid = valid;
            if (fmt_ok) {
                g_seg_paths[i].ch = ch;
                g_seg_paths[i].rate = rate;
                g_seg_paths[i].bits = bits;
                g_seg_paths[i].fmt_ok = 1;
            }
            return;
        }
    }
    i = g_seg_path_i++ % SEG_PATH_N;
    g_seg_paths[i].seg = seg;
    g_seg_paths[i].path[0] = '\0';
    if (path && path[0])
        strncpy(g_seg_paths[i].path, path, sizeof(g_seg_paths[i].path) - 1);
    g_seg_paths[i].valid = valid;
    g_seg_paths[i].ch = fmt_ok ? ch : 0;
    g_seg_paths[i].rate = fmt_ok ? rate : 0;
    g_seg_paths[i].bits = fmt_ok ? bits : 0;
    g_seg_paths[i].fmt_ok = fmt_ok ? 1 : 0;
}

static const char *seg_path_get(void *seg)
{
    int i;
    for (i = 0; i < SEG_PATH_N; ++i)
        if (g_seg_paths[i].seg == seg && g_seg_paths[i].path[0])
            return g_seg_paths[i].path;
    return "";
}

static int seg_fmt_get(void *seg, WORD *ch, DWORD *rate, WORD *bits)
{
    int i;
    for (i = 0; i < SEG_PATH_N; ++i) {
        if (g_seg_paths[i].seg == seg && g_seg_paths[i].fmt_ok) {
            if (ch)
                *ch = g_seg_paths[i].ch;
            if (rate)
                *rate = g_seg_paths[i].rate;
            if (bits)
                *bits = g_seg_paths[i].bits;
            return 1;
        }
    }
    return 0;
}

static void wrap_segment(void *seg);

static HRESULT STDMETHODCALLTYPE hook_InitAudio(void *This, void **ppDM, void **ppDS, HWND hwnd,
                                                DWORD pathType, DWORD pchannels, DWORD flags,
                                                void *params)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_init_n);
    LONGLONG t0;
    LARGE_INTEGER qpf, qpc0, qpc1;
    char js[360];

    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc0);
    hr = real_InitAudio(This, ppDM, ppDS, hwnd, pathType, pchannels, flags, params);
    QueryPerformanceCounter(&qpc1);
    t0 = (qpc1.QuadPart - qpc0.QuadPart) * 1000 / (qpf.QuadPart ? qpf.QuadPart : 1);

    /* #region agent log — flags as string so NDJSON stays valid */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"hr\":%ld,\"hwnd\":%lu,\"pathType\":%lu,\"pch\":%lu,\"flags\":\"0x%lX\","
             "\"ppDM\":%d,\"ppDS\":%d,\"dmOut\":%lu,\"dsOut\":%lu,\"ms\":%ld}",
             (long)n, (long)hr, (unsigned long)(ULONG_PTR)hwnd, (unsigned long)pathType,
             (unsigned long)pchannels, (unsigned long)flags, ppDM ? 1 : 0, ppDS ? 1 : 0,
             (unsigned long)(ppDM && *ppDM ? (ULONG_PTR)*ppDM : 0),
             (unsigned long)(ppDS && *ppDS ? (ULONG_PTR)*ppDS : 0), (long)t0);
    dm_agent("C1", "dm_trace.c:InitAudio", "init-audio", js);
    /* #endregion */
    log_msg("dm-trace: InitAudio #%ld hr=0x%08lx flags=0x%lx pathType=%lu pch=%lu ds=%p", (long)n,
            (unsigned long)hr, (unsigned long)flags, (unsigned long)pathType,
            (unsigned long)pchannels, ppDS && *ppDS ? *ppDS : NULL);
    /* #region agent log — stereo: DS primary PCM layout */
    if (SUCCEEDED(hr))
        ds_log_primary_format(ppDS && *ppDS ? *ppDS : NULL);
    /* #endregion */
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_PlaySegmentEx(void *This, void *source, WCHAR *name,
                                                    void *transition, DWORD flags, LONGLONG start,
                                                    void **ppState, void *from, void *audiopath)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_play_n);
    char js[520];
    char nm[96];
    char path_esc[220];
    nm[0] = '\0';
    if (name) {
        WideCharToMultiByte(CP_ACP, 0, name, -1, nm, (int)sizeof(nm), NULL, NULL);
        nm[sizeof(nm) - 1] = '\0';
    }
    if (source)
        wrap_segment(source);
    json_esc(seg_path_get(source), path_esc, sizeof(path_esc));
    {
        WORD sch = 0, sbits = 0;
        DWORD srate = 0;
        seg_fmt_get(source, &sch, &srate, &sbits);
        hr = real_PlaySegmentEx(This, source, name, transition, flags, start, ppState, from,
                                audiopath);
        /* #region agent log — full native contract fields for replace parity */
        {
            void *st = (ppState && *ppState) ? *ppState : NULL;
            DWORD st4 = st ? (DWORD)(ULONG_PTR) * (void **)((BYTE *)st + 4) : 0;
            DWORD st8 = st ? *(DWORD *)((BYTE *)st + 8) : 0;
            int sel = path_is_selection(path_esc);
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"hr\":%ld,\"flags\":\"0x%lX\",\"start\":%lld,\"src\":%lu,"
                     "\"transition\":%lu,\"from\":%lu,\"audiopath\":%lu,\"name\":\"%.48s\","
                     "\"file\":\"%.160s\",\"state\":%lu,\"st4\":%lu,\"st8\":%lu,\"sel\":%d,"
                     "\"ch\":%u,\"rate\":%lu,\"bits\":%u,\"tick\":%lu}",
                     (long)n, (long)hr, (unsigned long)flags, (long long)start,
                     (unsigned long)(ULONG_PTR)source, (unsigned long)(ULONG_PTR)transition,
                     (unsigned long)(ULONG_PTR)from, (unsigned long)(ULONG_PTR)audiopath, nm,
                     path_esc, (unsigned long)(ULONG_PTR)st, (unsigned long)st4, (unsigned long)st8,
                     sel, (unsigned)sch, (unsigned long)srate, (unsigned)sbits,
                     (unsigned long)GetTickCount());
            dm_agent(sel ? "N2" : "S2", "dm_trace.c:PlaySegmentEx", "play-seg", js);
        }
        /* #endregion */
    }
    /* Baseline: log every play (no throttle) — needed to diff vs replace. */
    log_msg("dm-trace: PlaySegmentEx #%ld hr=0x%08lx flags=0x%lx start=%lld name='%s' file='%s' "
            "path=%p from=%p trans=%p",
            (long)n, (unsigned long)hr, (unsigned long)flags, (long long)start, nm[0] ? nm : "",
            path_esc[0] ? path_esc : "(unknown)", audiopath, from, transition);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_PlaySegment(void *This, void *seg, DWORD flags, LONGLONG start,
                                                  void **ppState)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_play_old_n);
    char js[280];
    char path_esc[220];
    if (seg)
        wrap_segment(seg);
    json_esc(seg_path_get(seg), path_esc, sizeof(path_esc));
    hr = real_PlaySegment(This, seg, flags, start, ppState);
    /* #region agent log */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"hr\":%ld,\"flags\":\"0x%lX\",\"seg\":%lu,\"file\":\"%.160s\"}", (long)n,
             (long)hr, (unsigned long)flags, (unsigned long)(ULONG_PTR)seg, path_esc);
    dm_agent("C2", "dm_trace.c:PlaySegment", "play-seg-old", js);
    /* #endregion */
    log_msg("dm-trace: PlaySegment #%ld hr=0x%08lx file='%s'", (long)n, (unsigned long)hr, path_esc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateStdPath(void *This, DWORD type, DWORD pchannels,
                                                    DWORD flags, void **ppPath)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_path_n);
    char js[220];

    hr = real_CreateStdPath(This, type, pchannels, flags, ppPath);
    /* #region agent log */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"hr\":%ld,\"type\":%lu,\"pch\":%lu,\"flags\":\"0x%lX\",\"path\":%lu}",
             (long)n, (long)hr, (unsigned long)type, (unsigned long)pchannels, (unsigned long)flags,
             (unsigned long)(ppPath && *ppPath ? (ULONG_PTR)*ppPath : 0));
    dm_agent("C3", "dm_trace.c:CreateStandardAudioPath", "audio-path", js);
    /* #endregion */
    log_msg("dm-trace: CreateStandardAudioPath #%ld hr=0x%08lx type=%lu pch=%lu", (long)n,
            (unsigned long)hr, (unsigned long)type, (unsigned long)pchannels);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SetGlobalParam(void *This, REFGUID rguidType, void *pData,
                                                     DWORD dwSize)
{
    HRESULT hr = real_SetGlobalParam(This, rguidType, pData, dwSize);
    if (rguidType && IsEqualGUID(rguidType, &GUID_PerfAutoDownload)) {
        char js[160];
        int val = -1;
        if (pData && dwSize >= sizeof(BOOL))
            val = *(BOOL *)pData ? 1 : 0;
        snprintf(js, sizeof(js), "{\"hr\":%ld,\"size\":%lu,\"val\":%d}", (long)hr,
                 (unsigned long)dwSize, val);
        dm_agent("C4", "dm_trace.c:SetGlobalParam", "set-auto-download", js);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SetNotifHandle(void *This, HANDLE h, LONGLONG rt)
{
    HRESULT hr = real_SetNotifHandle(This, h, rt);
    /* #region agent log */
    {
        char js[120];
        snprintf(js, sizeof(js), "{\"hr\":%ld,\"h\":%lu,\"rt\":%lld}", (long)hr,
                 (unsigned long)(ULONG_PTR)h, (long long)rt);
        dm_agent("N0", "dm_trace.c:SetNotificationHandle", "set-notif-h", js);
    }
    /* #endregion */
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_AddNotifType(void *This, REFGUID g)
{
    HRESULT hr = real_AddNotifType(This, g);
    /* #region agent log */
    {
        char js[120];
        int seg = (g && IsEqualGUID(g, &k_GUID_NOTIFICATION_SEGMENT)) ? 1 : 0;
        snprintf(js, sizeof(js), "{\"hr\":%ld,\"seg\":%d,\"d1\":%08lX}", (long)hr, seg,
                 g ? (unsigned long)g->Data1 : 0UL);
        dm_agent("N0", "dm_trace.c:AddNotificationType", "add-notif-t", js);
    }
    /* #endregion */
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_GetNotif(void *This, void **ppMsg)
{
    HRESULT hr = real_GetNotif(This, ppMsg);
    LONG n = InterlockedIncrement(&g_notif_n);
    if (SUCCEEDED(hr) && hr != S_FALSE && ppMsg && *ppMsg) {
        LONG h = InterlockedIncrement(&g_notif_hit_n);
        /* #region agent log — baseline: log every delivered notification */
        notif_log_fields("get", "N1", *ppMsg, "");
        {
            char js[160];
            DWORD opt = *(DWORD *)((BYTE *)*ppMsg + NOTIF_OFF_OPT);
            DWORD typ = *(DWORD *)((BYTE *)*ppMsg + NOTIF_OFF_TYPE);
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"hit\":%ld,\"hr\":%ld,\"msg\":%lu,\"type\":%lu,\"opt\":%lu,"
                     "\"tick\":%lu}",
                     (long)n, (long)h, (long)hr, (unsigned long)(ULONG_PTR)*ppMsg,
                     (unsigned long)typ, (unsigned long)opt, (unsigned long)GetTickCount());
            dm_agent("C5", "dm_trace.c:GetNotificationPMsg", "notif-msg", js);
        }
        /* #endregion */
        if (h <= 40 || (h % 20) == 0)
            log_msg("dm-trace: GetNotificationPMsg hit=%ld hr=0x%08lx msg=%p", (long)h,
                    (unsigned long)hr, *ppMsg);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_FreePMsg(void *This, void *pMsg)
{
    LONG n = InterlockedIncrement(&g_free_notif_n);
    /* #region agent log */
    if (pMsg) {
        DWORD opt = *(DWORD *)((BYTE *)pMsg + NOTIF_OFF_OPT);
        if (opt == 1u || n <= 30 || (n % 40) == 0)
            notif_log_fields("free", "N1", pMsg, "");
    }
    /* #endregion */
    return real_FreePMsg(This, pMsg);
}

static HRESULT STDMETHODCALLTYPE hook_GetObject(void *This, void *pDesc, REFIID riid, void **ppv)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_getobj_n);
    DWORD valid = 0, dwSize = 0;
    char gclass[48], riid_s[48], path[200], path_esc[220], js[480];
    void *stream = NULL;

    gclass[0] = '\0';
    riid_s[0] = '\0';
    path[0] = '\0';
    if (riid)
        guid_str(riid, riid_s, sizeof(riid_s));
    if (pDesc) {
        DWORD *pd = (DWORD *)pDesc;
        dwSize = pd[0];
        valid = pd[1];
        /* DMUS_OBJECTDESC: size, valid, guidObject@8, guidClass@24, ... pbStream near end */
        if (dwSize >= 40)
            guid_str((const GUID *)((BYTE *)pDesc + 24), gclass, sizeof(gclass));
        if ((valid & DMUS_OBJ_STREAM) && dwSize >= 0x350)
            stream = *(void **)((BYTE *)pDesc + 0x34C);
        if (!stream && (valid & DMUS_OBJ_STREAM) && dwSize >= 8)
            stream = *(void **)((BYTE *)pDesc + dwSize - sizeof(void *));
        if (stream)
            scrape_stream_path(stream, path, sizeof(path));
    }

    {
        WORD ch = 0, bits = 0;
        DWORD rate = 0;
        int fmt_ok = 0;
        if (path[0])
            fmt_ok = probe_audio_fmt(path, &ch, &rate, &bits);

        hr = real_GetObject(This, pDesc, riid, ppv);
        if (SUCCEEDED(hr) && ppv && *ppv) {
            wrap_segment(*ppv);
            seg_path_put(*ppv, path, valid, ch, rate, bits, fmt_ok);
        }

        json_esc(path, path_esc, sizeof(path_esc));
        /* #region agent log */
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"hr\":%ld,\"dwSize\":%lu,\"valid\":\"0x%lX\",\"stream\":%d,\"memory\":%d,"
                 "\"filename\":%d,\"class\":\"%s\",\"iid\":\"%s\",\"file\":\"%.160s\",\"obj\":%lu,"
                 "\"streamPtr\":%lu,\"ch\":%u,\"rate\":%lu,\"bits\":%u,\"stereo\":%d,\"fmtOk\":%d}",
                 (long)n, (long)hr, (unsigned long)dwSize, (unsigned long)valid,
                 (valid & DMUS_OBJ_STREAM) ? 1 : 0, (valid & DMUS_OBJ_MEMORY) ? 1 : 0,
                 (valid & DMUS_OBJ_FILENAME) ? 1 : 0, gclass, riid_s, path_esc,
                 (unsigned long)(ppv && *ppv ? (ULONG_PTR)*ppv : 0),
                 (unsigned long)(ULONG_PTR)stream, (unsigned)ch, (unsigned long)rate, (unsigned)bits,
                 (fmt_ok && ch >= 2) ? 1 : 0, fmt_ok);
        dm_agent("S2", "dm_trace.c:GetObject", "get-object", js);
        /* #endregion */
        if (fmt_ok && (n <= 40 || (n % 20) == 0))
            log_msg("dm-trace: fmt file='%s' ch=%u rate=%lu bits=%u stereo=%d", path_esc,
                    (unsigned)ch, (unsigned long)rate, (unsigned)bits, ch >= 2 ? 1 : 0);
    }
    if (n <= 40 || (n % 20) == 0)
        log_msg("dm-trace: GetObject #%ld hr=0x%08lx file='%s' class=%s", (long)n, (unsigned long)hr,
                path_esc[0] ? path_esc : "(none)", gclass);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SetRepeats(void *This, DWORD repeats)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_rep_n);
    char js[280];
    char path_esc[220];
    json_esc(seg_path_get(This), path_esc, sizeof(path_esc));
    hr = real_SetRepeats(This, repeats);
    /* #region agent log */
    snprintf(js, sizeof(js), "{\"n\":%ld,\"hr\":%ld,\"repeats\":%lu,\"seg\":%lu,\"file\":\"%.160s\"}",
             (long)n, (long)hr, (unsigned long)repeats, (unsigned long)(ULONG_PTR)This, path_esc);
    dm_agent("C6", "dm_trace.c:SetRepeats", "seg-SetRepeats", js);
    /* #endregion */
    log_msg("dm-trace: SetRepeats #%ld hr=0x%08lx n=%lu file='%s'", (long)n, (unsigned long)hr,
            (unsigned long)repeats, path_esc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_Download(void *This, void *pAudioPath)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_dl_n);
    char js[280];
    char path_esc[220];
    json_esc(seg_path_get(This), path_esc, sizeof(path_esc));
    hr = real_Download(This, pAudioPath);
    /* #region agent log */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"hr\":%ld,\"audiopath\":%lu,\"seg\":%lu,\"file\":\"%.160s\"}", (long)n,
             (long)hr, (unsigned long)(ULONG_PTR)pAudioPath, (unsigned long)(ULONG_PTR)This,
             path_esc);
    dm_agent("C4", "dm_trace.c:Download", "seg-download", js);
    /* #endregion */
    log_msg("dm-trace: Segment::Download #%ld hr=0x%08lx file='%s' path=%p", (long)n,
            (unsigned long)hr, path_esc[0] ? path_esc : "(unknown)", pAudioPath);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_Unload(void *This, void *pAudioPath)
{
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_ul_n);
    char js[280];
    char path_esc[220];
    json_esc(seg_path_get(This), path_esc, sizeof(path_esc));
    hr = real_Unload(This, pAudioPath);
    /* #region agent log */
    {
        int sel = path_is_selection(path_esc);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"hr\":%ld,\"audiopath\":%lu,\"seg\":%lu,\"file\":\"%.160s\",\"sel\":%d}",
                 (long)n, (long)hr, (unsigned long)(ULONG_PTR)pAudioPath,
                 (unsigned long)(ULONG_PTR)This, path_esc, sel);
        dm_agent(sel ? "N3" : "C4", "dm_trace.c:Unload", "seg-unload", js);
    }
    /* #endregion */
    log_msg("dm-trace: Segment::Unload #%ld hr=0x%08lx file='%s' path=%p", (long)n,
            (unsigned long)hr, path_esc[0] ? path_esc : "(unknown)", pAudioPath);
    return hr;
}

static void **dup_vtable(void **orig, int count)
{
    void **nv;
    DWORD old;
    SIZE_T bytes = (SIZE_T)count * sizeof(void *);
    nv = (void **)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!nv)
        return NULL;
    memcpy(nv, orig, bytes);
    VirtualProtect(nv, bytes, PAGE_EXECUTE_READ, &old);
    return nv;
}

static void install_perf_wrap(void *obj)
{
    void **vt;
    void **nv;
    DWORD old;

    if (!obj)
        return;
    vt = *(void ***)obj;
    if (!vt)
        return;
    if (g_perf_vt_template && vt == g_perf_vt_template)
        return;
    if (vt[PERF_IDX_INITAUDIO] == (void *)hook_InitAudio)
        return;

    nv = dup_vtable(vt, PERF_VT_COUNT);
    if (!nv)
        return;
    real_InitAudio = (PFN_InitAudio)vt[PERF_IDX_INITAUDIO];
    real_PlaySegmentEx = (PFN_PlaySegmentEx)vt[PERF_IDX_PLAYSEGEX];
    real_PlaySegment = (PFN_PlaySegment)vt[PERF_IDX_PLAYSEG];
    real_CreateStdPath = (PFN_CreateStdPath)vt[PERF_IDX_CREATESTD];
    real_SetGlobalParam = (PFN_SetGlobalParam)vt[PERF_IDX_SETGLOBAL];
    real_GetNotif = (PFN_GetNotif)vt[PERF_IDX_GETNOTIF];
    real_FreePMsg = (PFN_FreePMsg)vt[PERF_IDX_FREEPMSG];
    real_SetNotifHandle = (PFN_SetNotifHandle)vt[PERF_IDX_SETNOTIF];
    real_AddNotifType = (PFN_AddNotifType)vt[PERF_IDX_ADDNOTIF];

    if (!VirtualProtect(nv, PERF_VT_COUNT * sizeof(void *), PAGE_READWRITE, &old))
        return;
    nv[PERF_IDX_INITAUDIO] = (void *)hook_InitAudio;
    nv[PERF_IDX_PLAYSEGEX] = (void *)hook_PlaySegmentEx;
    nv[PERF_IDX_PLAYSEG] = (void *)hook_PlaySegment;
    nv[PERF_IDX_CREATESTD] = (void *)hook_CreateStdPath;
    nv[PERF_IDX_SETGLOBAL] = (void *)hook_SetGlobalParam;
    nv[PERF_IDX_GETNOTIF] = (void *)hook_GetNotif;
    nv[PERF_IDX_FREEPMSG] = (void *)hook_FreePMsg;
    nv[PERF_IDX_SETNOTIF] = (void *)hook_SetNotifHandle;
    nv[PERF_IDX_ADDNOTIF] = (void *)hook_AddNotifType;
    VirtualProtect(nv, PERF_VT_COUNT * sizeof(void *), PAGE_EXECUTE_READ, &old);

    if (!VirtualProtect(obj, sizeof(void *), PAGE_READWRITE, &old))
        return;
    *(void ***)obj = nv;
    VirtualProtect(obj, sizeof(void *), old, &old);
    g_perf_vt_template = nv;
    log_msg("dm-trace: wrapped Performance %p vtable", obj);
    dm_agent("C0", "dm_trace.c:wrap", "wrap-perf", "{\"ok\":1}");
}

static void install_loader_wrap(void *obj)
{
    void **vt;
    void **nv;
    DWORD old;

    if (!obj)
        return;
    vt = *(void ***)obj;
    if (!vt)
        return;
    if (g_ldr_vt_template && vt == g_ldr_vt_template)
        return;
    if (vt[LDR_IDX_GETOBJECT] == (void *)hook_GetObject)
        return;

    nv = dup_vtable(vt, LDR_VT_COUNT);
    if (!nv)
        return;
    real_GetObject = (PFN_GetObject)vt[LDR_IDX_GETOBJECT];
    if (!VirtualProtect(nv, LDR_VT_COUNT * sizeof(void *), PAGE_READWRITE, &old))
        return;
    nv[LDR_IDX_GETOBJECT] = (void *)hook_GetObject;
    VirtualProtect(nv, LDR_VT_COUNT * sizeof(void *), PAGE_EXECUTE_READ, &old);

    if (!VirtualProtect(obj, sizeof(void *), PAGE_READWRITE, &old))
        return;
    *(void ***)obj = nv;
    VirtualProtect(obj, sizeof(void *), old, &old);
    g_ldr_vt_template = nv;
    log_msg("dm-trace: wrapped Loader %p vtable", obj);
    dm_agent("C0", "dm_trace.c:wrap", "wrap-loader", "{\"ok\":1}");
}

static void wrap_segment(void *seg)
{
    void **vt;
    void **nv;
    DWORD old;

    if (!seg)
        return;
    vt = *(void ***)seg;
    if (!vt)
        return;
    if (vt[SEG_IDX_DOWNLOAD] == (void *)hook_Download)
        return;
    if (g_seg_vt_template && g_seg_vt_orig && vt == g_seg_vt_orig) {
        if (VirtualProtect(seg, sizeof(void *), PAGE_READWRITE, &old)) {
            *(void ***)seg = g_seg_vt_template;
            VirtualProtect(seg, sizeof(void *), old, &old);
        }
        return;
    }

    nv = dup_vtable(vt, SEG_VT_COUNT);
    if (!nv)
        return;
    real_Download = (PFN_Download)vt[SEG_IDX_DOWNLOAD];
    real_Unload = (PFN_Download)vt[SEG_IDX_UNLOAD];
    real_SetRepeats = (PFN_SetRepeats)vt[SEG_IDX_SETREPEATS];
    if (!VirtualProtect(nv, SEG_VT_COUNT * sizeof(void *), PAGE_READWRITE, &old))
        return;
    nv[SEG_IDX_DOWNLOAD] = (void *)hook_Download;
    nv[SEG_IDX_UNLOAD] = (void *)hook_Unload;
    nv[SEG_IDX_SETREPEATS] = (void *)hook_SetRepeats;
    VirtualProtect(nv, SEG_VT_COUNT * sizeof(void *), PAGE_EXECUTE_READ, &old);

    if (!VirtualProtect(seg, sizeof(void *), PAGE_READWRITE, &old))
        return;
    *(void ***)seg = nv;
    VirtualProtect(seg, sizeof(void *), old, &old);
    g_seg_vt_orig = vt;
    g_seg_vt_template = nv;
}

void dm_trace_on_cocreate(REFCLSID clsid, void *iface)
{
    if (!clsid || !iface)
        return;
    if (IsEqualGUID(clsid, &CLSID_DMPerformance))
        install_perf_wrap(iface);
    else if (IsEqualGUID(clsid, &CLSID_DMLoader))
        install_loader_wrap(iface);
    else if (IsEqualGUID(clsid, &k_CLSID_DirectSound)) {
        char js[96];
        snprintf(js, sizeof(js), "{\"ds\":%lu}", (unsigned long)(ULONG_PTR)iface);
        dm_agent("C0", "dm_trace.c:cocreate", "cocreate-dsound", js);
        log_msg("dm-trace: CoCreate DirectSound iface=%p", iface);
    }
}

void dm_trace_install(void)
{
    int native = !env_on("CK_DM_REPLACE", 1);
    g_dm_run_id = native ? "native-baseline" : "dm-replace-cocreate";
    /* #region agent log */
    {
        char js[280];
        snprintf(js, sizeof(js),
                 "{\"native\":%d,\"runId\":\"%s\",\"hooks\":[\"InitAudio\",\"PlaySegmentEx\","
                 "\"PlaySegment\",\"GetNotif\",\"FreePMsg\",\"Unload\",\"Download\",\"SetRepeats\","
                 "\"AddNotifType\",\"SetNotifHandle\",\"CreateStdPath\",\"GetObject\"]}",
                 native, g_dm_run_id);
        dm_agent("C0", "dm_trace.c:install", "dm-trace-ready", js);
    }
    /* #endregion */
    log_msg("dm-trace: installed (%s) runId=%s — full PlaySegmentEx/notif baseline logging",
            native ? "native DM" : "replace+native cocreate", g_dm_run_id);
}
