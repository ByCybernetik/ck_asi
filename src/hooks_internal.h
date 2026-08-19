#ifndef HOOKS_INTERNAL_H
#define HOOKS_INTERNAL_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    LONG x;
    LONG y;
} TagPoint;

enum { CK_FORCE_W = 1920, CK_FORCE_H = 1080 };

enum {
    CK_SCANLINE_TAB = 0x0076ff78,
    CK_SCANLINE_END = 0x00774a78,
    CK_SCANLINE_N = 1600,
    CK_SCAN_I2_BASE = 0x007a1918,
    CK_SCAN_I2_END = 0x007a6418,
    CK_SCAN_I2_SIDE = 0x007a6434,
    CK_SCAN_I2_N = 1920
};

/* ---- patch ---- */
BOOL patch_iat_entry(HMODULE mod, const char *dll, const char *func, void *hook, void **orig_out,
                     void **slot_out);
void restore_iat_slot(void *slot, void *orig);
BOOL install_inline_hook(void *target, void *hook, SIZE_T steal, BYTE **tramp_out, BYTE *saved_out);
void remove_inline_hook(void *target, SIZE_T steal, const BYTE *saved, BYTE *tramp);

/* ---- util / env ---- */
extern int g_proxy_min;
extern int g_di_patch;
extern int g_hitch_wait;
extern int g_terrain_trace;
extern int g_force_res;
extern char g_game_root[MAX_PATH];

int env_is(const char *name, const char *want);
int env_on(const char *name, int def);
void resolve_game_root(void);
void path_join(char *out, size_t out_n, const char *leaf);
void dump_bmp24(const char *path, int w, int h, int src_bpp, int src_stride, const BYTE *bits,
                int bottom_up);
int path_is_safe(const char *p);
void path_upper_copy(char *dst, size_t n, const char *src);
void json_escape(char *dst, size_t n, const char *src);
void hooks_agent(const char *hid, const char *loc, const char *msg, const char *data_json);
/* 1 if CK_DEBUG_FULL — noisy per-frame agent fopen logs. */
int hooks_debug_full(void);
int mem_eq(const void *a, const void *b, SIZE_T n);
void *scan_sig(HMODULE mod, const BYTE *sig, SIZE_T siglen);
int looks_like_tpw(void);
int looks_like_celtic_kings(void);

/* ---- scanline ---- */
extern int g_scan_soft_max;
extern volatile LONG g_scan_clamps;
void install_scanline_oob_guards(void);

/* ---- vfs map crumbs (crash ctx / terrain decor) ---- */
extern char g_ck_phase[192];
extern char g_last_scenario[260];
extern volatile LONG g_deep_attach;
extern volatile LONG g_map_epoch;
extern volatile LONG g_ck_map_editor; /* 1 after AttachTerrain-editor, 0 after -game */
extern void *g_last_attach_map;
extern void *g_last_decor_dirg; /* last LoadDirg64_16 TERRAIN.DECOR self */
extern unsigned g_last_map_f4, g_last_map_f8;

/* ---- player_color helpers (VFS may call) ---- */
extern volatile LONG g_pc_vfs_logs;
int path_mentions_rome(const char *path);
void rome_arm_focus(const char *path);
void log_rome_vfs(const char *api, const char *path, unsigned mode, int ok);

/* ---- minimap exports for zoom ---- */
void mm_read_mgr(unsigned *a8, unsigned *ac, unsigned *b0, unsigned *b4);
void mm_read_cam(LONG *L, LONG *T, LONG *R, LONG *B);

/* ---- domain install/remove (order fixed by hooks.c) ---- */
void hooks_vfs_install(void);
void hooks_vfs_remove_iat(void);
void hooks_vfs_remove(void);
void hooks_hitch_install(void);
/* Re-bind Wait/Sleep IAT on late-loaded dinput/winmm (call from present path). */
void hooks_hitch_rebind_modules(void);
/* Take+reset main-thread sleep/GetTickCount counters since last present (H-GAP). */
void hooks_hitch_take_gap_stats(double *sleep_ms, long *sleep_n, long *gtc_n);
void hooks_hitch_dump_gtc_callers(char *out, size_t out_n);
void hooks_hitch_remove(void);
void hooks_video_install(void);
void hooks_video_movies_install(void);
void hooks_video_remove(void);
void hooks_terrain_install(void);
void hooks_terrain_remove(void);
void hooks_minimap_install(void);
void hooks_minimap_remove(void);
void hooks_zoom_install(void);
void hooks_zoom_remove(void);
void hooks_cam_smooth_install(void);
void hooks_cam_smooth_remove(void);
void cam_smooth_on_frame(void);   /* legacy no-op; pan is present-driven */
void cam_smooth_on_present(void); /* call once before vk present_fb (playfield scroll) */

void hooks_obj_install(void);
void hooks_obj_remove(void);
/* TEMP CK_SOFT_OBJ=1: retail MapObj over GPU terrain (scan civ/animal motion). */
int ck_soft_obj_enabled(void);
/* Crow/Eagle/… — retail CreateVisible even when GPU-first (scan fly clips). */
int ck_soft_bird_scan(const char *id);
void obj_soft_note_rle(int x, int y, void *owner);

/* Replace retail main menu with in-process MainMenu linked into CK.asi (CK_NATIVE_MENU). */
void hooks_native_menu_maybe(void);

#endif
