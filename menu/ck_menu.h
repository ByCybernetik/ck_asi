#ifndef CK_MENU_H
#define CK_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* In-process RE main menu (sources under menu/, linked into CK.asi).
 * Blocks other threads while menu runs. Quit → ExitProcess.
 * Tutorial → handoff to retail open Adventures/Tutorial.BFHP (returns 2).
 * Returns 0 if skipped/failed. */
int ck_menu_run(const char *game_root_win);

#ifdef __cplusplus
}
#endif

#endif
