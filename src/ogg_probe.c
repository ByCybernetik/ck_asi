#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_vorbis.c"

static short pcm_float_to_s16(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (short)(v * 32767.0f);
}

static void downmix_frame_to_s16(float **src, int src_ch, int frames, short *dst)
{
    int i;
    if (!dst || frames <= 0)
        return;
    for (i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        if (src_ch <= 0 || !src) {
            dst[i * 2 + 0] = 0;
            dst[i * 2 + 1] = 0;
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
        dst[i * 2 + 0] = pcm_float_to_s16(l);
        dst[i * 2 + 1] = pcm_float_to_s16(r);
    }
}

int main(int argc, char **argv)
{
    FILE *in, *out;
    long sz;
    unsigned char *data;
    stb_vorbis *v;
    stb_vorbis_info vi;
    int err = 0;
    long frames_total = 0;
    short *pcm = NULL;
    size_t pcm_cap = 0, pcm_len = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s input.ogg output.raw\n", argv[0]);
        return 2;
    }

    in = fopen(argv[1], "rb");
    if (!in) {
        perror("fopen input");
        return 1;
    }
    fseek(in, 0, SEEK_END);
    sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(in);
        return 1;
    }
    data = (unsigned char *)malloc((size_t)sz);
    if (!data) {
        fclose(in);
        return 1;
    }
    if (fread(data, 1, (size_t)sz, in) != (size_t)sz) {
        fclose(in);
        free(data);
        return 1;
    }
    fclose(in);

    v = stb_vorbis_open_memory(data, (int)sz, &err, NULL);
    if (!v) {
        fprintf(stderr, "open_memory failed err=%d\n", err);
        free(data);
        return 1;
    }
    vi = stb_vorbis_get_info(v);
    fprintf(stderr, "channels=%d rate=%u\n", vi.channels, vi.sample_rate);

    for (;;) {
        int ch = 0, tries;
        float **outs = NULL;
        int n = 0;
        for (tries = 0; tries < 8; ++tries) {
            n = stb_vorbis_get_frame_float(v, &ch, &outs);
            if (n > 0)
                break;
        }
        if (n <= 0)
            break;
        if (pcm_len + (size_t)n * 2 > pcm_cap) {
            size_t new_cap = pcm_cap ? pcm_cap * 2 : 16384;
            short *nb;
            while (new_cap < pcm_len + (size_t)n * 2)
                new_cap *= 2;
            nb = (short *)realloc(pcm, new_cap * sizeof(short));
            if (!nb) {
                free(pcm);
                stb_vorbis_close(v);
                free(data);
                return 1;
            }
            pcm = nb;
            pcm_cap = new_cap;
        }
        downmix_frame_to_s16(outs, ch, n, pcm + pcm_len);
        pcm_len += (size_t)n * 2;
        frames_total += n;
    }

    stb_vorbis_close(v);
    free(data);

    out = fopen(argv[2], "wb");
    if (!out) {
        perror("fopen output");
        free(pcm);
        return 1;
    }
    if (pcm_len && fwrite(pcm, sizeof(short), pcm_len, out) != pcm_len) {
        fclose(out);
        free(pcm);
        return 1;
    }
    fclose(out);
    free(pcm);

    fprintf(stderr, "frames=%ld samples16=%zu\n", frames_total, pcm_len);
    return 0;
}
