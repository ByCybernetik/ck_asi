#include "obj_player.h"

#include <windows.h>

enum {
    CK_PLAYER_TABLE = 0x008DD088u,
    CK_PC_SLOT = 0x0CD4u,
    CK_PC_RAMP = 0x0C48u,
    CK_PC_STRIDE = 0x25Eu,
    /* SetPlayer @ 0x4f80e0: (1<<player) in low 16 of +0x2a;
     * ramp slot i at base+0xC48+i*0x25E is CONST id(i+1) (BuildRamp H-PCN). */
    CK_MAPOBJ_FLAGS = 0x2Au,
    CK_MAPOBJ_PLAYER_PTR = 0x6Eu,
};

/* CONST.INI [PlayerColors] id1..id13 */
static const unsigned char k_const_rgb[14][3] = {
    {128, 128, 128},
    {255, 23, 23},   {255, 255, 0},   {0, 255, 0},     {0, 232, 232},
    {237, 160, 255}, {182, 230, 134}, {174, 0, 0},     {249, 155, 32},
    {16, 155, 18},   {0, 0, 248},     {196, 13, 198},  {159, 154, 0},
    {128, 128, 128},
};

static int page_ok(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD prot;
    if (!p || (ULONG_PTR)p < 0x10000u)
        return 0;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return 0;
    prot = mbi.Protect & 0xFFu;
    if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
        prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE &&
        prot != PAGE_EXECUTE_WRITECOPY)
        return 0;
    return (ULONG_PTR)p + n <= (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
}

/* Lowest set bit in low 16 of flags → 0-based player index, or -1. */
static int player_bit_from_flags(DWORD flags)
{
    DWORD bits = flags & 0xFFFFu;
    int i;
    if (!bits)
        return -1;
    for (i = 0; i < 16; ++i) {
        if (bits & (1u << i))
            return i;
    }
    return -1;
}

static int player_from_slot(BYTE *slot, BYTE *base)
{
    unsigned off;
    int id;

    if (!slot || !page_ok(slot, 12))
        return 0;
    id = *(int *)(slot + 8);
    if (id >= 0 && id <= 12)
        return id + 1; /* 0-based → CONST id */
    if (id >= 1 && id <= 13)
        return id;
    if (base && page_ok(base + CK_PC_SLOT, 4) &&
        (ULONG_PTR)slot >= (ULONG_PTR)(base + CK_PC_SLOT)) {
        off = (unsigned)(slot - (base + CK_PC_SLOT));
        if ((off % CK_PC_STRIDE) == 0) {
            id = (int)(off / CK_PC_STRIDE);
            if (id >= 0 && id <= 12)
                return id + 1;
        }
    }
    return 0;
}

int obj_mapobj_player_id(void *map_obj)
{
    BYTE *base, *slot;
    DWORD flags;
    int bit, id;

    if (!map_obj || !page_ok(map_obj, CK_MAPOBJ_PLAYER_PTR + 4))
        return 0;

    flags = *(DWORD *)((BYTE *)map_obj + CK_MAPOBJ_FLAGS);
    bit = player_bit_from_flags(flags);
    /* Human players 0..12 → CONST id1..id13. Bits 13..15 = gaia/animals — no tint. */
    if (bit >= 0 && bit <= 12)
        return bit + 1;

    slot = *(BYTE **)((BYTE *)map_obj + CK_MAPOBJ_PLAYER_PTR);
    base = *(BYTE **)(ULONG_PTR)CK_PLAYER_TABLE;
    id = player_from_slot(slot, base);
    if (id >= 1 && id <= 13)
        return id;
    return 0;
}

void obj_mapobj_player_debug(void *map_obj, DWORD *out_flags, unsigned *out_slot,
                             int *out_flag_id, int *out_slot_id)
{
    BYTE *base, *slot;
    DWORD flags = 0;
    int bit = -1, sid = 0;
    unsigned slot_u = 0;

    if (map_obj && page_ok(map_obj, CK_MAPOBJ_PLAYER_PTR + 4)) {
        flags = *(DWORD *)((BYTE *)map_obj + CK_MAPOBJ_FLAGS);
        bit = player_bit_from_flags(flags);
        slot = *(BYTE **)((BYTE *)map_obj + CK_MAPOBJ_PLAYER_PTR);
        if (slot)
            slot_u = (unsigned)(ULONG_PTR)slot;
        base = *(BYTE **)(ULONG_PTR)CK_PLAYER_TABLE;
        sid = player_from_slot(slot, base);
    }
    if (out_flags)
        *out_flags = flags;
    if (out_slot)
        *out_slot = slot_u;
    if (out_flag_id)
        *out_flag_id = bit; /* raw bit index, may be 0..15 */
    if (out_slot_id)
        *out_slot_id = sid;
}

void obj_player_rgb(int player_id, float *r, float *g, float *b)
{
    if (!r || !g || !b)
        return;
    *r = *g = *b = 0.5f;
    if (player_id < 1 || player_id > 13)
        player_id = 1;

    /* Base CONST / PlayerColors RGB for GPU BuildRamp (not ramp[0] shade). */
    *r = (float)k_const_rgb[player_id][0] / 255.0f;
    *g = (float)k_const_rgb[player_id][1] / 255.0f;
    *b = (float)k_const_rgb[player_id][2] / 255.0f;
}
