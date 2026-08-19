#include "hooks.h"
#include "hooks_internal.h"
#include "decor_spawn.h"
#include "hitch.h"
#include "ktx_decor.h"
#include "ktx_vq_replace.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

/* From vk_terrain.c — avoid pulling vulkan.h into this TU. */
int vk_terrain_draw_enabled(void);
int vk_terrain_soft_land_disabled(void);

/* When GPU-first / F8 GPU on, skip retail soft land draws (H-SOFT).
 * Opt out: CK_GPU_KEEP_SOFT_LAND=1. F8→SOFT re-enables soft land. */
static int soft_land_skip(void)
{
    return vk_terrain_soft_land_disabled();
}

static void soft_land_note(const char *which, int skipped, double ms)
{
    static LONG s_n;
    static double s_ms_call, s_ms_skip;
    static LONG s_n_call, s_n_skip;
    LONG n;
    s_ms_call += skipped ? 0.0 : ms;
    s_ms_skip += skipped ? ms : 0.0;
    if (skipped)
        s_n_skip++;
    else
        s_n_call++;
    n = InterlockedIncrement(&s_n);
    /* #region agent log */
    if (n <= 40 || (n % 120) == 0 || (!skipped && ms >= 2.0) || (skipped && (n % 60) == 0)) {
        char data[280];
        snprintf(data, sizeof(data),
                 "{\"n\":%ld,\"which\":\"%s\",\"skipped\":%d,\"ms\":%.3f,"
                 "\"sum_call_ms\":%.2f,\"sum_skip_ms\":%.2f,\"n_call\":%ld,\"n_skip\":%ld}",
                 (long)n, which ? which : "?", skipped, ms, s_ms_call, s_ms_skip, (long)s_n_call,
                 (long)s_n_skip);
        hooks_agent("H-SOFT", "hooks_terrain.c:soft_land", "soft-land-timing", data);
        if ((n % 120) == 0) {
            s_ms_call = 0;
            s_ms_skip = 0;
            s_n_call = 0;
            s_n_skip = 0;
        }
    }
    /* #endregion */
}

/* Retail CVXTerrain::DrawSingleTile @ VA 0x00477c20
 *   void thiscall(ushort *buf, long a, tagPoint p1, tagPoint p2)
 * Frame push: SetDIBitsToDevice (GDI32 IAT). */

typedef void(__attribute__((thiscall)) *PFN_DrawSingleTile)(void *self, unsigned short *buf,
                                                           LONG a, TagPoint p1, TagPoint p2);

static PFN_DrawSingleTile real_DrawSingleTile;
static BYTE *g_dst_tramp;
static BYTE g_dst_saved[16];
static SIZE_T g_dst_steal;
static void *g_dst_target;
static volatile LONG g_dst_logs;
static volatile LONG g_ptb_logs;
static volatile LONG g_eh_logs;
static volatile LONG g_eh_coast_logs;
static volatile LONG g_decor_spawn_logs;
static volatile LONG g_decor_coast_logs;

/* Retail packed terrain cell: this+0x1092 = DWORD*, this+0x10a2 = stride (dwords/row). */
static unsigned retail_cell_z(void *self, LONG px, LONG py)
{
    BYTE *base = (BYTE *)self;
    DWORD stride;
    DWORD *grid;
    DWORD cy, cx, idx, dword, shift;

    if (!self)
        return 0;
    stride = *(DWORD *)(base + 0x10a2);
    grid = *(DWORD **)(base + 0x1092);
    if (!grid || stride == 0)
        return 0;
    cy = ((DWORD)py) >> 6;
    cx = ((DWORD)px) >> 6;
    idx = stride * cy + (cx >> 2);
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

/* Height grid (a=32): this+0x103a = DWORD*, this+0x104a = stride. Same packing as z. */
static unsigned retail_cell_h(void *self, LONG px, LONG py)
{
    BYTE *base = (BYTE *)self;
    DWORD stride;
    DWORD *grid;
    DWORD cy, cx, idx, dword, shift;

    if (!self)
        return 0;
    stride = *(DWORD *)(base + 0x104a);
    grid = *(DWORD **)(base + 0x103a);
    if (!grid || stride == 0)
        return 0;
    cy = ((DWORD)py) >> 5;
    cx = ((DWORD)px) >> 5;
    idx = stride * cy + (cx >> 2);
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

/* Light grid (a=32): this+0x1066 / +0x1076. */
static unsigned retail_cell_light(void *self, LONG px, LONG py)
{
    BYTE *base = (BYTE *)self;
    DWORD stride;
    DWORD *grid;
    DWORD cy, cx, idx, dword, shift;

    if (!self)
        return 0;
    stride = *(DWORD *)(base + 0x1076);
    grid = *(DWORD **)(base + 0x1066);
    if (!grid || stride == 0)
        return 0;
    cy = ((DWORD)py) >> 5;
    cx = ((DWORD)px) >> 5;
    idx = stride * cy + (cx >> 2);
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

/*
 * Retail RenderTerrain remap for one 32×32 strip left edge (matches decomp @ 475fe0):
 *   flat0 = (y*181) & ~0xff;   dest0 = flat0 - h*256;  then >>8
 *   source_span ≈ ((y+32)*181 - y*181)>>8 = 22..23
 *   dest_span   = (flat1/256 - h1) - (flat0/256 - h0)
 * ratio = dest_span / source_span: >1 = vertical stretch (row stack), <1 = compress.
 */
static void probe_remap_strip(void *self, LONG wx, LONG wy, const char *tag)
{
    unsigned h00, h10, h01, h11, L00, L01;
    int flat0, flat1, dest0, dest1, src_span, dst_span;
    int z;

    if (!self)
        return;
    /* Snap to cell origin like retail (y & ~0x1f). */
    wy = (LONG)((DWORD)wy & ~0x1fu);
    wx = (LONG)((DWORD)wx & ~0x1fu);
    h00 = retail_cell_h(self, wx, wy);
    h10 = retail_cell_h(self, wx + 32, wy);
    h01 = retail_cell_h(self, wx, wy + 32);
    h11 = retail_cell_h(self, wx + 32, wy + 32);
    L00 = retail_cell_light(self, wx, wy);
    L01 = retail_cell_light(self, wx, wy + 32);
    z = (int)retail_cell_z(self, wx, wy);
    flat0 = (int)(((DWORD)wy * 0xb5u) & 0xffffff00u);
    flat1 = (int)((((DWORD)wy + 32u) * 0xb5u) & 0xffffff00u);
    dest0 = (flat0 - (int)h00 * 0x100) >> 8;
    dest1 = (flat1 - (int)h01 * 0x100) >> 8;
    src_span = (flat1 - flat0) >> 8;
    dst_span = dest1 - dest0;
    log_msg("REMAP %s xy=(%ld,%ld) h=[%u,%u,%u,%u] src=%d dst=%d ratio=%d%% destY=%d..%d",
            tag ? tag : "?", (long)wx, (long)wy, h00, h10, h01, h11, src_span, dst_span,
            src_span ? (dst_span * 100 / src_span) : 0, dest0, dest1);
}

static int is_water_z(unsigned z) { return z == 12u || z == 13u; }

static void rgb555_stats(const unsigned short *buf, int pitch, int tw, int th, int *mean_r,
                         int *mean_g, int *mean_b, int *max_lum, int *hi_pct)
{
    long sr = 0, sg = 0, sb = 0;
    int n = 0, hi = 0, ml = 0;
    int y, x;
    if (!buf || tw <= 0 || th <= 0 || pitch <= 0) {
        *mean_r = *mean_g = *mean_b = *max_lum = *hi_pct = 0;
        return;
    }
    for (y = 0; y < th; ++y) {
        const unsigned short *row = buf + y * pitch;
        for (x = 0; x < tw; ++x) {
            unsigned short p = row[x];
            int r = ((p >> 10) & 31) * 255 / 31;
            int g = ((p >> 5) & 31) * 255 / 31;
            int b = (p & 31) * 255 / 31;
            int lum = (r + g + b) / 3;
            sr += r;
            sg += g;
            sb += b;
            ++n;
            if (lum > ml)
                ml = lum;
            if (lum >= 160)
                ++hi;
        }
    }
    *mean_r = n ? (int)(sr / n) : 0;
    *mean_g = n ? (int)(sg / n) : 0;
    *mean_b = n ? (int)(sb / n) : 0;
    *max_lum = ml;
    *hi_pct = n ? (hi * 1000 / n) : 0; /* tenths of percent */
}

/* ExactHeight(thiscall, x, y) @ 0x475db0 — returns geometric height in eax. */
typedef int(__attribute__((thiscall)) *PFN_ExactHeight)(void *self, LONG x, LONG y);
static PFN_ExactHeight real_ExactHeight;
static BYTE *g_eh_tramp;
static BYTE g_eh_saved[16];
static SIZE_T g_eh_steal;
static void *g_eh_target;

/* GetExactHeight(thiscall, tagPoint*) @ 0x5430e0 — wrapper; log its caller. */
typedef int(__attribute__((thiscall)) *PFN_GetExactHeight)(void *self, TagPoint *pt);
static PFN_GetExactHeight real_GetExactHeight;
static BYTE *g_geh_tramp;
static BYTE g_geh_saved[16];
static SIZE_T g_geh_steal;
static void *g_geh_target;
static volatile LONG g_geh_logs;
static volatile LONG g_geh_coast_logs;

/* DrawTerrain(thiscall, x0,y0,x1,y1) @ 0x476680 */
typedef void(__attribute__((thiscall)) *PFN_DrawTerrain)(void *self, LONG x0, LONG y0, LONG x1,
                                                         LONG y1);
static PFN_DrawTerrain real_DrawTerrain;
static BYTE *g_dt_tramp;
static BYTE g_dt_saved[16];
static SIZE_T g_dt_steal;
static void *g_dt_target;
static volatile LONG g_dt_logs;

/* DrawFlatTerrain(thiscall, dest, x0,y0,x1,y1) @ 0x4775f0 */
typedef void(__attribute__((thiscall)) *PFN_DrawFlatTerrain)(void *self, void *dest, LONG x0,
                                                              LONG y0, LONG x1, LONG y1);
static PFN_DrawFlatTerrain real_DrawFlatTerrain;
static BYTE *g_dft_tramp;
static BYTE g_dft_saved[16];
static SIZE_T g_dft_steal;
static void *g_dft_target;
static volatile LONG g_dft_logs;

/* RenderTerrain(thiscall, dest, x0,y0,x1,y1, clipA, clipB) @ 0x475fe0 — 6 stack args. */
typedef void(__attribute__((thiscall)) *PFN_RenderTerrain)(void *self, void *dest, LONG x0, LONG y0,
                                                           LONG x1, LONG y1, LONG clipA, LONG clipB);
static PFN_RenderTerrain real_RenderTerrain;
static BYTE *g_rt_tramp;
static BYTE g_rt_saved[16];
static SIZE_T g_rt_steal;
static void *g_rt_target;
static volatile LONG g_rt_logs;

/* Decor world→screen enqueue — thiscall(packed, world_x, world_y), ret 0xc.
 * Sig-scanned (tpw 0x458840 / Celtic_Kings 0x4581D0).
 * Uses ExactHeight then projected_y = y*181/256 - height, then camera transform. */
typedef int(__attribute__((thiscall)) *PFN_DecorSpawn)(void *self, unsigned packed, LONG x,
                                                       LONG y);
static PFN_DecorSpawn real_DecorSpawn;
static BYTE *g_ds_tramp;
static BYTE g_ds_saved[16];
static SIZE_T g_ds_steal;
static void *g_ds_target;
static LONG g_ds_last_epoch = -1;

static int coast_xy(LONG x, LONG y)
{
    /* Hannibal coast palms ~ map 7200..8500 x 6800..7800 (native H10 samples). */
    return (x >= 7000 && x <= 9000 && y >= 6500 && y <= 8500);
}

/* LionThrone plaza on Riverside ~ (605, 3857). */
static int lion_xy(LONG x, LONG y)
{
    return (x >= 400 && x <= 900 && y >= 3600 && y <= 4100);
}

static int __attribute__((thiscall)) hook_ExactHeight(void *self, LONG x, LONG y)
{
    int h;
    LONG n;

    h = real_ExactHeight(self, x, y);
    n = InterlockedIncrement(&g_eh_logs);
    if (lion_xy(x, y) && (n <= 2000) && ((n % 40) == 0 || h >= 20)) {
        log_msg("ExactHeight #%ld xy=(%ld,%ld) h=%d", (long)n, (long)x, (long)y, h);
    }
    return h;
}

static int __attribute__((thiscall)) hook_GetExactHeight(void *self, TagPoint *pt)
{
    int h;
    LONG x = 0, y = 0;
    LONG n;

    if (pt) {
        x = pt->x;
        y = pt->y;
    }
    h = real_GetExactHeight(self, pt);
    n = InterlockedIncrement(&g_geh_logs);
    if (lion_xy(x, y) && (n <= 1500) && ((n % 50) == 0 || h >= 20))
        log_msg("GetExactHeight #%ld xy=(%ld,%ld) h=%d", (long)n, (long)x, (long)y, h);
    return h;
}

static void __attribute__((thiscall)) hook_DrawTerrain(void *self, LONG x0, LONG y0, LONG x1,
                                                       LONG y1)
{
    LONG n = InterlockedIncrement(&g_dt_logs);
    LONG cx, cy;
    void *retaddr = __builtin_return_address(0);
    int y_risk;
    LONGLONG t0;
    double ms;

    y_risk = (y0 < 0 || y1 < 0 || y0 >= CK_SCANLINE_N || y1 >= CK_SCANLINE_N || y1 < y0);

    if (g_terrain_trace && (n <= 30 || (n % 120) == 0 || y_risk)) {
        log_msg("DrawTerrain #%ld rect=(%ld,%ld)-(%ld,%ld) y_risk=%d this=%p ret=%p", (long)n,
                (long)x0, (long)y0, (long)x1, (long)y1, y_risk, self, retaddr);
    }

    /* Probe remap strips only when CK_TERRAIN_TRACE=1 (expensive log_msg). */
    if (g_terrain_trace &&
        (n <= 40 || ((n % 60) == 0 && y0 <= 4100 && y1 >= 3600 && x0 <= 900 && x1 >= 400))) {
        probe_remap_strip(self, 576, 3840, "lion-peak");
        probe_remap_strip(self, 544, 3808, "lion-slope");
        probe_remap_strip(self, 608, 3872, "lion-top");
        probe_remap_strip(self, 512, 3904, "lion-base");
        cx = (x0 + x1) / 2;
        cy = (y0 + y1) / 2;
        probe_remap_strip(self, cx, cy, "view-center");
    }

    if (soft_land_skip()) {
        soft_land_note("DrawTerrain", 1, 0.0);
        return;
    }
    t0 = hitch_qpc_now();
    real_DrawTerrain(self, x0, y0, x1, y1);
    ms = hitch_qpc_ms_since(t0);
    soft_land_note("DrawTerrain", 0, ms);
}

static void __attribute__((thiscall)) hook_DrawFlatTerrain(void *self, void *dest, LONG x0, LONG y0,
                                                           LONG x1, LONG y1)
{
    LONG n = InterlockedIncrement(&g_dft_logs);
    int sy0 = (int)((y0 * 181 + ((y0 * 181) >> 31 & 255)) >> 8);
    int sy1 = (int)(((y1 + 1) * 181 + (((y1 + 1) * 181) >> 31 & 255)) >> 8);
    LONGLONG t0;
    double ms;

    if (g_terrain_trace && (n <= 40 || (n % 80) == 0 || lion_xy(x0, y0) || lion_xy(x1, y1))) {
        log_msg("DrawFlat #%ld worldY=%ld..%ld -> flatSY=%d..%d (rows %d) NO height in UV", (long)n,
                (long)y0, (long)y1, sy0, sy1, sy1 - sy0);
    }
    if (soft_land_skip()) {
        soft_land_note("DrawFlat", 1, 0.0);
        return;
    }
    t0 = hitch_qpc_now();
    real_DrawFlatTerrain(self, dest, x0, y0, x1, y1);
    ms = hitch_qpc_ms_since(t0);
    soft_land_note("DrawFlat", 0, ms);
}

static void __attribute__((thiscall)) hook_RenderTerrain(void *self, void *dest, LONG x0, LONG y0,
                                                         LONG x1, LONG y1, LONG clipA, LONG clipB)
{
    LONG n = InterlockedIncrement(&g_rt_logs);
    int clip_risk;
    LONGLONG t0;
    double ms;

    clip_risk = (clipA < 0 || clipB < 0 || clipA >= CK_SCANLINE_N || clipB >= CK_SCANLINE_N ||
                 clipB < clipA);

    if (g_terrain_trace && (n <= 50 || (n % 100) == 0 || clip_risk || (y0 <= 4100 && y1 >= 3600))) {
        log_msg("RenderTerrain #%ld (%ld,%ld)-(%ld,%ld) clip=%ld,%ld risk=%d", (long)n, (long)x0,
                (long)y0, (long)x1, (long)y1, (long)clipA, (long)clipB, clip_risk);
        if (y0 <= 4100 && y1 >= 3600)
            probe_remap_strip(self, 576, 3840, "rt-lion");
    }
    if (soft_land_skip()) {
        soft_land_note("RenderTerrain", 1, 0.0);
        return;
    }
    ktx_vq_replace_fix_layers();
    t0 = hitch_qpc_now();
    real_RenderTerrain(self, dest, x0, y0, x1, y1, clipA, clipB);
    ms = hitch_qpc_ms_since(t0);
    soft_land_note("RenderTerrain", 0, ms);
}

static int __attribute__((thiscall)) hook_DecorSpawn(void *self, unsigned packed, LONG x, LONG y)
{
    int r;
    LONG n, nc;
    int height = -1;
    int projected = 0;
    void *mgr;
    void *terrain;
    const KtxDecorSprite *spr;

    if ((LONG)g_map_epoch != g_ds_last_epoch) {
        g_ds_last_epoch = (LONG)g_map_epoch;
        decor_spawn_clear();
    }
    decor_spawn_mark_epoch((LONG)g_map_epoch);
    decor_spawn_push(packed, x, y);

    n = InterlockedIncrement(&g_decor_spawn_logs);
    nc = coast_xy(x, y) ? InterlockedIncrement(&g_decor_coast_logs) : 0;

    if (n <= 20 || (nc > 0 && nc <= 5) || (g_deep_attach && n <= 40))
        log_msg("DecorSpawn #%ld packed=%u type=%u sub=%u xy=(%ld,%ld) coast=%ld epoch=%ld buf=%d",
                (long)n, packed, decor_spawn_type(packed), decor_spawn_sub(packed), (long)x,
                (long)y, (long)nc, (long)g_map_epoch, decor_spawn_count());

    /* #region agent log */
    if (n <= 30 || (n % 200) == 0) {
        char data[256];
        spr = ktx_decor_lookup((int)decor_spawn_type(packed), 0);
        snprintf(data, sizeof(data),
                 "{\"n\":%ld,\"packed\":%u,\"type\":%u,\"x\":%ld,\"y\":%ld,\"buf\":%d,"
                 "\"lookup\":\"%s\",\"hot\":[%d,%d]}",
                 (long)n, packed, decor_spawn_type(packed), (long)x, (long)y, decor_spawn_count(),
                 spr ? spr->id : "", spr ? spr->hot_x : 0, spr ? spr->hot_y : 0);
        hooks_agent("B", "hooks_terrain.c:DecorSpawn", "spawn", data);
    }
    /* #endregion */

    if ((n <= 80 || (nc > 0 && nc <= 40)) && real_ExactHeight) {
        mgr = *(void **)(ULONG_PTR)0x008C7B30;
        if (mgr) {
            terrain = *(void **)((BYTE *)mgr + 0xb8);
            if (terrain)
                height = real_ExactHeight(terrain, x, y);
        }
        projected = (int)((y * 181) / 256) - (height >= 0 ? height : 0);
        (void)projected;
    }

    /* GPU-first: never soft-stamp decors when soft land is off (catalog → GPU, else bare). */
    spr = ktx_decor_lookup((int)decor_spawn_type(packed), 0);
    if (vk_terrain_soft_land_disabled()) {
        int gpu = (ktx_decor_wanted() && ktx_decor_ready() && spr) ? 1 : 0;
        /* #region agent log */
        if (n <= 5 || (n % 500) == 0) {
            char data[160];
            snprintf(data, sizeof(data),
                     "{\"n\":%ld,\"type\":%u,\"id\":\"%s\",\"skip_soft\":1,\"gpu\":%d,\"ret\":1,\"buf\":%d}",
                     (long)n, decor_spawn_type(packed), spr ? spr->id : "", gpu,
                     decor_spawn_count());
            hooks_agent("E-SOFT", "hooks_terrain.c:DecorSpawn", "skip soft decor", data);
        }
        /* #endregion */
        return 1;
    }

    r = real_DecorSpawn(self, packed, x, y);
    /* #region agent log */
    if (n <= 5 || (n % 500) == 0) {
        char data[160];
        snprintf(data, sizeof(data),
                 "{\"n\":%ld,\"type\":%u,\"ret\":%d,\"buf\":%d,\"soft\":1,\"f8\":%d}",
                 (long)n, decor_spawn_type(packed), r, decor_spawn_count(),
                 vk_terrain_draw_enabled() ? 1 : 0);
        hooks_agent("E-SOFT", "hooks_terrain.c:DecorSpawn", "call soft decor", data);
    }
    /* #endregion */
    return r;
}

/* ProcessTransBmp(CBitmap*) @ 0x476b90 — in-place mask >>3 + row sentinels. */
typedef void (*PFN_ProcessTransBmp)(void *bmp);
static PFN_ProcessTransBmp real_ProcessTransBmp;
static BYTE *g_ptb_tramp;
static BYTE g_ptb_saved[16];
static SIZE_T g_ptb_steal;
static void *g_ptb_target;

static void hook_ProcessTransBmp(void *bmp)
{
    LONG n = InterlockedIncrement(&g_ptb_logs);
    if (bmp && n <= 60) {
        /* CBitmap layout (from RE): +0x08 = width (i16), +0xa = height (i16), +0xe = pitch (i16),
         * vtable[6]/GetBits) returns 8bpp pointer — we only log dims here. */
        short w = *(short *)((BYTE *)bmp + 0x8);
        short h = *(short *)((BYTE *)bmp + 0xa);
        short pitch = *(short *)((BYTE *)bmp + 0xe);
        log_msg("ProcessTransBmp #%ld bmp=%p w=%d h=%d pitch=%d", (long)n, bmp, (int)w, (int)h,
                (int)pitch);
    }
    real_ProcessTransBmp(bmp);
}

static void __attribute__((thiscall)) hook_DrawSingleTile(void *self, unsigned short *buf, LONG a,
                                                          TagPoint p1, TagPoint p2)
{
    LONG n = InterlockedIncrement(&g_dst_logs);
    unsigned z0 = retail_cell_z(self, p1.x, p1.y);
    unsigned z1 = retail_cell_z(self, p1.x + 64, p1.y);
    unsigned z2 = retail_cell_z(self, p1.x + 64, p1.y + 64);
    unsigned z3 = retail_cell_z(self, p1.x, p1.y + 64);
    int mixed = (z0 != z1 || z1 != z2 || z2 != z3);
    int any_w = is_water_z(z0) || is_water_z(z1) || is_water_z(z2) || is_water_z(z3);
    int any_l = !is_water_z(z0) || !is_water_z(z1) || !is_water_z(z2) || !is_water_z(z3);
    int shore = mixed && any_w && any_l;

    if (n <= 40 || (n % 200) == 0 || (shore && n <= 200) || (mixed && n <= 80)) {
        DWORD stride = self ? *(DWORD *)((BYTE *)self + 0x10a2) : 0;
        log_msg("DrawSingleTile #%ld this=%p buf=%p a=%ld p1=(%ld,%ld) p2=(%ld,%ld) "
                "z=[%u,%u,%u,%u] shore=%d stride=%lu",
                (long)n, self, (void *)buf, (long)a, (long)p1.x, (long)p1.y, (long)p2.x,
                (long)p2.y, z0, z1, z2, z3, shore, (unsigned long)stride);
    }

    real_DrawSingleTile(self, buf, a, p1, p2);
    (void)any_l;
}

void resolve_terrain_targets(HMODULE game)
{
    static const BYTE sig_rt[] = {0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57};
    static const BYTE sig_dt[] = {0x83, 0xEC, 0x68, 0x53, 0x8B, 0x5C, 0x24, 0x70};
    static const BYTE sig_dft[] = {0x83, 0xEC, 0x38, 0x53, 0x8B, 0x5C, 0x24, 0x40};
    /* DrawSingleTile: push ebp; mov ebp,esp; sub esp,68h/70h; push ebx; push esi; push edi */
    static const BYTE sig_dst[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x68, 0x53, 0x56, 0x57};
    static const BYTE sig_dst70[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x70, 0x53, 0x56, 0x57};
    static const BYTE sig_eh_tpw[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x20, 0x53, 0x57, 0x8B,
                                      0xF9};
    static const BYTE sig_eh_ck[] = {0x83, 0xEC, 0x20, 0x8B, 0x44, 0x24, 0x24, 0xA8, 0x1F};
    /* DecorSpawn: sub esp,24h; mov eax,[esp+28]; mov edx,[esp+2c]; push ebx; push esi; mov esi,ecx;
     * mov ecx,eax; and ecx,0FFh  — unique across tpw + Celtic_Kings. */
    static const BYTE sig_ds[] = {0x83, 0xEC, 0x24, 0x8B, 0x44, 0x24, 0x28, 0x8B, 0x54, 0x24, 0x2C,
                                  0x53, 0x56, 0x8B, 0xF1, 0x8B, 0xC8, 0x81, 0xE1, 0xFF, 0x00, 0x00,
                                  0x00};
    char exe[MAX_PATH];

    g_rt_target = scan_sig(game, sig_rt, sizeof(sig_rt));
    g_dt_target = scan_sig(game, sig_dt, sizeof(sig_dt));
    g_dft_target = scan_sig(game, sig_dft, sizeof(sig_dft));
    g_dst_target = scan_sig(game, sig_dst, sizeof(sig_dst));
    if (!g_dst_target)
        g_dst_target = scan_sig(game, sig_dst70, sizeof(sig_dst70));
    g_eh_target = scan_sig(game, sig_eh_tpw, sizeof(sig_eh_tpw));
    if (!g_eh_target)
        g_eh_target = scan_sig(game, sig_eh_ck, sizeof(sig_eh_ck));
    g_ds_target = scan_sig(game, sig_ds, sizeof(sig_ds));

    g_rt_steal = 6;
    g_dt_steal = 8;
    g_dft_steal = 8;
    g_dst_steal = 6;
    g_eh_steal = 7;
    g_ds_steal = 7; /* 83 EC 24 / 8B 44 24 28 */

    exe[0] = '\0';
    if (game)
        GetModuleFileNameA(game, exe, MAX_PATH);
    log_msg("terrain resolve: exe=%s RT=%p DT=%p DFT=%p DST=%p EH=%p DS=%p tpw=%d ck=%d", exe,
            g_rt_target, g_dt_target, g_dft_target, g_dst_target, g_eh_target, g_ds_target,
            looks_like_tpw(), looks_like_celtic_kings());
    /* #region agent log */
    {
        char data[160];
        snprintf(data, sizeof(data), "{\"DS\":\"%p\",\"tpw\":%d,\"ck\":%d}", g_ds_target,
                 looks_like_tpw(), looks_like_celtic_kings());
        hooks_agent("A", "hooks_terrain.c:resolve", "DecorSpawn target", data);
    }
    /* #endregion */
}


void hooks_terrain_install(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    /* ---- terrain draw / remap: signature-scan (Celtic_Kings.exe + tpw.exe) ---- */
    resolve_terrain_targets(game);

    /* Soft-land skip under F8 needs these even in profile=min (H-SOFT).
     * ExactHeight stays full-profile only (was gated to avoid min freezes). */
    if (g_proxy_min)
        log_msg("terrain land hooks ON in min (GPU soft-land skip); ExactHeight skipped");

    if (!g_proxy_min) {
        if (g_eh_target) {
            if (install_inline_hook(g_eh_target, (void *)hook_ExactHeight, g_eh_steal, &g_eh_tramp,
                                    g_eh_saved)) {
                real_ExactHeight = (PFN_ExactHeight)g_eh_tramp;
                log_msg("inline hooked ExactHeight @ %p", g_eh_target);
            } else
                log_msg("WARN: ExactHeight hook failed");
        } else
            log_msg("WARN: ExactHeight not found");
    }

    if (g_dt_target) {
        if (install_inline_hook(g_dt_target, (void *)hook_DrawTerrain, g_dt_steal, &g_dt_tramp,
                                g_dt_saved)) {
            real_DrawTerrain = (PFN_DrawTerrain)g_dt_tramp;
            log_msg("inline hooked DrawTerrain @ %p", g_dt_target);
        } else
            log_msg("WARN: DrawTerrain hook failed");
    } else
        log_msg("WARN: DrawTerrain not found");
    if (g_dft_target) {
        if (install_inline_hook(g_dft_target, (void *)hook_DrawFlatTerrain, g_dft_steal,
                                &g_dft_tramp, g_dft_saved)) {
            real_DrawFlatTerrain = (PFN_DrawFlatTerrain)g_dft_tramp;
            log_msg("inline hooked DrawFlatTerrain @ %p", g_dft_target);
        } else
            log_msg("WARN: DrawFlatTerrain hook failed");
    } else
        log_msg("WARN: DrawFlatTerrain not found");
    if (g_rt_target) {
        if (install_inline_hook(g_rt_target, (void *)hook_RenderTerrain, g_rt_steal, &g_rt_tramp,
                                g_rt_saved)) {
            real_RenderTerrain = (PFN_RenderTerrain)g_rt_tramp;
            log_msg("inline hooked RenderTerrain @ %p", g_rt_target);
        } else
            log_msg("WARN: RenderTerrain hook failed");
    } else
        log_msg("WARN: RenderTerrain not found");
    /* DrawSingleTile: address resolved for the map; hooking deferred — calling
     * convention differs between builds and a bad thiscall arity crashes Wine. */
    if (g_dst_target)
        log_msg("DrawSingleTile @ %p (resolved, not hooked — see pipeline doc)", g_dst_target);
    else
        log_msg("WARN: DrawSingleTile not found");

    /* DecorSpawn: needed for GPU decor instance list — install even in min profile. */
    if (g_ds_target) {
        if (install_inline_hook(g_ds_target, (void *)hook_DecorSpawn, g_ds_steal, &g_ds_tramp,
                                g_ds_saved)) {
            real_DecorSpawn = (PFN_DecorSpawn)g_ds_tramp;
            log_msg("inline hooked DecorSpawn @ %p", g_ds_target);
        } else
            log_msg("WARN: DecorSpawn hook failed");
    } else
        log_msg("WARN: DecorSpawn not found");
}

void hooks_terrain_remove(void)
{
    remove_inline_hook(g_eh_target, g_eh_steal, g_eh_saved, g_eh_tramp);
    g_eh_tramp = NULL;
    remove_inline_hook(g_dt_target, g_dt_steal, g_dt_saved, g_dt_tramp);
    g_dt_tramp = NULL;
    remove_inline_hook(g_dft_target, g_dft_steal, g_dft_saved, g_dft_tramp);
    g_dft_tramp = NULL;
    remove_inline_hook(g_rt_target, g_rt_steal, g_rt_saved, g_rt_tramp);
    g_rt_tramp = NULL;
    remove_inline_hook(g_dst_target, g_dst_steal, g_dst_saved, g_dst_tramp);
    g_dst_tramp = NULL;
    remove_inline_hook(g_ds_target, g_ds_steal, g_ds_saved, g_ds_tramp);
    g_ds_tramp = NULL;
    real_DecorSpawn = NULL;
}
