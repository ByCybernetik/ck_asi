#include "ktx_decor.h"
#include "hooks_internal.h"
#include "ktx_terrain.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DECOR_SPR_MAX = 4096, DECOR_ATL_MAX = 32 };

static char s_root[MAX_PATH];
static char s_season[32];
static int s_wanted;
static int s_ready;
static KtxDecorSprite s_spr[DECOR_SPR_MAX];
static int s_spr_n;
static KtxDecorAtlas s_atl[DECOR_ATL_MAX];
static int s_atl_n;

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

    /* Sibling of terrain root: .../ktx/terrain → .../ktx/decors */
    tr = ktx_terrain_root();
    if (tr && tr[0]) {
        char tmp[MAX_PATH];
        char *slash;
        lstrcpynA(tmp, tr, (int)sizeof(tmp));
        slash_norm(tmp);
        slash = strrchr(tmp, '\\');
        if (slash && _stricmp(slash + 1, "terrain") == 0) {
            *slash = '\0';
            snprintf(cand, sizeof(cand), "%s\\decors", tmp);
            if (dir_exists(cand)) {
                lstrcpynA(out, cand, (int)out_n);
                return 1;
            }
        }
    }
    if (g_game_root[0]) {
        snprintf(cand, sizeof(cand), "%sktx\\decors", g_game_root);
        if (dir_exists(cand)) {
            lstrcpynA(out, cand, (int)out_n);
            return 1;
        }
        snprintf(cand, sizeof(cand), "%s..\\ck_asi\\ktx\\decors", g_game_root);
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
    while (p < end && s_atl_n < DECOR_ATL_MAX) {
        const char *obj;
        const char *obj_end;
        const char *q;
        char tmp[64];
        int n;
        KtxDecorAtlas *a;

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
        if (a->ktx2[0]) {
            if (!a->name[0])
                lstrcpynA(a->name, a->ktx2, (int)sizeof(a->name));
            s_atl_n++;
        }        p = skip_ws(p);
        if (*p == ',')
            ++p;
    }
    return s_atl_n > 0;
}

static int parse_one_sprite(const char *obj, const char *obj_end, KtxDecorSprite *s)
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
    while (*p && s_spr_n < DECOR_SPR_MAX) {
        const char *obj;
        const char *close;
        KtxDecorSprite spr;

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

void ktx_decor_init(void)
{
    s_ready = 0;
    s_spr_n = 0;
    s_atl_n = 0;
    s_root[0] = '\0';
    s_season[0] = '\0';
    s_wanted = env_on("CK_GPU_DECOR", 1);
    if (!s_wanted) {
        log_msg("ktx_decor: disabled (CK_GPU_DECOR=0)");
        return;
    }
    if (!pick_root(s_root, sizeof(s_root))) {
        log_msg("ktx_decor: no ktx/decors (game_root=%s terrain=%s)", g_game_root,
                ktx_terrain_root() ? ktx_terrain_root() : "");
        return;
    }
    log_msg("ktx_decor: root=%s", s_root);
    /* Eager-load spring so catalog is verified at attach; season swap later. */
    ktx_decor_load_season("spring");
}

void ktx_decor_shutdown(void)
{
    s_ready = 0;
    s_spr_n = 0;
    s_atl_n = 0;
}

int ktx_decor_wanted(void)
{
    return s_wanted;
}

int ktx_decor_ready(void)
{
    return s_ready;
}

const char *ktx_decor_root(void)
{
    return s_root;
}

const char *ktx_decor_season(void)
{
    return s_season;
}

int ktx_decor_load_season(const char *season)
{
    char path[MAX_PATH];
    char *json;
    size_t len = 0;
    int ok;

    if (!s_wanted || !s_root[0] || !season || !season[0])
        return 0;
    if (s_ready && _stricmp(s_season, season) == 0)
        return 1;

    snprintf(path, sizeof(path), "%s\\%s\\decors.json", s_root, season);
    slash_norm(path);
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "%s/%s/decors.json", s_root, season);
        if (!file_exists(path)) {
            log_msg("ktx_decor: missing %s/%s/decors.json", s_root, season);
            return 0;
        }
    }
    json = load_file(path, &len);
    if (!json) {
        log_msg("ktx_decor: read fail %s", path);
        return 0;
    }
    s_ready = 0;
    s_spr_n = 0;
    s_atl_n = 0;
    parse_atlases(json);
    ok = parse_sprites(json);
    free(json);
    if (!ok || s_atl_n < 1) {
        log_msg("ktx_decor: parse fail season=%s sprites=%d atlases=%d", season, s_spr_n, s_atl_n);
        return 0;
    }
    lstrcpynA(s_season, season, (int)sizeof(s_season));
    s_ready = 1;
    log_msg("ktx_decor: loaded season=%s sprites=%d atlases=%d (%s)", s_season, s_spr_n, s_atl_n,
            path);
    return 1;
}

int ktx_decor_sprite_count(void)
{
    return s_spr_n;
}

const KtxDecorSprite *ktx_decor_sprites(void)
{
    return s_spr;
}

int ktx_decor_atlas_count(void)
{
    return s_atl_n;
}

const KtxDecorAtlas *ktx_decor_atlases(void)
{
    return s_atl;
}

int ktx_decor_atlas_path(int atlas_index, char *out, size_t out_n)
{
    if (!s_ready || atlas_index < 0 || atlas_index >= s_atl_n || !out || out_n < 8)
        return 0;
    snprintf(out, out_n, "%s\\%s\\%s", s_root, s_season, s_atl[atlas_index].ktx2);
    slash_norm(out);
    if (file_exists(out))
        return 1;
    snprintf(out, out_n, "%s/%s/%s", s_root, s_season, s_atl[atlas_index].ktx2);
    return file_exists(out);
}

const KtxDecorSprite *ktx_decor_lookup(int type, int frame)
{
    int i;
    const KtxDecorSprite *best = NULL;
    if (!s_ready)
        return NULL;
    for (i = 0; i < s_spr_n; ++i) {
        const KtxDecorSprite *s = &s_spr[i];
        if (s->type != type)
            continue;
        if (ktx_decor_sprite_is_shadow(s))
            continue;
        if (s->frame == frame)
            return s;
        if (!best || s->frame < best->frame)
            best = s;
    }
    return best;
}

int ktx_decor_sprite_is_shadow(const KtxDecorSprite *sp)
{
    if (!sp)
        return 0;
    /* Retail drawmode=shadow */
    if (sp->drawmode[0] && _stricmp(sp->drawmode, "shadow") == 0)
        return 1;
    /* Layer named shadow (MapObjects palms/cacti misuse player_color). */
    if (sp->layer[0] && _stricmp(sp->layer, "shadow") == 0)
        return 1;
    /* IMGRLE mode 7 = 1-bit shadow mask (black + alpha). */
    if (sp->mode == 7)
        return 1;
    return 0;
}
