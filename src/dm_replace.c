#include "dm_replace_internal.h"
#include "hooks_internal.h"

/*
 * DirectMusic replace entry: CoCreate Loader+Perf stubs, InitAudio → DS,
 * GetObject → decode → PlaySegmentEx → PCM.
 */

void dm_replace_install(void)
{
    const char *e = NULL;
    char envbuf[32];
    DWORD nenv = GetEnvironmentVariableA("CK_DM_REPLACE", envbuf, (DWORD)sizeof(envbuf));

    /*
     * Default ON — DirectMusic COM stub + DirectSound playback.
     * Set CK_DM_REPLACE=0 to use native / game dmusic DLLs via dm_native instead.
     */
    g_enabled = env_on("CK_DM_REPLACE", 1);
    if (nenv > 0 && nenv < sizeof(envbuf))
        e = envbuf;
    else
        e = getenv("CK_DM_REPLACE");
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
        log_msg("dm-replace: disabled (CK_DM_REPLACE=0) — using dm_native / game dmusic DLLs");
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

HMODULE dm_pin_module(const void *address)
{
    HMODULE module = NULL;
    if (!address ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCSTR)address, &module))
        return NULL;
    return module;
}

void dm_worker_exit(HMODULE module, DWORD code)
{
    if (module)
        FreeLibraryAndExitThread(module, code);
    ExitThread(code);
}

void dm_replace_shutdown(void)
{
    if (!g_enabled)
        return;
    beacon_stop();
    stop_all_live_sfx();
    stop_music_buf();
    dm_com_collect_all();
    g_enabled = 0;
}

HRESULT dm_replace_cocreate(REFCLSID clsid, REFIID iid, void **ppv)
{
    if (!g_enabled || !clsid || !ppv)
        return E_FAIL;
    *ppv = NULL;
    init_vtables();

    if (IsEqualGUID(clsid, &CLSID_DMPerformance)) {
        if (iid && !IsEqualGUID(iid, &IID_IUnknown) &&
            !IsEqualGUID(iid, &IID_IDirectMusicPerformance) &&
            !IsEqualGUID(iid, &IID_IDirectMusicPerformance2) &&
            !IsEqualGUID(iid, &IID_IDirectMusicPerformance8))
            return E_NOINTERFACE;
        CkPerf *p = (CkPerf *)calloc(1, sizeof(*p));
        if (!p)
            return E_OUTOFMEMORY;
        p->lpVtbl = g_perf_vt;
        p->refs = 1;
        InitializeCriticalSection(&p->lock);
        *ppv = p;
        dm_agent("R0", "dm_replace.c:cocreate", "cocreate-perf", "{\"ok\":1}");
        log_msg("dm-replace: CoCreate Performance stub %p", (void *)p);
        return S_OK;
    }
    if (IsEqualGUID(clsid, &CLSID_DMLoader)) {
        if (iid && !IsEqualGUID(iid, &IID_IUnknown) &&
            !IsEqualGUID(iid, &IID_IDirectMusicLoader) &&
            !IsEqualGUID(iid, &IID_IDirectMusicLoader8))
            return E_NOINTERFACE;
        CkLoader *l = (CkLoader *)calloc(1, sizeof(*l));
        if (!l)
            return E_OUTOFMEMORY;
        l->lpVtbl = g_ldr_vt;
        l->refs = 1;
        *ppv = l;
        dm_agent("R0", "dm_replace.c:cocreate", "cocreate-loader", "{\"ok\":1}");
        log_msg("dm-replace: CoCreate Loader stub %p", (void *)l);
        return S_OK;
    }
    return E_FAIL;
}
