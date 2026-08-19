#ifndef CK_DM_REPLACE_INTERNAL_H
#define CK_DM_REPLACE_INTERNAL_H

/*
 * Shared types/APIs for dm_replace_*.c modules.
 * Public surface remains dm_replace.h.
 */

#include "dm_replace.h"
#include "hooks.h"
#include "hitch.h"
#include "log.h"
#include "vk_present.h"

#include <dsound.h>
#include <mmreg.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum {
    OGG_DECODE_FULL = -1
};

enum {
    PERF_VT = 56,
    LDR_VT = 16,
    SEG_VT = 32,
    PATH_VT = 16,
    STATE_VT = 10,
    PERF_IDX_STOP = 5,
    PERF_IDX_SETNOTIF = 20,
    PERF_IDX_GETNOTIF = 21,
    PERF_IDX_ADDNOTIF = 22,
    PERF_IDX_FREEPMSG = 17,
    PERF_IDX_SETGLOBAL = 34,
    PERF_IDX_CLOSEDOWN = 38,
    PERF_IDX_INITAUDIO = 44,
    PERF_IDX_PLAYSEGEX = 45,
    PERF_IDX_STOPEX = 46,
    PERF_IDX_CREATESTD = 49,
    LDR_IDX_GETOBJECT = 3,
    LDR_IDX_SETOBJECT = 4,
    LDR_IDX_SETSEARCH = 5,
    LDR_IDX_SCANDIR = 6,
    LDR_IDX_CACHEOBJ = 7,
    LDR_IDX_RELEASEOBJ = 8,
    LDR_IDX_CLEARCACHE = 9,
    LDR_IDX_ENABLECACHE = 10,
    LDR_IDX_COLLECTGARBAGE = 11,
    SEG_IDX_DOWNLOAD = 29,
    SEG_IDX_UNLOAD = 30
};

enum {
    DMUS_OBJ_CLASS = 0x001,
    DMUS_OBJ_STREAM = 0x800
};

#define CK_DMUS_PMSGT_NOTIFICATION 3u
#define CK_DMUS_NOTIFICATION_SEGEND 1u
#define CK_PLAY_FADE_TRANSITION 0x40000000u
#define CK_NOTIF_SLOTS 128

#define CK_CAM_PTR_VA 0x007CA604u
#define CK_CAM_LEFT_OFF 0x4C8u
#define CK_CAM_TOP_OFF 0x4CCu
#define CK_CAM_RIGHT_OFF 0x4D0u
#define CK_CAM_BOTTOM_OFF 0x4D4u
#define CK_XFORM_RECT_VA 0x0047E4A0u
#define CK_YATTEN_MAX 10000
#define CK_DATTEN_MAX 4500

extern const GUID CLSID_DMPerformance;
extern const GUID CLSID_DMLoader;
extern const GUID CLSID_DMSegment;
extern const GUID IID_IDirectMusicPerformance8;
extern const GUID IID_IDirectMusicPerformance;
extern const GUID IID_IDirectMusicPerformance2;
extern const GUID IID_IDirectMusicLoader8;
extern const GUID IID_IDirectMusicLoader;
extern const GUID IID_IDirectMusicSegment8;
extern const GUID IID_IDirectMusicSegmentState8;
extern const GUID IID_IDirectMusicAudioPath8;
extern const GUID CK_GUID_NOTIFICATION_SEGMENT;

typedef struct CkSegmentTag {
    void **lpVtbl;
    LONG refs;
    BYTE *pcm;
    DWORD pcm_bytes;
    WAVEFORMATEX fmt;
    void *last_path;
    DWORD repeats;
    int pcm_cached;
    int unloaded;
    LONG destroying;
    struct CkSegmentTag *registry_next;
    char path[260];
} CkSegment;

typedef struct {
    void **lpVtbl;
    void *self;
    LONG refs;
    DWORD id;
    LPDIRECTSOUNDBUFFER ctrl;
    void *ctrl_proxy;
    LONG vol;
    LONG pan;
    LONG volume_generation;
    int active;
} CkPath;

typedef struct {
    void **lpVtbl;
    LONG refs;
    CkPath *path;
    LPDIRECTSOUNDBUFFER real;
} CkDsCtrl;

typedef struct CkState CkState;
struct CkState {
    void **lpVtbl;
    LONG refs;
    CkSegment *seg;
    DWORD repeats;
};

#pragma pack(push, 4)
typedef struct {
    DWORD dwSize;
    LONGLONG rtTime;
    LONGLONG mtTime;
    DWORD dwFlags;
    DWORD dwPChannel;
    DWORD dwVirtualTrackID;
    void *pTool;
    void *pGraph;
    DWORD dwType;
    DWORD dwVoiceID;
    DWORD dwGroupID;
    void *punkUser;
    GUID guidNotificationType;
    DWORD dwNotificationOption;
    DWORD dwField1;
    DWORD dwField2;
} CkNotifMsg;
#pragma pack(pop)

typedef struct {
    unsigned char phase;
    DWORD fire_ms;
    CkNotifMsg msg;
    CkState *state;
} CkNotifSlot;

typedef struct {
    void **lpVtbl;
    LONG refs;
    LPDIRECTSOUND ds;
    LPDIRECTSOUNDBUFFER primary;
    LPDIRECTSOUNDBUFFER music_buf;
    CkSegment *music_seg;
    DWORD music_path_id;
    DWORD clock_start_ms;
    HWND hwnd;
    CRITICAL_SECTION lock;
    HANDLE notif_event;
    int notif_segment;
    CkNotifSlot notif[CK_NOTIF_SLOTS];
} CkPerf;

typedef struct {
    void **lpVtbl;
    LONG refs;
} CkLoader;

typedef struct {
    LPDIRECTSOUNDBUFFER b;
    LPDIRECTSOUNDBUFFER bR;
    DWORD done_tick;
    DWORD path_id;
    CkSegment *seg;
    int spatial;
    int looping;
    int y_ok;
    int xy_ok;
    LONG sx, sy;
    LONG ref_span;
    LONG last_pan;
    LONG last_yatten;
    LONG base_vol;
} CkLiveBuf;

/* ---- globals owned across modules ---- */
extern int g_enabled;
extern void *g_perf_vt[PERF_VT];
extern void *g_ldr_vt[LDR_VT];
extern void *g_seg_vt[SEG_VT];
extern void *g_path_vt[PATH_VT];
extern void *g_state_vt[STATE_VT];
extern int g_vt_ready;
extern volatile LONG g_play_n;
extern volatile LONG g_get_n;
extern volatile DWORD g_path_id;
extern CkPerf *g_active_perf;
extern CkLiveBuf s_live[64];
extern int s_live_n;
extern CRITICAL_SECTION g_live_cs;
extern volatile LONG g_live_cs_ok;

extern volatile LONG g_last_onscreen_sx;
extern volatile LONG g_last_onscreen_sy;
extern volatile LONG g_last_onscreen_vt;
extern volatile LONG g_last_onscreen_vb;
extern volatile DWORD g_last_onscreen_ms;
extern volatile int g_last_onscreen_sy_ok;

/* ---- debug ---- */
void dm_agent(const char *hid, const char *loc, const char *msg, const char *data_json);
void ck_ring_push(int kind, int slot);
void ck_ring_dump(const char *why);
LONG CALLBACK ck_veh(struct _EXCEPTION_POINTERS *ep);
HMODULE dm_pin_module(const void *address);
void dm_worker_exit(HMODULE module, DWORD code);

/* ---- pcm / path classifiers ---- */
LONG path_clamp_vol(LONG vol);
LONG path_clamp_pan(LONG pan);
void path_apply_buf_vol(LPDIRECTSOUNDBUFFER buf, LONG vol);
void path_apply_buf_pan(LPDIRECTSOUNDBUFFER buf, LONG pan);
void pan_gains_q15(LONG pan, int *lg, int *rg);
LONG q15_to_dsvol(int g);
void stereo_wfx_from(const WAVEFORMATEX *src, WAVEFORMATEX *dst);
int ds_buf_write_all(LPDIRECTSOUNDBUFFER buf, const BYTE *data, DWORD bytes);
DWORD pcm_render_panned_stereo(const BYTE *src, const WAVEFORMATEX *fmt, DWORD src_bytes,
                               BYTE *dst, DWORD dst_cap, LONG pan);
DWORD pcm_render_one_channel(const BYTE *src, const WAVEFORMATEX *fmt, DWORD src_bytes,
                             BYTE *dst, DWORD dst_cap, int which);
int path_is_music(const char *path);
int path_is_rhastatus(const char *p);
int path_is_unit_voice(const char *p);
int path_is_building_sfx(const char *p);
int path_is_ambient_sfx(const char *p);
int path_is_walk_sfx(const char *p);
int path_is_fight_sfx(const char *p);
int path_is_death_sfx(const char *p);
int path_is_effects_sfx(const char *p);
int path_want_spatial(const char *p);

/* ---- assets / decode ---- */
int decode_to_pcm(const BYTE *raw, DWORD raw_len, WAVEFORMATEX *fmt, BYTE **pcm_out,
                  DWORD *pcm_len, int max_sec);
int cache_get(const char *path, WAVEFORMATEX *fmt, BYTE **pcm, DWORD *pcm_bytes);
void cache_put(const char *path, const WAVEFORMATEX *fmt, const BYTE *pcm, DWORD pcm_bytes);
int music_cache_get(const char *path, WAVEFORMATEX *fmt, BYTE **pcm, DWORD *pcm_bytes);
int music_cache_acquire(const char *path, WAVEFORMATEX *fmt, BYTE **pcm, DWORD *pcm_bytes);
BYTE *music_cache_intern(const char *path, const WAVEFORMATEX *fmt, BYTE *pcm, DWORD pcm_bytes);
BYTE *music_cache_intern_acquire(const char *path, const WAVEFORMATEX *fmt, BYTE *pcm,
                                 DWORD pcm_bytes, int *cached);
void music_cache_release(const char *path, const BYTE *pcm);
void music_cache_collect(void);
void music_preload_start(void);
void music_prefetch_full_async(const char *path);
void music_prefetch_siblings(const char *current_rel);
int music_cache_build_ds_for_path(LPDIRECTSOUND ds, const char *path);
int music_cache_try_acquire_ds_buf(const char *path, LPDIRECTSOUNDBUFFER *out);
void ensure_game_dir(void);
const char *dm_game_dir(void);
int scrape_stream_path(void *stream, char *out, size_t outn);
HRESULT load_wav_file_or_pak(const char *path, BYTE **out, DWORD *out_len);

/* ---- voice / spatial / play ---- */
void install_onscreen_pan_fix(void);
void beacon_stop(void);
void beacon_update_pan(LONG game_pan, DWORD path_id);
void beacon_start_from_seg(CkPerf *perf, CkSegment *seg, CkPath *apath);
int spatial_voice_play(CkPerf *perf, CkSegment *seg, CkPath *apath, int loop);
void spatial_apply_live(CkLiveBuf *lb, int force);
void path_propagate_volume(CkPath *path);
void path_propagate_pan(CkPath *path);
void live_apply_softpan(CkLiveBuf *lb, LONG pan);
void stop_music_buf(void);
void stop_live_for_segment(CkSegment *seg);
void stop_all_live_sfx(void);
void path_stop_live_sfx(DWORD path_id);
void live_cs_enter(void);
void live_cs_leave(void);
void live_free_slot(int i);
int segment_is_playing(CkSegment *seg);
HRESULT play_pcm(CkPerf *perf, CkSegment *seg, CkPath *apath, DWORD flags);

/* ---- COM ---- */
void init_vtables(void);
void notif_schedule_segend(CkPerf *perf, CkState *st, DWORD dur_ms);
void notif_tick(CkPerf *perf);
void state_init(CkState *st, CkSegment *seg, DWORD repeats);
ULONG ck_segment_addref(CkSegment *seg);
ULONG ck_segment_release(CkSegment *seg);
ULONG ck_perf_addref(CkPerf *perf);
ULONG ck_perf_release(CkPerf *perf);
void dm_com_collect_all(void);

#endif /* CK_DM_REPLACE_INTERNAL_H */
