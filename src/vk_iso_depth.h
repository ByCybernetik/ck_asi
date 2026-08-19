#ifndef VK_ISO_DEPTH_H
#define VK_ISO_DEPTH_H

#include <stdint.h>

#ifdef NO_GPU_SCENE

static inline void vk_iso_depth_shutdown(void)  {}

#else /* !NO_GPU_SCENE */

#include <vulkan/vulkan.h>

/* Shared D16 depth for GPU decor+obj isometric occlusion (sort_y → Z).
 * Cleared to 0 each frame; shaders write Z = sort_y/soft_h; compare GREATER
 * so southern (larger sort_y) sprites win over northern ones across passes. */

void vk_iso_depth_note_device(VkDevice device, VkPhysicalDevice phys,
                               PFN_vkGetDeviceProcAddr gpa, PFN_vkGetInstanceProcAddr gipa,
                               VkInstance instance);
void vk_iso_depth_shutdown(void);

/* Ensure depth image matches soft size; returns view or NULL. */
VkImageView vk_iso_depth_ensure(int soft_w, int soft_h);

/* Transition + clear depth to 0 (call once before decor each frame). */
void vk_iso_depth_clear(VkCommandBuffer cmd);

VkFormat vk_iso_depth_format(void);

#endif /* NO_GPU_SCENE */
#endif
