#ifndef WINMM_PROXY_LOG_H
#define WINMM_PROXY_LOG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void log_init(HMODULE self);
void log_shutdown(void);
void log_msg(const char *fmt, ...);
void log_guid(const char *label, const GUID *guid);

#ifdef __cplusplus
}
#endif

#endif
