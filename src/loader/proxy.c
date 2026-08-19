#include "proxy.h"

#include <stdio.h>
#include <string.h>

HMODULE g_real_winmm;

BOOL proxy_load(HMODULE self)
{
    char sysdir[MAX_PATH];
    char selfpath[MAX_PATH];
    char path[MAX_PATH];
    UINT n;
    char *slash;

    n = GetSystemDirectoryA(sysdir, MAX_PATH);
    if (!n || n >= MAX_PATH - 12)
        return FALSE;

    GetModuleFileNameA(self, selfpath, MAX_PATH);
    wsprintfA(path, "%s\\winmm.dll", sysdir);

    if (lstrcmpiA(path, selfpath) == 0) {
        slash = strrchr(selfpath, '\\');
        if (!slash)
            slash = strrchr(selfpath, '/');
        if (slash) {
            slash[1] = '\0';
            lstrcpyA(path, selfpath);
            lstrcatA(path, "winmm_orig.dll");
        } else {
            lstrcpyA(path, "winmm_orig.dll");
        }
    }

    g_real_winmm = LoadLibraryA(path);
    if (!g_real_winmm) {
        GetModuleFileNameA(self, selfpath, MAX_PATH);
        slash = strrchr(selfpath, '\\');
        if (!slash)
            slash = strrchr(selfpath, '/');
        if (slash) {
            slash[1] = '\0';
            lstrcpyA(path, selfpath);
            lstrcatA(path, "winmm_orig.dll");
            g_real_winmm = LoadLibraryA(path);
        }
    }

    if (!g_real_winmm)
        return FALSE;

#define BIND(name) \
    do { \
        extern void *p_##name; \
        p_##name = (void *)GetProcAddress(g_real_winmm, #name); \
    } while (0)

#include "bind_trampolines.inc"

#undef BIND
    (void)path;
    return TRUE;
}

void proxy_unload(void)
{
    if (g_real_winmm) {
        FreeLibrary(g_real_winmm);
        g_real_winmm = NULL;
    }
}

HMODULE proxy_real_module(void)
{
    return g_real_winmm;
}
