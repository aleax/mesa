
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "translate/translate.h"

#include "nvc0_context.h"
#include "nvc0_resource.h"

#include "nvc0_3d.xml.h"

struct push_context {
   struct nouveau_channel *chan;

   void *idxbuf;

   float edgeflag;
   int edgeflag_attr;

   uint32_t vertex_words;
   uint32_t packet_vertex_limit;

   struct translate *translate;

   boolean primitive_restart;
   uint32_t prim;
   uint32_t restart_index;
   uint32_t instance_id;
};

static INLINE unsigned
prim_restart_search_i08(uint8_t *elts, unsigned push, uint8_t index)
{
   unsigned i;
   for (i = 0; i < push; ++i)
      if (elts[i] == index)
         break;
   return i;
}

static INLINE unsigned
prim_restart_search_i16(uint16_t *elts, unsigned push, uint16_t index)
{
   unsigned i;
   for (i = 0; i < push; ++i)
      if (elts[i] == index)
         break;
   return i;
}

static INLINE unsigned
prim_restart_search_i32(uint32_t *elts, unsigned push, uint32_t index)
{
   unsigned i;
   for (i = 0; i < push; ++i)
      if (elts[i] == index)
         break;
   return i;
}

static void
emit_vertices_i08(struct push_context *ctx, unsigned start, unsigned count)
{
   uint8_t *elts = (uint8_t *)ctx->idxbuf + start;

   while (count) {
      unsigned push = MIN2(count, ctx->packet_vertex_limit);
      unsigned size, nr;

      nr = push;
      if (ctx->primitive_restart)
         nr = prim_restart_search_i08(elts, push, ctx->restart_index);

      size = ctx->vertex_words * nr;

      BEGIN_RING_NI(ctx->chan, RING_3D(VERTEX_DATA), size);

      ctx->translate->run_elts8(ctx->translate, elts, nr, ctx->instance_id,
                                ctx->chan->cur);

      ctx->chan->cur += size;
      count -= nr;
      elts += nr;

      if (nr != push) {
         count--;
         elts++;
         BEGIN_RING(ctx->chan, RING_3D(VERTEX_END_GL), 2);
         OUT_RING  (ctx->chan, 0);
         OUT_RING  (ctx->chan, NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_CONT |
                    (ctx->prim & ~NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_NEXT));
      }
   }
}

static void
emit_vertices_i16(struct push_context *ctx, unsigned start, unsigned count)
{
   uint16_t *elts = (uint16_t *)ctx->idxbuf + start;

   while (count) {
      unsigned push = MIN2(count, ctx->packet_vertex_limit);
      unsigned size, nr;

      nr = push;
      if (ctx->primitive_restart)
         nr = prim_restart_search_i16(elts, push, ctx->restart_index);

      size = ctx->vertex_words * nr;

      BEGIN_RING_NI(ctx->chan, RING_3D(VERTEX_DATA), size);

      ctx->translate->run_elts16(ctx->translate, elts, nr, ctx->instance_id,
                                 ctx->chan->cur);

      ctx->chan->cur += size;
      count -= nr;
      elts += nr;

      if (nr != push) {
         count--;
         elts++;
         BEGIN_RING(ctx->chan, RING_3D(VERTEX_END_GL), 2);
         OUT_RING  (ctx->chan, 0);
         OUT_RING  (ctx->chan, NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_CONT |
                    (ctx->prim & ~NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_NEXT));
      }
   }
}

static void
emit_vertices_i32(struct push_context *ctx, unsigned start, unsigned count)
{
   uint32_t *elts = (uint32_t *)ctx->idxbuf + start;

   while (count) {
      unsigned push = MIN2(count, ctx->packet_vertex_limit);
      unsigned size, nr;

      nr = push;
      if (ctx->primitive_restart)
         nr = prim_restart_search_i32(elts, push, ctx->restart_index);

      size = ctx->vertex_words * nr;

      BEGIN_RING_NI(ctx->chan, RING_3D(VERTEX_DATA), size);

      ctx->translate->run_elts(ctx->translate, elts, nr, ctx->instance_id,
                               ctx->chan->cur);

      ctx->chan->cur += size;
      count -= nr;
      elts += nr;

      if (nr != push) {
         count--;
         elts++;
         BEGIN_RING(ctx->chan, RING_3D(VERTEX_END_GL), 2);
         OUT_RING  (ctx->chan, 0);
         OUT_RING  (ctx->chan, NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_CONT |
                    (ctx->prim & ~NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_NEXT));
      }
   }
}

static void
emit_vertices_seq(struct push_context *ctx, unsigned start, unsigned count)
{
   while (count) {
      unsigned push = MIN2(count, ctx->packet_vertex_limit);
      unsigned size = ctx->vertex_words * push;

      BEGIN_RING_NI(ctx->chan, RING_3D(VERTEX_DATA), size);

      ctx->translate->run(ctx->translate, start, push, ctx->instance_id,
                          ctx->chan->cur);
      ctx->chan->cur += size;
      count -= push;
      start += push;
   }
}


#define NVC0_PRIM_GL_CASE(n) \
   case PIPE_PRIM_##n: return NVC0_3D_VERTEX_BEGIN_GL_PRIMITIVE_##n

static INLINE unsigned
nvc0_prim_gl(unsigned prim)
{
   switch (prim) {
   NVC0_PRIM_GL_CASE(POINTS);
   NVC0_PRIM_GL_CASE(LINES);
   NVC0_PRIM_GL_CASE(LINE_LOOP);
   NVC0_PRIM_GL_CASE(LINE_STRIP);
   NVC0_PRIM_GL_CASE(TRIANGLES);
   NVC0_PRIM_GL_CASE(TRIANGLE_STRIP);
   NVC0_PRIM_GL_CASE(TRIANGLE_FAN);
   NVC0_PRIM_GL_CASE(QUADS);
   NVC0_PRIM_GL_CASE(QUAD_STRIP);
   NVC0_PRIM_GL_CASE(POLYGON);
   NVC0_PRIM_GL_CASE(LINES_ADJACENCY);
   NVC0_PRIM_GL_CASE(LINE_STRIP_ADJACENCY);
   NVC0_PRIM_GL_CASE(TRIANGLES_ADJACENCY);
   NVC0_PRIM_GL_CASE(TRIANGLE_STRIP_ADJACENCY);
   /*
   NVC0_PRIM_GL_CASE(PATCHES); */
   default:
      return NVC0_3D_VERTEX_BEGIN_GL_PRIMITIVE_POINTS;
      break;
   }
}

void
nvc0_push_vbo(struct nvc0_context *nvc0, const struct pipe_draw_info *info)
{
   struct push_context ctx;
   unsigned i, index_size;
   unsigned inst = info->instance_count;
   boolean apply_bias = info->indexed && info->index_bias;

   ctx.chan = nvc0->screen->base.channel;
   ctx.translate = nvc0->vertex->translate;
   ctx.packet_vertex_limit = nvc0->vertex->vtx_per_packet_max;
   ctx.vertex_words = nvc0->vertex->vtx_size;

   for (i = 0; i < nvc0->num_vtxbufs; ++i) {
      uint8_t *data;
      struct pipe_vertex_buffer *vb = &nvc0->vtxbuf[i];
      struct nvc0_resource *res = nvc0_resource(vb->buffer);

      data = nvc0_resource_map_offset(nvc0, res,
                                      vb->buffer_offset, NOUVEAU_BO_RD);

      if (apply_bias && likely(!(nvc0->vertex->instance_bufs & (1 << i))))
         data += info->index_bias * vb->stride;

      ctx.translate->set_buffer(ctx.translate, i, data, vb->stride, ~0);
   }

   if (info->indexed) {
      ctx.idxbuf = nvc0_resource_map_offset(nvc0,
                                            nvc0_resource(nvc0->idxbuf.buffer),
                                            nvc0->idxbuf.offset, NOUVEAU_BO_RD);
      if (!ctx.idxbuf)
         return;
      index_size = nvc0->idxbuf.index_size;
      ctx.primitive_restart = info->primitive_restart;
      ctx.restart_index = info->restart_index;
   } else {
      ctx.idxbuf = NULL;
      index_size = 0;
      ctx.primitive_restart = FALSE;
      ctx.restart_index = 0;
   }

   ctx.instance_id = info->start_instance;
   ctx.prim = nvc0_prim_gl(info->mode);

   while (inst--) {
      BEGIN_RING(ctx.chan, RING_3D(VERTEX_BEGIN_GL), 1);
      OUT_RING  (ctx.chan, ctx.prim);
      switch (index_size) {
      case 0:
         emit_vertices_seq(&ctx, info->start, info->count);
         break;
      case 1:
         emit_vertices_i08(&ctx, info->start, info->count);
         break;
      case 2:
         emit_vertices_i16(&ctx, info->start, info->count);
         break;
      case 4:
         emit_vertices_i32(&ctx, info->start, info->count);
         break;
      default:
         assert(0);
         break;
      }
      IMMED_RING(ctx.chan, RING_3D(VERTEX_END_GL), 0);

      ctx.instance_id++;
      ctx.prim |= NVC0_3D_VERTEX_BEGIN_GL_INSTANCE_NEXT;
   }

   if (info->indexed)
      nvc0_resource_unmap(nvc0_resource(nvc0->idxbuf.buffer));

   for (i = 0; i < nvc0->num_vtxbufs; ++i)
      nvc0_resource_unmap(nvc0_resource(nvc0->vtxbuf[i].buffer));
}