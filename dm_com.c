#include "dm_replace_internal.h"

void notif_schedule_segend(CkPerf *perf, CkState *st, DWORD dur_ms)
{
    int i;
    CkNotifSlot *s;
    DWORD fire;

    if (!perf || !st || !perf->notif_segment)
        return;
    if (dur_ms < 50u)
        dur_ms = 50u;
    fire = GetTickCount() + dur_ms;

    EnterCriticalSection(&perf->lock);
    for (i = 0; i < CK_NOTIF_SLOTS; i++) {
        if (perf->notif[i].phase == 0)
            break;
    }
    if (i >= CK_NOTIF_SLOTS) {
        LeaveCriticalSection(&perf->lock);
        /* #region agent log */
        dm_agent("H48", "dm_replace.c:notif", "notif-full", "{\"drop\":1}");
        /* #endregion */
        return;
    }
    s = &perf->notif[i];
    memset(&s->msg, 0, sizeof(s->msg));
    s->msg.dwSize = sizeof(s->msg);
    s->msg.dwType = CK_DMUS_PMSGT_NOTIFICATION;
    s->msg.guidNotificationType = CK_GUID_NOTIFICATION_SEGMENT;
    s->msg.dwNotificationOption = CK_DMUS_NOTIFICATION_SEGEND;
    s->msg.punkUser = st;
    s->state = st;
    st->refs++;
    s->fire_ms = fire;
    s->phase = 1;
    LeaveCriticalSection(&perf->lock);

    if (perf->notif_event)
        SetEvent(perf->notif_event);

    /* #region agent log */
    {
        char js[220];
        BYTE *raw = (BYTE *)&s->msg;
        snprintf(js, sizeof(js),
                 "{\"slot\":%d,\"dur\":%lu,\"off_type\":%u,\"off_punk\":%u,\"off_guid\":%u,"
                 "\"off_opt\":%u,\"raw_type\":%lu,\"raw_opt\":%lu,\"punk\":%lu}",
                 i, (unsigned long)dur_ms,
                 (unsigned)offsetof(CkNotifMsg, dwType),
                 (unsigned)offsetof(CkNotifMsg, punkUser),
                 (unsigned)offsetof(CkNotifMsg, guidNotificationType),
                 (unsigned)offsetof(CkNotifMsg, dwNotificationOption),
                 (unsigned long)*(DWORD *)(raw + 0x28),
                 (unsigned long)*(DWORD *)(raw + 0x48),
                 (unsigned long)(ULONG_PTR) * (void **)(raw + 0x34));
        dm_agent("H51", "dm_replace.c:notif", "notif-sched", js);
    }
    /* #endregion */
}

static HRESULT STDMETHODCALLTYPE perf_GetNotificationPMsg(CkPerf *This, void **ppMsg)
{
    int i;
    DWORD now;
    CkNotifSlot *s;
    HRESULT hr = S_FALSE;

    if (!ppMsg)
        return E_POINTER;
    *ppMsg = NULL;
    if (!This)
        return E_POINTER;

    now = GetTickCount();
    EnterCriticalSection(&This->lock);
    for (i = 0; i < CK_NOTIF_SLOTS; i++) {
        s = &This->notif[i];
        if (s->phase == 1 && (LONG)(now - s->fire_ms) >= 0) {
            s->phase = 2;
            *ppMsg = &s->msg;
            hr = S_OK;
            /* #region agent log */
            {
                char js[160];
                snprintf(js, sizeof(js), "{\"slot\":%d,\"opt\":%lu,\"punk\":%lu}",
                         i, (unsigned long)s->msg.dwNotificationOption,
                         (unsigned long)(ULONG_PTR)s->msg.punkUser);
                dm_agent("H50", "dm_replace.c:GetNotificationPMsg", "notif-get", js);
            }
            /* #endregion */
            break;
        }
    }
    LeaveCriticalSection(&This->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE perf_FreePMsg(CkPerf *This, void *pMsg)
{
    int i;

    if (!This || !pMsg)
        return E_POINTER;
    EnterCriticalSection(&This->lock);
    for (i = 0; i < CK_NOTIF_SLOTS; i++) {
        CkNotifSlot *s = &This->notif[i];
        if (&s->msg == pMsg && s->phase == 2) {
            s->phase = 0;
            if (s->state) {
                InterlockedDecrement(&s->state->refs);
                s->state = NULL;
            }
            s->msg.punkUser = NULL;
            /* #region agent log */
            {
                char js[80];
                snprintf(js, sizeof(js), "{\"slot\":%d}", i);
                dm_agent("H48", "dm_replace.c:FreePMsg", "notif-free", js);
            }
            /* #endregion */
            LeaveCriticalSection(&This->lock);
            return S_OK;
        }
    }
    LeaveCriticalSection(&This->lock);
    return S_OK;
}

/* stdcall arity: N = stack args INCLUDING This (4 bytes each; __int64 = 2). */
static HRESULT STDMETHODCALLTYPE stub_n1(void *This)
{
    (void)This;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n2(void *This, void *a)
{
    (void)This;
    (void)a;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n3(void *This, void *a, void *b)
{
    (void)This;
    (void)a;
    (void)b;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n4(void *This, void *a, void *b, void *c)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n5(void *This, void *a, void *b, void *c, void *d)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n6(void *This, void *a, void *b, void *c, void *d, void *e)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n7(void *This, void *a, void *b, void *c, void *d, void *e, void *f)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n8(void *This, void *a, void *b, void *c, void *d, void *e, void *f,
                                        void *g)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n9(void *This, void *a, void *b, void *c, void *d, void *e, void *f,
                                        void *g, void *h)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    (void)h;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE stub_n10(void *This, void *a, void *b, void *c, void *d, void *e, void *f,
                                         void *g, void *h, void *i)
{
    (void)This;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    (void)h;
    (void)i;
    return S_OK;
}

static void *stub_by_argc(int argc)
{
    switch (argc) {
    case 1:
        return (void *)stub_n1;
    case 2:
        return (void *)stub_n2;
    case 3:
        return (void *)stub_n3;
    case 4:
        return (void *)stub_n4;
    case 5:
        return (void *)stub_n5;
    case 6:
        return (void *)stub_n6;
    case 7:
        return (void *)stub_n7;
    case 8:
        return (void *)stub_n8;
    case 9:
        return (void *)stub_n9;
    default:
        return (void *)stub_n10;
    }
}

/* Logged Segment stubs — identify which slot is hit after SetRepeats (crash hunt). */
#define CK_SEG_STUB2(i)                                                               \
    static HRESULT STDMETHODCALLTYPE seg_log2_##i(void *This, void *a)                \
    {                                                                                 \
        (void)This;                                                                   \
        (void)a;                                                                      \
        ck_ring_push(1, i);                                                           \
        return S_OK;                                                                  \
    }
#define CK_SEG_STUB3(i)                                                               \
    static HRESULT STDMETHODCALLTYPE seg_log3_##i(void *This, void *a, void *b)       \
    {                                                                                 \
        (void)This;                                                                   \
        (void)a;                                                                      \
        (void)b;                                                                      \
        ck_ring_push(1, i);                                                           \
        return S_OK;                                                                  \
    }
#define CK_SEG_STUB4(i)                                                               \
    static HRESULT STDMETHODCALLTYPE seg_log4_##i(void *This, void *a, void *b, void *c) \
    {                                                                                 \
        (void)This;                                                                   \
        (void)a;                                                                      \
        (void)b;                                                                      \
        (void)c;                                                                      \
        ck_ring_push(1, i);                                                           \
        return S_OK;                                                                  \
    }
#define CK_SEG_STUB5(i)                                                               \
    static HRESULT STDMETHODCALLTYPE seg_log5_##i(void *This, void *a, void *b, void *c, \
                                                  void *d)                            \
    {                                                                                 \
        (void)This;                                                                   \
        (void)a;                                                                      \
        (void)b;                                                                      \
        (void)c;                                                                      \
        (void)d;                                                                      \
        ck_ring_push(1, i);                                                           \
        return S_OK;                                                                  \
    }
#define CK_SEG_STUB6(i)                                                               \
    static HRESULT STDMETHODCALLTYPE seg_log6_##i(void *This, void *a, void *b, void *c, \
                                                  void *d, void *e)                   \
    {                                                                                 \
        (void)This;                                                                   \
        (void)a;                                                                      \
        (void)b;                                                                      \
        (void)c;                                                                      \
        (void)d;                                                                      \
        (void)e;                                                                      \
        ck_ring_push(1, i);                                                           \
        return S_OK;                                                                  \
    }
#define CK_SEG_STUB7(i)                                                               \
    static HRESULT STDMETHODCALLTYPE seg_log7_##i(void *This, void *a, void *b, void *c, \
                                                  void *d, void *e, void *f)          \
    {                                                                                 \
        (void)This;                                                                   \
        (void)a;                                                                      \
        (void)b;                                                                      \
        (void)c;                                                                      \
        (void)d;                                                                      \
        (void)e;                                                                      \
        (void)f;                                                                      \
        ck_ring_push(1, i);                                                           \
        return S_OK;                                                                  \
    }

CK_SEG_STUB2(7)
CK_SEG_STUB2(8)
CK_SEG_STUB5(9)
CK_SEG_STUB3(10)
CK_SEG_STUB3(11)
CK_SEG_STUB2(12)
CK_SEG_STUB4(13)
CK_SEG_STUB2(14)
CK_SEG_STUB2(15)
CK_SEG_STUB2(16)
CK_SEG_STUB2(17)
CK_SEG_STUB7(18)
CK_SEG_STUB6(19)
CK_SEG_STUB4(20)
CK_SEG_STUB2(21)
CK_SEG_STUB2(22)
CK_SEG_STUB3(23)
CK_SEG_STUB3(24)
CK_SEG_STUB3(25)
CK_SEG_STUB6(26)
CK_SEG_STUB2(27)
CK_SEG_STUB4(28)
CK_SEG_STUB2(30)

static const unsigned char g_perf_argc[PERF_VT] = {
    3, 1, 1, 4, 6, 5, 3, 2, 2, 2, 2, 2, 3, 4, 3, 3, 3, 2, 2, 2, 4, 2, 2, 2, 2, 2, 4, 5, 5, 9,
    3, 7, 6, 4, 4, 2, 2, 3, 1, 5, 6, 6, 7, 7, 8, 10, 5, 3, 4, 5, 2, 2, 8, 4, 4, 4,
};

/* stdcall arg counts including This — IDirectMusicSegment / Segment8 */
static const unsigned char g_seg_argc[SEG_VT] = {
    /* QI AddRef Release */
    3, 1, 1,
    /* Get/Set Length (2), GetRepeats GAME teardown 5-arg @20 (0x40cbb4), SetRepeats, DefaultResolution */
    2, 2, 5, 2, 2, 2,
    /* GetTrack(4), GetTrackGroup(2), InsertTrack(2), RemoveTrack(1) */
    5, 3, 3, 2,
    /* InitPlay(3), Get/SetGraph, Add/RemoveNotification */
    4, 2, 2, 2, 2,
    /* GetParam(6), SetParam(5), Clone(3) */
    7, 6, 4,
    /* Set/Get StartPoint, Set/Get LoopPoints, SetPChannelsUsed */
    2, 2, 3, 3, 3,
    /* Segment8: SetTrackConfig(5), GetAudioPathConfig(1), Compose(3), Download(1), Unload(1) */
    6, 2, 4, 2, 2,
};

static HRESULT STDMETHODCALLTYPE stub_false_pmsg(void *This, void **pp)
{
    static volatile LONG s_n;
    LONG n = InterlockedIncrement(&s_n);
    if ((n % 50) == 1)
        ck_ring_push(5, 21);
    (void)This;
    if (pp)
        *pp = NULL;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE stub_false_playing(void *This, void *a, void *b)
{
    (void)This;
    (void)a;
    (void)b;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE perf_GetTime(void *This, LONGLONG *prt, LONG *pmt)
{
    static volatile LONG s_n;
    static DWORD s_last_log;
    LONG n = InterlockedIncrement(&s_n);
    DWORD now = GetTickCount();
    (void)This;
    /* Stub returns zeros — suspect busy-wait if game polls until music time advances. */
    if (prt)
        *prt = 0;
    if (pmt)
        *pmt = 0;
    /* #region agent log */
    if (n <= 30 || (n % 500) == 0 || (now - s_last_log) >= 250u) {
        char js[160];
        s_last_log = now;
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"prt\":0,\"pmt\":0,\"tid\":%lu,\"now\":%lu}", (long)n,
                 (unsigned long)GetCurrentThreadId(), (unsigned long)now);
        dm_agent("H-GTIME", "dm_com.c:perf_GetTime", "dm-gettime", js);
    }
    /* #endregion */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE perf_SetNotificationHandle(CkPerf *This, HANDLE h, LONGLONG rt)
{
    char js[128];
    (void)rt;
    if (This)
        This->notif_event = h;
    /* #region agent log */
    snprintf(js, sizeof(js), "{\"h\":%lu,\"rt\":%lld}", (unsigned long)(ULONG_PTR)h, (long long)rt);
    dm_agent("C1", "dm_replace.c:SetNotificationHandle", "set-notif-handle", js);
    /* #endregion */
    log_msg("dm-replace: SetNotificationHandle h=%p", h);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE perf_AddNotificationType(CkPerf *This, REFGUID g)
{
    if (This && g && IsEqualGUID(g, &CK_GUID_NOTIFICATION_SEGMENT))
        This->notif_segment = 1;
    /* #region agent log */
    {
        char js[80];
        snprintf(js, sizeof(js), "{\"seg\":%d}", This && This->notif_segment ? 1 : 0);
        dm_agent("C1", "dm_replace.c:AddNotificationType", "add-notif", js);
    }
    /* #endregion */
    log_msg("dm-replace: AddNotificationType");
    return S_OK;
}

/* -------- path (IDirectMusicAudioPath) -------- */
static ULONG STDMETHODCALLTYPE path_AddRef(CkPath *This)
{
    return (ULONG)InterlockedIncrement(&This->refs);
}
static ULONG STDMETHODCALLTYPE path_Release(CkPath *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (r == 0) {
        if (This->ctrl_proxy) {
            CkDsCtrl *px = (CkDsCtrl *)This->ctrl_proxy;
            This->ctrl_proxy = NULL;
            px->path = NULL;
            if (InterlockedDecrement(&px->refs) == 0)
                free(px);
        }
        if (This->ctrl) {
            IDirectSoundBuffer_Release(This->ctrl);
            This->ctrl = NULL;
        }
        free(This);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE path_QI(CkPath *This, REFIID riid, void **ppv)
{
    (void)riid;
    if (!ppv)
        return E_POINTER;
    *ppv = This;
    path_AddRef(This);
    return S_OK;
}
/* Game FUN_0040be40(this=path): call [*(path+4)->vtbl+0x14](self, vol, 0) = SetVolume@12. */
static HRESULT STDMETHODCALLTYPE path_SetVolume(CkPath *This, LONG vol, DWORD dur)
{
    (void)dur;
    if (!This)
        return E_POINTER;
    This->vol = vol;
    path_propagate_volume(This);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE path_Activate(CkPath *This, BOOL on)
{
    (void)This;
    (void)on;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE path_ConvertPChannel(CkPath *This, DWORD in, DWORD *out)
{
    (void)This;
    if (out)
        *out = in;
    return S_OK;
}
/*
 * Game 0x40bbc0: GetObjectInPath(..., PATH_BUFFER, IID_IDirectSoundBuffer8) then SetPan.
 * Returning primary made SetPan mute/bias the whole mix (H32: SFX heard once).
 * Give each path a tiny CTRLVOLUME|CTRLPAN secondary + COM proxy so SetPan hits live SFX.
 */
enum { DSCTRL_VT = 20 };
static void *g_dsctrl_vt[DSCTRL_VT];
static int g_dsctrl_vt_ready;

static HRESULT STDMETHODCALLTYPE dsc_QI(CkDsCtrl *This, REFIID riid, void **ppv)
{
    (void)riid;
    if (!ppv)
        return E_POINTER;
    *ppv = This;
    InterlockedIncrement(&This->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE dsc_AddRef(CkDsCtrl *This)
{
    return (ULONG)InterlockedIncrement(&This->refs);
}
static ULONG STDMETHODCALLTYPE dsc_Release(CkDsCtrl *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (r == 0) {
        /* Real buffer owned by CkPath — do not Release here. */
        free(This);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE dsc_GetVolume(CkDsCtrl *This, LONG *vol)
{
    if (!This || !vol)
        return E_POINTER;
    *vol = This->path ? This->path->vol : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsc_GetPan(CkDsCtrl *This, LONG *pan)
{
    if (!This || !pan)
        return E_POINTER;
    *pan = This->path ? This->path->pan : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsc_SetVolume(CkDsCtrl *This, LONG vol)
{
    if (!This || !This->path)
        return E_POINTER;
    This->path->vol = vol;
    path_propagate_volume(This->path);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsc_SetPan(CkDsCtrl *This, LONG pan)
{
    if (!This || !This->path)
        return E_POINTER;
    This->path->pan = path_clamp_pan(pan);
    path_propagate_pan(This->path);
    beacon_update_pan(This->path->pan, This->path->id);
    return S_OK;
}

static void init_dsctrl_vt(void)
{
    int i;
    if (g_dsctrl_vt_ready)
        return;
    /* Default: stdcall This+1arg (GetCaps/etc). Unused paths should not be hit. */
    for (i = 0; i < DSCTRL_VT; ++i)
        g_dsctrl_vt[i] = stub_by_argc(2);
    g_dsctrl_vt[0] = (void *)dsc_QI;
    g_dsctrl_vt[1] = (void *)dsc_AddRef;
    g_dsctrl_vt[2] = (void *)dsc_Release;
    g_dsctrl_vt[6] = (void *)dsc_GetVolume;  /* 0x18 */
    g_dsctrl_vt[7] = (void *)dsc_GetPan;     /* 0x1c */
    g_dsctrl_vt[15] = (void *)dsc_SetVolume; /* 0x3c */
    g_dsctrl_vt[16] = (void *)dsc_SetPan;    /* 0x40 — game 0x40bbc0 */
    g_dsctrl_vt_ready = 1;
}

static CkDsCtrl *path_ensure_ctrl_proxy(CkPath *path)
{
    CkDsCtrl *px;
    if (!path)
        return NULL;
    if (path->ctrl_proxy)
        return (CkDsCtrl *)path->ctrl_proxy;
    init_dsctrl_vt();
    px = (CkDsCtrl *)calloc(1, sizeof(*px));
    if (!px)
        return NULL;
    px->lpVtbl = g_dsctrl_vt;
    px->refs = 1;
    px->path = path;
    px->real = path->ctrl;
    path->ctrl_proxy = px;
    return px;
}

static HRESULT STDMETHODCALLTYPE path_GetObjectInPath(CkPath *This, DWORD pch, DWORD stage, DWORD buf,
                                                       REFGUID guidObj, DWORD index, REFGUID iid,
                                                       void **ppObject)
{
    CkDsCtrl *px;
    (void)pch;
    (void)buf;
    (void)guidObj;
    (void)index;
    (void)iid;
    if (!ppObject)
        return E_POINTER;
    *ppObject = NULL;
    if (!This || stage != 0x6000u || !g_active_perf || !g_active_perf->ds)
        return E_FAIL;

    if (!This->ctrl) {
        DSBUFFERDESC desc;
        WAVEFORMATEX wfx;
        BYTE silence[256];
        LPVOID p1 = NULL, p2 = NULL;
        DWORD n1 = 0, n2 = 0;
        HRESULT hr;

        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 22050;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = 44100;
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_GLOBALFOCUS |
                       DSBCAPS_STATIC;
        desc.dwBufferBytes = sizeof(silence);
        desc.lpwfxFormat = &wfx;
        memset(silence, 0, sizeof(silence));
        hr = IDirectSound_CreateSoundBuffer(g_active_perf->ds, &desc, &This->ctrl, NULL);
        if (FAILED(hr) || !This->ctrl) {
            This->ctrl = NULL;
            return E_FAIL;
        }
        if (SUCCEEDED(IDirectSoundBuffer_Lock(This->ctrl, 0, sizeof(silence), &p1, &n1, &p2, &n2, 0))) {
            if (p1 && n1)
                memcpy(p1, silence, n1);
            IDirectSoundBuffer_Unlock(This->ctrl, p1, n1, p2, n2);
        }
        if (This->vol)
            IDirectSoundBuffer_SetVolume(This->ctrl, This->vol);
        if (This->pan)
            IDirectSoundBuffer_SetPan(This->ctrl, This->pan);
    }

    px = path_ensure_ctrl_proxy(This);
    if (!px)
        return E_OUTOFMEMORY;
    dsc_AddRef(px);
    *ppObject = px;
    return S_OK;
}

/* -------- state (IDirectMusicSegmentState8) -------- */
void state_init(CkState *st, CkSegment *seg, DWORD repeats)
{
    if (!st)
        return;
    st->lpVtbl = g_state_vt;
    st->refs = 1;
    st->seg = seg;
    st->repeats = repeats;
}

static ULONG STDMETHODCALLTYPE state_AddRef(CkState *This)
{
    return (ULONG)InterlockedIncrement(&This->refs);
}
static ULONG STDMETHODCALLTYPE state_Release(CkState *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (r == 0)
        free(This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE state_QI(CkState *This, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (!riid || IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirectMusicSegmentState8)) {
        *ppv = This;
        state_AddRef(This);
        /* #region agent log */
        dm_agent("H49", "dm_replace.c:state_QI", "state-qi-ok",
                 "{\"guid\":\"SegmentState8\"}");
        /* #endregion */
        return S_OK;
    }
    /* #region agent log */
    {
        char js[120];
        snprintf(js, sizeof(js), "{\"d1\":%08lX}", (unsigned long)riid->Data1);
        dm_agent("H49", "dm_replace.c:state_QI", "state-qi-fail", js);
    }
    /* #endregion */
    *ppv = NULL;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE state_GetRepeats(CkState *This, DWORD *repeats)
{
    if (!repeats)
        return E_POINTER;
    *repeats = This ? This->repeats : 0;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE seg_AddRef(CkSegment *This);

static HRESULT STDMETHODCALLTYPE state_GetSegment(CkState *This, void **segment)
{
    if (!segment)
        return E_POINTER;
    *segment = NULL;
    if (!This || !This->seg)
        return E_FAIL;
    *segment = This->seg;
    seg_AddRef(This->seg);
    /* #region agent log */
    {
        char js[120];
        snprintf(js, sizeof(js), "{\"seg\":%lu}", (unsigned long)(ULONG_PTR)This->seg);
        dm_agent("H49", "dm_replace.c:GetSegment", "state-GetSegment", js);
    }
    /* #endregion */
    return S_OK;
}

/* -------- segment -------- */
static ULONG STDMETHODCALLTYPE seg_AddRef(CkSegment *This)
{
    return (ULONG)InterlockedIncrement(&This->refs);
}
static ULONG STDMETHODCALLTYPE seg_Release(CkSegment *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    /* #region agent log */
    {
        char js[120];
        ck_ring_push(9, (int)r);
        snprintf(js, sizeof(js), "{\"refs\":%ld,\"path\":\"%.60s\"}", (long)r, This->path);
        dm_agent("H28", "dm_replace.c:Release", "seg-release", js);
    }
    /* #endregion */
    /*
     * Do not free — game touches segment after Release (crash-ctx: write AV @+8, ebp=seg,
     * path music/Game10.ogg, refs→0). Native DM keeps objects alive longer.
     */
    if (r <= 0) {
        This->refs = 1;
        return 1;
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE seg_QI(CkSegment *This, REFIID riid, void **ppv)
{
    ck_ring_push(4, 0);
    /* #region agent log */
    {
        char js[160];
        snprintf(js, sizeof(js),
                 "{\"pcm\":%lu,\"guid\":\"%08lX-%04X-%04X\",\"ppv\":%lu}",
                 (unsigned long)This->pcm_bytes,
                 riid ? (unsigned long)riid->Data1 : 0UL, riid ? (unsigned)riid->Data2 : 0,
                 riid ? (unsigned)riid->Data3 : 0, (unsigned long)(ULONG_PTR)ppv);
        dm_agent("H3", "dm_replace.c:seg_QI", "seg-qi", js);
    }
    /* #endregion */
    if (!ppv)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectMusicSegment8)) {
        *ppv = This;
        seg_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE seg_GetLength(CkSegment *This, LONG *pmt)
{
    ck_ring_push(7, 3);
    /* #region agent log */
    dm_agent("H2", "dm_replace.c:GetLength", "seg-GetLength",
             "{\"idx\":3}");
    /* #endregion */
    if (pmt) {
        if (This->fmt.nAvgBytesPerSec && This->pcm_bytes)
            *pmt = (LONG)((This->pcm_bytes * 768u) / This->fmt.nAvgBytesPerSec);
        else
            *pmt = 1;
        if (*pmt < 1)
            *pmt = 1;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE seg_SetLength(CkSegment *This, LONG mt)
{
    (void)This;
    (void)mt;
    /* #region agent log */
    dm_agent("H2", "dm_replace.c:SetLength", "seg-SetLength", "{\"idx\":4}");
    /* #endregion */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE seg_SetRepeats(CkSegment *This, DWORD n)
{
    void *retaddr = __builtin_return_address(0);
    /* #region agent log — H21: stack smash / bad ret; keep SetRepeats body minimal */
    ck_ring_push(2, 6);
    if (This)
        This->repeats = n;
    {
        char js[260];
        snprintf(js, sizeof(js),
                 "{\"idx\":6,\"n\":%lu,\"n_signed\":%ld,\"refs\":%ld,\"path\":\"%.80s\"}",
                 (unsigned long)n, (long)(LONG)n, (long)(This ? This->refs : -1),
                 This ? This->path : "");
        dm_agent("H47", "dm_replace.c:SetRepeats", "seg-SetRepeats", js);
    }
    ck_ring_push(2, -6);
    {
        char js[160];
        snprintf(js, sizeof(js), "{\"ok\":1,\"ret\":%lu,\"esp_chk\":1}",
                 (unsigned long)(ULONG_PTR)retaddr);
        dm_agent("H21", "dm_replace.c:SetRepeats", "seg-SetRepeats-exit", js);
    }
    /* #endregion */
    (void)retaddr;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE seg_GetRepeats(CkSegment *This, DWORD a, DWORD b, DWORD c, DWORD d)
{
    /*
     * Wine GetRepeats is (This, DWORD*) @8. Game teardown 0x40cbb4 pushes 5 dwords
     * onto segment vtbl+0x14 → must be @20 or stack smash → heap AV (H31).
     */
    ck_ring_push(1, 5);
    /* #region agent log */
    {
        char js[200];
        snprintf(js, sizeof(js),
                 "{\"slot\":5,\"a\":%lu,\"b\":%lu,\"c\":%lu,\"d\":%lu,\"refs\":%ld}",
                 (unsigned long)a, (unsigned long)b, (unsigned long)c, (unsigned long)d,
                 (long)(This ? This->refs : -1));
        dm_agent("H31", "dm_replace.c:GetRepeats", "seg-GetRepeats5", js);
    }
    /* #endregion */
    (void)This;
    (void)b;
    (void)c;
    (void)d;
    if (a > 0x10000u && !IsBadWritePtr((void *)(ULONG_PTR)a, sizeof(DWORD)))
        *(DWORD *)(ULONG_PTR)a = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE seg_Download(CkSegment *This, void *pAudioPath)
{
    ck_ring_push(3, 29);
    This->last_path = pAudioPath;
    /* #region agent log */
    {
        char js[160];
        snprintf(js, sizeof(js), "{\"seg\":%lu,\"path\":%lu,\"pcm\":%lu,\"refs\":%ld}",
                 (unsigned long)(ULONG_PTR)This, (unsigned long)(ULONG_PTR)pAudioPath,
                 (unsigned long)This->pcm_bytes, (long)This->refs);
        dm_agent("R4", "dm_replace.c:Download", "seg-download", js);
    }
    /* #endregion */
    log_msg("dm-replace: Download seg=%p pcm=%lu", (void *)This, (unsigned long)This->pcm_bytes);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE seg_Unload(CkSegment *This, void *pAudioPath)
{
    int is_music = This && path_is_music(This->path);

    (void)pAudioPath;
    /* H55: native Unload tears down the voice; we must Stop the DS buffer. */
    stop_live_for_segment(This);
    if (is_music) {
        /* Only stop BGM — do not wipe s_live (playlist track change Unloads music). */
        stop_music_buf();
    }
    (void)This;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE seg_InitPlay(CkSegment *This, void **ppState, void *pPerf, DWORD flags)
{
    CkState *st;
    (void)This;
    (void)pPerf;
    (void)flags;
    if (!ppState)
        return E_POINTER;
    st = (CkState *)calloc(1, sizeof(*st));
    if (!st)
        return E_OUTOFMEMORY;
    state_init(st, This, This ? (DWORD)This->repeats : 0);
    *ppState = st;
    /* #region agent log */
    dm_agent("H34", "dm_replace.c:InitPlay", "seg-InitPlay", "{\"ok\":1}");
    /* #endregion */
    return S_OK;
}

/* -------- WAV / DS play -------- */
/* -------- performance -------- */
static ULONG STDMETHODCALLTYPE perf_AddRef(CkPerf *This)
{
    return (ULONG)InterlockedIncrement(&This->refs);
}
static ULONG STDMETHODCALLTYPE perf_Release(CkPerf *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (r == 0) {
        beacon_stop();
        if (This->music_buf) {
            IDirectSoundBuffer_Stop(This->music_buf);
            IDirectSoundBuffer_Release(This->music_buf);
            This->music_buf = NULL;
        }
        if (This->primary)
            IDirectSoundBuffer_Release(This->primary);
        if (This->ds)
            IDirectSound_Release(This->ds);
        DeleteCriticalSection(&This->lock);
        free(This);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE perf_QI(CkPerf *This, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (!riid || IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectMusicPerformance8)) {
        *ppv = This;
        perf_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE perf_InitAudio(CkPerf *This, void **ppDM, void **ppDS, HWND hwnd,
                                                DWORD pathType, DWORD pchannels, DWORD flags,
                                                void *params)
{
    HRESULT hr;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;
    char js[256];

    (void)ppDM;
    (void)pathType;
    (void)pchannels;
    (void)params;
    This->hwnd = hwnd ? hwnd : GetForegroundWindow();

    hr = DirectSoundCreate(NULL, &This->ds, NULL);
    if (FAILED(hr)) {
        log_msg("dm-replace: DirectSoundCreate hr=0x%08lx", (unsigned long)hr);
        dm_agent("R1", "dm_replace.c:InitAudio", "init-fail", "{\"step\":\"dsc\"}");
        return hr;
    }
    hr = IDirectSound_SetCooperativeLevel(This->ds, This->hwnd ? This->hwnd : GetDesktopWindow(),
                                          DSSCL_PRIORITY);
    if (FAILED(hr))
        hr = IDirectSound_SetCooperativeLevel(This->ds, GetDesktopWindow(), DSSCL_NORMAL);

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 44100 * 4;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    hr = IDirectSound_CreateSoundBuffer(This->ds, &desc, &This->primary, NULL);
    if (SUCCEEDED(hr))
        IDirectSoundBuffer_SetFormat(This->primary, &wfx);

    if (ppDS)
        *ppDS = NULL;

    g_active_perf = This;

    /* Warm music OGG files off the game thread (H-AUD: tpw0 cold decode ~1.5s). */
    music_preload_start();

    /* #region agent log */
    snprintf(js, sizeof(js),
             "{\"hr\":%ld,\"flags\":\"0x%lX\",\"hwnd\":%lu,\"ds\":%lu}", (long)hr, (unsigned long)flags,
             (unsigned long)(ULONG_PTR)This->hwnd, (unsigned long)(ULONG_PTR)This->ds);
    dm_agent("R1", "dm_replace.c:InitAudio", "init-audio", js);
    /* #endregion */
    log_msg("dm-replace: InitAudio hr=0x%08lx flags=0x%lx ds=%p", (unsigned long)hr,
            (unsigned long)flags, (void *)This->ds);
    return SUCCEEDED(hr) ? S_OK : hr;
}

static HRESULT STDMETHODCALLTYPE perf_CreateStdPath(CkPerf *This, DWORD type, DWORD pchannels,
                                                    DWORD flags, void **ppPath)
{
    CkPath *p;
    char js[160];
    (void)This;
    (void)flags;
    if (!ppPath)
        return E_POINTER;
    p = (CkPath *)calloc(1, sizeof(*p));
    if (!p)
        return E_OUTOFMEMORY;
    p->lpVtbl = g_path_vt;
    p->self = p; /* Download wrapper reads *(path+4) */
    p->refs = 1;
    p->id = (DWORD)InterlockedIncrement((LONG *)&g_path_id);
    *ppPath = p;
    snprintf(js, sizeof(js), "{\"type\":%lu,\"pch\":%lu,\"id\":%lu,\"path\":%lu}", (unsigned long)type,
             (unsigned long)pchannels, (unsigned long)p->id, (unsigned long)(ULONG_PTR)p);
    dm_agent("R3", "dm_replace.c:CreateStdPath", "audio-path", js);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE perf_PlaySegmentEx(CkPerf *This, void *source, WCHAR *name,
                                                    void *transition, DWORD flags, LONGLONG start,
                                                    void **ppState, void *from, void *audiopath);

static HRESULT STDMETHODCALLTYPE perf_PlaySegment(CkPerf *This, void *source, DWORD flags, LONGLONG start,
                                                  void **ppState)
{
    /* #region agent log */
    {
        CkSegment *seg = (CkSegment *)source;
        char js[160];
        snprintf(js, sizeof(js), "{\"pcm\":%lu,\"flags\":0x%lX,\"seg\":%lu}",
                 (unsigned long)(seg ? seg->pcm_bytes : 0), (unsigned long)flags,
                 (unsigned long)(ULONG_PTR)source);
        dm_agent("H9", "dm_replace.c:PlaySegment", "play-seg-old", js);
    }
    /* #endregion */
    return perf_PlaySegmentEx(This, source, NULL, NULL, flags, start, ppState, NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE perf_PlaySegmentEx(CkPerf *This, void *source, WCHAR *name,
                                                    void *transition, DWORD flags, LONGLONG start,
                                                    void **ppState, void *from, void *audiopath)
{
    CkSegment *seg = (CkSegment *)source;
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_play_n);
    CkState *st = NULL;
    char js[280];
    CkPath *path = (CkPath *)(audiopath ? audiopath : from);

    (void)name;
    (void)transition;
    (void)start;

    ck_ring_push(6, 45);

    if (!seg || !seg->pcm)
        return E_INVALIDARG;
    if (!seg->last_path && path)
        seg->last_path = path;
    if (!path && seg->last_path)
        path = (CkPath *)seg->last_path;

    /* #region agent log */
    {
        char js0[240];
        snprintf(js0, sizeof(js0),
                 "{\"n\":%ld,\"pcm\":%lu,\"flags\":0x%lX,\"refs\":%ld,\"path_id\":%lu,\"vol\":%ld,"
                 "\"repeats\":%ld,\"path\":\"%.80s\"}",
                 (long)n, (unsigned long)seg->pcm_bytes, (unsigned long)flags, (long)seg->refs,
                 (unsigned long)(path ? path->id : 0), (long)(path ? path->vol : 0),
                 (long)(LONG)seg->repeats, seg->path);
        dm_agent("H40", "dm_replace.c:PlaySegmentEx", "play-enter", js0);
    }
    /* #endregion */

    hr = play_pcm(This, seg, path, flags);

    if (ppState) {
        st = (CkState *)calloc(1, sizeof(*st));
        if (st) {
            state_init(st, seg, (DWORD)(LONG)seg->repeats);
            *ppState = st;
        } else
            *ppState = NULL;
    }

    if (SUCCEEDED(hr) && st) {
        int music = !(flags & 0x80u) && path_is_music(seg->path);
        int loop = seg->repeats == (DWORD)-1;
        /* Non-loop music needs SEGEND so the game can advance the playlist. */
        if (!music || !loop) {
            DWORD dur = seg->fmt.nAvgBytesPerSec
                            ? (DWORD)((ULONGLONG)seg->pcm_bytes * 1000ull / seg->fmt.nAvgBytesPerSec)
                            : 1000u;
            notif_schedule_segend(This, st, dur);
        }
    }

    /* #region agent log */
    {
        int amb = path_is_ambient_sfx(seg->path);
        const char *pn = seg->path;
        const char *base = pn;
        const char *p;
        for (p = pn; *p; ++p) {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }
        snprintf(js, sizeof(js),
                 "{\"n\":%ld,\"hr\":%ld,\"flags\":0x%lX,\"pcm\":%lu,\"path_id\":%lu,\"vol\":%ld,"
                 "\"ambient\":%d,\"file\":\"%.48s\",\"tid\":%lu}",
                 (long)n, (long)hr, (unsigned long)flags, (unsigned long)seg->pcm_bytes,
                 (unsigned long)(path ? path->id : 0), (long)(path ? path->vol : 0), amb, base,
                 (unsigned long)GetCurrentThreadId());
        dm_agent(amb ? "H-AMB" : "H40", "dm_replace.c:PlaySegmentEx",
                 amb ? "ambient-play" : "play-seg", js);
    }
    /* #endregion */
    if (n <= 40 || (n % 25) == 0)
        log_msg("dm-replace: PlaySegmentEx #%ld hr=0x%08lx pcm=%lu", (long)n, (unsigned long)hr,
                (unsigned long)seg->pcm_bytes);
    return hr;
}

static HRESULT STDMETHODCALLTYPE perf_Stop(CkPerf *This, void *seg, void *state, DWORD mt, DWORD flags)
{
    (void)This;
    (void)mt;
    (void)flags;
    /* Native Stop(NULL,NULL) stops all; we only hold BGM in music_buf. */
    if (!seg && !state)
        stop_music_buf();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE perf_StopEx(CkPerf *This, void *obj, ULONGLONG when, DWORD flags)
{
    (void)This;
    (void)when;
    (void)flags;
    if (!obj)
        stop_music_buf();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE perf_CloseDown(CkPerf *This)
{
    stop_all_live_sfx();
    if (This->music_buf) {
        IDirectSoundBuffer_Stop(This->music_buf);
        IDirectSoundBuffer_Release(This->music_buf);
        This->music_buf = NULL;
    }
    if (This->primary) {
        IDirectSoundBuffer_Release(This->primary);
        This->primary = NULL;
    }
    if (This->ds) {
        IDirectSound_Release(This->ds);
        This->ds = NULL;
    }
    if (g_active_perf == This)
        g_active_perf = NULL;
    return S_OK;
}

/* -------- loader -------- */
static ULONG STDMETHODCALLTYPE ldr_AddRef(CkLoader *This)
{
    return (ULONG)InterlockedIncrement(&This->refs);
}
static ULONG STDMETHODCALLTYPE ldr_Release(CkLoader *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (r == 0)
        free(This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ldr_QI(CkLoader *This, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (!riid || IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectMusicLoader8)) {
        *ppv = This;
        ldr_AddRef(This);
        return S_OK;
    }
    /* Accept any — game may pass IID_Loader without 8 */
    *ppv = This;
    ldr_AddRef(This);
    return S_OK;
}

/* IDirectMusicLoader::ReleaseObject — drop the QI ref ba50 acquired (H52). */
static HRESULT STDMETHODCALLTYPE ldr_ReleaseObject(CkLoader *This, void *pObject)
{
    (void)This;
    /* #region agent log */
    {
        char js[80];
        snprintf(js, sizeof(js), "{\"obj\":%lu}", (unsigned long)(ULONG_PTR)pObject);
        dm_agent("H52", "dm_replace.c:ReleaseObject", "ldr-ReleaseObject", js);
    }
    /* #endregion */
    if (pObject)
        ((ULONG(STDMETHODCALLTYPE *)(void *))(*(void ***)pObject)[2])(pObject);
    return S_OK;
}

/* Game ba50: loader vtbl+0x34 (slot 13), 2 stack args — must ret@8 (H52). */
static HRESULT STDMETHODCALLTYPE ldr_slot13(CkLoader *This, void *a)
{
    (void)This;
    /* #region agent log */
    {
        char js[80];
        snprintf(js, sizeof(js), "{\"a\":%lu}", (unsigned long)(ULONG_PTR)a);
        dm_agent("H52", "dm_replace.c:ldr_slot13", "ldr-slot13", js);
    }
    /* #endregion */
    if (a)
        ((ULONG(STDMETHODCALLTYPE *)(void *))(*(void ***)a)[2])(a);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ldr_GetObject(CkLoader *This, void *pDesc, REFIID riid, void **ppv)
{
    DWORD *pd;
    DWORD valid = 0;
    void *stream = NULL;
    BYTE *raw = NULL;
    DWORD raw_len = 0;
    BYTE *pcm_buf = NULL;
    DWORD pcm_len = 0;
    WAVEFORMATEX fmt;
    CkSegment *seg;
    HRESULT hr;
    LONG n = InterlockedIncrement(&g_get_n);
    char js[280];
    char scraped[260];

    (void)This;
    (void)riid;
    if (!pDesc || !ppv)
        return E_POINTER;
    *ppv = NULL;

    pd = (DWORD *)pDesc;
    valid = pd[1];
    if (!(valid & DMUS_OBJ_STREAM)) {
        dm_agent("R2", "dm_replace.c:GetObject", "get-not-stream", "{\"valid\":0}");
        return E_FAIL;
    }

    if (pd[0] >= 0x350)
        stream = *(void **)((BYTE *)pDesc + 0x34C);
    if (!stream && pd[0] >= 8)
        stream = *(void **)((BYTE *)pDesc + pd[0] - sizeof(void *));

    if (!stream) {
        dm_agent("R2", "dm_replace.c:GetObject", "get-no-stream-ptr", "{\"fail\":1}");
        return E_FAIL;
    }

    scraped[0] = '\0';
    if (!scrape_stream_path(stream, scraped, sizeof(scraped))) {
        dm_agent("R2b", "dm_replace.c:GetObject", "no-path", "{\"fail\":1}");
        return E_FAIL;
    }

    /* Music: shared full-PCM cache (H-AUD). SFX: small copy cache. */
    if (path_is_music(scraped) && music_cache_get(scraped, &fmt, &pcm_buf, &pcm_len)) {
        /* #region agent log */
        {
            FILE *df = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
            if (df) {
                fprintf(df,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"hitch-map1\",\"hypothesisId\":\"H-AUD\","
                        "\"location\":\"dm_com.c:GetObject\",\"message\":\"audio-cache-hit\","
                        "\"data\":{\"n\":%ld,\"path\":\"%.100s\",\"pcm\":%lu,\"music\":1},"
                        "\"timestamp\":%lu}\n",
                        (long)n, scraped, (unsigned long)pcm_len, (unsigned long)GetTickCount());
                fclose(df);
            }
        }
        /* #endregion */
    } else if (!path_is_music(scraped) && cache_get(scraped, &fmt, &pcm_buf, &pcm_len)) {
        /* ok — SFX owned copy */
    } else {
        LONGLONG t_dec;
        double ms_load = 0, ms_dec = 0;
        int is_music = path_is_music(scraped);
        t_dec = hitch_qpc_now();
        hr = load_wav_file_or_pak(scraped, &raw, &raw_len);
        ms_load = hitch_qpc_ms_since(t_dec);
        if (FAILED(hr) || !raw) {
            if (n <= 8 || (n % 40) == 0) {
                snprintf(js, sizeof(js), "{\"n\":%ld,\"hr\":%ld,\"path\":\"%.120s\"}", (long)n, (long)hr,
                         scraped);
                dm_agent("R2", "dm_replace.c:GetObject", "get-load-fail", js);
            }
            free(raw);
            return FAILED(hr) ? hr : E_FAIL;
        }
        t_dec = hitch_qpc_now();
        if (!decode_to_pcm(raw, raw_len, &fmt, &pcm_buf, &pcm_len,
                           is_music ? OGG_DECODE_FULL : OGG_DECODE_MAX_SEC)) {
            snprintf(js, sizeof(js), "{\"n\":%ld,\"path\":\"%.120s\",\"raw\":%lu}", (long)n, scraped,
                     (unsigned long)raw_len);
            dm_agent("R2", "dm_replace.c:GetObject", "get-bad-decode", js);
            if (n <= 8)
                log_msg("dm-replace: decode fail '%s' len=%lu", scraped, (unsigned long)raw_len);
            free(raw);
            return E_FAIL;
        }
        ms_dec = hitch_qpc_ms_since(t_dec);
        free(raw);
        raw = NULL;
        if (is_music) {
            /* Full decode into music cache — DS STATIC buffer must hold the whole track. */
            pcm_buf = music_cache_intern(scraped, &fmt, pcm_buf, pcm_len);
        } else
            cache_put(scraped, &fmt, pcm_buf, pcm_len);
        if (n <= 30 || (n % 50) == 0 || is_music || ms_load + ms_dec >= 5.0) {
            snprintf(js, sizeof(js),
                     "{\"n\":%ld,\"path\":\"%.100s\",\"pcm\":%lu,\"rate\":%u,\"ch\":%u,\"music\":%d}",
                     (long)n, scraped, (unsigned long)pcm_len, (unsigned)fmt.nSamplesPerSec,
                     (unsigned)fmt.nChannels, is_music ? 1 : 0);
            dm_agent("H30", "dm_replace.c:GetObject", "decoded", js);
            log_msg("dm-replace: decoded '%s' pcm=%lu %uHz %uch ms=%.1f+%.1f", scraped,
                    (unsigned long)pcm_len, (unsigned)fmt.nSamplesPerSec, (unsigned)fmt.nChannels,
                    ms_load, ms_dec);
        }
    }

    seg = (CkSegment *)calloc(1, sizeof(*seg));
    if (!seg) {
        free(pcm_buf);
        return E_OUTOFMEMORY;
    }
    seg->lpVtbl = g_seg_vt;
    seg->refs = 1;
    seg->fmt = fmt;
    seg->pcm_bytes = pcm_len;
    seg->pcm = pcm_buf;
    strncpy(seg->path, scraped, sizeof(seg->path) - 1);
    *ppv = seg;

    /* #region agent log */
    {
        char js2[200];
        snprintf(js2, sizeof(js2),
                 "{\"sizeof\":%lu,\"off_refs\":%lu,\"off_pcm\":%lu,\"path\":\"%.80s\",\"pcm\":%lu}",
                 (unsigned long)sizeof(CkSegment), (unsigned long)offsetof(CkSegment, refs),
                 (unsigned long)offsetof(CkSegment, pcm), scraped, (unsigned long)pcm_len);
        dm_agent("R5", "dm_replace.c:GetObject", "seg-layout", js2);
    }
    /* #endregion */

    /* Always log successes — sparse logging hid BIRDS5 before crash. */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"hr\":0,\"pcm\":%lu,\"rate\":%u,\"ch\":%u,\"path\":\"%.120s\"}", (long)n,
             (unsigned long)pcm_len, (unsigned)fmt.nSamplesPerSec, (unsigned)fmt.nChannels, scraped);
    dm_agent("R2", "dm_replace.c:GetObject", "get-object", js);
    return S_OK;
}

void init_vtables(void)
{
    int i;
    if (g_vt_ready)
        return;

    for (i = 0; i < PERF_VT; ++i)
        g_perf_vt[i] = stub_by_argc(g_perf_argc[i] ? g_perf_argc[i] : 4);

    g_perf_vt[0] = (void *)perf_QI;
    g_perf_vt[1] = (void *)perf_AddRef;
    g_perf_vt[2] = (void *)perf_Release;
    g_perf_vt[4] = (void *)perf_PlaySegment; /* classic PlaySegment */
    g_perf_vt[PERF_IDX_STOP] = (void *)perf_Stop;
    g_perf_vt[14] = (void *)stub_false_playing; /* IsPlaying */
    g_perf_vt[15] = (void *)perf_GetTime;
    g_perf_vt[PERF_IDX_SETNOTIF] = (void *)perf_SetNotificationHandle;
    g_perf_vt[PERF_IDX_GETNOTIF] = (void *)perf_GetNotificationPMsg;
    g_perf_vt[PERF_IDX_FREEPMSG] = (void *)perf_FreePMsg;
    g_perf_vt[PERF_IDX_ADDNOTIF] = (void *)perf_AddNotificationType;
    g_perf_vt[PERF_IDX_CLOSEDOWN] = (void *)perf_CloseDown;
    g_perf_vt[PERF_IDX_INITAUDIO] = (void *)perf_InitAudio;
    g_perf_vt[PERF_IDX_PLAYSEGEX] = (void *)perf_PlaySegmentEx;
    g_perf_vt[PERF_IDX_STOPEX] = (void *)perf_StopEx;
    g_perf_vt[PERF_IDX_CREATESTD] = (void *)perf_CreateStdPath;

    for (i = 0; i < LDR_VT; ++i)
        g_ldr_vt[i] = stub_by_argc(2); /* default ret@8 — never stub_n4 (H52 stack smash) */
    g_ldr_vt[0] = (void *)ldr_QI;
    g_ldr_vt[1] = (void *)ldr_AddRef;
    g_ldr_vt[2] = (void *)ldr_Release;
    g_ldr_vt[LDR_IDX_GETOBJECT] = (void *)ldr_GetObject;
    g_ldr_vt[LDR_IDX_SETOBJECT] = stub_by_argc(2);
    g_ldr_vt[LDR_IDX_SETSEARCH] = stub_by_argc(4);
    g_ldr_vt[LDR_IDX_SCANDIR] = stub_by_argc(4);
    g_ldr_vt[LDR_IDX_CACHEOBJ] = stub_by_argc(2);
    g_ldr_vt[LDR_IDX_RELEASEOBJ] = (void *)ldr_ReleaseObject;
    g_ldr_vt[LDR_IDX_CLEARCACHE] = stub_by_argc(2);
    g_ldr_vt[LDR_IDX_ENABLECACHE] = stub_by_argc(3);
    g_ldr_vt[LDR_IDX_COLLECTGARBAGE] = stub_by_argc(1);
    g_ldr_vt[13] = (void *)ldr_slot13; /* ba50 +0x34 */

    for (i = 0; i < SEG_VT; ++i)
        g_seg_vt[i] = stub_by_argc(g_seg_argc[i] ? g_seg_argc[i] : 2);
    g_seg_vt[0] = (void *)seg_QI;
    g_seg_vt[1] = (void *)seg_AddRef;
    g_seg_vt[2] = (void *)seg_Release;
    g_seg_vt[3] = (void *)seg_GetLength;
    g_seg_vt[4] = (void *)seg_SetLength;
    g_seg_vt[5] = (void *)seg_GetRepeats;
    g_seg_vt[6] = (void *)seg_SetRepeats;
    g_seg_vt[7] = (void *)seg_log2_7;
    g_seg_vt[8] = (void *)seg_log2_8;
    g_seg_vt[9] = (void *)seg_log5_9;
    g_seg_vt[10] = (void *)seg_log3_10;
    g_seg_vt[11] = (void *)seg_log3_11;
    g_seg_vt[12] = (void *)seg_log2_12;
    g_seg_vt[13] = (void *)seg_log4_13;
    g_seg_vt[14] = (void *)seg_log2_14;
    g_seg_vt[15] = (void *)seg_log2_15;
    g_seg_vt[16] = (void *)seg_InitPlay;
    g_seg_vt[17] = (void *)seg_log2_17;
    g_seg_vt[18] = (void *)seg_log7_18;
    g_seg_vt[19] = (void *)seg_log6_19;
    g_seg_vt[20] = (void *)seg_log4_20;
    g_seg_vt[21] = (void *)seg_log2_21;
    g_seg_vt[22] = (void *)seg_log2_22;
    g_seg_vt[23] = (void *)seg_log3_23;
    g_seg_vt[24] = (void *)seg_log3_24;
    g_seg_vt[25] = (void *)seg_log3_25;
    g_seg_vt[26] = (void *)seg_log6_26;
    g_seg_vt[27] = (void *)seg_log2_27;
    g_seg_vt[28] = (void *)seg_log4_28;
    g_seg_vt[SEG_IDX_DOWNLOAD] = (void *)seg_Download;
    g_seg_vt[SEG_IDX_UNLOAD] = (void *)seg_Unload;

    /* IDirectMusicAudioPath argc incl. This; slot5 SetVolume must be @12 (see H26). */
    {
        static const unsigned char path_argc[PATH_VT] = {3, 1, 1, 8, 2, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2};
        for (i = 0; i < PATH_VT; ++i)
            g_path_vt[i] = stub_by_argc(path_argc[i]);
    }
    g_path_vt[0] = (void *)path_QI;
    g_path_vt[1] = (void *)path_AddRef;
    g_path_vt[2] = (void *)path_Release;
    g_path_vt[3] = (void *)path_GetObjectInPath;
    g_path_vt[4] = (void *)path_Activate;
    g_path_vt[5] = (void *)path_SetVolume;
    g_path_vt[6] = (void *)path_ConvertPChannel;

    for (i = 0; i < STATE_VT; ++i)
        g_state_vt[i] = stub_by_argc(2); /* GetStartTime/Seek/etc are 2-arg */
    g_state_vt[0] = (void *)state_QI;
    g_state_vt[1] = (void *)state_AddRef;
    g_state_vt[2] = (void *)state_Release;
    g_state_vt[3] = (void *)state_GetRepeats;
    g_state_vt[4] = (void *)state_GetSegment;

    g_vt_ready = 1;
    (void)CLSID_DMSegment;
}

