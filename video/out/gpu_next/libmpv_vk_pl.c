/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libplacebo/vulkan.h>

#include "libmpv_gpu_next.h"
#include "mpv/render_vk.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_vulkan vk;
    pl_tex target;
    VkImage wrapped;        // which VkImage `target` wraps
    VkImageLayout layout;   // layout the client last told us about
};

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_vulkan_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!init_params)
        return MPV_ERROR_INVALID_PARAMETER;
    if (!init_params->instance || !init_params->phys_device ||
        !init_params->device || !init_params->get_proc_addr)
        return MPV_ERROR_INVALID_PARAMETER;
    // Both or neither: a lock without an unlock would deadlock on first use.
    if (!init_params->lock_queue != !init_params->unlock_queue)
        return MPV_ERROR_INVALID_PARAMETER;

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_UNSUPPORTED;

    p->vk = pl_vulkan_import(ctx->pllog, pl_vulkan_import_params(
        .instance = init_params->instance,
        .get_proc_addr = init_params->get_proc_addr,
        .phys_device = init_params->phys_device,
        .device = init_params->device,
        .extensions = init_params->extensions,
        .num_extensions = init_params->num_extensions,
        .features = init_params->features,
        .queue_graphics = { init_params->queue_graphics.index,
                            init_params->queue_graphics.count },
        .queue_compute  = { init_params->queue_compute.index,
                            init_params->queue_compute.count },
        .queue_transfer = { init_params->queue_transfer.index,
                            init_params->queue_transfer.count },
        .lock_queue = init_params->lock_queue,
        .unlock_queue = init_params->unlock_queue,
        .queue_ctx = init_params->queue_ctx,
    ));
    if (!p->vk) {
        mp_fatal(ctx->log, "Failed importing the client's Vulkan device!\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->gpu = p->vk->gpu;
    // No ra_ctx: hardware decoding interop for Vulkan would need an ra wrapping
    // the imported device, which does not exist yet. Decoding still works, it
    // just does not stay on the GPU.
    ctx->ra_ctx = NULL;
    return 0;
}

static int wrap_fbo(struct libmpv_gpu_next_context *ctx,
                    mpv_render_param *params, struct pl_frame *out)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;
    if (!fbo->image || fbo->w <= 0 || fbo->h <= 0)
        return MPV_ERROR_INVALID_PARAMETER;
    // Rendering is asynchronous; without somewhere to signal completion the
    // client would have no way to know when the image is safe to use, so this
    // is a hard requirement rather than a best-effort.
    if (!fbo->signal_semaphore) {
        mp_err(ctx->log, "MPV_RENDER_PARAM_VULKAN_FBO requires "
                         "signal_semaphore to be set.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Rewrap whenever the client hands us a different image, or the same one
    // with different geometry. Wrapping is cheap; the texture is a view onto
    // memory the client owns.
    if (p->target && (p->wrapped != fbo->image ||
                      p->target->params.w != fbo->w ||
                      p->target->params.h != fbo->h))
    {
        pl_tex_destroy(ctx->gpu, &p->target);
    }

    if (!p->target) {
        p->target = pl_vulkan_wrap(ctx->gpu, pl_vulkan_wrap_params(
            .image = fbo->image,
            .width = fbo->w,
            .height = fbo->h,
            .format = fbo->format,
            .usage = fbo->usage,
        ));
        if (!p->target) {
            mp_err(ctx->log, "Failed wrapping the client's VkImage! Check that "
                             "its format and usage flags are supported.\n");
            return MPV_ERROR_UNSUPPORTED;
        }
        p->wrapped = fbo->image;
        // A freshly wrapped image is undefined as far as libplacebo knows.
        p->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // Hand the image over to libplacebo, telling it the layout the client left
    // it in. Without this it would assume the contents are undefined and
    // discard whatever the client drew underneath.
    pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
        .tex = p->target,
        .layout = fbo->layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { fbo->wait_semaphore, fbo->wait_value },
    ));
    p->layout = fbo->layout;

    int depth = *(int *)get_mpv_render_param(params, MPV_RENDER_PARAM_DEPTH,
                                             &(int){0});
    bool flip = *(int *)get_mpv_render_param(params, MPV_RENDER_PARAM_FLIP_Y,
                                             &(int){0});

    struct pl_swapchain_frame swframe = {
        .fbo = p->target,
        .flipped = flip,
        .color_repr = {
            .sys = PL_COLOR_SYSTEM_RGB,
            .levels = PL_COLOR_LEVELS_FULL,
            .alpha = PL_ALPHA_NONE,
            .bits.sample_depth = depth,
            .bits.color_depth = depth,
        },
        .color_space = pl_color_space_srgb,
    };
    pl_frame_from_swapchain(out, &swframe);
    return 0;
}

// Reclaim the image and put it in the layout the client asked for. Called after
// rendering, via done_frame(), because the layout is only settled once all
// rendering commands have been recorded.
static void finish_target(struct libmpv_gpu_next_context *ctx,
                          mpv_render_param *params)
{
    struct priv *p = ctx->priv;
    if (!p->target)
        return;

    mpv_vulkan_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_FBO, NULL);

    if (!fbo)
        return;
    VkImageLayout want = fbo->out_layout;
    VkImageLayout got = want;

    pl_vulkan_hold_ex(ctx->gpu, pl_vulkan_hold_params(
        .tex = p->target,
        .layout = want,
        .out_layout = want == VK_IMAGE_LAYOUT_UNDEFINED ? &got : NULL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { fbo->signal_semaphore, fbo->signal_value },
    ));

    p->layout = got;
    fbo->out_layout = got;
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool display_synced)
{
    pl_gpu_finish(ctx->gpu);
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->target)
        pl_tex_destroy(ctx->gpu, &p->target);
    if (p->vk)
        pl_vulkan_destroy(&p->vk);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk = {
    .api_name = MPV_RENDER_API_TYPE_VULKAN,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .finish_target = finish_target,
    .done_frame = done_frame,
    .destroy = destroy,
};
