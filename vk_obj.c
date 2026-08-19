#include "vk_obj.h"
#include "vk_iso_depth.h"
#include "obj_spawn.h"
#include "obj_player.h"
#include "hooks_internal.h"
#include "ktx_obj.h"
#include "ktx_terrain.h"
#include "hitch.h"
#include "log.h"

#include "shaders/obj_vert_spv.h"
#include "shaders/obj_frag_spv.h"

#include <ctype.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CK_MAP_MGR = 0x008C7B30u,
    VERT_MAX = 16384 * 6,
    DRAW_MAX = 8192,
    ATLAS_MAX = 512,
};

typedef struct {
    float x, y;
    float u, v;
    float layer; /* texture2DArray layer index */
    float shadow; /* 1 = drawmode shadow */
    float pc; /* 1 = apply team-color remap */
    float tr, tg, tb; /* team RGB 0..1 */
    float sort_y;
} ObjVert;

typedef struct {
    float sx0, sy0, sx1, sy1;
    float u0, v0, u1, v1;
    float sort_y;
    int layer;
    int shadow;
    int pc;
    float tr, tg, tb;
} ObjQuad;

/* Streaming atlas caches:
 * BC3 2048×3072×96 — albedo; R8 same dims — PC material; RGBA8 2048×2048×64 — soft-alpha.
 * Frag encodes RGBA layers as layer+1000. */
enum {
    ARR_W = 2048,
    ARR_H = 3072,
    ARR_LAYERS = 96,
    ARR_LAYER_BYTES = (ARR_W / 4) * (ARR_H / 4) * 16, /* BC3 */
    ARR_MAT_LAYER_BYTES = ARR_W * ARR_H,               /* R8 material */
    ARR_RGBA_W = 2048,
    ARR_RGBA_H = 2048,
    /* Soft-alpha / legacy index-in-alpha atlases only. */
    ARR_RGBA_LAYERS = 64,
    ARR_RGBA_LAYER_BYTES = ARR_RGBA_W * ARR_RGBA_H * 4,
    LAYER_RGBA_BASE = 1000
};

static struct {
    int ready;
    int wanted;
    VkDevice device;
    VkPhysicalDevice phys;
    VkQueue queue;
    uint32_t qfam;
    VkFormat soft_fmt;
    PFN_vkGetDeviceProcAddr gpa;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;

    struct {
        PFN_vkCreateShaderModule vkCreateShaderModule;
        PFN_vkDestroyShaderModule vkDestroyShaderModule;
        PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
        PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
        PFN_vkCreateRenderPass vkCreateRenderPass;
        PFN_vkDestroyRenderPass vkDestroyRenderPass;
        PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
        PFN_vkDestroyPipeline vkDestroyPipeline;
        PFN_vkCreateSampler vkCreateSampler;
        PFN_vkDestroySampler vkDestroySampler;
        PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
        PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
        PFN_vkResetDescriptorPool vkResetDescriptorPool;
        PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
        PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
        PFN_vkCreateBuffer vkCreateBuffer;
        PFN_vkDestroyBuffer vkDestroyBuffer;
        PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
        PFN_vkAllocateMemory vkAllocateMemory;
        PFN_vkFreeMemory vkFreeMemory;
        PFN_vkBindBufferMemory vkBindBufferMemory;
        PFN_vkMapMemory vkMapMemory;
        PFN_vkUnmapMemory vkUnmapMemory;
        PFN_vkCreateImage vkCreateImage;
        PFN_vkDestroyImage vkDestroyImage;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
        PFN_vkBindImageMemory vkBindImageMemory;
        PFN_vkCreateImageView vkCreateImageView;
        PFN_vkDestroyImageView vkDestroyImageView;
        PFN_vkCreateCommandPool vkCreateCommandPool;
        PFN_vkDestroyCommandPool vkDestroyCommandPool;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
        PFN_vkEndCommandBuffer vkEndCommandBuffer;
        PFN_vkQueueSubmit vkQueueSubmit;
        PFN_vkQueueWaitIdle vkQueueWaitIdle;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
        PFN_vkCreateFence vkCreateFence;
        PFN_vkDestroyFence vkDestroyFence;
        PFN_vkResetFences vkResetFences;
        PFN_vkGetFenceStatus vkGetFenceStatus;
        PFN_vkWaitForFences vkWaitForFences;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
        PFN_vkCreateFramebuffer vkCreateFramebuffer;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
        PFN_vkCmdBindPipeline vkCmdBindPipeline;
        PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
        PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
        PFN_vkCmdPushConstants vkCmdPushConstants;
        PFN_vkCmdDraw vkCmdDraw;
        PFN_vkCmdSetViewport vkCmdSetViewport;
        PFN_vkCmdSetScissor vkCmdSetScissor;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    } fn;

    VkShaderModule vert, frag;
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout pipe_layout;
    VkRenderPass rp;
    VkPipeline pipe;
    VkSampler sampler;
    VkDescriptorPool pool;
    VkBuffer vbo;
    VkDeviceMemory vbo_mem;
    void *vbo_ptr;
    VkCommandPool upload_pool;
    VkCommandBuffer upload_cmd;
    VkFence upload_fence;

    /* Persistent soft framebuffer (recreate only on size/view change). */
    VkFramebuffer soft_fb;
    VkImageView soft_fb_view;
    VkImageView soft_fb_depth;
    int soft_fb_w, soft_fb_h;

    /* Texture2DArray streaming cache (replaces per-atlas VkImage). */
    VkImage arr_img;
    VkDeviceMemory arr_mem;
    VkImageView arr_view;
    VkImage rgba_img;
    VkDeviceMemory rgba_mem;
    VkImageView rgba_view;
    VkImage mat_img;
    VkDeviceMemory mat_mem;
    VkImageView mat_view;
    VkDescriptorSet arr_dset;
    int atlas_layer[ATLAS_MAX]; /* -1 = not resident */
    int atlas_rgba[ATLAS_MAX];  /* 1 if resident in rgba array */
    int layer_atlas[ARR_LAYERS]; /* catalog idx or -1 (BC3) */
    int layer_ready[ARR_LAYERS];
    DWORD layer_tick[ARR_LAYERS];
    DWORD layer_pin[ARR_LAYERS]; /* == frame_id if used this record */
    int rgba_layer_atlas[ARR_RGBA_LAYERS];
    int rgba_layer_ready[ARR_RGBA_LAYERS];
    DWORD rgba_layer_tick[ARR_RGBA_LAYERS];
    DWORD rgba_layer_pin[ARR_RGBA_LAYERS];
    DWORD frame_id;
    int arr_resident;
    int rgba_resident;

    /* Persistent HOST staging for one BC3 layer (no per-upload alloc). */
    VkBuffer stage_buf;
    VkDeviceMemory stage_mem;
    void *stage_ptr;
    VkDeviceSize stage_size;

    int up_active;
    int up_idx;
    int up_layer;
    int up_rgba;
    double up_load_ms;
    char up_name[48];

    int want[192];
    int want_n;

    volatile LONG draws;
} s;

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

extern float g_vk_last_decor_sy_max;

#define LOAD_DEV(name)                                                                             \
    do {                                                                                           \
        s.fn.name = (PFN_##name)s.gpa(s.device, #name);                                            \
        if (!s.fn.name)                                                                            \
            return 0;                                                                              \
    } while (0)

#define LOAD_INST(name)                                                                            \
    do {                                                                                           \
        s.fn.name = (PFN_##name)(s.gipa ? s.gipa(s.instance, #name) : NULL);                       \
        if (!s.fn.name)                                                                            \
            return 0;                                                                              \
    } while (0)

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    if (!s.fn.vkGetPhysicalDeviceMemoryProperties)
        return 0;
    s.fn.vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return 0;
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

static float bilerp4(float a00, float a10, float a01, float a11, float fx, float fy)
{
    float u = a00 * (1.0f - fx) + a10 * fx;
    float v = a01 * (1.0f - fx) + a11 * fx;
    return u * (1.0f - fy) + v * fy;
}

static float exact_h(void *terr, LONG px, LONG py)
{
    LONG x0 = (LONG)((DWORD)px & ~31u);
    LONG y0 = (LONG)((DWORD)py & ~31u);
    float tx = (float)(px - x0) * (1.0f / 32.0f);
    float ty = (float)(py - y0) * (1.0f / 32.0f);
    return bilerp4((float)cell_h(terr, x0, y0), (float)cell_h(terr, x0 + 32, y0),
                   (float)cell_h(terr, x0, y0 + 32), (float)cell_h(terr, x0 + 32, y0 + 32), tx, ty);
}

static int map_to_proj_y(LONG wy)
{
    return (int)(((DWORD)wy * 0xb5u) >> 8);
}

static float map_to_view_yf(LONG wy, float h)
{
    return (float)map_to_proj_y(wy) - h;
}

/* Same iso Y as map_to_view_yf, but float map-Y for move lerp. */
static float map_to_view_yf_f(float wy, float h)
{
    return wy * (181.f / 256.f) - h;
}

static int load_fns(void)
{
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);
    LOAD_DEV(vkCreateDescriptorSetLayout);
    LOAD_DEV(vkDestroyDescriptorSetLayout);
    LOAD_DEV(vkCreatePipelineLayout);
    LOAD_DEV(vkDestroyPipelineLayout);
    LOAD_DEV(vkCreateRenderPass);
    LOAD_DEV(vkDestroyRenderPass);
    LOAD_DEV(vkCreateGraphicsPipelines);
    LOAD_DEV(vkDestroyPipeline);
    LOAD_DEV(vkCreateSampler);
    LOAD_DEV(vkDestroySampler);
    LOAD_DEV(vkCreateDescriptorPool);
    LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkResetDescriptorPool);
    LOAD_DEV(vkAllocateDescriptorSets);
    LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkMapMemory);
    LOAD_DEV(vkUnmapMemory);
    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageMemoryRequirements);
    LOAD_DEV(vkBindImageMemory);
    LOAD_DEV(vkCreateImageView);
    LOAD_DEV(vkDestroyImageView);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkQueueSubmit);
    LOAD_DEV(vkQueueWaitIdle);
    LOAD_DEV(vkDeviceWaitIdle);
    LOAD_DEV(vkCreateFence);
    LOAD_DEV(vkDestroyFence);
    LOAD_DEV(vkResetFences);
    LOAD_DEV(vkGetFenceStatus);
    LOAD_DEV(vkWaitForFences);
    LOAD_DEV(vkCmdPipelineBarrier);
    LOAD_DEV(vkCmdCopyBufferToImage);
    LOAD_DEV(vkCreateFramebuffer);
    LOAD_DEV(vkDestroyFramebuffer);
    LOAD_DEV(vkCmdBeginRenderPass);
    LOAD_DEV(vkCmdEndRenderPass);
    LOAD_DEV(vkCmdBindPipeline);
    LOAD_DEV(vkCmdBindVertexBuffers);
    LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdPushConstants);
    LOAD_DEV(vkCmdDraw);
    LOAD_DEV(vkCmdSetViewport);
    LOAD_DEV(vkCmdSetScissor);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    return 1;
}

static int atlas_fits_array(uint32_t w, uint32_t h, int is_rgba)
{
    if (w < 1 || h < 1)
        return 0;
    if (is_rgba)
        return w <= (uint32_t)ARR_RGBA_W && h <= (uint32_t)ARR_RGBA_H;
    return w <= (uint32_t)ARR_W && h <= (uint32_t)ARR_H;
}

int vk_obj_atlas_fits_array(int atlas_index)
{
    const KtxObjAtlas *a;
    int n = ktx_obj_atlas_count();
    if (atlas_index < 0 || atlas_index >= n)
        return 0;
    a = &ktx_obj_atlases()[atlas_index];
    return atlas_fits_array(a->width, a->height, a->is_rgba);
}

static void arr_maps_clear(void)
{
    int i;
    for (i = 0; i < ATLAS_MAX; ++i) {
        s.atlas_layer[i] = -1;
        s.atlas_rgba[i] = 0;
    }
    for (i = 0; i < ARR_LAYERS; ++i) {
        s.layer_atlas[i] = -1;
        s.layer_ready[i] = 0;
        s.layer_tick[i] = 0;
        s.layer_pin[i] = 0;
    }
    for (i = 0; i < ARR_RGBA_LAYERS; ++i) {
        s.rgba_layer_atlas[i] = -1;
        s.rgba_layer_ready[i] = 0;
        s.rgba_layer_tick[i] = 0;
        s.rgba_layer_pin[i] = 0;
    }
    s.arr_resident = 0;
    s.rgba_resident = 0;
}

static void destroy_atlases(void)
{
    if (s.up_active) {
        if (s.upload_fence && s.fn.vkWaitForFences)
            s.fn.vkWaitForFences(s.device, 1, &s.upload_fence, VK_TRUE, UINT64_MAX);
        s.up_active = 0;
    }
    if (s.stage_ptr && s.stage_mem && s.fn.vkUnmapMemory) {
        s.fn.vkUnmapMemory(s.device, s.stage_mem);
        s.stage_ptr = NULL;
    }
    if (s.stage_buf && s.fn.vkDestroyBuffer)
        s.fn.vkDestroyBuffer(s.device, s.stage_buf, NULL);
    if (s.stage_mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.stage_mem, NULL);
    s.stage_buf = VK_NULL_HANDLE;
    s.stage_mem = VK_NULL_HANDLE;
    s.stage_size = 0;
    if (s.arr_view && s.fn.vkDestroyImageView)
        s.fn.vkDestroyImageView(s.device, s.arr_view, NULL);
    if (s.arr_img && s.fn.vkDestroyImage)
        s.fn.vkDestroyImage(s.device, s.arr_img, NULL);
    if (s.arr_mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.arr_mem, NULL);
    s.arr_view = VK_NULL_HANDLE;
    s.arr_img = VK_NULL_HANDLE;
    s.arr_mem = VK_NULL_HANDLE;
    if (s.rgba_view && s.fn.vkDestroyImageView)
        s.fn.vkDestroyImageView(s.device, s.rgba_view, NULL);
    if (s.rgba_img && s.fn.vkDestroyImage)
        s.fn.vkDestroyImage(s.device, s.rgba_img, NULL);
    if (s.rgba_mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.rgba_mem, NULL);
    s.rgba_view = VK_NULL_HANDLE;
    s.rgba_img = VK_NULL_HANDLE;
    s.rgba_mem = VK_NULL_HANDLE;
    if (s.mat_view && s.fn.vkDestroyImageView)
        s.fn.vkDestroyImageView(s.device, s.mat_view, NULL);
    if (s.mat_img && s.fn.vkDestroyImage)
        s.fn.vkDestroyImage(s.device, s.mat_img, NULL);
    if (s.mat_mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.mat_mem, NULL);
    s.mat_view = VK_NULL_HANDLE;
    s.mat_img = VK_NULL_HANDLE;
    s.mat_mem = VK_NULL_HANDLE;
    s.arr_dset = VK_NULL_HANDLE;
    arr_maps_clear();
    s.want_n = 0;
}

static int ensure_array_resources(void)
{
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkImageViewCreateInfo vci;
    VkBufferCreateInfo bci;
    VkDescriptorSetAllocateInfo dai;
    VkDescriptorImageInfo dii[3];
    VkWriteDescriptorSet wds[3];
    VkImageMemoryBarrier barr[3];
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkResult r;
    VkDeviceSize stage_need;

    if (s.arr_img && s.arr_view && s.rgba_img && s.rgba_view && s.mat_img && s.mat_view &&
        s.arr_dset && s.stage_ptr)
        return 1;

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_BC3_UNORM_BLOCK;
    ici.extent.width = (uint32_t)ARR_W;
    ici.extent.height = (uint32_t)ARR_H;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = (uint32_t)ARR_LAYERS;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &s.arr_img);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkGetImageMemoryRequirements(s.device, s.arr_img, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &s.arr_mem);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkBindImageMemory(s.device, s.arr_img, s.arr_mem, 0);

    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s.arr_img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = VK_FORMAT_BC3_UNORM_BLOCK;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = (uint32_t)ARR_LAYERS;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &s.arr_view);
    if (r != VK_SUCCESS)
        return 0;

    ici.format = VK_FORMAT_R8G8B8A8_SRGB;
    ici.extent.width = (uint32_t)ARR_RGBA_W;
    ici.extent.height = (uint32_t)ARR_RGBA_H;
    ici.arrayLayers = (uint32_t)ARR_RGBA_LAYERS;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &s.rgba_img);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkGetImageMemoryRequirements(s.device, s.rgba_img, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &s.rgba_mem);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkBindImageMemory(s.device, s.rgba_img, s.rgba_mem, 0);

    vci.image = s.rgba_img;
    vci.format = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange.layerCount = (uint32_t)ARR_RGBA_LAYERS;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &s.rgba_view);
    if (r != VK_SUCCESS)
        return 0;

    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent.width = (uint32_t)ARR_W;
    ici.extent.height = (uint32_t)ARR_H;
    ici.arrayLayers = (uint32_t)ARR_LAYERS;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &s.mat_img);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkGetImageMemoryRequirements(s.device, s.mat_img, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &s.mat_mem);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkBindImageMemory(s.device, s.mat_img, s.mat_mem, 0);
    vci.image = s.mat_img;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange.layerCount = (uint32_t)ARR_LAYERS;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &s.mat_view);
    if (r != VK_SUCCESS)
        return 0;

    stage_need = (VkDeviceSize)ARR_LAYER_BYTES + (VkDeviceSize)ARR_MAT_LAYER_BYTES;
    if ((VkDeviceSize)ARR_RGBA_LAYER_BYTES > stage_need)
        stage_need = (VkDeviceSize)ARR_RGBA_LAYER_BYTES;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = stage_need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    r = s.fn.vkCreateBuffer(s.device, &bci, NULL, &s.stage_buf);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkGetBufferMemoryRequirements(s.device, s.stage_buf, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &s.stage_mem);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkBindBufferMemory(s.device, s.stage_buf, s.stage_mem, 0);
    r = s.fn.vkMapMemory(s.device, s.stage_mem, 0, bci.size, 0, &s.stage_ptr);
    if (r != VK_SUCCESS)
        return 0;
    s.stage_size = bci.size;
    memset(s.stage_ptr, 0, (size_t)s.stage_size);

    memset(&dai, 0, sizeof(dai));
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.dset_layout;
    r = s.fn.vkAllocateDescriptorSets(s.device, &dai, &s.arr_dset);
    if (r != VK_SUCCESS)
        return 0;
    memset(dii, 0, sizeof(dii));
    dii[0].sampler = s.sampler;
    dii[0].imageView = s.arr_view;
    dii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii[1].sampler = s.sampler;
    dii[1].imageView = s.rgba_view;
    dii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii[2].sampler = s.sampler;
    dii[2].imageView = s.mat_view;
    dii[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(wds, 0, sizeof(wds));
    wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[0].dstSet = s.arr_dset;
    wds[0].dstBinding = 0;
    wds[0].descriptorCount = 1;
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[0].pImageInfo = &dii[0];
    wds[1] = wds[0];
    wds[1].dstBinding = 1;
    wds[1].pImageInfo = &dii[1];
    wds[2] = wds[0];
    wds[2].dstBinding = 2;
    wds[2].pImageInfo = &dii[2];
    s.fn.vkUpdateDescriptorSets(s.device, 3, wds, 0, NULL);

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s.fn.vkBeginCommandBuffer(s.upload_cmd, &bi);
    memset(barr, 0, sizeof(barr));
    barr[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barr[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barr[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barr[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr[0].image = s.arr_img;
    barr[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barr[0].subresourceRange.levelCount = 1;
    barr[0].subresourceRange.layerCount = (uint32_t)ARR_LAYERS;
    barr[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barr[1] = barr[0];
    barr[1].image = s.rgba_img;
    barr[1].subresourceRange.layerCount = (uint32_t)ARR_RGBA_LAYERS;
    barr[2] = barr[0];
    barr[2].image = s.mat_img;
    barr[2].subresourceRange.layerCount = (uint32_t)ARR_LAYERS;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 3, barr);
    s.fn.vkEndCommandBuffer(s.upload_cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.upload_cmd;
    if (s.fn.vkQueueSubmit(s.queue, 1, &si, s.upload_fence) != VK_SUCCESS)
        return 0;
    s.fn.vkWaitForFences(s.device, 1, &s.upload_fence, VK_TRUE, UINT64_MAX);
    s.fn.vkResetFences(s.device, 1, &s.upload_fence);

    arr_maps_clear();
    log_msg("vk_obj: texarray BC3 %dx%dx%d + RGBA8 %dx%dx%d + R8 mat (~%d+%d+%d MiB)", ARR_W, ARR_H,
            ARR_LAYERS, ARR_RGBA_W, ARR_RGBA_H, ARR_RGBA_LAYERS,
            (ARR_LAYER_BYTES * ARR_LAYERS) / (1024 * 1024),
            (ARR_RGBA_LAYER_BYTES * ARR_RGBA_LAYERS) / (1024 * 1024),
            (ARR_MAT_LAYER_BYTES * ARR_LAYERS) / (1024 * 1024));
    /* #region agent log */
    {
        char data[220];
        snprintf(data, sizeof(data),
                 "{\"bc3\":[%d,%d,%d],\"rgba\":[%d,%d,%d],\"mat\":[%d,%d,%d],\"mode\":\"bc3+r8-mat\"}",
                 ARR_W, ARR_H, ARR_LAYERS, ARR_RGBA_W, ARR_RGBA_H, ARR_RGBA_LAYERS, ARR_W, ARR_H,
                 ARR_LAYERS);
        hooks_agent("H-OBJ", "vk_obj.c:ensure_array", "obj-arr-init", data);
    }
    /* #endregion */
    return 1;
}

static void obj_want_atlas(int idx)
{
    int i;
    int ncat = ktx_obj_atlas_count();
    if (idx < 0 || idx >= ATLAS_MAX || idx >= ncat)
        return;
    if (!vk_obj_atlas_fits_array(idx))
        return;
    if (s.atlas_layer[idx] >= 0)
        return;
    if (s.up_active && s.up_idx == idx)
        return;
    for (i = 0; i < s.want_n; ++i) {
        if (s.want[i] == idx)
            return;
    }
    if (s.want_n < (int)(sizeof(s.want) / sizeof(s.want[0])))
        s.want[s.want_n++] = idx;
}

int vk_obj_atlas_resident(int atlas_index)
{
    int layer;
    if (!s.ready || atlas_index < 0 || atlas_index >= ATLAS_MAX)
        return 0;
    layer = s.atlas_layer[atlas_index];
    if (layer < 0)
        return 0;
    if (s.atlas_rgba[atlas_index])
        return (layer < ARR_RGBA_LAYERS && s.rgba_layer_ready[layer]) ? 1 : 0;
    return (layer < ARR_LAYERS && s.layer_ready[layer]) ? 1 : 0;
}

void vk_obj_request_atlas(int atlas_index)
{
    obj_want_atlas(atlas_index);
}

void vk_obj_request_id(const char *id)
{
    char key[48];
    int si;
    int seen[32];
    int seen_n = 0;
    if (!id || !id[0] || !ktx_obj_resolve_id(id, key, sizeof(key)))
        return;
    for (si = ktx_obj_spr_head(key); si >= 0; si = ktx_obj_spr_next(si)) {
        const KtxObjSprite *sp = &ktx_obj_sprites()[si];
        int j, atl, dup = 0;
        if (ktx_obj_sprite_is_shadow(sp))
            continue;
        atl = sp->atlas_index;
        if (!vk_obj_atlas_fits_array(atl))
            continue;
        for (j = 0; j < seen_n; ++j) {
            if (seen[j] == atl) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        if (seen_n < (int)(sizeof(seen) / sizeof(seen[0])))
            seen[seen_n++] = atl;
        obj_want_atlas(atl);
    }
}

static uint32_t atlas_area(int idx)
{
    const KtxObjAtlas *a;
    int n = ktx_obj_atlas_count();
    if (idx < 0 || idx >= n)
        return 0;
    a = &ktx_obj_atlases()[idx];
    return a->width * a->height;
}

/* Prefer large building sheets (shipyard/fort) over tiny props in the want FIFO. */
static int want_take_best(void)
{
    int i, best_i = 0, best_idx;
    uint32_t best_a, a;
    if (s.want_n < 1)
        return -1;
    best_idx = s.want[0];
    best_a = atlas_area(best_idx);
    for (i = 1; i < s.want_n; ++i) {
        a = atlas_area(s.want[i]);
        if (a > best_a) {
            best_a = a;
            best_i = i;
            best_idx = s.want[i];
        }
    }
    for (i = best_i + 1; i < s.want_n; ++i)
        s.want[i - 1] = s.want[i];
    s.want_n--;
    return best_idx;
}

static int alloc_array_layer(int is_rgba)
{
    int i, best = -1, n;
    DWORD best_tick = 0xFFFFFFFFu;
    int *latlas;
    int *lready;
    DWORD *ltick;
    DWORD *lpin;

    if (is_rgba) {
        n = ARR_RGBA_LAYERS;
        latlas = s.rgba_layer_atlas;
        lready = s.rgba_layer_ready;
        ltick = s.rgba_layer_tick;
        lpin = s.rgba_layer_pin;
    } else {
        n = ARR_LAYERS;
        latlas = s.layer_atlas;
        lready = s.layer_ready;
        ltick = s.layer_tick;
        lpin = s.layer_pin;
    }

    for (i = 0; i < n; ++i) {
        if (latlas[i] < 0)
            return i;
    }
    for (i = 0; i < n; ++i) {
        if (s.up_active && s.up_layer == i && !!s.up_rgba == !!is_rgba)
            continue;
        if (lpin[i] == s.frame_id)
            continue;
        if (ltick[i] <= best_tick) {
            best_tick = ltick[i];
            best = i;
        }
    }
    if (best < 0) {
        /* #region agent log */
        {
            char data[96];
            snprintf(data, sizeof(data),
                     "{\"err\":\"all_pinned\",\"rgba\":%d,\"resident\":%d,\"frame\":%lu}", is_rgba,
                     is_rgba ? s.rgba_resident : s.arr_resident, (unsigned long)s.frame_id);
            hooks_agent("H-OBJ", "vk_obj.c:alloc_layer", "obj-arr-evict", data);
        }
        /* #endregion */
        return -1;
    }
    {
        int old = latlas[best];
        if (old >= 0 && old < ATLAS_MAX) {
            s.atlas_layer[old] = -1;
            s.atlas_rgba[old] = 0;
        }
        latlas[best] = -1;
        lready[best] = 0;
        if (is_rgba) {
            if (s.rgba_resident > 0)
                s.rgba_resident--;
        } else if (s.arr_resident > 0) {
            s.arr_resident--;
        }
        /* #region agent log */
        {
            char data[200];
            const char *oname = "";
            int has_idx = 0;
            if (old >= 0 && old < ktx_obj_atlas_count()) {
                const KtxObjAtlas *oa = &ktx_obj_atlases()[old];
                oname = oa->name[0] ? oa->name : oa->ktx2;
                has_idx = oa->has_idx;
            }
            snprintf(data, sizeof(data),
                     "{\"evict_layer\":%d,\"old_atlas\":%d,\"name\":\"%.32s\",\"has_idx\":%d,"
                     "\"rgba\":%d,\"resident\":%d,\"hyp\":\"A-evict\"}",
                     best, old, oname, has_idx, is_rgba,
                     is_rgba ? s.rgba_resident : s.arr_resident);
            hooks_agent("H-OBJ", "vk_obj.c:alloc_layer", "obj-arr-evict", data);
        }
        /* #endregion */
    }
    return best;
}

static void stage_bc3_tight(const uint8_t *src, uint32_t src_len)
{
    /* Vulkan streaming: copy only atlas bytes (not full 2048² layer). */
    if (!src || !s.stage_ptr || src_len == 0)
        return;
    if (src_len > s.stage_size)
        src_len = (uint32_t)s.stage_size;
    memcpy(s.stage_ptr, src, src_len);
}

/* Fence signaled → layer ready for sampling. Staging buffer is persistent. */
static int finish_atlas_upload(void)
{
    VkResult r;
    const KtxObjAtlas *atl;
    int layer;
    int is_rgba;

    if (!s.up_active)
        return 0;
    r = s.fn.vkGetFenceStatus(s.device, s.upload_fence);
    if (r == VK_NOT_READY)
        return 0;
    if (r != VK_SUCCESS) {
        if (s.up_idx >= 0 && s.up_idx < ATLAS_MAX) {
            s.atlas_layer[s.up_idx] = -1;
            s.atlas_rgba[s.up_idx] = 0;
        }
        if (s.up_rgba) {
            if (s.up_layer >= 0 && s.up_layer < ARR_RGBA_LAYERS) {
                s.rgba_layer_atlas[s.up_layer] = -1;
                s.rgba_layer_ready[s.up_layer] = 0;
            }
        } else if (s.up_layer >= 0 && s.up_layer < ARR_LAYERS) {
            s.layer_atlas[s.up_layer] = -1;
            s.layer_ready[s.up_layer] = 0;
        }
        s.up_active = 0;
        return 0;
    }

    layer = s.up_layer;
    is_rgba = s.up_rgba;
    atl = &ktx_obj_atlases()[s.up_idx];
    if (is_rgba) {
        s.rgba_layer_ready[layer] = 1;
        s.rgba_layer_tick[layer] = s.frame_id;
        s.atlas_layer[s.up_idx] = layer;
        s.atlas_rgba[s.up_idx] = 1;
        s.rgba_layer_atlas[layer] = s.up_idx;
        s.rgba_resident++;
    } else {
        s.layer_ready[layer] = 1;
        s.layer_tick[layer] = s.frame_id;
        s.atlas_layer[s.up_idx] = layer;
        s.atlas_rgba[s.up_idx] = 0;
        s.layer_atlas[layer] = s.up_idx;
        s.arr_resident++;
    }
    log_msg("vk_obj: arr[%d]<-atlas[%d] %s sprites=%d resident=%d%s", layer, s.up_idx, s.up_name,
            atl->sprite_count, is_rgba ? s.rgba_resident : s.arr_resident,
            is_rgba ? " rgba" : "");
    /* #region agent log */
    {
        static LONG s_up_n;
        LONG n = InterlockedIncrement(&s_up_n);
        char data[208];
        snprintf(data, sizeof(data),
                 "{\"n\":%ld,\"idx\":%d,\"layer\":%d,\"name\":\"%.32s\",\"resident\":%d,"
                 "\"load_ms\":%.2f,\"rgba\":%d,\"mode\":\"arr\"}",
                 (long)n, s.up_idx, layer, s.up_name, is_rgba ? s.rgba_resident : s.arr_resident,
                 s.up_load_ms, is_rgba);
        hooks_agent("H-OBJ", "vk_obj.c:finish_atlas", "obj-atlas-upload", data);
    }
    /* #endregion */
    s.fn.vkResetFences(s.device, 1, &s.upload_fence);
    s.up_active = 0;
    return 1;
}

/* Copy CPU texels into one array layer via persistent staging (no per-upload alloc).
 * Optional mat_blocks (R8) upload to the same layer of atlasMat. */
static int begin_atlas_upload_gpu(int idx, const Ktx2Info *info, uint8_t *blocks,
                                  const Ktx2Info *mat_info, uint8_t *mat_blocks, double load_ms)
{
    VkBufferImageCopy bic;
    VkImageMemoryBarrier barr;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkResult r;
    const KtxObjAtlas *atl;
    LONGLONG t0;
    int layer;
    int is_rgba;
    int is_bc3;
    VkImage dst_img;
    uint32_t mat_off = 0;
    uint32_t mat_bytes = 0;
    int have_mat = 0;

    if (!info || !blocks) {
        free(mat_blocks);
        return 0;
    }
    if (s.up_active || idx < 0 || idx >= ATLAS_MAX) {
        free(blocks);
        free(mat_blocks);
        return 0;
    }
    if (s.atlas_layer[idx] >= 0) {
        int L = s.atlas_layer[idx];
        int ok = s.atlas_rgba[idx] ? (L < ARR_RGBA_LAYERS && s.rgba_layer_ready[L])
                                    : (L < ARR_LAYERS && s.layer_ready[L]);
        if (ok) {
            free(blocks);
            free(mat_blocks);
            return 0;
        }
    }
    if (!ensure_array_resources()) {
        free(blocks);
        free(mat_blocks);
        return 0;
    }
    is_bc3 = (info->vk_format == 137u || info->vk_format == 138u);
    is_rgba = (info->vk_format == 37u || info->vk_format == 43u);
    if (!is_bc3 && !is_rgba) {
        /* #region agent log */
        {
            char data[128];
            snprintf(data, sizeof(data),
                     "{\"idx\":%d,\"fmt\":%u,\"wh\":[%u,%u],\"err\":\"bad_fmt\"}", idx,
                     info->vk_format, info->width, info->height);
            hooks_agent("H-OBJ", "vk_obj.c:begin_atlas", "obj-atlas-reject", data);
        }
        /* #endregion */
        free(blocks);
        free(mat_blocks);
        return 0;
    }
    if (!atlas_fits_array(info->width, info->height, is_rgba)) {
        /* #region agent log */
        {
            char data[128];
            snprintf(data, sizeof(data),
                     "{\"idx\":%d,\"fmt\":%u,\"wh\":[%u,%u],\"err\":\"too_big\"}", idx,
                     info->vk_format, info->width, info->height);
            hooks_agent("H-OBJ", "vk_obj.c:begin_atlas", "obj-atlas-reject", data);
        }
        /* #endregion */
        free(blocks);
        free(mat_blocks);
        return 0;
    }

    atl = &ktx_obj_atlases()[idx];
    s.up_load_ms = load_ms;
    t0 = hitch_qpc_now();
    layer = alloc_array_layer(is_rgba);
    if (layer < 0) {
        free(blocks);
        free(mat_blocks);
        return 0;
    }

    /* Material: use provided R8, else clear layer (no stale PC indices). */
    if (!is_rgba) {
        mat_off = (info->level0_len + 15u) & ~15u;
        mat_bytes = (uint32_t)(info->width * info->height);
        if (mat_bytes > (uint32_t)ARR_MAT_LAYER_BYTES)
            mat_bytes = (uint32_t)ARR_MAT_LAYER_BYTES;
        if (mat_off + mat_bytes > (uint32_t)s.stage_size) {
            free(blocks);
            free(mat_blocks);
            return 0;
        }
        have_mat = 1;
    }

    stage_bc3_tight(blocks, info->level0_len);
    free(blocks);
    if (have_mat) {
        if (mat_blocks && mat_info && mat_info->vk_format == 9u &&
            mat_info->width == info->width && mat_info->height == info->height &&
            mat_info->level0_len >= mat_bytes) {
            memcpy((uint8_t *)s.stage_ptr + mat_off, mat_blocks, mat_bytes);
        } else {
            memset((uint8_t *)s.stage_ptr + mat_off, 0, mat_bytes);
        }
    }
    free(mat_blocks);

    dst_img = is_rgba ? s.rgba_img : s.arr_img;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s.fn.vkBeginCommandBuffer(s.upload_cmd, &bi);
    memset(&barr, 0, sizeof(barr));
    barr.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barr.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.image = dst_img;
    barr.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barr.subresourceRange.baseArrayLayer = (uint32_t)layer;
    barr.subresourceRange.levelCount = 1;
    barr.subresourceRange.layerCount = 1;
    barr.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.baseArrayLayer = (uint32_t)layer;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = info->width;
    bic.imageExtent.height = info->height;
    bic.imageExtent.depth = 1;
    s.fn.vkCmdCopyBufferToImage(s.upload_cmd, s.stage_buf, dst_img,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);

    if (have_mat) {
        VkBufferImageCopy mbic;
        barr.image = s.mat_img;
        barr.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barr.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
        memset(&mbic, 0, sizeof(mbic));
        mbic.bufferOffset = mat_off;
        mbic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mbic.imageSubresource.baseArrayLayer = (uint32_t)layer;
        mbic.imageSubresource.layerCount = 1;
        mbic.imageExtent.width = info->width;
        mbic.imageExtent.height = info->height;
        mbic.imageExtent.depth = 1;
        s.fn.vkCmdCopyBufferToImage(s.upload_cmd, s.stage_buf, s.mat_img,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mbic);
        barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        s.fn.vkCmdPipelineBarrier(s.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                                  &barr);
    }

    s.fn.vkEndCommandBuffer(s.upload_cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.upload_cmd;
    r = s.fn.vkQueueSubmit(s.queue, 1, &si, s.upload_fence);
    if (r != VK_SUCCESS) {
        s.atlas_layer[idx] = -1;
        s.atlas_rgba[idx] = 0;
        if (is_rgba) {
            s.rgba_layer_atlas[layer] = -1;
            s.rgba_layer_ready[layer] = 0;
        } else {
            s.layer_atlas[layer] = -1;
            s.layer_ready[layer] = 0;
        }
        return 0;
    }

    s.up_active = 1;
    s.up_idx = idx;
    s.up_layer = layer;
    s.up_rgba = is_rgba;
    s.atlas_layer[idx] = layer;
    s.atlas_rgba[idx] = is_rgba ? 1 : 0;
    if (is_rgba) {
        s.rgba_layer_atlas[layer] = idx;
        s.rgba_layer_ready[layer] = 0;
    } else {
        s.layer_atlas[layer] = idx;
        s.layer_ready[layer] = 0;
    }
    lstrcpynA(s.up_name, atl->name[0] ? atl->name : atl->ktx2, (int)sizeof(s.up_name));
    /* #region agent log */
    {
        char data[240];
        snprintf(data, sizeof(data),
                 "{\"idx\":%d,\"layer\":%d,\"name\":\"%.32s\",\"cpu_load_ms\":%.2f,"
                 "\"gpu_ms\":%.2f,\"wh\":[%u,%u],\"bytes\":%u,\"rgba\":%d,\"mat\":%d,"
                 "\"has_idx\":%d,\"mode\":\"arr-mat\"}",
                 idx, layer, s.up_name, s.up_load_ms, hitch_qpc_ms_since(t0), info->width,
                 info->height, info->level0_len, is_rgba, have_mat, atl->has_idx);
        hooks_agent("H-OBJ", "vk_obj.c:begin_atlas", "obj-atlas-begin", data);
    }
    /* #endregion */
    return 1;
}

/* Background KTX file load — main thread only does GPU submit. */
static struct {
    CRITICAL_SECTION cs;
    HANDLE wake;
    HANDLE thread;
    volatile LONG quit;
    int req_idx;     /* -1 = none */
    int loading_idx; /* currently loading */
    int ready_idx;   /* -1 = none */
    Ktx2Info info;
    uint8_t *blocks;
    Ktx2Info mat_info;
    uint8_t *mat_blocks;
    int have_mat;
    double load_ms;
} s_ld;

static DWORD WINAPI obj_atlas_loader_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int idx;
        char path[MAX_PATH];
        Ktx2Info info;
        uint8_t *blocks = NULL;
        LONGLONG t0;
        double load_ms;

        WaitForSingleObject(s_ld.wake, INFINITE);
        if (InterlockedCompareExchange(&s_ld.quit, 0, 0))
            break;
        EnterCriticalSection(&s_ld.cs);
        idx = s_ld.req_idx;
        s_ld.req_idx = -1;
        s_ld.loading_idx = idx;
        LeaveCriticalSection(&s_ld.cs);
        if (idx < 0)
            continue;
        if (!ktx_obj_atlas_path(idx, path, sizeof(path))) {
            EnterCriticalSection(&s_ld.cs);
            s_ld.loading_idx = -1;
            LeaveCriticalSection(&s_ld.cs);
            continue;
        }
        t0 = hitch_qpc_now();
        if (!ktx_terrain_load_info(path, &info, &blocks) || !blocks) {
            EnterCriticalSection(&s_ld.cs);
            s_ld.loading_idx = -1;
            LeaveCriticalSection(&s_ld.cs);
            continue;
        }
        {
            char mpath[MAX_PATH];
            Ktx2Info minfo;
            uint8_t *mblocks = NULL;
            if (ktx_obj_atlas_idx_path(idx, mpath, sizeof(mpath)) &&
                ktx_terrain_load_info(mpath, &minfo, &mblocks) && mblocks) {
                EnterCriticalSection(&s_ld.cs);
                if (s_ld.mat_blocks) {
                    free(s_ld.mat_blocks);
                    s_ld.mat_blocks = NULL;
                }
                s_ld.mat_info = minfo;
                s_ld.mat_blocks = mblocks;
                s_ld.have_mat = 1;
                LeaveCriticalSection(&s_ld.cs);
            } else {
                free(mblocks);
                EnterCriticalSection(&s_ld.cs);
                if (s_ld.mat_blocks) {
                    free(s_ld.mat_blocks);
                    s_ld.mat_blocks = NULL;
                }
                s_ld.have_mat = 0;
                LeaveCriticalSection(&s_ld.cs);
            }
        }
        load_ms = hitch_qpc_ms_since(t0);
        /* #region agent log */
        {
            char data[200];
            const KtxDecorAtlas *atl = &ktx_obj_atlases()[idx];
            snprintf(data, sizeof(data),
                     "{\"idx\":%d,\"name\":\"%.32s\",\"load_ms\":%.2f,\"wh\":[%u,%u],"
                     "\"has_idx\":%d,\"thr\":1}",
                     idx, atl->name[0] ? atl->name : atl->ktx2, load_ms, info.width, info.height,
                     atl->has_idx);
            hooks_agent("H-OBJ", "vk_obj.c:loader", "obj-atlas-cpu", data);
        }
        /* #endregion */
        EnterCriticalSection(&s_ld.cs);
        if (s_ld.blocks) {
            free(s_ld.blocks);
            s_ld.blocks = NULL;
        }
        s_ld.ready_idx = idx;
        s_ld.info = info;
        s_ld.blocks = blocks;
        s_ld.load_ms = load_ms;
        s_ld.loading_idx = -1;
        LeaveCriticalSection(&s_ld.cs);
    }
    return 0;
}

static void obj_loader_start(void)
{
    if (s_ld.thread)
        return;
    InitializeCriticalSection(&s_ld.cs);
    s_ld.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_ld.req_idx = -1;
    s_ld.loading_idx = -1;
    s_ld.ready_idx = -1;
    s_ld.blocks = NULL;
    s_ld.mat_blocks = NULL;
    s_ld.have_mat = 0;
    InterlockedExchange(&s_ld.quit, 0);
    s_ld.thread = CreateThread(NULL, 0, obj_atlas_loader_thread, NULL, 0, NULL);
}

static void obj_loader_stop(void)
{
    if (!s_ld.thread)
        return;
    InterlockedExchange(&s_ld.quit, 1);
    if (s_ld.wake)
        SetEvent(s_ld.wake);
    WaitForSingleObject(s_ld.thread, 5000);
    CloseHandle(s_ld.thread);
    s_ld.thread = NULL;
    if (s_ld.wake) {
        CloseHandle(s_ld.wake);
        s_ld.wake = NULL;
    }
    EnterCriticalSection(&s_ld.cs);
    if (s_ld.blocks)
        free(s_ld.blocks);
    s_ld.blocks = NULL;
    if (s_ld.mat_blocks)
        free(s_ld.mat_blocks);
    s_ld.mat_blocks = NULL;
    s_ld.have_mat = 0;
    s_ld.ready_idx = -1;
    LeaveCriticalSection(&s_ld.cs);
    DeleteCriticalSection(&s_ld.cs);
}

static int obj_loader_busy(void)
{
    int busy;
    EnterCriticalSection(&s_ld.cs);
    busy = (s_ld.req_idx >= 0 || s_ld.loading_idx >= 0 || s_ld.ready_idx >= 0) ? 1 : 0;
    LeaveCriticalSection(&s_ld.cs);
    return busy;
}

static void obj_loader_request(int idx)
{
    int accepted = 0;
    if (idx < 0 || !s_ld.thread)
        return;
    EnterCriticalSection(&s_ld.cs);
    if (s_ld.req_idx < 0 && s_ld.loading_idx < 0 && s_ld.ready_idx < 0) {
        s_ld.req_idx = idx;
        accepted = 1;
    }
    LeaveCriticalSection(&s_ld.cs);
    if (accepted)
        SetEvent(s_ld.wake);
    else
        obj_want_atlas(idx);
}

static int ensure_atlas(int idx)
{
    int layer;
    int ncat = ktx_obj_atlas_count();
    if (idx < 0 || idx >= ATLAS_MAX || idx >= ncat)
        return 0;
    if (!vk_obj_atlas_fits_array(idx))
        return 0;
    layer = s.atlas_layer[idx];
    if (s.atlas_rgba[idx]) {
        if (layer >= 0 && layer < ARR_RGBA_LAYERS && s.rgba_layer_ready[layer]) {
            s.rgba_layer_tick[layer] = s.frame_id;
            s.rgba_layer_pin[layer] = s.frame_id;
            return 1;
        }
    } else if (layer >= 0 && layer < ARR_LAYERS && s.layer_ready[layer]) {
        s.layer_tick[layer] = s.frame_id;
        s.layer_pin[layer] = s.frame_id;
        return 1;
    }
    obj_want_atlas(idx);
    return 0;
}

static int ensure_atlases(void)
{
    return ktx_obj_ready() ? 1 : 0;
}

void vk_obj_pump_uploads(void)
{
    LONGLONG t0;
    double pump_ms;
    double budget_ms;
    int gpu_started = 0;
    int finished = 0;
    int cpu_req = -1;
    int burst_n = 0;
    int loops = 0;

    if (!s.ready || !env_on("CK_GPU_OBJ", 1))
        return;
    t0 = hitch_qpc_now();
    /* Cold start / backlog: chain several upload+load cycles so buildings pop in
     * within ~1 frame instead of ~1 atlas/frame (~400ms for a town). */
    budget_ms = 2.5;
    if (s.want_n >= 3 || s.arr_resident + s.rgba_resident < 12)
        budget_ms = 16.0;
    {
        const char *e = getenv("CK_OBJ_BURST_MS");
        if (e && e[0]) {
            double v = atof(e);
            if (v >= 0.0 && v <= 48.0)
                budget_ms = v;
        }
    }

    while (hitch_qpc_ms_since(t0) < budget_ms && loops < 24) {
        int progress = 0;
        int ready_idx = -1;
        Ktx2Info ready_info;
        uint8_t *ready_blocks = NULL;
        Ktx2Info ready_mat_info;
        uint8_t *ready_mat = NULL;
        double ready_load_ms = 0.0;
        loops++;

        if (s.up_active) {
            double rem = budget_ms - hitch_qpc_ms_since(t0);
            if (rem > 0.2) {
                uint64_t ns = (uint64_t)(rem * 1000000.0);
                if (ns < 200000ull)
                    ns = 200000ull;
                s.fn.vkWaitForFences(s.device, 1, &s.upload_fence, VK_TRUE, ns);
            }
            if (finish_atlas_upload()) {
                finished++;
                burst_n++;
                progress = 1;
            }
        }

        if (!s.up_active && s_ld.thread) {
            EnterCriticalSection(&s_ld.cs);
            if (s_ld.ready_idx >= 0 && s_ld.blocks) {
                ready_idx = s_ld.ready_idx;
                ready_info = s_ld.info;
                ready_blocks = s_ld.blocks;
                ready_load_ms = s_ld.load_ms;
                if (s_ld.have_mat && s_ld.mat_blocks) {
                    ready_mat_info = s_ld.mat_info;
                    ready_mat = s_ld.mat_blocks;
                    s_ld.mat_blocks = NULL;
                    s_ld.have_mat = 0;
                }
                s_ld.ready_idx = -1;
                s_ld.blocks = NULL;
            }
            LeaveCriticalSection(&s_ld.cs);
            if (ready_idx >= 0) {
                int layer = s.atlas_layer[ready_idx];
                int ready_ok = 0;
                if (layer >= 0) {
                    if (s.atlas_rgba[ready_idx])
                        ready_ok = (layer < ARR_RGBA_LAYERS && s.rgba_layer_ready[layer]);
                    else
                        ready_ok = (layer < ARR_LAYERS && s.layer_ready[layer]);
                }
                if (ready_ok) {
                    free(ready_blocks);
                    free(ready_mat);
                } else if (begin_atlas_upload_gpu(ready_idx, &ready_info, ready_blocks,
                                                 ready_mat ? &ready_mat_info : NULL, ready_mat,
                                                 ready_load_ms)) {
                    gpu_started++;
                    progress = 1;
                }
            }
        }

        if (!obj_loader_busy()) {
            while (s.want_n > 0) {
                int idx = want_take_best();
                int layer;
                if (idx < 0)
                    break;
                layer = s.atlas_layer[idx];
                if ((layer >= 0 && ((s.atlas_rgba[idx] && layer < ARR_RGBA_LAYERS &&
                                     s.rgba_layer_ready[layer]) ||
                                    (!s.atlas_rgba[idx] && layer < ARR_LAYERS &&
                                     s.layer_ready[layer]))) ||
                    (s.up_active && s.up_idx == idx))
                    continue;
                if (!vk_obj_atlas_fits_array(idx))
                    continue;
                obj_loader_request(idx);
                cpu_req = idx;
                progress = 1;
                break;
            }
        }

        if (!progress) {
            /* GPU idle, CPU still decoding — brief yield so loader can finish. */
            if (obj_loader_busy() && hitch_qpc_ms_since(t0) + 0.5 < budget_ms)
                Sleep(1);
            else
                break;
        }
    }

    pump_ms = hitch_qpc_ms_since(t0);
    /* #region agent log */
    if (gpu_started || finished || cpu_req >= 0 || pump_ms >= 4.0 || burst_n > 1) {
        char data[240];
        snprintf(data, sizeof(data),
                 "{\"pump_ms\":%.2f,\"gpu\":%d,\"fin\":%d,\"cpu_req\":%d,\"want\":%d,"
                 "\"resident\":%d,\"burst\":%d,\"loops\":%d,\"budget\":%.1f,\"mode\":\"arr\"}",
                 pump_ms, gpu_started, finished, cpu_req, s.want_n, s.arr_resident, burst_n, loops,
                 budget_ms);
        hooks_agent("H-OBJ", "vk_obj.c:pump", "obj-atlas-pump", data);
    }
    /* #endregion */
}

void vk_obj_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
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

int vk_obj_init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                  VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                  PFN_vkGetInstanceProcAddr gipa, VkInstance instance)
{
    VkShaderModuleCreateInfo smci;
    VkDescriptorSetLayoutCreateInfo dlci;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo plci;
    VkAttachmentDescription atts[2];
    VkAttachmentReference atr;
    VkAttachmentReference dtr;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo rpci;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[7];
    VkPipelineVertexInputStateCreateInfo viss;
    VkPipelineInputAssemblyStateCreateInfo iass;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rast;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cbs;
    VkPipelineDynamicStateCreateInfo dyn;
    VkDynamicState dyns[2];
    VkGraphicsPipelineCreateInfo gpci;
    VkSamplerCreateInfo sci;
    VkDescriptorPoolSize psz;
    VkDescriptorPoolCreateInfo dpci;
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cai;
    VkResult r;

    s.wanted = env_on("CK_GPU_OBJ", 1);
    if (!s.wanted) {
        log_msg("vk_obj: disabled (CK_GPU_OBJ=0)");
        return 0;
    }
    if (s.ready)
        return 1;
    if (!device || !phys || !queue || !gpa || !gipa || !instance || !ktx_obj_ready()) {
        log_msg("vk_obj: init skipped (device/ktx_obj)");
        return 0;
    }

    {
        int want = s.wanted;
        memset(&s, 0, sizeof(s));
        s.wanted = want;
    }
    s.device = device;
    s.phys = phys;
    s.queue = queue;
    s.qfam = qfam;
    s.soft_fmt = soft_format;
    s.gpa = gpa;
    s.gipa = gipa;
    s.instance = instance;
    if (!load_fns()) {
        log_msg("vk_obj: missing Vulkan procs");
        return 0;
    }

    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = obj_vert_spv_len;
    smci.pCode = (const uint32_t *)obj_vert_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.vert);
    if (r != VK_SUCCESS)
        return 0;
    smci.codeSize = obj_frag_spv_len;
    smci.pCode = (const uint32_t *)obj_frag_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.frag);
    if (r != VK_SUCCESS)
        return 0;

    {
        VkDescriptorSetLayoutBinding binds[3];
        memset(binds, 0, sizeof(binds));
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1] = binds[0];
        binds[1].binding = 1;
        binds[2] = binds[0];
        binds[2].binding = 2;
        memset(&dlci, 0, sizeof(dlci));
        dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dlci.bindingCount = 3;
        dlci.pBindings = binds;
        r = s.fn.vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &s.dset_layout);
        if (r != VK_SUCCESS)
            return 0;
    }

    memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = 8; /* vec2 screen */
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s.dset_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    r = s.fn.vkCreatePipelineLayout(s.device, &plci, NULL, &s.pipe_layout);
    if (r != VK_SUCCESS)
        return 0;

    memset(atts, 0, sizeof(atts));
    atts[0].format = soft_format;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    atts[1].format = vk_iso_depth_format();
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&atr, 0, sizeof(atr));
    atr.attachment = 0;
    atr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&dtr, 0, sizeof(dtr));
    dtr.attachment = 1;
    dtr.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &atr;
    sub.pDepthStencilAttachment = &dtr;
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    memset(&rpci, 0, sizeof(rpci));
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = atts;
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

    memset(&vb, 0, sizeof(vb));
    vb.binding = 0;
    vb.stride = sizeof(ObjVert);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof(va));
    va[0].location = 0;
    va[0].binding = 0;
    va[0].format = VK_FORMAT_R32G32_SFLOAT;
    va[0].offset = 0;
    va[1].location = 1;
    va[1].binding = 0;
    va[1].format = VK_FORMAT_R32G32_SFLOAT;
    va[1].offset = 8;
    va[2].location = 2;
    va[2].binding = 0;
    va[2].format = VK_FORMAT_R32_SFLOAT;
    va[2].offset = 16; /* layer */
    va[3].location = 3;
    va[3].binding = 0;
    va[3].format = VK_FORMAT_R32_SFLOAT;
    va[3].offset = 20; /* shadow */
    va[4].location = 4;
    va[4].binding = 0;
    va[4].format = VK_FORMAT_R32_SFLOAT;
    va[4].offset = 24; /* pc */
    va[5].location = 5;
    va[5].binding = 0;
    va[5].format = VK_FORMAT_R32G32B32_SFLOAT;
    va[5].offset = 28; /* team rgb */
    va[6].location = 6;
    va[6].binding = 0;
    va[6].format = VK_FORMAT_R32_SFLOAT;
    va[6].offset = 40; /* sort_y */
    memset(&viss, 0, sizeof(viss));
    viss.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viss.vertexBindingDescriptionCount = 1;
    viss.pVertexBindingDescriptions = &vb;
    viss.vertexAttributeDescriptionCount = 7;
    viss.pVertexAttributeDescriptions = va;
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
    memset(&dss, 0, sizeof(dss));
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_GREATER;
    memset(&cba, 0, sizeof(cba));
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cbs, 0, sizeof(cbs));
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    dyns[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dyns[1] = VK_DYNAMIC_STATE_SCISSOR;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyns;

    memset(&gpci, 0, sizeof(gpci));
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &viss;
    gpci.pInputAssemblyState = &iass;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rast;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &dss;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyn;
    gpci.layout = s.pipe_layout;
    gpci.renderPass = s.rp;
    r = s.fn.vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gpci, NULL, &s.pipe);
    if (r != VK_SUCCESS)
        return 0;

    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    r = s.fn.vkCreateSampler(s.device, &sci, NULL, &s.sampler);
    if (r != VK_SUCCESS)
        return 0;

    memset(&psz, 0, sizeof(psz));
    psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz.descriptorCount = 3;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    r = s.fn.vkCreateDescriptorPool(s.device, &dpci, NULL, &s.pool);
    if (r != VK_SUCCESS)
        return 0;

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)VERT_MAX * sizeof(ObjVert);
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
    pci.queueFamilyIndex = qfam;
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
    {
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = s.fn.vkCreateFence(s.device, &fci, NULL, &s.upload_fence);
        if (r != VK_SUCCESS)
            return 0;
    }

    arr_maps_clear();
    if (!ensure_array_resources()) {
        log_msg("vk_obj: texture2DArray init failed");
        return 0;
    }
    /* Layers stream via vk_obj_pump_uploads (before present CB) — no mid-frame WaitIdle. */
    s.ready = 1;
    obj_loader_start();
    log_msg("vk_obj: GPU pipeline ready fmt=%u (texarray+%d async staging)", (unsigned)soft_format,
            ARR_LAYERS);
    return 1;
}

void vk_obj_shutdown(void)
{
    if (!s.device)
        return;
    obj_loader_stop();
    if (s.fn.vkDeviceWaitIdle)
        s.fn.vkDeviceWaitIdle(s.device);
    destroy_atlases();
    if (s.vbo_ptr && s.fn.vkUnmapMemory)
        s.fn.vkUnmapMemory(s.device, s.vbo_mem);
    if (s.vbo && s.fn.vkDestroyBuffer)
        s.fn.vkDestroyBuffer(s.device, s.vbo, NULL);
    if (s.vbo_mem && s.fn.vkFreeMemory)
        s.fn.vkFreeMemory(s.device, s.vbo_mem, NULL);
    if (s.upload_fence && s.fn.vkDestroyFence)
        s.fn.vkDestroyFence(s.device, s.upload_fence, NULL);
    if (s.upload_pool && s.fn.vkDestroyCommandPool)
        s.fn.vkDestroyCommandPool(s.device, s.upload_pool, NULL);
    if (s.pool && s.fn.vkDestroyDescriptorPool)
        s.fn.vkDestroyDescriptorPool(s.device, s.pool, NULL);
    if (s.sampler && s.fn.vkDestroySampler)
        s.fn.vkDestroySampler(s.device, s.sampler, NULL);
    if (s.pipe && s.fn.vkDestroyPipeline)
        s.fn.vkDestroyPipeline(s.device, s.pipe, NULL);
    if (s.rp && s.fn.vkDestroyRenderPass)
        s.fn.vkDestroyRenderPass(s.device, s.rp, NULL);
    if (s.pipe_layout && s.fn.vkDestroyPipelineLayout)
        s.fn.vkDestroyPipelineLayout(s.device, s.pipe_layout, NULL);
    if (s.dset_layout && s.fn.vkDestroyDescriptorSetLayout)
        s.fn.vkDestroyDescriptorSetLayout(s.device, s.dset_layout, NULL);
    if (s.vert && s.fn.vkDestroyShaderModule)
        s.fn.vkDestroyShaderModule(s.device, s.vert, NULL);
    if (s.frag && s.fn.vkDestroyShaderModule)
        s.fn.vkDestroyShaderModule(s.device, s.frag, NULL);
    if (s.soft_fb && s.fn.vkDestroyFramebuffer)
        s.fn.vkDestroyFramebuffer(s.device, s.soft_fb, NULL);
    memset(&s, 0, sizeof(s));
}

int vk_obj_ready(void)
{
    return s.ready;
}

static int quad_cmp(const void *a, const void *b)
{
    float ya = ((const ObjQuad *)a)->sort_y;
    float yb = ((const ObjQuad *)b)->sort_y;
    return (ya > yb) - (ya < yb);
}

/* ENT default_duration ≈ 140 ms; unit walk ≈ 55 ms — use 70 for sheets. */
enum { OBJ_ANIM_MS = 70 };

static int decor_anim_tick(void)
{
    return (int)(GetTickCount() / (DWORD)OBJ_ANIM_MS);
}

static int decor_pingpong_idx(int tick, int n)
{
    int cycle, t;
    if (n <= 1)
        return 0;
    cycle = (n - 1) * 2;
    t = tick % cycle;
    if (t < 0)
        t += cycle;
    if (t >= n)
        t = cycle - t;
    return t;
}

static int decor_reverse_idx(int tick, int n)
{
    int t;
    if (n <= 1)
        return 0;
    t = ((tick % n) + n) % n;
    return (n - 1) - t;
}

/* 1=pingpong 2=reverse 3=loop 4=unit sheet (row×facing) 0=static */
static int obj_layer_anim_mode(const KtxObjSprite *sp)
{
    if (sp->remaping[0]) {
        if (_stricmp(sp->remaping, "pingpong") == 0)
            return 1;
        if (_stricmp(sp->remaping, "reverse") == 0)
            return 2;
        if (_stricmp(sp->remaping, "loop") == 0 || _stricmp(sp->remaping, "forward") == 0)
            return 3;
        if (_stricmp(sp->remaping, "none") != 0)
            return 0;
    }
    /* Vertical strip (1×N): MapObj ENT anims often leave remaping=none. */
    if (sp->frames_x <= 1 && sp->frames_y > 1 && sp->drawmode[0] &&
        (_stricmp(sp->drawmode, "normal") == 0 || _stricmp(sp->drawmode, "clouds") == 0 ||
         _stricmp(sp->drawmode, "player_color") == 0)) {
        if (sp->frames_y >= 5)
            return 3;
    }
    /* Unit Idle/Walk sheets: columns=facing, rows=anim (incl. drawmode=index).
     * Shadow sheets use drawmode=shadow but same row×facing layout. */
    if (sp->frames_x > 1 && sp->frames_y > 1 && sp->drawmode[0] &&
        (_stricmp(sp->drawmode, "player_color") == 0 || _stricmp(sp->drawmode, "normal") == 0 ||
         _stricmp(sp->drawmode, "index") == 0 || _stricmp(sp->drawmode, "shadow") == 0))
        return 4;
    return 0;
}

typedef struct {
    char id[32];
    char layer[48];
    int nfr;
    int mode;
    int frames_x;
    int frames_y;
} ObjLayerAnim;

/* Persistent layer-anim table: rebuilt only when sprite catalog size changes.
 * Per-frame rebuild of 34k sprites was ~350ms (3 FPS). */
#define OBJ_LAYER_MAX 4096
#define OBJ_LAYER_HASH 4096
static ObjLayerAnim g_obj_layer_cache[OBJ_LAYER_MAX];
static int g_obj_layer_n;
static int g_obj_layer_spr_n = -1;
static int g_obj_layer_hash[OBJ_LAYER_HASH];
static int g_obj_layer_next[OBJ_LAYER_MAX];

static unsigned obj_layer_key_hash(const char *id, const char *layer)
{
    unsigned h = 5381u;
    const char *p;
    for (p = id; p && *p; ++p)
        h = ((h << 5) + h) + (unsigned)(unsigned char)tolower(*p);
    h = ((h << 5) + h) + (unsigned)':';
    for (p = layer; p && *p; ++p)
        h = ((h << 5) + h) + (unsigned)(unsigned char)tolower(*p);
    return h;
}

static int obj_layer_cache_find(const char *id, const char *layer)
{
    unsigned h = obj_layer_key_hash(id, layer) & (OBJ_LAYER_HASH - 1);
    int i;
    for (i = g_obj_layer_hash[h]; i >= 0; i = g_obj_layer_next[i]) {
        if (_stricmp(g_obj_layer_cache[i].id, id) == 0 &&
            _stricmp(g_obj_layer_cache[i].layer, layer) == 0)
            return i;
    }
    return -1;
}

static int obj_ensure_layer_cache(const KtxObjSprite *sprs, int spr_n)
{
    int i, n;
    if (g_obj_layer_spr_n == spr_n && g_obj_layer_n > 0)
        return 0;
    for (i = 0; i < OBJ_LAYER_HASH; ++i)
        g_obj_layer_hash[i] = -1;
    n = 0;
    for (i = 0; i < spr_n && n < OBJ_LAYER_MAX; ++i) {
        int idx = obj_layer_cache_find(sprs[i].id, sprs[i].layer);
        int mode = obj_layer_anim_mode(&sprs[i]);
        if (idx < 0) {
            unsigned h = obj_layer_key_hash(sprs[i].id, sprs[i].layer) & (OBJ_LAYER_HASH - 1);
            lstrcpynA(g_obj_layer_cache[n].id, sprs[i].id, (int)sizeof(g_obj_layer_cache[n].id));
            lstrcpynA(g_obj_layer_cache[n].layer, sprs[i].layer,
                      (int)sizeof(g_obj_layer_cache[n].layer));
            g_obj_layer_cache[n].nfr = sprs[i].frame + 1;
            g_obj_layer_cache[n].mode = mode;
            g_obj_layer_cache[n].frames_x = sprs[i].frames_x > 0 ? sprs[i].frames_x : 1;
            g_obj_layer_cache[n].frames_y = sprs[i].frames_y > 0 ? sprs[i].frames_y : 1;
            g_obj_layer_next[n] = g_obj_layer_hash[h];
            g_obj_layer_hash[h] = n;
            n++;
        } else {
            if (sprs[i].frame + 1 > g_obj_layer_cache[idx].nfr)
                g_obj_layer_cache[idx].nfr = sprs[i].frame + 1;
            if (mode > g_obj_layer_cache[idx].mode)
                g_obj_layer_cache[idx].mode = mode;
            if (sprs[i].frames_x > g_obj_layer_cache[idx].frames_x)
                g_obj_layer_cache[idx].frames_x = sprs[i].frames_x;
            if (sprs[i].frames_y > g_obj_layer_cache[idx].frames_y)
                g_obj_layer_cache[idx].frames_y = sprs[i].frames_y;
        }
    }
    g_obj_layer_n = n;
    g_obj_layer_spr_n = spr_n;
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "vk_obj: layer_cache n=%d spr=%d (once)", n, spr_n);
        log_msg(msg);
    }
    return 1;
}

static int obj_wanted_frame_cached(const char *id, const char *layer, int tick, int inst_frame,
                                   int facing)
{
    int idx = obj_layer_cache_find(id, layer);
    int mode, nfr, fx, fy, row, col;
    if (idx < 0)
        return inst_frame;
    mode = g_obj_layer_cache[idx].mode;
    nfr = g_obj_layer_cache[idx].nfr;
    fx = g_obj_layer_cache[idx].frames_x;
    fy = g_obj_layer_cache[idx].frames_y;
    if (mode == 0 || nfr <= 1)
        return inst_frame;
    if (mode == 4) {
        if (fx < 1)
            fx = 1;
        if (fy < 1)
            fy = 1;
        col = facing % fx;
        if (col < 0)
            col += fx;
        row = tick % fy;
        if (row < 0)
            row += fy;
        return row * fx + col;
    }
    if (mode == 2)
        return decor_reverse_idx(tick, nfr);
    if (mode == 3) {
        int t = tick % nfr;
        if (t < 0)
            t += nfr;
        return t;
    }
    return decor_pingpong_idx(tick, nfr);
}

static int obj_layer_is_walk(const char *layer)
{
    return layer && (_stricmp(layer, "walk") == 0 || _stricmp(layer, "walk_shadow") == 0);
}

static int obj_layer_is_idle_body(const char *layer)
{
    return layer && (_stricmp(layer, "unit") == 0 || _stricmp(layer, "shadow") == 0);
}

/* MapObj+0x4e is ENT anim_idx-1 (Walk=0, Idle=12, Nosi=16, …). Layer names: walk, unit, anim16_nosi. */
static int obj_layer_clip_id(const char *layer)
{
    int n;
    if (!layer || !layer[0])
        return 12;
    if (obj_layer_is_walk(layer))
        return 0;
    if ((layer[0] == 'a' || layer[0] == 'A') && (layer[1] == 'n' || layer[1] == 'N') &&
        (layer[2] == 'i' || layer[2] == 'I') && (layer[3] == 'm' || layer[3] == 'M') &&
        layer[4] >= '0' && layer[4] <= '9') {
        n = 0;
        layer += 4;
        while (*layer >= '0' && *layer <= '9') {
            n = n * 10 + (*layer - '0');
            layer++;
        }
        if (n >= 0 && n < 64)
            return n;
    }
    return 12;
}

/* ---- P0: per-id sprite templates (no _stricmp / hash in the draw loop) ---- */
enum {
    TPL_WALK = 1,
    TPL_SHADOW = 2,
    TPL_PC = 4,
    OBJ_TPL_MAX = 65536,
    OBJ_ID_PACK_MAX = 4096,
    OBJ_ID_HASH = 4096
};

typedef struct {
    int16_t frame;
    int16_t atlas_index;
    int16_t w, h;
    int16_t ox, oy; /* offset_x+hot_x, offset_y+hot_y */
    int16_t sort_oy;
    int16_t z;
    int16_t nfr;
    int16_t frames_x, frames_y;
    uint8_t flags;
    uint8_t mode;
    uint8_t anim_id; /* MapObj+0x4e / ENT anim_idx-1 */
    float u0, v0, u1, v1;
} ObjTpl;

typedef struct {
    int begin;
    int count;
    uint8_t has_walk;
} ObjIdPack;

static ObjTpl g_tpl[OBJ_TPL_MAX];
static int g_tpl_n;
static ObjIdPack g_id_pack[OBJ_ID_PACK_MAX];
static char g_id_pack_key[OBJ_ID_PACK_MAX][32];
static int g_id_pack_n;
static int g_id_pack_hash[OBJ_ID_HASH];
static int g_id_pack_next[OBJ_ID_PACK_MAX];
static int g_tpl_spr_n = -1;

static unsigned obj_id_key_hash(const char *id)
{
    unsigned h = 5381u;
    const char *p;
    for (p = id; p && *p; ++p)
        h = ((h << 5) + h) + (unsigned)(unsigned char)tolower(*p);
    return h;
}

static int obj_tpl_wanted_frame(const ObjTpl *t, int tick, int inst_frame, int facing)
{
    int mode, nfr, fx, fy, row, col;
    if (!t)
        return inst_frame;
    mode = (int)t->mode;
    nfr = (int)t->nfr;
    fx = (int)t->frames_x;
    fy = (int)t->frames_y;
    if (mode == 0 || nfr <= 1)
        return inst_frame;
    if (mode == 4) {
        if (fx < 1)
            fx = 1;
        if (fy < 1)
            fy = 1;
        col = facing % fx;
        if (col < 0)
            col += fx;
        row = tick % fy;
        if (row < 0)
            row += fy;
        return row * fx + col;
    }
    if (mode == 2)
        return decor_reverse_idx(tick, nfr);
    if (mode == 3) {
        int tt = tick % nfr;
        if (tt < 0)
            tt += nfr;
        return tt;
    }
    return decor_pingpong_idx(tick, nfr);
}

static int obj_ensure_tpl_cache(const KtxObjSprite *sprs, int spr_n)
{
    int i, si, n_pack, n_tpl;
    const KtxObjAtlas *atls;
    int atl_n;

    if (g_tpl_spr_n == spr_n && g_tpl_n > 0 && g_id_pack_n > 0)
        return 0;
    if (!sprs || spr_n < 1)
        return 0;

    obj_ensure_layer_cache(sprs, spr_n);
    atls = ktx_obj_atlases();
    atl_n = ktx_obj_atlas_count();

    for (i = 0; i < OBJ_ID_HASH; ++i)
        g_id_pack_hash[i] = -1;
    n_pack = 0;
    n_tpl = 0;

    /* One pack per unique id from sprite chain heads. */
    for (i = 0; i < spr_n && n_pack < OBJ_ID_PACK_MAX && n_tpl < OBJ_TPL_MAX; ++i) {
        const char *id = sprs[i].id;
        unsigned h;
        int slot, found, begin, has_walk;

        if (!id || !id[0])
            continue;
        h = obj_id_key_hash(id) & (OBJ_ID_HASH - 1);
        found = 0;
        for (slot = g_id_pack_hash[h]; slot >= 0; slot = g_id_pack_next[slot]) {
            if (_stricmp(g_id_pack_key[slot], id) == 0) {
                found = 1;
                break;
            }
        }
        if (found)
            continue;

        begin = n_tpl;
        has_walk = 0;
        for (si = ktx_obj_spr_head(id); si >= 0 && n_tpl < OBJ_TPL_MAX; si = ktx_obj_spr_next(si)) {
            const KtxObjSprite *sp = &sprs[si];
            ObjTpl *t = &g_tpl[n_tpl];
            int ai = sp->atlas_index;
            float aw, ah;
            int is_rgba = 0;
            int mode;
            uint8_t flags = 0;

            if (sp->w < 1 || sp->h < 1)
                continue;
            /* Texarray is fixed ARR_* — UVs must be in array space, not catalog atlas size
             * (P0 regression: /atlas_w stretched/wrong crops on sub-2048 atlases). */
            if (ai >= 0 && ai < atl_n)
                is_rgba = atls[ai].is_rgba;
            aw = is_rgba ? (float)ARR_RGBA_W : (float)ARR_W;
            ah = is_rgba ? (float)ARR_RGBA_H : (float)ARR_H;
            if (obj_layer_is_walk(sp->layer)) {
                flags |= TPL_WALK;
                has_walk = 1;
            }
            if (ktx_obj_sprite_is_shadow(sp))
                flags |= TPL_SHADOW;
            if (ktx_obj_sprite_wants_pc(sp))
                flags |= TPL_PC;

            mode = obj_layer_anim_mode(sp);
            /* Prefer layer-cache nfr/mode when available (aggregated). */
            {
                int li = obj_layer_cache_find(sp->id, sp->layer);
                if (li >= 0) {
                    mode = g_obj_layer_cache[li].mode;
                    t->nfr = (int16_t)g_obj_layer_cache[li].nfr;
                    t->frames_x = (int16_t)g_obj_layer_cache[li].frames_x;
                    t->frames_y = (int16_t)g_obj_layer_cache[li].frames_y;
                } else {
                    t->nfr = (int16_t)(sp->frame + 1);
                    t->frames_x = (int16_t)(sp->frames_x > 0 ? sp->frames_x : 1);
                    t->frames_y = (int16_t)(sp->frames_y > 0 ? sp->frames_y : 1);
                }
            }

            t->frame = (int16_t)sp->frame;
            t->atlas_index = (int16_t)ai;
            t->w = (int16_t)sp->w;
            t->h = (int16_t)sp->h;
            t->ox = (int16_t)(sp->offset_x + sp->hot_x);
            t->oy = (int16_t)(sp->offset_y + sp->hot_y);
            t->sort_oy = (int16_t)sp->sort_oy;
            t->z = (int16_t)sp->z;
            t->flags = flags;
            t->mode = (uint8_t)mode;
            t->anim_id = (uint8_t)obj_layer_clip_id(sp->layer);
            t->u0 = (float)sp->atlas_x / aw;
            t->v0 = (float)sp->atlas_y / ah;
            t->u1 = (float)(sp->atlas_x + sp->w) / aw;
            t->v1 = (float)(sp->atlas_y + sp->h) / ah;
            n_tpl++;
        }

        if (n_tpl <= begin)
            continue;

        lstrcpynA(g_id_pack_key[n_pack], id, (int)sizeof(g_id_pack_key[n_pack]));
        g_id_pack[n_pack].begin = begin;
        g_id_pack[n_pack].count = n_tpl - begin;
        g_id_pack[n_pack].has_walk = (uint8_t)has_walk;
        g_id_pack_next[n_pack] = g_id_pack_hash[h];
        g_id_pack_hash[h] = n_pack;
        n_pack++;
    }

    g_tpl_n = n_tpl;
    g_id_pack_n = n_pack;
    g_tpl_spr_n = spr_n;
    {
        char msg[112];
        snprintf(msg, sizeof(msg), "vk_obj: tpl_cache packs=%d tpls=%d spr=%d (once)", n_pack, n_tpl,
                 spr_n);
        log_msg(msg);
    }
    /* #region agent log */
    {
        char data[192];
        float sample_u1 = 0.f;
        int sample_aw = 0, sample_arr = ARR_W;
        if (n_tpl > 0) {
            sample_u1 = g_tpl[0].u1;
            if (g_tpl[0].atlas_index >= 0 && g_tpl[0].atlas_index < atl_n) {
                sample_aw = (int)atls[g_tpl[0].atlas_index].width;
                sample_arr = atls[g_tpl[0].atlas_index].is_rgba ? ARR_RGBA_W : ARR_W;
            }
        }
        snprintf(data, sizeof(data),
                 "{\"packs\":%d,\"tpls\":%d,\"spr\":%d,\"uv\":\"arr\","
                 "\"sample_u1\":%.5f,\"atl_w\":%d,\"arr_w\":%d}",
                 n_pack, n_tpl, spr_n, sample_u1, sample_aw, sample_arr);
        hooks_agent("H-A", "vk_obj.c:tpl", "obj-tpl-ready", data);
    }
    /* #endregion */
    return 1;
}

static int obj_tpl_bind_item(ObjSpawnItem *it)
{
    unsigned h;
    int slot;
    char key[48];
    const char *id;
    if (!it || !it->id[0])
        return 0;
    if (it->tpl_bound)
        return it->tpl_count > 0;
    id = it->id;
    if (ktx_obj_resolve_id(it->id, key, sizeof(key)))
        id = key;
    h = obj_id_key_hash(id) & (OBJ_ID_HASH - 1);
    for (slot = g_id_pack_hash[h]; slot >= 0; slot = g_id_pack_next[slot]) {
        if (_stricmp(g_id_pack_key[slot], id) == 0) {
            it->tpl_begin = g_id_pack[slot].begin;
            it->tpl_count = g_id_pack[slot].count;
            it->has_walk = g_id_pack[slot].has_walk;
            it->tpl_bound = 1;
            return it->tpl_count > 0;
        }
    }
    it->tpl_bound = 1;
    it->tpl_begin = 0;
    it->tpl_count = 0;
    it->has_walk = 0;
    return 0;
}

static int ensure_ready(void)
{
    if (s.ready)
        return 1;
    if (!s_defer.have || !ktx_obj_wanted() || !ktx_obj_ready())
        return 0;
    return vk_obj_init(s_defer.device, s_defer.phys, s_defer.queue, s_defer.qfam, s_defer.soft_fmt,
                         s_defer.gpa, s_defer.gipa, s_defer.instance);
}

int vk_obj_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view,
                  VkImageView depth_view, int soft_w, int soft_h)
{
    void *terr;
    LONG L, T, R, B;
    int play_t = 80;
    int play_b;
    int cam_h;
    int soft_cam;
    int n_inst, i, qi, nvert;
    ObjQuad *quads;
    ObjVert *verts;
    float pc_screen[2];
    VkViewport vp;
    VkRect2D sc;
    VkFramebufferCreateInfo fci;
    VkRenderPassBeginInfo rpbi;
    VkDeviceSize off = 0;
    LONG n;
    int drawn = 0;
    ObjSpawnItem *items;
    const KtxObjSprite *sprs;
    int spr_n;
    static int s_env_obj = -1, s_env_pc = -1;
    static ObjQuad *s_quads;
    static int s_quads_cap;
    static DWORD s_now_mv;
    LONGLONG t_build0, t_sort0, t_vert0, t_fb0;
    double ms_lc = 0, ms_build = 0, ms_sort = 0, ms_vert = 0, ms_fb = 0;
    int vis_inst_log = 0;

    (void)soft_img;
    if (s_env_obj < 0) {
        s_env_obj = env_on("CK_GPU_OBJ", 1);
        s_env_pc = env_on("CK_GPU_PC", 1);
    }
    if (!s_env_obj)
        return 0;
    if (!ensure_ready() || !cmd || !soft_view || soft_w < 64 || soft_h < 64)
        return -1;
    if (!depth_view) {
        static int s_no_depth;
        if (!s_no_depth) {
            s_no_depth = 1;
            log_msg("vk_obj: no depth view, skipping GPU draw");
        }
        return 0;
    }
    if (!ensure_atlases())
        return -1;
    s.frame_id++;
    if (s.frame_id == 0)
        s.frame_id = 1;

    terr = terrain_obj();
    if (!terr)
        return -1;
    mm_read_cam(&L, &T, &R, &B);
    if (R <= L || B <= T)
        return -1;
    soft_cam = (L <= 64 && T <= 128 && R >= (LONG)soft_w - 64 && (R - L) >= (LONG)soft_w / 2);
    cam_h = (int)(B - T);
    if (cam_h < 64)
        cam_h = soft_h - play_t - 54;
    play_b = play_t + cam_h;
    if (play_b > soft_h - 8)
        play_b = soft_h - 8;
    if (play_b <= play_t + 32)
        play_b = soft_h - 8;

    n_inst = obj_spawn_count();
    items = obj_spawn_items_mut();
    sprs = ktx_obj_sprites();
    spr_n = ktx_obj_sprite_count();
    if (n_inst < 1 || spr_n < 1 || !items || !sprs)
        return 0;

    {
        LONGLONG t0 = hitch_qpc_now();
        int rebuilt = obj_ensure_tpl_cache(sprs, spr_n);
        ms_lc = hitch_qpc_ms_since(t0);
        if (rebuilt) {
            char data[96];
            snprintf(data, sizeof(data), "{\"ms\":%.3f,\"tpls\":%d,\"packs\":%d}", ms_lc, g_tpl_n,
                     g_id_pack_n);
            hooks_agent("H-A", "vk_obj.c:record", "obj-tpl-ready", data);
        }
    }

    if (s_quads_cap < DRAW_MAX) {
        ObjQuad *nq = (ObjQuad *)realloc(s_quads, sizeof(ObjQuad) * (size_t)DRAW_MAX);
        if (!nq)
            return -1;
        s_quads = nq;
        s_quads_cap = DRAW_MAX;
    }
    quads = s_quads;
    qi = 0;
    s_now_mv = GetTickCount();

    t_build0 = hitch_qpc_now();
    {
        int anim_tick = decor_anim_tick();
        int atl_cap = ktx_obj_atlas_count();
        int miss_atl = 0, vis_inst = 0;
        int bind_ok = 0, bind_miss = 0;
        int pc_env = s_env_pc;

        for (i = 0; i < n_inst && qi < DRAW_MAX; ++i) {
            ObjSpawnItem *it = &items[i];
            const char *oid = it->id;
            LONG wx_log = it->x;
            LONG wy_log = it->y;
            float vis_x, vis_y;
            LONG wx, wy;
            float h, esx, esy;
            int tick;
            int ifr = it->frame;
            int facing = it->facing;
            int moving = it->moving;
            int use_walk = 0;
            int clip = 12;
            int player = it->player;
            float tr = 1.f, tg = 23.f / 255.f, tb = 23.f / 255.f;
            int ti, tend;
            DWORD move_age, move_dur;

            if (!oid || !oid[0])
                continue;
            /* GPU draw uses visual lerp between discrete SetPos snaps (exe path ~32wu). */
            obj_spawn_vis_xy(it, s_now_mv, &vis_x, &vis_y);
            wx = (LONG)(vis_x + (vis_x >= 0.f ? 0.5f : -0.5f));
            wy = (LONG)(vis_y + (vis_y >= 0.f ? 0.5f : -0.5f));
            /* #region agent log */
            {
                float gap_x = vis_x - (float)wx_log;
                float gap_y = vis_y - (float)wy_log;
                if (it->moving &&
                    (gap_x > 0.5f || gap_x < -0.5f || gap_y > 0.5f || gap_y < -0.5f)) {
                    static unsigned s_gap_n;
                    if (s_gap_n < 40u) {
                        FILE *f = fopen(
                            "/home/cybernetik/Games/Imperivm/ck_asi/.cursor/debug-764ba7.log", "a");
                        if (f) {
                            fprintf(f,
                                "{\"sessionId\":\"764ba7\",\"runId\":\"post-fix\",\"hypothesisId\":\"H-D4\","
                                "\"location\":\"vk_obj.c:record\",\"message\":\"draw_using_vis\","
                                "\"data\":{\"id\":\"%.24s\",\"logical\":[%ld,%ld],\"draw\":[%.1f,%.1f],"
                                "\"gap_log\":[%.1f,%.1f],\"age_ms\":%lu,\"dur_ms\":%lu},\"timestamp\":%lu}\n",
                                oid, (long)wx_log, (long)wy_log, (double)vis_x, (double)vis_y,
                                (double)gap_x, (double)gap_y,
                                (unsigned long)(s_now_mv - it->last_move_tick),
                                (unsigned long)it->move_dur_ms,
                                (unsigned long)s_now_mv);
                            fclose(f);
                            ++s_gap_n;
                        }
                    }
                }
            }
            /* #endregion */
            if (wx < L - 400 || wx > R + 400)
                continue;

            /* Fire/smoke *Anim overlays ship player=0; inherit from body nearby. */
            if (player <= 0) {
                size_t olen = strlen(oid);
                if (olen > 4 && _stricmp(oid + (olen - 4), "Anim") == 0) {
                    int j, best_d = 64 * 64 + 1, best_pl = 0;
                    for (j = 0; j < n_inst; ++j) {
                        ObjSpawnItem *body = &items[j];
                        size_t blen;
                        LONG dx, dy;
                        int d2;
                        if (j == i || body->player <= 0 || !body->id[0])
                            continue;
                        blen = strlen(body->id);
                        if (blen + 4 != olen || _strnicmp(body->id, oid, blen) != 0)
                            continue;
                        dx = body->x - wx_log;
                        dy = body->y - wy_log;
                        d2 = (int)(dx * dx + dy * dy);
                        if (d2 < best_d) {
                            best_d = d2;
                            best_pl = body->player;
                        }
                    }
                    if (best_pl > 0)
                        player = best_pl;
                }
            }

            if (!it->tpl_bound)
                obj_tpl_bind_item(it);
            if (it->tpl_count < 1) {
                bind_miss++;
                continue;
            }
            bind_ok++;

            if (!it->h_valid || it->h_x != wx || it->h_y != wy) {
                h = soft_cam ? (float)cell_h(terr, wx, wy) : exact_h(terr, wx, wy);
                it->ground_h = h;
                it->h_x = wx;
                it->h_y = wy;
                it->h_valid = 1;
            } else {
                h = it->ground_h;
            }

            esx = vis_x - (float)L;
            esy = map_to_view_yf_f(vis_y, h) - (float)T + (float)play_t;
            if (esy < (float)(play_t - 320) || esy > (float)(play_b + 320))
                continue;

            vis_inst++;
            vk_obj_request_id(oid);

            move_age = it->last_move_tick ? (s_now_mv - it->last_move_tick) : 0;
            move_dur = it->move_dur_ms;
            /* Retail: walk clip = lerp window (Visible+0x8c). After that, idle until next SetPos. */
            if (moving && (!move_dur || move_age >= move_dur))
                moving = 0;
            if (player > 0)
                obj_player_rgb(player, &tr, &tg, &tb);
            clip = 12;
            if (it->map_obj && !IsBadReadPtr(it->map_obj, 0x52)) {
                int a = *(int *)((BYTE *)it->map_obj + 0x4e);
                if (a >= 0 && a < 64)
                    clip = a;
            } else if (moving)
                clip = 0;
            {
                int have = 0, ti2, tend2;
                tend2 = it->tpl_begin + it->tpl_count;
                for (ti2 = it->tpl_begin; ti2 < tend2; ++ti2) {
                    if ((int)g_tpl[ti2].anim_id == clip && !(g_tpl[ti2].flags & TPL_SHADOW)) {
                        have = 1;
                        break;
                    }
                }
                if (!have)
                    clip = (moving && it->has_walk) ? 0 : 12;
            }
            use_walk = (clip == 0);
            if (moving && clip != 12) {
                int fy = 1, ti2, tend2;
                tend2 = it->tpl_begin + it->tpl_count;
                for (ti2 = it->tpl_begin; ti2 < tend2; ++ti2) {
                    if ((int)g_tpl[ti2].anim_id == clip && !(g_tpl[ti2].flags & TPL_SHADOW) &&
                        g_tpl[ti2].frames_y > 0) {
                        fy = (int)g_tpl[ti2].frames_y;
                        break;
                    }
                }
                if (fy < 1)
                    fy = 1;
                tick = (int)((move_age * (DWORD)fy) / (move_dur ? move_dur : 1u));
                if (tick >= fy)
                    tick = fy - 1;
            } else if (it->has_walk)
                tick = anim_tick + (int)(((ULONG_PTR)it->map_obj >> 4) & 7u);
            else
                tick = anim_tick + (int)(((unsigned)wx * 17u + (unsigned)wy * 31u) & 63u);

            /* #region agent log */
            if (oid && (strstr(oid, "Peasant") || strstr(oid, "Villager") ||
                        strstr(oid, "Citizen"))) {
                static DWORD s_draw_tick;
                static int s_draw_n;
                DWORD nowd = s_now_mv;
                if (nowd - s_draw_tick > 1000u) {
                    s_draw_tick = nowd;
                    s_draw_n = 0;
                }
                if (s_draw_n < 16) {
                    char data[420];
                    int want0 = 0;
                    DWORD age = move_age;
                    int vis90 = -1, vis94 = -1, vis98 = -1;
                    int mo4e = 0, mo62 = 0, mo66 = 0, mo6a = 0;
                    if (it->tpl_count > 0) {
                        const ObjTpl *t0 = &g_tpl[it->tpl_begin];
                        int ti2;
                        for (ti2 = it->tpl_begin; ti2 < it->tpl_begin + it->tpl_count; ++ti2) {
                            if ((int)g_tpl[ti2].anim_id == clip && !(g_tpl[ti2].flags & TPL_SHADOW)) {
                                t0 = &g_tpl[ti2];
                                break;
                            }
                        }
                        want0 = obj_tpl_wanted_frame(t0, tick, ifr, facing);
                    }
                    if (it->map_obj && !IsBadReadPtr(it->map_obj, 0x70)) {
                        void *vis = *(void **)((BYTE *)it->map_obj + 0x3e);
                        mo4e = *(int *)((BYTE *)it->map_obj + 0x4e);
                        mo62 = *(int *)((BYTE *)it->map_obj + 0x62);
                        mo66 = *(int *)((BYTE *)it->map_obj + 0x66);
                        mo6a = *(int *)((BYTE *)it->map_obj + 0x6a);
                        if (vis && !IsBadReadPtr(vis, 0xa0)) {
                            vis90 = *(int *)((BYTE *)vis + 0x90);
                            vis94 = *(int *)((BYTE *)vis + 0x94);
                            vis98 = *(int *)((BYTE *)vis + 0x98);
                        }
                    }
                    snprintf(data, sizeof(data),
                             "{\"id\":\"%s\",\"face\":%d,\"mov\":%d,\"use_walk\":%d,\"has_walk\":%d,"
                             "\"clip\":%d,\"want\":%d,\"age_ms\":%lu,\"dur_ms\":%lu,\"tick\":%d,\"xy\":[%ld,%ld],"
                             "\"vis90\":%d,\"vis94\":%d,\"vis98\":%d,"
                             "\"mo4e\":%d,\"mo62\":%d,\"mo66\":%d,\"mo6a\":%d}",
                             oid, facing, moving, use_walk, (int)it->has_walk, clip, want0,
                             (unsigned long)age, (unsigned long)move_dur, tick, (long)wx, (long)wy,
                             vis90, vis94, vis98, mo4e, mo62, mo66, mo6a);
                    hooks_agent("H-C2", "vk_obj.c:record", "unit-civ", data);
                    s_draw_n++;
                }
            }
            /* #endregion */

            tend = it->tpl_begin + it->tpl_count;
            for (ti = it->tpl_begin; ti < tend && qi < DRAW_MAX; ++ti) {
                const ObjTpl *t = &g_tpl[ti];
                float dx, dy;
                int ai = (int)t->atlas_index;
                int is_shadow = (t->flags & TPL_SHADOW) != 0;
                int want_fr;
                int want_pc;
                int layer;

                if ((int)t->anim_id != clip)
                    continue;
                want_fr = obj_tpl_wanted_frame(t, tick, ifr, facing);
                if ((int)t->frame != want_fr)
                    continue;
                if (ai < 0 || ai >= atl_cap || !ensure_atlas(ai)) {
                    if (ai >= 0 && ai < atl_cap && vk_obj_atlas_fits_array(ai))
                        miss_atl++;
                    continue;
                }
                layer = s.atlas_layer[ai];
                if (s.atlas_rgba[ai])
                    layer += LAYER_RGBA_BASE;
                want_pc = (!is_shadow && player > 0 && (t->flags & TPL_PC) && pc_env) ? 1 : 0;
                dx = esx + (float)t->ox;
                dy = esy + (float)t->oy;
                if (dx + (float)t->w < -8.f || dx >= (float)soft_w + 8.f)
                    continue;
                if (dy + (float)t->h < (float)play_t || dy >= (float)play_b)
                    continue;
                quads[qi].sx0 = dx;
                quads[qi].sy0 = dy;
                quads[qi].sx1 = dx + (float)t->w;
                quads[qi].sy1 = dy + (float)t->h;
                quads[qi].u0 = t->u0;
                quads[qi].v0 = t->v0;
                quads[qi].u1 = t->u1;
                quads[qi].v1 = t->v1;
                quads[qi].sort_y = esy + (float)t->sort_oy + (float)t->z * 0.001f;
                if (is_shadow)
                    quads[qi].sort_y -= 1.0f;
                quads[qi].layer = layer;
                quads[qi].shadow = is_shadow;
                quads[qi].pc = want_pc;
                quads[qi].tr = tr;
                quads[qi].tg = tg;
                quads[qi].tb = tb;
                qi++;
            }
        }
        vis_inst_log = vis_inst;
        if (miss_atl > 0 || vis_inst > 0 || bind_miss > 0) {
            static DWORD s_last_m;
            static DWORD s_miss_t0;
            DWORD nowm = s_now_mv;
            DWORD gap = (miss_atl > 0) ? 200u : 1000u;
            if (miss_atl > 0 && s_miss_t0 == 0)
                s_miss_t0 = nowm;
            if (miss_atl == 0)
                s_miss_t0 = 0;
            if (nowm - s_last_m > gap) {
                char data[240];
                s_last_m = nowm;
                snprintf(data, sizeof(data),
                         "{\"vis_inst\":%d,\"quads\":%d,\"miss_atl\":%d,\"resident\":%d,"
                         "\"rgba_res\":%d,\"want\":%d,\"layers\":%d,\"bind_ok\":%d,\"bind_miss\":%d,"
                         "\"miss_age_ms\":%lu,\"hyp\":\"A-serial-stream\"}",
                         vis_inst, qi, miss_atl, s.arr_resident, s.rgba_resident, s.want_n,
                         ARR_LAYERS, bind_ok, bind_miss,
                         (unsigned long)(s_miss_t0 ? (nowm - s_miss_t0) : 0ul));
                hooks_agent("H-B", "vk_obj.c:record", "obj-miss", data);
            }
        }
    }
    ms_build = hitch_qpc_ms_since(t_build0);

    if (qi < 1)
        return 0;

    t_sort0 = hitch_qpc_now();
    qsort(quads, (size_t)qi, sizeof(ObjQuad), quad_cmp);
    ms_sort = hitch_qpc_ms_since(t_sort0);

    t_vert0 = hitch_qpc_now();
    verts = (ObjVert *)s.vbo_ptr;
    nvert = 0;
    for (i = 0; i < qi && nvert + 6 <= VERT_MAX; ++i) {
        ObjQuad *q = &quads[i];
        float lyr = (float)q->layer;
        float sh = q->shadow ? 1.f : 0.f;
        float pc = q->pc ? 1.f : 0.f;
        ObjVert tri[6] = {
            {q->sx0, q->sy0, q->u0, q->v0, lyr, sh, pc, q->tr, q->tg, q->tb, q->sort_y},
            {q->sx1, q->sy0, q->u1, q->v0, lyr, sh, pc, q->tr, q->tg, q->tb, q->sort_y},
            {q->sx1, q->sy1, q->u1, q->v1, lyr, sh, pc, q->tr, q->tg, q->tb, q->sort_y},
            {q->sx0, q->sy0, q->u0, q->v0, lyr, sh, pc, q->tr, q->tg, q->tb, q->sort_y},
            {q->sx1, q->sy1, q->u1, q->v1, lyr, sh, pc, q->tr, q->tg, q->tb, q->sort_y},
            {q->sx0, q->sy1, q->u0, q->v1, lyr, sh, pc, q->tr, q->tg, q->tb, q->sort_y},
        };
        memcpy(verts + nvert, tri, sizeof(tri));
        nvert += 6;
        drawn++;
    }
    ms_vert = hitch_qpc_ms_since(t_vert0);

    t_fb0 = hitch_qpc_now();
    {
        float obj_sy_min = FLT_MAX;
        int ci;
        for (ci = 0; ci < qi; ++ci) {
            if (quads[ci].sort_y < obj_sy_min)
                obj_sy_min = quads[ci].sort_y;
        }
        /* #region agent log */
        if (obj_sy_min < FLT_MAX && g_vk_last_decor_sy_max > obj_sy_min) {
            static DWORD s_po_last;
            DWORD now_po = s_now_mv;
            if (now_po - s_po_last > 1000u) {
                char data[128];
                s_po_last = now_po;
                snprintf(data, sizeof(data),
                         "{\"hyp\":\"A-pass-order\",\"decor_sy_max\":%.1f,\"obj_sy_min\":%.1f,"
                         "\"depth\":1}",
                         g_vk_last_decor_sy_max, obj_sy_min);
                hooks_agent("A-DEPTH", "vk_obj.c:record", "pass-order", data);
            }
        }
        /* #endregion */
    }
    if (!s.soft_fb || s.soft_fb_view != soft_view || s.soft_fb_depth != depth_view ||
        s.soft_fb_w != soft_w || s.soft_fb_h != soft_h) {
        if (s.soft_fb)
            s.fn.vkDestroyFramebuffer(s.device, s.soft_fb, NULL);
        s.soft_fb = VK_NULL_HANDLE;
        {
            VkImageView fb_atts[2];
            fb_atts[0] = soft_view;
            fb_atts[1] = depth_view;
            memset(&fci, 0, sizeof(fci));
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = s.rp;
            fci.attachmentCount = 2;
            fci.pAttachments = fb_atts;
            fci.width = (uint32_t)soft_w;
            fci.height = (uint32_t)soft_h;
            fci.layers = 1;
            if (s.fn.vkCreateFramebuffer(s.device, &fci, NULL, &s.soft_fb) != VK_SUCCESS)
                return -1;
        }
        s.soft_fb_view = soft_view;
        s.soft_fb_depth = depth_view;
        s.soft_fb_w = soft_w;
        s.soft_fb_h = soft_h;
    }

    memset(&rpbi, 0, sizeof(rpbi));
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = s.rp;
    rpbi.framebuffer = s.soft_fb;
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
    sc.extent.height = (uint32_t)(play_b - (int)sc.offset.y);
    s.fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    s.fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    s.fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
    s.fn.vkCmdBindVertexBuffers(cmd, 0, 1, &s.vbo, &off);
    pc_screen[0] = (float)soft_w;
    pc_screen[1] = (float)soft_h;
    s.fn.vkCmdPushConstants(cmd, s.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc_screen),
                            pc_screen);

    if (s.arr_dset && drawn > 0) {
        s.fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout, 0, 1,
                                     &s.arr_dset, 0, NULL);
        s.fn.vkCmdDraw(cmd, (uint32_t)(drawn * 6), 1, 0, 0);
    }
    s.fn.vkCmdEndRenderPass(cmd);
    ms_fb = hitch_qpc_ms_since(t_fb0);

    {
        static DWORD s_phase_last;
        if (s_now_mv - s_phase_last > 250u) {
            char data[360];
            s_phase_last = s_now_mv;
            snprintf(data, sizeof(data),
                     "{\"inst\":%d,\"vis\":%d,\"quads\":%d,\"drawn\":%d,"
                     "\"ms_lc\":%.3f,\"ms_build\":%.3f,\"ms_sort\":%.3f,"
                     "\"ms_vert\":%.3f,\"ms_fb\":%.3f,\"tpl\":1,\"uv\":\"arr\","
                     "\"runId\":\"fps-budget\"}",
                     n_inst, vis_inst_log, qi, drawn, ms_lc, ms_build, ms_sort, ms_vert, ms_fb);
            hooks_agent("H-A", "vk_obj.c:record", "obj-phase", data);
            hitch_set_obj_phases(ms_lc, ms_build, ms_sort, ms_vert, ms_fb, qi, vis_inst_log,
                                 n_inst);
        } else {
            hitch_set_obj_phases(ms_lc, ms_build, ms_sort, ms_vert, ms_fb, qi, vis_inst_log,
                                 n_inst);
        }
    }

    n = InterlockedIncrement(&s.draws);
    if (n <= 8 || (n % 120) == 0) {
        log_msg("vk_obj: draw #%ld quads=%d inst=%d resident=%d soft=%dx%d", (long)n, drawn, n_inst,
                s.arr_resident, soft_w, soft_h);
        {
            char data[192];
            snprintf(data, sizeof(data),
                     "{\"n\":%ld,\"quads\":%d,\"inst\":%d,\"resident\":%d,\"soft\":[%d,%d],"
                     "\"mode\":\"tpl\"}",
                     (long)n, drawn, n_inst, s.arr_resident, soft_w, soft_h);
            hooks_agent("H-OBJ", "vk_obj.c:record", "obj draw", data);
        }
    }
    return drawn;
}

