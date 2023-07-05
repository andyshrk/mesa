/*
 * Copyright (C) 2022 Collabora Ltd.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#pragma once

#if !defined(PAN_ARCH) || PAN_ARCH < 10
#error "cs_builder.h requires PAN_ARCH >= 10"
#endif

#include "gen_macros.h"

/*
 * cs_builder implements a builder for CSF command streams. It manages the
 * allocation and overflow behaviour of queues and provides helpers for emitting
 * commands to run on the CSF pipe.
 *
 * Users are responsible for the CS buffer allocation and must initialize the
 * command stream with an initial buffer using cs_builder_init(). The CS can
 * be extended with new buffers allocated with cs_builder_conf::alloc_buffer()
 * if the builder runs out of memory.
 */

struct cs_buffer {
   /* CPU pointer */
   uint64_t *cpu;

   /* GPU pointer */
   uint64_t gpu;

   /* Capacity in number of 64-bit instructions */
   uint32_t capacity;
};

struct cs_builder_conf {
   /* Number of 32-bit registers in the hardware register file */
   uint8_t nr_registers;

   /* Number of 32-bit registers used by the kernel at submission time */
   uint8_t nr_kernel_registers;

   /* CS buffer allocator */
   struct cs_buffer (*alloc_buffer)(void *cookie);

   /* Cookie passed back to alloc_buffer() */
   void *cookie;
};

/* The CS is formed of one or more CS chunks linked with JUMP instructions.
 * The builder keeps track of the current chunk and the position inside this
 * chunk, so it can emit new instructions, and decide when a new chunk needs
 * to be allocated.
 */
struct cs_chunk {
   /* CS buffer object backing this chunk */
   struct cs_buffer buffer;

   union {
      /* Current position in the buffer object when the chunk is active. */
      uint32_t pos;

      /* Chunk size when the chunk was wrapped. */
      uint32_t size;
   };
};

typedef struct cs_builder {
   /* CS builder configuration */
   struct cs_builder_conf conf;

   /* Initial (root) CS chunk. */
   struct cs_chunk root_chunk;

   /* Current CS chunk. */
   struct cs_chunk cur_chunk;

   /* Move immediate instruction at the end of the last CS chunk that needs to
    * be patched with the final length of the current CS chunk in order to
    * facilitate correct overflow behaviour.
    */
   uint32_t *length_patch;
} cs_builder;

static void
cs_builder_init(struct cs_builder *b, const struct cs_builder_conf *conf,
                struct cs_buffer root_buffer)
{
   *b = (struct cs_builder){
      .conf = *conf,
      .root_chunk.buffer = root_buffer,
      .cur_chunk.buffer = root_buffer,
   };
}

/*
 * Wrap the current queue. External users shouldn't call this function
 * directly, they should call cs_finish() when they are done building
 * the command stream, which will in turn call cs_wrap_queue().
 *
 * Internally, this is also used to finalize internal CS chunks when
 * allocating new sub-chunks. See cs_alloc_chunk() for details.
 *
 * This notably requires patching the previous chunk with the length
 * we ended up emitting for this chunk.
 */
static void
cs_wrap_chunk(cs_builder *b)
{
   if (b->length_patch) {
      *b->length_patch = (b->cur_chunk.pos * 8);
      b->length_patch = NULL;
   }

   if (b->root_chunk.buffer.gpu == b->cur_chunk.buffer.gpu)
      b->root_chunk.size = b->cur_chunk.size;
}

/* Call this when you are done building a command stream and want to prepare
 * it for submission.
 */
static unsigned
cs_finish(cs_builder *b)
{
   cs_wrap_chunk(b);
   return b->root_chunk.size;
}

enum cs_index_type {
   CS_INDEX_REGISTER = 0,
   CS_INDEX_IMMEDIATE = 1,
};

typedef struct cs_index {
   enum cs_index_type type;

   /* Number of 32-bit words in the index, must be nonzero */
   uint8_t size;

   union {
      uint64_t imm;
      uint8_t reg;
   };
} cs_index;

static uint8_t
cs_to_reg_tuple(cs_index idx, ASSERTED unsigned expected_size)
{
   assert(idx.type == CS_INDEX_REGISTER);
   assert(idx.size == expected_size);

   return idx.reg;
}

static uint8_t
cs_to_reg32(cs_index idx)
{
   return cs_to_reg_tuple(idx, 1);
}

static uint8_t
cs_to_reg64(cs_index idx)
{
   return cs_to_reg_tuple(idx, 2);
}

static cs_index
cs_reg_tuple(ASSERTED cs_builder *b, unsigned reg, unsigned size)
{
   assert(reg + size <= b->conf.nr_registers && "overflowed register file");
   assert(size < 16 && "unsupported");

   return (
      struct cs_index){.type = CS_INDEX_REGISTER, .size = size, .reg = reg};
}

static inline cs_index
cs_reg32(cs_builder *b, unsigned reg)
{
   return cs_reg_tuple(b, reg, 1);
}

static inline cs_index
cs_reg64(cs_builder *b, unsigned reg)
{
   assert((reg % 2) == 0 && "unaligned 64-bit reg");
   return cs_reg_tuple(b, reg, 2);
}

/*
 * The top of the register file is reserved for cs_builder internal use. We
 * need 3 spare registers for handling command queue overflow. These are
 * available here.
 */
static inline cs_index
cs_overflow_address(cs_builder *b)
{
   return cs_reg64(b, b->conf.nr_registers - 2);
}

static inline cs_index
cs_overflow_length(cs_builder *b)
{
   return cs_reg32(b, b->conf.nr_registers - 3);
}

static cs_index
cs_extract32(cs_builder *b, cs_index idx, unsigned word)
{
   assert(idx.type == CS_INDEX_REGISTER && "unsupported");
   assert(word < idx.size && "overrun");

   return cs_reg32(b, idx.reg + word);
}

#define JUMP_SEQ_INSTR_COUNT 4

static inline void *
cs_alloc_ins(cs_builder *b)
{
   /* If the current chunk runs out of space, allocate a new one and jump to it.
    * We actually do this a few instructions before running out, because the
    * sequence to jump to a new queue takes multiple instructions.
    */
   if (unlikely((b->cur_chunk.size + JUMP_SEQ_INSTR_COUNT) >
                b->cur_chunk.buffer.capacity)) {
      /* Now, allocate a new chunk */
      struct cs_buffer newbuf = b->conf.alloc_buffer(b->conf.cookie);

      uint64_t *ptr = b->cur_chunk.buffer.cpu + (b->cur_chunk.pos++);

      pan_pack(ptr, CEU_MOVE, I) {
         I.destination = cs_to_reg64(cs_overflow_address(b));
         I.immediate = newbuf.gpu;
      }

      ptr = b->cur_chunk.buffer.cpu + (b->cur_chunk.pos++);

      pan_pack(ptr, CEU_MOVE32, I) {
         I.destination = cs_to_reg32(cs_overflow_length(b));
      }

      /* The length will be patched in later */
      uint32_t *length_patch = (uint32_t *)ptr;

      ptr = b->cur_chunk.buffer.cpu + (b->cur_chunk.pos++);

      pan_pack(ptr, CEU_JUMP, I) {
         I.length = cs_to_reg32(cs_overflow_length(b));
         I.address = cs_to_reg64(cs_overflow_address(b));
      }

      /* Now that we've emitted everything, finish up the previous queue */
      cs_wrap_chunk(b);

      /* And make this one current */
      b->length_patch = length_patch;
      b->cur_chunk.buffer = newbuf;
      b->cur_chunk.pos = 0;
   }

   assert(b->cur_chunk.size < b->cur_chunk.buffer.capacity);
   return b->cur_chunk.buffer.cpu + (b->cur_chunk.pos++);
}

/*
 * Helper to emit a new instruction into the command queue. The allocation needs
 * to be separated out being pan_pack can evaluate its argument multiple times,
 * yet cs_alloc has side effects.
 */
#define cs_emit(b, T, cfg)                                                     \
   void *_dest = cs_alloc_ins(b);                                              \
   pan_pack(_dest, CEU_##T, cfg)

static inline void
cs_move32_to(cs_builder *b, cs_index dest, unsigned imm)
{
   cs_emit(b, MOVE32, I)
   {
      I.destination = cs_to_reg32(dest);
      I.immediate = imm;
   }
}

static inline void
cs_move48_to(cs_builder *b, cs_index dest, uint64_t imm)
{
   cs_emit(b, MOVE, I)
   {
      I.destination = cs_to_reg64(dest);
      I.immediate = imm;
   }
}

static inline void
cs_wait_slots(cs_builder *b, unsigned slots)
{
   cs_emit(b, WAIT, I)
   {
      I.slots = slots;
   }
}

static inline void
cs_branch(cs_builder *b, int offset, enum mali_ceu_condition cond, cs_index val)
{
   cs_emit(b, BRANCH, I)
   {
      I.offset = offset;
      I.condition = cond;
      I.value = cs_to_reg32(val);
   }
}

static inline void
cs_run_compute(cs_builder *b, unsigned task_increment,
               enum mali_task_axis task_axis)
{
   cs_emit(b, RUN_COMPUTE, I)
   {
      I.task_increment = task_increment;
      I.task_axis = task_axis;

      /* We always use the first table for compute jobs */
   }
}

static inline void
cs_run_compute_indirect(cs_builder *b, unsigned wg_per_task)
{
   cs_emit(b, RUN_COMPUTE_INDIRECT, I)
   {
      I.workgroups_per_task = wg_per_task;

      /* We always use the first table for compute jobs */
   }
}

static inline void
cs_run_idvs(cs_builder *b, enum mali_draw_mode draw_mode,
            enum mali_index_type index_type, bool secondary_shader)
{
   cs_emit(b, RUN_IDVS, I)
   {
      /* We do not have a use case for traditional IDVS */
      I.malloc_enable = true;

      /* We hardcode these settings for now, we can revisit this if we
       * rework how we emit state later.
       */
      I.fragment_srt_select = true;

      /* Pack the override we use */
      pan_pack(&I.flags_override, PRIMITIVE_FLAGS, cfg) {
         cfg.draw_mode = draw_mode;
         cfg.index_type = index_type;
         cfg.secondary_shader = secondary_shader;
      }
   }
}

static inline void
cs_run_fragment(cs_builder *b, bool enable_tem)
{
   cs_emit(b, RUN_FRAGMENT, I)
   {
      I.enable_tem = enable_tem;
   }
}

static inline void
cs_finish_tiling(cs_builder *b)
{
   cs_emit(b, FINISH_TILING, _);
}

static inline void
cs_finish_fragment(cs_builder *b, bool increment_frag_completed,
                   cs_index first_free_heap_chunk,
                   cs_index last_free_heap_chunk, unsigned scoreboard_mask,
                   unsigned signal_slot)
{
   cs_emit(b, FINISH_FRAGMENT, I)
   {
      I.increment_fragment_completed = increment_frag_completed;
      I.wait_mask = scoreboard_mask;
      I.first_heap_chunk = cs_to_reg64(first_free_heap_chunk);
      I.last_heap_chunk = cs_to_reg64(last_free_heap_chunk);
      I.scoreboard_entry = signal_slot;
   }
}

static inline void
cs_heap_set(cs_builder *b, cs_index address)
{
   cs_emit(b, HEAP_SET, I)
   {
      I.address = cs_to_reg64(address);
   }
}

static inline void
cs_load_to(cs_builder *b, cs_index dest, cs_index address, unsigned mask,
           int offset)
{
   cs_emit(b, LOAD_MULTIPLE, I)
   {
      I.base = cs_to_reg_tuple(dest, util_bitcount(mask));
      I.address = cs_to_reg64(address);
      I.mask = mask;
      I.offset = offset;
   }
}

static inline void
cs_store(cs_builder *b, cs_index data, cs_index address, unsigned mask,
         int offset)
{
   cs_emit(b, STORE_MULTIPLE, I)
   {
      I.base = cs_to_reg_tuple(data, util_bitcount(mask));
      I.address = cs_to_reg64(address);
      I.mask = mask;
      I.offset = offset;
   }
}

/*
 * Select which scoreboard entry will track endpoint tasks and other tasks
 * respectively. Pass to cs_wait to wait later.
 */
static inline void
cs_set_scoreboard_entry(cs_builder *b, unsigned ep, unsigned other)
{
   assert(ep < 8 && "invalid slot");
   assert(other < 8 && "invalid slot");

   cs_emit(b, SET_SB_ENTRY, I)
   {
      I.endpoint_entry = ep;
      I.other_entry = other;
   }
}

static inline void
cs_require_all(cs_builder *b)
{
   cs_emit(b, REQ_RESOURCE, I)
   {
      I.compute = true;
      I.tiler = true;
      I.idvs = true;
      I.fragment = true;
   }
}

static inline void
cs_require_compute(cs_builder *b)
{
   cs_emit(b, REQ_RESOURCE, I) I.compute = true;
}

static inline void
cs_require_fragment(cs_builder *b)
{
   cs_emit(b, REQ_RESOURCE, I) I.fragment = true;
}

static inline void
cs_require_idvs(cs_builder *b)
{
   cs_emit(b, REQ_RESOURCE, I)
   {
      I.compute = true;
      I.tiler = true;
      I.idvs = true;
   }
}

static inline void
cs_heap_operation(cs_builder *b, enum mali_ceu_heap_operation operation)
{
   cs_emit(b, HEAP_OPERATION, I) I.operation = operation;
}

static inline void
cs_vt_start(cs_builder *b)
{
   cs_heap_operation(b, MALI_CEU_HEAP_OPERATION_VERTEX_TILER_STARTED);
}

static inline void
cs_vt_end(cs_builder *b)
{
   cs_heap_operation(b, MALI_CEU_HEAP_OPERATION_VERTEX_TILER_COMPLETED);
}

static inline void
cs_frag_end(cs_builder *b)
{
   cs_heap_operation(b, MALI_CEU_HEAP_OPERATION_FRAGMENT_COMPLETED);
}

static inline void
cs_flush_caches(cs_builder *b, enum mali_ceu_flush_mode l2,
                enum mali_ceu_flush_mode lsc, bool other_inv, cs_index flush_id,
                unsigned scoreboard_mask, unsigned signal_slot)
{
   cs_emit(b, FLUSH_CACHE2, I)
   {
      I.l2_flush_mode = l2;
      I.lsc_flush_mode = lsc;
      I.other_invalidate = other_inv;
      I.scoreboard_mask = scoreboard_mask;
      I.latest_flush_id = cs_to_reg32(flush_id);
      I.scoreboard_entry = signal_slot;
   }
}

/* Pseudoinstructions follow */

static inline void
cs_move64_to(cs_builder *b, cs_index dest, uint64_t imm)
{
   if (imm < (1ull << 48)) {
      /* Zero extends */
      cs_move48_to(b, dest, imm);
   } else {
      cs_move32_to(b, cs_extract32(b, dest, 0), imm);
      cs_move32_to(b, cs_extract32(b, dest, 1), imm >> 32);
   }
}

static inline void
cs_load32_to(cs_builder *b, cs_index dest, cs_index address, int offset)
{
   cs_load_to(b, dest, address, BITFIELD_MASK(1), offset);
}

static inline void
cs_load64_to(cs_builder *b, cs_index dest, cs_index address, int offset)
{
   cs_load_to(b, dest, address, BITFIELD_MASK(2), offset);
}

static inline void
cs_store32(cs_builder *b, cs_index data, cs_index address, int offset)
{
   cs_store(b, data, address, BITFIELD_MASK(1), offset);
}

static inline void
cs_store64(cs_builder *b, cs_index data, cs_index address, int offset)
{
   cs_store(b, data, address, BITFIELD_MASK(2), offset);
}

static inline void
cs_wait_slot(cs_builder *b, unsigned slot)
{
   assert(slot < 8 && "invalid slot");

   cs_wait_slots(b, BITFIELD_BIT(slot));
}

static inline void
cs_store_state(cs_builder *b, unsigned signal_slot, cs_index address,
               enum mali_ceu_state state, unsigned wait_mask, int offset)
{
   cs_emit(b, STORE_STATE, I)
   {
      I.offset = offset;
      I.wait_mask = wait_mask;
      I.state = state;
      I.address = cs_to_reg64(address);
      I.scoreboard_slot = signal_slot;
   }
}

static inline void
cs_add64(cs_builder *b, cs_index dest, cs_index src, unsigned imm)
{
   cs_emit(b, ADD_IMMEDIATE64, I)
   {
      I.destination = cs_to_reg64(dest);
      I.source = cs_to_reg64(src);
      I.immediate = imm;
   }
}

static inline void
cs_add32(cs_builder *b, cs_index dest, cs_index src, unsigned imm)
{
   cs_emit(b, ADD_IMMEDIATE32, I)
   {
      I.destination = cs_to_reg32(dest);
      I.source = cs_to_reg32(src);
      I.immediate = imm;
   }
}
