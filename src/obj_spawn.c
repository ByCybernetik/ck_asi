#include "obj_spawn.h"
#include "obj_player.h"

#include <stdio.h>
#include <string.h>

static ObjSpawnItem s_items[OBJ_SPAWN_MAX];
static volatile LONG s_n;
static volatile LONG s_live_epoch = -1;
static CRITICAL_SECTION s_cs;
static int s_cs_ok;

static void ensure_cs(void)
{
    if (s_cs_ok)
        return;
    InitializeCriticalSection(&s_cs);
    s_cs_ok = 1;
}

/* Map delta → 8 facing columns (0=S, then clockwise). Good enough for sheet cols. */
static int facing_from_delta(LONG dx, LONG dy)
{
    int ax, ay;
    if (dx == 0 && dy == 0)
        return -1;
    ax = dx < 0 ? -dx : dx;
    ay = dy < 0 ? -dy : dy;
    if (ay * 2 <= ax) {
        /* mostly horizontal */
        return dx > 0 ? 2 : 6; /* E / W */
    }
    if (ax * 2 <= ay) {
        /* mostly vertical (map Y down-ish) */
        return dy > 0 ? 0 : 4; /* S / N */
    }
    if (dx > 0 && dy > 0)
        return 1; /* SE */
    if (dx > 0 && dy < 0)
        return 3; /* NE */
    if (dx < 0 && dy < 0)
        return 5; /* NW */
    return 7; /* SW */
}

/* Retail Visible::Draw: vis = from + delta * elapsed / dur (linear, no smoothstep).
 * dur is the walk clip (~449–666 ms villagers, ~1133 ms hen) ≈ last SetPos interval. */
static DWORD step_dur_ms(const ObjSpawnItem *it, DWORD now)
{
    DWORD dt;
    /* Prefer time since previous SetPos — that is this unit's walk-clip cadence. */
    if (it->last_move_tick) {
        dt = now - it->last_move_tick;
        if (dt >= 180u && dt <= 2500u)
            return dt;
    }
    if (it->move_dur_ms >= 180u && it->move_dur_ms <= 2500u)
        return it->move_dur_ms;
    return 500u;
}

static void apply_move(ObjSpawnItem *it, LONG x, LONG y, void *map_obj)
{
    LONG dx = x - it->x;
    LONG dy = y - it->y;
    int face;
    DWORD now, dur;
    float vx, vy;
    LONG adx, ady;

    if (dx == 0 && dy == 0)
        return;

    now = GetTickCount();
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;
    /* Spawn / warp — don't crawl across the map. Wolf steps were ≤76 wu. */
    if (adx > 96 || ady > 96) {
        it->from_x = x;
        it->from_y = y;
        it->x = x;
        it->y = y;
        it->h_valid = 0;
        it->moving = 0;
        it->last_move_tick = now;
        it->move_dur_ms = 0;
        return;
    }
    /* 1wu SetPos noise must not start a walk clip. */
    if (adx <= 2 && ady <= 2) {
        it->x = x;
        it->y = y;
        if (!it->moving) {
            it->from_x = x;
            it->from_y = y;
        }
        it->h_valid = 0;
        return;
    }

    /* Continue from current visual if mid-lerp (avoids snap-back on new step). */
    obj_spawn_vis_xy(it, now, &vx, &vy);
    it->from_x = (LONG)(vx + (vx >= 0.f ? 0.5f : -0.5f));
    it->from_y = (LONG)(vy + (vy >= 0.f ? 0.5f : -0.5f));
    dur = step_dur_ms(it, now);
    it->x = x;
    it->y = y;
    it->h_valid = 0;
    it->moving = 1;
    it->move_dur_ms = dur;
    it->last_move_tick = now;

    face = -1;
    if (map_obj && !IsBadReadPtr(map_obj, 0x50)) {
        SHORT dir_x = *(SHORT *)((BYTE *)map_obj + 0x46);
        SHORT dir_y = *(SHORT *)((BYTE *)map_obj + 0x4a);
        face = facing_from_delta((LONG)dir_x, (LONG)(-(int)dir_y));
    }
    if (face < 0 && (dx > -512 && dx < 512) && (dy > -512 && dy < 512))
        face = facing_from_delta(dx, (LONG)(-(int)dy));
    /* Retail Visible+0x98 = (n/2 + FaceFn(dir_x,-dir_y)) % n. 8-col sheets → +4. */
    if (face >= 0)
        it->facing = (face + 4) & 7;
    /* #region agent log */
    {
        static unsigned s_nlog;
        if (s_nlog < 48u) {
            FILE *f = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
            if (f) {
                fprintf(f,
                    "{\"sessionId\":\"764ba7\",\"runId\":\"post-fix\",\"hypothesisId\":\"H-F1\","
                    "\"location\":\"obj_spawn.c:apply_move\",\"message\":\"setpos_step\","
                    "\"data\":{\"dx\":%ld,\"dy\":%ld,\"from\":[%ld,%ld],\"to\":[%ld,%ld],"
                    "\"vis0\":[%.1f,%.1f],\"lerp_ms\":%u,\"face_raw\":%d,\"face\":%d},\"timestamp\":%lu}\n",
                    (long)dx, (long)dy, (long)it->from_x, (long)it->from_y, (long)x, (long)y,
                    (double)vx, (double)vy, (unsigned)dur, face, it->facing, (unsigned long)now);
                fclose(f);
                ++s_nlog;
            }
        }
    }
    /* #endregion */
}

void obj_spawn_vis_xy(const ObjSpawnItem *it, DWORD now, float *ox, float *oy)
{
    LONG dx, dy;
    DWORD dur, age;

    if (!it || !ox || !oy)
        return;
    *ox = (float)it->x;
    *oy = (float)it->y;
    if (!it->last_move_tick || !it->moving)
        return;
    dx = it->x - it->from_x;
    dy = it->y - it->from_y;
    if (dx == 0 && dy == 0)
        return;
    dur = it->move_dur_ms;
    if (dur < 1u)
        dur = 500u;
    age = now - it->last_move_tick;
    if (age >= dur)
        return;
    /* Integer-linear like exe: from + delta * elapsed / dur (no smoothstep). */
    *ox = (float)it->from_x + (float)dx * ((float)age / (float)dur);
    *oy = (float)it->from_y + (float)dy * ((float)age / (float)dur);
}

void obj_spawn_clear(void)
{
    ensure_cs();
    EnterCriticalSection(&s_cs);
    s_n = 0;
    LeaveCriticalSection(&s_cs);
}

void obj_spawn_mark_epoch(LONG epoch)
{
    InterlockedExchange(&s_live_epoch, epoch);
}

int obj_spawn_epoch_live(LONG epoch)
{
    return InterlockedCompareExchange(&s_live_epoch, 0, 0) == epoch;
}

void obj_spawn_upsert(void *map_obj, const char *id, LONG x, LONG y, int frame, int player)
{
    LONG i, n;
    if (!id || !id[0])
        return;
    if (player < 0)
        player = 0;
    if (player > 13)
        player = 13;
    ensure_cs();
    EnterCriticalSection(&s_cs);
    n = s_n;
    if (map_obj) {
        for (i = 0; i < n; ++i) {
            if (s_items[i].map_obj == map_obj) {
                int id_chg = _stricmp(s_items[i].id, id) != 0;
                lstrcpynA(s_items[i].id, id, (int)sizeof(s_items[i].id));
                if (s_items[i].x != x || s_items[i].y != y)
                    apply_move(&s_items[i], x, y, map_obj);
                s_items[i].frame = frame;
                if (player > 0)
                    s_items[i].player = player;
                if (id_chg) {
                    s_items[i].tpl_bound = 0;
                    s_items[i].tpl_begin = 0;
                    s_items[i].tpl_count = 0;
                    s_items[i].has_walk = 0;
                }
                LeaveCriticalSection(&s_cs);
                return;
            }
        }
    }
    if (n < OBJ_SPAWN_MAX) {
        memset(&s_items[n], 0, sizeof(s_items[n]));
        lstrcpynA(s_items[n].id, id, (int)sizeof(s_items[n].id));
        s_items[n].x = x;
        s_items[n].y = y;
        s_items[n].from_x = x;
        s_items[n].from_y = y;
        s_items[n].frame = frame;
        s_items[n].facing = 0;
        s_items[n].moving = 0;
        s_items[n].player = player;
        s_items[n].map_obj = map_obj;
        s_items[n].tpl_bound = 0;
        s_items[n].h_valid = 0;
        s_n = n + 1;
    }
    LeaveCriticalSection(&s_cs);
}

void obj_spawn_remove(void *map_obj)
{
    LONG i, n;
    if (!map_obj)
        return;
    ensure_cs();
    EnterCriticalSection(&s_cs);
    n = s_n;
    for (i = 0; i < n; ++i) {
        if (s_items[i].map_obj == map_obj) {
            if (i + 1 < n)
                s_items[i] = s_items[n - 1];
            s_n = n - 1;
            break;
        }
    }
    LeaveCriticalSection(&s_cs);
}

int obj_spawn_update_pos(void *map_obj, LONG x, LONG y)
{
    LONG i, n;
    if (!map_obj)
        return 0;
    ensure_cs();
    EnterCriticalSection(&s_cs);
    n = s_n;
    for (i = 0; i < n; ++i) {
        if (s_items[i].map_obj == map_obj) {
            int pid;
            if (s_items[i].x != x || s_items[i].y != y)
                apply_move(&s_items[i], x, y, map_obj);
            /* Refresh owner — Persist may set +0x6e after first CreateVisible. */
            pid = obj_mapobj_player_id(map_obj);
            if (pid > 0)
                s_items[i].player = pid;
            LeaveCriticalSection(&s_cs);
            return 1;
        }
    }
    LeaveCriticalSection(&s_cs);
    return 0;
}

int obj_spawn_count(void)
{
    return (int)s_n;
}

const ObjSpawnItem *obj_spawn_items(void)
{
    return s_items;
}

ObjSpawnItem *obj_spawn_items_mut(void)
{
    return s_items;
}
