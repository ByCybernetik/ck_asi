#ifndef WINMM_PROXY_LOG_H
#define WINMM_PROXY_LOG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NO_LOG
#define log_init(self) ((void)0)
#define log_shutdown() ((void)0)
#define log_msg(...) ((void)0)
#define log_guid(label, guid) ((void)0)
#else
void log_init(HMODULE self);
void log_shutdown(void);
void log_msg(const char *fmt, ...);
void log_guid(const char *label, const GUID *guid);
#endif

#ifdef __cplusplus
}
#endif

#endif
