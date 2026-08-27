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
#include "video/out/gpu/context.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_vulkan vk;
    // Bare ra_ctx wrapping the imported device, so hardware decoding interop
    // can be set up like for any other Vulkan context. It has no swapchain and
    // no window; interops that need more than an ra over the pl_gpu (i.e.
    // hwdec_vulkan, which wants the mpvk_ctx) skip themselves gracefully.
    struct ra_ctx *ra_ctx;
    pl_tex target;
    VkImage wrapped;        // which VkImage `target` wraps
    VkImageLayout layout;   // layout the client last told us about
    // Whether external access to `target` is currently held (by us/the
    // client) rather than owned by libplacebo. wrap_fbo() runs twice per
    // frame -- once via get_target_size(), once via render() -- and the image
    // must be released to libplacebo exactly once between finish_target()
    // holds, or libplacebo complains about releasing an unheld image.
    bool held;
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

    // An ra over the imported pl_gpu is all the hwdec machinery needs: every
    // interop driver only dereferences ra_ctx->ra (hwdec_cuda's Vulkan path
    // gets the pl_gpu back via ra_pl_get()). Without it we would still render
    // fine, but only from software-decoded or copied-back frames.
    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->log = ctx->log;
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->opts = (struct ra_ctx_opts){ .allow_sw = true };
    p->ra_ctx->ra = ra_create_pl(p->vk->gpu, ctx->log);
    if (p->ra_ctx->ra) {
        ctx->ra_ctx = p->ra_ctx;
    } else {
        mp_warn(ctx->log, "Failed creating an RA over the imported device; "
                          "hardware decoding will not be available.\n");
        TA_FREEP(&p->ra_ctx);
    }
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
        // Wrapped images start out owned by the client.
        p->held = true;
    }

    // Hand the image over to libplacebo, telling it the layout the client left
    // it in. Without this it would assume the contents are undefined and
    // discard whatever the client drew underneath. Skipped when the image was
    // already handed over this frame (see `held`).
    if (p->held) {
        pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
            .tex = p->target,
            .layout = fbo->layout,
            .qf = VK_QUEUE_FAMILY_IGNORED,
            .semaphore = { fbo->wait_semaphore, fbo->wait_value },
        ));
        p->held = false;
        p->layout = fbo->layout;
    }

    int depth = *(int *)get_mpv_render_param(params, MPV_RENDER_PARAM_DEPTH,
                                             &(int){0});
    bool flip = *(int *)get_mpv_render_param(params, MPV_RENDER_PARAM_FLIP_Y,
                                             &(int){0});

    // What the client says its target is; zeroed means it did not say, and
    // then this is sRGB as before.
    ctx->target_csp = (struct pl_color_space){0};
    if (fbo->primaries || fbo->transfer) {
        ctx->target_csp = (struct pl_color_space){
            .primaries = fbo->primaries,
            .transfer  = fbo->transfer,
            .hdr = {
                .min_luma = fbo->min_luma,
                .max_luma = fbo->max_luma,
            },
        };
    }

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
        .color_space = ctx->target_csp.transfer ? ctx->target_csp
                                                : pl_color_space_srgb,
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

    bool ok = pl_vulkan_hold_ex(ctx->gpu, pl_vulkan_hold_params(
        .tex = p->target,
        .layout = want,
        .out_layout = want == VK_IMAGE_LAYOUT_UNDEFINED ? &got : NULL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { fbo->signal_semaphore, fbo->signal_value },
    ));
    p->held = ok;

    p->layout = got;
    fbo->out_layout = got;
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool display_synced)
{
    // Submit, but do not wait: the client synchronizes via the FBO's signal
    // semaphore, so a device-wide finish here would only serialize the CPU
    // against the GPU once per frame. Matches the GL backend's glFlush-level
    // behavior.
    pl_gpu_flush(ctx->gpu);
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->target)
        pl_tex_destroy(ctx->gpu, &p->target);
    if (p->ra_ctx && p->ra_ctx->ra) {
        p->ra_ctx->ra->fns->destroy(p->ra_ctx->ra);
        p->ra_ctx->ra = NULL;
    }
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
