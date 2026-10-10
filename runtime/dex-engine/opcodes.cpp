#include "dex-engine/opcodes.h"

#include <array>
#include <cstdio>

#include "common/bytes.h"
#include "common/text.h"
#include "dex-engine/dex_file.h"

namespace aa::dex {

namespace {

using F = Format;
using I = IndexKind;

std::array<OpInfo, 256> build_table() {
  std::array<OpInfo, 256> t;
  t.fill({nullptr, F::kUnused, I::kNone});
  auto set = [&](int op, const char* name, F f, I i = I::kNone) { t[op] = {name, f, i}; };

  set(0x00, "nop", F::k10x);
  set(0x01, "move", F::k12x);
  set(0x02, "move/from16", F::k22x);
  set(0x03, "move/16", F::k32x);
  set(0x04, "move-wide", F::k12x);
  set(0x05, "move-wide/from16", F::k22x);
  set(0x06, "move-wide/16", F::k32x);
  set(0x07, "move-object", F::k12x);
  set(0x08, "move-object/from16", F::k22x);
  set(0x09, "move-object/16", F::k32x);
  set(0x0a, "move-result", F::k11x);
  set(0x0b, "move-result-wide", F::k11x);
  set(0x0c, "move-result-object", F::k11x);
  set(0x0d, "move-exception", F::k11x);
  set(0x0e, "return-void", F::k10x);
  set(0x0f, "return", F::k11x);
  set(0x10, "return-wide", F::k11x);
  set(0x11, "return-object", F::k11x);
  set(0x12, "const/4", F::k11n);
  set(0x13, "const/16", F::k21s);
  set(0x14, "const", F::k31i);
  set(0x15, "const/high16", F::k21h);
  set(0x16, "const-wide/16", F::k21s);
  set(0x17, "const-wide/32", F::k31i);
  set(0x18, "const-wide", F::k51l);
  set(0x19, "const-wide/high16", F::k21h);
  set(0x1a, "const-string", F::k21c, I::kString);
  set(0x1b, "const-string/jumbo", F::k31c, I::kString);
  set(0x1c, "const-class", F::k21c, I::kType);
  set(0x1d, "monitor-enter", F::k11x);
  set(0x1e, "monitor-exit", F::k11x);
  set(0x1f, "check-cast", F::k21c, I::kType);
  set(0x20, "instance-of", F::k22c, I::kType);
  set(0x21, "array-length", F::k12x);
  set(0x22, "new-instance", F::k21c, I::kType);
  set(0x23, "new-array", F::k22c, I::kType);
  set(0x24, "filled-new-array", F::k35c, I::kType);
  set(0x25, "filled-new-array/range", F::k3rc, I::kType);
  set(0x26, "fill-array-data", F::k31t);
  set(0x27, "throw", F::k11x);
  set(0x28, "goto", F::k10t);
  set(0x29, "goto/16", F::k20t);
  set(0x2a, "goto/32", F::k30t);
  set(0x2b, "packed-switch", F::k31t);
  set(0x2c, "sparse-switch", F::k31t);
  const char* cmp[] = {"cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double", "cmp-long"};
  for (int i = 0; i < 5; ++i) set(0x2d + i, cmp[i], F::k23x);
  const char* if2[] = {"if-eq", "if-ne", "if-lt", "if-ge", "if-gt", "if-le"};
  for (int i = 0; i < 6; ++i) set(0x32 + i, if2[i], F::k22t);
  const char* ifz[] = {"if-eqz", "if-nez", "if-ltz", "if-gez", "if-gtz", "if-lez"};
  for (int i = 0; i < 6; ++i) set(0x38 + i, ifz[i], F::k21t);
  const char* aget[] = {"aget", "aget-wide", "aget-object", "aget-boolean", "aget-byte", "aget-char", "aget-short"};
  const char* aput[] = {"aput", "aput-wide", "aput-object", "aput-boolean", "aput-byte", "aput-char", "aput-short"};
  const char* iget[] = {"iget", "iget-wide", "iget-object", "iget-boolean", "iget-byte", "iget-char", "iget-short"};
  const char* iput[] = {"iput", "iput-wide", "iput-object", "iput-boolean", "iput-byte", "iput-char", "iput-short"};
  const char* sget[] = {"sget", "sget-wide", "sget-object", "sget-boolean", "sget-byte", "sget-char", "sget-short"};
  const char* sput[] = {"sput", "sput-wide", "sput-object", "sput-boolean", "sput-byte", "sput-char", "sput-short"};
  for (int i = 0; i < 7; ++i) {
    set(0x44 + i, aget[i], F::k23x);
    set(0x4b + i, aput[i], F::k23x);
    set(0x52 + i, iget[i], F::k22c, I::kField);
    set(0x59 + i, iput[i], F::k22c, I::kField);
    set(0x60 + i, sget[i], F::k21c, I::kField);
    set(0x67 + i, sput[i], F::k21c, I::kField);
  }
  const char* inv[] = {"invoke-virtual", "invoke-super", "invoke-direct", "invoke-static", "invoke-interface"};
  const char* invr[] = {"invoke-virtual/range", "invoke-super/range", "invoke-direct/range", "invoke-static/range",
                        "invoke-interface/range"};
  for (int i = 0; i < 5; ++i) {
    set(0x6e + i, inv[i], F::k35c, I::kMethod);
    set(0x74 + i, invr[i], F::k3rc, I::kMethod);
  }
  const char* unop[] = {"neg-int",      "not-int",       "neg-long",      "not-long",     "neg-float",
                        "neg-double",   "int-to-long",   "int-to-float",  "int-to-double", "long-to-int",
                        "long-to-float", "long-to-double", "float-to-int", "float-to-long", "float-to-double",
                        "double-to-int", "double-to-long", "double-to-float", "int-to-byte", "int-to-char",
                        "int-to-short"};
  for (int i = 0; i < 21; ++i) set(0x7b + i, unop[i], F::k12x);
  static const char* binop[] = {"add-int",   "sub-int",   "mul-int",   "div-int",    "rem-int",    "and-int",
                                "or-int",    "xor-int",   "shl-int",   "shr-int",    "ushr-int",   "add-long",
                                "sub-long",  "mul-long",  "div-long",  "rem-long",   "and-long",   "or-long",
                                "xor-long",  "shl-long",  "shr-long",  "ushr-long",  "add-float",  "sub-float",
                                "mul-float", "div-float", "rem-float", "add-double", "sub-double", "mul-double",
                                "div-double", "rem-double"};
  static const char* binop2[] = {
      "add-int/2addr",    "sub-int/2addr",    "mul-int/2addr",    "div-int/2addr",    "rem-int/2addr",
      "and-int/2addr",    "or-int/2addr",     "xor-int/2addr",    "shl-int/2addr",    "shr-int/2addr",
      "ushr-int/2addr",   "add-long/2addr",   "sub-long/2addr",   "mul-long/2addr",   "div-long/2addr",
      "rem-long/2addr",   "and-long/2addr",   "or-long/2addr",    "xor-long/2addr",   "shl-long/2addr",
      "shr-long/2addr",   "ushr-long/2addr",  "add-float/2addr",  "sub-float/2addr",  "mul-float/2addr",
      "div-float/2addr",  "rem-float/2addr",  "add-double/2addr", "sub-double/2addr", "mul-double/2addr",
      "div-double/2addr", "rem-double/2addr"};
  for (int i = 0; i < 32; ++i) {
    set(0x90 + i, binop[i], F::k23x);
    set(0xb0 + i, binop2[i], F::k12x);
  }
  const char* lit16[] = {"add-int/lit16", "rsub-int",     "mul-int/lit16", "div-int/lit16",
                         "rem-int/lit16", "and-int/lit16", "or-int/lit16",  "xor-int/lit16"};
  for (int i = 0; i < 8; ++i) set(0xd0 + i, lit16[i], F::k22s);
  const char* lit8[] = {"add-int/lit8", "rsub-int/lit8", "mul-int/lit8", "div-int/lit8", "rem-int/lit8", "and-int/lit8",
                        "or-int/lit8",  "xor-int/lit8",  "shl-int/lit8", "shr-int/lit8", "ushr-int/lit8"};
  for (int i = 0; i < 11; ++i) set(0xd8 + i, lit8[i], F::k22b);
  set(0xfa, "invoke-polymorphic", F::k45cc, I::kMethodProto);
  set(0xfb, "invoke-polymorphic/range", F::k4rcc, I::kMethodProto);
  set(0xfc, "invoke-custom", F::k35c, I::kCallSite);
  set(0xfd, "invoke-custom/range", F::k3rc, I::kCallSite);
  set(0xfe, "const-method-handle", F::k21c, I::kMethodHandle);
  set(0xff, "const-method-type", F::k21c, I::kProto);
  return t;
}

const std::array<OpInfo, 256>& table() {
  static const auto t = build_table();
  return t;
}

}  // namespace

const OpInfo& op_info(uint8_t opcode) { return table()[opcode]; }

int find_opcode(std::string_view name) {
  for (int i = 0; i < 256; ++i) {
    if (table()[i].name && name == table()[i].name) return i;
  }
  return -1;
}

uint32_t format_units(Format f) {
  switch (f) {
    case F::k10x: case F::k12x: case F::k11n: case F::k11x: case F::k10t: return 1;
    case F::k20t: case F::k22x: case F::k21t: case F::k21s: case F::k21h: case F::k21c:
    case F::k23x: case F::k22b: case F::k22t: case F::k22s: case F::k22c: return 2;
    case F::k30t: case F::k32x: case F::k31i: case F::k31t: case F::k31c: case F::k35c: case F::k3rc: return 3;
    case F::k45cc: case F::k4rcc: return 4;
    case F::k51l: return 5;
    case F::kUnused: return 1;
  }
  return 1;
}

uint32_t payload_units(std::span<const uint16_t> insns, uint32_t pc) {
  uint16_t ident = insns[pc];
  uint64_t units = 0;
  auto need = [&](uint64_t n) {
    if (pc + n > insns.size()) throw_format("données de switch/tableau tronquées à " + hex(pc));
  };
  if (ident == kPackedSwitchPayload) {
    need(2);
    units = 4 + uint64_t(insns[pc + 1]) * 2;
  } else if (ident == kSparseSwitchPayload) {
    need(2);
    units = 2 + uint64_t(insns[pc + 1]) * 4;
  } else if (ident == kFillArrayDataPayload) {
    need(4);
    uint64_t width = insns[pc + 1];
    uint64_t count = insns[pc + 2] | (uint64_t(insns[pc + 3]) << 16);
    units = 4 + (width * count + 1) / 2;
  } else {
    return 0;
  }
  need(units);
  return uint32_t(units);
}

Decoded decode(std::span<const uint16_t> insns, uint32_t pc) {
  if (pc >= insns.size()) throw_format("instruction hors du code à " + hex(pc));
  Decoded d;
  uint16_t u0 = insns[pc];
  d.op = uint8_t(u0 & 0xff);
  const OpInfo& info = op_info(d.op);
  if (info.format == F::kUnused) throw_format("opcode inutilisé " + hex(d.op) + " à " + hex(pc));
  d.format = info.format;
  d.units = format_units(d.format);
  if (uint64_t(pc) + d.units > insns.size()) throw_format("instruction tronquée à " + hex(pc));
  auto u = [&](uint32_t i) { return uint32_t(insns[pc + i]); };
  uint32_t hi = u0 >> 8;
  switch (d.format) {
    case F::k10x: break;
    case F::k12x: d.a = hi & 0xf; d.b = u0 >> 12; break;
    case F::k11n: d.a = hi & 0xf; d.literal = int16_t(u0) >> 12; break;
    case F::k11x: d.a = hi; break;
    case F::k10t: d.literal = int8_t(hi); break;
    case F::k20t: d.literal = int16_t(u(1)); break;
    case F::k22x: d.a = hi; d.b = u(1); break;
    case F::k21t:
    case F::k21s:
    case F::k21h: d.a = hi; d.literal = int16_t(u(1)); break;
    case F::k21c: d.a = hi; d.index = u(1); break;
    case F::k23x: d.a = hi; d.b = u(1) & 0xff; d.c = u(1) >> 8; break;
    case F::k22b: d.a = hi; d.b = u(1) & 0xff; d.literal = int8_t(u(1) >> 8); break;
    case F::k22t:
    case F::k22s: d.a = hi & 0xf; d.b = u0 >> 12; d.literal = int16_t(u(1)); break;
    case F::k22c: d.a = hi & 0xf; d.b = u0 >> 12; d.index = u(1); break;
    case F::k30t: d.literal = int32_t(u(1) | (u(2) << 16)); break;
    case F::k32x: d.a = u(1); d.b = u(2); break;
    case F::k31i:
    case F::k31t: d.a = hi; d.literal = int32_t(u(1) | (u(2) << 16)); break;
    case F::k31c: d.a = hi; d.index = u(1) | (u(2) << 16); break;
    case F::k35c:
    case F::k45cc: {
      d.arg_count = u0 >> 12;
      if (d.arg_count > 5) throw_format("plus de 5 arguments à " + hex(pc));
      uint32_t regs = u(2);
      d.args[0] = regs & 0xf;
      d.args[1] = (regs >> 4) & 0xf;
      d.args[2] = (regs >> 8) & 0xf;
      d.args[3] = regs >> 12;
      d.args[4] = hi & 0xf;
      d.index = u(1);
      if (d.format == F::k45cc) d.index2 = u(3);
      break;
    }
    case F::k3rc:
    case F::k4rcc:
      d.arg_count = hi;
      d.index = u(1);
      d.c = u(2);
      if (d.format == F::k4rcc) d.index2 = u(3);
      break;
    case F::k51l:
      d.a = hi;
      d.literal = int64_t(uint64_t(u(1)) | (uint64_t(u(2)) << 16) | (uint64_t(u(3)) << 32) | (uint64_t(u(4)) << 48));
      break;
    case F::kUnused: break;
  }
  return d;
}

namespace {

std::string escape_string(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c);
    }
  }
  return out + "\"";
}

std::string index_text(const DexFile* dex, IndexKind kind, uint32_t index) {
  try {
    if (dex) {
      switch (kind) {
        case I::kString: return escape_string(text::printable(dex->string(index), 120));
        case I::kType: return dex->type_descriptor(index);
        case I::kField: return dex->pretty_field(index);
        case I::kMethod:
        case I::kMethodProto: return dex->pretty_method(index);
        case I::kProto: return dex->proto_signature(index);
        default: break;
      }
    }
  } catch (const FormatError&) {
    return "<index invalide " + std::to_string(index) + ">";
  }
  return "@" + std::to_string(index);
}

std::string target(uint32_t pc, int64_t offset) {
  char buf[32];
  std::snprintf(buf, sizeof buf, ":%04llx", static_cast<unsigned long long>(pc + offset));
  return buf;
}

}  // namespace

std::string disassemble(const DexFile* dex, const Decoded& d, uint32_t pc) {
  const OpInfo& info = op_info(d.op);
  std::string s = info.name;
  auto v = [](uint32_t r) { return "v" + std::to_string(r); };
  auto lit = [](int64_t x) { return x < 0 ? "-" + hex(uint64_t(-x)) : hex(uint64_t(x)); };
  switch (d.format) {
    case F::k10x: break;
    case F::k12x: s += " " + v(d.a) + ", " + v(d.b); break;
    case F::k11n: s += " " + v(d.a) + ", " + lit(d.literal); break;
    case F::k11x: s += " " + v(d.a); break;
    case F::k10t:
    case F::k20t:
    case F::k30t: s += " " + target(pc, d.literal); break;
    case F::k22x:
    case F::k32x: s += " " + v(d.a) + ", " + v(d.b); break;
    case F::k21t:
    case F::k31t: s += " " + v(d.a) + ", " + target(pc, d.literal); break;
    case F::k21s:
    case F::k31i:
    case F::k51l: s += " " + v(d.a) + ", " + lit(d.literal); break;
    case F::k21h:
      s += " " + v(d.a) + ", " + lit(d.op == 0x15 ? d.literal * 65536 : d.literal * 281474976710656LL);
      break;
    case F::k21c:
    case F::k31c: s += " " + v(d.a) + ", " + index_text(dex, info.index, d.index); break;
    case F::k23x: s += " " + v(d.a) + ", " + v(d.b) + ", " + v(d.c); break;
    case F::k22b:
    case F::k22s: s += " " + v(d.a) + ", " + v(d.b) + ", " + lit(d.literal); break;
    case F::k22t: s += " " + v(d.a) + ", " + v(d.b) + ", " + target(pc, d.literal); break;
    case F::k22c: s += " " + v(d.a) + ", " + v(d.b) + ", " + index_text(dex, info.index, d.index); break;
    case F::k35c:
    case F::k45cc: {
      s += " {";
      for (uint32_t i = 0; i < d.arg_count; ++i) s += (i ? ", " : "") + v(d.args[i]);
      s += "}, " + index_text(dex, info.index, d.index);
      break;
    }
    case F::k3rc:
    case F::k4rcc:
      s += " {" + (d.arg_count ? v(d.c) + " .. " + v(d.c + d.arg_count - 1) : std::string()) + "}, " +
           index_text(dex, info.index, d.index);
      break;
    case F::kUnused: break;
  }
  return s;
}

}  // namespace aa::dex
