#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dex-engine/opcodes.h"

// In-memory model of classes to emit as a DEX file. Instructions refer to
// strings/types/fields/methods symbolically through fixups; the writer assigns
// the final (sorted) indices and patches the code.
namespace aa::tool {

struct StaticValue {
  enum class Kind { kNone, kInt, kLong, kFloat, kDouble, kBool, kString, kNull } kind = Kind::kNone;
  uint64_t bits = 0;
  std::string str;
};

struct FieldDef {
  std::string name;
  std::string type;
  uint32_t access = 0;
  StaticValue initial;
};

struct Fixup {
  uint32_t unit = 0;  // code unit holding the index (low half if 32-bit)
  dex::IndexKind kind = dex::IndexKind::kNone;
  std::string ref;    // string value, type descriptor, "Lc;->f:T" or "Lc;->m(..)R"
  bool wide = false;  // 32-bit index (const-string/jumbo)
};

struct TryDef {
  uint32_t start = 0;
  uint32_t count = 0;
  std::vector<std::pair<std::string, uint32_t>> catches;  // (type, handler address)
  std::optional<uint32_t> catch_all;
};

struct MethodDef {
  std::string name;
  std::string signature;
  uint32_t access = 0;
  bool has_code = false;
  uint16_t registers = 0;
  uint16_t ins = 0;
  uint16_t outs = 0;
  std::vector<uint16_t> insns;
  std::vector<Fixup> fixups;
  std::vector<TryDef> tries;
};

struct ClassDef {
  std::string descriptor;
  std::string super;  // empty only for java/lang/Object
  std::vector<std::string> interfaces;
  uint32_t access = 0;
  std::string source_file;
  std::vector<FieldDef> static_fields;
  std::vector<FieldDef> instance_fields;
  std::vector<MethodDef> direct_methods;
  std::vector<MethodDef> virtual_methods;
};

// Builds a DEX 035 file. Throws std::runtime_error on inconsistent input.
std::vector<uint8_t> write_dex(const std::vector<ClassDef>& classes);

}  // namespace aa::tool
