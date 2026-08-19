#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include <string.h>

int g_scan_soft_max = CK_SCANLINE_N; /* 1920 after expand+stack-grow */

/*
 * Soft scanline: retail 1600 BSS entries (I2 @ 0x7A1918 + side @ 0x7A6434).
 * Soft 1920 needs (1) table expand and (2) stack frame grow in 0x457020:
 * flags[ecx] at [esp+0x94] packed against locals at [esp+0x6d8]; ecx→1919
 * smashes return path → EIP 0xFFBAB0B0 (expand-only runs). Index-only clamps
 * rejected: AV @ 0x457466 with GetRow==1919.
 */
volatile LONG g_scan_clamps; /* also read from dm_replace VEH */
static BYTE *g_scan_guard_tramp[5];
static BYTE g_scan_guard_saved[5][16];
static void *g_scan_guard_site[5];
static SIZE_T g_scan_guard_steal = 14;
static BYTE *g_scan_i2_tab;
static BYTE *g_scan_i2_side;
static BYTE *g_scan_i2_idx_tramp;
static BYTE g_scan_i2_idx_saved[16];
static void *g_scan_i2_idx_site;

static int patch_u32_if(void *at, DWORD expect, DWORD neu)
{
    DWORD oldprot;
    if (!at || *(DWORD *)at != expect)
        return 0;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &oldprot))
        return 0;
    *(DWORD *)at = neu;
    VirtualProtect(at, 4, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return 1;
}

/* Guard side-table index: ecx in [0, n). Expand-era crash had ecx=-1 → side-4. */
static int install_i2_side_index_guard(DWORD n, DWORD side)
{
    void *site = (void *)(ULONG_PTR)0x00457441u; /* lea edx,[ecx*4+side] */
    void *cont = (void *)(ULONG_PTR)0x00457448u;
    void *exit_loop = (void *)(ULONG_PTR)0x0045749Au;
    BYTE *tramp;
    BYTE *p;
    DWORD oldprot;
    INT32 rel;
    SIZE_T steal = 7;
    DWORD abs_ctr;

    if (*(BYTE *)site != 0x8D)
        return 0;
    tramp = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return 0;
    p = tramp;
    *p++ = 0x81;
    *p++ = 0xF9;
    memcpy(p, &n, 4);
    p += 4;
    *p++ = 0x73;
    *p++ = 0x10;
    *p++ = 0x85;
    *p++ = 0xC9;
    *p++ = 0x78;
    *p++ = 0x0C;
    *p++ = 0x8D;
    *p++ = 0x14;
    *p++ = 0x8D;
    memcpy(p, &side, 4);
    p += 4;
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)cont - (p + 4));
    memcpy(p, &rel, 4);
    p += 4;
    *p++ = 0xF0;
    *p++ = 0xFF;
    *p++ = 0x05;
    abs_ctr = (DWORD)(ULONG_PTR)&g_scan_clamps;
    memcpy(p, &abs_ctr, 4);
    p += 4;
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)exit_loop - (p + 4));
    memcpy(p, &rel, 4);

    memcpy(g_scan_i2_idx_saved, site, steal);
    if (!VirtualProtect(site, steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    *(BYTE *)site = 0xE9;
    rel = (INT32)(tramp - ((BYTE *)site + 5));
    memcpy((BYTE *)site + 1, &rel, 4);
    ((BYTE *)site)[5] = 0x90;
    ((BYTE *)site)[6] = 0x90;
    VirtualProtect(site, steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), site, steal);
    g_scan_i2_idx_tramp = tramp;
    g_scan_i2_idx_site = site;
    return 1;
}

/* Grow 0x457020 frame so flags[1920] at [esp+0x94] don't smash locals @ 0x6d8. */
static int install_i2_soft1920_stack_grow(void)
{
    enum { DELTA = 0x140 }; /* 1920-1600 */
    DWORD frame_old = 0x6c4;
    DWORD frame_new = frame_old + DELTA;
    int ok = 0;
    int i;
    static const DWORD high_old[] = {0x6d8, 0x6dc, 0x6e0, 0x6e4};
    static const DWORD high_at[] = {
        /* 0x6d8 */
        0x0045702Fu, 0x0045714Au, 0x004571A1u, 0x004572BDu, 0x004572FEu, 0x00457356u,
        0x00457367u, 0x00457397u, 0x00457430u,
        /* 0x6dc */
        0x00457161u, 0x00457180u, 0x0045737Bu, 0x004573FDu, 0x0045740Au, 0x00457423u,
        0x00457474u, 0x0045749Du, 0x004574AEu,
        /* 0x6e0 */
        0x00457151u, 0x0045735Du, 0x0045739Eu, 0x00457437u, 0x0045748Bu,
        /* 0x6e4 */
        0x0045715Au, 0x004571A8u, 0x00457382u, 0x004573F6u, 0x004574A4u,
    };
    static const DWORD high_expect[] = {
        0x6d8, 0x6d8, 0x6d8, 0x6d8, 0x6d8, 0x6d8, 0x6d8, 0x6d8, 0x6d8, 0x6dc, 0x6dc,
        0x6dc, 0x6dc, 0x6dc, 0x6dc, 0x6dc, 0x6dc, 0x6dc, 0x6e0, 0x6e0, 0x6e0, 0x6e0,
        0x6e0, 0x6e4, 0x6e4, 0x6e4, 0x6e4, 0x6e4,
    };

    (void)high_old;
    if (!patch_u32_if((void *)(ULONG_PTR)0x00457022u, frame_old, frame_new))
        return 0;
    ok++;
    if (!patch_u32_if((void *)(ULONG_PTR)0x004574D9u, frame_old, frame_new))
        return 0;
    ok++;
    for (i = 0; i < (int)(sizeof(high_at) / sizeof(high_at[0])); ++i) {
        if (patch_u32_if((void *)(ULONG_PTR)high_at[i], high_expect[i], high_expect[i] + DELTA))
            ok++;
        else
            log_msg("WARN: stack-grow patch fail @ %08lx", (unsigned long)high_at[i]);
    }
    log_msg("scanline I2 stack-grow frame 0x%lx->0x%lx patches=%d/30", (unsigned long)frame_old,
            (unsigned long)frame_new, ok);

    return ok >= 30;
}

static int install_i2_scanline_expand(void)
{
    BYTE *init = (BYTE *)(ULONG_PTR)0x0045706Eu;
    DWORD base, end, side, n = (DWORD)CK_SCAN_I2_N;
    int ok = 0;
    int i;

    if (init[0] != 0xBE || *(DWORD *)(init + 1) != (DWORD)CK_SCAN_I2_BASE || init[5] != 0xBF ||
        *(DWORD *)(init + 6) != (DWORD)CK_SCANLINE_N)
        return 0;

    g_scan_i2_tab =
        (BYTE *)VirtualAlloc(NULL, (SIZE_T)n * 12u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_scan_i2_side =
        (BYTE *)VirtualAlloc(NULL, (SIZE_T)n * 4u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_scan_i2_tab || !g_scan_i2_side)
        return 0;
    base = (DWORD)(ULONG_PTR)g_scan_i2_tab;
    end = base + n * 12u;
    side = (DWORD)(ULONG_PTR)g_scan_i2_side;

    {
        static const struct {
            DWORD at;
            DWORD expect;
            const char *tag;
        } spec[14] = {
            {0x0045706Fu, CK_SCAN_I2_BASE, "base-init"},
            {0x00457074u, CK_SCANLINE_N, "count-init"},
            {0x004570A5u, CK_SCAN_I2_BASE, "base-grow"},
            {0x0045711Fu, CK_SCAN_I2_END, "end-grow"},
            {0x00457126u, CK_SCAN_I2_BASE + 4u, "base4-a"},
            {0x00457141u, CK_SCAN_I2_END + 4u, "end4-a"},
            {0x004572CBu, CK_SCAN_I2_BASE, "base-wr0"},
            {0x0045730Cu, CK_SCAN_I2_BASE, "base-wr1"},
            {0x004573A5u, CK_SCAN_I2_SIDE, "side-mov"},
            {0x004573AAu, CK_SCAN_I2_BASE + 4u, "base4-b"},
            {0x004573EDu, CK_SCAN_I2_END + 4u, "end4-b"},
            {0x00457444u, CK_SCAN_I2_SIDE, "side-lea"},
            {0x004574E3u, CK_SCAN_I2_END, "end-dtor"},
            {0x004574E8u, CK_SCANLINE_N, "count-dtor"},
        };
        DWORD neu[14];
        neu[0] = base;
        neu[1] = n;
        neu[2] = base;
        neu[3] = end;
        neu[4] = base + 4u;
        neu[5] = end + 4u;
        neu[6] = base;
        neu[7] = base;
        neu[8] = side;
        neu[9] = base + 4u;
        neu[10] = end + 4u;
        neu[11] = side;
        neu[12] = end;
        neu[13] = n;
        for (i = 0; i < 14; ++i) {
            if (patch_u32_if((void *)(ULONG_PTR)spec[i].at, spec[i].expect, neu[i])) {
                ok++;
            } else {
                log_msg("scanline I2 patch FAIL %s @ %08lx expect=%08lx got=%08lx", spec[i].tag,
                        (unsigned long)spec[i].at, (unsigned long)spec[i].expect,
                        (unsigned long)*(DWORD *)(ULONG_PTR)spec[i].at);
            }
        }
    }

    if (ok < 14) {
        log_msg("scanline I2 expand aborted patches=%d/14", ok);
        return 0;
    }
    if (!install_i2_side_index_guard(n, side)) {
        log_msg("WARN: I2 side index guard failed");
        return 0;
    }

    log_msg("scanline I2 expand %dx12 (+side) @ %p end=%p", (int)n, (void *)g_scan_i2_tab,
            (void *)(ULONG_PTR)end);

    return 1;
}

int ck_scan_probe(int idx, unsigned *begin, unsigned *cur, unsigned *end)
{
    const DWORD *p;
    if (!g_scan_i2_tab || idx < 0 || idx >= (int)CK_SCAN_I2_N)
        return 0;
    p = (const DWORD *)(g_scan_i2_tab + (size_t)idx * 12u);
    if (begin)
        *begin = p[0];
    if (cur)
        *cur = p[1];
    if (end)
        *end = p[2];
    return 1;
}

/* Clamp eax = [esp+0x6d8] to <=1599 before span-table indexed write. */
static int install_i2_span_index_clamp(void *site, void *cont, int slot)
{
    BYTE *tramp;
    BYTE *p;
    DWORD oldprot;
    INT32 rel;
    SIZE_T steal = 7;
    DWORD abs_ctr;
    static const BYTE expect[7] = {0x8b, 0x84, 0x24, 0xd8, 0x06, 0x00, 0x00};

    if (slot < 0 || slot > 1 || !site || !cont)
        return 0;
    if (memcmp(site, expect, 7) != 0)
        return 0;
    tramp = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return 0;
    p = tramp;
    memcpy(p, expect, 7); /* mov eax,[esp+0x6d8] */
    p += 7;
    /* cmp eax, 1600 ; jb ok ; lock inc; mov eax,1599 */
    *p++ = 0x3D;
    *p++ = 0x40;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x72;
    *p++ = 0x0C;
    *p++ = 0xF0;
    *p++ = 0xFF;
    *p++ = 0x05;
    abs_ctr = (DWORD)(ULONG_PTR)&g_scan_clamps;
    memcpy(p, &abs_ctr, 4);
    p += 4;
    *p++ = 0xB8;
    *p++ = 0x3F;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)cont - (p + 4));
    memcpy(p, &rel, 4);

    memcpy(g_scan_guard_saved[slot], site, steal);
    if (!VirtualProtect(site, steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    *(BYTE *)site = 0xE9;
    rel = (INT32)(tramp - ((BYTE *)site + 5));
    memcpy((BYTE *)site + 1, &rel, 4);
    ((BYTE *)site)[5] = 0x90;
    ((BYTE *)site)[6] = 0x90;
    VirtualProtect(site, steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), site, steal);
    g_scan_guard_tramp[slot] = tramp;
    g_scan_guard_site[slot] = site;
    return 1;
}

/* Side dword table @ 0x7A6434[1600]: soft>1600 sets end=[esp+0x6e0] (e.g. 1861)
 * while table is only 1600. Crash @ 0x45746F mov ebx,[esi] with ecx=1602, esi=0.
 * Cap inclusive end to 1599 and skip when start ecx is OOB. */
static int install_i2_side_index_clamp(void)
{
    void *site = (void *)(ULONG_PTR)0x0045742Du; /* mov ecx,[esp+0x6d8] */
    void *cont = (void *)(ULONG_PTR)0x00457434u;
    void *exit_loop = (void *)(ULONG_PTR)0x0045749Au;
    BYTE *tramp;
    BYTE *p;
    DWORD oldprot;
    INT32 rel;
    SIZE_T steal = 7;
    DWORD abs_ctr;
    static const BYTE expect[7] = {0x8b, 0x8c, 0x24, 0xd8, 0x06, 0x00, 0x00};

    if (memcmp(site, expect, 7) != 0)
        return 0;
    tramp = (BYTE *)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return 0;
    p = tramp;
    /* mov ecx,[esp+0x6d8] */
    memcpy(p, expect, 7);
    p += 7;
    /* mov eax,[esp+0x6e0] ; end inclusive */
    *p++ = 0x8B;
    *p++ = 0x84;
    *p++ = 0x24;
    *p++ = 0xE0;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* cmp eax, 1599 ; jle end_ok */
    *p++ = 0x3D;
    *p++ = 0x3F;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x7E;
    *p++ = 0x12; /* skip lockinc(7)+mov dword(11)=18 */
    /* lock inc [g_scan_clamps] */
    *p++ = 0xF0;
    *p++ = 0xFF;
    *p++ = 0x05;
    abs_ctr = (DWORD)(ULONG_PTR)&g_scan_clamps;
    memcpy(p, &abs_ctr, 4);
    p += 4;
    /* mov dword [esp+0x6e0], 1599 */
    *p++ = 0xC7;
    *p++ = 0x84;
    *p++ = 0x24;
    *p++ = 0xE0;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x3F;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* end_ok: cmp ecx, 1600 ; jae skip ; test ecx,ecx ; js skip ; jmp cont */
    *p++ = 0x81;
    *p++ = 0xF9;
    *p++ = 0x40;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x73;
    *p++ = 0x09;
    *p++ = 0x85;
    *p++ = 0xC9;
    *p++ = 0x78;
    *p++ = 0x05;
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)cont - (p + 4));
    memcpy(p, &rel, 4);
    p += 4;
    /* skip: count + exit loop */
    *p++ = 0xF0;
    *p++ = 0xFF;
    *p++ = 0x05;
    abs_ctr = (DWORD)(ULONG_PTR)&g_scan_clamps;
    memcpy(p, &abs_ctr, 4);
    p += 4;
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)exit_loop - (p + 4));
    memcpy(p, &rel, 4);

    memcpy(g_scan_guard_saved[2], site, steal);
    if (!VirtualProtect(site, steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    *(BYTE *)site = 0xE9;
    rel = (INT32)(tramp - ((BYTE *)site + 5));
    memcpy((BYTE *)site + 1, &rel, 4);
    ((BYTE *)site)[5] = 0x90;
    ((BYTE *)site)[6] = 0x90;
    VirtualProtect(site, steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), site, steal);
    g_scan_guard_tramp[2] = tramp;
    g_scan_guard_site[2] = site;
    return 1;
}

static int install_i2_soft1920_clamps(void)
{
    BYTE *sig = (BYTE *)(ULONG_PTR)0x0045706Eu;
    int ok = 0;
    /* I2 only: mov esi, 0x7A1918 */
    if (sig[0] != 0xBE || *(DWORD *)(sig + 1) != (DWORD)CK_SCAN_I2_BASE)
        return 0;
    if (install_i2_span_index_clamp((void *)(ULONG_PTR)0x004572BAu, (void *)(ULONG_PTR)0x004572C1u, 0))
        ok++;
    if (install_i2_span_index_clamp((void *)(ULONG_PTR)0x004572FBu, (void *)(ULONG_PTR)0x00457302u, 1))
        ok++;
    if (install_i2_side_index_clamp())
        ok++;
    if (ok < 3) {
        log_msg("WARN: I2 soft1920 clamps incomplete (%d/3)", ok);
        return 0;
    }
    g_scan_soft_max = CK_FORCE_W; /* allow CVM 1920×1080; spans clamped to 1600 */
    log_msg("scanline I2 soft1920 clamps ok soft_max=%d (tab %d)", g_scan_soft_max, CK_SCANLINE_N);

    return 1;
}

static BOOL install_scanline_write_clamp(void *site, void *cont, int which)
{
    BYTE *tramp;
    BYTE *p;
    DWORD oldprot;
    INT32 rel;
    SIZE_T i;
    DWORD abs_ctr;
    static const BYTE expect[3] = {0x8b, 0x54, 0x24}; /* mov edx,[esp+…] */

    if (which < 0 || which > 1 || !site || !cont)
        return FALSE;
    if (memcmp(site, expect, 3) != 0)
        return FALSE;

    tramp = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return FALSE;
    p = tramp;
    /* cmp eax, 1600 */
    *p++ = 0x3D;
    *p++ = 0x40;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* jb ok (index < 1600) */
    *p++ = 0x72;
    *p++ = 0x0C;
    /* lock inc [g_scan_clamps] */
    *p++ = 0xF0;
    *p++ = 0xFF;
    *p++ = 0x05;
    abs_ctr = (DWORD)(ULONG_PTR)&g_scan_clamps;
    memcpy(p, &abs_ctr, 4);
    p += 4;
    /* mov eax, 1599 */
    *p++ = 0xB8;
    *p++ = 0x3F;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* ok: stolen original 14 bytes */
    memcpy(p, site, g_scan_guard_steal);
    p += g_scan_guard_steal;
    /* jmp cont */
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)cont - (p + 4));
    memcpy(p, &rel, 4);

    /* Fix jb offset: from after 72 XX to ok.
     * skip path was 12 bytes (lockinc 7 + mov eax 5); ok follows.
     * jb at offset 5, next=7, +12 → 19. stolen starts at 19. OK. */

    memcpy(g_scan_guard_saved[which], site, g_scan_guard_steal);
    if (!VirtualProtect(site, g_scan_guard_steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return FALSE;
    }
    *(BYTE *)site = 0xE9;
    rel = (INT32)(tramp - ((BYTE *)site + 5));
    memcpy((BYTE *)site + 1, &rel, 4);
    for (i = 5; i < g_scan_guard_steal; ++i)
        ((BYTE *)site)[i] = 0x90;
    VirtualProtect(site, g_scan_guard_steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), site, g_scan_guard_steal);
    g_scan_guard_tramp[which] = tramp;
    g_scan_guard_site[which] = site;
    return TRUE;
}

static BOOL install_scanline_consume_guard(void)
{
    /*
     * Clamp end Y [esp+0x6e0] to <=1599 before inner consume loop.
     * v3 bug (log): trampoline reused eax → 456e4d mov ebx,eax got 1599 →
     * fault read at 0x63f. Preserve eax (call result); use edx for end.
     * Site @ 456e3d (14 bytes) → cont 456e4b (jg). Esp offsets +4 after push.
     */
    void *site = (void *)(ULONG_PTR)0x00456E3D;
    void *cont = (void *)(ULONG_PTR)0x00456E4B;
    void *exit_loop = (void *)(ULONG_PTR)0x00456EAA;
    BYTE *tramp;
    BYTE *p;
    DWORD oldprot;
    INT32 rel;
    SIZE_T i;
    DWORD abs_ctr;
    static const BYTE expect[7] = {0x8b, 0x8c, 0x24, 0xd8, 0x06, 0x00, 0x00};

    if (memcmp(site, expect, 7) != 0)
        return FALSE;
    tramp = (BYTE *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return FALSE;
    p = tramp;
    *p++ = 0x50; /* push eax */
    /* mov ecx, [esp+0x6dc]  (0x6d8+4) */
    memcpy(p, "\x8b\x8c\x24\xdc\x06\x00\x00", 7);
    p += 7;
    /* mov edx, [esp+0x6e4]  (0x6e0+4) */
    memcpy(p, "\x8b\x94\x24\xe4\x06\x00\x00", 7);
    p += 7;
    /* cmp edx, 1599 */
    *p++ = 0x81;
    *p++ = 0xFA;
    *p++ = 0x3F;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* jle end_ok */
    *p++ = 0x7E;
    *p++ = 0x0C;
    *p++ = 0xF0;
    *p++ = 0xFF;
    *p++ = 0x05;
    abs_ctr = (DWORD)(ULONG_PTR)&g_scan_clamps;
    memcpy(p, &abs_ctr, 4);
    p += 4;
    /* mov edx, 1599 */
    *p++ = 0xBA;
    *p++ = 0x3F;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* end_ok: mov [esp+0x6e4], edx */
    memcpy(p, "\x89\x94\x24\xe4\x06\x00\x00", 7);
    p += 7;
    /* cmp ecx, 1600 */
    *p++ = 0x81;
    *p++ = 0xF9;
    *p++ = 0x40;
    *p++ = 0x06;
    *p++ = 0x00;
    *p++ = 0x00;
    /* jb start_ok */
    *p++ = 0x72;
    *p++ = 0x06;
    *p++ = 0x58; /* pop eax */
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)exit_loop - (p + 4));
    memcpy(p, &rel, 4);
    p += 4;
    /* start_ok: cmp ecx, edx ; pop eax ; jmp cont */
    *p++ = 0x3B;
    *p++ = 0xCA;
    *p++ = 0x58;
    *p++ = 0xE9;
    rel = (INT32)((BYTE *)cont - (p + 4));
    memcpy(p, &rel, 4);

    memcpy(g_scan_guard_saved[2], site, g_scan_guard_steal);
    if (!VirtualProtect(site, g_scan_guard_steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return FALSE;
    }
    *(BYTE *)site = 0xE9;
    rel = (INT32)(tramp - ((BYTE *)site + 5));
    memcpy((BYTE *)site + 1, &rel, 4);
    for (i = 5; i < g_scan_guard_steal; ++i)
        ((BYTE *)site)[i] = 0x90;
    VirtualProtect(site, g_scan_guard_steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), site, g_scan_guard_steal);
    g_scan_guard_tramp[2] = tramp;
    g_scan_guard_site[2] = site;
    return TRUE;
}

void install_scanline_oob_guards(void)
{
    /* Soft 1920: expand tables + grow stack flags[] frame (H108). */
    if (install_i2_scanline_expand()) {
        if (install_i2_soft1920_stack_grow()) {
            g_scan_soft_max = CK_FORCE_W;
            log_msg("scanline I2 soft1920 ready soft_max=%d", g_scan_soft_max);

            return;
        }
        log_msg("WARN: I2 stack-grow failed — keeping soft_max=%d", g_scan_soft_max);
    }
    if (install_scanline_write_clamp((void *)(ULONG_PTR)0x00456CD1, (void *)(ULONG_PTR)0x00456CDF, 0))
        log_msg("scanline write-clamp #0 @ 00456cd1 (tab %d)", CK_SCANLINE_N);
    else
        log_msg("WARN: scanline write-clamp #0 failed");
    if (install_scanline_write_clamp((void *)(ULONG_PTR)0x00456D12, (void *)(ULONG_PTR)0x00456D20, 1))
        log_msg("scanline write-clamp #1 @ 00456d12");
    else
        log_msg("WARN: scanline write-clamp #1 failed");
    if (install_scanline_consume_guard())
        log_msg("scanline consume end-clamp @ 00456e3d");
    else
        log_msg("WARN: scanline consume end-clamp failed");
}
