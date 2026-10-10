#pragma once

#include <cstdint>
#include <span>
#include <string>

// Dalvik instruction set: formats, operand kinds and a generic decoder shared
// by the verifier/interpreter, the disassembler and the smali assembler.
namespace aa::dex {

class DexFile;

enum class Format : uint8_t {
  k10x, k12x, k11n, k11x, k10t, k20t, k22x, k21t, k21s, k21h, k21c, k23x, k22b, k22t, k22s, k22c,
  k30t, k32x, k31i, k31t, k31c, k35c, k3rc, k45cc, k4rcc, k51l, kUnused,
};

enum class IndexKind : uint8_t { kNone, kString, kType, kField, kMethod, kCallSite, kMethodHandle, kProto, kMethodProto };

struct OpInfo {
  const char* name;
  Format format;
  IndexKind index;
};

const OpInfo& op_info(uint8_t opcode);
// Looks an opcode up by its mnemonic ("invoke-virtual"); returns -1 if none.
int find_opcode(std::string_view name);
uint32_t format_units(Format f);

// Pseudo-instruction identifiers stored in the "nop" slot.
constexpr uint16_t kPackedSwitchPayload = 0x0100;
constexpr uint16_t kSparseSwitchPayload = 0x0200;
constexpr uint16_t kFillArrayDataPayload = 0x0300;

// Size in code units of the payload starting at insns[pc], or 0 if insns[pc]
// is not a payload. Throws FormatError if the payload is truncated.
uint32_t payload_units(std::span<const uint16_t> insns, uint32_t pc);

struct Decoded {
  uint8_t op = 0;
  Format format = Format::k10x;
  uint32_t units = 1;
  uint32_t a = 0, b = 0, c = 0;  // register operands (or count for 35c/3rc)
  int64_t literal = 0;           // literals and branch offsets
  uint32_t index = 0;            // string/type/field/method/... index
  uint32_t index2 = 0;           // proto index for 45cc/4rcc
  uint32_t args[5] = {};         // 35c argument registers
  uint32_t arg_count = 0;
};

// Decodes the instruction at insns[pc]. Throws FormatError if truncated or
// if the opcode is unused.
Decoded decode(std::span<const uint16_t> insns, uint32_t pc);

// One-line disassembly ("invoke-virtual {v0, v1}, Lx;->m(I)V").
std::string disassemble(const DexFile* dex, const Decoded& d, uint32_t pc);

}  // namespace aa::dex
