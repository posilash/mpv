/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

#include <vulkan/vulkan_core.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vulkan backend, rendering with libplacebo (the vo=gpu-next renderer).
 *
 * Unlike the OpenGL backends, mpv does not create a device: it imports the
 * one the application already has, so both draw into the same VkImage without
 * a copy. The application stays in charge of the instance, device, queues and
 * of presentation; mpv only records rendering commands.
 *
 * Rendering requires:
 *
 *   MPV_RENDER_PARAM_API_TYPE             MPV_RENDER_API_TYPE_VULKAN
 *   MPV_RENDER_PARAM_VULKAN_INIT_PARAMS   mpv_vulkan_init_params
 *
 * and per frame:
 *
 *   MPV_RENDER_PARAM_VULKAN_FBO           mpv_vulkan_fbo
 *
 * Threading rules are the same as for the OpenGL backends: all
 * mpv_render_context_*() calls for one context must be serialised by the
 * caller. In addition, the queues named below must not be submitted to from
 * another thread while mpv_render_context_render() runs, unless lock_queue and
 * unlock_queue are provided.
 */

/**
 * A queue mpv may submit work to. Must belong to `device`.
 */
typedef struct mpv_vulkan_queue {
    /// Queue family index.
    uint32_t index;
    /// Number of queues available in this family that mpv may use, starting
    /// at queue 0. Use 1 if unsure.
    uint32_t count;
} mpv_vulkan_queue;

/**
 * For MPV_RENDER_PARAM_VULKAN_INIT_PARAMS.
 *
 * Every handle must outlive the mpv_render_context.
 */
typedef struct mpv_vulkan_init_params {
    /// The application's instance, physical device and logical device.
    VkInstance instance;
    VkPhysicalDevice phys_device;
    VkDevice device;
    /**
     * Loader entry point. Required, because mpv cannot assume it links against
     * the same loader as the application. Usually vkGetInstanceProcAddr.
     */
    PFN_vkGetInstanceProcAddr get_proc_addr;
    /**
     * The device extensions the application enabled when creating `device`,
     * and the features it enabled. mpv needs these to know what it may use;
     * getting them wrong causes validation errors rather than a clean failure.
     * `features` may be NULL, which is treated as "no optional features".
     */
    const char * const *extensions;
    int num_extensions;
    const VkPhysicalDeviceFeatures2 *features;
    /**
     * Queues mpv may submit to. `queue_graphics` must support
     * VK_QUEUE_GRAPHICS_BIT, `queue_compute` VK_QUEUE_COMPUTE_BIT and
     * `queue_transfer` VK_QUEUE_TRANSFER_BIT. They may all name the same
     * family, which is the common case.
     */
    mpv_vulkan_queue queue_graphics;
    mpv_vulkan_queue queue_compute;
    mpv_vulkan_queue queue_transfer;
    /**
     * Optional. If set, mpv calls these around every queue submission, so the
     * application can share queues with its own rendering thread. Both must be
     * set or both left NULL.
     */
    void (*lock_queue)(void *ctx, uint32_t queue_family, uint32_t queue_index);
    void (*unlock_queue)(void *ctx, uint32_t queue_family, uint32_t queue_index);
    void *queue_ctx;
} mpv_vulkan_init_params;

/**
 * For MPV_RENDER_PARAM_VULKAN_FBO. Describes the image to render into.
 *
 * The image must have been created with at least
 * VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, and with VK_IMAGE_USAGE_STORAGE_BIT if
 * the format supports it, otherwise mpv falls back to slower render paths.
 *
 * mpv transitions the image as it needs to and leaves it in `out_layout`. The
 * application must not assume the layout it passed in is still current when
 * mpv_render_context_render() returns.
 */
typedef struct mpv_vulkan_fbo {
    VkImage image;
    VkFormat format;
    int w;
    int h;
    /// Usage flags the image was created with.
    VkImageUsageFlags usage;
    /// Layout the image is in on entry. Use VK_IMAGE_LAYOUT_UNDEFINED if the
    /// contents do not need preserving.
    VkImageLayout layout;
    /// Layout mpv should leave the image in. If VK_IMAGE_LAYOUT_UNDEFINED,
    /// mpv picks one and reports it back here.
    VkImageLayout out_layout;
    /**
     * Semaphore mpv signals once it has finished rendering into `image`.
     * Required: rendering is asynchronous, so without it the application has
     * no way to know when the image may be read or presented. The application
     * must wait on it before touching the image again.
     *
     * For a timeline semaphore, set `signal_value` to the value mpv should
     * signal. Leave it 0 for a binary semaphore.
     */
    VkSemaphore signal_semaphore;
    uint64_t signal_value;
    /**
     * Optional. If the application has queued work of its own against `image`
     * (for example clearing it), the semaphore that signals when that work is
     * done, so mpv waits rather than racing it. NULL if there is none.
     */
    VkSemaphore wait_semaphore;
    uint64_t wait_value;
    /**
     * Colour space of `image`. Zeroed means unknown and mpv treats the target
     * as sRGB, as it did before these existed.
     *
     * --target-colorspace-hint reconfigures a swapchain mpv owns, and through
     * this API it owns none, so without these HDR is tone-mapped to sRGB
     * whatever the source. mpv reports what it settled on through
     * `video-target-params`.
     *
     * Same values as --target-prim and --target-trc. Luminance in cd/m^2,
     * 0 = unknown.
     */
    int primaries;
    int transfer;
    float min_luma;
    float max_luma;
} mpv_vulkan_fbo;

#ifdef __cplusplus
}
#endif

#endif
