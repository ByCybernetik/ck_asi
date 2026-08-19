#ifndef CK_TIP_FONT_H
#define CK_TIP_FONT_H

#include <stdint.h>
#include <windows.h>

/* FreeType tip / UI text (hinted Tahoma Bold). */

int tip_font_init(void);
void tip_font_shutdown(void);

/* Blend white tip + black shadow over BGRA staging (present path). */
int tip_font_draw_utf8(uint8_t *dst, int dst_w, int dst_h, int dx, int dy, const char *utf8,
                       int *out_w, int *out_h);

/*
 * Replace retail DrawTextBmp: draw into CBitmap soft surface (RGB555/565/8).
 * str is game encoding (CP1251 or UTF-8). Returns 1 if painted (skip native).
 */
int tip_font_draw_cbitmap(void *surf, void *font, int x, int y, const char *str, int maxlen,
                          int flags);

/*
 * Explicit RGB into the same surfaces as tip_font_draw_cbitmap (no flags==0 skip).
 * Used by CTextImage ImageButton labels (FontColor often black).
 * shadow: 1 = +1,+1 black shadow (tips); 0 = none (dark menu labels).
 */
int tip_font_draw_cbitmap_rgb(void *surf, void *font, int x, int y, const char *str, int maxlen,
                              int r, int g, int b, int shadow);

/*
 * Like tip_font_draw_cbitmap_rgb but at explicit point size (96dpi).
 * pt<=0 uses tip default (CK_FT_TIP_PT / 12). Regular face — ImageButton labels only.
 */
int tip_font_draw_cbitmap_rgb_pt(void *surf, void *font, int x, int y, const char *str, int maxlen,
                                 int r, int g, int b, int shadow, int pt);

/* Bold ANSI at pt — CUIText / non-button UI (not ImageButton). */
int tip_font_draw_cbitmap_bold_pt(void *surf, void *font, int x, int y, const char *str, int maxlen,
                                  int r, int g, int b, int shadow, int pt);

/* Measure CP1251/UTF-8 string at tip pt size. Returns 1 and sets out_w/out_h. */
int tip_font_measure(const char *str, int maxlen, int *out_w, int *out_h);

/* Measure at explicit pt (96dpi). pt<=0 → tip default. Regular (button layout). */
int tip_font_measure_pt(const char *str, int maxlen, int pt, int *out_w, int *out_h);

/* Measure UTF-16 at pt using Bold face. */
int tip_font_measure_wstr_pt(const WCHAR *ws, int nch, int pt, int *out_w, int *out_h);

/* Active CTextImage face style for debug logs ("Regular" / empty if missing). */
const char *tip_font_ctext_style(void);

/* Active Bold UI face style for debug logs. */
const char *tip_font_ui_style(void);

/* After measure/draw: ink top relative to dy (line-box top). */
int tip_font_last_ink_top(void);

/* Current face line box (px). Call after measure_pt. desc is typically negative. */
int tip_font_line_box_pt(int pt, int *asc, int *desc, int *line_h);

/* Probe CMemoryDC / CBitmap / soft DIB without drawing. Returns 1 if bits resolved. */
int tip_font_probe_surf(void *surf, int *out_path, int *out_w, int *out_h, unsigned long *out_vt0);

/*
 * UTF-16 UI string (TextW / DrawWide list+combo). Bold face. max_w>0 enables wrap.
 * Returns 1 if painted. Buttons stay Regular via tip_font_draw_cbitmap_rgb_pt.
 */
int tip_font_draw_cbitmap_wstr_pt(void *surf, int x, int y, const WCHAR *ws, int nch, int max_w,
                                  int r, int g, int b, int shadow, int pt);

/* 32-bit BGRA present overlay — queue in hooks, flush in vk compose_staging. */
int tip_font_present_want(void);
int tip_font_present_active(void);
int tip_font_present_queue_wstr(int x, int y, const WCHAR *ws, int nch, int max_w, int r, int g,
                                int b, int shadow, int pt, int bold);
int tip_font_present_queue_wstr_surf(void *surf, int x, int y, const WCHAR *ws, int nch, int max_w,
                                     int r, int g, int b, int shadow, int pt, int bold);
int tip_font_present_queue_str(int x, int y, const char *str, int maxlen, int max_w, int r, int g,
                               int b, int shadow, int pt, int bold);
int tip_font_present_queue_str_surf(void *surf, int x, int y, const char *str, int maxlen, int max_w,
                                    int r, int g, int b, int shadow, int pt, int bold);
void tip_font_present_flush(uint8_t *dst, int dst_w, int dst_h, int sw, int sh, int ox, int oy,
                            int dw, int dh);
/* Drop sticky present overlay (soft resize / full UI refresh). */
void tip_font_present_invalidate(void);

#endif
