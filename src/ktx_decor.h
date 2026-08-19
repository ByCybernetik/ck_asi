#ifndef KTX_DECOR_H
#define KTX_DECOR_H

#include <stddef.h>
#include <stdint.h>

/* GPU decor atlases under {ktx}/decors/<season>/ (sibling of ktx/terrain).
 * CK_GPU_DECOR=1 by default (load on); set 0 to disable. */

typedef struct {
    char id[32];
    int type;
    int frame;
    int frames_x;
    int frames_y;
    int hot_x, hot_y;
    int offset_x, offset_y;
    int sort_ox, sort_oy;
    int z;
    int w, h;
    int atlas_x, atlas_y;
    int atlas_index;
    char layer[48];
    char drawmode[16];
    char remaping[16]; /* none | pingpong | reverse — from ENT */
    int mode; /* IMGRLE RLE2 mode (7 = shadow mask) */
    /* IMGRLE.version: retail is_pc / BuildRamp only when ==2 (I2 native decors.cpp).
     * Ver1 keeps palette[0..63] as texture even if ENT says player_color. */
    int imgrle_version;
} KtxDecorSprite;

typedef struct {
    char ktx2[64];
    char name[48];
    char group[48];
    uint32_t width;
    uint32_t height;
    int sprite_count;
    /* 1 = R8G8B8A8 (soft-alpha / legacy index-in-alpha); 0 = BC3. From JSON "bc". */
    int is_rgba;
    /* Companion R8 material map (BuildRamp index+1 in texel). Empty if none. */
    char idx_ktx2[64];
    int has_idx;
} KtxDecorAtlas;

void ktx_decor_init(void);
void ktx_decor_shutdown(void);
int ktx_decor_wanted(void);
int ktx_decor_ready(void);
const char *ktx_decor_root(void);
const char *ktx_decor_season(void);

/* Load (or reload) season atlas JSON + remember ktx2 path. 1 = ok. */
int ktx_decor_load_season(const char *season);

int ktx_decor_sprite_count(void);
const KtxDecorSprite *ktx_decor_sprites(void);
int ktx_decor_atlas_count(void);
const KtxDecorAtlas *ktx_decor_atlases(void);

/* Absolute path to atlas KTX2 by index. */
int ktx_decor_atlas_path(int atlas_index, char *out, size_t out_n);

/* First non-shadow sprite for type at frame (or closest). NULL if missing. */
const KtxDecorSprite *ktx_decor_lookup(int type, int frame);

/* MapObjects palms tag mode-7 masks as drawmode=player_color — detect real shadows. */
int ktx_decor_sprite_is_shadow(const KtxDecorSprite *sp);

#endif
