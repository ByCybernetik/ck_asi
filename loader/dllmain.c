/* Thin winmm proxy: forward real winmm + LoadLibrary(scripts/CK.asi). */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#include "proxy.h"

static HMODULE g_asi;
static FILE *g_log;

static void ldr_log(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;
    if (!g_log)
        return;
    GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", (unsigned)st.wHour, (unsigned)st.wMinute,
            (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void try_load_asi(HMODULE self)
{
    char base[MAX_PATH];
    char path[MAX_PATH];
    char *slash;
    DWORD n;
    static const char *const rels[] = {
        "scripts\\CK.asi",
        "scripts/CK.asi",
        "plugins\\CK.asi",
        "plugins/CK.asi",
        "CK.asi",
        NULL,
    };
    int i;

    n = GetModuleFileNameA(self, base, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    slash = strrchr(base, '\\');
    if (!slash)
        slash = strrchr(base, '/');
    if (slash)
        slash[1] = '\0';
    else
        base[0] = '\0';

    for (i = 0; rels[i]; ++i) {
        lstrcpyA(path, base);
        lstrcatA(path, rels[i]);
        g_asi = LoadLibraryA(path);
        if (g_asi) {
            ldr_log("loaded ASI: %s", path);
            return;
        }
    }
    ldr_log("WARN: CK.asi not found under %s (tried scripts/plugins/.)", base);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    char logpath[MAX_PATH];
    char *slash;
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        GetModuleFileNameA(hinst, logpath, MAX_PATH);
        slash = strrchr(logpath, '\\');
        if (!slash)
            slash = strrchr(logpath, '/');
        if (slash)
            slash[1] = '\0';
        else
            logpath[0] = '\0';
        lstrcatA(logpath, "ck_loader.log");
        g_log = fopen(logpath, "a");

        if (!proxy_load(hinst)) {
            ldr_log("proxy_load failed");
            return FALSE;
        }
        ldr_log("winmm loader ready — loading CK.asi");
        try_load_asi(hinst);
        break;

    case DLL_PROCESS_DETACH:
        if (g_asi) {
            FreeLibrary(g_asi);
            g_asi = NULL;
        }
        proxy_unload();
        if (g_log) {
            ldr_log("winmm loader detached");
            fclose(g_log);
            g_log = NULL;
        }
        break;
    }
    return TRUE;
}
