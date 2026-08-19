#include "hooks.h"
#include "hooks_internal.h"
#include "ktx_terrain.h"
#include "ktx_decor.h"
#include "ktx_obj.h"
#include "ktx_gpu_terrain.h"
#include "ktx_vq_replace.h"
#include "log.h"
#include "vk_present.h"

#include <windows.h>
#include <stdio.h>

void hooks_install(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    char exe[MAX_PATH];
    int is_tpw;

    /* Freeze A/B: min profile. DI Wait(100)→1 on by default (see hooks_hitch). */
    g_proxy_min = !env_is("CK_PROXY_PROFILE", "full");
    g_di_patch = env_on("CK_DI_PATCH", 1);
    g_hitch_wait = env_on("CK_HITCH_WAIT", 0);
    g_terrain_trace = env_on("CK_TERRAIN_TRACE", 0);
    g_force_res = env_on("CK_FORCE_RES", 0);

    resolve_game_root();
    exe[0] = '\0';
    if (game)
        GetModuleFileNameA(game, exe, MAX_PATH);
    is_tpw = looks_like_tpw();
    log_msg("hooks_install: game_root=%s exe=%s tpw=%d ck=%d profile=%s di_patch=%d force_res=%d",
            g_game_root, exe, is_tpw, looks_like_celtic_kings(), g_proxy_min ? "min" : "full",
            g_di_patch, g_force_res);
    (void)is_tpw;

    /* Prefer RE MainMenu linked into CK.asi over retail UI. */
    hooks_native_menu_maybe();

    hooks_vfs_install();
    hooks_hitch_install();
    hooks_video_install();
    hooks_terrain_install();
    hooks_minimap_install();
    hooks_zoom_install();
    hooks_cam_smooth_install();
    hooks_video_movies_install();
    ktx_terrain_init();
    ktx_decor_init();
    ktx_obj_init();
    hooks_obj_install();
    ktx_vq_replace_install();
    ktx_gpu_terrain_init();

    log_msg("hooks installed (pipeline-trace Celtic_Kings/tpw)");
}

void hooks_remove(void)
{
    vk_present_shutdown();
    ktx_gpu_terrain_shutdown();
    ktx_vq_replace_remove();
    hooks_obj_remove();
    ktx_obj_shutdown();
    ktx_decor_shutdown();
    ktx_terrain_shutdown();
    hooks_vfs_remove_iat();
    hooks_hitch_remove();
    hooks_video_remove();
    hooks_vfs_remove();
    hooks_terrain_remove();
    hooks_minimap_remove();
    hooks_zoom_remove();
    hooks_cam_smooth_remove();
}
