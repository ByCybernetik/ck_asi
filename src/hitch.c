#include "hitch.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static LARGE_INTEGER g_qpf;
static LONGLONG g_last_present_qpc;
static char g_mark[96];
static LONGLONG g_mark_qpc;
static CRITICAL_SECTION g_cs;
static int g_ready;
static double g_cam_ms;
static DWORD g_main_tid;
static double g_pump_ms, g_terr_ms, g_decor_ms, g_obj_ms, g_ov_ms;
static double g_obj_lc, g_obj_build, g_obj_sort, g_obj_vert, g_obj_fb;
static int g_obj_quads, g_obj_vis, g_obj_inst;

void hitch_set_main_tid(DWORD tid)
{
    if (tid)
        g_main_tid = tid;
}

DWORD hitch_main_tid(void)
{
    return g_main_tid;
}

double hitch_ms_since_last_present(void)
{
    if (!g_ready || g_last_present_qpc <= 0)
        return 0.0;
    return hitch_qpc_ms_since(g_last_present_qpc);
}

LONGLONG hitch_qpc_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double hitch_qpc_ms_since(LONGLONG t0)
{
    LONGLONG now;
    if (!g_ready || g_qpf.QuadPart <= 0 || t0 <= 0)
        return 0.0;
    now = hitch_qpc_now();
    return (double)(now - t0) * 1000.0 / (double)g_qpf.QuadPart;
}

void hitch_init(void)
{
    if (g_ready)
        return;
    InitializeCriticalSection(&g_cs);
    QueryPerformanceFrequency(&g_qpf);
    g_mark[0] = '\0';
    g_mark_qpc = 0;
    g_last_present_qpc = 0;
    g_ready = 1;
}

void hitch_shutdown(void)
{
    if (!g_ready)
        return;
    DeleteCriticalSection(&g_cs);
    g_ready = 0;
}

void hitch_mark(const char *tag)
{
    DWORD tid;
    if (!g_ready || !tag)
        return;
    /* Ignore marks from audio/DI worker threads — they falsely blame Wait1 on present spikes. */
    tid = GetCurrentThreadId();
    if (g_main_tid && tid != g_main_tid)
        return;
    EnterCriticalSection(&g_cs);
    strncpy(g_mark, tag, sizeof(g_mark) - 1);
    g_mark[sizeof(g_mark) - 1] = '\0';
    g_mark_qpc = hitch_qpc_now();
    LeaveCriticalSection(&g_cs);
}

void hitch_get_mark(char *out, size_t n)
{
    if (!out || n == 0)
        return;
    out[0] = '\0';
    if (!g_ready)
        return;
    EnterCriticalSection(&g_cs);
    strncpy(out, g_mark, n - 1);
    out[n - 1] = '\0';
    LeaveCriticalSection(&g_cs);
}

void hitch_set_cam_ms(double cam_ms)
{
    g_cam_ms = cam_ms;
}

void hitch_set_submit_phases(double pump_ms, double terr_ms, double decor_ms, double obj_ms,
                             double ov_ms)
{
    g_pump_ms = pump_ms;
    g_terr_ms = terr_ms;
    g_decor_ms = decor_ms;
    g_obj_ms = obj_ms;
    g_ov_ms = ov_ms;
}

void hitch_set_obj_phases(double ms_lc, double ms_build, double ms_sort, double ms_vert,
                          double ms_fb, int quads, int vis, int inst)
{
    g_obj_lc = ms_lc;
    g_obj_build = ms_build;
    g_obj_sort = ms_sort;
    g_obj_vert = ms_vert;
    g_obj_fb = ms_fb;
    g_obj_quads = quads;
    g_obj_vis = vis;
    g_obj_inst = inst;
}

void hitch_note_ms(const char *hypothesisId, const char *location, const char *message,
                   const char *tag, double dt_ms, const char *extra_json)
{
    FILE *df;
    char mark[96];
    DWORD now;
    static int s_full = -1;

    if (!hypothesisId || !location || !message)
        return;
    if (s_full < 0) {
        char b[8];
        DWORD n = GetEnvironmentVariableA("CK_DEBUG_FULL", b, sizeof(b));
        s_full = (n > 0 && n < sizeof(b) && (b[0] == '1' || b[0] == 'y' || b[0] == 'Y')) ? 1 : 0;
    }
    /* Quiet default: skip Wait/Sleep spam unless long or wait-main/sleep-main. */
    if (!s_full) {
        int keep = 0;
        if (dt_ms >= 40.0)
            keep = 1;
        else if (message && (!strcmp(message, "wait-main") || !strcmp(message, "sleep-main")) &&
                 dt_ms >= 16.0)
            keep = 1;
        if (!keep) {
            if (tag && tag[0] && dt_ms >= 16.0)
                hitch_mark(tag);
            return;
        }
    }
    /* #region agent log */
    hitch_get_mark(mark, sizeof(mark));
    now = GetTickCount();
    df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
    if (df) {
        fprintf(df,
                "{\"sessionId\":\"764ba7\",\"runId\":\"hitch-src\","
                "\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\","
                "\"data\":{\"tag\":\"%s\",\"dt_ms\":%.3f,\"mark\":\"%.60s\","
                "\"extra\":%s},\"timestamp\":%lu}\n",
                hypothesisId, location, message, tag ? tag : "", dt_ms, mark[0] ? mark : "-",
                (extra_json && extra_json[0]) ? extra_json : "{}", (unsigned long)now);
        fclose(df);
    }
    /* #endregion */
    if (tag && tag[0] && dt_ms >= 16.0)
        hitch_mark(tag);
}

void hitch_present_sample(double gap_ms, double frame_ms, double blit_ms, double extend_ms,
                          double fence_ms, double compose_ms, double acquire_ms, double submit_ms,
                          int pr, int fullish, int blit_w, int blit_h, int gpu_busy,
                          int present_mode, int soft_w, int soft_h, int swap_w, int swap_h,
                          double pace_ms)
{
    static LONG s_n;
    static LONGLONG s_last_budget_qpc;
    LONG n;
    double pipe_ms, other_ms, fps, cmd_rest;
    double top_ms;
    const char *top;
    int emit;
    FILE *df;
    LONGLONG nowq;

    n = InterlockedIncrement(&s_n);
    if (frame_ms < gap_ms)
        frame_ms = gap_ms;
    pipe_ms = fence_ms + compose_ms + acquire_ms + submit_ms;
    other_ms = frame_ms - pipe_ms - g_cam_ms;
    if (other_ms < 0.0)
        other_ms = 0.0;
    fps = frame_ms > 0.1 ? 1000.0 / frame_ms : 0.0;
    cmd_rest = submit_ms - g_pump_ms - g_terr_ms - g_decor_ms - g_obj_ms - g_ov_ms;
    if (cmd_rest < 0.0)
        cmd_rest = 0.0;

    /* Pick dominant bucket for this frame. */
    top = "other";
    top_ms = other_ms;
#define CK_TOP(name, v)                                                                            \
    do {                                                                                           \
        if ((v) > top_ms) {                                                                        \
            top_ms = (v);                                                                          \
            top = (name);                                                                          \
        }                                                                                          \
    } while (0)
    CK_TOP("fence", fence_ms);
    CK_TOP("compose", compose_ms);
    CK_TOP("acquire", acquire_ms);
    CK_TOP("pump", g_pump_ms);
    CK_TOP("terrain", g_terr_ms);
    CK_TOP("decor", g_decor_ms);
    CK_TOP("obj", g_obj_ms);
    CK_TOP("overlay", g_ov_ms);
    CK_TOP("cmd_rest", cmd_rest);
    CK_TOP("cam", g_cam_ms);
    CK_TOP("blit", blit_ms);
    CK_TOP("pace", pace_ms);
#undef CK_TOP

    nowq = hitch_qpc_now();
    emit = 0;
    if (n <= 8)
        emit = 1;
    else if (frame_ms >= 25.0 || submit_ms >= 8.0 || g_cam_ms >= 8.0)
        emit = 1;
    else if (!s_last_budget_qpc || hitch_qpc_ms_since(s_last_budget_qpc) >= 250.0)
        emit = 1;

    /* #region agent log */
    if (emit) {
        char mark[96];
        hitch_get_mark(mark, sizeof(mark));
        s_last_budget_qpc = nowq;
        df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
        if (df) {
            fprintf(df,
                    "{\"sessionId\":\"764ba7\",\"runId\":\"cam-light-2\",\"hypothesisId\":\"H-CAM\","
                    "\"location\":\"hitch.c:present_sample\",\"message\":\"fps-budget\","
                    "\"data\":{\"n\":%ld,\"gap_ms\":%.3f,\"frame_ms\":%.3f,\"fps\":%.1f,"
                    "\"top\":\"%s\",\"top_ms\":%.3f,\"other_ms\":%.3f,\"fence_ms\":%.3f,"
                    "\"compose_ms\":%.3f,\"acquire_ms\":%.3f,\"submit_ms\":%.3f,\"pump_ms\":%.3f,"
                    "\"terr_ms\":%.3f,\"decor_ms\":%.3f,\"obj_ms\":%.3f,\"ov_ms\":%.3f,"
                    "\"cmd_rest_ms\":%.3f,\"cam_ms\":%.3f,\"blit_ms\":%.3f,\"pace_ms\":%.3f,"
                    "\"obj_lc\":%.3f,\"obj_build\":%.3f,\"obj_sort\":%.3f,\"obj_vert\":%.3f,"
                    "\"obj_fb\":%.3f,\"obj_quads\":%d,\"obj_vis\":%d,\"obj_inst\":%d,"
                    "\"pr\":%d,\"pm\":%d,\"soft\":[%d,%d],\"mark\":\"%.40s\"},\"timestamp\":%lu}\n",
                    (long)n, gap_ms, frame_ms, fps, top, top_ms, other_ms, fence_ms, compose_ms,
                    acquire_ms, submit_ms, g_pump_ms, g_terr_ms, g_decor_ms, g_obj_ms, g_ov_ms,
                    cmd_rest, g_cam_ms, blit_ms, pace_ms, g_obj_lc, g_obj_build, g_obj_sort,
                    g_obj_vert, g_obj_fb, g_obj_quads, g_obj_vis, g_obj_inst, pr, present_mode,
                    soft_w, soft_h, mark[0] ? mark : "-", (unsigned long)(nowq / 10000));
            fclose(df);
        }
        if (n <= 8 || frame_ms >= 40.0 || submit_ms >= 12.0) {
            df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
            if (df) {
                fprintf(df,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"cam-light-2\",\"hypothesisId\":\"H-CAM\","
                        "\"location\":\"hitch.c:present_sample\",\"message\":\"present-timing\","
                        "\"data\":{\"n\":%ld,\"frame_ms\":%.3f,\"fps\":%.1f,\"top\":\"%s\","
                        "\"top_ms\":%.3f,\"cam_ms\":%.3f,\"spike\":1},\"timestamp\":%lu}\n",
                        (long)n, frame_ms, fps, top, top_ms, g_cam_ms,
                        (unsigned long)(nowq / 10000));
                fclose(df);
            }
        }
        g_cam_ms = 0.0;
    }
    (void)extend_ms;
    (void)fullish;
    (void)blit_w;
    (void)blit_h;
    (void)gpu_busy;
    (void)swap_w;
    (void)swap_h;
    /* #endregion */

    if (g_ready)
        g_last_present_qpc = hitch_qpc_now();
}
