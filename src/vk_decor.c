#include "vk_decor.h"
#include "vk_iso_depth.h"
#include "decor_spawn.h"
#include "hooks_internal.h"
#include "ktx_decor.h"
#include "ktx_terrain.h"
#include "log.h"

#include "shaders/decor_vert_spv.h"
#include "shaders/decor_frag_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CK_MAP_MGR = 0x008C7B30u,
    VERT_MAX = 8192 * 6,
    DRAW_MAX = 4096,
    ATLAS_MAX = 32,
};

typedef struct {
    float x, y;
    float u, v;
    float shadow; /* 1 = drawmode shadow */
    float sort_y;
} DecorVert;

typedef struct {
    float sx0, sy0, sx1, sy1;
    float u0, v0, u1, v1;
    float sort_y;
    int atlas;
    int shadow;
} DecorQuad;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet dset;
    uint32_t w, h;
    char name[48];
} DecorAtlasGpu;

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

    DecorAtlasGpu atlases[ATLAS_MAX];
    int atlas_n;
    char season[32];

    volatile LONG draws;
} s;

float g_vk_last_decor_sy_max;

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

static void destroy_atlases(void)
{
    int i;
    for (i = 0; i < s.atlas_n; ++i) {
        DecorAtlasGpu *a = &s.atlases[i];
        if (a->view && s.fn.vkDestroyImageView)
            s.fn.vkDestroyImageView(s.device, a->view, NULL);
        if (a->img && s.fn.vkDestroyImage)
            s.fn.vkDestroyImage(s.device, a->img, NULL);
        if (a->mem && s.fn.vkFreeMemory)
            s.fn.vkFreeMemory(s.device, a->mem, NULL);
        memset(a, 0, sizeof(*a));
    }
    s.atlas_n = 0;
    s.season[0] = '\0';
}

static int upload_one_atlas(int idx, DecorAtlasGpu *out)
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
    VkWriteDescriptorSet wds;
    VkBufferImageCopy bic;
    VkImageMemoryBarrier barr;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkResult r;
    uint32_t fmt_use;
    const KtxDecorAtlas *atl = &ktx_decor_atlases()[idx];

    memset(out, 0, sizeof(*out));
    if (!ktx_decor_atlas_path(idx, path, sizeof(path)))
        return 0;
    if (!ktx_terrain_load_info(path, &info, &blocks) || !blocks)
        return 0;
    if (info.vk_format == 137u || info.vk_format == 138u) {
        fmt_use = 137u;
    } else if (info.vk_format == 37u || info.vk_format == 43u) {
        fmt_use = 37u;
    } else {
        free(blocks);
        log_msg("vk_decor: unexpected fmt %u idx=%d", (unsigned)info.vk_format, idx);
        return 0;
    }

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)fmt_use;
    ici.extent.width = info.width;
    ici.extent.height = info.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &out->img);
    if (r != VK_SUCCESS) {
        free(blocks);
        return 0;
    }
    s.fn.vkGetImageMemoryRequirements(s.device, out->img, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &out->mem);
    if (r != VK_SUCCESS) {
        free(blocks);
        s.fn.vkDestroyImage(s.device, out->img, NULL);
        out->img = VK_NULL_HANDLE;
        return 0;
    }
    s.fn.vkBindImageMemory(s.device, out->img, out->mem, 0);

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
        s.fn.vkDestroyBuffer(s.device, staging, NULL);
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
    barr.image = out->img;
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
    s.fn.vkCmdCopyBufferToImage(s.upload_cmd, staging, out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
    vci.image = out->img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = (VkFormat)fmt_use;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &out->view);
    if (r != VK_SUCCESS)
        return 0;

    memset(&dai, 0, sizeof(dai));
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.dset_layout;
    if (s.fn.vkAllocateDescriptorSets(s.device, &dai, &out->dset) != VK_SUCCESS)
        return 0;
    memset(&dii, 0, sizeof(dii));
    dii.sampler = s.sampler;
    dii.imageView = out->view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wds, 0, sizeof(wds));
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = out->dset;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    s.fn.vkUpdateDescriptorSets(s.device, 1, &wds, 0, NULL);

    out->w = info.width;
    out->h = info.height;
    lstrcpynA(out->name, atl->name[0] ? atl->name : atl->ktx2, (int)sizeof(out->name));
    log_msg("vk_decor: atlas[%d] %s %ux%u sprites=%d", idx, out->name, out->w, out->h,
            atl->sprite_count);
    return 1;
}

static int upload_atlases(void)
{
    int n, i;
    destroy_atlases();
    if (s.pool && s.fn.vkResetDescriptorPool)
        s.fn.vkResetDescriptorPool(s.device, s.pool, 0);
    if (!ktx_decor_ready())
        return 0;
    n = ktx_decor_atlas_count();
    if (n < 1)
        return 0;
    if (n > ATLAS_MAX)
        n = ATLAS_MAX;
    for (i = 0; i < n; ++i) {
        if (!upload_one_atlas(i, &s.atlases[s.atlas_n])) {
            log_msg("vk_decor: upload failed idx=%d", i);
            destroy_atlases();
            return 0;
        }
        s.atlas_n++;
    }
    lstrcpynA(s.season, ktx_decor_season(), (int)sizeof(s.season));
    log_msg("vk_decor: uploaded %d atlases season=%s sprites=%d", s.atlas_n, s.season,
            ktx_decor_sprite_count());
    /* #region agent log */
    {
        char data[256];
        snprintf(data, sizeof(data),
                 "{\"atlases\":%d,\"season\":\"%s\",\"sprites\":%d,\"a0\":\"%.24s\",\"a8\":\"%.24s\"}",
                 s.atlas_n, s.season, ktx_decor_sprite_count(),
                 s.atlas_n > 0 ? s.atlases[0].name : "",
                 s.atlas_n > 8 ? s.atlases[8].name : "");
        hooks_agent("E", "vk_decor.c:upload", "atlases ready", data);
    }
    /* #endregion */
    return s.atlas_n > 0;
}

static int ensure_atlases(void)
{
    const char *sea;
    if (!ktx_decor_ready())
        return 0;
    sea = ktx_decor_season();
    if (s.atlas_n > 0 && sea && _stricmp(s.season, sea) == 0)
        return 1;
    return upload_atlases();
}

void vk_decor_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
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

int vk_decor_init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                  VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                  PFN_vkGetInstanceProcAddr gipa, VkInstance instance)
{
    VkShaderModuleCreateInfo smci;
    VkDescriptorSetLayoutBinding bind;
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
    VkVertexInputAttributeDescription va[4];
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

    s.wanted = env_on("CK_GPU_DECOR", 1);
    if (!s.wanted) {
        log_msg("vk_decor: disabled (CK_GPU_DECOR=0)");
        return 0;
    }
    if (s.ready)
        return 1;
    if (!device || !phys || !queue || !gpa || !gipa || !instance || !ktx_decor_ready()) {
        log_msg("vk_decor: init skipped (device/ktx_decor)");
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
        log_msg("vk_decor: missing Vulkan procs");
        return 0;
    }

    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = decor_vert_spv_len;
    smci.pCode = (const uint32_t *)decor_vert_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.vert);
    if (r != VK_SUCCESS)
        return 0;
    smci.codeSize = decor_frag_spv_len;
    smci.pCode = (const uint32_t *)decor_frag_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.frag);
    if (r != VK_SUCCESS)
        return 0;

    memset(&bind, 0, sizeof(bind));
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&dlci, 0, sizeof(dlci));
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings = &bind;
    r = s.fn.vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &s.dset_layout);
    if (r != VK_SUCCESS)
        return 0;

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
    vb.stride = sizeof(DecorVert);
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
    va[2].offset = 16;
    va[3].location = 3;
    va[3].binding = 0;
    va[3].format = VK_FORMAT_R32_SFLOAT;
    va[3].offset = 20;
    memset(&viss, 0, sizeof(viss));
    viss.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viss.vertexBindingDescriptionCount = 1;
    viss.pVertexBindingDescriptions = &vb;
    viss.vertexAttributeDescriptionCount = 4;
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
    psz.descriptorCount = ATLAS_MAX;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = ATLAS_MAX;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    r = s.fn.vkCreateDescriptorPool(s.device, &dpci, NULL, &s.pool);
    if (r != VK_SUCCESS)
        return 0;

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)VERT_MAX * sizeof(DecorVert);
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

    if (!upload_atlases()) {
        log_msg("vk_decor: atlas upload failed");
        return 0;
    }

    s.ready = 1;
    log_msg("vk_decor: GPU pipeline ready fmt=%u", (unsigned)soft_format);
    return 1;
}

void vk_decor_shutdown(void)
{
    if (!s.device)
        return;
    if (s.fn.vkDeviceWaitIdle)
        s.fn.vkDeviceWaitIdle(s.device);
    destroy_atlases();
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
    memset(&s, 0, sizeof(s));
}

int vk_decor_ready(void)
{
    return s.ready;
}

static int quad_cmp(const void *a, const void *b)
{
    float ya = ((const DecorQuad *)a)->sort_y;
    float yb = ((const DecorQuad *)b)->sort_y;
    return (ya > yb) - (ya < yb);
}

/* ENT default_duration ≈ 140 ms (crowns); palm anim frames ≈ 166 ms — keep 140. */
enum { DECOR_ANIM_MS = 140 };

static int decor_anim_tick(void)
{
    return (int)(GetTickCount() / (DWORD)DECOR_ANIM_MS);
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

/* 1=pingpong, 2=reverse, 3=loop, 0=static (frame 0). */
static int decor_layer_anim_mode(const KtxDecorSprite *sp)
{
    if (sp->remaping[0]) {
        if (_stricmp(sp->remaping, "pingpong") == 0)
            return 1;
        if (_stricmp(sp->remaping, "reverse") == 0)
            return 2;
        if (_stricmp(sp->remaping, "loop") == 0 || _stricmp(sp->remaping, "forward") == 0)
            return 3;
        /* remaping=none → fall through to heuristics */
        if (_stricmp(sp->remaping, "none") != 0)
            return 0;
    }
    /* Tree crowns: remaping=pingpong, drawmode=normal, multi-row. */
    if (sp->frames_y > 1 && sp->drawmode[0] && _stricmp(sp->drawmode, "normal") == 0)
        return 1;
    /*
     * MapObjects palms / desert bushes: player_color + remaping=none + 6-row sheet.
     * ENT <anim> loops those rows (~166 ms). Quercus (11 damage rows) stays static.
     * Shadow masks (mode 7 / layer shadow) still loop so they stay in sync.
     */
    if (sp->frames_y > 1 && sp->frames_y <= 6 && sp->drawmode[0] &&
        _stricmp(sp->drawmode, "player_color") == 0)
        return 3;
    return 0;
}

typedef struct {
    int type;
    char layer[48];
    int nfr;
    int mode;
} DecorLayerAnim;

static int decor_layer_cache_find(const DecorLayerAnim *c, int n, int type, const char *layer)
{
    int i;
    for (i = 0; i < n; ++i) {
        if (c[i].type == type && _stricmp(c[i].layer, layer) == 0)
            return i;
    }
    return -1;
}

static int decor_build_layer_cache(const KtxDecorSprite *sprs, int spr_n, DecorLayerAnim *out,
                                   int out_max)
{
    int i, n = 0;
    for (i = 0; i < spr_n && n < out_max; ++i) {
        int idx = decor_layer_cache_find(out, n, sprs[i].type, sprs[i].layer);
        int mode = decor_layer_anim_mode(&sprs[i]);
        if (idx < 0) {
            out[n].type = sprs[i].type;
            lstrcpynA(out[n].layer, sprs[i].layer, (int)sizeof(out[n].layer));
            out[n].nfr = sprs[i].frame + 1;
            out[n].mode = mode;
            n++;
        } else {
            if (sprs[i].frame + 1 > out[idx].nfr)
                out[idx].nfr = sprs[i].frame + 1;
            if (mode > out[idx].mode)
                out[idx].mode = mode;
        }
    }
    return n;
}

static int decor_wanted_frame_cached(const DecorLayerAnim *c, int cn, int type, const char *layer,
                                     int tick)
{
    int idx = decor_layer_cache_find(c, cn, type, layer);
    int mode, nfr;
    if (idx < 0)
        return 0;
    mode = c[idx].mode;
    nfr = c[idx].nfr;
    if (mode == 0 || nfr <= 1)
        return 0;
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

static int ensure_ready(void)
{
    if (s.ready)
        return 1;
    if (!s_defer.have || !ktx_decor_wanted() || !ktx_decor_ready())
        return 0;
    return vk_decor_init(s_defer.device, s_defer.phys, s_defer.queue, s_defer.qfam, s_defer.soft_fmt,
                         s_defer.gpa, s_defer.gipa, s_defer.instance);
}

int vk_decor_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view,
                    VkImageView depth_view, int soft_w, int soft_h)
{
    void *terr;
    LONG L, T, R, B;
    int play_t = 80;
    int play_b;
    int cam_h;
    int soft_cam;
    int n_inst, i, qi, nvert;
    DecorQuad *quads;
    DecorVert *verts;
    float pc_screen[2];
    VkViewport vp;
    VkRect2D sc;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fci;
    VkImageView fb_atts[2];
    VkRenderPassBeginInfo rpbi;
    VkDeviceSize off = 0;
    LONG n;
    int drawn = 0;
    const DecorSpawnItem *items;
    const KtxDecorSprite *sprs;
    int spr_n;

    (void)soft_img;
    if (!env_on("CK_GPU_DECOR", 1))
        return 0;
    if (!ensure_ready() || !cmd || !soft_view || soft_w < 64 || soft_h < 64) {
        /* #region agent log */
        {
            static DWORD s_last;
            DWORD now = GetTickCount();
            if (now - s_last > 1000u) {
                char data[160];
                s_last = now;
                snprintf(data, sizeof(data),
                         "{\"why\":\"ready\",\"cmd\":%d,\"view\":%d,\"wh\":[%d,%d],\"er\":%d}",
                         cmd ? 1 : 0, soft_view ? 1 : 0, soft_w, soft_h,
                         (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0));
                hooks_agent("E-ED", "vk_decor.c:record", "decor early", data);
                log_msg("vk_decor: early ready/view editor=%d wh=%dx%d",
                        (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0), soft_w, soft_h);
            }
        }
        /* #endregion */
        return -1;
    }
    if (!depth_view) {
        static int s_no_depth;
        g_vk_last_decor_sy_max = 0.f;
        if (!s_no_depth) {
            s_no_depth = 1;
            log_msg("vk_decor: no depth view, skipping GPU draw");
        }
        return 0;
    }
    if (!ensure_atlases())
        return -1;

    terr = terrain_obj();
    if (!terr) {
        /* #region agent log */
        {
            static DWORD s_last;
            DWORD now = GetTickCount();
            if (now - s_last > 1000u) {
                char data[96];
                s_last = now;
                snprintf(data, sizeof(data), "{\"why\":\"terr\",\"editor\":%d,\"map\":%u}",
                         (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0),
                         (unsigned)(ULONG_PTR)g_last_attach_map);
                hooks_agent("E-ED", "vk_decor.c:record", "decor early", data);
                log_msg("vk_decor: early no-terr editor=%d map=%p",
                        (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0), g_last_attach_map);
            }
        }
        /* #endregion */
        return -1;
    }
    mm_read_cam(&L, &T, &R, &B);
    if (R <= L || B <= T) {
        /* #region agent log */
        {
            static DWORD s_last;
            DWORD now = GetTickCount();
            if (now - s_last > 1000u) {
                char data[128];
                s_last = now;
                snprintf(data, sizeof(data),
                         "{\"why\":\"cam\",\"cam\":[%ld,%ld,%ld,%ld],\"editor\":%d}", (long)L,
                         (long)T, (long)R, (long)B,
                         (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0));
                hooks_agent("E-ED", "vk_decor.c:record", "decor early", data);
                log_msg("vk_decor: early bad-cam [%ld,%ld]-[%ld,%ld] editor=%d", (long)L, (long)T,
                        (long)R, (long)B,
                        (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0));
            }
        }
        /* #endregion */
        return -1;
    }
    soft_cam = (L <= 64 && T <= 128 && R >= (LONG)soft_w - 64 && (R - L) >= (LONG)soft_w / 2);
    cam_h = (int)(B - T);
    if (cam_h < 64)
        cam_h = soft_h - play_t - 54;
    play_b = play_t + cam_h;
    if (play_b > soft_h - 8)
        play_b = soft_h - 8;
    if (play_b <= play_t + 32)
        play_b = soft_h - 8;

    n_inst = decor_spawn_count();
    {
        int editor = (int)InterlockedCompareExchange(&g_ck_map_editor, 0, 0);
        /* I2 editor uses AttachTerrain-game (clears editor flag on non-templates) and never
         * calls DecorSpawn — refill from DIRG whenever this map epoch has no live spawns. */
        int need_grid = editor || n_inst < 1 || !decor_spawn_epoch_live((LONG)g_map_epoch);
        if (need_grid) {
            void *map = g_last_attach_map;
            LONG fill_L = L, fill_T = T, fill_R = R, fill_B = B;
            int filled;
            int nz_all = 0;
            int reason = 0;
            void *decor = NULL;
            /* soft_cam boot view: L/R are soft pixels ≈ world origin strip — pad taller. */
            if (soft_cam) {
                fill_T = 0;
                fill_B = (LONG)soft_h * 2;
            }
            filled = decor_spawn_fill_from_map_grid_ex(map, fill_L, fill_T, fill_R, fill_B, &nz_all,
                                                       &reason, &decor);
            n_inst = decor_spawn_count();
            /* #region agent log */
            {
                static DWORD s_last;
                DWORD now = GetTickCount();
                if (now - s_last > 500u || filled > 0 || nz_all > 0 || reason > 1) {
                    char data[360];
                    void *inner = NULL;
                    unsigned inner_u = 0;
                    s_last = now;
                    if (map && !IsBadReadPtr(map, 8))
                        inner = *(void **)((char *)map + 4);
                    if (inner)
                        inner_u = (unsigned)(ULONG_PTR)inner;
                    snprintf(data, sizeof(data),
                             "{\"editor\":%d,\"soft_cam\":%d,\"filled\":%d,\"inst\":%d,\"map\":%u,"
                             "\"inner\":%u,\"decor\":%u,\"lastd\":%u,\"nz_all\":%d,\"reason\":%d,"
                             "\"cam\":[%ld,%ld,%ld,%ld],\"epoch\":%ld,\"live\":%d}",
                             editor, soft_cam, filled, n_inst, (unsigned)(ULONG_PTR)map, inner_u,
                             (unsigned)(ULONG_PTR)decor, (unsigned)(ULONG_PTR)g_last_decor_dirg, nz_all,
                             reason, (long)L, (long)T, (long)R, (long)B, (long)g_map_epoch,
                             decor_spawn_epoch_live((LONG)g_map_epoch));
                    hooks_agent("E-GRID", "vk_decor.c:record", "decor grid fill", data);
                    log_msg("vk_decor: grid-fill editor=%d soft_cam=%d filled=%d nz_all=%d reason=%d "
                            "inst=%d decor=%p lastd=%p cam=[%ld,%ld]-[%ld,%ld] epoch=%ld",
                            editor, soft_cam, filled, nz_all, reason, n_inst, decor, g_last_decor_dirg,
                            (long)L, (long)T, (long)R, (long)B, (long)g_map_epoch);
                }
            }
            /* #endregion */
        }
    }
    items = decor_spawn_items();
    sprs = ktx_decor_sprites();
    spr_n = ktx_decor_sprite_count();
    if (n_inst < 1 || spr_n < 1 || !items || !sprs || s.atlas_n < 1)
        return 0;

    quads = (DecorQuad *)malloc(sizeof(DecorQuad) * (size_t)DRAW_MAX);
    if (!quads)
        return -1;
    qi = 0;

    {
        int anim_tick = decor_anim_tick();
        DecorLayerAnim layer_cache[384];
        int layer_n = decor_build_layer_cache(sprs, spr_n, layer_cache, 384);
        for (i = 0; i < n_inst && qi < DRAW_MAX; ++i) {
            unsigned type = decor_spawn_type(items[i].packed);
            LONG wx = items[i].x;
            LONG wy = items[i].y;
            float h, esx, esy;
            int si;
            int tick = anim_tick + (int)(((unsigned)wx * 17u + (unsigned)wy * 31u) & 63u);

            if (wx < L - 400 || wx > R + 400)
                continue;
            h = soft_cam ? (float)cell_h(terr, wx, wy) : exact_h(terr, wx, wy);
            esx = (float)(wx - L);
            esy = map_to_view_yf(wy, h) - (float)T + (float)play_t;
            if (esy < (float)(play_t - 320) || esy > (float)(play_b + 320))
                continue;

            for (si = 0; si < spr_n && qi < DRAW_MAX; ++si) {
                const KtxDecorSprite *sp = &sprs[si];
                float dx, dy, aw, ah;
                int ai;
                int is_shadow;
                int want_fr;
                if ((unsigned)sp->type != type)
                    continue;
                want_fr = decor_wanted_frame_cached(layer_cache, layer_n, sp->type, sp->layer, tick);
                if (sp->frame != want_fr)
                    continue;
                if (sp->w < 1 || sp->h < 1)
                    continue;
                ai = sp->atlas_index;
                if (ai < 0 || ai >= s.atlas_n || !s.atlases[ai].view)
                    continue;
                aw = (float)s.atlases[ai].w;
                ah = (float)s.atlases[ai].h;
                if (aw < 1.f || ah < 1.f)
                    continue;
                is_shadow = ktx_decor_sprite_is_shadow(sp);
                dx = esx + (float)(sp->offset_x + sp->hot_x);
                dy = esy + (float)(sp->offset_y + sp->hot_y);
                if (dx + (float)sp->w < -8.f || dx >= (float)soft_w + 8.f)
                    continue;
                if (dy + (float)sp->h < (float)play_t || dy >= (float)play_b)
                    continue;
                quads[qi].sx0 = dx;
                quads[qi].sy0 = dy;
                quads[qi].sx1 = dx + (float)sp->w;
                quads[qi].sy1 = dy + (float)sp->h;
                quads[qi].u0 = (float)sp->atlas_x / aw;
                quads[qi].v0 = (float)sp->atlas_y / ah;
                quads[qi].u1 = (float)(sp->atlas_x + sp->w) / aw;
                quads[qi].v1 = (float)(sp->atlas_y + sp->h) / ah;
                /* Lower z / shadow draw under the main sprite (same footing). */
                quads[qi].sort_y = esy + (float)sp->sort_oy + (float)sp->z * 0.001f;
                if (is_shadow)
                    quads[qi].sort_y -= 1.0f;
                quads[qi].atlas = ai;
                quads[qi].shadow = is_shadow;
                qi++;
            }
        }
    }

    if (qi < 1) {
        g_vk_last_decor_sy_max = 0.f;
        free(quads);
        return 0;
    }
    qsort(quads, (size_t)qi, sizeof(DecorQuad), quad_cmp);

    verts = (DecorVert *)s.vbo_ptr;
    nvert = 0;
    for (i = 0; i < qi && nvert + 6 <= VERT_MAX; ++i) {
        DecorQuad *q = &quads[i];
        float sh = q->shadow ? 1.f : 0.f;
        DecorVert tri[6] = {
            {q->sx0, q->sy0, q->u0, q->v0, sh, q->sort_y},
            {q->sx1, q->sy0, q->u1, q->v0, sh, q->sort_y},
            {q->sx1, q->sy1, q->u1, q->v1, sh, q->sort_y},
            {q->sx0, q->sy0, q->u0, q->v0, sh, q->sort_y},
            {q->sx1, q->sy1, q->u1, q->v1, sh, q->sort_y},
            {q->sx0, q->sy1, q->u0, q->v1, sh, q->sort_y},
        };
        memcpy(verts + nvert, tri, sizeof(tri));
        nvert += 6;
        drawn++;
    }

    {
        float sy_max = 0.f;
        for (i = 0; i < qi; ++i) {
            if (quads[i].sort_y > sy_max)
                sy_max = quads[i].sort_y;
        }
        g_vk_last_decor_sy_max = sy_max;
    }

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
    if (s.fn.vkCreateFramebuffer(s.device, &fci, NULL, &fb) != VK_SUCCESS) {
        free(quads);
        return -1;
    }

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
    sc.extent.height = (uint32_t)(play_b - (int)sc.offset.y);
    s.fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    s.fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    s.fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
    s.fn.vkCmdBindVertexBuffers(cmd, 0, 1, &s.vbo, &off);
    pc_screen[0] = (float)soft_w;
    pc_screen[1] = (float)soft_h;
    s.fn.vkCmdPushConstants(cmd, s.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc_screen),
                            pc_screen);

    /* Y-sorted quads; rebind atlas descriptor when atlas_index changes. */
    {
        int run0 = 0;
        while (run0 < drawn) {
            int ai = quads[run0].atlas;
            int run1 = run0 + 1;
            while (run1 < drawn && quads[run1].atlas == ai)
                run1++;
            if (ai >= 0 && ai < s.atlas_n && s.atlases[ai].dset) {
                s.fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout, 0,
                                             1, &s.atlases[ai].dset, 0, NULL);
                s.fn.vkCmdDraw(cmd, (uint32_t)((run1 - run0) * 6), 1, (uint32_t)(run0 * 6), 0);
            }
            run0 = run1;
        }
    }
    s.fn.vkCmdEndRenderPass(cmd);
    s.fn.vkDestroyFramebuffer(s.device, fb, NULL);
    free(quads);

    n = InterlockedIncrement(&s.draws);
    if (n <= 8 || (n % 60) == 0) {
        log_msg("vk_decor: draw #%ld quads=%d inst=%d atlases=%d soft=%dx%d season=%s", (long)n,
                drawn, n_inst, s.atlas_n, soft_w, soft_h, s.season);
        /* #region agent log */
        {
            char data[192];
            snprintf(data, sizeof(data),
                     "{\"n\":%ld,\"quads\":%d,\"inst\":%d,\"atlases\":%d,\"soft\":[%d,%d],"
                     "\"season\":\"%s\"}",
                     (long)n, drawn, n_inst, s.atlas_n, soft_w, soft_h, s.season);
            hooks_agent("F", "vk_decor.c:record", "decor draw", data);
        }
        /* #endregion */
    }
    return drawn;
}
