#ifndef VK_DECOR_H
#define VK_DECOR_H

#include <stdint.h>
#include <vulkan/vulkan.h>

/* GPU PutDecor overpaint (BC3 atlas). Draws on soft_img after soft blit
 * (and after vk_terrain_record when GPU terrain ON). On by default; CK_GPU_DECOR=0 disables. */

void vk_decor_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                          VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                          PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
int vk_decor_init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                  VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                  PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
void vk_decor_shutdown(void);
int vk_decor_ready(void);

/* soft_img in COLOR_ATTACHMENT_OPTIMAL; left that way. Returns quads drawn or -1. */
int vk_decor_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view,
                    VkImageView depth_view, int soft_w, int soft_h);

#endif
