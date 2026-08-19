#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

/* Rome retail color pass — helpers used by VFS hooks. */
static volatile LONG g_rome_focus;
static char g_last_rome_path[512];
static volatile LONG g_rome_rle_m0;
static volatile LONG g_rome_rle_m1;
static volatile LONG g_rome_rle_m2;
static volatile LONG g_rome_rle_other;
static volatile LONG g_rome_vfs_logs;
static volatile LONG g_rome_bpp_logs;
static volatile LONG g_pc_drawmode_logs;
static volatile LONG g_pc_create_logs;
static volatile LONG g_pc_compose_logs;
volatile LONG g_pc_vfs_logs;

/* ---- player_color path (CreatePlayerCol / drawmode switch) --------------
 * H-PCA: drawmode==2 (player_color) hits CreatePlayerCol for IArena layers.
 * H-PCB: CreatePlayerCol opens .red/.green/.blue sheets (VFS ok).
 * H-PCC: CreatePlayerCol fails (NOTFOUND) — no channel sheets in install.
 * H-PCD: IArena uses drawmode==1 (index/CreateIndexMem) instead of 2.
 * H-PCE: ComposeRGB (FUN_0046de10) runs after successful channel load.
 */

/* FUN_0046d810 — thiscall(self, path?, a3, a4, drawmode). case2→CreatePlayerCol. */
typedef void *(__attribute__((thiscall)) *PFN_ImgDrawMode)(void *self, void *a2, void *a3,
                                                           void *a4, unsigned mode);
static PFN_ImgDrawMode real_ImgDrawMode;
static BYTE *g_idm_tramp;
static BYTE g_idm_saved[16];
static SIZE_T g_idm_steal;
static void *g_idm_target;

/* FUN_0046da70 CreatePlayerCol — thiscall(self, path, a3, a4). */
typedef void *(__attribute__((thiscall)) *PFN_CreatePlayerCol)(void *self, const char *path,
                                                               void *a3, void *a4);
static PFN_CreatePlayerCol real_CreatePlayerCol;
static BYTE *g_cpc_tramp;
static BYTE g_cpc_saved[16];
static SIZE_T g_cpc_steal;
static void *g_cpc_target;

/* FUN_0046de10 ComposeRGB from R/G/B bitmaps — thiscall-ish; log entry+ret only. */
typedef void *(__attribute__((thiscall)) *PFN_ComposePlayerRGB)(void *self, void *r, void *g,
                                                                void *b, void *a5);
static PFN_ComposePlayerRGB real_ComposePlayerRGB;
static BYTE *g_crgb_tramp;
static BYTE g_crgb_saved[16];
static SIZE_T g_crgb_steal;
static void *g_crgb_target;

/* FUN_0046e8d0 CreateIndexMem — drawmode==1. */
typedef void *(__attribute__((thiscall)) *PFN_CreateIndexMem)(void *self, void *src, void *a3,
                                                              void *a4);
static PFN_CreateIndexMem real_CreateIndexMem;
static BYTE *g_cim_tramp;
static BYTE g_cim_saved[16];
static SIZE_T g_cim_steal;
static void *g_cim_target;
static volatile LONG g_pc_index_logs;

/* FUN_00470dc0 — ENT image open for remaping==none (.rle ignores drawmode at load). */
typedef int(__cdecl *PFN_EntOpenImage)(const char *path, unsigned rows, unsigned cols,
                                       unsigned drawmode, void **out_obj);
static PFN_EntOpenImage real_EntOpenImage;
static BYTE *g_eoi_tramp;
static BYTE g_eoi_saved[16];
static SIZE_T g_eoi_steal;
static void *g_eoi_target;
static volatile LONG g_pc_entopen_logs;

/* FUN_00463fd0 — parse ENT <image> attrs; stores drawmode at this+0x16a0. */
typedef int(__attribute__((thiscall)) *PFN_EntParseImage)(void *self, void *xml);
static PFN_EntParseImage real_EntParseImage;
static BYTE *g_epi_tramp;
static BYTE g_epi_saved[16];
static SIZE_T g_epi_steal;
static void *g_epi_target;
static volatile LONG g_pc_entparse_logs;
static volatile LONG g_pc_draw_logs;
static volatile LONG g_pc_remap_logs;
static volatile LONG g_pc_rampbuild_logs;

static int path_mentions_iarena(const char *path)
{
    char u[512];
    if (!path_is_safe(path))
        return 0;
    path_upper_copy(u, sizeof(u), path);
    return strstr(u, "IARENA") != NULL;
}

/* Rome building / unit assets under Buildings\R* (and a few named leaves). */
int path_mentions_rome(const char *path)
{
    char u[512];
    if (!path_is_safe(path))
        return 0;
    path_upper_copy(u, sizeof(u), path);
    if (strstr(u, "RARENA") || strstr(u, "RBARRACKS") || strstr(u, "RBLACKSMITH") ||
        strstr(u, "RHOUSE") || strstr(u, "RTOWNHALL") || strstr(u, "RTEMPLE") ||
        strstr(u, "ROUTPOST") || strstr(u, "RWAREHOUSE") || strstr(u, "RSHIPYARD") ||
        strstr(u, "RARCH") || strstr(u, "RCATAPULT") || strstr(u, "RWALL") ||
        strstr(u, "RTOWER"))
        return 1;
    /* Folder prefix Buildings\R… */
    if (strstr(u, "BUILDINGS\\R") || strstr(u, "BUILDINGS/R"))
        return 1;
    return 0;
}

void rome_arm_focus(const char *path)
{
    if (!path_mentions_rome(path))
        return;
    path_upper_copy(g_last_rome_path, sizeof(g_last_rome_path), path);
    InterlockedExchange(&g_rome_focus, 1200);
    InterlockedExchange(&g_rome_rle_m0, 0);
    InterlockedExchange(&g_rome_rle_m1, 0);
    InterlockedExchange(&g_rome_rle_m2, 0);
    InterlockedExchange(&g_rome_rle_other, 0);
    InterlockedExchange(&g_rome_bpp_logs, 0);
}

void log_rome_vfs(const char *api, const char *path, unsigned mode, int ok)
{
    char esc[520];
    LONG n;

    if (!path_mentions_rome(path))
        return;
    rome_arm_focus(path);
    n = InterlockedIncrement(&g_rome_vfs_logs);
    if (n > 250)
        return;
    json_escape(esc, sizeof(esc), path ? path : "");
    if (n <= 60)
        log_msg("rome-vfs #%ld %s %s ok=%d", (long)n, api, path ? path : "", ok);
}

static void *__attribute__((thiscall)) hook_ImgDrawMode(void *self, void *a2, void *a3, void *a4,
                                                        unsigned mode)
{
    void *ret;
    LONG n = InterlockedIncrement(&g_pc_drawmode_logs);
    char esc[400];
    const char *path = (const char *)a2;
    int iarena = path_mentions_iarena(path);
    int rome = path_mentions_rome(path);
    /* Always log first N + any IArena/Rome + any player_color/index (2/1).
     * Rome mode==0 is the critical negative signal (H-ROME-A). */
    if (n <= 200 || iarena || rome || mode == 1 || mode == 2) {
        if (rome)
            rome_arm_focus(path);
        json_escape(esc, sizeof(esc), path_is_safe(path) ? path : "");
        if (n <= 80 || iarena || rome || mode == 2)
            log_msg("ImgDrawMode #%ld mode=%u rome=%d iarena=%d path=%s", (long)n, mode, rome,
                    iarena, path_is_safe(path) ? path : "?");
    }
    ret = real_ImgDrawMode(self, a2, a3, a4, mode);
        return ret;
}

static void *__attribute__((thiscall)) hook_CreatePlayerCol(void *self, const char *path, void *a3,
                                                            void *a4)
{
    void *ret;
    LONG n = InterlockedIncrement(&g_pc_create_logs);
    char esc[400];
    int iarena = path_mentions_iarena(path);
    int rome = path_mentions_rome(path);
    if (rome)
        rome_arm_focus(path);
    json_escape(esc, sizeof(esc), path_is_safe(path) ? path : "");
    if (n <= 100 || iarena || rome)
        log_msg("CreatePlayerCol #%ld rome=%d iarena=%d path=%s", (long)n, rome, iarena,
                path_is_safe(path) ? path : "?");
    ret = real_CreatePlayerCol(self, path, a3, a4);
    return ret;
}

static void *__attribute__((thiscall)) hook_ComposePlayerRGB(void *self, void *r, void *g, void *b,
                                                             void *a5)
{
    void *ret;
    LONG n = InterlockedIncrement(&g_pc_compose_logs);
    ret = real_ComposePlayerRGB(self, r, g, b, a5);
    if (n <= 40)
        log_msg("ComposePlayerRGB #%ld ret=%p", (long)n, ret);
    return ret;
}

static void *__attribute__((thiscall)) hook_CreateIndexMem(void *self, void *src, void *a3,
                                                           void *a4)
{
    void *ret;
    LONG n = InterlockedIncrement(&g_pc_index_logs);
    int rome = g_rome_focus > 0;
        ret = real_CreateIndexMem(self, src, a3, a4);
        return ret;
}

static int __cdecl hook_EntOpenImage(const char *path, unsigned rows, unsigned cols,
                                     unsigned drawmode, void **out_obj)
{
    int ret;
    LONG n = InterlockedIncrement(&g_pc_entopen_logs);
    char esc[400];
    int iarena = path_mentions_iarena(path);
    int rome = path_mentions_rome(path);
    /* H-ROME-F: remaping=none still receives ENT drawmode here — prove Rome mode values. */
    if (n <= 300 || iarena || rome || drawmode == 1 || drawmode == 2) {
        if (rome)
            rome_arm_focus(path);
        json_escape(esc, sizeof(esc), path_is_safe(path) ? path : "");
        if (n <= 100 || iarena || rome || drawmode == 2)
            log_msg("EntOpenImage #%ld mode=%u rome=%d %s", (long)n, drawmode, rome,
                    path_is_safe(path) ? path : "?");
    }
    ret = real_EntOpenImage(path, rows, cols, drawmode, out_obj);
    if (n <= 300 || iarena || rome || drawmode == 2) {
        unsigned mode_out = 0;
        if (out_obj && *out_obj)
            mode_out = *(unsigned *)((BYTE *)(*out_obj) + 4);
    }
    return ret;
}

static int __attribute__((thiscall)) hook_EntParseImage(void *self, void *xml)
{
    int ret;
    LONG n = InterlockedIncrement(&g_pc_entparse_logs);
    unsigned mode = 0, rows = 0, cols = 0, remap = 0;

    ret = real_EntParseImage(self, xml);
    if (self) {
        rows = *(unsigned *)((BYTE *)self + 0x1698);
        cols = *(unsigned *)((BYTE *)self + 0x169c);
        mode = *(unsigned *)((BYTE *)self + 0x16a0);
        remap = *(unsigned *)((BYTE *)self + 0x16a4);
    }
    /* H-PCG: ENT parse stores player_color as drawmode=2 on object. */
    if (n <= 200 || mode == 1 || mode == 2) {
        if (n <= 80 || mode == 2)
            log_msg("EntParseImage #%ld mode=%u remap=%u rows=%u cols=%u ret=%d", (long)n, mode,
                    remap, rows, cols, ret);
    }
    return ret;
}

/*
 * Draw-time player_color (retail):
 * H-PCH: FUN_0046fa90 blits with image->mode at +4 (1=index, 2=player_color).
 * H-PCI: FUN_00473e00 cache path when DAT_007b9df0 set; is_pc=(mode==2).
 * H-PCJ: FUN_004740b0 copies team palette (op0 + is_pc) from DAT_008dd088 table.
 */

/* FUN_0046fa90 — RLE vtable draw; switch(mode). */
typedef void(__attribute__((thiscall)) *PFN_RleDraw)(void *self, void *dst, int x, int y,
                                                     void *owner);
static PFN_RleDraw real_RleDraw;
static BYTE *g_rd_tramp;
static BYTE g_rd_saved[16];
static SIZE_T g_rd_steal;
static void *g_rd_target;

/* Twin FUN_0046f520 */
static PFN_RleDraw real_RleDraw2;
static BYTE *g_rd2_tramp;
static BYTE g_rd2_saved[16];
static SIZE_T g_rd2_steal;
static void *g_rd2_target;

/* FUN_004740b0(recipe, bmp, is_pc, out16) — builds remapped RGB565 palette.
 * CC is stdcall: epilogue `pop ecx; ret 16` (confirmed VA 0x474237). cdecl crashed. */
typedef void(__stdcall *PFN_BuildPlayerPal)(unsigned *recipe, void *bmp, int is_pc, void *out16);
static PFN_BuildPlayerPal real_BuildPlayerPal;
static BYTE *g_bpp_tramp;
static BYTE g_bpp_saved[16];
static SIZE_T g_bpp_steal;
static void *g_bpp_target;

static void __attribute__((thiscall)) hook_RleDraw_common(void *self, void *dst, int x, int y,
                                                          void *owner, PFN_RleDraw real, int which)
{
    unsigned mode = 0;
    LONG n;
    LONG focus;
    LONG mc;
    char esc[200];
    int in_rome;

    if (self)
        mode = *(unsigned *)((BYTE *)self + 4);
    n = InterlockedIncrement(&g_pc_draw_logs);
    focus = g_rome_focus;
    in_rome = focus > 0;
    if (in_rome) {
        InterlockedDecrement(&g_rome_focus);
        if (mode == 0)
            mc = InterlockedIncrement(&g_rome_rle_m0);
        else if (mode == 1)
            mc = InterlockedIncrement(&g_rome_rle_m1);
        else if (mode == 2)
            mc = InterlockedIncrement(&g_rome_rle_m2);
        else
            mc = InterlockedIncrement(&g_rome_rle_other);
        /* Log early samples per mode while Rome assets are on screen. */
        if (mc <= 25) {
            json_escape(esc, sizeof(esc), g_last_rome_path);
        }
    }
    /* Cap spam: first 20 any, first 40 mode1, first 30 mode2 (mode2 proven). */
    if (n <= 20 || (mode == 1 && n <= 80) || (mode == 2 && n <= 50)) {
        if (n <= 20 || mode == 2)
            log_msg("RleDraw#%d #%ld mode=%u xy=(%d,%d)", which, (long)n, mode, x, y);
    }
    obj_soft_note_rle(x, y, owner);
    real(self, dst, x, y, owner);
}

static void __attribute__((thiscall)) hook_RleDraw(void *self, void *dst, int x, int y, void *owner)
{
    hook_RleDraw_common(self, dst, x, y, owner, real_RleDraw, 1);
}

static void __attribute__((thiscall)) hook_RleDraw2(void *self, void *dst, int x, int y, void *owner)
{
    hook_RleDraw_common(self, dst, x, y, owner, real_RleDraw2, 2);
}

static void __stdcall hook_BuildPlayerPal(unsigned *recipe, void *bmp, int is_pc, void *out16)
{
    LONG n = InterlockedIncrement(&g_pc_remap_logs);
    unsigned ops = 0, idx = 0, count = 0, r2 = 0, r3 = 0, r4 = 0;
    unsigned short *out;
    unsigned short *team;
    BYTE *base;
    int colors = 0;
    static volatile LONG s_full_dumps;

    if (recipe) {
        ops = recipe[0];
        idx = recipe[1];
        r2 = recipe[2];
        r3 = recipe[3];
        r4 = recipe[4];
        count = recipe[5];
    }
    if (bmp)
        colors = ((int *)bmp)[2]; /* param_2[2] = color count */
    (void)count;
    (void)r2;
    (void)r3;
    (void)r4;

    /* H-ROME-B/D: during Rome focus, log first N BuildPlayerPal (incl. is_pc=0). */
    if (is_pc || n <= 40 || g_rome_focus > 0) {
        int log_rome = 0;
        if (g_rome_focus > 0) {
            LONG rn = InterlockedIncrement(&g_rome_bpp_logs);
            log_rome = (rn <= 80);
        }
        if (is_pc || n <= 40 || log_rome) {
            if (is_pc || n <= 20 || log_rome)
                log_msg("BuildPlayerPal #%ld is_pc=%d ops=0x%x idx=%u colors=%d rome=%ld",
                        (long)n, is_pc, ops, idx, colors, (long)g_rome_focus);
        }
    }
    real_BuildPlayerPal(recipe, bmp, is_pc, out16);

    if (is_pc && out16 && InterlockedIncrement(&s_full_dumps) <= 3) {
        out = (unsigned short *)out16;
        log_msg("BuildPlayerPal #%ld POST64 out[0..3]=%04x %04x %04x %04x", (long)n, out[0], out[1],
                out[2], out[3]);
        base = *(BYTE **)(ULONG_PTR)0x008DD088;
        if (base) {
            team = (unsigned short *)(base + 0xC48 + idx * 0x25E);
            log_msg("BuildPlayerPal #%ld TEAM64 base=%p idx=%u team[0]=%04x", (long)n, base, idx,
                    team[0]);
        } else {
            log_msg("BuildPlayerPal #%ld TEAM64 base=NULL", (long)n);
        }
    }
}

/* FUN_0046c110(color555, dest) — 8x8 HSV shade ramp builder (cdecl, ret plain).
 * H-PCM: dest equals blit palette base+0xC48+idx*0x25E
 * H-PCN: dest equals object+0xCD4+i*0x25E+0x1D2 (== palette of player i+1)
 * H-PCO: color is RGB555 from PlayerColors */
typedef void(__cdecl *PFN_BuildRamp)(unsigned color555, unsigned short *dest);
static PFN_BuildRamp real_BuildRamp;
static BYTE *g_br_tramp;
static BYTE g_br_saved[16];
static SIZE_T g_br_steal;
static void *g_br_target;

static void __cdecl hook_BuildRamp(unsigned color555, unsigned short *dest)
{
    LONG n = InterlockedIncrement(&g_pc_rampbuild_logs);
    BYTE *base;
    unsigned short *out;
    int match_pal = -1, match_1d2 = -1, i;
    unsigned off_from_base = 0;

    base = *(BYTE **)(ULONG_PTR)0x008DD088;
    if (base && dest) {
        off_from_base = (unsigned)((BYTE *)dest - base);
        for (i = 0; i < 16; ++i) {
            if ((BYTE *)dest == base + 0xC48 + (unsigned)i * 0x25E)
                match_pal = i;
            if ((BYTE *)dest == base + 0xCD4 + (unsigned)i * 0x25E + 0x1D2)
                match_1d2 = i;
        }
    }

    if (n <= 40) {
        log_msg("BuildRamp #%ld color=%04x dest=%p off=0x%x pal=%d 1d2=%d", (long)n,
                color555 & 0xffff, (void *)dest, off_from_base, match_pal, match_1d2);
    }

    real_BuildRamp(color555, dest);

    if (dest && n <= 12) {
        out = dest;
        log_msg("BuildRamp #%ld POST out[0]=%04x out[63]=%04x", (long)n, out[0], out[63]);
    }
}

