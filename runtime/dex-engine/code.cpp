#include "dex-engine/code.h"

#include <algorithm>
#include <cstring>

#include "common/bytes.h"
#include "dex-engine/opcodes.h"

namespace aa::rt {

using dex::Format;
using dex::IndexKind;

namespace {

bool ends_flow(uint8_t op) {
  return (op >= 0x0e && op <= 0x11) ||  // return*
         op == 0x27 ||                  // throw
         (op >= 0x28 && op <= 0x2a);    // goto*
}

}  // namespace

std::unique_ptr<PreparedCode> prepare_code(const dex::DexFile& dex, const dex::CodeItem& item) {
  auto out = std::make_unique<PreparedCode>();
  out->registers = item.registers_size;
  out->ins = item.ins_size;
  std::span<const uint16_t> insns(item.insns);
  const uint32_t n = uint32_t(insns.size());
  const uint32_t regs = item.registers_size;
  if (n == 0) throw_format("méthode sans instructions");

  std::vector<int32_t> index_of(n, -1);
  std::vector<uint8_t> payload_at(n, 0);  // payload ident high byte at each payload start
  std::vector<dex::Decoded> decoded;
  std::vector<uint32_t> addresses;
  for (uint32_t pc = 0; pc < n;) {
    uint32_t pu = dex::payload_units(insns, pc);
    if (pu) {
      payload_at[pc] = uint8_t(insns[pc] >> 8);
      pc += pu;
      continue;
    }
    dex::Decoded d = dex::decode(insns, pc);
    index_of[pc] = int32_t(decoded.size());
    decoded.push_back(d);
    addresses.push_back(pc);
    pc += d.units;
  }
  // Trailing nops (payload alignment padding) are unreachable when the last
  // real instruction ends the flow; a branch to them is caught at run time.
  auto last = std::find_if(decoded.rbegin(), decoded.rend(), [](const dex::Decoded& d) { return d.op != 0x00; });
  if (last == decoded.rend() || !ends_flow(last->op)) {
    throw_format("l'exécution peut dépasser la fin de la méthode");
  }

  auto check_reg = [&](uint32_t r, uint32_t pc) {
    if (r >= regs) throw_format("registre v" + std::to_string(r) + " hors limites à " + hex(pc));
  };
  auto branch = [&](uint32_t pc, int64_t offset) {
    int64_t t = int64_t(pc) + offset;
    if (t < 0 || t >= int64_t(n) || index_of[size_t(t)] < 0) throw_format("cible de branchement invalide à " + hex(pc));
    return index_of[size_t(t)];
  };

  out->insns.resize(decoded.size());
  {
    for (size_t k = 0; k < decoded.size(); ++k) {
      const dex::Decoded& d = decoded[k];
      const uint32_t pc = addresses[k];
      Insn& in = out->insns[k];
      in.op = d.op;
      in.a = d.a;
      in.b = d.b;
      in.c = d.c;
      in.lit = d.literal;
      in.index = d.index;
      in.index2 = d.index2;
      in.argc = uint8_t(d.arg_count);
      std::memcpy(in.args, d.args, sizeof in.args);
      in.pc = pc;

      switch (d.format) {
        case Format::k10x:
          break;
        case Format::k12x:
        case Format::k22t:
        case Format::k22s:
        case Format::k22c:
        case Format::k22x:
        case Format::k32x:
        case Format::k22b:
          check_reg(d.a, pc);
          check_reg(d.b, pc);
          break;
        case Format::k23x:
          check_reg(d.a, pc);
          check_reg(d.b, pc);
          check_reg(d.c, pc);
          break;
        case Format::k11n:
        case Format::k11x:
        case Format::k21t:
        case Format::k21s:
        case Format::k21h:
        case Format::k21c:
        case Format::k31i:
        case Format::k31t:
        case Format::k31c:
        case Format::k51l:
          check_reg(d.a, pc);
          break;
        case Format::k35c:
        case Format::k45cc:
          for (uint32_t i = 0; i < d.arg_count; ++i) check_reg(d.args[i], pc);
          break;
        case Format::k3rc:
        case Format::k4rcc:
          if (uint64_t(d.c) + d.arg_count > regs) throw_format("plage de registres hors limites à " + hex(pc));
          break;
        default:
          break;
      }

      auto check_index = [&](uint64_t limit, const char* what) {
        if (d.index >= limit) throw_format(std::string("index de ") + what + " invalide à " + hex(pc));
      };
      switch (dex::op_info(d.op).index) {
        case IndexKind::kString: check_index(dex.num_strings(), "chaîne"); break;
        case IndexKind::kType: check_index(dex.num_types(), "type"); break;
        case IndexKind::kField: check_index(dex.num_fields(), "champ"); break;
        case IndexKind::kMethod: check_index(dex.num_methods(), "méthode"); break;
        case IndexKind::kMethodProto:
          check_index(dex.num_methods(), "méthode");
          if (d.index2 >= dex.num_protos()) throw_format("index de prototype invalide à " + hex(pc));
          break;
        case IndexKind::kProto: check_index(dex.num_protos(), "prototype"); break;
        default: break;
      }

      switch (d.format) {
        case Format::k10t:
        case Format::k20t:
        case Format::k30t:
        case Format::k21t:
        case Format::k22t:
          in.target = branch(pc, d.literal);
          break;
        case Format::k31t: {
          int64_t ppc = int64_t(pc) + d.literal;
          if (ppc < 0 || ppc >= int64_t(n) || (ppc & 1)) throw_format("données de switch/tableau invalides à " + hex(pc));
          uint32_t p = uint32_t(ppc);
          uint8_t expected = d.op == 0x2b ? 1 : d.op == 0x2c ? 2 : 3;
          if (payload_at[p] != expected) throw_format("données de switch/tableau introuvables à " + hex(pc));
          if (d.op == 0x26) {
            ArrayPayload ap;
            ap.width = insns[p + 1];
            ap.count = insns[p + 2] | (uint32_t(insns[p + 3]) << 16);
            if (ap.width != 1 && ap.width != 2 && ap.width != 4 && ap.width != 8) throw_format("largeur de tableau invalide");
            size_t bytes = size_t(ap.width) * ap.count;
            ap.data.resize(bytes);
            std::memcpy(ap.data.data(), &insns[p + 4], bytes);
            in.target = int32_t(out->arrays.size());
            out->arrays.push_back(std::move(ap));
          } else {
            SwitchTable st;
            st.packed = d.op == 0x2b;
            uint32_t size = insns[p + 1];
            auto u32 = [&](uint32_t at) { return uint32_t(insns[at]) | (uint32_t(insns[at + 1]) << 16); };
            if (st.packed) {
              st.first_key = int32_t(u32(p + 2));
              for (uint32_t i = 0; i < size; ++i) st.targets.push_back(branch(pc, int32_t(u32(p + 4 + 2 * i))));
            } else {
              for (uint32_t i = 0; i < size; ++i) {
                st.keys.push_back(int32_t(u32(p + 2 + 2 * i)));
                st.targets.push_back(branch(pc, int32_t(u32(p + 2 + 2 * size + 2 * i))));
              }
              if (!std::is_sorted(st.keys.begin(), st.keys.end())) throw_format("clés de sparse-switch non triées");
            }
            in.target = int32_t(out->switches.size());
            out->switches.push_back(std::move(st));
          }
          break;
        }
        default:
          break;
      }
    }
  }

  for (const dex::TryItem& t : item.tries) {
    TryBlock tb;
    tb.start = t.start_addr;
    tb.end = t.start_addr + t.insn_count;
    const dex::CatchHandler& h = item.handlers.at(t.handler);
    auto handler_target = [&](uint32_t addr) {
      if (addr >= n || index_of[addr] < 0) throw_format("adresse de gestionnaire d'exception invalide " + hex(addr));
      return index_of[addr];
    };
    for (const auto& [type_idx, addr] : h.typed) tb.clauses.push_back({type_idx, handler_target(addr)});
    if (h.catch_all) tb.clauses.push_back({dex::kNoIndex, handler_target(*h.catch_all)});
    out->tries.push_back(std::move(tb));
  }
  return out;
}

}  // namespace aa::rt
