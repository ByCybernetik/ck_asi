#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_log;
static CRITICAL_SECTION g_cs;
static int g_cs_ready;

void log_init(HMODULE self)
{
    char path[MAX_PATH];
    char *slash;
    DWORD n;

    InitializeCriticalSection(&g_cs);
    g_cs_ready = 1;

    n = GetModuleFileNameA(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        lstrcpyA(path, "ck_asi.log");
    else {
        slash = strrchr(path, '\\');
        if (!slash)
            slash = strrchr(path, '/');
        if (slash)
            slash[1] = '\0';
        else
            path[0] = '\0';
        lstrcatA(path, "ck_asi.log");
    }

    g_log = fopen(path, "a");
    if (g_log) {
        setvbuf(g_log, NULL, _IONBF, 0);
        log_msg("==== CK.asi attached ====");
    }
}

void log_shutdown(void)
{
    if (g_log) {
        log_msg("==== CK.asi detached ====");
        fclose(g_log);
        g_log = NULL;
    }
    if (g_cs_ready) {
        DeleteCriticalSection(&g_cs);
        g_cs_ready = 0;
    }
}

void log_msg(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;

    if (!g_log)
        return;

    GetLocalTime(&st);
    if (g_cs_ready)
        EnterCriticalSection(&g_cs);

    fprintf(g_log, "[%02u:%02u:%02u.%03u] ",
            (unsigned)st.wHour, (unsigned)st.wMinute,
            (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);

    if (g_cs_ready)
        LeaveCriticalSection(&g_cs);
}

void log_guid(const char *label, const GUID *guid)
{
    if (!guid) {
        log_msg("%s: (null)", label);
        return;
    }
    log_msg("%s: {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            label,
            (unsigned)guid->Data1, (unsigned)guid->Data2, (unsigned)guid->Data3,
            guid->Data4[0], guid->Data4[1],
            guid->Data4[2], guid->Data4[3],
            guid->Data4[4], guid->Data4[5],
            guid->Data4[6], guid->Data4[7]);
}
