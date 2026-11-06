/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_PROFILER_H_
#define XENIA_CPU_PPC_PPC_PROFILER_H_

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace ppc {

// Instruction profile of guest code (cvars::ppc_profile).
//
// With the option, PPCHIRBuilder::Emit calls this builtin before every guest
// instruction, the instruction's address in PPCContext::scratch and the guest
// registers in the context. It decodes the instruction and records, per
// thread and per address:
// - how often it ran, and how often the instruction run before it branched
//   here (taken branch), with the taken edges (from, to);
// - a source register written by the instruction just before (dependency),
//   and whether that was an integer load (load-use);
// - floating-point stalls, taking about 10 cycles of latency;
// - load-hit-store cases: a load of bytes one of the last 24 stores wrote,
//   of the other bytes of the same doubleword, or of an address 4 KB away
//   from one (aliasing);
// - misses in a model of the caches: L1 instruction 32 KB 2-way, L1 data
//   32 KB 4-way (write-through without allocation on stores), and one core's
//   share (512 KB 8-way) of the shared L2, 128-byte lines; dcbt / dcbtst are
//   prefetches into L2 and L1 data.
// Cache sizes, associativity and line size are from the public description
// of the Xbox 360 CPU (Andrews and Baker, "Xbox 360 System Architecture",
// IEEE Micro, 2006). The model has no timing: it counts misses, not cycles,
// and the FP latency is an approximation.
//
// Recording starts cvars::ppc_profile_delay seconds after the first call and
// ends once one thread recorded cvars::ppc_profile_count instructions; the
// profile is then written to cvars::ppc_profile_out, little-endian, for every
// thread that recorded something:
//   uint32 magic 0x33525050 ("PPR3"), uint32 thread id,
//   uint32 record count, uint32 0, uint64 instructions recorded;
//   per record: uint32 address, uint32 instruction, uint64 counts: run,
//     taken, load-hit-store, 4 KB alias, L1 I miss, L2 I miss, L1 D miss,
//     L2 D miss, store L2 miss, dependency, load-use, FP stall cycles,
//     FP dependency, same doubleword;
//   uint64 edge count; per edge: uint64 (from << 32) | to, uint64 count.
void ProfileInsn(PPCContext* ctx, void* arg0, void* arg1);

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_PROFILER_H_
