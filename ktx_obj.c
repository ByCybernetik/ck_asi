#include "ktx_obj.h"
#include "hooks_internal.h"
#include "ktx_terrain.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extra unit clips (carry/work/…) pushed units.json over the old 36864 cap;
 * load_kind then rejected the whole units catalog → GPU-first hid all units. */
enum { OBJ_SPR_MAX = 65536, OBJ_ATL_MAX = 512 };

static char s_root[MAX_PATH];
static char s_atl_kind[OBJ_ATL_MAX][24]; /* "buildings" / "units" per atlas */
static int s_wanted;
static int s_ready;
static KtxObjSprite s_spr[OBJ_SPR_MAX];
static int s_spr_n;
static KtxObjAtlas s_atl[OBJ_ATL_MAX];
static int s_atl_n;

/* Unique entity-id set for O(1)-ish has_id (open addressing). */
enum { ID_HASH = 4096 };
static char s_id_hash[ID_HASH][32];
static int s_id_hash_n;
/* Per-id sprite linked lists (same hash space; collision resolved by id string). */
static int s_spr_head[ID_HASH];
static int s_spr_next[OBJ_SPR_MAX];
static char s_spr_hid[ID_HASH][32];

static unsigned id_hash_key(const char *id);
static void id_hash_rebuild(void);
static void spr_chain_rebuild(void);

static int dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void slash_norm(char *p)
{
    for (; p && *p; ++p)
        if (*p == '/')
            *p = '\\';
}

static int pick_root(char *out, size_t out_n)
{
    char cand[MAX_PATH];
    const char *tr;

    /* Sibling of terrain root: .../ktx/terrain → .../ktx/objects */
    tr = ktx_terrain_root();
    if (tr && tr[0]) {
        char tmp[MAX_PATH];
        char *slash;
        lstrcpynA(tmp, tr, (int)sizeof(tmp));
        slash_norm(tmp);
        slash = strrchr(tmp, '\\');
        if (slash && _stricmp(slash + 1, "terrain") == 0) {
            *slash = '\0';
            snprintf(cand, sizeof(cand), "%s\\objects", tmp);
            if (dir_exists(cand)) {
                lstrcpynA(out, cand, (int)out_n);
                return 1;
            }
        }
    }
    if (g_game_root[0]) {
        snprintf(cand, sizeof(cand), "%sktx\\objects", g_game_root);
        if (dir_exists(cand)) {
            lstrcpynA(out, cand, (int)out_n);
            return 1;
        }
        snprintf(cand, sizeof(cand), "%s..\\ck_asi\\ktx\\objects", g_game_root);
        if (dir_exists(cand)) {
            lstrcpynA(out, cand, (int)out_n);
            return 1;
        }
    }
    return 0;
}

static char *load_file(const char *path, size_t *len_out)
{
    FILE *f;
    long sz;
    char *buf;
    size_t n;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 32 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (len_out)
        *len_out = n;
    return buf;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    return p;
}

static const char *parse_string(const char *p, char *out, size_t out_n)
{
    size_t i = 0;
    p = skip_ws(p);
    if (*p != '"')
        return NULL;
    ++p;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
        }
        if (i + 1 < out_n)
            out[i++] = c;
    }
    if (*p != '"')
        return NULL;
    out[i] = '\0';
    return p + 1;
}

static const char *parse_number(const char *p, int *out)
{
    long v;
    char *end;
    p = skip_ws(p);
    v = strtol(p, &end, 10);
    if (end == p)
        return NULL;
    *out = (int)v;
    return end;
}

/* Find "key": within [begin, end). */
static const char *find_key_span(const char *begin, const char *end, const char *key)
{
    char pat[96];
    size_t plen;
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    plen = strlen(pat);
    for (p = begin; p + plen < end; ++p) {
        if (memcmp(p, pat, plen) != 0)
            continue;
        p += plen;
        p = skip_ws(p);
        if (p >= end || *p != ':')
            continue;
        p = skip_ws(p + 1);
        if (p < end)
            return p;
    }
    return NULL;
}

static int parse_atlases(const char *json)
{
    const char *arr = find_key_span(json, json + strlen(json), "atlases");
    const char *p;
    const char *end;

    s_atl_n = 0;
    if (!arr || *arr != '[')
        return 0;
    p = arr + 1;
    end = strchr(p, ']');
    if (!end)
        return 0;
    while (p < end && s_atl_n < OBJ_ATL_MAX) {
        const char *obj;
        const char *obj_end;
        const char *q;
        char tmp[64];
        int n;
        KtxObjAtlas *a;

        p = skip_ws(p);
        if (p >= end || *p == ']')
            break;
        if (*p != '{')
            break;
        obj = p;
        obj_end = strchr(obj + 1, '}');
        if (!obj_end || obj_end > end)
            break;
        p = obj_end + 1;
        a = &s_atl[s_atl_n];
        memset(a, 0, sizeof(*a));
        q = find_key_span(obj, obj_end, "ktx2");
        if (q && parse_string(q, tmp, sizeof(tmp)))
            lstrcpynA(a->ktx2, tmp, (int)sizeof(a->ktx2));
        q = find_key_span(obj, obj_end, "name");
        if (q && parse_string(q, tmp, sizeof(tmp)))
            lstrcpynA(a->name, tmp, (int)sizeof(a->name));
        q = find_key_span(obj, obj_end, "group");
        if (q && parse_string(q, tmp, sizeof(tmp)))
            lstrcpynA(a->group, tmp, (int)sizeof(a->group));
        q = find_key_span(obj, obj_end, "width");
        if (q && parse_number(q, &n))
            a->width = (uint32_t)n;
        q = find_key_span(obj, obj_end, "height");
        if (q && parse_number(q, &n))
            a->height = (uint32_t)n;
        q = find_key_span(obj, obj_end, "sprite_count");
        if (q && parse_number(q, &n))
            a->sprite_count = n;
        q = find_key_span(obj, obj_end, "bc");
        if (q && parse_string(q, tmp, sizeof(tmp)))
            a->is_rgba = (_stricmp(tmp, "rgba8") == 0 || _stricmp(tmp, "rgba") == 0) ? 1 : 0;
        q = find_key_span(obj, obj_end, "idx_ktx2");
        if (q && parse_string(q, tmp, sizeof(tmp))) {
            lstrcpynA(a->idx_ktx2, tmp, (int)sizeof(a->idx_ktx2));
            a->has_idx = a->idx_ktx2[0] ? 1 : 0;
        }
        if (a->ktx2[0]) {
            if (!a->name[0])
                lstrcpynA(a->name, a->ktx2, (int)sizeof(a->name));
            s_atl_n++;
        }
        p = skip_ws(p);
        if (*p == ',')
            ++p;
    }
    return s_atl_n > 0;
}

static int parse_one_sprite(const char *obj, const char *obj_end, KtxObjSprite *s)
{
    const char *q;
    char tmp[64];
    int n;

    memset(s, 0, sizeof(*s));
    q = find_key_span(obj, obj_end, "id");
    if (q && parse_string(q, tmp, sizeof(tmp)))
        lstrcpynA(s->id, tmp, (int)sizeof(s->id));
    q = find_key_span(obj, obj_end, "type");
    if (q && parse_number(q, &n))
        s->type = n;
    q = find_key_span(obj, obj_end, "frame");
    if (q && parse_number(q, &n))
        s->frame = n;
    q = find_key_span(obj, obj_end, "frames_x");
    if (q && parse_number(q, &n))
        s->frames_x = n;
    q = find_key_span(obj, obj_end, "frames_y");
    if (q && parse_number(q, &n))
        s->frames_y = n;
    q = find_key_span(obj, obj_end, "hot_x");
    if (q && parse_number(q, &n))
        s->hot_x = n;
    q = find_key_span(obj, obj_end, "hot_y");
    if (q && parse_number(q, &n))
        s->hot_y = n;
    q = find_key_span(obj, obj_end, "offset_x");
    if (q && parse_number(q, &n))
        s->offset_x = n;
    q = find_key_span(obj, obj_end, "offset_y");
    if (q && parse_number(q, &n))
        s->offset_y = n;
    q = find_key_span(obj, obj_end, "sort_ox");
    if (q && parse_number(q, &n))
        s->sort_ox = n;
    q = find_key_span(obj, obj_end, "sort_oy");
    if (q && parse_number(q, &n))
        s->sort_oy = n;
    q = find_key_span(obj, obj_end, "z");
    if (q && parse_number(q, &n))
        s->z = n;
    q = find_key_span(obj, obj_end, "w");
    if (q && parse_number(q, &n))
        s->w = n;
    q = find_key_span(obj, obj_end, "h");
    if (q && parse_number(q, &n))
        s->h = n;
    q = find_key_span(obj, obj_end, "atlas_x");
    if (q && parse_number(q, &n))
        s->atlas_x = n;
    q = find_key_span(obj, obj_end, "atlas_y");
    if (q && parse_number(q, &n))
        s->atlas_y = n;
    q = find_key_span(obj, obj_end, "atlas_index");
    if (q && parse_number(q, &n))
        s->atlas_index = n;
    q = find_key_span(obj, obj_end, "layer");
    if (q && parse_string(q, tmp, sizeof(tmp)))
        lstrcpynA(s->layer, tmp, (int)sizeof(s->layer));
    q = find_key_span(obj, obj_end, "drawmode");
    if (q && parse_string(q, tmp, sizeof(tmp)))
        lstrcpynA(s->drawmode, tmp, (int)sizeof(s->drawmode));
    q = find_key_span(obj, obj_end, "remaping");
    if (q && parse_string(q, tmp, sizeof(tmp)))
        lstrcpynA(s->remaping, tmp, (int)sizeof(s->remaping));
    q = find_key_span(obj, obj_end, "mode");
    if (q && parse_number(q, &n))
        s->mode = n;
    q = find_key_span(obj, obj_end, "imgrle_version");
    if (q && parse_number(q, &n))
        s->imgrle_version = n;
    else
        s->imgrle_version = 0;
    return s->w > 0 && s->h > 0;
}

static int parse_sprites(const char *json)
{
    const char *p = find_key_span(json, json + strlen(json), "sprites");
    s_spr_n = 0;
    if (!p || *p != '[')
        return 0;
    ++p;
    while (*p && s_spr_n < OBJ_SPR_MAX) {
        const char *obj;
        const char *close;
        KtxObjSprite spr;

        p = skip_ws(p);
        if (*p == ']')
            break;
        if (*p != '{')
            break;
        obj = p;
        close = strchr(obj + 1, '}');
        if (!close)
            break;
        if (parse_one_sprite(obj, close, &spr))
            s_spr[s_spr_n++] = spr;
        p = close + 1;
        p = skip_ws(p);
        if (*p == ',')
            ++p;
    }
    return s_spr_n;
}

/* Append one kind JSON (buildings.json / units.json); remaps atlas_index. */
static int load_kind(const char *kind, const char *json_name)
{
    char path[MAX_PATH];
    char *json;
    size_t len = 0;
    int atl0, spr0, i, n_atl, n_spr;
    KtxObjAtlas *atl_keep_buf = NULL;
    KtxObjSprite *spr_keep_buf = NULL;
    int atl_keep, spr_keep;

    snprintf(path, sizeof(path), "%s\\%s\\%s", s_root, kind, json_name);
    slash_norm(path);
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "%s/%s/%s", s_root, kind, json_name);
        if (!file_exists(path)) {
            log_msg("ktx_obj: skip missing %s/%s", kind, json_name);
            return 0;
        }
    }
    json = load_file(path, &len);
    if (!json) {
        log_msg("ktx_obj: read fail %s", path);
        return 0;
    }

    atl_keep = s_atl_n;
    spr_keep = s_spr_n;
    if (atl_keep > 0) {
        atl_keep_buf = (KtxObjAtlas *)malloc(sizeof(KtxObjAtlas) * (size_t)atl_keep);
        if (!atl_keep_buf) {
            free(json);
            return 0;
        }
        memcpy(atl_keep_buf, s_atl, sizeof(KtxObjAtlas) * (size_t)atl_keep);
    }
    if (spr_keep > 0) {
        spr_keep_buf = (KtxObjSprite *)malloc(sizeof(KtxObjSprite) * (size_t)spr_keep);
        if (!spr_keep_buf) {
            free(atl_keep_buf);
            free(json);
            return 0;
        }
        memcpy(spr_keep_buf, s_spr, sizeof(KtxObjSprite) * (size_t)spr_keep);
    }

    s_atl_n = 0;
    s_spr_n = 0;
    parse_atlases(json);
    parse_sprites(json);
    free(json);
    n_atl = s_atl_n;
    n_spr = s_spr_n;
    if (n_atl < 1 || atl_keep + n_atl > OBJ_ATL_MAX || spr_keep + n_spr > OBJ_SPR_MAX) {
        if (atl_keep_buf)
            memcpy(s_atl, atl_keep_buf, sizeof(KtxObjAtlas) * (size_t)atl_keep);
        if (spr_keep_buf)
            memcpy(s_spr, spr_keep_buf, sizeof(KtxObjSprite) * (size_t)spr_keep);
        s_atl_n = atl_keep;
        s_spr_n = spr_keep;
        free(atl_keep_buf);
        free(spr_keep_buf);
        log_msg("ktx_obj: parse fail/overflow kind=%s n_atl=%d n_spr=%d", kind, n_atl, n_spr);
        /* #region agent log */
        {
            char data[160];
            snprintf(data, sizeof(data),
                     "{\"kind\":\"%.24s\",\"n_atl\":%d,\"n_spr\":%d,\"spr_keep\":%d,"
                     "\"cap\":%d}",
                     kind, n_atl, n_spr, spr_keep, OBJ_SPR_MAX);
            hooks_agent("H-V1", "ktx_obj.c:load_kind", "obj-kind-overflow", data);
        }
        /* #endregion */
        return 0;
    }
    if (atl_keep > 0) {
        memmove(s_atl + atl_keep, s_atl, sizeof(KtxObjAtlas) * (size_t)n_atl);
        memcpy(s_atl, atl_keep_buf, sizeof(KtxObjAtlas) * (size_t)atl_keep);
    }
    if (spr_keep > 0) {
        memmove(s_spr + spr_keep, s_spr, sizeof(KtxObjSprite) * (size_t)n_spr);
        memcpy(s_spr, spr_keep_buf, sizeof(KtxObjSprite) * (size_t)spr_keep);
    }
    free(atl_keep_buf);
    free(spr_keep_buf);
    s_atl_n = atl_keep + n_atl;
    s_spr_n = spr_keep + n_spr;
    atl0 = atl_keep;
    spr0 = spr_keep;
    for (i = atl0; i < s_atl_n; ++i)
        lstrcpynA(s_atl_kind[i], kind, (int)sizeof(s_atl_kind[i]));
    for (i = spr0; i < s_spr_n; ++i)
        s_spr[i].atlas_index += atl0;
    log_msg("ktx_obj: loaded kind=%s sprites=%d atlases=%d (%s)", kind, n_spr, n_atl, path);
    return 1;
}

void ktx_obj_init(void)
{
    int ok_b, ok_u, ok_m;
    s_ready = 0;
    s_spr_n = 0;
    s_atl_n = 0;
    s_root[0] = '\0';
    s_wanted = env_on("CK_GPU_OBJ", 1);
    /* #region agent log */
    {
        char data[96];
        snprintf(data, sizeof(data), "{\"CK_GPU_OBJ\":%d,\"def\":1}", s_wanted);
        hooks_agent("H-OBJ", "ktx_obj.c:init", "obj-gpu-env", data);
    }
    /* #endregion */
    if (!s_wanted) {
        log_msg("ktx_obj: disabled (CK_GPU_OBJ=0) — soft MapObj path");
        return;
    }
    if (!pick_root(s_root, sizeof(s_root))) {
        log_msg("ktx_obj: no ktx/objects (game_root=%s terrain=%s)", g_game_root,
                ktx_terrain_root() ? ktx_terrain_root() : "");
        return;
    }
    log_msg("ktx_obj: root=%s", s_root);
    ok_b = load_kind("buildings", "buildings.json");
    ok_u = load_kind("units", "units.json");
    ok_m = load_kind("mapobjects", "mapobjects.json");
    if (!ok_b && !ok_u && !ok_m) {
        log_msg("ktx_obj: no kinds loaded");
        return;
    }
    s_ready = 1;
    id_hash_rebuild();
    log_msg("ktx_obj: ready sprites=%d atlases=%d ids=%d", s_spr_n, s_atl_n, s_id_hash_n);
    /* #region agent log */
    {
        int i, mo_atl = 0;
        char data[192];
        for (i = 0; i < s_atl_n; ++i)
            if (_stricmp(s_atl_kind[i], "mapobjects") == 0)
                mo_atl++;
        snprintf(data, sizeof(data),
                 "{\"sprites\":%d,\"atlases\":%d,\"ids\":%d,\"mapobj_atl\":%d,"
                 "\"ok_b\":%d,\"ok_u\":%d,\"ok_m\":%d}",
                 s_spr_n, s_atl_n, s_id_hash_n, mo_atl, ok_b, ok_u, ok_m);
        hooks_agent("MO-CAT", "ktx_obj.c:init", "obj catalog ready", data);
    }
    /* #endregion */
}

void ktx_obj_shutdown(void)
{
    s_ready = 0;
    s_spr_n = 0;
    s_atl_n = 0;
}

int ktx_obj_wanted(void)
{
    return s_wanted;
}

int ktx_obj_ready(void)
{
    return s_ready;
}

const char *ktx_obj_root(void)
{
    return s_root;
}

int ktx_obj_sprite_count(void)
{
    return s_spr_n;
}

const KtxObjSprite *ktx_obj_sprites(void)
{
    return s_spr;
}

int ktx_obj_atlas_count(void)
{
    return s_atl_n;
}

const KtxObjAtlas *ktx_obj_atlases(void)
{
    return s_atl;
}

int ktx_obj_atlas_path(int atlas_index, char *out, size_t out_n)
{
    const char *kind;
    if (!s_ready || atlas_index < 0 || atlas_index >= s_atl_n || !out || out_n < 8)
        return 0;
    kind = s_atl_kind[atlas_index][0] ? s_atl_kind[atlas_index] : "buildings";
    snprintf(out, out_n, "%s\\%s\\%s", s_root, kind, s_atl[atlas_index].ktx2);
    slash_norm(out);
    if (file_exists(out))
        return 1;
    snprintf(out, out_n, "%s/%s/%s", s_root, kind, s_atl[atlas_index].ktx2);
    return file_exists(out);
}

int ktx_obj_atlas_idx_path(int atlas_index, char *out, size_t out_n)
{
    const char *kind;
    if (!s_ready || atlas_index < 0 || atlas_index >= s_atl_n || !out || out_n < 8)
        return 0;
    if (!s_atl[atlas_index].has_idx || !s_atl[atlas_index].idx_ktx2[0])
        return 0;
    kind = s_atl_kind[atlas_index][0] ? s_atl_kind[atlas_index] : "buildings";
    snprintf(out, out_n, "%s\\%s\\%s", s_root, kind, s_atl[atlas_index].idx_ktx2);
    slash_norm(out);
    if (file_exists(out))
        return 1;
    snprintf(out, out_n, "%s/%s/%s", s_root, kind, s_atl[atlas_index].idx_ktx2);
    return file_exists(out);
}

static unsigned id_hash_key(const char *id)
{
    unsigned h = 2166136261u;
    for (; id && *id; ++id) {
        char c = *id;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        h ^= (unsigned char)c;
        h *= 16777619u;
    }
    return h % ID_HASH;
}

static void id_hash_clear(void)
{
    memset(s_id_hash, 0, sizeof(s_id_hash));
    s_id_hash_n = 0;
}

static void id_hash_add(const char *id)
{
    unsigned i, start;
    if (!id || !id[0] || s_id_hash_n >= ID_HASH - 8)
        return;
    start = id_hash_key(id);
    for (i = 0; i < ID_HASH; ++i) {
        unsigned slot = (start + i) % ID_HASH;
        if (!s_id_hash[slot][0]) {
            lstrcpynA(s_id_hash[slot], id, (int)sizeof(s_id_hash[slot]));
            s_id_hash_n++;
            return;
        }
        if (_stricmp(s_id_hash[slot], id) == 0)
            return;
    }
}

static void id_hash_rebuild(void)
{
    int i;
    id_hash_clear();
    for (i = 0; i < s_spr_n; ++i)
        id_hash_add(s_spr[i].id);
    spr_chain_rebuild();
}

static void spr_chain_rebuild(void)
{
    int i;
    for (i = 0; i < ID_HASH; ++i) {
        s_spr_head[i] = -1;
        s_spr_hid[i][0] = '\0';
    }
    for (i = 0; i < s_spr_n; ++i)
        s_spr_next[i] = -1;
    /* Reverse insert so forward walk is ascending index order. */
    for (i = s_spr_n - 1; i >= 0; --i) {
        unsigned start = id_hash_key(s_spr[i].id);
        unsigned j;
        for (j = 0; j < ID_HASH; ++j) {
            unsigned slot = (start + j) % ID_HASH;
            if (!s_spr_hid[slot][0]) {
                lstrcpynA(s_spr_hid[slot], s_spr[i].id, (int)sizeof(s_spr_hid[slot]));
                s_spr_next[i] = s_spr_head[slot];
                s_spr_head[slot] = i;
                break;
            }
            if (_stricmp(s_spr_hid[slot], s_spr[i].id) == 0) {
                s_spr_next[i] = s_spr_head[slot];
                s_spr_head[slot] = i;
                break;
            }
        }
    }
}

int ktx_obj_spr_head(const char *id)
{
    unsigned i, start;
    if (!id || !id[0] || !s_ready)
        return -1;
    start = id_hash_key(id);
    for (i = 0; i < ID_HASH; ++i) {
        unsigned slot = (start + i) % ID_HASH;
        if (!s_spr_hid[slot][0])
            return -1;
        if (_stricmp(s_spr_hid[slot], id) == 0)
            return s_spr_head[slot];
    }
    return -1;
}

int ktx_obj_spr_next(int si)
{
    if (si < 0 || si >= s_spr_n)
        return -1;
    return s_spr_next[si];
}

/* Map common retail class names → catalog folder/ENT ids when they differ. */
#include "ktx_obj_aliases.inc"

static int id_hash_contains(const char *id)
{
    unsigned i, start;
    if (!id || !id[0])
        return 0;
    start = id_hash_key(id);
    for (i = 0; i < ID_HASH; ++i) {
        unsigned slot = (start + i) % ID_HASH;
        if (!s_id_hash[slot][0])
            return 0;
        if (_stricmp(s_id_hash[slot], id) == 0)
            return 1;
    }
    return 0;
}

static const char *id_alias_lookup(const char *id)
{
    int i;
    if (!id || !id[0])
        return NULL;
    for (i = 0; i < ID_ALIAS_N; ++i) {
        if (_stricmp(s_id_aliases[i].from, id) == 0)
            return s_id_aliases[i].to;
    }
    return NULL;
}

int ktx_obj_resolve_id(const char *id, char *out, size_t out_n)
{
    const char *alt;
    char tmp[64];
    char stripped[64];
    size_t n;
    int i;
    if (!out || out_n < 2)
        return 0;
    out[0] = '\0';
    if (!s_ready || !id || !id[0])
        return 0;
    lstrcpynA(tmp, id, (int)sizeof(tmp));
    /* Strip seasonal ENT suffixes: _asw / _as / _w and spaced forms. */
    n = strlen(tmp);
    if (n > 4 && _stricmp(tmp + (n - 4), "_asw") == 0)
        tmp[n - 4] = '\0';
    else if (n > 3 && (_stricmp(tmp + (n - 3), "_as") == 0 || _stricmp(tmp + (n - 3), " as") == 0))
        tmp[n - 3] = '\0';
    else if (n > 2 && (_stricmp(tmp + (n - 2), "_w") == 0 || _stricmp(tmp + (n - 2), " w") == 0))
        tmp[n - 2] = '\0';
    lstrcpynA(stripped, tmp, (int)sizeof(stripped));

    if (id_hash_contains(stripped)) {
        for (i = 0; i < s_spr_n; ++i) {
            if (_stricmp(s_spr[i].id, stripped) == 0) {
                lstrcpynA(out, s_spr[i].id, (int)out_n);
                return 1;
            }
        }
        lstrcpynA(out, stripped, (int)out_n);
        return 1;
    }
    alt = id_alias_lookup(stripped);
    if (!alt)
        alt = id_alias_lookup(id);
    if (alt && id_hash_contains(alt)) {
        for (i = 0; i < s_spr_n; ++i) {
            if (_stricmp(s_spr[i].id, alt) == 0) {
                lstrcpynA(out, s_spr[i].id, (int)out_n);
                return 1;
            }
        }
        lstrcpynA(out, alt, (int)out_n);
        return 1;
    }
    return 0;
}

/* 1 if PC body on BC3 without R8 material map (legacy / broken PC). */
int ktx_obj_id_needs_soft_pc(const char *id)
{
    char key[48];
    int si;
    if (!ktx_obj_resolve_id(id, key, sizeof(key)))
        return 0;
    for (si = ktx_obj_spr_head(key); si >= 0; si = ktx_obj_spr_next(si)) {
        const KtxObjSprite *sp = &s_spr[si];
        const KtxObjAtlas *a;
        if (ktx_obj_sprite_is_shadow(sp))
            continue;
        if (!ktx_obj_sprite_wants_pc(sp))
            continue;
        if (sp->atlas_index < 0 || sp->atlas_index >= s_atl_n)
            continue;
        a = &s_atl[sp->atlas_index];
        if (!a->is_rgba && !a->has_idx)
            return 1;
    }
    return 0;
}

int ktx_obj_id_fits_gpu(const char *id)
{
    char key[48];
    int si;
    if (!ktx_obj_resolve_id(id, key, sizeof(key)))
        return 0;
    for (si = ktx_obj_spr_head(key); si >= 0; si = ktx_obj_spr_next(si)) {
        const KtxObjSprite *sp = &s_spr[si];
        const KtxObjAtlas *a;
        if (ktx_obj_sprite_is_shadow(sp))
            continue;
        if (sp->atlas_index < 0 || sp->atlas_index >= s_atl_n)
            continue;
        a = &s_atl[sp->atlas_index];
        if (a->width < 1 || a->height < 1)
            continue;
        if (a->is_rgba) {
            if (a->width <= 2048u && a->height <= 2048u)
                return 1;
        } else if (a->width <= 2048u && a->height <= 3072u) {
            return 1;
        }
    }
    return 0;
}

int ktx_obj_has_id(const char *id)
{
    char canon[48];
    return ktx_obj_resolve_id(id, canon, sizeof(canon));
}

const KtxObjSprite *ktx_obj_lookup(const char *id, int frame)
{
    int i;
    const KtxObjSprite *best = NULL;
    char key[48];
    if (!ktx_obj_resolve_id(id, key, sizeof(key)))
        return NULL;
    for (i = 0; i < s_spr_n; ++i) {
        const KtxObjSprite *s = &s_spr[i];
        if (_stricmp(s->id, key) != 0)
            continue;
        if (ktx_obj_sprite_is_shadow(s))
            continue;
        if (s->frame == frame)
            return s;
        if (!best || s->frame < best->frame)
            best = s;
    }
    return best;
}

int ktx_obj_sprite_is_shadow(const KtxObjSprite *sp)
{
    if (!sp)
        return 0;
    /* Retail drawmode=shadow */
    if (sp->drawmode[0] && _stricmp(sp->drawmode, "shadow") == 0)
        return 1;
    /* Layer named shadow / walk_shadow */
    if (sp->layer[0] &&
        (_stricmp(sp->layer, "shadow") == 0 || _stricmp(sp->layer, "walk_shadow") == 0))
        return 1;
    /* IMGRLE mode 7 = 1-bit shadow mask (black + alpha). */
    if (sp->mode == 7)
        return 1;
    return 0;
}

int ktx_obj_sprite_wants_pc(const KtxObjSprite *sp)
{
    const char *dm;
    const char *rem;

    if (!sp || sp->imgrle_version != 2 || ktx_obj_sprite_is_shadow(sp))
        return 0;
    dm = sp->drawmode[0] ? sp->drawmode : "normal";
    rem = sp->remaping[0] ? sp->remaping : "none";
    if (_stricmp(dm, "index") == 0 || _stricmp(dm, "clouds") == 0)
        return 0;
    if (_stricmp(dm, "player_color") == 0)
        return 1;
    /* ENT normal + remaping=none + ver2 → retail mode=2 (GOutpost, R*, Gaul houses). */
    if (_stricmp(dm, "normal") == 0 && _stricmp(rem, "none") == 0)
        return 1;
    return 0;
}
