#ifndef KTX_TERRAIN_H
#define KTX_TERRAIN_H

#include <stddef.h>
#include <stdint.h>

/* GPU terrain cache under {game}/ktx/terrain/ (symlink ok).
 * CK_KTX_TERRAIN=0 disables. CK_KTX_DIR overrides root path.
 * CK_KTX_PREVIEW=1 stamps a BC1 decode into soft FB corner. */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t vk_format;
    uint32_t level0_off;
    uint32_t level0_len;
} Ktx2Info;

void ktx_terrain_init(void);
void ktx_terrain_shutdown(void);

int ktx_terrain_ready(void);
int ktx_terrain_count(void);
const char *ktx_terrain_root(void);

/* Map retail VQ key (e.g. TERRAIN\\SPRING\\GRASS512.VQ) → absolute .ktx2 path.
 * Returns 1 if file exists. */
int ktx_terrain_resolve_vq(const char *vq_key, char *out, size_t out_n);

/* Load KTX2 header + optional level0 into *blob (malloc). blob may be NULL. */
int ktx_terrain_load_info(const char *path, Ktx2Info *info, uint8_t **level0_out);

/* Decode BC1 RGB(A) blocks → tightly packed BGRA8 (malloc). */
uint8_t *ktx_terrain_decode_bc1_bgra(const uint8_t *blocks, uint32_t w, uint32_t h,
                                     uint32_t block_bytes);

/* Soft-FB corner preview (no-op unless CK_KTX_PREVIEW and ready). */
void ktx_terrain_preview_soft(uint8_t *bgra, int w, int h);

#endif
