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

#include <libplacebo/opengl.h>

#include "libmpv_gpu_next.h"
#include "options/m_config.h"
#include "mpv/render_gl.h"
#include "video/out/opengl/common.h"
#include "video/out/opengl/context.h"
#include "video/out/opengl/ra_gl.h"
#include "video/out/placebo/utils.h"

#if HAVE_EGL
#include <EGL/egl.h>
#endif

struct priv {
    GL *gl;
    pl_opengl opengl;
    // Only used to give the renderer something to hang hwdec interop off; the
    // swapchain it creates is never presented from, because the client owns
    // presentation.
    struct ra_ctx *ra_ctx;
    pl_tex target;
};

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_opengl_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, NULL);
    if (!init_params)
        return MPV_ERROR_INVALID_PARAMETER;

    p->gl = talloc_zero(p, GL);
    mpgl_load_functions2(p->gl, init_params->get_proc_address,
                         init_params->get_proc_address_ctx, NULL, ctx->log);
    if (!p->gl->version && !p->gl->es) {
        mp_fatal(ctx->log, "OpenGL not initialized.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    struct ra_ctx_opts *ctx_opts = mp_get_config_group(ctx, ctx->global,
                                                       &ra_ctx_conf);
    bool debug = ctx_opts->debug;
    bool allow_sw = ctx_opts->allow_sw;
    talloc_free(ctx_opts);

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_UNSUPPORTED;

    struct pl_opengl_params gl_params = *pl_opengl_params(
        .debug = debug,
        .allow_software = allow_sw,
        .get_proc_addr_ex = (void *) p->gl->get_fn,
        .proc_ctx = p->gl->fn_ctx,
    );
#if HAVE_EGL
    gl_params.egl_display = eglGetCurrentDisplay();
    gl_params.egl_context = eglGetCurrentContext();
#endif

    p->opengl = pl_opengl_create(ctx->pllog, &gl_params);
    if (!p->opengl) {
        mp_fatal(ctx->log, "Failed creating libplacebo OpenGL context!\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    ctx->gpu = p->opengl->gpu;

    // A blank ra_ctx wrapping the same GL context, so hardware decoding interop
    // can be set up exactly as it is for the legacy backend. Without it we would
    // still render fine, but only from software-decoded frames.
    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->log = ctx->log;
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->opts = (struct ra_ctx_opts){ .allow_sw = true, .debug = debug };
    p->gl->SwapInterval = NULL; // don't let anything change this behind the client's back
    if (ra_gl_ctx_init(p->ra_ctx, p->gl, (struct ra_ctx_params){0})) {
        ra_gl_set_debug(p->ra_ctx->ra, debug);
        ctx->ra_ctx = p->ra_ctx;
    } else {
        mp_warn(ctx->log, "Failed setting up RA context; "
                          "hardware decoding will be unavailable.\n");
        TA_FREEP(&p->ra_ctx);
    }

    return 0;
}

static int wrap_fbo(struct libmpv_gpu_next_context *ctx,
                    mpv_render_param *params, struct pl_frame *out)
{
    struct priv *p = ctx->priv;

    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    if (fbo->w <= 0 || fbo->h <= 0)
        return MPV_ERROR_INVALID_PARAMETER;

    // pl_opengl_wrap takes ownership of nothing; the texture object is a thin
    // handle onto the client's framebuffer, so it is recreated whenever the
    // client's target changes.
    if (p->target && (p->target->params.w != fbo->w ||
                      p->target->params.h != fbo->h))
        pl_tex_destroy(ctx->gpu, &p->target);

    if (!p->target) {
        p->target = pl_opengl_wrap(ctx->gpu, pl_opengl_wrap_params(
            .framebuffer = fbo->fbo,
            .width = fbo->w,
            .height = fbo->h,
            .iformat = fbo->internal_format,
        ));
        if (!p->target) {
            mp_err(ctx->log, "Failed wrapping client framebuffer!\n");
            return MPV_ERROR_UNSUPPORTED;
        }
    }

    bool flip = *(int *)get_mpv_render_param(params, MPV_RENDER_PARAM_FLIP_Y,
                                             &(int){0});
    int depth = *(int *)get_mpv_render_param(params, MPV_RENDER_PARAM_DEPTH,
                                             &(int){0});

    // Reuse libplacebo's own swapchain-frame conversion rather than filling in
    // a pl_frame by hand, so target setup matches vo_gpu_next exactly.
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

static void done_frame(struct libmpv_gpu_next_context *ctx, bool display_synced)
{
    struct priv *p = ctx->priv;
    pl_gpu_flush(ctx->gpu);
    (void) p;
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->target)
        pl_tex_destroy(ctx->gpu, &p->target);
    if (p->ra_ctx)
        ra_gl_ctx_uninit(p->ra_ctx);
    if (p->opengl)
        pl_opengl_destroy(&p->opengl);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl = {
    .api_name = MPV_RENDER_API_TYPE_OPENGL_NEXT,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
