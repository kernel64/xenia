/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_profiler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/cpu_flags.h"

namespace xe {
namespace cpu {
namespace ppc {

namespace {

struct ProfRec {
  uint32_t code;
  uint64_t n, taken, lhs, alias, l1i, l2i, l1d, l2d, st2, dep1, ldUse, fpStall,
      fpDep, dw;  // dw: the other word of a doubleword stored just before
};

struct ProfCache {
  uint32_t sets, ways;
  std::vector<uint32_t> tag;
  std::vector<uint64_t> use;
  uint64_t tick = 0;
  void Init(uint32_t bytes, uint32_t w) {
    ways = w;
    sets = bytes / 128 / w;
    tag.assign(sets * ways, 0);
    use.assign(sets * ways, 0);
  }
  // 1: miss (the line is filled)
  int Access(uint32_t line, bool fill = true) {
    uint32_t s = line % sets, k, lru = 0;
    uint32_t* t = &tag[s * ways];
    uint64_t* u = &use[s * ways];
    ++tick;
    for (k = 0; k < ways; k++) {
      if (t[k] == line + 1) {
        u[k] = tick;
        return 0;
      }
    }
    if (!fill) return 1;
    for (k = 1; k < ways; k++) {
      if (u[k] < u[lru]) lru = k;
    }
    t[lru] = line + 1;
    u[lru] = tick;
    return 1;
  }
};

struct ProfThread {
  uint32_t thread_id = 0;
  uint64_t count = 0, recorded = 0;
  std::unordered_map<uint32_t, ProfRec> recs;
  std::unordered_map<uint64_t, uint64_t> edges;  // taken: (from << 32) | to
  uint32_t prev_pc = 0;
  ProfRec* prev_rec = nullptr;
  uint32_t last_iline = 0xFFFFFFFF;
  uint64_t gpr_at[32] = {}, fpr_at[32] = {};
  bool gpr_load[32] = {};
  uint32_t st_addr[32] = {}, st_size[32] = {};
  uint64_t st_at[32] = {};
  uint32_t st_pos = 0;
  ProfCache l1i, l1d, l2;
  ProfThread() {
    l1i.Init(32 * 1024, 2);
    l1d.Init(32 * 1024, 4);
    l2.Init(512 * 1024, 8);
    recs.reserve(1 << 18);
  }
};

std::mutex prof_mutex;
std::vector<ProfThread*> prof_threads;
std::atomic<int> prof_state{0};  // 0 waiting, 1 recording, 2 done
std::chrono::steady_clock::time_point prof_t0;
std::once_flag prof_once;
thread_local ProfThread* t_prof = nullptr;

void ProfDump() {
  std::this_thread::sleep_for(
      std::chrono::milliseconds(300));  // the others leave their handler
  FILE* f = xe::filesystem::OpenFile(cvars::ppc_profile_out, "wb");
  if (!f) return;
  std::lock_guard<std::mutex> lock(prof_mutex);
  for (auto* t : prof_threads) {
    if (!t->recorded) continue;
    uint32_t head[4] = {0x33525050u /* PPR3: edges, dw */, t->thread_id,
                        (uint32_t)t->recs.size(), 0};
    fwrite(head, 4, 4, f);
    fwrite(&t->recorded, 8, 1, f);
    for (auto& kv : t->recs) {
      const ProfRec& r = kv.second;
      uint32_t a[2] = {kv.first, r.code};
      uint64_t v[14] = {r.n,     r.taken,   r.lhs,   r.alias, r.l1i,
                        r.l2i,   r.l1d,     r.l2d,   r.st2,   r.dep1,
                        r.ldUse, r.fpStall, r.fpDep, r.dw};
      fwrite(a, 4, 2, f);
      fwrite(v, 8, 14, f);
    }
    uint64_t ne = t->edges.size();
    fwrite(&ne, 8, 1, f);
    for (auto& kv : t->edges) {
      uint64_t e[2] = {kv.first, kv.second};
      fwrite(e, 8, 2, f);
    }
  }
  fclose(f);
  XELOGI("ppc_profile: written to {}",
         xe::path_to_utf8(cvars::ppc_profile_out));
}

inline uint32_t Bit(uint32_t r) { return 1u << r; }

struct Dec {
  int gd = -1, fd = -1;
  uint32_t gsrc = 0, fsrc = 0;
  int mem = 0;  // 1 load, 2 store
  uint32_t ea = 0, size = 0;
  bool load_int = false;
};

void Decode(const PPCContext* ctx, uint32_t code, Dec& d) {
  uint32_t op = code >> 26, rD = (code >> 21) & 31, rA = (code >> 16) & 31,
           rB = (code >> 11) & 31;
  int32_t simm = (int16_t)(code & 0xFFFF);
  auto gpr = [&](uint32_t n) { return (uint32_t)ctx->r[n]; };
  auto base = [&]() { return rA ? gpr(rA) : 0u; };
  auto ldD = [&](uint32_t size, bool fp) {
    if (fp)
      d.fd = rD;
    else {
      d.gd = rD;
      d.load_int = true;
    }
    d.gsrc = rA ? Bit(rA) : 0;
    d.mem = 1;
    d.ea = base() + simm;
    d.size = size;
  };
  auto stD = [&](uint32_t size, bool fp) {
    if (fp)
      d.fsrc = Bit(rD);
    else
      d.gsrc = Bit(rD);
    if (rA) d.gsrc |= Bit(rA);
    d.mem = 2;
    d.ea = base() + simm;
    d.size = size;
  };
  switch (op) {
    case 14:
    case 15:
      d.gd = rD;
      d.gsrc = rA ? Bit(rA) : 0;
      break;
    case 7:
    case 8:
    case 12:
    case 13:
      d.gd = rD;
      d.gsrc = Bit(rA);
      break;
    case 10:
    case 11:
      d.gsrc = Bit(rA);
      break;
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 21:
    case 30:
      d.gd = rA;
      d.gsrc = Bit(rD);
      break;
    case 20:
      d.gd = rA;
      d.gsrc = Bit(rD) | Bit(rA);
      break;
    case 23:
      d.gd = rA;
      d.gsrc = Bit(rD) | Bit(rB);
      break;
    case 32:
    case 33:
      ldD(4, false);
      break;
    case 34:
    case 35:
      ldD(1, false);
      break;
    case 40:
    case 41:
    case 42:
    case 43:
      ldD(2, false);
      break;
    case 46:
      ldD(4, false);
      break;
    case 36:
    case 37:
    case 47:
      stD(4, false);
      break;
    case 38:
    case 39:
      stD(1, false);
      break;
    case 44:
    case 45:
      stD(2, false);
      break;
    case 48:
    case 49:
      ldD(4, true);
      break;
    case 50:
    case 51:
      ldD(8, true);
      break;
    case 52:
    case 53:
      stD(4, true);
      break;
    case 54:
    case 55:
      stD(8, true);
      break;
    case 58:
      d.gd = rD;
      d.load_int = true;
      d.gsrc = rA ? Bit(rA) : 0;
      d.mem = 1;
      d.ea = base() + (simm & ~3);
      d.size = (code & 3) == 2 ? 4 : 8;
      break;
    case 62:
      d.gsrc = Bit(rD) | (rA ? Bit(rA) : 0);
      d.mem = 2;
      d.ea = base() + (simm & ~3);
      d.size = 8;
      break;
    case 59:
    case 63: {
      uint32_t xo10 = (code >> 1) & 1023, xo5 = (code >> 1) & 31,
               rC = (code >> 6) & 31;
      if (op == 63 && (xo10 == 0 || xo10 == 32)) {
        d.fsrc = Bit(rA) | Bit(rB);
      } else if (op == 63 && xo5 < 18) {
        if (xo10 == 583)
          d.fd = rD;
        else if (xo10 == 711)
          d.fsrc = Bit(rB);
        else {
          d.fd = rD;
          d.fsrc = Bit(rB);
        }
      } else {
        d.fd = rD;
        d.fsrc = Bit(rA) | Bit(rB) | Bit(rC);
      }
      break;
    }
    case 31: {
      uint32_t xo = (code >> 1) & 1023;
      auto ldX = [&](uint32_t size, bool fp) {
        if (fp)
          d.fd = rD;
        else {
          d.gd = rD;
          d.load_int = true;
        }
        d.gsrc = (rA ? Bit(rA) : 0) | Bit(rB);
        d.mem = 1;
        d.ea = base() + gpr(rB);
        d.size = size;
      };
      auto stX = [&](uint32_t size, bool fp) {
        if (fp)
          d.fsrc = Bit(rD);
        else
          d.gsrc = Bit(rD);
        d.gsrc |= (rA ? Bit(rA) : 0) | Bit(rB);
        d.mem = 2;
        d.ea = base() + gpr(rB);
        d.size = size;
      };
      switch (xo) {
        case 23:
        case 55:
        case 341:
        case 534:
          ldX(4, false);
          break;
        case 87:
        case 119:
          ldX(1, false);
          break;
        case 279:
        case 311:
        case 343:
        case 375:
        case 790:
          ldX(2, false);
          break;
        case 21:
        case 53:
          ldX(8, false);
          break;
        case 151:
        case 183:
        case 662:
          stX(4, false);
          break;
        case 215:
        case 247:
          stX(1, false);
          break;
        case 407:
        case 439:
        case 918:
          stX(2, false);
          break;
        case 149:
        case 181:
          stX(8, false);
          break;
        case 535:
        case 567:
          ldX(4, true);
          break;
        case 599:
        case 631:
          ldX(8, true);
          break;
        case 663:
        case 695:
        case 983:
          stX(4, true);
          break;
        case 727:
        case 759:
          stX(8, true);
          break;
        case 103:
        case 7:
        case 39:
        case 71:
        case 519:
        case 551:
          d.gsrc = (rA ? Bit(rA) : 0) | Bit(rB);
          d.mem = 1;
          d.ea = (base() + gpr(rB)) & ~15u;
          d.size = 16;
          break;
        case 231:
        case 135:
        case 167:
        case 199:
        case 647:
        case 679:
          d.gsrc = (rA ? Bit(rA) : 0) | Bit(rB);
          d.mem = 2;
          d.ea = (base() + gpr(rB)) & ~15u;
          d.size = 16;
          break;
        case 278:
        case 246:  // dcbt, dcbtst: a prefetch (the cache model fills the line)
          d.gsrc = (rA ? Bit(rA) : 0) | Bit(rB);
          d.mem = 3;
          d.ea = base() + gpr(rB);
          d.size = 1;
          break;
        case 1014:
        case 54:
        case 86:
        case 470:
          d.gsrc = (rA ? Bit(rA) : 0) | Bit(rB);
          break;
        case 28:
        case 60:
        case 124:
        case 284:
        case 316:
        case 412:
        case 444:
        case 476:
        case 24:
        case 536:
        case 792:
        case 27:
        case 539:
        case 794:
          d.gd = rA;
          d.gsrc = Bit(rD) | Bit(rB);
          break;
        case 824:
        case 413:
        case 414:
        case 954:
        case 922:
        case 986:
        case 26:
        case 58:
          d.gd = rA;
          d.gsrc = Bit(rD);
          break;
        case 339:
        case 19:
        case 83:
        case 371:
          d.gd = rD;
          break;
        case 467:
        case 144:
        case 146:
        case 178:
          d.gsrc = Bit(rD);
          break;
        case 0:
        case 32:
          d.gsrc = Bit(rA) | Bit(rB);
          break;
        default:
          d.gd = rD;
          d.gsrc = Bit(rA) | Bit(rB);
          break;
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace

void ProfileInsn(PPCContext* ctx, void* arg0, void* arg1) {
  int state = prof_state.load(std::memory_order_relaxed);
  if (state == 2) return;
  std::call_once(prof_once, [] { prof_t0 = std::chrono::steady_clock::now(); });
  ProfThread* t = t_prof;
  if (!t) {
    t = new ProfThread();
    t->thread_id = ctx->thread_id;
    std::lock_guard<std::mutex> lock(prof_mutex);
    prof_threads.push_back(t);
    t_prof = t;
  }
  uint32_t pc = (uint32_t)ctx->scratch;
  if (state == 0) {
    if ((++t->count & 0xFFFFF) == 0 &&
        std::chrono::steady_clock::now() - prof_t0 >=
            std::chrono::seconds(cvars::ppc_profile_delay)) {
      int expected = 0;
      if (prof_state.compare_exchange_strong(expected, 1)) {
        XELOGI("ppc_profile: recording");
      }
    }
    t->prev_pc = pc;
    return;
  }
  uint64_t idx = ++t->recorded;
  if (idx > cvars::ppc_profile_count) {
    int expected = 1;
    if (prof_state.compare_exchange_strong(expected, 2)) ProfDump();
    return;
  }
  uint32_t code = xe::load_and_swap<uint32_t>(ctx->virtual_membase + pc);
  ProfRec& r = t->recs[pc];
  r.code = code;
  r.n++;
  if (t->prev_rec && pc != t->prev_pc + 4) {
    t->prev_rec->taken++;
    t->edges[((uint64_t)t->prev_pc << 32) | pc]++;
  }
  // instruction fetch: a new 128-byte line
  uint32_t iline = pc >> 7;
  if (iline != t->last_iline) {
    t->last_iline = iline;
    if (t->l1i.Access(iline)) {
      r.l1i++;
      if (t->l2.Access(iline)) r.l2i++;
    }
  }
  Dec d;
  Decode(ctx, code, d);
  // integer dependencies: a source written by the previous instruction
  for (uint32_t s = d.gsrc; s; s &= s - 1) {
    uint32_t reg = xe::tzcnt(s);
    if (t->gpr_at[reg] == idx - 1) {
      r.dep1++;
      if (t->gpr_load[reg]) r.ldUse++;
      break;
    }
  }
  // FP: ~10 cycles of latency on Xenon
  {
    uint64_t worst = 0;
    bool dep = false;
    for (uint32_t s = d.fsrc; s; s &= s - 1) {
      uint32_t reg = xe::tzcnt(s);
      if (t->fpr_at[reg] && idx - t->fpr_at[reg] < 10) {
        uint64_t st = 10 - (idx - t->fpr_at[reg]);
        if (st > worst) worst = st;
        dep = true;
      }
    }
    r.fpStall += worst;
    if (dep) r.fpDep++;
  }
  if (d.gd >= 0) {
    t->gpr_at[d.gd] = idx;
    t->gpr_load[d.gd] = d.load_int;
  }
  if (d.fd >= 0) t->fpr_at[d.fd] = idx;
  if (d.mem == 1) {
    // load-hit-store: a store of the last 24 instructions overlapping it
    for (uint32_t k = 0; k < 32; k++) {
      uint64_t at = t->st_at[k];
      if (!at || idx - at > 24) continue;
      uint32_t a = t->st_addr[k], sz = t->st_size[k];
      if (a < d.ea + d.size && d.ea < a + sz) {
        r.lhs++;
        break;
      }
      if ((a >> 3) == (d.ea >> 3)) {
        r.dw++;  // same doubleword, other bytes
        break;
      }
      if (((a ^ d.ea) & 0xFF8) == 0) {
        r.alias++;  // 4 KB apart
        break;
      }
    }
    uint32_t line = d.ea >> 7;
    if (t->l1d.Access(line)) {
      r.l1d++;
      if (t->l2.Access(line)) r.l2d++;
    }
  } else if (d.mem == 3) {
    // a prefetch: into L2 (and L1 D), no stall counted (an upper bound: the
    // model has no timing, the line is there when used)
    uint32_t line = d.ea >> 7;
    t->l2.Access(line);
    t->l1d.Access(line);
  } else if (d.mem == 2) {
    uint32_t k = t->st_pos++ & 31;
    t->st_addr[k] = d.ea;
    t->st_size[k] = d.size;
    t->st_at[k] = idx;
    uint32_t line = d.ea >> 7;
    t->l1d.Access(line, false);  // write-through, no allocation
    if (t->l2.Access(line)) r.st2++;
  }
  t->prev_pc = pc;
  t->prev_rec = &r;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
