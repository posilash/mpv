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

#pragma once

#include <libplacebo/renderer.h>

#include "mpv/render.h"
#include "video/out/gpu/ra.h"
#include "video/out/libmpv.h"

// Graphics-API-specific half of the gpu-next render backend. The backend itself
// (libmpv_gpu_next.c) is API-agnostic: it only needs a pl_gpu to render with and
// a pl_frame to render into. Everything that depends on which API the client
// handed us lives behind this interface, so adding Vulkan or D3D11 later means
// adding one of these, not touching the backend.
struct libmpv_gpu_next_context {
    struct mpv_global *global;
    struct mp_log *log;
    const struct libmpv_gpu_next_context_fns *fns;

    // Filled in by init().
    pl_log pllog;
    pl_gpu gpu;
    // Optional. When non-NULL the backend sets up hardware decoding interop
    // against it; when NULL, decoding falls back to software.
    struct ra_ctx *ra_ctx;

    // Colour space of the client's target, set by wrap_fbo(). Zeroed means
    // it did not say.
    struct pl_color_space target_csp;

    void *priv;
};

struct libmpv_gpu_next_context_fns {
    // The MPV_RENDER_API_TYPE_* string this implementation handles.
    const char *api_name;

    // Create the pl_gpu from the client's API objects. Returns a libmpv error
    // code; on failure destroy() is still called.
    int (*init)(struct libmpv_gpu_next_context *ctx, mpv_render_param *params);

    // Turn the client's render target (from mpv_render_context_render()
    // parameters) into a pl_frame. The returned frame is only valid until the
    // next call. Returns a libmpv error code.
    int (*wrap_fbo)(struct libmpv_gpu_next_context *ctx,
                    mpv_render_param *params, struct pl_frame *out);

    // Optional. Called after rendering has been recorded but before
    // done_frame(), with the same parameters wrap_fbo() got. APIs where the
    // client must be handed the target back in a defined state (Vulkan image
    // layouts) do that here; OpenGL leaves this NULL.
    void (*finish_target)(struct libmpv_gpu_next_context *ctx,
                          mpv_render_param *params);

    // Called after rendering has been submitted for one frame.
    void (*done_frame)(struct libmpv_gpu_next_context *ctx, bool display_synced);

    // Free everything init() created. Must tolerate a partially-initialised ctx.
    void (*destroy)(struct libmpv_gpu_next_context *ctx);
};

extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl;
extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk;
