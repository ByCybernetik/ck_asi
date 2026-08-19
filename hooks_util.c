#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "vk_present.h"
#include "hitch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/*
 * CK_PROXY_PROFILE:
 *   min (default) — soft-res/CVM/movie/SetDIBits; no Wait/Sleep IAT, no terrain draw hooks
 *   full — Wait/Sleep hitch + terrain probes (debug only; caused freezes)
 * CK_DI_PATCH=1 — Wait(100)→1 at DirectInput pump (default ON; kills ~100ms cam freezes)
 * CK_FORCE_RES=1 — force CDS/SetWindowPos/boot to 1920×1080 (default off)
 */
int g_proxy_min = 1;
int g_di_patch = 0;
int g_hitch_wait = 0;
int g_terrain_trace = 0;
int g_force_res = 0;

int env_is(const char *name, const char *want)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    const char *v = NULL;
    if (n > 0 && n < sizeof(buf))
        v = buf;
    else
        v = getenv(name);
    return v && want && strcmp(v, want) == 0;
}

int env_on(const char *name, int def)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    const char *v = NULL;
    char c0;
    if (n > 0 && n < sizeof(buf))
        v = buf;
    else
        v = getenv(name);
    if (!v || !v[0])
        return def;
    c0 = v[0];
    if (c0 == '0' || c0 == 'f' || c0 == 'F' || c0 == 'n' || c0 == 'N')
        return 0;
    return 1;
}

char g_game_root[MAX_PATH];
char g_ck_phase[192];

void hooks_crash_phase(const char *fmt, ...)
{
    va_list ap;
    if (!fmt)
        return;
    va_start(ap, fmt);
    vsnprintf(g_ck_phase, sizeof(g_ck_phase), fmt, ap);
    va_end(ap);
    g_ck_phase[sizeof(g_ck_phase) - 1] = '\0';
    hitch_mark(g_ck_phase);
}

void hooks_get_crash_ctx(char *out, size_t n)
{
    int sw = 0, sh = 0;
    char mark[96];
    if (!out || n == 0)
        return;
    vk_present_soft_size(&sw, &sh);
    hitch_get_mark(mark, sizeof(mark));
    snprintf(out, n,
             "phase=%.80s mark=%.60s epoch=%ld scen=%.120s soft=%dx%d map=%p deep=%ld mf=%u,%u",
             g_ck_phase[0] ? g_ck_phase : "-", mark[0] ? mark : "-", (long)g_map_epoch,
             g_last_scenario[0] ? g_last_scenario : "-", sw, sh, (void *)g_last_attach_map,
             (long)g_deep_attach, g_last_map_f4, g_last_map_f8);
    out[n - 1] = '\0';
}
void resolve_game_root(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    char *slash;
    DWORD n = GetModuleFileNameA(game, g_game_root, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        g_game_root[0] = '\0';
        return;
    }
    slash = strrchr(g_game_root, '\\');
    if (!slash)
        slash = strrchr(g_game_root, '/');
    if (slash)
        slash[1] = '\0';
}

void path_join(char *out, size_t out_n, const char *leaf)
{
    size_t n = (size_t)lstrlenA(g_game_root);
    size_t ln = (size_t)lstrlenA(leaf);
    if (n + ln + 1 >= out_n) {
        out[0] = '\0';
        return;
    }
    lstrcpyA(out, g_game_root);
    lstrcatA(out, leaf);
}

void dump_bmp24(const char *path, int w, int h, int src_bpp, int src_stride,
                       const BYTE *bits, int bottom_up)
{
    FILE *f;
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes & 3)) & 3;
    uint32_t img_size = (uint32_t)((row_bytes + pad) * h);
    uint32_t file_size = 54 + img_size;
    BYTE row[4096 * 3 + 4];
    int y;

    if (w <= 0 || h <= 0 || w > 4096 || !bits)
        return;
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("BM", 1, 2, f);
    fwrite(&file_size, 4, 1, f);
    {
        uint32_t z = 0, off = 54, dib = 40;
        uint16_t planes = 1, bpp = 24;
        uint32_t comp = 0, ppm = 2835, colors = 0;
        int32_t wi = w, hi = h;
        fwrite(&z, 4, 1, f);
        fwrite(&off, 4, 1, f);
        fwrite(&dib, 4, 1, f);
        fwrite(&wi, 4, 1, f);
        fwrite(&hi, 4, 1, f);
        fwrite(&planes, 2, 1, f);
        fwrite(&bpp, 2, 1, f);
        fwrite(&comp, 4, 1, f);
        fwrite(&img_size, 4, 1, f);
        fwrite(&ppm, 4, 1, f);
        fwrite(&ppm, 4, 1, f);
        fwrite(&colors, 4, 1, f);
        fwrite(&colors, 4, 1, f);
    }
    for (y = 0; y < h; ++y) {
        int sy = bottom_up ? y : (h - 1 - y);
        const BYTE *src = bits + sy * src_stride;
        int x;
        memset(row, 0, (size_t)row_bytes + (size_t)pad);
        for (x = 0; x < w; ++x) {
            BYTE r = 0, g = 0, b = 0;
            if (src_bpp == 15 || src_bpp == 16) {
                uint16_t p = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
                if (src_bpp == 15) {
                    /* RGB555 */
                    r = (BYTE)(((p >> 10) & 31) * 255 / 31);
                    g = (BYTE)(((p >> 5) & 31) * 255 / 31);
                    b = (BYTE)((p & 31) * 255 / 31);
                } else {
                    /* RGB565 */
                    r = (BYTE)(((p >> 11) & 31) * 255 / 31);
                    g = (BYTE)(((p >> 5) & 63) * 255 / 63);
                    b = (BYTE)((p & 31) * 255 / 31);
                }
            } else if (src_bpp == 24) {
                b = src[x * 3];
                g = src[x * 3 + 1];
                r = src[x * 3 + 2];
            } else if (src_bpp == 32) {
                b = src[x * 4];
                g = src[x * 4 + 1];
                r = src[x * 4 + 2];
            }
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, 1, (size_t)row_bytes + (size_t)pad, f);
    }
    fclose(f);
}
int path_is_safe(const char *p)
{
    size_t i;
    if (!p)
        return 0;
    for (i = 0; i < 480 && p[i]; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < 32 || c > 126)
            return 0;
    }
    return i > 0 && i < 480;
}

void path_upper_copy(char *dst, size_t n, const char *src)
{
    size_t i;
    for (i = 0; i + 1 < n && src[i]; ++i) {
        char c = src[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            c = '\\';
        dst[i] = c;
    }
    dst[i] = '\0';
}
void json_escape(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    size_t i;
    for (i = 0; src[i] && o + 2 < n; ++i) {
        char c = src[i];
        if (c == '\\' || c == '"') {
            if (o + 3 >= n)
                break;
            dst[o++] = '\\';
            dst[o++] = c;
        } else if ((unsigned char)c < 32) {
            dst[o++] = '?';
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}
int hooks_debug_full(void)
{
    static int s_full = -1;
    if (s_full < 0) {
        char b[8];
        DWORD n = GetEnvironmentVariableA("CK_DEBUG_FULL", b, sizeof(b));
        s_full = (n > 0 && n < sizeof(b) && (b[0] == '1' || b[0] == 'y' || b[0] == 'Y')) ? 1 : 0;
    }
    return s_full;
}

void hooks_agent(const char *hid, const char *loc, const char *msg, const char *data_json)
{
    FILE *df;
    /* #region agent log */
    /*
     * Hot-path fopen/fclose (~100+/s) can itself hitch the main thread.
     * Default: only freeze-relevant messages. CK_DEBUG_FULL=1 restores all.
     */
    if (!hooks_debug_full()) {
        if (!msg)
            return;
        if (strcmp(msg, "dib-gap") && strcmp(msg, "gtc-boost") && strcmp(msg, "gtc-spin") &&
            strcmp(msg, "gtc-spin-mid") && strcmp(msg, "present-timing") &&
            strcmp(msg, "tmr-clamp") && strcmp(msg, "tmr-install-ok") &&
            strcmp(msg, "tmr-install-fail") && strcmp(msg, "cam-center-slow") &&
            strcmp(msg, "cam-pan") &&
            strcmp(msg, "wait64-patch-ok") && strcmp(msg, "wait64-patch-fail") &&
            strcmp(msg, "cam-slow") && strcmp(msg, "terrain-in-submit") &&
            strcmp(msg, "decor-in-submit") && strcmp(msg, "decor draw") &&
            strcmp(msg, "obj-in-submit") && strcmp(msg, "obj draw") &&
            strcmp(msg, "obj-atlas-upload") && strcmp(msg, "obj-atlas-begin") &&
            strcmp(msg, "obj-atlas-pump") && strcmp(msg, "obj-atlas-cpu") &&
            strcmp(msg, "obj-atlas-reject") && strcmp(msg, "obj-arr-init") &&
            strcmp(msg, "obj-miss") && strcmp(msg, "obj-soft-skip") &&
            strcmp(msg, "obj-gpu-env") &&
            strcmp(msg, "obj catalog ready") &&
            strcmp(msg, "obj-kind-overflow") &&
            strcmp(msg, "obj-layer-cache") && strcmp(msg, "obj-soft-heavy") &&
            strcmp(msg, "skip soft obj gpu") &&
            strcmp(msg, "soft-pf-stats") && strcmp(msg, "soft-pf-stamp") &&
            strcmp(msg, "obj-soft-draw") && strcmp(msg, "soft-ov-ready") &&
            strcmp(msg, "soft-ov-draw") && strcmp(msg, "soft-ov-key") &&
            strcmp(msg, "obj-phase") && strcmp(msg, "obj-tpl-ready") &&
            strcmp(msg, "fps-budget") &&
            strcmp(msg, "cam-sync") &&
            strcmp(msg, "unit-civ") &&
            strcmp(msg, "soft-move") &&
            strcmp(msg, "soft-bird") &&
            strcmp(msg, "pass-order") &&
            strcmp(msg, "obj-arr-evict") &&
            strcmp(msg, "audio-prefetch-done"))
            return;
    }
    df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
    if (df) {
        fprintf(df,
                "{\"sessionId\":\"764ba7\",\"runId\":\"post-fake\",\"hypothesisId\":\"%s\","
                "\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lu}\n",
                hid ? hid : "?", loc ? loc : "?", msg ? msg : "?",
                data_json && data_json[0] ? data_json : "{}",
                /* Do not call GetTickCount — it is hooked and re-enters spin logging. */
                (unsigned long)(hitch_qpc_now() / 10000));
        fclose(df);
    }
    /* #endregion */
}
int mem_eq(const void *a, const void *b, SIZE_T n)
{
    const BYTE *pa = (const BYTE *)a, *pb = (const BYTE *)b;
    SIZE_T i;
    for (i = 0; i < n; ++i)
        if (pa[i] != pb[i])
            return 0;
    return 1;
}

/* Scan executable sections for a short unique prolog; returns VA or NULL. */
void *scan_sig(HMODULE mod, const BYTE *sig, SIZE_T siglen)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    WORD i, nsec;
    BYTE *start, *end, *p;

    if (!mod || !sig || siglen < 4)
        return NULL;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    sec = IMAGE_FIRST_SECTION(nt);
    nsec = nt->FileHeader.NumberOfSections;
    for (i = 0; i < nsec; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        start = base + sec[i].VirtualAddress;
        end = start + sec[i].Misc.VirtualSize;
        if (end <= start || (SIZE_T)(end - start) < siglen)
            continue;
        for (p = start; p + siglen <= end; ++p) {
            if (mem_eq(p, sig, siglen))
                return p;
        }
    }
    return NULL;
}

/* Cache once: RenderTerrain @ 0x475FE0 is inline-hooked later, so re-probing
 * the same VA after terrain install falsely reports "not tpw" (breaks cam_pan). */
int looks_like_tpw(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = mem_eq((void *)(ULONG_PTR)0x00475FE0, "\x81\xEC\x8C\x00\x00\x00", 6) ? 1 : 0;
    return cached;
}

int looks_like_celtic_kings(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = mem_eq((void *)(ULONG_PTR)0x00473750, "\x81\xEC\x8C\x00\x00\x00", 6) ? 1 : 0;
    return cached;
}
