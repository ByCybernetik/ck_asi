#include "ktx_terrain.h"
#include "hooks_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KTX_MAX = 128, KTX_NAME = 96 };

static char s_root[MAX_PATH];
static int s_ready;
static int s_count;
static int s_preview;
static int s_enabled;

static char s_names[KTX_MAX][KTX_NAME]; /* relative lowercase, e.g. spring/grass512.ktx2 */

static uint8_t *s_prev_bgra;
static int s_prev_w, s_prev_h;
static int s_prev_ok;

static void lower_copy(char *dst, size_t n, const char *src)
{
    size_t i;
    if (!dst || n == 0)
        return;
    for (i = 0; i + 1 < n && src[i]; ++i) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c == '\\')
            c = '/';
        dst[i] = c;
    }
    dst[i] = '\0';
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void add_name(const char *rel)
{
    size_t i;
    if (s_count >= KTX_MAX || !rel || !rel[0])
        return;
    for (i = 0; i < (size_t)s_count; ++i) {
        if (strcmp(s_names[i], rel) == 0)
            return;
    }
    lstrcpynA(s_names[s_count], rel, KTX_NAME);
    s_count++;
}

static void scan_dir(const char *abs_dir, const char *rel_prefix)
{
    char pattern[MAX_PATH];
    char child[MAX_PATH];
    char rel[KTX_NAME];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t n;

    n = (size_t)lstrlenA(abs_dir);
    if (n + 3 >= sizeof(pattern))
        return;
    lstrcpyA(pattern, abs_dir);
    lstrcatA(pattern, "\\*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        if (lstrlenA(abs_dir) + 1 + lstrlenA(fd.cFileName) >= (int)sizeof(child))
            continue;
        lstrcpyA(child, abs_dir);
        lstrcatA(child, "\\");
        lstrcatA(child, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub_rel[KTX_NAME];
            if (rel_prefix[0])
                snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_prefix, fd.cFileName);
            else
                lstrcpynA(sub_rel, fd.cFileName, KTX_NAME);
            lower_copy(sub_rel, sizeof(sub_rel), sub_rel);
            scan_dir(child, sub_rel);
        } else {
            size_t len = (size_t)lstrlenA(fd.cFileName);
            if (len < 6 || _stricmp(fd.cFileName + len - 5, ".ktx2") != 0)
                continue;
            if (rel_prefix[0])
                snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, fd.cFileName);
            else
                lstrcpynA(rel, fd.cFileName, KTX_NAME);
            lower_copy(rel, sizeof(rel), rel);
            add_name(rel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int pick_root(char *out, size_t out_n)
{
    char env[MAX_PATH];
    char cand[MAX_PATH];
    char norm[MAX_PATH];
    DWORD n;
    char *p;

    n = GetEnvironmentVariableA("CK_KTX_DIR", env, (DWORD)sizeof(env));
    if (n > 0 && n < sizeof(env)) {
        /* Wine often gets forward slashes from the host shell. */
        lstrcpynA(norm, env, (int)sizeof(norm));
        for (p = norm; *p; ++p)
            if (*p == '/')
                *p = '\\';
        if (dir_exists(norm)) {
            lstrcpynA(out, norm, (int)out_n);
            return 1;
        }
        log_msg("ktx_terrain: CK_KTX_DIR set but missing: %s", norm);
    } else {
        log_msg("ktx_terrain: CK_KTX_DIR unset (GetEnvironmentVariable n=%lu)", (unsigned long)n);
    }

    if (g_game_root[0]) {
        snprintf(cand, sizeof(cand), "%sktx\\terrain", g_game_root);
        if (dir_exists(cand)) {
            lstrcpynA(out, cand, (int)out_n);
            return 1;
        }
        /* Sibling repo layout: <Imperivm 2>/../ck_asi/ktx/terrain */
        snprintf(cand, sizeof(cand), "%s..\\ck_asi\\ktx\\terrain", g_game_root);
        if (dir_exists(cand)) {
            lstrcpynA(out, cand, (int)out_n);
            return 1;
        }
    }
    return 0;
}

/* BC1 block → 4×4 BGRA */
static void bc1_block(const uint8_t *blk, uint8_t *out4x4, int stride_px)
{
    uint16_t c0 = (uint16_t)(blk[0] | (blk[1] << 8));
    uint16_t c1 = (uint16_t)(blk[2] | (blk[3] << 8));
    uint32_t bits = (uint32_t)blk[4] | ((uint32_t)blk[5] << 8) | ((uint32_t)blk[6] << 16) |
                    ((uint32_t)blk[7] << 24);
    uint8_t r[4], g[4], b[4], a[4];
    int i, x, y;

    r[0] = (uint8_t)(((c0 >> 11) & 31) * 255 / 31);
    g[0] = (uint8_t)(((c0 >> 5) & 63) * 255 / 63);
    b[0] = (uint8_t)((c0 & 31) * 255 / 31);
    a[0] = 255;
    r[1] = (uint8_t)(((c1 >> 11) & 31) * 255 / 31);
    g[1] = (uint8_t)(((c1 >> 5) & 63) * 255 / 63);
    b[1] = (uint8_t)((c1 & 31) * 255 / 31);
    a[1] = 255;
    if (c0 > c1) {
        r[2] = (uint8_t)((2 * r[0] + r[1]) / 3);
        g[2] = (uint8_t)((2 * g[0] + g[1]) / 3);
        b[2] = (uint8_t)((2 * b[0] + b[1]) / 3);
        a[2] = 255;
        r[3] = (uint8_t)((r[0] + 2 * r[1]) / 3);
        g[3] = (uint8_t)((g[0] + 2 * g[1]) / 3);
        b[3] = (uint8_t)((b[0] + 2 * b[1]) / 3);
        a[3] = 255;
    } else {
        r[2] = (uint8_t)((r[0] + r[1]) / 2);
        g[2] = (uint8_t)((g[0] + g[1]) / 2);
        b[2] = (uint8_t)((b[0] + b[1]) / 2);
        a[2] = 255;
        r[3] = 0;
        g[3] = 0;
        b[3] = 0;
        a[3] = 0;
    }
    for (i = 0; i < 16; ++i) {
        int idx = (int)((bits >> (2 * i)) & 3u);
        y = i / 4;
        x = i % 4;
        {
            uint8_t *p = out4x4 + ((size_t)y * (size_t)stride_px + (size_t)x) * 4u;
            p[0] = b[idx];
            p[1] = g[idx];
            p[2] = r[idx];
            p[3] = a[idx];
        }
    }
}

uint8_t *ktx_terrain_decode_bc1_bgra(const uint8_t *blocks, uint32_t w, uint32_t h,
                                     uint32_t block_bytes)
{
    uint32_t bw, bh, need;
    uint8_t *rgba;
    uint32_t by, bx;
    const uint8_t *src;

    if (!blocks || w < 4 || h < 4 || (w & 3) || (h & 3))
        return NULL;
    bw = w / 4;
    bh = h / 4;
    need = bw * bh * 8u;
    if (block_bytes < need)
        return NULL;
    rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba)
        return NULL;
    src = blocks;
    for (by = 0; by < bh; ++by) {
        for (bx = 0; bx < bw; ++bx) {
            uint8_t *dst = rgba + ((size_t)by * 4u * (size_t)w + (size_t)bx * 4u) * 4u;
            bc1_block(src, dst, (int)w);
            src += 8;
        }
    }
    return rgba;
}

int ktx_terrain_load_info(const char *path, Ktx2Info *info, uint8_t **level0_out)
{
    FILE *f;
    uint8_t hdr[128];
    size_t nread;
    uint32_t vk_format, type_size, w, h, depth, layers, faces, levels, sc;
    uint32_t dfd_off, dfd_len, kvd_off, kvd_len;
    uint64_t sgd_off, sgd_len, l_off, l_len, l_unc;

    if (level0_out)
        *level0_out = NULL;
    if (!path || !info)
        return 0;
    memset(info, 0, sizeof(*info));
    f = fopen(path, "rb");
    if (!f)
        return 0;
    nread = fread(hdr, 1, sizeof(hdr), f);
    if (nread < 104 || memcmp(hdr, "\xABKTX 20\xBB\r\n\x1A\n", 12) != 0) {
        fclose(f);
        return 0;
    }
    memcpy(&vk_format, hdr + 12, 4);
    memcpy(&type_size, hdr + 16, 4);
    memcpy(&w, hdr + 20, 4);
    memcpy(&h, hdr + 24, 4);
    memcpy(&depth, hdr + 28, 4);
    memcpy(&layers, hdr + 32, 4);
    memcpy(&faces, hdr + 36, 4);
    memcpy(&levels, hdr + 40, 4);
    memcpy(&sc, hdr + 44, 4);
    (void)type_size;
    (void)depth;
    (void)layers;
    (void)faces;
    /* libktx 4.3 layout observed: u32 dfd_off/len, u32 kvd_off/len, u64 sgd_off/len, then levels */
    memcpy(&dfd_off, hdr + 48, 4);
    memcpy(&dfd_len, hdr + 52, 4);
    memcpy(&kvd_off, hdr + 56, 4);
    memcpy(&kvd_len, hdr + 60, 4);
    memcpy(&sgd_off, hdr + 64, 8);
    memcpy(&sgd_len, hdr + 72, 8);
    memcpy(&l_off, hdr + 80, 8);
    memcpy(&l_len, hdr + 88, 8);
    memcpy(&l_unc, hdr + 96, 8);
    (void)dfd_off;
    (void)dfd_len;
    (void)kvd_off;
    (void)kvd_len;
    (void)sgd_off;
    (void)sgd_len;
    (void)l_unc;
    (void)levels;
    (void)sc;
    if (!w || !h || l_off > 0xffffffffu || l_len > 0xffffffffu || l_len == 0) {
        fclose(f);
        return 0;
    }
    info->width = w;
    info->height = h;
    info->vk_format = vk_format;
    info->level0_off = (uint32_t)l_off;
    info->level0_len = (uint32_t)l_len;
    if (level0_out) {
        uint8_t *blob = (uint8_t *)malloc(info->level0_len);
        if (!blob) {
            fclose(f);
            return 0;
        }
        if (fseek(f, (long)info->level0_off, SEEK_SET) != 0 ||
            fread(blob, 1, info->level0_len, f) != info->level0_len) {
            free(blob);
            fclose(f);
            return 0;
        }
        *level0_out = blob;
    }
    fclose(f);
    return 1;
}

static int load_preview(void)
{
    char path[MAX_PATH];
    Ktx2Info info;
    uint8_t *blocks = NULL;

    if (!s_root[0])
        return 0;
    snprintf(path, sizeof(path), "%s\\spring\\grass512.ktx2", s_root);
    if (!file_exists(path))
        snprintf(path, sizeof(path), "%s/spring/grass512.ktx2", s_root);
    if (!file_exists(path))
        snprintf(path, sizeof(path), "%s\\dwater.ktx2", s_root);
    if (!file_exists(path))
        snprintf(path, sizeof(path), "%s/dwater.ktx2", s_root);
    if (!file_exists(path))
        return 0;
    if (!ktx_terrain_load_info(path, &info, &blocks))
        return 0;
    /* BC1_RGB_SRGB=132, BC1_RGB_UNORM=131, BC1_RGBA_SRGB=134, BC1_RGBA_UNORM=133 */
    if (info.vk_format < 131 || info.vk_format > 134) {
        free(blocks);
        return 0;
    }
    s_prev_bgra = ktx_terrain_decode_bc1_bgra(blocks, info.width, info.height, info.level0_len);
    free(blocks);
    if (!s_prev_bgra)
        return 0;
    s_prev_w = (int)info.width;
    s_prev_h = (int)info.height;
    s_prev_ok = 1;
    return 1;
}

void ktx_terrain_init(void)
{
    s_ready = 0;
    s_count = 0;
    s_root[0] = '\0';
    s_enabled = env_on("CK_KTX_TERRAIN", 1);
    s_preview = env_on("CK_KTX_PREVIEW", 0);
    if (!s_enabled) {
        log_msg("ktx_terrain: disabled (CK_KTX_TERRAIN=0)");
        return;
    }
    if (!pick_root(s_root, sizeof(s_root))) {
        log_msg("ktx_terrain: no ktx/terrain under game_root=%s (symlink, CK_KTX_DIR, or ../ck_asi/ktx/terrain)",
                g_game_root);
        return;
    }
    scan_dir(s_root, "");
    s_ready = s_count > 0;
    log_msg("ktx_terrain: root=%s count=%d preview=%d", s_root, s_count, s_preview);
    if (s_preview)
        load_preview();
}

void ktx_terrain_shutdown(void)
{
    free(s_prev_bgra);
    s_prev_bgra = NULL;
    s_prev_ok = 0;
    s_ready = 0;
    s_count = 0;
}

int ktx_terrain_ready(void)
{
    return s_ready;
}

int ktx_terrain_count(void)
{
    return s_count;
}

const char *ktx_terrain_root(void)
{
    return s_root;
}

int ktx_terrain_resolve_vq(const char *vq_key, char *out, size_t out_n)
{
    char rel[KTX_NAME];
    char tmp[KTX_NAME];
    char *p;
    size_t n;

    if (!s_ready || !vq_key || !out || out_n < 8)
        return 0;
    lower_copy(tmp, sizeof(tmp), vq_key);
    /* strip common retail prefixes */
    p = tmp;
    if (strncmp(p, "assets/", 7) == 0)
        p += 7;
    if (strncmp(p, "terrain/", 8) == 0)
        p += 8;
    n = strlen(p);
    if (n > 3 && strcmp(p + n - 3, ".vq") == 0)
        p[n - 3] = '\0';
    snprintf(rel, sizeof(rel), "%s.ktx2", p);
    if ((size_t)lstrlenA(s_root) + 1 + strlen(rel) + 1 >= out_n)
        return 0;
    lstrcpyA(out, s_root);
    lstrcatA(out, "\\");
    {
        char *q;
        lstrcatA(out, rel);
        for (q = out; *q; ++q)
            if (*q == '/')
                *q = '\\';
    }
    if (file_exists(out))
        return 1;
    /* try forward slashes */
    snprintf(out, out_n, "%s/%s", s_root, rel);
    return file_exists(out);
}

void ktx_terrain_preview_soft(uint8_t *bgra, int w, int h)
{
    int dw = 96, dh = 96;
    int x0, y0, y, x;
    if (!s_preview || !s_prev_ok || !s_prev_bgra || !bgra || w < dw + 16 || h < dh + 16)
        return;
    x0 = w - dw - 8;
    y0 = 8;
    for (y = 0; y < dh; ++y) {
        int sy = y * s_prev_h / dh;
        for (x = 0; x < dw; ++x) {
            int sx = x * s_prev_w / dw;
            uint8_t *d = bgra + ((size_t)(y0 + y) * (size_t)w + (size_t)(x0 + x)) * 4u;
            uint8_t *s = s_prev_bgra + ((size_t)sy * (size_t)s_prev_w + (size_t)sx) * 4u;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}
