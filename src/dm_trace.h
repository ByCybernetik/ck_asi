#ifndef CK_DM_TRACE_H
#define CK_DM_TRACE_H

#include <windows.h>

/* Deep DirectMusic/DirectSound runtime tracer. */
void dm_trace_install(void);
void dm_trace_on_cocreate(REFCLSID clsid, void *iface);

#endif
