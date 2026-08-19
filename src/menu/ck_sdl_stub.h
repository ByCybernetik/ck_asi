/* Stub SDL used by native ui when built into CK.asi (no libSDL). */
#pragma once

#include <windows.h>
#include <stdio.h>

#define SDL_Log(...) ((void)0)

static inline unsigned int SDL_GetTicks(void)
{
    return (unsigned int)GetTickCount();
}
