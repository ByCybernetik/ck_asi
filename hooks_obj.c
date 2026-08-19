#include "hooks.h"
#include "hooks_internal.h"
#include "ktx_obj.h"
#include "obj_player.h"
#include "obj_spawn.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/* From vk_terrain.c / vk_obj.c — avoid pulling vulkan.h into this TU. */
int vk_terrain_draw_enabled(void);
int vk_terrain_soft_land_disabled(void);
int vk_obj_atlas_fits_array(int atlas_index);
int vk_obj_atlas_resident(int atlas_index);
void vk_obj_request_atlas(int atlas_index);
void vk_obj_request_id(const char *id);
int ktx_obj_id_fits_gpu(const char *id);
int ktx_obj_id_needs_soft_pc(const char *id);

/*
 * MapObj::CreateVisible @ 0x53f360 — buildings.
 * DecorObj::CreateVisible @ 0x462440 — props (bridges/columns/ruins).
 * Soft-skip under GPU-first / F8 when catalog hit; LeaveMap/SetPos shared.
 */

typedef int *(__fastcall *PFN_CreateVisible)(void *self);
typedef void(__fastcall *PFN_LeaveMap)(void *self);
typedef void(__thiscall *PFN_SetPos)(void *self, LONG *pt, int a3, int a4);

static PFN_CreateVisible real_CreateVisible;
static PFN_CreateVisible real_DecorCreateVisible;
static PFN_LeaveMap real_LeaveMap;
static PFN_SetPos real_SetPos;
typedef void(__thiscall *PFN_VisDraw)(void *self, void *ctx);
static PFN_VisDraw real_VisDraw;
static BYTE *g_cv_tramp, *g_dcv_tramp, *g_lm_tramp, *g_sp_tramp, *g_vd_tramp;
static BYTE g_cv_saved[16], g_dcv_saved[16], g_lm_saved[16], g_sp_saved[16], g_vd_saved[16];
static SIZE_T g_cv_steal, g_dcv_steal, g_lm_steal, g_sp_steal, g_vd_steal;
static void *g_cv_target, *g_dcv_target, *g_lm_target, *g_sp_target, *g_vd_target;

/* TEMP: retail MapObj path to scan facing/anim. CK_SOFT_OBJ=0 restores GPU. */
int ck_soft_obj_enabled(void)
{
    return env_on("CK_SOFT_OBJ", 0);
}

/* GPU octant: 0=S clockwise. Retail CreateVisible: FaceFn(dir_x,-dir_y) then (n/2+)%n. */
static int scan_facing(LONG dx, LONG dy)
{
    int ax, ay;
    if (dx == 0 && dy == 0)
        return -1;
    ax = dx < 0 ? -dx : dx;
    ay = dy < 0 ? -dy : dy;
    if (ay * 2 <= ax)
        return dx > 0 ? 2 : 6; /* E / W */
    if (ax * 2 <= ay)
        return dy > 0 ? 0 : 4; /* S / N */
    if (dx > 0 && dy > 0)
        return 1; /* SE */
    if (dx > 0 && dy < 0)
        return 3; /* NE */
    if (dx < 0 && dy < 0)
        return 5; /* NW */
    return 7; /* SW */
}

static int is_scan_mover(const char *id)
{
    static const char *keys[] = {
        "Peasant", "Villager", "Citizen", "Worker", "Farmer", "Woman", "Slave", "Serf",
        "Shepherd", "Fisherman", "Woodcutter", "Miner", "Builder",
        "Wolf", "Deer", "Boar", "Horse", "Sheep", "Cow", "Cattle", "Goat", "Pig",
        "Bear", "Lion", "Elephant", "Dog", "Chicken", "Hen", "Donkey", "Camel", "Ox",
        "Bull", "Ram", "Stag", "Hare", "Rabbit", "Fox", "Bird", "Eagle", "Duck", "Crow",
        NULL
    };
    int i;
    if (!id || !id[0])
        return 0;
    for (i = 0; keys[i]; ++i) {
        if (strstr(id, keys[i]))
            return 1;
    }
    return 0;
}

static int is_scan_bird(const char *id)
{
    static const char *keys[] = { "Crow", "Eagle", "Duck", "Bird", "Owl", "Hawk", NULL };
    int i;
    if (!id || !id[0])
        return 0;
    for (i = 0; keys[i]; ++i) {
        if (strstr(id, keys[i]))
            return 1;
    }
    return 0;
}

int ck_soft_bird_scan(const char *id)
{
    return is_scan_bird(id);
}

static int want_soft_scan(const char *id)
{
    if (is_scan_bird(id))
        return 1;
    return ck_soft_obj_enabled() && is_scan_mover(id);
}

static int read_mo4e(void *self)
{
    if (!self || IsBadReadPtr(self, 0x52))
        return -1;
    return *(int *)((BYTE *)self + 0x4e);
}

typedef struct {
    void *obj;
    char id[32];
    LONG x, y;
    int vis_sx, vis_sy;
    int rle_x, rle_y;
    int last_draw_vx, last_draw_vy;
    int last_vis98, last_vis94;
    DWORD tick;
    DWORD setpos_tick;
    DWORD draw_log_tick;
} SoftScanSt;

static SoftScanSt s_scan[48];
static unsigned s_scan_cv_n, s_scan_sp_n, s_scan_rle_n, s_scan_draw_n;
static void *s_cv_self;
static char s_cv_id[32];
static LONG s_cv_x, s_cv_y;
static DWORD s_cv_tick;

static SoftScanSt *scan_slot(void *obj, int create)
{
    int i, free_i = -1;
    DWORD oldest = 0xffffffffu;
    int old_i = 0;
    if (!obj)
        return NULL;
    for (i = 0; i < (int)(sizeof(s_scan) / sizeof(s_scan[0])); ++i) {
        if (s_scan[i].obj == obj)
            return &s_scan[i];
        if (!s_scan[i].obj && free_i < 0)
            free_i = i;
        if (s_scan[i].tick < oldest) {
            oldest = s_scan[i].tick;
            old_i = i;
        }
    }
    if (!create)
        return NULL;
    i = free_i >= 0 ? free_i : old_i;
    memset(&s_scan[i], 0, sizeof(s_scan[i]));
    s_scan[i].obj = obj;
    return &s_scan[i];
}

void obj_soft_note_rle(int x, int y, void *owner)
{
    SoftScanSt *st;
    LONG camL = 0, camT = 0, camR = 0, camB = 0;
    DWORD now;
    int dx, dy;
    (void)owner;
    if (!s_cv_self || !want_soft_scan(s_cv_id))
        return;
    if (s_scan_rle_n >= 200u)
        return;
    st = scan_slot(s_cv_self, 1);
    if (!st)
        return;
    now = GetTickCount();
    dx = x - st->rle_x;
    dy = y - st->rle_y;
    /* First blit of this CreateVisible (body). Skip tiny repeats. */
    if (st->rle_x || st->rle_y) {
        if (dx == 0 && dy == 0)
            return;
    }
    mm_read_cam(&camL, &camT, &camR, &camB);
    /* #region agent log */
    {
        FILE *f = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
        if (f) {
            fprintf(f,
                    "{\"sessionId\":\"764ba7\",\"runId\":\"bird-soft\",\"hypothesisId\":\"H-B4\","
                    "\"location\":\"hooks_obj.c:RleDraw\",\"message\":\"%s\","
                    "\"data\":{\"id\":\"%.24s\",\"kind\":\"rle\",\"logical\":[%ld,%ld],"
                    "\"rle\":[%d,%d],\"d_rle\":[%d,%d],\"cam\":[%ld,%ld],"
                    "\"dt_ms\":%lu},\"timestamp\":%lu}\n",
                    is_scan_bird(s_cv_id) ? "soft-bird" : "soft-move",
                    s_cv_id, (long)s_cv_x, (long)s_cv_y, x, y, dx, dy, (long)camL, (long)camT,
                    (unsigned long)(st->tick ? now - st->tick : 0), (unsigned long)now);
            fclose(f);
            ++s_scan_rle_n;
        }
    }
    /* #endregion */
    st->rle_x = x;
    st->rle_y = y;
}

static int read_pos(void *self, LONG *x, LONG *y)
{
    if (!self || IsBadReadPtr(self, 0x30))
        return 0;
    *x = *(LONG *)((BYTE *)self + 0x22);
    *y = *(LONG *)((BYTE *)self + 0x26);
    return 1;
}

/* Fast printable C-string check — avoid IsBadStringPtrA / IsBadReadPtr (very slow on Wine). */
static int page_readable(const void *p, size_t n)
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

static int looks_id_cstr(const char *s)
{
    unsigned i;
    if (!s || (ULONG_PTR)s > 0x7FF00000u)
        return 0;
    if (!page_readable(s, 32))
        return 0;
    if (s[0] < 'A' || (s[0] > 'Z' && s[0] < 'a') || s[0] > 'z')
        return 0;
    for (i = 1; i < 32; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0)
            return i >= 2;
        if (c < 32 || c > 126)
            return 0;
    }
    return 0;
}

/* Probe MapObj / DecorObj for a catalog id string. */
static int probe_class_id(void *self, char *out, size_t out_n)
{
    static const int offs[] = {0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20,
                               0x2a, 0x2e, 0x32, 0x36, 0x3a, 0x42, 0x4e, 0x52, 0x56, 0x5a};
    int i, j;
    char miss[48];

    if (!out || out_n < 2)
        return 0;
    out[0] = '\0';
    miss[0] = '\0';
    /* One range check — per-field IsBadReadPtr was ~15–35ms/call on Wine. */
    if (!self || IsBadReadPtr(self, 0x60))
        return 0;
    for (i = 0; i < (int)(sizeof(offs) / sizeof(offs[0])); ++i) {
        void *p = *(void **)((BYTE *)self + offs[i]);
        const char *s;
        if (!p || (ULONG_PTR)p < 0x10000u)
            continue;
        if (looks_id_cstr((const char *)p)) {
            s = (const char *)p;
            /* Keep scanning: first C-string is often a non-catalog label. */
            if (ktx_obj_has_id(s)) {
                lstrcpynA(out, s, (int)out_n);
                return 1;
            }
            if (!miss[0])
                lstrcpynA(miss, s, (int)sizeof(miss));
        }
        if (!page_readable(p, 16))
            continue;
        for (j = 0; j <= 12; j += 4) {
            void *q = *(void **)((BYTE *)p + j);
            if (!looks_id_cstr((const char *)q))
                continue;
            s = (const char *)q;
            if (ktx_obj_has_id(s)) {
                lstrcpynA(out, s, (int)out_n);
                return 1;
            }
            if (!miss[0])
                lstrcpynA(miss, s, (int)sizeof(miss));
        }
    }
    if (miss[0])
        lstrcpynA(out, miss, (int)out_n);
    return 0;
}

static void track_obj(void *self, const char *id)
{
    LONG x = 0, y = 0;
    int player;
    if (!read_pos(self, &x, &y))
        return;
    player = obj_mapobj_player_id(self);
    if ((LONG)g_map_epoch != 0) {
        static LONG s_last_ep = -1;
        LONG ep = (LONG)g_map_epoch;
        if (ep != s_last_ep) {
            s_last_ep = ep;
            obj_spawn_clear();
            obj_spawn_mark_epoch(ep);
        }
    }
    obj_spawn_upsert(self, id, x, y, 0, player);
}

/* Capture + soft-skip under GPU terrain (bare or GPU-obj catalog). */
static int *create_visible_common(void *self, PFN_CreateVisible real)
{
    char id[48];
    char canon[48];
    LONG x = 0, y = 0;
    int hit, resolved;
    const KtxObjSprite *sp;
    int gpu_obj = ktx_obj_wanted() && ktx_obj_ready();
    int soft_land_off = vk_terrain_soft_land_disabled();
    int bird;

    id[0] = '\0';
    canon[0] = '\0';

    /* When CK_GPU_OBJ=0, soft CreateVisible is the only building/prop path — never
     * skip it under GPU terrain (old bare-terrain skip hid all MapObj). */

    hit = probe_class_id(self, id, sizeof(id));
    read_pos(self, &x, &y);
    resolved = ktx_obj_resolve_id(id, canon, sizeof(canon));
    sp = resolved ? ktx_obj_lookup(canon, 0) : NULL;
    if (!hit && resolved)
        hit = 1;

    /* GPU-first: no soft MapObj/DecorObj stamps when GPU obj + soft land off.
     * CK_SOFT_OBJ: keep retail CreateVisible so we can scan original motion.
     * Birds: keep retail Visible so VisDraw/clip can be logged (GPU still tracks). */
    bird = is_scan_bird(id) || is_scan_bird(canon);
    if (gpu_obj && soft_land_off && !ck_soft_obj_enabled() && !bird) {
        int fits = (hit && resolved && canon[0] && ktx_obj_id_fits_gpu(canon));
        if (fits) {
            track_obj(self, canon);
            vk_obj_request_id(canon);
        } else if (hit && resolved && canon[0]) {
            obj_spawn_remove(self);
        }
        /* #region agent log */
        {
            static DWORD s_last;
            DWORD now = GetTickCount();
            if (now - s_last > 800u) {
                char data[200];
                s_last = now;
                snprintf(data, sizeof(data),
                         "{\"id\":\"%.40s\",\"skip\":1,\"soft\":0,\"soft_land\":1,\"fits\":%d,"
                         "\"resident\":%d}",
                         canon[0] ? canon : (id[0] ? id : "?"), fits,
                         (sp && fits && vk_obj_atlas_resident(sp->atlas_index)) ? 1 : 0);
                hooks_agent("H-OBJ", "hooks_obj.c:CreateVisible", "obj-soft-skip", data);
            }
        }
        /* #endregion */
        return NULL;
    }

    if (hit && resolved && canon[0] && ktx_obj_id_fits_gpu(canon))
        track_obj(self, canon);

    /* #region agent log */
    {
        const char *sid = canon[0] ? canon : id;
        int *vis;
        if (want_soft_scan(sid) && s_scan_cv_n < 250u) {
            SoftScanSt *st = scan_slot(self, 1);
            DWORD now = GetTickCount();
            LONG dx = 0, dy = 0;
            DWORD dt = 0;
            LONG camL = 0, camT = 0, camR = 0, camB = 0;
            int vis_sx = 0, vis_sy = 0;
            SHORT dir_x = 0, dir_y = 0;
            int log_it = 0;
            int bird_it = is_scan_bird(sid);

            s_cv_self = self;
            lstrcpynA(s_cv_id, sid, (int)sizeof(s_cv_id));
            s_cv_x = x;
            s_cv_y = y;
            s_cv_tick = now;
            if (st) {
                dx = x - st->x;
                dy = y - st->y;
                dt = st->tick ? now - st->tick : 0;
                /* Log on pos change, or heartbeat ~200ms if still (H-S2 frozen GetPos). */
                if (dx || dy)
                    log_it = 1;
                else if (dt >= 180u)
                    log_it = 1;
            } else
                log_it = 1;
            vis = real ? real(self) : NULL;
            if (vis && !IsBadReadPtr(vis, 0x40)) {
                vis_sx = *(int *)((BYTE *)vis + 0x34);
                vis_sy = *(int *)((BYTE *)vis + 0x38);
            }
            if (self && !IsBadReadPtr(self, 0x50)) {
                dir_x = *(SHORT *)((BYTE *)self + 0x46);
                dir_y = *(SHORT *)((BYTE *)self + 0x4a);
            }
            if (log_it) {
                int vis98 = -1, vis94 = -1, vis90 = -1;
                int f_neg, f_raw, f_p4;
                int mo4e = read_mo4e(self);
                FILE *f = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log",
                                "a");
                mm_read_cam(&camL, &camT, &camR, &camB);
                if (vis && !IsBadReadPtr(vis, 0xa0)) {
                    vis90 = *(int *)((BYTE *)vis + 0x90);
                    vis94 = *(int *)((BYTE *)vis + 0x94);
                    vis98 = *(int *)((BYTE *)vis + 0x98);
                }
                f_neg = scan_facing((LONG)dir_x, (LONG)(-(int)dir_y));
                f_raw = scan_facing((LONG)dir_x, (LONG)dir_y);
                f_p4 = (f_neg >= 0) ? ((f_neg + 4) & 7) : -1;
                if (f) {
                    /* #region agent log */
                    fprintf(f,
                            "{\"sessionId\":\"764ba7\",\"runId\":\"bird-soft\",\"hypothesisId\":\"%s\","
                            "\"location\":\"hooks_obj.c:CreateVisible\",\"message\":\"%s\","
                            "\"data\":{\"id\":\"%.24s\",\"kind\":\"cv\",\"logical\":[%ld,%ld],"
                            "\"dxy\":[%ld,%ld],\"vis_scr\":[%d,%d],\"d_vis\":[%d,%d],"
                            "\"cam\":[%ld,%ld],\"dir\":[%d,%d],\"vis90\":%d,\"vis94\":%d,\"vis98\":%d,"
                            "\"mo4e\":%d,\"gpu_negY\":%d,\"gpu_raw\":%d,\"gpu_plus4\":%d,\"dt_ms\":%lu,"
                            "\"since_setpos_ms\":%lu},\"timestamp\":%lu}\n",
                            bird_it ? "H-B1" : "H-F3", bird_it ? "soft-bird" : "soft-move",
                            sid, (long)x, (long)y, (long)dx, (long)dy, vis_sx, vis_sy,
                            st ? vis_sx - st->vis_sx : 0, st ? vis_sy - st->vis_sy : 0,
                            (long)camL, (long)camT, (int)dir_x, (int)dir_y, vis90, vis94, vis98,
                            mo4e, f_neg, f_raw, f_p4, (unsigned long)dt,
                            (unsigned long)(st && st->setpos_tick ? now - st->setpos_tick : 0),
                            (unsigned long)now);
                    /* #endregion */
                    fclose(f);
                    ++s_scan_cv_n;
                }
            }
            if (st) {
                lstrcpynA(st->id, sid, (int)sizeof(st->id));
                st->x = x;
                st->y = y;
                st->vis_sx = vis_sx;
                st->vis_sy = vis_sy;
                st->tick = now;
            }
            s_cv_self = NULL;
            return vis;
        }
        {
            static DWORD s_soft_last;
            DWORD now = GetTickCount();
            if (now - s_soft_last > 800u) {
                char data[220];
                s_soft_last = now;
                snprintf(data, sizeof(data),
                         "{\"id\":\"%.40s\",\"soft\":1,\"gpu_obj\":%d,\"soft_land\":%d,\"fits\":%d,"
                         "\"soft_pc\":%d,\"soft_obj\":%d}",
                         canon[0] ? canon : (id[0] ? id : "?"), gpu_obj ? 1 : 0,
                         soft_land_off ? 1 : 0,
                         (resolved && canon[0] && ktx_obj_id_fits_gpu(canon)) ? 1 : 0,
                         (resolved && canon[0] && ktx_obj_id_needs_soft_pc(canon)) ? 1 : 0,
                         ck_soft_obj_enabled());
                hooks_agent("H-C", "hooks_obj.c:CreateVisible", "obj-soft-draw", data);
            }
        }
    }
    /* #endregion */
    return real ? real(self) : NULL;
}

static int *__fastcall hook_CreateVisible(void *self)
{
    return create_visible_common(self, real_CreateVisible);
}

/* CVXVisibleMapObj vtbl+0x14 @ 0x4822f0 — per-frame pos:
 * vis+0x34/38 = (a8,ac) + (a0,a4) * (ctx+4 - +0x88) / +0x8c  when ctx+8 != 0. */
static void __thiscall hook_VisDraw(void *self, void *ctx)
{
    void *mo;
    LONG lx = 0, ly = 0;
    LONG vx, vy, bx, by, dx, dy;
    DWORD t0, dur, now, elapsed, ctx_t, ctx_b;
    SoftScanSt *st;
    const char *oid;
    int gap_x, gap_y;

    if (real_VisDraw)
        real_VisDraw(self, ctx);
    if (s_scan_draw_n >= 400u)
        return;
    if (!self || IsBadReadPtr(self, 0x5f4))
        return;
    mo = *(void **)((BYTE *)self + 0x5f0);
    if (!mo)
        return;
    st = scan_slot(mo, 0);
    oid = (st && st->id[0]) ? st->id : NULL;
    if (!oid || !want_soft_scan(oid))
        return;
    read_pos(mo, &lx, &ly);
    vx = *(LONG *)((BYTE *)self + 0x34);
    vy = *(LONG *)((BYTE *)self + 0x38);
    bx = *(LONG *)((BYTE *)self + 0xa8);
    by = *(LONG *)((BYTE *)self + 0xac);
    dx = *(LONG *)((BYTE *)self + 0xa0);
    dy = *(LONG *)((BYTE *)self + 0xa4);
    t0 = *(DWORD *)((BYTE *)self + 0x88);
    dur = *(DWORD *)((BYTE *)self + 0x8c);
    now = GetTickCount();
    ctx_t = 0;
    ctx_b = 0;
    if (ctx && !IsBadReadPtr(ctx, 0x10)) {
        ctx_t = *(DWORD *)((BYTE *)ctx + 4);
        ctx_b = *(DWORD *)((BYTE *)ctx + 8);
    }
    elapsed = ctx_t ? (ctx_t - t0) : 0;
    gap_x = (int)(vx - lx);
    gap_y = (int)(vy - ly);
    {
        int vis90 = *(int *)((BYTE *)self + 0x90);
        int vis94 = *(int *)((BYTE *)self + 0x94);
        int vis98 = *(int *)((BYTE *)self + 0x98);
        SHORT dir_x = 0, dir_y = 0;
        int face_chg;
        int bird_it = is_scan_bird(oid);
        if (!bird_it && (!dur || (!dx && !dy)))
            return;
        if (mo && !IsBadReadPtr(mo, 0x50)) {
            dir_x = *(SHORT *)((BYTE *)mo + 0x46);
            dir_y = *(SHORT *)((BYTE *)mo + 0x4a);
        }
        face_chg = (vis98 != st->last_vis98) || (vis94 != st->last_vis94);
        if (!face_chg && st->draw_log_tick && now - st->draw_log_tick < (bird_it ? 120u : 80u) &&
            vx == st->last_draw_vx && vy == st->last_draw_vy)
            return;
        /* #region agent log */
        {
            int f_neg = scan_facing((LONG)dir_x, (LONG)(-(int)dir_y));
            int f_raw = scan_facing((LONG)dir_x, (LONG)dir_y);
            int f_p4 = (f_neg >= 0) ? ((f_neg + 4) & 7) : -1;
            int f_dlt = scan_facing(dx, dy);
            int mo4e = read_mo4e(mo);
            FILE *f = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
            if (f) {
                fprintf(f,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"bird-soft\",\"hypothesisId\":\"%s\","
                        "\"location\":\"hooks_obj.c:VisDraw\",\"message\":\"%s\","
                        "\"data\":{\"id\":\"%.24s\",\"kind\":\"draw\",\"logical\":[%ld,%ld],"
                        "\"vis\":[%ld,%ld],\"base\":[%ld,%ld],\"delta\":[%ld,%ld],"
                        "\"dur\":%lu,\"elapsed\":%lu,\"ctx8\":%lu,\"gap\":[%d,%d],"
                        "\"dir\":[%d,%d],\"vis90\":%d,\"vis94\":%d,\"vis98\":%d,\"mo4e\":%d,"
                        "\"gpu_negY\":%d,\"gpu_raw\":%d,\"gpu_plus4\":%d,\"gpu_dlt\":%d,"
                        "\"since_setpos_ms\":%lu},\"timestamp\":%lu}\n",
                        bird_it ? "H-B2" : "H-F1", bird_it ? "soft-bird" : "soft-move",
                        oid, (long)lx, (long)ly, (long)vx, (long)vy, (long)bx, (long)by, (long)dx,
                        (long)dy, (unsigned long)dur, (unsigned long)elapsed, (unsigned long)ctx_b,
                        gap_x, gap_y, (int)dir_x, (int)dir_y, vis90, vis94, vis98, mo4e, f_neg,
                        f_raw, f_p4, f_dlt,
                        (unsigned long)(st->setpos_tick ? now - st->setpos_tick : 0),
                        (unsigned long)now);
                fclose(f);
                ++s_scan_draw_n;
            }
        }
        /* #endregion */
        st->last_draw_vx = (int)vx;
        st->last_draw_vy = (int)vy;
        st->last_vis98 = vis98;
        st->last_vis94 = vis94;
        st->draw_log_tick = now;
    }
}

static int *__fastcall hook_DecorCreateVisible(void *self)
{
    return create_visible_common(self, real_DecorCreateVisible);
}

static void __fastcall hook_LeaveMap(void *self)
{
    obj_spawn_remove(self);
    real_LeaveMap(self);
}

static void __thiscall hook_SetPos(void *self, LONG *pt, int a3, int a4)
{
    LONG x = 0, y = 0;
    LONG ox = 0, oy = 0;
    const char *oid = NULL;
    SoftScanSt *st;

    real_SetPos(self, pt, a3, a4);
    if (!read_pos(self, &x, &y))
        return;
    ox = x;
    oy = y;
    st = scan_slot(self, 0);
    if (st && st->id[0]) {
        oid = st->id;
        ox = st->x;
        oy = st->y;
    } else if (obj_spawn_count() > 0) {
        const ObjSpawnItem *items = obj_spawn_items();
        int n = obj_spawn_count();
        int i;
        for (i = 0; i < n; ++i) {
            if (items[i].map_obj != self)
                continue;
            oid = items[i].id;
            ox = items[i].x;
            oy = items[i].y;
            break;
        }
    }
    if (obj_spawn_count() > 0)
        obj_spawn_update_pos(self, x, y);
    /* #region agent log */
    if (s_scan_sp_n < 250u && oid && want_soft_scan(oid) &&
        self && !IsBadReadPtr(self, 0x50)) {
        LONG dx = x - ox, dy = y - oy;
        if (dx || dy) {
            DWORD now = GetTickCount();
            SHORT dir_x = *(SHORT *)((BYTE *)self + 0x46);
            SHORT dir_y = *(SHORT *)((BYTE *)self + 0x4a);
            int vis98 = -1, vis94 = -1, vis90 = -1;
            int f_neg, f_raw, f_p4, f_dxy;
            int bird_it = is_scan_bird(oid);
            int mo4e = read_mo4e(self);
            void *vis = NULL;
            FILE *f = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
            if (!IsBadReadPtr(self, 0x42))
                vis = *(void **)((BYTE *)self + 0x3e);
            if (vis && !IsBadReadPtr(vis, 0xa0)) {
                vis90 = *(int *)((BYTE *)vis + 0x90);
                vis94 = *(int *)((BYTE *)vis + 0x94);
                vis98 = *(int *)((BYTE *)vis + 0x98);
            }
            f_neg = scan_facing((LONG)dir_x, (LONG)(-(int)dir_y));
            f_raw = scan_facing((LONG)dir_x, (LONG)dir_y);
            f_p4 = (f_neg >= 0) ? ((f_neg + 4) & 7) : -1;
            f_dxy = scan_facing(dx, dy);
            if (f) {
                fprintf(f,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"bird-soft\",\"hypothesisId\":\"%s\","
                        "\"location\":\"hooks_obj.c:SetPos\",\"message\":\"%s\","
                        "\"data\":{\"id\":\"%.24s\",\"kind\":\"setpos\",\"logical\":[%ld,%ld],"
                        "\"dxy\":[%ld,%ld],\"dir\":[%d,%d],\"vis90\":%d,\"vis94\":%d,\"vis98\":%d,"
                        "\"mo4e\":%d,\"a3\":%d,\"a4\":%d,"
                        "\"gpu_negY\":%d,\"gpu_raw\":%d,\"gpu_plus4\":%d,\"gpu_dxy\":%d,"
                        "\"dt_ms\":%lu},\"timestamp\":%lu}\n",
                        bird_it ? "H-B3" : "H-F1", bird_it ? "soft-bird" : "soft-move",
                        oid, (long)x, (long)y, (long)dx, (long)dy, (int)dir_x, (int)dir_y, vis90,
                        vis94, vis98, mo4e, a3, a4, f_neg, f_raw, f_p4, f_dxy,
                        (unsigned long)(st && st->setpos_tick ? now - st->setpos_tick : 0),
                        (unsigned long)now);
                fclose(f);
                ++s_scan_sp_n;
            }
            st = scan_slot(self, 1);
            if (st) {
                lstrcpynA(st->id, oid, (int)sizeof(st->id));
                st->x = x;
                st->y = y;
                st->setpos_tick = now;
            }
        }
    }
    /* #endregion */
}

/* Infer steal length from common MSVC prologs (min 5 for JMP rel32). */
static SIZE_T steal_from_prolog(const BYTE *p)
{
    if (!p)
        return 0;
    /* push -1; push SEH (MSVC EH) — MapObj CreateVisible / SetPos */
    if (p[0] == 0x6A && p[1] == 0xFF && p[2] == 0x68)
        return 7;
    /* DecorObj::CreateVisible: push esi; push edi; push imm32 */
    if (p[0] == 0x56 && p[1] == 0x57 && p[2] == 0x68)
        return 7;
    /* LeaveMap: push ecx; push esi; mov esi,ecx; mov ecx,[esi+0x3e] */
    if (p[0] == 0x51 && p[1] == 0x56 && p[2] == 0x8B && p[3] == 0xF1 && p[4] == 0x8B &&
        p[5] == 0x4E)
        return 7;
    /* push ebp; mov ebp,esp */
    if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC)
        return 5;
    /* mov edi,edi; push ebp; mov ebp,esp (hotpatch) */
    if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC)
        return 5;
    /* sub esp, imm8 */
    if (p[0] == 0x83 && p[1] == 0xEC)
        return 5;
    /* sub esp, imm32 */
    if (p[0] == 0x81 && p[1] == 0xEC)
        return 6;
    return 0;
}

static void log_prolog(const char *name, void *target)
{
    BYTE b[16];
    char hex[64];
    int i, n = 0;
    SIZE_T steal;
    if (!target || IsBadReadPtr(target, 16)) {
        log_msg("hooks_obj: prolog %s @ %p BAD", name, target);
        return;
    }
    memcpy(b, target, 16);
    for (i = 0; i < 16; ++i)
        n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x%s", b[i], i == 15 ? "" : " ");
    steal = steal_from_prolog(b);
    log_msg("hooks_obj: prolog %s @ %p steal=%u [%s]", name, target, (unsigned)steal, hex);
}

void hooks_obj_install(void)
{
    int want_hooks;
    int gpu_obj = ktx_obj_wanted();

    /* Always hook CreateVisible for soft-skip under GPU terrain, even if CK_GPU_OBJ=0
     * (otherwise retail soft buildings/props keep drawing — H-SOFT bare perf test). */
    g_cv_target = (void *)(ULONG_PTR)0x0053f360u;
    g_dcv_target = (void *)(ULONG_PTR)0x00462440u; /* CVXDecorObj::CreateVisible — props */
    g_lm_target = (void *)(ULONG_PTR)0x0053f740u;
    g_sp_target = (void *)(ULONG_PTR)0x0053f480u;
    g_vd_target = (void *)(ULONG_PTR)0x004822f0u; /* Visible::Draw — lerp vis+0x34 */

    log_prolog("CreateVisible", g_cv_target);
    log_prolog("DecorCreateVisible", g_dcv_target);
    log_prolog("LeaveMap", g_lm_target);
    log_prolog("SetPos", g_sp_target);
    log_prolog("VisDraw", g_vd_target);

    log_msg("hooks_obj: CK_SOFT_OBJ=%d (retail MapObj %s)", ck_soft_obj_enabled(),
            ck_soft_obj_enabled() ? "ON — GPU obj draw skipped" : "off");
    /* #region agent log */
    {
        char data[80];
        snprintf(data, sizeof(data), "{\"CK_SOFT_OBJ\":%d}", ck_soft_obj_enabled() ? 1 : 0);
        hooks_agent("H-S1", "hooks_obj.c:install", "soft-move", data);
    }
    /* #endregion */

    want_hooks = env_on("CK_GPU_OBJ_HOOKS", 1);
    g_cv_steal = steal_from_prolog((const BYTE *)g_cv_target);
    g_dcv_steal = steal_from_prolog((const BYTE *)g_dcv_target);
    g_lm_steal = steal_from_prolog((const BYTE *)g_lm_target);
    g_sp_steal = steal_from_prolog((const BYTE *)g_sp_target);
    g_vd_steal = steal_from_prolog((const BYTE *)g_vd_target);

    if (!want_hooks) {
        log_msg("hooks_obj: MapObj hooks DISABLED (CK_GPU_OBJ_HOOKS=0)");
        return;
    }
    if (g_cv_steal < 5) {
        log_msg("hooks_obj: refuse install — bad steal cv=%u", (unsigned)g_cv_steal);
        return;
    }

    if (install_inline_hook(g_cv_target, (void *)hook_CreateVisible, g_cv_steal, &g_cv_tramp,
                            g_cv_saved)) {
        real_CreateVisible = (PFN_CreateVisible)g_cv_tramp;
        log_msg("inline hooked MapObj::CreateVisible @ %p steal=%u gpu_obj=%d", g_cv_target,
                (unsigned)g_cv_steal, gpu_obj);
    } else
        log_msg("WARN: CreateVisible hook failed");

    /* Props (bridges/columns) use DecorObj::CreateVisible — not MapObj. */
    if (g_dcv_steal >= 5 &&
        install_inline_hook(g_dcv_target, (void *)hook_DecorCreateVisible, g_dcv_steal,
                            &g_dcv_tramp, g_dcv_saved)) {
        real_DecorCreateVisible = (PFN_CreateVisible)g_dcv_tramp;
        log_msg("inline hooked DecorObj::CreateVisible @ %p steal=%u", g_dcv_target,
                (unsigned)g_dcv_steal);
    } else
        log_msg("WARN: DecorObj::CreateVisible hook failed (steal=%u)", (unsigned)g_dcv_steal);

    if (g_vd_steal >= 5 &&
        install_inline_hook(g_vd_target, (void *)hook_VisDraw, g_vd_steal, &g_vd_tramp, g_vd_saved)) {
        real_VisDraw = (PFN_VisDraw)g_vd_tramp;
        log_msg("inline hooked Visible::Draw @ %p steal=%u", g_vd_target, (unsigned)g_vd_steal);
    } else
        log_msg("WARN: Visible::Draw hook failed steal=%u", (unsigned)g_vd_steal);

    /* LeaveMap/SetPos: keep even when GPU obj off / soft-scan so we see path ticks. */
    if (g_lm_steal < 5 || g_sp_steal < 5) {
        log_msg("hooks_obj: refuse LeaveMap/SetPos — bad steal lm=%u sp=%u", (unsigned)g_lm_steal,
                (unsigned)g_sp_steal);
        return;
    }

    if (install_inline_hook(g_lm_target, (void *)hook_LeaveMap, g_lm_steal, &g_lm_tramp,
                            g_lm_saved)) {
        real_LeaveMap = (PFN_LeaveMap)g_lm_tramp;
        log_msg("inline hooked MapObj::LeaveMap @ %p steal=%u", g_lm_target, (unsigned)g_lm_steal);
    } else
        log_msg("WARN: LeaveMap hook failed");

    if (install_inline_hook(g_sp_target, (void *)hook_SetPos, g_sp_steal, &g_sp_tramp, g_sp_saved)) {
        real_SetPos = (PFN_SetPos)g_sp_tramp;
        log_msg("inline hooked MapObj::SetPos @ %p steal=%u", g_sp_target, (unsigned)g_sp_steal);
    } else
        log_msg("WARN: SetPos hook failed");
}

void hooks_obj_remove(void)
{
    remove_inline_hook(g_cv_target, g_cv_steal, g_cv_saved, g_cv_tramp);
    g_cv_tramp = NULL;
    remove_inline_hook(g_dcv_target, g_dcv_steal, g_dcv_saved, g_dcv_tramp);
    g_dcv_tramp = NULL;
    remove_inline_hook(g_lm_target, g_lm_steal, g_lm_saved, g_lm_tramp);
    g_lm_tramp = NULL;
    remove_inline_hook(g_sp_target, g_sp_steal, g_sp_saved, g_sp_tramp);
    g_sp_tramp = NULL;
    remove_inline_hook(g_vd_target, g_vd_steal, g_vd_saved, g_vd_tramp);
    g_vd_tramp = NULL;
}
