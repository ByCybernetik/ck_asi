#ifndef WINMM_PROXY_HOOKS_H
#define WINMM_PROXY_HOOKS_H

#include <windows.h>

void hooks_install(void);
void hooks_remove(void);

/* Breadcrumb for VEH / editor-test crash capture. */
void hooks_crash_phase(const char *fmt, ...);
void hooks_get_crash_ctx(char *out, size_t n);

/* Debug: 12-byte scanline vector at column idx. Returns 0 if table missing. */
int ck_scan_probe(int idx, unsigned *begin, unsigned *cur, unsigned *end);

/* ZoomMap letterbox: playfield vs map rect in soft pixels. Returns 1 if active. */
int ck_zoom_letterbox_get(int *play_l, int *play_t, int *play_r, int *play_b, int *map_l,
                          int *map_t, int *map_r, int *map_b);
void ck_zoom_letterbox_clear(void);
/* After soft blit: refresh ZoomMap fill TTL when dest is letterboxed map. */
void ck_zoom_letterbox_note_blit(int x, int y, int w, int h);
/* Soft AABB of last shifted HelpText tip (for pillar glyph overlay). 1 if fresh. */
int ck_zoom_tip_overlay_get(int *x0, int *y0, int *x1, int *y1);
/* Tip string + soft draw origin after X-shift. */
int ck_zoom_tip_text_get(char *out, int out_n, int *x, int *y);

#endif
