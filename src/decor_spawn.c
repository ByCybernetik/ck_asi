#include "decor_spawn.h"
#include "hooks_internal.h"

#include <string.h>

static DecorSpawnItem s_items[DECOR_SPAWN_MAX];
static volatile LONG s_n;
static volatile LONG s_live_epoch = -1;
static CRITICAL_SECTION s_cs;
static int s_cs_ok;

static void ensure_cs(void)
{
    if (s_cs_ok)
        return;
    InitializeCriticalSection(&s_cs);
    s_cs_ok = 1;
}

void decor_spawn_clear(void)
{
    ensure_cs();
    EnterCriticalSection(&s_cs);
    s_n = 0;
    LeaveCriticalSection(&s_cs);
}

void decor_spawn_mark_epoch(LONG epoch)
{
    InterlockedExchange(&s_live_epoch, epoch);
}

int decor_spawn_epoch_live(LONG epoch)
{
    return InterlockedCompareExchange(&s_live_epoch, 0, 0) == epoch;
}

void decor_spawn_push(unsigned packed, LONG x, LONG y)
{
    LONG i;
    unsigned high = (packed >> 8) & 0xffu;
    /* Retail PutDecor: cell origin + offset_table[high] = ((h%16)*4, (h/16)*4).
     * DecorSpawn passes the 64×64 cell origin; fine placement is in the high byte. */
    LONG fx = x + (LONG)((high % 16u) * 4u);
    LONG fy = y + (LONG)((high / 16u) * 4u);
    ensure_cs();
    EnterCriticalSection(&s_cs);
    i = s_n;
    if (i < DECOR_SPAWN_MAX) {
        s_items[i].packed = packed;
        s_items[i].x = fx;
        s_items[i].y = fy;
        s_n = i + 1;
    }
    LeaveCriticalSection(&s_cs);
}

int decor_spawn_count(void)
{
    return (int)s_n;
}

const DecorSpawnItem *decor_spawn_items(void)
{
    return s_items;
}

unsigned decor_spawn_type(unsigned packed)
{
    return packed & 0xffu;
}

unsigned decor_spawn_sub(unsigned packed)
{
    return (packed >> 8) & 0xffu;
}

int decor_spawn_fill_from_map_grid(void *map_self, LONG cam_L, LONG cam_T, LONG cam_R, LONG cam_B)
{
    return decor_spawn_fill_from_map_grid_ex(map_self, cam_L, cam_T, cam_R, cam_B, NULL, NULL, NULL);
}

/* Only CurrentMap decor DIRG at [map+4]+0x78. Prefer live map layout; if Wine
 * IsBadReadPtr rejects a large probe, fall back to last CurrentMap LoadDirg self. */
static void *resolve_decor_obj(void *map_self)
{
    void *inner;
    void *decor;
    if (map_self && !IsBadReadPtr(map_self, 8)) {
        inner = *(void **)((char *)map_self + 4);
        /* Probe only through decor header (+0x78..+0x90), not full 0xC0 — Wine
         * IsBadReadPtr(inner,0xC0) falsely fails and left editor fill at reason=2. */
        if (inner && !IsBadReadPtr((char *)inner + 0x78, 0x18)) {
            decor = (char *)inner + 0x78;
            return decor;
        }
    }
    if (g_last_decor_dirg && !IsBadReadPtr(g_last_decor_dirg, 0x18))
        return g_last_decor_dirg;
    return NULL;
}

int decor_spawn_fill_from_map_grid_ex(void *map_self, LONG cam_L, LONG cam_T, LONG cam_R, LONG cam_B,
                                      int *out_nz_all, int *out_reason, void **out_decor)
{
    void *decor_obj;
    const unsigned short *cells;
    unsigned ww, wh, cols, rows;
    LONG x0, y0, x1, y1;
    unsigned cx0, cy0, cx1, cy1, cx, cy;
    LONG n = 0;
    size_t nbytes, i, ncells;
    int nz_all = 0;

    if (out_nz_all)
        *out_nz_all = 0;
    if (out_reason)
        *out_reason = 1;
    if (out_decor)
        *out_decor = NULL;

    decor_obj = resolve_decor_obj(map_self);
    if (!decor_obj) {
        if (out_reason)
            *out_reason = 2;
        return 0;
    }
    if (out_decor)
        *out_decor = decor_obj;

    cells = *(const unsigned short **)((char *)decor_obj + 8);
    ww = ((unsigned *)decor_obj)[0x10 / 4];
    wh = ((unsigned *)decor_obj)[0x14 / 4];
    if (!cells || (unsigned)(ULONG_PTR)cells < 0x10000u || ww < 64 || wh < 64 || ww > 65536u ||
        wh > 65536u) {
        if (out_reason)
            *out_reason = 3;
        return 0;
    }
    cols = ww / 64u;
    rows = wh / 64u;
    if (cols < 1 || rows < 1 || (size_t)cols * (size_t)rows > 1024u * 1024u) {
        if (out_reason)
            *out_reason = 4;
        return 0;
    }
    nbytes = (size_t)cols * (size_t)rows * sizeof(unsigned short);
    if (IsBadReadPtr((void *)cells, (UINT_PTR)nbytes)) {
        if (out_reason)
            *out_reason = 5;
        return 0;
    }

    ncells = (size_t)cols * (size_t)rows;
    for (i = 0; i < ncells; ++i) {
        if (cells[i])
            nz_all++;
    }
    if (out_nz_all)
        *out_nz_all = nz_all;

    x0 = cam_L - 512;
    y0 = cam_T - 512;
    x1 = cam_R + 512;
    y1 = cam_B + 768;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (LONG)ww)
        x1 = (LONG)ww;
    if (y1 > (LONG)wh)
        y1 = (LONG)wh;
    cx0 = (unsigned)x0 / 64u;
    cy0 = (unsigned)y0 / 64u;
    cx1 = (unsigned)(x1 > 0 ? x1 - 1 : 0) / 64u;
    cy1 = (unsigned)(y1 > 0 ? y1 - 1 : 0) / 64u;
    if (cx1 >= cols)
        cx1 = cols - 1;
    if (cy1 >= rows)
        cy1 = rows - 1;

    ensure_cs();
    EnterCriticalSection(&s_cs);
    s_n = 0;
    for (cy = cy0; cy <= cy1 && n < DECOR_SPAWN_MAX; ++cy) {
        for (cx = cx0; cx <= cx1 && n < DECOR_SPAWN_MAX; ++cx) {
            unsigned short v = cells[(size_t)cy * cols + cx];
            unsigned high;
            if (v == 0)
                continue;
            high = (unsigned)(v >> 8);
            s_items[n].packed = (unsigned)v;
            s_items[n].x = (LONG)(cx * 64u + (high % 16u) * 4u);
            s_items[n].y = (LONG)(cy * 64u + (high / 16u) * 4u);
            n++;
        }
    }
    s_n = n;
    LeaveCriticalSection(&s_cs);
    if (out_reason)
        *out_reason = (n > 0) ? 0 : (nz_all > 0 ? 6 : 7); /* 6=off-cam, 7=empty grid */
    return (int)n;
}
