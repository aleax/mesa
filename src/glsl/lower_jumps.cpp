/*
 * Copyright © 2010 Luca Barbieri
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file lower_jumps.cpp
 */

#include "glsl_types.h"
#include <string.h>
#include "ir.h"

enum jump_strength
{
   strength_none,
   strength_always_clears_execute_flag,
   strength_continue,
   strength_break,
   strength_return,
   strength_discard
};

struct block_record
{
   /* minimum jump strength (of lowered IR, not pre-lowering IR)
    *
    * If the block ends with a jump, must be the strength of the jump.
    * Otherwise, the jump would be dead and have been deleted before)
    *
    * If the block doesn't end with a jump, it can be different than strength_none if all paths before it lead to some jump
    * (e.g. an if with a return in one branch, and a break in the other, while not lowering them)
    * Note that identical jumps are usually unified though.
    */
   jump_strength min_strength;

   /* can anything clear the execute flag? */
   bool may_clear_execute_flag;

   block_record()
   {
      this->min_strength = strength_none;
      this->may_clear_execute_flag = false;
   }
};

struct loop_record
{
   ir_function_signature* signature;
   ir_loop* loop;

   /* used to avoid lowering the break used to represent lowered breaks */
   unsigned nesting_depth;
   bool in_if_at_the_end_of_the_loop;

   bool may_set_return_flag;

   ir_variable* break_flag;
   ir_variable* execute_flag; /* cleared to emulate continue */

   loop_record(ir_function_signature* p_signature = 0, ir_loop* p_loop = 0)
   {
      this->signature = p_signature;
      this->loop = p_loop;
      this->nesting_depth = 0;
      this->in_if_at_the_end_of_the_loop = false;
      this->may_set_return_flag = false;
      this->break_flag = 0;
      this->execute_flag = 0;
   }

   ir_variable* get_execute_flag()
   {
      /* also supported for the "function loop" */
      if(!this->execute_flag) {
         exec_list& list = this->loop ? this->loop->body_instructions : signature->body;
         this->execute_flag = new(this->signature) ir_variable(glsl_type::bool_type, "execute_flag", ir_var_temporary);
         list.push_head(new(this->signature) ir_assignment(new(this->signature) ir_dereference_variable(execute_flag), new(this->signature) ir_constant(true), 0));
         list.push_head(this->execute_flag);
      }
      return this->execute_flag;
   }

   ir_variable* get_break_flag()
   {
      assert(this->loop);
      if(!this->break_flag) {
         this->break_flag = new(this->signature) ir_variable(glsl_type::bool_type, "break_flag", ir_var_temporary);
         this->loop->insert_before(this->break_flag);
         this->loop->insert_before(new(this->signature) ir_assignment(new(this->signature) ir_dereference_variable(break_flag), new(this->signature) ir_constant(false), 0));
      }
      return this->break_flag;
   }
};

struct function_record
{
   ir_function_signature* signature;
   ir_variable* return_flag; /* used to break out of all loops and then jump to the return instruction */
   ir_variable* return_value;
   bool is_main;
   unsigned nesting_depth;

   function_record(ir_function_signature* p_signature = 0)
   {
      this->signature = p_signature;
      this->return_flag = 0;
      this->return_value = 0;
      this->nesting_depth = 0;
      this->is_main = this->signature && (strcmp(this->signature->function_name(), "main") == 0);
   }

   ir_variable* get_return_flag()
   {
      if(!this->return_flag) {
         this->return_flag = new(this->signature) ir_variable(glsl_type::bool_type, "return_flag", ir_var_temporary);
         this->signature->body.push_head(new(this->signature) ir_assignment(new(this->signature) ir_dereference_variable(return_flag), new(this->signature) ir_constant(false), 0));
         this->signature->body.push_head(this->return_flag);
      }
      return this->return_flag;
   }

   ir_variable* get_return_value()
   {
      if(!this->return_value) {
         assert(!this->signature->return_type->is_void());
         return_value = new(this->signature) ir_variable(this->signature->return_type, "return_value", ir_var_temporary);
         this->signature->body.push_head(this->return_value);
      }
      return this->return_value;
   }
};

struct ir_lower_jumps_visitor : public ir_control_flow_visitor {
   bool progress;

   struct function_record function;
   struct loop_record loop;
   struct block_record block;

   bool pull_out_jumps;
   bool lower_continue;
   bool lower_break;
   bool lower_sub_return;
   bool lower_main_return;

   ir_lower_jumps_visitor()
   {
      this->progress = false;
   }

   void truncate_after_instruction(exec_node *ir)
   {
      if (!ir)
         return;

      while (!ir->get_next()->is_tail_sentinel()) {
         ((ir_instruction *)ir->get_next())->remove();
         this->progress = true;
      }
   }

   void move_outer_block_inside(ir_instruction *ir, exec_list *inner_block)
   {
      while (!ir->get_next()->is_tail_sentinel()) {
         ir_instruction *move_ir = (ir_instruction *)ir->get_next();

         move_ir->remove();
         inner_block->push_tail(move_ir);
      }
   }

   virtual void visit(class ir_loop_jump * ir)
   {
      truncate_after_instruction(ir);
      this->block.min_strength = ir->is_break() ? strength_break : strength_continue;
   }

   virtual void visit(class ir_return * ir)
   {
      truncate_after_instruction(ir);
      this->block.min_strength = strength_return;
   }

   virtual void visit(class ir_discard * ir)
   {
      truncate_after_instruction(ir);
      this->block.min_strength = strength_discard;
   }

   enum jump_strength get_jump_strength(ir_instruction* ir)
   {
      if(!ir)
         return strength_none;
      else if(ir->ir_type == ir_type_loop_jump) {
         if(((ir_loop_jump*)ir)->is_break())
            return strength_break;
         else
            return strength_continue;
      } else if(ir->ir_type == ir_type_return)
         return strength_return;
      else if(ir->ir_type == ir_type_discard)
         return strength_discard;
      else
         return strength_none;
   }

   bool should_lower_jump(ir_jump* ir)
   {
      unsigned strength = get_jump_strength(ir);
      bool lower;
      switch(strength)
      {
      case strength_none:
         lower = false; /* don't change this, code relies on it */
         break;
      case strength_continue:
         lower = lower_continue;
         break;
      case strength_break:
         assert(this->loop.loop);
         /* never lower "canonical break" */
         if(ir->get_next()->is_tail_sentinel() && (this->loop.nesting_depth == 0
               || (this->loop.nesting_depth == 1 && this->loop.in_if_at_the_end_of_the_loop)))
            lower = false;
         else
            lower = lower_break;
         break;
      case strength_return:
         /* never lower return at the end of a this->function */
         if(this->function.nesting_depth == 0 && ir->get_next()->is_tail_sentinel())
            lower = false;
         else if (this->function.is_main)
            lower = lower_main_return;
         else
            lower = lower_sub_return;
         break;
      case strength_discard:
         lower = false; /* probably nothing needs this lowered */
         break;
      }
      return lower;
   }

   block_record visit_block(exec_list* list)
   {
      block_record saved_block = this->block;
      this->block = block_record();
      visit_exec_list(list, this);
      block_record ret = this->block;
      this->block = saved_block;
      return ret;
   }

   virtual void visit(ir_if *ir)
   {
      if(this->loop.nesting_depth == 0 && ir->get_next()->is_tail_sentinel())
         this->loop.in_if_at_the_end_of_the_loop = true;

      ++this->function.nesting_depth;
      ++this->loop.nesting_depth;

      block_record block_records[2];
      ir_jump* jumps[2];

      block_records[0] = visit_block(&ir->then_instructions);
      block_records[1] = visit_block(&ir->else_instructions);

retry: /* we get here if we put code after the if inside a branch */
   for(unsigned i = 0; i < 2; ++i) {
      exec_list& list = i ? ir->else_instructions : ir->then_instructions;
      jumps[i] = 0;
      if(!list.is_empty() && get_jump_strength((ir_instruction*)list.get_tail()))
         jumps[i] = (ir_jump*)list.get_tail();
   }

      for(;;) {
         jump_strength jump_strengths[2];

         for(unsigned i = 0; i < 2; ++i) {
            if(jumps[i]) {
               jump_strengths[i] = block_records[i].min_strength;
               assert(jump_strengths[i] == get_jump_strength(jumps[i]));
            } else
               jump_strengths[i] = strength_none;
         }

         /* move both jumps out if possible */
         if(pull_out_jumps && jump_strengths[0] == jump_strengths[1]) {
            bool unify = true;
            if(jump_strengths[0] == strength_continue)
               ir->insert_after(new(ir) ir_loop_jump(ir_loop_jump::jump_continue));
            else if(jump_strengths[0] == strength_break)
               ir->insert_after(new(ir) ir_loop_jump(ir_loop_jump::jump_break));
            /* FINISHME: unify returns with identical expressions */
            else if(jump_strengths[0] == strength_return && this->function.signature->return_type->is_void())
               ir->insert_after(new(ir) ir_return(NULL));
            /* FINISHME: unify discards */
            else
               unify = false;

            if(unify) {
               jumps[0]->remove();
               jumps[1]->remove();
               this->progress = true;

               jumps[0] = 0;
               jumps[1] = 0;
               block_records[0].min_strength = strength_none;
               block_records[1].min_strength = strength_none;
               break;
            }
         }

         /* lower a jump: if both need to lowered, start with the strongest one, so that
          * we might later unify the lowered version with the other one
          */
         bool should_lower[2];
         for(unsigned i = 0; i < 2; ++i)
            should_lower[i] = should_lower_jump(jumps[i]);

         int lower;
         if(should_lower[1] && should_lower[0])
            lower = jump_strengths[1] > jump_strengths[0];
         else if(should_lower[0])
            lower = 0;
         else if(should_lower[1])
            lower = 1;
         else
            break;

         if(jump_strengths[lower] == strength_return) {
            ir_variable* return_flag = this->function.get_return_flag();
            if(!this->function.signature->return_type->is_void()) {
               ir_variable* return_value = this->function.get_return_value();
               jumps[lower]->insert_before(new(ir) ir_assignment(new (ir) ir_dereference_variable(return_value), ((ir_return*)jumps[lower])->value, NULL));
            }
            jumps[lower]->insert_before(new(ir) ir_assignment(new (ir) ir_dereference_variable(return_flag), new (ir) ir_constant(true), NULL));
            this->loop.may_set_return_flag = true;
            if(this->loop.loop) {
               ir_loop_jump* lowered = 0;
               lowered = new(ir) ir_loop_jump(ir_loop_jump::jump_break);
               block_records[lower].min_strength = strength_break;
               jumps[lower]->replace_with(lowered);
               jumps[lower] = lowered;
            } else
               goto lower_continue;
            this->progress = true;
         } else if(jump_strengths[lower] == strength_break) {
            /* We can't lower to an actual continue because that would execute the increment.
             *
             * In the lowered code, we instead put the break check between the this->loop body and the increment,
             * which is impossible with a real continue as defined by the GLSL IR currently.
             *
             * Smarter options (such as undoing the increment) are possible but it's not worth implementing them,
             * because if break is lowered, continue is almost surely lowered too.
             */
            jumps[lower]->insert_before(new(ir) ir_assignment(new (ir) ir_dereference_variable(this->loop.get_break_flag()), new (ir) ir_constant(true), 0));
            goto lower_continue;
         } else if(jump_strengths[lower] == strength_continue) {
lower_continue:
            ir_variable* execute_flag = this->loop.get_execute_flag();
            jumps[lower]->replace_with(new(ir) ir_assignment(new (ir) ir_dereference_variable(execute_flag), new (ir) ir_constant(false), 0));
            jumps[lower] = 0;
            block_records[lower].min_strength = strength_always_clears_execute_flag;
            block_records[lower].may_clear_execute_flag = true;
            this->progress = true;
            break;
         }
      }

      /* move out a jump out if possible */
      if(pull_out_jumps) {
         int move_out = -1;
         if(jumps[0] && block_records[1].min_strength >= strength_continue)
            move_out = 0;
         else if(jumps[1] && block_records[0].min_strength >= strength_continue)
            move_out = 1;

         if(move_out >= 0)
         {
            jumps[move_out]->remove();
            ir->insert_after(jumps[move_out]);
            jumps[move_out] = 0;
            block_records[move_out].min_strength = strength_none;
            this->progress = true;
         }
      }

      if(block_records[0].min_strength < block_records[1].min_strength)
         this->block.min_strength = block_records[0].min_strength;
      else
         this->block.min_strength = block_records[1].min_strength;
      this->block.may_clear_execute_flag = this->block.may_clear_execute_flag || block_records[0].may_clear_execute_flag || block_records[1].may_clear_execute_flag;

      if(this->block.min_strength)
         truncate_after_instruction(ir);
      else if(this->block.may_clear_execute_flag)
      {
         int move_into = -1;
         if(block_records[0].min_strength && !block_records[1].may_clear_execute_flag)
            move_into = 1;
         else if(block_records[1].min_strength && !block_records[0].may_clear_execute_flag)
            move_into = 0;

         if(move_into >= 0) {
            assert(!block_records[move_into].min_strength && !block_records[move_into].may_clear_execute_flag); /* otherwise, we just truncated */

            exec_list* list = move_into ? &ir->else_instructions : &ir->then_instructions;
            exec_node* next = ir->get_next();
            if(!next->is_tail_sentinel()) {
               move_outer_block_inside(ir, list);

               exec_list list;
               list.head = next;
               block_records[move_into] = visit_block(&list);

               this->progress = true;
               goto retry;
            }
         } else {
            ir_instruction* ir_after;
            for(ir_after = (ir_instruction*)ir->get_next(); !ir_after->is_tail_sentinel();)
            {
               ir_if* ir_if = ir_after->as_if();
               if(ir_if && ir_if->else_instructions.is_empty()) {
                  ir_dereference_variable* ir_if_cond_deref = ir_if->condition->as_dereference_variable();
                  if(ir_if_cond_deref && ir_if_cond_deref->var == this->loop.execute_flag) {
                     ir_instruction* ir_next = (ir_instruction*)ir_after->get_next();
                     ir_after->insert_before(&ir_if->then_instructions);
                     ir_after->remove();
                     ir_after = ir_next;
                     continue;
                  }
               }
               ir_after = (ir_instruction*)ir_after->get_next();

               /* only set this if we find any unprotected instruction */
               this->progress = true;
            }

            if(!ir->get_next()->is_tail_sentinel()) {
               assert(this->loop.execute_flag);
               ir_if* if_execute = new(ir) ir_if(new(ir) ir_dereference_variable(this->loop.execute_flag));
               move_outer_block_inside(ir, &if_execute->then_instructions);
               ir->insert_after(if_execute);
            }
         }
      }
      --this->loop.nesting_depth;
      --this->function.nesting_depth;
   }

   virtual void visit(ir_loop *ir)
   {
      ++this->function.nesting_depth;
      loop_record saved_loop = this->loop;
      this->loop = loop_record(this->function.signature, ir);

      block_record body = visit_block(&ir->body_instructions);

      if(body.min_strength >= strength_break) {
         /* FINISHME: turn the this->loop into an if, or replace it with its body */
      }

      if(this->loop.break_flag) {
         ir_if* break_if = new(ir) ir_if(new(ir) ir_dereference_variable(this->loop.break_flag));
         break_if->then_instructions.push_tail(new(ir) ir_loop_jump(ir_loop_jump::jump_break));
         ir->body_instructions.push_tail(break_if);
      }

      if(this->loop.may_set_return_flag) {
         assert(this->function.return_flag);
         ir_if* return_if = new(ir) ir_if(new(ir) ir_dereference_variable(this->function.return_flag));
         return_if->then_instructions.push_tail(new(ir) ir_loop_jump(saved_loop.loop ? ir_loop_jump::jump_break : ir_loop_jump::jump_continue));
         ir->insert_after(return_if);
      }

      this->loop = saved_loop;
      --this->function.nesting_depth;
   }

   virtual void visit(ir_function_signature *ir)
   {
      /* these are not strictly necessary */
      assert(!this->function.signature);
      assert(!this->loop.loop);

      function_record saved_function = this->function;
      loop_record saved_loop = this->loop;
      this->function = function_record(ir);
      this->loop = loop_record(ir);

      assert(!this->loop.loop);
      visit_block(&ir->body);

      if(this->function.return_value)
         ir->body.push_tail(new(ir) ir_return(new (ir) ir_dereference_variable(this->function.return_value)));

      this->loop = saved_loop;
      this->function = saved_function;
   }

   virtual void visit(class ir_function * ir)
   {
      visit_block(&ir->signatures);
   }
};

bool
do_lower_jumps(exec_list *instructions, bool pull_out_jumps, bool lower_sub_return, bool lower_main_return, bool lower_continue, bool lower_break)
{
   ir_lower_jumps_visitor v;
   v.pull_out_jumps = pull_out_jumps;
   v.lower_continue = lower_continue;
   v.lower_break = lower_break;
   v.lower_sub_return = lower_sub_return;
   v.lower_main_return = lower_main_return;

   do {
      v.progress = false;
      visit_exec_list(instructions, &v);
   } while (v.progress);

   return v.progress;
}