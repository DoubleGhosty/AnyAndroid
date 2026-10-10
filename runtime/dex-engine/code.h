#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "dex-engine/dex_file.h"

namespace aa::rt {

// One pre-decoded instruction. Branch targets are converted to instruction
// indices so the interpreter never deals with raw code-unit offsets.
struct Insn {
  uint8_t op = 0;
  uint8_t argc = 0;
  uint32_t a = 0, b = 0, c = 0;
  int64_t lit = 0;
  uint32_t index = 0;
  uint32_t index2 = 0;  // call-site prototype of invoke-polymorphic
  uint32_t args[5] = {};
  uint32_t pc = 0;      // address in code units, for diagnostics and try ranges
  int32_t target = -1;  // branch target index, or payload table index (31t)
};

struct SwitchTable {
  bool packed = true;
  int32_t first_key = 0;
  std::vector<int32_t> keys;     // sparse only, sorted
  std::vector<int32_t> targets;  // instruction indices
};

struct ArrayPayload {
  uint16_t width = 0;
  uint32_t count = 0;
  std::vector<uint8_t> data;
};

struct CatchClause {
  uint32_t type_idx;  // dex::kNoIndex for catch-all
  int32_t target;
};

struct TryBlock {
  uint32_t start = 0, end = 0;  // [start, end) in code units
  std::vector<CatchClause> clauses;
};

struct PreparedCode {
  uint16_t registers = 0;
  uint16_t ins = 0;
  std::vector<Insn> insns;
  std::vector<SwitchTable> switches;
  std::vector<ArrayPayload> arrays;
  std::vector<TryBlock> tries;
};

// Decodes a method body and checks its structure: every instruction decodes,
// register and constant-pool indices are in range, branch/switch/handler
// targets land on instruction boundaries and execution cannot run off the
// end. This is not a type verifier: type confusion is caught dynamically by
// the interpreter, which keeps host memory safe either way.
// Throws FormatError with a precise message on failure.
std::unique_ptr<PreparedCode> prepare_code(const dex::DexFile& dex, const dex::CodeItem& code);

}  // namespace aa::rt
