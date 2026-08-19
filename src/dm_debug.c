#include "dm_replace_internal.h"

void dm_agent(const char *hid, const char *loc, const char *msg, const char *data_json)
{
    if (!msg)
        return;
    if (strcmp(msg, "crash-veh") && strcmp(msg, "crash-other") && strcmp(msg, "crash-ctx") &&
        strcmp(msg, "call-ring"))
        return;
    log_msg("dm-agent: id=%s loc=%s msg=%s data=%s", hid ? hid : "?", loc ? loc : "?",
            msg, data_json && data_json[0] ? data_json : "{}");
}

/* #region agent log — last COM hits before AV (no I/O on hot path) */
enum { CK_RING_N = 32 };
static struct {
    DWORD tick;
    DWORD tid;
    int kind; /* 1=seg 2=SetRep 3=DL 4=QI 5=GetNotif 6=Play 7=GetLen 8=path */
    int slot;
} g_ck_ring[CK_RING_N];
static volatile LONG g_ck_ring_i;

void ck_ring_push(int kind, int slot)
{
    LONG i = InterlockedIncrement(&g_ck_ring_i) - 1;
    int j = (int)(i & (CK_RING_N - 1));
    g_ck_ring[j].tick = GetTickCount();
    g_ck_ring[j].tid = GetCurrentThreadId();
    g_ck_ring[j].kind = kind;
    g_ck_ring[j].slot = slot;
}

void ck_ring_dump(const char *why)
{
    char js[720];
    int n = (int)g_ck_ring_i;
    int start = n > CK_RING_N ? n - CK_RING_N : 0;
    int pos;
    int k;
    pos = snprintf(js, sizeof(js), "{\"why\":\"%.24s\",\"n\":%d,\"hits\":[", why ? why : "?", n);
    for (k = start; k < n && pos + 40 < (int)sizeof(js); ++k) {
        int j = k & (CK_RING_N - 1);
        pos += snprintf(js + pos, sizeof(js) - (size_t)pos, "%s{\"t\":%lu,\"tid\":%lu,\"k\":%d,\"s\":%d}",
                        k > start ? "," : "", (unsigned long)g_ck_ring[j].tick,
                        (unsigned long)g_ck_ring[j].tid, g_ck_ring[j].kind, g_ck_ring[j].slot);
    }
    if (pos + 3 < (int)sizeof(js)) {
        js[pos++] = ']';
        js[pos++] = '}';
        js[pos] = '\0';
    }
    dm_agent("H11", "dm_replace.c:ring", "call-ring", js);
}
/* #endregion */

/* #region agent log */
LONG CALLBACK ck_veh(struct _EXCEPTION_POINTERS *ep)
{
    static volatile LONG s_av_n;
    char js[280];
    char ctx[360];
    char modpath[MAX_PATH];
    DWORD code, addr, eip, info0 = 0, info1 = 0;
    DWORD mod_base = 0, eip_rva = 0;
    LONG n;
    MEMORY_BASIC_INFORMATION mbi;
    HMODULE hm;

    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    code = ep->ExceptionRecord->ExceptionCode;
    /* DBG_PRINTEXCEPTION_C / OUTPUT_DEBUG_STRING and C++ EH — ignore */
    if (code == 0x40010006 || code == 0x40010007 || code == 0xE06D7363 || code == 0x406D1388)
        return EXCEPTION_CONTINUE_SEARCH;
    /* #region agent log — H70: non-AV fatals (stack/illegal) before exit */
    if (code != 0xC0000005) {
        if (code == 0xC00000FD || code == 0xC000001D || code == 0xC0000096 || code == 0x80000003) {
            n = InterlockedIncrement(&s_av_n);
            addr = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
            eip = ep->ContextRecord ? (DWORD)ep->ContextRecord->Eip : 0;
            hooks_get_crash_ctx(ctx, sizeof(ctx));
            log_msg("VEH-other #%ld code=0x%08lX eip=0x%08lX %s", (long)n, (unsigned long)code,
                    (unsigned long)eip, ctx);
            if (n <= 8) {
                snprintf(js, sizeof(js),
                         "{\"n\":%ld,\"code\":%lu,\"addr\":%lu,\"eip\":%lu,\"eip_hex\":\"0x%08lX\"}",
                         (long)n, (unsigned long)code, (unsigned long)addr, (unsigned long)eip,
                         (unsigned long)eip);
                dm_agent("H70", "dm_replace.c:VEH", "crash-other", js);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    /* #endregion */
    n = InterlockedIncrement(&s_av_n);
    addr = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    eip = ep->ContextRecord ? (DWORD)ep->ContextRecord->Eip : 0;
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        info0 = (DWORD)ep->ExceptionRecord->ExceptionInformation[0];
        info1 = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
    }
    modpath[0] = '\0';
    if (VirtualQuery((void *)(ULONG_PTR)eip, &mbi, sizeof(mbi))) {
        mod_base = (DWORD)(ULONG_PTR)mbi.AllocationBase;
        if (mod_base)
            eip_rva = eip - mod_base;
        hm = (HMODULE)mbi.AllocationBase;
        if (hm)
            GetModuleFileNameA(hm, modpath, MAX_PATH);
    }
    {
        char *p;
        for (p = modpath; *p; ++p) {
            if (*p == '\\' || *p == '"')
                *p = '/';
        }
    }
    hooks_get_crash_ctx(ctx, sizeof(ctx));
    /* kernel32 probe AVs are extremely hot with DI Wait patch — don't flood ck_asi.log. */
    if (modpath[0] && strstr(modpath, "kernel32") && n > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    /* Always write ck_asi.log (NDJSON capped) — editor test AVs were lost after n>3. */
    log_msg("VEH-AV #%ld eip=0x%08lX rva=0x%08lX av=0x%08lX rw=%lu mod=%s | %s", (long)n,
            (unsigned long)eip, (unsigned long)eip_rva, (unsigned long)info1, (unsigned long)info0,
            modpath[0] ? modpath : "?", ctx);
    if (n > 12)
        return EXCEPTION_CONTINUE_SEARCH;
    /* #region agent log — H22/H24: dump regs+stack at AV */
    {
        char js2[640];
        CONTEXT *c = ep->ContextRecord;
        DWORD *sp;
        DWORD s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
        if (c) {
            sp = (DWORD *)(ULONG_PTR)c->Esp;
            if (!IsBadReadPtr(sp, 32)) {
                s0 = sp[0];
                s1 = sp[1];
                s2 = sp[2];
                s3 = sp[3];
                s4 = sp[4];
                s5 = sp[5];
                s6 = sp[6];
                s7 = sp[7];
            }
            /* #region agent log */
            {
                extern volatile LONG g_scan_clamps;
                snprintf(js2, sizeof(js2),
                         "{\"n\":%ld,\"eip\":%lu,\"eip_hex\":\"0x%08lX\",\"rva\":%lu,"
                         "\"mod_base\":%lu,\"mod\":\"%.160s\",\"eax\":%lu,\"ebx\":%lu,\"ecx\":%lu,"
                         "\"edx\":%lu,\"esi\":%lu,\"edi\":%lu,\"ebp\":%lu,\"esp\":%lu,\"rw\":%lu,"
                         "\"av\":%lu,\"s0\":%lu,\"s1\":%lu,\"s2\":%lu,\"s3\":%lu,\"s4\":%lu,"
                         "\"s5\":%lu,\"s6\":%lu,\"s7\":%lu,\"scan_clamps\":%ld,\"ctx\":\"%.200s\"}",
                         (long)n, (unsigned long)eip, (unsigned long)eip, (unsigned long)eip_rva,
                         (unsigned long)mod_base, modpath, (unsigned long)c->Eax,
                         (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx,
                         (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp,
                         (unsigned long)c->Esp, (unsigned long)info0, (unsigned long)info1,
                         (unsigned long)s0, (unsigned long)s1, (unsigned long)s2, (unsigned long)s3,
                         (unsigned long)s4, (unsigned long)s5, (unsigned long)s6, (unsigned long)s7,
                         (long)g_scan_clamps, ctx);
                dm_agent("H24", "dm_replace.c:VEH", "crash-ctx", js2);
            }
            /* #endregion */
        }
    }
    /* #endregion */
    snprintf(js, sizeof(js),
             "{\"n\":%ld,\"code\":%lu,\"addr\":%lu,\"eip\":%lu,\"eip_hex\":\"0x%08lX\","
             "\"rva\":%lu,\"rw\":%lu,\"av_addr\":%lu,\"mod\":\"%.120s\"}",
             (long)n, (unsigned long)code, (unsigned long)addr, (unsigned long)eip,
             (unsigned long)eip, (unsigned long)eip_rva, (unsigned long)info0,
             (unsigned long)info1, modpath);
    dm_agent("H10", "dm_replace.c:VEH", "crash-veh", js);
    ck_ring_dump("veh-av");
    return EXCEPTION_CONTINUE_SEARCH;
}
/* #endregion */
