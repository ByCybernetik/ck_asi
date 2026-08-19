#ifndef KTX_OBJ_H
#define KTX_OBJ_H

#include "ktx_decor.h"
#include <stddef.h>

/* GPU object atlases under {ktx}/objects/{buildings,units}/.
 * Same sprite schema as decors (ck_decor_atlas_v1). On by default (CK_GPU_OBJ=1);
 * set 0 for soft MapObj only. */

typedef KtxDecorSprite KtxObjSprite;
typedef KtxDecorAtlas KtxObjAtlas;

void ktx_obj_init(void);
void ktx_obj_shutdown(void);
int ktx_obj_wanted(void);
int ktx_obj_ready(void);
const char *ktx_obj_root(void);

int ktx_obj_sprite_count(void);
const KtxObjSprite *ktx_obj_sprites(void);
int ktx_obj_atlas_count(void);
const KtxObjAtlas *ktx_obj_atlases(void);
int ktx_obj_atlas_path(int atlas_index, char *out, size_t out_n);
/* Companion R8 material map path (BuildRamp indices). 0 if atlas has none. */
int ktx_obj_atlas_idx_path(int atlas_index, char *out, size_t out_n);

/* Linked list of sprite indices for an id (O(chain) vs O(all sprites)). */
int ktx_obj_spr_head(const char *id);
int ktx_obj_spr_next(int si);

/* Case-insensitive id match; prefer non-shadow layer at frame (or closest). */
const KtxObjSprite *ktx_obj_lookup(const char *id, int frame);
int ktx_obj_has_id(const char *id);
/* Resolve retail class id → catalog sprite id (altid/entity aliases). */
int ktx_obj_resolve_id(const char *id, char *out, size_t out_n);
int ktx_obj_sprite_is_shadow(const KtxObjSprite *sp);
/* Retail BuildRamp eligibility (IMGRLE ver2 + player_color or normal/remaping=none). */
int ktx_obj_sprite_wants_pc(const KtxObjSprite *sp);

/* 1 if id has any atlas that fits the GPU texture array (≤2048×3072).
 * BC3 + player_color bodies return 0: DXT alpha destroys ramp indices → soft path. */
int ktx_obj_id_fits_gpu(const char *id);

/* 1 if a non-shadow layer needs BuildRamp and lives on a BC3 (non-RGBA) atlas. */
int ktx_obj_id_needs_soft_pc(const char *id);

#endif
