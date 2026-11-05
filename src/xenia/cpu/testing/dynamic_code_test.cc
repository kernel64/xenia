/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Guest code generated at run time (cvars::dynamic_code): code ranges, the
// dropping of translated functions when the guest rewrites their memory, and
// the code cache zone holding their host code.

#include "xenia/cpu/testing/util.h"

#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/entry_table.h"

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;

namespace {

// Sets cvars::dynamic_code for the scope of a test, before the processor and
// its code cache are created (the cache reads it when it is initialized).
class DynamicCodeCvar {
 public:
  explicit DynamicCodeCvar(bool value) : previous_(cvars::dynamic_code) {
    cvars::dynamic_code = value;
  }
  ~DynamicCodeCvar() { cvars::dynamic_code = previous_; }

 private:
  bool previous_;
};

constexpr uint32_t kCodeLow = 0x80000000;
constexpr uint32_t kCodeHigh = 0x80010000;

// A processor (x64 backend) with one test module covering
// [kCodeLow, kCodeHigh): every address there is a function returning at once.
class DynamicCodeTest {
 public:
  DynamicCodeTest() {
    memory_ = std::make_unique<xe::Memory>();
    memory_->Initialize();
    processor_ = std::make_unique<Processor>(memory_.get(), nullptr);
    processor_->Setup(std::make_unique<backend::x64::X64Backend>());
    auto module = std::make_unique<TestModule>(
        processor_.get(), "Test",
        [](uint32_t address) {
          return address >= kCodeLow && address < kCodeHigh;
        },
        [](HIRBuilder& b) {
          b.Return();
          return true;
        });
    module_ = module.get();
    processor_->AddModule(std::move(module));
    processor_->backend()->CommitExecutableRange(kCodeLow, kCodeHigh);
  }
  ~DynamicCodeTest() {
    processor_.reset();
    memory_.reset();
  }

  Processor* processor() const { return processor_.get(); }
  Module* module() const { return module_; }

 private:
  std::unique_ptr<xe::Memory> memory_;
  std::unique_ptr<Processor> processor_;
  Module* module_ = nullptr;
};

}  // namespace

TEST_CASE("DYNAMIC_CODE_DISABLED_IGNORES_RANGES", "[dynamic_code]") {
  DynamicCodeCvar cvar(false);
  DynamicCodeTest test;
  Processor* processor = test.processor();

  processor->AddDynamicCodeRange(kCodeLow, kCodeHigh);
  REQUIRE_FALSE(processor->has_dynamic_code());
  REQUIRE_FALSE(processor->IsDynamicCode(kCodeLow));
  REQUIRE(processor->backend()->DynamicCodeCapacity() == 0);

  // Translated functions are kept: KeSweepIcacheRange changes nothing.
  Function* function = processor->ResolveFunction(kCodeLow);
  REQUIRE(function != nullptr);
  processor->InvalidateCodeRange(kCodeLow, 4);
  REQUIRE(processor->ResolveFunction(kCodeLow) == function);
}

TEST_CASE("DYNAMIC_CODE_RANGES", "[dynamic_code]") {
  DynamicCodeCvar cvar(true);
  DynamicCodeTest test;
  Processor* processor = test.processor();

  REQUIRE_FALSE(processor->has_dynamic_code());
  processor->AddDynamicCodeRange(0x80001000, 0x80002000);
  // Empty ranges are ignored.
  processor->AddDynamicCodeRange(0x80003000, 0x80003000);
  REQUIRE(processor->has_dynamic_code());
  REQUIRE(processor->dynamic_code_ranges().size() == 1);

  REQUIRE_FALSE(processor->IsDynamicCode(0x80000FFC));
  REQUIRE(processor->IsDynamicCode(0x80001000));
  REQUIRE(processor->IsDynamicCode(0x80001FFC));
  REQUIRE_FALSE(processor->IsDynamicCode(0x80002000));
  REQUIRE_FALSE(processor->IsDynamicCode(0x80003000));
  REQUIRE(processor->backend()->DynamicCodeCapacity() > 0);
}

TEST_CASE("DYNAMIC_CODE_INVALIDATE_RETRANSLATES", "[dynamic_code]") {
  DynamicCodeCvar cvar(true);
  DynamicCodeTest test;
  Processor* processor = test.processor();
  processor->AddDynamicCodeRange(kCodeLow, kCodeLow + 0x1000);

  Function* dynamic_function = processor->ResolveFunction(kCodeLow);
  Function* static_function = processor->ResolveFunction(kCodeLow + 0x2000);
  REQUIRE(dynamic_function != nullptr);
  REQUIRE(static_function != nullptr);
  // Host code of dynamic functions goes into the dynamic zone only.
  size_t used = processor->backend()->DynamicCodeUsed();
  REQUIRE(used > 0);

  // A write elsewhere keeps the function.
  processor->InvalidateCodeRange(kCodeLow + 0x800, 4);
  REQUIRE(processor->ResolveFunction(kCodeLow) == dynamic_function);

  // A write over it drops it: it is translated again (a new function).
  processor->InvalidateCodeRange(kCodeLow, 4);
  Function* retranslated = processor->ResolveFunction(kCodeLow);
  REQUIRE(retranslated != nullptr);
  REQUIRE(retranslated != dynamic_function);
  REQUIRE(processor->backend()->DynamicCodeUsed() > used);

  // Code outside the dynamic ranges is never dropped.
  processor->InvalidateCodeRange(kCodeLow + 0x2000, 4);
  REQUIRE(processor->ResolveFunction(kCodeLow + 0x2000) == static_function);
}

TEST_CASE("DYNAMIC_CODE_ZONE_RESET", "[dynamic_code]") {
  DynamicCodeCvar cvar(true);
  DynamicCodeTest test;
  Processor* processor = test.processor();
  processor->AddDynamicCodeRange(kCodeLow, kCodeHigh);

  auto backend = processor->backend();
  REQUIRE(processor->ResolveFunction(kCodeLow) != nullptr);
  REQUIRE(backend->DynamicCodeUsed() > 0);
  // No host code of the zone runs on this thread.
  REQUIRE_FALSE(backend->DynamicCodeOnStack());
  backend->ResetDynamicCode();
  REQUIRE(backend->DynamicCodeUsed() == 0);
}

TEST_CASE("ENTRY_TABLE_REMOVE", "[dynamic_code]") {
  EntryTable table;
  Function* function = reinterpret_cast<Function*>(uintptr_t(0x1234));
  Function* removed = function;

  // Unknown address.
  REQUIRE_FALSE(table.Remove(0x82000000, &removed));
  REQUIRE(removed == nullptr);

  // Being translated (new or compiling): kept.
  Entry* entry = nullptr;
  REQUIRE(table.GetOrCreate(0x82000000, &entry) == Entry::STATUS_NEW);
  REQUIRE_FALSE(table.Remove(0x82000000, &removed));
  entry->status = Entry::STATUS_COMPILING;
  REQUIRE_FALSE(table.Remove(0x82000000, &removed));

  // Ready: removed, its function returned, the next lookup starts anew.
  entry->function = function;
  entry->status = Entry::STATUS_READY;
  REQUIRE(table.Remove(0x82000000, &removed));
  REQUIRE(removed == function);
  REQUIRE(table.Get(0x82000000) == nullptr);
  Entry* new_entry = nullptr;
  REQUIRE(table.GetOrCreate(0x82000000, &new_entry) == Entry::STATUS_NEW);
  REQUIRE(new_entry != entry);

  // Failed entries are removed too.
  new_entry->status = Entry::STATUS_FAILED;
  REQUIRE(table.Remove(0x82000000, &removed));
  REQUIRE(removed == nullptr);
}

TEST_CASE("MODULE_FORGET_FUNCTION", "[dynamic_code]") {
  DynamicCodeCvar cvar(false);
  DynamicCodeTest test;
  Module* module = test.module();

  Function* function = nullptr;
  REQUIRE(module->DeclareFunction(kCodeLow, &function) ==
          Symbol::Status::kDefined);
  Function* same = nullptr;
  REQUIRE(module->DeclareFunction(kCodeLow, &same) == Symbol::Status::kDefined);
  REQUIRE(same == function);

  // Unknown addresses are ignored.
  module->ForgetFunction(kCodeLow + 0x100);

  // Forgotten: the next declaration creates a new function.
  module->ForgetFunction(kCodeLow);
  Function* fresh = nullptr;
  REQUIRE(module->DeclareFunction(kCodeLow, &fresh) ==
          Symbol::Status::kDefined);
  REQUIRE(fresh != function);
}
