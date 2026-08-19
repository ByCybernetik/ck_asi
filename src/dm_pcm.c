#include "dm_replace_internal.h"

LONG path_clamp_vol(LONG vol)
{
    if (vol < -10000)
        return -10000;
    if (vol > 0)
        return 0;
    return vol;
}

LONG path_clamp_pan(LONG pan)
{
    if (pan < -10000)
        return -10000;
    if (pan > 10000)
        return 10000;
    return pan;
}

void path_apply_buf_vol(LPDIRECTSOUNDBUFFER buf, LONG vol)
{
    if (buf)
        IDirectSoundBuffer_SetVolume(buf, path_clamp_vol(vol));
}

/* Wine DS SetPan on secondary buffers is inaudible (H-E, runtime 764ba7). Software L/R. */
void pan_gains_q15(LONG pan, int *lg, int *rg)
{
    pan = path_clamp_pan(pan);
    if (pan <= 0) {
        *lg = 32768;
        *rg = (int)(((LONGLONG)(10000 + pan) * 32768LL) / 10000LL);
    } else {
        *lg = (int)(((LONGLONG)(10000 - pan) * 32768LL) / 10000LL);
        *rg = 32768;
    }
}

static short pcm_mul_q15(short s, int g)
{
    int v = (int)(((LONGLONG)s * (LONGLONG)g) >> 15);
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (short)v;
}

/* 16-bit PCM → interleaved stereo with pan. Returns stereo byte count, 0 on fail. */
DWORD pcm_render_panned_stereo(const BYTE *src, const WAVEFORMATEX *fmt, DWORD src_bytes,
                                      BYTE *dst, DWORD dst_cap, LONG pan)
{
    int lg, rg;
    DWORD i, frames;
    const short *in;
    short *out;

    if (!src || !fmt || !dst || fmt->wBitsPerSample != 16 || fmt->nChannels < 1 ||
        fmt->nChannels > 2 || src_bytes < 2)
        return 0;
    pan_gains_q15(pan, &lg, &rg);
    in = (const short *)src;
    out = (short *)dst;
    if (fmt->nChannels == 1) {
        frames = src_bytes / 2u;
        if (dst_cap < frames * 4u)
            return 0;
        for (i = 0; i < frames; ++i) {
            out[2 * i] = pcm_mul_q15(in[i], lg);
            out[2 * i + 1] = pcm_mul_q15(in[i], rg);
        }
        return frames * 4u;
    }
    frames = src_bytes / 4u;
    if (dst_cap < frames * 4u)
        return 0;
    for (i = 0; i < frames; ++i) {
        out[2 * i] = pcm_mul_q15(in[2 * i], lg);
        out[2 * i + 1] = pcm_mul_q15(in[2 * i + 1], rg);
    }
    return frames * 4u;
}

void stereo_wfx_from(const WAVEFORMATEX *src, WAVEFORMATEX *dst)
{
    *dst = *src;
    dst->nChannels = 2;
    dst->nBlockAlign = (WORD)((dst->wBitsPerSample / 8) * 2);
    dst->nAvgBytesPerSec = dst->nSamplesPerSec * dst->nBlockAlign;
}

int ds_buf_write_all(LPDIRECTSOUNDBUFFER buf, const BYTE *data, DWORD bytes)
{
    LPVOID p1 = NULL, p2 = NULL;
    DWORD n1 = 0, n2 = 0;
    if (!buf || !data || !bytes)
        return 0;
    if (FAILED(IDirectSoundBuffer_Lock(buf, 0, bytes, &p1, &n1, &p2, &n2, 0)))
        return 0;
    if (p1 && n1)
        memcpy(p1, data, n1 > bytes ? bytes : n1);
    if (p2 && n2 && n1 < bytes)
        memcpy(p2, data + n1, n2 > (bytes - n1) ? (bytes - n1) : n2);
    IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
    return 1;
}

void path_apply_buf_pan(LPDIRECTSOUNDBUFFER buf, LONG pan)
{
    /* Kept for ctrl silence buffers; live SFX use software pan (H-E). */
    if (buf)
        IDirectSoundBuffer_SetPan(buf, path_clamp_pan(pan));
}

/* Beacon: dual L/R half-buffers + SetVolume (frame tick; no Stop/rewrite lag). */
/* DS SetVolume is centibels (1/100 dB), NOT linear amplitude (H-L: linear map → hard L/R). */
LONG q15_to_dsvol(int g)
{
    double r;
    LONG v;

    if (g <= 0)
        return -10000;
    if (g >= 32768)
        return 0;
    r = (double)g / 32768.0;
    /* 20*log10(r) in dB → *100 for DS units */
    v = (LONG)(2000.0 * log10(r) + (r < 1.0 ? -0.5 : 0.5));
    if (v < -10000)
        return -10000;
    if (v > 0)
        return 0;
    return v;
}

DWORD pcm_render_one_channel(const BYTE *src, const WAVEFORMATEX *fmt, DWORD src_bytes,
                                    BYTE *dst, DWORD dst_cap, int which)
{
    DWORD i, frames;
    const short *in;
    short *out;

    if (!src || !fmt || !dst || fmt->wBitsPerSample != 16 || which < 0 || which > 1)
        return 0;
    in = (const short *)src;
    out = (short *)dst;
    if (fmt->nChannels == 1) {
        frames = src_bytes / 2u;
        if (dst_cap < frames * 4u)
            return 0;
        for (i = 0; i < frames; ++i) {
            out[2 * i] = (which == 0) ? in[i] : 0;
            out[2 * i + 1] = (which == 1) ? in[i] : 0;
        }
        return frames * 4u;
    }
    if (fmt->nChannels != 2)
        return 0;
    frames = src_bytes / 4u;
    if (dst_cap < frames * 4u)
        return 0;
    for (i = 0; i < frames; ++i) {
        short m = (short)(((int)in[2 * i] + (int)in[2 * i + 1]) / 2);
        out[2 * i] = (which == 0) ? m : 0;
        out[2 * i + 1] = (which == 1) ? m : 0;
    }
    return frames * 4u;
}

int path_is_rhastatus(const char *p)
{
    size_t i, n;
    if (!p || !p[0])
        return 0;
    n = strlen(p);
    for (i = 0; i + 9 <= n; ++i) {
        char u[10];
        size_t k;
        for (k = 0; k < 9; ++k)
            u[k] = (char)toupper((unsigned char)p[i + k]);
        u[9] = '\0';
        if (memcmp(u, "RHASTATUS", 9) == 0)
            return 1;
    }
    return 0;
}

/* Unit speech under CurrentLang/voices/... or VOICES\... — not music/SFX. */
int path_is_unit_voice(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "VOICES\\") != NULL || strstr(t, "\\VOICES") != NULL;
}

/* Building/object selection + gates — same world pan as units (SOUNDS\SELECTION\...). */
int path_is_building_sfx(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "SELECTION\\") != NULL || strstr(t, "\\SELECTION") != NULL ||
           strstr(t, "GATES\\") != NULL || strstr(t, "\\GATES") != NULL;
}

/* Birds/waves/wind + map animals — world-positioned environment (not UI/music). */
int path_is_ambient_sfx(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "AMBIENT\\") != NULL || strstr(t, "\\AMBIENT") != NULL ||
           strstr(t, "ANIMALS\\") != NULL || strstr(t, "\\ANIMALS") != NULL;
}

/* Army foot/horse movement (SOUNDS\WALK\...). */
int path_is_walk_sfx(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "WALK\\") != NULL || strstr(t, "\\WALK") != NULL;
}

/* Melee/arrows/combat hits (SOUNDS\FIGHT\...). */
int path_is_fight_sfx(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "FIGHT\\") != NULL || strstr(t, "\\FIGHT") != NULL;
}

/* Unit/ship death cries (SOUNDS\DEATH\...). */
int path_is_death_sfx(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "DEATH\\") != NULL || strstr(t, "\\DEATH") != NULL;
}

/* World FX: catapult, fire, explosions, heal/item (SOUNDS\EFFECTS\...). */
int path_is_effects_sfx(const char *p)
{
    char t[260];
    size_t i;
    if (!p || !p[0])
        return 0;
    for (i = 0; p[i] && i + 1 < sizeof(t); ++i) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        t[i] = c;
    }
    t[i] = '\0';
    return strstr(t, "EFFECTS\\") != NULL || strstr(t, "\\EFFECTS") != NULL;
}

int path_want_spatial(const char *p)
{
    return path_is_unit_voice(p) || path_is_building_sfx(p) || path_is_ambient_sfx(p) ||
           path_is_walk_sfx(p) || path_is_fight_sfx(p) || path_is_death_sfx(p) ||
           path_is_effects_sfx(p);
}

int path_is_music(const char *path)
{
    char t[260];
    size_t i;
    if (!path)
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
    return strstr(t, "MUSIC\\") != NULL || strncmp(t, "MUSIC", 5) == 0;
}

