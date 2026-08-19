#include "vk_iso_depth.h"
#include "log.h"

#include <string.h>

typedef struct {
    VkDevice device;
    VkPhysicalDevice phys;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    int w, h;
    int layout_ok; /* 1 = DEPTH_STENCIL_ATTACHMENT_OPTIMAL */
} IsoDepth;

static IsoDepth s;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    s.vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

void vk_iso_depth_note_device(VkDevice device, VkPhysicalDevice phys,
                               PFN_vkGetDeviceProcAddr gpa, PFN_vkGetInstanceProcAddr gipa,
                               VkInstance instance)
{
    if (!device || !phys || !gpa || !gipa || !instance)
        return;
    s.device = device;
    s.phys = phys;
#define LD(n) s.n = (PFN_##n)gpa(device, #n)
    LD(vkCreateImage);
    LD(vkDestroyImage);
    LD(vkGetImageMemoryRequirements);
    LD(vkAllocateMemory);
    LD(vkFreeMemory);
    LD(vkBindImageMemory);
    LD(vkCreateImageView);
    LD(vkDestroyImageView);
    LD(vkCmdPipelineBarrier);
    LD(vkCmdClearDepthStencilImage);
#undef LD
    s.vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance,
                                                      "vkGetPhysicalDeviceMemoryProperties");
}

void vk_iso_depth_shutdown(void)
{
    if (!s.device)
        return;
    if (s.view)
        s.vkDestroyImageView(s.device, s.view, NULL);
    if (s.img)
        s.vkDestroyImage(s.device, s.img, NULL);
    if (s.mem)
        s.vkFreeMemory(s.device, s.mem, NULL);
    memset(&s, 0, sizeof(s));
}

VkFormat vk_iso_depth_format(void)
{
    return VK_FORMAT_D16_UNORM;
}

VkImageView vk_iso_depth_ensure(int soft_w, int soft_h)
{
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkImageViewCreateInfo vci;
    uint32_t mi;
    VkResult r;

    if (!s.device || !s.phys || soft_w < 64 || soft_h < 64)
        return VK_NULL_HANDLE;
    if (s.view && s.w == soft_w && s.h == soft_h)
        return s.view;

    if (s.view)
        s.vkDestroyImageView(s.device, s.view, NULL);
    if (s.img)
        s.vkDestroyImage(s.device, s.img, NULL);
    if (s.mem)
        s.vkFreeMemory(s.device, s.mem, NULL);
    s.view = VK_NULL_HANDLE;
    s.img = VK_NULL_HANDLE;
    s.mem = VK_NULL_HANDLE;
    s.layout_ok = 0;

    if (!s.vkGetPhysicalDeviceMemoryProperties) {
        log_msg("vk_iso_depth: no GetPhysicalDeviceMemoryProperties");
        return VK_NULL_HANDLE;
    }

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_D16_UNORM;
    ici.extent.width = (uint32_t)soft_w;
    ici.extent.height = (uint32_t)soft_h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.vkCreateImage(s.device, &ici, NULL, &s.img);
    if (r != VK_SUCCESS)
        return VK_NULL_HANDLE;
    s.vkGetImageMemoryRequirements(s.device, s.img, &req);
    mi = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi == UINT32_MAX)
        mi = find_mem(req.memoryTypeBits, 0);
    if (mi == UINT32_MAX) {
        s.vkDestroyImage(s.device, s.img, NULL);
        s.img = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mi;
    if (s.vkAllocateMemory(s.device, &mai, NULL, &s.mem) != VK_SUCCESS) {
        s.vkDestroyImage(s.device, s.img, NULL);
        s.img = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    s.vkBindImageMemory(s.device, s.img, s.mem, 0);
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_D16_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (s.vkCreateImageView(s.device, &vci, NULL, &s.view) != VK_SUCCESS) {
        s.vkFreeMemory(s.device, s.mem, NULL);
        s.vkDestroyImage(s.device, s.img, NULL);
        s.img = VK_NULL_HANDLE;
        s.mem = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    s.w = soft_w;
    s.h = soft_h;
    log_msg("vk_iso_depth: %dx%d D16", soft_w, soft_h);
    return s.view;
}

void vk_iso_depth_clear(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier b;
    VkClearDepthStencilValue clear;
    VkImageSubresourceRange range;

    if (!cmd || !s.img || !s.vkCmdClearDepthStencilImage)
        return;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.image = s.img;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.oldLayout = s.layout_ok ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = s.layout_ok ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    s.vkCmdPipelineBarrier(cmd,
                           s.layout_ok ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    clear.depth = 0.0f;
    clear.stencil = 0;
    range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    s.vkCmdClearDepthStencilImage(cmd, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                  &range);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                           0, 0, NULL, 0, NULL, 1, &b);
    s.layout_ok = 1;
}
