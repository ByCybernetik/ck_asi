#ifndef WINMM_PROXY_VK_PRESENT_H
#define WINMM_PROXY_VK_PRESENT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Option B: replace GDI SetDIBitsToDevice with Wine vulkan-1 present.
 * Returns 1 if the frame was presented (caller must NOT call real GDI).
 * Returns 0 to fall back to real SetDIBitsToDevice.
 *
 * Disable: CK_GDI_FALLBACK=1 only (CK_VK_REPLACE ignored — stale under Wine).
 */
int vk_present_try(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc,
                   UINT start, UINT lines, CONST VOID *bits, CONST BITMAPINFO *bmi, UINT usage);

void vk_present_shutdown(void);

/* Soft DIB vs real client: used to spoof GetClientRect / remap mouse when scaled. */
int vk_present_scale_active(void);
int vk_present_soft_size(int *w, int *h);
HWND vk_present_hwnd(void);
/* Cache game 16bpp soft DIB (same buffer DrawText writes). Fill returns pixels or <0. */
void ck_soft_dib_note(const void *bits, int w, int h, int stride, int top_down, int bpp);
int ck_soft_dib_fill16(int x0, int y0, int x1, int y1, unsigned short c565);
/* Lock soft DIB for FreeType text. Returns 1 if bits writable. */
int ck_soft_dib_get(unsigned char **bits, int *w, int *h, int *stride, int *top_down);

/* Fullscreen BGRA frame during cutscenes (winegstreamer → Vulkan). Letterboxed. */
int vk_present_movie_frame(HWND hwnd, const void *bgra, int w, int h);

/* RE main-menu path: stretch BGRA to fill HWND (no letterbox), like native Renderer. */
int vk_present_menu_frame(HWND hwnd, const void *bgra, int w, int h);

/* Suspend Vulkan swapchain so Wine Video Renderer GDI can paint the HWND. */
void vk_present_movie_begin(HWND hwnd);
void vk_present_movie_end(void);
/* Reset soft DIB size to last game soft after cutscenes (keep scale spoof; avoid 640×). */
void vk_present_restore_game_soft(void);

/* 1 if Vulkan present path can accept BGRA overlay text this frame. */
int vk_present_ready(void);

/* Shift playfield band at present time only (does not mutate g.fb).
 * +dx/+dy move content right/down. Clears after present. */
void vk_present_set_pan_shift(int dx, int dy);
int vk_present_scroll_playfield(int dx, int dy); /* alias → set_pan_shift */
/* Debug sample of 4 playfield pixels. */
unsigned vk_present_fb_sample4(void);

#ifdef __cplusplus
}
#endif

#endif
