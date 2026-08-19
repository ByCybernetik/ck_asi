#ifndef WINMM_PROXY_HITCH_H
#define WINMM_PROXY_HITCH_H

#include <windows.h>
#include <stddef.h>

/* Deep freeze / hitch tracing (debug session). QPC-based. */

void hitch_init(void);
void hitch_shutdown(void);

/* Tag last high-level event for correlation (CVM, movie, map load, …). */
void hitch_mark(const char *tag);
/* Copy current mark (empty if none). Safe from VEH. */
void hitch_get_mark(char *out, size_t n);

/* Log if dt_ms >= threshold (default 50). */
void hitch_note_ms(const char *hypothesisId, const char *location, const char *message,
                   const char *tag, double dt_ms, const char *extra_json);

/* Optional: cam_smooth duration for next present_sample (H-CAM). */
void hitch_set_cam_ms(double cam_ms);

/* Optional: GPU submit sub-phases (pump/terrain/decor/obj/overlay) for fps-budget. */
void hitch_set_submit_phases(double pump_ms, double terr_ms, double decor_ms, double obj_ms,
                             double ov_ms);
/* Optional: last vk_obj_record phase split. */
void hitch_set_obj_phases(double ms_lc, double ms_build, double ms_sort, double ms_vert,
                          double ms_fb, int quads, int vis, int inst);

/* Frame pipeline sample after a present attempt.
 * frame_ms = wall time from previous present-end to this present-end (honest FPS). */
void hitch_present_sample(double gap_ms, double frame_ms, double blit_ms, double extend_ms,
                          double fence_ms, double compose_ms, double acquire_ms, double submit_ms,
                          int pr, int fullish, int blit_w, int blit_h, int gpu_busy,
                          int present_mode, int soft_w, int soft_h, int swap_w, int swap_h,
                          double pace_ms);

LONGLONG hitch_qpc_now(void);
double hitch_qpc_ms_since(LONGLONG t0);

/* Present/render thread id (SetDIBits); used to ignore audio/DI mark pollution. */
void hitch_set_main_tid(DWORD tid);
DWORD hitch_main_tid(void);
double hitch_ms_since_last_present(void);

#endif
