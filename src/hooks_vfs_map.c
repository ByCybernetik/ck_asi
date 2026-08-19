#include "hooks.h"
#include "hooks_internal.h"
#include "decor_spawn.h"
#include "obj_spawn.h"
#include "log.h"
#include "vk_present.h"
#include "hitch.h"
#include <stdio.h>
#include <string.h>

static volatile LONG g_vfs_logs;
static volatile LONG g_editor_trace_left; /* remaining DefFileOpen logs after editor trigger */
static volatile LONG g_editor_seq;
static volatile LONG g_cf_logs;

/* Entity / decor / unit asset load path (retail) — DefFileOpen only (safe cdecl). */
typedef void *(__cdecl *PFN_DefFileOpen)(const char *path, unsigned mode, void *a3);
typedef HANDLE(WINAPI *PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                        HANDLE);

static PFN_DefFileOpen real_DefFileOpen;
static BYTE *g_dfo_tramp;
static BYTE g_dfo_saved[16];
static SIZE_T g_dfo_steal;
static void *g_dfo_target;
static PFN_CreateFileA real_CreateFileA;
static void *g_iat_CreateFileA;

/* FileLoad @ 0x40f980 — cdecl(path, unused, int *err); opens via DefFileOpen mode 0x41, reads all. */
typedef void *(__cdecl *PFN_FileLoad)(const char *path, void *a2, int *err);
static PFN_FileLoad real_FileLoad;
static BYTE *g_fl_tramp;
static BYTE g_fl_saved[16];
static SIZE_T g_fl_steal;
static void *g_fl_target;
static volatile LONG g_fl_logs;

/* HPFS CFileService::Open @ 0x414c60 — thiscall(path, mode, err*), ret 0xc.
 * Registered for CHPFS mounts (wrapper around CHPFS). Path-bearing Open for CurrentMap files. */
typedef void *(__attribute__((thiscall)) *PFN_CHPFS_Open)(void *self, const char *path,
                                                          unsigned mode, void *err);
static PFN_CHPFS_Open real_CHPFS_Open;
static BYTE *g_chp_tramp;
static BYTE g_chp_saved[16];
static SIZE_T g_chp_steal;
static void *g_chp_target;
static volatile LONG g_chp_logs;

/* LoadDirg64_16 @ 0x535CD0 — thiscall(CFile*), ret 4. Expects DIRG cell=64 bits=16.
 * Terrain attach calls this after opening CurrentMap/terrain.decor.grid → fills map+0x80. */
typedef int(__attribute__((thiscall)) *PFN_LoadDirg)(void *self, void *file);
static PFN_LoadDirg real_LoadDirg64;
static BYTE *g_d64_tramp;
static BYTE g_d64_saved[16];
static SIZE_T g_d64_steal;
static void *g_d64_target;
static volatile LONG g_d64_logs;

/* LoadDirg32_8 @ 0x5503C0 — thiscall(CFile*), ret 4. Expects DIRG cell=32 bits=8. */
static PFN_LoadDirg real_LoadDirg32;
static BYTE *g_d32_tramp;
static BYTE g_d32_saved[16];
static SIZE_T g_d32_steal;
static void *g_d32_target;
static volatile LONG g_d32_logs;

/* LoadDirg16_1 @ 0x466230 — thiscall(CFile*), ret 4. Expects DIRG cell=16 bits=1.
 * Mostly MAPOBJECTS\*.PASS occlusion; also candidate for terrain.pass.grid attach. */
static PFN_LoadDirg real_LoadDirg16;
static BYTE *g_d16_tramp;
static BYTE g_d16_saved[16];
static SIZE_T g_d16_steal;
static void *g_d16_target;
static volatile LONG g_d16_logs;

/* LoadDirg64_4 @ 0x47a760 — thiscall(CFile*), ret 4. Expects DIRG cell=64 bits=4.
 * Used for terrain.trans.grid; candidate for terrain.pass.grid (retail emptyscn is 64/4). */
static PFN_LoadDirg real_LoadDirg64_4;
static BYTE *g_d644_tramp;
static BYTE g_d644_saved[16];
static SIZE_T g_d644_steal;
static void *g_d644_target;
static volatile LONG g_d644_logs;

/* Pass-grid attach wrapper @ 0x54b830 — thiscall(path), opens + LoadDirg16_1. */
typedef int(__attribute__((thiscall)) *PFN_AttachPassGrid)(void *self, const char *path);
static PFN_AttachPassGrid real_AttachPass;
static BYTE *g_ap_tramp;
static BYTE g_ap_saved[16];
static SIZE_T g_ap_steal;
static void *g_ap_target;
static volatile LONG g_ap_logs;

/* Game terrain attach @ 0x54BE40 — thiscall(prefix), ret 4.
 * Order: .light → BuildLight → .terrain → .height → .trans → .decor → AttachPass. */
typedef int(__attribute__((thiscall)) *PFN_AttachTerrain)(void *self, const char *prefix);
static PFN_AttachTerrain real_AttachGame;
static BYTE *g_atg_tramp;
static BYTE g_atg_saved[16];
static SIZE_T g_atg_steal;
static void *g_atg_target;
static volatile LONG g_atg_logs;

/* Editor terrain attach @ 0x54BBC0 — thiscall(prefix), ret 4.
 * Order: .terrain → .height → .light → .trans → .decor → .pass. */
static PFN_AttachTerrain real_AttachEdit;
static BYTE *g_ate_tramp;
static BYTE g_ate_saved[16];
static SIZE_T g_ate_steal;
static void *g_ate_target;
static volatile LONG g_ate_logs;

/* Load .terrain.grid (64×4) @ 0x54B890 — thiscall(CFile*), ret 4. */
static PFN_LoadDirg real_LoadTerrainZ;
static BYTE *g_ltz_tramp;
static BYTE g_ltz_saved[16];
static SIZE_T g_ltz_steal;
static void *g_ltz_target;
static volatile LONG g_ltz_logs;

/* BuildLight @ 0x546940 — cdecl(height*, light*, 4 dwords rect), cleans 0x18. */
typedef void(__cdecl *PFN_BuildLight)(void *height, void *light, unsigned a, unsigned b, unsigned c,
                                      unsigned d);
static PFN_BuildLight real_BuildLight;
static BYTE *g_bl_tramp;
static BYTE g_bl_saved[16];
static SIZE_T g_bl_steal;
static void *g_bl_target;

/* LoadMap(TVXMapData*) @ 0x550030 — cdecl(mapdata, load_obj, flag).
 * Calls LoadAll(AttachTerrain-game) then map.obj.xml / InitSequences. */
typedef int(__cdecl *PFN_LoadMap)(void *mapdata, int load_obj, int flag);
static PFN_LoadMap real_LoadMap;
static BYTE *g_lm_tramp;
static BYTE g_lm_saved[16];
static SIZE_T g_lm_steal;
static void *g_lm_target;
static volatile LONG g_lm_logs;

/* LoadMap_path @ 0x550230 — cdecl(path, mapdata, load_obj, flag). */
typedef void(__cdecl *PFN_LoadMapPath)(const char *path, void *mapdata, int load_obj, int flag);
static PFN_LoadMapPath real_LoadMapPath;
static BYTE *g_lmp_tramp;
static BYTE g_lmp_saved[16];
static SIZE_T g_lmp_steal;
static void *g_lmp_target;
static volatile LONG g_lmp_logs;

/* CVXMapSaveLoad::BeginElement @ 0x54EF60 — thiscall(self, CXMLParser*, TXMLTag*).
 * Virtual-style: two stack args; *tag = element name. */
typedef unsigned(__attribute__((thiscall)) *PFN_BeginElement)(void *self, void *parser, void *tag);
static PFN_BeginElement real_BeginElement;
static BYTE *g_be_tramp;
static BYTE g_be_saved[16];
static SIZE_T g_be_steal;
static void *g_be_target;
static volatile LONG g_be_logs;

/* CVXMap::InitSequences @ 0x5425C0 — thiscall(self). SEH prologue. */
typedef unsigned(__attribute__((thiscall)) *PFN_InitSequences)(void *self);
static PFN_InitSequences real_InitSequences;
static BYTE *g_is_tramp;
static BYTE g_is_saved[16];
static SIZE_T g_is_steal;
static void *g_is_target;
static volatile LONG g_is_logs;

/* Path-alias mount @ 0x434620 — cdecl(virt_prefix, real_path).
 * Editor/game: CurrentMap/ → CurrentGame/Maps/<id>/ ; LocalMapData/ → … */
typedef int(__cdecl *PFN_AliasMount)(const char *virt, const char *real);
static PFN_AliasMount real_AliasMount;
static BYTE *g_am_tramp;
static BYTE g_am_saved[16];
static SIZE_T g_am_steal;
static void *g_am_target;
static volatile LONG g_am_logs;


/* Last DefFileOpen path — correlate with DIRG loaders. */
static char g_last_dfo_path[512];
static volatile LONG g_last_dfo_ok;
char g_last_scenario[260];
volatile LONG g_deep_attach;
volatile LONG g_map_epoch; /* incremented on each AttachTerrain enter */
volatile LONG g_ck_map_editor; /* 1 = map editor AttachEdit path */
static volatile LONG g_map_seq;   /* monotonic event order within process */
static volatile LONG g_bl_logs;
static volatile LONG g_attach_dump;
void *g_last_attach_map; /* self from last AttachTerrain enter */
void *g_last_decor_dirg; /* last LoadDirg64 TERRAIN.DECOR.GRID object */
unsigned g_last_map_f4, g_last_map_f8;

/* Forward decls used by map-load path before definitions. */
static void dirg_peek(const void *buf, unsigned *magic, unsigned *cell, unsigned *bits);
static void dump_dirg_object(const char *role, void *obj, const char *hypothesisId, LONG epoch);
static void dump_attach_layers(void *map_self, const char *which, LONG epoch);


/* H1: retail opens entity/class/sprite assets via DefFileOpen. */
static int path_is_entity_asset(const char *path)
{
    char u[512];
    if (!path_is_safe(path))
        return 0;
    path_upper_copy(u, sizeof(u), path);
    if (strstr(u, ".ENT.XML"))
        return 1;
    if (strstr(u, ".SC.XML"))
        return 1;
    if (strstr(u, ".RLE.MMP"))
        return 1;
    if (strstr(u, "IMGRLE"))
        return 1;
    if (strstr(u, "MAPOBJECTS\\") || strstr(u, "BUILDINGS\\") || strstr(u, "UNITS\\"))
        return 1;
    if (strstr(u, "CLASSES\\") || strstr(u, "DECORS\\"))
        return 1;
    if (strstr(u, "MAP.OBJ.XML"))
        return 1;
    return 0;
}

/* Editor boot / map template / UI chrome — used to start a capture window. */
static int path_is_editor_trigger(const char *u)
{
    if (strstr(u, "EDITORINI"))
        return 1;
    if (strstr(u, "EMPTYADV"))
        return 1;
    if (strstr(u, "MAPTOOLSDLG"))
        return 1;
    if (strstr(u, "UI\\EDITOR") || strstr(u, "UI/EDITOR"))
        return 1;
    if (strstr(u, "OPENEDLG.INI") || strstr(u, "SAVEDLG.INI"))
        return 1;
    return 0;
}

/* Always interesting during editor capture (terrain/map/doc). */
static int path_is_editor_doc(const char *u)
{
    if (strstr(u, "TERRAIN"))
        return 1;
    if (strstr(u, "SEQUENCES"))
        return 1;
    if (strstr(u, "MINIMAP"))
        return 1;
    if (strstr(u, "RANDOMMAP"))
        return 1;
    if (strstr(u, "MAP.XML") || strstr(u, "GAME.XML") || strstr(u, "AREAS.XML") ||
        strstr(u, "MAP.OBJ"))
        return 1;
    if (strstr(u, "DECORS.INI") || strstr(u, "TERRAINS.XML"))
        return 1;
    if (strstr(u, "TRANSITIONS"))
        return 1;
    if (strstr(u, ".BFHP"))
        return 1;
    if (strstr(u, "CURRENTMAP") || strstr(u, "CURRENTGAME"))
        return 1;
    return 0;
}

static void editor_trace_maybe_arm(const char *u)
{
    if (!path_is_editor_trigger(u))
        return;
    /* Entering editor UI — kill GPU overpaint before AttachEdit (avoids cam[0,80..] hitch). */
    InterlockedExchange(&g_ck_map_editor, 1);
    if (InterlockedCompareExchange(&g_editor_trace_left, 800, 0) == 0) {
        log_msg("editor load trace ARMED (800 vfs opens)");
    }
}

static int path_is_tools_ui(const char *u)
{
    if (strstr(u, "MAPTOOLSDLG") || strstr(u, "TOOLSDLG") || strstr(u, "DEFAULTTOOL") ||
        strstr(u, "TOOLSETTINGS") || strstr(u, "BRUSHES") || strstr(u, "TERRAINSETTINGS") ||
        strstr(u, "HEIGHTPAINT") || strstr(u, "HEIGHTSET") || strstr(u, "HEIGHTBLUR") ||
        strstr(u, "DECORSETTINGS") || strstr(u, "DECORDELETE") || strstr(u, "PLACEOBJ") ||
        strstr(u, "PLACEDEFENCE") || strstr(u, "UI\\EDITOR") || strstr(u, "UI/EDITOR") ||
        strstr(u, "INTERFACE\\EDITOR") || strstr(u, "INTERFACE/EDITOR") ||
        strstr(u, "EDITORINI") || strstr(u, "EDITORRES") || strstr(u, "TREEARROW") ||
        strstr(u, "REEARROW") || strstr(u, "SCROLL2") || strstr(u, "SCROLLUP") ||
        strstr(u, "SCROLLDOWN") || strstr(u, "TERRAINS.XML") || strstr(u, "DECORS.INI") ||
        strstr(u, "INFOBAREDITOR") || strstr(u, "CMDBAREDITOR") || strstr(u, "MAP TOOLS") ||
        strstr(u, "MAPTOOLS"))
        return 1;
    return 0;
}

static void log_editor_vfs(const char *api, const char *path, unsigned mode, int ok)
{
    char u[512];
    LONG left;
    LONG seq;
    int interesting;
    int tools_ui;

    if (!path_is_safe(path))
        return;
    path_upper_copy(u, sizeof(u), path);
    editor_trace_maybe_arm(u);

    tools_ui = path_is_tools_ui(u);
    left = g_editor_trace_left;
    interesting = path_is_editor_trigger(u) || path_is_editor_doc(u) || tools_ui;
    if (left <= 0 && !interesting)
        return;
    if (left > 0)
        InterlockedDecrement(&g_editor_trace_left);

    seq = InterlockedIncrement(&g_editor_seq);
    if (seq <= 200 || tools_ui)
        log_msg("editor-vfs #%ld %s %s mode=%u ok=%d", (long)seq, api, path, mode, ok);
}

static int path_is_playercol_vfs(const char *path)
{
    char u[512];
    if (!path_is_safe(path))
        return 0;
    path_upper_copy(u, sizeof(u), path);
    return strstr(u, "IARENA") != NULL || strstr(u, ".RED.") != NULL ||
           strstr(u, ".GREEN.") != NULL || strstr(u, ".BLUE.") != NULL ||
           strstr(u, "PLAYER") != NULL || strstr(u, "LAYER1.RLE") != NULL ||
           strstr(u, "LAYER2.RLE") != NULL;
}

static void log_playercol_vfs(const char *api, const char *path, unsigned mode, int ok)
{
    char esc[520];
    char u[512];
    LONG n;
    int is_red, is_green, is_blue, is_iarena;

    if (!path_is_playercol_vfs(path))
        return;
    n = InterlockedIncrement(&g_pc_vfs_logs);
    if (n > 400)
        return;
    path_upper_copy(u, sizeof(u), path ? path : "");
    is_red = strstr(u, ".RED.") != NULL;
    is_green = strstr(u, ".GREEN.") != NULL;
    is_blue = strstr(u, ".BLUE.") != NULL;
    is_iarena = strstr(u, "IARENA") != NULL;
    json_escape(esc, sizeof(esc), path ? path : "");
    if (n <= 80)
        log_msg("playercol-vfs #%ld %s %s ok=%d rgb=%d%d%d", (long)n, api, path ? path : "", ok,
                is_red, is_green, is_blue);
}

static void dirg_peek(const void *buf, unsigned *magic, unsigned *cell, unsigned *bits);

static int path_is_mapload(const char *path)
{
    char u[512];
    if (!path_is_safe(path))
        return 0;
    path_upper_copy(u, sizeof(u), path);
    return strstr(u, ".BFHP") != NULL || strstr(u, "CURRENTMAP") != NULL ||
           strstr(u, "CURRENTGAME") != NULL || strstr(u, "CURRENTMAP") != NULL ||
           strstr(u, "LOCALMAPDATA") != NULL || strstr(u, "TERRAIN.") != NULL ||
           strstr(u, "TERRAIN\\") != NULL || strstr(u, "TERRAIN/") != NULL ||
           strstr(u, ".VQ") != NULL || strstr(u, ".GRID") != NULL ||
           strstr(u, "SEQUENCES") != NULL || strstr(u, "MAP.XML") != NULL ||
           strstr(u, "MAP.OBJ") != NULL || strstr(u, "GAME.XML") != NULL ||
           strstr(u, "SCENARIOS") != NULL || strstr(u, "ADVENTURES") != NULL ||
           strstr(u, "EMPTYSCN") != NULL || strstr(u, "EMPTYADV") != NULL ||
           strstr(u, "NEWMAP") != NULL || strstr(u, "MAPS/") != NULL ||
           strstr(u, "MAPS\\") != NULL || strstr(u, "TERRAINS.XML") != NULL ||
           strstr(u, "DECORS") != NULL;
}

static void log_mapload_vfs(const char *api, const char *path, unsigned mode, int ok,
                            const void *peek_buf)
{
    char esc[520];
    char u[512];
    unsigned magic = 0, cell = 0, bits = 0, w = 0, h = 0;
    LONG n;
    const unsigned char *p;
    int critical;
    int is_decor_noise;

    if (!path_is_mapload(path))
        return;
    path_upper_copy(u, sizeof(u), path ? path : "");
    /* Editor templates arrive via map-vfs (not editor-vfs) — arm GPU skip early. */
    if (strstr(u, "EMPTYADV") || strstr(u, "NEWMAP") || strstr(u, "EMPTYSCN") ||
        strstr(u, "EDITORINI") || strstr(u, "MAPTOOLSDLG")) {
        InterlockedExchange(&g_ck_map_editor, 1);
    }
    critical = strstr(u, "CURRENTMAP") != NULL || strstr(u, "CURRENTGAME") != NULL ||
               strstr(u, ".GRID") != NULL || strstr(u, "TERRAIN.") != NULL ||
               strstr(u, "MAP.XML") != NULL || strstr(u, "MAP.OBJ") != NULL ||
               strstr(u, "GAME.XML") != NULL || strstr(u, ".BFHP") != NULL ||
               strstr(u, "TERRAINS.XML") != NULL || strstr(u, ".VQ") != NULL ||
               strstr(u, "SEQUENCES") != NULL;
    is_decor_noise = strstr(u, "DECORS") != NULL && !critical;
    n = InterlockedIncrement(&g_vfs_logs);
    /* Cap DECORS flood; never drop CurrentMap / .grid / VQ / BFHP. */
    if (!critical && (n > 200 || (is_decor_noise && n > 40)))
        return;
    if (critical && n > 2000)
        return;
    json_escape(esc, sizeof(esc), path ? path : "");
    p = (const unsigned char *)peek_buf;
    if (p && p[0] == 'D' && p[1] == 'I' && p[2] == 'R' && p[3] == 'G') {
        dirg_peek(p, &magic, &cell, &bits);
        w = (unsigned)p[12] | ((unsigned)p[13] << 8) | ((unsigned)p[14] << 16) |
            ((unsigned)p[15] << 24);
        h = (unsigned)p[16] | ((unsigned)p[17] << 8) | ((unsigned)p[18] << 16) |
            ((unsigned)p[19] << 24);
    }
    {
        char scen_esc[280];
        const char *hid = "REMAP-B";
        json_escape(scen_esc, sizeof(scen_esc), g_last_scenario);
        if (strstr(api, "CHPFS"))
            hid = "REMAP-C";
        else if (strstr(u, ".VQ") || (strstr(u, "TERRAIN\\") && strstr(u, ".VQ")) ||
                 strstr(u, "TERRAIN/"))
            hid = "TEX1";
        else if (strstr(u, "MAP.OBJ"))
            hid = "L4";
        else if (strstr(u, "DECOR") || strstr(u, "SEQUENCES") || strstr(u, "DECORS"))
            hid = "L3";
        else if (strstr(u, "TERRAIN") || strstr(u, "HEIGHT") || strstr(u, ".GRID"))
            hid = "L1";
    }
    if (critical || n <= 300)
        log_msg("map-vfs #%ld %s %s ok=%d dirg=%d cell=%u bits=%u", (long)n, api,
                path ? path : "", ok, magic == 0x47524944u ? 1 : 0, cell, bits);
}

static int __cdecl hook_AliasMount(const char *virt, const char *real)
{
    LONG n;
    int rc = real_AliasMount(virt, real);
    n = InterlockedIncrement(&g_am_logs);
    if (n <= 80)
        log_msg("alias-mount #%ld virt=%s real=%s rc=%d", (long)n, virt ? virt : "",
                real ? real : "", rc);
    return rc;
}

static void * __cdecl hook_DefFileOpen(const char *path, unsigned mode, void *a3)
{
    LONGLONG t0 = hitch_qpc_now();
    void *file = real_DefFileOpen(path, mode, a3);
    double dt = hitch_qpc_ms_since(t0);
    if (dt >= 50.0) {
        char extra[256];
        snprintf(extra, sizeof(extra), "{\"path\":\"%.180s\",\"mode\":%u,\"ok\":%d}",
                 path ? path : "", mode, file ? 1 : 0);
        hitch_note_ms("F5", "hooks.c:DefFileOpen", "slow-vfs", "DefFileOpen", dt, extra);
    }
    if (path_is_safe(path)) {
        path_upper_copy(g_last_dfo_path, sizeof(g_last_dfo_path), path);
        g_last_dfo_ok = file ? 1 : 0;
        /* Track last scenario/adventure BFHP for multi-map deep trace. */
        if (file && (strstr(g_last_dfo_path, ".BFHP") || strstr(g_last_dfo_path, ".bfhp"))) {
            if (strstr(g_last_dfo_path, "SCENARIO") || strstr(g_last_dfo_path, "ADVENTURE") ||
                strstr(g_last_dfo_path, "EMPTY") || strstr(g_last_dfo_path, "NEWMAP") ||
                strstr(g_last_dfo_path, "PACKS\\") || strstr(g_last_dfo_path, "PACKS/")) {
                lstrcpynA(g_last_scenario, path, (int)sizeof(g_last_scenario));
            }
        }
    } else {
        g_last_dfo_path[0] = '\0';
        g_last_dfo_ok = 0;
    }
    log_mapload_vfs("DefFileOpen", path ? path : "", mode, file ? 1 : 0, NULL);
    log_editor_vfs("DefFileOpen", path ? path : "", mode, file ? 1 : 0);
    return file;
}

static int path_has_sequences(const char *path)
{
    char u[512];
    if (!path_is_safe(path))
        return 0;
    path_upper_copy(u, sizeof(u), path);
    return strstr(u, "SEQUENCES") != NULL;
}

/* Peek DIRG header: magic + cell + bits (retail grid / Sequences.bin). */
static void dirg_peek(const void *buf, unsigned *magic, unsigned *cell, unsigned *bits)
{
    const unsigned char *p = (const unsigned char *)buf;
    *magic = *cell = *bits = 0;
    if (!p)
        return;
    *magic = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
             ((unsigned)p[3] << 24);
    if (*magic == 0x47524944u) { /* 'DIRG' LE */
        *cell = (unsigned)p[4] | ((unsigned)p[5] << 8) | ((unsigned)p[6] << 16) |
                ((unsigned)p[7] << 24);
        *bits = (unsigned)p[8] | ((unsigned)p[9] << 8) | ((unsigned)p[10] << 16) |
                ((unsigned)p[11] << 24);
    }
}

static void log_seq_open(const char *api, const char *path, unsigned mode, int ok,
                         const void *buf, const char *hypothesisId)
{
    char esc[520];
    unsigned magic = 0, cell = 0, bits = 0;
    LONG n;

    if (!path_has_sequences(path))
        return;

    n = InterlockedIncrement(&g_fl_logs);
    if (n > 200)
        return;
    if (buf)
        dirg_peek(buf, &magic, &cell, &bits);
    json_escape(esc, sizeof(esc), path ? path : "");
    log_msg("seq-open #%ld %s %s ok=%d dirg=%d", (long)n, api, path ? path : "", ok,
            magic == 0x47524944u ? 1 : 0);
}

static void * __cdecl hook_FileLoad(const char *path, void *a2, int *err)
{
    void *buf = real_FileLoad(path, a2, err);
    log_mapload_vfs("FileLoad", path ? path : "", 0x41, buf ? 1 : 0, buf);
    log_editor_vfs("FileLoad", path ? path : "", 0x41, buf ? 1 : 0);
    return buf;
}

static void *__attribute__((thiscall)) hook_CHPFS_Open(void *self, const char *path,
                                                       unsigned mode, void *err)
{
    void *file = real_CHPFS_Open(self, path, mode, err);
    if (path_is_safe(path)) {
        path_upper_copy(g_last_dfo_path, sizeof(g_last_dfo_path), path);
        g_last_dfo_ok = file ? 1 : 0;
    }
    log_mapload_vfs("CHPFS_Open", path ? path : "", mode, file ? 1 : 0, NULL);
    log_editor_vfs("CHPFS_Open", path ? path : "", mode, file ? 1 : 0);
    return file;
}

static int path_is_terrain_grid(const char *path)
{
    char u[512];
    if (!path || !path[0])
        return 0;
    path_upper_copy(u, sizeof(u), path);
    return strstr(u, "TERRAIN") != NULL || strstr(u, ".GRID") != NULL ||
           strstr(u, "CURRENTMAP") != NULL || strstr(u, "MAPS\\") != NULL ||
           strstr(u, "MAPS/") != NULL;
}

static void log_dirg_load(const char *api, const char *hypothesisId, void *self, int rc,
                          void *retaddr, volatile LONG *counter, int terrain_only)
{
    char esc[520];
    char scen[280];
    char role[32];
    char selfhex[132];
    LONG n;
    unsigned *sw;
    unsigned char sample[16];
    int i;
    void *data_ptr;
    unsigned uniq = 0, nz = 0, maxv = 0, s0 = 0;
    unsigned ww = 0, wh = 0, cell = 0;
    unsigned char seen[256];
    size_t ncells = 0, ni;

    if (terrain_only && !path_is_terrain_grid(g_last_dfo_path))
        return;

    n = InterlockedIncrement(counter);
    if (n > 400)
        return;
    sw = (unsigned *)self;
    data_ptr = self ? *(void **)((char *)self + 8) : NULL;
    ww = self ? sw[0x10 / 4] : 0u;
    wh = self ? sw[0x14 / 4] : 0u;
    if (strstr(api, "32"))
        cell = 32;
    else if (strstr(api, "16_1") || strstr(api, "LoadDirg16"))
        cell = 16;
    else if (strstr(api, "64"))
        cell = 64;
    memset(sample, 0, sizeof(sample));
    memset(seen, 0, sizeof(seen));
    selfhex[0] = '\0';
    if (self) {
        unsigned char *sb = (unsigned char *)self;
        for (i = 0; i < 32; ++i)
            snprintf(selfhex + i * 2, 3, "%02x", sb[i]);
    }
    if (data_ptr) {
        for (i = 0; i < 16; ++i)
            sample[i] = ((unsigned char *)data_ptr)[i];
        s0 = sample[0];
        ncells = 4096;
        if (ww && wh && cell)
            ncells = (size_t)(ww / cell) * (size_t)(wh / cell);
        if (ncells > 65536)
            ncells = 65536;
        if (ncells < 256)
            ncells = 4096;
        for (ni = 0; ni < ncells; ++ni) {
            unsigned char v = ((unsigned char *)data_ptr)[ni];
            if (!seen[v]) {
                seen[v] = 1;
                ++uniq;
            }
            if (v) {
                ++nz;
                if (v > maxv)
                    maxv = v;
            }
        }
    }
    role[0] = '\0';
    if (strstr(g_last_dfo_path, "TERRAIN.PASS"))
        lstrcpyA(role, "pass");
    else if (strstr(g_last_dfo_path, "TERRAIN.HEIGHT"))
        lstrcpyA(role, "height");
    else if (strstr(g_last_dfo_path, "TERRAIN.LIGHT"))
        lstrcpyA(role, "light");
    else if (strstr(g_last_dfo_path, "TERRAIN.DECOR"))
        lstrcpyA(role, "decor");
    else if (strstr(g_last_dfo_path, "TERRAIN.TRANS"))
        lstrcpyA(role, "trans");
    else if (strstr(g_last_dfo_path, "TERRAIN.TERRAIN"))
        lstrcpyA(role, "terrain_z");
    else if (strstr(g_last_dfo_path, "SEQUENCES"))
        lstrcpyA(role, "sequences");
    json_escape(esc, sizeof(esc), g_last_dfo_path);
    json_escape(scen, sizeof(scen), g_last_scenario);
    log_msg("%s #%ld role=%s path=%s ww=%u wh=%u uniq=%u nz=%u max=%u", api, (long)n, role,
            g_last_dfo_path, ww, wh, uniq, nz, maxv);
}

static int __attribute__((thiscall)) hook_LoadDirg64(void *self, void *file)
{
    int rc = real_LoadDirg64(self, file);
    log_dirg_load("LoadDirg64_16", "MAP2", self, rc, __builtin_return_address(0), &g_d64_logs, 0);
    if (self && strstr(g_last_dfo_path, "TERRAIN.DECOR")) {
        void *expect = NULL;
        if (g_last_attach_map && !IsBadReadPtr(g_last_attach_map, 8)) {
            void *inner = *(void **)((char *)g_last_attach_map + 4);
            if (inner && !IsBadReadPtr(inner, 0xc0))
                expect = (char *)inner + 0x78;
        }
        /* Only remember CurrentMap decor — settlements pack is a separate junk grid. */
        if (self == expect || strstr(g_last_dfo_path, "CURRENTMAP"))
            g_last_decor_dirg = self;
        log_msg("decor-dirg self=%p expect=%p match=%d path=%s", self, expect,
                (self == expect) ? 1 : 0, g_last_dfo_path);
        /* #region agent log */
        {
            char data[220];
            snprintf(data, sizeof(data),
                     "{\"self\":%u,\"expect\":%u,\"match\":%d,\"kept\":%d,\"path\":\"%.80s\"}",
                     (unsigned)(ULONG_PTR)self, (unsigned)(ULONG_PTR)expect,
                     (self == expect) ? 1 : 0, (g_last_decor_dirg == self) ? 1 : 0, g_last_dfo_path);
            hooks_agent("E-DIRG", "hooks_vfs_map.c:LoadDirg64", "decor dirg load", data);
        }
        /* #endregion */
    }
    return rc;
}

static int __attribute__((thiscall)) hook_LoadDirg32(void *self, void *file)
{
    int rc = real_LoadDirg32(self, file);
    log_dirg_load("LoadDirg32_8", "MAP3", self, rc, __builtin_return_address(0), &g_d32_logs, 0);
    return rc;
}

static int __attribute__((thiscall)) hook_LoadDirg16(void *self, void *file)
{
    /* Expects DIRG cell=16 bits=1 (static from 0x466284/0x46628f). */
    int rc = real_LoadDirg16(self, file);
    /* Skip MAPOBJECTS\*.PASS noise — only log terrain / CurrentMap grids. */
    log_dirg_load("LoadDirg16_1", "MAP4", self, rc, __builtin_return_address(0), &g_d16_logs, 1);
    return rc;
}

static int __attribute__((thiscall)) hook_LoadDirg64_4(void *self, void *file)
{
    int rc = real_LoadDirg64_4(self, file);
    log_dirg_load("LoadDirg64_4", "MAP5", self, rc, __builtin_return_address(0), &g_d644_logs, 0);
    return rc;
}

static int __attribute__((thiscall)) hook_AttachPass(void *self, const char *path)
{
    char esc[520];
    LONG n;
    int rc;

    rc = real_AttachPass(self, path);
    n = InterlockedIncrement(&g_ap_logs);
    if (n <= 80) {
        json_escape(esc, sizeof(esc), path ? path : g_last_dfo_path);
        log_msg("AttachPassGrid #%ld path=%s rc=%d", (long)n, path ? path : "", rc);
    }
    return rc;
}

/* Fingerprint a DIRG-like object: data@+8, world_w@+0x10, world_h@+0x14. */
static void dump_dirg_object(const char *role, void *obj, const char *hypothesisId, LONG epoch)
{
    char samplehex[48];
    unsigned *sw;
    void *data_ptr;
    unsigned ww, wh, uniq = 0, nz = 0, maxv = 0, s0 = 0;
    unsigned mid_nz = 0, mid_uniq = 0, mid_max = 0, mid_period4 = 0;
    unsigned char seen[256], mid_seen[256];
    unsigned char sample[8];
    size_t ncells, ni, cols, rows, mid, x;
    int i;

    if (!obj || IsBadReadPtr(obj, 0x18)) {
        log_msg("attach-dump %s SKIP bad-obj=%p", role, obj);
        return;
    }
    sw = (unsigned *)obj;
    data_ptr = *(void **)((char *)obj + 8);
    ww = sw[0x10 / 4];
    wh = sw[0x14 / 4];
    /* Guard: bogus world size or non-pointer data@+8 (editor PRE crash: AV @0x65). */
    if ((unsigned)(ULONG_PTR)data_ptr < 0x10000u)
        data_ptr = NULL;
    if (ww > 65536u || wh > 65536u) {
        ww = 0;
        wh = 0;
    }
    memset(seen, 0, sizeof(seen));
    memset(mid_seen, 0, sizeof(mid_seen));
    memset(sample, 0, sizeof(sample));
    samplehex[0] = '\0';
    ncells = 0;
    if (data_ptr && !IsBadReadPtr(data_ptr, 8)) {
        for (i = 0; i < 8; ++i)
            sample[i] = ((unsigned char *)data_ptr)[i];
        s0 = sample[0];
        snprintf(samplehex, sizeof(samplehex), "%02x%02x%02x%02x%02x%02x%02x%02x", sample[0],
                 sample[1], sample[2], sample[3], sample[4], sample[5], sample[6], sample[7]);
        ncells = 4096;
        if (ww >= 64 && wh >= 64) {
            /* Prefer cell=64 guess; if huge, clamp. */
            cols = ww / 64;
            rows = wh / 64;
            if (cols * rows > 0 && cols * rows <= 65536)
                ncells = cols * rows;
            else if ((ww / 32) * (wh / 32) <= 65536)
                ncells = (size_t)(ww / 32) * (size_t)(wh / 32);
        }
        if (ncells > 65536)
            ncells = 65536;
        if (IsBadReadPtr(data_ptr, (UINT_PTR)ncells)) {
            log_msg("attach-dump %s SKIP bad-data=%p ncells=%u ww=%u wh=%u", role, data_ptr,
                    (unsigned)ncells, ww, wh);
            ncells = 0;
        }
        for (ni = 0; ni < ncells; ++ni) {
            unsigned char v = ((unsigned char *)data_ptr)[ni];
            if (!seen[v]) {
                seen[v] = 1;
                ++uniq;
            }
            if (v) {
                ++nz;
                if (v > maxv)
                    maxv = v;
            }
        }
        /* Mid-row strip probe (native scrub target ~row 127/130 on 256-cell maps). */
        cols = (ww >= 64) ? (ww / 64) : 0;
        rows = (wh >= 64) ? (wh / 64) : 0;
        if (cols >= 16 && rows >= 16 && data_ptr && ncells > 0) {
            mid = rows / 2;
            if (mid * cols + cols <= ncells) {
                for (x = 0; x < cols; ++x) {
                    unsigned char v = ((unsigned char *)data_ptr)[mid * cols + x];
                    if (!mid_seen[v]) {
                        mid_seen[v] = 1;
                        ++mid_uniq;
                    }
                    if (v) {
                        ++mid_nz;
                        if (v > mid_max)
                            mid_max = v;
                        if ((x & 3) == 0 && v)
                            ++mid_period4;
                    }
                }
            }
        }
    }
    {
        char scen[280];
        json_escape(scen, sizeof(scen), g_last_scenario);
    }
    log_msg("attach-dump %s ww=%u wh=%u uniq=%u nz=%u mid_nz=%u mid_p4=%u", role, ww, wh, uniq, nz,
            mid_nz, mid_period4);
    (void)hypothesisId;
    (void)epoch;
    (void)s0;
    (void)samplehex;
}

/* Game attach layout (ebx=map): terrain-z = map self; height/light/trans under
 * [map+4]+0xB8 + {0x1032,0x105E,0x10B6}; decor=[map+4]+0x78; pass=[map+4]+0x4C. */
static void dump_attach_layers(void *map_self, const char *which, LONG epoch)
{
    void *inner;
    void *vx;
    LONG n;

    if (!map_self || IsBadReadPtr(map_self, 8))
        return;
    n = InterlockedIncrement(&g_attach_dump);
    if (n > 80)
        return;
    dump_dirg_object("terrain_z_self", map_self, "LAY1", epoch);
    inner = *(void **)((char *)map_self + 4);
    if (!inner || IsBadReadPtr(inner, 0xc0))
        return;
    dump_dirg_object("decor", (char *)inner + 0x78, "LAY3", epoch);
    dump_dirg_object("pass", (char *)inner + 0x4C, "LAY4", epoch);
    vx = *(void **)((char *)inner + 0xb8);
    if (!vx || IsBadReadPtr(vx, 0x10c0))
        return;
    dump_dirg_object("height", (char *)vx + 0x1032, "LAY2", epoch);
    dump_dirg_object("light", (char *)vx + 0x105e, "LAY2", epoch);
    dump_dirg_object("trans", (char *)vx + 0x10b6, "LAY2", epoch);
    dump_dirg_object("terrain_z_108a", (char *)vx + 0x108a, "LAY1", epoch);
    (void)which;
}

static void __cdecl hook_BuildLight(void *height, void *light, unsigned a, unsigned b, unsigned c,
                                    unsigned d)
{
    LONG n;
    unsigned *hw, *lw;
    void *hdata, *ldata;
    unsigned h_ww = 0, h_wh = 0, l_ww = 0, l_wh = 0;
    unsigned hun = 0, hnz = 0, lun = 0, lnz = 0, lmax = 0, lmid = 0;
    unsigned char seen[256];
    size_t i, ncells;
    void *retaddr = __builtin_return_address(0);

    n = InterlockedIncrement(&g_bl_logs);
    hw = (unsigned *)height;
    lw = (unsigned *)light;
    hdata = height ? *(void **)((char *)height + 8) : NULL;
    ldata = light ? *(void **)((char *)light + 8) : NULL;
    if (height) {
        h_ww = hw[0x10 / 4];
        h_wh = hw[0x14 / 4];
    }
    if (light) {
        l_ww = lw[0x10 / 4];
        l_wh = lw[0x14 / 4];
    }
    if (hdata && h_ww && h_wh) {
        ncells = (size_t)(h_ww / 32) * (size_t)(h_wh / 32);
        if (ncells > 16384)
            ncells = 16384;
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < ncells; ++i) {
            unsigned char v = ((unsigned char *)hdata)[i];
            if (!seen[v]) {
                seen[v] = 1;
                ++hun;
            }
            if (v)
                ++hnz;
        }
    }
    real_BuildLight(height, light, a, b, c, d);
    if (ldata && l_ww && l_wh) {
        ncells = (size_t)(l_ww / 32) * (size_t)(l_wh / 32);
        if (ncells > 16384)
            ncells = 16384;
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < ncells; ++i) {
            unsigned char v = ((unsigned char *)ldata)[i];
            if (!seen[v]) {
                seen[v] = 1;
                ++lun;
            }
            if (v) {
                ++lnz;
                if (v > lmax)
                    lmax = v;
            }
        }
        if (ncells)
            lmid = ((unsigned char *)ldata)[ncells / 2];
    }
    if (n <= 80) {
        char scen[280];
        json_escape(scen, sizeof(scen), g_last_scenario);
        log_msg("BuildLight #%ld rect=(%u,%u,%u,%u) hnz=%u lnz=%u lmax=%u epoch=%ld", (long)n, a, b,
                c, d, hnz, lnz, lmax, (long)g_map_epoch);
    }
}

static int __attribute__((thiscall)) hook_AttachTerrain_common(void *self, const char *prefix,
                                                               PFN_AttachTerrain real, const char *which,
                                                               volatile LONG *counter,
                                                               const char *hypothesisId)
{
    char esc[520];
    char scen[280];
    LONG n, epoch, seq;
    int rc;
    int sw = 0, sh = 0;

    epoch = InterlockedIncrement(&g_map_epoch);
    seq = InterlockedIncrement(&g_map_seq);
    g_last_attach_map = self;
    /* Drop game DecorSpawn buffer — editor never respawns; vk_decor refills from DIRG. */
    decor_spawn_clear();
    obj_spawn_clear();
    InterlockedExchange(&g_deep_attach, 1);
    json_escape(esc, sizeof(esc), prefix ? prefix : "");
    json_escape(scen, sizeof(scen), g_last_scenario);
    vk_present_soft_size(&sw, &sh);
    hooks_crash_phase("AttachTerrain-%s PRE epoch=%ld soft=%dx%d map=%ux%u scen=%s", which,
                      (long)epoch, sw, sh, g_last_map_f4, g_last_map_f8, g_last_scenario);
    log_msg("AttachTerrain-%s ENTER prefix=%s scen=%s epoch=%ld soft=%dx%d self=%p map=%ux%u",
            which, prefix ? prefix : "", g_last_scenario, (long)epoch, sw, sh, self, g_last_map_f4,
            g_last_map_f8);
    /* Dump BEFORE native — prior unsafe dump AV'd on bad DIRG; guarded now. */
    dump_attach_layers(self, which, epoch);

    hooks_crash_phase("AttachTerrain-%s IN epoch=%ld", which, (long)epoch);
    rc = real(self, prefix);

    n = InterlockedIncrement(counter);
    seq = InterlockedIncrement(&g_map_seq);
    dump_attach_layers(self, which, epoch);
    InterlockedExchange(&g_deep_attach, 0);
    hooks_crash_phase("AttachTerrain-%s OUT rc=%d epoch=%ld", which, rc, (long)epoch);
    log_msg("AttachTerrain-%s LEAVE rc=%d epoch=%ld", which, rc, (long)epoch);
    (void)hypothesisId;
    (void)seq;
    (void)esc;
    (void)scen;
    return rc;
}

static int scenario_is_editor_template(void)
{
    char u[512];
    path_upper_copy(u, sizeof(u), g_last_scenario);
    return strstr(u, "EMPTYADV") != NULL || strstr(u, "NEWMAP") != NULL ||
           strstr(u, "EMPTYSCN") != NULL;
}

static int __attribute__((thiscall)) hook_AttachGame(void *self, const char *prefix)
{
    /* Imperivm2 map editor still calls AttachTerrain-game (never -editor). Do not
     * clear editor flag on emptyadv/newmap templates. */
    if (scenario_is_editor_template())
        InterlockedExchange(&g_ck_map_editor, 1);
    else
        InterlockedExchange(&g_ck_map_editor, 0);
    return hook_AttachTerrain_common(self, prefix, real_AttachGame, "game", &g_atg_logs, "ATTACH-A");
}

static int __attribute__((thiscall)) hook_AttachEdit(void *self, const char *prefix)
{
    InterlockedExchange(&g_ck_map_editor, 1);
    return hook_AttachTerrain_common(self, prefix, real_AttachEdit, "editor", &g_ate_logs, "ATTACH-B");
}

static int __cdecl hook_LoadMap(void *mapdata, int load_obj, int flag)
{
    char scen[280];
    LONG n, seq;
    int rc;
    unsigned f4 = 0, f8 = 0;
    void *retaddr = __builtin_return_address(0);
    LONGLONG t0 = hitch_qpc_now();

    hitch_mark("LoadMap");
    n = InterlockedIncrement(&g_lm_logs);
    seq = InterlockedIncrement(&g_map_seq);
    if (mapdata) {
        f4 = *(unsigned *)((char *)mapdata + 4);
        f8 = *(unsigned *)((char *)mapdata + 8);
        g_last_map_f4 = f4;
        g_last_map_f8 = f8;
    }
    json_escape(scen, sizeof(scen), g_last_scenario);
    log_msg("LoadMap ENTER #%ld md=%p f4=%u f8=%u load_obj=%d flag=%d", (long)n, mapdata, f4, f8,
            load_obj, flag);
    hooks_crash_phase("LoadMap ENTER #%ld f4=%u f8=%u", (long)n, f4, f8);

    rc = real_LoadMap(mapdata, load_obj, flag);

    seq = InterlockedIncrement(&g_map_seq);
    log_msg("LoadMap LEAVE #%ld rc=%d", (long)n, rc);
    hooks_crash_phase("LoadMap LEAVE #%ld rc=%d", (long)n, rc);
    hitch_note_ms("F5d", "hooks.c:LoadMap", "load-map", "LoadMap", hitch_qpc_ms_since(t0), "{}");
    (void)retaddr;
    return rc;
}

static void __cdecl hook_LoadMapPath(const char *path, void *mapdata, int load_obj, int flag)
{
    char esc[520];
    char scen[280];
    LONG n, seq;
    void *retaddr = __builtin_return_address(0);

    n = InterlockedIncrement(&g_lmp_logs);
    seq = InterlockedIncrement(&g_map_seq);
    json_escape(esc, sizeof(esc), path ? path : "");
    json_escape(scen, sizeof(scen), g_last_scenario);
    log_msg("LoadMapPath ENTER #%ld path=%s load_obj=%d", (long)n, path ? path : "", load_obj);

    real_LoadMapPath(path, mapdata, load_obj, flag);

    seq = InterlockedIncrement(&g_map_seq);
    log_msg("LoadMapPath LEAVE #%ld", (long)n);
}

static unsigned __attribute__((thiscall)) hook_BeginElement(void *self, void *parser, void *tag)
{
    char name_esc[200];
    char scen[280];
    const char *name = "";
    LONG n, seq;
    unsigned rc;
    void *retaddr = __builtin_return_address(0);

    n = InterlockedIncrement(&g_be_logs);
    if (tag) {
        char *pn = *(char **)tag;
        if (pn && !IsBadStringPtrA(pn, 64) && pn[0] >= 0x20 && pn[0] < 0x7f)
            name = pn;
    }
    /* Cap volume: always log interesting tags; otherwise first 150. */
    if (n <= 150 || strstr(name, "map") || strstr(name, "script") || strstr(name, "grid") ||
        strstr(name, "settlement") || strstr(name, "group") || strstr(name, "object")) {
        seq = InterlockedIncrement(&g_map_seq);
        json_escape(name_esc, sizeof(name_esc), name);
        json_escape(scen, sizeof(scen), g_last_scenario);
        if (n <= 40 || strstr(name, "mapobject") || strstr(name, "scriptobj"))
            log_msg("BeginElement #%ld tag=%s", (long)n, name);
    }

    rc = real_BeginElement(self, parser, tag);
    return rc;
}

static unsigned __attribute__((thiscall)) hook_InitSequences(void *self)
{
    char scen[280];
    LONG n, seq;
    unsigned rc;
    void *retaddr = __builtin_return_address(0);

    n = InterlockedIncrement(&g_is_logs);
    seq = InterlockedIncrement(&g_map_seq);
    json_escape(scen, sizeof(scen), g_last_scenario);
    log_msg("InitSequences ENTER #%ld self=%p", (long)n, self);

    rc = real_InitSequences(self);

    seq = InterlockedIncrement(&g_map_seq);
    log_msg("InitSequences LEAVE #%ld rc=%u", (long)n, rc);
    return rc;
}

static int __attribute__((thiscall)) hook_LoadTerrainZ(void *self, void *file)
{
    int rc = real_LoadTerrainZ(self, file);
    /* Correlate with last open path — should be …terrain.terrain.grid / CurrentMap. */
    if (!strstr(g_last_dfo_path, "TERRAIN.TERRAIN") && g_last_dfo_path[0]) {
        /* still log — retail may open via short suffix */
    }
    log_dirg_load("LoadTerrainZ_64x4", "ATTACH-Z", self, rc, __builtin_return_address(0),
                  &g_ltz_logs, 0);
    return rc;
}

static HANDLE WINAPI hook_CreateFileA(LPCSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                                      HANDLE template_file)
{
    HANDLE h = real_CreateFileA(path, access, share, sa, disp, flags, template_file);
    log_mapload_vfs("CreateFileA", path ? path : "", access,
                    (h && h != INVALID_HANDLE_VALUE) ? 1 : 0, NULL);
    log_editor_vfs("CreateFileA", path ? path : "", access,
                   (h && h != INVALID_HANDLE_VALUE) ? 1 : 0);
    return h;
}


void hooks_vfs_install(void)
{
    HMODULE game = GetModuleHandleA(NULL);
    int is_tpw = looks_like_tpw();

    /* Map-load VAs are tpw.exe-only. Celtic_Kings.exe must NOT get them. */
    if (is_tpw) {
    /* DefFileOpen: sub esp,8; lea eax,[esp] = 7 bytes */
    g_dfo_target = (void *)(ULONG_PTR)0x0040F610;
    g_dfo_steal = 7;
    if (install_inline_hook(g_dfo_target, (void *)hook_DefFileOpen, g_dfo_steal, &g_dfo_tramp,
                            g_dfo_saved)) {
        real_DefFileOpen = (PFN_DefFileOpen)g_dfo_tramp;
        log_msg("inline hooked DefFileOpen @ %p", g_dfo_target);
    } else {
        log_msg("WARN: DefFileOpen hook failed");
    }

    /* FileLoad: push ebx; push ebp; mov ebp,[esp+14h] = 8 bytes */
    g_fl_target = (void *)(ULONG_PTR)0x0040F980;
    g_fl_steal = 8;
    if (install_inline_hook(g_fl_target, (void *)hook_FileLoad, g_fl_steal, &g_fl_tramp,
                            g_fl_saved)) {
        real_FileLoad = (PFN_FileLoad)g_fl_tramp;
        log_msg("inline hooked FileLoad @ %p", g_fl_target);
    } else {
        log_msg("WARN: FileLoad hook failed");
    }

    /* HPFS/CHPFS CFileService::Open: sub esp,110h; push ebx = 7 bytes */
    g_chp_target = (void *)(ULONG_PTR)0x00414C60;
    g_chp_steal = 7;
    if (install_inline_hook(g_chp_target, (void *)hook_CHPFS_Open, g_chp_steal, &g_chp_tramp,
                            g_chp_saved)) {
        real_CHPFS_Open = (PFN_CHPFS_Open)g_chp_tramp;
        log_msg("inline hooked CHPFS_Open @ %p", g_chp_target);
    } else {
        log_msg("WARN: CHPFS_Open hook failed");
    }

    /* LoadDirg64_16: sub esp,14h; push ebx; mov ebx,[esp+1Ch] = 8 bytes */
    g_d64_target = (void *)(ULONG_PTR)0x00535CD0;
    g_d64_steal = 8;
    if (install_inline_hook(g_d64_target, (void *)hook_LoadDirg64, g_d64_steal, &g_d64_tramp,
                            g_d64_saved)) {
        real_LoadDirg64 = (PFN_LoadDirg)g_d64_tramp;
        log_msg("inline hooked LoadDirg64_16 @ %p", g_d64_target);
    } else {
        log_msg("WARN: LoadDirg64_16 hook failed");
    }

    /* LoadDirg32_8: sub esp,14h; push ebx; mov ebx,[esp+1Ch] = 8 bytes */
    g_d32_target = (void *)(ULONG_PTR)0x005503C0;
    g_d32_steal = 8;
    if (install_inline_hook(g_d32_target, (void *)hook_LoadDirg32, g_d32_steal, &g_d32_tramp,
                            g_d32_saved)) {
        real_LoadDirg32 = (PFN_LoadDirg)g_d32_tramp;
        log_msg("inline hooked LoadDirg32_8 @ %p", g_d32_target);
    } else {
        log_msg("WARN: LoadDirg32_8 hook failed");
    }

    /* LoadDirg16_1: same prologue as other LoadDirg* (filter logs to terrain paths) */
    g_d16_target = (void *)(ULONG_PTR)0x00466230;
    g_d16_steal = 8;
    if (install_inline_hook(g_d16_target, (void *)hook_LoadDirg16, g_d16_steal, &g_d16_tramp,
                            g_d16_saved)) {
        real_LoadDirg16 = (PFN_LoadDirg)g_d16_tramp;
        log_msg("inline hooked LoadDirg16_1 @ %p", g_d16_target);
    } else {
        log_msg("WARN: LoadDirg16_1 hook failed");
    }

    /* LoadDirg64_4: sub esp,14h; push ebx; mov ebx,[esp+1Ch] = 8 bytes */
    g_d644_target = (void *)(ULONG_PTR)0x0047A760;
    g_d644_steal = 8;
    if (install_inline_hook(g_d644_target, (void *)hook_LoadDirg64_4, g_d644_steal, &g_d644_tramp,
                            g_d644_saved)) {
        real_LoadDirg64_4 = (PFN_LoadDirg)g_d644_tramp;
        log_msg("inline hooked LoadDirg64_4 @ %p", g_d644_target);
    } else {
        log_msg("WARN: LoadDirg64_4 hook failed");
    }

    /* AttachPassGrid @ 0x54b830:
     *   push esi; push edi; lea eax,[esp+0Ch]; push eax  = 7 bytes
     * (steal=8 was mid-instruction on mov edi,ecx → crash) */
    g_ap_target = (void *)(ULONG_PTR)0x0054B830;
    g_ap_steal = 7;
    if (install_inline_hook(g_ap_target, (void *)hook_AttachPass, g_ap_steal, &g_ap_tramp,
                            g_ap_saved)) {
        real_AttachPass = (PFN_AttachPassGrid)g_ap_tramp;
        log_msg("inline hooked AttachPassGrid @ %p", g_ap_target);
    } else {
        log_msg("WARN: AttachPassGrid hook failed");
    }

    /* Game AttachTerrain @ 0x54BE40: sub esp,0x3F0 = 6 bytes */
    g_atg_target = (void *)(ULONG_PTR)0x0054BE40;
    g_atg_steal = 6;
    if (install_inline_hook(g_atg_target, (void *)hook_AttachGame, g_atg_steal, &g_atg_tramp,
                            g_atg_saved)) {
        real_AttachGame = (PFN_AttachTerrain)g_atg_tramp;
        log_msg("inline hooked AttachTerrainGame @ %p", g_atg_target);
    } else {
        log_msg("WARN: AttachTerrainGame hook failed");
    }

    /* Editor AttachTerrain @ 0x54BBC0: same prologue */
    g_ate_target = (void *)(ULONG_PTR)0x0054BBC0;
    g_ate_steal = 6;
    if (install_inline_hook(g_ate_target, (void *)hook_AttachEdit, g_ate_steal, &g_ate_tramp,
                            g_ate_saved)) {
        real_AttachEdit = (PFN_AttachTerrain)g_ate_tramp;
        log_msg("inline hooked AttachTerrainEditor @ %p", g_ate_target);
    } else {
        log_msg("WARN: AttachTerrainEditor hook failed");
    }

    /* LoadTerrainZ 64×4 @ 0x54B890: sub esp,50h; mov eax,[imm32] = 8 bytes */
    g_ltz_target = (void *)(ULONG_PTR)0x0054B890;
    g_ltz_steal = 8;
    if (install_inline_hook(g_ltz_target, (void *)hook_LoadTerrainZ, g_ltz_steal, &g_ltz_tramp,
                            g_ltz_saved)) {
        real_LoadTerrainZ = (PFN_LoadDirg)g_ltz_tramp;
        log_msg("inline hooked LoadTerrainZ @ %p", g_ltz_target);
    } else {
        log_msg("WARN: LoadTerrainZ hook failed");
    }

    /* AliasMount @ 0x434620: sub esp,0Ch; push ebx; push ebp; push esi; push edi = 7 bytes */
    g_am_target = (void *)(ULONG_PTR)0x00434620;
    g_am_steal = 7;
    if (install_inline_hook(g_am_target, (void *)hook_AliasMount, g_am_steal, &g_am_tramp,
                            g_am_saved)) {
        real_AliasMount = (PFN_AliasMount)g_am_tramp;
        log_msg("inline hooked AliasMount @ %p", g_am_target);
    } else {
        log_msg("WARN: AliasMount hook failed");
    }

    /* BuildLight @ 0x546940: sub esp,24h; mov eax,[esp+3Ch]; push ebx = 8 bytes */
    g_bl_target = (void *)(ULONG_PTR)0x00546940;
    g_bl_steal = 8;
    if (install_inline_hook(g_bl_target, (void *)hook_BuildLight, g_bl_steal, &g_bl_tramp,
                            g_bl_saved)) {
        real_BuildLight = (PFN_BuildLight)g_bl_tramp;
        log_msg("inline hooked BuildLight @ %p", g_bl_target);
    } else {
        log_msg("WARN: BuildLight hook failed");
    }

    /* LoadMap @ 0x550030: mov ecx,[0x8c7b30] = 6 bytes */
    g_lm_target = (void *)(ULONG_PTR)0x00550030;
    g_lm_steal = 6;
    if (install_inline_hook(g_lm_target, (void *)hook_LoadMap, g_lm_steal, &g_lm_tramp,
                            g_lm_saved)) {
        real_LoadMap = (PFN_LoadMap)g_lm_tramp;
        log_msg("inline hooked LoadMap @ %p", g_lm_target);
    } else {
        log_msg("WARN: LoadMap hook failed");
    }

    /* LoadMap_path @ 0x550230: mov eax,[esp+4]; push esi = 5 bytes */
    g_lmp_target = (void *)(ULONG_PTR)0x00550230;
    g_lmp_steal = 5;
    if (install_inline_hook(g_lmp_target, (void *)hook_LoadMapPath, g_lmp_steal, &g_lmp_tramp,
                            g_lmp_saved)) {
        real_LoadMapPath = (PFN_LoadMapPath)g_lmp_tramp;
        log_msg("inline hooked LoadMapPath @ %p", g_lmp_target);
    } else {
        log_msg("WARN: LoadMapPath hook failed");
    }

    /* BeginElement @ 0x54EF60: sub esp,0x120 = 6 bytes */
    g_be_target = (void *)(ULONG_PTR)0x0054EF60;
    g_be_steal = 6;
    if (install_inline_hook(g_be_target, (void *)hook_BeginElement, g_be_steal, &g_be_tramp,
                            g_be_saved)) {
        real_BeginElement = (PFN_BeginElement)g_be_tramp;
        log_msg("inline hooked BeginElement @ %p", g_be_target);
    } else {
        log_msg("WARN: BeginElement hook failed");
    }

    /* InitSequences @ 0x5425C0: push -1; push seh_handler = 7 bytes (SEH) */
    g_is_target = (void *)(ULONG_PTR)0x005425C0;
    g_is_steal = 7;
    if (install_inline_hook(g_is_target, (void *)hook_InitSequences, g_is_steal, &g_is_tramp,
                            g_is_saved)) {
        real_InitSequences = (PFN_InitSequences)g_is_tramp;
        log_msg("inline hooked InitSequences @ %p", g_is_target);
    } else {
        log_msg("WARN: InitSequences hook failed");
    }

    } else {
        log_msg("SKIP tpw map-load hooks — Celtic_Kings/unknown binary (terrain-only mode)");
    }

    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32)
            real_CreateFileA = (PFN_CreateFileA)GetProcAddress(k32, "CreateFileA");
        if (game && real_CreateFileA) {
            if (patch_iat_entry(game, "KERNEL32.dll", "CreateFileA", (void *)hook_CreateFileA,
                                (void **)&real_CreateFileA, &g_iat_CreateFileA))
                log_msg("IAT hooked kernel32!CreateFileA");
            else
                log_msg("WARN: CreateFileA IAT hook failed");
        }
    }
}

void hooks_vfs_remove_iat(void)
{
    restore_iat_slot(g_iat_CreateFileA, (void *)real_CreateFileA);
}

void hooks_vfs_remove(void)
{
    remove_inline_hook(g_dfo_target, g_dfo_steal, g_dfo_saved, g_dfo_tramp);
    g_dfo_tramp = NULL;
    remove_inline_hook(g_fl_target, g_fl_steal, g_fl_saved, g_fl_tramp);
    g_fl_tramp = NULL;
    remove_inline_hook(g_chp_target, g_chp_steal, g_chp_saved, g_chp_tramp);
    g_chp_tramp = NULL;
    remove_inline_hook(g_d64_target, g_d64_steal, g_d64_saved, g_d64_tramp);
    g_d64_tramp = NULL;
    remove_inline_hook(g_d32_target, g_d32_steal, g_d32_saved, g_d32_tramp);
    g_d32_tramp = NULL;
    remove_inline_hook(g_d16_target, g_d16_steal, g_d16_saved, g_d16_tramp);
    g_d16_tramp = NULL;
    remove_inline_hook(g_d644_target, g_d644_steal, g_d644_saved, g_d644_tramp);
    g_d644_tramp = NULL;
    remove_inline_hook(g_ap_target, g_ap_steal, g_ap_saved, g_ap_tramp);
    g_ap_tramp = NULL;
    remove_inline_hook(g_atg_target, g_atg_steal, g_atg_saved, g_atg_tramp);
    g_atg_tramp = NULL;
    remove_inline_hook(g_ate_target, g_ate_steal, g_ate_saved, g_ate_tramp);
    g_ate_tramp = NULL;
    remove_inline_hook(g_ltz_target, g_ltz_steal, g_ltz_saved, g_ltz_tramp);
    g_ltz_tramp = NULL;
    remove_inline_hook(g_am_target, g_am_steal, g_am_saved, g_am_tramp);
    g_am_tramp = NULL;
    remove_inline_hook(g_bl_target, g_bl_steal, g_bl_saved, g_bl_tramp);
    g_bl_tramp = NULL;
    remove_inline_hook(g_lm_target, g_lm_steal, g_lm_saved, g_lm_tramp);
    g_lm_tramp = NULL;
    remove_inline_hook(g_lmp_target, g_lmp_steal, g_lmp_saved, g_lmp_tramp);
    g_lmp_tramp = NULL;
    remove_inline_hook(g_be_target, g_be_steal, g_be_saved, g_be_tramp);
    g_be_tramp = NULL;
    remove_inline_hook(g_is_target, g_is_steal, g_is_saved, g_is_tramp);
    g_is_tramp = NULL;

}
