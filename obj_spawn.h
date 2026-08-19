#ifndef OBJ_SPAWN_H
#define OBJ_SPAWN_H

#include <windows.h>
#include <stdint.h>

enum { OBJ_SPAWN_MAX = 4096 };

/* Live map-object instances for GPU draw (buildings/units). */
typedef struct {
    char id[32];
    LONG x, y; /* logical map pos (CVXMapObj+0x22/+0x26) — discrete path steps */
    LONG from_x, from_y; /* visual lerp start (previous snap / mid-lerp) */
    DWORD move_dur_ms; /* retail Visible+0x8c: walk-clip / last SetPos interval */
    int frame;
    int facing; /* 0..7 sheet column */
    int moving; /* 1 = recently moved (use walk sheet) */
    int player; /* 0=none; 1..13 team from MapObj+0x6e */
    DWORD last_move_tick;
    void *map_obj; /* CVXMapObj* — tracking key; may be NULL */

    /* P0: template pack bound once per id (filled by vk_obj). */
    uint8_t tpl_bound;
    uint8_t has_walk;
    int tpl_begin;
    int tpl_count;

    /* P1: ground height cache — recompute only when XY changes. */
    uint8_t h_valid;
    LONG h_x, h_y;
    float ground_h;
} ObjSpawnItem;

/* Visual map XY for GPU draw — lerps between discrete SetPos snaps (exe path is ~32wu steps). */
void obj_spawn_vis_xy(const ObjSpawnItem *it, DWORD now, float *ox, float *oy);

void obj_spawn_clear(void);
void obj_spawn_mark_epoch(LONG epoch);
int obj_spawn_epoch_live(LONG epoch);

/* Upsert by map_obj pointer (or by id+xy if map_obj NULL). */
void obj_spawn_upsert(void *map_obj, const char *id, LONG x, LONG y, int frame, int player);
void obj_spawn_remove(void *map_obj);
/* Cheap: if map_obj already tracked, refresh XY from caller. Returns 1 if updated. */
int obj_spawn_update_pos(void *map_obj, LONG x, LONG y);

int obj_spawn_count(void);
const ObjSpawnItem *obj_spawn_items(void);
/* Mutable view for per-frame caches (tpl bind / height) — main thread only. */
ObjSpawnItem *obj_spawn_items_mut(void);

#endif
