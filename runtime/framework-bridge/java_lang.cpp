// java.lang subset beyond the runtime core: StringBuilder, boxed primitives,
// Math, System, PrintStream, Runnable, Enum and String.format.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

#include "common/clock.h"
#include "common/text.h"
#include "dex-engine/jlang.h"
#include "framework-bridge/internal.h"

namespace aa::bridge {

using rt::ApiStatus;
using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = ApiStatus;

namespace {

struct StringBuilderPeer final : rt::NativePeer {
  std::u16string buf;
};

struct PrintStreamPeer final : rt::NativePeer {
  bool err = false;
  std::string pending;
};

struct EnumPeer final : rt::NativePeer {
  std::u16string name;
  int32_t ordinal = 0;
};

std::u16string str_of(Runtime& rt, Handle h) { return h == kNull ? u"null" : rt.string_value(h); }

std::u16string number_text(char type, uint64_t bits) {
  switch (type) {
    case 'Z': return bits ? u"true" : u"false";
    case 'C': return std::u16string(1, char16_t(bits));
    case 'J': return rt::u16(std::to_string(int64_t(bits)));
    case 'F': {
      uint32_t b = uint32_t(bits);
      float f;
      std::memcpy(&f, &b, 4);
      return rt::u16(rt::java_fp_to_string(f, true));
    }
    case 'D': {
      double d;
      std::memcpy(&d, &bits, 8);
      return rt::u16(rt::java_fp_to_string(d, false));
    }
    default: return rt::u16(std::to_string(int32_t(bits)));
  }
}

double as_double(char type, uint64_t bits) {
  switch (type) {
    case 'J': return double(int64_t(bits));
    case 'F': {
      uint32_t b = uint32_t(bits);
      float f;
      std::memcpy(&f, &b, 4);
      return f;
    }
    case 'D': {
      double d;
      std::memcpy(&d, &bits, 8);
      return d;
    }
    default: return double(int32_t(bits));
  }
}

int64_t as_long(char type, uint64_t bits) {
  switch (type) {
    case 'J': return int64_t(bits);
    case 'F':
    case 'D': {
      double d = as_double(type, bits);
      if (std::isnan(d)) return 0;
      if (d >= 9.2233720368547758e18) return std::numeric_limits<int64_t>::max();
      if (d <= -9.2233720368547758e18) return std::numeric_limits<int64_t>::min();
      return int64_t(d);
    }
    case 'Z': return bits ? 1 : 0;
    case 'C': return int64_t(bits & 0xFFFF);
    default: return int32_t(bits);
  }
}

[[noreturn]] void number_format_error(Runtime& rt, const std::u16string& s) {
  rt.throw_new("Ljava/lang/NumberFormatException;", u"For input string: \"" + s + u"\"");
}

int64_t parse_integer(Runtime& rt, const std::u16string& s, int radix, int64_t min, int64_t max) {
  if (radix < 2 || radix > 36 || s.empty()) number_format_error(rt, s);
  size_t i = 0;
  bool neg = false;
  if (s[0] == u'-' || s[0] == u'+') {
    neg = s[0] == u'-';
    if (s.size() == 1) number_format_error(rt, s);
    i = 1;
  }
  // Accumulate negatively to cover the minimum value without overflow.
  int64_t limit = neg ? min : -max;
  int64_t result = 0;
  for (; i < s.size(); ++i) {
    char16_t c = s[i];
    int d = (c >= u'0' && c <= u'9') ? c - u'0' : (c >= u'a' && c <= u'z') ? c - u'a' + 10
            : (c >= u'A' && c <= u'Z') ? c - u'A' + 10 : 99;
    if (d >= radix) number_format_error(rt, s);
    if (result < (limit + d) / radix) number_format_error(rt, s);
    result = result * radix - d;
    if (result < limit) number_format_error(rt, s);
  }
  return neg ? result : -result;
}

double parse_double(Runtime& rt, const std::u16string& s) {
  std::string u = text::trim(text::utf16_to_utf8(s));
  if (u.empty()) number_format_error(rt, s);
  if (u == "NaN") return NAN;
  if (u == "Infinity" || u == "+Infinity") return HUGE_VAL;
  if (u == "-Infinity") return -HUGE_VAL;
  if (!u.empty() && (u.back() == 'f' || u.back() == 'F' || u.back() == 'd' || u.back() == 'D')) u.pop_back();
  for (char c : u) {
    if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+')) {
      number_format_error(rt, s);
    }
  }
  char* end = nullptr;
  double v = std::strtod(u.c_str(), &end);
  if (!end || *end != '\0') number_format_error(rt, s);
  return v;
}

void register_string_builder(rt::NativeRegistry& r, const char* desc) {
  std::string d = desc;
  std::string ret = "L" + d.substr(1, d.size() - 2) + ";";
  auto sb = [](Runtime& rt, Handle h) -> std::u16string& { return peer_or_throw<StringBuilderPeer>(rt, h, "StringBuilder").buf; };
  auto b = r.define(d, "Ljava/lang/Object;", {"Ljava/lang/CharSequence;", "Ljava/io/Serializable;"}, 0x1 | 0x10);
  auto make = [](Runtime& rt, NativeArgs& a, std::u16string init) -> uint64_t {
    auto p = std::make_unique<StringBuilderPeer>();
    p->buf = std::move(init);
    a.self().peer = std::move(p);
    return 0;
  };
  b.ctor("()V", [make](Runtime& rt, NativeArgs& a) { return make(rt, a, {}); });
  b.ctor("(I)V", [make](Runtime& rt, NativeArgs& a) {
    if (a.i32(1) < 0) rt.throw_new("Ljava/lang/NegativeArraySizeException;", std::to_string(a.i32(1)));
    return make(rt, a, {});
  });
  b.ctor("(Ljava/lang/String;)V", [make](Runtime& rt, NativeArgs& a) { return make(rt, a, a.str(1)); });
  b.ctor("(Ljava/lang/CharSequence;)V", [make](Runtime& rt, NativeArgs& a) { return make(rt, a, a.str(1)); });
  auto append = [sb](std::u16string (*conv)(Runtime&, NativeArgs&)) {
    return [sb, conv](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::u16string s = conv(rt, a);
      sb(rt, a.h(0)) += s;
      return a.h(0);
    };
  };
  b.method("append", "(Ljava/lang/String;)" + ret, append([](Runtime& rt, NativeArgs& a) { return str_of(rt, a.h(1)); }));
  b.method("append", "(Ljava/lang/Object;)" + ret, append([](Runtime& rt, NativeArgs& a) { return str_of(rt, a.h(1)); }));
  b.method("append", "(Ljava/lang/CharSequence;)" + ret, append([](Runtime& rt, NativeArgs& a) { return str_of(rt, a.h(1)); }));
  b.method("append", "(I)" + ret, append([](Runtime&, NativeArgs& a) { return number_text('I', a.raw(1)); }));
  b.method("append", "(J)" + ret, append([](Runtime&, NativeArgs& a) { return number_text('J', uint64_t(a.i64(1))); }));
  b.method("append", "(Z)" + ret, append([](Runtime&, NativeArgs& a) { return number_text('Z', a.raw(1)); }));
  b.method("append", "(C)" + ret, append([](Runtime&, NativeArgs& a) { return number_text('C', a.raw(1)); }));
  b.method("append", "(F)" + ret, append([](Runtime&, NativeArgs& a) { return number_text('F', a.raw(1)); }));
  b.method("append", "(D)" + ret, append([](Runtime&, NativeArgs& a) { return number_text('D', uint64_t(a.i64(1))); }));
  b.method("append", "([C)" + ret, append([](Runtime& rt, NativeArgs& a) {
    rt::ArrayObject& arr = rt.array(a.h(1), "append(char[])");
    std::u16string v(arr.length, u'\0');
    std::memcpy(v.data(), arr.data.data(), arr.data.size());
    return v;
  }));
  b.method("toString", "()Ljava/lang/String;", [sb](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.new_string(sb(rt, a.h(0))); });
  b.method("length", "()I", [sb](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(int32_t(sb(rt, a.h(0)).size())); });
  b.method("charAt", "(I)C", [sb](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& s = sb(rt, a.h(0));
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= s.size()) rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "index " + std::to_string(i));
    return s[size_t(i)];
  });
  b.method("reverse", "()" + ret, [sb](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& s = sb(rt, a.h(0));
    std::reverse(s.begin(), s.end());
    return a.h(0);
  }, S::kPartial, "les paires de substitution ne sont pas préservées");
  b.method("setLength", "(I)V", [sb](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.i32(1) < 0) rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", std::to_string(a.i32(1)));
    sb(rt, a.h(0)).resize(size_t(a.i32(1)), u'\0');
    return 0;
  });
  b.method("insert", "(ILjava/lang/String;)" + ret, [sb](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& s = sb(rt, a.h(0));
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) > s.size()) rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "offset " + std::to_string(i));
    s.insert(size_t(i), str_of(rt, a.h(2)));
    return a.h(0);
  });
  b.method("deleteCharAt", "(I)" + ret, [sb](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& s = sb(rt, a.h(0));
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= s.size()) rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "index " + std::to_string(i));
    s.erase(size_t(i), 1);
    return a.h(0);
  });
  b.method("indexOf", "(Ljava/lang/String;)I", [sb](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = sb(rt, a.h(0)).find(a.str(1));
    return rt::ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  });
}

struct BoxSpec {
  const char* cls;
  char type;
  const char* value_method;  // intValue, ...
};

const BoxSpec kBoxes[] = {
    {"Ljava/lang/Integer;", 'I', "intValue"},   {"Ljava/lang/Long;", 'J', "longValue"},
    {"Ljava/lang/Short;", 'S', "shortValue"},   {"Ljava/lang/Byte;", 'B', "byteValue"},
    {"Ljava/lang/Float;", 'F', "floatValue"},   {"Ljava/lang/Double;", 'D', "doubleValue"},
    {"Ljava/lang/Character;", 'C', "charValue"}, {"Ljava/lang/Boolean;", 'Z', "booleanValue"},
};

const BoxSpec* box_spec(char type) {
  for (const auto& b : kBoxes) {
    if (b.type == type) return &b;
  }
  return nullptr;
}

uint64_t read_prim(NativeArgs& a, uint32_t i, char type) {
  return (type == 'J' || type == 'D') ? uint64_t(a.i64(i)) : a.raw(i);
}

int32_t box_hash(char type, uint64_t bits) {
  switch (type) {
    case 'J':
    case 'D': return int32_t(uint32_t(bits ^ (bits >> 32)));
    case 'Z': return bits ? 1231 : 1237;
    default: return int32_t(bits);
  }
}

void register_boxes(rt::NativeRegistry& r) {
  r.define("Ljava/lang/Number;", "Ljava/lang/Object;", {"Ljava/io/Serializable;"}, 0x1 | 0x400);
  auto number = r.extend("Ljava/lang/Number;");
  auto num_method = [&](const char* name, char out) {
    std::string sig = std::string("()") + out;
    number.method(name, sig, [out](Runtime& rt, NativeArgs& a) -> uint64_t {
      BoxPeer& p = peer_or_throw<BoxPeer>(rt, a.h(0), "Number");
      int64_t l = as_long(p.type, p.bits);
      double d = as_double(p.type, p.bits);
      switch (out) {
        case 'I': return rt::ret_int(int32_t(l));
        case 'J': return rt::ret_long(l);
        case 'S': return rt::ret_int(int16_t(l));
        case 'B': return rt::ret_int(int8_t(l));
        case 'F': return rt::ret_float(float(d));
        default: return rt::ret_double(d);
      }
    });
  };
  num_method("intValue", 'I');
  num_method("longValue", 'J');
  num_method("shortValue", 'S');
  num_method("byteValue", 'B');
  num_method("floatValue", 'F');
  num_method("doubleValue", 'D');

  for (const BoxSpec& spec : kBoxes) {
    const char type = spec.type;
    bool numeric = type != 'C' && type != 'Z';
    std::string prim(1, type);
    std::string cls = spec.cls;
    auto b = r.define(cls, numeric ? "Ljava/lang/Number;" : "Ljava/lang/Object;",
                      {"Ljava/io/Serializable;", "Ljava/lang/Comparable;"}, 0x1 | 0x10);
    b.ctor("(" + prim + ")V", [type](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto p = std::make_unique<BoxPeer>();
      p->type = type;
      p->bits = read_prim(a, 1, type);
      a.self().peer = std::move(p);
      return 0;
    });
    b.static_method("valueOf", "(" + prim + ")" + cls, [type](Runtime& rt, NativeArgs& a) -> uint64_t {
      return box(rt, type, read_prim(a, 0, type));
    });
    if (!numeric) {
      // Number already provides the numeric accessors.
      b.method(spec.value_method, "()" + prim, [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return peer_or_throw<BoxPeer>(rt, a.h(0), "box").bits;
      });
    }
    b.method("toString", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      BoxPeer& p = peer_or_throw<BoxPeer>(rt, a.h(0), "box");
      return rt.new_string(number_text(p.type, p.bits));
    });
    b.static_method("toString", "(" + prim + ")Ljava/lang/String;", [type](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt.new_string(number_text(type, read_prim(a, 0, type)));
    });
    b.method("hashCode", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      BoxPeer& p = peer_or_throw<BoxPeer>(rt, a.h(0), "box");
      return rt::ret_int(box_hash(p.type, p.bits));
    });
    b.method("equals", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      BoxPeer& p = peer_or_throw<BoxPeer>(rt, a.h(0), "box");
      auto* o = rt.peer<BoxPeer>(a.h(1));
      return rt::ret_bool(o && o->type == p.type && o->bits == p.bits && rt.class_of(a.h(1)) == rt.class_of(a.h(0)));
    });
    auto cmp = [](Runtime& rt, NativeArgs& a) -> uint64_t {
      BoxPeer& x = peer_or_throw<BoxPeer>(rt, a.h(0), "box");
      BoxPeer& y = peer_or_throw<BoxPeer>(rt, a.h(1), "compareTo");
      if (x.type == 'F' || x.type == 'D') {
        double dx = as_double(x.type, x.bits), dy = as_double(y.type, y.bits);
        return rt::ret_int(dx < dy ? -1 : (dx > dy ? 1 : 0));
      }
      int64_t lx = as_long(x.type, x.bits), ly = as_long(y.type, y.bits);
      return rt::ret_int(lx < ly ? -1 : (lx > ly ? 1 : 0));
    };
    b.method("compareTo", "(" + cls + ")I", cmp);
    b.method("compareTo", "(Ljava/lang/Object;)I", cmp);
  }

  auto integer = r.extend("Ljava/lang/Integer;");
  integer.static_field("MAX_VALUE", "I", [](Runtime&) -> uint64_t { return 0x7fffffff; });
  integer.static_field("MIN_VALUE", "I", [](Runtime&) -> uint64_t { return 0x80000000u; });
  integer.static_method("parseInt", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(parse_integer(rt, a.str_or(0, u"null"), 10, INT32_MIN, INT32_MAX)));
  });
  integer.static_method("parseInt", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(parse_integer(rt, a.str_or(0, u"null"), a.i32(1), INT32_MIN, INT32_MAX)));
  });
  integer.static_method("valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return box(rt, 'I', uint32_t(int32_t(parse_integer(rt, a.str_or(0, u"null"), 10, INT32_MIN, INT32_MAX))));
  });
  integer.static_method("toHexString", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%x", a.raw(0));
    return rt.new_string_utf8(buf);
  });
  integer.static_method("toBinaryString", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint32_t v = a.raw(0);
    std::string s;
    do {
      s.insert(s.begin(), char('0' + (v & 1)));
      v >>= 1;
    } while (v);
    return rt.new_string_utf8(s);
  });
  integer.static_method("compare", "(II)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_int(a.i32(0) < a.i32(1) ? -1 : (a.i32(0) > a.i32(1) ? 1 : 0));
  });
  auto lng = r.extend("Ljava/lang/Long;");
  lng.static_method("parseLong", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(parse_integer(rt, a.str_or(0, u"null"), 10, INT64_MIN, INT64_MAX));
  });
  lng.static_field("MAX_VALUE", "J", [](Runtime&) -> uint64_t { return 0x7fffffffffffffffull; });
  lng.static_field("MIN_VALUE", "J", [](Runtime&) -> uint64_t { return 0x8000000000000000ull; });
  r.extend("Ljava/lang/Double;")
      .static_method("parseDouble", "(Ljava/lang/String;)D", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return rt::ret_double(parse_double(rt, a.str_or(0, u"null")));
      })
      .static_method("isNaN", "(D)Z", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_bool(std::isnan(a.f64(0))); });
  r.extend("Ljava/lang/Float;")
      .static_method("parseFloat", "(Ljava/lang/String;)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return rt::ret_float(float(parse_double(rt, a.str_or(0, u"null"))));
      })
      .static_method("isNaN", "(F)Z", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_bool(std::isnan(a.f32(0))); });
  r.extend("Ljava/lang/Boolean;")
      .static_method("parseBoolean", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        std::u16string s = a.str_or(0, u"");
        for (auto& c : s) c = rt::java_to_lower(c);
        return rt::ret_bool(s == u"true");
      })
      .static_field("TRUE", "Ljava/lang/Boolean;", [](Runtime& rt) -> uint64_t { return box(rt, 'Z', 1); })
      .static_field("FALSE", "Ljava/lang/Boolean;", [](Runtime& rt) -> uint64_t { return box(rt, 'Z', 0); });
  r.extend("Ljava/lang/Character;")
      .static_method("isDigit", "(C)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
        char16_t c = char16_t(a.raw(0));
        return rt::ret_bool(c >= u'0' && c <= u'9');
      }, S::kPartial, "chiffres ASCII uniquement")
      .static_method("isLetter", "(C)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
        char16_t c = char16_t(a.raw(0));
        return rt::ret_bool((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7));
      }, S::kPartial, "alphabet latin uniquement")
      .static_method("isWhitespace", "(C)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
        char16_t c = char16_t(a.raw(0));
        return rt::ret_bool(c == u' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F));
      })
      .static_method("toUpperCase", "(C)C", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::java_to_upper(char16_t(a.raw(0))); },
                     S::kPartial, "ASCII et Latin-1 uniquement")
      .static_method("toLowerCase", "(C)C", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::java_to_lower(char16_t(a.raw(0))); },
                     S::kPartial, "ASCII et Latin-1 uniquement");
}

void register_math(rt::NativeRegistry& r) {
  auto m = r.define("Ljava/lang/Math;", "Ljava/lang/Object;", {}, 0x1 | 0x10);
  m.static_field("PI", "D", [](Runtime&) { return rt::ret_double(3.141592653589793); });
  m.static_field("E", "D", [](Runtime&) { return rt::ret_double(2.718281828459045); });
  m.static_method("abs", "(I)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    int32_t v = a.i32(0);
    return rt::ret_int(v < 0 ? int32_t(0u - uint32_t(v)) : v);
  });
  m.static_method("abs", "(J)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    int64_t v = a.i64(0);
    return rt::ret_long(v < 0 ? int64_t(0ull - uint64_t(v)) : v);
  });
  m.static_method("abs", "(F)F", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_float(std::fabs(a.f32(0))); });
  m.static_method("abs", "(D)D", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(std::fabs(a.f64(0))); });
  m.static_method("max", "(II)I", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_int(std::max(a.i32(0), a.i32(1))); });
  m.static_method("min", "(II)I", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_int(std::min(a.i32(0), a.i32(1))); });
  m.static_method("max", "(JJ)J", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_long(std::max(a.i64(0), a.i64(2))); });
  m.static_method("min", "(JJ)J", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_long(std::min(a.i64(0), a.i64(2))); });
  auto fmax = [](double x, double y) { return std::isnan(x) || std::isnan(y) ? NAN : (x > y ? x : y); };
  auto fmin = [](double x, double y) { return std::isnan(x) || std::isnan(y) ? NAN : (x < y ? x : y); };
  m.static_method("max", "(FF)F", [fmax](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_float(float(fmax(a.f32(0), a.f32(1)))); });
  m.static_method("min", "(FF)F", [fmin](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_float(float(fmin(a.f32(0), a.f32(1)))); });
  m.static_method("max", "(DD)D", [fmax](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(fmax(a.f64(0), a.f64(2))); });
  m.static_method("min", "(DD)D", [fmin](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(fmin(a.f64(0), a.f64(2))); });
  auto unary = [&](const char* name, double (*fn)(double)) {
    m.static_method(name, "(D)D", [fn](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(fn(a.f64(0))); });
  };
  unary("sqrt", [](double x) { return std::sqrt(x); });
  unary("cbrt", [](double x) { return std::cbrt(x); });
  unary("floor", [](double x) { return std::floor(x); });
  unary("ceil", [](double x) { return std::ceil(x); });
  unary("sin", [](double x) { return std::sin(x); });
  unary("cos", [](double x) { return std::cos(x); });
  unary("tan", [](double x) { return std::tan(x); });
  unary("atan", [](double x) { return std::atan(x); });
  unary("exp", [](double x) { return std::exp(x); });
  unary("log", [](double x) { return std::log(x); });
  unary("log10", [](double x) { return std::log10(x); });
  unary("signum", [](double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : x); });
  unary("toRadians", [](double x) { return x / 180.0 * 3.141592653589793; });
  unary("toDegrees", [](double x) { return x * 180.0 / 3.141592653589793; });
  m.static_method("pow", "(DD)D", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(std::pow(a.f64(0), a.f64(2))); });
  m.static_method("atan2", "(DD)D", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(std::atan2(a.f64(0), a.f64(2))); });
  m.static_method("hypot", "(DD)D", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(std::hypot(a.f64(0), a.f64(2))); });
  m.static_method("round", "(F)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    float v = a.f32(0);
    if (std::isnan(v)) return 0;
    double r2 = std::floor(double(v) + 0.5);
    if (r2 >= 2147483647.0) return rt::ret_int(INT32_MAX);
    if (r2 <= -2147483648.0) return rt::ret_int(INT32_MIN);
    return rt::ret_int(int32_t(r2));
  });
  m.static_method("round", "(D)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    double v = a.f64(0);
    if (std::isnan(v)) return 0;
    double r2 = std::floor(v + 0.5);
    if (r2 >= 9.2233720368547758e18) return rt::ret_long(INT64_MAX);
    if (r2 <= -9.2233720368547758e18) return rt::ret_long(INT64_MIN);
    return rt::ret_long(int64_t(r2));
  });
  m.static_method("random", "()D", [](Runtime&, NativeArgs&) -> uint64_t {
    static std::mt19937_64 gen{std::random_device{}()};
    return rt::ret_double(std::uniform_real_distribution<double>(0.0, 1.0)(gen));
  });
}

void print_line(Host& host, PrintStreamPeer& p, const std::string& text, bool newline) {
  p.pending += text;
  size_t nl;
  while ((nl = p.pending.find('\n')) != std::string::npos) {
    host.console(p.err, p.pending.substr(0, nl));
    p.pending.erase(0, nl + 1);
  }
  if (newline) {
    host.console(p.err, p.pending);
    p.pending.clear();
  }
}

void register_system(rt::NativeRegistry& r, Host& host) {
  auto ps = r.define("Ljava/io/PrintStream;", "Ljava/lang/Object;");
  auto stream = [](Runtime& rt, Handle h) -> PrintStreamPeer& { return peer_or_throw<PrintStreamPeer>(rt, h, "PrintStream"); };
  struct Variant {
    const char* sig;
    std::u16string (*conv)(Runtime&, NativeArgs&);
  };
  static const Variant variants[] = {
      {"(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) { return str_of(rt, a.h(1)); }},
      {"(Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) { return str_of(rt, a.h(1)); }},
      {"(I)V", [](Runtime&, NativeArgs& a) { return number_text('I', a.raw(1)); }},
      {"(J)V", [](Runtime&, NativeArgs& a) { return number_text('J', uint64_t(a.i64(1))); }},
      {"(Z)V", [](Runtime&, NativeArgs& a) { return number_text('Z', a.raw(1)); }},
      {"(C)V", [](Runtime&, NativeArgs& a) { return number_text('C', a.raw(1)); }},
      {"(F)V", [](Runtime&, NativeArgs& a) { return number_text('F', a.raw(1)); }},
      {"(D)V", [](Runtime&, NativeArgs& a) { return number_text('D', uint64_t(a.i64(1))); }},
  };
  for (const Variant& v : variants) {
    auto conv = v.conv;
    ps.method("println", v.sig, [&host, stream, conv](Runtime& rt, NativeArgs& a) -> uint64_t {
      print_line(host, stream(rt, a.h(0)), text::utf16_to_utf8(conv(rt, a)), true);
      return 0;
    });
    ps.method("print", v.sig, [&host, stream, conv](Runtime& rt, NativeArgs& a) -> uint64_t {
      print_line(host, stream(rt, a.h(0)), text::utf16_to_utf8(conv(rt, a)), false);
      return 0;
    });
  }
  ps.method("println", "()V", [&host, stream](Runtime& rt, NativeArgs& a) -> uint64_t {
    print_line(host, stream(rt, a.h(0)), "", true);
    return 0;
  });
  ps.method("flush", "()V", [&host, stream](Runtime& rt, NativeArgs& a) -> uint64_t {
    PrintStreamPeer& p = stream(rt, a.h(0));
    if (!p.pending.empty()) print_line(host, p, "", true);
    return 0;
  });
  ps.method("printf", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;",
            [&host, stream](Runtime& rt, NativeArgs& a) -> uint64_t {
              print_line(host, stream(rt, a.h(0)), text::utf16_to_utf8(java_format(rt, a.str(1), a.h(2))), false);
              return a.h(0);
            },
            S::kPartial, "sous-ensemble des formats (%s %d %x %c %b %f %n)");

  auto make_stream = [](bool err) {
    return [err](Runtime& rt) -> uint64_t {
      Handle h = rt.alloc(rt.find_class("Ljava/io/PrintStream;"));
      auto p = std::make_unique<PrintStreamPeer>();
      p->err = err;
      rt.object(h)->peer = std::move(p);
      return h;
    };
  };
  auto sys = r.define("Ljava/lang/System;", "Ljava/lang/Object;", {}, 0x1 | 0x10);
  sys.static_field("out", "Ljava/io/PrintStream;", make_stream(false));
  sys.static_field("err", "Ljava/io/PrintStream;", make_stream(true));
  sys.static_method("currentTimeMillis", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(aa::clock::unix_millis());
  });
  sys.static_method("nanoTime", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(aa::clock::monotonic_nanos());
  });
  sys.static_method("exit", "(I)V", [](Runtime&, NativeArgs& a) -> uint64_t { throw rt::ExitRequest{a.i32(0)}; });
  sys.static_method("identityHashCode", "(Ljava/lang/Object;)I", [](Runtime&, NativeArgs& a) -> uint64_t { return a.raw(0); });
  sys.static_method("lineSeparator", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t { return rt.intern(u"\n"); });
  sys.static_method("gc", "()V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.request_gc();
    return 0;
  }, S::kPartial, "collecte au prochain point de sûreté");
  sys.unsupported("loadLibrary", "(Ljava/lang/String;)V",
                  "bibliothèques natives ELF non prises en charge (couche native prévue après le MVP)");
  sys.spec().methods.back().access |= 0x8;
  sys.unsupported("load", "(Ljava/lang/String;)V", "bibliothèques natives ELF non prises en charge");
  sys.spec().methods.back().access |= 0x8;
  sys.static_method("getProperty", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string k = a.str(0);
    if (k == u"line.separator") return rt.intern(u"\n");
    if (k == u"java.vm.name") return rt.intern(u"AnyAndroid");
    if (k == u"os.name") return rt.intern(u"Linux");
    return kNull;
  }, S::kPartial, "quelques propriétés seulement");
  sys.static_method("arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& src = rt.array(a.h(0), "arraycopy(src)");
    rt::ArrayObject& dst = rt.array(a.h(2), "arraycopy(dst)");
    int32_t sp = a.i32(1), dp = a.i32(3), len = a.i32(4);
    if (src.width != dst.width || bool(src.klass->component->primitive) != bool(dst.klass->component->primitive) ||
        (src.klass->component->primitive && src.klass->component != dst.klass->component)) {
      rt.throw_new("Ljava/lang/ArrayStoreException;", "arraycopy : types incompatibles");
    }
    if (sp < 0 || dp < 0 || len < 0 || int64_t(sp) + len > src.length || int64_t(dp) + len > dst.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "arraycopy : indices hors limites");
    }
    if (!src.klass->component->primitive && !rt.is_assignable(src.klass->component, dst.klass->component)) {
      for (int32_t i = 0; i < len; ++i) {
        Handle v;
        std::memcpy(&v, &src.data[size_t(sp + i) * 4], 4);
        if (v != kNull && !rt.is_assignable(rt.class_of(v), dst.klass->component)) {
          rt.throw_new("Ljava/lang/ArrayStoreException;", "arraycopy : élément incompatible");
        }
      }
    }
    std::memmove(dst.data.data() + size_t(dp) * dst.width, src.data.data() + size_t(sp) * src.width, size_t(len) * src.width);
    return 0;
  });
}

void register_enum(rt::NativeRegistry& r) {
  auto e = r.define("Ljava/lang/Enum;", "Ljava/lang/Object;", {"Ljava/lang/Comparable;", "Ljava/io/Serializable;"},
                    0x1 | 0x400);
  auto ep = [](Runtime& rt, Handle h) -> EnumPeer& { return peer_or_throw<EnumPeer>(rt, h, "Enum"); };
  e.ctor("(Ljava/lang/String;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<EnumPeer>();
    p->name = a.str(1);
    p->ordinal = a.i32(2);
    a.self().peer = std::move(p);
    return 0;
  });
  e.method("name", "()Ljava/lang/String;", [ep](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.new_string(ep(rt, a.h(0)).name); });
  e.method("toString", "()Ljava/lang/String;", [ep](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.new_string(ep(rt, a.h(0)).name); });
  e.method("ordinal", "()I", [ep](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(ep(rt, a.h(0)).ordinal); });
  auto cmp = [ep](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(ep(rt, a.h(0)).ordinal - ep(rt, a.h(1)).ordinal); };
  e.method("compareTo", "(Ljava/lang/Enum;)I", cmp);
  e.method("compareTo", "(Ljava/lang/Object;)I", cmp);
  e.method("getDeclaringClass", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = a.self().klass;
    while (c->super && c->super->descriptor != "Ljava/lang/Enum;") c = c->super;
    return rt.class_mirror(c);
  });
  e.static_method("valueOf", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", [ep](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = rt.mirror_class(a.h(0));
    if (!c) rt.throw_npe("Enum.valueOf(null)");
    std::u16string name = a.str(1);
    rt::Method* values = rt.find_static(c, "values", "()[" + c->descriptor);
    if (!values) rt.throw_new("Ljava/lang/IllegalArgumentException;", c->java_name() + " n'est pas une énumération");
    rt.ensure_initialized(c);
    Handle arr = Handle(rt.invoke(values, nullptr, 0));
    rt::ArrayObject& array = rt.array(arr, "values()");
    for (uint32_t i = 0; i < array.length; ++i) {
      Handle h;
      std::memcpy(&h, &array.data[i * 4], 4);
      if (h && ep(rt, h).name == name) return h;
    }
    rt.throw_new("Ljava/lang/IllegalArgumentException;", u"No enum constant " + rt::u16(c->java_name()) + u"." + name);
  });
}

}  // namespace

Handle box(Runtime& rt, char type, uint64_t bits) {
  const BoxSpec* spec = box_spec(type);
  if (!spec) rt.throw_new("Ljava/lang/InternalError;", "type encapsulé inconnu");
  // Integer cache semantics (-128..127, Boolean.TRUE/FALSE).
  bool cacheable = type == 'Z' || ((type == 'I' || type == 'S' || type == 'B' || type == 'J') &&
                                   as_long(type, bits) >= -128 && as_long(type, bits) <= 127) ||
                   (type == 'C' && bits < 128);
  if (type == 'S') bits = uint32_t(int32_t(int16_t(bits)));
  if (type == 'B') bits = uint32_t(int32_t(int8_t(bits)));
  if (type == 'C') bits &= 0xFFFF;
  if (type == 'Z') bits = bits ? 1 : 0;
  rt::Class* cls = rt.find_class(spec->cls);
  if (!cls->framework) {
    // libcore mode: the box class is bytecode, valueOf() applies its caches.
    std::vector<uint32_t> args{uint32_t(bits)};
    if (type == 'J' || type == 'D') args.push_back(uint32_t(bits >> 32));
    return Handle(rt.call_static(spec->cls, "valueOf", std::string("(") + type + ")" + spec->cls, args));
  }
  std::string key = std::string("box:") + type + std::to_string(bits);
  if (cacheable) {
    auto it = rt.object_cache().find(key);
    if (it != rt.object_cache().end()) return it->second;
  }
  Handle h = rt.alloc(cls);
  auto p = std::make_unique<BoxPeer>();
  p->type = type;
  p->bits = bits;
  rt.object(h)->peer = std::move(p);
  if (cacheable) rt.object_cache()[key] = h;
  return h;
}

bool unbox(Runtime& rt, Handle h, char& type, uint64_t& bits) {
  if (auto* p = rt.peer<BoxPeer>(h)) {
    type = p->type;
    bits = p->bits;
    return true;
  }
  rt::Object* o = rt.object(h);
  if (!o || o->klass->framework) return false;
  for (const auto& b : kBoxes) {  // libcore boxes keep their primitive in "value"
    if (o->klass->descriptor != b.cls) continue;
    rt::Field* f = o->klass->declared_field("value", std::string(1, b.type));
    if (!f || f->is_static()) return false;
    type = b.type;
    bits = o->fields.at(f->slot);
    if (f->wide()) bits |= uint64_t(o->fields.at(f->slot + 1)) << 32;
    return true;
  }
  return false;
}

bool java_equals(Runtime& rt, Handle a, Handle b) {
  if (a == b) return true;
  if (a == kNull || b == kNull) return false;
  return rt.call_virtual(a, "equals", "(Ljava/lang/Object;)Z", {b}) != 0;
}

std::u16string java_format(Runtime& rt, const std::u16string& fmt, Handle args_array) {
  std::vector<Handle> args;
  if (args_array != kNull) {
    rt::ArrayObject& arr = rt.array(args_array, "format");
    for (uint32_t i = 0; i < arr.length; ++i) {
      Handle h;
      std::memcpy(&h, &arr.data[i * 4], 4);
      args.push_back(h);
    }
  }
  std::u16string out;
  size_t next_arg = 0;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != u'%') {
      out.push_back(fmt[i]);
      continue;
    }
    if (++i >= fmt.size()) rt.throw_new("Ljava/util/UnknownFormatConversionException;", "%");
    bool left = false, zero = false;
    while (i < fmt.size() && (fmt[i] == u'-' || fmt[i] == u'0')) {
      (fmt[i] == u'-' ? left : zero) = true;
      ++i;
    }
    int width = 0, precision = -1;
    while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9') width = width * 10 + (fmt[i++] - u'0');
    if (i < fmt.size() && fmt[i] == u'.') {
      precision = 0;
      ++i;
      while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9') precision = precision * 10 + (fmt[i++] - u'0');
    }
    if (i >= fmt.size()) break;
    char16_t conv = fmt[i];
    if (conv == u'%') {
      out.push_back(u'%');
      continue;
    }
    if (conv == u'n') {
      out.push_back(u'\n');
      continue;
    }
    if (next_arg >= args.size()) rt.throw_new("Ljava/util/MissingFormatArgumentException;", std::string("Format specifier '%") + char(conv) + "'");
    Handle arg = args[next_arg++];
    std::u16string piece;
    char t = 0;
    uint64_t bits = 0;
    bool boxed = unbox(rt, arg, t, bits);
    switch (conv) {
      case u's':
      case u'S':
        piece = str_of(rt, arg);
        if (precision >= 0 && piece.size() > size_t(precision)) piece.resize(size_t(precision));
        if (conv == u'S') {
          for (auto& c : piece) c = rt::java_to_upper(c);
        }
        break;
      case u'd':
        if (!boxed || t == 'F' || t == 'D' || t == 'Z' || t == 'C') rt.throw_new("Ljava/util/IllegalFormatConversionException;", "d");
        piece = rt::u16(std::to_string(as_long(t, bits)));
        break;
      case u'x':
      case u'X': {
        if (!boxed || t == 'F' || t == 'D') rt.throw_new("Ljava/util/IllegalFormatConversionException;", "x");
        char buf[32];
        uint64_t v = t == 'J' ? bits : uint64_t(uint32_t(bits));
        std::snprintf(buf, sizeof buf, conv == u'x' ? "%llx" : "%llX", static_cast<unsigned long long>(v));
        piece = rt::u16(buf);
        break;
      }
      case u'c': piece = boxed ? std::u16string(1, char16_t(bits)) : u"null"; break;
      case u'b': piece = arg == kNull ? u"false" : (boxed && t == 'Z' ? (bits ? u"true" : u"false") : u"true"); break;
      case u'f':
      case u'e': {
        if (!boxed) rt.throw_new("Ljava/util/IllegalFormatConversionException;", "f");
        char buf[64];
        std::snprintf(buf, sizeof buf, conv == u'f' ? "%.*f" : "%.*e", precision < 0 ? 6 : precision, as_double(t, bits));
        piece = rt::u16(buf);
        break;
      }
      default:
        rt.throw_unsupported("Ljava/lang/String;->format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",
                             std::string("conversion %") + char(conv) + " non prise en charge");
    }
    if (int(piece.size()) < width) {
      std::u16string pad(size_t(width) - piece.size(), zero && !left ? u'0' : u' ');
      if (left) {
        piece += pad;
      } else if (zero && !piece.empty() && piece[0] == u'-') {
        piece = u"-" + pad + piece.substr(1);
      } else {
        piece = pad + piece;
      }
    }
    out += piece;
  }
  return out;
}

void register_java_lang(rt::NativeRegistry& r, Host& host) {
  r.define_interface("Ljava/lang/AutoCloseable;").abstract_method("close", "()V");
  r.define_interface("Ljava/lang/Iterable;").abstract_method("iterator", "()Ljava/util/Iterator;");
  register_string_builder(r, "Ljava/lang/StringBuilder;");
  register_string_builder(r, "Ljava/lang/StringBuffer;");
  register_boxes(r);
  register_math(r);
  register_system(r, host);
  register_enum(r);
  r.define("Ljava/util/UnknownFormatConversionException;", "Ljava/lang/IllegalArgumentException;");
  r.define("Ljava/util/MissingFormatArgumentException;", "Ljava/lang/IllegalArgumentException;");
  r.define("Ljava/util/IllegalFormatConversionException;", "Ljava/lang/IllegalArgumentException;");
  r.extend("Ljava/lang/String;")
      .static_method("format", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",
                     [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.new_string(java_format(rt, a.str(0), a.h(1))); },
                     S::kPartial, "sous-ensemble des formats (%s %d %x %c %b %f %n), sans Locale");
}

}  // namespace aa::bridge
