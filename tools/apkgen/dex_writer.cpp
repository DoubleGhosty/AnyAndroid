#include "dex_writer.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "common/bytes.h"
#include "common/hash.h"
#include "common/text.h"
#include "dex-engine/dex_file.h"

namespace aa::tool {

namespace {

using FieldKey = std::tuple<std::string, std::string, std::string>;   // class, name, type
using MethodKey = std::tuple<std::string, std::string, std::string>;  // class, name, signature

FieldKey parse_field_ref(const std::string& ref) {
  size_t arrow = ref.find("->");
  size_t colon = ref.find(':', arrow == std::string::npos ? 0 : arrow);
  if (arrow == std::string::npos || colon == std::string::npos) throw std::runtime_error("référence de champ invalide : " + ref);
  return {ref.substr(0, arrow), ref.substr(arrow + 2, colon - arrow - 2), ref.substr(colon + 1)};
}

MethodKey parse_method_ref(const std::string& ref) {
  size_t arrow = ref.find("->");
  size_t paren = ref.find('(', arrow == std::string::npos ? 0 : arrow);
  if (arrow == std::string::npos || paren == std::string::npos) throw std::runtime_error("référence de méthode invalide : " + ref);
  return {ref.substr(0, arrow), ref.substr(arrow + 2, paren - arrow - 2), ref.substr(paren)};
}

std::string shorty_of(const std::string& sig) {
  std::vector<std::string> params;
  std::string ret;
  if (!dex::split_signature(sig, params, ret)) throw std::runtime_error("signature invalide : " + sig);
  auto c = [](const std::string& t) { return (t[0] == 'L' || t[0] == '[') ? 'L' : t[0]; };
  std::string s(1, c(ret));
  for (const auto& p : params) s.push_back(c(p));
  return s;
}

bool utf16_less(const std::string& a, const std::string& b) { return text::utf8_to_utf16(a) < text::utf8_to_utf16(b); }

void write_mutf8(ByteWriter& w, const std::string& s) {
  std::u16string u = text::utf8_to_utf16(s);
  w.uleb128(uint32_t(u.size()));
  for (char16_t c : u) {
    if (c != 0 && c < 0x80) {
      w.u8(uint8_t(c));
    } else if (c < 0x800) {
      w.u8(uint8_t(0xC0 | (c >> 6)));
      w.u8(uint8_t(0x80 | (c & 0x3F)));
    } else {
      w.u8(uint8_t(0xE0 | (c >> 12)));
      w.u8(uint8_t(0x80 | ((c >> 6) & 0x3F)));
      w.u8(uint8_t(0x80 | (c & 0x3F)));
    }
  }
  w.u8(0);
}

void enc_signed(ByteWriter& w, uint8_t type, int64_t v, int max_bytes) {
  int n = 1;
  for (; n < max_bytes; ++n) {
    int64_t lo = int64_t(uint64_t(v) << (64 - 8 * n)) >> (64 - 8 * n);
    if (lo == v) break;
  }
  w.u8(uint8_t(((n - 1) << 5) | type));
  for (int i = 0; i < n; ++i) w.u8(uint8_t(uint64_t(v) >> (8 * i)));
}

void enc_unsigned(ByteWriter& w, uint8_t type, uint64_t v) {
  int n = 1;
  while (n < 8 && (v >> (8 * n)) != 0) ++n;
  w.u8(uint8_t(((n - 1) << 5) | type));
  for (int i = 0; i < n; ++i) w.u8(uint8_t(v >> (8 * i)));
}

void enc_float(ByteWriter& w, uint8_t type, uint64_t bits, int width) {
  int k = 0;
  while (k < width - 1 && ((bits >> (8 * k)) & 0xFF) == 0) ++k;
  int n = width - k;
  w.u8(uint8_t(((n - 1) << 5) | type));
  for (int i = 0; i < n; ++i) w.u8(uint8_t(bits >> (8 * (k + i))));
}

}  // namespace

std::vector<uint8_t> write_dex(const std::vector<ClassDef>& classes) {
  // ---- Collect every referenced item ------------------------------------
  std::set<std::string> string_set, type_set, proto_set;
  std::set<FieldKey> field_set;
  std::set<MethodKey> method_set;
  auto add_type = [&](const std::string& t) {
    type_set.insert(t);
    string_set.insert(t);
  };
  auto add_proto = [&](const std::string& sig) {
    std::vector<std::string> params;
    std::string ret;
    if (!dex::split_signature(sig, params, ret)) throw std::runtime_error("signature invalide : " + sig);
    proto_set.insert(sig);
    string_set.insert(shorty_of(sig));
    add_type(ret);
    for (const auto& p : params) add_type(p);
  };
  auto add_field = [&](const FieldKey& f) {
    add_type(std::get<0>(f));
    string_set.insert(std::get<1>(f));
    add_type(std::get<2>(f));
    field_set.insert(f);
  };
  auto add_method = [&](const MethodKey& m) {
    add_type(std::get<0>(m));
    string_set.insert(std::get<1>(m));
    add_proto(std::get<2>(m));
    method_set.insert(m);
  };

  std::map<std::string, const ClassDef*> defined;
  for (const ClassDef& c : classes) {
    if (!defined.emplace(c.descriptor, &c).second) throw std::runtime_error("classe définie deux fois : " + c.descriptor);
    add_type(c.descriptor);
    if (!c.super.empty()) add_type(c.super);
    for (const auto& i : c.interfaces) add_type(i);
    if (!c.source_file.empty()) string_set.insert(c.source_file);
    for (const auto* list : {&c.static_fields, &c.instance_fields}) {
      for (const FieldDef& f : *list) {
        add_field({c.descriptor, f.name, f.type});
        if (f.initial.kind == StaticValue::Kind::kString) string_set.insert(f.initial.str);
      }
    }
    for (const auto* list : {&c.direct_methods, &c.virtual_methods}) {
      for (const MethodDef& m : *list) {
        add_method({c.descriptor, m.name, m.signature});
        for (const Fixup& fx : m.fixups) {
          switch (fx.kind) {
            case dex::IndexKind::kString: string_set.insert(fx.ref); break;
            case dex::IndexKind::kType: add_type(fx.ref); break;
            case dex::IndexKind::kField: add_field(parse_field_ref(fx.ref)); break;
            case dex::IndexKind::kMethod: add_method(parse_method_ref(fx.ref)); break;
            default: throw std::runtime_error("type de référence non pris en charge par apkgen");
          }
        }
        for (const TryDef& t : m.tries) {
          for (const auto& [type, addr] : t.catches) add_type(type);
        }
      }
    }
  }

  // ---- Sorted index assignment (required by the DEX format) ---------------
  std::vector<std::string> strings(string_set.begin(), string_set.end());
  std::sort(strings.begin(), strings.end(), utf16_less);
  std::map<std::string, uint32_t> string_idx;
  for (uint32_t i = 0; i < strings.size(); ++i) string_idx[strings[i]] = i;

  std::vector<std::string> types(type_set.begin(), type_set.end());
  std::sort(types.begin(), types.end(), [&](const auto& a, const auto& b) { return string_idx[a] < string_idx[b]; });
  std::map<std::string, uint32_t> type_idx;
  for (uint32_t i = 0; i < types.size(); ++i) type_idx[types[i]] = i;
  if (types.size() > 65535) throw std::runtime_error("trop de types");

  struct Proto {
    std::string sig;
    uint32_t ret;
    std::vector<uint16_t> params;
  };
  std::vector<Proto> protos;
  for (const auto& sig : proto_set) {
    std::vector<std::string> params;
    std::string ret;
    dex::split_signature(sig, params, ret);
    Proto p{sig, type_idx[ret], {}};
    for (const auto& t : params) p.params.push_back(uint16_t(type_idx[t]));
    protos.push_back(std::move(p));
  }
  std::sort(protos.begin(), protos.end(), [](const Proto& a, const Proto& b) {
    return std::tie(a.ret, a.params) < std::tie(b.ret, b.params);
  });
  std::map<std::string, uint32_t> proto_idx;
  for (uint32_t i = 0; i < protos.size(); ++i) proto_idx[protos[i].sig] = i;

  std::vector<FieldKey> fields(field_set.begin(), field_set.end());
  std::sort(fields.begin(), fields.end(), [&](const FieldKey& a, const FieldKey& b) {
    return std::make_tuple(type_idx[std::get<0>(a)], string_idx[std::get<1>(a)], type_idx[std::get<2>(a)]) <
           std::make_tuple(type_idx[std::get<0>(b)], string_idx[std::get<1>(b)], type_idx[std::get<2>(b)]);
  });
  std::map<FieldKey, uint32_t> field_idx;
  for (uint32_t i = 0; i < fields.size(); ++i) field_idx[fields[i]] = i;

  std::vector<MethodKey> methods(method_set.begin(), method_set.end());
  std::sort(methods.begin(), methods.end(), [&](const MethodKey& a, const MethodKey& b) {
    return std::make_tuple(type_idx[std::get<0>(a)], string_idx[std::get<1>(a)], proto_idx[std::get<2>(a)]) <
           std::make_tuple(type_idx[std::get<0>(b)], string_idx[std::get<1>(b)], proto_idx[std::get<2>(b)]);
  });
  std::map<MethodKey, uint32_t> method_idx;
  for (uint32_t i = 0; i < methods.size(); ++i) method_idx[methods[i]] = i;

  // Superclasses and interfaces defined in this file must come first.
  std::vector<const ClassDef*> order;
  std::set<std::string> visited;
  std::function<void(const std::string&, int)> visit = [&](const std::string& d, int depth) {
    auto it = defined.find(d);
    if (it == defined.end() || visited.count(d)) return;
    if (depth > 1000) throw std::runtime_error("hiérarchie de classes circulaire autour de " + d);
    visited.insert(d);
    if (!it->second->super.empty()) visit(it->second->super, depth + 1);
    for (const auto& i : it->second->interfaces) visit(i, depth + 1);
    order.push_back(it->second);
  };
  for (const ClassDef& c : classes) visit(c.descriptor, 0);

  // ---- Layout ---------------------------------------------------------------
  const uint32_t string_ids_off = 0x70;
  const uint32_t type_ids_off = string_ids_off + 4 * uint32_t(strings.size());
  const uint32_t proto_ids_off = type_ids_off + 4 * uint32_t(types.size());
  const uint32_t field_ids_off = proto_ids_off + 12 * uint32_t(protos.size());
  const uint32_t method_ids_off = field_ids_off + 8 * uint32_t(fields.size());
  const uint32_t class_defs_off = method_ids_off + 8 * uint32_t(methods.size());
  const uint32_t data_off = class_defs_off + 32 * uint32_t(order.size());

  ByteWriter w;
  w.zeros(data_off);
  struct MapItem {
    uint16_t type;
    uint32_t size;
    uint32_t offset;
  };
  std::vector<MapItem> map_items;

  // Debug information (positions only): the encoding of dx and d8, special
  // opcodes when the deltas fit, DBG_ADVANCE_LINE and DBG_ADVANCE_PC otherwise.
  std::map<const MethodDef*, uint32_t> debug_off;
  uint32_t first_debug = 0, debug_count = 0;
  for (const ClassDef* c : order) {
    for (const auto* list : {&c->direct_methods, &c->virtual_methods}) {
      for (const MethodDef& m : *list) {
        if (!m.has_code || m.positions.empty()) continue;
        if (debug_count++ == 0) first_debug = uint32_t(w.size());
        debug_off[&m] = uint32_t(w.size());
        std::vector<std::string> params;
        std::string ret;
        dex::split_signature(m.signature, params, ret);
        uint32_t address = 0, line = m.positions.front().second;
        w.uleb128(line);  // line_start
        w.uleb128(uint32_t(params.size()));
        for (size_t i = 0; i < params.size(); ++i) w.uleb128(0);  // no parameter name (uleb128p1 of NO_INDEX)
        for (const auto& [addr, ln] : m.positions) {
          if (addr < address) throw std::runtime_error("positions non croissantes : " + m.name);
          uint32_t address_delta = addr - address;
          int64_t line_delta = int64_t(ln) - int64_t(line);
          if (line_delta < -4 || line_delta > 10) {  // DBG_LINE_BASE -4, DBG_LINE_RANGE 15
            w.u8(0x02);  // DBG_ADVANCE_LINE
            w.sleb128(int32_t(line_delta));
            line_delta = 0;
          }
          if ((line_delta + 4) + 15 * uint64_t(address_delta) + 0x0a > 0xff) {
            w.u8(0x01);  // DBG_ADVANCE_PC
            w.uleb128(address_delta);
            address_delta = 0;
          }
          w.u8(uint8_t((line_delta + 4) + 15 * address_delta + 0x0a));  // DBG_FIRST_SPECIAL 0x0a
          address = addr;
          line = ln;
        }
        w.u8(0x00);  // DBG_END_SEQUENCE
      }
    }
  }
  if (debug_count) map_items.push_back({0x2003, debug_count, first_debug});

  // Code items.
  std::map<const MethodDef*, uint32_t> code_off;
  uint32_t first_code = 0, code_count = 0;
  for (const ClassDef* c : order) {
    for (const auto* list : {&c->direct_methods, &c->virtual_methods}) {
      for (const MethodDef& m : *list) {
        if (!m.has_code) continue;
        w.align(4);
        if (code_count++ == 0) first_code = uint32_t(w.size());
        code_off[&m] = uint32_t(w.size());
        std::vector<uint16_t> insns = m.insns;
        for (const Fixup& fx : m.fixups) {
          uint32_t idx = 0;
          switch (fx.kind) {
            case dex::IndexKind::kString: idx = string_idx.at(fx.ref); break;
            case dex::IndexKind::kType: idx = type_idx.at(fx.ref); break;
            case dex::IndexKind::kField: idx = field_idx.at(parse_field_ref(fx.ref)); break;
            case dex::IndexKind::kMethod: idx = method_idx.at(parse_method_ref(fx.ref)); break;
            default: break;
          }
          if (fx.wide) {
            insns.at(fx.unit) = uint16_t(idx);
            insns.at(fx.unit + 1) = uint16_t(idx >> 16);
          } else {
            if (idx > 0xFFFF) throw std::runtime_error("index trop grand pour une instruction 16 bits : " + fx.ref);
            insns.at(fx.unit) = uint16_t(idx);
          }
        }
        w.u16(m.registers);
        w.u16(m.ins);
        w.u16(m.outs);
        w.u16(uint16_t(m.tries.size()));
        w.u32(debug_off.count(&m) ? debug_off.at(&m) : 0);  // debug_info_off
        w.u32(uint32_t(insns.size()));
        for (uint16_t u : insns) w.u16(u);
        if (!m.tries.empty()) {
          if (insns.size() % 2) w.u16(0);
          // Deduplicated encoded_catch_handler_list.
          std::vector<std::vector<uint8_t>> handlers;
          std::vector<uint32_t> handler_of_try;
          for (const TryDef& t : m.tries) {
            ByteWriter h;
            int32_t n = int32_t(t.catches.size());
            h.sleb128(t.catch_all ? -n : n);
            for (const auto& [type, addr] : t.catches) {
              h.uleb128(type_idx.at(type));
              h.uleb128(addr);
            }
            if (t.catch_all) h.uleb128(*t.catch_all);
            auto it = std::find(handlers.begin(), handlers.end(), h.buffer());
            handler_of_try.push_back(uint32_t(it - handlers.begin()));
            if (it == handlers.end()) handlers.push_back(h.take());
          }
          ByteWriter handler_list;
          handler_list.uleb128(uint32_t(handlers.size()));
          std::vector<uint32_t> offsets;
          for (const auto& h : handlers) {
            offsets.push_back(uint32_t(handler_list.size()));
            handler_list.bytes(h);
          }
          for (size_t i = 0; i < m.tries.size(); ++i) {
            w.u32(m.tries[i].start);
            w.u16(uint16_t(m.tries[i].count));
            w.u16(uint16_t(offsets[handler_of_try[i]]));
          }
          w.bytes(handler_list.buffer());
        }
      }
    }
  }
  if (code_count) map_items.push_back({0x2001, code_count, first_code});

  // Type lists (prototype parameters, interfaces), deduplicated.
  std::map<std::vector<uint16_t>, uint32_t> type_lists;
  uint32_t first_type_list = 0;
  auto type_list = [&](const std::vector<uint16_t>& items) -> uint32_t {
    if (items.empty()) return 0;
    auto it = type_lists.find(items);
    if (it != type_lists.end()) return it->second;
    w.align(4);
    uint32_t off = uint32_t(w.size());
    if (type_lists.empty()) first_type_list = off;
    w.u32(uint32_t(items.size()));
    for (uint16_t t : items) w.u16(t);
    type_lists.emplace(items, off);
    return off;
  };
  std::vector<uint32_t> proto_params_off;
  for (const Proto& p : protos) proto_params_off.push_back(type_list(p.params));
  std::map<const ClassDef*, uint32_t> interfaces_off;
  for (const ClassDef* c : order) {
    std::vector<uint16_t> items;
    for (const auto& i : c->interfaces) items.push_back(uint16_t(type_idx.at(i)));
    interfaces_off[c] = type_list(items);
  }
  if (!type_lists.empty()) map_items.push_back({0x1001, uint32_t(type_lists.size()), first_type_list});

  // String data.
  std::vector<uint32_t> string_data_off;
  for (const auto& s : strings) {
    string_data_off.push_back(uint32_t(w.size()));
    write_mutf8(w, s);
  }
  if (!strings.empty()) map_items.push_back({0x2002, uint32_t(strings.size()), string_data_off[0]});

  // Class data and static values.
  std::map<const ClassDef*, uint32_t> class_data_off, static_values_off;
  uint32_t class_data_count = 0, first_class_data = 0;
  for (const ClassDef* c : order) {
    if (c->static_fields.empty() && c->instance_fields.empty() && c->direct_methods.empty() && c->virtual_methods.empty()) {
      continue;
    }
    uint32_t off = uint32_t(w.size());
    if (class_data_count++ == 0) first_class_data = off;
    class_data_off[c] = off;
    w.uleb128(uint32_t(c->static_fields.size()));
    w.uleb128(uint32_t(c->instance_fields.size()));
    w.uleb128(uint32_t(c->direct_methods.size()));
    w.uleb128(uint32_t(c->virtual_methods.size()));
    auto write_fields = [&](const std::vector<FieldDef>& list) {
      std::vector<std::pair<uint32_t, uint32_t>> sorted;
      for (const FieldDef& f : list) sorted.emplace_back(field_idx.at({c->descriptor, f.name, f.type}), f.access);
      std::sort(sorted.begin(), sorted.end());
      uint32_t prev = 0;
      for (size_t i = 0; i < sorted.size(); ++i) {
        w.uleb128(i == 0 ? sorted[i].first : sorted[i].first - prev);
        w.uleb128(sorted[i].second);
        prev = sorted[i].first;
      }
    };
    auto write_methods = [&](const std::vector<MethodDef>& list) {
      std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> sorted;
      for (const MethodDef& m : list) {
        auto it = code_off.find(&m);
        sorted.emplace_back(method_idx.at({c->descriptor, m.name, m.signature}), m.access,
                            it == code_off.end() ? 0 : it->second);
      }
      std::sort(sorted.begin(), sorted.end());
      uint32_t prev = 0;
      for (size_t i = 0; i < sorted.size(); ++i) {
        auto [idx, access, off2] = sorted[i];
        if (i > 0 && idx == prev) throw std::runtime_error("méthode définie deux fois dans " + c->descriptor);
        w.uleb128(i == 0 ? idx : idx - prev);
        w.uleb128(access);
        w.uleb128(off2);
        prev = idx;
      }
    };
    write_fields(c->static_fields);
    write_fields(c->instance_fields);
    write_methods(c->direct_methods);
    write_methods(c->virtual_methods);
  }
  if (class_data_count) map_items.push_back({0x2000, class_data_count, first_class_data});

  uint32_t encoded_count = 0, first_encoded = 0;
  for (const ClassDef* c : order) {
    std::vector<std::pair<uint32_t, const FieldDef*>> sorted;
    for (const FieldDef& f : c->static_fields) sorted.emplace_back(field_idx.at({c->descriptor, f.name, f.type}), &f);
    std::sort(sorted.begin(), sorted.end());
    int last = -1;
    for (size_t i = 0; i < sorted.size(); ++i) {
      if (sorted[i].second->initial.kind != StaticValue::Kind::kNone) last = int(i);
    }
    if (last < 0) continue;
    uint32_t off = uint32_t(w.size());
    if (encoded_count++ == 0) first_encoded = off;
    static_values_off[c] = off;
    w.uleb128(uint32_t(last + 1));
    for (int i = 0; i <= last; ++i) {
      const FieldDef& f = *sorted[size_t(i)].second;
      const StaticValue& v = f.initial;
      using K = StaticValue::Kind;
      char t = f.type[0];
      if (v.kind == K::kNone || v.kind == K::kNull) {
        switch (t) {
          case 'Z': w.u8(0x1f); break;
          case 'B': enc_signed(w, 0x00, 0, 1); break;
          case 'S': enc_signed(w, 0x02, 0, 2); break;
          case 'C': enc_unsigned(w, 0x03, 0); break;
          case 'I': enc_signed(w, 0x04, 0, 4); break;
          case 'J': enc_signed(w, 0x06, 0, 8); break;
          case 'F': enc_float(w, 0x10, 0, 4); break;
          case 'D': enc_float(w, 0x11, 0, 8); break;
          default: w.u8(0x1e);
        }
        continue;
      }
      switch (t) {
        case 'Z': w.u8(uint8_t(((v.bits ? 1 : 0) << 5) | 0x1f)); break;
        case 'B': enc_signed(w, 0x00, int8_t(v.bits), 1); break;
        case 'S': enc_signed(w, 0x02, int16_t(v.bits), 2); break;
        case 'C': enc_unsigned(w, 0x03, uint16_t(v.bits)); break;
        case 'I': enc_signed(w, 0x04, int32_t(v.bits), 4); break;
        case 'J': enc_signed(w, 0x06, int64_t(v.bits), 8); break;
        case 'F': enc_float(w, 0x10, uint32_t(v.bits), 4); break;
        case 'D': enc_float(w, 0x11, v.bits, 8); break;
        default:
          if (v.kind != K::kString) throw std::runtime_error("valeur initiale incompatible pour " + c->descriptor + "->" + f.name);
          enc_unsigned(w, 0x17, string_idx.at(v.str));
      }
    }
  }
  if (encoded_count) map_items.push_back({0x2005, encoded_count, first_encoded});

  // Map list.
  w.align(4);
  uint32_t map_off = uint32_t(w.size());
  std::vector<MapItem> all = {{0x0000, 1, 0}};
  auto add_section = [&](uint16_t type, size_t count, uint32_t off) {
    if (count) all.push_back({type, uint32_t(count), off});
  };
  add_section(0x0001, strings.size(), string_ids_off);
  add_section(0x0002, types.size(), type_ids_off);
  add_section(0x0003, protos.size(), proto_ids_off);
  add_section(0x0004, fields.size(), field_ids_off);
  add_section(0x0005, methods.size(), method_ids_off);
  add_section(0x0006, order.size(), class_defs_off);
  all.insert(all.end(), map_items.begin(), map_items.end());
  all.push_back({0x1000, 1, map_off});
  std::sort(all.begin(), all.end(), [](const MapItem& a, const MapItem& b) { return a.offset < b.offset; });
  w.u32(uint32_t(all.size()));
  for (const MapItem& m : all) {
    w.u16(m.type);
    w.u16(0);
    w.u32(m.size);
    w.u32(m.offset);
  }
  const uint32_t file_size = uint32_t(w.size());

  // ---- Id sections and header ----------------------------------------------------
  for (uint32_t i = 0; i < strings.size(); ++i) w.patch_u32(string_ids_off + 4 * i, string_data_off[i]);
  for (uint32_t i = 0; i < types.size(); ++i) w.patch_u32(type_ids_off + 4 * i, string_idx.at(types[i]));
  for (uint32_t i = 0; i < protos.size(); ++i) {
    w.patch_u32(proto_ids_off + 12 * i, string_idx.at(shorty_of(protos[i].sig)));
    w.patch_u32(proto_ids_off + 12 * i + 4, protos[i].ret);
    w.patch_u32(proto_ids_off + 12 * i + 8, proto_params_off[i]);
  }
  for (uint32_t i = 0; i < fields.size(); ++i) {
    w.patch_u16(field_ids_off + 8 * i, uint16_t(type_idx.at(std::get<0>(fields[i]))));
    w.patch_u16(field_ids_off + 8 * i + 2, uint16_t(type_idx.at(std::get<2>(fields[i]))));
    w.patch_u32(field_ids_off + 8 * i + 4, string_idx.at(std::get<1>(fields[i])));
  }
  for (uint32_t i = 0; i < methods.size(); ++i) {
    w.patch_u16(method_ids_off + 8 * i, uint16_t(type_idx.at(std::get<0>(methods[i]))));
    w.patch_u16(method_ids_off + 8 * i + 2, uint16_t(proto_idx.at(std::get<2>(methods[i]))));
    w.patch_u32(method_ids_off + 8 * i + 4, string_idx.at(std::get<1>(methods[i])));
  }
  for (uint32_t i = 0; i < order.size(); ++i) {
    const ClassDef* c = order[i];
    uint32_t base = class_defs_off + 32 * i;
    w.patch_u32(base, type_idx.at(c->descriptor));
    w.patch_u32(base + 4, c->access);
    w.patch_u32(base + 8, c->super.empty() ? dex::kNoIndex : type_idx.at(c->super));
    w.patch_u32(base + 12, interfaces_off[c]);
    w.patch_u32(base + 16, c->source_file.empty() ? dex::kNoIndex : string_idx.at(c->source_file));
    w.patch_u32(base + 20, 0);
    w.patch_u32(base + 24, class_data_off.count(c) ? class_data_off[c] : 0);
    w.patch_u32(base + 28, static_values_off.count(c) ? static_values_off[c] : 0);
  }

  std::vector<uint8_t>& b = w.buffer();
  const char magic[8] = {'d', 'e', 'x', '\n', '0', '3', '5', '\0'};
  std::copy(magic, magic + 8, b.begin());
  auto sect = [&](size_t at, size_t count, uint32_t off) {
    w.patch_u32(at, uint32_t(count));
    w.patch_u32(at + 4, count ? off : 0);
  };
  w.patch_u32(32, file_size);
  w.patch_u32(36, 0x70);
  w.patch_u32(40, 0x12345678);
  w.patch_u32(44, 0);
  w.patch_u32(48, 0);
  w.patch_u32(52, map_off);
  sect(56, strings.size(), string_ids_off);
  sect(64, types.size(), type_ids_off);
  sect(72, protos.size(), proto_ids_off);
  sect(80, fields.size(), field_ids_off);
  sect(88, methods.size(), method_ids_off);
  sect(96, order.size(), class_defs_off);
  w.patch_u32(104, file_size - data_off);
  w.patch_u32(108, data_off);

  auto digest = hash::sha1(std::span<const uint8_t>(b).subspan(32));
  std::copy(digest.begin(), digest.end(), b.begin() + 12);
  w.patch_u32(8, hash::adler32(std::span<const uint8_t>(b).subspan(12)));
  return w.take();
}

}  // namespace aa::tool
