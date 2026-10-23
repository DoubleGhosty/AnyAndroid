// Native methods of AOSP's libcore (core-oj, core-libart), implemented for
// Windows (G2, ADR-004). In ART they live in libjavacore.so, libopenjdk.so and
// libart.so; here they are bound by exact signature to the bytecode classes of
// the boot class path. A native method absent from this file fails explicitly
// (UnsatisfiedLinkError) and is listed by `anyandroid libcore natives`.
//
// Isolation (threat model M7/M8): the emulated process only sees standard
// streams, a fixed Android-like environment and fixed identity values; no
// host file, environment variable or user name is exposed.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <thread>
#include <tuple>

#include "common/clock.h"
#include "common/log.h"
#include "common/text.h"
#include "common/unicode.h"
#include "dex-engine/jlang.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"

#include <windows.h>

namespace aa::bridge {

LibcoreConfig& libcore_config() {
  static LibcoreConfig config;
  return config;
}

// --- Helpers shared with the other libcore modules (internal.h) ---------------------

rt::Field* find_field(rt::Class* cls, std::string_view name, std::string_view type) {
  for (rt::Class* c = cls; c; c = c->super) {
    if (rt::Field* f = c->declared_field(name, type)) return f;
  }
  return nullptr;
}

int32_t fd_of(rt::Runtime& rt, rt::Handle fd) {
  rt::Object& o = rt.nonnull(fd, "FileDescriptor");
  rt::Field* f = find_field(o.klass, "descriptor", "I");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/InternalError;", "FileDescriptor sans champ descriptor");
  return int32_t(o.fields.at(f->slot));
}

rt::Handle construct(rt::Runtime& rt, std::string_view cls, std::string_view sig, std::vector<uint32_t> args) {
  rt::Class* c = rt.find_class(cls);
  rt.ensure_initialized(c);
  rt::Method* ctor = c->declared_method("<init>", sig);
  if (!ctor) rt.throw_new("Ljava/lang/NoSuchMethodError;", c->java_name() + ".<init>" + std::string(sig));
  rt::Handle h = rt.alloc(c);
  args.insert(args.begin(), h);
  rt.invoke(ctor, args.data(), uint32_t(args.size()));
  return h;
}

void throw_errno(rt::Runtime& rt, const char* function, int32_t err) {
  if (rt.lookup_class("Landroid/system/ErrnoException;")) {
    throw rt::JavaThrow{construct(rt, "Landroid/system/ErrnoException;", "(Ljava/lang/String;I)V",
                                  {rt.new_string_utf8(function), uint32_t(err)})};
  }
  rt.throw_new("Ljava/io/IOException;", std::string(function) + " : errno " + std::to_string(err));
}

rt::Handle string_array(rt::Runtime& rt, const std::vector<std::string>& items) {
  rt::Handle arr = rt.new_array("[Ljava/lang/String;", int32_t(items.size()));
  rt::GlobalRef keep(rt, arr);
  for (size_t i = 0; i < items.size(); ++i) {
    rt::Handle s = rt.new_string_utf8(items[i]);
    std::memcpy(&rt.array(arr, "String[]").data[i * 4], &s, 4);
  }
  return arr;
}

namespace {

struct FreeFunctions {
  std::mutex lock;
  std::vector<std::function<void(int64_t)>> fns;
};

FreeFunctions& free_functions() {
  static FreeFunctions f;
  return f;
}

}  // namespace


int64_t register_free_function(std::function<void(int64_t)> fn) {
  FreeFunctions& f = free_functions();
  std::lock_guard<std::mutex> guard(f.lock);
  f.fns.push_back(std::move(fn));
  return int64_t(f.fns.size());
}

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// Linux errno values (asm-generic), also published through OsConstants.
enum Errno : int32_t { kENOENT = 2 };

// Environment of the emulated Android process: Android's at start (never the
// host's), then what the process itself sets (Os.setenv, unsetenv).
struct Environment {
  std::mutex lock;
  std::vector<std::pair<std::string, std::string>> vars = {
      {"ANDROID_ROOT", "/system"},
      {"ANDROID_DATA", "/data"},
      {"ANDROID_STORAGE", "/storage"},
      {"ANDROID_ART_ROOT", "/apex/com.android.art"},
      {"ANDROID_I18N_ROOT", "/apex/com.android.i18n"},
      {"ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata"},
      {"PATH", "/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin"},
      {"TMPDIR", "/data/local/tmp"},
  };
};

Environment& environment() {
  static auto* e = new Environment;  // never destroyed
  return *e;
}

std::vector<std::pair<std::string, std::string>> android_environment() {
  Environment& e = environment();
  std::lock_guard<std::mutex> guard(e.lock);
  return e.vars;
}

using aa::clock::monotonic_nanos;
using aa::clock::unix_millis;

std::u16string chars_of(Runtime& rt, Handle arr, int32_t offset, int32_t count) {
  rt::ArrayObject& a = rt.array(arr, "char[]");
  if (offset < 0 || count < 0 || int64_t(offset) + count > a.length) {
    rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "offset " + std::to_string(offset) + ", count " +
                                                                    std::to_string(count) + ", length " +
                                                                    std::to_string(a.length));
  }
  std::u16string s(size_t(count), u'\0');
  if (count) std::memcpy(s.data(), a.data.data() + size_t(offset) * 2, size_t(count) * 2);
  return s;
}

// --- java.lang.System, Runtime, Math, Float, Double ----------------------------------------

void array_copy(Runtime& rt, Handle src_h, int32_t sp, Handle dst_h, int32_t dp, int32_t len) {
  rt::ArrayObject& src = rt.array(src_h, "arraycopy(src)");
  rt::ArrayObject& dst = rt.array(dst_h, "arraycopy(dst)");
  rt::Class* sc = src.klass->component;
  rt::Class* dc = dst.klass->component;
  if (bool(sc->primitive) != bool(dc->primitive) || (sc->primitive && sc != dc)) {
    rt.throw_new("Ljava/lang/ArrayStoreException;", "arraycopy: type mismatch: can not copy " + src.klass->java_name() +
                                                        " into " + dst.klass->java_name());
  }
  if (sp < 0 || dp < 0 || len < 0 || int64_t(sp) + len > src.length || int64_t(dp) + len > dst.length) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
                 "src.length=" + std::to_string(src.length) + " srcPos=" + std::to_string(sp) + " dst.length=" +
                     std::to_string(dst.length) + " dstPos=" + std::to_string(dp) + " length=" + std::to_string(len));
  }
  if (!sc->primitive && !rt.is_assignable(sc, dc)) {
    for (int32_t i = 0; i < len; ++i) {
      Handle h;
      std::memcpy(&h, &src.data[size_t(sp + i) * 4], 4);
      if (h != kNull && !rt.instance_of(h, dc)) {
        rt.throw_new("Ljava/lang/ArrayStoreException;", "arraycopy: element type mismatch");
      }
      std::memcpy(&dst.data[size_t(dp + i) * 4], &h, 4);
    }
    return;
  }
  if (len) std::memmove(dst.data.data() + size_t(dp) * dst.width, src.data.data() + size_t(sp) * src.width, size_t(len) * src.width);
}

void register_system(rt::NativeRegistry& r) {
  const char* sys = "Ljava/lang/System;";
  r.bind(sys, "currentTimeMillis", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(unix_millis()); });
  r.bind(sys, "nanoTime", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(monotonic_nanos()); });
  auto copy = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    array_copy(rt, a.h(0), a.i32(1), a.h(2), a.i32(3), a.i32(4));
    return 0;
  };
  r.bind(sys, "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", copy);
  const std::pair<const char*, char> typed[] = {{"Char", 'C'},  {"Byte", 'B'},   {"Short", 'S'},  {"Int", 'I'},
                                                {"Long", 'J'},  {"Float", 'F'},  {"Double", 'D'}, {"Boolean", 'Z'}};
  for (const auto& [name, c] : typed) {
    std::string sig = std::string("([") + c + "I[" + c + "II)V";
    r.bind(sys, std::string("arraycopy") + name + "Unchecked", sig, copy);
  }
  r.bind(sys, "log", "(CLjava/lang/String;Ljava/lang/Throwable;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string line = std::string("System.") + char(a.raw(0)) + ": " + (a.h(1) ? rt.string_utf8(a.h(1)) : "null") + "\n";
    if (a.h(2)) line += rt.describe_throwable(a.h(2));
    std::fwrite(line.data(), 1, line.size(), stderr);
    return 0;
  });
  r.bind(sys, "mapLibraryName", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(u"lib" + a.str(0) + u".so");
  });
  r.bind(sys, "specialProperties", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    // java.library.path: the default namespace of the 64-bit linker, as
    // android_get_LD_LIBRARY_PATH reports it on a device.
    return string_array(rt, {"java.io.tmpdir=/data/local/tmp", "user.home=", "user.dir=/",
                             "java.library.path=/system/lib64:/system_ext/lib64"});
  });
  // System.setOut/setErr/setIn write the final static fields natively.
  for (const auto& [name, field, type] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
           {"setOut0", "out", "Ljava/io/PrintStream;"},
           {"setErr0", "err", "Ljava/io/PrintStream;"},
           {"setIn0", "in", "Ljava/io/InputStream;"}}) {
    std::string f = field, t = type;
    r.bind(sys, name, "(" + t + ")V", [f, t](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* c = rt.find_class("Ljava/lang/System;");
      rt::Field* fld = c->declared_field(f, t);
      if (!fld || !fld->is_static()) rt.throw_new("Ljava/lang/InternalError;", "System." + f + " introuvable");
      c->statics.at(fld->slot) = a.h(0);
      return 0;
    });
  }

  const char* th = "Ljava/lang/Thread;";
  r.bind(th, "currentThread", "()Ljava/lang/Thread;",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return rt.current_thread_peer(); });
  r.bind(th, "currentCarrierThread", "()Ljava/lang/Thread;",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return rt.current_thread_peer(); });
  // ART applies the niceness with setpriority and returns 0 (errno on
  // failure); the Java side keeps it in Thread.niceness. The host scheduler
  // is left alone: the value is remembered, not applied.
  r.bind(th, "setNiceness0", "(I)I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial,
         "niceness conservée par Java, non appliquée à l'ordonnanceur de l'hôte");

  const char* fdc = "Ljava/io/FileDescriptor;";
  r.bind(fdc, "getAppend", "(I)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial,
         "sorties standard uniquement");
  r.bind(fdc, "isSocket", "(I)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial,
         "aucune socket exposée");
  r.bind(fdc, "sync", "()V", [](Runtime&, NativeArgs&) -> uint64_t {
    std::fflush(stdout);
    std::fflush(stderr);
    return 0;
  });

  r.bind("Ljdk/internal/misc/VM;", "getNanoTimeAdjustment", "(J)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    // Nanoseconds since `offset` seconds after the epoch; -1, as HotSpot, when
    // `offset` is 2^32 seconds or more away (the caller then falls back to
    // currentTimeMillis), which also keeps the arithmetic in range.
    int64_t ms = unix_millis(), seconds = ms / 1000, offset = a.i64(0);
    constexpr int64_t kMaxDiff = int64_t(1) << 32;
    if (offset <= seconds - kMaxDiff || offset >= seconds + kMaxDiff) return rt::ret_long(-1);
    return rt::ret_long((seconds - offset) * 1000000000 + (ms % 1000) * 1000000);
  });
  r.bind("Ljava/lang/Runtime;", "nativeGc", "()V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.request_gc();
    return 0;
  }, S::kPartial, "collecte au prochain point de sûreté");

  const char* ver = "Lsun/misc/Version;";
  r.bind(ver, "getJvmSpecialVersion", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8("");
  });
  r.bind(ver, "getJdkSpecialVersion", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8("");
  });
  r.bind(ver, "getJvmVersionInfo", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ver, "getJdkVersionInfo", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  const char* run = "Ljava/lang/Runtime;";
  r.bind(run, "nativeExit", "(I)V", [](Runtime&, NativeArgs& a) -> uint64_t { throw rt::ExitRequest{a.i32(0)}; });
  r.bind(run, "nativeLoad", "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/String;",
         [](Runtime&, NativeArgs&) -> uint64_t { return kNull; }, S::kStub,
         "chargement de bibliothèques natives .so simulé (G4)");
  r.bind(run, "gc", "()V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.request_gc();
    return 0;
  }, S::kPartial, "collecte au prochain point de sûreté");
  r.bind(run, "freeMemory", "()J", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt::ret_long(int64_t(rt.options().max_objects - std::min(rt.options().max_objects, rt.object_count())) * 64);
  }, S::kPartial, "estimation à partir du plafond d'objets");
  r.bind(run, "totalMemory", "()J", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt::ret_long(int64_t(rt.options().max_objects) * 64);
  }, S::kPartial, "estimation à partir du plafond d'objets");
  r.bind(run, "maxMemory", "()J", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt::ret_long(int64_t(rt.options().max_objects) * 64);
  }, S::kPartial, "estimation à partir du plafond d'objets");

  // Math / StrictMath: the C library gives correctly rounded results for the
  // operations Java requires to be exact (sqrt, ceil, floor, rint).
  using D1 = double (*)(double);
  using D2 = double (*)(double, double);
  const std::pair<const char*, D1> unary[] = {
      {"sin", [](double x) { return std::sin(x); }},     {"cos", [](double x) { return std::cos(x); }},
      {"tan", [](double x) { return std::tan(x); }},     {"asin", [](double x) { return std::asin(x); }},
      {"acos", [](double x) { return std::acos(x); }},   {"atan", [](double x) { return std::atan(x); }},
      {"exp", [](double x) { return std::exp(x); }},     {"log", [](double x) { return std::log(x); }},
      {"log10", [](double x) { return std::log10(x); }}, {"sqrt", [](double x) { return std::sqrt(x); }},
      {"cbrt", [](double x) { return std::cbrt(x); }},   {"ceil", [](double x) { return std::ceil(x); }},
      {"floor", [](double x) { return std::floor(x); }}, {"rint", [](double x) { return std::nearbyint(x); }},
      {"sinh", [](double x) { return std::sinh(x); }},   {"cosh", [](double x) { return std::cosh(x); }},
      {"tanh", [](double x) { return std::tanh(x); }},   {"expm1", [](double x) { return std::expm1(x); }},
      {"log1p", [](double x) { return std::log1p(x); }},
  };
  const std::pair<const char*, D2> binary[] = {
      {"IEEEremainder", [](double x, double y) { return std::remainder(x, y); }},
      {"atan2", [](double x, double y) { return std::atan2(x, y); }},
      {"pow", [](double x, double y) { return std::pow(x, y); }},
      {"hypot", [](double x, double y) { return std::hypot(x, y); }},
  };
  for (const char* cls : {"Ljava/lang/Math;", "Ljava/lang/StrictMath;"}) {
    for (const auto& [name, fn] : unary) {
      r.bind(cls, name, "(D)D", [fn](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(fn(a.f64(0))); });
    }
    for (const auto& [name, fn] : binary) {
      r.bind(cls, name, "(DD)D", [fn](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_double(fn(a.f64(0), a.f64(2))); });
    }
  }

  r.bind("Ljava/lang/Float;", "floatToRawIntBits", "(F)I", [](Runtime&, NativeArgs& a) -> uint64_t { return a.raw(0); });
  r.bind("Ljava/lang/Float;", "intBitsToFloat", "(I)F", [](Runtime&, NativeArgs& a) -> uint64_t { return a.raw(0); });
  r.bind("Ljava/lang/Double;", "doubleToRawLongBits", "(D)J",
         [](Runtime&, NativeArgs& a) -> uint64_t { return uint64_t(a.i64(0)); });
  r.bind("Ljava/lang/Double;", "longBitsToDouble", "(J)D", [](Runtime&, NativeArgs& a) -> uint64_t { return uint64_t(a.i64(0)); });
}

// --- java.lang.StringFactory, Character ---------------------------------------------------------

void register_strings(rt::NativeRegistry& r) {
  const char* sf = "Ljava/lang/StringFactory;";
  r.bind(sf, "newStringFromChars", "(II[C)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(chars_of(rt, a.h(2), a.i32(0), a.i32(1)));
  });
  r.bind(sf, "newStringFromString", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(a.str(0));
  });
  r.bind(sf, "newStringFromBytes", "([BIII)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& b = rt.array(a.h(0), "byte[]");
    int32_t high = a.i32(1), off = a.i32(2), count = a.i32(3);
    if (off < 0 || count < 0 || int64_t(off) + count > b.length) rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "");
    std::u16string s(size_t(count), u'\0');
    for (int32_t i = 0; i < count; ++i) s[size_t(i)] = char16_t(((high & 0xFF) << 8) | b.data[size_t(off + i)]);
    return rt.new_string(std::move(s));
  });
  // UTF-16 code units stored little-endian in a byte[]; offset and count in chars.
  r.bind(sf, "newStringFromUtf16Bytes", "([BII)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& b = rt.array(a.h(0), "byte[]");
    int32_t off = a.i32(1), count = a.i32(2);
    if (off < 0 || count < 0 || int64_t(b.length / 2) - off < count) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "offset " + std::to_string(off) + ", count " + std::to_string(count));
    }
    std::u16string s(size_t(count), u'\0');
    if (count) std::memcpy(s.data(), b.data.data() + size_t(off) * 2, size_t(count) * 2);
    return rt.new_string(std::move(s));
  });
  r.bind(sf, "newStringFromUtf8Bytes", "([BII)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& b = rt.array(a.h(0), "byte[]");
    int32_t off = a.i32(1), count = a.i32(2);
    if (off < 0 || count < 0 || int64_t(off) + count > b.length) rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "");
    std::string_view bytes(reinterpret_cast<const char*>(b.data.data()) + off, size_t(count));
    return rt.new_string(text::utf8_to_utf16(bytes));
  });

  // libcore.util.CharsetUtils: fast paths of the standard charsets (static).
  const char* cu = "Llibcore/util/CharsetUtils;";
  r.bind(cu, "asciiBytesToChars", "([BII[C)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& src = rt.array(a.h(0), "bytes");
    rt::ArrayObject& dst = rt.array(a.h(3), "chars");
    int32_t off = a.i32(1), len = a.i32(2);
    if (off < 0 || len < 0 || int64_t(off) + len > src.length || len > int64_t(dst.length) || src.width != 1 || dst.width != 2) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "asciiBytesToChars");
    }
    for (int32_t i = 0; i < len; ++i) {
      int8_t b = int8_t(src.data[size_t(off + i)]);
      char16_t c = b >= 0 ? char16_t(b) : u'�';
      std::memcpy(dst.data.data() + size_t(i) * 2, &c, 2);
    }
    return 0;
  });
  auto encode_range = [](Runtime& rt, NativeArgs& a, char32_t limit) -> uint64_t {
    rt::StringObject* s = rt.as_string(a.h(0));
    if (!s) rt.throw_npe("String");
    int32_t off = a.i32(1), len = a.i32(2);
    if (off < 0 || len < 0 || int64_t(off) + len > int64_t(s->value.size())) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "CharsetUtils");
    }
    std::string out;
    for (int32_t i = off; i < off + len; ++i) {
      char16_t c = s->value[size_t(i)];
      if (limit != 0) {  // single-byte charsets: unmappable characters become '?'
        out += c <= limit ? char(c) : '?';
        continue;
      }
      uint32_t cp = c;
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < off + len && s->value[size_t(i + 1)] >= 0xDC00 &&
          s->value[size_t(i + 1)] <= 0xDFFF) {
        cp = 0x10000 + ((uint32_t(c) - 0xD800) << 10) + (uint32_t(s->value[size_t(++i)]) - 0xDC00);
      } else if (c >= 0xD800 && c <= 0xDFFF) {
        out += '?';  // unpaired surrogate, as libcore
        continue;
      }
      if (cp < 0x80) {
        out += char(cp);
      } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
      } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
      }
    }
    Handle arr = rt.new_array("[B", int32_t(out.size()));
    if (!out.empty()) std::memcpy(rt.array(arr, "byte[]").data.data(), out.data(), out.size());
    return arr;
  };
  r.bind(cu, "toAsciiBytes", "(Ljava/lang/String;II)[B",
         [encode_range](Runtime& rt, NativeArgs& a) -> uint64_t { return encode_range(rt, a, 0x7F); });
  r.bind(cu, "toIsoLatin1Bytes", "(Ljava/lang/String;II)[B",
         [encode_range](Runtime& rt, NativeArgs& a) -> uint64_t { return encode_range(rt, a, 0xFF); });
  r.bind(cu, "toUtf8Bytes", "(Ljava/lang/String;II)[B",
         [encode_range](Runtime& rt, NativeArgs& a) -> uint64_t { return encode_range(rt, a, 0); });

  // java.lang.String natives reached by libcore's String bytecode (merged).
  const char* str = "Ljava/lang/String;";
  auto self_value = [](Runtime& rt, NativeArgs& a) -> const std::u16string& {
    rt::StringObject* s = rt.as_string(a.h(0));
    if (!s) rt.throw_npe("String");
    return s->value;
  };
  r.bind(str, "fillBytesLatin1", "([BI)V", [self_value](Runtime& rt, NativeArgs& a) -> uint64_t {
    const std::u16string& v = self_value(rt, a);
    rt::ArrayObject& dst = rt.array(a.h(1), "dst");
    int32_t at = a.i32(2);
    if (dst.width != 1 || at < 0 || int64_t(at) + int64_t(v.size()) > dst.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "fillBytesLatin1");
    }
    for (size_t i = 0; i < v.size(); ++i) dst.data[size_t(at) + i] = uint8_t(v[i]);
    return 0;
  });
  r.bind(str, "fillBytesUTF16", "([BI)V", [self_value](Runtime& rt, NativeArgs& a) -> uint64_t {
    const std::u16string& v = self_value(rt, a);
    rt::ArrayObject& dst = rt.array(a.h(1), "dst");
    int32_t at = a.i32(2);  // byte offset
    if (dst.width != 1 || at < 0 || int64_t(at) + 2 * int64_t(v.size()) > dst.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "fillBytesUTF16");
    }
    // StringUTF16 stores chars in the platform byte order (little-endian).
    if (!v.empty()) std::memcpy(dst.data.data() + at, v.data(), v.size() * 2);
    return 0;
  });
  r.bind(str, "doRepeat", "(I)Ljava/lang/String;", [self_value](Runtime& rt, NativeArgs& a) -> uint64_t {
    const std::u16string& v = self_value(rt, a);
    int32_t n = a.i32(1);
    if (n < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "count is negative: " + std::to_string(n));
    if (!v.empty() && uint64_t(n) * v.size() > uint64_t(rt.options().max_array_length)) {
      rt.throw_new("Ljava/lang/OutOfMemoryError;", "Required length exceeds implementation limit");
    }
    std::u16string out;
    out.reserve(v.size() * size_t(n));
    for (int32_t i = 0; i < n; ++i) out += v;
    return rt.new_string(std::move(out));
  });
  r.bind(str, "doReplace", "(CC)Ljava/lang/String;", [self_value](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string v = self_value(rt, a);
    char16_t from = char16_t(a.raw(1)), to = char16_t(a.raw(2));
    for (auto& c : v) {
      if (c == from) c = to;
    }
    return rt.new_string(std::move(v));
  });

  // Character properties: the BMP is classified with the C library (Unicode
  // tables of the UCRT); supplementary code points get conservative answers.
  const char* ch = "Ljava/lang/Character;";
  auto wc = [](NativeArgs& a) { return wint_t(a.i32(0)); };
  auto bmp = [](NativeArgs& a) { return a.i32(0) >= 0 && a.i32(0) <= 0xFFFF; };
  using Pred = int (*)(wint_t);
  const std::pair<const char*, Pred> preds[] = {
      {"isLetterImpl", [](wint_t c) { return std::iswalpha(c); }},
      {"isDigitImpl", [](wint_t c) { return std::iswdigit(c) || (c >= 0x660 && c <= 0x669) || (c >= 0xFF10 && c <= 0xFF19) ? 1 : 0; }},
      {"isLetterOrDigitImpl", [](wint_t c) { return std::iswalnum(c); }},
      {"isUpperCaseImpl", [](wint_t c) { return std::iswupper(c); }},
      {"isLowerCaseImpl", [](wint_t c) { return std::iswlower(c); }},
      {"isWhitespaceImpl", [](wint_t c) {
         return (c >= 0x9 && c <= 0xD) || (c >= 0x1C && c <= 0x20) || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) ||
                        (c >= 0x2008 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x205F || c == 0x3000
                    ? 1
                    : 0;
       }},
      {"isSpaceCharImpl", [](wint_t c) { return c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ? 1 : 0; }},
      {"isAlphabeticImpl", [](wint_t c) { return std::iswalpha(c); }},
      {"isDefinedImpl", [](wint_t c) { return c < 0xD800 || c > 0xDFFF ? 1 : 0; }},
  };
  for (const auto& [name, pred] : preds) {
    r.bind(ch, name, "(I)Z", [pred, wc, bmp](Runtime&, NativeArgs& a) -> uint64_t {
      return rt::ret_bool(bmp(a) && pred(wc(a)) != 0);
    }, S::kPartial, "plan multilingue de base, tables Unicode de la bibliothèque C");
  }
  // Simple case mapping of the system's Unicode tables (not the C locale's).
  auto mapped = [](char32_t (*map)(char32_t)) {
    return [map](Runtime&, NativeArgs& a) -> uint64_t {
      int32_t c = a.i32(0);
      return rt::ret_int(c >= 0 && c <= 0x10FFFF ? int32_t(map(char32_t(c))) : c);
    };
  };
  r.bind(ch, "toLowerCaseImpl", "(I)I", mapped(unicode::to_lower), S::kPartial, "correspondance simple (sans règles de locale)");
  r.bind(ch, "toUpperCaseImpl", "(I)I", mapped(unicode::to_upper), S::kPartial, "correspondance simple (sans règles de locale)");
  r.bind(ch, "toTitleCaseImpl", "(I)I", mapped(unicode::to_upper), S::kPartial, "majuscule (casse de titre approchée)");
  // libcore.icu.ICU: versions of the ICU data shipped by the i18n module of
  // Android 17 (icudt78l.dat); fixed default locale, never the host's.
  const char* icu = "Llibcore/icu/ICU;";
  auto text_result = [](const char* v) {
    std::string s = v;
    return [s](Runtime& rt, NativeArgs&) -> uint64_t { return rt.new_string_utf8(s); };
  };
  r.bind(icu, "getIcuVersion", "()Ljava/lang/String;", text_result("78.1"), S::kPartial, "version des données ICU d'Android 17");
  r.bind(icu, "getUnicodeVersion", "()Ljava/lang/String;", text_result("17.0"));
  r.bind(icu, "getCldrVersion", "()Ljava/lang/String;", text_result("48"));
  r.bind(icu, "getDefaultLocale", "()Ljava/lang/String;", text_result("en-US"), S::kPartial, "locale fixe en-US");
  r.bind(icu, "getScript", "(Ljava/lang/String;)Ljava/lang/String;", text_result(""), S::kPartial, "script non déduit");

  // General category and bidirectional class: ART answers from ICU4C; ICU4J
  // (android.icu.lang.UCharacter) reads the same data of the i18n module.
  // Both are ICU values: libcore's Character maps them to Java's.
  r.bind(ch, "getTypeImpl", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.call_static("Landroid/icu/lang/UCharacter;", "getType", "(I)I", {a.raw(0)});
  });
  r.bind(ch, "getDirectionalityImpl", "(I)B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int8_t(rt.call_static("Landroid/icu/lang/UCharacter;", "getDirection", "(I)I", {a.raw(0)})));
  });
  r.bind(ch, "digitImpl", "(II)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    int32_t c = a.i32(0), radix = a.i32(1), v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else if (c >= 0xFF10 && c <= 0xFF19) v = c - 0xFF10;
    return rt::ret_int(v >= 0 && v < radix ? v : -1);
  }, S::kPartial);
}

// --- dalvik.system.VMRuntime, VMStack ------------------------------------------------------------

void register_vm(rt::NativeRegistry& r) {
  const char* vm = "Ldalvik/system/VMRuntime;";
  r.bind(vm, "properties", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return string_array(rt, libcore_config().properties);
  });
  r.bind(vm, "bootClassPath", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8(libcore_config().boot_class_path);
  });
  r.bind(vm, "classPath", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8(libcore_config().class_path);
  });
  auto constant = [](const char* value) {
    std::string v = value;
    return [v](Runtime& rt, NativeArgs&) -> uint64_t { return rt.new_string_utf8(v); };
  };
  r.bind(vm, "vmVersion", "()Ljava/lang/String;", constant("2.1.0"));
  r.bind(vm, "vmLibrary", "()Ljava/lang/String;", constant("libanyandroid"));
  r.bind(vm, "vmInstructionSet", "()Ljava/lang/String;", constant("x86_64"));
  r.bind(vm, "getCurrentInstructionSet", "()Ljava/lang/String;", constant("x86_64"));
  auto boolean = [](bool v) { return [v](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_bool(v); }; };
  r.bind(vm, "is64Bit", "()Z", boolean(true));
  r.bind(vm, "isCheckJniEnabled", "()Z", boolean(false));
  r.bind(vm, "isNativeDebuggable", "()Z", boolean(false));
  r.bind(vm, "isJavaDebuggable", "()Z", boolean(false));
  r.bind(vm, "hasBootImageSpaces", "()Z", boolean(false));
  r.bind(vm, "getTargetHeapUtilization", "()F", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_float(0.75f); });
  auto noop = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"nativeSetTargetHeapUtilization", "(F)V"}, {"setTargetSdkVersionNative", "(I)V"},
           {"clearGrowthLimit", "()V"}, {"clampGrowthLimit", "()V"}, {"trimHeap", "()V"},
           {"registerNativeAllocation", "(J)V"}, {"registerNativeAllocation", "(I)V"},
           {"registerNativeFree", "(J)V"}, {"registerNativeFree", "(I)V"}, {"requestHeapTrim", "()V"},
           {"startJitCompilation", "()V"}, {"disableJitCompilation", "()V"}, {"setHiddenApiExemptions", "([Ljava/lang/String;)V"},
           {"setHiddenApiAccessLogSamplingRate", "(I)V"}, {"registerSensitiveThread", "()V"}}) {
    r.bind(vm, name, sig, noop, S::kStub, "sans objet pour AnyAndroid");
  }
  r.bind("Llibcore/util/NativeAllocationRegistry;", "applyFreeFunction", "(JJ)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    std::function<void(int64_t)> fn;
    {
      FreeFunctions& f = free_functions();
      std::lock_guard<std::mutex> guard(f.lock);
      int64_t id = a.i64(0);
      if (id > 0 && uint64_t(id) <= f.fns.size()) fn = f.fns[size_t(id - 1)];
    }
    if (fn) fn(a.i64(2));
    return 0;
  });
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"notifyNativeAllocationsInternal", "()V"}, {"bootCompleted", "()V"}, {"resetJitCounters", "()V"},
           {"notifyStartupCompleted", "()V"}, {"preloadDexCaches", "()V"}, {"runHeapTasks", "()V"},
           {"startHeapTaskProcessor", "()V"}, {"stopHeapTaskProcessor", "()V"}, {"updateProcessState", "(I)V"},
           {"setProcessPackageName", "(Ljava/lang/String;)V"}, {"setProcessDataDirectory", "(Ljava/lang/String;)V"},
           {"setDedupeHiddenApiWarnings", "(Z)V"}, {"setDisabledCompatChangesNative", "([J)V"},
           {"registerAppInfo", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;I)V"}}) {
    r.bind(vm, name, sig, noop, S::kStub, "sans objet pour AnyAndroid");
  }
  r.bind(vm, "getNotifyNativeInterval", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 384; });
  r.bind(vm, "getFinalizerTimeoutMs", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return 10000; });
  r.bind(vm, "getFullGcCount", "()J", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt::ret_long(int64_t(rt.gc_stats().collections));
  });
  r.bind(vm, "getSdkVersionNative", "(I)I", [](Runtime&, NativeArgs&) -> uint64_t { return 37; }, S::kPartial,
         "SDK d'Android 17 (37)");
  r.bind(vm, "getIntSystemProperty", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fallback = a.i32(1);
    if (a.h(0) == kNull) return rt::ret_int(fallback);
    auto v = system_property(rt.string_utf8(a.h(0)));
    if (!v || v->empty()) return rt::ret_int(fallback);
    try {
      size_t used = 0;
      int64_t n = std::stoll(*v, &used, 0);
      if (used != v->size() || n < INT32_MIN || n > INT32_MAX) return rt::ret_int(fallback);
      return rt::ret_int(int32_t(n));
    } catch (const std::exception&) {
      return rt::ret_int(fallback);
    }
  });
  r.bind(vm, "isArtTestRwFlagEnabled", "()Z", boolean(false));
  r.bind(vm, "isValidClassLoaderContext", "(Ljava/lang/String;)Z", boolean(true));
  r.bind(vm, "isBootClassPathOnDisk", "(Ljava/lang/String;)Z", boolean(true));
  r.bind(vm, "getSystemDaemonNiceness", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  // The base APK's state as ART reports it: interpreted straight from the APK,
  // no ahead-of-time compilation (compiler filter, compilation reason).
  r.bind(vm, "getBaseApkOptimizationInfo", "()Ldalvik/system/DexFile$OptimizationInfo;",
         [](Runtime& rt, NativeArgs&) -> uint64_t {
           rt::Class* cls = rt.find_class("Ldalvik/system/DexFile$OptimizationInfo;");
           rt::GlobalRef info(rt, rt.alloc(cls));
           rt::Object& o = rt.nonnull(info.get(), "OptimizationInfo");
           const std::pair<const char*, const char*> values[] = {{"status", "run-from-apk"}, {"reason", "unknown"}};
           for (const auto& [name, value] : values) {
             rt::Field* f = find_field(cls, name, "Ljava/lang/String;");
             if (f) o.fields.at(f->slot) = rt.new_string_utf8(value);
           }
           return info.get();
         });
  r.bind(vm, "requestConcurrentGC", "()V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.request_gc();
    return 0;
  });
  auto new_array = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* component = rt.mirror_class(a.h(1));
    if (!component) rt.throw_npe("componentType");
    if (component->primitive == 'V') rt.throw_new("Ljava/lang/NoClassDefFoundError;", "void[]");
    std::string desc = "[" + component->descriptor;
    return rt.new_array(desc, a.i32(2));
  };
  r.bind(vm, "newNonMovableArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", new_array);
  r.bind(vm, "newUnpaddedArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", new_array);
  r.bind("Ljava/lang/VMClassLoader;", "getBootClassPathEntries", "()[Ljava/lang/String;",
         [](Runtime& rt, NativeArgs&) -> uint64_t {
           std::vector<std::string> entries;
           const std::string& bcp = libcore_config().boot_class_path;
           size_t start = 0;
           while (start <= bcp.size()) {
             size_t end = bcp.find(':', start);
             if (end == std::string::npos) end = bcp.size();
             if (end > start) entries.push_back(bcp.substr(start, end - start));
             start = end + 1;
           }
           return string_array(rt, entries);
         });
  r.bind("Ljava/lang/VMClassLoader;", "findLoadedClass",
         "(Ljava/lang/ClassLoader;Ljava/lang/String;)Ljava/lang/Class;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(1) == kNull) return kNull;
           rt::Class* c = find_loaded_class(rt, a.h(0), rt.string_utf8(a.h(1)));
           return c ? rt.class_mirror(c) : kNull;
         },
         S::kPartial, "un seul espace de classes (ADR-006 J2)");
  r.bind(vm, "addressOf", "(Ljava/lang/Object;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return uint64_t(native_address_of(rt, a.h(1)));
  }, S::kPartial, "adresse de la mémoire native émulée (native_memory.h)");

  // VMDebug: the process registers its identity and debugging state; there is
  // no debugger, method tracing nor allocation tracker in AnyAndroid.
  const char* dbg = "Ldalvik/system/VMDebug;";
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"addApplication", "(Ljava/lang/String;)V"}, {"removeApplication", "(Ljava/lang/String;)V"},
           {"setCurrentProcessName", "(Ljava/lang/String;)V"}, {"setUserId", "(I)V"},
           {"setWaitingForDebugger", "(Z)V"}, {"allowHiddenApiReflectionFrom", "(Ljava/lang/Class;)V"},
           {"startAllocCounting", "()V"}, {"stopAllocCounting", "()V"}, {"resetAllocCount", "(I)V"},
           {"setAllocTrackerStackDepth", "(I)V"}, {"dumpReferenceTables", "()V"}, {"printLoadedClasses", "(I)V"}}) {
    r.bind(dbg, name, sig, noop, S::kStub, "pas de débogueur ni de traçage dans AnyAndroid");
  }
  r.bind(dbg, "isDebuggerConnected", "()Z", boolean(false));
  // DDM (Android Studio's debugging channel): no transport, chunks go nowhere.
  const char* ddm = "Lorg/apache/harmony/dalvik/ddmc/DdmServer;";
  r.bind(ddm, "isDdmDisabled", "()Z", boolean(true));
  r.bind(ddm, "nativeSendChunk", "(I[BII)V", noop, S::kStub, "pas de canal DDM dans AnyAndroid");
  // ICU4C's default locale (uloc_setDefault): there is no ICU4C, ICU4J keeps its own in Java.
  r.bind("Lcom/android/icu/util/LocaleNative;", "setDefaultNative", "(Ljava/lang/String;)V", noop, S::kPartial,
         "pas d'ICU4C : la locale par défaut d'ICU4J suffit");
  r.bind(dbg, "isDebuggingEnabled", "()Z", boolean(false));
  r.bind(dbg, "lastDebuggerActivity", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(-1); });
  r.bind(dbg, "getMethodTracingMode", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(dbg, "getAllocCount", "(I)I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kStub);
  r.bind(dbg, "getLoadedClassCount", "()I", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt::ret_int(int32_t(rt.loaded_classes().size()));
  });
  r.bind(dbg, "getVmFeatureList", "()[Ljava/lang/String;",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return string_array(rt, {}); });
  r.bind(dbg, "threadCpuTimeNanos", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return rt::ret_long(-1);
    uint64_t k = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    uint64_t u = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    return rt::ret_long(int64_t((k + u) * 100));
  });

  // VMStack: frames of the current thread, innermost last. Frame n-1 is the
  // native method itself, n-2 the method that called it.
  const char* st = "Ldalvik/system/VMStack;";
  auto stack_class = [](size_t depth) {
    return [depth](Runtime& rt, NativeArgs&) -> uint64_t {
      const auto& frames = rt.current_thread()->frames;
      if (frames.size() <= depth) return kNull;
      return rt.class_mirror(frames[frames.size() - 1 - depth].method->owner);
    };
  };
  r.bind(st, "getStackClass1", "()Ljava/lang/Class;", stack_class(2));
  r.bind(st, "getStackClass2", "()Ljava/lang/Class;", stack_class(3));
  // Loaders as ART sees them internally: null for the boot space.
  r.bind(st, "getCallingClassLoader", "()Ljava/lang/ClassLoader;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    const auto& frames = rt.current_thread()->frames;
    if (frames.size() < 3) return kNull;
    return class_loader_of(rt, frames[frames.size() - 3].method->owner, true);
  });
  r.bind(st, "getClosestUserClassLoader", "()Ljava/lang/ClassLoader;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    const auto& frames = rt.current_thread()->frames;
    for (size_t i = frames.size(); i-- > 0;) {
      rt::Class* owner = frames[i].method->owner;
      if (!rt::Runtime::in_boot_space(owner)) return class_loader_of(rt, owner, true);
    }
    return kNull;
  });
}

// --- libcore.io.Linux, android.system.OsConstants -----------------------------------------------

struct OsConstant {
  const char* name;
  int32_t value;
};

// Linux x86_64 values. Constants absent from this table get distinct
// sentinel values so that no two unknown constants compare equal.
const OsConstant kOsConstants[] = {
    {"EPERM", 1}, {"ENOENT", 2}, {"ESRCH", 3}, {"EINTR", 4}, {"EIO", 5}, {"ENXIO", 6}, {"E2BIG", 7},
    {"ENOEXEC", 8}, {"EBADF", 9}, {"ECHILD", 10}, {"EAGAIN", 11}, {"EWOULDBLOCK", 11}, {"ENOMEM", 12},
    {"EACCES", 13}, {"EFAULT", 14}, {"EBUSY", 16}, {"EEXIST", 17}, {"EXDEV", 18}, {"ENODEV", 19},
    {"ENOTDIR", 20}, {"EISDIR", 21}, {"EINVAL", 22}, {"ENFILE", 23}, {"EMFILE", 24}, {"ENOTTY", 25},
    {"ETXTBSY", 26}, {"EFBIG", 27}, {"ENOSPC", 28}, {"ESPIPE", 29}, {"EROFS", 30}, {"EMLINK", 31},
    {"EPIPE", 32}, {"EDOM", 33}, {"ERANGE", 34}, {"EDEADLK", 35}, {"ENAMETOOLONG", 36}, {"ENOLCK", 37},
    {"ENOSYS", 38}, {"ENOTEMPTY", 39}, {"ELOOP", 40}, {"ENOMSG", 42}, {"EIDRM", 43}, {"ENOSTR", 60},
    {"ENODATA", 61}, {"ETIME", 62}, {"ENOSR", 63}, {"ENOLINK", 67}, {"EPROTO", 71}, {"EMULTIHOP", 72},
    {"EBADMSG", 74}, {"EOVERFLOW", 75}, {"EILSEQ", 84}, {"ENOTSOCK", 88}, {"EDESTADDRREQ", 89},
    {"EMSGSIZE", 90}, {"EPROTOTYPE", 91}, {"ENOPROTOOPT", 92}, {"EPROTONOSUPPORT", 93}, {"EOPNOTSUPP", 95},
    {"ENOTSUP", 95}, {"EAFNOSUPPORT", 97}, {"EADDRINUSE", 98}, {"EADDRNOTAVAIL", 99}, {"ENETDOWN", 100},
    {"ENETUNREACH", 101}, {"ENETRESET", 102}, {"ECONNABORTED", 103}, {"ECONNRESET", 104}, {"ENOBUFS", 105},
    {"EISCONN", 106}, {"ENOTCONN", 107}, {"ETIMEDOUT", 110}, {"ECONNREFUSED", 111}, {"EHOSTUNREACH", 113},
    {"EALREADY", 114}, {"EINPROGRESS", 115}, {"ESTALE", 116}, {"EDQUOT", 122}, {"ECANCELED", 125},
    {"EOWNERDEAD", 130}, {"ENOTRECOVERABLE", 131},
    {"O_RDONLY", 0}, {"O_WRONLY", 1}, {"O_RDWR", 2}, {"O_ACCMODE", 3}, {"O_CREAT", 0100}, {"O_EXCL", 0200},
    {"O_NOCTTY", 0400}, {"O_TRUNC", 01000}, {"O_APPEND", 02000}, {"O_NONBLOCK", 04000}, {"O_DSYNC", 010000},
    {"O_SYNC", 04010000}, {"O_DIRECTORY", 0200000}, {"O_NOFOLLOW", 0400000}, {"O_CLOEXEC", 02000000},
    {"F_DUPFD", 0}, {"F_GETFD", 1}, {"F_SETFD", 2}, {"F_GETFL", 3}, {"F_SETFL", 4}, {"F_GETLK", 5},
    {"F_SETLK", 6}, {"F_SETLKW", 7}, {"F_SETOWN", 8}, {"F_GETOWN", 9}, {"F_DUPFD_CLOEXEC", 1030},
    {"FD_CLOEXEC", 1}, {"F_RDLCK", 0}, {"F_WRLCK", 1}, {"F_UNLCK", 2}, {"F_OK", 0}, {"X_OK", 1}, {"W_OK", 2},
    {"R_OK", 4}, {"SEEK_SET", 0}, {"SEEK_CUR", 1}, {"SEEK_END", 2}, {"STDIN_FILENO", 0}, {"STDOUT_FILENO", 1},
    {"STDERR_FILENO", 2},
    {"S_IFMT", 0170000}, {"S_IFSOCK", 0140000}, {"S_IFLNK", 0120000}, {"S_IFREG", 0100000}, {"S_IFBLK", 060000},
    {"S_IFDIR", 040000}, {"S_IFCHR", 020000}, {"S_IFIFO", 010000}, {"S_ISUID", 04000}, {"S_ISGID", 02000},
    {"S_ISVTX", 01000}, {"S_IRWXU", 0700}, {"S_IRUSR", 0400}, {"S_IWUSR", 0200}, {"S_IXUSR", 0100},
    {"S_IRWXG", 070}, {"S_IRGRP", 040}, {"S_IWGRP", 020}, {"S_IXGRP", 010}, {"S_IRWXO", 07}, {"S_IROTH", 04},
    {"S_IWOTH", 02}, {"S_IXOTH", 01},
    {"_SC_ARG_MAX", 0}, {"_SC_CHILD_MAX", 1}, {"_SC_CLK_TCK", 2}, {"_SC_NGROUPS_MAX", 3}, {"_SC_OPEN_MAX", 4},
    {"_SC_PAGESIZE", 30}, {"_SC_PAGE_SIZE", 30}, {"_SC_NPROCESSORS_CONF", 83}, {"_SC_NPROCESSORS_ONLN", 84},
    {"_SC_PHYS_PAGES", 85}, {"_SC_AVPHYS_PAGES", 86},
    {"AF_UNSPEC", 0}, {"AF_UNIX", 1}, {"AF_INET", 2}, {"AF_INET6", 10}, {"AF_NETLINK", 16}, {"AF_PACKET", 17},
    {"SOCK_STREAM", 1}, {"SOCK_DGRAM", 2}, {"SOCK_RAW", 3}, {"SOCK_SEQPACKET", 5}, {"SOCK_NONBLOCK", 04000},
    {"SOCK_CLOEXEC", 02000000}, {"IPPROTO_IP", 0}, {"IPPROTO_ICMP", 1}, {"IPPROTO_TCP", 6}, {"IPPROTO_UDP", 17},
    {"IPPROTO_IPV6", 41}, {"IPPROTO_ICMPV6", 58}, {"IPPROTO_RAW", 255}, {"SOL_SOCKET", 1},
    {"SIGHUP", 1}, {"SIGINT", 2}, {"SIGQUIT", 3}, {"SIGILL", 4}, {"SIGTRAP", 5}, {"SIGABRT", 6}, {"SIGBUS", 7},
    {"SIGFPE", 8}, {"SIGKILL", 9}, {"SIGUSR1", 10}, {"SIGSEGV", 11}, {"SIGUSR2", 12}, {"SIGPIPE", 13},
    {"SIGALRM", 14}, {"SIGTERM", 15}, {"SIGCHLD", 17}, {"SIGCONT", 18}, {"SIGSTOP", 19},
    {"MAP_SHARED", 1}, {"MAP_PRIVATE", 2}, {"MAP_FIXED", 0x10}, {"MAP_ANONYMOUS", 0x20}, {"PROT_NONE", 0},
    {"PROT_READ", 1}, {"PROT_WRITE", 2}, {"PROT_EXEC", 4}, {"POLLIN", 1}, {"POLLPRI", 2}, {"POLLOUT", 4},
    {"POLLERR", 8}, {"POLLHUP", 16}, {"POLLNVAL", 32},
};

int64_t sysconf_value(int32_t name) {
  switch (name) {
    case 2: return 100;                                                       // _SC_CLK_TCK
    case 4: return 1024;                                                      // _SC_OPEN_MAX
    case 30: return 4096;                                                     // _SC_PAGESIZE
    case 83: case 84: return std::max(1u, std::thread::hardware_concurrency());  // processors
    case 85: case 86: return int64_t(1) << 20;                                // 4 GiB of pages
    default: return -1;
  }
}

}  // namespace

std::string strerror_text(int32_t err) {
  switch (err) {
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 4: return "Interrupted system call";
    case 5: return "I/O error";
    case 9: return "Bad file descriptor";
    case 11: return "Try again";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 18: return "Cross-device link";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 25: return "Not a typewriter";
    case 27: return "File too large";
    case 28: return "No space left on device";
    case 29: return "Illegal seek";
    case 30: return "Read-only file system";
    case 32: return "Broken pipe";
    case 36: return "File name too long";
    case 38: return "Function not implemented";
    case 39: return "Directory not empty";
    case 40: return "Too many symbolic links encountered";
    case 61: return "No data available";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    default: return "Unknown error " + std::to_string(err);
  }
}

namespace {

void register_os(rt::NativeRegistry& r) {
  // Android 17 moved the native part to OsConstantsHolder; both fill OsConstants.
  auto init_constants = [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt::Class* cls = rt.find_class("Landroid/system/OsConstants;");
    int32_t sentinel = 0x7E000000;
    size_t unknown = 0;
    for (auto& f : cls->fields) {
      if (!f->is_static() || f->type != "I") continue;
      int32_t v = sentinel++;
      bool known = false;
      for (const auto& c : kOsConstants) {
        if (f->name == c.name) {
          v = c.value;
          known = true;
          break;
        }
      }
      unknown += !known;
      cls->statics.at(f->slot) = uint32_t(v);
    }
    if (unknown) log::debug("libcore", "OsConstants : " + std::to_string(unknown) + " constantes sans valeur Linux connue");
    return 0;
  };
  for (const char* cls : {"Landroid/system/OsConstants;", "Landroid/system/OsConstantsHolder;"}) {
    r.bind(cls, "initConstants", "()V", init_constants, S::kPartial,
           "constantes Linux x86_64 usuelles ; les autres reçoivent des valeurs distinctes");
  }

  const char* lx = "Llibcore/io/Linux;";
  // Instance methods: argument 0 is the Linux object. Descriptor and file
  // natives (read, write, open, stat...) live in libcore_files.cpp.
  r.bind(lx, "getenv", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = rt.string_utf8(a.h(1));
    for (const auto& [k, v] : android_environment()) {
      if (k == name) return rt.new_string_utf8(v);
    }
    return kNull;
  }, S::kPartial, "environnement Android, jamais celui de l'hôte");
  r.bind(lx, "environ", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    std::vector<std::string> items;
    for (const auto& [k, v] : android_environment()) items.push_back(k + "=" + v);
    return string_array(rt, items);
  }, S::kPartial, "environnement Android, jamais celui de l'hôte");
  // setenv(3) and unsetenv(3): EINVAL for an empty name or one with '='.
  auto env_name = [](Runtime& rt, Handle h, const char* function) {
    if (h == kNull) rt.throw_npe("name == null");
    std::string name = rt.string_utf8(h);
    if (name.empty() || name.find('=') != std::string::npos) throw_errno(rt, function, 22);
    return name;
  };
  r.bind(lx, "setenv", "(Ljava/lang/String;Ljava/lang/String;Z)V", [env_name](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = env_name(rt, a.h(1), "setenv");
    if (a.h(2) == kNull) rt.throw_npe("value == null");
    std::string value = rt.string_utf8(a.h(2));
    Environment& e = environment();
    std::lock_guard<std::mutex> guard(e.lock);
    for (auto& [k, v] : e.vars) {
      if (k != name) continue;
      if (a.i32(3)) v = value;  // overwrite
      return 0;
    }
    e.vars.emplace_back(name, value);
    return 0;
  });
  r.bind(lx, "unsetenv", "(Ljava/lang/String;)V", [env_name](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = env_name(rt, a.h(1), "unsetenv");
    Environment& e = environment();
    std::lock_guard<std::mutex> guard(e.lock);
    std::erase_if(e.vars, [&](const auto& kv) { return kv.first == name; });
    return 0;
  });
  auto ident = [](int32_t v) { return [v](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(v); }; };
  r.bind(lx, "getpid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(int32_t(GetCurrentProcessId())); });
  r.bind(lx, "gettid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(int32_t(GetCurrentThreadId())); });
  r.bind(lx, "getppid", "()I", ident(1));
  for (const char* name : {"getuid", "geteuid", "getgid", "getegid"}) {
    r.bind(lx, name, "()I", ident(10000), S::kPartial, "identité d'application Android fixe (u0_a0)");
  }
  // Android 17 names (static natives behind the Os methods).
  for (const char* name : {"nativeGetuid", "nativeGeteuid", "nativeGetgid", "nativeGetegid"}) {
    r.bind(lx, name, "()I", ident(10000), S::kPartial, "identité d'application Android fixe (u0_a0)");
  }
  r.bind(lx, "nativeGetpid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(int32_t(GetCurrentProcessId())); });
  r.bind(lx, "nativeGettid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(int32_t(GetCurrentThreadId())); });
  r.bind(lx, "nativeGetppid", "()I", ident(1));
  // umask(2): the previous mask; an application process starts with zygote's 077. Files of the
  // VFS keep their own permissions: the mask is only remembered.
  r.bind(lx, "umaskImpl", "(I)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    static std::atomic<int32_t> mask{077};
    return rt::ret_int(mask.exchange(a.i32(1) & 0777));
  }, S::kPartial, "masque retenu, sans effet sur les permissions du VFS");
  // fdsan (descriptor ownership tags): no tagging on Windows.
  r.bind(lx, "android_fdsan_exchange_owner_tag", "(Ljava/io/FileDescriptor;JJ)V",
         [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial, "fdsan sans objet");
  r.bind(lx, "android_fdsan_get_owner_tag", "(Ljava/io/FileDescriptor;)J",
         [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial, "fdsan sans objet");
  r.bind(lx, "android_fdsan_get_tag_type", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8("none");
  }, S::kStub, "fdsan sans objet");
  r.bind(lx, "android_fdsan_get_tag_value", "(J)J", [](Runtime&, NativeArgs&) -> uint64_t { return 0; },
         S::kStub, "fdsan sans objet");
  r.bind(lx, "getpwuid", "(I)Landroid/system/StructPasswd;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    throw_errno(rt, "getpwuid", kENOENT);
  }, S::kPartial, "aucune base d'utilisateurs");
  r.bind(lx, "strerror", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(strerror_text(a.i32(1)));
  });
  r.bind(lx, "sysconf", "(I)J", [](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_long(sysconf_value(a.i32(1))); });
  r.bind(lx, "uname", "()Landroid/system/StructUtsname;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    std::vector<uint32_t> args;
    for (const char* v : {"Linux", "localhost", "6.6.0-anyandroid", "#1 SMP PREEMPT AnyAndroid", "x86_64"}) {
      args.push_back(rt.new_string_utf8(v));
    }
    return construct(rt, "Landroid/system/StructUtsname;",
                     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", args);
  });
}

// --- sun.misc.Unsafe / jdk.internal.misc.Unsafe ----------------------------------------------------
//
// Offsets are opaque cookies: kFieldBase + 4 * slot for instance fields,
// kArrayBase + index * scale for array elements.
//
// References (32-bit handles) only flow through reference storage: the
// Object/Reference accessors need a reference field or a reference array
// element, and raw accesses (primitive accessors, copyMemory, setMemory)
// never write into one, nor into native memory as a reference. Otherwise
// bytes chosen by the application would become handles to arbitrary objects.

constexpr int64_t kFieldBase = rt::kFieldOffsetBase;
constexpr int64_t kArrayBase = 16;

struct Access {
  bool write = false;
  bool reference = false;  // Object/Reference accessors (one 4-byte handle)
};

[[noreturn]] void bad_unsafe(Runtime& rt, const std::string& why, int64_t offset) {
  rt.throw_new("Ljava/lang/IllegalArgumentException;", "Unsafe : " + why + " (offset " + std::to_string(offset) + ")");
}

bool is_ref_slot(const rt::Class* cls, uint64_t slot) {
  return std::find(cls->ref_slots.begin(), cls->ref_slots.end(), slot) != cls->ref_slots.end();
}

// `size` bytes at (obj, offset), checked against the object, its kind of
// storage and `access`.
uint8_t* locate(Runtime& rt, Handle obj, int64_t offset, uint64_t size, Access access) {
  if (obj == kNull) {
    // `offset` is an absolute address of the emulated native memory, which
    // holds bytes, never references.
    if (access.reference) bad_unsafe(rt, "référence lue ou écrite hors d'un objet", offset);
    return native_span(rt, offset, size, access.write).data();
  }
  rt::Object& o = rt.nonnull(obj, "Unsafe");
  if (auto* arr = dynamic_cast<rt::ArrayObject*>(&o)) {
    bool refs = arr->klass->component && !arr->klass->component->primitive;
    if (access.reference && !refs) bad_unsafe(rt, "référence dans un tableau primitif", offset);
    if (access.write && !access.reference && refs) {
      bad_unsafe(rt, "octets écrits dans un tableau de références", offset);
    }
    if (offset < kArrayBase || uint64_t(offset - kArrayBase) > arr->data.size() ||
        size > arr->data.size() - uint64_t(offset - kArrayBase)) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "Unsafe");
    }
    uint64_t at = uint64_t(offset - kArrayBase);
    if (access.reference && at % 4) bad_unsafe(rt, "référence non alignée", offset);
    return arr->data.data() + at;
  }
  uint64_t bytes = uint64_t(o.fields.size()) * 4;
  if (offset < kFieldBase || (offset - kFieldBase) % 4 || uint64_t(offset - kFieldBase) > bytes ||
      size > bytes - uint64_t(offset - kFieldBase)) {
    bad_unsafe(rt, "offset invalide", offset);
  }
  uint64_t at = uint64_t(offset - kFieldBase);
  if (access.reference) {
    if (!is_ref_slot(o.klass, at / 4)) bad_unsafe(rt, "le champ n'est pas une référence", offset);
  } else if (access.write) {
    for (uint64_t slot = at / 4; slot < (at + size + 3) / 4; ++slot) {
      if (is_ref_slot(o.klass, slot)) bad_unsafe(rt, "octets écrits dans un champ référence", offset);
    }
  }
  return reinterpret_cast<uint8_t*>(o.fields.data()) + at;
}

template <typename T>
T load(Runtime& rt, Handle obj, int64_t off, bool reference = false) {
  T v;
  std::memcpy(&v, locate(rt, obj, off, sizeof(T), Access{false, reference}), sizeof(T));
  return v;
}

template <typename T>
void store(Runtime& rt, Handle obj, int64_t off, T v, bool reference = false) {
  std::memcpy(locate(rt, obj, off, sizeof(T), Access{true, reference}), &v, sizeof(T));
}

void register_unsafe(rt::NativeRegistry& r) {
  for (const char* u : {"Lsun/misc/Unsafe;", "Ljdk/internal/misc/Unsafe;"}) {
    auto scale = [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* c = rt.mirror_class(a.h(0));
      if (!c) rt.throw_npe("componentType");
      switch (c->primitive) {
        case 'Z': case 'B': return 1;
        case 'C': case 'S': return 2;
        case 'J': case 'D': return 8;
        default: return 4;
      }
    };
    r.bind(u, "getArrayBaseOffsetForComponentType", "(Ljava/lang/Class;)I",
           [](Runtime&, NativeArgs&) -> uint64_t { return uint64_t(kArrayBase); });
    r.bind(u, "getArrayIndexScaleForComponentType", "(Ljava/lang/Class;)I", scale);
    // Accessors: (Object, long offset[, value]) on instance methods (arg 0 = Unsafe).
    struct Kind {
      const char* name;  // Int, Long, Object/Reference...
      char type;
    };
    for (Kind k : {Kind{"Int", 'I'}, Kind{"Long", 'J'}, Kind{"Object", 'L'}, Kind{"Reference", 'L'}, Kind{"Boolean", 'Z'},
                   Kind{"Byte", 'B'}, Kind{"Short", 'S'}, Kind{"Char", 'C'}, Kind{"Float", 'F'}, Kind{"Double", 'D'}}) {
      std::string jt = k.type == 'L' ? "Ljava/lang/Object;" : std::string(1, k.type);
      bool wide = k.type == 'J' || k.type == 'D';
      auto get = [type = k.type](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle o = a.h(1);
        int64_t off = a.i64(2);
        switch (type) {
          case 'J': case 'D': return load<uint64_t>(rt, o, off);
          case 'Z': case 'B': return uint32_t(int32_t(load<int8_t>(rt, o, off)));
          case 'S': return uint32_t(int32_t(load<int16_t>(rt, o, off)));
          case 'C': return load<uint16_t>(rt, o, off);
          default: return load<uint32_t>(rt, o, off, type == 'L');
        }
      };
      auto put = [type = k.type](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle o = a.h(1);
        int64_t off = a.i64(2);
        switch (type) {
          case 'J': case 'D': store<uint64_t>(rt, o, off, uint64_t(a.i64(4))); break;
          case 'Z': case 'B': store<uint8_t>(rt, o, off, uint8_t(a.raw(4))); break;
          case 'S': case 'C': store<uint16_t>(rt, o, off, uint16_t(a.raw(4))); break;
          default: store<uint32_t>(rt, o, off, a.raw(4), type == 'L');
        }
        return 0;
      };
      std::string getsig = "(Ljava/lang/Object;J)" + jt;
      std::string putsig = "(Ljava/lang/Object;J" + jt + ")V";
      // The GIL makes every access sequentially consistent.
      for (const char* suffix : {"", "Volatile", "Acquire", "Opaque"}) {
        r.bind(u, std::string("get") + k.name + suffix, getsig, get);
      }
      for (const char* suffix : {"", "Volatile", "Release", "Opaque"}) {
        r.bind(u, std::string("put") + k.name + suffix, putsig, put);
      }
      if (k.type == 'I' || k.type == 'J' || k.type == 'L') {
        r.bind(u, std::string("putOrdered") + k.name, putsig, put);
        std::string cas_sig = "(Ljava/lang/Object;J" + jt + jt + ")Z";
        bool ref = k.type == 'L';
        auto cas = [wide, ref](Runtime& rt, NativeArgs& a) -> uint64_t {
          Handle o = a.h(1);
          int64_t off = a.i64(2);
          if (wide) {
            uint64_t expected = uint64_t(a.i64(4)), value = uint64_t(a.i64(6));
            if (load<uint64_t>(rt, o, off) != expected) return 0;
            store<uint64_t>(rt, o, off, value);
            return 1;
          }
          uint32_t expected = a.raw(4), value = a.raw(5);
          if (load<uint32_t>(rt, o, off, ref) != expected) return 0;
          store<uint32_t>(rt, o, off, value, ref);
          return 1;
        };
        for (const char* prefix : {"compareAndSwap", "compareAndSet", "weakCompareAndSet"}) {
          r.bind(u, std::string(prefix) + k.name, cas_sig, cas);
        }
        // compareAndExchange: returns the previous value.
        auto cae = [wide, ref](Runtime& rt, NativeArgs& a) -> uint64_t {
          Handle o = a.h(1);
          int64_t off = a.i64(2);
          if (wide) {
            uint64_t old = load<uint64_t>(rt, o, off);
            if (old == uint64_t(a.i64(4))) store<uint64_t>(rt, o, off, uint64_t(a.i64(6)));
            return old;
          }
          uint32_t old = load<uint32_t>(rt, o, off, ref);
          if (old == a.raw(4)) store<uint32_t>(rt, o, off, a.raw(5), ref);
          return old;
        };
        std::string cae_sig = "(Ljava/lang/Object;J" + jt + jt + ")" + jt;
        r.bind(u, std::string("compareAndExchange") + k.name, cae_sig, cae);
      }
    }
    auto fence = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
    for (const char* f : {"loadFence", "storeFence", "fullFence"}) r.bind(u, f, "()V", fence);
    r.bind(u, "addressSize", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 8; });
    r.bind(u, "pageSize", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 4096; });
    // LockSupport.park/unpark.
    r.bind(u, "park", "(ZJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt.park_current(a.z(1), a.i64(2));
      return 0;
    });
    r.bind(u, "unpark", "(Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      if (a.h(1) == kNull) rt.throw_npe("unpark(null)");
      rt.unpark(a.h(1));
      return 0;
    });
    r.bind(u, "allocateMemory", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      int64_t bytes = a.i64(1);
      if (bytes < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "taille négative");
      return uint64_t(native_alloc(rt, uint64_t(bytes)));
    }, S::kPartial, "mémoire native émulée (native_memory.h)");
    r.bind(u, "reallocateMemory", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      int64_t bytes = a.i64(3);
      if (bytes < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "taille négative");
      return uint64_t(native_realloc(rt, a.i64(1), uint64_t(bytes)));
    });
    r.bind(u, "freeMemory", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      native_free(rt, a.i64(1));
      return 0;
    });
    // (Object, long) pairs: an object and an offset in it, or null and an address.
    auto do_copy = [](Runtime& rt, NativeArgs& a) -> uint64_t {
      int64_t bytes = a.i64(7);
      if (bytes < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "taille négative");
      if (bytes == 0) return 0;
      uint8_t* src = locate(rt, a.h(1), a.i64(2), uint64_t(bytes), Access{false, false});
      uint8_t* dst = locate(rt, a.h(4), a.i64(5), uint64_t(bytes), Access{true, false});
      std::memmove(dst, src, size_t(bytes));
      return 0;
    };
    auto do_set = [](Runtime& rt, NativeArgs& a) -> uint64_t {
      int64_t bytes = a.i64(4);
      if (bytes < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "taille négative");
      if (bytes == 0) return 0;
      std::memset(locate(rt, a.h(1), a.i64(2), uint64_t(bytes), Access{true, false}), int(uint8_t(a.raw(6))),
                  size_t(bytes));
      return 0;
    };
    r.bind(u, "setMemory", "(Ljava/lang/Object;JJB)V", do_set);
    r.bind(u, "setMemory0", "(Ljava/lang/Object;JJB)V", do_set);
    r.bind(u, "copyMemory", "(Ljava/lang/Object;JLjava/lang/Object;JJ)V", do_copy);
    r.bind(u, "copyMemory0", "(Ljava/lang/Object;JLjava/lang/Object;JJ)V", do_copy);
    r.bind(u, "getByte", "(J)B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return uint32_t(int32_t(load<int8_t>(rt, kNull, a.i64(1))));
    });
    r.bind(u, "putByte", "(JB)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      store<uint8_t>(rt, kNull, a.i64(1), uint8_t(a.raw(3)));
      return 0;
    });
    r.bind(u, "getInt", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return load<uint32_t>(rt, kNull, a.i64(1));
    });
    r.bind(u, "putInt", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      store<uint32_t>(rt, kNull, a.i64(1), a.raw(3));
      return 0;
    });
    r.bind(u, "getLong", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return load<uint64_t>(rt, kNull, a.i64(1));
    });
    r.bind(u, "putLong", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      store<uint64_t>(rt, kNull, a.i64(1), uint64_t(a.i64(3)));
      return 0;
    });
    r.bind(u, "allocateInstance", "(Ljava/lang/Class;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* c = rt.mirror_class(a.h(1));
      if (!c) rt.throw_npe("Class");
      rt.ensure_initialized(c);
      return rt.alloc(c);
    });
  }
}

// --- java.lang.invoke.VarHandle (invoke-polymorphic) --------------------------------------
//
// libcore builds FieldVarHandle / StaticFieldVarHandle (field identified by
// Field.getArtField(), an opaque cookie) and ArrayElementVarHandle; ART
// implements their access modes natively, typed by the call-site prototype.

struct VarLocation {
  char type = 'I';                 // 'Z','B','S','C','I','J','F','D' or 'L'
  uint32_t* words = nullptr;       // field storage (1 or 2 words)
  uint8_t* bytes = nullptr;        // array element storage
  rt::Class* ref_type = nullptr;   // reference storage: declared type, checked on store
  bool read_only = false;          // final field
};

char kind_of(std::string_view t) { return t.empty() ? 'V' : (t[0] == '[' ? 'L' : t[0]); }
bool is_wide(char t) { return t == 'J' || t == 'D'; }

// Canonical bits of a value of type t (narrow types sign/zero-extended).
uint64_t canonical(char t, uint64_t v) {
  switch (t) {
    case 'Z': return v & 1;
    case 'B': return uint32_t(int32_t(int8_t(v)));
    case 'C': return v & 0xFFFF;
    case 'S': return uint32_t(int32_t(int16_t(v)));
    case 'J': case 'D': return v;
    default: return uint32_t(v);
  }
}

uint64_t load_location(const VarLocation& l) {
  if (l.words) return is_wide(l.type) ? uint64_t(l.words[0]) | (uint64_t(l.words[1]) << 32) : canonical(l.type, l.words[0]);
  uint64_t v = 0;
  size_t n = l.type == 'Z' || l.type == 'B' ? 1 : l.type == 'C' || l.type == 'S' ? 2 : is_wide(l.type) ? 8 : 4;
  std::memcpy(&v, l.bytes, n);
  return canonical(l.type, v);
}

void store_location(Runtime& rt, const VarLocation& l, uint64_t v) {
  v = canonical(l.type, v);
  if (l.type == 'L' && l.ref_type && v != kNull && !rt.instance_of(Handle(v), l.ref_type)) {
    rt.throw_new(l.words ? "Ljava/lang/ClassCastException;" : "Ljava/lang/ArrayStoreException;",
                 rt.class_of(Handle(v))->java_name() + " -> " + l.ref_type->java_name());
  }
  if (l.words) {
    l.words[0] = uint32_t(v);
    if (is_wide(l.type)) l.words[1] = uint32_t(v >> 32);
    return;
  }
  size_t n = l.type == 'Z' || l.type == 'B' ? 1 : l.type == 'C' || l.type == 'S' ? 2 : is_wide(l.type) ? 8 : 4;
  std::memcpy(l.bytes, &v, n);
}

[[noreturn]] void wrong_type(Runtime& rt, const std::string& what) {
  rt.throw_new("Ljava/lang/invoke/WrongMethodTypeException;", what);
}

// Converts a call-site value of type `given` to the storage type `field`.
uint64_t to_storage(Runtime& rt, char field, char given, uint64_t v) {
  if (field == given) return v;
  if (field != 'L' && given == 'L') {
    char t;
    uint64_t bits;
    if (v != kNull && unbox(rt, Handle(v), t, bits) && t == field) return bits;
    wrong_type(rt, std::string("valeur incompatible avec le type ") + field);
  }
  if (field == 'L') return box(rt, given, v);
  // Widening primitive conversions accepted by VarHandle.
  if (field == 'J' && std::strchr("BSCI", given)) return uint64_t(int64_t(int32_t(v)));
  if (field == 'I' && std::strchr("BSC", given)) return v;
  wrong_type(rt, std::string("type ") + given + " incompatible avec " + field);
}

// Converts a stored value to the call-site return type.
uint64_t from_storage(Runtime& rt, char field, char wanted, uint64_t v) {
  if (wanted == 'V') return 0;
  if (field == wanted) return v;
  if (wanted == 'L') return field == 'L' ? v : box(rt, field, v);
  if (field == 'L') {
    char t;
    uint64_t bits;
    if (v != kNull && unbox(rt, Handle(v), t, bits) && t == wanted) return bits;
    wrong_type(rt, std::string("résultat incompatible avec le type ") + wanted);
  }
  if (wanted == 'J' && std::strchr("BSCI", field)) return uint64_t(int64_t(int32_t(v)));
  if (wanted == 'I' && std::strchr("BSC", field)) return v;
  wrong_type(rt, std::string("type ") + field + " incompatible avec " + wanted);
}

uint64_t numeric_add(Runtime& rt, char t, uint64_t a, uint64_t b) {
  switch (t) {
    case 'J': return a + b;
    case 'F': {
      float x, y;
      uint32_t ua = uint32_t(a), ub = uint32_t(b);
      std::memcpy(&x, &ua, 4);
      std::memcpy(&y, &ub, 4);
      return rt::ret_float(x + y);
    }
    case 'D': {
      double x, y;
      std::memcpy(&x, &a, 8);
      std::memcpy(&y, &b, 8);
      return rt::ret_double(x + y);
    }
    case 'Z': case 'L':
      rt.throw_new("Ljava/lang/UnsupportedOperationException;", "getAndAdd sur un type non numérique");
    default: return uint32_t(a) + uint32_t(b);
  }
}

uint64_t varhandle_access(Runtime& rt, const std::string& mode, const std::string& callsite, const uint32_t* args,
                          uint32_t count) {
  std::vector<std::string> params;
  std::string ret;
  if (!dex::split_signature(callsite, params, ret)) wrong_type(rt, "prototype d'appel invalide " + callsite);
  std::vector<uint64_t> vals;
  uint32_t w = 1;  // argument 0 is the VarHandle
  for (const auto& p : params) {
    bool wide = p == "J" || p == "D";
    if (w + (wide ? 2 : 1) > count) wrong_type(rt, "arguments insuffisants pour " + callsite);
    vals.push_back(wide ? uint64_t(args[w]) | (uint64_t(args[w + 1]) << 32) : args[w]);
    w += wide ? 2 : 1;
  }

  rt::Object& vh = rt.nonnull(args[0], "VarHandle");
  bool is_field = false, is_static = false, is_array = false;
  for (rt::Class* c = vh.klass; c; c = c->super) {
    if (c->descriptor == "Ljava/lang/invoke/StaticFieldVarHandle;") is_static = true;
    if (c->descriptor == "Ljava/lang/invoke/FieldVarHandle;") is_field = true;
    if (c->descriptor == "Ljava/lang/invoke/ArrayElementVarHandle;") is_array = true;
  }
  if (!is_field && !is_array) {
    rt.throw_unsupported(vh.klass->descriptor + "->" + mode, "seuls les VarHandle de champ et d'élément de tableau sont pris en charge");
  }
  size_t coords = is_array ? 2 : (is_static ? 0 : 1);
  if (params.size() < coords) wrong_type(rt, "coordonnées manquantes pour " + callsite);

  VarLocation loc;
  if (is_field) {
    rt::Field* cookie_field = find_field(vh.klass, "artField", "J");
    if (!cookie_field) rt.throw_new("Ljava/lang/InternalError;", "FieldVarHandle sans artField");
    uint64_t cookie = uint64_t(vh.fields.at(cookie_field->slot)) | (uint64_t(vh.fields.at(cookie_field->slot + 1)) << 32);
    rt::Field* f = rt.field_from_cookie(cookie);
    loc.type = kind_of(f->type);
    loc.read_only = (f->access & dex::kAccFinal) != 0;
    if (f->is_static() != is_static) rt.throw_new("Ljava/lang/IllegalStateException;", "VarHandle incohérent pour " + f->name);
    if (is_static) {
      rt.ensure_initialized(f->owner);
      if (f->slot + (is_wide(loc.type) ? 2u : 1u) > f->owner->statics.size()) {
        rt.throw_new("Ljava/lang/InternalError;", "champ statique hors limites");
      }
      loc.words = &f->owner->statics[f->slot];
    } else {
      if (kind_of(params[0]) != 'L') wrong_type(rt, "receveur attendu pour " + callsite);
      rt::Object& o = rt.nonnull(Handle(vals[0]), "VarHandle : receveur");
      if (!rt.is_assignable(o.klass, f->owner)) {
        rt.throw_new("Ljava/lang/ClassCastException;", o.klass->java_name() + " -> " + f->owner->java_name());
      }
      if (f->slot + (is_wide(loc.type) ? 2u : 1u) > o.fields.size()) rt.throw_new("Ljava/lang/InternalError;", "champ hors de l'objet");
      loc.words = &o.fields[f->slot];
    }
    if (loc.type == 'L') loc.ref_type = rt.find_class(f->type);
  } else {
    if (kind_of(params[0]) != 'L' || params[1] != "I") wrong_type(rt, "coordonnées (tableau, int) attendues : " + callsite);
    rt::ArrayObject& arr = rt.array(Handle(vals[0]), "VarHandle : tableau");
    int32_t idx = int32_t(vals[1]);
    if (idx < 0 || uint32_t(idx) >= arr.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "length=" + std::to_string(arr.length) + "; index=" + std::to_string(idx));
    }
    rt::Class* comp = arr.klass->component;
    loc.type = comp->primitive ? comp->primitive : 'L';
    loc.bytes = arr.data.data() + size_t(idx) * arr.width;
    if (loc.type == 'L') loc.ref_type = comp;
  }

  auto starts = [&](const char* p) { return mode.rfind(p, 0) == 0; };
  size_t nvalues = params.size() - coords;
  auto need = [&](size_t n) {
    if (nvalues != n) wrong_type(rt, mode + " : " + std::to_string(n) + " valeur(s) attendue(s), prototype " + callsite);
  };
  auto value = [&](size_t i) { return to_storage(rt, loc.type, kind_of(params[coords + i]), vals[coords + i]); };
  auto result = [&](uint64_t v) { return from_storage(rt, loc.type, kind_of(ret), v); };
  auto writable = [&] {
    if (loc.read_only) rt.throw_new("Ljava/lang/UnsupportedOperationException;", "VarHandle en lecture seule (champ final)");
  };

  if (mode == "get" || mode == "getVolatile" || mode == "getAcquire" || mode == "getOpaque") {
    need(0);
    return result(load_location(loc));
  }
  if (mode == "set" || mode == "setVolatile" || mode == "setRelease" || mode == "setOpaque") {
    need(1);
    writable();
    store_location(rt, loc, value(0));
    return 0;
  }
  // The GIL serialises Java threads: every access mode is atomic and
  // sequentially consistent.
  if (starts("compareAndSet") || starts("weakCompareAndSet")) {
    need(2);
    writable();
    uint64_t expected = canonical(loc.type, value(0)), update = value(1);
    if (load_location(loc) != expected) return 0;
    store_location(rt, loc, update);
    return 1;
  }
  if (starts("compareAndExchange")) {
    need(2);
    writable();
    uint64_t old = load_location(loc);
    if (old == canonical(loc.type, value(0))) store_location(rt, loc, value(1));
    return result(old);
  }
  if (starts("getAndSet")) {
    need(1);
    writable();
    uint64_t old = load_location(loc);
    store_location(rt, loc, value(0));
    return result(old);
  }
  if (starts("getAndAdd")) {
    need(1);
    writable();
    uint64_t old = load_location(loc);
    store_location(rt, loc, numeric_add(rt, loc.type, old, value(0)));
    return result(old);
  }
  if (starts("getAndBitwise")) {
    need(1);
    writable();
    if (loc.type == 'L' || loc.type == 'F' || loc.type == 'D') {
      rt.throw_new("Ljava/lang/UnsupportedOperationException;", mode + " sur un type non entier");
    }
    uint64_t old = load_location(loc), v = value(0), out;
    if (starts("getAndBitwiseOr")) out = old | v;
    else if (starts("getAndBitwiseAnd")) out = old & v;
    else out = old ^ v;
    store_location(rt, loc, out);
    return result(old);
  }
  rt.throw_new("Ljava/lang/UnsupportedOperationException;", "mode d'accès VarHandle inconnu : " + mode);
}

// --- java.lang.invoke.MethodHandle ---------------------------------------------------------------
//
// art/runtime/method_handles.cc (android-17.0.0_r1). invokeExact requires the
// call site's type to be exactly the handle's; invoke() is invokeExact on the
// handle's asType(call site) adapter, libcore's Java transformers, unless the
// call site converts in place (MethodType::IsInPlaceConvertible). A transformer
// runs MethodHandle.transformInternal on an EmulatedStackFrame holding the
// arguments; the transformers call back through invokeExactWithFrame.

// MethodHandle's kinds in Android 17 (mirror::MethodHandle::Kind).
enum MethodHandleKind : int32_t {
  kInvokeVirtual = 0,
  kInvokeSuper = 1,
  kInvokeDirect = 2,
  kInvokeStatic = 3,
  kInvokeInterface = 4,
  kInvokeTransform = 5,
  kInvokeVarHandle = 6,
  kInvokeVarHandleExact = 7,
  kInstanceGet = 8,
  kInstancePut = 9,
  kStaticGet = 10,
  kStaticPut = 11,
};

// A method type as descriptors. Classes are named by their descriptor in the
// runtime's single class namespace, so equal descriptors are IsExactMatch.
struct TypeDesc {
  std::vector<std::string> params;
  std::string ret;
  bool operator==(const TypeDesc&) const = default;
};

TypeDesc type_from_signature(Runtime& rt, const std::string& sig) {
  TypeDesc t;
  if (!dex::split_signature(sig, t.params, t.ret)) wrong_type(rt, "prototype d'appel invalide " + sig);
  return t;
}

TypeDesc type_from_object(Runtime& rt, Handle mt) {
  rt::Object& o = rt.nonnull(mt, "MethodType");
  rt::Field* rf = find_field(o.klass, "rtype", "Ljava/lang/Class;");
  rt::Field* pf = find_field(o.klass, "ptypes", "[Ljava/lang/Class;");
  if (!rf || !pf) rt.throw_new("Ljava/lang/InternalError;", "MethodType incomplet");
  TypeDesc t;
  t.ret = rt.mirror_class(Handle(o.fields.at(rf->slot)))->descriptor;
  rt::ArrayObject& arr = rt.array(Handle(o.fields.at(pf->slot)), "MethodType.ptypes");
  for (uint32_t i = 0; i < arr.length; ++i) {
    Handle c;
    std::memcpy(&c, &arr.data[size_t(i) * 4], 4);
    t.params.push_back(rt.mirror_class(c)->descriptor);
  }
  return t;
}

// MethodType.methodType(rtype, ptypes): the MethodType object of a call site.
Handle type_object(Runtime& rt, const TypeDesc& t) {
  Handle ptypes = rt.new_array("[Ljava/lang/Class;", int32_t(t.params.size()));
  rt::GlobalRef keep(rt, ptypes);
  for (size_t i = 0; i < t.params.size(); ++i) {
    Handle c = rt.class_mirror(rt.find_class(t.params[i]));
    std::memcpy(&rt.array(ptypes, "ptypes").data[i * 4], &c, 4);
  }
  return Handle(rt.call_static("Ljava/lang/invoke/MethodType;", "methodType",
                               "(Ljava/lang/Class;[Ljava/lang/Class;)Ljava/lang/invoke/MethodType;",
                               {rt.class_mirror(rt.find_class(t.ret)), ptypes}));
}

// MethodType::PrettyDescriptor: "(int, java.lang.String)void".
std::string pretty_type(const TypeDesc& t) {
  std::string s = "(";
  for (size_t i = 0; i < t.params.size(); ++i) s += (i ? ", " : "") + text::descriptor_to_java(t.params[i]);
  return s + ")" + text::descriptor_to_java(t.ret);
}

[[noreturn]] void throw_wrong_method_type(Runtime& rt, const TypeDesc& expected, const TypeDesc& actual) {
  wrong_type(rt, "Expected " + pretty_type(expected) + " but was " + pretty_type(actual));
}

Handle handle_field(Runtime& rt, Handle mh, const char* name, const char* type) {
  rt::Object& o = rt.nonnull(mh, "MethodHandle");
  rt::Field* f = find_field(o.klass, name, type);
  if (!f) rt.throw_new("Ljava/lang/InternalError;", std::string("MethodHandle.") + name + " absent");
  return Handle(o.fields.at(f->slot));
}

int32_t handle_kind(Runtime& rt, Handle mh) { return int32_t(handle_field(rt, mh, "handleKind", "I")); }

TypeDesc handle_type(Runtime& rt, Handle mh) {
  return type_from_object(rt, handle_field(rt, mh, "type", "Ljava/lang/invoke/MethodType;"));
}

uint64_t art_field_or_method(Runtime& rt, Handle mh) {
  rt::Object& o = rt.nonnull(mh, "MethodHandle");
  rt::Field* f = find_field(o.klass, "artFieldOrMethod", "J");
  if (!f) rt.throw_new("Ljava/lang/InternalError;", "MethodHandle.artFieldOrMethod absent");
  return uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32);
}

// IsParameterInPlaceConvertible: the same register representation.
bool in_place_convertible(Runtime& rt, const std::string& from, const std::string& to) {
  if (from == to) return true;
  char f = kind_of(from), t = kind_of(to);
  if ((f == 'L') != (t == 'L')) return false;  // boxing or unboxing
  if (f != 'L') {
    if (f == 'B') return t == 'S' || t == 'I';
    if (f == 'C' || f == 'S') return t == 'I';
    return false;
  }
  return rt.is_assignable(rt.find_class(from), rt.find_class(to));
}

// MethodType::IsInPlaceConvertible.
bool in_place_convertible(Runtime& rt, const TypeDesc& callsite, const TypeDesc& target) {
  if (callsite.params.size() != target.params.size()) return false;
  for (size_t i = 0; i < callsite.params.size(); ++i) {
    if (!in_place_convertible(rt, callsite.params[i], target.params[i])) return false;
  }
  return callsite.ret == "V" || in_place_convertible(rt, target.ret, callsite.ret);
}

uint64_t invoke_exact(Runtime& rt, Handle mh, const TypeDesc& callsite, const uint32_t* words, uint32_t n);

// DoMethodHandleInvokeMethod and RefineTargetMethod.
uint64_t invoke_method_target(Runtime& rt, Handle mh, int32_t kind, const TypeDesc& type, const uint32_t* words,
                              uint32_t n) {
  rt::Method* target = rt.method_from_cookie(art_field_or_method(rt, mh));
  Handle receiver = n > 0 ? Handle(words[0]) : kNull;
  rt::Method* called = target;
  if (kind == kInvokeVirtual || kind == kInvokeInterface) {
    if (receiver == kNull) rt.throw_npe("null receiver");
    if (!rt.instance_of(receiver, target->owner)) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "Expected receiver of type " + target->owner->java_name() +
                                                               ", but got " + rt.class_of(receiver)->java_name());
    }
    called = rt.resolve_virtual(rt.class_of(receiver), target->name, target->signature);
  } else if (kind == kInvokeDirect) {
    if (receiver == kNull) rt.throw_npe("null receiver");
  } else if (kind == kInvokeSuper) {
    // The static type of the receiver, recorded in the handle's type: the
    // special caller of the lookup.
    rt::Class* referrer = rt.find_class(type.params.at(0));
    if (referrer != target->owner && !target->owner->is_interface() && referrer->super) {
      called = rt.resolve_virtual(referrer->super, target->name, target->signature);
    }
  } else if (kind == kInvokeStatic) {
    rt.ensure_initialized(target->owner);
  }
  if (!called || called->is_abstract()) {
    rt.throw_new("Ljava/lang/AbstractMethodError;", target->pretty());
  }
  return rt.invoke(called, words, n);
}

// MethodHandleFieldAccess.
uint64_t field_access(Runtime& rt, Handle mh, int32_t kind, const uint32_t* words, uint32_t n) {
  rt::Field* f = rt.field_from_cookie(art_field_or_method(rt, mh));
  char t = kind_of(f->type);
  uint32_t* slot = nullptr;
  uint32_t value_at = 0;
  if (kind == kInstanceGet || kind == kInstancePut) {
    if (n == 0 || words[0] == kNull) rt.throw_npe("Receiver is null");
    slot = &rt.object(words[0])->fields.at(f->slot);
    value_at = 1;
  } else {
    rt.ensure_initialized(f->owner);
    slot = &f->owner->statics.at(f->slot);
  }
  if (kind == kInstanceGet || kind == kStaticGet) {
    return is_wide(t) ? uint64_t(slot[0]) | (uint64_t(slot[1]) << 32) : canonical(t, slot[0]);
  }
  slot[0] = uint32_t(canonical(t, words[value_at]));
  if (is_wide(t)) slot[1] = words[value_at + 1];
  return 0;
}

// --- dalvik.system.EmulatedStackFrame (art/runtime/mirror/emulated_stack_frame.cc): the
// references in `references`, the primitives packed in `stackFrame` (4 bytes,
// 8 for long and double), then the slot of the return value.

struct FrameArrays {
  Handle references = kNull;
  Handle stack_frame = kNull;
};

FrameArrays frame_arrays(Runtime& rt, Handle frame) {
  rt::Object& o = rt.nonnull(frame, "EmulatedStackFrame");
  rt::Field* r = find_field(o.klass, "references", "[Ljava/lang/Object;");
  rt::Field* s = find_field(o.klass, "stackFrame", "[B");
  if (!r || !s) rt.throw_new("Ljava/lang/InternalError;", "EmulatedStackFrame incomplet");
  return {Handle(o.fields.at(r->slot)), Handle(o.fields.at(s->slot))};
}

// CreateFromShadowFrameAndArgs: a frame of `type` holding the arguments.
Handle create_frame(Runtime& rt, Handle type_object_handle, const TypeDesc& type, const uint32_t* words, uint32_t n) {
  size_t frame_size = 0, refs = 0;
  auto count = [&](const std::string& d) {
    char k = kind_of(d);
    if (k == 'L') {
      ++refs;
    } else {
      frame_size += is_wide(k) ? 8 : 4;
    }
  };
  for (const auto& p : type.params) count(p);
  count(type.ret);
  Handle references = rt.new_array("[Ljava/lang/Object;", int32_t(refs));
  rt::GlobalRef keep_refs(rt, references);
  Handle stack_frame = rt.new_array("[B", int32_t(frame_size));
  rt::GlobalRef keep_frame(rt, stack_frame);
  size_t r = 0, at = 0;
  uint32_t w = 0;
  for (const auto& p : type.params) {
    char k = kind_of(p);
    uint32_t width = k == 'L' ? 1 : is_wide(k) ? 2 : 1;
    if (w + width > n) wrong_type(rt, "arguments insuffisants pour " + pretty_type(type));
    if (k == 'L') {
      std::memcpy(&rt.array(references, "references").data[r++ * 4], &words[w], 4);
    } else {
      std::memcpy(&rt.array(stack_frame, "stackFrame").data[at], &words[w], width * 4);
      at += width * 4;
    }
    w += width;
  }
  rt::Class* esf = rt.find_class("Ldalvik/system/EmulatedStackFrame;");
  Handle frame = rt.alloc(esf);
  rt::Object& o = *rt.object(frame);
  o.fields.at(find_field(esf, "type", "Ljava/lang/invoke/MethodType;")->slot) = type_object_handle;
  o.fields.at(find_field(esf, "references", "[Ljava/lang/Object;")->slot) = references;
  o.fields.at(find_field(esf, "stackFrame", "[B")->slot) = stack_frame;
  return frame;
}

// WriteToShadowFrame: the arguments of a frame of `type` as registers.
std::vector<uint32_t> frame_arguments(Runtime& rt, Handle frame, const TypeDesc& type) {
  FrameArrays a = frame_arrays(rt, frame);
  std::vector<uint32_t> words;
  size_t r = 0, at = 0;
  for (const auto& p : type.params) {
    char k = kind_of(p);
    if (k == 'L') {
      Handle h;
      std::memcpy(&h, &rt.array(a.references, "references").data.at(r++ * 4), 4);
      words.push_back(h);
      continue;
    }
    uint32_t width = is_wide(k) ? 2 : 1;
    rt::ArrayObject& bytes = rt.array(a.stack_frame, "stackFrame");
    if (at + width * 4 > bytes.length) rt.throw_new("Ljava/lang/InternalError;", "EmulatedStackFrame trop court");
    for (uint32_t i = 0; i < width; ++i) {
      uint32_t v;
      std::memcpy(&v, &bytes.data[at], 4);
      words.push_back(v);
      at += 4;
    }
  }
  return words;
}

// GetReturnValue / SetReturnValue: the last reference, or the last 4 or 8 bytes.
uint64_t frame_return_value(Runtime& rt, Handle frame, const std::string& ret) {
  FrameArrays a = frame_arrays(rt, frame);
  char k = kind_of(ret);
  if (k == 'L') {
    rt::ArrayObject& refs = rt.array(a.references, "references");
    Handle h;
    std::memcpy(&h, &refs.data[size_t(refs.length - 1) * 4], 4);
    return h;
  }
  rt::ArrayObject& bytes = rt.array(a.stack_frame, "stackFrame");
  size_t width = is_wide(k) ? 8 : 4;
  uint64_t v = 0;
  std::memcpy(&v, &bytes.data[bytes.length - width], width);
  return v;
}

void set_frame_return_value(Runtime& rt, Handle frame, const std::string& ret, uint64_t v) {
  FrameArrays a = frame_arrays(rt, frame);
  char k = kind_of(ret);
  if (k == 'L') {
    rt::ArrayObject& refs = rt.array(a.references, "references");
    Handle h = Handle(v);
    std::memcpy(&refs.data[size_t(refs.length - 1) * 4], &h, 4);
    return;
  }
  rt::ArrayObject& bytes = rt.array(a.stack_frame, "stackFrame");
  size_t width = is_wide(k) ? 8 : 4;
  std::memcpy(&bytes.data[bytes.length - width], &v, width);
}

// MethodHandleInvokeTransform: transformInternal(frame) on the handle.
uint64_t invoke_transform(Runtime& rt, Handle mh, const TypeDesc& type, const uint32_t* words, uint32_t n) {
  rt::Method* transform_internal = rt.method_from_cookie(art_field_or_method(rt, mh));
  Handle frame = create_frame(rt, handle_field(rt, mh, "type", "Ljava/lang/invoke/MethodType;"), type, words, n);
  uint32_t args[2] = {mh, frame};
  rt.invoke(transform_internal, args, 2);
  return frame_return_value(rt, frame, type.ret);
}

// DoVarHandleInvokeTranslation: a VarHandle invoker (MethodHandles.varHandleInvoker).
uint64_t varhandle_translation(Runtime& rt, Handle mh, int32_t kind, const TypeDesc& callsite, const uint32_t* words,
                               uint32_t n) {
  TypeDesc type = handle_type(rt, mh);
  const char* vh_desc = "Ljava/lang/invoke/VarHandle;";
  if (callsite.params.empty() || type.params.empty() || type.params[0] != vh_desc ||
      !rt.is_assignable(rt.find_class(callsite.params[0]), rt.find_class(vh_desc))) {
    throw_wrong_method_type(rt, type, callsite);
  }
  if (n == 0 || words[0] == kNull) rt.throw_npe("Expected argument 1 to be a non-null VarHandle");
  // The access mode of the invoker's target, a VarHandle accessor.
  std::string mode = rt.method_from_cookie(art_field_or_method(rt, mh))->name;
  auto without_vh = [](TypeDesc t) {
    t.params.erase(t.params.begin());
    return t;
  };
  if (kind == kInvokeVarHandleExact) {
    Handle access_mode = Handle(rt.call_static("Ljava/lang/invoke/VarHandle$AccessMode;", "valueFromMethodName",
                                               "(Ljava/lang/String;)Ljava/lang/invoke/VarHandle$AccessMode;",
                                               {rt.new_string_utf8(mode)}));
    TypeDesc vh_type = type_from_object(
        rt, Handle(rt.call_virtual(words[0], "accessModeType",
                                   "(Ljava/lang/invoke/VarHandle$AccessMode;)Ljava/lang/invoke/MethodType;", {access_mode})));
    if (!(without_vh(type) == vh_type)) throw_wrong_method_type(rt, vh_type, without_vh(type));
  }
  TypeDesc accessor = without_vh(callsite);
  std::string sig = "(";
  for (const auto& p : accessor.params) sig += p;
  return varhandle_access(rt, mode, sig + ")" + accessor.ret, words, n);
}

// MethodHandleInvokeExact.
uint64_t invoke_exact(Runtime& rt, Handle mh, const TypeDesc& callsite, const uint32_t* words, uint32_t n) {
  TypeDesc type = handle_type(rt, mh);
  if (!(callsite == type)) throw_wrong_method_type(rt, type, callsite);
  int32_t kind = handle_kind(rt, mh);
  switch (kind) {
    case kInvokeVirtual:
    case kInvokeSuper:
    case kInvokeDirect:
    case kInvokeStatic:
    case kInvokeInterface: return invoke_method_target(rt, mh, kind, type, words, n);
    case kInstanceGet:
    case kInstancePut:
    case kStaticGet:
    case kStaticPut: return field_access(rt, mh, kind, words, n);
    case kInvokeTransform: return invoke_transform(rt, mh, callsite, words, n);
    case kInvokeVarHandle:
    case kInvokeVarHandleExact: return varhandle_translation(rt, mh, kind, callsite, words, n);
    default: rt.throw_new("Ljava/lang/InternalError;", "genre de MethodHandle inconnu " + std::to_string(kind));
  }
}

// MethodHandleInvoke: invokeExact on the handle, or on its asType adapter
// (MethodHandle.asTypeCache, else MethodHandle.asType).
uint64_t invoke_converting(Runtime& rt, Handle mh, const TypeDesc& callsite, const uint32_t* words, uint32_t n) {
  TypeDesc type = handle_type(rt, mh);
  if (in_place_convertible(rt, callsite, type)) return invoke_exact(rt, mh, type, words, n);
  Handle adapter = handle_field(rt, mh, "asTypeCache", "Ljava/lang/invoke/MethodHandle;");
  if (adapter == kNull || !(callsite == handle_type(rt, adapter))) {
    Handle mt = type_object(rt, callsite);
    adapter = Handle(rt.call_virtual(mh, "asType", "(Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;", {mt}));
  }
  return invoke_exact(rt, adapter, callsite, words, n);
}

uint64_t methodhandle_invoke(Runtime& rt, const std::string& name, const std::string& callsite, const uint32_t* args,
                             uint32_t count) {
  TypeDesc type = type_from_signature(rt, callsite);
  if (name == "invokeExact") return invoke_exact(rt, args[0], type, args + 1, count - 1);
  if (name == "invoke") return invoke_converting(rt, args[0], type, args + 1, count - 1);
  rt.throw_new("Ljava/lang/NoSuchMethodError;", "MethodHandle." + name);
}

void register_invoke(rt::NativeRegistry& r) {
  // java_lang_invoke_MethodHandleImpl.cc: the Field, Method or Constructor of a direct handle.
  r.bind("Ljava/lang/invoke/MethodHandleImpl;", "getMemberInternal", "()Ljava/lang/reflect/Member;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           uint64_t target = art_field_or_method(rt, a.h(0));
           if (handle_kind(rt, a.h(0)) >= kInstanceGet) return rt.reflect_field(rt.field_from_cookie(target));
           return rt.reflect_method(rt.method_from_cookie(target));
         });
  // MethodHandleInvokeExactWithFrame: the transformers' call of another handle.
  r.bind("Ljava/lang/invoke/MethodHandle;", "invokeExactWithFrame", "(Ldalvik/system/EmulatedStackFrame;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           Handle frame = a.h(1);
           rt::Object& o = rt.nonnull(frame, "EmulatedStackFrame");
           rt::Field* tf = find_field(o.klass, "type", "Ljava/lang/invoke/MethodType;");
           if (!tf) rt.throw_new("Ljava/lang/InternalError;", "EmulatedStackFrame.type absent");
           TypeDesc type = type_from_object(rt, Handle(o.fields.at(tf->slot)));
           std::vector<uint32_t> words = frame_arguments(rt, frame, type);
           uint64_t result = invoke_exact(rt, a.h(0), type, words.data(), uint32_t(words.size()));
           set_frame_return_value(rt, frame, type.ret, result);
           return 0;
         });
  r.set_polymorphic_handler([](Runtime& rt, rt::Class* declared, const std::string& name, const std::string& callsite,
                               const uint32_t* args, uint32_t count) -> uint64_t {
    for (rt::Class* c = declared; c; c = c->super) {
      if (c->descriptor == "Ljava/lang/invoke/VarHandle;") return varhandle_access(rt, name, callsite, args, count);
      if (c->descriptor == "Ljava/lang/invoke/MethodHandle;") return methodhandle_invoke(rt, name, callsite, args, count);
    }
    rt.throw_unsupported(declared->descriptor + "->" + name + callsite,
                         "invoke-polymorphic non pris en charge sur " + declared->java_name());
  });
}

}  // namespace

namespace {

// java.nio.DirectByteBuffer absolute getters. libcore's bytecode spends five
// calls per access (isAccessible, checkIndex, ix, get(long), Memory.peek*);
// these intrinsics keep its checks and exception messages and read through
// the emulated native memory.
struct DirectBuffer {
  int64_t address;
  int32_t limit;
  bool native_order;
};

DirectBuffer direct_buffer(Runtime& rt, Handle self) {
  rt::Object& o = rt.nonnull(self, "DirectByteBuffer");
  rt::Field* ref = find_field(o.klass, "memoryRef", "Ljava/nio/DirectByteBuffer$MemoryRef;");
  rt::Field* limit = find_field(o.klass, "limit", "I");
  rt::Field* address = find_field(o.klass, "address", "J");
  rt::Field* order = find_field(o.klass, "nativeByteOrder", "Z");
  if (!ref || !limit || !address || !order) rt.throw_new("Ljava/lang/InternalError;", "DirectByteBuffer : champs absents");
  rt::Object& mem = rt.nonnull(Handle(o.fields.at(ref->slot)), "DirectByteBuffer.memoryRef");
  rt::Field* accessible = find_field(mem.klass, "isAccessible", "Z");
  if (!accessible) rt.throw_new("Ljava/lang/InternalError;", "MemoryRef sans isAccessible");
  if (mem.fields.at(accessible->slot) == 0) rt.throw_new("Ljava/lang/IllegalStateException;", "buffer is inaccessible");
  int64_t at = int64_t(uint64_t(o.fields.at(address->slot)) | (uint64_t(o.fields.at(address->slot + 1)) << 32));
  return {at, int32_t(o.fields.at(limit->slot)), o.fields.at(order->slot) != 0};
}

template <typename T>
uint64_t direct_get(Runtime& rt, NativeArgs& a) {
  DirectBuffer b = direct_buffer(rt, a.h(0));
  int32_t i = a.i32(1);
  constexpr int32_t nb = int32_t(sizeof(T));
  // 64-bit: `limit` is a field the application can set to anything.
  if (i < 0 || int64_t(i) + nb > int64_t(b.limit)) {
    std::string msg = "index=" + std::to_string(i) + " out of bounds (limit=" + std::to_string(b.limit);
    if (nb > 1) msg += ", nb=" + std::to_string(nb);
    rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", msg + ")");
  }
  // `address` is a field of the buffer, so any value: unsigned arithmetic, the
  // sum being just another address that native_span checks.
  T v;
  std::memcpy(&v, native_span(rt, int64_t(uint64_t(b.address) + uint32_t(i)), sizeof(T)).data(), sizeof(T));
  if (!b.native_order) {
    if constexpr (sizeof(T) == 2) v = T(__builtin_bswap16(uint16_t(v)));
    if constexpr (sizeof(T) == 4) v = T(__builtin_bswap32(uint32_t(v)));
  }
  return uint32_t(int32_t(v));
}

void register_direct_buffers(rt::NativeRegistry& r) {
  const char* dbb = "Ljava/nio/DirectByteBuffer;";
  r.intrinsic(dbb, "get", "(I)B", direct_get<int8_t>, "accès direct à la mémoire native émulée");
  r.intrinsic(dbb, "getShort", "(I)S", direct_get<int16_t>, "accès direct à la mémoire native émulée");
  r.intrinsic(dbb, "getInt", "(I)I", direct_get<int32_t>, "accès direct à la mémoire native émulée");
}

}  // namespace

void register_libcore_natives(rt::NativeRegistry& r) {
  register_system(r);
  register_strings(r);
  register_vm(r);
  register_os(r);
  register_unsafe(r);
  register_direct_buffers(r);
  register_invoke(r);
  register_libcore_charsets(r);
  register_libcore_files(r);
  register_libcore_nio(r);
  register_libcore_zip(r);
  register_libcore_crypto(r);
  register_libcore_dex(r);
  register_libcore_regex(r);
  register_framework_natives(r);
  register_appmem_natives(r);
}

int64_t unsafe_field_offset(uint32_t slot) { return kFieldBase + 4 * int64_t(slot); }

void start_libcore(rt::Runtime& rt) {
  if (!rt.libcore_mode()) return;
  // Linux.writeBytes must deliver the bytes unchanged ("\n" stays "\n").
  std::fflush(stdout);
  std::fflush(stderr);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  // ART initialises System while it creates the system class loader, before
  // any application code: System.<clinit> needs VMRuntime, whose own
  // initialisation reaches System again (System.arraycopy), which is only
  // harmless in this order.
  rt.ensure_initialized(rt.find_class("Ljava/lang/System;"));
  // ART then makes the system class loader the main thread's context class
  // loader (Runtime::Start); threads created later inherit it.
  rt::Handle loader = rt.call_static("Ljava/lang/ClassLoader;", "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  rt::Object& main_thread = rt.nonnull(rt.current_thread_peer(), "Thread");
  if (rt::Field* f = rt.find_class("Ljava/lang/Thread;")->declared_field("contextClassLoader", "Ljava/lang/ClassLoader;")) {
    main_thread.fields.at(f->slot) = loader;
  }
}

}  // namespace aa::bridge
