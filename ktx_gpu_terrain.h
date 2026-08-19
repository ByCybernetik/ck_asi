#ifndef KTX_GPU_TERRAIN_H
#define KTX_GPU_TERRAIN_H

#include <stdint.h>

/* Soft-FB AABB overpaint (experimental). Not retail VQ texture replacement.
 * CK_GPU_TERRAIN_OVERPAINT=1 enables. */

void ktx_gpu_terrain_init(void);
void ktx_gpu_terrain_shutdown(void);

/* Paint visible z-grid tiles into soft BGRA (screen space). */
void ktx_gpu_terrain_paint_soft(uint8_t *bgra, int w, int h);

#endif
