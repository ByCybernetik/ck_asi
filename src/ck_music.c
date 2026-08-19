/*
 * Self-contained streaming music player.
 * Decodes OGG via stb_vorbis pull API on a dedicated thread,
 * outputs through a DirectSound streaming (circular) buffer.
 * No dependency on COM stubs or GetObject/PlaySegmentEx flow.
 */
#include "ck_music.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* stb_vorbis forward declarations */
typedef struct stb_vorbis stb_vorbis;
typedef struct { unsigned int sample_rate; int channels; } stb_vorbis_info;
extern stb_vorbis *stb_vorbis_open_memory(const unsigned char *, int, int *, const void *);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *);
extern int stb_vorbis_get_frame_float(stb_vorbis *, int *, float ***);
extern void stb_vorbis_close(stb_vorbis *);
extern int stb_vorbis_seek_start(stb_vorbis *);

enum {
    CKM_OUT_HZ     = 44100,
    CKM_OUT_CH     = 2,
    CKM_OUT_BPS    = 16,
    CKM_BLOCK      = CKM_OUT_CH * (CKM_OUT_BPS / 8),
    CKM_BUF_MS     = 200,
    CKM_BUF_FRAMES = (CKM_OUT_HZ * CKM_BUF_MS / 1000),
    CKM_BUF_BYTES  = CKM_BUF_FRAMES * CKM_BLOCK,
    CKM_HALF_BYTES = CKM_BUF_BYTES / 2,
    CKM_DECODE_FRAMES = 2048
};

typedef struct {
    LPDIRECTSOUND ds;
    LPDIRECTSOUNDBUFFER buf;
    HANDLE thread;
    HANDLE wake;
    CRITICAL_SECTION lock;
    volatile LONG run;
    volatile LONG playing;

    stb_vorbis *vorbis;
    BYTE *ogg_data;
    DWORD ogg_len;
    int src_ch;
    int src_rate;
    int loop;
    LONG vol;

    float *decode_tmp;
    double resample_frac;
    int decode_pos;
    int decode_len;

    DWORD write_cursor;
    int last_half;

    CkMusicEndCallback end_cb;
    void *end_ctx;
} CkMusicPlayer;

static CkMusicPlayer g_mp;

static float dsvol_linear(LONG vol)
{
    if (vol <= -10000) return 0.0f;
    if (vol >= 0)      return 1.0f;
    return (float)pow(10.0, (double)vol / 2000.0);
}

static void ckm_downmix_frame(float **src, int src_ch, int frames, float *dst)
{
    int i;
    if (!dst || frames <= 0)
        return;
    for (i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        if (src_ch <= 0 || !src) {
            dst[i * 2 + 0] = 0.0f;
            dst[i * 2 + 1] = 0.0f;
            continue;
        }
        if (src_ch == 1) {
            float m = src[0] ? src[0][i] : 0.0f;
            l = m;
            r = m;
        } else {
            l = src[0] ? src[0][i] : 0.0f;
            r = src[1] ? src[1][i] : 0.0f;
            if (src_ch >= 3 && src[2]) {
                float c = src[2][i] * 0.5f;
                l += c;
                r += c;
            }
            if (src_ch >= 4 && src[3])
                l += src[3][i] * 0.5f;
            if (src_ch >= 5 && src[4])
                r += src[4][i] * 0.5f;
        }
        dst[i * 2 + 0] = l;
        dst[i * 2 + 1] = r;
    }
}

static int ckm_decode_more(CkMusicPlayer *p)
{
    int got = 0, ch = 0, tries;
    float **outs = NULL;
    if (!p->vorbis) return 0;
    for (tries = 0; tries < 8; ++tries) {
        log_msg("ck_music: get_frame_float enter vorbis=%p pos=%d len=%d", (void *)p->vorbis, p->decode_pos, p->decode_len);
        got = stb_vorbis_get_frame_float(p->vorbis, &ch, &outs);
        log_msg("ck_music: get_frame_float exit got=%d ch=%d outs=%p", got, ch, (void *)outs);
        if (got > 0)
            break;
    }
    if (got <= 0) return 0;
    if (got > CKM_DECODE_FRAMES)
        got = CKM_DECODE_FRAMES;
    ckm_downmix_frame(outs, ch, got, p->decode_tmp);
    p->decode_pos = 0;
    p->decode_len = got;
    return got;
}

static void ckm_read_sample(CkMusicPlayer *p, float *l, float *r)
{
    *l = 0.0f; *r = 0.0f;
    if (p->decode_pos >= p->decode_len) {
        if (!ckm_decode_more(p)) {
            if (p->loop) {
                stb_vorbis_seek_start(p->vorbis);
                if (!ckm_decode_more(p)) { p->playing = 0; return; }
            } else {
                p->playing = 0;
                return;
            }
        }
    }
    {
        int idx = p->decode_pos * CKM_OUT_CH;
        *l = p->decode_tmp[idx];
        *r = p->decode_tmp[idx + 1];
    }
}

static void ckm_fill_half(CkMusicPlayer *p, BYTE *dst, DWORD bytes)
{
    short *out = (short *)dst;
    DWORD frames = bytes / CKM_BLOCK;
    DWORD f;
    float gain;
    if (!out || !bytes || !p->decode_tmp) return;
    gain = dsvol_linear(p->vol);
    double step = (p->src_rate > 0)
                  ? (double)p->src_rate / (double)CKM_OUT_HZ
                  : 1.0;

    for (f = 0; f < frames; ++f) {
        float l, r;
        while (p->resample_frac >= 1.0) {
            p->decode_pos++;
            p->resample_frac -= 1.0;
        }
        if (!p->playing) {
            memset(out + f * CKM_OUT_CH, 0, (frames - f) * CKM_BLOCK);
            return;
        }
        ckm_read_sample(p, &l, &r);
        l *= gain;
        r *= gain;
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        out[f * 2]     = (short)(l * 32767.0f);
        out[f * 2 + 1] = (short)(r * 32767.0f);
        p->resample_frac += step;
    }
}

static DWORD WINAPI ckm_thread(void *arg)
{
    CkMusicPlayer *p = (CkMusicPlayer *)arg;
    int was_playing = 0;

    while (p->run) {
        DWORD play_pos = 0, write_pos = 0;
        int cur_half;
        LPVOID ptr1 = NULL, ptr2 = NULL;
        DWORD len1 = 0, len2 = 0;

        WaitForSingleObject(p->wake, 20);

        if (!p->buf) continue;

        IDirectSoundBuffer_GetCurrentPosition(p->buf, &play_pos, &write_pos);
        cur_half = (play_pos >= (DWORD)CKM_HALF_BYTES) ? 1 : 0;

        if (cur_half == p->last_half)
            continue;
        p->last_half = cur_half;

        {
            DWORD lock_off = cur_half ? 0 : (DWORD)CKM_HALF_BYTES;
            HRESULT hr;

            EnterCriticalSection(&p->lock);
            hr = IDirectSoundBuffer_Lock(p->buf, lock_off, CKM_HALF_BYTES,
                                         &ptr1, &len1, &ptr2, &len2, 0);
            log_msg("ck_music: DS lock hr=0x%08lx off=%lu req=%u p1=%p n1=%lu p2=%p n2=%lu playing=%ld",
                    (unsigned long)hr, (unsigned long)lock_off, (unsigned)CKM_HALF_BYTES,
                    ptr1, (unsigned long)len1, ptr2, (unsigned long)len2, (long)p->playing);
            if (SUCCEEDED(hr) && ptr1 && len1) {
                if (p->playing && p->vorbis && p->decode_tmp) {
                    ckm_fill_half(p, (BYTE *)ptr1, len1);
                    if (ptr2 && len2)
                        ckm_fill_half(p, (BYTE *)ptr2, len2);
                } else {
                    memset(ptr1, 0, len1);
                    if (ptr2 && len2) memset(ptr2, 0, len2);
                }
                IDirectSoundBuffer_Unlock(p->buf, ptr1, len1, ptr2, len2);
                log_msg("ck_music: DS unlock done");
            } else if (SUCCEEDED(hr)) {
                IDirectSoundBuffer_Unlock(p->buf, ptr1, len1, ptr2, len2);
                log_msg("ck_music: DS unlock empty-lock done");
            }

            if (was_playing && !p->playing) {
                CkMusicEndCallback cb = p->end_cb;
                void *ctx = p->end_ctx;
                LeaveCriticalSection(&p->lock);
                if (cb) cb(ctx);
            } else {
                LeaveCriticalSection(&p->lock);
            }
            was_playing = p->playing;
        }
    }
    return 0;
}

int ck_music_init(LPDIRECTSOUND ds)
{
    CkMusicPlayer *p = &g_mp;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;
    HRESULT hr;

    if (p->thread) return 1;
    memset(p, 0, sizeof(*p));
    if (!ds) return 0;
    p->ds = ds;

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels        = CKM_OUT_CH;
    wfx.nSamplesPerSec   = CKM_OUT_HZ;
    wfx.wBitsPerSample   = CKM_OUT_BPS;
    wfx.nBlockAlign      = CKM_BLOCK;
    wfx.nAvgBytesPerSec  = CKM_OUT_HZ * CKM_BLOCK;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize          = sizeof(desc);
    desc.dwFlags         = DSBCAPS_LOCSOFTWARE | DSBCAPS_GLOBALFOCUS |
                           DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes   = CKM_BUF_BYTES;
    desc.lpwfxFormat     = &wfx;

    hr = IDirectSound_CreateSoundBuffer(ds, &desc, &p->buf, NULL);
    if (FAILED(hr) || !p->buf) {
        log_msg("ck_music: CreateSoundBuffer failed 0x%08lx", (unsigned long)hr);
        return 0;
    }

    {
        LPVOID ptr; DWORD len;
        if (SUCCEEDED(IDirectSoundBuffer_Lock(p->buf, 0, CKM_BUF_BYTES, &ptr, &len, NULL, NULL, 0))) {
            memset(ptr, 0, len);
            IDirectSoundBuffer_Unlock(p->buf, ptr, len, NULL, 0);
        }
    }

    IDirectSoundBuffer_Play(p->buf, 0, 0, DSBPLAY_LOOPING);

    InitializeCriticalSection(&p->lock);
    p->decode_tmp = (float *)malloc(CKM_DECODE_FRAMES * CKM_OUT_CH * sizeof(float));
    if (!p->decode_tmp) {
        IDirectSoundBuffer_Release(p->buf);
        p->buf = NULL;
        return 0;
    }
    p->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    p->run = 1;
    p->last_half = -1;
    p->thread = CreateThread(NULL, 0, ckm_thread, p, 0, NULL);
    if (!p->thread) {
        p->run = 0;
        IDirectSoundBuffer_Release(p->buf);
        p->buf = NULL;
        free(p->decode_tmp);
        return 0;
    }
    log_msg("ck_music: init ok buf=%u frames=%u", (unsigned)CKM_BUF_BYTES, (unsigned)CKM_BUF_FRAMES);
    return 1;
}

void ck_music_shutdown(void)
{
    CkMusicPlayer *p = &g_mp;
    /* Stop worker and wait until it exits before freeing decode state.
     * Previously we used a short timeout and freed/zeroed fields while the
     * thread could still be inside stb_vorbis → crash in stb_vorbis. */
    log_msg("ck_music: shutdown enter thread=%p playing=%ld", (void *)p->thread, (long)p->playing);
    EnterCriticalSection(&p->lock);
    p->playing = 0;
    p->run = 0;
    LeaveCriticalSection(&p->lock);

    if (p->wake)
        SetEvent(p->wake);

    if (p->thread) {
        WaitForSingleObject(p->thread, INFINITE);
        CloseHandle(p->thread);
        p->thread = NULL;
    }

    /* From this point worker can't access p->vorbis/p->ogg_data/p->decode_tmp. */
    EnterCriticalSection(&p->lock);
    if (p->vorbis) { stb_vorbis_close(p->vorbis); p->vorbis = NULL; }
    free(p->ogg_data); p->ogg_data = NULL;
    LeaveCriticalSection(&p->lock);
    if (p->buf) { IDirectSoundBuffer_Stop(p->buf); IDirectSoundBuffer_Release(p->buf); }
    free(p->decode_tmp);
    if (p->wake) CloseHandle(p->wake);
    DeleteCriticalSection(&p->lock);
    memset(p, 0, sizeof(*p));
    log_msg("ck_music: shutdown done");
}

int ck_music_play(const char *path, int loop, LONG vol_mb)
{
    CkMusicPlayer *p = &g_mp;
    HANDLE hf;
    DWORD sz, rd;
    BYTE *data;
    stb_vorbis *v;
    stb_vorbis_info vi;
    int err = 0;
    char full[MAX_PATH];

    if (!p->thread || !path || !path[0])
        return 0;

    {
        extern void ensure_game_dir(void);
        extern const char *dm_game_dir(void);
        const char *root;
        ensure_game_dir();
        root = dm_game_dir();
        if (root && root[0])
            snprintf(full, sizeof(full), "%s%s", root, path);
        else
            snprintf(full, sizeof(full), "%s", path);
        { char *c; for (c = full; *c; ++c) if (*c == '/') *c = '\\'; }
    }

    hf = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_msg("ck_music: open failed '%s'", full);
        return 0;
    }
    sz = GetFileSize(hf, NULL);
    data = (BYTE *)malloc(sz);
    if (!data || !ReadFile(hf, data, sz, &rd, NULL) || rd != sz) {
        CloseHandle(hf);
        free(data);
        log_msg("ck_music: read failed '%s' sz=%lu", full, (unsigned long)sz);
        return 0;
    }
    CloseHandle(hf);

    v = stb_vorbis_open_memory(data, (int)sz, &err, NULL);
    if (!v) {
        free(data);
        log_msg("ck_music: vorbis open failed '%s' err=%d", full, err);
        return 0;
    }
    vi = stb_vorbis_get_info(v);
    if (vi.channels < 1 || vi.sample_rate < 1) {
        stb_vorbis_close(v);
        free(data);
        return 0;
    }

    EnterCriticalSection(&p->lock);
    p->playing = 0;
    if (p->vorbis) stb_vorbis_close(p->vorbis);
    free(p->ogg_data);
    p->vorbis   = v;
    p->ogg_data = data;
    p->ogg_len  = sz;
    p->src_ch   = vi.channels;
    p->src_rate = (int)vi.sample_rate;
    p->loop     = loop;
    p->vol      = vol_mb;
    p->decode_pos = 0;
    p->decode_len = 0;
    p->resample_frac = 0.0;
    p->playing  = 1;
    LeaveCriticalSection(&p->lock);

    if (p->wake) SetEvent(p->wake);
    log_msg("ck_music: play '%s' %dch %dHz loop=%d vol=%ld",
            path, vi.channels, (int)vi.sample_rate, loop, (long)vol_mb);
    return 1;
}

void ck_music_stop(void)
{
    CkMusicPlayer *p = &g_mp;
    if (!p->thread) return;
    EnterCriticalSection(&p->lock);
    p->playing = 0;
    if (p->vorbis) { stb_vorbis_close(p->vorbis); p->vorbis = NULL; }
    free(p->ogg_data); p->ogg_data = NULL;
    LeaveCriticalSection(&p->lock);
}

int ck_music_is_playing(void)
{
    return g_mp.thread && g_mp.playing;
}

void ck_music_set_volume(LONG vol_mb)
{
    g_mp.vol = vol_mb;
}

void ck_music_set_end_callback(CkMusicEndCallback cb, void *ctx)
{
    g_mp.end_cb  = cb;
    g_mp.end_ctx = ctx;
}
