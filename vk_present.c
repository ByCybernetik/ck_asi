/* Wine PE32: load vulkan-1.dll at runtime; present software frames to game HWND. */
#include "vk_present.h"
#include "dm_replace.h"
#include "log.h"
#include "hitch.h"
#include "hooks.h"
#include "hooks_internal.h"
#include "ktx_terrain.h"
#include "ktx_gpu_terrain.h"
#include "tip_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mmsystem.h>

enum { CK_UI_ASSET_W = 1600, CK_UI_MIRROR_SPAN = 256 };

/* Quiet default: skip per-frame agent fopen unless CK_DEBUG_FULL=1. */
static FILE *vk_agent_open(void)
{
    if (!hooks_debug_full())
        return NULL;
    return vk_agent_open();
}

/* H-DIB: game soft 16bpp DIB (SetDIBits bits). Stable across frames; DrawText writes here. */
static const uint8_t *g_soft_dib_bits;
static int g_soft_dib_w, g_soft_dib_h, g_soft_dib_stride, g_soft_dib_top_down, g_soft_dib_bpp;

void ck_soft_dib_note(const void *bits, int w, int h, int stride, int top_down, int bpp)
{
    if (!bits || w < 640 || h < 400 || stride < w || bpp != 16)
        return;
    if (IsBadReadPtr(bits, (SIZE_T)stride))
        return;
    g_soft_dib_bits = (const uint8_t *)bits;
    g_soft_dib_w = w;
    g_soft_dib_h = h;
    g_soft_dib_stride = stride;
    g_soft_dib_top_down = top_down ? 1 : 0;
    g_soft_dib_bpp = bpp;
}

int ck_soft_dib_fill16(int x0, int y0, int x1, int y1, unsigned short c565)
{
    int y, x, filled = 0;
    uint8_t *bits;

    if (!g_soft_dib_bits || g_soft_dib_bpp != 16 || g_soft_dib_w < 64 || g_soft_dib_h < 64)
        return -1;
    bits = (uint8_t *)g_soft_dib_bits;
    if (IsBadWritePtr(bits, (SIZE_T)g_soft_dib_stride))
        return -2;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > g_soft_dib_w)
        x1 = g_soft_dib_w;
    if (y1 > g_soft_dib_h)
        y1 = g_soft_dib_h;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    for (y = y0; y < y1; ++y) {
        uint8_t *row;
        unsigned short *px;
        int sy = g_soft_dib_top_down ? y : (g_soft_dib_h - 1 - y);
        row = bits + (size_t)sy * (size_t)g_soft_dib_stride;
        px = (unsigned short *)row;
        for (x = x0; x < x1; ++x) {
            px[x] = c565;
            filled++;
        }
    }
    return filled;
}

int ck_soft_dib_get(unsigned char **bits, int *w, int *h, int *stride, int *top_down)
{
    if (!g_soft_dib_bits || g_soft_dib_bpp != 16 || g_soft_dib_w < 64 || g_soft_dib_h < 64)
        return 0;
    if (IsBadWritePtr((void *)g_soft_dib_bits, (SIZE_T)g_soft_dib_stride))
        return 0;
    if (bits)
        *bits = (unsigned char *)g_soft_dib_bits;
    if (w)
        *w = g_soft_dib_w;
    if (h)
        *h = g_soft_dib_h;
    if (stride)
        *stride = g_soft_dib_stride;
    if (top_down)
        *top_down = g_soft_dib_top_down;
    return 1;
}

static unsigned ck_fb_px(const uint8_t *fb, int soft_w, int x, int y)
{
    const uint8_t *p;
    if (!fb || soft_w <= 0 || x < 0 || y < 0)
        return 0;
    p = fb + ((size_t)y * (size_t)soft_w + (size_t)x) * 4u;
    return ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[0];
}

static int ui_col_lit(const uint8_t *fb, int soft_w, int x, int y)
{
    const uint8_t *p = fb + ((size_t)y * (size_t)soft_w + (size_t)x) * 4u;
    return p[0] > 12 || p[1] > 12 || p[2] > 12;
}

/*
 * ZoomMap letterbox: paint fog pillars into the *present* buffer only (H-Z8).
 * Never mutate g.fb — otherwise fog sticks until the next full playfield blit
 * (camera move) after the map closes.
 */
static uint8_t g_zoom_fog[4];
static int g_zoom_have_fog;

static void compose_zoom_visibility_pillars(uint8_t *dst, int dst_w, const uint8_t *src, int sw,
                                           int sh, int ox, int oy, int dw, int dh)
{
    int pL, pT, pR, pB, mL, mT, mR, mB;
    int y, x, sx, sy;
    uint8_t fog[4];

    if (!dst || !src || sw <= 0 || sh <= 0 || dst_w < 1 || dw < 1 || dh < 1)
        return;
    if (!ck_zoom_letterbox_get(&pL, &pT, &pR, &pB, &mL, &mT, &mR, &mB))
        return;
    if (mL <= pL && mR >= pR)
        return;
    if (pT < 0)
        pT = 0;
    if (pB >= sh)
        pB = sh - 1;
    if (mT < pT)
        mT = pT;
    if (mB > pB)
        mB = pB;
    if (mL < 0)
        mL = 0;
    if (mR >= sw)
        mR = sw - 1;
    sx = mL + (mR - mL) / 5;
    sy = mT + (mB - mT) / 5;
    if (sx >= 0 && sy >= 0 && sx < sw && sy < sh) {
        const uint8_t *s = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4u;
        if (s[0] > 40 && s[1] > 40 && s[2] > 40) {
            g_zoom_fog[0] = s[0];
            g_zoom_fog[1] = s[1];
            g_zoom_fog[2] = s[2];
            g_zoom_fog[3] = 255;
            g_zoom_have_fog = 1;
        }
    }
    if (!g_zoom_have_fog) {
        g_zoom_fog[0] = 74;
        g_zoom_fog[1] = 115;
        g_zoom_fog[2] = 122;
        g_zoom_fog[3] = 255;
        g_zoom_have_fog = 1;
    }
    fog[0] = g_zoom_fog[0];
    fog[1] = g_zoom_fog[1];
    fog[2] = g_zoom_fog[2];
    fog[3] = g_zoom_fog[3];
    for (y = pT; y <= pB; ++y) {
        int dy = oy + (int)(((long)y * dh) / sh);
        uint8_t *row;
        if (dy < oy || dy >= oy + dh)
            continue;
        row = dst + (size_t)dy * (size_t)dst_w * 4u;
        for (x = pL; x < mL && x <= pR; ++x) {
            int dx = ox + (int)(((long)x * dw) / sw);
            uint8_t *d;
            if (dx < ox || dx >= ox + dw)
                continue;
            d = row + (size_t)dx * 4u;
            d[0] = fog[0];
            d[1] = fog[1];
            d[2] = fog[2];
            d[3] = fog[3];
        }
        for (x = mR + 1; x <= pR && x < sw; ++x) {
            int dx = ox + (int)(((long)x * dw) / sw);
            uint8_t *d;
            if (dx < ox || dx >= ox + dw)
                continue;
            d = row + (size_t)dx * 4u;
            d[0] = fog[0];
            d[1] = fog[1];
            d[2] = fog[2];
            d[3] = fog[3];
        }
    }
    /* Tip drawn later in compose_zoom_tip. */
}

static void mirror_fill_row(uint8_t *fb, int soft_w, int y, int asset_w)
{
    int x;
    for (x = asset_w; x < soft_w; ++x) {
        int i = x - asset_w;
        int k = i % (CK_UI_MIRROR_SPAN * 2);
        int sx = (k < CK_UI_MIRROR_SPAN) ? (asset_w - 1 - k)
                                         : (asset_w - CK_UI_MIRROR_SPAN + (k - CK_UI_MIRROR_SPAN));
        uint8_t *d;
        const uint8_t *s;
        if (sx < 0)
            sx = 0;
        d = fb + ((size_t)y * (size_t)soft_w + (size_t)x) * 4u;
        s = fb + ((size_t)y * (size_t)soft_w + (size_t)sx) * 4u;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[3];
    }
}

/*
 * UI bars authored at 1600×N (InfoBar, editor InfoBar/CmdBar). Soft is 1920.
 * Mirror texture from x=1599 across the gap. Top band y∈[0,80). Bottom: only
 * rows where x=1590 is lit and x=1600 is dark (gameplay CmdBar Frame-stretches).
 */
static void extend_ui_bar_gaps(uint8_t *fb, int soft_w, int soft_h)
{
    const int asset_w = CK_UI_ASSET_W;
    const int top_h = 80;
    int y, bot_n = 0;

    if (!fb || soft_w <= asset_w + 8 || soft_h < top_h)
        return;

    for (y = 0; y < top_h; ++y)
        mirror_fill_row(fb, soft_w, y, asset_w);

    for (y = soft_h - 1; y >= soft_h - 100 && y >= top_h; --y) {
        int left_lit = ui_col_lit(fb, soft_w, asset_w - 10, y);
        int gap_dark = !ui_col_lit(fb, soft_w, asset_w, y);
        if (!left_lit) {
            if (bot_n > 0)
                break;
            continue;
        }
        if (!gap_dark)
            continue;
        mirror_fill_row(fb, soft_w, y, asset_w);
        bot_n++;
    }
}

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "vk_terrain.h"
#include "vk_decor.h"
#include "vk_obj.h"
#include "vk_iso_depth.h"
#include "vk_soft_overlay.h"
#include "ktx_obj.h"
#include "ktx_decor.h"

#define LOAD_VK(name)                                                                                  \
    do {                                                                                               \
        g.fn.name = (PFN_##name)g.vkGetInstanceProcAddr(NULL, #name);                                  \
        if (!g.fn.name && g.vkGetInstanceProcAddr)                                                     \
            g.fn.name = (PFN_##name)GetProcAddress(g.lib, #name);                                      \
    } while (0)

#define LOAD_IK(name)                                                                                  \
    do {                                                                                               \
        g.fn.name = (PFN_##name)g.vkGetInstanceProcAddr(g.instance, #name);                            \
    } while (0)

#define LOAD_DK(name)                                                                                  \
    do {                                                                                               \
        g.fn.name = (PFN_##name)g.vkGetDeviceProcAddr(g.device, #name);                                 \
    } while (0)

typedef int(WINAPI *PFN_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT,
                                          CONST VOID *, CONST BITMAPINFO *, UINT);

typedef struct {
    HMODULE lib;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

    struct {
        PFN_vkCreateInstance vkCreateInstance;
        PFN_vkDestroyInstance vkDestroyInstance;
        PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
        PFN_vkCreateDevice vkCreateDevice;
        PFN_vkDestroyDevice vkDestroyDevice;
        PFN_vkGetDeviceQueue vkGetDeviceQueue;
        PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
        PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
        PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
        PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
        PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
        PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
        PFN_vkQueuePresentKHR vkQueuePresentKHR;
        PFN_vkCreateCommandPool vkCreateCommandPool;
        PFN_vkDestroyCommandPool vkDestroyCommandPool;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
        PFN_vkEndCommandBuffer vkEndCommandBuffer;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
        PFN_vkCmdBlitImage vkCmdBlitImage;
        PFN_vkCmdClearColorImage vkCmdClearColorImage;
        PFN_vkCreateBuffer vkCreateBuffer;
        PFN_vkDestroyBuffer vkDestroyBuffer;
        PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
        PFN_vkCreateImage vkCreateImage;
        PFN_vkDestroyImage vkDestroyImage;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
        PFN_vkBindImageMemory vkBindImageMemory;
        PFN_vkAllocateMemory vkAllocateMemory;
        PFN_vkFreeMemory vkFreeMemory;
        PFN_vkBindBufferMemory vkBindBufferMemory;
        PFN_vkMapMemory vkMapMemory;
        PFN_vkUnmapMemory vkUnmapMemory;
        PFN_vkCreateFence vkCreateFence;
        PFN_vkDestroyFence vkDestroyFence;
        PFN_vkWaitForFences vkWaitForFences;
        PFN_vkResetFences vkResetFences;
        PFN_vkCreateSemaphore vkCreateSemaphore;
        PFN_vkDestroySemaphore vkDestroySemaphore;
        PFN_vkQueueSubmit vkQueueSubmit;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
        PFN_vkCreateImageView vkCreateImageView;
        PFN_vkDestroyImageView vkDestroyImageView;
    } fn;

    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t qfam;
    VkSurfaceKHR surface;
    HWND hwnd;
    HINSTANCE hinst;

    VkSwapchainKHR swapchain;
    VkFormat swap_fmt;
    VkExtent2D extent;
    VkImage *images;
    uint32_t image_count;

    VkCommandPool cmd_pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore img_avail;
    VkSemaphore render_done;

    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *staging_ptr;
    VkDeviceSize staging_size;

    /* GPU scale: soft-sized image + staging; blit LINEAR to swapchain letterbox. */
    VkImage soft_img;
    VkDeviceMemory soft_img_mem;
    VkImageView soft_view;
    int soft_view_local; /* 1 = created via present fn (not terrain) */
    int soft_img_w, soft_img_h;
    VkBuffer soft_upload;
    VkDeviceMemory soft_upload_mem;
    void *soft_upload_ptr;
    VkDeviceSize soft_upload_size;
    int gpu_want; /* -1 unset; CK_VK_GPU (default on) */
    int gpu_ok;
    int gpu_logged;

    uint8_t *fb; /* soft-DIB-sized BGRA; scaled to swapchain on present */
    int fb_w, fb_h;
    int soft_w, soft_h; /* last BMI size (logical game framebuffer) */
    int game_soft_w, game_soft_h; /* last non-cutscene soft (restore target) */
    int last_full_x, last_full_y;
    int have_full;
    int fb_bgra;
    int ready;
    int failed;
    int scale_logged;
    int force_letterbox; /* movie frames: keep source aspect (bars) */
    LONG presents;
    int dirty;
    DWORD last_present_tick;
    DWORD last_blit_ms;
    DWORD last_present_ms;
    VkPresentModeKHR present_mode;
    int present_mode_logged;
    /* One-shot content shift for this present only (does not mutate g.fb). */
    int pan_shift_x, pan_shift_y;

    /* Optional custom 565→RGBA LUT: 65536×4 bytes (CK_VK_LUT or ck_vk_lut565.rgba). */
    uint8_t *lut565;
    int lut_loaded;

    /* Cached GDI 32bpp converter (real gdi32, not IAT hook). */
    PFN_SetDIBitsToDevice gdi_setdi;
    HDC conv_dc;
    HBITMAP conv_bmp;
    HBITMAP conv_old;
    void *conv_bits;
    int conv_w, conv_h;
} VkPresentState;

static VkPresentState g;

static int env_truthy(const char *name, int def)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    const char *v = NULL;
    char c0, c1;
    if (n > 0 && n < sizeof(buf))
        v = buf;
    else
        v = getenv(name);
    if (!v || !v[0])
        return def;
    /* trim leading spaces */
    while (*v == ' ' || *v == '\t')
        ++v;
    c0 = v[0];
    c1 = v[1];
    if (c0 == '0' && (c1 == '\0' || c1 == ' ' || c1 == '\t'))
        return 0;
    if (c0 == 'f' || c0 == 'F' || c0 == 'n' || c0 == 'N' || c0 == 'o' || c0 == 'O')
        return 0; /* false / no / off */
    return 1;
}

/* Vulkan present always ON unless CK_GDI_FALLBACK=1.
 * CK_VK_REPLACE is ignored: Wine/PE often keeps a stale 0 from earlier A/B runs. */
static int vk_replace_wanted(void)
{
    return !env_truthy("CK_GDI_FALLBACK", 0);
}

/*
 * CK_FPS — optional present pacing. Default OFF (0).
 * Sleeping after present does NOT lock game FPS: retail measures FPS from
 * Tick deltas (~1s window at 0x5764e2), and scroll uses timer 0x19A87453 with
 * time-integrated steps (0x592536). Present rate ≠ Tick/GameSpeed/ScrollRefresh.
 */
static int ck_target_fps(void)
{
    static int s_fps = -2;
    char buf[32];
    DWORD n;
    int v;

    if (s_fps != -2)
        return s_fps;
    n = GetEnvironmentVariableA("CK_FPS", buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        s_fps = 0; /* default off — present lock ≠ game FPS */
        return s_fps;
    }
    if ((buf[0] == '0' || buf[0] == 'o' || buf[0] == 'O' || buf[0] == 'n' || buf[0] == 'N') &&
        buf[1] == '\0') {
        s_fps = 0;
        return 0;
    }
    v = atoi(buf);
    if (v < 0)
        v = 0;
    if (v > 240)
        v = 240;
    s_fps = v;
    return s_fps;
}

/* Wait so that (now - frame_start) >= 1000/CK_FPS. Returns ms spent waiting. */
static double pace_frame(LONGLONG frame_start_qpc)
{
    static int s_logged;
    static int s_period;
    int fps = ck_target_fps();
    double target_ms, elapsed, waited;
    LONGLONG t_wait0;

    if (fps <= 0 || frame_start_qpc <= 0)
        return 0.0;
    if (!s_period) {
        timeBeginPeriod(1);
        s_period = 1;
    }
    if (!s_logged) {
        s_logged = 1;
        log_msg("vk_present: FPS lock %d (CK_FPS, 0=off)", fps);
    }
    target_ms = 1000.0 / (double)fps;
    elapsed = hitch_qpc_ms_since(frame_start_qpc);
    if (elapsed >= target_ms)
        return 0.0;
    t_wait0 = hitch_qpc_now();
    for (;;) {
        elapsed = hitch_qpc_ms_since(frame_start_qpc);
        if (elapsed >= target_ms)
            break;
        if (target_ms - elapsed > 2.0)
            Sleep(1);
        else
            Sleep(0);
    }
    waited = hitch_qpc_ms_since(t_wait0);
    return waited > 0.0 ? waited : 0.0;
}

static uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    g.fn.vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

static void destroy_soft_gpu(void)
{
    if (!g.device)
        return;
    if (g.soft_upload_ptr) {
        g.fn.vkUnmapMemory(g.device, g.soft_upload_mem);
        g.soft_upload_ptr = NULL;
    }
    if (g.soft_upload)
        g.fn.vkDestroyBuffer(g.device, g.soft_upload, NULL);
    if (g.soft_upload_mem)
        g.fn.vkFreeMemory(g.device, g.soft_upload_mem, NULL);
    g.soft_upload = VK_NULL_HANDLE;
    g.soft_upload_mem = VK_NULL_HANDLE;
    g.soft_upload_size = 0;
    if (g.soft_img)
        g.fn.vkDestroyImage(g.device, g.soft_img, NULL);
    if (g.soft_img_mem)
        g.fn.vkFreeMemory(g.device, g.soft_img_mem, NULL);
    if (g.soft_view) {
        if (g.soft_view_local && g.fn.vkDestroyImageView)
            g.fn.vkDestroyImageView(g.device, g.soft_view, NULL);
        else
            vk_terrain_destroy_soft_view(g.soft_view);
        g.soft_view = VK_NULL_HANDLE;
        g.soft_view_local = 0;
    }
    g.soft_img = VK_NULL_HANDLE;
    g.soft_img_mem = VK_NULL_HANDLE;
    g.soft_img_w = g.soft_img_h = 0;
}

static int ensure_soft_view(void)
{
    VkImageViewCreateInfo vci;
    VkResult r;

    if (g.soft_view)
        return 1;
    if (!g.soft_img || !g.device || !g.fn.vkCreateImageView)
        return 0;
    if (vk_terrain_ready()) {
        g.soft_view = vk_terrain_create_soft_view(g.soft_img, g.swap_fmt);
        g.soft_view_local = 0;
        if (g.soft_view) {
            log_msg("vk_present: soft_view ok (terrain) %dx%d", g.soft_img_w, g.soft_img_h);
            return 1;
        }
    }
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = g.soft_img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = g.swap_fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = g.fn.vkCreateImageView(g.device, &vci, NULL, &g.soft_view);
    if (r != VK_SUCCESS || !g.soft_view) {
        log_msg("vk_present: soft_view create failed (local)");
        g.soft_view = VK_NULL_HANDLE;
        return 0;
    }
    g.soft_view_local = 1;
    log_msg("vk_present: soft_view ok (local/decor) %dx%d", g.soft_img_w, g.soft_img_h);
    return 1;
}

static void destroy_swapchain(void)
{
    uint32_t i;
    if (!g.device)
        return;
    g.fn.vkDeviceWaitIdle(g.device);
    destroy_soft_gpu();
    if (g.staging_ptr) {
        g.fn.vkUnmapMemory(g.device, g.staging_mem);
        g.staging_ptr = NULL;
    }
    if (g.staging)
        g.fn.vkDestroyBuffer(g.device, g.staging, NULL);
    if (g.staging_mem)
        g.fn.vkFreeMemory(g.device, g.staging_mem, NULL);
    g.staging = VK_NULL_HANDLE;
    g.staging_mem = VK_NULL_HANDLE;
    g.staging_size = 0;
    if (g.images) {
        free(g.images);
        g.images = NULL;
    }
    if (g.swapchain)
        g.fn.vkDestroySwapchainKHR(g.device, g.swapchain, NULL);
    g.swapchain = VK_NULL_HANDLE;
    g.image_count = 0;
    (void)i;
}

static int ensure_staging(VkDeviceSize need)
{
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkResult r;

    if (g.staging && g.staging_size >= need)
        return 1;
    if (g.staging_ptr) {
        g.fn.vkUnmapMemory(g.device, g.staging_mem);
        g.staging_ptr = NULL;
    }
    if (g.staging)
        g.fn.vkDestroyBuffer(g.device, g.staging, NULL);
    if (g.staging_mem)
        g.fn.vkFreeMemory(g.device, g.staging_mem, NULL);
    g.staging = VK_NULL_HANDLE;
    g.staging_mem = VK_NULL_HANDLE;

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    r = g.fn.vkCreateBuffer(g.device, &bci, NULL, &g.staging);
    if (r != VK_SUCCESS)
        return 0;
    g.fn.vkGetBufferMemoryRequirements(g.device, g.staging, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = g.fn.vkAllocateMemory(g.device, &mai, NULL, &g.staging_mem);
    if (r != VK_SUCCESS)
        return 0;
    g.fn.vkBindBufferMemory(g.device, g.staging, g.staging_mem, 0);
    r = g.fn.vkMapMemory(g.device, g.staging_mem, 0, need, 0, &g.staging_ptr);
    if (r != VK_SUCCESS)
        return 0;
    g.staging_size = need;
    return 1;
}

static int gpu_blit_wanted(void)
{
    /* Default ON: GPU blit scale/letterbox. CK_VK_GPU=0 → CPU compose_staging. */
    return env_truthy("CK_VK_GPU", 1);
}

static int ensure_soft_gpu(int w, int h)
{
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkBufferCreateInfo bci;
    VkResult r;
    VkDeviceSize need;
    int resized = 0;

    if (w < 1 || h < 1 || !g.device || !g.fn.vkCreateImage || !g.fn.vkCmdBlitImage)
        return 0;
    need = (VkDeviceSize)w * (VkDeviceSize)h * 4u;

    if (g.soft_img && g.soft_img_w == w && g.soft_img_h == h && g.soft_upload_ptr &&
        g.soft_upload_size >= need)
        return 1;

    if (g.soft_img && (g.soft_img_w != w || g.soft_img_h != h))
        resized = 1;

    if (g.soft_upload_ptr) {
        g.fn.vkUnmapMemory(g.device, g.soft_upload_mem);
        g.soft_upload_ptr = NULL;
    }
    if (g.soft_upload)
        g.fn.vkDestroyBuffer(g.device, g.soft_upload, NULL);
    if (g.soft_upload_mem)
        g.fn.vkFreeMemory(g.device, g.soft_upload_mem, NULL);
    g.soft_upload = VK_NULL_HANDLE;
    g.soft_upload_mem = VK_NULL_HANDLE;
    g.soft_upload_size = 0;
    if (g.soft_img)
        g.fn.vkDestroyImage(g.device, g.soft_img, NULL);
    if (g.soft_img_mem)
        g.fn.vkFreeMemory(g.device, g.soft_img_mem, NULL);
    if (g.soft_view) {
        if (g.soft_view_local && g.fn.vkDestroyImageView)
            g.fn.vkDestroyImageView(g.device, g.soft_view, NULL);
        else
            vk_terrain_destroy_soft_view(g.soft_view);
        g.soft_view = VK_NULL_HANDLE;
        g.soft_view_local = 0;
    }
    g.soft_img = VK_NULL_HANDLE;
    g.soft_img_mem = VK_NULL_HANDLE;

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = g.swap_fmt;
    ici.extent.width = (uint32_t)w;
    ici.extent.height = (uint32_t)h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = g.fn.vkCreateImage(g.device, &ici, NULL, &g.soft_img);
    if (r != VK_SUCCESS)
        return 0;
    g.fn.vkGetImageMemoryRequirements(g.device, g.soft_img, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = g.fn.vkAllocateMemory(g.device, &mai, NULL, &g.soft_img_mem);
    if (r != VK_SUCCESS)
        return 0;
    g.fn.vkBindImageMemory(g.device, g.soft_img, g.soft_img_mem, 0);
    g.soft_img_w = w;
    g.soft_img_h = h;
    /* soft_view created lazily in present (terrain and/or KTX decor). */

    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    r = g.fn.vkCreateBuffer(g.device, &bci, NULL, &g.soft_upload);
    if (r != VK_SUCCESS)
        return 0;
    g.fn.vkGetBufferMemoryRequirements(g.device, g.soft_upload, &req);
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = g.fn.vkAllocateMemory(g.device, &mai, NULL, &g.soft_upload_mem);
    if (r != VK_SUCCESS)
        return 0;
    g.fn.vkBindBufferMemory(g.device, g.soft_upload, g.soft_upload_mem, 0);
    r = g.fn.vkMapMemory(g.device, g.soft_upload_mem, 0, need, 0, &g.soft_upload_ptr);
    if (r != VK_SUCCESS)
        return 0;
    g.soft_upload_size = need;
    if (resized)
        tip_font_present_invalidate();
    return 1;
}

/* Compute letterbox/fill geometry. Returns 1 and fills ox,oy,dw,dh. scale_off→1:1 top-left. */
static int compose_geom(uint32_t cw, uint32_t ch, int sw, int sh, int *ox, int *oy, int *dw,
                        int *dh, int *letterbox_out, int *scale_off_out, int *bilinear_out)
{
    char mode[32];
    DWORD n;
    int letterbox = 0;
    int scale_off = 0;

    n = GetEnvironmentVariableA("CK_VK_SCALE", mode, (DWORD)sizeof(mode));
    if (n > 0 && n < sizeof(mode)) {
        if (mode[0] == '0' && mode[1] == '\0')
            scale_off = 1;
        else if (mode[0] == 'l' || mode[0] == 'L' || mode[0] == 'f' || mode[0] == 'F')
            letterbox = 1;
    }
    if (g.force_letterbox && !scale_off)
        letterbox = 1;

    if (scale_off || (sw == (int)cw && sh == (int)ch)) {
        *dw = sw < (int)cw ? sw : (int)cw;
        *dh = sh < (int)ch ? sh : (int)ch;
        *ox = 0;
        *oy = 0;
        if (letterbox_out)
            *letterbox_out = 0;
        if (scale_off_out)
            *scale_off_out = scale_off;
        if (bilinear_out)
            *bilinear_out = 0;
        return 1;
    }
    if (letterbox) {
        if (sw * (int)ch <= sh * (int)cw) {
            *dh = (int)ch;
            *dw = sw * (int)ch / sh;
        } else {
            *dw = (int)cw;
            *dh = sh * (int)cw / sw;
        }
        if (*dw < 1)
            *dw = 1;
        if (*dh < 1)
            *dh = 1;
        *ox = ((int)cw - *dw) / 2;
        *oy = ((int)ch - *dh) / 2;
    } else {
        *dw = (int)cw;
        *dh = (int)ch;
        *ox = 0;
        *oy = 0;
    }
    if (letterbox_out)
        *letterbox_out = letterbox;
    if (scale_off_out)
        *scale_off_out = 0;
    if (bilinear_out)
        *bilinear_out = (*dw > sw || *dh > sh);
    return 1;
}

/* Pack g.fb (+ pillars/tip/FT) into soft_upload for GPU blit. */
static void compose_zoom_tip(uint8_t *dst, int dst_w, int dst_h, int sw, int sh, int ox, int oy,
                             int dw, int dh);

static void pack_soft_upload(int sw, int sh)
{
    uint8_t *dst = (uint8_t *)g.soft_upload_ptr;
    const uint8_t *src = g.fb;
    size_t npix = (size_t)sw * (size_t)sh;
    int bgra = (g.swap_fmt == VK_FORMAT_B8G8R8A8_UNORM ||
                g.swap_fmt == VK_FORMAT_B8G8R8A8_SRGB);
    size_t i;

    if (!dst || !src)
        return;
    /* Present-time playfield shift removed: seams at InfoBar/CmdBar/right HUD
     * (user: artifacts top/bottom/right). g.pan_shift_* ignored. */
    (void)g.pan_shift_x;
    (void)g.pan_shift_y;
    if (bgra)
        memcpy(dst, src, npix * 4u);
    else {
        for (i = 0; i < npix; ++i) {
            dst[i * 4 + 0] = src[i * 4 + 2];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 0];
            dst[i * 4 + 3] = 255;
        }
    }

    /*
     * Wipe soft playfield under GPU land so leftover soft MapObj/decor never show
     * through LOAD_OP_LOAD. Tips/pillars are composed AFTER the wipe.
     */
    {
        int has_map = vk_terrain_has_map();
        int bare = has_map && vk_terrain_draw_enabled() && vk_terrain_soft_land_disabled();
        int y0 = 80, y1 = sh - 54;
        int x, y, cyan = 0, green = 0, dark = 0, op = 0;
        int step = 4;
        int did_clear = 0;
        static DWORD s_last_pf;
        DWORD now = GetTickCount();

        if (y1 <= y0)
            y1 = sh;

        /* #region agent log */
        if (now - s_last_pf > 1500u && sw > 200 && sh > 200) {
            int soft_pf, lit = 0, mid = 0;
            s_last_pf = now;
            for (y = y0; y < y1; y += step) {
                for (x = 0; x < sw; x += step) {
                    size_t o = ((size_t)y * (size_t)sw + (size_t)x) * 4u;
                    int r = dst[o + (bgra ? 2 : 0)];
                    int gch = dst[o + 1];
                    int b = dst[o + (bgra ? 0 : 2)];
                    op++;
                    if (b > r + 25 && b > gch + 10 && b > 110)
                        cyan++;
                    else if (gch > r + 15 && gch > b + 15 && gch > 60)
                        green++;
                    else if (r < 40 && gch < 40 && b < 40)
                        dark++;
                    else if (r > 80 || gch > 80 || b > 80)
                        lit++;
                    else
                        mid++;
                }
            }
            soft_pf = env_on("CK_GPU_SOFT_PLAYFIELD", 0);
            {
                char data[320];
                snprintf(data, sizeof(data),
                         "{\"bare\":%d,\"cleared\":%d,\"soft_pf\":%d,\"has_map\":%d,"
                         "\"cyan\":%d,\"green\":%d,\"dark\":%d,\"lit\":%d,\"mid\":%d,\"n\":%d,"
                         "\"gpu_decor\":%d,\"gpu_obj\":%d,\"soft_land_off\":%d,\"wh\":[%d,%d]}",
                         bare ? 1 : 0, bare ? 1 : 0, soft_pf, has_map, cyan, green, dark, lit, mid,
                         op, ktx_decor_wanted() ? 1 : 0, ktx_obj_wanted() ? 1 : 0,
                         vk_terrain_soft_land_disabled() ? 1 : 0, sw, sh);
                hooks_agent("H-D", "vk_present.c:pack_soft_upload", "soft-pf-stats", data);
            }
        }
        /* #endregion */

        if (bare && y1 > y0 && sw > 0) {
            for (y = y0; y < y1; ++y)
                memset(dst + (size_t)y * (size_t)sw * 4u, 0, (size_t)sw * 4u);
            did_clear = 1;
        }
        (void)did_clear;
    }

    /* Soft-space overlays (after PF wipe so tips survive). */
    compose_zoom_visibility_pillars(dst, sw, src, sw, sh, 0, 0, sw, sh);
    compose_zoom_tip(dst, sw, sh, sw, sh, 0, 0, sw, sh);
    tip_font_present_flush(dst, sw, sh, sw, sh, 0, 0, sw, sh);
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR *fmts = NULL;
    VkPresentModeKHR *modes = NULL;
    uint32_t nfmt = 0, nmode = 0, i;
    VkSurfaceFormatKHR chosen;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR sci;
    VkResult r;
    int have_mailbox = 0, have_immediate = 0, have_fifo = 0;
    char mode_pref[32];

    mode_pref[0] = '\0';
    g.fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    g.fn.vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, NULL);
    if (!nfmt)
        return 0;
    fmts = (VkSurfaceFormatKHR *)malloc(sizeof(*fmts) * nfmt);
    g.fn.vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, fmts);
    chosen = fmts[0];
    for (i = 0; i < nfmt; ++i) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = fmts[i];
            break;
        }
    }
    free(fmts);
    g.swap_fmt = chosen.format;

    g.fn.vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &nmode, NULL);
    if (nmode) {
        modes = (VkPresentModeKHR *)malloc(sizeof(*modes) * nmode);
        g.fn.vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &nmode, modes);
        for (i = 0; i < nmode; ++i) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                have_mailbox = 1;
            else if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                have_immediate = 1;
            else if (modes[i] == VK_PRESENT_MODE_FIFO_KHR)
                have_fifo = 1;
        }
        /* Default: MAILBOX > FIFO (prior behavior). CK_VK_PRESENT overrides.
         * Prefer IMMEDIATE only when explicitly requested — uncapped path for 60fps tests. */
        GetEnvironmentVariableA("CK_VK_PRESENT", mode_pref, (DWORD)sizeof(mode_pref));
        if (mode_pref[0] == 'i' || mode_pref[0] == 'I') {
            if (have_immediate)
                present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (have_mailbox)
                present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (mode_pref[0] == 'm' || mode_pref[0] == 'M') {
            if (have_mailbox)
                present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (mode_pref[0] == 'f' || mode_pref[0] == 'F') {
            present_mode = VK_PRESENT_MODE_FIFO_KHR;
        } else if (have_mailbox) {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else {
            present_mode = VK_PRESENT_MODE_FIFO_KHR;
        }
        free(modes);
    }
    g.present_mode = present_mode;

    if (caps.currentExtent.width != 0xFFFFFFFFu)
        g.extent = caps.currentExtent;
    else {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        g.extent.width = (uint32_t)(rc.right - rc.left);
        g.extent.height = (uint32_t)(rc.bottom - rc.top);
        if (g.extent.width < caps.minImageExtent.width)
            g.extent.width = caps.minImageExtent.width;
        if (g.extent.height < caps.minImageExtent.height)
            g.extent.height = caps.minImageExtent.height;
        if (g.extent.width > caps.maxImageExtent.width)
            g.extent.width = caps.maxImageExtent.width;
        if (g.extent.height > caps.maxImageExtent.height)
            g.extent.height = caps.maxImageExtent.height;
    }
    if (g.extent.width == 0 || g.extent.height == 0)
        return 0;

    /* #region agent log */
    if (!g.present_mode_logged) {
        char js[240];
        snprintf(js, sizeof(js),
                 "{\"pm\":%d,\"have\":{\"fifo\":%d,\"mailbox\":%d,\"immediate\":%d},"
                 "\"extent\":[%u,%u],\"pref\":\"%.16s\"}",
                 (int)present_mode, have_fifo, have_mailbox, have_immediate, g.extent.width,
                 g.extent.height, mode_pref[0] ? mode_pref : "auto");
        hooks_agent("H-FPS", "vk_present.c:create_swapchain", "present-mode", js);
        g.present_mode_logged = 1;
        log_msg("vk present_mode=%d (fifo=%d mailbox=%d immediate=%d)", (int)present_mode,
                have_fifo, have_mailbox, have_immediate);
    }
    /* #endregion */

    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = g.surface;
    sci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
        sci.minImageCount = caps.maxImageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = g.extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;
    r = g.fn.vkCreateSwapchainKHR(g.device, &sci, NULL, &g.swapchain);
    if (r != VK_SUCCESS)
        return 0;

    g.fn.vkGetSwapchainImagesKHR(g.device, g.swapchain, &g.image_count, NULL);
    g.images = (VkImage *)calloc(g.image_count, sizeof(VkImage));
    g.fn.vkGetSwapchainImagesKHR(g.device, g.swapchain, &g.image_count, g.images);

    if (!ensure_staging((VkDeviceSize)g.extent.width * g.extent.height * 4))
        return 0;
    return 1;
}

static int ensure_fb(int w, int h)
{
    size_t n;
    if (w <= 0 || h <= 0)
        return 0;
    if (g.fb && g.fb_w == w && g.fb_h == h)
        return 1;
    free(g.fb);
    n = (size_t)w * (size_t)h * 4u;
    g.fb = (uint8_t *)malloc(n);
    if (!g.fb)
        return 0;
    memset(g.fb, 0, n);
    g.fb_w = w;
    g.fb_h = h;
    g.fb_bgra = 1;
    g.have_full = 0;
    tip_font_present_invalidate();
    return 1;
}

/* Stretch soft fb into swapchain staging. Default: fill (may distort 5:4→16:9).
 * CK_VK_SCALE=letterbox|fit keeps aspect; CK_VK_SCALE=0 disables (1:1 top-left).
 * Upscale uses bilinear (nearest looks like "cubes" at 640→1080). */
static void sample_bgra_bilinear(const uint8_t *src, int sw, int sh, int fx, int fy, uint8_t *out)
{
    int x0 = fx >> 8;
    int y0 = fy >> 8;
    int tx = fx & 255;
    int ty = fy & 255;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    const uint8_t *p00, *p10, *p01, *p11;
    int i;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x0 >= sw)
        x0 = sw - 1;
    if (y0 >= sh)
        y0 = sh - 1;
    if (x1 >= sw)
        x1 = sw - 1;
    if (y1 >= sh)
        y1 = sh - 1;
    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;

    p00 = src + ((size_t)y0 * (size_t)sw + (size_t)x0) * 4u;
    p10 = src + ((size_t)y0 * (size_t)sw + (size_t)x1) * 4u;
    p01 = src + ((size_t)y1 * (size_t)sw + (size_t)x0) * 4u;
    p11 = src + ((size_t)y1 * (size_t)sw + (size_t)x1) * 4u;
    for (i = 0; i < 4; ++i) {
        int a = p00[i] + (((p10[i] - p00[i]) * tx) >> 8);
        int b = p01[i] + (((p11[i] - p01[i]) * tx) >> 8);
        out[i] = (uint8_t)(a + (((b - a) * ty) >> 8));
    }
}

/* ZoomMap tip: stb_truetype oversampled AA into present staging. */
static void compose_zoom_tip(uint8_t *dst, int dst_w, int dst_h, int sw, int sh, int ox, int oy,
                             int dw, int dh)
{
    char tip[256];
    int tip_x = 0, tip_y = 0, dx, dy;
    int pL, pT, pR, pB, mL, mT, mR, mB;

    tip[0] = 0;
    if (!dst || sw <= 0 || sh <= 0 || dst_w < 1 || dst_h < 1 || dw < 1 || dh < 1)
        return;
    if (!ck_zoom_letterbox_get(&pL, &pT, &pR, &pB, &mL, &mT, &mR, &mB))
        return;
    if (mL <= pL && mR >= pR)
        return;
    if (!ck_zoom_tip_text_get(tip, (int)sizeof(tip), &tip_x, &tip_y) || !tip[0])
        return;
    tip[sizeof(tip) - 1] = 0;

    dx = ox + (int)(((long)tip_x * dw) / sw);
    dy = oy + (int)(((long)tip_y * dh) / sh);
    if (dx < ox)
        dx = ox;
    if (dy < oy)
        dy = oy;
    tip_font_draw_utf8(dst, dst_w, dst_h, dx, dy, tip, NULL, NULL);
    (void)pT;
    (void)pB;
}

static void compose_staging(uint32_t cw, uint32_t ch)
{
    int sw = g.fb_w, sh = g.fb_h;
    uint8_t *dst = (uint8_t *)g.staging_ptr;
    const uint8_t *src = g.fb;
    char mode[32];
    DWORD n;
    int letterbox = 0;
    int scale_off = 0;
    int dw, dh, ox, oy;
    int y;
    int use_bilinear = 0;

    n = GetEnvironmentVariableA("CK_VK_SCALE", mode, (DWORD)sizeof(mode));
    if (n > 0 && n < sizeof(mode)) {
        if (mode[0] == '0' && mode[1] == '\0')
            scale_off = 1;
        else if (mode[0] == 'l' || mode[0] == 'L' || mode[0] == 'f' || mode[0] == 'F')
            letterbox = 1;
    }
    /* Cutscenes: always preserve aspect unless CK_VK_SCALE=0. */
    if (g.force_letterbox && !scale_off)
        letterbox = 1;

    if (!src || sw <= 0 || sh <= 0) {
        memset(dst, 0, (size_t)cw * ch * 4u);
        return;
    }

    if (scale_off || (sw == (int)cw && sh == (int)ch)) {
        if (sw == (int)cw && sh == (int)ch) {
            memcpy(dst, src, (size_t)cw * ch * 4u);
            compose_zoom_visibility_pillars(dst, (int)cw, src, sw, sh, 0, 0, sw, sh);
            compose_zoom_tip(dst, (int)cw, (int)ch, sw, sh, 0, 0, sw, sh);
            tip_font_present_flush(dst, (int)cw, (int)ch, sw, sh, 0, 0, sw, sh);
        } else {
            memset(dst, 0, (size_t)cw * ch * 4u);
            dh = sh < (int)ch ? sh : (int)ch;
            dw = sw < (int)cw ? sw : (int)cw;
            for (y = 0; y < dh; ++y)
                memcpy(dst + (size_t)y * cw * 4u, src + (size_t)y * (size_t)sw * 4u,
                       (size_t)dw * 4u);
            compose_zoom_visibility_pillars(dst, (int)cw, src, sw, sh, 0, 0, dw, dh);
            compose_zoom_tip(dst, (int)cw, (int)ch, sw, sh, 0, 0, dw, dh);
            tip_font_present_flush(dst, (int)cw, (int)ch, sw, sh, 0, 0, dw, dh);
        }
        return;
    }

    memset(dst, 0, (size_t)cw * ch * 4u);
    if (letterbox) {
        if (sw * (int)ch <= sh * (int)cw) {
            dh = (int)ch;
            dw = sw * (int)ch / sh;
        } else {
            dw = (int)cw;
            dh = sh * (int)cw / sw;
        }
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        ox = ((int)cw - dw) / 2;
        oy = ((int)ch - dh) / 2;
    } else {
        dw = (int)cw;
        dh = (int)ch;
        ox = 0;
        oy = 0;
    }

    /* Nearest-neighbour upscale makes large identical squares ("cubes"). */
    use_bilinear = (dw > sw || dh > sh);

    if (use_bilinear) {
        for (y = 0; y < dh; ++y) {
            int x;
            int fy = ((y * 2 + 1) * sh * 128) / dh - 128;
            uint8_t *row = dst + ((size_t)(oy + y) * cw + (size_t)ox) * 4u;
            for (x = 0; x < dw; ++x) {
                int fx = ((x * 2 + 1) * sw * 128) / dw - 128;
                sample_bgra_bilinear(src, sw, sh, fx, fy, row + (size_t)x * 4u);
            }
        }
    } else {
        for (y = 0; y < dh; ++y) {
            int sy = y * sh / dh;
            int x;
            uint8_t *row = dst + ((size_t)(oy + y) * cw + (size_t)ox) * 4u;
            const uint8_t *srow = src + (size_t)sy * (size_t)sw * 4u;
            for (x = 0; x < dw; ++x) {
                int sx = x * sw / dw;
                memcpy(row + (size_t)x * 4u, srow + (size_t)sx * 4u, 4);
            }
        }
    }

    compose_zoom_visibility_pillars(dst, (int)cw, src, sw, sh, ox, oy, dw, dh);
    compose_zoom_tip(dst, (int)cw, (int)ch, sw, sh, ox, oy, dw, dh);
    tip_font_present_flush(dst, (int)cw, (int)ch, sw, sh, ox, oy, dw, dh);

    if (!g.scale_logged) {
        g.scale_logged = 1;
        log_msg("vk_present: scale soft %dx%d -> swap %ux%u dst %dx%d origin %d,%d lb=%d bil=%d",
                sw, sh, cw, ch, dw, dh, ox, oy, letterbox, use_bilinear);
        /* #region agent log */
        {
            FILE *df = vk_agent_open();
            if (df) {
                fprintf(df,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"scale1\",\"hypothesisId\":\"H-SCALE\","
                        "\"location\":\"vk_present.c:compose_staging\",\"message\":\"vk-scale\","
                        "\"data\":{\"soft\":[%d,%d],\"swap\":[%u,%u],\"dst\":[%d,%d],\"origin\":[%d,%d],"
                        "\"lb\":%d,\"bilinear\":%d,\"scale_off\":%d},"
                        "\"timestamp\":%lu}\n",
                        sw, sh, cw, ch, dw, dh, ox, oy, letterbox, use_bilinear, scale_off,
                        (unsigned long)GetTickCount());
                fclose(df);
            }
        }
        /* #endregion */
    }
}


static int vk_init(HWND hwnd)
{
    const char *iexts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkApplicationInfo ai;
    VkInstanceCreateInfo ici;
    VkWin32SurfaceCreateInfoKHR sci;
    uint32_t ndev = 0, i, nq = 0;
    VkPhysicalDevice *devs = NULL;
    VkDeviceQueueCreateInfo qci;
    VkDeviceCreateInfo dci;
    float prio = 1.f;
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cai;
    VkFenceCreateInfo fci;
    VkSemaphoreCreateInfo sei;
    VkResult r;

    if (g.ready)
        return 1;
    if (g.failed)
        return 0;

    g.lib = LoadLibraryA("vulkan-1.dll");
    if (!g.lib) {
        log_msg("vk_present: LoadLibrary vulkan-1.dll failed (%lu)", GetLastError());
        g.failed = 1;
        return 0;
    }
    g.vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)GetProcAddress(g.lib, "vkGetInstanceProcAddr");
    if (!g.vkGetInstanceProcAddr) {
        g.failed = 1;
        return 0;
    }

    LOAD_VK(vkCreateInstance);
    if (!g.fn.vkCreateInstance) {
        g.failed = 1;
        return 0;
    }

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "CelticKings-vk-replace";
    ai.apiVersion = VK_API_VERSION_1_0;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iexts;
    r = g.fn.vkCreateInstance(&ici, NULL, &g.instance);
    if (r != VK_SUCCESS) {
        log_msg("vk_present: vkCreateInstance=%d", (int)r);
        g.failed = 1;
        return 0;
    }

    g.vkGetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g.vkGetInstanceProcAddr(g.instance, "vkGetDeviceProcAddr");
    LOAD_IK(vkDestroyInstance);
    LOAD_IK(vkEnumeratePhysicalDevices);
    LOAD_IK(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_IK(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_IK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_IK(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_IK(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_IK(vkGetPhysicalDeviceMemoryProperties);
    LOAD_IK(vkCreateDevice);
    LOAD_IK(vkDestroyDevice);
    LOAD_IK(vkGetDeviceQueue);
    LOAD_IK(vkCreateWin32SurfaceKHR);
    LOAD_IK(vkDestroySurfaceKHR);

    g.hwnd = hwnd;
    g.hinst = (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE);
    if (!g.hinst)
        g.hinst = GetModuleHandleA(NULL);

    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = g.hinst;
    sci.hwnd = hwnd;
    r = g.fn.vkCreateWin32SurfaceKHR(g.instance, &sci, NULL, &g.surface);
    if (r != VK_SUCCESS) {
        g.failed = 1;
        return 0;
    }

    g.fn.vkEnumeratePhysicalDevices(g.instance, &ndev, NULL);
    if (!ndev) {
        g.failed = 1;
        return 0;
    }
    devs = (VkPhysicalDevice *)calloc(ndev, sizeof(*devs));
    g.fn.vkEnumeratePhysicalDevices(g.instance, &ndev, devs);
    g.phys = VK_NULL_HANDLE;
    for (i = 0; i < ndev && !g.phys; ++i) {
        VkQueueFamilyProperties *qps;
        g.fn.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        qps = (VkQueueFamilyProperties *)calloc(nq, sizeof(*qps));
        g.fn.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qps);
        {
            uint32_t q;
            for (q = 0; q < nq; ++q) {
                VkBool32 support = VK_FALSE;
                if (!(qps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    continue;
                g.fn.vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, g.surface, &support);
                if (support) {
                    g.phys = devs[i];
                    g.qfam = q;
                    break;
                }
            }
        }
        free(qps);
    }
    free(devs);
    if (!g.phys) {
        g.failed = 1;
        return 0;
    }

    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    {
        /* Needed to sample BC1/BC3 KTX terrain on the GPU path. */
        static VkPhysicalDeviceFeatures feats;
        memset(&feats, 0, sizeof(feats));
        feats.textureCompressionBC = VK_TRUE;
        dci.pEnabledFeatures = &feats;
    }
    r = g.fn.vkCreateDevice(g.phys, &dci, NULL, &g.device);
    if (r != VK_SUCCESS) {
        g.failed = 1;
        return 0;
    }
    g.fn.vkGetDeviceQueue(g.device, g.qfam, 0, &g.queue);

    LOAD_DK(vkCreateSwapchainKHR);
    LOAD_DK(vkDestroySwapchainKHR);
    LOAD_DK(vkGetSwapchainImagesKHR);
    LOAD_DK(vkAcquireNextImageKHR);
    LOAD_DK(vkQueuePresentKHR);
    LOAD_DK(vkCreateCommandPool);
    LOAD_DK(vkDestroyCommandPool);
    LOAD_DK(vkAllocateCommandBuffers);
    LOAD_DK(vkBeginCommandBuffer);
    LOAD_DK(vkEndCommandBuffer);
    LOAD_DK(vkCmdPipelineBarrier);
    LOAD_DK(vkCmdCopyBufferToImage);
    LOAD_DK(vkCmdBlitImage);
    LOAD_DK(vkCmdClearColorImage);
    LOAD_DK(vkCreateBuffer);
    LOAD_DK(vkDestroyBuffer);
    LOAD_DK(vkGetBufferMemoryRequirements);
    LOAD_DK(vkCreateImage);
    LOAD_DK(vkDestroyImage);
    LOAD_DK(vkGetImageMemoryRequirements);
    LOAD_DK(vkBindImageMemory);
    LOAD_DK(vkAllocateMemory);
    LOAD_DK(vkFreeMemory);
    LOAD_DK(vkBindBufferMemory);
    LOAD_DK(vkMapMemory);
    LOAD_DK(vkUnmapMemory);
    LOAD_DK(vkCreateFence);
    LOAD_DK(vkDestroyFence);
    LOAD_DK(vkWaitForFences);
    LOAD_DK(vkResetFences);
    LOAD_DK(vkCreateSemaphore);
    LOAD_DK(vkDestroySemaphore);
    LOAD_DK(vkQueueSubmit);
    LOAD_DK(vkDeviceWaitIdle);
    LOAD_DK(vkCreateImageView);
    LOAD_DK(vkDestroyImageView);
    LOAD_DK(vkDestroyDevice);

    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = g.qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    g.fn.vkCreateCommandPool(g.device, &pci, NULL, &g.cmd_pool);
    memset(&cai, 0, sizeof(cai));
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g.cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    g.fn.vkAllocateCommandBuffers(g.device, &cai, &g.cmd);
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    g.fn.vkCreateFence(g.device, &fci, NULL, &g.fence);
    memset(&sei, 0, sizeof(sei));
    sei.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    g.fn.vkCreateSemaphore(g.device, &sei, NULL, &g.img_avail);
    g.fn.vkCreateSemaphore(g.device, &sei, NULL, &g.render_done);

    if (!create_swapchain()) {
        g.failed = 1;
        return 0;
    }

    if (vk_terrain_wanted()) {
        vk_terrain_note_device(g.device, g.phys, g.queue, g.qfam, g.swap_fmt, g.vkGetDeviceProcAddr,
                               g.vkGetInstanceProcAddr, g.instance);
        vk_decor_note_device(g.device, g.phys, g.queue, g.qfam, g.swap_fmt, g.vkGetDeviceProcAddr,
                             g.vkGetInstanceProcAddr, g.instance);
        vk_obj_note_device(g.device, g.phys, g.queue, g.qfam, g.swap_fmt, g.vkGetDeviceProcAddr,
                           g.vkGetInstanceProcAddr, g.instance);
        vk_iso_depth_note_device(g.device, g.phys, g.vkGetDeviceProcAddr, g.vkGetInstanceProcAddr,
                                 g.instance);
        vk_soft_overlay_note_device(g.device, g.phys, g.queue, g.qfam, g.swap_fmt,
                                    g.vkGetDeviceProcAddr, g.vkGetInstanceProcAddr, g.instance);
        log_msg("vk_present: vk_terrain noted (AUTO=%s F8 toggles)",
                env_on("CK_GPU_TERRAIN_AUTO", 1) ? "on" : "off");
    } else {
        log_msg("vk_present: vk_terrain skipped (OVERPAINT/BLEND off)");
    }

    g.ready = 1;
    log_msg("vk_present: ready hwnd=%p extent=%ux%u fmt=%d", (void *)hwnd, g.extent.width,
            g.extent.height, (int)g.swap_fmt);
    return 1;
}

static void pix16_to_rgba(uint16_t p, uint32_t rm, uint32_t gm, uint32_t bm, int bi_rgb,
                          int allow_lut, uint8_t *o)
{
    uint32_t r, gcol, b;
    if (allow_lut && g.lut565) {
        memcpy(o, g.lut565 + (size_t)p * 4u, 4);
        return;
    }
    if (rm == 0x7C00 && gm == 0x03E0 && bm == 0x001F) {
        r = (p >> 10) & 31;
        gcol = (p >> 5) & 31;
        b = p & 31;
        o[0] = (uint8_t)((r * 255) / 31);
        o[1] = (uint8_t)((gcol * 255) / 31);
        o[2] = (uint8_t)((b * 255) / 31);
    } else if (rm == 0xF800 && gm == 0x07E0 && bm == 0x001F) {
        r = (p >> 11) & 31;
        gcol = (p >> 5) & 63;
        b = p & 31;
        o[0] = (uint8_t)((r * 255) / 31);
        o[1] = (uint8_t)((gcol * 255) / 63);
        o[2] = (uint8_t)((b * 255) / 31);
    } else if (rm == 0x001F && gm == 0x07E0 && bm == 0xF800) {
        b = (p >> 11) & 31;
        gcol = (p >> 5) & 63;
        r = p & 31;
        o[0] = (uint8_t)((r * 255) / 31);
        o[1] = (uint8_t)((gcol * 255) / 63);
        o[2] = (uint8_t)((b * 255) / 31);
    } else if (bi_rgb || (!rm && !gm && !bm)) {
        /* BI_RGB 16bpp = RGB555 (log evidence: comp=0, masks=0) */
        r = (p >> 10) & 31;
        gcol = (p >> 5) & 31;
        b = p & 31;
        o[0] = (uint8_t)((r * 255) / 31);
        o[1] = (uint8_t)((gcol * 255) / 31);
        o[2] = (uint8_t)((b * 255) / 31);
    } else {
        r = (p >> 11) & 31;
        gcol = (p >> 5) & 63;
        b = p & 31;
        o[0] = (uint8_t)((r * 255) / 31);
        o[1] = (uint8_t)((gcol * 255) / 63);
        o[2] = (uint8_t)((b * 255) / 31);
    }
    o[3] = 255;
}

static void try_load_lut(void)
{
    char auto_path[MAX_PATH];
    const char *candidates[4];
    int n = 0, ci;
    FILE *f = NULL;
    long sz;
    const char *used = NULL;

    if (g.lut_loaded)
        return;
    g.lut_loaded = 1;

    if (getenv("CK_VK_LUT") && getenv("CK_VK_LUT")[0])
        candidates[n++] = getenv("CK_VK_LUT");
    if (getenv("CK_VK_LUT_UNIX") && getenv("CK_VK_LUT_UNIX")[0])
        candidates[n++] = getenv("CK_VK_LUT_UNIX");

    {
        char mod[MAX_PATH];
        DWORD m = GetModuleFileNameA(NULL, mod, MAX_PATH);
        if (m && m < MAX_PATH) {
            char *slash = strrchr(mod, '\\');
            if (!slash)
                slash = strrchr(mod, '/');
            if (slash) {
                slash[1] = '\0';
                snprintf(auto_path, sizeof(auto_path), "%sck_vk_lut565.rgba", mod);
                candidates[n++] = auto_path;
            }
        }
    }
    candidates[n++] = "ck_vk_lut565.rgba";
    candidates[n++] =
        "Z:\\home\\cybernetik\\Games\\Imperivm\\Imperivm\\ck_vk_lut565.rgba";

    for (ci = 0; ci < n; ++ci) {
        f = fopen(candidates[ci], "rb");
        if (f) {
            used = candidates[ci];
            break;
        }
    }
    if (!f) {
        /* Built-in RGB555→RGBA LUT (256KB). Matches manual expand in blit loop. */
        unsigned p;
        g.lut565 = (uint8_t *)malloc(65536u * 4u);
        if (!g.lut565)
            return;
        for (p = 0; p < 65536u; ++p) {
            unsigned r = (p >> 10) & 31u;
            unsigned gc = (p >> 5) & 31u;
            unsigned b = p & 31u;
            uint8_t *o = g.lut565 + (size_t)p * 4u;
            o[0] = (uint8_t)((r * 255u) / 31u);
            o[1] = (uint8_t)((gc * 255u) / 31u);
            o[2] = (uint8_t)((b * 255u) / 31u);
            o[3] = 255;
        }
        log_msg("vk_present: built-in RGB555 LUT ready");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != 65536L * 4L) {
        fclose(f);
        return;
    }
    g.lut565 = (uint8_t *)malloc((size_t)sz);
    if (!g.lut565 || fread(g.lut565, 1, (size_t)sz, f) != (size_t)sz) {
        free(g.lut565);
        g.lut565 = NULL;
        fclose(f);
        return;
    }
    fclose(f);
    log_msg("vk_present: custom LUT loaded from %s", used);
}

static void destroy_conv(void)
{
    if (g.conv_dc) {
        if (g.conv_old)
            SelectObject(g.conv_dc, g.conv_old);
        g.conv_old = NULL;
        if (g.conv_bmp)
            DeleteObject(g.conv_bmp);
        g.conv_bmp = NULL;
        g.conv_bits = NULL;
        DeleteDC(g.conv_dc);
        g.conv_dc = NULL;
    }
    g.conv_w = g.conv_h = 0;
}

static int ensure_conv(int w, int h)
{
    BITMAPINFO bmi;
    HMODULE gdi;

    if (w <= 0 || h <= 0)
        return 0;
    if (g.conv_dc && g.conv_bmp && g.conv_bits && g.conv_w == w && g.conv_h == h)
        return 1;
    destroy_conv();
    if (!g.gdi_setdi) {
        gdi = GetModuleHandleA("gdi32.dll");
        if (!gdi)
            gdi = LoadLibraryA("gdi32.dll");
        if (!gdi)
            return 0;
        g.gdi_setdi = (PFN_SetDIBitsToDevice)GetProcAddress(gdi, "SetDIBitsToDevice");
        if (!g.gdi_setdi)
            return 0;
    }
    g.conv_dc = CreateCompatibleDC(NULL);
    if (!g.conv_dc)
        return 0;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* top-down BGRA */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g.conv_bmp = CreateDIBSection(g.conv_dc, &bmi, DIB_RGB_COLORS, &g.conv_bits, NULL, 0);
    if (!g.conv_bmp || !g.conv_bits) {
        destroy_conv();
        return 0;
    }
    g.conv_old = (HBITMAP)SelectObject(g.conv_dc, g.conv_bmp);
    g.conv_w = w;
    g.conv_h = h;
    return 1;
}

/* Fast RGB555→BGRA blit into a destination buffer. */
static void blit_into_bgra(uint8_t *dst_fb, int dst_w, int dst_h, int xDest, int yDest, int w,
                           int h, int xSrc, int ySrc, UINT start, UINT lines, const void *bits,
                           const BITMAPINFO *bmi, UINT usage)
{
    const BITMAPINFOHEADER *bh;
    int biw, bih, abs_h, bpp, top_down, stride;
    int y, x;
    const uint8_t *src_base;
    DWORD t0;
    int use_lut, use_gdi;

    if (!dst_fb || !bits || !bmi || w <= 0 || h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    bh = &bmi->bmiHeader;
    biw = bh->biWidth;
    bih = bh->biHeight;
    abs_h = bih < 0 ? -bih : bih;
    bpp = bh->biBitCount;
    top_down = bih < 0;
    src_base = (const uint8_t *)bits;
    use_lut = !env_truthy("CK_VK_LUT_OFF", 0); /* default ON — 1080p convert was ~16ms/frame */
    use_gdi = env_truthy("CK_VK_GDI_CONVERT", 0); /* optional; manual555 matches BI_RGB */

    if (bh->biSizeImage > 0 && abs_h > 0)
        stride = (int)(bh->biSizeImage / (DWORD)abs_h);
    else
        stride = ((biw * bpp + 31) / 32) * 4;

    /* Cache soft DIB for HelpText tip clear (H-DIB); only full-ish frames. */
    if (bpp == 16 && biw >= 800 && abs_h >= 600)
        ck_soft_dib_note(bits, biw, abs_h, stride, top_down, bpp);

    try_load_lut();
    t0 = GetTickCount();
    (void)usage;

    if (bpp == 16 && use_gdi && ensure_conv(w, h) && g.gdi_setdi) {
        UINT gdi_lines = lines ? lines : (UINT)h;
        int rc;
        if (gdi_lines > (UINT)h)
            gdi_lines = (UINT)h;
        rc = g.gdi_setdi(g.conv_dc, 0, 0, (DWORD)w, (DWORD)h, xSrc, ySrc, start, gdi_lines, bits,
                         bmi, usage);
        {
            const uint8_t *src = (const uint8_t *)g.conv_bits;
            if (rc && src) {
                for (y = 0; y < h; ++y) {
                    int dst_y = yDest + y, x0, x1;
                    if (dst_y < 0 || dst_y >= dst_h)
                        continue;
                    x0 = xDest < 0 ? -xDest : 0;
                    x1 = w;
                    if (xDest + x1 > dst_w)
                        x1 = dst_w - xDest;
                    if (x0 < x1)
                        memcpy(dst_fb + ((size_t)dst_y * (size_t)dst_w + (size_t)(xDest + x0)) * 4u,
                               src + ((size_t)y * (size_t)w + (size_t)x0) * 4u,
                               (size_t)(x1 - x0) * 4u);
                }
                g.fb_bgra = 1;
                g.last_blit_ms = GetTickCount() - t0;
                return;
            }
        }
    }

    if (bpp == 16) {
        /*
         * SetDIBitsToDevice: YSrc is the lower-left of the source rect (MSDN).
         * Wine maps to top-down row: src_y0 = startscan + lines - (ySrc + h).
         * Using ySrc as a top-down offset (old code) mismatched whenever
         * 2*ySrc+h != lines — that produced editor mosaic / UI shredding.
         */
        UINT dib_lines = lines ? lines : (UINT)abs_h;
        int src_y0 = (int)start + (int)dib_lines - (ySrc + h);
        /* #region agent log */
        if (w >= 1800 && h >= 400 && yDest >= 40 && yDest <= 120) {
            static volatile LONG s_src_n;
            LONG sn = InterlockedIncrement(&s_src_n);
            if (sn <= 8 || (sn % 60) == 0) {
            int mid = h / 2;
            int sy = src_y0 + mid;
            FILE *df;
            unsigned s1599 = 0, s1600 = 0, s1720 = 0, s1910 = 0;
            if (sy >= 0 && sy < abs_h && src_base) {
                const uint8_t *row = top_down
                    ? (src_base + (size_t)sy * (size_t)stride)
                    : (src_base + (size_t)(abs_h - 1 - sy) * (size_t)stride);
                if (1599 < biw)
                    s1599 = *(const uint16_t *)(row + 1599u * 2u);
                if (1600 < biw)
                    s1600 = *(const uint16_t *)(row + 1600u * 2u);
                if (1720 < biw)
                    s1720 = *(const uint16_t *)(row + 1720u * 2u);
                if (1910 < biw)
                    s1910 = *(const uint16_t *)(row + 1910u * 2u);
            }
            df = vk_agent_open();
            if (df) {
                unsigned clip_b4 = 0, clip_ac = 0, clip_a8 = 0, clip_b0 = 0, clip_bc = 0;
                void *mgr = *(void **)(ULONG_PTR)0x008C7B30u;
                if (mgr && (ULONG_PTR)mgr > 0x10000u) {
                    BYTE *m = (BYTE *)mgr;
                    clip_a8 = *(unsigned *)(m + 0xa8);
                    clip_ac = *(unsigned *)(m + 0xac);
                    clip_b0 = *(unsigned *)(m + 0xb0);
                    clip_b4 = *(unsigned *)(m + 0xb4);
                    clip_bc = *(unsigned *)(m + 0xbc);
                }
                unsigned b1599 = 0, c1599 = 0, e1599 = 0, n1599 = 0;
                unsigned b1600 = 0, c1600 = 0, e1600 = 0, n1600 = 0;
                unsigned b1910 = 0, c1910 = 0, e1910 = 0, n1910 = 0;
                if (ck_scan_probe(1599, &b1599, &c1599, &e1599) && c1599 >= b1599)
                    n1599 = (c1599 - b1599) / 4u;
                if (ck_scan_probe(1600, &b1600, &c1600, &e1600) && c1600 >= b1600)
                    n1600 = (c1600 - b1600) / 4u;
                if (ck_scan_probe(1910, &b1910, &c1910, &e1910) && c1910 >= b1910)
                    n1910 = (c1910 - b1910) / 4u;
                fprintf(df,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"minimap-6\",\"hypothesisId\":\"H-P\","
                        "\"location\":\"vk_present.c:blit\",\"message\":\"src-dib\","
                        "\"data\":{\"dest\":[%d,%d,%d,%d],\"src0\":%d,\"sy\":%d,\"top_down\":%d,"
                        "\"biw\":%d,\"s16\":[%u,%u,%u,%u],"
                        "\"mgr\":%lu,\"off\":[%u,%u,%u,%u,%u],"
                        "\"spanN\":[%u,%u,%u],\"span1600\":[%u,%u,%u]},\"timestamp\":%lu}\n",
                        xDest, yDest, w, h, src_y0, sy, top_down, biw, s1599, s1600, s1720, s1910,
                        (unsigned long)(ULONG_PTR)mgr, clip_a8, clip_ac, clip_b0, clip_b4, clip_bc,
                        n1599, n1600, n1910, b1600, c1600, e1600,
                        (unsigned long)GetTickCount());
                fclose(df);
            }
            }
        }
        /* #endregion */
        /* Fast path: full-width unclipped rows + LUT (typical gameplay 1920×N). */
        if (use_lut && g.lut565 && xDest == 0 && xSrc == 0 && w == dst_w && w <= biw &&
            yDest >= 0 && yDest + h <= dst_h) {
            for (y = 0; y < h; ++y) {
                int src_y = src_y0 + y;
                const uint8_t *row;
                uint8_t *dst_row;
                int x;
                if (src_y < 0 || src_y >= abs_h)
                    continue;
                row = top_down ? (src_base + (size_t)src_y * (size_t)stride)
                               : (src_base + (size_t)(abs_h - 1 - src_y) * (size_t)stride);
                dst_row = dst_fb + (size_t)(yDest + y) * (size_t)dst_w * 4u;
                for (x = 0; x < w; ++x) {
                    uint16_t p = *(const uint16_t *)(row + (size_t)x * 2u);
                    const uint8_t *L = g.lut565 + (size_t)p * 4u;
                    uint8_t *dst = dst_row + (size_t)x * 4u;
                    dst[0] = L[2];
                    dst[1] = L[1];
                    dst[2] = L[0];
                    dst[3] = 255;
                }
            }
            g.fb_bgra = 1;
            g.last_blit_ms = GetTickCount() - t0;
            return;
        }
        for (y = 0; y < h; ++y) {
            int src_y = src_y0 + y;
            int dst_y = yDest + y;
            const uint8_t *row;
            uint8_t *dst_row;
            if (dst_y < 0 || dst_y >= dst_h || src_y < 0 || src_y >= abs_h)
                continue;
            row = top_down ? (src_base + (size_t)src_y * (size_t)stride)
                           : (src_base + (size_t)(abs_h - 1 - src_y) * (size_t)stride);
            dst_row = dst_fb + (size_t)dst_y * (size_t)dst_w * 4u;
            for (x = 0; x < w; ++x) {
                int src_x = xSrc + x, dst_x = xDest + x;
                uint16_t p;
                uint8_t *dst;
                unsigned r, gc, b;
                if (dst_x < 0 || dst_x >= dst_w || src_x < 0 || src_x >= biw)
                    continue;
                p = *(const uint16_t *)(row + (size_t)src_x * 2u);
                dst = dst_row + (size_t)dst_x * 4u;
                if (use_lut && g.lut565) {
                    const uint8_t *L = g.lut565 + (size_t)p * 4u;
                    dst[0] = L[2];
                    dst[1] = L[1];
                    dst[2] = L[0];
                    dst[3] = 255;
                } else {
                    r = (p >> 10) & 31;
                    gc = (p >> 5) & 31;
                    b = p & 31;
                    dst[0] = (uint8_t)((b * 255) / 31);
                    dst[1] = (uint8_t)((gc * 255) / 31);
                    dst[2] = (uint8_t)((r * 255) / 31);
                    dst[3] = 255;
                }
            }
        }
        g.fb_bgra = 1;
        g.last_blit_ms = GetTickCount() - t0;
        return;
    }
    g.last_blit_ms = GetTickCount() - t0;
}

static void rgba_to_swapchain_fmt(const uint8_t *src, uint8_t *dst, size_t npix)
{
    size_t i;
    int bgra = (g.swap_fmt == VK_FORMAT_B8G8R8A8_UNORM ||
                g.swap_fmt == VK_FORMAT_B8G8R8A8_SRGB);
    if (bgra) {
        for (i = 0; i < npix; ++i) {
            dst[i * 4 + 0] = src[i * 4 + 2];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 0];
            dst[i * 4 + 3] = 255;
        }
    } else {
        memcpy(dst, src, npix * 4);
    }
}

static int present_fb(double *out_fence_ms, double *out_compose_ms, double *out_acquire_ms,
                      double *out_submit_ms)
{
    uint32_t idx = 0;
    VkResult r;
    VkCommandBufferBeginInfo bi;
    VkImageMemoryBarrier barr;
    VkBufferImageCopy region;
    VkImageBlit blit;
    VkClearColorValue clear;
    VkImageSubresourceRange clear_range;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    uint32_t cw, ch;
    LONGLONG t0;
    double fence_ms = 0, compose_ms = 0, acquire_ms = 0, submit_ms = 0;
    int sw, sh, ox = 0, oy = 0, dw = 0, dh = 0, lb = 0, soff = 0, bil = 0;
    int use_gpu = 0;

    if (out_fence_ms)
        *out_fence_ms = 0;
    if (out_compose_ms)
        *out_compose_ms = 0;
    if (out_acquire_ms)
        *out_acquire_ms = 0;
    if (out_submit_ms)
        *out_submit_ms = 0;

    if (!g.ready || !g.fb)
        return 0;

    cw = g.extent.width;
    ch = g.extent.height;
    sw = g.fb_w;
    sh = g.fb_h;
    if (sw < 1 || sh < 1)
        return 0;

    use_gpu = gpu_blit_wanted() && g.fn.vkCmdBlitImage && g.fn.vkCmdClearColorImage &&
              g.fn.vkCreateImage;
    if (use_gpu && !ensure_soft_gpu(sw, sh)) {
        use_gpu = 0;
        g.gpu_ok = 0;
    } else if (use_gpu)
        g.gpu_ok = 1;

    if (!use_gpu && !g.staging_ptr)
        return 0;

    {
        /* Wait up to ~8ms; never fall through to real GDI (that caused double UI). */
        t0 = hitch_qpc_now();
        {
            VkResult wr = g.fn.vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, 8000000ull);
            fence_ms = hitch_qpc_ms_since(t0);
            if (out_fence_ms)
                *out_fence_ms = fence_ms;
            if (wr == VK_TIMEOUT)
                return -1;
            if (wr != VK_SUCCESS && wr != VK_TIMEOUT)
                return 0;
        }
    }
    g.fn.vkResetFences(g.device, 1, &g.fence);

    compose_geom(cw, ch, sw, sh, &ox, &oy, &dw, &dh, &lb, &soff, &bil);

    t0 = hitch_qpc_now();
    /* F8 soft/GPU toggle — always poll (even if this frame uses CPU compose). */
    vk_terrain_poll_toggle();
    if (use_gpu)
        pack_soft_upload(sw, sh);
    else
        compose_staging(cw, ch);
    compose_ms = hitch_qpc_ms_since(t0);
    if (out_compose_ms)
        *out_compose_ms = compose_ms;

    t0 = hitch_qpc_now();
    r = g.fn.vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.img_avail, VK_NULL_HANDLE,
                                   &idx);
    acquire_ms = hitch_qpc_ms_since(t0);
    if (out_acquire_ms)
        *out_acquire_ms = acquire_ms;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        static int s_recreate_depth;
        LONGLONG tr = hitch_qpc_now();
        if (s_recreate_depth >= 2)
            return 0;
        hitch_mark("swapchain-recreate");
        destroy_swapchain();
        if (!create_swapchain())
            return 0;
        hitch_note_ms("F4", "vk_present.c:present_fb", "swapchain-recreate", "swapchain",
                      hitch_qpc_ms_since(tr), "{}");
        s_recreate_depth++;
        {
            int pr = present_fb(out_fence_ms, out_compose_ms, out_acquire_ms, out_submit_ms);
            s_recreate_depth--;
            return pr;
        }
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return 0;
    if (idx >= g.image_count || !g.images)
        return 0;

    t0 = hitch_qpc_now();
    {
        double pump_ms = 0, terr_ms = 0, decor_ms = 0, obj_ms = 0, ov_ms = 0;
        LONGLONG tp;
        if (ktx_obj_wanted() && !ck_soft_obj_enabled()) {
            tp = hitch_qpc_now();
            vk_obj_pump_uploads();
            pump_ms = hitch_qpc_ms_since(tp);
        }
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g.fn.vkBeginCommandBuffer(g.cmd, &bi);

        if (use_gpu) {
            VkImageMemoryBarrier barrs[2];

            /* soft: UNDEFINED → TRANSFER_DST */
            memset(&barr, 0, sizeof(barr));
            barr.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barr.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barr.image = g.soft_img;
            barr.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barr.subresourceRange.levelCount = 1;
            barr.subresourceRange.layerCount = 1;
        barr.srcAccessMask = 0;
        barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);

        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (uint32_t)sw;
        region.imageExtent.height = (uint32_t)sh;
        region.imageExtent.depth = 1;
        g.fn.vkCmdCopyBufferToImage(g.cmd, g.soft_upload, g.soft_img,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        /* Optional GPU terrain (F8) and/or KTX decor overpaint into soft_img.
         * Soft mode (F8 off): retail PutDecor only — do not GPU-overpaint decors. */
        {
            int want_terrain = vk_terrain_ready() && vk_terrain_draw_enabled();
            int want_decor = want_terrain; /* KTX decor with GPU terrain only */
            if ((want_terrain || want_decor) && g.soft_img)
                ensure_soft_view();
            if ((want_terrain || want_decor) && g.soft_view) {
                VkImageMemoryBarrier tb;
                int decor_only;
                memset(&tb, 0, sizeof(tb));
                tb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                tb.image = g.soft_img;
                tb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                tb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                tb.subresourceRange.levelCount = 1;
                tb.subresourceRange.layerCount = 1;
                tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                tb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                tb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
                                          0, NULL, 1, &tb);
                if (want_terrain) {
                    LONGLONG t_tr = hitch_qpc_now();
                    (void)vk_terrain_record(g.cmd, g.soft_img, g.soft_view, sw, sh);
                    terr_ms = hitch_qpc_ms_since(t_tr);
                }
                {
                    VkImageView depth_view = vk_iso_depth_ensure(sw, sh);
                    if (depth_view)
                        vk_iso_depth_clear(g.cmd);
                    LONGLONG t_dc = hitch_qpc_now();
                    (void)vk_decor_record(g.cmd, g.soft_img, g.soft_view, depth_view, sw, sh);
                    decor_ms = hitch_qpc_ms_since(t_dc);
                    if (ktx_obj_wanted() && !ck_soft_obj_enabled()) {
                        LONGLONG t_ob = hitch_qpc_now();
                        (void)vk_obj_record(g.cmd, g.soft_img, g.soft_view, depth_view, sw, sh);
                        obj_ms = hitch_qpc_ms_since(t_ob);
                    }
                }
                if (vk_soft_overlay_wanted()) {
                    LONGLONG t_ov = hitch_qpc_now();
                    vk_soft_overlay_record(g.cmd, g.soft_img, g.soft_view, g.soft_upload, sw, sh);
                    ov_ms = hitch_qpc_ms_since(t_ov);
                }
                decor_only = !want_terrain;
                /* Height-warped terrain verts go into InfoBar — restore UI from soft_upload.
                 * Decor-only on soft terrain: just transition back to TRANSFER_SRC. */
                {
                    VkImageMemoryBarrier ub;
                    memset(&ub, 0, sizeof(ub));
                    ub.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    ub.image = g.soft_img;
                    ub.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    ub.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    ub.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    ub.subresourceRange.levelCount = 1;
                    ub.subresourceRange.layerCount = 1;
                    ub.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ub.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    ub.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    ub.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                                              &ub);
                    if (!decor_only) {
                        VkBufferImageCopy ui_regs[2];
                        int top_h = 80;
                        int bot_h = (sh > 100) ? 54 : 0;
                        int play_h = sh - top_h - bot_h;
                        int nreg = 0;
                        memset(ui_regs, 0, sizeof(ui_regs));
                        if (top_h > 0 && top_h < sh) {
                            ui_regs[nreg].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ui_regs[nreg].imageSubresource.layerCount = 1;
                            ui_regs[nreg].imageOffset.x = 0;
                            ui_regs[nreg].imageOffset.y = 0;
                            ui_regs[nreg].imageExtent.width = (uint32_t)sw;
                            ui_regs[nreg].imageExtent.height = (uint32_t)top_h;
                            ui_regs[nreg].imageExtent.depth = 1;
                            ui_regs[nreg].bufferOffset = 0;
                            ui_regs[nreg].bufferRowLength = (uint32_t)sw;
                            ui_regs[nreg].bufferImageHeight = (uint32_t)sh;
                            nreg++;
                        }
                        if (bot_h > 0 && bot_h < sh) {
                            ui_regs[nreg].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ui_regs[nreg].imageSubresource.layerCount = 1;
                            ui_regs[nreg].imageOffset.x = 0;
                            ui_regs[nreg].imageOffset.y = sh - bot_h;
                            ui_regs[nreg].imageExtent.width = (uint32_t)sw;
                            ui_regs[nreg].imageExtent.height = (uint32_t)bot_h;
                            ui_regs[nreg].imageExtent.depth = 1;
                            ui_regs[nreg].bufferOffset =
                                (VkDeviceSize)(size_t)(sh - bot_h) * (VkDeviceSize)sw * 4u;
                            ui_regs[nreg].bufferRowLength = (uint32_t)sw;
                            ui_regs[nreg].bufferImageHeight = (uint32_t)sh;
                            nreg++;
                        }
                        if (nreg > 0)
                            g.fn.vkCmdCopyBufferToImage(g.cmd, g.soft_upload, g.soft_img,
                                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                        (uint32_t)nreg, ui_regs);
                        /* Soft playfield opaque stamp removed — vk_soft_overlay alpha-blends
                         * retail sprites onto GPU terrain (see soft overlay before UI restore). */
                        (void)play_h;
                        /* #region agent log */
                        {
                            static LONG s_ui_logs;
                            LONG n = InterlockedIncrement(&s_ui_logs);
                            if (n <= 6) {
                                FILE *df = vk_agent_open();
                                if (df) {
                                    fprintf(df,
                                            "{\"sessionId\":\"764ba7\",\"runId\":\"tools-blank-1\","
                                            "\"hypothesisId\":\"T1\",\"location\":\"vk_present.c:present\","
                                            "\"message\":\"ui-band-restore\","
                                            "\"data\":{\"top\":%d,\"bot\":%d,\"nreg\":%d,\"soft\":[%d,%d],"
                                            "\"tools_strip_restored\":0,\"soft_obj\":%d},"
                                            "\"timestamp\":%lu}\n",
                                            top_h, bot_h, nreg, sw, sh,
                                            ktx_obj_wanted() ? 0 : 1,
                                            (unsigned long)GetTickCount());
                                    fclose(df);
                                }
                            }
                        }
                        /* #endregion */
                    }
                    ub.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    ub.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    ub.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    ub.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                                              &ub);
                }

                /* Only swapchain needs UNDEFINED→DST */
                memset(barrs, 0, sizeof(barrs));
                barrs[0] = barr;
                barrs[0].image = g.images[idx];
                barrs[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrs[0].srcAccessMask = 0;
                barrs[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                                          &barrs[0]);
            } else {
                /* soft DST→SRC ; swap UNDEFINED→DST */
                memset(barrs, 0, sizeof(barrs));
                barrs[0] = barr;
                barrs[0].image = g.soft_img;
                barrs[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrs[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrs[1] = barr;
                barrs[1].image = g.images[idx];
                barrs[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrs[1].srcAccessMask = 0;
                barrs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
                                          barrs);
            }
        }

        memset(&clear, 0, sizeof(clear));
        memset(&clear_range, 0, sizeof(clear_range));
        clear_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear_range.levelCount = 1;
        clear_range.layerCount = 1;
        g.fn.vkCmdClearColorImage(g.cmd, g.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &clear, 1, &clear_range);

        memset(&blit, 0, sizeof(blit));
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[0].x = 0;
        blit.srcOffsets[0].y = 0;
        blit.srcOffsets[0].z = 0;
        blit.srcOffsets[1].x = sw;
        blit.srcOffsets[1].y = sh;
        blit.srcOffsets[1].z = 1;
        blit.dstOffsets[0].x = ox;
        blit.dstOffsets[0].y = oy;
        blit.dstOffsets[0].z = 0;
        blit.dstOffsets[1].x = ox + dw;
        blit.dstOffsets[1].y = oy + dh;
        blit.dstOffsets[1].z = 1;
        g.fn.vkCmdBlitImage(g.cmd, g.soft_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            g.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                            bil ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

        barr.image = g.images[idx];
        barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barr.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barr.dstAccessMask = 0;
        g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                                  &barr);
    } else {
        memset(&barr, 0, sizeof(barr));
        barr.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barr.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barr.image = g.images[idx];
        barr.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barr.subresourceRange.levelCount = 1;
        barr.subresourceRange.layerCount = 1;
        barr.srcAccessMask = 0;
        barr.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);

        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = cw;
        region.imageExtent.height = ch;
        region.imageExtent.depth = 1;
        g.fn.vkCmdCopyBufferToImage(g.cmd, g.staging, g.images[idx],
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barr.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barr.dstAccessMask = 0;
        g.fn.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                                  &barr);
    }
    g.fn.vkEndCommandBuffer(g.cmd);

    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.img_avail;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.render_done;
    r = g.fn.vkQueueSubmit(g.queue, 1, &si, g.fence);
    if (r != VK_SUCCESS)
        return 0;

    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.render_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &idx;
    r = g.fn.vkQueuePresentKHR(g.queue, &pi);
    submit_ms = hitch_qpc_ms_since(t0);
    hitch_set_submit_phases(pump_ms, terr_ms, decor_ms, obj_ms, ov_ms);
    if (out_submit_ms)
        *out_submit_ms = submit_ms;
    } /* pump/terr/decor/obj timing scope */

    if (!g.gpu_logged && use_gpu) {
        g.gpu_logged = 1;
        log_msg("vk_present: GPU blit soft %dx%d -> swap %ux%u dst %dx%d origin %d,%d lb=%d bil=%d",
                sw, sh, cw, ch, dw, dh, ox, oy, lb, bil);
        /* #region agent log */
        {
            FILE *df = vk_agent_open();
            if (df) {
                fprintf(df,
                        "{\"sessionId\":\"764ba7\",\"runId\":\"gpu1\",\"hypothesisId\":\"H-GPU\","
                        "\"location\":\"vk_present.c:present_fb\",\"message\":\"gpu-blit\","
                        "\"data\":{\"soft\":[%d,%d],\"swap\":[%u,%u],\"dst\":[%d,%d],\"origin\":[%d,%d],"
                        "\"lb\":%d,\"bilinear\":%d,\"compose_ms\":%.3f},\"timestamp\":%lu}\n",
                        sw, sh, cw, ch, dw, dh, ox, oy, lb, bil, compose_ms,
                        (unsigned long)GetTickCount());
                fclose(df);
            }
        }
        /* #endregion */
    }

    {
        LONG n = InterlockedIncrement(&g.presents);
        g.last_present_ms = (DWORD)(fence_ms + compose_ms + acquire_ms + submit_ms);
        if (n <= 8 || (n % 120) == 0) {
            log_msg("vk_present: present #%ld r=%d ms=%lu gpu=%d", (long)n, (int)r,
                    (unsigned long)g.last_present_ms, use_gpu);
        }
    }
    g.pan_shift_x = g.pan_shift_y = 0;
    return (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) ? 1 : 0;
}

void vk_present_shutdown(void)
{
    if (g.device) {
        g.fn.vkDeviceWaitIdle(g.device);
        vk_decor_shutdown();
        vk_obj_shutdown();
        vk_iso_depth_shutdown();
        vk_soft_overlay_shutdown();
        vk_terrain_shutdown();
        destroy_swapchain();
        if (g.render_done)
            g.fn.vkDestroySemaphore(g.device, g.render_done, NULL);
        if (g.img_avail)
            g.fn.vkDestroySemaphore(g.device, g.img_avail, NULL);
        if (g.fence)
            g.fn.vkDestroyFence(g.device, g.fence, NULL);
        if (g.cmd_pool)
            g.fn.vkDestroyCommandPool(g.device, g.cmd_pool, NULL);
        g.fn.vkDestroyDevice(g.device, NULL);
    }
    if (g.surface && g.fn.vkDestroySurfaceKHR)
        g.fn.vkDestroySurfaceKHR(g.instance, g.surface, NULL);
    if (g.instance && g.fn.vkDestroyInstance)
        g.fn.vkDestroyInstance(g.instance, NULL);
    if (g.lib)
        FreeLibrary(g.lib);
    destroy_conv();
    tip_font_shutdown();
    free(g.lut565);
    free(g.fb);
    memset(&g, 0, sizeof(g));
}

int vk_present_try(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc, UINT start,
                   UINT lines, CONST VOID *bits, CONST BITMAPINFO *bmi, UINT usage)
{
    HWND hwnd;
    RECT rc;
    int cw, ch;
    int is_map;
    LONGLONG t_blit = 0;
    double blit_ms = 0, extend_ms = 0;
    int blit_w = 0, blit_h = 0, fullish_for_log = 0;

    if (!vk_replace_wanted())
        return 0;
    if (g.failed || !bits || !bmi)
        return 0;

    hwnd = WindowFromDC(hdc);
    if (!hwnd)
        return 0;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw < 64 || ch < 64)
        return 0;

    is_map = ((int)w >= 640 && (int)h >= 400);
    if (!is_map && !g.ready)
        return 0;

    if (!g.ready) {
        if (!vk_init(hwnd))
            return 0;
    } else if (hwnd != g.hwnd) {
        /* stick to first hwnd */
    }

    if ((uint32_t)cw != g.extent.width || (uint32_t)ch != g.extent.height) {
        destroy_swapchain();
        if (!create_swapchain()) {
            g.failed = 1;
            return 0;
        }
    }

    {
        const BITMAPINFOHEADER *bh = &bmi->bmiHeader;
        int biw = bh->biWidth;
        int absh = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
        int soft_w = biw > 0 ? biw : cw;
        int soft_h = absh > 0 ? absh : ch;
        int fullish;
        int clamped = 0;

        /*
         * Soft size = logical game framebuffer, NOT every BMI.
         * - Full frames (>=800): adopt as soft / game_soft
         * - Classic cutscene 640×480: keep game_soft (never spoof 640)
         * - Tiny UI sprites (tips 241², panels): keep current soft
         */
        if (soft_w == 640 && soft_h == 480 && g.extent.width >= 1280 && g.extent.height >= 720) {
            if (g.game_soft_w >= 800 && g.game_soft_h >= 600) {
                soft_w = g.game_soft_w;
                soft_h = g.game_soft_h;
            } else {
                soft_w = (int)g.extent.width;
                soft_h = (int)g.extent.height;
            }
            clamped = 1;
        } else if (soft_w >= 800 && soft_h >= 600) {
            if (g.game_soft_w != soft_w || g.game_soft_h != soft_h)
                g.scale_logged = 0;
            g.game_soft_w = soft_w;
            g.game_soft_h = soft_h;
        } else {
            if (g.soft_w >= 800 && g.soft_h >= 600) {
                soft_w = g.soft_w;
                soft_h = g.soft_h;
            } else if (g.game_soft_w >= 800 && g.game_soft_h >= 600) {
                soft_w = g.game_soft_w;
                soft_h = g.game_soft_h;
            } else {
                soft_w = cw;
                soft_h = ch;
            }
        }

        {
            static int s_last_logged_sw, s_last_logged_sh;
            if (soft_w != s_last_logged_sw || soft_h != s_last_logged_sh) {
                s_last_logged_sw = soft_w;
                s_last_logged_sh = soft_h;

            }
        }
        g.soft_w = soft_w;
        g.soft_h = soft_h;
        if (!ensure_fb(soft_w, soft_h))
            return 0;
        if (clamped) {
            static volatile LONG s_clamp_n;
            if (InterlockedIncrement(&s_clamp_n) <= 8) {

                log_msg("vk_present: clamp cutscene soft %dx%d -> %dx%d", biw, absh, soft_w, soft_h);
            }
        }

        fullish = ((int)w * 10 >= soft_w * 9 && (int)h * 10 >= soft_h * 9);
        if (fullish) {
            if (!g.have_full || g.last_full_x != xDest || g.last_full_y != yDest)
                memset(g.fb, 0, (size_t)g.fb_w * (size_t)g.fb_h * 4u);
            g.last_full_x = xDest;
            g.last_full_y = yDest;
            g.have_full = 1;
            /* Do NOT tip_font_present_invalidate here — gameplay full blits every
             * frame and would clear sticky tips (blink). Size-change invalidate only. */
        }
        t_blit = hitch_qpc_now();
        /* Soft-space GDI dest; scaled to client on present. */
        blit_into_bgra(g.fb, g.fb_w, g.fb_h, xDest, yDest, (int)w, (int)h, xSrc, ySrc, start, lines,
                       bits, bmi, usage);
        blit_ms = hitch_qpc_ms_since(t_blit);

        /* Arm/disarm ZoomMap pillar fill from blit dest (H-Z7). */
        ck_zoom_letterbox_note_blit(xDest, yDest, (int)w, (int)h);

        /* Fog pillars are composed at present — do not paint into g.fb (H-Z8). */

        /* #region agent log */
        {
            /* H-T: bottom-left text in zoom — find which blit carries it vs gameplay. */
            int zoom_on = ck_zoom_letterbox_get(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
            int y2 = yDest + (int)h;
            int botish = (yDest >= 880 || y2 >= 1000);
            int leftish = (xDest <= 320);
            int textish = ((int)h >= 12 && (int)h <= 120 && (int)w >= 40 && (int)w <= 900);
            int strip = (botish && (int)w >= soft_w / 2);
            if (botish || (zoom_on && leftish && (int)h < 200)) {
                static volatile LONG s_t_n;
                LONG tn = InterlockedIncrement(&s_t_n);
                if (tn <= 80 || (tn % 25) == 0) {
                    int mL = -1, mT = -1, mR = -1, mB = -1;
                    unsigned px0 = 0, px40 = 0, px120 = 0;
                    int sy = yDest + ((int)h > 4 ? (int)h / 2 : 0);
                    if (sy < 0)
                        sy = 0;
                    if (sy >= soft_h)
                        sy = soft_h - 1;
                    ck_zoom_letterbox_get(NULL, NULL, NULL, NULL, &mL, &mT, &mR, &mB);
                    if (xDest >= 0 && xDest < soft_w)
                        px0 = ck_fb_px(g.fb, soft_w, xDest, sy);
                    if (xDest + 40 >= 0 && xDest + 40 < soft_w)
                        px40 = ck_fb_px(g.fb, soft_w, xDest + 40, sy);
                    if (xDest + 120 >= 0 && xDest + 120 < soft_w)
                        px120 = ck_fb_px(g.fb, soft_w, xDest + 120, sy);
                    {
                        FILE *df = vk_agent_open();
                        if (df) {
                            fprintf(df,
                                    "{\"sessionId\":\"764ba7\",\"runId\":\"zoom-text-1\",\"hypothesisId\":\"H-T\","
                                    "\"location\":\"vk_present.c:try\",\"message\":\"zoom-text-blit\","
                                    "\"data\":{\"n\":%ld,\"zoom\":%d,\"dest\":[%d,%d,%d,%d],"
                                    "\"botish\":%d,\"leftish\":%d,\"textish\":%d,\"strip\":%d,"
                                    "\"map\":[%d,%d,%d,%d],\"px\":[%u,%u,%u]},\"timestamp\":%lu}\n",
                                    (long)tn, zoom_on, xDest, yDest, (int)w, (int)h, botish, leftish,
                                    textish, strip, mL, mT, mR, mB, px0, px40, px120,
                                    (unsigned long)GetTickCount());
                            fclose(df);
                        }
                    }
                }
            }
        }
        /* #endregion */

        /* #region agent log */
        if (!ck_zoom_letterbox_get(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) &&
            yDest <= 90 && (int)h >= 400 && (int)w >= soft_w * 3 / 4) {
            static volatile LONG s_probe_n;
            LONG pn = InterlockedIncrement(&s_probe_n);
            if (pn <= 30 || (pn % 45) == 0) {
                int yy = (yDest + (int)h / 2);
                int x, x0 = -1, x1 = -1;
                unsigned Lpx, Mpx, Rpx;
                if (yy < 0)
                    yy = soft_h / 2;
                if (yy >= soft_h)
                    yy = soft_h - 1;
                for (x = 0; x < soft_w; ++x) {
                    if (ck_fb_px(g.fb, soft_w, x, yy) > 0x101010u) {
                        if (x0 < 0)
                            x0 = x;
                        x1 = x;
                    }
                }
                Lpx = ck_fb_px(g.fb, soft_w, 80, yy);
                Mpx = ck_fb_px(g.fb, soft_w, soft_w / 2, yy);
                Rpx = ck_fb_px(g.fb, soft_w, soft_w - 80, yy);
                {
                    FILE *df =
                        vk_agent_open();
                    if (df) {
                        fprintf(df,
                                "{\"sessionId\":\"764ba7\",\"runId\":\"zoom-pillar-6\",\"hypothesisId\":\"H-S\","
                                "\"location\":\"vk_present.c:probe\",\"message\":\"play-pillar-probe\","
                                "\"data\":{\"n\":%ld,\"dest\":[%d,%d,%d,%d],\"soft\":[%d,%d],"
                                "\"content_x\":[%d,%d],\"Lpx\":%u,\"Mpx\":%u,\"Rpx\":%u},"
                                "\"timestamp\":%lu}\n",
                                (long)pn, xDest, yDest, (int)w, (int)h, soft_w, soft_h, x0, x1, Lpx,
                                Mpx, Rpx, (unsigned long)GetTickCount());
                        fclose(df);
                    }
                }
            }
        }
        /* #endregion */

        /* Extend 1600-capped top/bottom UI bars to soft_w (InfoBar + editor CmdBar). */
        {
            int near_top = (yDest <= 2 && (int)h >= 40 && (int)h <= 120);
            int near_bot = ((int)yDest + (int)h >= soft_h - 4 && (int)h >= 40 && (int)h <= 120);
            int did_extend = 0;
            if (fullish || near_top || near_bot) {
                LONGLONG te = hitch_qpc_now();
                extend_ui_bar_gaps(g.fb, soft_w, soft_h);
                extend_ms = hitch_qpc_ms_since(te);
                did_extend = 1;
            }
            blit_w = (int)w;
            blit_h = (int)h;
            fullish_for_log = fullish;

            /* #region agent log */
            {
                static DWORD s_mm_last;
                static volatile LONG s_mm_n;
                static int s_dumped;
                static int s_dumped_hj;
                DWORD now = GetTickCount();
                int rightish = ((int)xDest + (int)w > soft_w * 3 / 4);
                int covers1600 = ((int)xDest + (int)w > 1590);
                int do_log = 0;
                LONG nn = InterlockedIncrement(&s_mm_n);
                if (nn <= 20)
                    do_log = 1;
                else if ((nn % 45) == 0 && (fullish || rightish || covers1600))
                    do_log = 1;
                else if ((now - s_mm_last) >= 2000u && (fullish || rightish || near_top || near_bot))
                    do_log = 1;
                if (do_log) {
                    int stride_calc;
                    int panel0 = soft_w > 500 ? soft_w - 448 : soft_w * 3 / 4;
                    int xs[6];
                    int yi, xi;
                    unsigned corr[5];
                    unsigned uniq[6];
                    FILE *df;
                    const BITMAPINFOHEADER *bh2 = &bmi->bmiHeader;
                    int bpp2 = bh2->biBitCount;
                    int absh2 = bh2->biHeight < 0 ? -bh2->biHeight : bh2->biHeight;
                    if (bh2->biSizeImage > 0 && absh2 > 0)
                        stride_calc = (int)(bh2->biSizeImage / (DWORD)absh2);
                    else
                        stride_calc = ((biw * bpp2 + 31) / 32) * 4;
                    xs[0] = panel0;
                    xs[1] = panel0 + 64;
                    xs[2] = soft_w > 1600 ? 1590 : panel0 + 100;
                    xs[3] = soft_w > 1600 ? 1600 : panel0 + 120;
                    xs[4] = soft_w > 200 ? soft_w - 200 : panel0 + 200;
                    xs[5] = soft_w > 20 ? soft_w - 10 : panel0;
                    for (xi = 0; xi < 6; ++xi) {
                        unsigned seen[8];
                        int nseen = 0;
                        uniq[xi] = 0;
                        for (yi = 0; yi < 8; ++yi) {
                            int yy = soft_h / 10 + yi * (soft_h / 10);
                            unsigned p = ck_fb_px(g.fb, soft_w, xs[xi], yy);
                            int k, found = 0;
                            for (k = 0; k < nseen; ++k)
                                if (seen[k] == p) {
                                    found = 1;
                                    break;
                                }
                            if (!found && nseen < 8)
                                seen[nseen++] = p;
                        }
                        uniq[xi] = (unsigned)nseen;
                    }
                    for (xi = 0; xi < 5; ++xi) {
                        unsigned same = 0;
                        for (yi = 0; yi < 16; ++yi) {
                            int yy = soft_h / 8 + yi * (soft_h / 20);
                            if (ck_fb_px(g.fb, soft_w, xs[xi], yy) ==
                                ck_fb_px(g.fb, soft_w, xs[xi + 1], yy))
                                same++;
                        }
                        corr[xi] = same; /* 0..16; high => neighbor columns identical */
                    }
                    /* Dump + stripe metric on live FULL frame (only blit that paints panel mid). */
                    {
                        int live = (uniq[0] > 1 || uniq[5] > 1);
                        int mid_wide = ((int)w >= soft_w - 8 && (int)h >= soft_h / 2 &&
                                        yDest >= 40 && yDest <= 120);
                        int do_dump = fullish || mid_wide;
                        if (!s_dumped && do_dump && live && g.fb && soft_w >= 1600 && soft_h >= 600) {
                            FILE *bmp;
                            int pw = soft_w - panel0;
                            int ph = fullish ? (soft_h - 156) : (int)h;
                            int y0 = fullish ? 80 : yDest;
                            int row, col;
                            unsigned char hdr[54];
                            unsigned dib_stride;
                            unsigned stripe_edges = 0, stripe_n = 0;
                            int yy;
                            if (ph < 100)
                                ph = soft_h / 2;
                            if (y0 + ph > soft_h)
                                ph = soft_h - y0;
                            for (yy = y0 + 8; yy < y0 + ph && yy + 8 < soft_h; yy += 32) {
                                int x;
                                for (x = panel0 + 1; x < soft_w; ++x) {
                                    unsigned a = ck_fb_px(g.fb, soft_w, x - 1, yy);
                                    unsigned b = ck_fb_px(g.fb, soft_w, x, yy);
                                    unsigned va = ck_fb_px(g.fb, soft_w, x, yy + 8);
                                    if ((b == va) && (a != b))
                                        stripe_edges++;
                                    stripe_n++;
                                }
                            }
                            memset(hdr, 0, sizeof(hdr));
                            dib_stride = ((unsigned)pw * 3u + 3u) & ~3u;
                            hdr[0] = 'B';
                            hdr[1] = 'M';
                            *(unsigned *)(hdr + 2) = 54u + dib_stride * (unsigned)ph;
                            *(unsigned *)(hdr + 10) = 54;
                            *(unsigned *)(hdr + 14) = 40;
                            *(int *)(hdr + 18) = pw;
                            *(int *)(hdr + 22) = ph;
                            *(unsigned short *)(hdr + 26) = 1;
                            *(unsigned short *)(hdr + 28) = 24;
                            bmp = fopen("/home/cybernetik/Games/Imperivm/ck_asi/.cursor/minimap_right_mid.bmp",
                                        "wb");
                            if (bmp) {
                                fwrite(hdr, 1, 54, bmp);
                                for (row = ph - 1; row >= 0; --row) {
                                    unsigned char pad[4] = {0, 0, 0, 0};
                                    int sy = y0 + row;
                                    for (col = 0; col < pw; ++col) {
                                        const uint8_t *p =
                                            g.fb + ((size_t)sy * (size_t)soft_w +
                                                    (size_t)(panel0 + col)) *
                                                       4u;
                                        unsigned char bgr[3] = {p[0], p[1], p[2]};
                                        fwrite(bgr, 1, 3, bmp);
                                    }
                                    fwrite(pad, 1, dib_stride - (unsigned)pw * 3u, bmp);
                                }
                                fclose(bmp);
                                s_dumped = 1;
                            }
                            {
                                FILE *df2 = vk_agent_open();
                                if (df2) {
                                    fprintf(df2,
                                            "{\"sessionId\":\"764ba7\",\"runId\":\"minimap-4\","
                                            "\"hypothesisId\":\"H-K\",\"location\":\"vk_present.c:dump\","
                                            "\"message\":\"panel-dump\","
                                            "\"data\":{\"fullish\":%d,\"mid_wide\":%d,\"y0\":%d,\"ph\":%d,"
                                            "\"stripe_edges\":%u,\"stripe_n\":%u,\"stripe_pct\":%.1f,"
                                            "\"dumped\":%d},\"timestamp\":%lu}\n",
                                            fullish, mid_wide, y0, ph, stripe_edges, stripe_n,
                                            stripe_n ? (100.0 * stripe_edges / stripe_n) : 0.0,
                                            s_dumped, (unsigned long)now);
                                    fclose(df2);
                                }
                            }
                        }
                    }
                    s_mm_last = now;
                    {
                        unsigned px1599 = soft_w > 1600 ? ck_fb_px(g.fb, soft_w, 1599, soft_h / 2) : 0;
                        unsigned px1600 = soft_w > 1600 ? ck_fb_px(g.fb, soft_w, 1600, soft_h / 2) : 0;
                        unsigned px1601 = soft_w > 1601 ? ck_fb_px(g.fb, soft_w, 1601, soft_h / 2) : 0;
                        unsigned px1720 = soft_w > 1720 ? ck_fb_px(g.fb, soft_w, 1720, soft_h / 2) : 0;
                        unsigned px1600t = soft_w > 1600 ? ck_fb_px(g.fb, soft_w, 1600, 40) : 0;
                        const char *hid =
                            (uniq[3] <= 1 && corr[3] >= 12)
                                ? "H-J"
                                : ((int)w >= soft_w - 8 && (int)h >= soft_h / 2 ? "H-G" : "H-H");
                        df = vk_agent_open();
                        if (df) {
                            fprintf(df,
                                    "{\"sessionId\":\"764ba7\",\"runId\":\"minimap-5\",\"hypothesisId\":\"%s\","
                                    "\"location\":\"vk_present.c:try\",\"message\":\"present-blit\","
                                    "\"data\":{\"n\":%ld,\"biw\":%d,\"bih\":%d,\"soft\":[%d,%d],"
                                    "\"dest\":[%d,%d,%lu,%lu],\"src\":[%d,%d],\"start\":%u,\"lines\":%u,"
                                    "\"stride\":%d,\"bpp\":%d,\"fullish\":%d,"
                                    "\"rightish\":%d,\"extend\":%d,\"panel0\":%d,\"xs\":[%d,%d,%d,%d,%d,%d],"
                                    "\"uniq\":[%u,%u,%u,%u,%u,%u],\"corr\":[%u,%u,%u,%u,%u],\"dumped\":%d,"
                                    "\"px\":[%u,%u,%u,%u],\"px1600t\":%u},\"timestamp\":%lu}\n",
                                    hid, (long)nn, biw, absh, soft_w, soft_h, xDest, yDest,
                                    (unsigned long)w, (unsigned long)h, xSrc, ySrc, start, lines,
                                    stride_calc, bpp2, fullish, rightish, did_extend, panel0, xs[0],
                                    xs[1], xs[2], xs[3], xs[4], xs[5], uniq[0], uniq[1], uniq[2],
                                    uniq[3], uniq[4], uniq[5], corr[0], corr[1], corr[2], corr[3],
                                    corr[4], s_dumped, px1599, px1600, px1601, px1720, px1600t,
                                    (unsigned long)now);
                            fclose(df);
                        }
                        /* Dump playfield when x=1600 first goes solid (H-J live). */
                        if (!s_dumped_hj && uniq[3] <= 1 && uniq[0] > 1 && px1600 != 0 && g.fb &&
                            soft_w >= 1600 && soft_h >= 600) {
                            FILE *bmp;
                            int pw = soft_w - panel0, ph = soft_h - 156, row, col;
                            unsigned char hdr[54];
                            unsigned dib_stride;
                            if (ph < 100)
                                ph = soft_h / 2;
                            memset(hdr, 0, sizeof(hdr));
                            dib_stride = ((unsigned)pw * 3u + 3u) & ~3u;
                            hdr[0] = 'B';
                            hdr[1] = 'M';
                            *(unsigned *)(hdr + 2) = 54u + dib_stride * (unsigned)ph;
                            *(unsigned *)(hdr + 10) = 54;
                            *(unsigned *)(hdr + 14) = 40;
                            *(int *)(hdr + 18) = pw;
                            *(int *)(hdr + 22) = ph;
                            *(unsigned short *)(hdr + 26) = 1;
                            *(unsigned short *)(hdr + 28) = 24;
                            bmp = fopen(
                                "/home/cybernetik/Games/Imperivm/ck_asi/.cursor/minimap_right_hj.bmp",
                                "wb");
                            if (bmp) {
                                fwrite(hdr, 1, 54, bmp);
                                for (row = ph - 1; row >= 0; --row) {
                                    unsigned char pad[4] = {0, 0, 0, 0};
                                    int sy = 80 + row;
                                    for (col = 0; col < pw; ++col) {
                                        const uint8_t *p =
                                            g.fb + ((size_t)sy * (size_t)soft_w +
                                                    (size_t)(panel0 + col)) *
                                                       4u;
                                        unsigned char bgr[3] = {p[0], p[1], p[2]};
                                        fwrite(bgr, 1, 3, bmp);
                                    }
                                    fwrite(pad, 1, dib_stride - (unsigned)pw * 3u, bmp);
                                }
                                fclose(bmp);
                                s_dumped_hj = 1;
                            }
                        }
                    }
                }
            }
            /* #endregion */
        }
    }

    g.dirty = 1;
    {
        static LONGLONG s_last_gap_qpc;
        double fence_ms = 0, compose_ms = 0, acquire_ms = 0, submit_ms = 0, gap_ms = 0;
        double pace_ms = 0, frame_ms = 0;
        LONGLONG frame_start;
        int pr;

        frame_start = s_last_gap_qpc;
        if (!frame_start)
            frame_start = hitch_qpc_now();
        if (s_last_gap_qpc)
            gap_ms = hitch_qpc_ms_since(s_last_gap_qpc);
        {
            LONGLONG t_cam = hitch_qpc_now();
            cam_smooth_on_present();
            hitch_set_cam_ms(hitch_qpc_ms_since(t_cam));
        }
        if (g.fb && g.fb_w > 0 && g.fb_h > 0) {
            ktx_gpu_terrain_paint_soft(g.fb, g.fb_w, g.fb_h);
            ktx_terrain_preview_soft(g.fb, g.fb_w, g.fb_h);
        }
        pr = present_fb(&fence_ms, &compose_ms, &acquire_ms, &submit_ms);
        /* Optional present pacing (CK_FPS; default off). Does not lock game Tick/FPS. */
        if (pr == 1 || pr == -1)
            pace_ms = pace_frame(frame_start);
        s_last_gap_qpc = hitch_qpc_now();
        frame_ms = hitch_qpc_ms_since(frame_start);
        hitch_present_sample(gap_ms, frame_ms, blit_ms, extend_ms, fence_ms, compose_ms,
                             acquire_ms, submit_ms, pr, fullish_for_log, blit_w, blit_h, pr == -1,
                             (int)g.present_mode, g.soft_w, g.soft_h, (int)g.extent.width,
                             (int)g.extent.height, pace_ms);
        if (pr == 1) {
            g.dirty = 0;
            g.last_present_tick = GetTickCount();
            dm_replace_beacon_tick();
            return 1;
        }
        if (pr == -1) {
            dm_replace_beacon_tick();
            return 1;
        }
        return 0;
    }
}

int vk_present_ready(void)
{
    return g.ready && g.staging_ptr && g.fb_w > 0 && g.fb_h > 0 ? 1 : 0;
}

unsigned vk_present_fb_sample4(void)
{
    int w, h, cx, cy;
    const uint8_t *p;
    unsigned a, b, c, d;
    if (!g.fb || g.fb_w < 64 || g.fb_h < 64)
        return 0;
    w = g.fb_w;
    h = g.fb_h;
    cx = w / 4;
    cy = h / 2;
    p = g.fb + ((size_t)cy * (size_t)w + (size_t)cx) * 4u;
    a = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    p = g.fb + ((size_t)cy * (size_t)w + (size_t)(cx + 8)) * 4u;
    b = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    p = g.fb + ((size_t)(cy + 8) * (size_t)w + (size_t)cx) * 4u;
    c = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    p = g.fb + ((size_t)(cy + 8) * (size_t)w + (size_t)(cx + 8)) * 4u;
    d = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3);
}

void vk_present_set_pan_shift(int dx, int dy)
{
    g.pan_shift_x = dx;
    g.pan_shift_y = dy;
}

int vk_present_scroll_playfield(int dx, int dy)
{
    /* No longer mutates g.fb (black-edge artifacts). Present-time shift only. */
    vk_present_set_pan_shift(dx, dy);
    return (dx || dy) ? 1 : 0;
}

int vk_present_scale_active(void)
{
    char mode[32];
    DWORD n;
    if (!g.ready || g.soft_w <= 0 || g.soft_h <= 0)
        return 0;
    if (g.soft_w == (int)g.extent.width && g.soft_h == (int)g.extent.height)
        return 0;
    n = GetEnvironmentVariableA("CK_VK_SCALE", mode, (DWORD)sizeof(mode));
    if (n > 0 && n < sizeof(mode) && mode[0] == '0' && mode[1] == '\0')
        return 0;
    return 1;
}

int vk_present_soft_size(int *w, int *h)
{
    if (!g.ready || g.soft_w <= 0 || g.soft_h <= 0)
        return 0;
    if (w)
        *w = g.soft_w;
    if (h)
        *h = g.soft_h;
    return 1;
}

HWND vk_present_hwnd(void)
{
    return g.ready ? g.hwnd : NULL;
}

void vk_present_movie_begin(HWND hwnd)
{
    LONGLONG t0 = hitch_qpc_now();
    (void)hwnd;
    hitch_mark("movie-begin");
    if (g.ready && g.device) {
        g.fn.vkDeviceWaitIdle(g.device);
        destroy_swapchain();
        log_msg("vk_present: movie_begin — swapchain destroyed for GDI VR");
        hitch_note_ms("F7", "vk_present.c:movie_begin", "movie-begin-idle", "movie",
                      hitch_qpc_ms_since(t0), "{}");
    }
}

void vk_present_movie_end(void)
{
    /* Next SetDIBits / movie frame will recreate swapchain via vk_init path. */
    if (g.ready && g.hwnd && !g.swapchain) {
        if (!create_swapchain())
            log_msg("vk_present: movie_end recreate swapchain failed");
        else
            log_msg("vk_present: movie_end — swapchain restored");
    }
}

/* After cutscenes: restore pre-movie game soft (match DIB; e.g. 1536×864 under gamescope). */
void vk_present_restore_game_soft(void)
{
    int prev_w = g.soft_w, prev_h = g.soft_h;
    int target_w, target_h;
    if (!g.ready || g.extent.width < 64 || g.extent.height < 64)
        return;
    g.force_letterbox = 0;
    if (g.game_soft_w >= 800 && g.game_soft_h >= 600) {
        target_w = g.game_soft_w;
        target_h = g.game_soft_h;
    } else {
        target_w = (int)g.extent.width;
        target_h = (int)g.extent.height;
    }
    g.soft_w = target_w;
    g.soft_h = target_h;
    g.scale_logged = 0;
    if (prev_w != g.soft_w || prev_h != g.soft_h) {
        log_msg("vk_present: restore soft %dx%d -> %dx%d (game soft)", prev_w, prev_h, g.soft_w,
                g.soft_h);
    }

}

static int present_bgra_frame(HWND hwnd, const void *bgra, int w, int h, int letterbox)
{
    RECT rc;
    int cw, ch;

    if (!bgra || w < 1 || h < 1)
        return 0;
    if (!vk_replace_wanted())
        return 0;
    if (g.failed)
        return 0;
    if (!hwnd)
        hwnd = g.hwnd;
    if (!hwnd)
        return 0;

    /* Menu host may switch from tpw HWND to its own window — rebind Vulkan. */
    if (g.ready && hwnd != g.hwnd) {
        log_msg("vk_present: rebind hwnd %p -> %p", (void *)g.hwnd, (void *)hwnd);
        vk_present_shutdown();
    }

    if (!g.ready && !vk_init(hwnd))
        return 0;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw < 64 || ch < 64)
        return 0;

    if ((uint32_t)cw != g.extent.width || (uint32_t)ch != g.extent.height) {
        destroy_swapchain();
        if (!create_swapchain()) {
            g.failed = 1;
            return 0;
        }
    }

    if (!ensure_fb(cw, ch))
        return 0;
    {
        int dw, dh, ox, oy, y, x;
        memset(g.fb, 0, (size_t)cw * (size_t)ch * 4u);
        if (!letterbox) {
            /* RE Renderer::draw_frame — fullscreen stretch. */
            dw = cw;
            dh = ch;
            ox = 0;
            oy = 0;
        } else if (w * ch <= h * cw) {
            dh = ch;
            dw = w * ch / h;
            ox = (cw - dw) / 2;
            oy = (ch - dh) / 2;
        } else {
            dw = cw;
            dh = h * cw / w;
            ox = (cw - dw) / 2;
            oy = (ch - dh) / 2;
        }
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        for (y = 0; y < dh; ++y) {
            int fy = ((y * 2 + 1) * h * 128) / dh - 128;
            uint8_t *row = g.fb + ((size_t)(oy + y) * (size_t)cw + (size_t)ox) * 4u;
            for (x = 0; x < dw; ++x) {
                int fx = ((x * 2 + 1) * w * 128) / dw - 128;
                sample_bgra_bilinear((const uint8_t *)bgra, w, h, fx, fy, row + (size_t)x * 4u);
            }
        }
    }
    if (g.game_soft_w >= 800 && g.game_soft_h >= 600) {
        g.soft_w = g.game_soft_w;
        g.soft_h = g.game_soft_h;
    } else {
        g.soft_w = cw;
        g.soft_h = ch;
    }
    g.have_full = 1;
    g.dirty = 1;
    g.force_letterbox = 0;
    {
        double fence_ms = 0, compose_ms = 0, acquire_ms = 0, submit_ms = 0;
        int pr = present_fb(&fence_ms, &compose_ms, &acquire_ms, &submit_ms);
        if (pr == 1) {
            g.dirty = 0;
            g.last_present_tick = GetTickCount();
            return 1;
        }
        return 0;
    }
}

int vk_present_movie_frame(HWND hwnd, const void *bgra, int w, int h)
{
    return present_bgra_frame(hwnd, bgra, w, h, 1);
}

int vk_present_menu_frame(HWND hwnd, const void *bgra, int w, int h)
{
    /* Keep 1024x768 (4:3) aspect inside widescreen client — pillar/letter bars. */
    return present_bgra_frame(hwnd, bgra, w, h, 1);
}
