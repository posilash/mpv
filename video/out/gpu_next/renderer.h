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

#include "video/mp_image.h"
#include "video/out/gpu/ra.h"
#include "video/out/vo.h"

// The gpu-next renderer, as used by vo_gpu_next and by the libmpv render API.
// Opaque: the definition lives in vo_gpu_next.c.
//
// This mirrors the split between gl_video and vo_gpu: the renderer owns
// everything from the pl_gpu upwards (pl_renderer, pl_queue, options, hwdec,
// OSD, caches), and knows nothing about how the output surface is obtained.
// Callers supply an already-acquired pl_frame to render into.
struct priv;

struct mp_hwdec_devices;

// Allocate a zeroed renderer under `ta_parent`. vo_gpu_next does not use this:
// its instance is allocated by the VO framework as vo->priv.
struct priv *gpu_next_renderer_alloc(void *ta_parent);

// First half of construction: everything that does not need a GPU. Must be
// called before gpu_next_renderer_init_gpu().
void gpu_next_renderer_preinit(struct priv *p, struct mpv_global *global,
                               struct mp_log *log, const char *stats_name);

// Second half: binds the renderer to a GPU. `ra_ctx` and `hwdec_devs` may be
// NULL, in which case hardware decoding interop is not set up. Returns false on
// failure, after which gpu_next_renderer_uninit() is still safe to call.
//
// `load_all_hwdecs` must be true unless the caller has registered a lazy loader
// on `hwdec_devs` via hwdec_devices_set_loader(). vo_gpu_next does, and so
// loads interops on demand; the libmpv render API has no such hook, so without
// this everything silently falls back to software decoding.
bool gpu_next_renderer_init_gpu(struct priv *p, pl_log pllog, pl_gpu gpu,
                                struct ra_ctx *ra_ctx,
                                struct mp_hwdec_devices *hwdec_devs,
                                bool load_all_hwdecs);

// Tear down everything created by the two init halves. Does not free `p`, and
// does not destroy `hwdec_devs`, which the caller owns.
void gpu_next_renderer_uninit(struct priv *p, struct mp_hwdec_devices *hwdec_devs);

// The VO this renderer serves, or NULL when driven through the libmpv render
// API before a VO has attached. Setting it is how the libmpv backend hands over
// the OSD and the target-parameter reporting path.
void gpu_next_renderer_set_vo(struct priv *p, struct vo *vo);

// Apply the queue depth the renderer wants to `vo`. No-op if `vo` is NULL.
void gpu_next_renderer_configure_queue(struct priv *p, struct vo *vo);

bool gpu_next_renderer_check_format(struct priv *p, int imgfmt);
void gpu_next_renderer_config(struct priv *p, struct mp_image_params *params);
void gpu_next_renderer_resize(struct priv *p, struct mp_rect *src,
                              struct mp_rect *dst, struct mp_osd_res *osd);
void gpu_next_renderer_reset(struct priv *p);
struct mp_image *gpu_next_renderer_get_image(struct priv *p, int imgfmt, int w,
                                             int h, int stride_align, int flags);
void gpu_next_renderer_perfdata(struct priv *p,
                                struct voctrl_performance_data *out);

// Render into an already-acquired target. `target_csp` describes the output
// surface; pass {0} if unknown, which selects the same fallback vo_gpu_next
// uses for backends without target_csp() support.
//
// Colour space hinting is deliberately not done here: it is swapchain business
// and meaningless for a caller-supplied FBO, so vo_gpu_next performs it between
// negotiation and acquisition.
bool gpu_next_renderer_render(struct priv *p, struct vo_frame *frame,
                              struct pl_frame *target,
                              struct pl_color_space target_csp);
