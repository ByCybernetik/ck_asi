#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include "hitch.h"
#include <stdio.h>
#include <string.h>

typedef void(WINAPI *PFN_Sleep)(DWORD);
typedef DWORD(WINAPI *PFN_SleepEx)(DWORD, BOOL);
typedef DWORD(WINAPI *PFN_WaitForSingleObject)(HANDLE, DWORD);
typedef DWORD(WINAPI *PFN_WaitForSingleObjectEx)(HANDLE, DWORD, BOOL);
typedef DWORD(WINAPI *PFN_WaitForMultipleObjects)(DWORD, const HANDLE *, BOOL, DWORD);
typedef DWORD(WINAPI *PFN_MsgWaitForMultipleObjects)(DWORD, const HANDLE *, BOOL, DWORD, DWORD);
typedef BOOL(WINAPI *PFN_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL(WINAPI *PFN_GetMessageA)(LPMSG, HWND, UINT, UINT);
typedef void(WINAPI *PFN_WaitMessage)(void);
typedef void(WINAPI *PFN_EnterCriticalSection)(LPCRITICAL_SECTION);
typedef void(WINAPI *PFN_LeaveCriticalSection)(LPCRITICAL_SECTION);
typedef DWORD(WINAPI *PFN_GetTickCount)(void);

/* tpw custom timer: thiscall sched(this, a0, flags, b, c, delay_ms) @ 0x406F40 */
typedef void(__attribute__((thiscall)) *PFN_TimerSched)(void *This, void *a0, DWORD flags, DWORD b,
                                                         DWORD c, DWORD delay_ms);

static PFN_Sleep real_Sleep;
static void *g_iat_Sleep;
static PFN_SleepEx real_SleepEx;
static void *g_iat_SleepEx;
static PFN_WaitForSingleObject real_WaitForSingleObject;
static PFN_WaitForSingleObjectEx real_WaitForSingleObjectEx;
static void *g_iat_WaitEx;
static PFN_WaitForMultipleObjects real_WaitForMultipleObjects;
static PFN_MsgWaitForMultipleObjects real_MsgWaitForMultipleObjects;
static PFN_PeekMessageA real_PeekMessageA;
static void *g_iat_PeekMessageA;
static PFN_GetMessageA real_GetMessageA;
static void *g_iat_GetMessageA;
static PFN_WaitMessage real_WaitMessage;
static void *g_iat_WaitMessage;
static PFN_EnterCriticalSection real_EnterCriticalSection;
static void *g_iat_EnterCS;
static PFN_LeaveCriticalSection real_LeaveCriticalSection;
static void *g_iat_LeaveCS;

/* Inline hotpatch on kernel32 stubs — catches GetProcAddress callers (IAT-invisible). */
static void *g_k32_wait1_tgt;
static BYTE *g_k32_wait1_tramp;
static BYTE g_k32_wait1_saved[16];
static void *g_k32_waitx_tgt;
static BYTE *g_k32_waitx_tramp;
static BYTE g_k32_waitx_saved[16];
static void *g_k32_sleep_tgt;
static BYTE *g_k32_sleep_tramp;
static BYTE g_k32_sleep_saved[16];
enum { CK_K32_STEAL = 5 };

/* Multi-module IAT slots (game + dinput/…); restore all on remove. */
enum { CK_WAIT_SLOTS = 24 };
static void *g_wait1_slots[CK_WAIT_SLOTS];
static int g_wait1_n;
static void *g_waitn_slots[CK_WAIT_SLOTS];
static int g_waitn_n;
static void *g_msgwait_slots[CK_WAIT_SLOTS];
static int g_msgwait_n;
static void *g_sleep_slots[CK_WAIT_SLOTS];
static int g_sleep_n;

static void ra_mod_name(void *ra, char *out, size_t n)
{
    HMODULE hm = NULL;
    char path[MAX_PATH];
    const char *base;
    out[0] = '?';
    out[1] = '\0';
    if (!ra || n < 2)
        return;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)ra, &hm) ||
        !hm)
        return;
    if (!GetModuleFileNameA(hm, path, MAX_PATH))
        return;
    base = path;
    {
        const char *p;
        for (p = path; *p; ++p) {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }
    }
    strncpy(out, base, n - 1);
    out[n - 1] = '\0';
}

static int iat_push(void **slots, int *n, void *slot)
{
    int i;
    if (!slot || !n || *n >= CK_WAIT_SLOTS)
        return 0;
    for (i = 0; i < *n; ++i) {
        if (slots[i] == slot)
            return 0;
    }
    slots[(*n)++] = slot;
    return 1;
}

static int patch_k32_on(HMODULE mod, const char *mod_tag, const char *func, void *hook,
                        void **real_fn, void **slots, int *nslots)
{
    void *slot = NULL;
    void *orig;
    if (!mod || !real_fn || !*real_fn)
        return 0;
    orig = *real_fn;
    if (!patch_iat_entry(mod, "KERNEL32.dll", func, hook, &orig, &slot))
        return 0;
    if (iat_push(slots, nslots, slot))
        log_msg("IAT hooked %s!%s (%s)", "kernel32", func, mod_tag ? mod_tag : "?");
    return 1;
}

/* Observe Sleep; accumulate main-thread sleep (incl. ms<16 pacing loops). */
static double g_main_sleep_acc_ms;
static LONG g_main_sleep_n;
static LONG g_main_gtc_n;
typedef struct {
    ULONG_PTR ra;
    LONG n;
} CkGtcCaller;
static CkGtcCaller g_gtc_callers[32];
static LONG g_gtc_callers_n;
static PFN_GetTickCount real_GetTickCount;
static void *g_k32_gtc_tgt;
static BYTE *g_k32_gtc_tramp;
static BYTE g_k32_gtc_saved[16];

/* H-100: clamp long custom-timer delays (push 0x64 then busy-wait on obj+0xc). */
enum { CK_TIMER_SCHED = 0x00406F40u, CK_TIMER_STEAL = 7 };
static void *g_tmr_tgt;
static BYTE *g_tmr_tramp;
static BYTE g_tmr_saved[16];
static PFN_TimerSched real_TimerSched;
static volatile LONG g_tmr_clamp_n;

void hooks_hitch_take_gap_stats(double *sleep_ms, long *sleep_n, long *gtc_n)
{
    if (sleep_ms)
        *sleep_ms = g_main_sleep_acc_ms;
    if (sleep_n)
        *sleep_n = (long)g_main_sleep_n;
    if (gtc_n)
        *gtc_n = (long)g_main_gtc_n;
    g_main_sleep_acc_ms = 0.0;
    g_main_sleep_n = 0;
    g_main_gtc_n = 0;
}

void hooks_hitch_dump_gtc_callers(char *out, size_t out_n)
{
    LONG i, j;
    size_t used = 0;
    if (!out || out_n == 0)
        return;
    out[0] = '\0';
    for (i = 0; i < g_gtc_callers_n; ++i) {
        for (j = i + 1; j < g_gtc_callers_n; ++j) {
            if (g_gtc_callers[j].n > g_gtc_callers[i].n) {
                CkGtcCaller t = g_gtc_callers[i];
                g_gtc_callers[i] = g_gtc_callers[j];
                g_gtc_callers[j] = t;
            }
        }
    }
    for (i = 0; i < g_gtc_callers_n && i < 8; ++i) {
        int n = snprintf(out + used, out_n - used, "%s{\"ra\":\"0x%lX\",\"n\":%ld}",
                         i ? "," : "", (unsigned long)g_gtc_callers[i].ra,
                         (long)g_gtc_callers[i].n);
        if (n <= 0 || (size_t)n >= out_n - used)
            break;
        used += (size_t)n;
    }
    g_gtc_callers_n = 0;
    memset(g_gtc_callers, 0, sizeof(g_gtc_callers));
}

/* Hot GetTickCount return sites from freeze logs (tpw.exe). */
static int gtc_ra_is_hot(ULONG_PTR ra)
{
    return ra == 0x005924E2u || ra == 0x00581762u || ra == 0x00581FEFu || ra == 0x006F2BACu ||
           ra == 0x00426DFBu || ra == 0x00427031u || ra == 0x00406FA5u || ra == 0x0040702Eu;
}

/* Cam/timer cluster (0x9BFA38 GetTime) — not the 0x426DFB load path. */
static int gtc_ra_is_cam_hot(ULONG_PTR ra)
{
    return ra == 0x005924E2u || ra == 0x00581762u || ra == 0x00581FEFu || ra == 0x006F2BACu;
}

/*
 * H-BOOST / H-BOOST2 REJECTED:
 * - sticky +100/64 → bias millions + livelock
 * - floor one-shot → GetTickCount stuck ahead of real → cam dib-gap ~145ms
 *   (busy-wait until wall clock catches floor). Do not fake time.
 */

/* Aggregate hot-RA busy bursts (game interleaves several RAs in one wait loop). */
static ULONG_PTR s_spin_top_ra;
static LONGLONG s_spin_t0;
static LONG s_spin_n;
static DWORD s_spin_tick0;
static LONGLONG s_last_hot_qpc;
static ULONG_PTR s_spin_ras[6];
static LONG s_spin_rn[6];

static void gtc_spin_reset(void)
{
    int i;
    s_spin_top_ra = 0;
    s_spin_t0 = 0;
    s_spin_n = 0;
    s_spin_tick0 = 0;
    for (i = 0; i < 6; ++i) {
        s_spin_ras[i] = 0;
        s_spin_rn[i] = 0;
    }
}

/* #region agent log */
/* Walk EBP chain for place-specific busy-wait (H-LOC). */
static void gtc_stack_snap(char *out, size_t out_n)
{
    ULONG_PTR frames[4];
    ULONG_PTR *bp = 0;
    int i, n = 0;
    size_t used = 0;
    if (!out || out_n == 0)
        return;
    out[0] = '\0';
#if defined(__i386__) || defined(_M_IX86)
    __asm__ volatile("mov %%ebp, %0" : "=r"(bp));
#endif
    for (i = 0; i < 4 && bp && (ULONG_PTR)bp > 0x10000u && !IsBadReadPtr(bp, 8); ++i) {
        frames[n++] = bp[1];
        bp = (ULONG_PTR *)bp[0];
    }
    for (i = 0; i < n; ++i) {
        int w = snprintf(out + used, out_n - used, "%s\"0x%lX\"", i ? "," : "",
                         (unsigned long)frames[i]);
        if (w <= 0 || (size_t)w >= out_n - used)
            break;
        used += (size_t)w;
    }
}

static void gtc_cam_snap(char *out, size_t out_n)
{
    LONG L = 0, T = 0, R = 0, B = 0;
    if (!out || out_n == 0)
        return;
    mm_read_cam(&L, &T, &R, &B);
    snprintf(out, out_n, "[%ld,%ld,%ld,%ld]", (long)L, (long)T, (long)R, (long)B);
}
/* #endregion */

static void gtc_spin_note_ra(ULONG_PTR ra)
{
    int i;
    for (i = 0; i < 6; ++i) {
        if (s_spin_ras[i] == ra) {
            s_spin_rn[i]++;
            return;
        }
        if (s_spin_ras[i] == 0) {
            s_spin_ras[i] = ra;
            s_spin_rn[i] = 1;
            return;
        }
    }
}

static void gtc_spin_flush(DWORD tick1)
{
    double ms;
    char js[640];
    char top[160];
    char stk[128];
    char cam[64];
    size_t tu = 0;
    int i;
    static volatile LONG s_spin_logs;
    LONG sn;
    LONG n = s_spin_n;
    LONGLONG t0 = s_spin_t0;
    DWORD tick0 = s_spin_tick0;
    ULONG_PTR top_ra = s_spin_top_ra;
    ULONG_PTR ras[6];
    LONG rn[6];
    if (n < 200 || t0 <= 0) {
        gtc_spin_reset();
        return;
    }
    ms = hitch_qpc_ms_since(t0);
    for (i = 0; i < 6; ++i) {
        ras[i] = s_spin_ras[i];
        rn[i] = s_spin_rn[i];
    }
    /* #region agent log */
    gtc_stack_snap(stk, sizeof(stk));
    gtc_cam_snap(cam, sizeof(cam));
    /* #endregion */
    /* Reset before logging — hooks_agent must not see live spin state. */
    gtc_spin_reset();
    if (ms < 2.0)
        return;
    sn = InterlockedIncrement(&s_spin_logs);
    if (sn > 40 && (sn % 8) != 0 && ms < 40.0)
        return;
    top[0] = 0;
    for (i = 0; i < 6 && ras[i]; ++i) {
        int w = snprintf(top + tu, sizeof(top) - tu, "%s{\"ra\":\"0x%lX\",\"n\":%ld}",
                         i ? "," : "", (unsigned long)ras[i], (long)rn[i]);
        if (w <= 0 || (size_t)w >= sizeof(top) - tu)
            break;
        tu += (size_t)w;
    }
    /* #region agent log */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"ms\":%.3f,\"tick0\":%lu,\"tick1\":%lu,\"dtick\":%ld,\"rate\":%.0f,"
             "\"top_ra\":\"0x%lX\",\"ras\":[%s],\"stack\":[%s],\"cam\":%s}",
             (long)n, ms, (unsigned long)tick0, (unsigned long)tick1, (long)(tick1 - tick0),
             ms > 0.001 ? ((double)n * 1000.0 / ms) : 0.0, (unsigned long)top_ra, top, stk, cam);
    hooks_agent("H-LOC", "hooks_hitch.c:GetTickCount", "gtc-spin", js);
    /* #endregion */
}

static void WINAPI hook_Sleep(DWORD ms)
{
    LONGLONG t0 = hitch_qpc_now();
    double dt;
    DWORD tid = GetCurrentThreadId();
    DWORD mt = hitch_main_tid();
    int on_main = (mt && tid == mt);
    static volatile LONG s_short_n;
    if (ms >= 16 && on_main)
        hitch_mark("Sleep");
    real_Sleep(ms);
    dt = hitch_qpc_ms_since(t0);
    if (on_main) {
        g_main_sleep_acc_ms += dt;
        InterlockedIncrement(&g_main_sleep_n);
    }
    {
        int want = 0;
        if (ms >= 16 || dt >= 16.0)
            want = 1;
        else if (on_main && ms >= 1) {
            LONG n = InterlockedIncrement(&s_short_n);
            if (n <= 40 || (n % 40) == 0 || dt >= 8.0)
                want = 1;
        }
        if (want) {
            char extra[128];
            snprintf(extra, sizeof(extra), "{\"reqMs\":%lu,\"tid\":%lu,\"main\":%d}",
                     (unsigned long)ms, (unsigned long)tid, on_main);
            hitch_note_ms("H-SLP", "hooks.c:Sleep", on_main ? "sleep-main" : "sleep", "Sleep", dt,
                          extra);
        }
    }
}

static DWORD WINAPI hook_GetTickCount(void)
{
    static LONG s_reent;
    DWORD tick = real_GetTickCount();
    DWORD mt;
    if (InterlockedCompareExchange(&s_reent, 1, 0) != 0)
        return tick;
    mt = hitch_main_tid();
    if (mt && GetCurrentThreadId() == mt) {
        void *ra0 = __builtin_return_address(0);
        ULONG_PTR ra = (ULONG_PTR)ra0;
        LONG i;
        LONGLONG now = hitch_qpc_now();
        InterlockedIncrement(&g_main_gtc_n);
        for (i = 0; i < g_gtc_callers_n; ++i) {
            if (g_gtc_callers[i].ra == ra) {
                InterlockedIncrement(&g_gtc_callers[i].n);
                goto spin_track;
            }
        }
        if (g_gtc_callers_n < (LONG)(sizeof(g_gtc_callers) / sizeof(g_gtc_callers[0]))) {
            i = InterlockedIncrement(&g_gtc_callers_n) - 1;
            if (i >= 0 && i < (LONG)(sizeof(g_gtc_callers) / sizeof(g_gtc_callers[0]))) {
                g_gtc_callers[i].ra = ra;
                g_gtc_callers[i].n = 1;
            }
        }
    spin_track:
        if (gtc_ra_is_hot(ra)) {
            double gap_hot = s_last_hot_qpc ? hitch_qpc_ms_since(s_last_hot_qpc) : 999.0;
            if (s_spin_n > 0 && gap_hot > 2.0)
                gtc_spin_flush(tick);
            if (s_spin_n == 0) {
                s_spin_t0 = now;
                s_spin_tick0 = tick;
                s_spin_top_ra = ra;
            }
            s_spin_n++;
            gtc_spin_note_ra(ra);
            s_last_hot_qpc = now;
            /* Log mid only for long non-cam path; no I/O on every short cam spin. */
            if (!gtc_ra_is_cam_hot(ra) &&
                (s_spin_n == 2000 || s_spin_n == 5000 || s_spin_n == 10000)) {
                char js[400];
                char stk[128];
                char cam[64];
                /* #region agent log */
                gtc_stack_snap(stk, sizeof(stk));
                gtc_cam_snap(cam, sizeof(cam));
                snprintf(js, sizeof(js),
                         "{\"ra\":\"0x%lX\",\"n\":%ld,\"ms\":%.3f,\"tick\":%lu,\"phase\":\"mid\","
                         "\"stack\":[%s],\"cam\":%s}",
                         (unsigned long)ra, (long)s_spin_n, hitch_qpc_ms_since(s_spin_t0),
                         (unsigned long)tick, stk, cam);
                hooks_agent("H-LOC", "hooks_hitch.c:GetTickCount", "gtc-spin-mid", js);
                /* #endregion */
            }
        } else if (s_spin_n > 0) {
            double gap_hot = s_last_hot_qpc ? hitch_qpc_ms_since(s_last_hot_qpc) : 999.0;
            if (gap_hot > 2.0)
                gtc_spin_flush(tick);
        }
    }
    InterlockedExchange(&s_reent, 0);
    return tick;
}

/*
 * H-100: game schedules delay then busy-pumps until due (see 0x4605AA push 100).
 * Clamp 50..150ms → 8ms (same idea as CK_DI_PATCH Wait(100)→8).
 */
static void __attribute__((thiscall)) hook_TimerSched(void *This, void *a0, DWORD flags, DWORD b,
                                                       DWORD c, DWORD delay_ms)
{
    DWORD use = delay_ms;
    void *ra = __builtin_return_address(0);
    if (delay_ms >= 50u && delay_ms <= 150u) {
        use = 8;
        InterlockedIncrement(&g_tmr_clamp_n);
        /* #region agent log */
        {
            static volatile LONG s_n;
            LONG n = InterlockedIncrement(&s_n);
            if (n <= 40 || (n % 25) == 0) {
                char js[200];
                snprintf(js, sizeof(js),
                         "{\"n\":%ld,\"delay\":%lu,\"use\":%lu,\"flags\":%lu,\"ra\":\"0x%lX\","
                         "\"clamp_n\":%ld}",
                         (long)n, (unsigned long)delay_ms, (unsigned long)use,
                         (unsigned long)flags, (unsigned long)(ULONG_PTR)ra,
                         (long)g_tmr_clamp_n);
                hooks_agent("H-100", "hooks_hitch.c:TimerSched", "tmr-clamp", js);
            }
        }
        /* #endregion */
    }
    real_TimerSched(This, a0, flags, b, c, use);
}

static DWORD WINAPI hook_SleepEx(DWORD ms, BOOL alertable)
{
    LONGLONG t0 = hitch_qpc_now();
    DWORD r;
    double dt;
    if (ms >= 16)
        hitch_mark("SleepEx");
    r = real_SleepEx(ms, alertable);
    dt = hitch_qpc_ms_since(t0);
    if (ms >= 16 || dt >= 16.0) {
        char extra[96];
        snprintf(extra, sizeof(extra), "{\"reqMs\":%lu,\"alert\":%d}", (unsigned long)ms,
                 (int)alertable);
        hitch_note_ms("H-SLP", "hooks.c:SleepEx", "sleep", "SleepEx", dt, extra);
    }
    return r;
}

static DWORD WINAPI hook_WaitForSingleObject(HANDLE h, DWORD ms)
{
    static volatile LONG s_di_logs;
    static volatile LONG s_x_logs;
    LONGLONG t0;
    DWORD r;
    void *ret = __builtin_return_address(0);
    DWORD ms_use = ms;
    int di_patch = 0;
    ULONG_PTR ra = (ULONG_PTR)ret;
    /* tpw DirectInput pump: Wait(100) — VA band + any Wait(100) under CK_DI_PATCH. */
    int is_di_band = (ms == 100 && ra >= 0x0040C700u && ra <= 0x0040C900u);
    int is_wait100 = (ms == 100);
    double dt;
    char mod[48];

    /*
     * Default ON: stock Wait(100) TIMEOUT freezes keyboard cam (H-WAIT).
     * useMs=8: cuts 100ms stalls without Wait(1) busy-spin (VEH/spin noise).
     * Broaden beyond DI VA band — empty ~90ms gaps had no game-IAT waits logged.
     */
    if (g_di_patch && is_wait100) {
        ms_use = 8;
        di_patch = 1;
    }

    t0 = hitch_qpc_now();
    /* Only mark on main/render thread — audio/DI Wait1 marks poisoned present spikes. */
    if (!di_patch && ms_use >= 16) {
        DWORD mt = hitch_main_tid();
        if (!mt || GetCurrentThreadId() == mt)
            hitch_mark("Wait1");
    }
    r = real_WaitForSingleObject(h, ms_use);
    dt = hitch_qpc_ms_since(t0);
    ra_mod_name(ret, mod, sizeof(mod));
    {
        LONG nlog = 0;
        int want = 0;
        DWORD tid = GetCurrentThreadId();
        DWORD mt = hitch_main_tid();
        int on_main = (mt && tid == mt);
        if (di_patch || (is_di_band && dt >= 16.0))
            nlog = InterlockedIncrement(&s_di_logs);
        if ((!di_patch && (dt >= 16.0 || ms >= 50)) ||
            (di_patch && (nlog <= 12 || (nlog % 200) == 0)) || (di_patch && dt >= 12.0))
            want = 1;
        /* H-WAITX: capture non-DI waits; throttle dsound Wait(50) spam (audio thread). */
        if (!is_di_band && (ms >= 50 || dt >= 20.0)) {
            LONG nx = InterlockedIncrement(&s_x_logs);
            int is_ds = (mod[0] && (strstr(mod, "dsound") || strstr(mod, "DSOUND")));
            if (is_ds && !on_main) {
                if (nx <= 20 || (nx % 80) == 0 || dt >= 40.0)
                    want = 1;
                else
                    want = 0;
            } else if (nx <= 80 || (nx % 20) == 0 || dt >= 40.0) {
                want = 1;
            }
        }
        /* Always keep main-thread waits — H-GAP ~95ms likely Wait(100) off-IAT. */
        if (on_main && (ms >= 16 || dt >= 8.0))
            want = 1;
        if (want) {
            char extra[340];
            snprintf(extra, sizeof(extra),
                     "{\"reqMs\":%lu,\"useMs\":%lu,\"r\":%lu,\"h\":%lu,\"ret\":%lu,"
                     "\"diPatch\":%d,\"isDiBand\":%d,\"mod\":\"%.40s\",\"tid\":%lu,"
                     "\"main\":%d}",
                     (unsigned long)ms, (unsigned long)ms_use, (unsigned long)r,
                     (unsigned long)(ULONG_PTR)h, (unsigned long)ra, di_patch, is_di_band, mod,
                     (unsigned long)tid, on_main);
            hitch_note_ms(on_main ? "H-WAIT-MAIN" : (di_patch ? "H-WAIT-FIX" : "H-WAITX"),
                          "hooks.c:WaitForSingleObject", on_main ? "wait-main" : "wait",
                          di_patch ? "WaitDI" : "Wait1", dt, extra);
        }
    }
    return r;
}

static DWORD WINAPI hook_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    LONGLONG t0 = hitch_qpc_now();
    DWORD r;
    double dt;
    void *ret = __builtin_return_address(0);
    char mod[48];
    DWORD tid = GetCurrentThreadId();
    DWORD mt = hitch_main_tid();
    if (ms >= 16 && (!mt || tid == mt))
        hitch_mark("WaitEx");
    r = real_WaitForSingleObjectEx(h, ms, alertable);
    dt = hitch_qpc_ms_since(t0);
    if ((dt >= 8.0 || ms >= 50) && (!mt || tid == mt)) {
        ra_mod_name(ret, mod, sizeof(mod));
        {
            char extra[240];
            snprintf(extra, sizeof(extra),
                     "{\"reqMs\":%lu,\"r\":%lu,\"alert\":%d,\"ret\":%lu,\"mod\":\"%.40s\","
                     "\"tid\":%lu}",
                     (unsigned long)ms, (unsigned long)r, (int)alertable,
                     (unsigned long)(ULONG_PTR)ret, mod, (unsigned long)tid);
            hitch_note_ms("H-WAITX", "hooks.c:WaitForSingleObjectEx", "wait", "WaitEx", dt, extra);
        }
    }
    return r;
}

static void WINAPI hook_EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    LONGLONG t0;
    double dt;
    DWORD tid = GetCurrentThreadId();
    DWORD mt = hitch_main_tid();
    void *ret = __builtin_return_address(0);
    t0 = hitch_qpc_now();
    real_EnterCriticalSection(cs);
    dt = hitch_qpc_ms_since(t0);
    /* Contended CS blocks via NtWait — invisible to WaitForSingleObject IAT. */
    if (dt >= 8.0 && (!mt || tid == mt)) {
        char mod[48];
        char extra[200];
        ra_mod_name(ret, mod, sizeof(mod));
        snprintf(extra, sizeof(extra),
                 "{\"dt\":%.3f,\"cs\":%lu,\"ret\":%lu,\"mod\":\"%.40s\",\"tid\":%lu}", dt,
                 (unsigned long)(ULONG_PTR)cs, (unsigned long)(ULONG_PTR)ret, mod,
                 (unsigned long)tid);
        hitch_note_ms("H-CS", "hooks.c:EnterCriticalSection", "enter-cs", "EnterCS", dt, extra);
        hitch_mark("EnterCS");
    }
}

static void WINAPI hook_LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    real_LeaveCriticalSection(cs);
}

static DWORD WINAPI hook_WaitForMultipleObjects(DWORD n, const HANDLE *handles, BOOL wait_all,
                                                DWORD ms)
{
    LONGLONG t0;
    DWORD r;
    double dt;
    void *ret = __builtin_return_address(0);
    char mod[48];
    t0 = hitch_qpc_now();
    if (ms >= 16)
        hitch_mark("WaitN");
    r = real_WaitForMultipleObjects(n, handles, wait_all, ms);
    dt = hitch_qpc_ms_since(t0);
    if (dt >= 16.0 || ms >= 50) {
        ra_mod_name(ret, mod, sizeof(mod));
        {
            char extra[192];
            snprintf(extra, sizeof(extra),
                     "{\"n\":%lu,\"all\":%d,\"reqMs\":%lu,\"r\":%lu,\"ret\":%lu,\"mod\":\"%.40s\"}",
                     (unsigned long)n, (int)wait_all, (unsigned long)ms, (unsigned long)r,
                     (unsigned long)(ULONG_PTR)ret, mod);
            hitch_note_ms("H-WAITX", "hooks.c:WaitForMultipleObjects", "wait", "WaitN", dt, extra);
        }
    }
    return r;
}

static DWORD WINAPI hook_MsgWaitForMultipleObjects(DWORD n, const HANDLE *handles, BOOL wait_all,
                                                   DWORD ms, DWORD wake)
{
    LONGLONG t0;
    DWORD r;
    double dt;
    void *ret = __builtin_return_address(0);
    char mod[48];
    t0 = hitch_qpc_now();
    if (ms >= 16)
        hitch_mark("MsgWait");
    r = real_MsgWaitForMultipleObjects(n, handles, wait_all, ms, wake);
    dt = hitch_qpc_ms_since(t0);
    if (dt >= 16.0 || ms >= 50) {
        ra_mod_name(ret, mod, sizeof(mod));
        {
            char extra[220];
            snprintf(extra, sizeof(extra),
                     "{\"n\":%lu,\"all\":%d,\"reqMs\":%lu,\"wake\":%lu,\"r\":%lu,\"ret\":%lu,"
                     "\"mod\":\"%.40s\"}",
                     (unsigned long)n, (int)wait_all, (unsigned long)ms, (unsigned long)wake,
                     (unsigned long)r, (unsigned long)(ULONG_PTR)ret, mod);
            hitch_note_ms("H-WAITX", "hooks.c:MsgWaitForMultipleObjects", "wait", "MsgWait", dt,
                          extra);
        }
    }
    return r;
}

static BOOL WINAPI hook_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    LONGLONG t0 = hitch_qpc_now();
    BOOL r = real_PeekMessageA(msg, hwnd, min, max, remove);
    double dt = hitch_qpc_ms_since(t0);
    if (dt >= 20.0) {
        char extra[160];
        snprintf(extra, sizeof(extra),
                 "{\"dt\":%.3f,\"hwnd\":%lu,\"minmax\":[%u,%u],\"rm\":%u,\"hit\":%d}", dt,
                 (unsigned long)(ULONG_PTR)hwnd, min, max, remove, (int)r);
        hitch_note_ms("H-MSG", "hooks.c:PeekMessageA", "peek-msg", "PeekMsg", dt, extra);
    }
    return r;
}

static BOOL WINAPI hook_GetMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max)
{
    LONGLONG t0 = hitch_qpc_now();
    BOOL r;
    double dt;
    hitch_mark("GetMsg");
    r = real_GetMessageA(msg, hwnd, min, max);
    dt = hitch_qpc_ms_since(t0);
    if (dt >= 16.0) {
        char extra[128];
        snprintf(extra, sizeof(extra), "{\"dt\":%.3f,\"hwnd\":%lu,\"minmax\":[%u,%u],\"r\":%d}", dt,
                 (unsigned long)(ULONG_PTR)hwnd, min, max, (int)r);
        hitch_note_ms("H-MSG", "hooks.c:GetMessageA", "get-msg", "GetMsg", dt, extra);
    }
    return r;
}

static void WINAPI hook_WaitMessage(void)
{
    LONGLONG t0 = hitch_qpc_now();
    double dt;
    hitch_mark("WaitMsg");
    real_WaitMessage();
    dt = hitch_qpc_ms_since(t0);
    if (dt >= 16.0) {
        char extra[64];
        snprintf(extra, sizeof(extra), "{\"dt\":%.3f}", dt);
        hitch_note_ms("H-MSG", "hooks.c:WaitMessage", "wait-msg", "WaitMsg", dt, extra);
    }
}

static void patch_waits_on_module(HMODULE mod, const char *tag)
{
    if (!mod)
        return;
    patch_k32_on(mod, tag, "WaitForSingleObject", (void *)hook_WaitForSingleObject,
                 (void **)&real_WaitForSingleObject, g_wait1_slots, &g_wait1_n);
    patch_k32_on(mod, tag, "WaitForMultipleObjects", (void *)hook_WaitForMultipleObjects,
                 (void **)&real_WaitForMultipleObjects, g_waitn_slots, &g_waitn_n);
    patch_k32_on(mod, tag, "MsgWaitForMultipleObjects", (void *)hook_MsgWaitForMultipleObjects,
                 (void **)&real_MsgWaitForMultipleObjects, g_msgwait_slots, &g_msgwait_n);
    patch_k32_on(mod, tag, "Sleep", (void *)hook_Sleep, (void **)&real_Sleep, g_sleep_slots,
                 &g_sleep_n);
}

void hooks_hitch_rebind_modules(void)
{
    static LONG s_n;
    LONG n = InterlockedIncrement(&s_n);
    const char *di_names[] = {"dinput.dll", "dinput8.dll", "winmm.dll", "dsound.dll", NULL};
    int i;
    int before = g_wait1_n;
    /* Cheap: every 120 presents + first 8. */
    if (n > 8 && (n % 120) != 0)
        return;
    for (i = 0; di_names[i]; ++i)
        patch_waits_on_module(GetModuleHandleA(di_names[i]), di_names[i]);
    if (g_wait1_n != before) {
        log_msg("hitch rebind: wait1 %d→%d", before, g_wait1_n);
        /* #region agent log */
        {
            char js[96];
            snprintf(js, sizeof(js), "{\"wait1\":%d,\"was\":%d,\"n\":%ld}", g_wait1_n, before,
                     (long)n);
            hooks_agent("H-WAITX", "hooks_hitch.c:rebind", "hitch-wait-rebind", js);
        }
        /* #endregion */
    }
}

void hooks_hitch_install(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE user = GetModuleHandleA("user32.dll");
    const char *di_names[] = {"dinput.dll", "dinput8.dll", "winmm.dll", "dsound.dll",
                              "user32.dll", NULL};
    int i;

    if (k32) {
        real_Sleep = (PFN_Sleep)GetProcAddress(k32, "Sleep");
        real_SleepEx = (PFN_SleepEx)GetProcAddress(k32, "SleepEx");
        real_WaitForSingleObject =
            (PFN_WaitForSingleObject)GetProcAddress(k32, "WaitForSingleObject");
        real_WaitForSingleObjectEx =
            (PFN_WaitForSingleObjectEx)GetProcAddress(k32, "WaitForSingleObjectEx");
        real_WaitForMultipleObjects =
            (PFN_WaitForMultipleObjects)GetProcAddress(k32, "WaitForMultipleObjects");
        real_MsgWaitForMultipleObjects =
            (PFN_MsgWaitForMultipleObjects)GetProcAddress(k32, "MsgWaitForMultipleObjects");
        real_EnterCriticalSection =
            (PFN_EnterCriticalSection)GetProcAddress(k32, "EnterCriticalSection");
        real_LeaveCriticalSection =
            (PFN_LeaveCriticalSection)GetProcAddress(k32, "LeaveCriticalSection");
    }
    /*
     * Inline-hook Wine hotpatch stubs (mov edi,edi; push ebp; mov ebp,esp).
     * Catches Wait/Sleep reached via GetProcAddress — IAT-only missed main-thread ~95ms gaps.
     */
    if (real_WaitForSingleObject) {
        g_k32_wait1_tgt = (void *)real_WaitForSingleObject;
        if (install_inline_hook(g_k32_wait1_tgt, (void *)hook_WaitForSingleObject, CK_K32_STEAL,
                                &g_k32_wait1_tramp, g_k32_wait1_saved)) {
            real_WaitForSingleObject = (PFN_WaitForSingleObject)g_k32_wait1_tramp;
            log_msg("inline hooked kernel32!WaitForSingleObject (global)");
        } else
            log_msg("WARN: inline WaitForSingleObject failed");
    }
    if (real_WaitForSingleObjectEx) {
        g_k32_waitx_tgt = (void *)real_WaitForSingleObjectEx;
        if (install_inline_hook(g_k32_waitx_tgt, (void *)hook_WaitForSingleObjectEx, CK_K32_STEAL,
                                &g_k32_waitx_tramp, g_k32_waitx_saved)) {
            real_WaitForSingleObjectEx = (PFN_WaitForSingleObjectEx)g_k32_waitx_tramp;
            log_msg("inline hooked kernel32!WaitForSingleObjectEx (global)");
        }
    }
    if (real_Sleep) {
        g_k32_sleep_tgt = (void *)real_Sleep;
        if (install_inline_hook(g_k32_sleep_tgt, (void *)hook_Sleep, CK_K32_STEAL,
                                &g_k32_sleep_tramp, g_k32_sleep_saved)) {
            real_Sleep = (PFN_Sleep)g_k32_sleep_tramp;
            log_msg("inline hooked kernel32!Sleep (global)");
        }
    }
    real_GetTickCount = k32 ? (PFN_GetTickCount)GetProcAddress(k32, "GetTickCount") : NULL;
    if (real_GetTickCount) {
        g_k32_gtc_tgt = (void *)real_GetTickCount;
        if (install_inline_hook(g_k32_gtc_tgt, (void *)hook_GetTickCount, CK_K32_STEAL,
                                &g_k32_gtc_tramp, g_k32_gtc_saved)) {
            real_GetTickCount = (PFN_GetTickCount)g_k32_gtc_tramp;
            log_msg("inline hooked kernel32!GetTickCount (main spin probe)");
        }
    }

    /* Custom timer sched @ 0x406F40 — H-100 clamp of 100ms busy-wait pumps. */
    if (looks_like_tpw()) {
        g_tmr_tgt = (void *)(ULONG_PTR)CK_TIMER_SCHED;
        if (install_inline_hook(g_tmr_tgt, (void *)hook_TimerSched, CK_TIMER_STEAL, &g_tmr_tramp,
                                g_tmr_saved)) {
            real_TimerSched = (PFN_TimerSched)g_tmr_tramp;
            log_msg("inline hooked tpw!TimerSched (clamp 50..150ms→8)");
            /* #region agent log */
            hooks_agent("H-100", "hooks_hitch.c:install", "tmr-install-ok", "{}");
            /* #endregion */
        } else {
            log_msg("WARN: TimerSched hook failed");
            /* #region agent log */
            hooks_agent("H-100", "hooks_hitch.c:install", "tmr-install-fail", "{}");
            /* #endregion */
        }
        /*
         * H-WAIT64 REJECTED: push 100 at 0x426DF4 is a poll interval, not duration.
         * Shrinking to 8 increased GetTickCount poll rate; spins still 200–600ms.
         */
    }

    /* Game + input/audio modules — empty ~90ms gaps had zero game-IAT waits. */
    patch_waits_on_module(game, "tpw");
    for (i = 0; di_names[i]; ++i)
        patch_waits_on_module(GetModuleHandleA(di_names[i]), di_names[i]);

    if (game && real_SleepEx) {
        if (patch_iat_entry(game, "KERNEL32.dll", "SleepEx", (void *)hook_SleepEx,
                            (void **)&real_SleepEx, &g_iat_SleepEx))
            log_msg("IAT hooked kernel32!SleepEx (observe)");
    }
    if (game && real_WaitForSingleObjectEx) {
        if (patch_iat_entry(game, "KERNEL32.dll", "WaitForSingleObjectEx",
                            (void *)hook_WaitForSingleObjectEx,
                            (void **)&real_WaitForSingleObjectEx, &g_iat_WaitEx))
            log_msg("IAT hooked kernel32!WaitForSingleObjectEx (observe)");
    }
    if (game && real_EnterCriticalSection) {
        if (patch_iat_entry(game, "KERNEL32.dll", "EnterCriticalSection",
                            (void *)hook_EnterCriticalSection, (void **)&real_EnterCriticalSection,
                            &g_iat_EnterCS))
            log_msg("IAT hooked kernel32!EnterCriticalSection (observe)");
    }
    if (game && real_LeaveCriticalSection) {
        if (patch_iat_entry(game, "KERNEL32.dll", "LeaveCriticalSection",
                            (void *)hook_LeaveCriticalSection, (void **)&real_LeaveCriticalSection,
                            &g_iat_LeaveCS))
            log_msg("IAT hooked kernel32!LeaveCriticalSection (observe)");
    }

    if (user) {
        real_PeekMessageA = (PFN_PeekMessageA)GetProcAddress(user, "PeekMessageA");
        real_GetMessageA = (PFN_GetMessageA)GetProcAddress(user, "GetMessageA");
        real_WaitMessage = (PFN_WaitMessage)GetProcAddress(user, "WaitMessage");
    }
    if (game && real_PeekMessageA) {
        if (patch_iat_entry(game, "USER32.dll", "PeekMessageA", (void *)hook_PeekMessageA,
                            (void **)&real_PeekMessageA, &g_iat_PeekMessageA))
            log_msg("IAT hooked user32!PeekMessageA (observe)");
    }
    if (game && real_GetMessageA) {
        if (patch_iat_entry(game, "USER32.dll", "GetMessageA", (void *)hook_GetMessageA,
                            (void **)&real_GetMessageA, &g_iat_GetMessageA))
            log_msg("IAT hooked user32!GetMessageA (observe)");
    }
    if (game && real_WaitMessage) {
        if (patch_iat_entry(game, "USER32.dll", "WaitMessage", (void *)hook_WaitMessage,
                            (void **)&real_WaitMessage, &g_iat_WaitMessage))
            log_msg("IAT hooked user32!WaitMessage (observe)");
    }

    log_msg("hitch waits: slots wait1=%d waitn=%d msgwait=%d sleep=%d di_patch=%d", g_wait1_n,
            g_waitn_n, g_msgwait_n, g_sleep_n, g_di_patch);

    /* #region agent log */
    {
        char js[160];
        snprintf(js, sizeof(js),
                 "{\"wait1\":%d,\"waitn\":%d,\"msgwait\":%d,\"sleep\":%d,\"di_patch\":%d}",
                 g_wait1_n, g_waitn_n, g_msgwait_n, g_sleep_n, g_di_patch);
        hooks_agent("H-WAITX", "hooks_hitch.c:install", "hitch-wait-install", js);
    }
    /* #endregion */
}

void hooks_hitch_remove(void)
{
    int i;
    for (i = 0; i < g_wait1_n; ++i)
        restore_iat_slot(g_wait1_slots[i], (void *)real_WaitForSingleObject);
    for (i = 0; i < g_waitn_n; ++i)
        restore_iat_slot(g_waitn_slots[i], (void *)real_WaitForMultipleObjects);
    for (i = 0; i < g_msgwait_n; ++i)
        restore_iat_slot(g_msgwait_slots[i], (void *)real_MsgWaitForMultipleObjects);
    for (i = 0; i < g_sleep_n; ++i)
        restore_iat_slot(g_sleep_slots[i], (void *)real_Sleep);
    g_wait1_n = g_waitn_n = g_msgwait_n = g_sleep_n = 0;
    restore_iat_slot(g_iat_Sleep, (void *)real_Sleep);
    restore_iat_slot(g_iat_SleepEx, (void *)real_SleepEx);
    restore_iat_slot(g_iat_WaitEx, (void *)real_WaitForSingleObjectEx);
    restore_iat_slot(g_iat_EnterCS, (void *)real_EnterCriticalSection);
    restore_iat_slot(g_iat_LeaveCS, (void *)real_LeaveCriticalSection);
    restore_iat_slot(g_iat_PeekMessageA, (void *)real_PeekMessageA);
    restore_iat_slot(g_iat_GetMessageA, (void *)real_GetMessageA);
    restore_iat_slot(g_iat_WaitMessage, (void *)real_WaitMessage);
    if (g_k32_wait1_tgt && g_k32_wait1_tramp)
        remove_inline_hook(g_k32_wait1_tgt, CK_K32_STEAL, g_k32_wait1_saved, g_k32_wait1_tramp);
    if (g_k32_waitx_tgt && g_k32_waitx_tramp)
        remove_inline_hook(g_k32_waitx_tgt, CK_K32_STEAL, g_k32_waitx_saved, g_k32_waitx_tramp);
    if (g_k32_sleep_tgt && g_k32_sleep_tramp)
        remove_inline_hook(g_k32_sleep_tgt, CK_K32_STEAL, g_k32_sleep_saved, g_k32_sleep_tramp);
    if (g_k32_gtc_tgt && g_k32_gtc_tramp)
        remove_inline_hook(g_k32_gtc_tgt, CK_K32_STEAL, g_k32_gtc_saved, g_k32_gtc_tramp);
    if (g_tmr_tgt && g_tmr_tramp)
        remove_inline_hook(g_tmr_tgt, CK_TIMER_STEAL, g_tmr_saved, g_tmr_tramp);
    g_k32_wait1_tramp = g_k32_waitx_tramp = g_k32_sleep_tramp = g_k32_gtc_tramp = NULL;
    g_tmr_tramp = NULL;
}
