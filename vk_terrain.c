#include "vk_terrain.h"
#include "ktx_terrain.h"
#include "ktx_decor.h"
#include "ktx_vq_replace.h"
#include "hooks_internal.h"
#include "hitch.h"
#include "log.h"

#include "shaders/terrain_vert_spv.h"
#include "shaders/terrain_frag_spv.h"
#include "shaders/water_blend_vert_spv.h"
#include "shaders/water_blend_frag_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

enum {
    CK_MAP_MGR = 0x008C7B30u,
    CK_SEASON = 0x008DD090u,
    TEX_N = 24,
    /* Sorted unique 1..4-type combinations for 21 terrain IDs:
     * C(21,1)+C(21,2)+C(21,3)+C(21,4)=7546. 96 exhausted after visiting
     * several maps and silently fell back to the base-only descriptor. */
    PAIR_N = 8192,
    /* A frame cannot contain more batches than visible terrain cells. Keep this
     * independent from the persistent descriptor cache to avoid a huge stack. */
    BATCH_N = 768,
    LAND_BIND_N = 4,
    MASK_BIND = 4,
    TEX_BIND_N = 5, /* 0..3 land, 4 = transitions atlas */
    /* ~700 cells × 4×4 subdiv × 6 verts; leave headroom. */
    VERT_MAX = 720 * 16 * 6,
    BATCH_HASH = 1024
};

typedef struct {
    float x, y, u, v, shade;
    float w0, w1, w2, w3;
    float mu, mv;
    uint32_t cz_pack;
    uint32_t type_pack;
    uint32_t meta; /* bit0=use_mask; bits8-9=nTypes; bits16-23=set_pack (2b×4) */
} TerrVert;

typedef struct {
    char name[48];
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet dset;
    int w, h;
    int ok;
} TerrTex;

#define LOAD(name)                                                                                     \
    do {                                                                                               \
        s.fn.name = (PFN_##name)s.gpa(s.device, #name);                                                \
        if (!s.fn.name)                                                                                \
            log_msg("vk_terrain: missing %s", #name);                                                  \
    } while (0)

/* Physical-device / instance entry points must use GetInstanceProcAddr. */
#define LOAD_I(name)                                                                                   \
    do {                                                                                               \
        s.fn.name = (PFN_##name)(s.gipa ? s.gipa(s.instance, #name) : NULL);                           \
        if (!s.fn.name)                                                                                \
            log_msg("vk_terrain: missing %s (instance)", #name);                                       \
    } while (0)

static struct {
    int wanted;
    int ready;
    VkDevice device;
    VkPhysicalDevice phys;
    VkInstance instance;
    VkQueue queue;
    uint32_t qfam;
    VkFormat soft_fmt;
    PFN_vkGetDeviceProcAddr gpa;
    PFN_vkGetInstanceProcAddr gipa;

    struct {
        PFN_vkCreateShaderModule vkCreateShaderModule;
        PFN_vkDestroyShaderModule vkDestroyShaderModule;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
        PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
        PFN_vkDestroyPipeline vkDestroyPipeline;
        PFN_vkCreateRenderPass vkCreateRenderPass;
        PFN_vkDestroyRenderPass vkDestroyRenderPass;
        PFN_vkCreateFramebuffer vkCreateFramebuffer;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
        PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
        PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
        PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
        PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
        PFN_vkResetDescriptorPool vkResetDescriptorPool;
        PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
        PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
        PFN_vkCreateSampler vkCreateSampler;
        PFN_vkDestroySampler vkDestroySampler;
        PFN_vkCreateBuffer vkCreateBuffer;
        PFN_vkDestroyBuffer vkDestroyBuffer;
        PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
        PFN_vkCreateImage vkCreateImage;
        PFN_vkDestroyImage vkDestroyImage;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
        PFN_vkAllocateMemory vkAllocateMemory;
        PFN_vkFreeMemory vkFreeMemory;
        PFN_vkBindBufferMemory vkBindBufferMemory;
        PFN_vkBindImageMemory vkBindImageMemory;
        PFN_vkMapMemory vkMapMemory;
        PFN_vkUnmapMemory vkUnmapMemory;
        PFN_vkCreateImageView vkCreateImageView;
        PFN_vkDestroyImageView vkDestroyImageView;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
        PFN_vkCmdBindPipeline vkCmdBindPipeline;
        PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
        PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
        PFN_vkCmdPushConstants vkCmdPushConstants;
        PFN_vkCmdDraw vkCmdDraw;
        PFN_vkCmdSetViewport vkCmdSetViewport;
        PFN_vkCmdSetScissor vkCmdSetScissor;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
        PFN_vkCreateCommandPool vkCreateCommandPool;
        PFN_vkDestroyCommandPool vkDestroyCommandPool;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
        PFN_vkEndCommandBuffer vkEndCommandBuffer;
        PFN_vkQueueSubmit vkQueueSubmit;
        PFN_vkQueueWaitIdle vkQueueWaitIdle;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
    } fn;

    VkShaderModule vert, frag;
    VkShaderModule water_vert, water_frag;
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout pipe_layout;
    VkRenderPass rp;
    VkPipeline pipe;
    VkPipeline water_pipe;
    VkSampler sampler;
    VkSampler mask_sampler;
    VkDescriptorPool pool;
    VkBuffer vbo;
    VkDeviceMemory vbo_mem;
    void *vbo_ptr;
    VkCommandPool upload_pool;
    VkCommandBuffer upload_cmd;

    TerrTex tex[TEX_N];
    int tex_n;
    TerrTex mask_atlas;
    int mask_ok;
    char season[16];
    volatile LONG draws;

    int overpaint;     /* CK_GPU_TERRAIN_OVERPAINT — all z AABB */
    int water_blend;   /* CK_WATER_GPU_BLEND — water dual-sample only */
    int proj;          /* foreshorten + height corner remap (default on) */
    int light;         /* light.grid shade via (L+4)/20 (default on) */
    int trans;         /* corner-z blend (Hermite GPU or soft BMP leave-through) */
    int soft_trans;    /* 1: mixed→soft when no KTX masks */
    int want_mask;     /* CK_GPU_TERRAIN_MASK — use transitions atlas when loaded */
    int subdiv;        /* split each 64 cell into N×N (default 4) */
    int draw_enabled;  /* runtime toggle (F8); starts = overpaint||water_blend */
    int f8_was_down;
    LARGE_INTEGER anim_qpc;
    LARGE_INTEGER qpc_freq;
    double anim_period_sec;
    int anim_have_tick;

    /* Multi-tex descriptor cache (bindings 0..3 land; 4 = mask atlas). */
    struct {
        TerrTex *t[LAND_BIND_N];
        VkDescriptorSet dset;
    } pair[PAIR_N];
    int pair_n;
} s;

/* Device stash for lazy/AUTO init (vk_terrain_init memsets `s`). */
static struct {
    VkDevice device;
    VkPhysicalDevice phys;
    VkQueue queue;
    uint32_t qfam;
    VkFormat soft_fmt;
    PFN_vkGetDeviceProcAddr gpa;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    int have;
} s_defer;

/* Survive vk_terrain_init memset — AUTO GPU-first vs F8-forced soft. */
static int g_terr_auto = -1;
static int g_terr_force_soft = 0;

static int terr_auto_on(void)
{
    if (g_terr_auto < 0)
        g_terr_auto = env_on("CK_GPU_TERRAIN_AUTO", 1);
    return g_terr_auto;
}

/* Push constants: land uses screen only; water also uses anim (32 bytes). */
typedef struct {
    float screen[2];
    float pad[2];
    float frac;
    float frame_i;
    float frames;
    float inv_frames;
} TerrPC;

int vk_terrain_wanted(void)
{
    /* Default ON for debug: Wine/gamescope often drop CK_* exports (log: present
     * without "GPU pipeline ready"). Opt out: CK_GPU_TERRAIN_OVERPAINT=0. */
    return env_on("CK_GPU_TERRAIN_OVERPAINT", 1) || env_on("CK_WATER_GPU_BLEND", 0);
}

void vk_terrain_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                            VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                            PFN_vkGetInstanceProcAddr gipa, VkInstance instance)
{
    s_defer.device = device;
    s_defer.phys = phys;
    s_defer.queue = queue;
    s_defer.qfam = qfam;
    s_defer.soft_fmt = soft_format;
    s_defer.gpa = gpa;
    s_defer.gipa = gipa;
    s_defer.instance = instance;
    s_defer.have = (device && phys && queue && gpa && gipa && instance) ? 1 : 0;
}

void vk_terrain_note_anim_tick(void)
{
    LARGE_INTEGER now;
    if (!s.water_blend && !s.wanted)
        return;
    if (!s.qpc_freq.QuadPart)
        QueryPerformanceFrequency(&s.qpc_freq);
    QueryPerformanceCounter(&now);
    if (s.anim_have_tick && s.qpc_freq.QuadPart > 0) {
        double dt = (double)(now.QuadPart - s.anim_qpc.QuadPart) / (double)s.qpc_freq.QuadPart;
        if (dt > 0.02 && dt < 1.5)
            s.anim_period_sec = s.anim_period_sec * 0.75 + dt * 0.25;
    }
    s.anim_qpc = now;
    s.anim_have_tick = 1;
}

static float water_blend_frac(void)
{
    LARGE_INTEGER now;
    double dt, p;
    if (!s.anim_have_tick || s.qpc_freq.QuadPart <= 0)
        return 0.f;
    QueryPerformanceCounter(&now);
    p = s.anim_period_sec > 0.03 ? s.anim_period_sec : 0.12;
    dt = (double)(now.QuadPart - s.anim_qpc.QuadPart) / (double)s.qpc_freq.QuadPart;
    if (dt < 0.0)
        return 0.f;
    if (dt >= p)
        return 1.f;
    return (float)(dt / p);
}

/* Layer table @ *[0x8DD088]+0xc8c, stride 0x5A: +0x0c frame_i, +0x10 frames, +0x14 frame_h. */
static int read_water_layer_frame(unsigned prefer_i, DWORD *frame_i, DWORD *frames, DWORD *frame_h)
{
    BYTE *obj, *table, *layer;
    DWORD n, i;
    if (frame_i)
        *frame_i = 0;
    if (frames)
        *frames = 10;
    if (frame_h)
        *frame_h = 512;
    obj = *(BYTE **)(ULONG_PTR)0x008DD088u;
    if (!obj || IsBadReadPtr(obj + 0xc88, 8))
        return 0;
    n = *(DWORD *)(obj + 0xc88);
    table = *(BYTE **)(obj + 0xc8c);
    if (!table || n == 0 || n > 64u || IsBadReadPtr(table, n * 0x5au))
        return 0;
    if (prefer_i < n) {
        layer = table + prefer_i * 0x5au;
        if (frames)
            *frames = *(DWORD *)(layer + 0x10);
        if (frame_h)
            *frame_h = *(DWORD *)(layer + 0x14);
        if (frame_i)
            *frame_i = *(DWORD *)(layer + 0x0c);
        if (*frames >= 2 && *frame_h >= 64)
            return 1;
    }
    for (i = 0; i < n; ++i) {
        DWORD f, h;
        layer = table + i * 0x5au;
        f = *(DWORD *)(layer + 0x10);
        h = *(DWORD *)(layer + 0x14);
        if (f >= 2 && f <= 64 && h >= 64 && h <= 2048) {
            if (frames)
                *frames = f;
            if (frame_h)
                *frame_h = h;
            if (frame_i)
                *frame_i = *(DWORD *)(layer + 0x0c);
            return 1;
        }
    }
    return 0;
}

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    if (!s.fn.vkGetPhysicalDeviceMemoryProperties)
        return 0;
    s.fn.vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return 0;
}

/* TERRAINS.XML z → KTX basename (must match retail VQ names). */
static const char *z_basename(unsigned z)
{
    switch (z & 0xFFu) {
    case 0:
        return "ground1024"; /* Ground 1 */
    case 1:
        return "ground256"; /* Ground 2 */
    case 2:
        return "ground"; /* Ground 3 — was wrongly ground1024 */
    case 3:
        return "grass1024";
    case 4:
        return "grass512";
    case 5:
        return "sands";
    case 6:
        return "rocks1024";
    case 7:
        return "roads";
    case 8:
        return "rocks256";
    case 9:
        return "sandswave";
    case 10:
        return "waves";
    case 11:
        return "rocks2";
    case 12:
        return "swater";
    case 13:
        return "dwater";
    case 14:
        return "rroads";
    case 15:
        return "invalid";
    case 16:
        return "sand_grass512";
    case 17:
        return "sands_mix512";
    case 18:
        return "swamp1024";
    case 19:
        return "sroads";
    case 20:
        return "iroads";
    default:
        return "ground";
    }
}

static int z_seasonal(unsigned z)
{
    z &= 0xFFu;
    /* Non-seasonal: waves, water, invalid (same as retail paths without %season%). */
    return !(z == 10 || z == 12 || z == 13 || z == 15);
}

/* TERRAINS.XML type=6: roads / rroads / sroads / iroads. */
static int is_road_z(unsigned z)
{
    z &= 0xFFu;
    return z == 7u || z == 14u || z == 19u || z == 20u;
}

static void *terrain_obj(void)
{
    void *mgr;
    if (IsBadReadPtr((void *)(ULONG_PTR)CK_MAP_MGR, 4))
        return NULL;
    mgr = *(void **)(ULONG_PTR)CK_MAP_MGR;
    if (!mgr || (ULONG_PTR)mgr < 0x10000u || IsBadReadPtr((BYTE *)mgr + 0xb8, 4))
        return NULL;
    return *(void **)((BYTE *)mgr + 0xb8);
}

static unsigned cell_z(void *terr, LONG px, LONG py)
{
    BYTE *base;
    DWORD stride, *grid, cy, cx, idx, dword, shift;
    if (!terr)
        return 0;
    base = (BYTE *)terr;
    if (IsBadReadPtr(base + 0x10a2, 4) || IsBadReadPtr(base + 0x1092, 4))
        return 0;
    stride = *(DWORD *)(base + 0x10a2);
    grid = *(DWORD **)(base + 0x1092);
    if (!grid || !stride || IsBadReadPtr(grid, 4))
        return 0;
    cy = ((DWORD)py) >> 6;
    cx = ((DWORD)px) >> 6;
    idx = stride * cy + (cx >> 2);
    if (IsBadReadPtr(grid + idx, 4))
        return 0;
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

/* Height grid cell 32: this+0x103a / +0x104a (same packing as z). */
static unsigned cell_h(void *terr, LONG px, LONG py)
{
    BYTE *base;
    DWORD stride, *grid, cy, cx, idx, dword, shift;
    if (!terr)
        return 0;
    base = (BYTE *)terr;
    if (IsBadReadPtr(base + 0x104a, 4) || IsBadReadPtr(base + 0x103a, 4))
        return 0;
    stride = *(DWORD *)(base + 0x104a);
    grid = *(DWORD **)(base + 0x103a);
    if (!grid || !stride || IsBadReadPtr(grid, 4))
        return 0;
    cy = ((DWORD)py) >> 5;
    cx = ((DWORD)px) >> 5;
    idx = stride * cy + (cx >> 2);
    if (IsBadReadPtr(grid + idx, 4))
        return 0;
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

/* Light grid cell 32: this+0x1066 / +0x1076. Values 0..21, mid 16 = identity. */
static unsigned cell_light(void *terr, LONG px, LONG py)
{
    BYTE *base;
    DWORD stride, *grid, cy, cx, idx, dword, shift;
    if (!terr)
        return 16;
    base = (BYTE *)terr;
    if (IsBadReadPtr(base + 0x1076, 4) || IsBadReadPtr(base + 0x1066, 4))
        return 16;
    stride = *(DWORD *)(base + 0x1076);
    grid = *(DWORD **)(base + 0x1066);
    if (!grid || !stride || IsBadReadPtr(grid, 4))
        return 16;
    cy = ((DWORD)py) >> 5;
    cx = ((DWORD)px) >> 5;
    idx = stride * cy + (cx >> 2);
    if (IsBadReadPtr(grid + idx, 4))
        return 16;
    dword = grid[idx];
    shift = (cx & 3u) * 8u;
    return (dword >> shift) & 0xFFu;
}

/* Frame-hot grid handles — skip IsBadReadPtr / pointer chase per cell (H-TERR).
 * Must clamp cx/cy: cam pads past map edge (crash log world y→18176 on 16384 maps). */
typedef struct {
    DWORD *z_grid;
    DWORD z_stride;
    DWORD z_nx; /* 64px cells in X */
    DWORD z_ny;
    DWORD *h_grid;
    DWORD h_stride;
    DWORD h_nx; /* 32px cells in X */
    DWORD h_ny;
    DWORD *l_grid;
    DWORD l_stride;
    DWORD l_nx;
    DWORD l_ny;
    int ok;
} TerrGrids;

static DWORD grid_rows_from_vq(void *ptr, DWORD stride_dwords)
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T avail;
    DWORD row_bytes;
    if (!ptr || !stride_dwords)
        return 0;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return 0;
    avail = (SIZE_T)((BYTE *)mbi.BaseAddress + mbi.RegionSize - (BYTE *)ptr);
    row_bytes = stride_dwords * 4u;
    if (row_bytes == 0 || avail < row_bytes)
        return 0;
    return (DWORD)(avail / row_bytes);
}

static int terr_grids_bind(void *terr, TerrGrids *g)
{
    BYTE *base;
    memset(g, 0, sizeof(*g));
    if (!terr || IsBadReadPtr(terr, 0x10b0))
        return 0;
    base = (BYTE *)terr;
    g->z_stride = *(DWORD *)(base + 0x10a2);
    g->z_grid = *(DWORD **)(base + 0x1092);
    g->h_stride = *(DWORD *)(base + 0x104a);
    g->h_grid = *(DWORD **)(base + 0x103a);
    g->l_stride = *(DWORD *)(base + 0x1076);
    g->l_grid = *(DWORD **)(base + 0x1066);
    if (!g->z_grid || !g->z_stride || !g->h_grid || !g->h_stride)
        return 0;
    if (IsBadReadPtr(g->z_grid, 16) || IsBadReadPtr(g->h_grid, 16))
        return 0;
    /* 4 cells packed per DWORD. */
    g->z_nx = g->z_stride * 4u;
    g->h_nx = g->h_stride * 4u;
    g->z_ny = grid_rows_from_vq(g->z_grid, g->z_stride);
    g->h_ny = grid_rows_from_vq(g->h_grid, g->h_stride);
    if (g->z_nx < 4 || g->z_ny < 4 || g->h_nx < 4 || g->h_ny < 4)
        return 0;
    if (g->l_grid && g->l_stride && !IsBadReadPtr(g->l_grid, 16)) {
        g->l_nx = g->l_stride * 4u;
        g->l_ny = grid_rows_from_vq(g->l_grid, g->l_stride);
        if (g->l_nx < 4 || g->l_ny < 4) {
            g->l_grid = NULL;
            g->l_stride = 0;
            g->l_nx = g->l_ny = 0;
        }
    } else {
        g->l_grid = NULL;
        g->l_stride = 0;
    }
    g->ok = 1;
    return 1;
}

static unsigned grid_byte(DWORD *grid, DWORD stride, DWORD cx, DWORD cy, DWORD nx, DWORD ny)
{
    DWORD idx, dword;
    if (!grid || !stride || nx == 0 || ny == 0)
        return 0;
    if (cx >= nx)
        cx = nx - 1;
    if (cy >= ny)
        cy = ny - 1;
    idx = stride * cy + (cx >> 2);
    dword = grid[idx];
    return (dword >> ((cx & 3u) * 8u)) & 0xFFu;
}

static unsigned cell_z_g(const TerrGrids *g, LONG px, LONG py)
{
    if (!g || !g->ok)
        return 0;
    return grid_byte(g->z_grid, g->z_stride, ((DWORD)px) >> 6, ((DWORD)py) >> 6, g->z_nx,
                     g->z_ny);
}

static unsigned cell_h_g(const TerrGrids *g, LONG px, LONG py)
{
    if (!g || !g->ok)
        return 0;
    return grid_byte(g->h_grid, g->h_stride, ((DWORD)px) >> 5, ((DWORD)py) >> 5, g->h_nx,
                     g->h_ny);
}

static unsigned cell_light_g(const TerrGrids *g, LONG px, LONG py)
{
    if (!g || !g->ok || !g->l_grid || !g->l_stride)
        return 16;
    return grid_byte(g->l_grid, g->l_stride, ((DWORD)px) >> 5, ((DWORD)py) >> 5, g->l_nx,
                     g->l_ny);
}

/* BuildLightTable R-curve: out = clamp(in * (L+4)/20, 0..31); L=16 → ×1. */
static float light_shade(unsigned L)
{
    if (L > 31u)
        L = 31u;
    return (float)(L + 4u) / 20.0f;
}

static int is_water_z(unsigned z)
{
    z &= 0xFFu;
    return z == 12u || z == 13u;
}

static unsigned batch_key_hash(TerrTex *a, TerrTex *b, TerrTex *c, TerrTex *d, int water)
{
    uintptr_t x = (uintptr_t)a ^ ((uintptr_t)b << 1) ^ ((uintptr_t)c << 2) ^ ((uintptr_t)d << 3) ^
                  ((uintptr_t)(unsigned)water * 0x9e3779b9u);
    x ^= x >> 17;
    return (unsigned)x & (BATCH_HASH - 1u);
}

static int batch_hash_find(const int *bidx, TerrTex *bt[][LAND_BIND_N], const int *batch_water,
                           int batch_n, TerrTex *tt[LAND_BIND_N], int water, unsigned *slot_out)
{
    unsigned slot = batch_key_hash(tt[0], tt[1], tt[2], tt[3], water);
    unsigned start = slot;
    (void)batch_n;
    for (;;) {
        int bi = bidx[slot];
        if (bi < 0) {
            if (slot_out)
                *slot_out = slot;
            return -1;
        }
        if (bt[bi][0] == tt[0] && bt[bi][1] == tt[1] && bt[bi][2] == tt[2] &&
            bt[bi][3] == tt[3] && batch_water[bi] == water) {
            if (slot_out)
                *slot_out = slot;
            return bi;
        }
        slot = (slot + 1u) & (BATCH_HASH - 1u);
        if (slot == start)
            return -1;
    }
}

/* Mapviewer / GPU: unique types among 4 cell corners (keep full z for TERRAINS.XML).
 * Order: ascending z — retail DrawSingleTile base = first present type (lowest z). */
static int count_corner_types(unsigned z00, unsigned z10, unsigned z11, unsigned z01,
                              unsigned types_out[4])
{
    unsigned corners[4];
    int n = 0, i, t;
    corners[0] = z00 & 0xFFu;
    corners[1] = z10 & 0xFFu;
    corners[2] = z11 & 0xFFu;
    corners[3] = z01 & 0xFFu;
    for (i = 0; i < 4; ++i) {
        unsigned z = corners[i];
        if (z == 15u)
            z = 0u;
        for (t = 0; t < n; ++t) {
            if (types_out[t] == z)
                break;
        }
        if (t == n && n < 4)
            types_out[n++] = z;
    }
    /* Sort ascending so types[0] is retail underpaint base. */
    for (i = 0; i < n; ++i) {
        for (t = i + 1; t < n; ++t) {
            if (types_out[t] < types_out[i]) {
                unsigned tmp = types_out[i];
                types_out[i] = types_out[t];
                types_out[t] = tmp;
            }
        }
    }
    return n;
}

static float hermite01(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

/* Foreshortened V in 64×46 mask: ly∈[0,63] → (ly·181/256)/46. */
static float mask_v_from_fy(float fy)
{
    int ly = (int)(fy * 64.0f);
    int my;
    if (ly < 0)
        ly = 0;
    if (ly > 63)
        ly = 63;
    my = (ly * 0xb5) >> 8;
    return ((float)my + 0.5f) / 46.0f;
}

/* terrains.xml transition @ layer+0x1c. Retail DrawSingleTile uses piVar1[7] RAW as
 * set index into A–D (0..3): (trans + cy_parity)*16 + (bits^15). Do NOT treat as
 * 1-based — logs showed raw=2 for all; 1-based map picked weak row B. */
static int layer_trans_set(unsigned z)
{
    BYTE *obj, *table;
    int tr;
    z &= 0xFFu;
    if (z > 31u)
        z = 31u;
    obj = *(BYTE **)(ULONG_PTR)0x008DD088u;
    if (!obj || IsBadReadPtr(obj + 0xc8c, 4))
        return 0;
    table = *(BYTE **)(obj + 0xc8c);
    if (!table || IsBadReadPtr(table + z * 0x5au + 0x1c, 4))
        return 0;
    tr = *(int *)(table + z * 0x5au + 0x1c);
    if (tr < 0)
        tr = 0;
    return tr & 3;
}

/* Hermite(fx,fy) then accumulate corner membership into out_w[0..nTypes). */
static void hermite_type_w(unsigned z00, unsigned z10, unsigned z11, unsigned z01,
                           const unsigned *types, int nTypes, float fx, float fy, float out_w[4])
{
    float hx = hermite01(fx);
    float hy = hermite01(fy);
    float wt[4];
    unsigned corners[4];
    int i, t;
    wt[0] = (1.0f - hx) * (1.0f - hy);
    wt[1] = hx * (1.0f - hy);
    wt[2] = hx * hy;
    wt[3] = (1.0f - hx) * hy;
    corners[0] = z00 & 0xFFu;
    corners[1] = z10 & 0xFFu;
    corners[2] = z11 & 0xFFu;
    corners[3] = z01 & 0xFFu;
    out_w[0] = out_w[1] = out_w[2] = out_w[3] = 0.0f;
    for (i = 0; i < 4; ++i) {
        unsigned z = corners[i];
        if (z == 15u)
            z = 0u;
        for (t = 0; t < nTypes; ++t) {
            if (types[t] == z) {
                out_w[t] += wt[i];
                break;
            }
        }
    }
}

/* Cam Y is MapToProj space; ThreadAnimate uses ×0x16a>>8 to recover world Y. */
static LONG cam_y_to_world(LONG cam_y)
{
    return (LONG)(((LONGLONG)cam_y * 0x16aLL) >> 8);
}

/* MapToView Y: retail (flat − h·256)>>8 with flat=(y·181)&~0xff ≡ (y·181>>8)−h. */
static int map_to_view_y(LONG wy, unsigned h)
{
    int flat = (int)(((DWORD)wy * 0xb5u) & 0xffffff00u);
    return (flat - (int)h * 0x100) >> 8;
}

/* Foreshorten only (DrawFlatTerrain) — MapToProj / camera Y space. */
static int map_to_proj_y(LONG wy)
{
    return (int)(((DWORD)wy * 0xb5u) >> 8);
}

static float bilerp4(float a00, float a10, float a01, float a11, float fx, float fy)
{
    float u = a00 * (1.0f - fx) + a10 * fx;
    float v = a01 * (1.0f - fx) + a11 * fx;
    return u * (1.0f - fy) + v * fy;
}

/* ExactHeight via 3×3 of 32-grid samples covering one 64×64 land cell. */
static void load_h3x3(const TerrGrids *g, LONG wx, LONG wy, float h[3][3])
{
    int i, j;
    for (j = 0; j < 3; ++j)
        for (i = 0; i < 3; ++i)
            h[j][i] = (float)cell_h_g(g, wx + i * 32, wy + j * 32);
}

static float exact_h_3x3(const float h[3][3], LONG wx, LONG wy, LONG px, LONG py)
{
    LONG ox = px - wx;
    LONG oy = py - wy;
    int cx, cy;
    float tx, ty;
    if (ox < 0)
        ox = 0;
    if (oy < 0)
        oy = 0;
    if (ox > 64)
        ox = 64;
    if (oy > 64)
        oy = 64;
    cx = (int)(ox >> 5);
    cy = (int)(oy >> 5);
    if (cx > 1)
        cx = 1;
    if (cy > 1)
        cy = 1;
    tx = (float)(ox - cx * 32) * (1.0f / 32.0f);
    ty = (float)(oy - cy * 32) * (1.0f / 32.0f);
    return bilerp4(h[cy][cx], h[cy][cx + 1], h[cy + 1][cx], h[cy + 1][cx + 1], tx, ty);
}

/* ExactHeight @0x475db0 — bilinear on 32×32 height grid (editor exact_height). */
static float exact_h(void *terr, LONG px, LONG py)
{
    LONG x0 = (LONG)((DWORD)px & ~31u);
    LONG y0 = (LONG)((DWORD)py & ~31u);
    float tx = (float)(px - x0) * (1.0f / 32.0f);
    float ty = (float)(py - y0) * (1.0f / 32.0f);
    float h00 = (float)cell_h(terr, x0, y0);
    float h10 = (float)cell_h(terr, x0 + 32, y0);
    float h01 = (float)cell_h(terr, x0, y0 + 32);
    float h11 = (float)cell_h(terr, x0 + 32, y0 + 32);
    return bilerp4(h00, h10, h01, h11, tx, ty);
}

static float map_to_view_yf(LONG wy, float h)
{
    return (float)map_to_proj_y(wy) - h;
}

static void emit_quad(TerrVert *out, float x0, float y0, float x1, float y1, float u0, float v0,
                      float u1, float v1, float s00, float s10, float s11, float s01,
                      const float *w00, const float *w10, const float *w11, const float *w01,
                      float mu0, float mv0, float mu1, float mv1, uint32_t cz, uint32_t types,
                      uint32_t meta)
{
    TerrVert q[6] = {
        {x0, y0, u0, v0, s00, w00[0], w00[1], w00[2], w00[3], mu0, mv0, cz, types, meta},
        {x1, y0, u1, v0, s10, w10[0], w10[1], w10[2], w10[3], mu1, mv0, cz, types, meta},
        {x1, y1, u1, v1, s11, w11[0], w11[1], w11[2], w11[3], mu1, mv1, cz, types, meta},
        {x0, y0, u0, v0, s00, w00[0], w00[1], w00[2], w00[3], mu0, mv0, cz, types, meta},
        {x1, y1, u1, v1, s11, w11[0], w11[1], w11[2], w11[3], mu1, mv1, cz, types, meta},
        {x0, y1, u0, v1, s01, w01[0], w01[1], w01[2], w01[3], mu0, mv1, cz, types, meta},
    };
    memcpy(out, q, sizeof(q));
}

/* Trapezoid: TL,TR,BR,BL in screen space (height-warped). */
static void emit_trap(TerrVert *out, float x0, float y0, float x1, float y1, float x2, float y2,
                      float x3, float y3, float u0, float v0, float u1, float v1, float s00,
                      float s10, float s11, float s01, const float *w00, const float *w10,
                      const float *w11, const float *w01, float mu0, float mv0, float mu1,
                      float mv1, uint32_t cz, uint32_t types, uint32_t meta)
{
    TerrVert q[6] = {
        {x0, y0, u0, v0, s00, w00[0], w00[1], w00[2], w00[3], mu0, mv0, cz, types, meta},
        {x1, y1, u1, v0, s10, w10[0], w10[1], w10[2], w10[3], mu1, mv0, cz, types, meta},
        {x2, y2, u1, v1, s11, w11[0], w11[1], w11[2], w11[3], mu1, mv1, cz, types, meta},
        {x0, y0, u0, v0, s00, w00[0], w00[1], w00[2], w00[3], mu0, mv0, cz, types, meta},
        {x2, y2, u1, v1, s11, w11[0], w11[1], w11[2], w11[3], mu1, mv1, cz, types, meta},
        {x3, y3, u0, v1, s01, w01[0], w01[1], w01[2], w01[3], mu0, mv1, cz, types, meta},
    };
    memcpy(out, q, sizeof(q));
}

static void flush_tex_cache(void)
{
    int i;
    if (!s.ready || !s.device)
        return;
    if (s.fn.vkDeviceWaitIdle)
        s.fn.vkDeviceWaitIdle(s.device);
    for (i = 0; i < s.tex_n; ++i) {
        if (s.tex[i].view && s.fn.vkDestroyImageView)
            s.fn.vkDestroyImageView(s.device, s.tex[i].view, NULL);
        if (s.tex[i].img && s.fn.vkDestroyImage)
            s.fn.vkDestroyImage(s.device, s.tex[i].img, NULL);
        if (s.tex[i].mem && s.fn.vkFreeMemory)
            s.fn.vkFreeMemory(s.device, s.tex[i].mem, NULL);
        memset(&s.tex[i], 0, sizeof(s.tex[i]));
    }
    s.tex_n = 0;
    s.pair_n = 0;
    memset(s.pair, 0, sizeof(s.pair));
    if (s.pool && s.fn.vkResetDescriptorPool)
        s.fn.vkResetDescriptorPool(s.device, s.pool, 0);
}

static void refresh_season(void)
{
    DWORD v = 0;
    char prev[16];
    const char *from_vq;

    lstrcpynA(prev, s.season, (int)sizeof(prev));
    if (!IsBadReadPtr((void *)(ULONG_PTR)CK_SEASON, 4))
        v = *(DWORD *)(ULONG_PTR)CK_SEASON;
    /* Prefer season folder from actual VQ loads (matches soft). Global @0x8DD090
     * can disagree (logs: spring VQ + autumn GPU). */
    from_vq = ktx_vq_detected_season();
    if (from_vq && from_vq[0]) {
        lstrcpynA(s.season, from_vq, (int)sizeof(s.season));
    } else if (!s.season[0]) {
        /* Global @0x8DD090 disagreed with VQ paths (spring loads, value==1→autumn).
         * Do not trust it alone — default spring until a VQ path is seen. */
        lstrcpyA(s.season, "spring");
        (void)v;
    }
    if (prev[0] && strcmp(prev, s.season) != 0) {
        flush_tex_cache();
        log_msg("vk_terrain: season %s → %s (global=%u vq=%s)", prev, s.season, (unsigned)v,
                from_vq ? from_vq : "");
        ktx_decor_load_season(s.season);
    }
}

static int upload_bc1(TerrTex *t, const char *rel)
{
    char path[MAX_PATH];
    Ktx2Info info;
    uint8_t *blocks = NULL;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkBufferCreateInfo bci;
    VkImageViewCreateInfo vci;
    VkDescriptorSetAllocateInfo dai;
    VkDescriptorImageInfo dii;
    VkBufferImageCopy bic;
    VkImageMemoryBarrier barr;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkResult r;
    snprintf(path, sizeof(path), "%s\\%s", ktx_terrain_root(), rel);
    {
        char *q;
        for (q = path; *q; ++q)
            if (*q == '/')
                *q = '\\';
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(path, sizeof(path), "%s/%s", ktx_terrain_root(), rel);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            return 0;
    }
    if (!ktx_terrain_load_info(path, &info, &blocks))
        return 0;
    if (info.vk_format < 131 || info.vk_format > 134 || !blocks) {
        free(blocks);
        return 0;
    }
    /*
     * Soft FB is B8G8R8A8_UNORM (swapchain fmt=44). KTX files are tagged
     * BC1_*_SRGB (132/134): sampling them auto-linearizes → dark overpaint.
     * Retail CPU decode treats blocks as display-referred UNORM — match that.
     */
    {
        uint32_t fmt_in = info.vk_format;
        uint32_t fmt_use = fmt_in;
        if (fmt_in == 132u)
            fmt_use = 131u; /* BC1_RGB_SRGB → UNORM */
        else if (fmt_in == 134u)
            fmt_use = 133u; /* BC1_RGBA_SRGB → UNORM */
        info.vk_format = fmt_use;
        if (s.tex_n < 3)
            log_msg("vk_terrain: upload %s fmt %u→%u %ux%u", rel, (unsigned)fmt_in,
                    (unsigned)fmt_use, info.width, info.height);
    }

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)info.vk_format;
    ici.extent.width = info.width;
    ici.extent.height = info.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &t->img);
    if (r != VK_SUCCESS) {
        free(blocks);
        return 0;
    }
    s.fn.vkGetImageMemoryRequirements(s.device, t->img, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &t->mem);
    if (r != VK_SUCCESS) {
        free(blocks);
        return 0;
    }
    s.fn.vkBindImageMemory(s.device, t->img, t->mem, 0);

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = info.level0_len;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    r = s.fn.vkCreateBuffer(s.device, &bci, NULL, &staging);
    if (r != VK_SUCCESS) {
        free(blocks);
        return 0;
    }
    s.fn.vkGetBufferMemoryRequirements(s.device, staging, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &staging_mem);
    if (r != VK_SUCCESS) {
        free(blocks);
        return 0;
    }
    s.fn.vkBindBufferMemory(s.device, staging, staging_mem, 0);
    s.fn.vkMapMemory(s.device, staging_mem, 0, info.level0_len, 0, &mapped);
    memcpy(mapped, blocks, info.level0_len);
    s.fn.vkUnmapMemory(s.device, staging_mem);
    free(blocks);

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s.fn.vkBeginCommandBuffer(s.upload_cmd, &bi);
    memset(&barr, 0, sizeof(barr));
    barr.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barr.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.image = t->img;
    barr.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barr.subresourceRange.levelCount = 1;
    barr.subresourceRange.layerCount = 1;
    barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = info.width;
    bic.imageExtent.height = info.height;
    bic.imageExtent.depth = 1;
    s.fn.vkCmdCopyBufferToImage(s.upload_cmd, staging, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                1, &bic);
    barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
    s.fn.vkEndCommandBuffer(s.upload_cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.upload_cmd;
    s.fn.vkQueueSubmit(s.queue, 1, &si, VK_NULL_HANDLE);
    s.fn.vkQueueWaitIdle(s.queue);

    s.fn.vkDestroyBuffer(s.device, staging, NULL);
    s.fn.vkFreeMemory(s.device, staging_mem, NULL);

    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t->img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = (VkFormat)info.vk_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &t->view);
    if (r != VK_SUCCESS)
        return 0;

    memset(&dai, 0, sizeof(dai));
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.dset_layout;
    r = s.fn.vkAllocateDescriptorSets(s.device, &dai, &t->dset);
    if (r != VK_SUCCESS)
        return 0;
    memset(&dii, 0, sizeof(dii));
    dii.sampler = s.sampler;
    dii.imageView = t->view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        VkWriteDescriptorSet wds[TEX_BIND_N];
        VkDescriptorImageInfo dii_mask;
        int bi;
        memset(wds, 0, sizeof(wds));
        for (bi = 0; bi < LAND_BIND_N; ++bi) {
            wds[bi].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[bi].dstSet = t->dset;
            wds[bi].dstBinding = (uint32_t)bi;
            wds[bi].descriptorCount = 1;
            wds[bi].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds[bi].pImageInfo = &dii;
        }
        dii_mask = dii;
        if (s.mask_ok && s.mask_atlas.ok) {
            dii_mask.sampler = s.mask_sampler ? s.mask_sampler : s.sampler;
            dii_mask.imageView = s.mask_atlas.view;
        }
        wds[MASK_BIND].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[MASK_BIND].dstSet = t->dset;
        wds[MASK_BIND].dstBinding = MASK_BIND;
        wds[MASK_BIND].descriptorCount = 1;
        wds[MASK_BIND].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds[MASK_BIND].pImageInfo = &dii_mask;
        s.fn.vkUpdateDescriptorSets(s.device, TEX_BIND_N, wds, 0, NULL);
    }

    lstrcpynA(t->name, rel, (int)sizeof(t->name));
    t->w = (int)info.width;
    t->h = (int)info.height;
    t->ok = 1;
    return 1;
}

static VkDescriptorSet ensure_quad_dset(TerrTex *t0, TerrTex *t1, TerrTex *t2, TerrTex *t3)
{
    TerrTex *tt[LAND_BIND_N];
    int i, k;
    VkDescriptorSetAllocateInfo dai;
    VkDescriptorImageInfo dii[TEX_BIND_N];
    VkWriteDescriptorSet wds[TEX_BIND_N];
    VkResult r;
    tt[0] = t0;
    tt[1] = t1 ? t1 : t0;
    tt[2] = t2 ? t2 : t0;
    tt[3] = t3 ? t3 : t0;
    if (!tt[0] || !tt[0]->ok)
        return VK_NULL_HANDLE;
    for (k = 1; k < LAND_BIND_N; ++k)
        if (!tt[k] || !tt[k]->ok)
            tt[k] = tt[0];
    if (tt[0] == tt[1] && tt[1] == tt[2] && tt[2] == tt[3] && (!s.mask_ok || !s.want_mask))
        return tt[0]->dset;
    for (i = 0; i < s.pair_n; ++i) {
        if (s.pair[i].t[0] == tt[0] && s.pair[i].t[1] == tt[1] && s.pair[i].t[2] == tt[2] &&
            s.pair[i].t[3] == tt[3])
            return s.pair[i].dset;
    }
    if (s.pair_n >= PAIR_N) {
        return tt[0]->dset;
    }
    memset(&dai, 0, sizeof(dai));
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.dset_layout;
    r = s.fn.vkAllocateDescriptorSets(s.device, &dai, &s.pair[s.pair_n].dset);
    if (r != VK_SUCCESS)
        return tt[0]->dset;
    memset(dii, 0, sizeof(dii));
    memset(wds, 0, sizeof(wds));
    for (k = 0; k < LAND_BIND_N; ++k) {
        dii[k].sampler = s.sampler;
        dii[k].imageView = tt[k]->view;
        dii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wds[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[k].dstSet = s.pair[s.pair_n].dset;
        wds[k].dstBinding = (uint32_t)k;
        wds[k].descriptorCount = 1;
        wds[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds[k].pImageInfo = &dii[k];
        s.pair[s.pair_n].t[k] = tt[k];
    }
    dii[MASK_BIND].sampler = (s.mask_ok && s.mask_sampler) ? s.mask_sampler : s.sampler;
    dii[MASK_BIND].imageView =
        (s.mask_ok && s.mask_atlas.ok) ? s.mask_atlas.view : tt[0]->view;
    dii[MASK_BIND].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wds[MASK_BIND].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[MASK_BIND].dstSet = s.pair[s.pair_n].dset;
    wds[MASK_BIND].dstBinding = MASK_BIND;
    wds[MASK_BIND].descriptorCount = 1;
    wds[MASK_BIND].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[MASK_BIND].pImageInfo = &dii[MASK_BIND];
    s.fn.vkUpdateDescriptorSets(s.device, TEX_BIND_N, wds, 0, NULL);
    return s.pair[s.pair_n++].dset;
}

/* Upload R8_UNORM KTX2 (transition atlas). Uses s.upload_cmd. */
static int upload_r8_atlas(TerrTex *t, const char *rel)
{
    char path[MAX_PATH];
    Ktx2Info info;
    uint8_t *pixels = NULL;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkBufferCreateInfo bci;
    VkImageViewCreateInfo vci;
    VkBufferImageCopy bic;
    VkImageMemoryBarrier barr;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkResult r;

    snprintf(path, sizeof(path), "%s\\%s", ktx_terrain_root(), rel);
    {
        char *q;
        for (q = path; *q; ++q)
            if (*q == '/')
                *q = '\\';
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(path, sizeof(path), "%s/%s", ktx_terrain_root(), rel);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            return 0;
    }
    if (!ktx_terrain_load_info(path, &info, &pixels) || !pixels)
        return 0;
    /* VK_FORMAT_R8_UNORM = 9 */
    if (info.vk_format != 9u) {
        free(pixels);
        return 0;
    }

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent.width = info.width;
    ici.extent.height = info.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &t->img);
    if (r != VK_SUCCESS) {
        free(pixels);
        return 0;
    }
    s.fn.vkGetImageMemoryRequirements(s.device, t->img, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &t->mem);
    if (r != VK_SUCCESS) {
        free(pixels);
        return 0;
    }
    s.fn.vkBindImageMemory(s.device, t->img, t->mem, 0);

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = info.level0_len;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    r = s.fn.vkCreateBuffer(s.device, &bci, NULL, &staging);
    if (r != VK_SUCCESS) {
        free(pixels);
        return 0;
    }
    s.fn.vkGetBufferMemoryRequirements(s.device, staging, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &staging_mem);
    if (r != VK_SUCCESS) {
        free(pixels);
        return 0;
    }
    s.fn.vkBindBufferMemory(s.device, staging, staging_mem, 0);
    s.fn.vkMapMemory(s.device, staging_mem, 0, info.level0_len, 0, &mapped);
    memcpy(mapped, pixels, info.level0_len);
    s.fn.vkUnmapMemory(s.device, staging_mem);
    free(pixels);

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s.fn.vkBeginCommandBuffer(s.upload_cmd, &bi);
    memset(&barr, 0, sizeof(barr));
    barr.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barr.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.image = t->img;
    barr.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barr.subresourceRange.levelCount = 1;
    barr.subresourceRange.layerCount = 1;
    barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = info.width;
    bic.imageExtent.height = info.height;
    bic.imageExtent.depth = 1;
    s.fn.vkCmdCopyBufferToImage(s.upload_cmd, staging, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                1, &bic);
    barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
    s.fn.vkEndCommandBuffer(s.upload_cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.upload_cmd;
    s.fn.vkQueueSubmit(s.queue, 1, &si, VK_NULL_HANDLE);
    s.fn.vkQueueWaitIdle(s.queue);
    s.fn.vkDestroyBuffer(s.device, staging, NULL);
    s.fn.vkFreeMemory(s.device, staging_mem, NULL);

    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t->img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &t->view);
    if (r != VK_SUCCESS)
        return 0;
    lstrcpynA(t->name, rel, (int)sizeof(t->name));
    t->w = (int)info.width;
    t->h = (int)info.height;
    t->ok = 1;
    t->dset = VK_NULL_HANDLE;
    return 1;
}

static TerrTex *get_tex(const char *base, int seasonal)
{
    char rel[64];
    int i;
    if (seasonal)
        snprintf(rel, sizeof(rel), "%s/%s.ktx2", s.season, base);
    else
        snprintf(rel, sizeof(rel), "%s.ktx2", base);
    for (i = 0; i < s.tex_n; ++i)
        if (strcmp(s.tex[i].name, rel) == 0)
            return s.tex[i].ok ? &s.tex[i] : NULL;
    if (s.tex_n >= TEX_N)
        return NULL;
    memset(&s.tex[s.tex_n], 0, sizeof(s.tex[0]));
    if (!upload_bc1(&s.tex[s.tex_n], rel)) {
        s.tex_n++; /* skip slot */
        return NULL;
    }
    return &s.tex[s.tex_n++];
}

int vk_terrain_init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                    VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                    PFN_vkGetInstanceProcAddr gipa, VkInstance instance)
{
    VkShaderModuleCreateInfo smci;
    VkDescriptorSetLayoutBinding bind[TEX_BIND_N];
    VkDescriptorSetLayoutCreateInfo dlci;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo plci;
    VkAttachmentDescription att;
    VkAttachmentReference att_ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo rpci;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vib;
    VkVertexInputAttributeDescription via[8];
    VkPipelineVertexInputStateCreateInfo viss;
    VkPipelineInputAssemblyStateCreateInfo iass;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rast;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cbs;
    VkPipelineDynamicStateCreateInfo dyn;
    VkDynamicState dyn_states[2];
    VkGraphicsPipelineCreateInfo gpci;
    VkSamplerCreateInfo sci;
    VkDescriptorPoolSize dps;
    VkDescriptorPoolCreateInfo dpci;
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cai;
    VkResult r;

    memset(&s, 0, sizeof(s));
    s.overpaint = env_on("CK_GPU_TERRAIN_OVERPAINT", 1);
    s.water_blend = env_on("CK_WATER_GPU_BLEND", 0);
    /* Projection/height remap — default ON with overpaint. CK_GPU_TERRAIN_PROJ=0 → old AABB. */
    s.proj = env_on("CK_GPU_TERRAIN_PROJ", 1);
    /* Light LUT shade — default ON. CK_GPU_TERRAIN_LIGHT=0 → flat ×1. */
    s.light = env_on("CK_GPU_TERRAIN_LIGHT", 1);
    /* Corner transitions — always GPU (no soft FB holes):
     *  MASK=1 (default): mixed cells → transitions atlas (majority underpaint + stamp).
     *  MASK=0: Hermite for all mixed. */
    s.trans = env_on("CK_GPU_TERRAIN_TRANS", 1);
    s.soft_trans = env_on("CK_GPU_TERRAIN_SOFT_TRANS", 0); /* legacy; never leave to soft */
    s.want_mask = env_on("CK_GPU_TERRAIN_MASK", 1);
    /* Subdivide each 64 cell into N×N. Default 1 (H-TERR: mesh CPU dominates).
     * CK_GPU_TERRAIN_SUB=2|4 for smoother height warp. */
    {
        char buf[16];
        DWORD n = GetEnvironmentVariableA("CK_GPU_TERRAIN_SUB", buf, (DWORD)sizeof(buf));
        int sub = 1;
        if (n > 0 && n < sizeof(buf))
            sub = atoi(buf);
        if (sub < 1)
            sub = 1;
        if (sub > 8)
            sub = 8;
        /* Only powers-of-two divide 64 evenly for integer steps. */
        if (sub != 1 && sub != 2 && sub != 4 && sub != 8)
            sub = 1;
        s.subdiv = sub;
    }
    s.wanted = s.overpaint || s.water_blend;
    s.anim_period_sec = 0.12;
    QueryPerformanceFrequency(&s.qpc_freq);
    {
        log_msg("vk_terrain: flags overpaint=%d water_blend=%d proj=%d light=%d trans=%d "
                "soft_trans=%d want_mask=%d subdiv=%d",
                s.overpaint, s.water_blend, s.proj, s.light, s.trans, s.soft_trans, s.want_mask,
                s.subdiv);
    }
    s.draw_enabled = terr_auto_on() ? 1 : 0; /* AUTO: GPU on after init; else wait for F8 */
    s.f8_was_down = 0;
    if (!s.wanted)
        return 0;
    if (!device || !phys || !queue || !gpa || !gipa || !instance || !ktx_terrain_ready()) {
        log_msg("vk_terrain: init skipped (device/ktx)");
        return 0;
    }
    s.device = device;
    s.phys = phys;
    s.instance = instance;
    s.queue = queue;
    s.qfam = qfam;
    s.soft_fmt = soft_format;
    s.gpa = gpa;
    s.gipa = gipa;
    lstrcpyA(s.season, "spring");

    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreateGraphicsPipelines);
    LOAD(vkDestroyPipeline);
    LOAD(vkCreateRenderPass);
    LOAD(vkDestroyRenderPass);
    LOAD(vkCreateFramebuffer);
    LOAD(vkDestroyFramebuffer);
    LOAD(vkCreateDescriptorSetLayout);
    LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreateDescriptorPool);
    LOAD(vkDestroyDescriptorPool);
    LOAD(vkResetDescriptorPool);
    LOAD(vkAllocateDescriptorSets);
    LOAD(vkUpdateDescriptorSets);
    LOAD(vkCreateSampler);
    LOAD(vkDestroySampler);
    LOAD(vkCreateBuffer);
    LOAD(vkDestroyBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkCreateImage);
    LOAD(vkDestroyImage);
    LOAD(vkGetImageMemoryRequirements);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
    LOAD(vkBindBufferMemory);
    LOAD(vkBindImageMemory);
    LOAD(vkMapMemory);
    LOAD(vkUnmapMemory);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCmdBeginRenderPass);
    LOAD(vkCmdEndRenderPass);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBindVertexBuffers);
    LOAD(vkCmdBindDescriptorSets);
    LOAD(vkCmdPushConstants);
    LOAD(vkCmdDraw);
    LOAD(vkCmdSetViewport);
    LOAD(vkCmdSetScissor);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCmdCopyBufferToImage);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkQueueSubmit);
    LOAD(vkQueueWaitIdle);
    LOAD(vkDeviceWaitIdle);
    /* Device GPA returns NULL for this on Wine; must use instance GPA (H-A). */
    LOAD_I(vkGetPhysicalDeviceMemoryProperties);
    if (!s.fn.vkCreateGraphicsPipelines || !s.fn.vkCmdDraw ||
        !s.fn.vkGetPhysicalDeviceMemoryProperties) {
        log_msg("vk_terrain: critical procs missing (pipeline/draw/memprops)");
        return 0;
    }

    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = terrain_vert_spv_len;
    smci.pCode = (const uint32_t *)terrain_vert_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.vert);
    if (r != VK_SUCCESS)
        return 0;
    smci.codeSize = terrain_frag_spv_len;
    smci.pCode = (const uint32_t *)terrain_frag_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.frag);
    if (r != VK_SUCCESS)
        return 0;

    memset(bind, 0, sizeof(bind));
    bind[0].binding = 0;
    bind[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind[0].descriptorCount = 1;
    bind[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bind[1] = bind[0];
    bind[1].binding = 1;
    bind[2] = bind[0];
    bind[2].binding = 2;
    bind[3] = bind[0];
    bind[3].binding = 3;
    bind[4] = bind[0];
    bind[4].binding = 4;
    memset(&dlci, 0, sizeof(dlci));
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = TEX_BIND_N;
    dlci.pBindings = bind;
    r = s.fn.vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &s.dset_layout);
    if (r != VK_SUCCESS)
        return 0;

    memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(TerrPC);
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s.dset_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    r = s.fn.vkCreatePipelineLayout(s.device, &plci, NULL, &s.pipe_layout);
    if (r != VK_SUCCESS)
        return 0;

    memset(&att, 0, sizeof(att));
    att.format = soft_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&att_ref, 0, sizeof(att_ref));
    att_ref.attachment = 0;
    att_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &att_ref;
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    memset(&rpci, 0, sizeof(rpci));
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    r = s.fn.vkCreateRenderPass(s.device, &rpci, NULL, &s.rp);
    if (r != VK_SUCCESS)
        return 0;

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s.frag;
    stages[1].pName = "main";

    memset(&vib, 0, sizeof(vib));
    vib.binding = 0;
    vib.stride = sizeof(TerrVert);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(via, 0, sizeof(via));
    via[0].location = 0;
    via[0].binding = 0;
    via[0].format = VK_FORMAT_R32G32_SFLOAT;
    via[0].offset = 0;
    via[1].location = 1;
    via[1].binding = 0;
    via[1].format = VK_FORMAT_R32G32_SFLOAT;
    via[1].offset = sizeof(float) * 2;
    via[2].location = 2;
    via[2].binding = 0;
    via[2].format = VK_FORMAT_R32_SFLOAT;
    via[2].offset = sizeof(float) * 4;
    via[3].location = 3;
    via[3].binding = 0;
    via[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    via[3].offset = sizeof(float) * 5;
    via[4].location = 4;
    via[4].binding = 0;
    via[4].format = VK_FORMAT_R32G32_SFLOAT;
    via[4].offset = sizeof(float) * 9;
    via[5].location = 5;
    via[5].binding = 0;
    via[5].format = VK_FORMAT_R32_UINT;
    via[5].offset = sizeof(float) * 11;
    via[6].location = 6;
    via[6].binding = 0;
    via[6].format = VK_FORMAT_R32_UINT;
    via[6].offset = sizeof(float) * 11 + sizeof(uint32_t);
    via[7].location = 7;
    via[7].binding = 0;
    via[7].format = VK_FORMAT_R32_UINT;
    via[7].offset = sizeof(float) * 11 + sizeof(uint32_t) * 2;
    memset(&viss, 0, sizeof(viss));
    viss.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viss.vertexBindingDescriptionCount = 1;
    viss.pVertexBindingDescriptions = &vib;
    viss.vertexAttributeDescriptionCount = 8;
    viss.pVertexAttributeDescriptions = via;

    memset(&iass, 0, sizeof(iass));
    iass.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iass.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    memset(&rast, 0, sizeof(rast));
    rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;
    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_FALSE;
    memset(&cbs, 0, sizeof(cbs));
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    dyn_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dyn_states[1] = VK_DYNAMIC_STATE_SCISSOR;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    memset(&gpci, 0, sizeof(gpci));
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &viss;
    gpci.pInputAssemblyState = &iass;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rast;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyn;
    gpci.layout = s.pipe_layout;
    gpci.renderPass = s.rp;
    r = s.fn.vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gpci, NULL, &s.pipe);
    if (r != VK_SUCCESS) {
        log_msg("vk_terrain: pipeline create failed %d", (int)r);
        return 0;
    }

    /* Water dual-frame blend pipeline (optional). */
    if (s.water_blend) {
        smci.codeSize = water_blend_vert_spv_len;
        smci.pCode = (const uint32_t *)water_blend_vert_spv;
        r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.water_vert);
        if (r != VK_SUCCESS)
            return 0;
        smci.codeSize = water_blend_frag_spv_len;
        smci.pCode = (const uint32_t *)water_blend_frag_spv;
        r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.water_frag);
        if (r != VK_SUCCESS)
            return 0;
        stages[0].module = s.water_vert;
        stages[1].module = s.water_frag;
        gpci.pStages = stages;
        r = s.fn.vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gpci, NULL, &s.water_pipe);
        if (r != VK_SUCCESS) {
            log_msg("vk_terrain: water blend pipeline failed %d", (int)r);
            return 0;
        }
        log_msg("vk_terrain: water GPU blend pipeline ready");
    }

    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    /* LINEAR + subdiv softens 64-cell "cubes"; NEAREST made seams harsher. */
    sci.magFilter = VK_FILTER_NEAREST; /* match retail DrawFlat point sample */
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    r = s.fn.vkCreateSampler(s.device, &sci, NULL, &s.sampler);
    if (r != VK_SUCCESS)
        return 0;
    /* NEAREST + CLAMP for R8 transition atlas (0x80/0xC0 sentinels). */
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    r = s.fn.vkCreateSampler(s.device, &sci, NULL, &s.mask_sampler);
    if (r != VK_SUCCESS)
        return 0;

    memset(&dps, 0, sizeof(dps));
    dps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dps.descriptorCount = (TEX_N + PAIR_N) * TEX_BIND_N;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = TEX_N + PAIR_N;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    r = s.fn.vkCreateDescriptorPool(s.device, &dpci, NULL, &s.pool);
    if (r != VK_SUCCESS)
        return 0;

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(TerrVert) * VERT_MAX;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    r = s.fn.vkCreateBuffer(s.device, &bci, NULL, &s.vbo);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkGetBufferMemoryRequirements(s.device, s.vbo, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &s.vbo_mem);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkBindBufferMemory(s.device, s.vbo, s.vbo_mem, 0);
    s.fn.vkMapMemory(s.device, s.vbo_mem, 0, bci.size, 0, &s.vbo_ptr);

    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = s.qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    r = s.fn.vkCreateCommandPool(s.device, &pci, NULL, &s.upload_pool);
    if (r != VK_SUCCESS)
        return 0;
    memset(&cai, 0, sizeof(cai));
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = s.upload_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = s.fn.vkAllocateCommandBuffers(s.device, &cai, &s.upload_cmd);
    if (r != VK_SUCCESS)
        return 0;

    s.mask_ok = 0;
    if (s.want_mask) {
        memset(&s.mask_atlas, 0, sizeof(s.mask_atlas));
        if (upload_r8_atlas(&s.mask_atlas, "transitions_atlas.ktx2")) {
            s.mask_ok = 1;
            log_msg("vk_terrain: transition mask atlas %dx%d", s.mask_atlas.w, s.mask_atlas.h);
        } else {
            log_msg("vk_terrain: transition atlas missing — soft_trans leave for mixed");
        }
    }

    s.ready = 1;
    log_msg("vk_terrain: GPU pipeline ready fmt=%u overpaint=%d water_blend=%d",
            (unsigned)soft_format, s.overpaint, s.water_blend);
    return 1;
}

void vk_terrain_shutdown(void)
{
    int i;
    if (!s.device)
        return;
    if (s.fn.vkDeviceWaitIdle)
        s.fn.vkDeviceWaitIdle(s.device);
    for (i = 0; i < s.tex_n; ++i) {
        if (s.tex[i].view && s.fn.vkDestroyImageView)
            s.fn.vkDestroyImageView(s.device, s.tex[i].view, NULL);
        if (s.tex[i].img && s.fn.vkDestroyImage)
            s.fn.vkDestroyImage(s.device, s.tex[i].img, NULL);
        if (s.tex[i].mem && s.fn.vkFreeMemory)
            s.fn.vkFreeMemory(s.device, s.tex[i].mem, NULL);
    }
    if (s.vbo_ptr && s.fn.vkUnmapMemory)
        s.fn.vkUnmapMemory(s.device, s.vbo_mem);
    if (s.vbo && s.fn.vkDestroyBuffer)
        s.fn.vkDestroyBuffer(s.device, s.vbo, NULL);
    if (s.vbo_mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.vbo_mem, NULL);
    if (s.upload_pool && s.fn.vkDestroyCommandPool)
        s.fn.vkDestroyCommandPool(s.device, s.upload_pool, NULL);
    if (s.pool && s.fn.vkDestroyDescriptorPool)
        s.fn.vkDestroyDescriptorPool(s.device, s.pool, NULL);
    if (s.sampler && s.fn.vkDestroySampler)
        s.fn.vkDestroySampler(s.device, s.sampler, NULL);
    if (s.mask_sampler && s.fn.vkDestroySampler)
        s.fn.vkDestroySampler(s.device, s.mask_sampler, NULL);
    if (s.mask_atlas.view && s.fn.vkDestroyImageView)
        s.fn.vkDestroyImageView(s.device, s.mask_atlas.view, NULL);
    if (s.mask_atlas.img && s.fn.vkDestroyImage)
        s.fn.vkDestroyImage(s.device, s.mask_atlas.img, NULL);
    if (s.mask_atlas.mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.mask_atlas.mem, NULL);
    if (s.water_pipe && s.fn.vkDestroyPipeline)
        s.fn.vkDestroyPipeline(s.device, s.water_pipe, NULL);
    if (s.pipe && s.fn.vkDestroyPipeline)
        s.fn.vkDestroyPipeline(s.device, s.pipe, NULL);
    if (s.rp && s.fn.vkDestroyRenderPass)
        s.fn.vkDestroyRenderPass(s.device, s.rp, NULL);
    if (s.pipe_layout && s.fn.vkDestroyPipelineLayout)
        s.fn.vkDestroyPipelineLayout(s.device, s.pipe_layout, NULL);
    if (s.dset_layout && s.fn.vkDestroyDescriptorSetLayout)
        s.fn.vkDestroyDescriptorSetLayout(s.device, s.dset_layout, NULL);
    if (s.water_vert && s.fn.vkDestroyShaderModule)
        s.fn.vkDestroyShaderModule(s.device, s.water_vert, NULL);
    if (s.water_frag && s.fn.vkDestroyShaderModule)
        s.fn.vkDestroyShaderModule(s.device, s.water_frag, NULL);
    if (s.vert && s.fn.vkDestroyShaderModule)
        s.fn.vkDestroyShaderModule(s.device, s.vert, NULL);
    if (s.frag && s.fn.vkDestroyShaderModule)
        s.fn.vkDestroyShaderModule(s.device, s.frag, NULL);
    memset(&s, 0, sizeof(s));
}

int vk_terrain_ready(void)
{
    return s.ready;
}

int vk_terrain_draw_enabled(void)
{
    return s.ready && s.draw_enabled;
}

/* Soft land skip: GPU-first (AUTO default) or F8-on. F8→SOFT restores retail land. */
int vk_terrain_soft_land_disabled(void)
{
    if (env_on("CK_GPU_KEEP_SOFT_LAND", 0))
        return 0;
    if (!vk_terrain_wanted())
        return 0;
    if (g_terr_force_soft)
        return 0;
    if (vk_terrain_draw_enabled())
        return 1;
    /* AUTO: skip soft even before pipeline ready (avoids soft cost during first load). */
    return terr_auto_on() ? 1 : 0;
}

int vk_terrain_has_map(void)
{
    return terrain_obj() ? 1 : 0;
}

static int vk_terrain_try_load(const char *why)
{
    if (s.ready)
        return 1;
    if (!s_defer.have) {
        log_msg("vk_terrain: %s — no device stash", why ? why : "load");
        return 0;
    }
    if (!vk_terrain_wanted())
        return 0;
    if (!vk_terrain_init(s_defer.device, s_defer.phys, s_defer.queue, s_defer.qfam, s_defer.soft_fmt,
                         s_defer.gpa, s_defer.gipa, s_defer.instance)) {
        log_msg("vk_terrain: %s init FAILED", why ? why : "load");
        return 0;
    }
    s.draw_enabled = 1;
    g_terr_force_soft = 0;
    log_msg("vk_terrain: %s → GPU terrain ON", why ? why : "load");
    return 1;
}

void vk_terrain_poll_toggle(void)
{
    SHORT st;
    int down;
    DWORD now;
    static DWORD s_last_toggle_ms;
    static DWORD s_ignore_until; /* refractory after release — Wine re-asserts F8 */
    static int s_need_keyup = 0; /* after a toggle, ignore until physical release */
    enum { F8_DEBOUNCE_MS = 350u, F8_POST_UP_MS = 300u };

    /* AUTO: load GPU on first present — no F8 required. */
    if (terr_auto_on() && vk_terrain_wanted() && !s.ready && s_defer.have)
        vk_terrain_try_load("AUTO");

    st = GetAsyncKeyState(VK_F8);
    down = (st & 0x8000) ? 1 : 0;
    now = GetTickCount();

    if (!down) {
        if (s.f8_was_down || s_need_keyup) {
            /* Block rising-edge ghosts that Wine reports right after release. */
            s_ignore_until = now + F8_POST_UP_MS;
        }
        s.f8_was_down = 0;
        s_need_keyup = 0;
        return;
    }

    /* Held through after a toggle — wait for keyup. */
    if (s_need_keyup) {
        s.f8_was_down = 1;
        return;
    }

    /* Post-release refractory: ignore down flicker / Wine auto-repeat ghosts. */
    if (s_ignore_until != 0 && now < s_ignore_until) {
        s.f8_was_down = 1; /* don't treat exit from cooldown as rising edge */
        return;
    }

    if (!(down && !s.f8_was_down)) {
        s.f8_was_down = down;
        return;
    }

    /* Rising edge — debounce rapid double-fires (bounce / Wine). */
    if (s_last_toggle_ms != 0 && (now - s_last_toggle_ms) < F8_DEBOUNCE_MS) {
        s.f8_was_down = 1;
        s_need_keyup = 1;
        return;
    }

    s_last_toggle_ms = now;
    s.f8_was_down = 1;
    s_need_keyup = 1;

    if (!s.ready) {
        if (!s_defer.have) {
            log_msg("vk_terrain: F8 ignored (no Vulkan device yet)");
            return;
        }
        if (!vk_terrain_wanted()) {
            log_msg("vk_terrain: F8 ignored (OVERPAINT/BLEND off)");
            return;
        }
        vk_terrain_try_load("F8");
        return;
    }

    s.draw_enabled = s.draw_enabled ? 0 : 1;
    g_terr_force_soft = s.draw_enabled ? 0 : 1;
    log_msg("vk_terrain: F8 → %s (overpaint AABB %s)", s.draw_enabled ? "GPU" : "SOFT",
            s.draw_enabled ? "ON" : "OFF");
}

VkImageView vk_terrain_create_soft_view(VkImage soft_img, VkFormat soft_format)
{
    VkImageViewCreateInfo vci;
    VkImageView view = VK_NULL_HANDLE;
    if (!s.ready || !soft_img)
        return VK_NULL_HANDLE;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = soft_img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = soft_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    s.fn.vkCreateImageView(s.device, &vci, NULL, &view);
    return view;
}

void vk_terrain_destroy_soft_view(VkImageView view)
{
    if (s.ready && view && s.fn.vkDestroyImageView)
        s.fn.vkDestroyImageView(s.device, view, NULL);
}

static int vk_terrain_record_grids(VkCommandBuffer cmd, VkImageView soft_view, int soft_w,
                                   int soft_h, const TerrGrids *grids);

int vk_terrain_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view, int soft_w,
                      int soft_h)
{
    void *terr;
    TerrGrids grids;

    (void)soft_img;
    if (!s.ready || !cmd || !soft_view || soft_w < 64 || soft_h < 64)
        return -1;
    terr = terrain_obj();
    if (!terr || !terr_grids_bind(terr, &grids))
        return -1;
    return vk_terrain_record_grids(cmd, soft_view, soft_w, soft_h, &grids);
}

static int vk_terrain_record_grids(VkCommandBuffer cmd, VkImageView soft_view, int soft_w,
                                   int soft_h, const TerrGrids *grids)
{
    LONG L, T, R, B;
    LONG wx0, wy0, wx1, wy1, wx, wy;
    TerrVert *verts;
    TerrTex *bt[BATCH_N][LAND_BIND_N];
    VkDescriptorSet bdset[BATCH_N];
    int batch_count[BATCH_N];
    int batch_water[BATCH_N];
    int batch_n = 0;
    int nvert = 0;
    int tiles = 0;
    int trans_tiles = 0;
    int play_t = 80; /* InfoBar — soft playfield origin (zoom blit [0,80,w,946]) */
    int play_b;
    int cam_h;
    int soft_cam = 0;
    int frame_subdiv;
    int bi;
    TerrPC pc;
    VkViewport vp;
    VkRect2D sc;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fci;
    VkRenderPassBeginInfo rpbi;
    VkDeviceSize off = 0;
    LONG n;
    DWORD fi_sw = 0, fr_sw = 10, fh_sw = 512;
    DWORD fi_dw = 0, fr_dw = 10, fh_dw = 512;
    float frac;
    /* #region agent log */
    LONGLONG t_all, t_p1, t_p2, t_draw;
    double ms_p1 = 0, ms_p2 = 0, ms_draw = 0, ms_all = 0;
    /* #endregion */

    mm_read_cam(&L, &T, &R, &B);
    if (R <= L || B <= T)
        return -1;
    /* Soft-space camera (editor boot / zoom): L≈0 T≈80 R≈soft_w.
     * Editor: GPU overpaint disabled via vk_terrain_draw_enabled(); cheap subdiv if drawn. */
    soft_cam = (L <= 64 && T <= 128 && R >= (LONG)soft_w - 64 && (R - L) >= (LONG)soft_w / 2);
    frame_subdiv = soft_cam ? 1 : s.subdiv;
    refresh_season();
    cam_h = (int)(B - T);
    if (cam_h < 64)
        cam_h = soft_h - play_t - 54;
    play_b = play_t + cam_h;
    if (play_b > soft_h - 8)
        play_b = soft_h - 8;
    if (play_b <= play_t + 32)
        play_b = soft_h - 8;

    wx0 = (L & ~63) - 64;
    wx1 = ((R + 63) & ~63) + 128;
    if (s.proj) {
        /* Cam T/B are MapToProj Y; convert to world for z-grid walk (×0x16a>>8). */
        wy0 = (cam_y_to_world(T) & ~63) - 64;
        wy1 = (cam_y_to_world(B) & ~63) + 192;
    } else {
        wy0 = (T & ~63) - 64;
        wy1 = ((B + 63) & ~63) + 128;
    }

    verts = (TerrVert *)s.vbo_ptr;
    memset(bt, 0, sizeof(bt));
    memset(bdset, 0, sizeof(bdset));
    memset(batch_count, 0, sizeof(batch_count));
    memset(batch_water, 0, sizeof(batch_water));
    frac = s.water_blend ? water_blend_frac() : 0.f;
    if (s.water_blend) {
        read_water_layer_frame(12, &fi_sw, &fr_sw, &fh_sw);
        read_water_layer_frame(13, &fi_dw, &fr_dw, &fh_dw);
    }

    /* Pass 1: discover up-to-4-tex batches (mapviewer multi-type). */
    /* #region agent log */
    t_all = hitch_qpc_now();
    t_p1 = t_all;
    /* #endregion */
    {
        int bidx[BATCH_HASH];
        memset(bidx, 0xff, sizeof(bidx));
        for (wy = wy0; wy < wy1; wy += 64) {
            for (wx = wx0; wx < wx1; wx += 64) {
                unsigned z00 = cell_z_g(grids, wx, wy) & 0xFFu;
                unsigned z10 = cell_z_g(grids, wx + 64, wy) & 0xFFu;
                unsigned z11 = cell_z_g(grids, wx + 64, wy + 64) & 0xFFu;
                unsigned z01 = cell_z_g(grids, wx, wy + 64) & 0xFFu;
                unsigned types[4];
                int nTypes;
                int is_w = is_water_z(z00) && is_water_z(z10) && is_water_z(z11) && is_water_z(z01);
                TerrTex *tt[LAND_BIND_N];
                VkDescriptorSet dset;
                int water;
                int b, k;
                unsigned slot;

                if (!s.overpaint && !(is_water_z(z00) || is_water_z(z10) || is_water_z(z11) ||
                                      is_water_z(z01)))
                    continue;

                water = is_w && s.water_blend && s.water_pipe;
                if (water || !s.trans) {
                    types[0] = z00 & 0xFFu;
                    if (types[0] == 15u)
                        types[0] = 0u;
                    nTypes = 1;
                } else {
                    nTypes = count_corner_types(z00, z10, z11, z01, types);
                    if (nTypes < 1) {
                        types[0] = 0;
                        nTypes = 1;
                    }
                }

                memset(tt, 0, sizeof(tt));
                for (k = 0; k < nTypes; ++k) {
                    tt[k] = get_tex(z_basename(types[k]), z_seasonal(types[k]));
                    if (!tt[k]) {
                        if (k == 0)
                            break;
                        tt[k] = tt[0];
                    }
                }
                if (!tt[0])
                    continue;
                for (k = nTypes; k < LAND_BIND_N; ++k)
                    tt[k] = tt[0];

                dset = ensure_quad_dset(tt[0], tt[1], tt[2], tt[3]);
                if (!dset)
                    continue;
                b = batch_hash_find(bidx, bt, batch_water, batch_n, tt, water, &slot);
                if (b < 0) {
                    if (batch_n >= BATCH_N)
                        continue;
                    for (k = 0; k < LAND_BIND_N; ++k)
                        bt[batch_n][k] = tt[k];
                    bdset[batch_n] = dset;
                    batch_water[batch_n] = water;
                    bidx[slot] = batch_n;
                    batch_n++;
                }
            }
        }
    }
    if (batch_n <= 0) {
        return 0;
    }

    /* #region agent log */
    ms_p1 = hitch_qpc_ms_since(t_p1);
    t_p2 = hitch_qpc_now();
    /* #endregion */

    /* Pass 2: two cell-walks (count + emit) — O(cells), not O(batches×cells).
     * Log evidence: batches>20 → ms_p2 ~13–40ms with per-batch full scans. */
    {
        int bquad[BATCH_N];
        int bbase[BATCH_N];
        int bfill[BATCH_N];
        int bidx[BATCH_HASH];
        int total_verts;
        int pass;

        memset(bquad, 0, sizeof(bquad));
        memset(batch_count, 0, sizeof(batch_count));
        memset(bidx, 0xff, sizeof(bidx));
        for (bi = 0; bi < batch_n; ++bi) {
            unsigned slot;
            TerrTex *tt[LAND_BIND_N];
            int k;
            for (k = 0; k < LAND_BIND_N; ++k)
                tt[k] = bt[bi][k];
            if (batch_hash_find(bidx, bt, batch_water, batch_n, tt, batch_water[bi], &slot) < 0)
                bidx[slot] = bi;
        }

        for (pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                total_verts = 0;
                for (bi = 0; bi < batch_n; ++bi) {
                    int q = bquad[bi];
                    int room = (VERT_MAX - total_verts) / 6;
                    if (q > room)
                        q = room > 0 ? room : 0;
                    bquad[bi] = q;
                    bbase[bi] = total_verts;
                    bfill[bi] = 0;
                    total_verts += q * 6;
                }
                nvert = total_verts;
                tiles = 0;
                trans_tiles = 0;
            }
            for (wy = wy0; wy < wy1; wy += 64) {
                for (wx = wx0; wx < wx1; wx += 64) {
                    unsigned z00 = cell_z_g(grids, wx, wy) & 0xFFu;
                    unsigned z10 = cell_z_g(grids, wx + 64, wy) & 0xFFu;
                    unsigned z11 = cell_z_g(grids, wx + 64, wy + 64) & 0xFFu;
                    unsigned z01 = cell_z_g(grids, wx, wy + 64) & 0xFFu;
                    unsigned types[4];
                    int nTypes;
                    int is_w = is_water_z(z00) && is_water_z(z10) && is_water_z(z11) &&
                               is_water_z(z01);
                    int mixed;
                    TerrTex *tt[LAND_BIND_N];
                    TerrTex *tex0;
                    float sx0, sx1, u0, v0, u1, v1;
                    float y0, y3;
                    unsigned L00, L10, L01, L11;
                    float sh00, sh10, sh01, sh11;
                    float ymin, ymax;
                    int uw, uh, k, water, frame_h, b;
                    int dest;

                    if (!s.overpaint && !(is_water_z(z00) || is_water_z(z10) || is_water_z(z11) ||
                                          is_water_z(z01)))
                        continue;

                    water = is_w && s.water_blend && s.water_pipe;

                    if (water || !s.trans) {
                        types[0] = z00 & 0xFFu;
                        if (types[0] == 15u)
                            types[0] = 0u;
                        nTypes = 1;
                        mixed = 0;
                    } else {
                        nTypes = count_corner_types(z00, z10, z11, z01, types);
                        if (nTypes < 1) {
                            types[0] = 0;
                            nTypes = 1;
                        }
                        mixed = nTypes > 1;
                    }

                    memset(tt, 0, sizeof(tt));
                    for (k = 0; k < nTypes; ++k) {
                        tt[k] = get_tex(z_basename(types[k]), z_seasonal(types[k]));
                        if (!tt[k]) {
                            if (k == 0)
                                break;
                            tt[k] = tt[0];
                        }
                    }
                    if (!tt[0])
                        continue;
                    for (k = nTypes; k < LAND_BIND_N; ++k)
                        tt[k] = tt[0];

                    b = batch_hash_find(bidx, bt, batch_water, batch_n, tt, water, NULL);
                    if (b < 0)
                        continue;

                    tex0 = bt[b][0];
                    frame_h = water ? (int)(tex0->h >= tex0->w * 2 ? tex0->w : (int)fh_sw) : tex0->h;
                    if (frame_h < 1)
                        frame_h = tex0->w > 0 ? tex0->w : 512;

                    sx0 = (float)(wx - L);
                    sx1 = sx0 + 64.0f;
                    if (sx1 < 0.f || sx0 >= (float)soft_w)
                        continue;
                    if (water) {
                        uw = tex0->w > 0 ? tex0->w : 512;
                        uh = frame_h;
                        u0 = (float)((wx % uw + uw) % uw) / (float)uw;
                        v0 = (float)((wy % uh + uh) % uh) / (float)uh;
                        u1 = u0 + 64.0f / (float)uw;
                        v1 = v0 + 64.0f / (float)uh;
                    } else {
                        u0 = (float)wx;
                        v0 = (float)wy;
                        u1 = (float)(wx + 64);
                        v1 = (float)(wy + 64);
                    }

                    if (s.light) {
                        L00 = cell_light_g(grids, wx, wy);
                        L10 = cell_light_g(grids, wx + 64, wy);
                        L01 = cell_light_g(grids, wx, wy + 64);
                        L11 = cell_light_g(grids, wx + 64, wy + 64);
                        sh00 = light_shade(L00);
                        sh10 = light_shade(L10);
                        sh01 = light_shade(L01);
                        sh11 = light_shade(L11);
                    } else {
                        sh00 = sh10 = sh01 = sh11 = 1.0f;
                        L00 = L10 = L01 = L11 = 16;
                    }
                    {
                        int sub = water ? 1 : frame_subdiv;
                        int step = 64 / sub;
                        int need = sub * sub * 6;
                        int ix, iy;
                        int use_mask;
                        float hpatch[3][3];
                        use_mask = !water && mixed && s.want_mask && s.mask_ok && nTypes >= 2;
                        uint32_t cz_pack = (z00 & 255u) | ((z10 & 255u) << 8) |
                                           ((z11 & 255u) << 16) | ((z01 & 255u) << 24);
                        uint32_t type_pack = 0;
                        uint32_t set_pack = 0;
                        uint32_t meta;
                        int cy_parity = ((int)(wy >> 6)) & 1;

                        for (k = 0; k < LAND_BIND_N; ++k) {
                            unsigned tz = (k < nTypes) ? types[k] : types[0];
                            int set = (layer_trans_set(tz) + cy_parity) & 3;
                            type_pack |= (tz & 255u) << (k * 8);
                            set_pack |= ((uint32_t)set & 3u) << (k * 2);
                        }
                        meta = (use_mask ? 1u : 0u) | (((uint32_t)nTypes & 7u) << 8) |
                               (set_pack << 16);

                        if (s.proj) {
                            float ha, hb, hc, hd;
                            load_h3x3(grids, wx, wy, hpatch);
                            ha = exact_h_3x3(hpatch, wx, wy, wx, wy);
                            hb = exact_h_3x3(hpatch, wx, wy, wx + 64, wy);
                            hc = exact_h_3x3(hpatch, wx, wy, wx, wy + 64);
                            hd = exact_h_3x3(hpatch, wx, wy, wx + 64, wy + 64);
                            y0 = map_to_view_yf(wy, ha) - (float)T + (float)play_t;
                            y3 = map_to_view_yf(wy + 64, hc) - (float)T + (float)play_t;
                            {
                                float y1 = map_to_view_yf(wy, hb) - (float)T + (float)play_t;
                                float y2 = map_to_view_yf(wy + 64, hd) - (float)T + (float)play_t;
                                ymin = y0;
                                if (y1 < ymin)
                                    ymin = y1;
                                if (y2 < ymin)
                                    ymin = y2;
                                if (y3 < ymin)
                                    ymin = y3;
                                ymax = y0;
                                if (y1 > ymax)
                                    ymax = y1;
                                if (y2 > ymax)
                                    ymax = y2;
                                if (y3 > ymax)
                                    ymax = y3;
                            }
                            if (ymax <= (float)play_t || ymin >= (float)play_b)
                                continue;
                        } else {
                            float sy0 = (float)(wy - T) + (float)play_t;
                            float sy1 = sy0 + 64.0f;
                            if (sy1 <= (float)play_t || sy0 >= (float)play_b)
                                continue;
                        }

                        if (pass == 0) {
                            bquad[b] += sub * sub;
                            continue;
                        }

                        dest = bbase[b] + bfill[b];
                        if (dest + need > VERT_MAX)
                            continue;

                        for (iy = 0; iy < sub; ++iy) {
                            for (ix = 0; ix < sub; ++ix) {
                                float fx0 = (float)ix / (float)sub;
                                float fx1 = (float)(ix + 1) / (float)sub;
                                float fy0 = (float)iy / (float)sub;
                                float fy1 = (float)(iy + 1) / (float)sub;
                                LONG wx0s = wx + ix * step;
                                LONG wy0s = wy + iy * step;
                                LONG wx1s = wx0s + step;
                                LONG wy1s = wy0s + step;
                                float xa = sx0 + fx0 * 64.0f;
                                float xb = sx0 + fx1 * 64.0f;
                                float sa, sb, sc, sd;
                                float wa[4], wb[4], wc[4], wd[4];
                                float ua, ub, va, vc;

                                sa = bilerp4(sh00, sh10, sh01, sh11, fx0, fy0);
                                sb = bilerp4(sh00, sh10, sh01, sh11, fx1, fy0);
                                sc = bilerp4(sh00, sh10, sh01, sh11, fx0, fy1);
                                sd = bilerp4(sh00, sh10, sh01, sh11, fx1, fy1);
                                if (water || !mixed || use_mask) {
                                    wa[0] = wb[0] = wc[0] = wd[0] = 1.0f;
                                    wa[1] = wb[1] = wc[1] = wd[1] = 0.0f;
                                    wa[2] = wb[2] = wc[2] = wd[2] = 0.0f;
                                    wa[3] = wb[3] = wc[3] = wd[3] = 0.0f;
                                } else {
                                    hermite_type_w(z00, z10, z11, z01, types, nTypes, fx0, fy0, wa);
                                    hermite_type_w(z00, z10, z11, z01, types, nTypes, fx1, fy0, wb);
                                    hermite_type_w(z00, z10, z11, z01, types, nTypes, fx0, fy1, wc);
                                    hermite_type_w(z00, z10, z11, z01, types, nTypes, fx1, fy1, wd);
                                }
                                {
                                    float mu0 = fx0;
                                    float mu1 = fx1;
                                    float mv0 = mask_v_from_fy(fy0);
                                    float mv1 = mask_v_from_fy(fy1);

                                    if (water) {
                                        ua = u0 + (u1 - u0) * fx0;
                                        ub = u0 + (u1 - u0) * fx1;
                                        va = v0 + (v1 - v0) * fy0;
                                        vc = v0 + (v1 - v0) * fy1;
                                    } else {
                                        ua = (float)wx0s;
                                        ub = (float)wx1s;
                                        va = (float)wy0s;
                                        vc = (float)wy1s;
                                    }

                                    if (s.proj) {
                                        float ha, hb, hc, hd;
                                        ha = exact_h_3x3(hpatch, wx, wy, wx0s, wy0s);
                                        hb = exact_h_3x3(hpatch, wx, wy, wx1s, wy0s);
                                        hc = exact_h_3x3(hpatch, wx, wy, wx0s, wy1s);
                                        hd = exact_h_3x3(hpatch, wx, wy, wx1s, wy1s);
                                        float ya = map_to_view_yf(wy0s, ha) - (float)T + (float)play_t;
                                        float yb = map_to_view_yf(wy0s, hb) - (float)T + (float)play_t;
                                        float yc = map_to_view_yf(wy1s, hc) - (float)T + (float)play_t;
                                        float yd = map_to_view_yf(wy1s, hd) - (float)T + (float)play_t;
                                        emit_trap(verts + dest, xa, ya, xb, yb, xb, yd, xa, yc, ua,
                                                  va, ub, vc, sa, sb, sd, sc, wa, wb, wd, wc, mu0,
                                                  mv0, mu1, mv1, cz_pack, type_pack, meta);
                                    } else {
                                        float sy0 = (float)(wy0s - T) + (float)play_t;
                                        float sy1 = (float)(wy1s - T) + (float)play_t;
                                        if (sy0 < (float)play_t)
                                            sy0 = (float)play_t;
                                        if (sy1 > (float)play_b)
                                            sy1 = (float)play_b;
                                        emit_quad(verts + dest, xa, sy0, xb, sy1, ua, va, ub, vc, sa,
                                                  sb, sd, sc, wa, wb, wd, wc, mu0, mv0, mu1, mv1,
                                                  cz_pack, type_pack, meta);
                                    }
                                }
                                dest += 6;
                            }
                        }
                        bfill[b] = dest - bbase[b];
                        tiles++;
                        if (mixed)
                            trans_tiles++;
                    }
                }
            }
        }
        for (bi = 0; bi < batch_n; ++bi)
            batch_count[bi] = bfill[bi] / 6;
    }

    if (tiles <= 0)
        return 0;

    /* #region agent log */
    ms_p2 = hitch_qpc_ms_since(t_p2);
    t_draw = hitch_qpc_now();
    /* #endregion */

    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = s.rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &soft_view;
    fci.width = (uint32_t)soft_w;
    fci.height = (uint32_t)soft_h;
    fci.layers = 1;
    if (s.fn.vkCreateFramebuffer(s.device, &fci, NULL, &fb) != VK_SUCCESS)
        return -1;

    memset(&rpbi, 0, sizeof(rpbi));
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = s.rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea.extent.width = (uint32_t)soft_w;
    rpbi.renderArea.extent.height = (uint32_t)soft_h;
    s.fn.vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    memset(&vp, 0, sizeof(vp));
    vp.width = (float)soft_w;
    vp.height = (float)soft_h;
    vp.maxDepth = 1.0f;
    memset(&sc, 0, sizeof(sc));
    sc.offset.y = play_t > 0 ? (int32_t)play_t : 0;
    sc.extent.width = (uint32_t)soft_w;
    sc.extent.height = (uint32_t)(play_b > play_t ? (play_b - play_t) : soft_h);
    s.fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    s.fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    s.fn.vkCmdBindVertexBuffers(cmd, 0, 1, &s.vbo, &off);

    {
        int base = 0;
        for (bi = 0; bi < batch_n; ++bi) {
            int quads = batch_count[bi];
            int water = batch_water[bi];
            DWORD fi, fr;
            if (quads <= 0)
                continue;
            memset(&pc, 0, sizeof(pc));
            pc.screen[0] = (float)soft_w;
            pc.screen[1] = (float)soft_h;
            if (water && s.water_pipe) {
                int is_deep = (strstr(bt[bi][0]->name, "dwater") != NULL);
                fi = is_deep ? fi_dw : fi_sw;
                fr = is_deep ? fr_dw : fr_sw;
                if (fr < 2)
                    fr = (DWORD)(bt[bi][0]->h / (bt[bi][0]->w > 0 ? bt[bi][0]->w : 512));
                if (fr < 2)
                    fr = 10;
                pc.frac = frac;
                pc.frame_i = (float)(fi % fr);
                pc.frames = (float)fr;
                pc.inv_frames = 1.0f / (float)fr;
                s.fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.water_pipe);
                s.fn.vkCmdPushConstants(cmd, s.pipe_layout,
                                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(pc), &pc);
            } else {
                s.fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
                s.fn.vkCmdPushConstants(cmd, s.pipe_layout,
                                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(pc), &pc);
            }
            s.fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout, 0, 1,
                                         &bdset[bi], 0, NULL);
            s.fn.vkCmdDraw(cmd, (uint32_t)(quads * 6), 1, (uint32_t)base, 0);
            base += quads * 6;
        }
    }
    s.fn.vkCmdEndRenderPass(cmd);
    s.fn.vkDestroyFramebuffer(s.device, fb, NULL);

    /* #region agent log */
    ms_draw = hitch_qpc_ms_since(t_draw);
    ms_all = hitch_qpc_ms_since(t_all);
    {
        static LONG s_perf_n;
        LONG pn = InterlockedIncrement(&s_perf_n);
        /* Baseline + always log heavy frames (map regions with many batches / slow mesh). */
        if (pn <= 30 || (pn % 30) == 0 || ms_all >= 3.0 || batch_n >= 40 || nvert >= 12000) {
            char data[480];
            snprintf(data, sizeof(data),
                     "{\"n\":%ld,\"ms_all\":%.2f,\"ms_p1\":%.2f,\"ms_p2\":%.2f,\"ms_draw\":%.2f,"
                     "\"tiles\":%d,\"nvert\":%d,\"batches\":%d,\"trans\":%d,\"subdiv\":%d,"
                     "\"soft_cam\":%d,\"cam\":[%ld,%ld,%ld,%ld],\"world\":[%ld,%ld,%ld,%ld],"
                     "\"soft\":[%d,%d],\"overpaint\":%d,\"proj\":%d,\"light\":%d,\"trans_on\":%d,"
                     "\"zgrid\":[%u,%u],\"hgrid\":[%u,%u],\"spike\":%d}",
                     (long)pn, ms_all, ms_p1, ms_p2, ms_draw, tiles, nvert, batch_n, trans_tiles,
                     frame_subdiv, soft_cam, (long)L, (long)T, (long)R, (long)B, (long)wx0,
                     (long)wy0, (long)wx1, (long)wy1, soft_w, soft_h, s.overpaint, s.proj, s.light,
                     s.trans, grids->z_nx, grids->z_ny, grids->h_nx, grids->h_ny,
                     (ms_all >= 3.0 || batch_n >= 40) ? 1 : 0);
            hooks_agent("H-TERR", "vk_terrain.c:record", "terrain-cpu", data);
        }
    }
    /* #endregion */

    n = InterlockedIncrement(&s.draws);
    if (n <= 5 || (n % 60) == 0) {
        log_msg("vk_terrain: draw #%ld tiles=%d trans=%d batches=%d pairs=%d soft=%dx%d", (long)n,
                tiles, trans_tiles, batch_n, s.pair_n, soft_w, soft_h);
    }
    return tiles;
}
