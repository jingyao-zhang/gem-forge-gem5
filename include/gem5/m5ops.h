/*
 * Copyright (c) 2003-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __GEM5_M5OP_H__
#define __GEM5_M5OP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <assert.h>

#include <gem5/asm/generic/m5ops.h>

void m5_arm(uint64_t address);
void m5_quiesce(void);
void m5_quiesce_ns(uint64_t ns);
void m5_quiesce_cycle(uint64_t cycles);
uint64_t m5_quiesce_time(void);
uint64_t m5_rpns();
void m5_wake_cpu(uint64_t cpuid);

void m5_exit(uint64_t ns_delay);
void m5_fail(uint64_t ns_delay, uint64_t code);
// m5_sum is for sanity checking the gem5 op interface.
unsigned m5_sum(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e,
                unsigned f);
uint64_t m5_init_param(uint64_t key_str1, uint64_t key_str2);
void m5_checkpoint(uint64_t ns_delay, uint64_t ns_period);
void m5_reset_stats(uint64_t ns_delay, uint64_t ns_period);
void m5_dump_stats(uint64_t ns_delay, uint64_t ns_period);
void m5_dump_reset_stats(uint64_t ns_delay, uint64_t ns_period);
uint64_t m5_read_file(void *buffer, uint64_t len, uint64_t offset);
uint64_t m5_write_file(void *buffer, uint64_t len, uint64_t offset,
                       const char *filename);
void m5_debug_break(void);
void m5_switch_cpu(void);
void m5_dist_toggle_sync(void);
void m5_add_symbol(uint64_t addr, const char *symbol);
void m5_load_symbol();
void m5_panic(void);
void m5_work_begin(uint64_t workid, uint64_t threadid);
void m5_work_end(uint64_t workid, uint64_t threadid);
void m5_work_mark(uint64_t workid, uint64_t threadid);

void m5_llvm_trace_map(const char *base, void *vaddr);
void m5_llvm_trace_replay(const char *trace, void *vaddr);

void m5_stream_nuca_region(const char *regionName, const void *buffer,
                           uint64_t elementSize, uint64_t dim1, uint64_t dim2,
                           uint64_t dim3);
/**
 * Specify the alignment requirement between two arrays.
 *
 * Negative element offset will specify some indirect alignment.
 *
 * This is the general data structure we try to specify. It handles
 * both indirect element and pointers.
 *
 * Node;
 * IndElement {
 *   int32_t out_v;
 *   ... // Other metadata.
 * };
 * PtrElement {
 *   Node *ptr;
 *   ...// Other metadata.
 * };
 *
 * Node {
 *   int32_t indCount;
 *   IndElement inds[indCount];
 *   int32_t ptrCount;
 *   PtrElement ptrs[ptrCount];
 *   ...
 * };
 *
 * Then it defines the following fields (size = 0 means no this field).
 *  indCountOffset, indCountSize
 *  indOffset, indSize, indStride
 *  ptrCountOffset, ptrCountSize
 *  ptrOffset, ptrSize, ptrStride
 *
 * -((1 << 16) | (offset << 8) | size).
 *
 *
 * We also add information about the CSR index array.
 * For now this is used to inform the hardware about the CSR data structure.
 */
enum StreamNUCAAffinityField {
  STREAM_NUCA_AFFINITY_IND_COUNT_OFFSET = 0,
  STREAM_NUCA_AFFINITY_IND_COUNT_SIZE,
  STREAM_NUCA_AFFINITY_IND_OFFSET,
  STREAM_NUCA_AFFINITY_IND_SIZE,
  STREAM_NUCA_AFFINITY_IND_STRIDE,
  STREAM_NUCA_AFFINITY_PTR_COUNT_OFFSET,
  STREAM_NUCA_AFFINITY_PTR_COUNT_SIZE,
  STREAM_NUCA_AFFINITY_PTR_OFFSET,
  STREAM_NUCA_AFFINITY_PTR_SIZE,
  STREAM_NUCA_AFFINITY_PTR_STRIDE,
  STREAM_NUCA_AFFINITY_NUM_FIELDS
};
#define STREAM_NUCA_AFFINITY_FIELD_BITS 6
inline int64_t stream_nuca_affinity_encode(int64_t field, int64_t value) {
  assert(value < (1 << STREAM_NUCA_AFFINITY_FIELD_BITS));
  return value << (field * STREAM_NUCA_AFFINITY_FIELD_BITS);
}
inline int64_t stream_nuca_affinity_decode(int64_t field, int64_t value) {
  return (value >> (field * STREAM_NUCA_AFFINITY_FIELD_BITS)) & 0xFF;
}

// Some simple cases.
inline int64_t m5_stream_nuca_encode_ind_align(int64_t offset, int64_t size) {
  return -(
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_NUM_FIELDS, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_COUNT_OFFSET, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_COUNT_SIZE, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_OFFSET, offset) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_SIZE, size) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_STRIDE, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_COUNT_OFFSET, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_COUNT_SIZE, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_OFFSET, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_SIZE, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_STRIDE, 0));
}

inline int64_t m5_stream_nuca_encode_multi_ind(int64_t cntOffset,
                                               int64_t cntSize, int64_t offset,
                                               int64_t size, int64_t stride) {
  return -(
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_NUM_FIELDS, 0) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_COUNT_OFFSET,
                                  cntOffset) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_COUNT_SIZE,
                                  cntSize) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_OFFSET, offset) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_SIZE, size) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_IND_STRIDE, stride));
}

inline int64_t m5_stream_nuca_encode_ptr_align(int64_t offset, int64_t size) {
  return -(
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_NUM_FIELDS, 1) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_OFFSET, offset) |
      stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_PTR_SIZE, size));
}

inline int64_t m5_stream_nuca_encode_csr_index() {
  return -(stream_nuca_affinity_encode(STREAM_NUCA_AFFINITY_NUM_FIELDS, 2));
}

void m5_stream_nuca_align(const void *A, const void *B, int64_t elementOffset);

/**
 * A generic implementation to set some property of the region.
 * Such a long name to avoid polluting C global scope.
 */
enum StreamNUCARegionProperty {
  // Manually overrite the interleaving (in bytes).
  STREAM_NUCA_REGION_PROPERTY_INTERLEAVE = 0,
  // Manually set if the region is used as PUM.
  STREAM_NUCA_REGION_PROPERTY_USE_PUM,
  // Manually set if region need initilization (from DRAM) when used as PUM.
  STREAM_NUCA_REGION_PROPERTY_PUM_NO_INIT,
  // Manually set PUM tile size.
  STREAM_NUCA_REGION_PROPERTY_PUM_TILE_SIZE_DIM0,
  // Specify which dimension we are going to reduce over.
  STREAM_NUCA_REGION_PROPERTY_REDUCE_DIM,
  // Specify which dimension we are going to broadcast.
  STREAM_NUCA_REGION_PROPERTY_BROADCAST_DIM,
  // Get total number of banks.
  STREAM_NUCA_REGION_PROPERTY_TOTAL_BANKS,
  // Get number of bank rows.
  STREAM_NUCA_REGION_PROPERTY_BANK_ROWS,
  // Get number of bank cols.
  STREAM_NUCA_REGION_PROPERTY_BANK_COLS,
  // Start bank after remapped.
  STREAM_NUCA_REGION_PROPERTY_START_BANK,
  // Start vaddr.
  STREAM_NUCA_REGION_PROPERTY_START_VADDR,
  // End vaddr.
  STREAM_NUCA_REGION_PROPERTY_END_VADDR,
};
void m5_stream_nuca_set_property(const void *buffer,
                                 enum StreamNUCARegionProperty property,
                                 uint64_t value);
uint64_t m5_stream_nuca_get_property(const void *buffer,
                                     enum StreamNUCARegionProperty property);

void m5_stream_nuca_remap();
uint64_t m5_stream_nuca_get_cached_bytes(void *buffer);

/*
 * Send a very generic poke to the workload so it can do something. It's up to
 * the workload to know what information to look for to interpret an event,
 * such as what PC it came from, what register values are, or the context of
 * the workload itself (is this SE mode? which OS is running?).
 */
void m5_workload();

/*
 * Create _addr and _semi versions all declarations, e.g. m5_exit_addr and
 * m5_exit_semi. These expose the the memory and semihosting variants of the
 * ops.
 *
 * Some of those declarations are not defined for certain ISAs, e.g. X86
 * does not have _semi, but we felt that ifdefing them out could cause more
 * trouble tham leaving them in.
 */
#define M5OP(name, func)                                                       \
  __typeof__(name) M5OP_MERGE_TOKENS(name, _addr);                             \
  __typeof__(name) M5OP_MERGE_TOKENS(name, _semi);
M5OP_FOREACH
#undef M5OP

#define m5_detail_sim_start() m5_switch_cpu();

#define m5_detail_sim_end() m5_dump_stats(0, 0);

#define m5_gem_forge_region_simpoint()                                         \
  m5_reset_stats(0, 0);                                                        \
  m5_switch_cpu();

#ifdef __cplusplus
}
#endif
#endif // __GEM5_M5OP_H__
