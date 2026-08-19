/* Soft retail sprites blended over GPU terrain (original MapObj textures). */
#include "vk_soft_overlay.h"
#include "hooks_internal.h"
#include "ktx_obj.h"
#include "log.h"
#include "vk_terrain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaders/soft_overlay_vert_spv.h"
#include "shaders/soft_overlay_frag_spv.h"

typedef struct {
    int wanted;
    int ready;
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
        PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
        PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
        PFN_vkCreateImage vkCreateImage;
        PFN_vkDestroyImage vkDestroyImage;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
        PFN_vkBindImageMemory vkBindImageMemory;
        PFN_vkCreateImageView vkCreateImageView;
        PFN_vkDestroyImageView vkDestroyImageView;
        PFN_vkAllocateMemory vkAllocateMemory;
        PFN_vkFreeMemory vkFreeMemory;
        PFN_vkCreateFramebuffer vkCreateFramebuffer;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
        PFN_vkCmdBindPipeline vkCmdBindPipeline;
        PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
        PFN_vkCmdDraw vkCmdDraw;
        PFN_vkCmdSetViewport vkCmdSetViewport;
        PFN_vkCmdSetScissor vkCmdSetScissor;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
    } fn;

    VkShaderModule vert, frag;
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout pipe_layout;
    VkRenderPass rp;
    VkPipeline pipe;
    VkSampler sampler;
    VkDescriptorPool pool;
    VkDescriptorSet dset;

    VkImage ov_img;
    VkDeviceMemory ov_mem;
    VkImageView ov_view;
    int ov_w, ov_h;
} SoftOv;

static SoftOv s;
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
    s.fn.vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return 0;
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
    LOAD_DEV(vkAllocateDescriptorSets);
    LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageMemoryRequirements);
    LOAD_DEV(vkBindImageMemory);
    LOAD_DEV(vkCreateImageView);
    LOAD_DEV(vkDestroyImageView);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkCreateFramebuffer);
    LOAD_DEV(vkDestroyFramebuffer);
    LOAD_DEV(vkCmdPipelineBarrier);
    LOAD_DEV(vkCmdCopyBufferToImage);
    LOAD_DEV(vkCmdBeginRenderPass);
    LOAD_DEV(vkCmdEndRenderPass);
    LOAD_DEV(vkCmdBindPipeline);
    LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdDraw);
    LOAD_DEV(vkCmdSetViewport);
    LOAD_DEV(vkCmdSetScissor);
    LOAD_DEV(vkDeviceWaitIdle);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    return 1;
}

static void destroy_ov_img(void)
{
    if (!s.device)
        return;
    if (s.ov_view)
        s.fn.vkDestroyImageView(s.device, s.ov_view, NULL);
    if (s.ov_img)
        s.fn.vkDestroyImage(s.device, s.ov_img, NULL);
    if (s.ov_mem)
        s.fn.vkFreeMemory(s.device, s.ov_mem, NULL);
    s.ov_view = VK_NULL_HANDLE;
    s.ov_img = VK_NULL_HANDLE;
    s.ov_mem = VK_NULL_HANDLE;
    s.ov_w = s.ov_h = 0;
}

static int soft_overlay_init(void);

int vk_soft_overlay_wanted(void)
{
    /* Off by default — soft MapObj/decor stamps are skipped under GPU terrain.
     * CK_SOFT_OBJ=1 (temp scan) or CK_GPU_SOFT_PLAYFIELD=1 composites retail sprites. */
    if (ck_soft_obj_enabled())
        return 1;
    return env_on("CK_GPU_SOFT_PLAYFIELD", 0);
}

void vk_soft_overlay_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                                 uint32_t qfam, VkFormat soft_format,
                                 PFN_vkGetDeviceProcAddr gpa, PFN_vkGetInstanceProcAddr gipa,
                                 VkInstance instance)
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

int vk_soft_overlay_ready(void)
{
    return s.ready;
}

static int soft_overlay_init(void)
{
    VkShaderModuleCreateInfo smci;
    VkDescriptorSetLayoutBinding bind;
    VkDescriptorSetLayoutCreateInfo dlci;
    VkPipelineLayoutCreateInfo plci;
    VkAttachmentDescription att;
    VkAttachmentReference atr;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo rpci;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo viss;
    VkPipelineInputAssemblyStateCreateInfo iass;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rast;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cbs;
    VkPipelineDynamicStateCreateInfo dyn;
    VkDynamicState dyns[2];
    VkGraphicsPipelineCreateInfo gpci;
    VkSamplerCreateInfo sci;
    VkDescriptorPoolSize psz;
    VkDescriptorPoolCreateInfo dpci;
    VkDescriptorSetAllocateInfo dai;
    VkResult r;

    if (!vk_soft_overlay_wanted())
        return 0;
    if (s.ready)
        return 1;
    if (!s_defer.have) {
        log_msg("vk_soft_overlay: no device");
        return 0;
    }

    memset(&s, 0, sizeof(s));
    s.wanted = 1;
    s.device = s_defer.device;
    s.phys = s_defer.phys;
    s.queue = s_defer.queue;
    s.qfam = s_defer.qfam;
    s.soft_fmt = s_defer.soft_fmt;
    s.gpa = s_defer.gpa;
    s.gipa = s_defer.gipa;
    s.instance = s_defer.instance;
    if (!load_fns()) {
        log_msg("vk_soft_overlay: missing procs");
        return 0;
    }

    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = soft_overlay_vert_spv_len;
    smci.pCode = (const uint32_t *)soft_overlay_vert_spv;
    r = s.fn.vkCreateShaderModule(s.device, &smci, NULL, &s.vert);
    if (r != VK_SUCCESS)
        return 0;
    smci.codeSize = soft_overlay_frag_spv_len;
    smci.pCode = (const uint32_t *)soft_overlay_frag_spv;
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

    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s.dset_layout;
    r = s.fn.vkCreatePipelineLayout(s.device, &plci, NULL, &s.pipe_layout);
    if (r != VK_SUCCESS)
        return 0;

    memset(&att, 0, sizeof(att));
    att.format = s.soft_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&atr, 0, sizeof(atr));
    atr.attachment = 0;
    atr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &atr;
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
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

    memset(&viss, 0, sizeof(viss));
    viss.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    psz.descriptorCount = 1;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    r = s.fn.vkCreateDescriptorPool(s.device, &dpci, NULL, &s.pool);
    if (r != VK_SUCCESS)
        return 0;

    memset(&dai, 0, sizeof(dai));
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.dset_layout;
    r = s.fn.vkAllocateDescriptorSets(s.device, &dai, &s.dset);
    if (r != VK_SUCCESS)
        return 0;

    s.ready = 1;
    log_msg("vk_soft_overlay: ready (retail MapObj on GPU terrain)");
    /* #region agent log */
    {
        char data[96];
        snprintf(data, sizeof(data), "{\"ready\":1,\"soft_pf\":%d,\"gpu_obj\":%d}",
                 env_on("CK_GPU_SOFT_PLAYFIELD", 0) ? 1 : 0, ktx_obj_wanted() ? 1 : 0);
        hooks_agent("H-OV", "vk_soft_overlay.c:init", "soft-ov-ready", data);
    }
    /* #endregion */
    return 1;
}

void vk_soft_overlay_shutdown(void)
{
    if (!s.device)
        return;
    if (s.fn.vkDeviceWaitIdle)
        s.fn.vkDeviceWaitIdle(s.device);
    destroy_ov_img();
    if (s.pipe)
        s.fn.vkDestroyPipeline(s.device, s.pipe, NULL);
    if (s.rp)
        s.fn.vkDestroyRenderPass(s.device, s.rp, NULL);
    if (s.pipe_layout)
        s.fn.vkDestroyPipelineLayout(s.device, s.pipe_layout, NULL);
    if (s.dset_layout)
        s.fn.vkDestroyDescriptorSetLayout(s.device, s.dset_layout, NULL);
    if (s.sampler)
        s.fn.vkDestroySampler(s.device, s.sampler, NULL);
    if (s.pool)
        s.fn.vkDestroyDescriptorPool(s.device, s.pool, NULL);
    if (s.vert)
        s.fn.vkDestroyShaderModule(s.device, s.vert, NULL);
    if (s.frag)
        s.fn.vkDestroyShaderModule(s.device, s.frag, NULL);
    memset(&s, 0, sizeof(s));
}

int vk_soft_overlay_ensure(int soft_w, int soft_h)
{
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkImageViewCreateInfo vci;
    VkResult r;

    if (!soft_overlay_init())
        return 0;
    if (s.ov_img && s.ov_w == soft_w && s.ov_h == soft_h)
        return 1;
    destroy_ov_img();

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = s.soft_fmt;
    ici.extent.width = (uint32_t)soft_w;
    ici.extent.height = (uint32_t)soft_h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.fn.vkCreateImage(s.device, &ici, NULL, &s.ov_img);
    if (r != VK_SUCCESS)
        return 0;
    s.fn.vkGetImageMemoryRequirements(s.device, s.ov_img, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = s.fn.vkAllocateMemory(s.device, &mai, NULL, &s.ov_mem);
    if (r != VK_SUCCESS) {
        destroy_ov_img();
        return 0;
    }
    s.fn.vkBindImageMemory(s.device, s.ov_img, s.ov_mem, 0);

    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s.ov_img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = s.soft_fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = s.fn.vkCreateImageView(s.device, &vci, NULL, &s.ov_view);
    if (r != VK_SUCCESS) {
        destroy_ov_img();
        return 0;
    }
    s.ov_w = soft_w;
    s.ov_h = soft_h;
    return 1;
}

int vk_soft_overlay_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view,
                           VkBuffer soft_upload, int soft_w, int soft_h)
{
    VkImageMemoryBarrier barr;
    VkBufferImageCopy region;
    VkDescriptorImageInfo dii;
    VkWriteDescriptorSet wds;
    VkFramebuffer fb;
    VkFramebufferCreateInfo fci;
    VkRenderPassBeginInfo rpbi;
    VkViewport vp;
    VkRect2D sc;
    int play_t = 80, play_b;

    (void)soft_img;
    if (!vk_soft_overlay_wanted())
        return 0;
    if (!cmd || !soft_view || !soft_upload || soft_w < 64 || soft_h < 64)
        return -1;
    if (!vk_terrain_draw_enabled() || !vk_terrain_soft_land_disabled())
        return 0;
    if (!vk_soft_overlay_ensure(soft_w, soft_h))
        return -1;

    play_b = soft_h - 54;
    if (play_b <= play_t + 32)
        play_b = soft_h;

    /* soft_upload → overlay (TRANSFER_DST) */
    memset(&barr, 0, sizeof(barr));
    barr.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barr.image = s.ov_img;
    barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barr.subresourceRange.levelCount = 1;
    barr.subresourceRange.layerCount = 1;
    barr.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.srcAccessMask = 0;
    barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    s.fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &barr);

    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)soft_w;
    region.imageExtent.height = (uint32_t)soft_h;
    region.imageExtent.depth = 1;
    s.fn.vkCmdCopyBufferToImage(cmd, soft_upload, s.ov_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &region);

    barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    s.fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);

    memset(&dii, 0, sizeof(dii));
    dii.sampler = s.sampler;
    dii.imageView = s.ov_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wds, 0, sizeof(wds));
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = s.dset;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    s.fn.vkUpdateDescriptorSets(s.device, 1, &wds, 0, NULL);

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
    s.fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
    memset(&vp, 0, sizeof(vp));
    vp.width = (float)soft_w;
    vp.height = (float)soft_h;
    vp.maxDepth = 1.0f;
    memset(&sc, 0, sizeof(sc));
    sc.offset.y = play_t;
    sc.extent.width = (uint32_t)soft_w;
    sc.extent.height = (uint32_t)(play_b - play_t);
    s.fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    s.fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    s.fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout, 0, 1, &s.dset,
                                 0, NULL);
    s.fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    s.fn.vkCmdEndRenderPass(cmd);
    s.fn.vkDestroyFramebuffer(s.device, fb, NULL);

    /* #region agent log */
    {
        static LONG s_n;
        LONG n = InterlockedIncrement(&s_n);
        if (n <= 8 || (n % 60) == 0) {
            char data[128];
            snprintf(data, sizeof(data), "{\"n\":%ld,\"wh\":[%d,%d],\"play\":[%d,%d],\"alpha\":1}",
                     (long)n, soft_w, soft_h, play_t, play_b);
            hooks_agent("H-OV", "vk_soft_overlay.c:record", "soft-ov-draw", data);
        }
    }
    /* #endregion */
    return 1;
}
