#ifndef WINMM_PROXY_MOVIE_PLAYER_H
#define WINMM_PROXY_MOVIE_PLAYER_H

#include <windows.h>

/* Play cutscene via Wine quartz→winegstreamer decode, present via Vulkan.
 * Returns 1 on success (played or skipped), 0 on hard failure. */
int ck_movie_play(const char *path);

int ck_movie_active(void);

/* Resolve game movie path (avi/webm). Returns 1 if a readable file was found. */
int ck_movie_resolve(const char *in, char *out, size_t out_sz);

/* 1 if resolved path is .webm (VP9 — own decode → Vulkan via ck_webm_dec). */
int ck_movie_is_webm_path(const char *path);

/* Own WebM decode (TCP → Linux libav daemon) → Vulkan + waveOut. */
int play_webm_own_vulkan(const char *path_a, HWND hwnd);

#endif
