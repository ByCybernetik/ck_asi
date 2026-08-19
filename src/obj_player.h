#ifndef OBJ_PLAYER_H
#define OBJ_PLAYER_H

#include <windows.h>

/* 0 = none/neutral; 1..13 = CONST.INI [PlayerColors] idN. */
int obj_mapobj_player_id(void *map_obj);
void obj_player_rgb(int player_id, float *r, float *g, float *b);
void obj_mapobj_player_debug(void *map_obj, DWORD *out_flags, unsigned *out_slot,
                            int *out_flag_id, int *out_slot_id);

#endif
