#ifndef WINMM_PROXY_PROXY_H
#define WINMM_PROXY_PROXY_H

#include <windows.h>

BOOL proxy_load(HMODULE self);
void proxy_unload(void);
HMODULE proxy_real_module(void);

#endif
