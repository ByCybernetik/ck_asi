#include "dm_replace_internal.h"

int g_enabled;

void *g_perf_vt[PERF_VT];
void *g_ldr_vt[LDR_VT];
void *g_seg_vt[SEG_VT];
void *g_path_vt[PATH_VT];
void *g_state_vt[STATE_VT];
int g_vt_ready;
volatile LONG g_play_n;
volatile LONG g_get_n;
volatile DWORD g_path_id;
CkPerf *g_active_perf;

CkLiveBuf s_live[64];
int s_live_n;

volatile LONG g_last_onscreen_sx;
volatile LONG g_last_onscreen_sy;
volatile LONG g_last_onscreen_vt;
volatile LONG g_last_onscreen_vb;
volatile DWORD g_last_onscreen_ms;
volatile int g_last_onscreen_sy_ok;

CRITICAL_SECTION g_live_cs;
volatile LONG g_live_cs_ok;
