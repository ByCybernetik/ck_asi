#ifndef KTX_VQ_REPLACE_H
#define KTX_VQ_REPLACE_H

/* Hook LoadVQBitmap and optionally rebuild CVQBitmap pixels from ktx/terrain.
 * CK_KTX_VQ_REPLACE=1 enables substitution (default off — soft uses retail VQ).
 * CK_WATER_SMOOTH=1 inserts interpolated frames into water strips for smoother anim. */

void ktx_vq_replace_install(void);
void ktx_vq_replace_remove(void);

/* Patch layer frames/frame_h for expanded water VQs (call from terrain present/draw). */
void ktx_vq_replace_fix_layers(void);

/* Season folder inferred from LoadVQBitmap paths ("spring"/"autumn"/"winter").
 * Empty string if not seen yet — GPU should fall back to global. */
const char *ktx_vq_detected_season(void);

#endif
