#include "dm_replace_internal.h"

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <math.h>

typedef struct stb_vorbis stb_vorbis;
typedef struct {
    unsigned int sample_rate;
    int channels;
} stb_vorbis_info;
extern stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error,
                                           const void *alloc);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer,
                                                     int num_shorts);
extern void stb_vorbis_close(stb_vorbis *f);
extern int stb_vorbis_seek_start(stb_vorbis *f);
extern float stb_vorbis_stream_length_in_seconds(stb_vorbis *f);

typedef struct {
    IMMDevice *dev;
    IAudioClient *client;
    IAudioRenderClient *render;
    WAVEFORMATEX *mixfmt;
    HANDLE ev;
    HANDLE thread;
    CRITICAL_SECTION lock;
    int lock_ready;
    volatile LONG run;
    volatile LONG active;

    stb_vorbis *vorbis;
    BYTE *ogg_data;
    DWORD ogg_len;
    int vorbis_ch;
    int vorbis_rate;
    int loop;
    LONG vol;
    int mix_float;

    short *decode_buf;
    int decode_buf_cap;
    int decode_buf_pos;
    int decode_buf_len;
    double resample_frac;
} WasapiState;

static WasapiState g_wasapi;

enum { DECODE_CHUNK = 2048 };

static float dsvol_to_linear(LONG vol)
{
    if (vol <= -10000)
        return 0.0f;
    if (vol >= 0)
        return 1.0f;
    return (float)pow(10.0, (double)vol / 2000.0);
}

static int wasapi_decode_more(WasapiState *s)
{
    int got;
    if (!s->vorbis)
        return 0;
    got = stb_vorbis_get_samples_short_interleaved(
        s->vorbis, s->vorbis_ch, s->decode_buf,
        DECODE_CHUNK * s->vorbis_ch);
    if (got <= 0)
        return 0;
    s->decode_buf_pos = 0;
    s->decode_buf_len = got;
    return got;
}

static void wasapi_read_frame(WasapiState *s, float *l, float *r)
{
    int idx, vch;
    short *buf;

    *l = 0.0f;
    *r = 0.0f;

    if (s->decode_buf_pos >= s->decode_buf_len) {
        if (!wasapi_decode_more(s)) {
            if (s->loop) {
                stb_vorbis_seek_start(s->vorbis);
                if (!wasapi_decode_more(s)) {
                    s->active = 0;
                    return;
                }
            } else {
                s->active = 0;
                return;
            }
        }
    }

    vch = s->vorbis_ch;
    buf = s->decode_buf;
    idx = s->decode_buf_pos * vch;

    if (vch >= 2) {
        *l = (float)buf[idx + 0] / 32768.0f;
        *r = (float)buf[idx + 1] / 32768.0f;
    } else {
        *l = *r = (float)buf[idx] / 32768.0f;
    }
}

static void wasapi_fill(BYTE *dst, UINT32 frames)
{
    UINT32 ch, f;
    float gain;
    double step;
    WasapiState *s = &g_wasapi;
    if (!dst || !s->mixfmt)
        return;
    ch = s->mixfmt->nChannels ? s->mixfmt->nChannels : 2;
    gain = dsvol_to_linear(s->vol);

    if (!s->active || !s->vorbis || s->vorbis_rate < 1) {
        if (s->mix_float)
            memset(dst, 0, frames * ch * sizeof(float));
        else
            memset(dst, 0, frames * ch * sizeof(short));
        return;
    }

    step = (double)s->vorbis_rate / (double)s->mixfmt->nSamplesPerSec;

    if (s->mix_float) {
        float *out = (float *)dst;
        for (f = 0; f < frames; ++f) {
            float l, r;
            UINT32 c;
            while (s->resample_frac >= 1.0) {
                s->decode_buf_pos++;
                s->resample_frac -= 1.0;
            }
            wasapi_read_frame(s, &l, &r);
            if (!s->active) {
                for (; f < frames; ++f) {
                    for (c = 0; c < ch; ++c)
                        out[f * ch + c] = 0.0f;
                }
                return;
            }
            l *= gain;
            r *= gain;
            out[f * ch + 0] = l;
            if (ch > 1) out[f * ch + 1] = r;
            for (c = 2; c < ch; ++c)
                out[f * ch + c] = 0.0f;
            s->resample_frac += step;
        }
    } else {
        short *out = (short *)dst;
        for (f = 0; f < frames; ++f) {
            float l, r;
            UINT32 c;
            while (s->resample_frac >= 1.0) {
                s->decode_buf_pos++;
                s->resample_frac -= 1.0;
            }
            wasapi_read_frame(s, &l, &r);
            if (!s->active) {
                for (; f < frames; ++f) {
                    for (c = 0; c < ch; ++c)
                        out[f * ch + c] = 0;
                }
                return;
            }
            l *= gain;
            r *= gain;
            out[f * ch + 0] = (short)(l * 32767.0f);
            if (ch > 1) out[f * ch + 1] = (short)(r * 32767.0f);
            for (c = 2; c < ch; ++c)
                out[f * ch + c] = 0;
            s->resample_frac += step;
        }
    }
}

static DWORD WINAPI wasapi_thread_proc(void *arg)
{
    WasapiState *s = (WasapiState *)arg;
    if (!s->client || !s->render || !s->ev)
        return 0;
    IAudioClient_Start(s->client);
    while (s->run) {
        UINT32 pad = 0, buf_frames = 0, avail = 0;
        BYTE *dst = NULL;
        WaitForSingleObject(s->ev, 50);
        if (FAILED(IAudioClient_GetCurrentPadding(s->client, &pad)))
            continue;
        if (FAILED(IAudioClient_GetBufferSize(s->client, &buf_frames)))
            continue;
        if (pad >= buf_frames)
            continue;
        avail = buf_frames - pad;
        if (FAILED(IAudioRenderClient_GetBuffer(s->render, avail, &dst)))
            continue;
        if (s->lock_ready)
            EnterCriticalSection(&s->lock);
        wasapi_fill(dst, avail);
        if (s->lock_ready)
            LeaveCriticalSection(&s->lock);
        IAudioRenderClient_ReleaseBuffer(s->render, avail, 0);
    }
    IAudioClient_Stop(s->client);
    return 0;
}

int audio_wasapi_init(HWND hwnd)
{
    HRESULT hr;
    IMMDeviceEnumerator *en = NULL;
    REFERENCE_TIME dur = 1000000;
    WAVEFORMATEX *mix = NULL;
    WasapiState *s = &g_wasapi;
    const char *backend = getenv("CK_AUDIO_BACKEND");
    (void)hwnd;
    if (backend && backend[0]) {
        if (backend[0] == 'd' || backend[0] == 'D')
            return 0;
    }
    if (s->client && s->thread)
        return 1;
    memset(s, 0, sizeof(*s));
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr) || !en)
        return 0;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &s->dev);
    if (FAILED(hr) || !s->dev)
        goto fail;
    hr = IMMDevice_Activate(s->dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&s->client);
    if (FAILED(hr) || !s->client)
        goto fail;
    hr = IAudioClient_GetMixFormat(s->client, &mix);
    if (FAILED(hr) || !mix)
        goto fail;
    s->mixfmt = mix;
    hr = IAudioClient_Initialize(s->client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 dur, 0, s->mixfmt, NULL);
    if (FAILED(hr))
        goto fail;
    s->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s->ev)
        goto fail;
    hr = IAudioClient_SetEventHandle(s->client, s->ev);
    if (FAILED(hr))
        goto fail;
    hr = IAudioClient_GetService(s->client, &IID_IAudioRenderClient, (void **)&s->render);
    if (FAILED(hr) || !s->render)
        goto fail;
    InitializeCriticalSection(&s->lock);
    s->lock_ready = 1;
    s->vol = 0;
    s->mix_float = (s->mixfmt->wBitsPerSample == 32) ? 1 : 0;
    s->decode_buf = (short *)malloc(DECODE_CHUNK * 8 * sizeof(short));
    s->decode_buf_cap = DECODE_CHUNK * 8;
    if (!s->decode_buf)
        goto fail;
    s->run = 1;
    s->thread = CreateThread(NULL, 0, wasapi_thread_proc, s, 0, NULL);
    if (!s->thread)
        goto fail;
    IMMDeviceEnumerator_Release(en);
    log_msg("dm-replace: WASAPI streaming init ok mixfmt=%uHz %uch %ubit float=%d",
            (unsigned)s->mixfmt->nSamplesPerSec, (unsigned)s->mixfmt->nChannels,
            (unsigned)s->mixfmt->wBitsPerSample, s->mix_float);
    return 1;
fail:
    if (en)
        IMMDeviceEnumerator_Release(en);
    audio_wasapi_shutdown();
    return 0;
}

void audio_wasapi_shutdown(void)
{
    WasapiState *s = &g_wasapi;
    if (s->run) {
        s->run = 0;
        if (s->ev)
            SetEvent(s->ev);
    }
    if (s->thread) {
        WaitForSingleObject(s->thread, 500);
        CloseHandle(s->thread);
    }
    if (s->lock_ready)
        EnterCriticalSection(&s->lock);
    if (s->vorbis) {
        stb_vorbis_close(s->vorbis);
        s->vorbis = NULL;
    }
    free(s->ogg_data);
    s->ogg_data = NULL;
    s->ogg_len = 0;
    if (s->lock_ready)
        LeaveCriticalSection(&s->lock);
    if (s->render)
        IAudioRenderClient_Release(s->render);
    if (s->client)
        IAudioClient_Release(s->client);
    if (s->dev)
        IMMDevice_Release(s->dev);
    if (s->mixfmt)
        CoTaskMemFree(s->mixfmt);
    if (s->ev)
        CloseHandle(s->ev);
    free(s->decode_buf);
    if (s->lock_ready)
        DeleteCriticalSection(&s->lock);
    memset(s, 0, sizeof(*s));
}

int audio_wasapi_play_ogg(const BYTE *ogg_data, DWORD ogg_len, int loop, LONG vol, DWORD *duration_ms_out)
{
    WasapiState *s = &g_wasapi;
    stb_vorbis *v;
    stb_vorbis_info vi;
    float dur_sec;
    int err = 0;
    int need;
    BYTE *copy;

    if (!s->thread || !ogg_data || !ogg_len)
        return 0;

    copy = (BYTE *)malloc(ogg_len);
    if (!copy)
        return 0;
    memcpy(copy, ogg_data, ogg_len);

    v = stb_vorbis_open_memory(copy, (int)ogg_len, &err, NULL);
    if (!v) {
        free(copy);
        return 0;
    }
    vi = stb_vorbis_get_info(v);
    if (vi.channels < 1 || vi.sample_rate < 1) {
        stb_vorbis_close(v);
        free(copy);
        return 0;
    }
    dur_sec = stb_vorbis_stream_length_in_seconds(v);
    if (duration_ms_out) {
        if (dur_sec > 0.01f)
            *duration_ms_out = (DWORD)(dur_sec * 1000.0f);
        else
            *duration_ms_out = 0;
    }

    EnterCriticalSection(&s->lock);
    need = DECODE_CHUNK * vi.channels;
    if (need > s->decode_buf_cap) {
        short *nb = (short *)realloc(s->decode_buf, (size_t)need * sizeof(short));
        if (!nb) {
            LeaveCriticalSection(&s->lock);
            stb_vorbis_close(v);
            free(copy);
            return 0;
        }
        s->decode_buf = nb;
        s->decode_buf_cap = need;
    }
    if (s->vorbis)
        stb_vorbis_close(s->vorbis);
    free(s->ogg_data);
    s->vorbis = v;
    s->ogg_data = copy;
    s->ogg_len = ogg_len;
    s->vorbis_ch = vi.channels;
    s->vorbis_rate = (int)vi.sample_rate;
    s->loop = loop ? 1 : 0;
    s->vol = vol;
    s->decode_buf_pos = 0;
    s->decode_buf_len = 0;
    s->resample_frac = 0.0;
    s->active = 1;
    LeaveCriticalSection(&s->lock);
    if (s->ev)
        SetEvent(s->ev);
    log_msg("dm-replace: WASAPI play ogg %luB %dch %dHz loop=%d dur=%.1fs",
            (unsigned long)ogg_len, vi.channels, (int)vi.sample_rate, loop, (double)dur_sec);
    return 1;
}

void audio_wasapi_stop(void)
{
    WasapiState *s = &g_wasapi;
    if (!s->thread)
        return;
    EnterCriticalSection(&s->lock);
    s->active = 0;
    if (s->vorbis) {
        stb_vorbis_close(s->vorbis);
        s->vorbis = NULL;
    }
    free(s->ogg_data);
    s->ogg_data = NULL;
    s->ogg_len = 0;
    s->decode_buf_pos = 0;
    s->decode_buf_len = 0;
    s->resample_frac = 0.0;
    LeaveCriticalSection(&s->lock);
}

int audio_wasapi_is_playing(void)
{
    WasapiState *s = &g_wasapi;
    return s->thread && s->active;
}

void audio_wasapi_set_volume(LONG vol)
{
    WasapiState *s = &g_wasapi;
    if (!s->thread)
        return;
    EnterCriticalSection(&s->lock);
    s->vol = vol;
    LeaveCriticalSection(&s->lock);
}
