#ifndef VK_SOFT_OVERLAY_H
#define VK_SOFT_OVERLAY_H

#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * Soft retail MapObj sprites (original IMGRLE→RleDraw) over GPU terrain.
 * CK_GPU_SOFT_PLAYFIELD=1, or auto when CK_GPU_OBJ=0 + soft land off.
 * Alpha-keyed near-black playfield; blends onto soft_img after terrain/decor.
 */

void vk_soft_overlay_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                                 uint32_t qfam, VkFormat soft_format,
                                 PFN_vkGetDeviceProcAddr gpa, PFN_vkGetInstanceProcAddr gipa,
                                 VkInstance instance);
int vk_soft_overlay_wanted(void);
int vk_soft_overlay_ready(void);
void vk_soft_overlay_shutdown(void);

/* Upload soft_upload (BGRA, alpha-keyed) into overlay tex; sample while drawing. */
int vk_soft_overlay_ensure(int soft_w, int soft_h);

/* soft_img must be COLOR_ATTACHMENT_OPTIMAL; left that way. soft_upload = host BGRA. */
int vk_soft_overlay_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view,
                           VkBuffer soft_upload, int soft_w, int soft_h);

#endif
