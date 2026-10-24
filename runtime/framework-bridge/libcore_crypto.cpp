// Message digests of conscrypt (com.android.org.conscrypt.NativeCrypto's
// EVP_MD / EVP_MD_CTX natives) on top of common/hash, behind
// MessageDigest.getInstance("MD5"/"SHA-1"/"SHA-224"/"SHA-256"/"SHA-384"/"SHA-512"),
// and RAND_bytes behind SecureRandom (conscrypt's OpenSSLRandom, the default).
//
// Java never holds a pointer. An EVP_MD is a constant naming an algorithm,
// tagged so that a context handle or a forged long is not taken for one. An
// EVP_MD_CTX lives in a bounded handle table: NativeRef$EVP_MD_CTX.address is
// a kind-tagged handle, and every call keeps its context alive with a
// shared_ptr. Contexts are streaming (constant memory whatever the message
// length). Byte arrays are bounds-checked in 64 bits and direct buffers go
// through the emulated native memory. A forged, stale or out-of-range value
// raises the exception conscrypt raises for a null or invalid argument.
//
// Lifetime: conscrypt frees a context in NativeRef.finalize(), which AnyAndroid
// never calls. Instead, the first native call that receives a
// NativeRef$EVP_MD_CTX attaches a peer to it, and the collector frees the
// context with the object. A context that is created but never handed back
// (a MessageDigest never used) stays until the process ends, so the table is
// capped at kMaxContexts (about 300 bytes each): past it, after a collection,
// EVP_MD_CTX_create throws OutOfMemoryError as conscrypt does when allocation
// fails.
#define _CRT_RAND_S  // rand_s(): the system's cryptographic generator
#include "framework-bridge/internal.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "common/hash.h"
#include "dex-engine/runtime.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/native_memory.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;
using hash::Algorithm;

constexpr size_t kMaxContexts = size_t(1) << 16;

// EVP_MD values: tag 0x0c00 in bits 48-62 (reserved for them: no HandleTable
// uses it), algorithm + 1 below. The EVP_MD_CTX table uses kind 0x0c01.
constexpr uint64_t kMdTag = uint64_t(0x0c00) << 48;

int64_t md_value(Algorithm alg) { return int64_t(kMdTag | (uint64_t(alg) + 1)); }

std::optional<Algorithm> md_algorithm(int64_t value) {
  uint64_t v = uint64_t(value);
  if ((v & ~uint64_t(0xff)) != kMdTag) return std::nullopt;
  uint64_t n = v & 0xff;
  if (n < 1 || n > uint64_t(Algorithm::kSha512) + 1) return std::nullopt;
  return Algorithm(n - 1);
}

// NativeCrypto passes EVP_MD as a long: 0 is conscrypt's NullPointerException,
// any other value that is not one of ours is treated the same way.
Algorithm need_md(Runtime& rt, int64_t value) {
  if (value == 0) rt.throw_npe("evp_md == null");
  auto alg = md_algorithm(value);
  if (!alg) rt.throw_npe("EVP_MD invalide");
  return *alg;
}

// The native side of an EVP_MD_CTX: no digest until EVP_DigestInit_ex, as
// after EVP_MD_CTX_cleanup and EVP_DigestFinal_ex.
struct EvpMdCtx {
  std::optional<hash::Digest> digest;
};

struct Tables {
  HandleTable<EvpMdCtx> contexts{0x0c01};
};

HandleTable<EvpMdCtx>& contexts() {
  // Never destroyed: peers of objects still alive at exit reach it from the
  // runtime's destructor.
  static auto* t = new Tables;
  return t->contexts;
}

// Attached to a NativeRef$EVP_MD_CTX: frees its context when the collector
// frees the object (what NativeRef.finalize() does on Android).
struct ContextOwner final : rt::NativePeer {
  explicit ContextOwner(int64_t h) : handle(h) {}
  ~ContextOwner() override { contexts().remove(handle); }
  int64_t handle;
};

constexpr const char* kCtxClass = "Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;";

int64_t long_field(const rt::Object& o, const rt::Field& f) {
  return int64_t(uint64_t(o.fields.at(f.slot)) | (uint64_t(o.fields.at(f.slot + 1)) << 32));
}

// conscrypt's fromContextObject: NullPointerException for a null reference
// or a zero address, and here also for a handle that names no live context.
std::shared_ptr<EvpMdCtx> context(Runtime& rt, Handle ref) {
  if (ref == kNull) rt.throw_npe("contextObject == null");
  rt::Object& o = rt.nonnull(ref, "NativeRef");
  rt::Field* f = find_field(o.klass, "address", "J");
  if (o.klass->descriptor != kCtxClass || !f || f->is_static()) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", o.klass->java_name() + " n'est pas une NativeRef$EVP_MD_CTX");
  }
  int64_t handle = long_field(o, *f);
  if (handle == 0) rt.throw_npe("ref == null");
  auto ctx = contexts().get(handle);
  if (!ctx) rt.throw_npe("EVP_MD_CTX : contexte natif invalide ou détruit");
  if (!o.peer) o.peer = std::make_unique<ContextOwner>(handle);
  return ctx;
}

// An update or a final on a context without digest: BoringSSL would follow a
// null EVP_MD; conscrypt reports BoringSSL failures as RuntimeException.
hash::Digest& initialized(Runtime& rt, EvpMdCtx& ctx, const char* what) {
  if (!ctx.digest) rt.throw_new("Ljava/lang/RuntimeException;", std::string(what) + " : EVP_MD_CTX non initialisé");
  return *ctx.digest;
}

[[noreturn]] void out_of_bounds(Runtime& rt, const char* what, int64_t off, int64_t len, int64_t size) {
  rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
               std::string(what) + " : décalage " + std::to_string(off) + ", longueur " + std::to_string(len) +
                   ", taille " + std::to_string(size));
}

// A Java byte[] (NullPointerException for null, with conscrypt's message).
rt::ArrayObject& byte_array(Runtime& rt, Handle array, const char* what, const char* null_message) {
  if (array == kNull) rt.throw_npe(null_message);
  rt::ArrayObject& a = rt.array(array, what);
  if (a.width != 1 || a.data.size() < a.length) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string(what) + " : tableau d'octets attendu");
  }
  return a;
}

// conscrypt's ARRAY_CHUNK_INVALID, in 64 bits: ArrayIndexOutOfBoundsException.
std::span<uint8_t> byte_range(Runtime& rt, rt::ArrayObject& a, int32_t off, int64_t len, const char* what) {
  if (off < 0 || len < 0 || int64_t(off) + len > int64_t(a.length)) out_of_bounds(rt, what, off, len, a.length);
  return {a.data.data() + off, size_t(len)};
}

}  // namespace

void register_libcore_crypto(rt::NativeRegistry& r) {
  const char* nc = "Lcom/android/org/conscrypt/NativeCrypto;";
  const std::string ref = kCtxClass;

  // Names accepted by BoringSSL's EVP_get_digestbyname as conscrypt calls it
  // (EvpMdRef: "md5", "sha1"...; case-insensitive).
  r.bind(nc, "EVP_get_digestbyname", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("algorithm == null");
    std::string name = rt.string_utf8(a.h(0));
    for (char& c : name) {
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    static const struct {
      const char* name;
      Algorithm alg;
    } kNames[] = {{"md5", Algorithm::kMd5},       {"sha1", Algorithm::kSha1},     {"sha224", Algorithm::kSha224},
                  {"sha256", Algorithm::kSha256}, {"sha384", Algorithm::kSha384}, {"sha512", Algorithm::kSha512}};
    for (const auto& n : kNames) {
      if (name == n.name) return rt::ret_long(md_value(n.alg));
    }
    rt.throw_new("Ljava/lang/RuntimeException;", "algorithme de hachage introuvable : " + name);
  }, S::kPartial, "MD5, SHA-1, SHA-224, SHA-256, SHA-384, SHA-512 (pas MD4)");

  r.bind(nc, "EVP_MD_size", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(hash::Digest(need_md(rt, a.i64(0))).size()));
  });

  r.bind(nc, "EVP_MD_CTX_create", "()J", [](Runtime& rt, NativeArgs&) -> uint64_t {
    if (contexts().live() >= kMaxContexts) {
      rt.collect_garbage();  // frees the contexts of unreachable NativeRefs
      if (contexts().live() >= kMaxContexts) {
        rt.throw_new("Ljava/lang/OutOfMemoryError;", "EVP_MD_CTX : trop de contextes de hachage vivants");
      }
    }
    return rt::ret_long(contexts().add(std::make_shared<EvpMdCtx>()));
  });

  // Called by NativeRef.finalize() on Android (never by AnyAndroid). 0 is
  // ignored, as by conscrypt; a stale or forged handle is refused.
  r.bind(nc, "EVP_MD_CTX_destroy", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t handle = a.i64(0);
    if (handle != 0 && !contexts().remove(handle)) rt.throw_npe("EVP_MD_CTX : contexte natif invalide ou détruit");
    return 0;
  });

  r.bind(nc, "EVP_MD_CTX_cleanup", "(" + ref + ")V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    context(rt, a.h(0))->digest.reset();
    return 0;
  });

  r.bind(nc, "EVP_MD_CTX_copy_ex", "(" + ref + ref + ")I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto dst = context(rt, a.h(0));
    auto src = context(rt, a.h(1));
    // BoringSSL refuses an uninitialized source; conscrypt then throws.
    if (!src->digest) rt.throw_new("Ljava/lang/RuntimeException;", "EVP_MD_CTX_copy_ex : source non initialisée");
    if (dst != src) dst->digest = src->digest;
    return rt::ret_int(1);
  });

  r.bind(nc, "EVP_DigestInit_ex", "(" + ref + "J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto ctx = context(rt, a.h(0));
    ctx->digest.emplace(need_md(rt, a.i64(1)));
    return rt::ret_int(1);
  });

  r.bind(nc, "EVP_DigestUpdate", "(" + ref + "[BII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto ctx = context(rt, a.h(0));
    rt::ArrayObject& arr = byte_array(rt, a.h(1), "inBytes", "inBytes");
    std::span<const uint8_t> in = byte_range(rt, arr, a.i32(2), a.i32(3), "inBytes");
    if (!in.empty()) initialized(rt, *ctx, "EVP_DigestUpdate").update(in);
    return 0;
  });

  // `ptr` is DirectByteBuffer.address + position: an emulated native address.
  r.bind(nc, "EVP_DigestUpdateDirect", "(" + ref + "JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto ctx = context(rt, a.h(0));
    int64_t ptr = a.i64(1);
    int32_t len = a.i32(3);
    if (ptr == 0) rt.throw_npe("ptr == null");
    if (len < 0) out_of_bounds(rt, "EVP_DigestUpdateDirect", 0, len, 0);
    if (len == 0) return 0;
    hash::Digest& d = initialized(rt, *ctx, "EVP_DigestUpdateDirect");
    d.update(native_span(rt, ptr, uint64_t(len)));
    return 0;
  });

  // Writes the digest at hash[offset] (conscrypt does not check the range;
  // here it is checked before the context is consumed) and leaves the context
  // without digest until the next EVP_DigestInit_ex.
  r.bind(nc, "EVP_DigestFinal_ex", "(" + ref + "[BI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto ctx = context(rt, a.h(0));
    rt::ArrayObject& arr = byte_array(rt, a.h(1), "hash", "hash == null");
    hash::Digest& d = initialized(rt, *ctx, "EVP_DigestFinal_ex");
    std::span<uint8_t> out = byte_range(rt, arr, a.i32(2), int64_t(d.size()), "EVP_DigestFinal_ex");
    d.finish(out);
    ctx->digest.reset();
    return rt::ret_int(int32_t(out.size()));
  });

  // NativeCrypto.<clinit>: the cipher suites of the TLS stack (none: TLS is
  // not implemented, so the SSL natives stay unbound) and AES hardware.
  r.bind(nc, "get_cipher_names", "(Ljava/lang/String;)[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt::ret_obj(string_array(rt, {}));
  }, S::kStub, "aucune suite TLS : TLS non pris en charge");
  r.bind(nc, "EVP_has_aes_hardware", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(0); });

  // OpenSSLRandom.engineNextBytes: BoringSSL's RAND_bytes reads the kernel's
  // generator; here the system's (RtlGenRandom, as /dev/urandom). A null array
  // is ScopedByteArrayRW's NullPointerException.
  r.bind(nc, "RAND_bytes", "([B)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& arr = byte_array(rt, a.h(0), "output", "output == null");
    for (size_t i = 0; i < arr.data.size();) {
      unsigned int bits = 0;
      if (rand_s(&bits) != 0) rt.throw_new("Ljava/lang/RuntimeException;", "NativeCrypto_RAND_bytes");
      for (int k = 0; k < 4 && i < arr.data.size(); ++k, ++i) arr.data[i] = uint8_t(bits >> (8 * k));
    }
    return 0;
  });

  // JNI GetDirectBufferAddress: Buffer.address, an emulated native address for
  // a direct buffer (0 otherwise, and conscrypt then copies the bytes).
  r.bind(nc, "getDirectBufferAddress", "(Ljava/nio/Buffer;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) return 0;
    rt::Object& o = rt.nonnull(a.h(0), "Buffer");
    rt::Field* f = find_field(o.klass, "address", "J");
    if (!f || f->is_static()) return 0;
    int64_t address = long_field(o, *f);
    return rt::ret_long(is_native_address(address) ? address : 0);
  });
}

}  // namespace aa::bridge
