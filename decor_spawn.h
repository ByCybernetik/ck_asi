#ifndef DECOR_SPAWN_H
#define DECOR_SPAWN_H

#include <windows.h>

/* Captured PutDecor enqueue (DecorSpawn): packed = type|(sub<<8), world XY. */

typedef struct {
    unsigned packed;
    LONG x;
    LONG y;
} DecorSpawnItem;

enum { DECOR_SPAWN_MAX = 8192 };

void decor_spawn_clear(void);
void decor_spawn_push(unsigned packed, LONG x, LONG y);
int decor_spawn_count(void);
const DecorSpawnItem *decor_spawn_items(void);
unsigned decor_spawn_type(unsigned packed); /* packed & 0xff */
unsigned decor_spawn_sub(unsigned packed);  /* packed >> 8 */

/* DecorSpawn marks the current map epoch as "live" (retail enqueue). Editor never
 * calls DecorSpawn — then vk_decor refills from DIRG when epoch is not live. */
void decor_spawn_mark_epoch(LONG epoch);
int decor_spawn_epoch_live(LONG epoch);

/* Editor path: retail does not call DecorSpawn. Rebuild visible instances from
 * terrain.decor.grid DIRG. Prefer map layout; fall back to g_last_decor_dirg.
 * Returns count; optional *out_nz_all = non-zero cells in whole grid. */
int decor_spawn_fill_from_map_grid(void *map_self, LONG cam_L, LONG cam_T, LONG cam_R, LONG cam_B);
int decor_spawn_fill_from_map_grid_ex(void *map_self, LONG cam_L, LONG cam_T, LONG cam_R, LONG cam_B,
                                      int *out_nz_all, int *out_reason, void **out_decor);

#endif
