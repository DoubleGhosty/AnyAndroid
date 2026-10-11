// Emulated native memory (native_memory.h): checked tagged addresses,
// read-only regions, regions over Java arrays that die with their array and
// the global budget.
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
.class public Ljava/nio/ReadOnlyBufferException;
.super Ljava/lang/UnsupportedOperationException;
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
  bridge::native_free(rt, b);
  CHECK_EQ(bridge::native_memory_stats().used, used);
  // Arrays belong to the Java heap: their regions are not charged.
  rt::GlobalRef arr(rt, rt.new_array("[B", 4096));
  bridge::native_address_of(rt, arr.get());
  CHECK_EQ(bridge::native_memory_stats().used, used);
}
