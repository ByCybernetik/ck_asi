#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "vk_present.h"
#include <stdio.h>

/*
 * CVXMiniMap (vxMiniMap.obj): widget rect + BuildMinimap bitmap size drive aspect.
 * FixRect sizes ≈256/512 square; DrawAfter maps camera via mgr[a8..b4] and hardcodes 0x3FF
 * (zoommap 1024). At soft 1920 wrong SetRect → stretched panel / bad viewport (H-R/H-T).
 */
typedef void(__attribute__((thiscall)) *PFN_MMSetRect)(void *self, const void *rect, int a, int b);
typedef DWORD(__attribute__((thiscall)) *PFN_MMBuild)(void *self, void **pbmp, LONG sx, LONG sy);
typedef void(__attribute__((thiscall)) *PFN_MMDrawAfter)(void *self, void *cdc, const void *dirty);
typedef void (*PFN_FlatMiniMapTerrain)(void *bmp, LONG lod);

static PFN_MMSetRect real_MMSetRect;
static PFN_MMBuild real_MMBuild;
static PFN_MMDrawAfter real_MMDrawAfter;
static PFN_FlatMiniMapTerrain real_FlatMiniMapTerrain;
static BYTE *g_mm_sr_tramp, *g_mm_bm_tramp, *g_mm_da_tramp, *g_mm_ft_tramp;
static BYTE g_mm_sr_saved[16], g_mm_bm_saved[16], g_mm_da_saved[16], g_mm_ft_saved[16];
static SIZE_T g_mm_sr_steal = 9, g_mm_bm_steal = 6, g_mm_da_steal = 6, g_mm_ft_steal = 6;
static void *g_mm_sr_target, *g_mm_bm_target, *g_mm_da_target, *g_mm_ft_target;
static volatile LONG g_mm_sr_n, g_mm_bm_n, g_mm_da_n, g_mm_ft_n;

void mm_read_mgr(unsigned *a8, unsigned *ac, unsigned *b0, unsigned *b4)
{
    void *mgr = *(void **)(ULONG_PTR)0x008C7B30u;
    *a8 = *ac = *b0 = *b4 = 0;
    if (mgr && (ULONG_PTR)mgr > 0x10000u) {
        BYTE *m = (BYTE *)mgr;
        *a8 = *(unsigned *)(m + 0xa8);
        *ac = *(unsigned *)(m + 0xac);
        *b0 = *(unsigned *)(m + 0xb0);
        *b4 = *(unsigned *)(m + 0xb4);
    }
}

void mm_read_cam(LONG *L, LONG *T, LONG *R, LONG *B)
{
    DWORD cam = 0;
    *L = *T = *R = *B = 0;
    if (!IsBadReadPtr((void *)(ULONG_PTR)0x007CA604u, 4))
        cam = *(DWORD *)(ULONG_PTR)0x007CA604u;
    if (cam && !IsBadReadPtr((void *)(ULONG_PTR)(cam + 0x4d4), 4)) {
        *L = *(LONG *)(ULONG_PTR)(cam + 0x4c8);
        *T = *(LONG *)(ULONG_PTR)(cam + 0x4cc);
        *R = *(LONG *)(ULONG_PTR)(cam + 0x4d0);
        *B = *(LONG *)(ULONG_PTR)(cam + 0x4d4);
    }
}

static void __attribute__((thiscall)) hook_MMSetRect(void *self, const void *rect, int a, int b)
{
    LONG n = InterlockedIncrement(&g_mm_sr_n);
    short rl = 0, rt = 0, rr = 0, rb = 0;
    LONG wl = 0, wt = 0, wr = 0, wb = 0;
    int soft_w = 0, soft_h = 0;
    char js[320];

    if (rect && !IsBadReadPtr(rect, 8)) {
        const short *r = (const short *)rect;
        rl = r[0];
        rt = r[1];
        rr = r[2];
        rb = r[3];
    }
    real_MMSetRect(self, rect, a, b);
    if (self && !IsBadReadPtr((BYTE *)self + 0x7c, 4)) {
        wl = *(LONG *)((BYTE *)self + 0x70);
        wt = *(LONG *)((BYTE *)self + 0x74);
        wr = *(LONG *)((BYTE *)self + 0x78);
        wb = *(LONG *)((BYTE *)self + 0x7c);
    }
    (void)vk_present_soft_size(&soft_w, &soft_h);
    if (n <= 40 || (n % 30) == 0) {
        log_msg("MiniMap.SetRect #%ld in=(%d,%d)-(%d,%d) a=%d b=%d widget=(%ld,%ld)-(%ld,%ld) "
                "wh=%ldx%ld soft=%dx%d flag=%ld",
                (long)n, (int)rl, (int)rt, (int)rr, (int)rb, a, b, (long)wl, (long)wt, (long)wr,
                (long)wb, (long)(wr - wl + 1), (long)(wb - wt + 1), soft_w, soft_h,
                self ? (long)*(LONG *)((BYTE *)self + 0x8c) : -1L);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"in\":[%d,%d,%d,%d],\"ab\":[%d,%d],\"widget\":[%ld,%ld,%ld,%ld],"
                 "\"wh\":[%ld,%ld],\"soft\":[%d,%d],\"flag\":%ld}",
                 (long)n, (int)rl, (int)rt, (int)rr, (int)rb, a, b, (long)wl, (long)wt, (long)wr,
                 (long)wb, (long)(wr - wl + 1), (long)(wb - wt + 1), soft_w, soft_h,
                 self ? (long)*(LONG *)((BYTE *)self + 0x8c) : -1L);
        hooks_agent("H-R", "hooks.c:MMSetRect", "mm-setrect", js);
    }
}

static DWORD __attribute__((thiscall)) hook_MMBuild(void *self, void **pbmp, LONG sx, LONG sy)
{
    LONG n = InterlockedIncrement(&g_mm_bm_n);
    DWORD rc;
    unsigned a8, ac, b0, b4;
    short bw = 0, bh = 0;
    char js[280];

    mm_read_mgr(&a8, &ac, &b0, &b4);
    rc = real_MMBuild(self, pbmp, sx, sy);
    if (pbmp && *pbmp && !IsBadReadPtr(*pbmp, 12)) {
        /* CBitmap: +8 = short w, +10 = short h (used by DrawFlat MiniMapTerrain). */
        bw = *(short *)((BYTE *)*pbmp + 8);
        bh = *(short *)((BYTE *)*pbmp + 10);
    }
    if (n <= 40 || (n % 20) == 0) {
        log_msg("MiniMap.Build #%ld ask=%ldx%ld bmp=%dx%d mgr=[%u,%u,%u,%u] rc=%lu", (long)n,
                (long)sx, (long)sy, (int)bw, (int)bh, a8, ac, b0, b4, (unsigned long)rc);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"ask\":[%ld,%ld],\"bmp\":[%d,%d],\"mgr\":[%u,%u,%u,%u],\"rc\":%lu}",
                 (long)n, (long)sx, (long)sy, (int)bw, (int)bh, a8, ac, b0, b4, (unsigned long)rc);
        hooks_agent("H-T", "hooks.c:MMBuild", "mm-build", js);
    }
    return rc;
}

static void __attribute__((thiscall)) hook_MMDrawAfter(void *self, void *cdc, const void *dirty)
{
    LONG n = InterlockedIncrement(&g_mm_da_n);
    LONG wl = 0, wt = 0, wr = 0, wb = 0;
    LONG cL = 0, cT = 0, cR = 0, cB = 0;
    unsigned a8, ac, b0, b4;
    int soft_w = 0, soft_h = 0;
    short bw = 0, bh = 0;
    void *bmp;
    char js[360];

    if (self && !IsBadReadPtr((BYTE *)self + 0x7c, 4)) {
        wl = *(LONG *)((BYTE *)self + 0x70);
        wt = *(LONG *)((BYTE *)self + 0x74);
        wr = *(LONG *)((BYTE *)self + 0x78);
        wb = *(LONG *)((BYTE *)self + 0x7c);
        bmp = *(void **)((BYTE *)self + 0x5c);
        if (bmp && !IsBadReadPtr(bmp, 12)) {
            bw = *(short *)((BYTE *)bmp + 8);
            bh = *(short *)((BYTE *)bmp + 10);
        }
    }
    mm_read_mgr(&a8, &ac, &b0, &b4);
    mm_read_cam(&cL, &cT, &cR, &cB);
    (void)vk_present_soft_size(&soft_w, &soft_h);
    if (n <= 60 || (n % 45) == 0) {
        long ww = (long)(wr - wl + 1), wh = (long)(wb - wt + 1);
        log_msg("MiniMap.DrawAfter #%ld widget=(%ld,%ld) %ldx%ld bmp=%dx%d soft=%dx%d "
                "cam=[%ld,%ld-%ld,%ld] mgr=[%u,%u,%u,%u] aspect_w/h=%.3f",
                (long)n, (long)wl, (long)wt, ww, wh, (int)bw, (int)bh, soft_w, soft_h, (long)cL,
                (long)cT, (long)cR, (long)cB, a8, ac, b0, b4,
                wh > 0 ? (double)ww / (double)wh : 0.0);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"widget\":[%ld,%ld,%ld,%ld],\"wh\":[%ld,%ld],\"bmp\":[%d,%d],"
                 "\"soft\":[%d,%d],\"cam\":[%ld,%ld,%ld,%ld],\"mgr\":[%u,%u,%u,%u]}",
                 (long)n, (long)wl, (long)wt, (long)wr, (long)wb, ww, wh, (int)bw, (int)bh, soft_w,
                 soft_h, (long)cL, (long)cT, (long)cR, (long)cB, a8, ac, b0, b4);
        hooks_agent("H-R", "hooks.c:MMDrawAfter", "mm-draw", js);
    }
    real_MMDrawAfter(self, cdc, dirty);
}

static void hook_FlatMiniMapTerrain(void *bmp, LONG lod)
{
    LONG n = InterlockedIncrement(&g_mm_ft_n);
    short bw = 0, bh = 0;
    unsigned a8, ac, b0, b4;
    char js[200];

    if (bmp && !IsBadReadPtr(bmp, 12)) {
        bw = *(short *)((BYTE *)bmp + 8);
        bh = *(short *)((BYTE *)bmp + 10);
    }
    mm_read_mgr(&a8, &ac, &b0, &b4);
    if (n <= 40 || (n % 20) == 0) {
        log_msg("FlatMiniMapTerrain #%ld bmp=%dx%d lod=%ld mgr=[%u,%u,%u,%u]", (long)n, (int)bw,
                (int)bh, (long)lod, a8, ac, b0, b4);
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"bmp\":[%d,%d],\"lod\":%ld,\"mgr\":[%u,%u,%u,%u]}", (long)n, (int)bw,
                 (int)bh, (long)lod, a8, ac, b0, b4);
        hooks_agent("H-T", "hooks.c:FlatMM", "mm-flat", js);
    }
    real_FlatMiniMapTerrain(bmp, lod);
}

void hooks_minimap_install(void)
{
    /* MiniMap path — always on (aspect / right-panel debug). */
    g_mm_sr_target = (void *)(ULONG_PTR)0x004519E0u;
    g_mm_bm_target = (void *)(ULONG_PTR)0x004520E0u;
    g_mm_da_target = (void *)(ULONG_PTR)0x00451A80u;
    g_mm_ft_target = (void *)(ULONG_PTR)0x0045B590u;
    if (*(BYTE *)g_mm_sr_target == 0x8B) {
        if (install_inline_hook(g_mm_sr_target, (void *)hook_MMSetRect, g_mm_sr_steal, &g_mm_sr_tramp,
                                g_mm_sr_saved)) {
            real_MMSetRect = (PFN_MMSetRect)g_mm_sr_tramp;
            log_msg("inline hooked MiniMap.SetRect @ %p", g_mm_sr_target);
        } else
            log_msg("WARN: MiniMap.SetRect hook failed");
    } else
        log_msg("WARN: MiniMap.SetRect sig mismatch");
    if (*(BYTE *)g_mm_bm_target == 0x83) {
        if (install_inline_hook(g_mm_bm_target, (void *)hook_MMBuild, g_mm_bm_steal, &g_mm_bm_tramp,
                                g_mm_bm_saved)) {
            real_MMBuild = (PFN_MMBuild)g_mm_bm_tramp;
            log_msg("inline hooked MiniMap.Build @ %p", g_mm_bm_target);
        } else
            log_msg("WARN: MiniMap.Build hook failed");
    } else
        log_msg("WARN: MiniMap.Build sig mismatch");
    if (*(BYTE *)g_mm_da_target == 0x83) {
        if (install_inline_hook(g_mm_da_target, (void *)hook_MMDrawAfter, g_mm_da_steal,
                                &g_mm_da_tramp, g_mm_da_saved)) {
            real_MMDrawAfter = (PFN_MMDrawAfter)g_mm_da_tramp;
            log_msg("inline hooked MiniMap.DrawAfter @ %p", g_mm_da_target);
        } else
            log_msg("WARN: MiniMap.DrawAfter hook failed");
    } else
        log_msg("WARN: MiniMap.DrawAfter sig mismatch");
    if (*(BYTE *)g_mm_ft_target == 0x81) {
        if (install_inline_hook(g_mm_ft_target, (void *)hook_FlatMiniMapTerrain, g_mm_ft_steal,
                                &g_mm_ft_tramp, g_mm_ft_saved)) {
            real_FlatMiniMapTerrain = (PFN_FlatMiniMapTerrain)g_mm_ft_tramp;
            log_msg("inline hooked FlatMiniMapTerrain @ %p", g_mm_ft_target);
        } else
            log_msg("WARN: FlatMiniMapTerrain hook failed");
    } else
        log_msg("WARN: FlatMiniMapTerrain sig mismatch");

}

void hooks_minimap_remove(void)
{
    remove_inline_hook(g_mm_sr_target, g_mm_sr_steal, g_mm_sr_saved, g_mm_sr_tramp);
    g_mm_sr_tramp = NULL;
    remove_inline_hook(g_mm_bm_target, g_mm_bm_steal, g_mm_bm_saved, g_mm_bm_tramp);
    g_mm_bm_tramp = NULL;
    remove_inline_hook(g_mm_da_target, g_mm_da_steal, g_mm_da_saved, g_mm_da_tramp);
    g_mm_da_tramp = NULL;
    remove_inline_hook(g_mm_ft_target, g_mm_ft_steal, g_mm_ft_saved, g_mm_ft_tramp);
    g_mm_ft_tramp = NULL;

}
