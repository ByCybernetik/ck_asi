#ifndef CK_DM_NATIVE_H
#define CK_DM_NATIVE_H

#include <windows.h>

/* Prefer DirectMusic/DirectSound DLLs from the game directory over Wine builtin. */
void dm_native_install(void);
void dm_native_remove(void);

#endif
