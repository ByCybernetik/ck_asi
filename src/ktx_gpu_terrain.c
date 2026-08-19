#include "ktx_gpu_terrain.h"
#include "ktx_terrain.h"
#include "hooks_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CK_MAP_MGR = 0x008C7B30u,
    CK_SEASON = 0x008DD090u,
    Z_N = 16,
    TEX_CACHE = 24
};

typedef struct {
    char name[48];
    uint8_t *bgra;
    int w, h;
} TexSlot;

static int s_on;
static int s_checker;
static TexSlot s_tex[TEX_CACHE];
static int s_tex_n;
static char s_season[16] = "spring";
static volatile LONG s_paint_n;
static int s_logged_map;

/* TERRAINS.XML z → basename (fallbacks where retail grass2/3 missing in pak). */
static const char *z_basename(unsigned z)
{
    switch (z & 0xFFu) {
    case 0:
        return "ground1024";
    case 1:
        return "ground256";
    case 2:
        return "ground";
    case 3:
        return "grass1024";
    case 4:
        return "grass512";
    case 5:
        return "sands";
    case 6:
        return "rocks1024";
    case 7:
        return "roads";
    case 8:
        return "rocks256";
    case 9:
        return "sandswave";
    case 10:
        return "waves";
    case 11:
        return "rocks2";
    case 12:
        return "swater";
    case 13:
        return "dwater";
    case 14:
        return "rroads";
    case 15:
        return "invalid";
    case 16:
        return "sand_grass512";
    case 17:
        return "sands_mix512";
    case 18:
        return "swamp1024";
    case 19:
        return "sroads";
    case 20:
        return "iroads";
    default:
        return "ground";
    }
}

static int z_seasonal(unsigned z)
{
    z &= 0x0Fu;
    return !(z == 10 || z == 12 || z == 13 || z == 15);
}

static void agent(const char *msg, const char *data_json)
{
    /* #region agent log */
    hooks_agent("H-GPUT", "ktx_gpu_terrain.c", msg, data_json);
    /* #endregion */
}

static void *terrain_obj(void)
{
    void *mgr;
    if (IsBadReadPtr((void *)(ULONG_PTR)CK_MAP_MGR, 4))
        return NULL;
    mgr = *(void **)(ULONG_PTR)CK_MAP_MGR;
    if (!mgr || (ULONG_PTR)mgr < 0x10000u || IsBadReadPtr((BYTE *)mgr + 0xb8, 4))
        return NULL;
    return *(void **)((BYTE *)mgr + 0xb8);
}

static unsigned cell_z(void *terr, LONG px, LONG py)
{
    BYTE *base;
    DWORD stride;
    DWORD *grid;
    DWORD cy, cx, idx, dword, shift;

    if (!terr)
        return 0;
    base = (BYTE *)terr;
    if (IsBadReadPtr(base + 0x10a2, 4) || IsBadReadPtr(base + 0x1092, 4))
        return 0;
    stride = *(DWORD *)(base + 0x10a2);
    grid = *(DWORD **)(base + 0x1092);
    if (!grid || !stride || IsBadReadPtr(grid, 4))
        return 0;
    cy = ((DWORD)py) >> 6;
    cx = ((DWORD)px) >> 6;
    idx = stride * cy + (cx >> 2);
    if (IsBadReadPtr(grid + idx, 4))
        return 0;
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

static void refresh_season(void)
{
    DWORD v = 0;
    if (!IsBadReadPtr((void *)(ULONG_PTR)CK_SEASON, 4))
        v = *(DWORD *)(ULONG_PTR)CK_SEASON;
    /* Empirical: 0 spring, 1 autumn, 2 winter — adjust if logs disagree. */
    if (v == 1)
        lstrcpyA(s_season, "autumn");
    else if (v == 2)
        lstrcpyA(s_season, "winter");
    else
        lstrcpyA(s_season, "spring");
}

static TexSlot *load_tex(const char *base, int seasonal)
{
    char rel[96];
    char path[MAX_PATH];
    Ktx2Info info;
    uint8_t *blocks = NULL;
    TexSlot *slot;
    int i;

    if (!base || !base[0] || !ktx_terrain_ready())
        return NULL;
    if (seasonal)
        snprintf(rel, sizeof(rel), "%s/%s.ktx2", s_season, base);
    else
        snprintf(rel, sizeof(rel), "%s.ktx2", base);

    for (i = 0; i < s_tex_n; ++i) {
        if (strcmp(s_tex[i].name, rel) == 0)
            return &s_tex[i];
    }
    if (s_tex_n >= TEX_CACHE)
        return s_tex_n > 0 ? &s_tex[0] : NULL;

    snprintf(path, sizeof(path), "%s\\%s", ktx_terrain_root(), rel);
    {
        char *q;
        for (q = path; *q; ++q)
            if (*q == '/')
                *q = '\\';
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(path, sizeof(path), "%s/%s", ktx_terrain_root(), rel);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            return NULL;
    }
    if (!ktx_terrain_load_info(path, &info, &blocks))
        return NULL;
    if (info.vk_format < 131 || info.vk_format > 134) {
        free(blocks);
        return NULL;
    }
    slot = &s_tex[s_tex_n];
    slot->bgra = ktx_terrain_decode_bc1_bgra(blocks, info.width, info.height, info.level0_len);
    free(blocks);
    if (!slot->bgra)
        return NULL;
    lstrcpynA(slot->name, rel, (int)sizeof(slot->name));
    slot->w = (int)info.width;
    slot->h = (int)info.height;
    s_tex_n++;
    return slot;
}

static void sample_put(uint8_t *fb, int fw, int fh, int dx, int dy, const TexSlot *tex, int sx,
                       int sy)
{
    uint8_t *d, *s;
    if (!fb || !tex || !tex->bgra || dx < 0 || dy < 0 || dx >= fw || dy >= fh)
        return;
    if (tex->w <= 0 || tex->h <= 0)
        return;
    sx %= tex->w;
    sy %= tex->h;
    if (sx < 0)
        sx += tex->w;
    if (sy < 0)
        sy += tex->h;
    d = fb + ((size_t)dy * (size_t)fw + (size_t)dx) * 4u;
    s = tex->bgra + ((size_t)sy * (size_t)tex->w + (size_t)sx) * 4u;
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
    d[3] = 255;
}

void ktx_gpu_terrain_init(void)
{
    /* Off by default: AABB overpaint ≠ retail DrawFlat/RenderTerrain.
     * Opt-in: CK_GPU_TERRAIN_OVERPAINT=1 (legacy CK_GPU_TERRAIN ignored). */
    s_on = env_on("CK_GPU_TERRAIN_OVERPAINT", 0);
    /* Optional A/B vs retail when experimenting. */
    s_checker = env_on("CK_GPU_CHECKER", 0);
    s_tex_n = 0;
    s_paint_n = 0;
    s_logged_map = 0;
    if (s_on)
        log_msg("ktx_gpu_terrain: OVERPAINT on (not texture replace) checker=%d", s_checker);
    {
        char js[64];
        snprintf(js, sizeof(js), "{\"on\":%d,\"checker\":%d}", s_on, s_checker);
        agent("gpu-init", js);
    }
}

void ktx_gpu_terrain_shutdown(void)
{
    int i;
    for (i = 0; i < s_tex_n; ++i) {
        free(s_tex[i].bgra);
        s_tex[i].bgra = NULL;
    }
    s_tex_n = 0;
    s_on = 0;
}

void ktx_gpu_terrain_paint_soft(uint8_t *bgra, int w, int h)
{
    void *terr;
    LONG L, T, R, B;
    LONG wx0, wy0, wx1, wy1;
    LONG wx, wy;
    LONG n;
    int tiles = 0;
    int zhist[16];
    char js[320];
    int play_t = 80; /* InfoBar — skip top UI band */
    int play_b;

    if (!s_on || !bgra || w < 640 || h < 400 || !ktx_terrain_ready())
        return;

    terr = terrain_obj();
    if (!terr)
        return;

    mm_read_cam(&L, &T, &R, &B);
    if (R <= L || B <= T)
        return;

    refresh_season();
    memset(zhist, 0, sizeof(zhist));
    play_b = h - 120;
    if (play_b <= play_t + 64)
        play_b = h - 8;

    /*
     * Cam L/T/R/B match soft pixels 1:1 (logs: R-L≈1919 @ soft 1920).
     * H-Y: do NOT apply y*181/256 again — that crushed tiles into the upper band.
     */
    wx0 = (L & ~63) - 64;
    wy0 = (T & ~63) - 64;
    wx1 = ((R + 63) & ~63) + 128;
    wy1 = ((B + 63) & ~63) + 128;

    for (wy = wy0; wy < wy1; wy += 64) {
        int sy0 = (int)(wy - T);
        int th = 64;
        for (wx = wx0; wx < wx1; wx += 64) {
            unsigned z = cell_z(terr, wx, wy) & 0x0Fu;
            const char *base = z_basename(z);
            TexSlot *tex;
            int sx0 = (int)(wx - L);
            int x, y;
            zhist[z]++;
            /* Checker vs retail: odd cells keep software terrain for alignment A/B.
             * CK_GPU_CHECKER=0 → paint all cells. */
            if (s_checker && ((((wx >> 6) + (wy >> 6)) & 1) == 0))
                continue;
            tex = load_tex(base, z_seasonal(z));
            if (!tex)
                continue;
            tiles++;
            for (y = 0; y < th; ++y) {
                int dy = sy0 + y;
                int v = (int)wy + y;
                if (dy < play_t || dy >= play_b)
                    continue;
                for (x = 0; x < 64; ++x) {
                    int dx = sx0 + x;
                    if (dx < 0 || dx >= w)
                        continue;
                    sample_put(bgra, w, h, dx, dy, tex, (int)wx + x, v);
                }
            }
        }
    }

    n = InterlockedIncrement(&s_paint_n);
    if (!s_logged_map || n <= 3 || (n % 120) == 0) {
        int sy_sample = (int)(wy0 + 64 - T);
        s_logged_map = 1;
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"cam\":[%ld,%ld,%ld,%ld],\"span\":[%ld,%ld],\"soft\":[%d,%d],"
                 "\"tiles\":%d,\"season\":\"%s\",\"checker\":%d,\"sy0\":%d,"
                 "\"zh\":[%d,%d,%d,%d,%d,%d,%d,%d]}",
                 (long)n, (long)L, (long)T, (long)R, (long)B, (long)(R - L), (long)(B - T), w, h,
                 tiles, s_season, s_checker, sy_sample, zhist[0], zhist[3], zhist[5], zhist[9],
                 zhist[10], zhist[12], zhist[13], zhist[15]);
        agent("gpu-paint", js);
        log_msg("ktx_gpu_terrain: paint #%ld cam=(%ld,%ld)-(%ld,%ld) span=%ldx%ld soft=%dx%d "
                "tiles=%d checker=%d sy0=%d season=%s",
                (long)n, (long)L, (long)T, (long)R, (long)B, (long)(R - L), (long)(B - T), w, h,
                tiles, s_checker, sy_sample, s_season);
    }
}
