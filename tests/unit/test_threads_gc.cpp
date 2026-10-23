// G1: Java threads, monitors and the garbage collector.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/clock.h"
#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kShared = R"SMALI(
.class public LShared;
.super Ljava/lang/Object;
.field public static count:I
.field public static result:I
.field public static LOCK:Ljava/lang/Object;
.field public static held:[I
.field public static ref:Ljava/lang/ref/WeakReference;
.field public static strong:Ljava/lang/Object;
.method static constructor <clinit>()V
    .registers 1
    new-instance v0, Ljava/lang/Object;
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    sput-object v0, LShared;->LOCK:Ljava/lang/Object;
    return-void
.end method
)SMALI";

const char* kIncrementer = R"SMALI(
.class public LIncrementer;
.super Ljava/lang/Object;
.implements Ljava/lang/Runnable;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public run()V
    .registers 5
    const/4 v0, 0x0
    const/16 v1, 0x2710
    :loop
    if-ge v0, v1, :done
    sget-object v2, LShared;->LOCK:Ljava/lang/Object;
    monitor-enter v2
    sget v3, LShared;->count:I
    add-int/lit8 v3, v3, 0x1
    sput v3, LShared;->count:I
    monitor-exit v2
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    return-void
.end method
)SMALI";

const char* kBox = R"SMALI(
.class public LBox;
.super Ljava/lang/Object;
.field private value:I
.field private full:Z
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public put(I)V
    .registers 3
    monitor-enter p0
    :wait_loop
    iget-boolean v0, p0, LBox;->full:Z
    if-eqz v0, :store
    invoke-virtual {p0}, Ljava/lang/Object;->wait()V
    goto :wait_loop
    :store
    iput p1, p0, LBox;->value:I
    const/4 v0, 0x1
    iput-boolean v0, p0, LBox;->full:Z
    invoke-virtual {p0}, Ljava/lang/Object;->notifyAll()V
    monitor-exit p0
    return-void
.end method
.method public take()I
    .registers 3
    monitor-enter p0
    :wait_loop
    iget-boolean v0, p0, LBox;->full:Z
    if-nez v0, :load
    invoke-virtual {p0}, Ljava/lang/Object;->wait()V
    goto :wait_loop
    :load
    iget v1, p0, LBox;->value:I
    const/4 v0, 0x0
    iput-boolean v0, p0, LBox;->full:Z
    invoke-virtual {p0}, Ljava/lang/Object;->notifyAll()V
    monitor-exit p0
    return v1
.end method
)SMALI";

const char* kProducer = R"SMALI(
.class public LProducer;
.super Ljava/lang/Object;
.implements Ljava/lang/Runnable;
.field private box:LBox;
.method public constructor <init>(LBox;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, LProducer;->box:LBox;
    return-void
.end method
.method public run()V
    .registers 4
    const/4 v0, 0x1
    const/16 v1, 0x64
    :loop
    if-gt v0, v1, :done
    iget-object v2, p0, LProducer;->box:LBox;
    invoke-virtual {v2, v0}, LBox;->put(I)V
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    return-void
.end method
)SMALI";

const char* kSleeper = R"SMALI(
.class public LSleeper;
.super Ljava/lang/Object;
.implements Ljava/lang/Runnable;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public run()V
    .registers 3
    :try_start
    const-wide/16 v0, 0x2710
    invoke-static {v0, v1}, Ljava/lang/Thread;->sleep(J)V
    const/4 v0, 0x0
    sput v0, LShared;->result:I
    return-void
    :try_end
    .catch Ljava/lang/InterruptedException; {:try_start .. :try_end} :caught
    :caught
    const/16 v0, 0x2a
    sput v0, LShared;->result:I
    return-void
.end method
)SMALI";

const char* kThrower = R"SMALI(
.class public LThrower;
.super Ljava/lang/Object;
.implements Ljava/lang/Runnable;
.implements Ljava/lang/Thread$UncaughtExceptionHandler;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public run()V
    .registers 3
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "échec dans le thread"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
.end method
.method public uncaughtException(Ljava/lang/Thread;Ljava/lang/Throwable;)V
    .registers 4
    const/4 v0, 0x7
    sput v0, LShared;->result:I
    return-void
.end method
)SMALI";

const char* kSpinner = R"SMALI(
.class public LSpinner;
.super Ljava/lang/Object;
.implements Ljava/lang/Runnable;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public run()V
    .registers 1
    :l
    goto :l
.end method
)SMALI";

const char* kMain = R"SMALI(
.class public LMain;
.super Ljava/lang/Object;

.method public static counter()I
    .registers 4
    new-instance v1, LIncrementer;
    invoke-direct {v1}, LIncrementer;-><init>()V
    new-instance v0, Ljava/lang/Thread;
    invoke-direct {v0, v1}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;)V
    new-instance v2, Ljava/lang/Thread;
    invoke-direct {v2, v1}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;)V
    invoke-virtual {v0}, Ljava/lang/Thread;->start()V
    invoke-virtual {v2}, Ljava/lang/Thread;->start()V
    invoke-virtual {v0}, Ljava/lang/Thread;->join()V
    invoke-virtual {v2}, Ljava/lang/Thread;->join()V
    sget v3, LShared;->count:I
    return v3
.end method

.method public static producerConsumer()I
    .registers 6
    new-instance v0, LBox;
    invoke-direct {v0}, LBox;-><init>()V
    new-instance v1, LProducer;
    invoke-direct {v1, v0}, LProducer;-><init>(LBox;)V
    new-instance v2, Ljava/lang/Thread;
    invoke-direct {v2, v1}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;)V
    invoke-virtual {v2}, Ljava/lang/Thread;->start()V
    const/4 v3, 0x0
    const/4 v4, 0x0
    :loop
    const/16 v5, 0x64
    if-ge v4, v5, :done
    invoke-virtual {v0}, LBox;->take()I
    move-result v5
    add-int/2addr v3, v5
    add-int/lit8 v4, v4, 0x1
    goto :loop
    :done
    invoke-virtual {v2}, Ljava/lang/Thread;->join()V
    return v3
.end method

.method public static interruptSleeper()I
    .registers 4
    new-instance v1, LSleeper;
    invoke-direct {v1}, LSleeper;-><init>()V
    new-instance v0, Ljava/lang/Thread;
    invoke-direct {v0, v1}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;)V
    invoke-virtual {v0}, Ljava/lang/Thread;->start()V
    const-wide/16 v2, 0x14
    invoke-static {v2, v3}, Ljava/lang/Thread;->sleep(J)V
    invoke-virtual {v0}, Ljava/lang/Thread;->interrupt()V
    invoke-virtual {v0}, Ljava/lang/Thread;->join()V
    sget v2, LShared;->result:I
    return v2
.end method

.method public static uncaught()I
    .registers 3
    new-instance v1, LThrower;
    invoke-direct {v1}, LThrower;-><init>()V
    new-instance v0, Ljava/lang/Thread;
    invoke-direct {v0, v1}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;)V
    invoke-virtual {v0, v1}, Ljava/lang/Thread;->setUncaughtExceptionHandler(Ljava/lang/Thread$UncaughtExceptionHandler;)V
    invoke-virtual {v0}, Ljava/lang/Thread;->start()V
    invoke-virtual {v0}, Ljava/lang/Thread;->join()V
    sget v2, LShared;->result:I
    return v2
.end method

.method public static spinForever()V
    .registers 2
    new-instance v1, LSpinner;
    invoke-direct {v1}, LSpinner;-><init>()V
    new-instance v0, Ljava/lang/Thread;
    invoke-direct {v0, v1}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;)V
    const/4 v1, 0x1
    invoke-virtual {v0, v1}, Ljava/lang/Thread;->setDaemon(Z)V
    invoke-virtual {v0}, Ljava/lang/Thread;->start()V
    return-void
.end method

.method public static currentName()Ljava/lang/String;
    .registers 1
    invoke-static {}, Ljava/lang/Thread;->currentThread()Ljava/lang/Thread;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Thread;->getName()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static illegalWait()V
    .registers 1
    new-instance v0, Ljava/lang/Object;
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    invoke-virtual {v0}, Ljava/lang/Object;->wait()V
    return-void
.end method

.method public static churn(I)I
    .registers 4
    const/4 v0, 0x0
    :loop
    if-ge v0, p0, :done
    new-instance v1, Ljava/lang/Object;
    invoke-direct {v1}, Ljava/lang/Object;-><init>()V
    const/16 v2, 0x10
    new-array v2, v2, [I
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    return v0
.end method

.method public static keep()V
    .registers 3
    const/16 v0, 0x3e8
    new-array v0, v0, [I
    const/16 v1, 0x3e7
    const/16 v2, 0x4d2
    aput v2, v0, v1
    sput-object v0, LShared;->held:[I
    return-void
.end method

.method public static check()I
    .registers 3
    sget-object v0, LShared;->held:[I
    const/16 v1, 0x3e7
    aget v2, v0, v1
    return v2
.end method

.method public static makeWeak(Z)V
    .registers 4
    new-instance v0, Ljava/lang/Object;
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    new-instance v1, Ljava/lang/ref/WeakReference;
    invoke-direct {v1, v0}, Ljava/lang/ref/WeakReference;-><init>(Ljava/lang/Object;)V
    sput-object v1, LShared;->ref:Ljava/lang/ref/WeakReference;
    if-eqz p0, :drop
    sput-object v0, LShared;->strong:Ljava/lang/Object;
    :drop
    const/4 v0, 0x0
    const/4 v1, 0x0
    return-void
.end method

.method public static weakAlive()Z
    .registers 2
    sget-object v0, LShared;->ref:Ljava/lang/ref/WeakReference;
    invoke-virtual {v0}, Ljava/lang/ref/WeakReference;->get()Ljava/lang/Object;
    move-result-object v1
    if-eqz v1, :dead
    const/4 v0, 0x1
    return v0
    :dead
    const/4 v0, 0x0
    return v0
.end method
)SMALI";

struct ThreadVm {
  std::unique_ptr<dex::DexFile> dex;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;

  explicit ThreadVm(rt::RuntimeOptions opts = {}) {
    std::vector<tool::ClassDef> classes;
    for (const char* s : {kShared, kIncrementer, kBox, kProducer, kSleeper, kThrower, kSpinner, kMain}) {
      classes.push_back(tool::assemble_smali(s, "threads.smali"));
    }
    dex = dex::DexFile::open(tool::write_dex(classes), "threads.dex");
    rt = std::make_unique<rt::Runtime>(opts);
    rt->add_dex(dex.get());
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
  ~ThreadVm() {
    scope.reset();
    rt.reset();  // joins (or kills) every started thread
  }
  uint64_t call(const char* name, const char* sig, std::vector<uint32_t> args = {}) {
    return rt->call_static("LMain;", name, sig, std::move(args));
  }
  std::string thrown(const char* name, const char* sig) {
    try {
      call(name, sig);
    } catch (const rt::JavaThrow& t) {
      return rt->class_of(t.exception)->java_name();
    }
    return "";
  }
};

// Overwrites dead stack memory so that stale handles left by finished calls
// are not picked up by the conservative stack scan.
// Zeroes the stack below the caller, so that the conservative scan does not
// find handles left there by earlier calls. The asm statement makes the buffer
// observable: without it, GCC removes the stores and then the whole function.
[[gnu::noinline]] void scrub_stack() {
  char buf[128 * 1024];
  std::memset(buf, 0, sizeof buf);
  asm volatile("" : : "r"(buf) : "memory");
}

// Allocates a string whose handle never appears in clear in the caller's frame.
[[gnu::noinline]] uint32_t make_hidden_string(rt::Runtime& rt) { return rt.new_string_utf8("éphémère") ^ 0x5A5A5A5Au; }

}  // namespace

TEST(threads_synchronized_counter) {
  ThreadVm vm;
  CHECK_EQ(int32_t(vm.call("counter", "()I")), 20000);
  CHECK_EQ(vm.rt->stats().threads_started, uint64_t(2));
}

TEST(threads_wait_notify_producer_consumer) {
  ThreadVm vm;
  CHECK_EQ(int32_t(vm.call("producerConsumer", "()I")), 5050);
}

TEST(threads_interrupt_sleep) {
  ThreadVm vm;
  CHECK_EQ(int32_t(vm.call("interruptSleeper", "()I")), 42);
}

TEST(threads_uncaught_exception_handler) {
  ThreadVm vm;
  CHECK_EQ(int32_t(vm.call("uncaught", "()I")), 7);
}

TEST(threads_current_thread_and_monitor_errors) {
  ThreadVm vm;
  CHECK_EQ(vm.rt->string_utf8(rt::Handle(vm.call("currentName", "()Ljava/lang/String;"))), std::string("test"));
  CHECK_EQ(vm.thrown("illegalWait", "()V"), std::string("java.lang.IllegalMonitorStateException"));
}

TEST(threads_shutdown_kills_running_threads) {
  ThreadVm vm;
  vm.call("spinForever", "()V");
  // ~ThreadVm must stop the spinning daemon thread instead of hanging.
}

// A 1 ms timed wait must last about 1 ms. With the default Windows tick
// (~15.6 ms) each one would last a whole tick: Looper delays and animations
// would run many times too slowly, with nothing in the logs (the CI's Windows
// Server measured 14 ms per wait despite timeBeginPeriod). Judged on the
// second fastest of 15 waits: a loaded machine delays some waits (a median of
// 9 ms seen under wine with 8 parallel tests), the tick delays all of them.
TEST(threads_timed_waits_are_precise) {
  ThreadVm vm;
  auto fast_ms = [](const auto& wait) {
    std::vector<int64_t> d;
    for (int i = 0; i < 15; ++i) {
      int64_t t0 = clock::monotonic_nanos();
      wait();
      d.push_back(clock::monotonic_nanos() - t0);
    }
    std::sort(d.begin(), d.end());
    return double(d[1]) / 1e6;
  };
  double sleep_ms = fast_ms([&] { vm.rt->sleep(1); });
  double park_ms = fast_ms([&] { vm.rt->park_current(false, 1000000); });
  if (sleep_ms >= 10 || park_ms >= 10) {
    std::fprintf(stderr, "  attente de 1 ms : sleep %.2f ms, park %.2f ms\n", sleep_ms, park_ms);
  }
  CHECK(sleep_ms >= 1.0);  // never shorter than asked
  CHECK(sleep_ms < 10.0);
  CHECK(park_ms < 10.0);
}

TEST(gc_reclaims_garbage_under_object_limit) {
  rt::RuntimeOptions opts;
  opts.gc_min_objects = 10000;
  opts.max_objects = 50000;
  ThreadVm vm(opts);
  vm.call("keep", "()V");
  CHECK_EQ(int32_t(vm.call("churn", "(I)I", {200000})), 200000);
  CHECK(vm.rt->gc_stats().collections > 0);
  CHECK(vm.rt->object_count() < 50000);
  CHECK_EQ(int32_t(vm.call("check", "()I")), 1234);  // statics survive
}

TEST(gc_weak_references) {
  ThreadVm vm;
  vm.call("makeWeak", "(Z)V", {0});
  scrub_stack();
  vm.rt->collect_garbage();
  CHECK_EQ(vm.call("weakAlive", "()Z"), uint64_t(0));  // only weakly reachable: cleared
  vm.call("makeWeak", "(Z)V", {1});
  scrub_stack();
  vm.rt->collect_garbage();
  CHECK_EQ(vm.call("weakAlive", "()Z"), uint64_t(1));  // also held by a static: kept
}

TEST(gc_detects_stale_handles) {
  ThreadVm vm;
  std::vector<uint32_t> hidden;
  for (int i = 0; i < 100; ++i) hidden.push_back(make_hidden_string(*vm.rt));
  scrub_stack();
  vm.rt->collect_garbage();
  // The collector scans native stacks conservatively, so a few unrelated
  // words may retain an object; the vast majority must be freed and their
  // handles detected as stale.
  int stale = 0;
  for (uint32_t h : hidden) stale += !vm.rt->is_valid_handle(h ^ 0x5A5A5A5Au);
  CHECK(stale >= 95);
  // Reused slots get a new generation: old handles stay invalid.
  for (int i = 0; i < 200; ++i) vm.rt->new_string_utf8("x");
  int still_stale = 0;
  for (uint32_t h : hidden) still_stale += !vm.rt->is_valid_handle(h ^ 0x5A5A5A5Au);
  CHECK(still_stale >= stale);
}

namespace {

// Libcore's side of reference processing, reduced to what the collector calls:
// ReferenceQueue.add receives the circular list (linked by pendingNext) of the
// references a collection cleared. The real one appends it to unenqueued for
// the ReferenceQueueDaemon.
const char* kRefBoot = R"SMALI(
.class public Ljava/lang/ref/ReferenceQueue;
.super Ljava/lang/Object;
.field public static unenqueued:Ljava/lang/ref/Reference;
.field public static adds:I
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method static add(Ljava/lang/ref/Reference;)V
    .registers 2
    sput-object p0, Ljava/lang/ref/ReferenceQueue;->unenqueued:Ljava/lang/ref/Reference;
    sget v0, Ljava/lang/ref/ReferenceQueue;->adds:I
    add-int/lit8 v0, v0, 0x1
    sput v0, Ljava/lang/ref/ReferenceQueue;->adds:I
    return-void
.end method
)SMALI";

// makeRefs(queue, refs, kept): refs[i] = new WeakReference(new Object(), queue)
// for i < refs.length - 3, then a reference without queue, one whose referent
// stays reachable (kept[0]) and one already enqueued (queueNext set): these
// three must not be delivered.
const char* kRefApp = R"SMALI(
.class public LRefs;
.super Ljava/lang/Object;
.method public static makeRefs(Ljava/lang/ref/ReferenceQueue;[Ljava/lang/ref/WeakReference;[Ljava/lang/Object;)V
    .registers 8
    array-length v0, p1
    add-int/lit8 v0, v0, -0x3
    const/4 v1, 0x0
    :loop
    if-ge v1, v0, :tail
    new-instance v2, Ljava/lang/Object;
    invoke-direct {v2}, Ljava/lang/Object;-><init>()V
    new-instance v3, Ljava/lang/ref/WeakReference;
    invoke-direct {v3, v2, p0}, Ljava/lang/ref/WeakReference;-><init>(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V
    aput-object v3, p1, v1
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :tail
    new-instance v2, Ljava/lang/Object;
    invoke-direct {v2}, Ljava/lang/Object;-><init>()V
    new-instance v3, Ljava/lang/ref/WeakReference;
    invoke-direct {v3, v2}, Ljava/lang/ref/WeakReference;-><init>(Ljava/lang/Object;)V
    aput-object v3, p1, v0
    add-int/lit8 v0, v0, 0x1
    new-instance v2, Ljava/lang/Object;
    invoke-direct {v2}, Ljava/lang/Object;-><init>()V
    const/4 v4, 0x0
    aput-object v2, p2, v4
    new-instance v3, Ljava/lang/ref/WeakReference;
    invoke-direct {v3, v2, p0}, Ljava/lang/ref/WeakReference;-><init>(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V
    aput-object v3, p1, v0
    add-int/lit8 v0, v0, 0x1
    new-instance v2, Ljava/lang/Object;
    invoke-direct {v2}, Ljava/lang/Object;-><init>()V
    new-instance v3, Ljava/lang/ref/WeakReference;
    invoke-direct {v3, v2, p0}, Ljava/lang/ref/WeakReference;-><init>(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V
    iput-object v3, v3, Ljava/lang/ref/Reference;->queueNext:Ljava/lang/ref/Reference;
    aput-object v3, p1, v0
    const/4 v2, 0x0
    const/4 v3, 0x0
    return-void
.end method
)SMALI";

}  // namespace

// The collector clears the weak references to unreachable objects and hands
// those that have a queue to libcore as ART does: not during the collection
// (it may start inside an allocation) but at the next safepoint, as one
// circular list through Reference.pendingNext passed to ReferenceQueue.add.
TEST(gc_hands_cleared_references_to_libcore) {
  std::vector<tool::ClassDef> boot_classes{tool::assemble_smali(kRefBoot, "boot.smali")};
  std::vector<tool::ClassDef> app_classes{tool::assemble_smali(kRefApp, "refs.smali")};
  auto boot = dex::DexFile::open(tool::write_dex(boot_classes), "core-test.jar!classes.dex");
  auto app = dex::DexFile::open(tool::write_dex(app_classes), "refs.dex");
  auto runtime = std::make_unique<rt::Runtime>();
  runtime->add_dex(app.get());
  runtime->add_boot_dex(boot.get());
  {
    rt::Runtime::Scope scope(*runtime, "test");
    rt::Runtime& rt = *runtime;
    constexpr uint32_t kRefs = 40, kQueued = kRefs - 3;
    rt::Class* queue_cls = rt.find_class("Ljava/lang/ref/ReferenceQueue;");
    CHECK(queue_cls->boot);  // libcore's class, not the runtime's
    rt::GlobalRef queue(rt, rt.alloc(queue_cls));
    rt.call_virtual(queue.get(), "<init>", "()V");
    rt::GlobalRef refs(rt, rt.new_array("[Ljava/lang/ref/WeakReference;", int32_t(kRefs)));
    rt::GlobalRef kept(rt, rt.new_array("[Ljava/lang/Object;", 1));
    rt.call_static("LRefs;", "makeRefs",
                   "(Ljava/lang/ref/ReferenceQueue;[Ljava/lang/ref/WeakReference;[Ljava/lang/Object;)V",
                   {queue.get(), refs.get(), kept.get()});
    auto ref_at = [&](uint32_t i) {
      rt::Handle h;
      std::memcpy(&h, &rt.array(refs.get(), "WeakReference[]").data[size_t(i) * 4], 4);
      return h;
    };
    auto referent = [&](rt::Handle ref) { return rt::Handle(rt.call_virtual(ref, "get", "()Ljava/lang/Object;")); };
    rt::Field* pending_f =
        rt.find_class("Ljava/lang/ref/Reference;")->declared_field("pendingNext", "Ljava/lang/ref/Reference;");
    rt::Field* unenqueued_f = queue_cls->declared_field("unenqueued", "Ljava/lang/ref/Reference;");
    rt::Field* adds_f = queue_cls->declared_field("adds", "I");

    scrub_stack();
    rt.collect_garbage();
    CHECK_EQ(queue_cls->statics.at(adds_f->slot), 0u);  // no Java code inside the collection
    rt.safepoint();
    CHECK_EQ(queue_cls->statics.at(adds_f->slot), 1u);
    rt::Handle head = queue_cls->statics.at(unenqueued_f->slot);
    CHECK(head != rt::kNull);
    std::vector<rt::Handle> list;
    for (rt::Handle r = head; list.size() <= kRefs;) {
      list.push_back(r);
      r = rt.object(r)->fields.at(pending_f->slot);
      if (r == head) break;
    }
    CHECK(list.size() <= kQueued);
    // The native stacks are scanned conservatively: a stray word may keep a
    // referent alive, never more than a couple here.
    CHECK(list.size() + 2 >= kQueued);
    for (rt::Handle r : list) {
      CHECK_EQ(referent(r), rt::kNull);
      bool queued = false;
      for (uint32_t i = 0; i < kQueued; ++i) queued |= ref_at(i) == r;
      CHECK(queued);
    }
    CHECK_EQ(referent(ref_at(kQueued)), rt::kNull);      // cleared, but without a queue
    CHECK(referent(ref_at(kQueued + 1)) != rt::kNull);  // referent still reachable
    CHECK_EQ(referent(ref_at(kQueued + 2)), rt::kNull);  // cleared, already enqueued
    // The references delivered are cleared: a second collection hands nothing.
    rt.collect_garbage();
    rt.safepoint();
    CHECK_EQ(queue_cls->statics.at(adds_f->slot), 1u);
  }
  runtime.reset();
}
