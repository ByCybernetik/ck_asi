/* In-process RE menu from sources linked into CK.asi (not external ELF). */
#include "hooks_internal.h"
#include "log.h"
#include "menu/ck_menu.h"

#include <stdio.h>
#include <windows.h>

static DWORD WINAPI native_menu_thread(void *param)
{
    (void)param;
    Sleep(500); /* leave DllMain; let video mode create tpw HWND */
    if (!g_game_root[0]) {
        log_msg("native_menu: empty game_root");
        return 0;
    }
    log_msg("native_menu: starting embedded ck_menu_run root=%s", g_game_root);
    /* #region agent log */
    {
        FILE *df = fopen("Z:\\home\\cybernetik\\Games\\Imperivm\\ck_asi\\.cursor\\debug-764ba7.log", "a");
        if (df) {
            fprintf(df,
                    "{\"sessionId\":\"764ba7\",\"runId\":\"embed1\",\"hypothesisId\":\"H-EMBED\","
                    "\"location\":\"hooks_native_menu.c\",\"message\":\"ck_menu_run\","
                    "\"data\":{\"root\":\"%.200s\"},\"timestamp\":%lu}\n",
                    g_game_root, (unsigned long)GetTickCount());
            fclose(df);
        }
    }
    /* #endregion */
    ck_menu_run(g_game_root);
    return 0;
}

void hooks_native_menu_maybe(void)
{
    /* Off by default — retail CVXUIMainMenu. Set CK_NATIVE_MENU=1 to re-enable RE host. */
    if (!env_on("CK_NATIVE_MENU", 0)) {
        log_msg("native_menu: skipped (retail menu)");
        return;
    }
    if (!looks_like_tpw()) {
        log_msg("native_menu: skipped (not tpw.exe)");
        return;
    }
    if (!CreateThread(NULL, 0, native_menu_thread, NULL, 0, NULL))
        log_msg("native_menu: CreateThread failed (%lu)", GetLastError());
    else
        log_msg("native_menu: embedded menu thread armed");
}
