#include "dm_replace_internal.h"

/*
 * DirectMusic replace entry: CoCreate Loader+Perf stubs, InitAudio → DS,
 * GetObject → decode → PlaySegmentEx → PCM.
 */

void dm_replace_install(void)
{
    char envbuf[32];
    DWORD nenv = GetEnvironmentVariableA("CK_DM_REPLACE", envbuf, (DWORD)sizeof(envbuf));
    const char *e = NULL;
    /*
     * Default OFF. Runtime+RE (Celtic_Kings ds8.cpp): our COM stub crashes after the first
     * Segment::SetRepeats (AV execute @ 0xFFFFFE0C / SEH). Game already ships DX dmusic DLLs;
     * dm_native redirects CoCreate/LoadLibrary there. Set CK_DM_REPLACE=1 only to test the stub.
     * Prefer Win32 GetEnvironmentVariableA — Wine often does not expose Unix env to CRT getenv.
     */
    if (nenv > 0 && nenv < sizeof(envbuf))
        e = envbuf;
    else
        e = getenv("CK_DM_REPLACE");
    g_enabled = (e && (e[0] == '1') && e[1] == '\0');
    if (!g_live_cs_ok) {
        InitializeCriticalSection(&g_live_cs);
        InterlockedExchange(&g_live_cs_ok, 1);
    }
    /* #region agent log — VEH always: crash capture even when stub off (H70/H71) */
    AddVectoredExceptionHandler(1, ck_veh);
    {
        char js[160];
        snprintf(js, sizeof(js), "{\"enabled\":%d,\"env\":\"%.32s\"}", g_enabled,
                 e ? e : "(null)");
        dm_agent("H71", "dm_replace.c:install", g_enabled ? "dm-replace-ready" : "dm-replace-disabled",
                 js);
    }
    /* #endregion */
    if (!g_enabled) {
        log_msg("dm-replace: disabled (default) — using dm_native / game dmusic DLLs; "
                "CK_DM_REPLACE=1 to force stub");
        return;
    }
    init_vtables();
    install_onscreen_pan_fix();
    log_msg("dm-replace: ENABLED — CoCreate Loader/Performance → stub + DirectSound");
}

int dm_replace_enabled(void)
{
    return g_enabled;
}

HRESULT dm_replace_cocreate(REFCLSID clsid, REFIID iid, void **ppv)
{
    if (!g_enabled || !clsid || !ppv)
        return E_FAIL;
    *ppv = NULL;
    init_vtables();

    if (IsEqualGUID(clsid, &CLSID_DMPerformance)) {
        CkPerf *p = (CkPerf *)calloc(1, sizeof(*p));
        if (!p)
            return E_OUTOFMEMORY;
        p->lpVtbl = g_perf_vt;
        p->refs = 1;
        InitializeCriticalSection(&p->lock);
        *ppv = p;
        (void)iid;
        dm_agent("R0", "dm_replace.c:cocreate", "cocreate-perf", "{\"ok\":1}");
        log_msg("dm-replace: CoCreate Performance stub %p", (void *)p);
        return S_OK;
    }
    if (IsEqualGUID(clsid, &CLSID_DMLoader)) {
        CkLoader *l = (CkLoader *)calloc(1, sizeof(*l));
        if (!l)
            return E_OUTOFMEMORY;
        l->lpVtbl = g_ldr_vt;
        l->refs = 1;
        *ppv = l;
        (void)iid;
        dm_agent("R0", "dm_replace.c:cocreate", "cocreate-loader", "{\"ok\":1}");
        log_msg("dm-replace: CoCreate Loader stub %p", (void *)l);
        return S_OK;
    }
    return E_FAIL;
}
