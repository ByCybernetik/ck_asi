#include "ktx_vq_replace.h"
#include "ktx_terrain.h"
#include "hooks_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_terrain.h"

/* tpw.exe fixed VAs (ImageBase 0x400000). */
enum {
    VA_LoadVQBitmap = 0x00461F60u,
    VA_rt_new = 0x0070A44Bu,
    VA_rt_free = 0x0070A440u
};

typedef DWORD(__cdecl *PFN_LoadVQBitmap)(const char *path, void **out_vq);
typedef void *(__cdecl *PFN_rt_new)(unsigned size);
typedef void(__cdecl *PFN_rt_free)(void *p);

/* CVQBitmap — from LoadVQBitmap @ 0x461fc0 + GetQuantAddress @ 0x479c00. */
typedef struct {
    void *indices;     /* +0x00 */
    void *codebook;    /* +0x04 — entries of 8 bytes = 4×RGB555 */
    DWORD mode;        /* +0x08 — 0=u8 idx, 1=u16 idx */
    DWORD field_0c;    /* +0x0c */
    DWORD width;       /* +0x10 */
    DWORD height;      /* +0x14 */
    DWORD idx_stride;  /* +0x18 — width/4 */
    DWORD field_1c;
    DWORD field_20;
    DWORD field_24;
    DWORD field_28;
} CVQBitmap;

static PFN_LoadVQBitmap real_LoadVQBitmap;
static PFN_rt_new rt_new;
static PFN_rt_free rt_free;
static BYTE *g_lvq_tramp;
static BYTE g_lvq_saved[16];
static SIZE_T g_lvq_steal = 9;
static void *g_lvq_target;
static int s_replace;
static int s_smooth;
static int s_smooth_subs;
static volatile LONG s_loads;
static volatile LONG s_swaps;
static volatile LONG s_skips;
static char s_detected_season[16];

static void note_season_from_path(const char *path)
{
    char u[256];
    int i;
    if (!path || !path[0])
        return;
    for (i = 0; path[i] && i + 1 < (int)sizeof(u); ++i) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c == '\\')
            c = '/';
        u[i] = c;
    }
    u[i] = 0;
    if (strstr(u, "/autumn/") || strstr(u, "terrain/autumn/"))
        lstrcpyA(s_detected_season, "autumn");
    else if (strstr(u, "/winter/") || strstr(u, "terrain/winter/"))
        lstrcpyA(s_detected_season, "winter");
    else if (strstr(u, "/spring/") || strstr(u, "terrain/spring/"))
        lstrcpyA(s_detected_season, "spring");
}

const char *ktx_vq_detected_season(void)
{
    return s_detected_season;
}

enum { WATER_SMOOTH_MAX = 4 };
typedef struct {
    void *vq;
    DWORD frames;
    DWORD frame_h;
    /* Deferred until layer frames/frame_h patch — avoid height/frames=1024 race. */
    uint8_t *pending_idx;
    DWORD pending_h;
} WaterSmoothEnt;
static WaterSmoothEnt s_smooth_ents[WATER_SMOOTH_MAX];
static int s_smooth_n;

static void smooth_register(void *vq, DWORD frames, DWORD frame_h, uint8_t *pending_idx,
                            DWORD pending_h)
{
    int i;
    if (!vq || frames < 2 || !frame_h)
        return;
    for (i = 0; i < s_smooth_n; ++i) {
        if (s_smooth_ents[i].vq == vq) {
            if (s_smooth_ents[i].pending_idx && s_smooth_ents[i].pending_idx != pending_idx)
                rt_free(s_smooth_ents[i].pending_idx);
            s_smooth_ents[i].frames = frames;
            s_smooth_ents[i].frame_h = frame_h;
            s_smooth_ents[i].pending_idx = pending_idx;
            s_smooth_ents[i].pending_h = pending_h;
            return;
        }
    }
    if (s_smooth_n >= WATER_SMOOTH_MAX) {
        if (pending_idx)
            rt_free(pending_idx);
        return;
    }
    s_smooth_ents[s_smooth_n].vq = vq;
    s_smooth_ents[s_smooth_n].frames = frames;
    s_smooth_ents[s_smooth_n].frame_h = frame_h;
    s_smooth_ents[s_smooth_n].pending_idx = pending_idx;
    s_smooth_ents[s_smooth_n].pending_h = pending_h;
    s_smooth_n++;
}

static WaterSmoothEnt *smooth_find(void *vq)
{
    int i;
    for (i = 0; i < s_smooth_n; ++i) {
        if (s_smooth_ents[i].vq == vq)
            return &s_smooth_ents[i];
    }
    return NULL;
}

static int smooth_lookup(void *vq, DWORD *frames, DWORD *frame_h)
{
    WaterSmoothEnt *e = smooth_find(vq);
    if (!e)
        return 0;
    if (frames)
        *frames = e->frames;
    if (frame_h)
        *frame_h = e->frame_h;
    return 1;
}

static void smooth_apply_pending(CVQBitmap *vq, WaterSmoothEnt *e)
{
    if (!vq || !e || !e->pending_idx || !e->pending_h)
        return;
    if (vq->indices)
        rt_free(vq->indices);
    vq->indices = e->pending_idx;
    vq->height = e->pending_h;
    vq->field_1c = e->pending_h;
    log_msg("ktx_vq: water smooth apply h=%u frames=%u frame_h=%u", (unsigned)e->pending_h,
            (unsigned)e->frames, (unsigned)e->frame_h);
    e->pending_idx = NULL;
    e->pending_h = 0;
}

void ktx_vq_replace_fix_layers(void)
{
    BYTE *obj;
    BYTE *table;
    DWORD n, i;

    if (!s_smooth || s_smooth_n <= 0)
        return;
    obj = *(BYTE **)(ULONG_PTR)0x008DD088u;
    if (!obj)
        return;
    n = *(DWORD *)(obj + 0xc88);
    table = *(BYTE **)(obj + 0xc8c);
    if (!table || n == 0 || n > 64u)
        return;
    for (i = 0; i < n; ++i) {
        BYTE *layer = table + i * 0x5au;
        void *vq = *(void **)(layer + 0x08);
        WaterSmoothEnt *e;
        DWORD want_f, want_h, cur_f, cur_h;
        if (!vq)
            continue;
        e = smooth_find(vq);
        if (!e)
            continue;
        /* Atomically: taller atlas + frames/frame_h (never leave frame_h=height/10). */
        smooth_apply_pending((CVQBitmap *)vq, e);
        want_f = e->frames;
        want_h = e->frame_h;
        cur_f = *(DWORD *)(layer + 0x10);
        cur_h = *(DWORD *)(layer + 0x14);
        if (cur_f == want_f && cur_h == want_h)
            continue;
        *(DWORD *)(layer + 0x10) = want_f;
        *(DWORD *)(layer + 0x14) = want_h;
        if (*(DWORD *)(layer + 0x0c) >= want_f)
            *(DWORD *)(layer + 0x0c) = 0;
        log_msg("ktx_vq: smooth layer[%u] frames %u→%u frame_h %u→%u", (unsigned)i,
                (unsigned)cur_f, (unsigned)want_f, (unsigned)cur_h, (unsigned)want_h);
    }
}


static uint16_t rgb555(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

/*
 * Anim strips (swater/dwater): keep retail u8 indices (motion), rewrite
 * codebook colors in-place from KTX. Average R/G/B channels separately —
 * averaging packed RGB555 integers scrambles bits (magenta/green snow).
 */
static int refresh_mode0_codebook(CVQBitmap *vq, const uint16_t *rgb, uint32_t w, uint32_t h,
                                  const char *path)
{
    uint32_t ncodes, bi, y, x, c, k;
    const uint8_t *idx;
    uint8_t *cb;
    uint64_t *sum_r = NULL;
    uint64_t *sum_g = NULL;
    uint64_t *sum_b = NULL;
    uint32_t *cnt = NULL;

    if (!vq || !rgb || vq->mode != 0 || !vq->indices || !vq->codebook)
        return 0;
    ncodes = vq->field_0c;
    if (ncodes == 0 || ncodes > 256u)
        return 0;
    if (vq->width != w || vq->height != h || vq->idx_stride != w / 4u)
        return 0;
    idx = (const uint8_t *)vq->indices;
    cb = (uint8_t *)vq->codebook;
    sum_r = (uint64_t *)calloc((size_t)ncodes * 4u, sizeof(uint64_t));
    sum_g = (uint64_t *)calloc((size_t)ncodes * 4u, sizeof(uint64_t));
    sum_b = (uint64_t *)calloc((size_t)ncodes * 4u, sizeof(uint64_t));
    cnt = (uint32_t *)calloc(ncodes, sizeof(uint32_t));
    if (!sum_r || !sum_g || !sum_b || !cnt) {
        free(sum_r);
        free(sum_g);
        free(sum_b);
        free(cnt);
        return 0;
    }
    bi = 0;
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; x += 4) {
            const uint16_t *p = rgb + (size_t)y * w + x;
            c = idx[bi];
            if (c < ncodes) {
                for (k = 0; k < 4; ++k) {
                    uint16_t pix = p[k];
                    size_t si = (size_t)c * 4u + k;
                    sum_r[si] += (pix >> 10) & 31u;
                    sum_g[si] += (pix >> 5) & 31u;
                    sum_b[si] += pix & 31u;
                }
                cnt[c]++;
            }
            bi++;
        }
    }
    for (c = 0; c < ncodes; ++c) {
        uint16_t *slot = (uint16_t *)(cb + (size_t)c * 8u);
        if (!cnt[c])
            continue;
        for (k = 0; k < 4; ++k) {
            size_t si = (size_t)c * 4u + k;
            uint32_t r = (uint32_t)(sum_r[si] / cnt[c]);
            uint32_t g = (uint32_t)(sum_g[si] / cnt[c]);
            uint32_t b = (uint32_t)(sum_b[si] / cnt[c]);
            slot[k] = (uint16_t)((r << 10) | (g << 5) | b);
        }
    }
    free(sum_r);
    free(sum_g);
    free(sum_b);
    free(cnt);

    InterlockedIncrement(&s_swaps);
    log_msg("ktx_vq: anim refresh mode0 %ux%u codes=%u path=%s", w, h, ncodes,
            path ? path : "?");
    return 1;
}

/* Mid-frames: per-block pick ca vs cb by L1 to lerped 4px (no mean-LUT garbage). */
static int expand_anim_smooth(CVQBitmap *vq, const char *path)
{
    uint32_t w, h, nsrc, frame_h, stride, blocks_pf, ndst, new_h, ncodes;
    uint32_t i, s, bi, y, x, k, tnum, tden;
    uint8_t *old_idx, *new_idx;
    uint16_t *cb;

    if (!vq || vq->mode != 0 || !vq->indices || !vq->codebook)
        return 0;
    if (smooth_find(vq))
        return 0; /* already queued/applied */
    w = vq->width;
    h = vq->height;
    if (w < 4 || (w & 3u) || h < w * 2u || (h % w) != 0)
        return 0;
    nsrc = h / w;
    frame_h = w;
    if (nsrc < 2 || nsrc > 32 || s_smooth_subs < 1)
        return 0;
    ncodes = vq->field_0c;
    if (ncodes == 0 || ncodes > 256u)
        return 0;
    stride = w / 4u;
    blocks_pf = stride * frame_h;
    ndst = nsrc * (uint32_t)(s_smooth_subs + 1);
    new_h = frame_h * ndst;
    old_idx = (uint8_t *)vq->indices;
    cb = (uint16_t *)vq->codebook;

    new_idx = (uint8_t *)rt_new(stride * new_h + 16u);
    if (!new_idx)
        return 0;
    memset(new_idx, 0, stride * new_h + 16u);

    tden = (uint32_t)(s_smooth_subs + 1);
    for (i = 0; i < nsrc; ++i) {
        const uint8_t *fa = old_idx + (size_t)i * blocks_pf;
        const uint8_t *fb = old_idx + (size_t)((i + 1u) % nsrc) * blocks_pf;
        for (s = 0; s < tden; ++s) {
            uint8_t *dst = new_idx + (size_t)(i * tden + s) * blocks_pf;
            tnum = s;
            if (s == 0) {
                memcpy(dst, fa, blocks_pf);
                continue;
            }
            bi = 0;
            for (y = 0; y < frame_h; ++y) {
                for (x = 0; x < w; x += 4) {
                    uint8_t ca = fa[bi], cbx = fb[bi];
                    uint32_t d0 = 0, d1 = 0;
                    if (ca >= ncodes)
                        ca = 0;
                    if (cbx >= ncodes)
                        cbx = 0;
                    for (k = 0; k < 4; ++k) {
                        uint16_t pa = cb[ca * 4u + k], pb = cb[cbx * 4u + k];
                        int ra = (pa >> 10) & 31, ga = (pa >> 5) & 31, ba = pa & 31;
                        int rb = (pb >> 10) & 31, gb = (pb >> 5) & 31, bb = pb & 31;
                        int lr = ra + (int)(((rb - ra) * (int)tnum) / (int)tden);
                        int lg = ga + (int)(((gb - ga) * (int)tnum) / (int)tden);
                        int lb = ba + (int)(((bb - ba) * (int)tnum) / (int)tden);
                        int dra = lr - ra, dga = lg - ga, dba = lb - ba;
                        int drb = lr - rb, dgb = lg - gb, dbb = lb - bb;
                        d0 += (uint32_t)((dra < 0 ? -dra : dra) + (dga < 0 ? -dga : dga) +
                                         (dba < 0 ? -dba : dba));
                        d1 += (uint32_t)((drb < 0 ? -drb : drb) + (dgb < 0 ? -dgb : dgb) +
                                         (dbb < 0 ? -dbb : dbb));
                    }
                    dst[bi] = (d0 <= d1) ? ca : cbx;
                    bi++;
                }
            }
        }
    }

    /* Keep retail 10×512 until fix_layers can set frames=20 with the tall atlas. */
    smooth_register(vq, ndst, frame_h, new_idx, new_h);

    log_msg("ktx_vq: water smooth queued %u→%u frames (subs=%d deferred) path=%s", (unsigned)nsrc,
            (unsigned)ndst, s_smooth_subs, path ? path : "?");
    return 1;
}

static int path_is_water_anim(const char *path)
{
    char u[MAX_PATH];
    size_t i;
    if (!path)
        return 0;
    for (i = 0; path[i] && i + 1 < sizeof(u); ++i) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c == '\\')
            c = '/';
        u[i] = c;
    }
    u[i] = 0;
    return strstr(u, "swater") != NULL || strstr(u, "dwater") != NULL;
}

static int rebuild_vq_from_rgb555(CVQBitmap *vq, const uint16_t *rgb, uint32_t w, uint32_t h,
                                  const char *path)
{
    uint32_t nblocks, bi, y, x, stride;
    uint16_t *idx;
    uint8_t *cb;

    if (!vq || !rgb || w < 4 || (w & 3u) || h < 1)
        return 0;
    stride = w / 4u;
    nblocks = stride * h;
    if (nblocks == 0 || nblocks > 65536u) {
        InterlockedIncrement(&s_skips);
        log_msg("ktx_vq: skip oversized %ux%u blocks=%u path=%s", w, h, nblocks,
                path ? path : "?");
        return 0;
    }

    /* Retail indices block is (stride*rows*index_bytes)+16 trailer. */
    idx = (uint16_t *)rt_new(nblocks * 2u + 16u);
    cb = (uint8_t *)rt_new(nblocks * 8u);
    if (!idx || !cb) {
        if (idx)
            rt_free(idx);
        if (cb)
            rt_free(cb);
        return 0;
    }
    memset(idx, 0, nblocks * 2u + 16u);
    memset(cb, 0, nblocks * 8u);

    bi = 0;
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; x += 4) {
            const uint16_t *p = rgb + (size_t)y * w + x;
            uint16_t *slot = (uint16_t *)(cb + (size_t)bi * 8u);
            slot[0] = p[0];
            slot[1] = p[1];
            slot[2] = p[2];
            slot[3] = p[3];
            idx[bi] = (uint16_t)bi;
            bi++;
        }
    }

    if (vq->indices)
        rt_free(vq->indices);
    if (vq->codebook)
        rt_free(vq->codebook);

    vq->indices = idx;
    vq->codebook = cb;
    vq->mode = 1; /* ushort indices */
    vq->width = w;
    vq->height = h;
    vq->idx_stride = stride;
    /*
     * AverageColor@0x479d00: hist size = field_0c; index loop = stride*field_1c.
     * Codebook bytes = field_24*field_20*field_0c*2 must be nblocks*8.
     */
    vq->field_0c = nblocks;
    vq->field_1c = h;
    vq->field_20 = 1;
    vq->field_24 = 4;
    vq->field_28 = 2;

    InterlockedIncrement(&s_swaps);
    log_msg("ktx_vq: swapped %ux%u blocks=%u codes=%u path=%s", w, h, nblocks,
            (unsigned)vq->field_0c, path ? path : "?");
    return 1;
}

static int try_replace(CVQBitmap *vq, const char *path)
{
    char ktx[MAX_PATH];
    Ktx2Info info;
    uint8_t *blocks = NULL;
    uint8_t *bgra = NULL;
    uint16_t *rgb = NULL;
    uint32_t i, n, use_w, use_h, nblocks;
    int ok = 0;
    int anim_strip;

    if (!vq || !path || !ktx_terrain_ready())
        return 0;
    if (!ktx_terrain_resolve_vq(path, ktx, sizeof(ktx)))
        return 0;
    if (!ktx_terrain_load_info(ktx, &info, &blocks) || !blocks)
        return 0;
    if (info.vk_format < 131 || info.vk_format > 134) {
        free(blocks);
        return 0;
    }
    /* BC1 needs 4×4; pad height up if needed by rejecting odd sizes. */
    if ((info.width & 3u) || (info.height & 3u) || info.width < 4 || info.height < 4) {
        free(blocks);
        InterlockedIncrement(&s_skips);
        log_msg("ktx_vq: skip non-BC1-aligned %ux%u %s", info.width, info.height, ktx);
        return 0;
    }
    bgra = ktx_terrain_decode_bc1_bgra(blocks, info.width, info.height, info.level0_len);
    free(blocks);
    if (!bgra)
        return 0;
    n = info.width * info.height;
    rgb = (uint16_t *)malloc((size_t)n * 2u);
    if (!rgb) {
        free(bgra);
        return 0;
    }
    for (i = 0; i < n; ++i) {
        uint8_t *p = bgra + (size_t)i * 4u;
        rgb[i] = rgb555(p[2], p[1], p[0]); /* BGRA → RGB555 */
    }
    free(bgra);

    use_w = (uint32_t)vq->width;
    use_h = (uint32_t)vq->height;

    /* Width must match; KTX may be pad4-taller by a few rows — crop to VQ size. */
    if (info.width != use_w || info.height < use_h || info.height > use_h + 8u) {
        InterlockedIncrement(&s_skips);
        log_msg("ktx_vq: skip dim VQ %ux%u vs ktx %ux%u path=%s", use_w, use_h, info.width,
                info.height, path);
        free(rgb);
        return 0;
    }

    nblocks = (use_w / 4u) * use_h;
    anim_strip = (use_h >= use_w * 2u);

    /* Anim water: refresh codebook only. Land: mode-1 identity; oversized keep retail. */
    if (anim_strip && vq->mode == 0 && nblocks > 65536u) {
        ok = refresh_mode0_codebook(vq, rgb, use_w, use_h, path);
        free(rgb);
        if (ok && s_smooth && path_is_water_anim(path))
            expand_anim_smooth(vq, path);
        return ok;
    }
    if (nblocks > 65536u) {
        InterlockedIncrement(&s_skips);
        log_msg("ktx_vq: skip oversized %ux%u blocks=%u path=%s", use_w, use_h, nblocks, path);
        free(rgb);
        return 0;
    }

    ok = rebuild_vq_from_rgb555(vq, rgb, use_w, use_h, path);
    free(rgb);
    return ok;
}

static DWORD __cdecl hook_LoadVQBitmap(const char *path, void **out_vq)
{
    DWORD r;
    LONG n;
    CVQBitmap *vq;
    int loaded;

    r = real_LoadVQBitmap(path, out_vq);
    /* Retail returns 0 on success (error-code style); trust out pointer. */
    vq = (out_vq && *out_vq) ? (CVQBitmap *)*out_vq : NULL;
    loaded = vq && vq->width > 0 && vq->height > 0 && (vq->codebook || vq->indices);
    if (path)
        note_season_from_path(path);
    n = InterlockedIncrement(&s_loads);
    if (n <= 40 || (n % 20) == 0)
        log_msg("LoadVQBitmap #%ld ret=%lu loaded=%d path=%s vq=%p %ux%u mode=%u", (long)n,
                (unsigned long)r, loaded, path ? path : "?", (void *)vq,
                vq ? (unsigned)vq->width : 0, vq ? (unsigned)vq->height : 0,
                vq ? (unsigned)vq->mode : 0);
    if (loaded && path) {
        if (s_replace)
            try_replace(vq, path);
        /* Smooth even if KTX replace skipped (e.g. replace off): expand retail strip. */
        if (s_smooth && path_is_water_anim(path) && vq->mode == 0 &&
            vq->height >= vq->width * 2u && (vq->height % vq->width) == 0 &&
            !smooth_lookup(vq, NULL, NULL))
            expand_anim_smooth(vq, path);
    }
    return r;
}

enum { VA_ThreadAnimate = 0x004777A0u };

typedef void(__attribute__((thiscall)) *PFN_ThreadAnimate)(void *self, void *msg);
static PFN_ThreadAnimate real_ThreadAnimate;
static BYTE *g_ta_tramp;
static BYTE g_ta_saved[16];
static SIZE_T g_ta_steal = 6;
static void *g_ta_target;

static void __attribute__((thiscall)) hook_ThreadAnimate(void *self, void *msg)
{
    vk_terrain_note_anim_tick();
    ktx_vq_replace_fix_layers();
    real_ThreadAnimate(self, msg);
}

void ktx_vq_replace_install(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    char subs[16];
    DWORD n;

    /* Soft keeps retail VQ by default; GPU overpaint uses KTX separately.
     * Opt in: CK_KTX_VQ_REPLACE=1 (Wine may need this in launcher env). */
    s_replace = env_on("CK_KTX_VQ_REPLACE", 0);
    s_smooth = env_on("CK_WATER_SMOOTH", 1);
    s_smooth_subs = 1;
    n = GetEnvironmentVariableA("CK_WATER_SMOOTH_SUBS", subs, (DWORD)sizeof(subs));
    if (n > 0 && n < sizeof(subs)) {
        int v = atoi(subs);
        if (v >= 1 && v <= 3)
            s_smooth_subs = v;
    }
    s_loads = s_swaps = s_skips = 0;
    s_smooth_n = 0;
    memset(s_smooth_ents, 0, sizeof(s_smooth_ents));
    rt_new = (PFN_rt_new)(ULONG_PTR)VA_rt_new;
    rt_free = (PFN_rt_free)(ULONG_PTR)VA_rt_free;
    g_lvq_target = (void *)(ULONG_PTR)VA_LoadVQBitmap;
    g_ta_target = (void *)(ULONG_PTR)VA_ThreadAnimate;
    if (!game) {
        log_msg("ktx_vq: no game module");
        return;
    }
    if ((ULONG_PTR)g_lvq_target < 0x400000u || (ULONG_PTR)g_lvq_target > 0x00800000u) {
        log_msg("ktx_vq: bad LoadVQBitmap VA");
        return;
    }
    if (!install_inline_hook(g_lvq_target, (void *)hook_LoadVQBitmap, g_lvq_steal, &g_lvq_tramp,
                             g_lvq_saved)) {
        log_msg("ktx_vq: LoadVQBitmap hook failed");
        return;
    }
    real_LoadVQBitmap = (PFN_LoadVQBitmap)g_lvq_tramp;
    /* ThreadAnimate: water mid-frame layer fix and/or GPU blend frac clock. */
    if ((s_smooth || env_on("CK_WATER_GPU_BLEND", 0)) &&
        install_inline_hook(g_ta_target, (void *)hook_ThreadAnimate, g_ta_steal, &g_ta_tramp,
                            g_ta_saved)) {
        real_ThreadAnimate = (PFN_ThreadAnimate)g_ta_tramp;
        log_msg("ktx_vq: hooked ThreadAnimate @ %p smooth=%d gpu_blend=%d", g_ta_target, s_smooth,
                env_on("CK_WATER_GPU_BLEND", 0));
    } else if (s_smooth || env_on("CK_WATER_GPU_BLEND", 0)) {
        log_msg("ktx_vq: ThreadAnimate hook failed (smooth/blend degraded)");
        g_ta_tramp = NULL;
    }
    log_msg("ktx_vq: hooked LoadVQBitmap @ %p replace=%d smooth=%d ktx_ready=%d", g_lvq_target,
            s_replace, s_smooth, ktx_terrain_ready());
}

void ktx_vq_replace_remove(void)
{
    if (g_ta_target && g_ta_tramp)
        remove_inline_hook(g_ta_target, g_ta_steal, g_ta_saved, g_ta_tramp);
    g_ta_tramp = NULL;
    real_ThreadAnimate = NULL;
    if (g_lvq_target && g_lvq_tramp)
        remove_inline_hook(g_lvq_target, g_lvq_steal, g_lvq_saved, g_lvq_tramp);
    g_lvq_tramp = NULL;
    real_LoadVQBitmap = NULL;
}
