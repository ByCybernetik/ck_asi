#ifndef CK_MUSIC_H
#define CK_MUSIC_H

#include <windows.h>
#include <dsound.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CkMusicEndCallback)(void *ctx);

int  ck_music_init(LPDIRECTSOUND ds);
void ck_music_shutdown(void);

int  ck_music_play(const char *path, int loop, LONG vol_mb);
void ck_music_stop(void);
int  ck_music_is_playing(void);
void ck_music_set_volume(LONG vol_mb);
void ck_music_set_end_callback(CkMusicEndCallback cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
