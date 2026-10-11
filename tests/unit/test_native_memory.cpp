// Emulated native memory (native_memory.h) and the libcore natives built on
// it: checked tagged addresses, read-only regions, regions over Java arrays
// that die with their array, the global budget, Unsafe, libcore.io.Memory,
// DirectByteBuffer and Os.mmap/munmap.
#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kBootClasses[] = {
    R"(
.class public final Landroid/system/ErrnoException;
.super Ljava/lang/Exception;
.field public final errno:I
.method public constructor <init>(Ljava/lang/String;I)V
    .registers 3
    invoke-direct {p0, p1}, Ljava/lang/Exception;-><init>(Ljava/lang/String;)V
    iput p2, p0, Landroid/system/ErrnoException;->errno:I
    return-void
.end method
)",
    R"(
.class public final Ljava/io/FileDescriptor;
.super Ljava/lang/Object;
.field private descriptor:I
)",
    R"(
.class public Ljava/nio/ReadOnlyBufferException;
.super Ljava/lang/UnsupportedOperationException;
)",
    // The bytecode of peekInt(JZ) is replaced by an intrinsic: -1 means it ran.
    R"(
.class public final Llibcore/io/Memory;
.super Ljava/lang/Object;
.method public static peekInt(JZ)I
    .registers 4
    const/4 v0, -1
    return v0
.end method
)",
    R"(
.class public abstract Ljava/nio/Buffer;
.super Ljava/lang/Object;
.field limit:I
.field address:J
)",
    R"(
.class final Ljava/nio/DirectByteBuffer$MemoryRef;
.super Ljava/lang/Object;
.field isAccessible:Z
)",
    R"(
.class public Ljava/nio/DirectByteBuffer;
.super Ljava/nio/Buffer;
.field final memoryRef:Ljava/nio/DirectByteBuffer$MemoryRef;
.field nativeByteOrder:Z
)",
};

const char* kAppClasses[] = {
    R"(
.class public LApp;
.super Ljava/lang/Object;
.field public count:I
.field public ref:Ljava/lang/Object;
)",
};

struct Vm {
  std::unique_ptr<dex::DexFile> boot, app;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;

  Vm() {
    std::vector<tool::ClassDef> b, a;
    for (const char* s : kBootClasses) b.push_back(tool::assemble_smali(s, "boot.smali"));
    for (const char* s : kAppClasses) a.push_back(tool::assemble_smali(s, "app.smali"));
    boot = dex::DexFile::open(tool::write_dex(b), "core-test.jar!classes.dex");
    app = dex::DexFile::open(tool::write_dex(a), "app.dex");
    rt = std::make_unique<rt::Runtime>();
    rt->add_boot_dex(boot.get());
    rt->add_dex(app.get());
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
  ~Vm() {
    scope.reset();
    rt.reset();
  }
};

const std::string kIllegalArgument = "java.lang.IllegalArgumentException";
const std::string kOutOfBounds = "java.lang.IndexOutOfBoundsException";
const std::string kArrayOutOfBounds = "java.lang.ArrayIndexOutOfBoundsException";
const std::string kOutOfMemory = "java.lang.OutOfMemoryError";
const std::string kReadOnly = "java.nio.ReadOnlyBufferException";

// Java name of the exception thrown by `f`, "" when nothing is thrown.
template <typename F>
std::string thrown(rt::Runtime& rt, F&& f) {
  try {
    f();
  } catch (const rt::JavaThrow& t) {
    return rt.class_of(t.exception)->java_name();
  }
  return "";
}

// errno of the ErrnoException thrown by `f`, -1 when nothing is thrown.
template <typename F>
int32_t thrown_errno(rt::Runtime& rt, F&& f) {
  try {
    f();
  } catch (const rt::JavaThrow& t) {
    rt::Class* c = rt.class_of(t.exception);
    if (c->java_name() != "android.system.ErrnoException") return -2;
    return int32_t(rt.object(t.exception)->fields.at(c->declared_field("errno", "I")->slot));
  }
  return -1;
}

// Calls the native bound to `cls->name sig` with raw argument registers.
uint64_t call(rt::Runtime& rt, const char* cls, const char* name, const char* sig, std::vector<uint32_t> args) {
  const rt::NativeBinding* b = rt.registry().binding(cls, name, sig);
  CHECK(b != nullptr);
  rt::NativeArgs a(rt, args.data(), uint32_t(args.size()));
  return b->fn(rt, a);
}

// The exception thrown by that call, "" when nothing is thrown.
std::string failure(rt::Runtime& rt, const char* cls, const char* name, const char* sig, std::vector<uint32_t> args) {
  return thrown(rt, [&] { call(rt, cls, name, sig, std::move(args)); });
}

uint32_t lo(int64_t v) { return uint32_t(uint64_t(v)); }
uint32_t hi(int64_t v) { return uint32_t(uint64_t(v) >> 32); }

// Restores the budget of the emulated native memory at the end of a test.
struct LimitGuard {
  uint64_t saved = bridge::native_memory_stats().limit;
  ~LimitGuard() { bridge::set_native_memory_limit(saved); }
};

constexpr uint32_t kMask = 0x5A5A5A5Au;
constexpr int64_t kMask64 = 0x0A5A5A5A5A5A5A5A;

// Volatile stores, which the compiler cannot drop as dead (a memset could be).
[[gnu::noinline]] void scrub_stack() {
  volatile char buf[128 * 1024];
  for (size_t i = 0; i < sizeof buf; ++i) buf[i] = 0;
}

// An int[4] and its native address, neither in clear in the caller's frame
// (the collector scans native stacks conservatively).
struct Hidden {
  uint32_t array;
  int64_t address;
};
[[gnu::noinline]] Hidden hidden_array(rt::Runtime& rt) {
  rt::Handle h = rt.new_array("[I", 4);
  return {h ^ kMask, bridge::native_address_of(rt, h) ^ kMask64};
}

// Whether a hidden handle still designates an object.
[[gnu::noinline]] bool valid(rt::Runtime& rt, uint32_t hidden_array) {
  return rt.is_valid_handle(hidden_array ^ kMask);
}

// Whether a hidden address still designates memory.
[[gnu::noinline]] bool resolves(rt::Runtime& rt, int64_t hidden_address) {
  try {
    bridge::native_span(rt, hidden_address ^ kMask64, 4);
    return true;
  } catch (const rt::JavaThrow&) {
    return false;
  }
}

}  // namespace

TEST(native_memory_bounds_stale_and_read_only) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  int64_t a = bridge::native_alloc(rt, 16);
  CHECK(bridge::is_native_address(a));
  std::memcpy(bridge::native_span(rt, a + 4, 4, true).data(), "abcd", 4);
  CHECK_EQ(std::memcmp(bridge::native_span(rt, a, 8).data() + 4, "abcd", 4), 0);
  CHECK_EQ(bridge::native_span(rt, a + 16, 0).size(), size_t(0));
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, a + 15, 2); }), kOutOfBounds);
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, 0x1234, 1); }), kIllegalArgument);
  CHECK(!bridge::native_try_free(a + 4));  // not the base
  bridge::native_free(rt, a);
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, a, 1); }), kIllegalArgument);
  CHECK_EQ(thrown(rt, [&] { bridge::native_free(rt, a); }), kIllegalArgument);
  // A freed slot is not reused right away: its 8-bit generation would bring
  // the stale address back to life after 255 reuses.
  for (int i = 0; i < 600; ++i) {
    int64_t b = bridge::native_alloc(rt, 1);
    CHECK(b != a);
    bridge::native_free(rt, b);
  }

  int64_t ro = bridge::native_wrap(rt, {1, 2, 3}, false);
  CHECK_EQ(bridge::native_span(rt, ro, 3)[2], uint8_t(3));
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, ro, 1, true); }), kReadOnly);
  CHECK(bridge::native_try_free(ro));
}

TEST(native_memory_array_region_dies_with_its_array) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  rt::Handle objects = rt.new_array("[Ljava/lang/Object;", 2);
  CHECK_EQ(thrown(rt, [&] { bridge::native_address_of(rt, objects); }), kIllegalArgument);

  rt::GlobalRef ints(rt, rt.new_array("[I", 2));
  int64_t addr = bridge::native_address_of(rt, ints.get());
  CHECK_EQ(bridge::native_address_of(rt, ints.get()), addr);  // stable while alive
  int32_t v = 0x11223344;
  std::memcpy(bridge::native_span(rt, addr + 4, 4, true).data(), &v, 4);
  CHECK_EQ(std::memcmp(rt.array(ints.get(), "int[]").data.data() + 4, &v, 4), 0);
  CHECK(&bridge::native_array(rt, addr) == &rt.array(ints.get(), "int[]"));
  CHECK(!bridge::native_try_free(addr));  // released with the array only
  rt.collect_garbage();
  CHECK(bridge::native_span(rt, addr, 8).size() == 8);  // still reachable: still mapped

  // Collected arrays release their regions and their addresses.
  size_t before = bridge::native_memory_stats().regions;
  std::vector<Hidden> dead;
  for (int i = 0; i < 40; ++i) dead.push_back(hidden_array(rt));
  CHECK_EQ(bridge::native_memory_stats().regions, before + 40);
  scrub_stack();
  rt.collect_garbage();
  CHECK(bridge::native_memory_stats().regions <= before + 2);  // the stack scan is conservative
  int mapped = 0;
  for (const Hidden& h : dead) mapped += resolves(rt, h.address);
  CHECK(mapped <= 2);

  // ABA: the collector hands the slot of a dead array out again (last freed,
  // first reused) with an 8-bit generation, and the host allocator the same
  // memory, so that after 255 reuses an impostor array carries the dead
  // array's handle at the dead array's address. Its old native address must
  // still be rejected. Each cycle leaves a single garbage object (the new
  // array) so that it always takes that slot.
  Hidden d{};
  bool collected = false;
  for (int attempt = 0; attempt < 5 && !collected; ++attempt) {
    scrub_stack();
    rt.collect_garbage();
    d = hidden_array(rt);
    scrub_stack();
    rt.collect_garbage();
    collected = !valid(rt, d.array);
  }
  CHECK(collected);
  for (int cycle = 0; cycle < 600; ++cycle) {
    hidden_array(rt);
    if (valid(rt, d.array) && resolves(rt, d.address)) {
      throw test::Failure("adresse d'un tableau collecté réattribuée au cycle " + std::to_string(cycle));
    }
    scrub_stack();
    rt.collect_garbage();
  }
}

TEST(native_memory_budget) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  LimitGuard guard;
  uint64_t used = bridge::native_memory_stats().used;
  bridge::set_native_memory_limit(used + 1000);
  CHECK_EQ(bridge::native_memory_stats().limit, used + 1000);
  CHECK_EQ(thrown(rt, [&] { bridge::native_alloc(rt, 2000); }), kOutOfMemory);
  int64_t a = bridge::native_alloc(rt, 600);
  CHECK_EQ(bridge::native_memory_stats().used, used + 600);
  CHECK(!bridge::native_memory_fits(500));
  CHECK_EQ(thrown(rt, [&] { bridge::native_alloc(rt, 500); }), kOutOfMemory);
  CHECK_EQ(thrown(rt, [&] { bridge::native_wrap(rt, std::vector<uint8_t>(500), true); }), kOutOfMemory);
  // realloc keeps the old block when the new one does not fit.
  bridge::native_span(rt, a, 1, true)[0] = 42;
  CHECK_EQ(thrown(rt, [&] { bridge::native_realloc(rt, a, 700); }), kOutOfMemory);
  CHECK_EQ(bridge::native_span(rt, a, 1)[0], uint8_t(42));
  int64_t b = bridge::native_realloc(rt, a, 300);
  CHECK_EQ(bridge::native_span(rt, b, 1)[0], uint8_t(42));
  CHECK_EQ(bridge::native_memory_stats().used, used + 300);
  // Os.mmap reports ENOMEM, as mmap(2).
  rt::Handle linux = rt.alloc(rt.find_class("Ljava/lang/Object;"));
  const char* lx = "Llibcore/io/Linux;";
  const char* mmap = "(JJIILjava/io/FileDescriptor;J)J";
  CHECK_EQ(thrown_errno(rt, [&] { call(rt, lx, "mmap", mmap, {linux, 0, 0, 4096, 0, 3, 0x22, 0, 0, 0}); }), 12);
  bridge::native_free(rt, b);
  CHECK_EQ(bridge::native_memory_stats().used, used);
  // Arrays belong to the Java heap: their regions are not charged.
  rt::GlobalRef arr(rt, rt.new_array("[B", 4096));
  bridge::native_address_of(rt, arr.get());
  CHECK_EQ(bridge::native_memory_stats().used, used);
}

TEST(native_memory_unsafe_never_forges_references) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* u = "Lsun/misc/Unsafe;";
  const char* get_object = "(Ljava/lang/Object;J)Ljava/lang/Object;";
  const char* put_object = "(Ljava/lang/Object;JLjava/lang/Object;)V";
  const char* get_int = "(Ljava/lang/Object;J)I";
  const char* put_int = "(Ljava/lang/Object;JI)V";
  const char* copy = "(Ljava/lang/Object;JLjava/lang/Object;JJ)V";
  rt::Handle unsafe = rt.alloc(rt.find_class("Ljava/lang/Object;"));
  int64_t addr = bridge::native_alloc(rt, 16);
  rt::Handle s = rt.new_string_utf8("x");
  std::memcpy(bridge::native_span(rt, addr, 4, true).data(), &s, 4);
  // No reference through native memory (null object + address).
  CHECK_EQ(failure(rt, u, "getObject", get_object, {unsafe, 0, lo(addr), hi(addr)}), kIllegalArgument);
  CHECK_EQ(failure(rt, u, "putReferenceVolatile", put_object, {unsafe, 0, lo(addr), hi(addr), s}), kIllegalArgument);
  CHECK_EQ(failure(rt, u, "compareAndSwapObject", "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z",
                   {unsafe, 0, lo(addr), hi(addr), s, s}),
           kIllegalArgument);
  // Nor from a primitive array, nor bytes into a reference array or field.
  rt::Handle ints = rt.new_array("[I", 2);
  std::memcpy(rt.array(ints, "int[]").data.data(), &s, 4);
  CHECK_EQ(failure(rt, u, "getObject", get_object, {unsafe, ints, 16, 0}), kIllegalArgument);
  rt::Handle objects = rt.new_array("[Ljava/lang/Object;", 2);
  CHECK_EQ(failure(rt, u, "putInt", put_int, {unsafe, objects, 16, 0, s}), kIllegalArgument);
  CHECK_EQ(failure(rt, u, "copyMemory", copy, {unsafe, ints, 16, 0, objects, 16, 0, 4, 0}), kIllegalArgument);
  rt::Class* app = rt.find_class("LApp;");
  rt::Handle obj = rt.alloc(app);
  int64_t ref_off = bridge::unsafe_field_offset(app->declared_field("ref", "Ljava/lang/Object;")->slot);
  int64_t count_off = bridge::unsafe_field_offset(app->declared_field("count", "I")->slot);
  CHECK_EQ(failure(rt, u, "putInt", put_int, {unsafe, obj, lo(ref_off), hi(ref_off), s}), kIllegalArgument);
  CHECK_EQ(failure(rt, u, "getObject", get_object, {unsafe, obj, lo(count_off), hi(count_off)}), kIllegalArgument);
  // Typed accesses still work, and reading a reference as bytes is harmless.
  call(rt, u, "putObject", put_object, {unsafe, objects, 20, 0, s});
  CHECK_EQ(call(rt, u, "getObject", get_object, {unsafe, objects, 20, 0}), uint64_t(s));
  CHECK_EQ(call(rt, u, "getInt", get_int, {unsafe, objects, 20, 0}), uint64_t(s));
  call(rt, u, "putInt", put_int, {unsafe, obj, lo(count_off), hi(count_off), 7});
  CHECK_EQ(call(rt, u, "getInt", get_int, {unsafe, obj, lo(count_off), hi(count_off)}), uint64_t(7));
  // Offsets far outside the object: an exception, no arithmetic overflow.
  CHECK_EQ(failure(rt, u, "getInt", get_int, {unsafe, ints, 0, 0x80000000u}), kArrayOutOfBounds);
  CHECK_EQ(failure(rt, u, "getInt", get_int, {unsafe, obj, 0, 0x80000000u}), kIllegalArgument);
  bridge::native_free(rt, addr);
}

TEST(native_memory_unsafe_reads_read_only_mappings) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* u = "Lsun/misc/Unsafe;";
  const char* copy = "(Ljava/lang/Object;JLjava/lang/Object;JJ)V";
  rt::Handle unsafe = rt.alloc(rt.find_class("Ljava/lang/Object;"));
  int64_t ro = bridge::native_wrap(rt, {1, 2, 3, 4, 5, 6, 7, 8}, false);
  CHECK_EQ(call(rt, u, "getByte", "(J)B", {unsafe, lo(ro + 1), hi(ro + 1)}), uint64_t(2));
  CHECK_EQ(call(rt, u, "getInt", "(Ljava/lang/Object;J)I", {unsafe, 0, lo(ro), hi(ro)}), uint64_t(0x04030201));
  rt::Handle bytes = rt.new_array("[B", 8);
  call(rt, u, "copyMemory", copy, {unsafe, 0, lo(ro), hi(ro), bytes, 16, 0, 8, 0});
  CHECK_EQ(rt.array(bytes, "byte[]").data[7], uint8_t(8));
  CHECK_EQ(failure(rt, u, "putByte", "(JB)V", {unsafe, lo(ro), hi(ro), 9}), kReadOnly);
  CHECK_EQ(failure(rt, u, "copyMemory", copy, {unsafe, bytes, 16, 0, 0, lo(ro), hi(ro), 8, 0}), kReadOnly);
  bridge::native_free(rt, ro);
}

TEST(native_memory_libcore_memory_natives) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* mem = "Llibcore/io/Memory;";
  int64_t addr = bridge::native_alloc(rt, 32);
  auto native = bridge::native_span(rt, addr, 32, true);
  for (int i = 0; i < 32; ++i) native[size_t(i)] = uint8_t(i);

  // peekInt(JZ) & co. are intrinsics: the bytecode (-1) never runs.
  rt::Method* peek_int = rt.find_class(mem)->declared_method("peekInt", "(JZ)I");
  uint32_t plain[] = {lo(addr + 4), hi(addr + 4), 0};
  CHECK_EQ(rt.invoke(peek_int, plain, 3), uint64_t(0x07060504));
  uint32_t swapped[] = {lo(addr + 4), hi(addr + 4), 1};
  CHECK_EQ(rt.invoke(peek_int, swapped, 3), uint64_t(0x04050607));
  CHECK(rt.registry().binding(mem, "pokeShort", "(JSZ)V")->replaces_bytecode);
  CHECK(rt.registry().binding(mem, "pokeInt", "(JIZ)V")->replaces_bytecode);
  CHECK(rt.registry().binding(mem, "pokeLong", "(JJZ)V")->replaces_bytecode);
  call(rt, mem, "pokeLong", "(JJZ)V", {lo(addr), hi(addr), 0x01020304, 0x05060708, 1});
  CHECK_EQ(native[0], uint8_t(0x05));
  CHECK_EQ(native[7], uint8_t(0x04));

  // peek/poke<Type>Array: offset and count in elements, optional swap.
  rt::Handle shorts = rt.new_array("[S", 4);
  call(rt, mem, "peekShortArray", "(J[SIIZ)V", {lo(addr + 8), hi(addr + 8), shorts, 1, 2, 1});
  auto& sa = rt.array(shorts, "short[]").data;
  CHECK_EQ(sa[2], uint8_t(9));
  CHECK_EQ(sa[3], uint8_t(8));
  CHECK_EQ(sa[4], uint8_t(11));
  CHECK_EQ(sa[0], uint8_t(0));
  rt::Handle longs = rt.new_array("[J", 2);
  rt.array(longs, "long[]").data[8] = 0xAB;
  call(rt, mem, "pokeLongArray", "(J[JIIZ)V", {lo(addr + 16), hi(addr + 16), longs, 1, 1, 0});
  CHECK_EQ(native[16], uint8_t(0xAB));
  CHECK_EQ(failure(rt, mem, "peekShortArray", "(J[SIIZ)V", {lo(addr), hi(addr), shorts, 3, 2, 0}), kArrayOutOfBounds);
  CHECK_EQ(failure(rt, mem, "pokeLongArray", "(J[JIIZ)V", {lo(addr), hi(addr), longs, 0, uint32_t(INT32_MAX), 0}),
           kArrayOutOfBounds);
  CHECK_EQ(failure(rt, mem, "peekIntArray", "(J[IIIZ)V", {lo(addr), hi(addr), shorts, 0, 1, 0}),
           kIllegalArgument);  // a short[] is not an int[]
  CHECK_EQ(failure(rt, mem, "peekShortArray", "(J[SIIZ)V", {lo(addr + 30), hi(addr + 30), shorts, 0, 2, 0}),
           kOutOfBounds);

  // memmove and the bulk copies only take primitive arrays (bytes copied into
  // an Object[] would become handles to arbitrary objects).
  const char* memmove = "(Ljava/lang/Object;ILjava/lang/Object;IJ)V";
  const char* bulk_get = "(Ljava/lang/Object;II[BIIZ)V";
  rt::Handle bytes = rt.new_array("[B", 8);
  rt::Handle s = rt.new_string_utf8("x");
  std::memcpy(rt.array(bytes, "byte[]").data.data(), &s, 4);
  rt::Handle objects = rt.new_array("[Ljava/lang/Object;", 2);
  CHECK_EQ(failure(rt, mem, "memmove", memmove, {objects, 0, bytes, 0, 4, 0}), kIllegalArgument);
  CHECK_EQ(failure(rt, mem, "unsafeBulkGet", bulk_get, {objects, 0, 4, bytes, 0, 4, 0}), kIllegalArgument);
  rt::Handle ints = rt.new_array("[I", 2);
  call(rt, mem, "memmove", memmove, {ints, 4, bytes, 0, 4, 0});
  CHECK_EQ(std::memcmp(rt.array(ints, "int[]").data.data() + 4, &s, 4), 0);
  rt::Handle chars = rt.new_array("[C", 2);
  std::memcpy(rt.array(bytes, "byte[]").data.data(), "\x01\x02\x03\x04", 4);
  call(rt, mem, "unsafeBulkGet", bulk_get, {chars, 1, 2, bytes, 2, 2, 1});
  CHECK_EQ(rt.array(chars, "char[]").data[2], uint8_t(4));
  CHECK_EQ(rt.array(chars, "char[]").data[3], uint8_t(3));
  call(rt, mem, "unsafeBulkPut", "([BIILjava/lang/Object;IIZ)V", {bytes, 4, 2, chars, 1, 2, 0});
  CHECK_EQ(rt.array(bytes, "byte[]").data[4], uint8_t(4));
  CHECK_EQ(failure(rt, mem, "unsafeBulkGet", bulk_get, {chars, 1, 4, bytes, 0, 2, 0}), kArrayOutOfBounds);

  // memmove to a direct buffer: its `address` field, checked as any address.
  rt::Class* dbb = rt.find_class("Ljava/nio/DirectByteBuffer;");
  rt::Handle buffer = rt.alloc(dbb);
  uint32_t slot = bridge::find_field(dbb, "address", "J")->slot;
  rt.object(buffer)->fields.at(slot) = lo(addr);
  rt.object(buffer)->fields.at(slot + 1) = hi(addr);
  call(rt, mem, "memmove", memmove, {buffer, 24, bytes, 0, 4, 0});
  CHECK_EQ(native[25], uint8_t(2));
  CHECK_EQ(failure(rt, mem, "memmove", memmove, {buffer, 30, bytes, 0, 4, 0}), kOutOfBounds);
  bridge::native_free(rt, addr);
}

TEST(native_memory_direct_buffer_address_arithmetic) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  rt::Class* dbb = rt.find_class("Ljava/nio/DirectByteBuffer;");
  rt::Class* ref_class = rt.find_class("Ljava/nio/DirectByteBuffer$MemoryRef;");
  rt::Handle buffer = rt.alloc(dbb);
  rt::Handle ref = rt.alloc(ref_class);
  rt.object(ref)->fields.at(ref_class->declared_field("isAccessible", "Z")->slot) = 1;
  rt::Object& b = *rt.object(buffer);
  b.fields.at(bridge::find_field(dbb, "memoryRef", "Ljava/nio/DirectByteBuffer$MemoryRef;")->slot) = ref;
  b.fields.at(bridge::find_field(dbb, "limit", "I")->slot) = 16;
  b.fields.at(bridge::find_field(dbb, "nativeByteOrder", "Z")->slot) = 1;
  uint32_t slot = bridge::find_field(dbb, "address", "J")->slot;
  auto set_address = [&](int64_t address) {
    b.fields.at(slot) = lo(address);
    b.fields.at(slot + 1) = hi(address);
  };
  int64_t addr = bridge::native_alloc(rt, 16);
  bridge::native_span(rt, addr, 16, true)[9] = 0x5C;
  set_address(addr);
  const char* d = "Ljava/nio/DirectByteBuffer;";
  CHECK_EQ(call(rt, d, "get", "(I)B", {buffer, 9}), uint64_t(0x5C));
  // An address forged near INT64_MAX: the sum wraps (unsigned) and is rejected.
  set_address(INT64_MAX - 2);
  CHECK_EQ(failure(rt, d, "getInt", "(I)I", {buffer, 8}), kIllegalArgument);
  bridge::native_free(rt, addr);
}

TEST(native_memory_mmap_anonymous_and_munmap_errno) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* lx = "Llibcore/io/Linux;";
  const char* mmap = "(JJIILjava/io/FileDescriptor;J)J";
  rt::Handle linux = rt.alloc(rt.find_class("Ljava/lang/Object;"));
  constexpr uint32_t kPrivateAnonymous = 0x22, kRead = 1, kReadWrite = 3;
  int64_t rw = int64_t(call(rt, lx, "mmap", mmap, {linux, 0, 0, 64, 0, kReadWrite, kPrivateAnonymous, 0, 0, 0}));
  CHECK_EQ(bridge::native_span(rt, rw, 64)[63], uint8_t(0));
  bridge::native_span(rt, rw, 1, true)[0] = 1;
  int64_t ro = int64_t(call(rt, lx, "mmap", mmap, {linux, 0, 0, 64, 0, kRead, kPrivateAnonymous, 0, 0, 0}));
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, ro, 1, true); }), kReadOnly);
  // MAP_FIXED would impose an address the emulation cannot honour.
  CHECK_EQ(thrown_errno(rt, [&] { call(rt, lx, "mmap", mmap, {linux, 0, 0, 64, 0, kRead, 0x32, 0, 0, 0}); }), 22);

  // munmap: EINVAL (ErrnoException) for anything but the start of a mapping.
  auto munmap = [&](int64_t address, int64_t length) {
    call(rt, lx, "munmap", "(JJ)V", {linux, lo(address), hi(address), lo(length), hi(length)});
  };
  CHECK_EQ(thrown_errno(rt, [&] { munmap(rw + 8, 56); }), 22);
  CHECK_EQ(thrown_errno(rt, [&] { munmap(rw, 0); }), 22);
  CHECK_EQ(thrown_errno(rt, [&] { munmap(0x1234, 64); }), 22);
  CHECK_EQ(bridge::native_span(rt, rw, 1)[0], uint8_t(1));  // still mapped
  munmap(rw, 64);
  CHECK_EQ(thrown_errno(rt, [&] { munmap(rw, 64); }), 22);
  rt::GlobalRef arr(rt, rt.new_array("[B", 8));
  CHECK_EQ(thrown_errno(rt, [&] { munmap(bridge::native_address_of(rt, arr.get()), 8); }), 22);

  // FileChannelImpl.unmap0: IOException, as the real native.
  const char* fci = "Lsun/nio/ch/FileChannelImpl;";
  CHECK_EQ(failure(rt, fci, "unmap0", "(JJ)I", {lo(ro + 4), hi(ro + 4), 60, 0}), std::string("java.io.IOException"));
  call(rt, fci, "unmap0", "(JJ)I", {lo(ro), hi(ro), 64, 0});
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, ro, 1); }), kIllegalArgument);
}

TEST(native_memory_read_only_file_mapping_is_lazy) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  auto& cfg = bridge::libcore_config();
  auto saved = cfg.vfs;
  auto content = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
  cfg.vfs = std::make_shared<vfs::Vfs>();
  cfg.vfs->mount("/m", vfs::memory_mount({{"f", content}}));
  int32_t fd = cfg.vfs->open("/m/f", vfs::kO_RDONLY, 0);
  uint64_t before = bridge::native_memory_stats().used;
  // Mapped past the end of the file (zeros there), then the descriptor is
  // closed before the first access, as Java code commonly does.
  int64_t addr = bridge::map_fd_copy(rt, fd, 4, 10, false);
  cfg.vfs->close(fd);
  CHECK_EQ(bridge::native_memory_stats().used, before);  // nothing read yet
  auto bytes = bridge::native_span(rt, addr, 10);
  const uint8_t expected[10] = {5, 6, 7, 8, 0, 0, 0, 0, 0, 0};
  CHECK(std::memcmp(bytes.data(), expected, 10) == 0);
  CHECK(bridge::native_memory_stats().used >= before + 10);
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, addr, 1, true); }), kReadOnly);
  CHECK_EQ(thrown(rt, [&] { bridge::native_span(rt, addr + 8, 4); }), kOutOfBounds);
  bridge::native_free(rt, addr);
  CHECK_EQ(bridge::native_memory_stats().used, before);

  // Never touched: freed without being read.
  fd = cfg.vfs->open("/m/f", vfs::kO_RDONLY, 0);
  int64_t untouched = bridge::map_fd_copy(rt, fd, 0, 8, false);
  cfg.vfs->close(fd);
  bridge::native_free(rt, untouched);
  CHECK_EQ(bridge::native_memory_stats().used, before);
  cfg.vfs = saved;
}
