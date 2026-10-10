#include "smali.h"

#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "common/text.h"
#include "dex-engine/dex_file.h"
#include "dex-engine/opcodes.h"

namespace aa::tool {

namespace {

using dex::Format;
using dex::IndexKind;

struct Line {
  int number;
  std::string text;
};

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Removes a '#' comment that is not inside a string or char literal.
std::string strip_comment(const std::string& s) {
  bool in_str = false, in_chr = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if ((in_str || in_chr) && c == '\\') {
      ++i;
      continue;
    }
    if (!in_chr && c == '"') in_str = !in_str;
    else if (!in_str && c == '\'') in_chr = !in_chr;
    else if (!in_str && !in_chr && c == '#') return s.substr(0, i);
  }
  return s;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// Splits operands on commas outside braces and literals.
std::vector<std::string> split_operands(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool in_str = false, in_chr = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if ((in_str || in_chr) && c == '\\' && i + 1 < s.size()) {
      cur.push_back(c);
      cur.push_back(s[++i]);
      continue;
    }
    if (!in_chr && c == '"') in_str = !in_str;
    else if (!in_str && c == '\'') in_chr = !in_chr;
    else if (!in_str && !in_chr && c == '{') ++depth;
    else if (!in_str && !in_chr && c == '}') --depth;
    if (c == ',' && depth == 0 && !in_str && !in_chr) {
      out.push_back(text::trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!text::trim(cur).empty()) out.push_back(text::trim(cur));
  return out;
}

uint32_t access_flag(const std::string& word) {
  static const std::map<std::string, uint32_t> flags = {
      {"public", 0x1},          {"private", 0x2},     {"protected", 0x4},     {"static", 0x8},
      {"final", 0x10},          {"synchronized", 0x20}, {"volatile", 0x40},   {"bridge", 0x40},
      {"transient", 0x80},      {"varargs", 0x80},    {"native", 0x100},      {"interface", 0x200},
      {"abstract", 0x400},      {"strictfp", 0x800},  {"synthetic", 0x1000},  {"annotation", 0x2000},
      {"enum", 0x4000},         {"constructor", 0x10000}, {"declared-synchronized", 0x20000}};
  auto it = flags.find(word);
  return it == flags.end() ? 0 : it->second;
}

std::string parse_string_literal(const std::string& tok) {
  if (tok.size() < 2 || tok.front() != '"' || tok.back() != '"') throw Error("chaîne entre guillemets attendue : " + tok);
  std::u16string out;
  std::string body = tok.substr(1, tok.size() - 2);
  for (size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    if (c != '\\') {
      // Copy one UTF-8 sequence.
      size_t len = (static_cast<unsigned char>(c) < 0x80) ? 1 : (static_cast<unsigned char>(c) >= 0xF0) ? 4
                   : (static_cast<unsigned char>(c) >= 0xE0) ? 3 : 2;
      out += text::utf8_to_utf16(body.substr(i, len));
      i += len - 1;
      continue;
    }
    if (++i >= body.size()) throw Error("échappement incomplet");
    switch (body[i]) {
      case 'n': out.push_back(u'\n'); break;
      case 't': out.push_back(u'\t'); break;
      case 'r': out.push_back(u'\r'); break;
      case 'b': out.push_back(u'\b'); break;
      case 'f': out.push_back(u'\f'); break;
      case '0': out.push_back(u'\0'); break;
      case '"': out.push_back(u'"'); break;
      case '\'': out.push_back(u'\''); break;
      case '\\': out.push_back(u'\\'); break;
      case 'u': {
        if (i + 4 >= body.size()) throw Error("\\u incomplet");
        out.push_back(char16_t(std::stoul(body.substr(i + 1, 4), nullptr, 16)));
        i += 4;
        break;
      }
      default: throw Error(std::string("échappement inconnu \\") + body[i]);
    }
  }
  return text::utf16_to_utf8(out);
}

struct Literal {
  bool is_fp = false;
  bool is_float = false;  // 'f' suffix
  int64_t integer = 0;
  double fp = 0;
};

Literal parse_literal(std::string tok) {
  Literal lit;
  if (tok.empty()) throw Error("littéral vide");
  if (tok.size() >= 3 && tok.front() == '\'' && tok.back() == '\'') {
    std::string inner = parse_string_literal("\"" + tok.substr(1, tok.size() - 2) + "\"");
    std::u16string u = text::utf8_to_utf16(inner);
    if (u.size() != 1) throw Error("caractère invalide : " + tok);
    lit.integer = u[0];
    return lit;
  }
  if (tok == "true" || tok == "false") {
    lit.integer = tok == "true";
    return lit;
  }
  bool neg = tok[0] == '-';
  std::string body = neg || tok[0] == '+' ? tok.substr(1) : tok;
  bool hex = body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X');
  char last = body.back();
  if (!hex) {
    std::string b = body;
    bool f = false, d = false;
    if (last == 'f' || last == 'F') f = true, b.pop_back();
    else if (last == 'd' || last == 'D') d = true, b.pop_back();
    bool looks_fp = f || d || b.find('.') != std::string::npos || b.find('e') != std::string::npos ||
                    b.find('E') != std::string::npos || b == "Infinity" || b == "NaN";
    if (looks_fp) {
      lit.is_fp = true;
      lit.is_float = f;
      if (b == "Infinity") lit.fp = HUGE_VAL;
      else if (b == "NaN") lit.fp = NAN;
      else lit.fp = std::stod(b);
      if (neg) lit.fp = -lit.fp;
      return lit;
    }
  }
  if (last == 'L' || last == 'l' || last == 't' || last == 'T' || last == 's' || last == 'S') body.pop_back();
  size_t used = 0;
  uint64_t magnitude;
  try {
    magnitude = std::stoull(body, &used, hex ? 16 : 10);
  } catch (...) {
    throw Error("littéral invalide : " + tok);
  }
  if (used != body.size()) throw Error("littéral invalide : " + tok);
  lit.integer = neg ? -int64_t(magnitude) : int64_t(magnitude);
  return lit;
}

uint32_t float_bits(double v) {
  float f = float(v);
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

uint64_t double_bits(double v) {
  uint64_t b;
  std::memcpy(&b, &v, 8);
  return b;
}

// 32-bit value of a literal used by a 32-bit instruction.
int64_t literal32(const Literal& l) { return l.is_fp ? int64_t(int32_t(float_bits(l.fp))) : l.integer; }
int64_t literal64(const Literal& l) {
  if (!l.is_fp) return l.integer;
  return l.is_float ? int64_t(float_bits(l.fp)) : int64_t(double_bits(l.fp));
}

struct Item {
  enum class Kind { kLabel, kInsn, kPayload, kPadding } kind;
  int line = 0;
  std::string label;
  // Instruction.
  int opcode = -1;
  std::vector<std::string> operands;
  // Payload.
  enum class PayloadKind { kPacked, kSparse, kArray } payload = PayloadKind::kPacked;
  int32_t first_key = 0;
  std::vector<int32_t> keys;
  std::vector<std::string> targets;
  uint16_t width = 0;
  std::vector<std::string> values;
  uint32_t addr = 0;
};

struct CatchDirective {
  int line;
  std::string type;  // empty for catch-all
  std::string start, end, handler;
};

uint32_t payload_size(const Item& it) {
  switch (it.payload) {
    case Item::PayloadKind::kPacked: return 4 + 2 * uint32_t(it.targets.size());
    case Item::PayloadKind::kSparse: return 2 + 4 * uint32_t(it.targets.size());
    case Item::PayloadKind::kArray: return 4 + (uint32_t(it.width) * uint32_t(it.values.size()) + 1) / 2;
  }
  return 0;
}

class MethodAssembler {
 public:
  MethodAssembler(MethodDef& m, const std::vector<Line>& body, const std::string& file)
      : m_(m), body_(body), file_(file) {}

  void run() {
    bool is_static = m_.access & dex::kAccStatic;
    uint32_t ins = dex::arg_words(m_.signature) + (is_static ? 0 : 1);
    m_.ins = uint16_t(ins);
    parse();
    if (m_.access & (dex::kAccAbstract | dex::kAccNative)) {
      if (!items_.empty()) fail(body_.empty() ? 0 : body_[0].number, "méthode abstraite/native avec du code");
      return;
    }
    if (registers_ < 0) fail(header_line(), ".registers ou .locals manquant");
    if (uint32_t(registers_) < ins) fail(header_line(), "moins de registres que d'arguments");
    m_.registers = uint16_t(registers_);
    m_.has_code = true;
    layout();
    encode();
    build_tries();
  }

 private:
  [[noreturn]] void fail(int line, const std::string& msg) const {
    throw std::runtime_error(file_ + ":" + std::to_string(line) + " : " + msg);
  }
  int header_line() const { return body_.empty() ? 0 : body_.front().number - 1; }

  void parse() {
    for (size_t i = 0; i < body_.size(); ++i) {
      const Line& ln = body_[i];
      const std::string& t = ln.text;
      try {
        if (t[0] == ':') {
          Item it{Item::Kind::kLabel};
          it.line = ln.number;
          it.label = t.substr(1);
          items_.push_back(std::move(it));
        } else if (t[0] == '.') {
          auto words = split_ws(t);
          const std::string& d = words[0];
          if (d == ".registers" || d == ".locals") {
            if (words.size() != 2) throw Error(d + " attend un nombre");
            int n = int(std::stol(words[1], nullptr, 0));
            registers_ = d == ".registers" ? n : n + int(m_.ins);
          } else if (d == ".catch" || d == ".catchall") {
            catches_.push_back(parse_catch(t, ln.number, d == ".catchall"));
          } else if (d == ".packed-switch" || d == ".sparse-switch" || d == ".array-data") {
            i = parse_payload(i);
          } else if (d == ".annotation") {
            while (i < body_.size() && body_[i].text != ".end annotation") ++i;
          } else if (d == ".line" || d == ".prologue" || d == ".epilogue" || d == ".param" || d == ".end" ||
                     d == ".local" || d == ".restart" || d == ".source") {
            // Debug information: ignored.
          } else {
            throw Error("directive inconnue " + d);
          }
        } else {
          auto sp = t.find_first_of(" \t");
          std::string op = sp == std::string::npos ? t : t.substr(0, sp);
          Item it{Item::Kind::kInsn};
          it.line = ln.number;
          it.opcode = dex::find_opcode(op);
          if (it.opcode < 0) throw Error("instruction inconnue " + op);
          if (sp != std::string::npos) it.operands = split_operands(t.substr(sp + 1));
          items_.push_back(std::move(it));
        }
      } catch (const Error& e) {
        fail(ln.number, e.what());
      } catch (const std::invalid_argument&) {
        fail(ln.number, "nombre invalide");
      } catch (const std::out_of_range&) {
        fail(ln.number, "nombre hors limites");
      }
    }
  }

  CatchDirective parse_catch(const std::string& t, int line, bool all) {
    // .catch Ltype; {:start .. :end} :handler    |   .catchall {:start .. :end} :handler
    CatchDirective c{line, "", "", "", ""};
    size_t lb = t.find('{'), rb = t.find('}');
    if (lb == std::string::npos || rb == std::string::npos) throw Error("syntaxe .catch invalide");
    if (!all) c.type = text::trim(t.substr(7, lb - 7));
    std::string range = t.substr(lb + 1, rb - lb - 1);
    size_t dots = range.find("..");
    if (dots == std::string::npos) throw Error("plage .catch invalide");
    c.start = text::trim(range.substr(0, dots));
    c.end = text::trim(range.substr(dots + 2));
    c.handler = text::trim(t.substr(rb + 1));
    for (auto* s : {&c.start, &c.end, &c.handler}) {
      if (s->empty() || (*s)[0] != ':') throw Error("étiquette attendue dans .catch");
      *s = s->substr(1);
    }
    return c;
  }

  size_t parse_payload(size_t i) {
    const Line& head = body_[i];
    auto words = split_ws(head.text);
    Item it{Item::Kind::kPayload};
    it.line = head.number;
    std::string end;
    if (words[0] == ".packed-switch") {
      it.payload = Item::PayloadKind::kPacked;
      if (words.size() != 2) throw Error(".packed-switch attend la première clé");
      it.first_key = int32_t(parse_literal(words[1]).integer);
      end = ".end packed-switch";
    } else if (words[0] == ".sparse-switch") {
      it.payload = Item::PayloadKind::kSparse;
      end = ".end sparse-switch";
    } else {
      it.payload = Item::PayloadKind::kArray;
      if (words.size() != 2) throw Error(".array-data attend une largeur");
      it.width = uint16_t(std::stoul(words[1], nullptr, 0));
      if (it.width != 1 && it.width != 2 && it.width != 4 && it.width != 8) throw Error("largeur .array-data invalide");
      end = ".end array-data";
    }
    for (++i; i < body_.size(); ++i) {
      const std::string& t = body_[i].text;
      if (t == end) {
        items_.push_back(std::move(it));
        return i;
      }
      if (it.payload == Item::PayloadKind::kPacked) {
        if (t[0] != ':') throw Error("étiquette attendue dans .packed-switch");
        it.targets.push_back(t.substr(1));
      } else if (it.payload == Item::PayloadKind::kSparse) {
        size_t arrow = t.find("->");
        if (arrow == std::string::npos) throw Error("« clé -> :étiquette » attendu");
        it.keys.push_back(int32_t(parse_literal(text::trim(t.substr(0, arrow))).integer));
        std::string target = text::trim(t.substr(arrow + 2));
        if (target.empty() || target[0] != ':') throw Error("étiquette attendue");
        it.targets.push_back(target.substr(1));
      } else {
        for (const auto& v : split_ws(t)) it.values.push_back(v);
      }
    }
    throw Error("fin de directive manquante : " + end);
  }

  void layout() {
    uint32_t addr = 0;
    std::vector<std::string> pending;  // labels at the current address
    std::vector<Item> out;
    for (Item& it : items_) {
      switch (it.kind) {
        case Item::Kind::kLabel:
          if (labels_.count(it.label)) fail(it.line, "étiquette dupliquée :" + it.label);
          labels_[it.label] = addr;
          pending.push_back(it.label);
          break;
        case Item::Kind::kInsn:
          it.addr = addr;
          addr += dex::format_units(dex::op_info(uint8_t(it.opcode)).format);
          pending.clear();
          break;
        case Item::Kind::kPayload:
          if (addr % 2) {  // payloads are 4-byte aligned
            Item pad{Item::Kind::kPadding};
            pad.addr = addr;
            out.push_back(pad);
            ++addr;
            for (const auto& l : pending) labels_[l] = addr;
          }
          it.addr = addr;
          addr += payload_size(it);
          pending.clear();
          break;
        case Item::Kind::kPadding:
          break;
      }
      out.push_back(std::move(it));
    }
    items_ = std::move(out);
    size_ = addr;
  }

  uint32_t label_addr(const std::string& tok, int line) const {
    if (tok.empty() || tok[0] != ':') fail(line, "étiquette attendue : " + tok);
    auto it = labels_.find(tok.substr(1));
    if (it == labels_.end()) fail(line, "étiquette inconnue " + tok);
    return it->second;
  }

  uint32_t reg(const std::string& tok, int line, uint32_t limit) const {
    if (tok.size() < 2 || (tok[0] != 'v' && tok[0] != 'p')) fail(line, "registre attendu : " + tok);
    uint32_t n;
    try {
      size_t used = 0;
      n = uint32_t(std::stoul(tok.substr(1), &used, 10));
      if (used != tok.size() - 1) throw 0;
    } catch (...) {
      fail(line, "registre invalide : " + tok);
    }
    if (tok[0] == 'p') n += uint32_t(m_.registers) - m_.ins;
    if (n >= m_.registers) fail(line, "registre " + tok + " hors de .registers");
    if (n >= limit) fail(line, "registre " + tok + " trop grand pour ce format");
    return n;
  }

  // {v0, v1} or {v0 .. v3}
  std::vector<uint32_t> reg_list(const std::string& tok, int line, bool& is_range) const {
    if (tok.size() < 2 || tok.front() != '{' || tok.back() != '}') fail(line, "liste de registres attendue");
    std::string inner = text::trim(tok.substr(1, tok.size() - 2));
    std::vector<uint32_t> regs;
    is_range = inner.find("..") != std::string::npos;
    if (inner.empty()) return regs;
    if (is_range) {
      size_t dots = inner.find("..");
      uint32_t a = reg(text::trim(inner.substr(0, dots)), line, 65536);
      uint32_t b = reg(text::trim(inner.substr(dots + 2)), line, 65536);
      if (b < a) fail(line, "plage de registres inversée");
      for (uint32_t r = a; r <= b; ++r) regs.push_back(r);
    } else {
      for (const auto& r : text::split(inner, ',')) regs.push_back(reg(text::trim(r), line, 65536));
    }
    return regs;
  }

  void need(const Item& it, size_t n) const {
    if (it.operands.size() != n) {
      fail(it.line, std::string(dex::op_info(uint8_t(it.opcode)).name) + " attend " + std::to_string(n) + " opérande(s)");
    }
  }

  void check_range(const Item& it, int64_t v, int64_t lo, int64_t hi) const {
    if (v < lo || v > hi) fail(it.line, "valeur " + std::to_string(v) + " hors limites pour " + dex::op_info(uint8_t(it.opcode)).name);
  }

  void add_ref(const Item& it, IndexKind kind, const std::string& tok, uint32_t unit, bool wide) {
    Fixup f;
    f.unit = unit;
    f.kind = kind;
    f.wide = wide;
    switch (kind) {
      case IndexKind::kString: f.ref = parse_string_literal(tok); break;
      case IndexKind::kType:
        if (tok.empty() || (tok[0] != 'L' && tok[0] != '[' && tok.size() != 1)) fail(it.line, "type attendu : " + tok);
        f.ref = tok;
        break;
      case IndexKind::kField:
        if (tok.find("->") == std::string::npos || tok.find(':') == std::string::npos) fail(it.line, "champ attendu : " + tok);
        f.ref = tok;
        break;
      case IndexKind::kMethod:
        if (tok.find("->") == std::string::npos || tok.find('(') == std::string::npos) fail(it.line, "méthode attendue : " + tok);
        f.ref = tok;
        break;
      default: fail(it.line, "instruction non prise en charge par l'assembleur");
    }
    m_.fixups.push_back(std::move(f));
  }

  void encode() {
    std::vector<uint16_t>& out = m_.insns;
    for (const Item& it : items_) {
      if (it.kind == Item::Kind::kInsn && (it.opcode == 0x2b || it.opcode == 0x2c) && it.operands.size() == 2) {
        switch_sources_[label_addr(it.operands[1], it.line)] = it.addr;
      }
    }
    for (const Item& it : items_) {
      if (it.kind == Item::Kind::kLabel) continue;
      if (out.size() != it.addr) fail(it.line, "erreur interne d'adressage");
      if (it.kind == Item::Kind::kPadding) {
        out.push_back(0);
        continue;
      }
      if (it.kind == Item::Kind::kPayload) {
        encode_payload(it);
        continue;
      }
      encode_insn(it);
    }
  }

  void encode_insn(const Item& it) {
    std::vector<uint16_t>& out = m_.insns;
    const uint8_t op = uint8_t(it.opcode);
    const dex::OpInfo& info = dex::op_info(op);
    const auto& o = it.operands;
    const uint32_t base = uint32_t(out.size());
    auto rel = [&](const std::string& tok) { return int64_t(label_addr(tok, it.line)) - int64_t(it.addr); };
    switch (info.format) {
      case Format::k10x:
        need(it, 0);
        out.push_back(op);
        break;
      case Format::k12x:
        need(it, 2);
        out.push_back(uint16_t(op | reg(o[0], it.line, 16) << 8 | reg(o[1], it.line, 16) << 12));
        break;
      case Format::k11n: {
        need(it, 2);
        int64_t v = parse_literal(o[1]).integer;
        check_range(it, v, -8, 7);
        out.push_back(uint16_t(op | reg(o[0], it.line, 16) << 8 | (uint32_t(v) & 0xF) << 12));
        break;
      }
      case Format::k11x:
        need(it, 1);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        break;
      case Format::k10t: {
        need(it, 1);
        int64_t off = rel(o[0]);
        check_range(it, off, -128, 127);
        out.push_back(uint16_t(op | (uint32_t(off) & 0xFF) << 8));
        break;
      }
      case Format::k20t: {
        need(it, 1);
        int64_t off = rel(o[0]);
        check_range(it, off, -32768, 32767);
        out.push_back(op);
        out.push_back(uint16_t(off));
        break;
      }
      case Format::k30t: {
        need(it, 1);
        int64_t off = rel(o[0]);
        out.push_back(op);
        out.push_back(uint16_t(off));
        out.push_back(uint16_t(uint32_t(off) >> 16));
        break;
      }
      case Format::k22x:
        need(it, 2);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(reg(o[1], it.line, 65536)));
        break;
      case Format::k32x:
        need(it, 2);
        out.push_back(op);
        out.push_back(uint16_t(reg(o[0], it.line, 65536)));
        out.push_back(uint16_t(reg(o[1], it.line, 65536)));
        break;
      case Format::k21t: {
        need(it, 2);
        int64_t off = rel(o[1]);
        check_range(it, off, -32768, 32767);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(off));
        break;
      }
      case Format::k21s: {
        need(it, 2);
        Literal l = parse_literal(o[1]);
        int64_t v = op == 0x16 ? literal64(l) : literal32(l);
        check_range(it, v, -32768, 32767);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(v));
        break;
      }
      case Format::k21h: {
        need(it, 2);
        Literal l = parse_literal(o[1]);
        uint16_t high;
        if (op == 0x15) {
          uint32_t v = uint32_t(literal32(l));
          if (v & 0xFFFF) fail(it.line, "const/high16 : les 16 bits de poids faible doivent être nuls");
          high = uint16_t(v >> 16);
        } else {
          uint64_t v = uint64_t(l.is_fp && !l.is_float ? int64_t(double_bits(l.fp)) : literal64(l));
          if (v & 0xFFFFFFFFFFFFull) fail(it.line, "const-wide/high16 : les 48 bits de poids faible doivent être nuls");
          high = uint16_t(v >> 48);
        }
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(high);
        break;
      }
      case Format::k21c:
        need(it, 2);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(0);
        add_ref(it, info.index, o[1], base + 1, false);
        break;
      case Format::k31c:
        need(it, 2);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(0);
        out.push_back(0);
        add_ref(it, info.index, o[1], base + 1, true);
        break;
      case Format::k23x:
        need(it, 3);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(reg(o[1], it.line, 256) | reg(o[2], it.line, 256) << 8));
        break;
      case Format::k22b: {
        need(it, 3);
        int64_t v = parse_literal(o[2]).integer;
        check_range(it, v, -128, 127);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(reg(o[1], it.line, 256) | (uint32_t(v) & 0xFF) << 8));
        break;
      }
      case Format::k22t: {
        need(it, 3);
        int64_t off = rel(o[2]);
        check_range(it, off, -32768, 32767);
        out.push_back(uint16_t(op | reg(o[0], it.line, 16) << 8 | reg(o[1], it.line, 16) << 12));
        out.push_back(uint16_t(off));
        break;
      }
      case Format::k22s: {
        need(it, 3);
        int64_t v = parse_literal(o[2]).integer;
        check_range(it, v, -32768, 32767);
        out.push_back(uint16_t(op | reg(o[0], it.line, 16) << 8 | reg(o[1], it.line, 16) << 12));
        out.push_back(uint16_t(v));
        break;
      }
      case Format::k22c:
        need(it, 3);
        out.push_back(uint16_t(op | reg(o[0], it.line, 16) << 8 | reg(o[1], it.line, 16) << 12));
        out.push_back(0);
        add_ref(it, info.index, o[2], base + 1, false);
        break;
      case Format::k31i: {
        need(it, 2);
        Literal l = parse_literal(o[1]);
        int64_t v = op == 0x17 ? literal64(l) : literal32(l);
        if (op == 0x17) check_range(it, v, INT32_MIN, INT32_MAX);
        else check_range(it, v, INT32_MIN, UINT32_MAX);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(v));
        out.push_back(uint16_t(uint64_t(v) >> 16));
        break;
      }
      case Format::k31t: {
        need(it, 2);
        int64_t off = rel(o[1]);
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        out.push_back(uint16_t(off));
        out.push_back(uint16_t(uint32_t(off) >> 16));
        break;
      }
      case Format::k51l: {
        need(it, 2);
        Literal l = parse_literal(o[1]);
        uint64_t v = uint64_t(l.is_fp && !l.is_float ? int64_t(double_bits(l.fp)) : literal64(l));
        out.push_back(uint16_t(op | reg(o[0], it.line, 256) << 8));
        for (int k = 0; k < 4; ++k) out.push_back(uint16_t(v >> (16 * k)));
        break;
      }
      case Format::k35c: {
        need(it, 2);
        bool range = false;
        auto regs = reg_list(o[0], it.line, range);
        if (range || regs.size() > 5) fail(it.line, "35c : au plus 5 registres listés (utilisez la variante /range)");
        uint32_t r[5] = {};
        for (size_t k = 0; k < regs.size(); ++k) {
          if (regs[k] >= 16) fail(it.line, "35c : registres v0-v15 uniquement");
          r[k] = regs[k];
        }
        out.push_back(uint16_t(op | uint32_t(regs.size()) << 12 | r[4] << 8));
        out.push_back(0);
        out.push_back(uint16_t(r[0] | r[1] << 4 | r[2] << 8 | r[3] << 12));
        add_ref(it, info.index, o[1], base + 1, false);
        if (info.index == IndexKind::kMethod) m_.outs = std::max<uint16_t>(m_.outs, uint16_t(regs.size()));
        break;
      }
      case Format::k3rc: {
        need(it, 2);
        bool range = false;
        auto regs = reg_list(o[0], it.line, range);
        if (!regs.empty() && !range && regs.size() > 1) {
          for (size_t k = 1; k < regs.size(); ++k) {
            if (regs[k] != regs[0] + k) fail(it.line, "3rc : registres consécutifs attendus");
          }
        }
        if (regs.size() > 255) fail(it.line, "3rc : trop de registres");
        out.push_back(uint16_t(op | uint32_t(regs.size()) << 8));
        out.push_back(0);
        out.push_back(uint16_t(regs.empty() ? 0 : regs[0]));
        add_ref(it, info.index, o[1], base + 1, false);
        if (info.index == IndexKind::kMethod) m_.outs = std::max<uint16_t>(m_.outs, uint16_t(regs.size()));
        break;
      }
      default:
        fail(it.line, std::string(info.name) + " n'est pas pris en charge par l'assembleur");
    }
  }

  void encode_payload(const Item& it) {
    std::vector<uint16_t>& out = m_.insns;
    auto u32 = [&](uint32_t v) {
      out.push_back(uint16_t(v));
      out.push_back(uint16_t(v >> 16));
    };
    if (it.payload == Item::PayloadKind::kArray) {
      out.push_back(dex::kFillArrayDataPayload);
      out.push_back(it.width);
      u32(uint32_t(it.values.size()));
      std::vector<uint8_t> bytes;
      for (const auto& v : it.values) {
        Literal l = parse_literal(v);
        uint64_t bits = it.width == 8 ? uint64_t(l.is_fp && !l.is_float ? int64_t(double_bits(l.fp)) : literal64(l))
                                      : uint64_t(literal32(l));
        for (int k = 0; k < it.width; ++k) bytes.push_back(uint8_t(bits >> (8 * k)));
      }
      if (bytes.size() % 2) bytes.push_back(0);
      for (size_t k = 0; k < bytes.size(); k += 2) out.push_back(uint16_t(bytes[k] | (bytes[k + 1] << 8)));
      return;
    }
    auto src = switch_sources_.find(it.addr);
    if (src == switch_sources_.end()) fail(it.line, "aucune instruction switch ne référence ces données");
    int64_t from = src->second;
    if (it.payload == Item::PayloadKind::kPacked) {
      out.push_back(dex::kPackedSwitchPayload);
      out.push_back(uint16_t(it.targets.size()));
      u32(uint32_t(it.first_key));
      for (const auto& t : it.targets) u32(uint32_t(int64_t(label_addr(":" + t, it.line)) - from));
    } else {
      out.push_back(dex::kSparseSwitchPayload);
      out.push_back(uint16_t(it.targets.size()));
      for (int32_t k : it.keys) u32(uint32_t(k));
      for (const auto& t : it.targets) u32(uint32_t(int64_t(label_addr(":" + t, it.line)) - from));
    }
  }

  // Splits overlapping .catch ranges into the disjoint, sorted try items the
  // DEX format requires.
  void build_tries() {
    if (catches_.empty()) return;
    struct Range {
      uint32_t start, end;
      const CatchDirective* c;
      uint32_t handler;
    };
    std::vector<Range> ranges;
    std::set<uint32_t> bounds;
    for (const auto& c : catches_) {
      uint32_t s = label_addr(":" + c.start, c.line), e = label_addr(":" + c.end, c.line);
      if (e <= s) fail(c.line, "plage .catch vide ou inversée");
      ranges.push_back({s, e, &c, label_addr(":" + c.handler, c.line)});
      bounds.insert(s);
      bounds.insert(e);
    }
    std::vector<uint32_t> b(bounds.begin(), bounds.end());
    for (size_t k = 0; k + 1 < b.size(); ++k) {
      TryDef t;
      t.start = b[k];
      t.count = b[k + 1] - b[k];
      std::set<std::string> seen;
      for (const Range& r : ranges) {
        if (r.start > b[k] || r.end < b[k + 1] || t.catch_all) continue;
        if (r.c->type.empty()) {
          t.catch_all = r.handler;
        } else if (seen.insert(r.c->type).second) {
          t.catches.emplace_back(r.c->type, r.handler);
        }
      }
      if (t.catches.empty() && !t.catch_all) continue;
      if (!m_.tries.empty()) {
        TryDef& prev = m_.tries.back();
        if (prev.start + prev.count == t.start && prev.catches == t.catches && prev.catch_all == t.catch_all) {
          prev.count += t.count;
          continue;
        }
      }
      m_.tries.push_back(std::move(t));
    }
    for (const TryDef& t : m_.tries) {
      if (t.count > 0xFFFF) fail(catches_.front().line, "bloc try trop long");
    }
  }

  MethodDef& m_;
  const std::vector<Line>& body_;
  const std::string& file_;
  int registers_ = -1;
  std::vector<Item> items_;
  std::vector<CatchDirective> catches_;
  std::map<std::string, uint32_t> labels_;
  std::map<uint32_t, uint32_t> switch_sources_;  // payload address -> switch instruction address
  uint32_t size_ = 0;
};

StaticValue parse_static_value(const std::string& raw, const std::string& type) {
  StaticValue v;
  std::string s = text::trim(raw);
  using K = StaticValue::Kind;
  if (s == "null") {
    v.kind = K::kNull;
  } else if (!s.empty() && s[0] == '"') {
    v.kind = K::kString;
    v.str = parse_string_literal(s);
  } else if (s == "true" || s == "false") {
    v.kind = K::kBool;
    v.bits = s == "true";
  } else {
    Literal l = parse_literal(s);
    if (type == "F") {
      v.kind = K::kFloat;
      v.bits = l.is_fp ? float_bits(l.fp) : float_bits(double(l.integer));
    } else if (type == "D") {
      v.kind = K::kDouble;
      v.bits = double_bits(l.is_fp ? l.fp : double(l.integer));
    } else {
      v.kind = type == "J" ? K::kLong : K::kInt;
      v.bits = uint64_t(l.integer);
    }
  }
  return v;
}

}  // namespace

ClassDef assemble_smali(std::string_view source, const std::string& file_name) {
  std::vector<Line> lines;
  {
    int n = 0;
    for (const auto& raw : text::split(source, '\n')) {
      ++n;
      std::string t = text::trim(strip_comment(raw));
      if (!t.empty()) lines.push_back({n, t});
    }
  }
  auto fail = [&](int line, const std::string& msg) -> void {
    throw std::runtime_error(file_name + ":" + std::to_string(line) + " : " + msg);
  };

  ClassDef cls;
  bool have_class = false, have_super = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const Line& ln = lines[i];
    auto words = split_ws(ln.text);
    const std::string& d = words[0];
    if (d == ".class") {
      if (have_class) fail(ln.number, ".class dupliqué");
      for (size_t k = 1; k + 1 < words.size(); ++k) cls.access |= access_flag(words[k]);
      if (words.size() < 2) fail(ln.number, "descripteur de classe manquant");
      cls.descriptor = words.back();
      have_class = true;
    } else if (d == ".super") {
      if (words.size() != 2) fail(ln.number, ".super attend un type");
      cls.super = words[1];
      have_super = true;
    } else if (d == ".implements") {
      if (words.size() != 2) fail(ln.number, ".implements attend un type");
      cls.interfaces.push_back(words[1]);
    } else if (d == ".source") {
      try {
        cls.source_file = parse_string_literal(text::trim(ln.text.substr(7)));
      } catch (const std::exception& e) {
        fail(ln.number, e.what());
      }
    } else if (d == ".annotation") {
      while (i < lines.size() && lines[i].text != ".end annotation") ++i;
    } else if (d == ".end" && words.size() > 1 && words[1] == "field") {
      // closes a field carrying annotations
    } else if (d == ".field") {
      std::string decl = ln.text;
      std::string value;
      if (size_t eq = decl.find(" = "); eq != std::string::npos) {
        value = decl.substr(eq + 3);
        decl = decl.substr(0, eq);
      }
      auto fw = split_ws(decl);
      FieldDef f;
      for (size_t k = 1; k + 1 < fw.size(); ++k) f.access |= access_flag(fw[k]);
      size_t colon = fw.back().find(':');
      if (colon == std::string::npos) fail(ln.number, "nom:type attendu");
      f.name = fw.back().substr(0, colon);
      f.type = fw.back().substr(colon + 1);
      if (!value.empty()) {
        if (!(f.access & dex::kAccStatic)) fail(ln.number, "valeur initiale sur un champ d'instance");
        try {
          f.initial = parse_static_value(value, f.type);
        } catch (const std::exception& e) {
          fail(ln.number, e.what());
        }
      }
      (f.access & dex::kAccStatic ? cls.static_fields : cls.instance_fields).push_back(std::move(f));
    } else if (d == ".method") {
      MethodDef m;
      for (size_t k = 1; k + 1 < words.size(); ++k) m.access |= access_flag(words[k]);
      const std::string& decl = words.back();
      size_t paren = decl.find('(');
      if (paren == std::string::npos) fail(ln.number, "déclaration de méthode invalide");
      m.name = decl.substr(0, paren);
      m.signature = decl.substr(paren);
      std::vector<std::string> params;
      std::string ret;
      if (!dex::split_signature(m.signature, params, ret)) fail(ln.number, "signature invalide " + m.signature);
      if (m.name == "<init>" || m.name == "<clinit>") m.access |= dex::kAccConstructor;
      std::vector<Line> body;
      size_t j = i + 1;
      for (; j < lines.size() && lines[j].text != ".end method"; ++j) body.push_back(lines[j]);
      if (j >= lines.size()) fail(ln.number, ".end method manquant");
      MethodAssembler(m, body, file_name).run();
      bool direct = (m.access & (dex::kAccStatic | dex::kAccPrivate | dex::kAccConstructor)) != 0;
      (direct ? cls.direct_methods : cls.virtual_methods).push_back(std::move(m));
      i = j;
    } else {
      fail(ln.number, "directive inattendue : " + d);
    }
  }
  if (!have_class) throw std::runtime_error(file_name + " : directive .class manquante");
  if (!have_super && cls.descriptor != "Ljava/lang/Object;") throw std::runtime_error(file_name + " : directive .super manquante");
  return cls;
}

}  // namespace aa::tool
