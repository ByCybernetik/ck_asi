#include <windows.h>
#include <stdio.h>

#include "log.h"
#include "hooks.h"
#include "hitch.h"
#include "dm_native.h"
#include "dm_replace.h"
#include "dm_trace.h"

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        log_init(hinst);
        hitch_init();
        log_msg("CK.asi attached — installing hooks");
        dm_replace_install();
        dm_native_install();
        dm_trace_install();
        hooks_install();
        break;

    case DLL_PROCESS_DETACH:

        hooks_remove();
        dm_native_remove();
        hitch_shutdown();
        log_shutdown();
        break;
    }
    return TRUE;
}
