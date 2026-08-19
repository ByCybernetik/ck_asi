#include "dm_replace_internal.h"

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <math.h>

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
    volatile LONG started;
    volatile LONG active;
    BYTE *src_pcm;
    DWORD src_bytes;
    WAVEFORMATEX src_fmt;
    double src_pos;
    double src_step;
    DWORD src_frames;
    int loop;
    LONG vol;
    int mix_float;
} WasapiState;

static WasapiState g_wasapi;

static float dsvol_to_linear(LONG vol)
{
    if (vol <= -10000)
        return 0.0f;
    if (vol >= 0)
        return 1.0f;
    return (float)pow(10.0, (double)vol / 2000.0);
}

static void wasapi_fill(BYTE *dst, UINT32 frames)
{
    UINT32 ch, f;
    float gain;
    WasapiState *s = &g_wasapi;
    if (!dst || !s->mixfmt)
        return;
    ch = s->mixfmt->nChannels ? s->mixfmt->nChannels : 2;
    gain = dsvol_to_linear(s->vol);

    if (s->mix_float) {
        float *out = (float *)dst;
        for (f = 0; f < frames; ++f) {
            float l = 0.0f, r = 0.0f;
            if (s->active && s->src_pcm && s->src_frames) {
                DWORD i = (DWORD)s->src_pos;
                short *src16 = (short *)s->src_pcm;
                if (i >= s->src_frames) {
                    if (s->loop) {
                        s->src_pos = 0.0;
                        i = 0;
                    } else {
                        s->active = 0;
                    }
                }
                if (s->active) {
                    if (s->src_fmt.nChannels >= 2) {
                        l = (float)src16[i * 2 + 0] / 32768.0f;
                        r = (float)src16[i * 2 + 1] / 32768.0f;
                    } else {
                        l = r = (float)src16[i] / 32768.0f;
                    }
                    s->src_pos += s->src_step;
                }
            }
            l *= gain;
            r *= gain;
            out[f * ch + 0] = l;
            if (ch > 1)
                out[f * ch + 1] = r;
            for (UINT32 c = 2; c < ch; ++c)
                out[f * ch + c] = 0.5f * (l + r);
        }
    } else {
        short *out = (short *)dst;
        for (f = 0; f < frames; ++f) {
            int l = 0, r = 0;
            if (s->active && s->src_pcm && s->src_frames) {
                DWORD i = (DWORD)s->src_pos;
                short *src16 = (short *)s->src_pcm;
                if (i >= s->src_frames) {
                    if (s->loop) {
                        s->src_pos = 0.0;
                        i = 0;
                    } else {
                        s->active = 0;
                    }
                }
                if (s->active) {
                    if (s->src_fmt.nChannels >= 2) {
                        l = src16[i * 2 + 0];
                        r = src16[i * 2 + 1];
                    } else {
                        l = r = src16[i];
                    }
                    s->src_pos += s->src_step;
                }
            }
            l = (int)((float)l * gain);
            r = (int)((float)r * gain);
            out[f * ch + 0] = (short)l;
            if (ch > 1)
                out[f * ch + 1] = (short)r;
            for (UINT32 c = 2; c < ch; ++c)
                out[f * ch + c] = (short)((l + r) / 2);
        }
    }
}

static DWORD WINAPI wasapi_thread_proc(void *arg)
{
    WasapiState *s = (WasapiState *)arg;
    if (!s->client || !s->render || !s->ev)
        return 0;
    IAudioClient_Start(s->client);
    s->started = 1;
    while (s->run) {
        UINT32 pad = 0, frames = 0, avail = 0;
        BYTE *dst = NULL;
        WaitForSingleObject(s->ev, 50);
        if (FAILED(IAudioClient_GetCurrentPadding(s->client, &pad)))
            continue;
        if (FAILED(IAudioClient_GetBufferSize(s->client, &frames)))
            continue;
        if (pad >= frames)
            continue;
        avail = frames - pad;
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
    s->started = 0;
    return 0;
}

int audio_wasapi_init(HWND hwnd)
{
    HRESULT hr;
    IMMDeviceEnumerator *en = NULL;
    REFERENCE_TIME dur = 1000000; /* 100ms */
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
    s->run = 1;
    s->thread = CreateThread(NULL, 0, wasapi_thread_proc, s, 0, NULL);
    if (!s->thread)
        goto fail;
    IMMDeviceEnumerator_Release(en);
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
    if (s->lock_ready)
        DeleteCriticalSection(&s->lock);
    memset(s, 0, sizeof(*s));
}

int audio_wasapi_play(const BYTE *pcm, DWORD pcm_bytes, const WAVEFORMATEX *fmt, int loop, LONG vol)
{
    WasapiState *s = &g_wasapi;
    if (!s->thread || !fmt || !pcm || !pcm_bytes || fmt->nBlockAlign == 0 || fmt->nSamplesPerSec == 0)
        return 0;
    EnterCriticalSection(&s->lock);
    s->src_pcm = (BYTE *)pcm;
    s->src_bytes = pcm_bytes;
    s->src_fmt = *fmt;
    s->src_frames = pcm_bytes / fmt->nBlockAlign;
    s->src_pos = 0.0;
    s->src_step = (double)fmt->nSamplesPerSec / (double)s->mixfmt->nSamplesPerSec;
    s->loop = loop ? 1 : 0;
    s->vol = vol;
    s->active = 1;
    LeaveCriticalSection(&s->lock);
    if (s->ev)
        SetEvent(s->ev);
    return 1;
}

void audio_wasapi_stop(void)
{
    WasapiState *s = &g_wasapi;
    if (!s->thread)
        return;
    EnterCriticalSection(&s->lock);
    s->active = 0;
    s->src_pcm = NULL;
    s->src_bytes = 0;
    s->src_frames = 0;
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
