/*
 * Copyright 2010 Christoph Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nvc0_pc.h"
#include "nvc0_program.h"

#define DESCEND_ARBITRARY(j, f)                                 \
do {                                                            \
   b->pass_seq = ctx->pc->pass_seq;                             \
                                                                \
   for (j = 0; j < 2; ++j)                                      \
      if (b->out[j] && b->out[j]->pass_seq < ctx->pc->pass_seq) \
         f(ctx, b->out[j]);	                                  \
} while (0)

static INLINE boolean
registers_interfere(struct nv_value *a, struct nv_value *b)
{
   if (a->reg.file != b->reg.file)
      return FALSE;
   if (NV_IS_MEMORY_FILE(a->reg.file) || NV_IS_MEMORY_FILE(b->reg.file))
      return FALSE;

   assert(a->join->reg.id >= 0 && b->join->reg.id >= 0);

   if (a->join->reg.id < b->join->reg.id) {
      return (a->join->reg.id + a->reg.size >= b->join->reg.id);
   } else
   if (a->join->reg.id > b->join->reg.id) {
      return (b->join->reg.id + b->reg.size >= a->join->reg.id);
   }

   return FALSE;
}

static INLINE boolean
values_equal(struct nv_value *a, struct nv_value *b)
{
   if (a->reg.file != b->reg.file || a->reg.size != b->reg.size)
      return FALSE;
   if (NV_IS_MEMORY_FILE(a->reg.file))
      return a->reg.address == b->reg.address;
   else
      return a->join->reg.id == b->join->reg.id;
}

#if 0
static INLINE boolean
inst_commutation_check(struct nv_instruction *a, struct nv_instruction *b)
{
   int si, di;

   for (di = 0; di < 4 && a->def[di]; ++di)
      for (si = 0; si < 5 && b->src[si]; ++si)
         if (registers_interfere(a->def[di], b->src[si]->value))
            return FALSE;

   return TRUE;
}

/* Check whether we can swap the order of the instructions,
 * where a & b may be either the earlier or the later one.
 */
static boolean
inst_commutation_legal(struct nv_instruction *a, struct nv_instruction *b)
{
   return inst_commutation_check(a, b) && inst_commutation_check(b, a);
}
#endif

static INLINE boolean
inst_removable(struct nv_instruction *nvi)
{
   if (nvi->opcode == NV_OP_ST)
      return FALSE;
   return (!(nvi->terminator ||
             nvi->join ||
             nvi->target ||
             nvi->fixed ||
             nvc0_insn_refcount(nvi)));
}

static INLINE boolean
inst_is_noop(struct nv_instruction *nvi)
{
   if (nvi->opcode == NV_OP_UNDEF || nvi->opcode == NV_OP_BIND)
      return TRUE;
   if (nvi->terminator || nvi->join)
      return FALSE;
   if (nvi->def[0] && nvi->def[0]->join->reg.id < 0)
      return TRUE;
   if (nvi->opcode != NV_OP_MOV && nvi->opcode != NV_OP_SELECT)
      return FALSE;
   if (nvi->def[0]->reg.file != nvi->src[0]->value->reg.file)
      return FALSE;

   if (nvi->src[0]->value->join->reg.id < 0) {
      NOUVEAU_DBG("inst_is_noop: orphaned value detected\n");
      return TRUE;
   }

   if (nvi->opcode == NV_OP_SELECT)
      if (!values_equal(nvi->def[0], nvi->src[1]->value))
         return FALSE;
   return values_equal(nvi->def[0], nvi->src[0]->value);
}

struct nv_pass {
   struct nv_pc *pc;
   int n;
   void *priv;
};

static int
nv_pass_flatten(struct nv_pass *ctx, struct nv_basic_block *b);

static void
nv_pc_pass_pre_emission(void *priv, struct nv_basic_block *b)
{
   struct nv_pc *pc = (struct nv_pc *)priv;
   struct nv_basic_block *in;
   struct nv_instruction *nvi, *next;
   int j;

   for (j = pc->num_blocks - 1; j >= 0 && !pc->bb_list[j]->emit_size; --j);

   if (j >= 0) {
      in = pc->bb_list[j];

      /* check for no-op branches (BRA $PC+8) */
      if (in->exit && in->exit->opcode == NV_OP_BRA && in->exit->target == b) {
         in->emit_size -= 8;
         pc->emit_size -= 8;

         for (++j; j < pc->num_blocks; ++j)
            pc->bb_list[j]->emit_pos -= 8;

         nvc0_insn_delete(in->exit);
      }
      b->emit_pos = in->emit_pos + in->emit_size;
   }

   pc->bb_list[pc->num_blocks++] = b;

   /* visit node */

   for (nvi = b->entry; nvi; nvi = next) {
      next = nvi->next;
      if (inst_is_noop(nvi) ||
          (pc->is_fragprog && nvi->opcode == NV_OP_EXPORT)) {
         nvc0_insn_delete(nvi);
      } else
         b->emit_size += 8;
   }
   pc->emit_size += b->emit_size;

#ifdef NOUVEAU_DEBUG
   if (!b->entry)
      debug_printf("BB:%i is now empty\n", b->id);
   else
      debug_printf("BB:%i size = %u\n", b->id, b->emit_size);
#endif
}

static int
nv_pc_pass2(struct nv_pc *pc, struct nv_basic_block *root)
{
   struct nv_pass pass;

   pass.pc = pc;

   pc->pass_seq++;
   nv_pass_flatten(&pass, root);

   nvc0_pc_pass_in_order(root, nv_pc_pass_pre_emission, pc);

   return 0;
}

int
nvc0_pc_exec_pass2(struct nv_pc *pc)
{
   int i, ret;

   NOUVEAU_DBG("preparing %u blocks for emission\n", pc->num_blocks);

   pc->num_blocks = 0; /* will reorder bb_list */

   for (i = 0; i < pc->num_subroutines + 1; ++i)
      if (pc->root[i] && (ret = nv_pc_pass2(pc, pc->root[i])))
         return ret;
   return 0;
}

static INLINE boolean
is_cspace_load(struct nv_instruction *nvi)
{
   if (!nvi)
      return FALSE;
   assert(nvi->indirect != 0);
   return (nvi->opcode == NV_OP_LD &&
           nvi->src[0]->value->reg.file >= NV_FILE_MEM_C(0) &&
           nvi->src[0]->value->reg.file <= NV_FILE_MEM_C(15));
}

static INLINE boolean
is_immd32_load(struct nv_instruction *nvi)
{
   if (!nvi)
      return FALSE;
   return (nvi->opcode == NV_OP_MOV &&
           nvi->src[0]->value->reg.file == NV_FILE_IMM &&
           nvi->src[0]->value->reg.size == 4);
}

static INLINE void
check_swap_src_0_1(struct nv_instruction *nvi)
{
   static const uint8_t cc_swapped[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

   struct nv_ref *src0 = nvi->src[0];
   struct nv_ref *src1 = nvi->src[1];

   if (!nv_op_commutative(nvi->opcode) && NV_BASEOP(nvi->opcode) != NV_OP_SET)
      return;
   assert(src0 && src1 && src0->value && src1->value);

   if (is_cspace_load(src0->value->insn)) {
      if (!is_cspace_load(src1->value->insn)) {
         nvi->src[0] = src1;
         nvi->src[1] = src0;
      }
   } else
   if (is_immd32_load(src0->value->insn)) {
      if (!is_cspace_load(src1->value->insn) &&
          !is_immd32_load(src1->value->insn)) {
         nvi->src[0] = src1;
         nvi->src[1] = src0;
      }
   }

   if (nvi->src[0] != src0 && nvi->opcode == NV_OP_SET)
      nvi->set_cond = cc_swapped[nvi->set_cond];
}

static void
nvi_set_indirect_load(struct nv_pc *pc,
                      struct nv_instruction *nvi, struct nv_value *val)
{
   for (nvi->indirect = 0; nvi->indirect < 6 && nvi->src[nvi->indirect];
        ++nvi->indirect);
   assert(nvi->indirect < 6);
   nv_reference(pc, nvi, nvi->indirect, val);
}

static int
nvc0_pass_fold_loads(struct nv_pass *ctx, struct nv_basic_block *b)
{
   struct nv_instruction *nvi, *ld;
   int s;

   for (nvi = b->entry; nvi; nvi = nvi->next) {
      check_swap_src_0_1(nvi);

      for (s = 0; s < 3 && nvi->src[s]; ++s) {
         ld = nvi->src[s]->value->insn;
         if (!ld || (ld->opcode != NV_OP_LD && ld->opcode != NV_OP_MOV))
            continue;
         if (!nvc0_insn_can_load(nvi, s, ld))
            continue;

         /* fold it ! */
         nv_reference(ctx->pc, nvi, s, ld->src[0]->value);
         if (ld->indirect >= 0)
            nvi_set_indirect_load(ctx->pc, nvi, ld->src[ld->indirect]->value);

         if (!nvc0_insn_refcount(ld))
            nvc0_insn_delete(ld);
      }
   }
   DESCEND_ARBITRARY(s, nvc0_pass_fold_loads);

   return 0;
}

/* NOTE: Assumes loads have not yet been folded. */
static int
nv_pass_lower_mods(struct nv_pass *ctx, struct nv_basic_block *b)
{
   struct nv_instruction *nvi, *mi, *next;
   int j;
   uint8_t mod;

   for (nvi = b->entry; nvi; nvi = next) {
      next = nvi->next;
      if (nvi->opcode == NV_OP_SUB) {
         nvi->src[1]->mod ^= NV_MOD_NEG;
         nvi->opcode = NV_OP_ADD;
      }

      for (j = 0; j < 3 && nvi->src[j]; ++j) {
         mi = nvi->src[j]->value->insn;
         if (!mi)
            continue;
         if (mi->def[0]->refc > 1 || mi->predicate >= 0)
            continue;

         if (NV_BASEOP(mi->opcode) == NV_OP_NEG) mod = NV_MOD_NEG;
         else
         if (NV_BASEOP(mi->opcode) == NV_OP_ABS) mod = NV_MOD_ABS;
         else
            continue;
         assert(!(mod & mi->src[0]->mod & NV_MOD_NEG));

         mod |= mi->src[0]->mod;

         if ((nvi->opcode == NV_OP_ABS) || (nvi->src[j]->mod & NV_MOD_ABS)) {
            /* abs neg [abs] = abs */
            mod &= ~(NV_MOD_NEG | NV_MOD_ABS);
         } else
         if ((nvi->opcode == NV_OP_NEG) && (mod & NV_MOD_NEG)) {
            /* neg as opcode and modifier on same insn cannot occur */
            /* neg neg abs = abs, neg neg = identity */
            assert(j == 0);
            if (mod & NV_MOD_ABS)
               nvi->opcode = NV_OP_ABS;
            else
               nvi->opcode = NV_OP_MOV;
            mod = 0;
         }

         if ((nv_op_supported_src_mods(nvi->opcode) & mod) != mod)
            continue;

         nv_reference(ctx->pc, nvi, j, mi->src[0]->value);

         nvi->src[j]->mod ^= mod;
      }

      if (nvi->opcode == NV_OP_SAT) {
         mi = nvi->src[0]->value->insn;

         if (mi->def[0]->refc > 1 ||
             (mi->opcode != NV_OP_ADD &&
              mi->opcode != NV_OP_MUL &&
              mi->opcode != NV_OP_MAD))
            continue;
         mi->saturate = 1;
         mi->def[0] = nvi->def[0];
         mi->def[0]->insn = mi;
         nvc0_insn_delete(nvi);
      }
   }
   DESCEND_ARBITRARY(j, nv_pass_lower_mods);

   return 0;
}

#define SRC_IS_MUL(s) ((s)->insn && (s)->insn->opcode == NV_OP_MUL)

static void
apply_modifiers(uint32_t *val, uint8_t type, uint8_t mod)
{
   if (mod & NV_MOD_ABS) {
      if (type == NV_TYPE_F32)
         *val &= 0x7fffffff;
      else
      if ((*val) & (1 << 31))
         *val = ~(*val) + 1;
   }
   if (mod & NV_MOD_NEG) {
      if (type == NV_TYPE_F32)
         *val ^= 0x80000000;
      else
         *val = ~(*val) + 1;
   }
   if (mod & NV_MOD_SAT) {
      union {
         float f;
         uint32_t u;
         int32_t i;
      } u;
      u.u = *val;
      if (type == NV_TYPE_F32) {
         u.f = CLAMP(u.f, -1.0f, 1.0f);
      } else
      if (type == NV_TYPE_U16) {
         u.u = MIN2(u.u, 0xffff);
      } else
      if (type == NV_TYPE_S16) {
         u.i = CLAMP(u.i, -32768, 32767);
      }
      *val = u.u;
   }
   if (mod & NV_MOD_NOT)
      *val = ~*val;
}

static void
constant_expression(struct nv_pc *pc, struct nv_instruction *nvi,
                    struct nv_value *src0, struct nv_value *src1)
{
   struct nv_value *val;
   union {
      float f32;
      uint32_t u32;
      int32_t s32;
   } u0, u1, u;
   ubyte type;

   if (!nvi->def[0])
      return;
   type = NV_OPTYPE(nvi->opcode);

   u.u32 = 0;
   u0.u32 = src0->reg.imm.u32;
   u1.u32 = src1->reg.imm.u32;

   apply_modifiers(&u0.u32, type, nvi->src[0]->mod);
   apply_modifiers(&u1.u32, type, nvi->src[1]->mod);

   switch (nvi->opcode) {
   case NV_OP_MAD_F32:
      if (nvi->src[2]->value->reg.file != NV_FILE_GPR)
         return;
      /* fall through */
   case NV_OP_MUL_F32:
      u.f32 = u0.f32 * u1.f32;
      break;
   case NV_OP_MUL_B32:
      u.u32 = u0.u32 * u1.u32;
      break;
   case NV_OP_ADD_F32:
      u.f32 = u0.f32 + u1.f32;
      break;
   case NV_OP_ADD_B32:
      u.u32 = u0.u32 + u1.u32;
      break;
   case NV_OP_SUB_F32:
      u.f32 = u0.f32 - u1.f32;
      break;
      /*
   case NV_OP_SUB_B32:
      u.u32 = u0.u32 - u1.u32;
      break;
      */
   default:
      return;
   }

   val = new_value(pc, NV_FILE_IMM, nv_type_sizeof(type));
   val->reg.imm.u32 = u.u32;

   nv_reference(pc, nvi, 1, NULL);
   nv_reference(pc, nvi, 0, val);

   if (nvi->opcode == NV_OP_MAD_F32) {
      nvi->src[1] = nvi->src[0];
      nvi->src[0] = nvi->src[2];
      nvi->src[2] = NULL;
      nvi->opcode = NV_OP_ADD_F32;

      if (val->reg.imm.u32 == 0) {
         nvi->src[1] = NULL;
         nvi->opcode = NV_OP_MOV;
      }
   } else {
      nvi->opcode = NV_OP_MOV;
   }
}

static void
constant_operand(struct nv_pc *pc,
                 struct nv_instruction *nvi, struct nv_value *val, int s)
{
   union {
      float f32;
      uint32_t u32;
      int32_t s32;
   } u;
   int shift;
   int t = s ? 0 : 1;
   uint op;
   ubyte type;

   if (!nvi->def[0])
      return;
   type = NV_OPTYPE(nvi->opcode);

   u.u32 = val->reg.imm.u32;
   apply_modifiers(&u.u32, type, nvi->src[s]->mod);

   if (u.u32 == 0 && NV_BASEOP(nvi->opcode) == NV_OP_MUL) {
      nvi->opcode = NV_OP_MOV;
      nv_reference(pc, nvi, t, NULL);
      if (s) {
         nvi->src[0] = nvi->src[1];
         nvi->src[1] = NULL;
      }
      return;
   }

   switch (nvi->opcode) {
   case NV_OP_MUL_F32:
      if (u.f32 == 1.0f || u.f32 == -1.0f) {
         if (u.f32 == -1.0f)
            nvi->src[t]->mod ^= NV_MOD_NEG;
         switch (nvi->src[t]->mod) {
         case 0: op = nvi->saturate ? NV_OP_SAT : NV_OP_MOV; break;
         case NV_MOD_NEG: op = NV_OP_NEG_F32; break;
         case NV_MOD_ABS: op = NV_OP_ABS_F32; break;
         default:
            return;
         }
         nvi->opcode = op;
         nv_reference(pc, nvi, 0, nvi->src[t]->value);
         nv_reference(pc, nvi, 1, NULL);
         nvi->src[0]->mod = 0;
      } else
      if (u.f32 == 2.0f || u.f32 == -2.0f) {
         if (u.f32 == -2.0f)
            nvi->src[t]->mod ^= NV_MOD_NEG;
         nvi->opcode = NV_OP_ADD_F32;
         nv_reference(pc, nvi, s, nvi->src[t]->value);
         nvi->src[s]->mod = nvi->src[t]->mod;
      }
   case NV_OP_ADD_F32:
      if (u.u32 == 0) {
         switch (nvi->src[t]->mod) {
         case 0: op = nvi->saturate ? NV_OP_SAT : NV_OP_MOV; break;
         case NV_MOD_NEG: op = NV_OP_NEG_F32; break;
         case NV_MOD_ABS: op = NV_OP_ABS_F32; break;
         case NV_MOD_NEG | NV_MOD_ABS:
            op = NV_OP_CVT;
            nvi->ext.cvt.s = nvi->ext.cvt.d = type;
            break;
         default:
            return;
         }
         nvi->opcode = op;
         nv_reference(pc, nvi, 0, nvi->src[t]->value);
         nv_reference(pc, nvi, 1, NULL);
         if (nvi->opcode != NV_OP_CVT)
            nvi->src[0]->mod = 0;
      }
   case NV_OP_ADD_B32:
      if (u.u32 == 0) {
         assert(nvi->src[t]->mod == 0);
         nvi->opcode = nvi->saturate ? NV_OP_CVT : NV_OP_MOV;
         nvi->ext.cvt.s = nvi->ext.cvt.d = type;
         nv_reference(pc, nvi, 0, nvi->src[t]->value);
         nv_reference(pc, nvi, 1, NULL);
      }
      break;
   case NV_OP_MUL_B32:
      /* multiplication by 0 already handled above */
      assert(nvi->src[s]->mod == 0);
      shift = ffs(u.s32) - 1;
      if (shift == 0) {
         nvi->opcode = NV_OP_MOV;
         nv_reference(pc, nvi, 0, nvi->src[t]->value);
         nv_reference(pc, nvi, 1, NULL);
      } else
      if (u.s32 > 0 && u.s32 == (1 << shift)) {
         nvi->opcode = NV_OP_SHL;
         (val = new_value(pc, NV_FILE_IMM, NV_TYPE_U32))->reg.imm.s32 = shift;
         nv_reference(pc, nvi, 0, nvi->src[t]->value);
         nv_reference(pc, nvi, 1, val);
         break;
      }
      break;
   case NV_OP_RCP:
      u.f32 = 1.0f / u.f32;
      (val = new_value(pc, NV_FILE_IMM, NV_TYPE_F32))->reg.imm.f32 = u.f32;
      nvi->opcode = NV_OP_MOV;
      assert(s == 0);
      nv_reference(pc, nvi, 0, val);
      break;
   case NV_OP_RSQ:
      u.f32 = 1.0f / sqrtf(u.f32);
      (val = new_value(pc, NV_FILE_IMM, NV_TYPE_F32))->reg.imm.f32 = u.f32;
      nvi->opcode = NV_OP_MOV;
      assert(s == 0);
      nv_reference(pc, nvi, 0, val);
      break;
   default:
      break;
   }
}

static int
nv_pass_lower_arith(struct nv_pass *ctx, struct nv_basic_block *b)
{
   struct nv_instruction *nvi, *next;
   int j;

   for (nvi = b->entry; nvi; nvi = next) {
      struct nv_value *src0, *src1, *src;
      int s;
      uint8_t mod[4];

      next = nvi->next;

      src0 = nvc0_pc_find_immediate(nvi->src[0]);
      src1 = nvc0_pc_find_immediate(nvi->src[1]);

      if (src0 && src1)
         constant_expression(ctx->pc, nvi, src0, src1);
      else {
         if (src0)
            constant_operand(ctx->pc, nvi, src0, 0);
         else
         if (src1)
            constant_operand(ctx->pc, nvi, src1, 1);
      }

      /* check if we can MUL + ADD -> MAD/FMA */
      if (nvi->opcode != NV_OP_ADD)
         continue;

      src0 = nvi->src[0]->value;
      src1 = nvi->src[1]->value;

      if (SRC_IS_MUL(src0) && src0->refc == 1)
         src = src0;
      else
      if (SRC_IS_MUL(src1) && src1->refc == 1)
         src = src1;
      else
         continue;

      /* could have an immediate from above constant_*  */
      if (src0->reg.file != NV_FILE_GPR || src1->reg.file != NV_FILE_GPR)
         continue;
      s = (src == src0) ? 0 : 1;

      mod[0] = nvi->src[0]->mod;
      mod[1] = nvi->src[1]->mod;
      mod[2] = src->insn->src[0]->mod;
      mod[3] = src->insn->src[0]->mod;

      if ((mod[0] | mod[1] | mod[2] | mod[3]) & ~NV_MOD_NEG)
         continue;

      nvi->opcode = NV_OP_MAD;
      nv_reference(ctx->pc, nvi, s, NULL);
      nvi->src[2] = nvi->src[!s];

      nvi->src[0] = new_ref(ctx->pc, src->insn->src[0]->value);
      nvi->src[1] = new_ref(ctx->pc, src->insn->src[1]->value);
      nvi->src[0]->mod = mod[2] ^ mod[s];
      nvi->src[1]->mod = mod[3];
   }
   DESCEND_ARBITRARY(j, nv_pass_lower_arith);

   return 0;
}

/* TODO: redundant store elimination */

struct mem_record {
   struct mem_record *next;
   struct nv_instruction *insn;
   uint32_t ofst;
   uint32_t base;
   uint32_t size;
};

#define MEM_RECORD_POOL_SIZE 1024

struct pass_reld_elim {
   struct nv_pc *pc;

   struct mem_record *imm;
   struct mem_record *mem_v;
   struct mem_record *mem_a;
   struct mem_record *mem_c[16];
   struct mem_record *mem_l;

   struct mem_record pool[MEM_RECORD_POOL_SIZE];
   int alloc;
};

/* Extend the load operation in @rec to also cover the data loaded by @ld.
 * The two loads may not overlap but reference adjacent memory locations.
 */
static void
combine_load(struct mem_record *rec, struct nv_instruction *ld)
{
   struct nv_instruction *fv = rec->insn;
   struct nv_value *mem = ld->src[0]->value;
   uint32_t size = rec->size + mem->reg.size;
   int j;
   int d = rec->size / 4;

   assert(rec->size < 16);
   if (rec->ofst > mem->reg.address) {
      if ((size == 8 && mem->reg.address & 3) ||
          (size > 8 && mem->reg.address & 7))
         return;
      rec->ofst = mem->reg.address;
      for (j = 0; j < d; ++j)
         fv->def[mem->reg.size / 4 + j] = fv->def[j];
      d = 0;
   } else
   if ((size == 8 && rec->ofst & 3) ||
       (size > 8 && rec->ofst & 7)) {
      return;
   }

   for (j = 0; j < mem->reg.size / 4; ++j) {
      fv->def[d] = ld->def[j];
      fv->def[d++]->insn = fv;
   }

   fv->src[0]->value->reg.address = rec->ofst;
   fv->src[0]->value->reg.size = rec->size = size;

   nvc0_insn_delete(ld);
}

static void
combine_export(struct mem_record *rec, struct nv_instruction *ex)
{

}

static INLINE void
add_mem_record(struct pass_reld_elim *ctx, struct mem_record **rec,
               uint32_t base, uint32_t ofst, struct nv_instruction *nvi)
{
   struct mem_record *it = &ctx->pool[ctx->alloc++];

   it->next = *rec;
   *rec = it;
   it->base = base;
   it->ofst = ofst;
   it->insn = nvi;
   it->size = nvi->src[0]->value->reg.size;
}

/* vectorize and reuse loads from memory or of immediates */
static int
nv_pass_mem_opt(struct pass_reld_elim *ctx, struct nv_basic_block *b)
{
   struct mem_record **rec, *it;
   struct nv_instruction *ld, *next;
   struct nv_value *mem;
   uint32_t base, ofst;
   int s;

   for (ld = b->entry; ld; ld = next) {
      next = ld->next;

      if (is_cspace_load(ld)) {
         mem = ld->src[0]->value;
         rec = &ctx->mem_c[ld->src[0]->value->reg.file - NV_FILE_MEM_C(0)];
      } else
      if (ld->opcode == NV_OP_VFETCH) {
         mem = ld->src[0]->value;
         rec = &ctx->mem_a;
      } else
      if (ld->opcode == NV_OP_EXPORT) {
         mem = ld->src[0]->value;
         if (mem->reg.file != NV_FILE_MEM_V)
            continue;
         rec = &ctx->mem_v;
      } else {
         continue;
      }
      if (ld->def[0] && ld->def[0]->refc == 0)
         continue;
      ofst = mem->reg.address;
      base = (ld->indirect >= 0) ? ld->src[ld->indirect]->value->n : 0;

      for (it = *rec; it; it = it->next) {
         if (it->base == base &&
             ((it->ofst >> 4) == (ofst >> 4)) &&
             ((it->ofst + it->size == ofst) ||
              (it->ofst - mem->reg.size == ofst))) {
            /* only NV_OP_VFETCH can load exactly 12 bytes */
            if (ld->opcode == NV_OP_LD && it->size + mem->reg.size == 12)
               continue;
            if (it->ofst < ofst) {
               if ((it->ofst & 0xf) == 4)
                  continue;
            } else
            if ((ofst & 0xf) == 4)
               continue;
            break;
         }
      }
      if (it) {
         switch (ld->opcode) {
         case NV_OP_EXPORT: combine_export(it, ld); break;
         default:
            combine_load(it, ld);
            break;
         }
      } else
      if (ctx->alloc < MEM_RECORD_POOL_SIZE) {
         add_mem_record(ctx, rec, base, ofst, ld);
      }
   }

   DESCEND_ARBITRARY(s, nv_pass_mem_opt);
   return 0;
}

static void
eliminate_store(struct mem_record *rec, struct nv_instruction *st)
{
}

/* elimination of redundant stores */
static int
pass_store_elim(struct pass_reld_elim *ctx, struct nv_basic_block *b)
{
   struct mem_record **rec, *it;
   struct nv_instruction *st, *next;
   struct nv_value *mem;
   uint32_t base, ofst, size;
   int s;

   for (st = b->entry; st; st = next) {
      next = st->next;

      if (st->opcode == NV_OP_ST) {
         mem = st->src[0]->value;
         rec = &ctx->mem_l;
      } else
      if (st->opcode == NV_OP_EXPORT) {
         mem = st->src[0]->value;
         if (mem->reg.file != NV_FILE_MEM_V)
            continue;
         rec = &ctx->mem_v;
      } else
      if (st->opcode == NV_OP_ST) {
         /* TODO: purge */
      }
      ofst = mem->reg.address;
      base = (st->indirect >= 0) ? st->src[st->indirect]->value->n : 0;
      size = mem->reg.size;

      for (it = *rec; it; it = it->next) {
         if (it->base == base &&
             (it->ofst <= ofst && (it->ofst + size) > ofst))
            break;
      }
      if (it)
         eliminate_store(it, st);
      else
         add_mem_record(ctx, rec, base, ofst, st);
   }

   DESCEND_ARBITRARY(s, nv_pass_mem_opt);
   return 0;
}

/* TODO: properly handle loads from l[] memory in the presence of stores */
static int
nv_pass_reload_elim(struct pass_reld_elim *ctx, struct nv_basic_block *b)
{
#if 0
   struct load_record **rec, *it;
   struct nv_instruction *ld, *next;
   uint64_t data[2];
   struct nv_value *val;
   int j;

   for (ld = b->entry; ld; ld = next) {
      next = ld->next;
      if (!ld->src[0])
         continue;
      val = ld->src[0]->value;
      rec = NULL;

      if (ld->opcode == NV_OP_LINTERP || ld->opcode == NV_OP_PINTERP) {
         data[0] = val->reg.id;
         data[1] = 0;
         rec = &ctx->mem_v;
      } else
      if (ld->opcode == NV_OP_LDA) {
         data[0] = val->reg.id;
         data[1] = ld->src[4] ? ld->src[4]->value->n : ~0ULL;
         if (val->reg.file >= NV_FILE_MEM_C(0) &&
             val->reg.file <= NV_FILE_MEM_C(15))
            rec = &ctx->mem_c[val->reg.file - NV_FILE_MEM_C(0)];
         else
         if (val->reg.file == NV_FILE_MEM_S)
            rec = &ctx->mem_s;
         else
         if (val->reg.file == NV_FILE_MEM_L)
            rec = &ctx->mem_l;
      } else
      if ((ld->opcode == NV_OP_MOV) && (val->reg.file == NV_FILE_IMM)) {
         data[0] = val->reg.imm.u32;
         data[1] = 0;
         rec = &ctx->imm;
      }

      if (!rec || !ld->def[0]->refc)
         continue;

      for (it = *rec; it; it = it->next)
         if (it->data[0] == data[0] && it->data[1] == data[1])
            break;

      if (it) {
         if (ld->def[0]->reg.id >= 0)
            it->value = ld->def[0];
         else
         if (!ld->fixed)
            nvc0_pc_replace_value(ctx->pc, ld->def[0], it->value);
      } else {
         if (ctx->alloc == LOAD_RECORD_POOL_SIZE)
            continue;
         it = &ctx->pool[ctx->alloc++];
         it->next = *rec;
         it->data[0] = data[0];
         it->data[1] = data[1];
         it->value = ld->def[0];
         *rec = it;
      }
   }

   ctx->imm = NULL;
   ctx->mem_s = NULL;
   ctx->mem_v = NULL;
   for (j = 0; j < 16; ++j)
      ctx->mem_c[j] = NULL;
   ctx->mem_l = NULL;
   ctx->alloc = 0;

   DESCEND_ARBITRARY(j, nv_pass_reload_elim);
#endif
   return 0;
}

static int
nv_pass_tex_mask(struct nv_pass *ctx, struct nv_basic_block *b)
{
   int i, c, j;

   for (i = 0; i < ctx->pc->num_instructions; ++i) {
      struct nv_instruction *nvi = &ctx->pc->instructions[i];
      struct nv_value *def[4];

      if (!nv_is_texture_op(nvi->opcode))
         continue;
      nvi->tex_mask = 0;

      for (c = 0; c < 4; ++c) {
         if (nvi->def[c]->refc)
            nvi->tex_mask |= 1 << c;
         def[c] = nvi->def[c];
      }

      j = 0;
      for (c = 0; c < 4; ++c)
         if (nvi->tex_mask & (1 << c))
            nvi->def[j++] = def[c];
      for (c = 0; c < 4; ++c)
         if (!(nvi->tex_mask & (1 << c)))
           nvi->def[j++] = def[c];
      assert(j == 4);
   }
   return 0;
}

struct nv_pass_dce {
   struct nv_pc *pc;
   uint removed;
};

static int
nv_pass_dce(struct nv_pass_dce *ctx, struct nv_basic_block *b)
{
   int j;
   struct nv_instruction *nvi, *next;

   for (nvi = b->phi ? b->phi : b->entry; nvi; nvi = next) {
      next = nvi->next;

      if (inst_removable(nvi)) {
         nvc0_insn_delete(nvi);
         ++ctx->removed;
      }
   }
   DESCEND_ARBITRARY(j, nv_pass_dce);

   return 0;
}

#if 0
/* Register allocation inserted ELSE blocks for all IF/ENDIF without ELSE.
 * Returns TRUE if @bb initiates an IF/ELSE/ENDIF clause, or is an IF with
 * BREAK and dummy ELSE block.
 */
static INLINE boolean
bb_is_if_else_endif(struct nv_basic_block *bb)
{
   if (!bb->out[0] || !bb->out[1])
      return FALSE;

   if (bb->out[0]->out_kind[0] == CFG_EDGE_LOOP_LEAVE) {
      return (bb->out[0]->out[1] == bb->out[1]->out[0] &&
              !bb->out[1]->out[1]);
   } else {
      return (bb->out[0]->out[0] == bb->out[1]->out[0] &&
              !bb->out[0]->out[1] &&
              !bb->out[1]->out[1]);
   }
}

/* predicate instructions and remove branch at the end */
static void
predicate_instructions(struct nv_pc *pc, struct nv_basic_block *b,
                       struct nv_value *p, ubyte cc)
{

}
#endif

/* NOTE: Run this after register allocation, we can just cut out the cflow
 * instructions and hook the predicates to the conditional OPs if they are
 * not using immediates; better than inserting SELECT to join definitions.
 *
 * NOTE: Should adapt prior optimization to make this possible more often.
 */
static int
nv_pass_flatten(struct nv_pass *ctx, struct nv_basic_block *b)
{
   return 0;
}

/* local common subexpression elimination, stupid O(n^2) implementation */
static int
nv_pass_cse(struct nv_pass *ctx, struct nv_basic_block *b)
{
   struct nv_instruction *ir, *ik, *next;
   struct nv_instruction *entry = b->phi ? b->phi : b->entry;
   int s;
   unsigned int reps;

   do {
      reps = 0;
      for (ir = entry; ir; ir = next) {
         next = ir->next;
         for (ik = entry; ik != ir; ik = ik->next) {
            if (ir->opcode != ik->opcode || ir->fixed)
               continue;

            if (!ir->def[0] || !ik->def[0] || ir->def[1] || ik->def[1])
               continue;

            if (ik->indirect != ir->indirect || ik->predicate != ir->predicate)
               continue;

            if (!values_equal(ik->def[0], ir->def[0]))
               continue;

            for (s = 0; s < 3; ++s) {
               struct nv_value *a, *b;

               if (!ik->src[s]) {
                  if (ir->src[s])
                     break;
                  continue;
               }
               if (ik->src[s]->mod != ir->src[s]->mod)
                  break;
               a = ik->src[s]->value;
               b = ir->src[s]->value;
               if (a == b)
                  continue;
               if (a->reg.file != b->reg.file ||
                   a->reg.id < 0 ||
                   a->reg.id != b->reg.id)
                  break;
            }
            if (s == 3) {
               nvc0_insn_delete(ir);
               ++reps;
               nvc0_pc_replace_value(ctx->pc, ir->def[0], ik->def[0]);
               break;
            }
         }
      }
   } while(reps);

   DESCEND_ARBITRARY(s, nv_pass_cse);

   return 0;
}

/* Make sure all sources of an NV_OP_BIND are distinct, they need to occupy
 * neighbouring registers. CSE might have messed this up.
 */
static int
nv_pass_fix_bind(struct nv_pass *ctx, struct nv_basic_block *b)
{
   struct nv_value *val;
   struct nv_instruction *bnd, *nvi, *next;
   int s, t;

   for (bnd = b->entry; bnd; bnd = next) {
      next = bnd->next;
      if (bnd->opcode != NV_OP_BIND)
         continue;
      for (s = 0; s < 4 && bnd->src[s]; ++s) {
         val = bnd->src[s]->value;
         for (t = s + 1; t < 4 && bnd->src[t]; ++t) {
            if (bnd->src[t]->value != val)
               continue;
            nvi = nv_alloc_instruction(ctx->pc, NV_OP_MOV);
            nvi->def[0] = new_value_like(ctx->pc, val);
            nvi->def[0]->insn = nvi;
            nv_reference(ctx->pc, nvi, 0, val);
            nvc0_insn_insert_before(bnd, nvi);

            nv_reference(ctx->pc, bnd, t, nvi->def[0]);
         }
      }
   }
   DESCEND_ARBITRARY(t, nv_pass_fix_bind);

   return 0;
}

static int
nv_pc_pass0(struct nv_pc *pc, struct nv_basic_block *root)
{
   struct pass_reld_elim *reldelim;
   struct nv_pass pass;
   struct nv_pass_dce dce;
   int ret;

   pass.n = 0;
   pass.pc = pc;

   /* Do this first, so we don't have to pay attention
    * to whether sources are supported memory loads.
    */
   pc->pass_seq++;
   ret = nv_pass_lower_arith(&pass, root);
   if (ret)
      return ret;

   pc->pass_seq++;
   ret = nv_pass_lower_mods(&pass, root);
   if (ret)
      return ret;

   pc->pass_seq++;
   ret = nvc0_pass_fold_loads(&pass, root);
   if (ret)
      return ret;

   if (pc->opt_reload_elim) {
      reldelim = CALLOC_STRUCT(pass_reld_elim);
      reldelim->pc = pc;

      pc->pass_seq++;
      ret = nv_pass_reload_elim(reldelim, root);
      if (ret) {
         FREE(reldelim);
         return ret;
      }
      memset(reldelim, 0, sizeof(struct pass_reld_elim));
      reldelim->pc = pc;
   }

   pc->pass_seq++;
   ret = nv_pass_cse(&pass, root);
   if (ret)
      return ret;

   dce.pc = pc;
   do {
      dce.removed = 0;
      pc->pass_seq++;
      ret = nv_pass_dce(&dce, root);
      if (ret)
         return ret;
   } while (dce.removed);

   if (pc->opt_reload_elim) {
      pc->pass_seq++;
      ret = nv_pass_mem_opt(reldelim, root);
      if (!ret) {
         memset(reldelim, 0, sizeof(struct pass_reld_elim));
         reldelim->pc = pc;

         pc->pass_seq++;
         ret = nv_pass_mem_opt(reldelim, root);
      }
      FREE(reldelim);
      if (ret)
         return ret;
   }

   ret = nv_pass_tex_mask(&pass, root);
   if (ret)
      return ret;

   pc->pass_seq++;
   ret = nv_pass_fix_bind(&pass, root);

   return ret;
}

int
nvc0_pc_exec_pass0(struct nv_pc *pc)
{
   int i, ret;

   for (i = 0; i < pc->num_subroutines + 1; ++i)
      if (pc->root[i] && (ret = nv_pc_pass0(pc, pc->root[i])))
         return ret;
   return 0;
}