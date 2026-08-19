#ifndef CK_DM_REPLACE_H
#define CK_DM_REPLACE_H

#include <windows.h>
#include <objbase.h>

/* COM stub replacing DirectMusic Performance/Loader (default ON; CK_DM_REPLACE=0 disables). */
void dm_replace_install(void);
int dm_replace_enabled(void);
HRESULT dm_replace_cocreate(REFCLSID clsid, REFIID iid, void **ppv);
/* Silence looping BGM (cutscenes). */
void dm_replace_silence_music(void);
/* Per-frame spatial update for unit/building voices (+ optional CK_AUDIO_BEACON loop). */
void dm_replace_beacon_tick(void);

#endif
