/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <assert.h>

#include "iris_batch.h"
#include "iris_resource.h"
#include "iris_context.h"

#include "blorp/blorp_genX_exec.h"
#include "util/u_upload_mgr.h"

static uint32_t *
stream_state(struct iris_batch *batch,
             struct u_upload_mgr *uploader,
             unsigned size,
             unsigned alignment,
             uint32_t *out_offset,
             struct iris_bo **out_bo,
             bool relative_to_base)
{
   struct pipe_resource *res = NULL;
   void *ptr = NULL;

   u_upload_alloc(uploader, 0, size, alignment, out_offset, &res, &ptr);

   struct iris_bo *bo = iris_resource_bo(res);
   iris_use_pinned_bo(batch, bo, false);

   *out_bo = bo;
   *out_offset += relative_to_base ? iris_bo_offset_from_base_address(bo)
                                   : bo->gtt_offset;

   pipe_resource_reference(&res, NULL);

   return ptr;
}

static void *
blorp_emit_dwords(struct blorp_batch *blorp_batch, unsigned n)
{
   struct iris_batch *batch = blorp_batch->driver_batch;
   return iris_get_command_space(batch, n * sizeof(uint32_t));
}

static uint64_t
combine_and_pin_address(struct blorp_batch *blorp_batch,
                        struct blorp_address addr)
{
   struct iris_batch *batch = blorp_batch->driver_batch;
   struct iris_bo *bo = addr.buffer;

   iris_use_pinned_bo(batch, bo, addr.reloc_flags & RELOC_WRITE);

   /* Assume this is a general address, not relative to a base. */
   return bo->gtt_offset + addr.offset;
}

static uint64_t
blorp_emit_reloc(struct blorp_batch *blorp_batch, UNUSED void *location,
                 struct blorp_address addr, uint32_t delta)
{
   return combine_and_pin_address(blorp_batch, addr) + delta;
}

static void
blorp_surface_reloc(struct blorp_batch *blorp_batch, uint32_t ss_offset,
                    struct blorp_address addr, uint32_t delta)
{
   /* Let blorp_get_surface_address do the pinning. */
}

static uint64_t
blorp_get_surface_address(struct blorp_batch *blorp_batch,
                          struct blorp_address addr)
{
   return combine_and_pin_address(blorp_batch, addr);
}

UNUSED static struct blorp_address
blorp_get_surface_base_address(UNUSED struct blorp_batch *blorp_batch)
{
   return (struct blorp_address) { .offset = IRIS_MEMZONE_SURFACE_START };
}

static void *
blorp_alloc_dynamic_state(struct blorp_batch *blorp_batch,
                          uint32_t size,
                          uint32_t alignment,
                          uint32_t *offset)
{
   struct iris_context *ice = blorp_batch->blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;
   struct iris_bo *bo;

   return stream_state(batch, ice->state.dynamic_uploader,
                       size, alignment, offset, &bo, true);
}

static void
blorp_alloc_binding_table(struct blorp_batch *blorp_batch,
                          unsigned num_entries,
                          unsigned state_size,
                          unsigned state_alignment,
                          uint32_t *bt_offset,
                          uint32_t *surface_offsets,
                          void **surface_maps)
{
   struct iris_context *ice = blorp_batch->blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;
   struct iris_bo *bo;

   uint32_t *bt_map = iris_binder_reserve(&ice->state.binder,
                                          num_entries * sizeof(uint32_t),
                                          bt_offset);

   for (unsigned i = 0; i < num_entries; i++) {
      surface_maps[i] = stream_state(batch, ice->state.surface_uploader,
                                     state_size, state_alignment,
                                     &surface_offsets[i], &bo, true);
      bt_map[i] = surface_offsets[i];
   }
}

static void *
blorp_alloc_vertex_buffer(struct blorp_batch *blorp_batch,
                          uint32_t size,
                          struct blorp_address *addr)
{
   struct iris_context *ice = blorp_batch->blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;
   struct iris_bo *bo;
   uint32_t offset;

   void *map = stream_state(batch, ice->ctx.stream_uploader, size, 64,
                            &offset, &bo, false);

   *addr = (struct blorp_address) {
      .buffer = bo,
      .offset = offset,
      // XXX: Broadwell MOCS
      .mocs = I915_MOCS_CACHED,
   };

   return map;
}

/**
 * See vf_invalidate_for_vb_48b_transitions in iris_state.c.
 * XXX: actually add this
 */
static void
blorp_vf_invalidate_for_vb_48b_transitions(struct blorp_batch *batch,
                                           const struct blorp_address *addrs,
                                           unsigned num_vbs)
{
#if 0
   struct iris_context *ice = blorp_batch->blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;
   bool need_invalidate = false;

   for (unsigned i = 0; i < num_vbs; i++) {
      struct iris_bo *bo = addrs[i].buffer;
      uint16_t high_bits = bo ? bo->gtt_offset >> 32u : 0;

      if (high_bits != ice->state.last_vbo_high_bits[i]) {
         need_invalidate = true;
         ice->state.last_vbo_high_bits[i] = high_bits;
      }
   }

   if (need_invalidate) {
      iris_emit_pipe_control_flush(batch, PIPE_CONTROL_VF_CACHE_INVALIDATE);
   }
#endif
}

static struct blorp_address
blorp_get_workaround_page(struct blorp_batch *blorp_batch)
{
   struct iris_batch *batch = blorp_batch->driver_batch;

   return (struct blorp_address) { .buffer = batch->screen->workaround_bo };
}

static void
blorp_flush_range(UNUSED struct blorp_batch *blorp_batch,
                  UNUSED void *start,
                  UNUSED size_t size)
{
   /* All allocated states come from the batch which we will flush before we
    * submit it.  There's nothing for us to do here.
    */
}

static void
blorp_emit_urb_config(struct blorp_batch *blorp_batch,
                      unsigned vs_entry_size,
                      UNUSED unsigned sf_entry_size)
{
   // XXX: URB...
#if 0
   if (ice->urb.vsize >= vs_entry_size)
      return;

   gen7_upload_urb(ice, vs_entry_size, false, false);
#endif
}

static void
iris_blorp_exec(struct blorp_batch *blorp_batch,
                const struct blorp_params *params)
{
   struct iris_context *ice = blorp_batch->blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;

   /* Flush the sampler and render caches.  We definitely need to flush the
    * sampler cache so that we get updated contents from the render cache for
    * the glBlitFramebuffer() source.  Also, we are sometimes warned in the
    * docs to flush the cache between reinterpretations of the same surface
    * data with different formats, which blorp does for stencil and depth
    * data.
    */
   if (params->src.enabled)
      iris_cache_flush_for_read(batch, params->src.addr.buffer);
   if (params->dst.enabled) {
      iris_cache_flush_for_render(batch, params->dst.addr.buffer,
                                  params->dst.view.format,
                                  params->dst.aux_usage);
   }
   if (params->depth.enabled)
      iris_cache_flush_for_depth(batch, params->depth.addr.buffer);
   if (params->stencil.enabled)
      iris_cache_flush_for_depth(batch, params->stencil.addr.buffer);

   iris_require_command_space(batch, 1400);
   //iris_require_statebuffer_space(ice, 600); // XXX: THIS.  Need this.
   batch->no_wrap = true;

   // XXX: Emit L3 state

#if GEN_GEN == 8
   // XXX: PMA - gen8_write_pma_stall_bits(ice, 0);
#endif

   // XXX: knock this off...land Jason's i965 patches...
   blorp_emit(blorp_batch, GENX(3DSTATE_DRAWING_RECTANGLE), rect) {
      rect.ClippedDrawingRectangleXMax = MAX2(params->x1, params->x0) - 1;
      rect.ClippedDrawingRectangleYMax = MAX2(params->y1, params->y0) - 1;
   }

   blorp_exec(blorp_batch, params);

   batch->no_wrap = false;

   // XXX: aperture checks?

   /* We've smashed all state compared to what the normal 3D pipeline
    * rendering tracks for GL.
    */
   // XXX: skip some if (!(batch->flags & BLORP_BATCH_NO_EMIT_DEPTH_STENCIL))
   ice->state.dirty |= ~(IRIS_DIRTY_POLYGON_STIPPLE |
                         IRIS_DIRTY_LINE_STIPPLE);

#if 0
   ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS |
                       IRIS_DIRTY_COLOR_CALC_STATE |
                       IRIS_DIRTY_CONSTANTS_VS |
                       IRIS_DIRTY_CONSTANTS_TCS |
                       IRIS_DIRTY_CONSTANTS_TES |
                       IRIS_DIRTY_CONSTANTS_GS |
                       IRIS_DIRTY_CONSTANTS_PS |
                       IRIS_DIRTY_CONSTANTS_PS |
                       IRIS_DIRTY_SAMPLER_STATES_VS |
                       IRIS_DIRTY_SAMPLER_STATES_TCS |
                       IRIS_DIRTY_SAMPLER_STATES_TES |
                       IRIS_DIRTY_SAMPLER_STATES_GS |
                       IRIS_DIRTY_SAMPLER_STATES_PS |
                       IRIS_DIRTY_SAMPLER_STATES_PS |
                       IRIS_DIRTY_MULTISAMPLE |
                       IRIS_DIRTY_SAMPLE_MASK |
                       IRIS_DIRTY_VS |
                       IRIS_DIRTY_TCS |
                       IRIS_DIRTY_TES |
                       // IRIS_DIRTY_STREAMOUT |
                       IRIS_DIRTY_GS |
                       IRIS_DIRTY_CLIP |
                       IRIS_DIRTY_FS |
                       IRIS_DIRTY_CC_VIEWPORT |
#endif

   if (params->dst.enabled) {
      iris_render_cache_add_bo(batch, params->dst.addr.buffer,
                               params->dst.view.format,
                               params->dst.aux_usage);
   }
   if (params->depth.enabled)
      iris_depth_cache_add_bo(batch, params->depth.addr.buffer);
   if (params->stencil.enabled)
      iris_depth_cache_add_bo(batch, params->stencil.addr.buffer);
}

void
genX(init_blorp)(struct iris_context *ice)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;

   blorp_init(&ice->blorp, ice, &screen->isl_dev);
   ice->blorp.compiler = screen->compiler;
   ice->blorp.lookup_shader = iris_blorp_lookup_shader;
   ice->blorp.upload_shader = iris_blorp_upload_shader;
   ice->blorp.exec = iris_blorp_exec;
}