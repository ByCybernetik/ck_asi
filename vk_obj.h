#ifndef VK_OBJ_H
#define VK_OBJ_H

#include <stdint.h>
#include <vulkan/vulkan.h>

/* GPU buildings/units overpaint (BC3 atlas). Draws on soft_img after soft blit
 * (and after vk_terrain_record when GPU terrain ON). On by default (CK_GPU_OBJ=1);
 * set 0 for soft MapObj only. Streaming texture2DArray + async staging. */

void vk_obj_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                          VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                          PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
int vk_obj_init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                  VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                  PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
void vk_obj_shutdown(void);
int vk_obj_ready(void);

/* 1 if catalog atlas packs into the 2048×3072 array cache. */
int vk_obj_atlas_fits_array(int atlas_index);
/* 1 if atlas layer is uploaded and ready to sample. */
int vk_obj_atlas_resident(int atlas_index);
/* Queue atlas for async CPU→staging→array upload. */
void vk_obj_request_atlas(int atlas_index);

/* Queue all fitting atlases used by a catalog sprite id. */
void vk_obj_request_id(const char *id);

/* Complete/start at most one atlas upload before recording the present CB. */
void vk_obj_pump_uploads(void);

/* soft_img in COLOR_ATTACHMENT_OPTIMAL; left that way. Returns quads drawn or -1. */
int vk_obj_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view,
                  VkImageView depth_view, int soft_w, int soft_h);

#endif
