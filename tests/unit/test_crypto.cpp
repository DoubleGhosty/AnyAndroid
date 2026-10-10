// conscrypt's message digest natives (libcore_crypto.cpp) against a "mini
// conscrypt" declaring the same NativeCrypto methods and NativeRef classes:
// digests, then what untrusted code can pass instead of valid arguments
// (forged or stale handles, out-of-range offsets, foreign addresses).
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/hash.h"
#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/native_memory.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kConscrypt[] = {
    R"(
.class public final Lcom/android/org/conscrypt/NativeCrypto;
.super Ljava/lang/Object;
.method static native EVP_get_digestbyname(Ljava/lang/String;)J
.end method
.method static native EVP_MD_size(J)I
.end method
.method static native EVP_MD_CTX_create()J
.end method
.method static native EVP_MD_CTX_destroy(J)V
.end method
.method static native EVP_MD_CTX_cleanup(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;)V
.end method
.method static native EVP_MD_CTX_copy_ex(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;)I
.end method
.method static native EVP_DigestInit_ex(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;J)I
.end method
.method static native EVP_DigestUpdate(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;[BII)V
.end method
.method static native EVP_DigestUpdateDirect(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;JI)V
.end method
.method static native EVP_DigestFinal_ex(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;[BI)I
.end method
)",
    R"(
.class abstract Lcom/android/org/conscrypt/NativeRef;
.super Ljava/lang/Object;
.field final address:J
.method constructor <init>(J)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-wide p1, p0, Lcom/android/org/conscrypt/NativeRef;->address:J
    return-void
.end method
)",
    R"(
.class final Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;
.super Lcom/android/org/conscrypt/NativeRef;
.method constructor <init>(J)V
    .registers 3
    invoke-direct {p0, p1, p2}, Lcom/android/org/conscrypt/NativeRef;-><init>(J)V
    return-void
.end method
)",
    R"(
.class public final Lcom/android/org/conscrypt/Probe;
.super Ljava/lang/Object;
# What OpenSSLMessageDigestJDK does: a context, its NativeRef, then
# EVP_DigestInit_ex. Only the handle is returned: the NativeRef becomes garbage.
.method public static make(J)J
    .registers 6
    invoke-static {}, Lcom/android/org/conscrypt/NativeCrypto;->EVP_MD_CTX_create()J
    move-result-wide v0
    new-instance v2, Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;
    invoke-direct {v2, v0, v1}, Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;-><init>(J)V
    invoke-static {v2, p0, p1}, Lcom/android/org/conscrypt/NativeCrypto;->EVP_DigestInit_ex(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;J)I
    return-wide v0
.end method
)",
};

[[gnu::noinline]] void scrub_stack() {
  volatile char buf[128 * 1024];
  std::memset(const_cast<char*>(buf), 0, sizeof buf);
}

uint32_t lo(int64_t v) { return uint32_t(uint64_t(v)); }
uint32_t hi(int64_t v) { return uint32_t(uint64_t(v) >> 32); }

struct CryptoVm {
  std::unique_ptr<dex::DexFile> boot;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;
  rt::Class* nc = nullptr;

  CryptoVm() {
    std::vector<tool::ClassDef> b;
    for (const char* s : kConscrypt) b.push_back(tool::assemble_smali(s, "conscrypt.smali"));
    boot = dex::DexFile::open(tool::write_dex(b), "conscrypt-test.jar!classes.dex");
    rt = std::make_unique<rt::Runtime>();
    rt->add_boot_dex(boot.get());
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
    nc = rt->find_class("Lcom/android/org/conscrypt/NativeCrypto;");
  }
  ~CryptoVm() {
    scope.reset();
    rt.reset();
  }

  uint64_t call(const char* name, const char* sig, std::vector<uint32_t> args = {}) {
    rt::Method* m = nc->declared_method(name, sig);
    if (!m) throw test::Failure(std::string("méthode absente : ") + name);
    return rt->invoke(m, args.data(), uint32_t(args.size()));
  }
  // "java.lang.X: message" of what the call throws, "" if it returns.
  template <typename F>
  std::string thrown(F&& f) {
    try {
      f();
    } catch (const rt::JavaThrow& t) {
      std::string d = rt->describe_throwable(t.exception);
      return d.substr(0, d.find('\n'));
    }
    return "";
  }

  int64_t md(const char* name) {
    return int64_t(call("EVP_get_digestbyname", "(Ljava/lang/String;)J", {rt->new_string_utf8(name)}));
  }
  int64_t create() { return int64_t(call("EVP_MD_CTX_create", "()J")); }
  // A NativeRef$EVP_MD_CTX holding any value as its address.
  rt::Handle ref(int64_t address) {
    rt::Class* c = rt->find_class("Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;");
    rt::Handle h = rt->alloc(c);
    rt::Field* f = rt->find_class("Lcom/android/org/conscrypt/NativeRef;")->declared_field("address", "J");
    rt->object(h)->fields.at(f->slot) = lo(address);
    rt->object(h)->fields.at(f->slot + 1) = hi(address);
    return h;
  }
  rt::Handle bytes(const std::string& s) {
    rt::Handle a = rt->new_array("[B", uint32_t(s.size()));
    if (!s.empty()) std::memcpy(rt->array(a, "byte[]").data.data(), s.data(), s.size());
    return a;
  }
  void init(rt::Handle ctx, int64_t md) {
    call("EVP_DigestInit_ex", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;J)I", {ctx, lo(md), hi(md)});
  }
  void update(rt::Handle ctx, rt::Handle array, int32_t off, int32_t len) {
    call("EVP_DigestUpdate", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;[BII)V",
         {ctx, array, uint32_t(off), uint32_t(len)});
  }
  void update_direct(rt::Handle ctx, int64_t ptr, int32_t len) {
    call("EVP_DigestUpdateDirect", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;JI)V",
         {ctx, lo(ptr), hi(ptr), uint32_t(len)});
  }
  int32_t final(rt::Handle ctx, rt::Handle out, int32_t off) {
    return int32_t(call("EVP_DigestFinal_ex", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;[BI)I",
                        {ctx, out, uint32_t(off)}));
  }
  void cleanup(rt::Handle ctx) {
    call("EVP_MD_CTX_cleanup", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;)V", {ctx});
  }
  int32_t copy(rt::Handle dst, rt::Handle src) {
    return int32_t(
        call("EVP_MD_CTX_copy_ex",
             "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;)I",
             {dst, src}));
  }
  void destroy(int64_t h) { call("EVP_MD_CTX_destroy", "(J)V", {lo(h), hi(h)}); }

  // Final into a fresh array, as hex.
  std::string digest_hex(rt::Handle ctx, size_t size) {
    rt::Handle out = rt->new_array("[B", uint32_t(size));
    CHECK_EQ(final(ctx, out, 0), int32_t(size));
    return hash::to_hex(rt->array(out, "byte[]").data);
  }
};

}  // namespace

TEST(crypto_digests_through_native_crypto) {
  CryptoVm vm;
  struct {
    const char* name;
    int32_t size;
    const char* abc;
  } cases[] = {
      {"md5", 16, "900150983cd24fb0d6963f7d28e17f72"},
      {"SHA1", 20, "a9993e364706816aba3e25717850c26c9cd0d89d"},
      {"sha224", 28, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
      {"sha256", 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"sha384", 48,
       "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"},
      {"Sha512", 64,
       "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80"
       "e2a9ac94fa54ca49f"},
  };
  for (const auto& c : cases) {
    int64_t md = vm.md(c.name);
    CHECK_EQ(int32_t(vm.call("EVP_MD_size", "(J)I", {lo(md), hi(md)})), c.size);
    rt::Handle ctx = vm.ref(vm.create());
    vm.init(ctx, md);
    rt::Handle abc = vm.bytes("xabcx");
    vm.update(ctx, abc, 1, 1);  // "a", then "bc": chunks are streamed
    vm.update(ctx, abc, 2, 2);
    vm.update(ctx, abc, 5, 0);  // empty range at the very end
    CHECK_EQ(vm.digest_hex(ctx, size_t(c.size)), std::string(c.abc));
    // Final leaves the context without digest; init starts a new message.
    vm.init(ctx, md);
    vm.update(ctx, vm.bytes("abc"), 0, 3);
    rt::Handle out = vm.rt->new_array("[B", uint32_t(c.size) + 3);
    CHECK_EQ(vm.final(ctx, out, 3), c.size);
    CHECK_EQ(hash::to_hex(std::span(vm.rt->array(out, "byte[]").data).subspan(3)), std::string(c.abc));
  }
}

TEST(crypto_unknown_digest_names_and_invalid_evp_md) {
  CryptoVm vm;
  // Only BoringSSL's names, as conscrypt's EVP_get_digestbyname.
  CHECK_EQ(vm.thrown([&] { vm.md("whirlpool"); }),
           std::string("java.lang.RuntimeException: algorithme de hachage introuvable : whirlpool"));
  CHECK(vm.thrown([&] { vm.md("SHA-256"); }).find("java.lang.RuntimeException") == 0);
  CHECK(vm.thrown([&] {
            vm.call("EVP_get_digestbyname", "(Ljava/lang/String;)J", {rt::kNull});
          }).find("java.lang.NullPointerException") == 0);

  rt::Handle ctx = vm.ref(vm.create());
  int64_t ctx_handle = vm.create();
  for (int64_t bad : {int64_t(0), int64_t(3), int64_t(0x0c00000000000000) | 0x40, ctx_handle}) {
    CHECK(vm.thrown([&] {
              vm.call("EVP_MD_size", "(J)I", {lo(bad), hi(bad)});
            }).find("java.lang.NullPointerException") == 0);
    CHECK(vm.thrown([&] { vm.init(ctx, bad); }).find("java.lang.NullPointerException") == 0);
  }
  CHECK_EQ(vm.thrown([&] { vm.init(ctx, 0); }), std::string("java.lang.NullPointerException: evp_md == null"));
  vm.destroy(ctx_handle);
}

TEST(crypto_context_handles_reject_forged_and_stale_values) {
  CryptoVm vm;
  int64_t md = vm.md("sha1");
  int64_t h = vm.create();
  CHECK_EQ(uint64_t(h) >> 48, uint64_t(0x0c01));  // a handle, not a pointer
  rt::Handle ctx = vm.ref(h);
  vm.init(ctx, md);

  const std::string npe = "java.lang.NullPointerException";
  CHECK_EQ(vm.thrown([&] { vm.cleanup(rt::kNull); }), npe + ": contextObject == null");
  CHECK_EQ(vm.thrown([&] { vm.cleanup(vm.ref(0)); }), npe + ": ref == null");
  for (int64_t forged :
       {int64_t(0x41414141), h + 1, h ^ (int64_t(1) << 32), (h & 0xffffffff) | (int64_t(0x0b01) << 48)}) {
    CHECK(vm.thrown([&] { vm.cleanup(vm.ref(forged)); }).find(npe) == 0);
    CHECK(vm.thrown([&] { vm.init(vm.ref(forged), md); }).find(npe) == 0);
    CHECK(vm.thrown([&] { vm.destroy(forged); }).find(npe) == 0);
  }
  // Another object than a NativeRef$EVP_MD_CTX is refused.
  CHECK(vm.thrown([&] { vm.cleanup(vm.bytes("x")); }).find("java.lang.IllegalArgumentException") == 0);

  vm.destroy(0);  // ignored, as by conscrypt
  vm.destroy(h);
  // Stale: the slot may be reused, but with another generation.
  int64_t h2 = vm.create();
  CHECK(h2 != h);
  CHECK(vm.thrown([&] { vm.destroy(h); }).find(npe) == 0);
  CHECK(vm.thrown([&] { vm.update(ctx, vm.bytes("abc"), 0, 3); }).find(npe) == 0);
  CHECK(vm.thrown([&] { vm.final(ctx, vm.rt->new_array("[B", 20), 0); }).find(npe) == 0);
  vm.destroy(h2);
}

TEST(crypto_byte_array_ranges_are_checked_in_64_bits) {
  CryptoVm vm;
  int64_t md = vm.md("sha256");
  rt::Handle ctx = vm.ref(vm.create());
  vm.init(ctx, md);
  rt::Handle data = vm.bytes(std::string(16, 'z'));
  const std::string aioobe = "java.lang.ArrayIndexOutOfBoundsException";
  // int32 off + len would wrap around and pass a 32-bit check.
  CHECK(vm.thrown([&] { vm.update(ctx, data, 1, 0x7fffffff); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.update(ctx, data, 0x7fffffff, 2); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.update(ctx, data, -1, 1); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.update(ctx, data, 0, -1); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.update(ctx, data, 17, 0); }).find(aioobe) == 0);
  CHECK_EQ(vm.thrown([&] { vm.update(ctx, rt::kNull, 0, 0); }), std::string("java.lang.NullPointerException: inBytes"));
  CHECK(vm.thrown([&] {
            vm.update(ctx, vm.rt->new_array("[J", 4), 0, 1);
          }).find("java.lang.IllegalArgumentException") == 0);

  // The digest must fit at the offset; nothing is written otherwise.
  rt::Handle out = vm.rt->new_array("[B", 40);
  CHECK(vm.thrown([&] { vm.final(ctx, out, 0x7ffffff0); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.final(ctx, out, 9); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.final(ctx, out, -1); }).find(aioobe) == 0);
  CHECK(vm.thrown([&] { vm.final(ctx, vm.rt->new_array("[B", 31), 0); }).find(aioobe) == 0);
  CHECK_EQ(vm.thrown([&] { vm.final(ctx, rt::kNull, 0); }),
           std::string("java.lang.NullPointerException: hash == null"));
  for (uint8_t b : vm.rt->array(out, "byte[]").data) CHECK_EQ(b, uint8_t(0));
  // The failed calls did not consume the context.
  CHECK_EQ(vm.final(ctx, out, 8), 32);
}

TEST(crypto_update_direct_reads_emulated_native_memory) {
  CryptoVm vm;
  int64_t md = vm.md("sha1");
  rt::Handle ctx = vm.ref(vm.create());
  vm.init(ctx, md);
  int64_t region = bridge::native_alloc(*vm.rt, 8);
  std::memcpy(bridge::native_span(*vm.rt, region, 8, true).data(), "--abc---", 8);
  vm.update_direct(ctx, region + 2, 3);  // DirectByteBuffer.address + position
  vm.update_direct(ctx, region + 8, 0);  // empty, at the end of the region
  CHECK_EQ(vm.digest_hex(ctx, 20), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));

  vm.init(ctx, md);
  // A host address or a value past the region never reaches memory.
  CHECK(vm.thrown([&] {
            vm.update_direct(ctx, int64_t(reinterpret_cast<uintptr_t>(&md)), 4);
          }).find("java.lang.IllegalArgumentException") == 0);
  CHECK(vm.thrown([&] { vm.update_direct(ctx, region + 6, 3); }).find("java.lang.IndexOutOfBoundsException") == 0);
  CHECK(vm.thrown([&] { vm.update_direct(ctx, region, -1); }).find("java.lang.ArrayIndexOutOfBoundsException") == 0);
  CHECK(vm.thrown([&] { vm.update_direct(ctx, 0, 1); }).find("java.lang.NullPointerException") == 0);
  bridge::native_free(*vm.rt, region);
  CHECK(vm.thrown([&] { vm.update_direct(ctx, region, 1); }).find("java.lang.IllegalArgumentException") == 0);
}

TEST(crypto_copy_cleanup_and_uninitialized_contexts) {
  CryptoVm vm;
  int64_t md = vm.md("sha256");
  rt::Handle src = vm.ref(vm.create());
  rt::Handle dst = vm.ref(vm.create());
  const std::string runtime = "java.lang.RuntimeException";
  // BoringSSL refuses to copy an uninitialized context.
  CHECK(vm.thrown([&] { vm.copy(dst, src); }).find(runtime) == 0);
  vm.init(src, md);
  vm.update(src, vm.bytes("ab"), 0, 2);
  CHECK_EQ(vm.copy(dst, src), 1);  // the copy carries the streaming state
  CHECK_EQ(vm.copy(src, src), 1);
  vm.update(src, vm.bytes("c"), 0, 1);
  vm.update(dst, vm.bytes("c"), 0, 1);
  const std::string abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  CHECK_EQ(vm.digest_hex(src, 32), abc);
  CHECK_EQ(vm.digest_hex(dst, 32), abc);
  // After final or cleanup the context has no digest until the next init.
  CHECK(vm.thrown([&] { vm.final(src, vm.rt->new_array("[B", 32), 0); }).find(runtime) == 0);
  vm.init(src, md);
  vm.cleanup(src);
  CHECK(vm.thrown([&] { vm.update(src, vm.bytes("a"), 0, 1); }).find(runtime) == 0);
  vm.update(src, vm.bytes("a"), 0, 0);  // empty update: nothing to do, as conscrypt
}

TEST(crypto_contexts_follow_their_ref_and_are_capped) {
  CryptoVm vm;
  int64_t md = vm.md("md5");
  rt::Class* probe = vm.rt->find_class("Lcom/android/org/conscrypt/Probe;");
  std::vector<int64_t> handles;
  for (int i = 0; i < 50; ++i) {
    uint32_t args[] = {lo(md), hi(md)};
    handles.push_back(int64_t(vm.rt->invoke(probe->declared_method("make", "(J)J"), args, 2)));
  }
  scrub_stack();
  vm.rt->collect_garbage();
  // The collector freed the NativeRefs, and their contexts with them (a few
  // may be retained by conservative stack scanning).
  int freed = 0;
  for (int64_t h : handles) freed += !vm.thrown([&] { vm.destroy(h); }).empty();
  CHECK(freed >= 45);

  // Contexts never handed back (no NativeRef seen) stay: bounded by a cap.
  std::vector<int64_t> leaked;
  std::string error;
  while (error.empty() && leaked.size() < 70000) {
    error = vm.thrown([&] { leaked.push_back(vm.create()); });
  }
  CHECK(error.find("java.lang.OutOfMemoryError") == 0);
  CHECK(leaked.size() <= 65536);
  for (int64_t h : leaked) vm.destroy(h);
  vm.destroy(vm.create());  // usable again
}
