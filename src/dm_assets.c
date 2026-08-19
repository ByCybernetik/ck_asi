#include "dm_replace_internal.h"

/* stb_vorbis (compiled as stb_vorbis.o) */
typedef struct stb_vorbis stb_vorbis;
typedef struct {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
} stb_vorbis_info;
typedef struct {
    char *alloc_buffer;
    int alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;
extern stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error,
                                         const stb_vorbis_alloc *alloc);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer,
                                                    int num_shorts);
extern int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels,
                                    int *sample_rate, short **output);
extern int stb_vorbis_get_frame_short_interleaved(stb_vorbis *f, int num_c, short *buffer, int num_shorts);
extern void stb_vorbis_close(stb_vorbis *f);

static int parse_wav(const BYTE *data, DWORD size, WAVEFORMATEX *fmt, const BYTE **pcm, DWORD *pcm_len)
{
    DWORD pos;
    if (!data || size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return 0;
    memset(fmt, 0, sizeof(*fmt));
    *pcm = NULL;
    *pcm_len = 0;
    pos = 12;
    while (pos + 8 <= size) {
        DWORD cksz = *(const DWORD *)(data + pos + 4);
        const BYTE *chunk = data + pos + 8;
        if (pos + 8 + cksz > size)
            break;
        if (memcmp(data + pos, "fmt ", 4) == 0) {
            if (cksz < 16)
                return 0;
            memcpy(fmt, chunk, cksz < sizeof(WAVEFORMATEX) ? cksz : sizeof(WAVEFORMATEX));
            if (fmt->wFormatTag != WAVE_FORMAT_PCM)
                return 0;
        } else if (memcmp(data + pos, "data", 4) == 0) {
            *pcm = chunk;
            *pcm_len = cksz;
        }
        pos += 8 + cksz + (cksz & 1);
    }
    return (*pcm && *pcm_len && fmt->nChannels && fmt->nSamplesPerSec && fmt->wBitsPerSample) ? 1 : 0;
}

/* Decode RIFF or Ogg into owned PCM buffer (*pcm_out must free). */
int decode_to_pcm(const BYTE *raw, DWORD raw_len, WAVEFORMATEX *fmt, BYTE **pcm_out,
                         DWORD *pcm_len, int max_sec)
{
    const BYTE *p;
    DWORD plen;
    stb_vorbis *v;
    stb_vorbis_info vi;
    int err = 0, max_frames, buf_shorts, got;
    short *samples = NULL;

    *pcm_out = NULL;
    *pcm_len = 0;
    memset(fmt, 0, sizeof(*fmt));

    if (raw_len >= 4 && memcmp(raw, "RIFF", 4) == 0) {
        if (!parse_wav(raw, raw_len, fmt, &p, &plen))
            return 0;
        *pcm_out = (BYTE *)malloc(plen ? plen : 1);
        if (!*pcm_out)
            return 0;
        memcpy(*pcm_out, p, plen);
        *pcm_len = plen;
        return 1;
    }

    if (raw_len < 4 || memcmp(raw, "OggS", 4) != 0)
        return 0;

    /* Music tracks are 3–8 min; OGG_DECODE_FULL must stay negative (0 was clamped to 1s). */
    if (max_sec < 0) {
        /*
         * stb_vorbis_decode_memory uses v->channels internally for
         * stb_vorbis_get_frame_short_interleaved.  Some game OGGs report
         * 6-8 channels, causing stb_vorbis internal OOM (NULL channel_buffers)
         * → crash.  Decode manually with stereo downmix instead.
         */
        stb_vorbis *vf;
        stb_vorbis_info vfi;
        int limit_f, total_s, offset_s = 0, data_len = 0, err2 = 0;
        short *out = NULL;
        const int out_ch = 2;

        vf = stb_vorbis_open_memory(raw, (int)raw_len, &err2, NULL);
        if (!vf) {
            dm_agent("H44", "dm_replace.c:decode_to_pcm", "ogg-decode-full-fail", "{\"err\":1}");
            return 0;
        }
        vfi = stb_vorbis_get_info(vf);
        if (vfi.channels < 1 || vfi.channels > 2 || vfi.sample_rate < 1) {
            stb_vorbis_close(vf);
            return 0;
        }
        limit_f = 4096;
        total_s = limit_f * out_ch;
        out = (short *)malloc((size_t)total_s * sizeof(short));
        if (!out) { stb_vorbis_close(vf); return 0; }
        for (;;) {
            int n = stb_vorbis_get_frame_short_interleaved(vf, out_ch,
                        out + offset_s, total_s - offset_s);
            if (n == 0) break;
            data_len += n;
            offset_s += n * out_ch;
            if (offset_s + limit_f * out_ch > total_s) {
                short *nb;
                total_s *= 2;
                nb = (short *)realloc(out, (size_t)total_s * sizeof(short));
                if (!nb) { free(out); stb_vorbis_close(vf); return 0; }
                out = nb;
            }
        }
        stb_vorbis_close(vf);
        if (data_len <= 0 || !out) { free(out); return 0; }
        fmt->wFormatTag = WAVE_FORMAT_PCM;
        fmt->nChannels = (WORD)out_ch;
        fmt->nSamplesPerSec = (DWORD)vfi.sample_rate;
        fmt->wBitsPerSample = 16;
        fmt->nBlockAlign = (WORD)(out_ch * 2);
        fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
        *pcm_len = (DWORD)data_len * (DWORD)out_ch * 2u;
        *pcm_out = (BYTE *)out;
        /* #region agent log */
        {
            char js[180];
            snprintf(js, sizeof(js),
                     "{\"got\":%d,\"pcm\":%lu,\"cap_sec\":0,\"dur_sec\":%.2f,\"ch\":%d,\"rate\":%u}",
                     data_len, (unsigned long)*pcm_len,
                     (double)data_len / (double)vfi.sample_rate,
                     out_ch, (unsigned)vfi.sample_rate);
            dm_agent("H44", "dm_replace.c:decode_to_pcm", "ogg-decode-full", js);
        }
        /* #endregion */
        return 1;
    }

    if (max_sec < 1)
        max_sec = 1;

    v = stb_vorbis_open_memory(raw, (int)raw_len, &err, NULL);
    if (!v)
        return 0;
    vi = stb_vorbis_get_info(v);
    if (vi.channels < 1 || vi.channels > 2 || vi.sample_rate < 1) {
        stb_vorbis_close(v);
        return 0;
    }
    max_frames = (int)vi.sample_rate * max_sec;
    if (max_frames < 1)
        max_frames = 1;
    {
        const int out_ch = 2;
        buf_shorts = max_frames * out_ch;
        samples = (short *)malloc((size_t)buf_shorts * sizeof(short));
        if (!samples) {
            stb_vorbis_close(v);
            return 0;
        }
        got = stb_vorbis_get_samples_short_interleaved(v, out_ch, samples, buf_shorts);
        stb_vorbis_close(v);
        if (got <= 0) {
            free(samples);
            return 0;
        }
        fmt->wFormatTag = WAVE_FORMAT_PCM;
        fmt->nChannels = (WORD)out_ch;
        fmt->nSamplesPerSec = (DWORD)vi.sample_rate;
        fmt->wBitsPerSample = 16;
        fmt->nBlockAlign = (WORD)(out_ch * 2);
        fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
        *pcm_len = (DWORD)got * (DWORD)out_ch * 2u;
    }
    *pcm_out = (BYTE *)samples;
    /* #region agent log */
    {
        char js[140];
        snprintf(js, sizeof(js), "{\"got\":%d,\"pcm\":%lu,\"cap_sec\":%d,\"ch\":%d,\"rate\":%u}", got,
                 (unsigned long)*pcm_len, max_sec, vi.channels, (unsigned)vi.sample_rate);
        dm_agent("H35", "dm_replace.c:decode_to_pcm", "ogg-decode", js);
    }
    /* #endregion */
    return 1;
}

enum { PCM_CACHE_N = 64 };
static struct {
    char path[260];
    BYTE *pcm;
    DWORD pcm_bytes;
    WAVEFORMATEX fmt;
} g_pcm_cache[PCM_CACHE_N];
static int g_pcm_cache_n;

int cache_get(const char *path, WAVEFORMATEX *fmt, BYTE **pcm, DWORD *pcm_bytes)
{
    int i;
    for (i = 0; i < g_pcm_cache_n; ++i) {
        if (lstrcmpiA(g_pcm_cache[i].path, path) == 0) {
            *fmt = g_pcm_cache[i].fmt;
            *pcm_bytes = g_pcm_cache[i].pcm_bytes;
            *pcm = (BYTE *)malloc(*pcm_bytes);
            if (!*pcm)
                return 0;
            memcpy(*pcm, g_pcm_cache[i].pcm, *pcm_bytes);
            return 1;
        }
    }
    return 0;
}

void cache_put(const char *path, const WAVEFORMATEX *fmt, const BYTE *pcm, DWORD pcm_bytes)
{
    int i;
    if (!path || !pcm || !pcm_bytes)
        return;
    /* SFX only — music uses music_cache_* (shared, no 45MB memcpy). */
    if (pcm_bytes > 4u * 1024u * 1024u)
        return;
    for (i = 0; i < g_pcm_cache_n; ++i) {
        if (lstrcmpiA(g_pcm_cache[i].path, path) == 0)
            return;
    }
    if (g_pcm_cache_n >= PCM_CACHE_N)
        return;
    strncpy(g_pcm_cache[g_pcm_cache_n].path, path, sizeof(g_pcm_cache[0].path) - 1);
    g_pcm_cache[g_pcm_cache_n].fmt = *fmt;
    g_pcm_cache[g_pcm_cache_n].pcm_bytes = pcm_bytes;
    g_pcm_cache[g_pcm_cache_n].pcm = (BYTE *)malloc(pcm_bytes);
    if (!g_pcm_cache[g_pcm_cache_n].pcm)
        return;
    memcpy(g_pcm_cache[g_pcm_cache_n].pcm, pcm, pcm_bytes);
    g_pcm_cache_n++;
}

/* Music BGM: shared PCM cache with byte budget (32-bit: full-folder preload OOM'd tpw*). */
enum { MUSIC_CACHE_N = 12 };
enum { MUSIC_CACHE_MAX_BYTES = 96u * 1024u * 1024u };
typedef struct {
    char path[260];
    BYTE *pcm;
    DWORD pcm_bytes;
    WAVEFORMATEX fmt;
    LONG refs;
    LPDIRECTSOUNDBUFFER ds_buf;
    volatile LONG ds_building;
} MusicCacheEnt;
static MusicCacheEnt g_music_cache[MUSIC_CACHE_N];
static int g_music_cache_n;
static DWORD g_music_cache_bytes;
static CRITICAL_SECTION g_music_cs;
static volatile LONG g_music_cs_ready;
static volatile LONG g_music_preload_started;

static void music_cache_ensure_cs(void)
{
    if (g_music_cs_ready == 2)
        return;
    if (InterlockedCompareExchange(&g_music_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_music_cs);
        InterlockedExchange(&g_music_cs_ready, 2);
    } else {
        while (g_music_cs_ready != 2)
            Sleep(0);
    }
}

static void music_path_key(const char *in, char *out, size_t n)
{
    size_t i, j = 0;
    if (!out || n == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    while (in[0] == '.' && (in[1] == '\\' || in[1] == '/'))
        in += 2;
    for (i = 0; in[i] && j + 1 < n; ++i) {
        char c = in[i];
        if (c == '/')
            c = '\\';
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        out[j++] = c;
    }
    out[j] = '\0';
}

static int music_cache_evict_one_unlocked(void)
{
    int i;
    if (g_music_cache_n <= 0)
        return 0;
    for (i = 0; i < g_music_cache_n; ++i) {
        if (g_music_cache[i].refs == 0)
            break;
    }
    if (i >= g_music_cache_n)
        return 0;
    if (g_music_cache[i].ds_buf) {
        IDirectSoundBuffer_Release(g_music_cache[i].ds_buf);
        g_music_cache[i].ds_buf = NULL;
    }
    free(g_music_cache[i].pcm);
    if (g_music_cache_bytes >= g_music_cache[i].pcm_bytes)
        g_music_cache_bytes -= g_music_cache[i].pcm_bytes;
    else
        g_music_cache_bytes = 0;
    for (++i; i < g_music_cache_n; ++i)
        g_music_cache[i - 1] = g_music_cache[i];
    g_music_cache_n--;
    memset(&g_music_cache[g_music_cache_n], 0, sizeof(g_music_cache[0]));
    return 1;
}

int music_cache_get(const char *path, WAVEFORMATEX *fmt, BYTE **pcm, DWORD *pcm_bytes)
{
    char key[260];
    int i, found = 0;
    if (!path || !fmt || !pcm || !pcm_bytes)
        return 0;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            /* LRU: move hit to end. */
            if (i + 1 < g_music_cache_n) {
                MusicCacheEnt tmp;
                memcpy(&tmp, &g_music_cache[i], sizeof(tmp));
                memmove(&g_music_cache[i], &g_music_cache[i + 1],
                        (size_t)(g_music_cache_n - i - 1) * sizeof(g_music_cache[0]));
                memcpy(&g_music_cache[g_music_cache_n - 1], &tmp, sizeof(tmp));
                i = g_music_cache_n - 1;
            }
            *fmt = g_music_cache[i].fmt;
            *pcm = g_music_cache[i].pcm;
            *pcm_bytes = g_music_cache[i].pcm_bytes;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_music_cs);
    return found;
}

int music_cache_acquire(const char *path, WAVEFORMATEX *fmt, BYTE **pcm, DWORD *pcm_bytes)
{
    char key[260];
    int i, found = 0;
    if (!path || !fmt || !pcm || !pcm_bytes)
        return 0;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            *fmt = g_music_cache[i].fmt;
            *pcm = g_music_cache[i].pcm;
            *pcm_bytes = g_music_cache[i].pcm_bytes;
            InterlockedIncrement(&g_music_cache[i].refs);
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_music_cs);
    return found;
}

BYTE *music_cache_intern(const char *path, const WAVEFORMATEX *fmt, BYTE *pcm, DWORD pcm_bytes)
{
    char key[260];
    int i;
    BYTE *ret = pcm;
    if (!path || !fmt || !pcm || !pcm_bytes)
        return pcm;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            if (g_music_cache[i].pcm != pcm)
                free(pcm);
            ret = g_music_cache[i].pcm;
            LeaveCriticalSection(&g_music_cs);
            return ret;
        }
    }
    while (g_music_cache_n > 0 &&
           (g_music_cache_n >= MUSIC_CACHE_N ||
            g_music_cache_bytes + pcm_bytes > MUSIC_CACHE_MAX_BYTES)) {
        if (!music_cache_evict_one_unlocked())
            break;
    }
    if (g_music_cache_n >= MUSIC_CACHE_N ||
        g_music_cache_bytes + pcm_bytes > MUSIC_CACHE_MAX_BYTES) {
        /* Still no room for this track alone — leave caller-owned. */
        LeaveCriticalSection(&g_music_cs);
        return pcm;
    }
    strncpy(g_music_cache[g_music_cache_n].path, key, sizeof(g_music_cache[0].path) - 1);
    g_music_cache[g_music_cache_n].path[sizeof(g_music_cache[0].path) - 1] = '\0';
    g_music_cache[g_music_cache_n].fmt = *fmt;
    g_music_cache[g_music_cache_n].pcm = pcm;
    g_music_cache[g_music_cache_n].pcm_bytes = pcm_bytes;
    g_music_cache_bytes += pcm_bytes;
    g_music_cache_n++;
    ret = pcm;
    LeaveCriticalSection(&g_music_cs);
    return ret;
}

BYTE *music_cache_intern_acquire(const char *path, const WAVEFORMATEX *fmt, BYTE *pcm,
                                 DWORD pcm_bytes, int *cached)
{
    char key[260];
    int i;
    if (cached)
        *cached = 0;
    if (!path || !fmt || !pcm || !pcm_bytes)
        return pcm;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            if (g_music_cache[i].pcm != pcm)
                free(pcm);
            InterlockedIncrement(&g_music_cache[i].refs);
            pcm = g_music_cache[i].pcm;
            if (cached)
                *cached = 1;
            LeaveCriticalSection(&g_music_cs);
            return pcm;
        }
    }
    while (g_music_cache_n > 0 &&
           (g_music_cache_n >= MUSIC_CACHE_N ||
            g_music_cache_bytes + pcm_bytes > MUSIC_CACHE_MAX_BYTES)) {
        if (!music_cache_evict_one_unlocked())
            break;
    }
    if (g_music_cache_n < MUSIC_CACHE_N &&
        g_music_cache_bytes + pcm_bytes <= MUSIC_CACHE_MAX_BYTES) {
        MusicCacheEnt *ent = &g_music_cache[g_music_cache_n++];
        memset(ent, 0, sizeof(*ent));
        strncpy(ent->path, key, sizeof(ent->path) - 1);
        ent->fmt = *fmt;
        ent->pcm = pcm;
        ent->pcm_bytes = pcm_bytes;
        ent->refs = 1;
        g_music_cache_bytes += pcm_bytes;
        if (cached)
            *cached = 1;
    }
    LeaveCriticalSection(&g_music_cs);
    return pcm;
}

void music_cache_release(const char *path, const BYTE *pcm)
{
    char key[260];
    int i;
    if (!path || !pcm)
        return;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (g_music_cache[i].pcm == pcm && strcmp(g_music_cache[i].path, key) == 0) {
            if (g_music_cache[i].refs > 0)
                InterlockedDecrement(&g_music_cache[i].refs);
            break;
        }
    }
    LeaveCriticalSection(&g_music_cs);
}

void music_cache_collect(void)
{
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    while (music_cache_evict_one_unlocked()) {
    }
    LeaveCriticalSection(&g_music_cs);
}

static int music_list_mission_files(char names[][80], int maxn)
{
    const char *root;
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    if (!names || maxn < 1)
        return 0;
    root = dm_game_dir();
    if (!root || !root[0])
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
            continue;
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

static int music_cache_build_ds_unlocked(MusicCacheEnt *ent, LPDIRECTSOUND ds,
                                         LPDIRECTSOUNDBUFFER *out_buf)
{
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    HRESULT hr;

    if (out_buf)
        *out_buf = NULL;
    if (!ent || !ds || !ent->pcm || !ent->pcm_bytes)
        return 0;
    if (ent->ds_buf) {
        if (out_buf) {
            *out_buf = ent->ds_buf;
            IDirectSoundBuffer_AddRef(*out_buf);
        }
        return 1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS |
                   DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_STATIC;
    desc.dwBufferBytes = ent->pcm_bytes;
    desc.lpwfxFormat = &ent->fmt;
    hr = IDirectSound_CreateSoundBuffer(ds, &desc, &buf, NULL);
    if (SUCCEEDED(hr) && buf) {
        if (!ds_buf_write_all(buf, ent->pcm, ent->pcm_bytes)) {
            IDirectSoundBuffer_Release(buf);
            buf = NULL;
        }
    }
    if (buf && out_buf) {
        *out_buf = buf;
        return 1;
    }
    if (buf)
        IDirectSoundBuffer_Release(buf);
    return 0;
}

int music_cache_build_ds_for_path(LPDIRECTSOUND ds, const char *path)
{
    char key[260];
    BYTE *pcm = NULL;
    DWORD pcm_bytes = 0;
    WAVEFORMATEX fmt;
    LPDIRECTSOUNDBUFFER built = NULL;
    MusicCacheEnt tmp;
    int i, ok = 0;

    if (!ds || !path || !path[0])
        return 0;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            if (g_music_cache[i].ds_buf) {
                LeaveCriticalSection(&g_music_cs);
                return 1;
            }
            if (InterlockedCompareExchange(&g_music_cache[i].ds_building, 1, 0) != 0) {
                LeaveCriticalSection(&g_music_cs);
                return 0;
            }
            pcm = g_music_cache[i].pcm;
            pcm_bytes = g_music_cache[i].pcm_bytes;
            fmt = g_music_cache[i].fmt;
            LeaveCriticalSection(&g_music_cs);
            goto build;
        }
    }
    LeaveCriticalSection(&g_music_cs);
    return 0;

build:
    memset(&tmp, 0, sizeof(tmp));
    tmp.pcm = pcm;
    tmp.pcm_bytes = pcm_bytes;
    tmp.fmt = fmt;
    ok = music_cache_build_ds_unlocked(&tmp, ds, &built);

    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            if (ok && built && !g_music_cache[i].ds_buf && g_music_cache[i].pcm == pcm)
                g_music_cache[i].ds_buf = built;
            else if (built)
                IDirectSoundBuffer_Release(built);
            ok = g_music_cache[i].ds_buf != NULL;
            InterlockedExchange(&g_music_cache[i].ds_building, 0);
            break;
        }
    }
    LeaveCriticalSection(&g_music_cs);
    return ok;
}

int music_cache_try_acquire_ds_buf(const char *path, LPDIRECTSOUNDBUFFER *out)
{
    char key[260];
    int i, ok = 0;

    if (out)
        *out = NULL;
    if (!path || !out)
        return 0;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0 && g_music_cache[i].ds_buf) {
            *out = g_music_cache[i].ds_buf;
            IDirectSoundBuffer_AddRef(*out);
            ok = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_music_cs);
    return ok;
}

static BYTE *music_cache_upgrade(const char *path, const WAVEFORMATEX *fmt, BYTE *pcm,
                                 DWORD pcm_bytes)
{
    char key[260];
    int i;
    BYTE *ret = pcm;
    if (!path || !fmt || !pcm || !pcm_bytes)
        return pcm;
    music_path_key(path, key, sizeof(key));
    music_cache_ensure_cs();
    EnterCriticalSection(&g_music_cs);
    for (i = 0; i < g_music_cache_n; ++i) {
        if (strcmp(g_music_cache[i].path, key) == 0) {
            if (pcm_bytes <= g_music_cache[i].pcm_bytes) {
                if (g_music_cache[i].pcm != pcm)
                    free(pcm);
                ret = g_music_cache[i].pcm;
                LeaveCriticalSection(&g_music_cs);
                return ret;
            }
            /* Never replace storage while a CkSegment references it. */
            if (g_music_cache[i].refs > 0) {
                if (g_music_cache[i].pcm != pcm)
                    free(pcm);
                ret = g_music_cache[i].pcm;
                LeaveCriticalSection(&g_music_cs);
                return ret;
            }
            if (g_music_cache_bytes >= g_music_cache[i].pcm_bytes)
                g_music_cache_bytes -= g_music_cache[i].pcm_bytes;
            else
                g_music_cache_bytes = 0;
            if (g_music_cache[i].ds_buf) {
                IDirectSoundBuffer_Release(g_music_cache[i].ds_buf);
                g_music_cache[i].ds_buf = NULL;
            }
            if (g_music_cache[i].pcm != pcm)
                free(g_music_cache[i].pcm);
            g_music_cache[i].fmt = *fmt;
            g_music_cache[i].pcm = pcm;
            g_music_cache[i].pcm_bytes = pcm_bytes;
            g_music_cache_bytes += pcm_bytes;
            LeaveCriticalSection(&g_music_cs);
            return pcm;
        }
    }
    LeaveCriticalSection(&g_music_cs);
    return music_cache_intern(path, fmt, pcm, pcm_bytes);
}

enum { MUSIC_PREFETCH_N = 8 };
static char g_music_prefetch_keys[MUSIC_PREFETCH_N][260];
static volatile LONG g_music_prefetch_n;

static int music_prefetch_claim(const char *key)
{
    LONG i, n;
    for (i = 0; i < g_music_prefetch_n && i < MUSIC_PREFETCH_N; ++i) {
        if (strcmp(g_music_prefetch_keys[i], key) == 0)
            return 0;
    }
    n = InterlockedIncrement(&g_music_prefetch_n) - 1;
    if (n < 0 || n >= MUSIC_PREFETCH_N) {
        InterlockedDecrement(&g_music_prefetch_n);
        return 0;
    }
    strncpy(g_music_prefetch_keys[n], key, sizeof(g_music_prefetch_keys[0]) - 1);
    g_music_prefetch_keys[n][sizeof(g_music_prefetch_keys[0]) - 1] = '\0';
    return 1;
}

static DWORD WINAPI music_prefetch_full_proc(void *arg)
{
    char *path = (char *)arg;
    HMODULE module = NULL;
    BYTE *raw = NULL, *pcm = NULL;
    DWORD raw_len = 0, pcm_len = 0;
    WAVEFORMATEX fmt;
    LONGLONG t0;
    double ms;

    if (!path)
        goto out;
    module = dm_pin_module((const void *)music_prefetch_full_proc);
    t0 = hitch_qpc_now();
    if (SUCCEEDED(load_wav_file_or_pak(path, &raw, &raw_len)) && raw &&
        decode_to_pcm(raw, raw_len, &fmt, &pcm, &pcm_len, OGG_DECODE_FULL) && pcm) {
        CkPerf *perf;
        pcm = music_cache_upgrade(path, &fmt, pcm, pcm_len);
        ms = hitch_qpc_ms_since(t0);
        log_msg("dm-replace: music prefetch full '%s' pcm=%lu ms=%.1f", path,
                (unsigned long)pcm_len, ms);
        live_cs_enter();
        perf = g_active_perf;
        if (perf)
            ck_perf_addref(perf);
        live_cs_leave();
        if (perf && perf->ds)
            music_cache_build_ds_for_path(perf->ds, path);
        if (perf)
            ck_perf_release(perf);
    }
out:
    free(raw);
    free(path);
    if (module)
        dm_worker_exit(module, 0);
    return 0;
}

void music_prefetch_full_async(const char *path)
{
    char key[260];
    char *dup;
    HANDLE th;
    WAVEFORMATEX fmt;
    BYTE *pcm = NULL;
    DWORD pcm_len = 0;
    DWORD need;

    if (!path || !path[0])
        return;
    music_path_key(path, key, sizeof(key));
    /* Already have ~60s+ — treat as full enough. */
    if (music_cache_get(path, &fmt, &pcm, &pcm_len) && fmt.nAvgBytesPerSec) {
        need = fmt.nAvgBytesPerSec * 60u;
        if (pcm_len >= need)
            return;
    }
    if (!music_prefetch_claim(key))
        return;
    dup = (char *)malloc(strlen(path) + 1);
    if (!dup)
        return;
    strcpy(dup, path);
    th = CreateThread(NULL, 0, music_prefetch_full_proc, dup, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        free(dup);
}

static int music_preload_one(const char *rel, int *n_ok, int *n_skip, int *n_fail)
{
    BYTE *raw = NULL, *pcm = NULL;
    DWORD raw_len = 0, pcm_len = 0;
    WAVEFORMATEX fmt;
    LONGLONG t0;
    double ms;
    int cached = 0;
    if (music_cache_get(rel, &fmt, &pcm, &pcm_len)) {
        (*n_skip)++;
        return 1;
    }
    t0 = hitch_qpc_now();
    if (FAILED(load_wav_file_or_pak(rel, &raw, &raw_len)) || !raw) {
        (*n_fail)++;
        return 0;
    }
    if (!decode_to_pcm(raw, raw_len, &fmt, &pcm, &pcm_len, OGG_DECODE_FULL) || !pcm) {
        free(raw);
        (*n_fail)++;
        return 0;
    }
    free(raw);
    pcm = music_cache_intern_acquire(rel, &fmt, pcm, pcm_len, &cached);
    ms = hitch_qpc_ms_since(t0);
    if (!cached) {
        /* Budget rejected — drop unique buffer to avoid leaking preload allocs. */
        free(pcm);
        (*n_fail)++;
        log_msg("dm-replace: music preload drop '%s' (cache budget)", rel);
        return 0;
    }
    music_cache_release(rel, pcm);
    (*n_ok)++;
    log_msg("dm-replace: music preload '%s' pcm=%lu ms=%.1f", rel, (unsigned long)pcm_len, ms);
    return 1;
}

static DWORD WINAPI music_preload_proc(void *arg)
{
    char names[64][80];
    int n, i, n_ok = 0, n_skip = 0, n_fail = 0;
    HMODULE module = (HMODULE)arg;
    CkPerf *perf;

    music_preload_one("music\\_menu.ogg", &n_ok, &n_skip, &n_fail);
    n = music_list_mission_files(names, 64);
    for (i = 0; i < n; ++i) {
        char rel[260];
        snprintf(rel, sizeof(rel), "music\\%s", names[i]);
        music_preload_one(rel, &n_ok, &n_skip, &n_fail);
        live_cs_enter();
        perf = g_active_perf;
        if (perf)
            ck_perf_addref(perf);
        live_cs_leave();
        if (perf && perf->ds)
            music_cache_build_ds_for_path(perf->ds, rel);
        if (perf)
            ck_perf_release(perf);
    }
    log_msg("dm-replace: music preload done ok=%d skip=%d fail=%d cached=%d bytes=%lu", n_ok, n_skip,
            n_fail, g_music_cache_n, (unsigned long)g_music_cache_bytes);
    dm_worker_exit(module, 0);
    return 0;
}

void music_preload_start(void)
{
    HANDLE th;
    HMODULE module;
    if (InterlockedCompareExchange(&g_music_preload_started, 1, 0) != 0)
        return;
    music_cache_ensure_cs();
    module = dm_pin_module((const void *)music_preload_proc);
    if (!module) {
        InterlockedExchange(&g_music_preload_started, 0);
        return;
    }
    th = CreateThread(NULL, 0, music_preload_proc, module, 0, NULL);
    if (th)
        CloseHandle(th);
    else {
        FreeLibrary(module);
        InterlockedExchange(&g_music_preload_started, 0);
    }
}

static char g_game_dir[MAX_PATH];
static int g_pak_loaded;

enum { PAK_BUF_MAX = 8 };

typedef struct {
    char name[260];
    DWORD off;
    DWORD size;
    BYTE *base; /* owning HMMSYS buffer (Sounds.pak / *_voices.pak) */
} PakEntry;

static BYTE *g_pak_bufs[PAK_BUF_MAX];
static int g_pak_nbufs;
static PakEntry *g_pak_ents;
static int g_pak_nents;
static int g_pak_cap;

void ensure_game_dir(void)
{
    char *slash;
    if (g_game_dir[0])
        return;
    if (!GetModuleFileNameA(NULL, g_game_dir, MAX_PATH))
        g_game_dir[0] = '\0';
    slash = strrchr(g_game_dir, '\\');
    if (!slash)
        slash = strrchr(g_game_dir, '/');
    if (slash)
        slash[1] = '\0';
}

const char *dm_game_dir(void)
{
    ensure_game_dir();
    return g_game_dir;
}

static void norm_pak_key(const char *in, char *out, size_t n)
{
    size_t i, j = 0;
    for (i = 0; in[i] && j + 1 < n; ++i) {
        char c = in[i];
        if (c == '/')
            c = '\\';
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        out[j++] = c;
    }
    out[j] = '\0';
}

/* assets/sounds/foo.wav or Sounds/foo.wav → SOUNDS\FOO.WAV (HMMSYS keys). */
static void path_to_pak_key(const char *in, char *out, size_t n)
{
    char tmp[260];
    const char *p;
    norm_pak_key(in, tmp, sizeof(tmp));
    p = tmp;
    while (p[0] == '.' && p[1] == '\\')
        p += 2;
    if (strncmp(p, "ASSETS\\", 7) == 0)
        p += 7;
    strncpy(out, p, n - 1);
    out[n - 1] = '\0';
}

/* Append one HMMSYS pack into the shared index (Sounds + Local voice paks). */
static int append_hmmsys_pak(const char *path)
{
    HANDLE h;
    DWORD rd = 0, pak_size;
    BYTE *pak_data = NULL;
    const BYTE *d;
    DWORD num, pos, i;
    char prev[260];
    int added = 0;

    if (g_pak_nbufs >= PAK_BUF_MAX)
        return 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                    NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    pak_size = GetFileSize(h, NULL);
    pak_data = (BYTE *)malloc(pak_size ? pak_size : 1);
    if (!pak_data || !ReadFile(h, pak_data, pak_size, &rd, NULL) || rd != pak_size) {
        CloseHandle(h);
        free(pak_data);
        return 0;
    }
    CloseHandle(h);
    d = pak_data;
    if (pak_size < 40 || memcmp(d, "HMMSYS PackFile\n", 16) != 0) {
        free(pak_data);
        return 0;
    }
    num = *(const DWORD *)(d + 32);
    if (num > 200000) {
        free(pak_data);
        return 0;
    }
    if (g_pak_nents + (int)num > g_pak_cap) {
        int ncap = g_pak_cap ? g_pak_cap * 2 : 512;
        PakEntry *ne;
        while (ncap < g_pak_nents + (int)num)
            ncap *= 2;
        ne = (PakEntry *)realloc(g_pak_ents, (size_t)ncap * sizeof(PakEntry));
        if (!ne) {
            free(pak_data);
            return 0;
        }
        g_pak_ents = ne;
        g_pak_cap = ncap;
    }
    pos = 40;
    prev[0] = '\0';
    for (i = 0; i < num && pos + 2 < pak_size; ++i) {
        BYTE nlen = d[pos++];
        BYTE reuse = d[pos++];
        DWORD part = (DWORD)nlen - (DWORD)reuse;
        char name[260];
        DWORD off, size;
        if (reuse > strlen(prev) || part + reuse >= sizeof(name) || pos + part + 8 > pak_size)
            break;
        memcpy(name, prev, reuse);
        memcpy(name + reuse, d + pos, part);
        name[reuse + part] = '\0';
        pos += part;
        off = *(const DWORD *)(d + pos);
        size = *(const DWORD *)(d + pos + 4);
        pos += 8;
        if (off + size > pak_size)
            continue;
        memset(&g_pak_ents[g_pak_nents], 0, sizeof(g_pak_ents[0]));
        strncpy(g_pak_ents[g_pak_nents].name, name, sizeof(g_pak_ents[0].name) - 1);
        g_pak_ents[g_pak_nents].off = off;
        g_pak_ents[g_pak_nents].size = size;
        g_pak_ents[g_pak_nents].base = pak_data;
        g_pak_nents++;
        added++;
        strncpy(prev, name, sizeof(prev) - 1);
    }
    if (added <= 0) {
        free(pak_data);
        return 0;
    }
    g_pak_bufs[g_pak_nbufs++] = pak_data;
    log_msg("dm-replace: HMMSYS indexed %d entries from %s (total %d)", added, path, g_pak_nents);
    /* #region agent log */
    {
        char js[360];
        snprintf(js, sizeof(js),
                 "{\"added\":%d,\"total\":%d,\"bufs\":%d,\"path\":\"%.200s\"}", added, g_pak_nents,
                 g_pak_nbufs, path);
        dm_agent("V1", "dm_replace.c:pak", "hmmsys-index", js);
    }
    /* #endregion */
    return 1;
}

static int load_assets_pak_index(void)
{
    char path[MAX_PATH];
    static const char *const candidates[] = {
        "assets.pak",          /* Imperivm / CK unpacked tree */
        "Packs\\Sounds.pak",   /* Imperivm 2 / tpw retail */
        "Packs/Sounds.pak",
    };
    size_t i;
    WIN32_FIND_DATAA fd;
    HANDLE find;

    if (g_pak_loaded)
        return g_pak_nents > 0;
    g_pak_loaded = 1;
    ensure_game_dir();
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        snprintf(path, sizeof(path), "%s%s", g_game_dir, candidates[i]);
        if (append_hmmsys_pak(path))
            break;
        log_msg("dm-replace: pak miss %s", path);
    }
    /* Unit voices live in Local/<Lang>_voices.pak (keys CURRENTLANG\VOICES\...). */
    snprintf(path, sizeof(path), "%sLocal\\*_voices.pak", g_game_dir);
    find = FindFirstFileA(path, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        snprintf(path, sizeof(path), "%sLocal/*_voices.pak", g_game_dir);
        find = FindFirstFileA(path, &fd);
    }
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            snprintf(path, sizeof(path), "%sLocal\\%s", g_game_dir, fd.cFileName);
            if (!append_hmmsys_pak(path))
                log_msg("dm-replace: voice pak miss %s", path);
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    } else {
        /* #region agent log */
        dm_agent("V1", "dm_replace.c:pak", "voice-pak-miss", "{\"glob\":0}");
        /* #endregion */
        log_msg("dm-replace: no Local/*_voices.pak found");
    }
    return g_pak_nents > 0;
}

static int pak_find(const char *key, const BYTE **data, DWORD *len)
{
    char k[260], k2[260];
    int i;
    if (!load_assets_pak_index())
        return 0;
    path_to_pak_key(key, k, sizeof(k));
    for (i = 0; i < g_pak_nents; ++i) {
        if (strcmp(g_pak_ents[i].name, k) == 0) {
            *data = g_pak_ents[i].base + g_pak_ents[i].off;
            *len = g_pak_ents[i].size;
            return 1;
        }
    }
    /* also try with SOUNDS\ prefix if path was sounds\... already stripped assets */
    if (strncmp(k, "SOUNDS\\", 7) != 0) {
        snprintf(k2, sizeof(k2), "SOUNDS\\%s", k);
        for (i = 0; i < g_pak_nents; ++i) {
            if (strcmp(g_pak_ents[i].name, k2) == 0) {
                *data = g_pak_ents[i].base + g_pak_ents[i].off;
                *len = g_pak_ents[i].size;
                return 1;
            }
        }
    }
    return 0;
}

static int mem_readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const BYTE *b = (const BYTE *)p;
    size_t left = n;
    if (!p || !n)
        return 0;
    while (left) {
        SIZE_T chunk;
        if (!VirtualQuery(b, &mbi, sizeof(mbi)))
            return 0;
        if (mbi.State != MEM_COMMIT)
            return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return 0;
        chunk = (SIZE_T)(((BYTE *)mbi.BaseAddress + mbi.RegionSize) - b);
        if (chunk > left)
            chunk = left;
        b += chunk;
        left -= chunk;
    }
    return 1;
}

static int path_looks_audio(const char *s)
{
    size_t i, n;
    int dot = 0;
    if (!s || (unsigned char)s[0] < 32)
        return 0;
    n = 0;
    while (n < 240 && s[n])
        n++;
    if (n < 5 || n >= 240)
        return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126)
            return 0;
        if (c == '.')
            dot = 1;
    }
    if (!dot)
        return 0;
    return (strstr(s, ".wav") || strstr(s, ".WAV") || strstr(s, ".ogg") || strstr(s, ".OGG") ||
            strstr(s, "Sounds") || strstr(s, "SOUNDS") || strstr(s, "sounds"))
               ? 1
               : 0;
}

int scrape_stream_path(void *stream, char *out, size_t outn)
{
    /* Game wav wrapper stores path at +8 (FUN_0040cfa0). Also probe a few nearby offsets. */
    static const int offs[] = {8, 12, 16, 20, 24, 32, 64, 128, -1};
    int i;
    if (!stream || !out || outn < 8)
        return 0;
    if (!mem_readable(stream, 0x120))
        return 0;
    for (i = 0; offs[i] >= 0; ++i) {
        const char *p = (const char *)stream + offs[i];
        if (!mem_readable(p, 64))
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

HRESULT load_wav_file_or_pak(const char *path, BYTE **out, DWORD *out_len)
{
    char key[260], disk[MAX_PATH], unpacked[MAX_PATH];
    const BYTE *pdata;
    DWORD plen;
    HANDLE h;
    DWORD sz, rd;
    BYTE *buf;

    *out = NULL;
    *out_len = 0;
    if (!path || !path[0])
        return E_FAIL;

    ensure_game_dir();

    /* 1) assets_unpacked — try raw + pak-style key */
    path_to_pak_key(path, key, sizeof(key));
    snprintf(unpacked, sizeof(unpacked), "%sassets_unpacked\\%s", g_game_dir, key);
    h = CreateFileA(unpacked, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(unpacked, sizeof(unpacked), "%s%s", g_game_dir, path);
        {
            char *p;
            for (p = unpacked; *p; ++p)
                if (*p == '/')
                    *p = '\\';
        }
        h = CreateFileA(unpacked, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (h != INVALID_HANDLE_VALUE) {
        sz = GetFileSize(h, NULL);
        buf = (BYTE *)malloc(sz ? sz : 1);
        if (buf && ReadFile(h, buf, sz, &rd, NULL) && rd == sz) {
            CloseHandle(h);
            *out = buf;
            *out_len = sz;
            return S_OK;
        }
        free(buf);
        CloseHandle(h);
    }

    /* 2) assets.pak */
    if (pak_find(path, &pdata, &plen)) {
        buf = (BYTE *)malloc(plen);
        if (!buf)
            return E_OUTOFMEMORY;
        memcpy(buf, pdata, plen);
        *out = buf;
        *out_len = plen;
        return S_OK;
    }

    /* 3) raw under game dir (music ogg files) */
    snprintf(disk, sizeof(disk), "%s%s", g_game_dir, path);
    {
        char *p;
        for (p = disk; *p; ++p)
            if (*p == '/')
                *p = '\\';
    }
    h = CreateFileA(disk, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return E_FAIL;
    sz = GetFileSize(h, NULL);
    buf = (BYTE *)malloc(sz ? sz : 1);
    if (!buf || !ReadFile(h, buf, sz, &rd, NULL) || rd != sz) {
        free(buf);
        CloseHandle(h);
        return E_FAIL;
    }
    CloseHandle(h);
    *out = buf;
    *out_len = sz;
    return S_OK;
}

static HRESULT load_audio_from_stream(void *stream, BYTE **out, DWORD *out_len, char *path_out,
                                      size_t path_n)
{
    HRESULT hr;
    char spath[260];

    path_out[0] = '\0';
    *out = NULL;
    *out_len = 0;

    if (!scrape_stream_path(stream, spath, sizeof(spath))) {
        dm_agent("R2b", "dm_replace.c:load", "no-path", "{\"fail\":1}");
        return E_FAIL;
    }
    strncpy(path_out, spath, path_n - 1);
    path_out[path_n - 1] = '\0';
    hr = load_wav_file_or_pak(spath, out, out_len);
    if (SUCCEEDED(hr)) {
        static volatile LONG s_load_log;
        LONG ln = InterlockedIncrement(&s_load_log);
        if (ln <= 30 || (ln % 50) == 0) {
            char js[300];
            snprintf(js, sizeof(js), "{\"hr\":0,\"path\":\"%.180s\",\"len\":%lu,\"n\":%ld}", spath,
                     (unsigned long)(*out_len), (long)ln);
            dm_agent("R2b", "dm_replace.c:load", "path-load", js);
            log_msg("dm-replace: loaded '%s' (%lu bytes)", spath, (unsigned long)*out_len);
        }
    } else {
        char js[300];
        snprintf(js, sizeof(js), "{\"hr\":%ld,\"path\":\"%.180s\"}", (long)hr, spath);
        dm_agent("R2b", "dm_replace.c:load", "path-load-fail", js);
    }
    return hr;
}

