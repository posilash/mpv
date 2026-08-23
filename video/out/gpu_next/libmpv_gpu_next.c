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

#include "config.h"

#include "common/common.h"
#include "libmpv_gpu_next.h"
#include "renderer.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/libmpv.h"

// gpu-next render backend for the libmpv render API.
//
// This is deliberately thin. All rendering lives in the gpu-next renderer
// (video/out/gpu_next/renderer.h), shared verbatim with vo_gpu_next; the only
// thing this file adds is turning the client's render target into a pl_frame
// and driving the renderer's lifecycle from render_backend_fns.

static const struct libmpv_gpu_next_context_fns *context_backends[] = {
#if HAVE_GL
    &libmpv_gpu_next_context_gl,
#endif
    NULL
};

// `struct priv` here is the gpu-next renderer handle, which is opaque outside
// vo_gpu_next.c, so it can only be held by pointer.
struct backend_priv {
    struct libmpv_gpu_next_context *context;
    struct priv *renderer;
    struct vo *vo;
};

static void destroy(struct render_backend *ctx)
{
    struct backend_priv *p = ctx->priv;
    if (!p)
        return;

    if (p->renderer) {
        gpu_next_renderer_uninit(p->renderer, ctx->hwdec_devs);
        talloc_free(p->renderer);
        p->renderer = NULL;
    }

    hwdec_devices_destroy(ctx->hwdec_devs);
    ctx->hwdec_devs = NULL;

    if (p->context) {
        p->context->fns->destroy(p->context);
        talloc_free(p->context->priv);
        talloc_free(p->context);
        p->context = NULL;
    }
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct backend_priv);
    struct backend_priv *p = ctx->priv;

    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;

    for (int n = 0; context_backends[n]; n++) {
        const struct libmpv_gpu_next_context_fns *backend = context_backends[n];
        if (strcmp(backend->api_name, api) == 0) {
            p->context = talloc_zero(NULL, struct libmpv_gpu_next_context);
            *p->context = (struct libmpv_gpu_next_context){
                .global = ctx->global,
                .log = ctx->log,
                .fns = backend,
            };
            break;
        }
    }

    if (!p->context)
        return MPV_ERROR_NOT_IMPLEMENTED;

    int err = p->context->fns->init(p->context, params);
    if (err < 0)
        return err;

    p->renderer = gpu_next_renderer_alloc(NULL);
    gpu_next_renderer_preinit(p->renderer, ctx->global, ctx->log,
                              "libmpv/gpu-next");

    ctx->hwdec_devs = hwdec_devices_create();
    // true: there is no lazy hwdec loader on this path, so interops have to be
    // loaded up front or hardware decoding never becomes available.
    if (!gpu_next_renderer_init_gpu(p->renderer, p->context->pllog,
                                    p->context->gpu, p->context->ra_ctx,
                                    ctx->hwdec_devs, true))
        return MPV_ERROR_UNSUPPORTED;

    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct backend_priv *p = ctx->priv;
    return gpu_next_renderer_check_format(p->renderer, imgfmt);
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    // Nothing to do: unlike gl_video, the gpu-next renderer derives everything
    // it needs from the frames themselves.
}

static void reset(struct render_backend *ctx)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_renderer_reset(p->renderer);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct backend_priv *p = ctx->priv;

    // The renderer needs a VO for the OSD and for reporting target parameters
    // back to the player. It may legitimately be NULL between files.
    p->vo = vo;
    gpu_next_renderer_set_vo(p->renderer, vo);
    gpu_next_renderer_configure_queue(p->renderer, vo);
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_renderer_resize(p->renderer, src, dst, osd);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    struct backend_priv *p = ctx->priv;

    // Wrapping the target is cheap, and cheaper than a second entry point.
    struct pl_frame target;
    int err = p->context->fns->wrap_fbo(p->context, params, &target);
    if (err < 0)
        return err;

    *out_w = target.planes[0].texture->params.w;
    *out_h = target.planes[0].texture->params.h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct backend_priv *p = ctx->priv;

    struct pl_frame target;
    int err = p->context->fns->wrap_fbo(p->context, params, &target);
    if (err < 0)
        return err;

    // The client owns the target surface and tells us nothing about its colour
    // space, so leave it unknown. The renderer then applies the same fallback
    // vo_gpu_next uses for backends without target_csp() support.
    gpu_next_renderer_render(p->renderer, frame, &target,
                             (struct pl_color_space){0});

    p->context->fns->done_frame(p->context, frame->display_synced);
    return 0;
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    struct backend_priv *p = ctx->priv;
    return gpu_next_renderer_get_image(p->renderer, imgfmt, w, h, stride_align,
                                       flags);
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_renderer_perfdata(p->renderer, out);
}

const struct render_backend_fns render_backend_gpu_next = {
    .init = init,
    .check_format = check_format,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = get_image,
    .perfdata = perfdata,
    .destroy = destroy,
};
