/* UI / tip text via FreeType (smooth light-hinted grayscale AA). */
#include "tip_font.h"
#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "vk_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <ft2build.h>
#include FT_FREETYPE_H

enum {
    TIP_PT_DEFAULT = 12,
    TIP_DPI = 96,
    FT_PX_FALLBACK = 13
};

static FT_Library s_ft;
static FT_Face s_face_bold;
static FT_Face s_face_reg;
static FT_Face s_face; /* active face for size/draw */
static int s_ready;
static int s_fail;
static int s_cur_px;
static int s_tip_pt = TIP_PT_DEFAULT;
static int s_last_ink_top;

/* Present BGRA overlay queue (declared early for shutdown). */
enum { FT_OV_MAX = 512, FT_OV_NCH = 160 };
typedef struct {
    WCHAR t[FT_OV_NCH];
    int nch;
    int x, y, max_w;
    int r, g, b, shadow, pt, bold;
} FtOvItem;
static FtOvItem s_ov[FT_OV_MAX];
static int s_ov_n;
static CRITICAL_SECTION s_ov_cs;
static int s_ov_cs_ok;
static int s_ov_want = -1;

static void use_face(FT_Face f)
{
    if (!f)
        return;
    if (s_face != f) {
        s_face = f;
        s_cur_px = 0; /* size cache is per-face */
    }
}

static int load_face_into(const char *path, FT_Face *out)
{
    FT_Error e;
    FT_Face face = NULL;
    if (!path || !path[0] || !out)
        return 0;
    e = FT_New_Face(s_ft, path, 0, &face);
    if (e || !face)
        return 0;
    *out = face;
    log_msg("tip_font: FreeType face=%s", path);
    return 1;
}

static int try_load_named(const char *const *names, FT_Face *out)
{
    char win[MAX_PATH];
    char path[MAX_PATH + 64];
    DWORD n;
    int i;
    HMODULE mod;
    char modpath[MAX_PATH];
    char *slash;
    static const char *const linux_dirs[] = {
        "Z:\\usr\\share\\fonts\\liberation",
        "Z:\\usr\\share\\fonts\\TTF",
        "Z:\\usr\\share\\fonts\\noto",
        "Z:\\usr\\share\\fonts\\carlito",
        "/usr/share/fonts/liberation",
        "/usr/share/fonts/TTF",
        "/usr/share/fonts/noto",
        NULL};

    if (!names || !out)
        return 0;

    /* Prefer fonts next to CK.asi (scripts/) first — Wine often has no tahoma.ttf). */
    mod = GetModuleHandleA("CK.asi");
    if (mod && GetModuleFileNameA(mod, modpath, (DWORD)sizeof(modpath))) {
        slash = strrchr(modpath, '\\');
        if (!slash)
            slash = strrchr(modpath, '/');
        if (slash) {
            *slash = 0;
            for (i = 0; names[i]; ++i) {
                snprintf(path, sizeof(path), "%s\\%s", modpath, names[i]);
                if (load_face_into(path, out))
                    return 1;
                snprintf(path, sizeof(path), "%s/%s", modpath, names[i]);
                if (load_face_into(path, out))
                    return 1;
            }
        }
    }

    n = GetWindowsDirectoryA(win, (DWORD)sizeof(win));
    if (n > 0 && n < sizeof(win)) {
        for (i = 0; names[i]; ++i) {
            snprintf(path, sizeof(path), "%s\\fonts\\%s", win, names[i]);
            if (load_face_into(path, out))
                return 1;
        }
    }

    for (i = 0; linux_dirs[i]; ++i) {
        int j;
        for (j = 0; names[j]; ++j) {
            snprintf(path, sizeof(path), "%s\\%s", linux_dirs[i], names[j]);
            if (load_face_into(path, out))
                return 1;
            snprintf(path, sizeof(path), "%s/%s", linux_dirs[i], names[j]);
            if (load_face_into(path, out))
                return 1;
        }
    }
    return 0;
}

static int try_load_faces(void)
{
    char env[MAX_PATH];
    DWORD n;
    static const char *const bold_names[] = {"tahomabd.ttf", "arialbd.ttf", NULL};
    static const char *const reg_names[] = {
        "tahoma.ttf", "LiberationSans-Regular.ttf", "arial.ttf", "DejaVuSans.ttf",
        "NotoSans-Regular.ttf", "Carlito-Regular.ttf", NULL};

    n = GetEnvironmentVariableA("CK_TIP_FONT", env, (DWORD)sizeof(env));
    if (n > 0 && n < sizeof(env))
        load_face_into(env, &s_face_bold);

    n = GetEnvironmentVariableA("CK_FT_CTEXT_FONT", env, (DWORD)sizeof(env));
    if (n > 0 && n < sizeof(env))
        load_face_into(env, &s_face_reg);

    if (!s_face_bold && !try_load_named(bold_names, &s_face_bold))
        return 0;
    if (!s_face_reg)
        try_load_named(reg_names, &s_face_reg);
    if (!s_face_reg || s_face_reg == s_face_bold) {
        log_msg("tip_font: WARN no regular TTF (need tahoma.ttf beside CK.asi) — menu stays native/Bold");
        s_face_reg = NULL;
    } else {
        log_msg("tip_font: bold='%s'/'%s' reg='%s'/'%s'",
                s_face_bold->family_name ? s_face_bold->family_name : "?",
                s_face_bold->style_name ? s_face_bold->style_name : "?",
                s_face_reg->family_name ? s_face_reg->family_name : "?",
                s_face_reg->style_name ? s_face_reg->style_name : "?");
    }
    use_face(s_face_bold);
    return 1;
}

static int ensure_px(int px)
{
    FT_Error e;
    if (px < 8)
        px = 8;
    if (px > 48)
        px = 48;
    if (s_cur_px == px)
        return 1;
    e = FT_Set_Pixel_Sizes(s_face, 0, (FT_UInt)px);
    if (e)
        return 0;
    s_cur_px = px;
    return 1;
}

static int ensure_pt(int pt)
{
    FT_Error e;
    if (pt < 8)
        pt = 8;
    if (pt > 24)
        pt = 24;
    if (s_cur_px == -pt)
        return 1;
    e = FT_Set_Char_Size(s_face, 0, pt * 64, TIP_DPI, TIP_DPI);
    if (e)
        return 0;
    s_cur_px = -pt; /* mark pt mode */
    return 1;
}

static int ensure_tip_pt(void)
{
    return ensure_pt(s_tip_pt);
}

int tip_font_init(void)
{
    FT_Error e;
    char env[32];
    DWORD n;
    if (s_ready)
        return 1;
    if (s_fail)
        return 0;
    e = FT_Init_FreeType(&s_ft);
    if (e) {
        log_msg("tip_font: FT_Init_FreeType failed %d", (int)e);
        s_fail = 1;
        return 0;
    }
    if (!try_load_faces()) {
        log_msg("tip_font: no TTF for FreeType (CK_TIP_FONT / fonts / scripts)");
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
        s_fail = 1;
        return 0;
    }
    n = GetEnvironmentVariableA("CK_FT_TIP_PT", env, (DWORD)sizeof(env));
    if (n > 0 && n < sizeof(env)) {
        int v = atoi(env);
        if (v >= 8 && v <= 24)
            s_tip_pt = v;
    }
    s_ready = 1;
    return 1;
}

void tip_font_shutdown(void)
{
    if (s_ov_cs_ok) {
        DeleteCriticalSection(&s_ov_cs);
        s_ov_cs_ok = 0;
        s_ov_n = 0;
    }
    if (s_face_reg && s_face_reg != s_face_bold) {
        FT_Done_Face(s_face_reg);
        s_face_reg = NULL;
    }
    if (s_face_bold) {
        FT_Done_Face(s_face_bold);
        s_face_bold = NULL;
    }
    s_face = NULL;
    if (s_ft) {
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
    }
    s_ready = 0;
    s_fail = 0;
    s_cur_px = 0;
}

static int utf8_next(const char **ps)
{
    const unsigned char *p = (const unsigned char *)*ps;
    unsigned c = *p++;
    if (!c)
        return 0;
    if (c < 0x80) {
        *ps = (const char *)p;
        return (int)c;
    }
    if ((c & 0xE0) == 0xC0 && p[0]) {
        unsigned c2 = *p++;
        *ps = (const char *)p;
        return (int)(((c & 0x1F) << 6) | (c2 & 0x3F));
    }
    if ((c & 0xF0) == 0xE0 && p[0] && p[1]) {
        unsigned c2 = *p++, c3 = *p++;
        *ps = (const char *)p;
        return (int)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
    }
    if ((c & 0xF8) == 0xF0 && p[0] && p[1] && p[2]) {
        unsigned c2 = *p++, c3 = *p++, c4 = *p++;
        *ps = (const char *)p;
        return (int)(((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) |
                     (c4 & 0x3F));
    }
    *ps = (const char *)p;
    return (int)'?';
}

static int looks_like_utf8(const char *s, int n)
{
    int i = 0, has_mb = 0;
    if (n < 0) {
        n = 0;
        while (s[n])
            n++;
    }
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        has_mb = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) {
            i += 2;
            continue;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 &&
            (s[i + 2] & 0xC0) == 0x80) {
            i += 3;
            continue;
        }
        return 0;
    }
    return has_mb || n > 0;
}

/* Decode game string → UTF-16 in wbuf; returns wchar count. */
static int str_to_wide(const char *str, int maxlen, WCHAR *wbuf, int wmax)
{
    int n, wn;
    if (!str || !wbuf || wmax < 2)
        return 0;
    n = 0;
    while (str[n] && (maxlen < 0 || n < maxlen))
        n++;
    if (n <= 0)
        return 0;
    if (looks_like_utf8(str, n)) {
        wn = MultiByteToWideChar(CP_UTF8, 0, str, n, wbuf, wmax - 1);
        if (wn > 0) {
            wbuf[wn] = 0;
            return wn;
        }
    }
    wn = MultiByteToWideChar(1251, 0, str, n, wbuf, wmax - 1);
    if (wn <= 0)
        wn = MultiByteToWideChar(CP_ACP, 0, str, n, wbuf, wmax - 1);
    if (wn <= 0)
        return 0;
    wbuf[wn] = 0;
    return wn;
}

static void blend_rgb555(uint16_t *p, unsigned a, int r, int g, int b)
{
    unsigned o = *p;
    unsigned or_ = (o >> 10) & 31, og = (o >> 5) & 31, ob = o & 31;
    unsigned nr = (unsigned)(r >> 3), ng = (unsigned)(g >> 3), nb = (unsigned)(b >> 3);
    unsigned ia = 255 - a;
    or_ = (nr * a + or_ * ia) / 255;
    og = (ng * a + og * ia) / 255;
    ob = (nb * a + ob * ia) / 255;
    *p = (uint16_t)((or_ << 10) | (og << 5) | ob);
}

static void blend_rgb565(uint16_t *p, unsigned a, int r, int g, int b)
{
    unsigned o = *p;
    unsigned or_ = (o >> 11) & 31, og = (o >> 5) & 63, ob = o & 31;
    unsigned nr = (unsigned)(r >> 3), ng = (unsigned)(g >> 2), nb = (unsigned)(b >> 3);
    unsigned ia = 255 - a;
    or_ = (nr * a + or_ * ia) / 255;
    og = (ng * a + og * ia) / 255;
    ob = (nb * a + ob * ia) / 255;
    *p = (uint16_t)((or_ << 11) | (og << 5) | ob);
}

static void blit_gray_bgra(uint8_t *dst, int dst_w, int dst_h, int dx, int dy, const FT_Bitmap *bm,
                           int r, int g, int b)
{
    int y, x;
    int w = (int)bm->width;
    int h = (int)bm->rows;
    int pitch = bm->pitch;

    for (y = 0; y < h; ++y) {
        int sy = dy + y;
        const unsigned char *row;
        if (sy < 0 || sy >= dst_h)
            continue;
        row = bm->buffer + (pitch >= 0 ? y * pitch : (h - 1 - y) * (-pitch));
        for (x = 0; x < w; ++x) {
            int sx = dx + x;
            unsigned a, ia;
            uint8_t *p;
            if (sx < 0 || sx >= dst_w)
                continue;
            a = row[x];
            if (!a)
                continue;
            ia = 255 - a;
            p = dst + ((size_t)sy * (size_t)dst_w + (size_t)sx) * 4u;
            p[0] = (uint8_t)((b * a + p[0] * ia) / 255);
            p[1] = (uint8_t)((g * a + p[1] * ia) / 255);
            p[2] = (uint8_t)((r * a + p[2] * ia) / 255);
            p[3] = 255;
        }
    }
}

static void blit_gray_16(uint8_t *base, int pitch, int dst_w, int dst_h, int dx, int dy,
                         const FT_Bitmap *bm, int r, int g, int b, int rgb565)
{
    int y, x;
    int w = (int)bm->width;
    int h = (int)bm->rows;
    int bp = bm->pitch;

    for (y = 0; y < h; ++y) {
        int sy = dy + y;
        const unsigned char *row;
        uint8_t *line;
        if (sy < 0 || sy >= dst_h)
            continue;
        row = bm->buffer + (bp >= 0 ? y * bp : (h - 1 - y) * (-bp));
        line = base + (size_t)sy * (size_t)pitch;
        for (x = 0; x < w; ++x) {
            int sx = dx + x;
            unsigned a;
            uint16_t *p;
            if (sx < 0 || sx >= dst_w)
                continue;
            a = row[x];
            if (!a)
                continue;
            p = (uint16_t *)(line + (size_t)sx * 2u);
            if (rgb565)
                blend_rgb565(p, a, r, g, b);
            else
                blend_rgb555(p, a, r, g, b);
        }
    }
}

static int draw_wide(uint8_t *dst, int dst_w, int dst_h, int pitch, int bpp, int dx, int dy,
                     const WCHAR *ws, int wn, int r, int g, int b, int with_shadow, int *out_w,
                     int *out_h)
{
    int pen_x, pen_y, max_x, min_y, max_y, i, n_glyphs = 0;
    /* H-AA: TARGET_NORMAL snaps stems → less glow on dark UI / after RGB555.
     * CK_FT_HINT=normal|light|none|auto (default normal). */
    FT_Int32 load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL;
    int rgb565 = 0;
    int measure_only = (dst == NULL);

    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!ws || wn < 1)
        return 0;
    if (!measure_only && (!dst || dst_w < 1 || dst_h < 1))
        return 0;

    {
        static int s_hint = -1;
        if (s_hint < 0) {
            char env[32];
            DWORD nenv = GetEnvironmentVariableA("CK_FT_HINT", env, (DWORD)sizeof(env));
            s_hint = 0; /* normal — default crisp */
            if (nenv > 0 && nenv < sizeof(env)) {
                if (!_strnicmp(env, "light", 5))
                    s_hint = 1;
                else if (!_strnicmp(env, "none", 4))
                    s_hint = 2;
                else if (!_strnicmp(env, "auto", 4))
                    s_hint = 3;
                else if (!_strnicmp(env, "normal", 6))
                    s_hint = 0;
            }
        }
        if (s_hint == 0)
            load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL;
        else if (s_hint == 2)
            load_flags = FT_LOAD_RENDER | FT_LOAD_NO_HINTING;
        else if (s_hint == 3)
            load_flags = FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;
        else
            load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT;
    }

    if (!measure_only) {
        if (bpp == 16) {
            rgb565 = 0;
        } else if (bpp != 32 && bpp != 0) {
            if (bpp != 16)
                return 0;
        }
    }

    /* dy = top of line box (retail APF / CTextImage). Baseline = top + ascender. */
    pen_x = dx + (measure_only ? 0 : 1);
    pen_y = dy + ((s_face->size->metrics.ascender + 63) >> 6) + (measure_only ? 0 : 1);
    max_x = pen_x;
    min_y = pen_y;
    max_y = pen_y;

    for (i = 0; i < wn; ++i) {
        FT_UInt gindex;
        FT_Error e;
        FT_GlyphSlot slot;
        int bx, by;
        int cp = (int)ws[i];
        if (cp == 0)
            break;
        gindex = FT_Get_Char_Index(s_face, (FT_ULong)cp);
        e = FT_Load_Glyph(s_face, gindex, load_flags);
        if (e)
            continue;
        slot = s_face->glyph;
        bx = pen_x + slot->bitmap_left;
        by = pen_y - slot->bitmap_top;
        /* #region agent log */
        if (!measure_only && n_glyphs == 0) {
            static volatile LONG s_aa_n;
            LONG aan = InterlockedIncrement(&s_aa_n);
            if (aan <= 12) {
                FILE *df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log",
                                 "a");
                if (df) {
                    const FT_Bitmap *bm = &slot->bitmap;
                    int mid = 0, full = 0, zero = 0, px, py, tot;
                    int bp = bm->pitch;
                    tot = (int)bm->width * (int)bm->rows;
                    for (py = 0; py < (int)bm->rows; ++py) {
                        const unsigned char *row =
                            bm->buffer + (bp >= 0 ? py * bp : ((int)bm->rows - 1 - py) * (-bp));
                        for (px = 0; px < (int)bm->width; ++px) {
                            unsigned a = row[px];
                            if (!a)
                                zero++;
                            else if (a >= 250)
                                full++;
                            else
                                mid++;
                        }
                    }
                    fprintf(df,
                            "{\"sessionId\":\"764ba7\",\"runId\":\"aa-normal\",\"hypothesisId\":\"H-AA\","
                            "\"location\":\"tip_font.c:draw_wide\",\"message\":\"glyph-aa\","
                            "\"data\":{\"n\":%ld,\"cp\":%d,\"pm\":%d,\"ng\":%d,\"wh\":[%u,%u],"
                            "\"zero\":%d,\"mid\":%d,\"full\":%d,\"tot\":%d,\"bpp\":%d,\"flags\":%u},"
                            "\"timestamp\":%lu}\n",
                            (long)aan, cp, (int)bm->pixel_mode, (int)bm->num_grays, bm->width,
                            bm->rows, zero, mid, full, tot, bpp, (unsigned)load_flags,
                            (unsigned long)GetTickCount());
                    fclose(df);
                }
            }
        }
        /* #endregion */
        if (!measure_only) {
            if (bpp == 32 || pitch == dst_w * 4) {
                if (with_shadow)
                    blit_gray_bgra(dst, dst_w, dst_h, bx + 1, by + 1, &slot->bitmap, 0, 0, 0);
                blit_gray_bgra(dst, dst_w, dst_h, bx, by, &slot->bitmap, r, g, b);
            } else {
                if (with_shadow)
                    blit_gray_16(dst, pitch, dst_w, dst_h, bx + 1, by + 1, &slot->bitmap, 0, 0, 0,
                                 rgb565);
                blit_gray_16(dst, pitch, dst_w, dst_h, bx, by, &slot->bitmap, r, g, b, rgb565);
            }
        }
        pen_x += (int)(slot->advance.x >> 6);
        if (pen_x > max_x)
            max_x = pen_x;
        if (by < min_y)
            min_y = by;
        if (by + (int)slot->bitmap.rows > max_y)
            max_y = by + (int)slot->bitmap.rows;
        n_glyphs++;
    }

    if (out_w)
        *out_w = max_x - dx + (measure_only ? 0 : 2);
    if (out_h)
        *out_h = (max_y - min_y) + (measure_only ? 0 : 2);
    /* Ink row relative to dy (line-box top). Draw sits this many px below dy. */
    s_last_ink_top = min_y - dy;
    if (s_last_ink_top < 0)
        s_last_ink_top = 0;
    return n_glyphs > 0;
}

int tip_font_measure_pt(const char *str, int maxlen, int pt, int *out_w, int *out_h)
{
    WCHAR wbuf[512];
    int wn;
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!str || IsBadReadPtr(str, 1) || !str[0])
        return 0;
    if (!tip_font_init() || !s_face_bold)
        return 0;
    /* Menu/CTextImage: regular weight only (never Bold fallback). */
    if (!s_face_reg)
        return 0;
    use_face(s_face_reg);
    if (pt <= 0)
        pt = s_tip_pt;
    if (!ensure_pt(pt))
        return 0;
    wn = str_to_wide(str, maxlen, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn <= 0)
        return 0;
    return draw_wide(NULL, 0, 0, 0, 0, 0, 0, wbuf, wn, 0, 0, 0, 0, out_w, out_h);
}

int tip_font_measure(const char *str, int maxlen, int *out_w, int *out_h)
{
    return tip_font_measure_pt(str, maxlen, 0, out_w, out_h);
}

int tip_font_measure_wstr_pt(const WCHAR *ws, int nch, int pt, int *out_w, int *out_h)
{
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!ws || nch < 1 || IsBadReadPtr(ws, nch * 2))
        return 0;
    if (!tip_font_init() || !s_face_bold)
        return 0;
    use_face(s_face_bold);
    if (pt <= 0)
        pt = s_tip_pt;
    if (!ensure_pt(pt))
        return 0;
    if (nch > 800)
        nch = 800;
    return draw_wide(NULL, 0, 0, 0, 0, 0, 0, ws, nch, 0, 0, 0, 0, out_w, out_h);
}

const char *tip_font_ctext_style(void)
{
    if (!s_ready || !s_face_reg)
        return "";
    return s_face_reg->style_name ? s_face_reg->style_name : "Regular";
}

const char *tip_font_ui_style(void)
{
    if (!s_ready || !s_face_bold)
        return "";
    return s_face_bold->style_name ? s_face_bold->style_name : "Bold";
}

int tip_font_last_ink_top(void)
{
    return s_last_ink_top;
}

int tip_font_line_box_pt(int pt, int *asc, int *desc, int *line_h)
{
    int a, d;
    if (!s_face || !s_face->size)
        return 0;
    (void)pt;
    a = (int)((s_face->size->metrics.ascender + 63) >> 6);
    d = (int)((s_face->size->metrics.descender + 63) >> 6);
    if (asc)
        *asc = a;
    if (desc)
        *desc = d;
    if (line_h)
        *line_h = a - d;
    return 1;
}

int tip_font_draw_utf8(uint8_t *dst, int dst_w, int dst_h, int dx, int dy, const char *utf8,
                       int *out_w, int *out_h)
{
    WCHAR wbuf[512];
    int wn;
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!dst || !utf8 || !utf8[0] || dst_w < 1 || dst_h < 1)
        return 0;
    if (!tip_font_init() || !s_face_bold)
        return 0;
    use_face(s_face_bold);
    if (!ensure_tip_pt())
        return 0;
    wn = str_to_wide(utf8, -1, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn <= 0)
        return 0;
    return draw_wide(dst, dst_w, dst_h, dst_w * 4, 32, dx, dy, wbuf, wn, 255, 255, 255, 0, out_w,
                     out_h);
}

/* CBitmap::GetBits thiscall — vt+0x18, ret 8. Explicit asm: Wine/mingw thiscall fp risks. */
static void *cbitmap_get_bits(void *surf, int x, int y)
{
    void **vt;
    void *fn;
    void *ret = NULL;
    if (!surf)
        return NULL;
    vt = *(void ***)surf;
    if (!vt)
        return NULL;
    fn = vt[0x18 / 4];
    if (!fn)
        return NULL;
    __asm__ __volatile__(
        "pushl %3\n\t"
        "pushl %2\n\t"
        "call *%1\n\t"
        : "=a"(ret)
        : "r"(fn), "r"(x), "r"(y), "c"(surf)
        : "memory", "cc");
    return ret;
}

static int guess_font_px(void *font)
{
    char env[32];
    DWORD n;
    unsigned char *b;
    int i, best = FT_PX_FALLBACK;

    n = GetEnvironmentVariableA("CK_FT_PX", env, (DWORD)sizeof(env));
    if (n > 0 && n < sizeof(env)) {
        int v = atoi(env);
        if (v >= 8 && v <= 32)
            return v;
    }
    if (!font || IsBadReadPtr(font, 0x40))
        return FT_PX_FALLBACK;
    b = (unsigned char *)font;
    for (i = 4; i < 0x30; i++) {
        unsigned char v = b[i];
        if (v == 13 || v == 14 || v == 16 || v == 15 || v == 12)
            return (int)v;
    }
    for (i = 4; i <= 0x28; i += 4) {
        unsigned v = *(unsigned *)(b + i);
        if (v >= 12 && v <= 18)
            best = (int)v;
    }
    return best;
}

/* #region agent log */
static int str_looks_menu(const char *str)
{
    /* English keys + common RU (CP1251) menu labels — detect main-menu draws. */
    static const char *const keys[] = {
        "Tutorial", "Adventure", "Single", "Multi", "Load game", "Editor", "Options",
        "Credits", "Quit", "Change", "News", "Next", "More Info", "Tips", NULL};
    int i, j;
    if (!str)
        return 0;
    for (i = 0; keys[i]; ++i) {
        const char *k = keys[i];
        for (j = 0; str[j]; ++j) {
            int m = 0;
            while (k[m] && str[j + m] &&
                   ((str[j + m] | 32) == (k[m] | 32) || str[j + m] == k[m]))
                m++;
            if (!k[m])
                return 1;
        }
    }
    /* Cyrillic lead bytes common in RU menu (CP1251): 0xC0-0xFF */
    {
        int cyr = 0, n = 0;
        for (i = 0; str[i] && n < 24; ++i, ++n) {
            unsigned char c = (unsigned char)str[i];
            if (c >= 0xC0)
                cyr++;
        }
        if (cyr >= 3 && n >= 4)
            return 1;
    }
    return 0;
}

static void agent_ft(const char *msg, const char *why, int x, int y, int w, int h, int pitch,
                     int bpp, ULONG_PTR vt0, int flags, int path, const char *str)
{
    static volatile LONG s_n;
    LONG n = InterlockedIncrement(&s_n);
    FILE *df;
    char esc[72];
    int i;
    int menu = str_looks_menu(str);
    const char *hid = "H-FTALL";
    if (!hooks_debug_full())
        return;
    if (menu)
        hid = (flags == 0) ? "H-MENU-A" : "H-MENU-D";
    else if (flags == 0 && why && why[0] == 's')
        hid = "H-MENU-A";
    if (n > 200 && !menu)
        return;
    if (str) {
        for (i = 0; i < 64 && str[i]; ++i) {
            char c = str[i];
            esc[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '.';
        }
        esc[i] = 0;
    } else
        esc[0] = 0;
    df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
    if (!df)
        return;
    fprintf(df,
            "{\"sessionId\":\"764ba7\",\"runId\":\"menu1\",\"hypothesisId\":\"%s\","
            "\"location\":\"tip_font.c:cbitmap\",\"message\":\"%s\","
            "\"data\":{\"why\":\"%s\",\"xy\":[%d,%d],\"wh\":[%d,%d],\"pitch\":%d,\"bpp\":%d,"
            "\"vt0\":%lu,\"flags\":%d,\"path\":%d,\"menu\":%d,\"str\":\"%s\"},"
            "\"timestamp\":%lu}\n",
            hid, msg, why ? why : "", x, y, w, h, pitch, bpp, (unsigned long)vt0, flags, path,
            menu, esc, (unsigned long)GetTickCount());
    fclose(df);
}
/* #endregion */

/* Resolve CMemoryDC / CBitmap / soft DIB → bits. Returns 1 and fills outs.
 * noinline: GCC -O2 IPA constprop clone clobbered out_ox (GetBits thiscall
 * kills edx; list text drew at x=5 instead of x+origin). */
static int __attribute__((noinline, noclone)) resolve_cbitmap_bits(void *surf, int x, int y, void **out_bits,
                                                          int *out_w, int *out_h, int *out_pitch,
                                                          int *out_ox, int *out_oy, int *out_path,
                                                          ULONG_PTR *out_vt0, const char **out_why)
{
    void **vt;
    void *bits = NULL;
    void *bmp = NULL;
    short w = 0, h = 0, pitch_s = 0, bpp_s = 0;
    int pitch = 0, bpp = 0, path = 0;
    int soft_w = 0, soft_h = 0, soft_stride = 0, soft_td = 0;
    uint8_t *soft_bits = NULL;
    ULONG_PTR vt0 = 0;
    const char *why = NULL;
    int ox = 0, oy = 0;

    if (!surf || IsBadReadPtr(surf, 0x20)) {
        if (out_why)
            *out_why = "bad_surf";
        return 0;
    }

    vt = *(void ***)surf;
    if (vt && !IsBadReadPtr(vt, 4))
        vt0 = (ULONG_PTR)vt[0];

    /*
     * Retail DrawText arg0 is CMemoryDC (vt+0 = 0x41d650), not CBitmap.
     * Layout (ctor 0x41d680): +0x14/+0x16 origin; +0x18 = CBitmap*; +0x1c = lock bits.
     * CBitmap: +8 w, +a h, +c bpp, +e pitch, GetBits @ vt+0x18.
     */
    if (vt0 == 0x0041d650u || vt0 == 0x41d650u) {
        bmp = *(void **)((BYTE *)surf + 0x18);
        ox = (int)*(short *)((BYTE *)surf + 0x14);
        oy = (int)*(short *)((BYTE *)surf + 0x16);
        path = 10;
    } else if (vt0 == 0x00412250u || vt0 == 0x412250u) {
        bmp = surf;
        path = 11;
    } else {
        void *cand = *(void **)((BYTE *)surf + 0x18);
        if (cand && !IsBadReadPtr(cand, 0x10)) {
            short cb = *(short *)((BYTE *)cand + 0xc);
            short cw = *(short *)((BYTE *)cand + 0x8);
            if ((cb == 16 || cb == 15 || cb == 8) && cw >= 8 && cw <= 4096) {
                bmp = cand;
                ox = (int)*(short *)((BYTE *)surf + 0x14);
                oy = (int)*(short *)((BYTE *)surf + 0x16);
                path = 12;
            }
        }
        if (!bmp)
            bmp = surf;
    }

    if (bmp && !IsBadReadPtr(bmp, 0x10)) {
        w = *(short *)((BYTE *)bmp + 0x8);
        h = *(short *)((BYTE *)bmp + 0xa);
        bpp_s = *(short *)((BYTE *)bmp + 0xc);
        pitch_s = *(short *)((BYTE *)bmp + 0xe);
        bpp = (int)bpp_s;
        pitch = (int)pitch_s;
    }

    if (bmp && w >= 8 && h >= 8 && w <= 4096 && h <= 4096 && (bpp == 16 || bpp == 15)) {
        int stride = pitch < 0 ? -pitch : pitch;
        if (stride == 0)
            stride = w * 2;
        if (stride >= w) {
            bits = cbitmap_get_bits(bmp, 0, 0);
            if (bits) {
                pitch = pitch == 0 ? stride : pitch;
                path = (path >= 10) ? (path + 10) : 1;
            } else
                why = "getbits_null";
        } else
            why = "bad_pitch";
    } else if (bmp)
        why = (bpp != 16 && bpp != 15) ? "bpp_not16" : "bad_wh";
    else
        why = "no_bmp";

    if (!bits && x >= 0 && y >= 0 &&
        ck_soft_dib_get(&soft_bits, &soft_w, &soft_h, &soft_stride, &soft_td) && x < soft_w &&
        y < soft_h) {
        bits = soft_bits;
        w = (short)soft_w;
        h = (short)soft_h;
        bpp = 16;
        ox = oy = 0;
        if (soft_td)
            pitch = soft_stride;
        else {
            bits = soft_bits + (size_t)(soft_h - 1) * (size_t)soft_stride;
            pitch = -soft_stride;
        }
        path = 2;
        why = NULL;
    }

    if (!bits) {
        if (out_why)
            *out_why = why ? why : "no_bits";
        if (out_vt0)
            *out_vt0 = vt0;
        if (out_path)
            *out_path = path;
        if (out_w)
            *out_w = (int)w;
        if (out_h)
            *out_h = (int)h;
        return 0;
    }

    if (out_bits)
        *out_bits = bits;
    if (out_w)
        *out_w = (int)w;
    if (out_h)
        *out_h = (int)h;
    if (out_pitch)
        *out_pitch = pitch;
    if (out_ox)
        *out_ox = ox;
    if (out_oy)
        *out_oy = oy;
    if (out_path)
        *out_path = path;
    if (out_vt0)
        *out_vt0 = vt0;
    if (out_why)
        *out_why = NULL;
    (void)bpp_s;
    (void)pitch_s;
    return 1;
}

int tip_font_probe_surf(void *surf, int *out_path, int *out_w, int *out_h, unsigned long *out_vt0)
{
    void *bits = NULL;
    int w = 0, h = 0, pitch = 0, ox = 0, oy = 0, path = 0;
    ULONG_PTR vt0 = 0;
    const char *why = NULL;
    int ok = resolve_cbitmap_bits(surf, 0, 0, &bits, &w, &h, &pitch, &ox, &oy, &path, &vt0, &why);
    if (out_path)
        *out_path = path;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    if (out_vt0)
        *out_vt0 = (unsigned long)vt0;
    (void)bits;
    (void)why;
    return ok;
}

int tip_font_draw_cbitmap_rgb_pt(void *surf, void *font, int x, int y, const char *str, int maxlen,
                                 int r, int g, int b, int shadow, int pt)
{
    void *bits = NULL;
    int w = 0, h = 0, pitch = 0, ox = 0, oy = 0, path = 0, wn;
    ULONG_PTR vt0 = 0;
    const char *why = NULL;
    WCHAR wbuf[512];

    (void)font;
    if (!str || IsBadReadPtr(str, 1)) {
        agent_ft("ft-fail", "bad_str", x, y, 0, 0, 0, 0, 0, -1, 0, NULL);
        return 0;
    }
    if (!tip_font_init() || !s_face_bold) {
        agent_ft("ft-fail", "no_ft", x, y, 0, 0, 0, 0, 0, -1, 0, str);
        return 0;
    }
    if (!resolve_cbitmap_bits(surf, x, y, &bits, &w, &h, &pitch, &ox, &oy, &path, &vt0, &why)) {
        agent_ft("ft-fail", why ? why : "no_bits", x, y, w, h, 0, 0, vt0, -1, path, str);
        return 0;
    }
    /* ImageButton labels: Regular only. All other UI uses Bold via wstr/DrawTextBmp. */
    if (!s_face_reg) {
        agent_ft("ft-fail", "no_reg_face", x, y, w, h, pitch, 16, vt0, -1, path, str);
        return 0;
    }
    use_face(s_face_reg);
    if (pt <= 0)
        pt = s_tip_pt;
    if (!ensure_pt(pt)) {
        agent_ft("ft-fail", "set_pt", x, y, w, h, pitch, 16, vt0, -1, path, str);
        return 0;
    }
    wn = str_to_wide(str, maxlen, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn <= 0) {
        agent_ft("ft-fail", "enc", x, y, w, h, pitch, 16, vt0, -1, path, str);
        return 0;
    }
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    if (!draw_wide((uint8_t *)bits, w, h, pitch, 16, x + ox, y + oy, wbuf, wn, r, g, b,
                   shadow ? 1 : 0, NULL, NULL)) {
        agent_ft("ft-fail", "draw0", x, y, w, h, pitch, 16, vt0, -1, path, str);
        return 0;
    }
    agent_ft("ft-ctext", path >= 20 ? "memdc" : (path == 2 ? "soft_dib" : "getbits"), x + ox,
             y + oy, w, h, pitch, 16, vt0, (r << 16) | (g << 8) | b, path, str);
    return 1;
}

int tip_font_draw_cbitmap_wstr_pt(void *surf, int x, int y, const WCHAR *ws, int nch, int max_w,
                                  int r, int g, int b, int shadow, int pt)
{
    void *bits = NULL;
    int w = 0, h = 0, pitch = 0, ox = 0, oy = 0, path = 0;
    ULONG_PTR vt0 = 0;
    const char *why = NULL;
    int painted = 0, line_h = 14, cy, i;
    WCHAR line[384];

    if (!ws || nch < 1 || IsBadReadPtr(ws, nch * 2))
        return 0;
    if (!tip_font_init() || !s_face_bold)
        return 0;
    if (!resolve_cbitmap_bits(surf, x, y, &bits, &w, &h, &pitch, &ox, &oy, &path, &vt0, &why))
        return 0;
    /* CMemoryDC origin at +0x14/+0x16. GCC -O2 IPA clone of resolve clobbered out_ox
     * (DrawWide list text at x=5 instead of 5+origin). Re-read after resolve. */
    if (vt0 == 0x0041d650u || vt0 == 0x41d650u) {
        ox = (int)*(short *)((BYTE *)surf + 0x14);
        oy = (int)*(short *)((BYTE *)surf + 0x16);
    }
    use_face(s_face_bold);
    if (pt <= 0)
        pt = s_tip_pt;
    if (!ensure_pt(pt))
        return 0;
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    {
        int dummy = 0;
        WCHAR sample[3] = {L'A', L'y', 0};
        if (draw_wide(NULL, 0, 0, 0, 0, 0, 0, sample, 2, 0, 0, 0, 0, &dummy, &line_h) &&
            line_h > 0)
            line_h += 2;
        else
            line_h = 14;
    }
    if (max_w < 8)
        max_w = 0;
    cy = y + oy;
    i = 0;
    if (nch > 800)
        nch = 800;
    while (i < nch && ws[i]) {
        int li = 0, last_sp = -1;
        int tw = 0, th = 0;
        int wrapped = 0;
        if (ws[i] == L'\r') {
            i++;
            continue;
        }
        if (ws[i] == L'\n') {
            cy += line_h;
            i++;
            continue;
        }
        while (i < nch && ws[i] && ws[i] != L'\n' && ws[i] != L'\r' && li < 380) {
            line[li] = ws[i];
            line[li + 1] = 0;
            if (max_w > 0) {
                if (!draw_wide(NULL, 0, 0, 0, 0, 0, 0, line, li + 1, 0, 0, 0, 0, &tw, &th))
                    break;
                if (tw > max_w && li > 0) {
                    if (last_sp >= 0) {
                        i = i - (li - last_sp) + 1;
                        li = last_sp;
                    }
                    line[li] = 0;
                    wrapped = 1;
                    break;
                }
            }
            if (ws[i] == L' ')
                last_sp = li;
            li++;
            i++;
        }
        if (li > 0) {
            if (draw_wide((uint8_t *)bits, w, h, pitch, 16, x + ox, cy, line, li, r, g, b,
                          shadow ? 1 : 0, NULL, NULL))
                painted = 1;
            cy += line_h;
        }
        if (wrapped) {
            while (i < nch && ws[i] == L' ')
                i++;
        }
        if (i < nch && (ws[i] == L'\n' || ws[i] == L'\r'))
            i++;
        if (cy > h - 2)
            break;
    }
    if (painted)
        agent_ft("ft-textw", path >= 20 ? "memdc" : (path == 2 ? "soft_dib" : "getbits"), x + ox, y + oy,
                 w, h, pitch, 16, vt0, (r << 16) | (g << 8) | b, path, "wstr");
    return painted;
}

int tip_font_draw_cbitmap_rgb(void *surf, void *font, int x, int y, const char *str, int maxlen,
                              int r, int g, int b, int shadow)
{
    return tip_font_draw_cbitmap_rgb_pt(surf, font, x, y, str, maxlen, r, g, b, shadow, 0);
}

/* Bold ANSI UI (CUIText etc). Same as rgb_pt but tahomabd. */
int tip_font_draw_cbitmap_bold_pt(void *surf, void *font, int x, int y, const char *str, int maxlen,
                                  int r, int g, int b, int shadow, int pt)
{
    void *bits = NULL;
    int w = 0, h = 0, pitch = 0, ox = 0, oy = 0, path = 0, wn;
    ULONG_PTR vt0 = 0;
    const char *why = NULL;
    WCHAR wbuf[512];

    (void)font;
    if (!str || IsBadReadPtr(str, 1))
        return 0;
    if (!tip_font_init() || !s_face_bold)
        return 0;
    if (!resolve_cbitmap_bits(surf, x, y, &bits, &w, &h, &pitch, &ox, &oy, &path, &vt0, &why))
        return 0;
    use_face(s_face_bold);
    if (pt <= 0)
        pt = s_tip_pt;
    if (!ensure_pt(pt))
        return 0;
    wn = str_to_wide(str, maxlen, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn <= 0)
        return 0;
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    if (!draw_wide((uint8_t *)bits, w, h, pitch, 16, x + ox, y + oy, wbuf, wn, r, g, b,
                   shadow ? 1 : 0, NULL, NULL))
        return 0;
    /* #region agent log */
    agent_ft("ft-bold", path >= 20 ? "memdc" : (path == 2 ? "soft_dib" : "getbits"), x + ox, y + oy,
             w, h, pitch, 16, vt0, (r << 16) | (g << 8) | b, path, str);
    /* #endregion */
    return 1;
}

int tip_font_draw_cbitmap(void *surf, void *font, int x, int y, const char *str, int maxlen,
                          int flags)
{
    void *bits = NULL;
    int w = 0, h = 0, pitch = 0, ox = 0, oy = 0, path = 0, wn;
    int fx = 0, fy = 0;
    ULONG_PTR vt0 = 0;
    const char *why = NULL;
    WCHAR wbuf[512];

    (void)font;
    if (!str || IsBadReadPtr(str, 1)) {
        agent_ft("ft-fail", "bad_str", x, y, 0, 0, 0, 0, 0, flags, 0, NULL);
        return 0;
    }
    if (!tip_font_init() || !s_face_bold) {
        agent_ft("ft-fail", "no_ft", x, y, 0, 0, 0, 0, 0, flags, 0, str);
        return 0;
    }
    if (!resolve_cbitmap_bits(surf, x, y, &bits, &w, &h, &pitch, &ox, &oy, &path, &vt0, &why)) {
        agent_ft("ft-fail", why ? why : "no_bits", x, y, w, h, 0, 0, vt0, flags, path, str);
        return 0;
    }

    /* Soft/bitmap pixel position. HelpText often passes map-space XY; DC origin Y
     * (-camT) is set but origin X is often 0 (logs: origin[0,-2352], fx=12k).
     * Editor status stays in-bounds while X tracks camL (hint-ox1: ox=0,
     * raw≈camL+10) — OOB-only cam subtract never ran; pin bottom-band X. */
    fx = x + ox;
    fy = y + oy;
    if (ox == 0 && w > 0 && h > 0) {
        LONG cL = 0, cT = 0, cR = 0, cB = 0;
        int pL = 0, pT = 0, pR = 0, pB = 0, mL = 0, mT = 0, mR = 0, mB = 0;
        int lb = ck_zoom_letterbox_get(&pL, &pT, &pR, &pB, &mL, &mT, &mR, &mB);
        mm_read_cam(&cL, &cT, &cR, &cB);
        if (cR > cL) {
            int nx = x - (int)cL;
            int ny = (oy != 0) ? fy : (y - (int)cT);
            int oob = (fx < -8 || fx >= w + 8);
            int bottom = (fy >= h - 120);
            if (lb && mL > pL)
                nx += mL;
            if (nx >= -8 && nx < w + 8) {
                if (oob && ny >= -8 && ny < h + 8) {
                    fx = nx;
                    fy = ny;
                } else if (bottom) {
                    fx = nx; /* Y already stable via oy / screen Y */
                }
            }
        }
    }
    if (w > 0 && h > 0 && (fx < -8 || fy < -8 || fx >= w + 8 || fy >= h + 8))
        return 0; /* native DrawTextBmp — MemoryDC transform */

    /* Same FreeType size as ZoomMap tip (12pt @ 96dpi). */
    use_face(s_face_bold);
    if (!ensure_tip_pt()) {
        agent_ft("ft-fail", "set_pt", x, y, w, h, pitch, 16, vt0, flags, path, str);
        return 0;
    }
    wn = str_to_wide(str, maxlen, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn <= 0) {
        agent_ft("ft-fail", "enc", x, y, w, h, pitch, 16, vt0, flags, path, str);
        return 0;
    }
    /* Retail draws 2–3 offset passes (flags=0 black, flags=0x7FFF white). We skip
     * shadow passes; no FT drop-shadow (looks smeared on dark tip box). */
    {
        unsigned f = (unsigned)flags & 0x7FFFu;
        int r, g, b;
        if (f == 0) {
            agent_ft("ft-drawtext", "skip_shadow_pass", fx, fy, w, h, pitch, 16, vt0, flags, path,
                     str);
            return 1; /* swallow native black outline pass */
        }
        r = (int)(((f >> 10) & 31u) * 255u / 31u);
        g = (int)(((f >> 5) & 31u) * 255u / 31u);
        b = (int)((f & 31u) * 255u / 31u);
        if (!draw_wide((uint8_t *)bits, w, h, pitch, 16, fx, fy, wbuf, wn, r, g, b, 0, NULL,
                       NULL)) {
            agent_ft("ft-fail", "draw0", fx, fy, w, h, pitch, 16, vt0, flags, path, str);
            return 0;
        }
    }
    agent_ft("ft-drawtext", path >= 20 ? "memdc" : (path == 2 ? "soft_dib" : "getbits"), fx, fy, w,
             h, pitch, 16, vt0, flags, path, str);
    return 1;
}

/* —— 32-bit BGRA present overlay (KDE-like AA) —— */

static void ov_cs_init(void)
{
    if (!s_ov_cs_ok) {
        InitializeCriticalSection(&s_ov_cs);
        s_ov_cs_ok = 1;
    }
}

int tip_font_present_want(void)
{
    if (s_ov_want < 0) {
        char env[32];
        DWORD n = GetEnvironmentVariableA("CK_FT_PRESENT", env, (DWORD)sizeof(env));
        /* Default OFF. Sticky live/upsert caused ghost piles (live→60+).
         * Soft FT is the stable path. Opt-in: CK_FT_PRESENT=1 (ov-only, no sticky). */
        s_ov_want = 0;
        if (n > 0 && n < sizeof(env) &&
            (env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' || env[0] == 'T'))
            s_ov_want = 1;
    }
    return s_ov_want;
}

int tip_font_present_active(void)
{
    return tip_font_present_want() && vk_present_ready();
}

static int ov_push(int x, int y, const WCHAR *ws, int nch, int max_w, int r, int g, int b,
                   int shadow, int pt, int bold)
{
    FtOvItem *it;
    int i, lim;
    if (!ws || nch < 1)
        return 0;
    if (!tip_font_init())
        return 0;
    ov_cs_init();
    EnterCriticalSection(&s_ov_cs);
    if (s_ov_n >= FT_OV_MAX) {
        LeaveCriticalSection(&s_ov_cs);
        return 0;
    }
    it = &s_ov[s_ov_n];
    lim = nch < FT_OV_NCH - 1 ? nch : FT_OV_NCH - 1;
    for (i = 0; i < lim; ++i)
        it->t[i] = ws[i];
    it->t[lim] = 0;
    it->nch = lim;
    it->x = x;
    it->y = y;
    it->max_w = max_w;
    it->r = r < 0 ? 0 : (r > 255 ? 255 : r);
    it->g = g < 0 ? 0 : (g > 255 ? 255 : g);
    it->b = b < 0 ? 0 : (b > 255 ? 255 : b);
    it->shadow = shadow ? 1 : 0;
    it->pt = pt > 0 ? pt : s_tip_pt;
    it->bold = bold ? 1 : 0;
    s_ov_n++;
    LeaveCriticalSection(&s_ov_cs);
    return 1;
}

int tip_font_present_queue_wstr(int x, int y, const WCHAR *ws, int nch, int max_w, int r, int g,
                                int b, int shadow, int pt, int bold)
{
    if (!tip_font_present_want())
        return 0;
    if (nch < 0) {
        nch = 0;
        if (ws)
            while (ws[nch] && nch < 2000)
                nch++;
    }
    return ov_push(x, y, ws, nch, max_w, r, g, b, shadow, pt, bold);
}

/* Local CMemoryDC coords → soft absolute (origin at +0x14/+0x16). */
int tip_font_present_queue_wstr_surf(void *surf, int x, int y, const WCHAR *ws, int nch, int max_w,
                                     int r, int g, int b, int shadow, int pt, int bold)
{
    int ox = 0, oy = 0;
    if (surf && !IsBadReadPtr(surf, 0x18)) {
        ULONG_PTR vt0 = 0;
        void **vt = *(void ***)surf;
        if (vt && !IsBadReadPtr(vt, 4))
            vt0 = (ULONG_PTR)vt[0];
        if (vt0 == 0x0041d650u || vt0 == 0x41d650u) {
            ox = (int)*(short *)((BYTE *)surf + 0x14);
            oy = (int)*(short *)((BYTE *)surf + 0x16);
        }
    }
    return tip_font_present_queue_wstr(x + ox, y + oy, ws, nch, max_w, r, g, b, shadow, pt, bold);
}

int tip_font_present_queue_str_surf(void *surf, int x, int y, const char *str, int maxlen, int max_w,
                                    int r, int g, int b, int shadow, int pt, int bold)
{
    WCHAR wbuf[FT_OV_NCH];
    int wn;
    if (!str)
        return 0;
    wn = str_to_wide(str, maxlen, wbuf, FT_OV_NCH);
    if (wn <= 0)
        return 0;
    return tip_font_present_queue_wstr_surf(surf, x, y, wbuf, wn, max_w, r, g, b, shadow, pt, bold);
}

int tip_font_present_queue_str(int x, int y, const char *str, int maxlen, int max_w, int r, int g,
                               int b, int shadow, int pt, int bold)
{
    WCHAR wbuf[FT_OV_NCH];
    int wn;
    if (!tip_font_present_want() || !str)
        return 0;
    wn = str_to_wide(str, maxlen, wbuf, FT_OV_NCH);
    if (wn <= 0)
        return 0;
    return ov_push(x, y, wbuf, wn, max_w, r, g, b, shadow, pt, bold);
}

static int paint_ov_item_bgra(uint8_t *dst, int dst_w, int dst_h, int dx, int dy, const FtOvItem *it,
                              int pt)
{
    FT_Face face = it->bold ? s_face_bold : s_face_reg;
    int max_w = it->max_w;
    int line_h = 14, cy, i, painted = 0;
    WCHAR line[FT_OV_NCH];

    if (!face || !dst)
        return 0;
    use_face(face);
    if (!ensure_pt(pt))
        return 0;
    {
        int dummy = 0;
        WCHAR sample[3] = {L'A', L'y', 0};
        if (draw_wide(NULL, 0, 0, 0, 0, 0, 0, sample, 2, 0, 0, 0, 0, &dummy, &line_h) && line_h > 0)
            line_h += 2;
        else
            line_h = 14;
    }
    if (max_w < 8)
        max_w = 0;
    cy = dy;
    i = 0;
    while (i < it->nch && it->t[i]) {
        int li = 0, last_sp = -1, wrapped = 0, tw = 0, th = 0;
        if (it->t[i] == L'\r') {
            i++;
            continue;
        }
        if (it->t[i] == L'\n') {
            cy += line_h;
            i++;
            continue;
        }
        while (i < it->nch && it->t[i] && it->t[i] != L'\n' && it->t[i] != L'\r' &&
               li < FT_OV_NCH - 2) {
            line[li] = it->t[i];
            line[li + 1] = 0;
            if (max_w > 0) {
                if (!draw_wide(NULL, 0, 0, 0, 0, 0, 0, line, li + 1, 0, 0, 0, 0, &tw, &th))
                    break;
                if (tw > max_w && li > 0) {
                    if (last_sp >= 0) {
                        i = i - (li - last_sp) + 1;
                        li = last_sp;
                    }
                    line[li] = 0;
                    wrapped = 1;
                    break;
                }
            }
            if (it->t[i] == L' ')
                last_sp = li;
            li++;
            i++;
        }
        if (li > 0) {
            if (draw_wide(dst, dst_w, dst_h, dst_w * 4, 32, dx, cy, line, li, it->r, it->g, it->b,
                          it->shadow, NULL, NULL))
                painted = 1;
            cy += line_h;
        }
        if (wrapped) {
            while (i < it->nch && it->t[i] == L' ')
                i++;
        } else if (i < it->nch && (it->t[i] == L'\n' || it->t[i] == L'\r'))
            i++;
        else if (li == 0)
            break;
    }
    return painted;
}

void tip_font_present_invalidate(void)
{
    ov_cs_init();
    EnterCriticalSection(&s_ov_cs);
    s_ov_n = 0;
    LeaveCriticalSection(&s_ov_cs);
}

void tip_font_present_flush(uint8_t *dst, int dst_w, int dst_h, int sw, int sh, int ox, int oy,
                            int dw, int dh)
{
    FtOvItem local[FT_OV_MAX];
    int n = 0, i, painted = 0;
    if (!dst || dst_w < 1 || dst_h < 1 || sw < 1 || sh < 1 || dw < 1 || dh < 1)
        return;
    if (!tip_font_present_want() || !tip_font_init())
        return;
    ov_cs_init();
    EnterCriticalSection(&s_ov_cs);
    /* This frame only — never sticky s_live (upsert grew to 60+ ghosts). */
    n = s_ov_n;
    if (n > FT_OV_MAX)
        n = FT_OV_MAX;
    if (n > 0)
        memcpy(local, s_ov, (size_t)n * sizeof(FtOvItem));
    s_ov_n = 0;
    /* #region agent log */
    {
        static volatile LONG s_fl_n;
        LONG fn = InterlockedIncrement(&s_fl_n);
        if (fn <= 40 || n > 0) {
            FILE *df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
            if (df) {
                fprintf(df,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"ghost1\",\"hypothesisId\":\"H-GHOST\","
                        "\"location\":\"tip_font.c:present_flush\",\"message\":\"ft-present\","
                        "\"data\":{\"n\":%ld,\"ov\":%d,\"live\":0,\"soft\":[%d,%d],\"dst\":[%d,%d]},"
                        "\"timestamp\":%lu}\n",
                        (long)fn, n, sw, sh, dst_w, dst_h, (unsigned long)GetTickCount());
                fclose(df);
            }
        }
    }
    /* #endregion */
    LeaveCriticalSection(&s_ov_cs);
    for (i = 0; i < n; ++i) {
        int sx = ox + (int)(((long)local[i].x * dw) / sw);
        int sy = oy + (int)(((long)local[i].y * dh) / sh);
        int mw = local[i].max_w;
        int pt = local[i].pt;
        if (dw != sw && sw > 0) {
            pt = (pt * dw + sw / 2) / sw;
            if (pt < 8)
                pt = 8;
            if (pt > 32)
                pt = 32;
            if (mw > 0)
                mw = (mw * dw + sw / 2) / sw;
        }
        local[i].max_w = mw;
        if (paint_ov_item_bgra(dst, dst_w, dst_h, sx, sy, &local[i], pt))
            painted++;
    }
    (void)painted;
}

