/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/cpu_flags.h"

DEFINE_string(cpu, "any", "CPU backend [any, x64].", "CPU");

DEFINE_string(
    load_module_map, "",
    "Loads a .map for symbol names and to diff with the generated symbol "
    "database.",
    "CPU");

DEFINE_bool(disassemble_functions, false,
            "Disassemble functions during generation.", "CPU");

DEFINE_bool(trace_functions, false, "Generate tracing for function statistics.",
            "CPU");
DEFINE_bool(trace_function_coverage, false,
            "Generate tracing for function instruction coverage statistics.",
            "CPU");
DEFINE_bool(trace_function_references, false,
            "Generate tracing for function address references.", "CPU");
DEFINE_bool(trace_function_data, false,
            "Generate tracing for function result data.", "CPU");

DEFINE_bool(
    disable_global_lock, false,
    "Disables global lock usage in guest code. Does not affect host code.",
    "CPU");

DEFINE_bool(validate_hir, false,
            "Perform validation checks on the HIR during compilation.", "CPU");

DEFINE_uint64(
    pvr, 0x710700,
    "Processor version and revision number.\nBits 0 to 15 are the version "
    "number.\nBits 16 to 31 are the revision number.\nNote: Some XEXs (such as "
    "mfgbootlauncher.xex) may check for a value that's less than 0x710700.",
    "CPU");

// Breakpoints:
DEFINE_uint64(break_on_instruction, 0,
              "int3 before the given guest address is executed.", "CPU");
DEFINE_int32(break_condition_gpr, -1, "GPR compared to", "CPU");
DEFINE_uint64(break_condition_value, 0, "value compared against", "CPU");
DEFINE_string(break_condition_op, "eq", "comparison operator", "CPU");
DEFINE_bool(break_condition_truncate, true, "truncate value to 32-bits", "CPU");

DEFINE_bool(break_on_debugbreak, true, "int3 on JITed __debugbreak requests.",
            "CPU");

DEFINE_bool(ppc_profile, false,
            "Instruction profile of guest code (ppc_profiler.h): a call before "
            "every guest instruction records how often it ran, taken "
            "branches, dependencies, load-hit-stores and misses in a cache "
            "model. Very slow.",
            "CPU");
DEFINE_path(ppc_profile_out, "ppcprof.bin",
            "ppc_profile: file the profile is written to.", "CPU");
DEFINE_uint64(ppc_profile_delay, 60,
              "ppc_profile: seconds (wall clock) before recording starts.",
              "CPU");
DEFINE_uint64(ppc_profile_count, 200000000,
              "ppc_profile: instructions of the busiest thread to record, then "
              "the profile is written.",
              "CPU");
