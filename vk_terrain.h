#ifndef VK_TERRAIN_H
#define VK_TERRAIN_H

#include <stdint.h>
#include <vulkan/vulkan.h>

/* CK_GPU_TERRAIN_OVERPAINT=1: AABB quads into soft_img (NOT texture replace).
 * CK_GPU_TERRAIN_AUTO=1 (default): load GPU on first present; soft land skipped.
 * F8 toggles GPU↔soft. CK_GPU_KEEP_SOFT_LAND=1 keeps retail soft under GPU. */
int vk_terrain_wanted(void);
/* Stash Vulkan handles for lazy F8 init (no pipeline/KTX upload yet). */
void vk_terrain_note_device(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                            VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                            PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
int vk_terrain_init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                    VkFormat soft_format, PFN_vkGetDeviceProcAddr gpa,
                    PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
void vk_terrain_shutdown(void);
int vk_terrain_ready(void);
int vk_terrain_draw_enabled(void); /* runtime: overpaint/blend currently drawing */
int vk_terrain_soft_land_disabled(void); /* 1 = skip retail soft land (GPU-first / F8) */
int vk_terrain_has_map(void); /* 1 = MapMgr terrain object live (in-game / editor) */
void vk_terrain_poll_toggle(void); /* AUTO load + F8 toggle — call per present */

/* Call from ThreadAnimate when water frame_i advances — drives blend frac. */
void vk_terrain_note_anim_tick(void);

VkImageView vk_terrain_create_soft_view(VkImage soft_img, VkFormat soft_format);
void vk_terrain_destroy_soft_view(VkImageView view);

/* soft_img in COLOR_ATTACHMENT_OPTIMAL; left that way. Returns tiles drawn or -1. */
int vk_terrain_record(VkCommandBuffer cmd, VkImage soft_img, VkImageView soft_view, int soft_w,
                      int soft_h);

#endif
