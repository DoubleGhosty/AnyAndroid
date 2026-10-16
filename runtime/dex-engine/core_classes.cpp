// java.lang classes the runtime depends on: Object, String, Class and the
// Throwable hierarchy. The framework bridge extends this set.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/log.h"
#include "common/text.h"
#include "common/unicode.h"
#include "dex-engine/jlang.h"
#include "dex-engine/runtime.h"

namespace aa::rt {

std::u16string u16(std::string_view utf8) { return text::utf8_to_utf16(utf8); }

std::string java_fp_to_string(double v, bool is_float) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
  if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
  // Shortest scientific representation that round-trips.
  char buf[64];
  int max_digits = is_float ? 9 : 17;
  for (int p = 1; p <= max_digits; ++p) {
    std::snprintf(buf, sizeof buf, "%.*e", p - 1, v);
    double back = std::strtod(buf, nullptr);
    if (is_float ? float(back) == float(v) : back == v) break;
  }
  std::string s = buf;
  bool negative = s[0] == '-';
  if (negative) s.erase(0, 1);
  size_t e = s.find('e');
  int exponent = std::atoi(s.c_str() + e + 1);
  std::string digits;
  for (size_t i = 0; i < e; ++i) {
    if (s[i] != '.') digits.push_back(s[i]);
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  double a = std::fabs(v);
  std::string out;
  if (a >= 1e-3 && a < 1e7) {
    if (exponent >= 0) {
      std::string int_part = digits.substr(0, std::min<size_t>(digits.size(), size_t(exponent) + 1));
      while (int_part.size() < size_t(exponent) + 1) int_part.push_back('0');
      std::string frac = digits.size() > size_t(exponent) + 1 ? digits.substr(size_t(exponent) + 1) : "0";
      out = int_part + "." + frac;
    } else {
      out = "0." + std::string(size_t(-exponent - 1), '0') + digits;
    }
  } else {
    out = digits.substr(0, 1) + "." + (digits.size() > 1 ? digits.substr(1) : "0") + "E" + std::to_string(exponent);
  }
  return negative ? "-" + out : out;
}

int32_t java_string_hash(std::u16string_view s) {
  uint32_t h = 0;
  for (char16_t c : s) h = 31 * h + c;
  return int32_t(h);
}

char16_t java_to_upper(char16_t c) { return char16_t(unicode::to_upper(c)); }

char16_t java_to_lower(char16_t c) { return char16_t(unicode::to_lower(c)); }

namespace {

using S = ApiStatus;

StringObject& self_string(Runtime& rt, NativeArgs& a) {
  StringObject* s = rt.as_string(a.h(0));
  if (!s) rt.throw_npe("String");
  return *s;
}

[[noreturn]] void string_index_error(Runtime& rt, int64_t index, size_t length) {
  rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;",
               "index " + std::to_string(index) + ", length " + std::to_string(length));
}

Handle string_array(Runtime& rt, const std::vector<std::u16string>& parts) {
  Handle arr = rt.new_array("[Ljava/lang/String;", int32_t(parts.size()));
  ArrayObject& a = rt.array(arr, "split");
  for (size_t i = 0; i < parts.size(); ++i) {
    Handle h = rt.new_string(parts[i]);
    std::memcpy(&a.data[i * 4], &h, 4);
  }
  return arr;
}

void init_throwable(Runtime& rt, Handle self, const std::u16string* message, Handle cause) {
  Object& o = rt.nonnull(self, "Throwable");
  auto p = std::make_unique<ThrowablePeer>();
  if (message) {
    p->message = *message;
    p->has_message = true;
  }
  p->cause = cause;
  p->stack = rt.capture_stack();
  o.peer = std::move(p);
}

ThrowablePeer& throwable_peer(Runtime& rt, Handle h) {
  Object& o = rt.nonnull(h, "Throwable");
  if (!o.peer) o.peer = std::make_unique<ThrowablePeer>();  // constructor not called
  auto* p = dynamic_cast<ThrowablePeer*>(o.peer.get());
  if (!p) rt.throw_new("Ljava/lang/InternalError;", "état natif de Throwable invalide");
  return *p;
}

// A shallow copy of a reference array (Object.clone).
Handle clone_array(Runtime& rt, Handle src, const char* descriptor) {
  uint32_t n = rt.array(src, "array").length;
  Handle copy = rt.new_array(descriptor, int32_t(n));
  std::memcpy(rt.array(copy, "array").data.data(), rt.array(src, "array").data.data(), size_t(n) * 4);
  return copy;
}

// The throwable's StackTraceElement[], built from the captured frames
// ("pkg.Class.method(pc 0x0012)" or "...(Native Method)": no file or line
// numbers, -2 marking native methods as in StackTraceElement).
Handle our_stack_trace(Runtime& rt, Handle self) {
  if (Handle known = throwable_peer(rt, self).stack_trace) return known;
  Class* element = rt.lookup_class("Ljava/lang/StackTraceElement;");
  Method* ctor =
      element ? element->declared_method("<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V") : nullptr;
  if (!ctor) rt.throw_unsupported("Ljava/lang/Throwable;->getStackTrace()[Ljava/lang/StackTraceElement;", "sans la libcore");
  rt.ensure_initialized(element);
  std::vector<std::string> lines = throwable_peer(rt, self).stack;
  GlobalRef arr(rt, rt.new_array("[Ljava/lang/StackTraceElement;", int32_t(lines.size())));
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    size_t paren = line.find('(');
    size_t dot = line.rfind('.', paren);
    std::string cls = dot == std::string::npos ? std::string() : line.substr(0, dot);
    std::string method = line.substr(dot == std::string::npos ? 0 : dot + 1, paren - (dot == std::string::npos ? 0 : dot + 1));
    bool native = line.find("(Native Method)") != std::string::npos;
    GlobalRef e(rt, rt.alloc(element));
    GlobalRef c(rt, rt.new_string_utf8(cls));
    Handle m = rt.new_string_utf8(method);
    uint32_t args[5] = {e.get(), c.get(), m, kNull, uint32_t(native ? -2 : -1)};
    rt.invoke(ctor, args, 5);
    Handle h = e.get();
    std::memcpy(&rt.array(arr.get(), "stackTrace").data[i * 4], &h, 4);
  }
  throwable_peer(rt, self).stack_trace = arr.get();
  return arr.get();
}

void add_throwable_ctors(ClassBuilder& b) {
  b.ctor("()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    init_throwable(rt, a.h(0), nullptr, kNull);
    return 0;
  });
  b.ctor("(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string msg = a.h(1) ? rt.string_value(a.h(1)) : u"";
    init_throwable(rt, a.h(0), a.h(1) ? &msg : nullptr, kNull);
    return 0;
  });
  b.ctor("(Ljava/lang/String;Ljava/lang/Throwable;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string msg = a.h(1) ? rt.string_value(a.h(1)) : u"";
    init_throwable(rt, a.h(0), a.h(1) ? &msg : nullptr, a.h(2));
    return 0;
  });
  b.ctor("(Ljava/lang/Throwable;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string msg;
    if (a.h(1)) msg = rt.string_value(a.h(1));
    init_throwable(rt, a.h(0), a.h(1) ? &msg : nullptr, a.h(1));
    return 0;
  });
}

void register_object(NativeRegistry& r) {
  r.define("Ljava/lang/Object;", "")
      .ctor("()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; })
      .method("toString", "()Ljava/lang/String;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Object& o = a.self();
                auto hc = uint32_t(rt.call_virtual(a.h(0), "hashCode", "()I"));
                char buf[16];
                std::snprintf(buf, sizeof buf, "@%x", hc);
                return rt.new_string_utf8(o.klass->java_name() + buf);
              })
      .method("hashCode", "()I", [](Runtime&, NativeArgs& a) -> uint64_t { return a.raw(0); })
      // ART: private static native, called by System.identityHashCode().
      .static_method("identityHashCode", "(Ljava/lang/Object;)I", [](Runtime&, NativeArgs& a) -> uint64_t { return a.raw(0); })
      .method("equals", "(Ljava/lang/Object;)Z",
              [](Runtime&, NativeArgs& a) -> uint64_t { return ret_bool(a.h(0) == a.h(1)); })
      .method("getClass", "()Ljava/lang/Class;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.class_mirror(a.self().klass); })
      .method("clone", "()Ljava/lang/Object;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Object& o = a.self();
                if (auto* arr = dynamic_cast<ArrayObject*>(&o)) {
                  Handle h = rt.new_array(arr->klass, int32_t(arr->length));
                  rt.array(h, "clone").data = arr->data;
                  return h;
                }
                if (o.peer || !rt.instance_of(a.h(0), rt.find_class("Ljava/lang/Cloneable;"))) {
                  rt.throw_new("Ljava/lang/CloneNotSupportedException;", o.klass->java_name());
                }
                Handle h = rt.alloc(o.klass);
                rt.object(h)->fields = o.fields;
                return h;
              })
      .method("notify", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt.monitor_notify(a.h(0), false);
        return 0;
      })
      .method("notifyAll", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt.monitor_notify(a.h(0), true);
        return 0;
      })
      .method("wait", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt.monitor_wait(a.h(0), 0);
        return 0;
      })
      .method("wait", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt.monitor_wait(a.h(0), a.i64(1));
        return 0;
      })
      .method("wait", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt.monitor_wait(a.h(0), a.i64(1) + (a.i32(3) > 0 ? 1 : 0));
        return 0;
      })
      .stub("finalize", "()V", "finaliseurs jamais appelés par le ramasse-miettes");

  r.define_interface("Ljava/lang/Cloneable;");
  r.define_interface("Ljava/io/Serializable;");
  r.define_interface("Ljava/lang/Comparable;").abstract_method("compareTo", "(Ljava/lang/Object;)I");
  r.define_interface("Ljava/lang/CharSequence;")
      .abstract_method("length", "()I")
      .abstract_method("charAt", "(I)C")
      .abstract_method("toString", "()Ljava/lang/String;")
      .abstract_method("subSequence", "(II)Ljava/lang/CharSequence;");
}

void register_string(NativeRegistry& r) {
  auto b = r.define("Ljava/lang/String;", "Ljava/lang/Object;",
                    {"Ljava/io/Serializable;", "Ljava/lang/Comparable;", "Ljava/lang/CharSequence;"},
                    0x1 | 0x10);
  // ART's layout, read directly by libcore: count = (length << 1) | coder,
  // maintained by Runtime::set_string_value; hash is a cache.
  b.field("count", "I", 0x2 | 0x10);
  b.field("hash", "I", 0x2);
  b.ctor("()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt.set_string_value(self_string(rt, a), u"");
    return 0;
  });
  b.ctor("(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt.set_string_value(self_string(rt, a), a.str(1));
    return 0;
  });
  b.ctor("([C)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ArrayObject& arr = rt.array(a.h(1), "String(char[])");
    std::u16string v(arr.length, u'\0');
    std::memcpy(v.data(), arr.data.data(), arr.data.size());
    rt.set_string_value(self_string(rt, a), std::move(v));
    return 0;
  });
  b.ctor("([CII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ArrayObject& arr = rt.array(a.h(1), "String(char[], int, int)");
    int32_t off = a.i32(2), count = a.i32(3);
    if (off < 0 || count < 0 || int64_t(off) + count > arr.length) string_index_error(rt, off, arr.length);
    std::u16string v(size_t(count), u'\0');
    std::memcpy(v.data(), arr.data.data() + size_t(off) * 2, size_t(count) * 2);
    rt.set_string_value(self_string(rt, a), std::move(v));
    return 0;
  });
  b.ctor("([B)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ArrayObject& arr = rt.array(a.h(1), "String(byte[])");
    rt.set_string_value(self_string(rt, a), text::utf8_to_utf16(std::string(arr.data.begin(), arr.data.end())));
    return 0;
  }, S::kPartial, "décodage UTF-8 uniquement");
  b.method("length", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_int(int32_t(self_string(rt, a).value.size()));
  });
  b.method("isEmpty", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_bool(self_string(rt, a).value.empty());
  });
  b.method("charAt", "(I)C", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= v.size()) string_index_error(rt, i, v.size());
    return v[size_t(i)];
  });
  b.method("toString", "()Ljava/lang/String;", [](Runtime&, NativeArgs& a) -> uint64_t { return a.h(0); });
  b.method("hashCode", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_int(java_string_hash(self_string(rt, a).value));
  });
  b.method("equals", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    StringObject* other = rt.as_string(a.h(1));
    return ret_bool(other && other->value == self_string(rt, a).value);
  });
  b.method("equalsIgnoreCase", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    StringObject* other = rt.as_string(a.h(1));
    const auto& v = self_string(rt, a).value;
    if (!other || other->value.size() != v.size()) return 0;
    for (size_t i = 0; i < v.size(); ++i) {
      if (java_to_lower(v[i]) != java_to_lower(other->value[i])) return 0;
    }
    return 1;
  }, S::kPartial, "casse ASCII et Latin-1 uniquement");
  auto compare = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& x = self_string(rt, a).value;
    StringObject* y = rt.as_string(a.h(1));
    if (!y) {
      if (a.h(1) == kNull) rt.throw_npe("compareTo(null)");
      rt.throw_new("Ljava/lang/ClassCastException;", "compareTo avec un objet qui n'est pas une chaîne");
    }
    size_t n = std::min(x.size(), y->value.size());
    for (size_t i = 0; i < n; ++i) {
      if (x[i] != y->value[i]) return ret_int(int32_t(x[i]) - int32_t(y->value[i]));
    }
    return ret_int(int32_t(x.size()) - int32_t(y->value.size()));
  };
  b.method("compareTo", "(Ljava/lang/String;)I", compare);
  b.method("compareTo", "(Ljava/lang/Object;)I", compare);
  b.method("concat", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(self_string(rt, a).value + a.str(1));
  });
  auto substring = [](Runtime& rt, NativeArgs& a, int32_t begin, int32_t end) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    if (begin < 0 || end > int32_t(v.size()) || begin > end) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;",
                   "begin " + std::to_string(begin) + ", end " + std::to_string(end) + ", length " +
                       std::to_string(v.size()));
    }
    return rt.new_string(v.substr(size_t(begin), size_t(end - begin)));
  };
  b.method("substring", "(I)Ljava/lang/String;", [substring](Runtime& rt, NativeArgs& a) -> uint64_t {
    return substring(rt, a, a.i32(1), int32_t(self_string(rt, a).value.size()));
  });
  b.method("substring", "(II)Ljava/lang/String;", [substring](Runtime& rt, NativeArgs& a) -> uint64_t {
    return substring(rt, a, a.i32(1), a.i32(2));
  });
  b.method("subSequence", "(II)Ljava/lang/CharSequence;", [substring](Runtime& rt, NativeArgs& a) -> uint64_t {
    return substring(rt, a, a.i32(1), a.i32(2));
  });
  // Package-private helpers that libcore (AbstractStringBuilder...) calls.
  b.method("fastSubstring", "(II)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    int32_t start = a.i32(1), len = a.i32(2);
    if (start < 0 || len < 0 || int64_t(start) + len > int64_t(v.size())) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "fastSubstring");
    }
    return rt.new_string(v.substr(size_t(start), size_t(len)));
  });
  auto get_chars = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    int32_t begin = a.i32(1), end = a.i32(2), dst_begin = a.i32(4);
    ArrayObject& dst = rt.array(a.h(3), "dst");
    if (begin < 0 || end > int32_t(v.size()) || begin > end || dst_begin < 0 ||
        int64_t(dst_begin) + (end - begin) > dst.length || dst.width != 2) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "getChars");
    }
    if (end > begin) std::memcpy(dst.data.data() + size_t(dst_begin) * 2, v.data() + begin, size_t(end - begin) * 2);
    return 0;
  };
  b.method("getChars", "(II[CI)V", get_chars);
  b.method("getCharsNoCheck", "(II[CI)V", get_chars);
  b.method("indexOf", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = self_string(rt, a).value.find(char16_t(a.i32(1)));
    return ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  }, S::kPartial, "caractères du plan multilingue de base uniquement");
  b.method("indexOf", "(II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    int32_t from = std::max(0, a.i32(2));
    auto p = size_t(from) >= v.size() ? std::u16string::npos : v.find(char16_t(a.i32(1)), size_t(from));
    return ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  }, S::kPartial, "caractères du plan multilingue de base uniquement");
  b.method("indexOf", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = self_string(rt, a).value.find(a.str(1));
    return ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  });
  b.method("indexOf", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    size_t from = size_t(std::max(0, a.i32(2)));
    auto p = from > v.size() ? std::u16string::npos : v.find(a.str(1), from);
    return ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  });
  b.method("lastIndexOf", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = self_string(rt, a).value.rfind(char16_t(a.i32(1)));
    return ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  }, S::kPartial, "caractères du plan multilingue de base uniquement");
  b.method("lastIndexOf", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = self_string(rt, a).value.rfind(a.str(1));
    return ret_int(p == std::u16string::npos ? -1 : int32_t(p));
  });
  b.method("contains", "(Ljava/lang/CharSequence;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_bool(self_string(rt, a).value.find(a.str(1)) != std::u16string::npos);
  });
  b.method("startsWith", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_bool(self_string(rt, a).value.starts_with(a.str(1)));
  });
  b.method("startsWith", "(Ljava/lang/String;I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    int32_t off = a.i32(2);
    if (off < 0 || size_t(off) > v.size()) return 0;
    return ret_bool(std::u16string_view(v).substr(size_t(off)).starts_with(a.str(1)));
  });
  b.method("endsWith", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_bool(self_string(rt, a).value.ends_with(a.str(1)));
  });
  b.method("trim", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    size_t s = 0, e = v.size();
    while (s < e && v[s] <= u' ') ++s;
    while (e > s && v[e - 1] <= u' ') --e;
    if (s == 0 && e == v.size()) return a.h(0);
    return rt.new_string(v.substr(s, e - s));
  });
  b.method("toUpperCase", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string v = self_string(rt, a).value;
    for (auto& c : v) c = java_to_upper(c);
    return rt.new_string(std::move(v));
  }, S::kPartial, "casse ASCII et Latin-1 uniquement");
  b.method("toLowerCase", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string v = self_string(rt, a).value;
    for (auto& c : v) c = java_to_lower(c);
    return rt.new_string(std::move(v));
  }, S::kPartial, "casse ASCII et Latin-1 uniquement");
  b.method("replace", "(CC)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string v = self_string(rt, a).value;
    std::replace(v.begin(), v.end(), char16_t(a.raw(1)), char16_t(a.raw(2)));
    return rt.new_string(std::move(v));
  });
  b.method("replace", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;",
           [](Runtime& rt, NativeArgs& a) -> uint64_t {
             const auto& v = self_string(rt, a).value;
             std::u16string from = a.str(1), to = a.str(2), out;
             if (from.empty()) {
               for (char16_t c : v) out += to + c;
               out += to;
               return rt.new_string(std::move(out));
             }
             size_t pos = 0;
             while (true) {
               size_t p = v.find(from, pos);
               if (p == std::u16string::npos) break;
               out += v.substr(pos, p - pos) + to;
               pos = p + from.size();
             }
             out += v.substr(pos);
             return rt.new_string(std::move(out));
           });
  b.method("split", "(Ljava/lang/String;)[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    std::u16string sep = a.str(1);
    // Literal separators only (Java's own fast path); real regexes are refused.
    if (sep.size() == 2 && sep[0] == u'\\' && !std::isalnum(int(sep[1] & 0x7f))) sep = sep.substr(1);
    else if (sep.find_first_of(u".$|()[{^?*+\\") != std::u16string::npos || sep.empty()) {
      if (rt.lookup_class("Ljava/util/regex/Pattern;")) {  // libcore mode: Pattern.compile(regex).split(this)
        GlobalRef pattern(rt, Handle(rt.call_static("Ljava/util/regex/Pattern;", "compile",
                                                    "(Ljava/lang/String;)Ljava/util/regex/Pattern;", {a.raw(1)})));
        return rt.call_virtual(pattern.get(), "split", "(Ljava/lang/CharSequence;)[Ljava/lang/String;", {a.raw(0)});
      }
      rt.throw_unsupported("Ljava/lang/String;->split(Ljava/lang/String;)[Ljava/lang/String;",
                           "expressions régulières non prises en charge (séparateurs littéraux uniquement)");
    }
    std::vector<std::u16string> parts;
    if (v.empty()) return string_array(rt, {v});
    size_t pos = 0;
    while (true) {
      size_t p = v.find(sep, pos);
      if (p == std::u16string::npos) break;
      parts.push_back(v.substr(pos, p - pos));
      pos = p + sep.size();
    }
    parts.push_back(v.substr(pos));
    while (!parts.empty() && parts.back().empty()) parts.pop_back();
    return string_array(rt, parts);
  }, S::kPartial, "séparateurs littéraux uniquement");
  b.method("toCharArray", "()[C", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const auto& v = self_string(rt, a).value;
    Handle h = rt.new_array("[C", int32_t(v.size()));
    std::memcpy(rt.array(h, "toCharArray").data.data(), v.data(), v.size() * 2);
    return h;
  });
  b.method("getBytes", "()[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string u = text::utf16_to_utf8(self_string(rt, a).value);
    Handle h = rt.new_array("[B", int32_t(u.size()));
    std::memcpy(rt.array(h, "getBytes").data.data(), u.data(), u.size());
    return h;
  }, S::kPartial, "encodage UTF-8 uniquement");
  b.method("intern", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.intern(self_string(rt, a).value);
  });
  b.method("repeat", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t n = a.i32(1);
    if (n < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "count is negative: " + std::to_string(n));
    const auto& v = self_string(rt, a).value;
    if (uint64_t(n) * v.size() > (16u << 20)) rt.throw_new("Ljava/lang/OutOfMemoryError;", "chaîne trop longue");
    std::u16string out;
    for (int32_t i = 0; i < n; ++i) out += v;
    return rt.new_string(std::move(out));
  });
  b.static_method("valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) return rt.intern(u"null");
    return rt.call_virtual(a.h(0), "toString", "()Ljava/lang/String;");
  });
  b.static_method("valueOf", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(std::to_string(a.i32(0)));
  });
  b.static_method("valueOf", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(std::to_string(a.i64(0)));
  });
  b.static_method("valueOf", "(Z)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.intern(a.z(0) ? u"true" : u"false");
  });
  b.static_method("valueOf", "(C)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(std::u16string(1, char16_t(a.raw(0))));
  });
  b.static_method("valueOf", "(F)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(java_fp_to_string(a.f32(0), true));
  });
  b.static_method("valueOf", "(D)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(java_fp_to_string(a.f64(0), false));
  });
  b.static_method("valueOf", "([C)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ArrayObject& arr = rt.array(a.h(0), "valueOf(char[])");
    std::u16string v(arr.length, u'\0');
    std::memcpy(v.data(), arr.data.data(), arr.data.size());
    return rt.new_string(std::move(v));
  });
  // The regex methods are libcore's: java.util.regex when it is loaded
  // (libcore mode), refused otherwise (bridge mode has no regex engine).
  auto regex = [](Runtime& rt, const char* method) {
    if (!rt.lookup_class("Ljava/util/regex/Pattern;")) {
      rt.throw_unsupported(std::string("Ljava/lang/String;->") + method, "expressions régulières non prises en charge");
    }
  };
  b.method("matches", "(Ljava/lang/String;)Z", [regex](Runtime& rt, NativeArgs& a) -> uint64_t {
    regex(rt, "matches(Ljava/lang/String;)Z");
    return rt.call_static("Ljava/util/regex/Pattern;", "matches", "(Ljava/lang/String;Ljava/lang/CharSequence;)Z",
                          {a.raw(1), a.raw(0)});
  });
  for (const char* name : {"replaceAll", "replaceFirst"}) {
    std::string method = std::string(name) + "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;";
    b.method(name, "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
             [regex, name, method](Runtime& rt, NativeArgs& a) -> uint64_t {
               regex(rt, method.c_str());
               // Pattern.compile(regex).matcher(this).replaceAll/replaceFirst(replacement)
               GlobalRef pattern(rt, Handle(rt.call_static("Ljava/util/regex/Pattern;", "compile",
                                                           "(Ljava/lang/String;)Ljava/util/regex/Pattern;", {a.raw(1)})));
               GlobalRef matcher(rt, Handle(rt.call_virtual(pattern.get(), "matcher",
                                                            "(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",
                                                            {a.raw(0)})));
               return rt.call_virtual(matcher.get(), name, "(Ljava/lang/String;)Ljava/lang/String;", {a.raw(2)});
             });
  }
}

void register_class(NativeRegistry& r) {
  r.define("Ljava/lang/Class;", "Ljava/lang/Object;", {"Ljava/io/Serializable;"}, 0x1 | 0x10)
      // ART's lazily allocated extension, read directly by libcore (ClassValue).
      .field("extData", "Ldalvik/system/ClassExt;", 0x2)
      .method("getName", "()Ljava/lang/String;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                return rt.new_string_utf8(c ? c->java_name() : "?");
              })
      .method("getSimpleName", "()Ljava/lang/String;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                return rt.new_string_utf8(c ? c->simple_name() : "?");
              })
      .method("toString", "()Ljava/lang/String;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                if (!c) return rt.intern(u"class ?");
                if (c->primitive) return rt.new_string_utf8(c->java_name());
                return rt.new_string_utf8((c->is_interface() ? "interface " : "class ") + c->java_name());
              })
      .method("desiredAssertionStatus", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; })
      .method("isInstance", "(Ljava/lang/Object;)Z",
              [](Runtime& rt, NativeArgs& a) -> uint64_t { return ret_bool(rt.instance_of(a.h(1), rt.mirror_class(a.h(0)))); })
      .method("isArray", "()Z",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                return ret_bool(c && c->is_array());
              })
      .method("isInterface", "()Z",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                return ret_bool(c && c->is_interface());
              })
      .method("getSuperclass", "()Ljava/lang/Class;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                return c && c->super && !c->is_interface() ? rt.class_mirror(c->super) : kNull;
              })
      .method("newInstance", "()Ljava/lang/Object;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                Class* c = rt.mirror_class(a.h(0));
                if (!c) rt.throw_npe("Class");
                rt.ensure_initialized(c);
                Method* ctor = c->declared_method("<init>", "()V");
                if (!ctor) rt.throw_new("Ljava/lang/InstantiationException;", c->java_name());
                Handle h = rt.alloc(c);
                uint32_t args[1] = {h};
                rt.invoke(ctor, args, 1);
                return h;
              },
              S::kPartial, "constructeur sans argument uniquement, sans contrôle d'accès");
}

void register_throwables(NativeRegistry& r) {
  auto t = r.define("Ljava/lang/Throwable;", "Ljava/lang/Object;", {"Ljava/io/Serializable;"});
  add_throwable_ctors(t);
  t.method("getMessage", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThrowablePeer& p = throwable_peer(rt, a.h(0));
    return p.has_message ? rt.new_string(p.message) : kNull;
  });
  t.method("getLocalizedMessage", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.call_virtual(a.h(0), "getMessage", "()Ljava/lang/String;");
  });
  t.method("getCause", "()Ljava/lang/Throwable;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return throwable_peer(rt, a.h(0)).cause;
  });
  t.method("initCause", "(Ljava/lang/Throwable;)Ljava/lang/Throwable;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    throwable_peer(rt, a.h(0)).cause = a.h(1);
    return a.h(0);
  });
  t.method("toString", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = a.self().klass->java_name();
    Handle msg = Handle(rt.call_virtual(a.h(0), "getLocalizedMessage", "()Ljava/lang/String;"));
    if (msg == kNull) return rt.new_string_utf8(name);
    return rt.new_string(text::utf8_to_utf16(name + ": ") + rt.string_value(msg));
  });
  t.method("printStackTrace", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    log::warn("app", "printStackTrace :\n" + rt.describe_throwable(a.h(0)));
    return 0;
  }, S::kPartial, "écrit dans le journal AnyAndroid");
  t.method("addSuppressed", "(Ljava/lang/Throwable;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(1) == a.h(0)) rt.throw_new("Ljava/lang/IllegalArgumentException;", "Self-suppression not permitted");
    if (a.h(1) == kNull) rt.throw_npe("Cannot suppress a null exception.");
    throwable_peer(rt, a.h(0)).suppressed.push_back(a.h(1));
    return 0;
  });
  t.method("getSuppressed", "()[Ljava/lang/Throwable;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<Handle> items = throwable_peer(rt, a.h(0)).suppressed;
    Handle arr = rt.new_array("[Ljava/lang/Throwable;", int32_t(items.size()));
    std::memcpy(rt.array(arr, "suppressed").data.data(), items.data(), items.size() * 4);
    return arr;
  });
  // The stack as StackTraceElement objects (libcore's class): built once from
  // the captured frames, unless setStackTrace replaced it. getOurStackTrace
  // is the private one libcore's printStackTrace uses.
  t.method("getOurStackTrace", "()[Ljava/lang/StackTraceElement;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return our_stack_trace(rt, a.h(0));
  });
  t.method("getStackTrace", "()[Ljava/lang/StackTraceElement;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return clone_array(rt, our_stack_trace(rt, a.h(0)), "[Ljava/lang/StackTraceElement;");
  });
  t.method("setStackTrace", "([Ljava/lang/StackTraceElement;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(1) == kNull) rt.throw_npe("stackTrace");
    Handle copy = clone_array(rt, a.h(1), "[Ljava/lang/StackTraceElement;");
    ArrayObject& arr = rt.array(copy, "stackTrace");
    for (uint32_t i = 0; i < arr.length; ++i) {
      Handle e;
      std::memcpy(&e, &arr.data[size_t(i) * 4], 4);
      if (e == kNull) rt.throw_npe("stackTrace[" + std::to_string(i) + "]");
    }
    throwable_peer(rt, a.h(0)).stack_trace = copy;
    return 0;
  });
  t.method("fillInStackTrace", "()Ljava/lang/Throwable;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThrowablePeer& p = throwable_peer(rt, a.h(0));
    p.stack = rt.capture_stack();
    // Without this native frame and those of fillInStackTrace overrides.
    while (!p.stack.empty() && p.stack.front().find(".fillInStackTrace(") != std::string::npos) p.stack.erase(p.stack.begin());
    p.stack_trace = kNull;
    return a.h(0);
  });

  struct Def {
    const char* cls;
    const char* super;
  };
  static const Def defs[] = {
      {"Ljava/lang/Exception;", "Ljava/lang/Throwable;"},
      {"Ljava/lang/Error;", "Ljava/lang/Throwable;"},
      {"Ljava/lang/RuntimeException;", "Ljava/lang/Exception;"},
      {"Ljava/lang/NullPointerException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/ArithmeticException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/IndexOutOfBoundsException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/ArrayIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;"},
      {"Ljava/lang/StringIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;"},
      {"Ljava/lang/ClassCastException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/ArrayStoreException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/NegativeArraySizeException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/IllegalArgumentException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/NumberFormatException;", "Ljava/lang/IllegalArgumentException;"},
      {"Ljava/lang/IllegalStateException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/UnsupportedOperationException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/SecurityException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/util/NoSuchElementException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/util/ConcurrentModificationException;", "Ljava/lang/RuntimeException;"},
      {"Ljava/lang/CloneNotSupportedException;", "Ljava/lang/Exception;"},
      {"Ljava/lang/InterruptedException;", "Ljava/lang/Exception;"},
      {"Ljava/lang/ReflectiveOperationException;", "Ljava/lang/Exception;"},
      {"Ljava/lang/ClassNotFoundException;", "Ljava/lang/ReflectiveOperationException;"},
      {"Ljava/lang/InstantiationException;", "Ljava/lang/ReflectiveOperationException;"},
      {"Ljava/io/IOException;", "Ljava/lang/Exception;"},
      {"Ljava/lang/LinkageError;", "Ljava/lang/Error;"},
      {"Ljava/lang/NoClassDefFoundError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/ClassCircularityError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/ClassFormatError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/VerifyError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/UnsatisfiedLinkError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/ExceptionInInitializerError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/IncompatibleClassChangeError;", "Ljava/lang/LinkageError;"},
      {"Ljava/lang/NoSuchMethodError;", "Ljava/lang/IncompatibleClassChangeError;"},
      {"Ljava/lang/NoSuchFieldError;", "Ljava/lang/IncompatibleClassChangeError;"},
      {"Ljava/lang/AbstractMethodError;", "Ljava/lang/IncompatibleClassChangeError;"},
      {"Ljava/lang/InstantiationError;", "Ljava/lang/IncompatibleClassChangeError;"},
      {"Ljava/lang/VirtualMachineError;", "Ljava/lang/Error;"},
      {"Ljava/lang/StackOverflowError;", "Ljava/lang/VirtualMachineError;"},
      {"Ljava/lang/OutOfMemoryError;", "Ljava/lang/VirtualMachineError;"},
      {"Ljava/lang/InternalError;", "Ljava/lang/VirtualMachineError;"},
  };
  for (const auto& d : defs) {
    auto b = r.define(d.cls, d.super);
    add_throwable_ctors(b);
  }
  auto ae = r.define("Ljava/lang/AssertionError;", "Ljava/lang/Error;");
  add_throwable_ctors(ae);
  ae.ctor("(Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string msg = a.h(1) ? rt.string_value(a.h(1)) : u"null";
    init_throwable(rt, a.h(0), &msg, kNull);
    return 0;
  });
}

ThreadPeer& thread_peer(Runtime& rt, Handle h) {
  Object& o = rt.nonnull(h, "Thread");
  auto* p = dynamic_cast<ThreadPeer*>(o.peer.get());
  if (!p) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Thread non initialisé");
  return *p;
}

void register_thread(NativeRegistry& r) {
  r.define_interface("Ljava/lang/Thread$UncaughtExceptionHandler;")
      .abstract_method("uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V");
  r.define("Ljava/lang/IllegalThreadStateException;", "Ljava/lang/IllegalArgumentException;");
  r.define("Ljava/lang/IllegalMonitorStateException;", "Ljava/lang/RuntimeException;");

  auto t = r.define("Ljava/lang/Thread;", "Ljava/lang/Object;", {"Ljava/lang/Runnable;"});
  // Read and written by libcore's Thread.blockedOn (interruptible channels: FileChannel...);
  // blockerLock is set for every Thread object (init_thread_object).
  t.field("blocker", "Lsun/nio/ch/Interruptible;");
  t.field("blockerLock", "Ljava/lang/Object;");
  // Read and written directly by libcore's ThreadLocal / InheritableThreadLocal.
  t.field("threadLocals", "Ljava/lang/ThreadLocal$ThreadLocalMap;");
  t.field("inheritableThreadLocals", "Ljava/lang/ThreadLocal$ThreadLocalMap;");
  // Read and written by libcore's get/setContextClassLoader (java.util.logging, JCA providers).
  t.field("contextClassLoader", "Ljava/lang/ClassLoader;");
  // Read by libcore's Runtime.addShutdownHook; mirrors ThreadPeer::started.
  t.field("started", "Z");
  auto init = [](Runtime& rt, NativeArgs& a, Handle target, std::u16string name) -> uint64_t {
    auto p = std::make_unique<ThreadPeer>();
    p->target = target;
    if (name.empty()) {
      static std::atomic<int> counter{0};
      name = u16("Thread-" + std::to_string(counter++));
    }
    p->name = std::move(name);
    if (auto* cur = rt.peer<ThreadPeer>(rt.current_thread_peer())) p->daemon = cur->daemon;
    a.self().peer = std::move(p);
    init_thread_object(rt, a.h(0));
    return 0;
  };
  t.ctor("()V", [init](Runtime& rt, NativeArgs& a) { return init(rt, a, kNull, u""); });
  t.ctor("(Ljava/lang/Runnable;)V", [init](Runtime& rt, NativeArgs& a) { return init(rt, a, a.h(1), u""); });
  t.ctor("(Ljava/lang/String;)V", [init](Runtime& rt, NativeArgs& a) { return init(rt, a, kNull, a.str(1)); });
  t.ctor("(Ljava/lang/Runnable;Ljava/lang/String;)V",
         [init](Runtime& rt, NativeArgs& a) { return init(rt, a, a.h(1), a.str(2)); });
  t.method("run", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle target = thread_peer(rt, a.h(0)).target;
    if (target) rt.call_virtual(target, "run", "()V");
    return 0;
  });
  t.method("start", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    if (p.started) rt.throw_new("Ljava/lang/IllegalThreadStateException;", "thread déjà démarré");
    p.started = true;
    for (Class* c = a.self().klass; c; c = c->super) {
      if (Field* f = c->declared_field("started", "Z"); f && !f->is_static()) {
        a.self().fields.at(f->slot) = 1;
        break;
      }
    }
    p.vm = rt.start_thread(a.h(0), text::utf16_to_utf8(p.name), p.daemon);
    if (p.pending_interrupt) rt.interrupt(p.vm);
    return 0;
  });
  t.static_method("currentThread", "()Ljava/lang/Thread;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.current_thread_peer();
  });
  t.static_method("sleep", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt.sleep(a.i64(0));
    return 0;
  });
  t.static_method("sleep", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt.sleep(a.i64(0) + (a.i32(2) > 0 ? 1 : 0));
    return 0;
  });
  t.static_method("yield", "()V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.yield_thread();
    return 0;
  });
  t.static_method("interrupted", "()Z", [](Runtime& rt, NativeArgs&) -> uint64_t {
    VmThread* cur = rt.current_thread();
    bool was = cur->interrupted;
    cur->interrupted = false;
    return ret_bool(was);
  });
  t.static_method("holdsLock", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return ret_bool(rt.holds_lock(a.h(0)));
  });
  t.method("interrupt", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    if (p.vm) {
      rt.interrupt(p.vm);
    } else {
      p.pending_interrupt = true;
    }
    return 0;
  });
  t.method("isInterrupted", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    return ret_bool(p.vm ? p.vm->interrupted : p.pending_interrupt);
  });
  auto join = [](Runtime& rt, Handle self, int64_t millis) {
    ThreadPeer& p = thread_peer(rt, self);
    if (millis < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "timeout value is negative");
    if (p.vm && p.vm->alive && !p.vm->attached) rt.join(p.vm, millis);
  };
  t.method("join", "()V", [join](Runtime& rt, NativeArgs& a) -> uint64_t {
    join(rt, a.h(0), 0);
    return 0;
  });
  t.method("join", "(J)V", [join](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.i64(1) == 0) {
      join(rt, a.h(0), 0);
    } else {
      join(rt, a.h(0), std::max<int64_t>(1, a.i64(1)));
    }
    return 0;
  });
  t.method("isAlive", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    return ret_bool(p.started && p.vm && p.vm->alive);
  });
  t.method("getName", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(thread_peer(rt, a.h(0)).name);
  });
  t.method("setName", "(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    thread_peer(rt, a.h(0)).name = a.str(1);
    return 0;
  });
  t.method("getId", "()J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    return ret_long(p.vm ? int64_t(p.vm->id) : int64_t(a.h(0)));
  });
  t.method("setDaemon", "(Z)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    if (p.started) rt.throw_new("Ljava/lang/IllegalThreadStateException;", "thread déjà démarré");
    p.daemon = a.z(1);
    return 0;
  });
  t.method("isDaemon", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t { return ret_bool(thread_peer(rt, a.h(0)).daemon); });
  t.method("setPriority", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.i32(1) < 1 || a.i32(1) > 10) rt.throw_new("Ljava/lang/IllegalArgumentException;", "priorité hors de [1, 10]");
    thread_peer(rt, a.h(0)).priority = a.i32(1);
    return 0;
  }, ApiStatus::kPartial, "mémorisée, sans effet sur l'ordonnancement (verrou global)");
  t.method("getPriority", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return ret_int(thread_peer(rt, a.h(0)).priority); });
  t.method("setUncaughtExceptionHandler", "(Ljava/lang/Thread$UncaughtExceptionHandler;)V",
           [](Runtime& rt, NativeArgs& a) -> uint64_t {
             thread_peer(rt, a.h(0)).handler = a.h(1);
             return 0;
           });
  t.method("getUncaughtExceptionHandler", "()Ljava/lang/Thread$UncaughtExceptionHandler;",
           [](Runtime& rt, NativeArgs& a) -> uint64_t { return thread_peer(rt, a.h(0)).handler; });
  t.static_method("setDefaultUncaughtExceptionHandler", "(Ljava/lang/Thread$UncaughtExceptionHandler;)V",
                  [](Runtime& rt, NativeArgs& a) -> uint64_t {
                    if (a.h(0)) {
                      rt.object_cache()["thread:default_handler"] = a.h(0);
                    } else {
                      rt.object_cache().erase("thread:default_handler");
                    }
                    return 0;
                  });
  t.static_method("getDefaultUncaughtExceptionHandler", "()Ljava/lang/Thread$UncaughtExceptionHandler;",
                  [](Runtime& rt, NativeArgs&) -> uint64_t {
                    auto it = rt.object_cache().find("thread:default_handler");
                    return it == rt.object_cache().end() ? kNull : it->second;
                  });
  t.method("toString", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ThreadPeer& p = thread_peer(rt, a.h(0));
    return rt.new_string(u"Thread[" + p.name + u"," + u16(std::to_string(p.priority)) + u",main]");
  });
}

void register_references(NativeRegistry& r) {
  r.define("Ljava/lang/ref/ReferenceQueue;", "Ljava/lang/Object;", {}, 0x1, ApiStatus::kPartial,
           "les références ne sont jamais mises en file")
      .ctor("()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  auto ref = r.define("Ljava/lang/ref/Reference;", "Ljava/lang/Object;", {}, 0x1 | 0x400);
  // Queue links read and written by libcore's ReferenceQueue. The referent
  // itself stays in the native peer so that the collector treats it weakly.
  ref.field("queue", "Ljava/lang/ref/ReferenceQueue;");
  ref.field("queueNext", "Ljava/lang/ref/Reference;");
  ref.field("pendingNext", "Ljava/lang/ref/Reference;");
  // Weak by default (PhantomReference, FinalizerReference, Cleaner).
  auto make_ref = [](bool weak) {
    return [weak](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto p = std::make_unique<ReferencePeer>();
      p->referent = a.h(1);
      p->weak = weak;
      Object& self = a.self();
      self.peer = std::move(p);
      if (a.count() > 2) {
        Field* q = rt.find_class("Ljava/lang/ref/Reference;")->declared_field("queue", "Ljava/lang/ref/ReferenceQueue;");
        self.fields.at(q->slot) = a.h(2);
      }
      return 0;
    };
  };
  ref.ctor("(Ljava/lang/Object;)V", make_ref(true));
  ref.ctor("(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V", make_ref(true), ApiStatus::kPartial,
           "jamais mise en file par le ramasse-miettes");
  ref.method("get", "()Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto* p = rt.peer<ReferencePeer>(a.h(0));
    return p ? p->referent : kNull;
  });
  ref.method("clear", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (auto* p = rt.peer<ReferencePeer>(a.h(0))) p->referent = kNull;
    return 0;
  });
  // The argument is a root of the caller's frame until the call returns,
  // which is all the fence guarantees; libcore's bytecode version costs
  // several instructions on every DirectByteBuffer access.
  ref.static_method("reachabilityFence", "(Ljava/lang/Object;)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  for (bool weak : {true, false}) {
    auto b = r.define(weak ? "Ljava/lang/ref/WeakReference;" : "Ljava/lang/ref/SoftReference;", "Ljava/lang/ref/Reference;",
                      {}, 0x1, weak ? ApiStatus::kSupported : ApiStatus::kPartial,
                      weak ? "" : "conservée tant que le prototype n'a pas de notion de pression mémoire");
    if (!weak) {
      b.field("timestamp", "J");
    }
    b.ctor("(Ljava/lang/Object;)V", make_ref(weak));
    b.ctor("(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V", make_ref(weak), ApiStatus::kPartial,
           "jamais mise en file par le ramasse-miettes");
  }
}

}  // namespace

void init_thread_object(Runtime& rt, Handle thread) {
  Class* cls = rt.find_class("Ljava/lang/Thread;");
  Field* f = cls->declared_field("blockerLock", "Ljava/lang/Object;");
  Object& o = rt.nonnull(thread, "Thread");
  if (f && o.fields.at(f->slot) == kNull) o.fields.at(f->slot) = rt.alloc(rt.find_class("Ljava/lang/Object;"));
  // As in ART, a new thread inherits its creator's context class loader (the
  // main thread gets the system class loader from start_libcore).
  Field* ccl = cls->declared_field("contextClassLoader", "Ljava/lang/ClassLoader;");
  Handle creator = rt.current_thread_peer();  // the new main thread's peer is itself
  if (ccl && creator != kNull && creator != thread) {
    o.fields.at(ccl->slot) = rt.nonnull(creator, "Thread").fields.at(ccl->slot);
  }
}

void register_core_classes(NativeRegistry& r) {
  register_object(r);
  register_string(r);
  register_class(r);
  register_throwables(r);
  r.define_interface("Ljava/lang/Runnable;").abstract_method("run", "()V");
  register_thread(r);
  register_references(r);
}

}  // namespace aa::rt
