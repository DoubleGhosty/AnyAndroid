// Small libcore natives of java.io and java.lang (libcore/ojluni/src/main/native,
// android-17.0.0_r1): the conversions of object serialization
// (ObjectInputStream.c, ObjectOutputStream.c, ObjectStreamClass.c) and
// UNIXProcess, which never starts a host process.
#include <cstdint>
#include <cstring>
#include <string>

#include "framework-bridge/internal.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// The elements [pos, pos + count) of a primitive array of `width`-byte
// elements. The C natives trust their Java callers; out-of-range indices
// throw here instead of corrupting memory.
uint8_t* elements(Runtime& rt, Handle array, int32_t pos, int32_t count, size_t width) {
  rt::ArrayObject& arr = rt.array(array, "array");
  if (pos < 0 || count < 0 || int64_t(pos) + count > int64_t(arr.length)) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
                 "length=" + std::to_string(arr.length) + "; index=" + std::to_string(int64_t(pos) + count));
  }
  return arr.data.data() + size_t(pos) * width;
}

// bytesToFloats / bytesToDoubles: big-endian bytes to IEEE values, bit for bit.
template <typename Bits>
uint64_t bytes_to_values(Runtime& rt, NativeArgs& a) {
  constexpr size_t kWidth = sizeof(Bits);
  int32_t count = a.i32(4);
  if (count == 0) return 0;
  if (a.h(0) == kNull || a.h(2) == kNull) rt.throw_npe({});
  const uint8_t* src = elements(rt, a.h(0), a.i32(1), int32_t(count * kWidth), 1);
  uint8_t* dst = elements(rt, a.h(2), a.i32(3), count, kWidth);
  for (int32_t i = 0; i < count; ++i) {
    Bits v = 0;
    for (size_t b = 0; b < kWidth; ++b) v = Bits(v << 8) | src[size_t(i) * kWidth + b];
    std::memcpy(dst + size_t(i) * kWidth, &v, kWidth);
  }
  return 0;
}

// floatsToBytes / doublesToBytes: Float.floatToIntBits and
// Double.doubleToLongBits (NaNs collapsed to the canonical NaN), big-endian.
template <typename Bits>
uint64_t values_to_bytes(Runtime& rt, NativeArgs& a) {
  constexpr size_t kWidth = sizeof(Bits);
  constexpr Bits kCanonicalNaN = kWidth == 4 ? Bits(0x7fc00000) : Bits(0x7ff8000000000000ull);
  constexpr Bits kExponent = kWidth == 4 ? Bits(0x7f800000) : Bits(0x7ff0000000000000ull);
  constexpr Bits kMantissa = kWidth == 4 ? Bits(0x007fffff) : Bits(0x000fffffffffffffull);
  int32_t count = a.i32(4);
  if (count == 0) return 0;
  if (a.h(0) == kNull || a.h(2) == kNull) rt.throw_npe({});
  const uint8_t* src = elements(rt, a.h(0), a.i32(1), count, kWidth);
  uint8_t* dst = elements(rt, a.h(2), a.i32(3), int32_t(count * kWidth), 1);
  for (int32_t i = 0; i < count; ++i) {
    Bits v;
    std::memcpy(&v, src + size_t(i) * kWidth, kWidth);
    if ((v & kExponent) == kExponent && (v & kMantissa) != 0) v = kCanonicalNaN;
    for (size_t b = 0; b < kWidth; ++b) dst[size_t(i) * kWidth + b] = uint8_t(v >> (8 * (kWidth - 1 - b)));
  }
  return 0;
}

}  // namespace

void register_libcore_lang(rt::NativeRegistry& r) {
  const char* in = "Ljava/io/ObjectInputStream;";
  r.bind(in, "bytesToFloats", "([BI[FII)V", bytes_to_values<uint32_t>);
  r.bind(in, "bytesToDoubles", "([BI[DII)V", bytes_to_values<uint64_t>);
  const char* out = "Ljava/io/ObjectOutputStream;";
  r.bind(out, "floatsToBytes", "([FI[BII)V", values_to_bytes<uint32_t>);
  r.bind(out, "doublesToBytes", "([DI[BII)V", values_to_bytes<uint64_t>);

  // ObjectStreamClass.hasStaticInitializer: JNI's GetStaticMethodID finds the
  // first <clinit> up the superclass chain (initializing the class); without
  // inheritStaticInitializer, one inherited from a superclass does not count.
  r.bind("Ljava/io/ObjectStreamClass;", "hasStaticInitializer", "(Ljava/lang/Class;Z)Z",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           rt::Class* c = rt.mirror_class(a.h(0));
           if (!c) rt.throw_npe({});
           rt.ensure_initialized(c);
           auto first_clinit = [](rt::Class* k) -> rt::Method* {
             for (; k; k = k->super) {
               if (rt::Method* m = k->declared_method("<clinit>", "()V")) return m;
             }
             return nullptr;
           };
           rt::Method* clinit = first_clinit(c);
           if (!clinit) return rt::ret_bool(false);
           if (a.z(1) || !c->super) return rt::ret_bool(true);
           return rt::ret_bool(clinit != first_clinit(c->super));
         });

  // UNIXProcess_md.c: no host process is ever started for an application, so
  // forkAndExec fails the way exec(2) refusing the program does, with
  // throwIOException's message ("error=13, Permission denied").
  const char* up = "Ljava/lang/UNIXProcess;";
  r.bind(up, "initIDs", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(up, "forkAndExec", "([B[BI[BI[B[IZ)I", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt::Handle e = construct(rt, "Ljava/io/IOException;", "(Ljava/lang/String;)V",
                             {rt.new_string_utf8("error=13, " + strerror_text(13))});
    throw rt::JavaThrow{e};
  }, S::kPartial, "aucun processus de l'hôte : exec refusé (EACCES)");
  r.bind(up, "waitForProcessExit", "(I)I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); },
         S::kStub, "aucun processus créé par forkAndExec");
  r.bind(up, "destroyProcess", "(I)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kStub,
         "aucun processus créé par forkAndExec");
}

}  // namespace aa::bridge
