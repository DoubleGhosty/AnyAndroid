// Libcore mode (G2): a boot class path in bytecode takes precedence over the
// bridge and the application, VM core classes stay native, and libcore's
// native methods are bound by signature. The "mini libcore" below mimics the
// shape of AOSP classes with a handful of methods.
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"
#include "smali.h"
#include "test.h"
#include "vfs/vfs.h"

using namespace aa;

namespace {

const char* kBootClasses[] = {
    R"(
.class public Ljava/util/ArrayList;
.super Ljava/lang/Object;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public size()I
    .registers 2
    const/16 v0, 42
    return v0
.end method
)",
    // VM core class: its native methods win, its bytecode only fills gaps.
    R"(
.class public final Ljava/lang/String;
.super Ljava/lang/Object;
.method public length()I
    .registers 2
    const/4 v0, -1
    return v0
.end method
.method public constructor <init>([III)V
    .registers 4
    new-instance v0, Ljava/lang/UnsupportedOperationException;
    invoke-direct {v0}, Ljava/lang/UnsupportedOperationException;-><init>()V
    throw v0
.end method
.method public shout()Ljava/lang/String;
    .registers 2
    const-string v0, "!"
    invoke-virtual {p0, v0}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
)",
    R"(
.class public final Ljava/lang/StringFactory;
.super Ljava/lang/Object;
.method public static newStringFromCodePoints([III)Ljava/lang/String;
    .registers 3
    const-string v0, "via StringFactory"
    return-object v0
.end method
)",
    R"(
.class public abstract Ljava/lang/invoke/VarHandle;
.super Ljava/lang/Object;
)",
    R"(
.class public Ljava/lang/invoke/FieldVarHandle;
.super Ljava/lang/invoke/VarHandle;
.field final artField:J
)",
    R"(
.class public final Ljava/lang/invoke/StaticFieldVarHandle;
.super Ljava/lang/invoke/FieldVarHandle;
)",
    R"(
.class public final Ljava/lang/invoke/ArrayElementVarHandle;
.super Ljava/lang/invoke/VarHandle;
)",
    R"(
.class public Ljava/lang/invoke/MethodHandle;
.super Ljava/lang/Object;
.field public artFieldOrMethod:J
)",
    R"(
.class public final Ljava/lang/Integer;
.super Ljava/lang/Object;
.field private final value:I
.method private constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Ljava/lang/Integer;->value:I
    return-void
.end method
.method public static valueOf(I)Ljava/lang/Integer;
    .registers 2
    new-instance v0, Ljava/lang/Integer;
    invoke-direct {v0, p0}, Ljava/lang/Integer;-><init>(I)V
    return-object v0
.end method
)",
    R"(
.class public final Ljava/lang/Float;
.super Ljava/lang/Object;
.method public static native floatToRawIntBits(F)I
.end method
.method public static bits(F)I
    .registers 2
    invoke-static {p0}, Ljava/lang/Float;->floatToRawIntBits(F)I
    move-result v0
    return v0
.end method
)",
    R"(
.class public Llibcore/Missing;
.super Ljava/lang/Object;
.method public static native nope()V
.end method
)",
    R"(
.class public final Landroid/system/OsConstants;
.super Ljava/lang/Object;
.field public static final EINTR:I
.field public static final O_CREAT:I
.field public static final FOO_UNKNOWN:I
.field public static final BAR_UNKNOWN:I
.method static constructor <clinit>()V
    .registers 0
    invoke-static {}, Landroid/system/OsConstants;->initConstants()V
    return-void
.end method
.method private static native initConstants()V
.end method
)",
    R"(
.class public final Ljava/io/FileDescriptor;
.super Ljava/lang/Object;
.field private descriptor:I
.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Ljava/io/FileDescriptor;->descriptor:I
    return-void
.end method
)",
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
.class public final Llibcore/io/Linux;
.super Ljava/lang/Object;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method private native writeBytes(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I
.end method
)",
    R"(
.class public final Lsun/misc/Unsafe;
.super Ljava/lang/Object;
.method public native compareAndSwapInt(Ljava/lang/Object;JII)Z
.end method
.method public native getInt(Ljava/lang/Object;J)I
.end method
.method public native putObject(Ljava/lang/Object;JLjava/lang/Object;)V
.end method
.method public native getObject(Ljava/lang/Object;J)Ljava/lang/Object;
.end method
)",
    R"(
.class public Ljava/nio/ByteBuffer;
.super Ljava/lang/Object;
.field public address:J
.field public hb:[B
)",
};

const char* kAppClasses[] = {
    R"(
.class public LApp;
.super Ljava/lang/Object;
.field public count:I
.field public ref:Ljava/lang/Object;
.field public static total:J
.field public final frozen:I
.method public static fromCodePoints()Ljava/lang/String;
    .registers 3
    const/4 v1, 0x0
    new-array v1, v1, [I
    const/4 v2, 0x0
    new-instance v0, Ljava/lang/String;
    invoke-direct {v0, v1, v2, v2}, Ljava/lang/String;-><init>([III)V
    return-object v0
.end method
.method public static shout()Ljava/lang/String;
    .registers 1
    const-string v0, "salut"
    invoke-virtual {v0}, Ljava/lang/String;->shout()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
.method public static listSize()I
    .registers 1
    new-instance v0, Ljava/util/ArrayList;
    invoke-direct {v0}, Ljava/util/ArrayList;-><init>()V
    invoke-virtual {v0}, Ljava/util/ArrayList;->size()I
    move-result v0
    return v0
.end method
.method public static strLen()I
    .registers 1
    const-string v0, "abc"
    invoke-virtual {v0}, Ljava/lang/String;->length()I
    move-result v0
    return v0
.end method
.method public static floatBits()I
    .registers 1
    const v0, 0x3fc00000
    invoke-static {v0}, Ljava/lang/Float;->bits(F)I
    move-result v0
    return v0
.end method
.method public static missing()V
    .registers 0
    invoke-static {}, Llibcore/Missing;->nope()V
    return-void
.end method
.method public static eintr()I
    .registers 1
    sget v0, Landroid/system/OsConstants;->EINTR:I
    return v0
.end method
.method public static addNumbers(II)I
    .registers 3
    add-int v0, p0, p1
    return v0
.end method
.method public multiply(I)I
    .registers 3
    iget v0, p0, LApp;->count:I
    mul-int/2addr v0, p1
    return v0
.end method
)",
    // An application cannot replace a boot class.
    R"(
.class public Ljava/util/ArrayList;
.super Ljava/lang/Object;
.method public size()I
    .registers 2
    const/4 v0, 7
    return v0
.end method
)",
};

struct LibcoreVm {
  std::unique_ptr<dex::DexFile> boot, app;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;

  LibcoreVm() {
    std::vector<tool::ClassDef> b, a;
    for (const char* s : kBootClasses) b.push_back(tool::assemble_smali(s, "boot.smali"));
    for (const char* s : kAppClasses) a.push_back(tool::assemble_smali(s, "app.smali"));
    boot = dex::DexFile::open(tool::write_dex(b), "core-test.jar!classes.dex");
    app = dex::DexFile::open(tool::write_dex(a), "app.dex");
    rt = std::make_unique<rt::Runtime>();
    rt->add_dex(app.get());  // added first on purpose: the boot class path still wins
    rt->add_boot_dex(boot.get());
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
  ~LibcoreVm() {
    scope.reset();
    rt.reset();
  }
  uint64_t call(const char* name, const char* sig, const char* cls = "LApp;") { return rt->call_static(cls, name, sig); }
  std::string thrown_message(const char* name, const char* sig) {
    try {
      call(name, sig);
    } catch (const rt::JavaThrow& t) {
      return rt->class_of(t.exception)->java_name() + ": " + rt->describe_throwable(t.exception);
    }
    return "";
  }
};

}  // namespace

TEST(libcore_boot_classes_take_precedence_except_vm_core) {
  LibcoreVm vm;
  CHECK(vm.rt->libcore_mode());
  CHECK_EQ(vm.call("listSize", "()I"), uint64_t(42));  // boot ArrayList, not the bridge's nor the app's
  CHECK(vm.rt->find_class("Ljava/util/ArrayList;")->boot);
  CHECK_EQ(vm.call("strLen", "()I"), uint64_t(3));  // native String kept
  CHECK(vm.rt->find_class("Ljava/lang/String;")->framework);
  // Classes absent from the boot class path still come from the bridge.
  CHECK(vm.rt->find_class("Landroid/app/Activity;")->framework);
}

TEST(libcore_natives_are_bound_by_signature) {
  LibcoreVm vm;
  CHECK_EQ(vm.call("floatBits", "()I"), uint64_t(0x3fc00000));
  std::string msg = vm.thrown_message("missing", "()V");
  CHECK(msg.find("java.lang.UnsatisfiedLinkError") == 0);
  CHECK(msg.find("[AnyAndroid] méthode native de la plateforme non implémentée : Llibcore/Missing;->nope()V") !=
        std::string::npos);
  CHECK(vm.rt->unsupported_apis().count("Llibcore/Missing;->nope()V"));
}

TEST(libcore_os_constants_and_errno) {
  LibcoreVm vm;
  CHECK_EQ(vm.call("eintr", "()I"), uint64_t(4));
  rt::Class* oc = vm.rt->find_class("Landroid/system/OsConstants;");
  auto value = [&](const char* name) { return oc->statics.at(oc->declared_field(name, "I")->slot); };
  CHECK_EQ(value("O_CREAT"), uint32_t(0100));
  CHECK(value("FOO_UNKNOWN") != value("BAR_UNKNOWN"));
  CHECK(value("FOO_UNKNOWN") >= 0x7E000000u);

  // writeBytes on a descriptor other than stdout/stderr: ErrnoException(EBADF).
  rt::Runtime& rt = *vm.rt;
  rt::Class* fdc = rt.find_class("Ljava/io/FileDescriptor;");
  rt::Handle fd = rt.alloc(fdc);
  rt.object(fd)->fields.at(fdc->declared_field("descriptor", "I")->slot) = 7;
  rt::Class* linux = rt.find_class("Llibcore/io/Linux;");
  rt::Handle os = rt.alloc(linux);
  rt::Handle buf = rt.new_array("[B", 4);
  rt::Method* write = linux->declared_method("writeBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I");
  uint32_t args[] = {os, fd, buf, 0, 4};
  bool thrown = false;
  try {
    rt.invoke(write, args, 5);
  } catch (const rt::JavaThrow& t) {
    rt::Class* ec = rt.class_of(t.exception);
    CHECK_EQ(ec->java_name(), std::string("android.system.ErrnoException"));
    CHECK_EQ(rt.object(t.exception)->fields.at(ec->declared_field("errno", "I")->slot), uint32_t(9));
    thrown = true;
  }
  CHECK(thrown);
}

TEST(libcore_boxes_use_bytecode_value_field) {
  LibcoreVm vm;
  rt::Handle h = bridge::box(*vm.rt, 'I', 1234);
  CHECK(vm.rt->class_of(h)->boot);
  char type = 0;
  uint64_t bits = 0;
  CHECK(bridge::unbox(*vm.rt, h, type, bits));
  CHECK_EQ(type, 'I');
  CHECK_EQ(bits, uint64_t(1234));
}

TEST(libcore_unsafe_field_and_array_access) {
  LibcoreVm vm;
  rt::Runtime& rt = *vm.rt;
  rt::Class* app = rt.find_class("LApp;");
  rt::Handle obj = rt.alloc(app);
  int64_t off = bridge::unsafe_field_offset(app->declared_field("count", "I")->slot);
  rt::Class* u = rt.find_class("Lsun/misc/Unsafe;");
  rt::Handle unsafe = rt.alloc(u);
  rt::Method* cas = u->declared_method("compareAndSwapInt", "(Ljava/lang/Object;JII)Z");
  uint32_t ok[] = {unsafe, obj, uint32_t(off), uint32_t(uint64_t(off) >> 32), 0, 5};
  CHECK_EQ(rt.invoke(cas, ok, 6), uint64_t(1));
  CHECK_EQ(rt.object(obj)->fields.at(app->declared_field("count", "I")->slot), uint32_t(5));
  CHECK_EQ(rt.invoke(cas, ok, 6), uint64_t(0));  // expected value no longer matches

  int64_t ref_off = bridge::unsafe_field_offset(app->declared_field("ref", "Ljava/lang/Object;")->slot);
  rt::Handle s = rt.new_string_utf8("x");
  uint32_t put[] = {unsafe, obj, uint32_t(ref_off), uint32_t(uint64_t(ref_off) >> 32), s};
  rt.invoke(u->declared_method("putObject", "(Ljava/lang/Object;JLjava/lang/Object;)V"), put, 5);
  uint32_t get[] = {unsafe, obj, uint32_t(ref_off), uint32_t(uint64_t(ref_off) >> 32)};
  CHECK_EQ(rt.invoke(u->declared_method("getObject", "(Ljava/lang/Object;J)Ljava/lang/Object;"), get, 4), uint64_t(s));

  rt::Handle arr = rt.new_array("[I", 4);
  int32_t v = 77;
  std::memcpy(&rt.array(arr, "int[]").data[2 * 4], &v, 4);
  uint32_t geti[] = {unsafe, arr, 16 + 2 * 4, 0};
  CHECK_EQ(rt.invoke(u->declared_method("getInt", "(Ljava/lang/Object;J)I"), geti, 4), uint64_t(77));
  uint32_t bad[] = {unsafe, arr, 16 + 4 * 4, 0};
  CHECK_THROWS(rt.invoke(u->declared_method("getInt", "(Ljava/lang/Object;J)I"), bad, 4), rt::JavaThrow);
}

TEST(libcore_vm_core_classes_merge_bytecode_and_string_factory) {
  LibcoreVm vm;
  // String.shout() exists only in the boot bytecode: merged into the native String.
  CHECK_EQ(vm.rt->string_utf8(rt::Handle(vm.call("shout", "()Ljava/lang/String;"))), std::string("salut!"));
  // String(int[], int, int) is redirected to StringFactory, as in ART.
  CHECK_EQ(vm.rt->string_utf8(rt::Handle(vm.call("fromCodePoints", "()Ljava/lang/String;"))), std::string("via StringFactory"));
  // ART's count field: (length << 1) | uncompressed flag.
  rt::Class* s = vm.rt->find_class("Ljava/lang/String;");
  uint32_t slot = s->declared_field("count", "I")->slot;
  CHECK_EQ(vm.rt->object(vm.rt->new_string(u"abc"))->fields.at(slot), uint32_t(6));
  CHECK_EQ(vm.rt->object(vm.rt->new_string(u"été"))->fields.at(slot), uint32_t(7));
}

namespace {

// Calls a VarHandle access mode as invoke-polymorphic would.
uint64_t vh_call(rt::Runtime& rt, rt::Handle vh, const char* mode, const char* callsite, std::vector<uint32_t> args) {
  args.insert(args.begin(), vh);
  return rt.invoke_polymorphic(rt.find_class("Ljava/lang/invoke/VarHandle;"), mode, callsite, args.data(),
                               uint32_t(args.size()));
}

// Calls a MethodHandle as invoke-polymorphic would.
uint64_t mh_call(rt::Runtime& rt, rt::Handle mh, const char* name, const char* callsite, std::vector<uint32_t> args) {
  args.insert(args.begin(), mh);
  return rt.invoke_polymorphic(rt.find_class("Ljava/lang/invoke/MethodHandle;"), name, callsite, args.data(),
                               uint32_t(args.size()));
}

rt::Handle field_handle(rt::Runtime& rt, const char* cls, rt::Field* f) {
  rt::Class* c = rt.find_class(cls);
  rt::Handle vh = rt.alloc(c);
  rt::Field* cookie = rt.find_class("Ljava/lang/invoke/FieldVarHandle;")->declared_field("artField", "J");
  uint64_t v = rt.field_cookie(f);
  rt.object(vh)->fields.at(cookie->slot) = uint32_t(v);
  rt.object(vh)->fields.at(cookie->slot + 1) = uint32_t(v >> 32);
  return vh;
}

}  // namespace

TEST(libcore_varhandles_on_fields_statics_and_arrays) {
  LibcoreVm vm;
  rt::Runtime& rt = *vm.rt;
  rt::Class* app = rt.find_class("LApp;");
  rt::Handle obj = rt.alloc(app);
  rt::Field* count = app->declared_field("count", "I");
  rt::Handle vh = field_handle(rt, "Ljava/lang/invoke/FieldVarHandle;", count);
  CHECK_EQ(vh_call(rt, vh, "compareAndSet", "(LApp;II)Z", {obj, 0, 5}), uint64_t(1));
  CHECK_EQ(vh_call(rt, vh, "compareAndSet", "(LApp;II)Z", {obj, 0, 9}), uint64_t(0));
  CHECK_EQ(vh_call(rt, vh, "getAndAdd", "(LApp;I)I", {obj, 3}), uint64_t(5));
  CHECK_EQ(vh_call(rt, vh, "getVolatile", "(LApp;)I", {obj}), uint64_t(8));
  vh_call(rt, vh, "setRelease", "(LApp;I)V", {obj, 42});
  CHECK_EQ(rt.object(obj)->fields.at(count->slot), uint32_t(42));
  CHECK_EQ(vh_call(rt, vh, "getAndBitwiseOr", "(LApp;I)I", {obj, 1}), uint64_t(42));
  CHECK_EQ(rt.object(obj)->fields.at(count->slot), uint32_t(43));
  // Boxed call site (Object) on an int field.
  rt::Handle boxed = rt::Handle(vh_call(rt, vh, "get", "(LApp;)Ljava/lang/Object;", {obj}));
  char type = 0;
  uint64_t bits = 0;
  CHECK(bridge::unbox(rt, boxed, type, bits) && type == 'I' && bits == 43);
  // Wrong receiver type and unknown handle.
  CHECK_THROWS(vh_call(rt, vh, "get", "(Ljava/lang/Object;)I", {rt.new_string(u"x")}), rt::JavaThrow);
  rt::Handle forged = rt.alloc(rt.find_class("Ljava/lang/invoke/FieldVarHandle;"));
  rt.object(forged)->fields.at(rt.find_class("Ljava/lang/invoke/FieldVarHandle;")->declared_field("artField", "J")->slot) = 0xDEAD;
  CHECK_THROWS(vh_call(rt, forged, "get", "(LApp;)I", {obj}), rt::JavaThrow);
  // Final fields are read-only.
  rt::Handle frozen = field_handle(rt, "Ljava/lang/invoke/FieldVarHandle;", app->declared_field("frozen", "I"));
  CHECK_THROWS(vh_call(rt, frozen, "set", "(LApp;I)V", {obj, 1}), rt::JavaThrow);

  // Static long field: wide values span two argument words.
  rt::Handle st = field_handle(rt, "Ljava/lang/invoke/StaticFieldVarHandle;", app->declared_field("total", "J"));
  int64_t big = int64_t(1) << 40;
  vh_call(rt, st, "set", "(J)V", {uint32_t(big), uint32_t(uint64_t(big) >> 32)});
  CHECK_EQ(vh_call(rt, st, "getAndAdd", "(J)J", {1, 0}), uint64_t(big));
  CHECK_EQ(vh_call(rt, st, "get", "()J", {}), uint64_t(big + 1));

  // Array elements, with bounds checks.
  rt::Handle arr = rt.new_array("[I", 3);
  rt::Handle ah = rt.alloc(rt.find_class("Ljava/lang/invoke/ArrayElementVarHandle;"));
  CHECK_EQ(vh_call(rt, ah, "compareAndExchange", "([IIII)I", {arr, 2, 0, 7}), uint64_t(0));
  CHECK_EQ(vh_call(rt, ah, "getAcquire", "([II)I", {arr, 2}), uint64_t(7));
  CHECK_THROWS(vh_call(rt, ah, "get", "([II)I", {arr, 3}), rt::JavaThrow);
  rt::Handle strings = rt.new_array("[Ljava/lang/String;", 1);
  CHECK_THROWS(vh_call(rt, ah, "set", "([Ljava/lang/Object;ILjava/lang/Object;)V", {strings, 0, rt.alloc(app)}),
               rt::JavaThrow);  // ArrayStoreException
}

namespace {

uint64_t native_call(rt::Runtime& rt, const char* cls, const char* name, const char* sig, std::vector<uint32_t> args) {
  const rt::NativeBinding* b = rt.registry().binding(cls, name, sig);
  CHECK(b != nullptr);
  rt::NativeArgs a(rt, args.data(), uint32_t(args.size()));
  return b->fn(rt, a);
}

}  // namespace

// Android 17 frees themes through NativeAllocationRegistry (ResourcesImpl.ThemeImpl):
// the free "function" is a bounded id, and the value passed to it is checked
// like any other handle.
TEST(framework_theme_free_function) {
  LibcoreVm vm;
  rt::Runtime& rt = *vm.rt;
  const char* am = "Landroid/content/res/AssetManager;";
  auto words = [](uint64_t v) { return std::vector<uint32_t>{uint32_t(v), uint32_t(v >> 32)}; };
  uint64_t manager = native_call(rt, am, "nativeCreate", "()J", {});
  uint64_t theme = native_call(rt, am, "nativeThemeCreate", "(J)J", words(manager));
  auto changing = [&](uint64_t t) { return native_call(rt, am, "nativeThemeGetChangingConfigurations", "(J)I", words(t)); };
  CHECK_EQ(changing(theme), uint64_t(0));
  uint64_t free_fn = native_call(rt, am, "nativeGetThemeFreeFunction", "()J", {});
  CHECK(free_fn != 0);
  CHECK_EQ(native_call(rt, am, "nativeGetThemeFreeFunction", "()J", {}), free_fn);
  auto apply = [&](uint64_t ptr) {
    std::vector<uint32_t> args = words(free_fn);
    for (uint32_t w : words(ptr)) args.push_back(w);
    native_call(rt, "Llibcore/util/NativeAllocationRegistry;", "applyFreeFunction", "(JJ)V", args);
  };
  apply(manager);  // another kind of handle: ignored
  apply(theme);
  CHECK_THROWS(changing(theme), rt::JavaThrow);  // freed
  apply(theme);                                  // stale: ignored
  apply(0x4141414141414141);                     // forged: ignored
  uint64_t other = native_call(rt, am, "nativeThemeCreate", "(J)J", words(manager));  // the manager is untouched
  CHECK_EQ(changing(other), uint64_t(0));
  apply(other);
  native_call(rt, am, "nativeDestroy", "(J)V", words(manager));
}

// libcore's Thread.blockedOn (AbstractInterruptibleChannel.begin, on every
// FileChannel read or write) locks Thread.blockerLock, set up by the native
// constructors and for the threads the runtime creates (here the main thread).
TEST(libcore_thread_blocked_on) {
  const char* boot_thread = R"(
.class public Ljava/lang/Thread;
.super Ljava/lang/Object;
.method public blockedOn(Lsun/nio/ch/Interruptible;)V
    .registers 3
    iget-object v0, p0, Ljava/lang/Thread;->blockerLock:Ljava/lang/Object;
    monitor-enter v0
    iput-object p1, p0, Ljava/lang/Thread;->blocker:Lsun/nio/ch/Interruptible;
    monitor-exit v0
    return-void
.end method
)";
  const char* app = R"(
.class public LBlocking;
.super Ljava/lang/Object;
.method public static run()V
    .registers 2
    const/4 v1, 0x0
    invoke-static {}, Ljava/lang/Thread;->currentThread()Ljava/lang/Thread;
    move-result-object v0
    invoke-virtual {v0, v1}, Ljava/lang/Thread;->blockedOn(Lsun/nio/ch/Interruptible;)V
    new-instance v0, Ljava/lang/Thread;
    invoke-direct {v0}, Ljava/lang/Thread;-><init>()V
    invoke-virtual {v0, v1}, Ljava/lang/Thread;->blockedOn(Lsun/nio/ch/Interruptible;)V
    return-void
.end method
)";
  auto boot = dex::DexFile::open(tool::write_dex({tool::assemble_smali(boot_thread, "boot.smali")}), "core-test.jar!classes.dex");
  auto code = dex::DexFile::open(tool::write_dex({tool::assemble_smali(app, "app.smali")}), "app.dex");
  bridge::NullHost host;
  auto runtime = std::make_unique<rt::Runtime>();
  runtime->add_boot_dex(boot.get());
  runtime->add_dex(code.get());
  bridge::install(runtime->registry(), host);
  {
    rt::Runtime::Scope scope(*runtime, "test");
    std::string thrown;
    try {
      runtime->call_static("LBlocking;", "run", "()V");
    } catch (const rt::JavaThrow& t) {
      thrown = runtime->describe_throwable(t.exception);
    }
    CHECK_EQ(thrown, std::string());
  }
}

TEST(libcore_charset_converters) {
  LibcoreVm vm;
  rt::Runtime& rt = *vm.rt;
  const char* nc = "Lcom/android/icu/charset/NativeConverter;";
  auto open = [&](const char* name) {
    return native_call(rt, nc, "openConverter", "(Ljava/lang/String;)J", {rt.new_string_utf8(name)});
  };
  uint64_t utf8 = open("utf8");  // aliases are case-insensitive
  std::u16string text = u"Aé€\U0001F600";
  rt::Handle chars = rt.new_array("[C", int32_t(text.size()));
  std::memcpy(rt.array(chars, "c").data.data(), text.data(), text.size() * 2);
  rt::Handle bytes = rt.new_array("[B", 16);
  rt::Handle data = rt.new_array("[I", 3);
  auto data_at = [&](int i) {
    int32_t v;
    std::memcpy(&v, &rt.array(data, "d").data[size_t(i) * 4], 4);
    return v;
  };
  auto set_data = [&](int32_t a0, int32_t a1) {
    int32_t d[3] = {a0, a1, 0};
    std::memcpy(rt.array(data, "d").data.data(), d, 12);
  };
  auto encode = [&](uint64_t conv, int32_t src_end, int32_t dst_end, bool flush) {
    return int32_t(native_call(rt, nc, "encode", "(J[CI[BI[IZ)I",
                               {uint32_t(conv), uint32_t(conv >> 32), chars, uint32_t(src_end), bytes, uint32_t(dst_end),
                                data, flush ? 1u : 0u}));
  };
  set_data(0, 0);
  CHECK_EQ(encode(utf8, int32_t(text.size()), 16, true), 0);
  CHECK_EQ(data_at(0), 5);   // chars consumed
  CHECK_EQ(data_at(1), 10);  // bytes written
  const uint8_t expected[] = {'A', 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80};
  CHECK(std::memcmp(rt.array(bytes, "b").data.data(), expected, sizeof expected) == 0);

  // From a later output offset, the encoder reports the absolute end.
  set_data(0, 2);
  CHECK_EQ(encode(utf8, int32_t(text.size()), 16, true), 0);
  CHECK_EQ(data_at(0), 5);
  CHECK_EQ(data_at(1), 12);
  CHECK(std::memcmp(rt.array(bytes, "b").data.data() + 2, expected, sizeof expected) == 0);
  set_data(0, 0);
  CHECK_EQ(encode(utf8, int32_t(text.size()), 16, true), 0);

  // Overflow: stops before a character that does not fit.
  set_data(0, 0);
  CHECK_EQ(encode(utf8, int32_t(text.size()), 4, false), 15);
  CHECK_EQ(data_at(0), 2);
  CHECK_EQ(data_at(1), 3);

  // US-ASCII cannot map 'é': reported as unmappable, one char.
  uint64_t ascii = open("US-ASCII");
  set_data(0, 0);
  CHECK_EQ(encode(ascii, int32_t(text.size()), 16, true), 10);
  CHECK_EQ(data_at(0), 2);
  CHECK_EQ(data_at(2), 1);

  // Decoding back, then a malformed byte.
  rt::Handle out = rt.new_array("[C", 8);
  auto decode = [&](uint64_t conv, int32_t src_end) {
    return int32_t(native_call(rt, nc, "decode", "(J[BI[CI[IZ)I",
                               {uint32_t(conv), uint32_t(conv >> 32), bytes, uint32_t(src_end), out, 8, data, 1}));
  };
  set_data(0, 0);
  CHECK_EQ(decode(utf8, 10), 0);
  CHECK_EQ(data_at(1), 5);
  CHECK(std::memcmp(rt.array(out, "o").data.data(), text.data(), text.size() * 2) == 0);
  // From a later output offset, the decoder reports the chars written (as
  // CharsetDecoderICU adds it to the buffer's position), not the end.
  set_data(1, 3);
  CHECK_EQ(decode(utf8, 10), 0);
  CHECK_EQ(data_at(0), 9);
  CHECK_EQ(data_at(1), 4);
  CHECK(std::memcmp(rt.array(out, "o").data.data() + 6, text.data() + 1, 4 * 2) == 0);
  rt.array(bytes, "b").data[1] = 0xFF;
  set_data(0, 0);
  CHECK_EQ(decode(utf8, 10), 12);
  CHECK_EQ(data_at(2), 1);
  CHECK_THROWS(native_call(rt, nc, "openConverter", "(Ljava/lang/String;)J", {rt.new_string_utf8("EBCDIC-XYZ")}),
               rt::JavaThrow);
  CHECK_THROWS(native_call(rt, nc, "getMaxBytesPerChar", "(J)I", {12345, 0}), rt::JavaThrow);
}

TEST(libcore_scatter_gather_readv_writev) {
  LibcoreVm vm;
  rt::Runtime& rt = *vm.rt;
  auto& cfg = bridge::libcore_config();
  auto saved_vfs = cfg.vfs;

  auto temp_dir = std::filesystem::temp_directory_path() / "aa-test-scatter-gather";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  cfg.vfs = std::make_shared<vfs::Vfs>();
  cfg.vfs->mount("/tmp", vfs::host_mount(temp_dir, false));

  int32_t raw_fd = cfg.vfs->open("/tmp/sg.bin", vfs::kO_RDWR | vfs::kO_CREAT | vfs::kO_TRUNC, 0666);
  CHECK(raw_fd >= 0);

  rt::Class* fdc = rt.find_class("Ljava/io/FileDescriptor;");
  rt::Handle fd = rt.alloc(fdc);
  rt.object(fd)->fields.at(fdc->declared_field("descriptor", "I")->slot) = uint32_t(raw_fd);

  // 1. Linux.writev with byte[] and ByteBuffer
  rt::Handle b1 = rt.new_array("[B", 5);
  std::memcpy(rt.array(b1, "b1").data.data(), "Hello", 5);

  rt::Class* bbc = rt.find_class("Ljava/nio/ByteBuffer;");
  rt::Handle b2 = rt.alloc(bbc);
  int64_t n_mem = bridge::native_alloc(rt, 6);
  std::memcpy(bridge::native_span(rt, n_mem, 6, true).data(), " World", 6);
  rt.object(b2)->fields.at(bbc->declared_field("address", "J")->slot) = uint32_t(n_mem);
  rt.object(b2)->fields.at(bbc->declared_field("address", "J")->slot + 1) = uint32_t(uint64_t(n_mem) >> 32);

  rt::Handle bufs = rt.new_array("[Ljava/lang/Object;", 2);
  uint32_t b1_h = b1, b2_h = b2;
  std::memcpy(&rt.array(bufs, "bufs").data[0], &b1_h, 4);
  std::memcpy(&rt.array(bufs, "bufs").data[4], &b2_h, 4);

  rt::Handle offsets = rt.new_array("[I", 2);
  int32_t offs[2] = {0, 0};
  std::memcpy(rt.array(offsets, "offs").data.data(), offs, sizeof(offs));

  rt::Handle counts = rt.new_array("[I", 2);
  int32_t cnts[2] = {5, 6};
  std::memcpy(rt.array(counts, "cnts").data.data(), cnts, sizeof(cnts));

  rt::Class* lx = rt.find_class("Llibcore/io/Linux;");
  rt::Handle os = rt.alloc(lx);
  uint64_t written = native_call(rt, "Llibcore/io/Linux;", "writev",
                                 "(Ljava/io/FileDescriptor;[Ljava/lang/Object;[I[I)I",
                                 {os, fd, bufs, offsets, counts});
  CHECK_EQ(written, 11u);

  // 2. Linux.readv
  cfg.vfs->lseek(raw_fd, 0, 0);
  rt::Handle r1 = rt.new_array("[B", 5);
  rt::Handle r2 = rt.new_array("[B", 6);
  rt::Handle rbufs = rt.new_array("[Ljava/lang/Object;", 2);
  uint32_t r1_h = r1, r2_h = r2;
  std::memcpy(&rt.array(rbufs, "rbufs").data[0], &r1_h, 4);
  std::memcpy(&rt.array(rbufs, "rbufs").data[4], &r2_h, 4);

  uint64_t read_bytes = native_call(rt, "Llibcore/io/Linux;", "readv",
                                    "(Ljava/io/FileDescriptor;[Ljava/lang/Object;[I[I)I",
                                    {os, fd, rbufs, offsets, counts});
  CHECK_EQ(read_bytes, 11u);
  CHECK(std::memcmp(rt.array(r1, "r1").data.data(), "Hello", 5) == 0);
  CHECK(std::memcmp(rt.array(r2, "r2").data.data(), " World", 6) == 0);

  // 3. FileDispatcherImpl.writev0 / readv0
  cfg.vfs->lseek(raw_fd, 0, 0);
  int64_t iov_write = bridge::native_alloc(rt, 32);
  int64_t w1 = bridge::native_alloc(rt, 4);
  int64_t w2 = bridge::native_alloc(rt, 4);
  std::memcpy(bridge::native_span(rt, w1, 4, true).data(), "ABC-", 4);
  std::memcpy(bridge::native_span(rt, w2, 4, true).data(), "1234", 4);

  uint64_t iov0_base = uint64_t(w1);
  uint64_t iov0_len = 4;
  std::memcpy(bridge::native_span(rt, iov_write, 8, true).data(), &iov0_base, 8);
  std::memcpy(bridge::native_span(rt, iov_write + 8, 8, true).data(), &iov0_len, 8);

  uint64_t iov1_base = uint64_t(w2);
  uint64_t iov1_len = 4;
  std::memcpy(bridge::native_span(rt, iov_write + 16, 8, true).data(), &iov1_base, 8);
  std::memcpy(bridge::native_span(rt, iov_write + 24, 8, true).data(), &iov1_len, 8);

  uint64_t written_v0 = native_call(rt, "Lsun/nio/ch/FileDispatcherImpl;", "writev0",
                                    "(Ljava/io/FileDescriptor;JI)J",
                                    {fd, uint32_t(iov_write), uint32_t(uint64_t(iov_write) >> 32), 2});
  CHECK_EQ(written_v0, 8u);

  // Read back with readv0
  cfg.vfs->lseek(raw_fd, 0, 0);
  int64_t iov_read = bridge::native_alloc(rt, 32);
  int64_t rd1 = bridge::native_alloc(rt, 4);
  int64_t rd2 = bridge::native_alloc(rt, 4);

  uint64_t r_iov0_base = uint64_t(rd1);
  uint64_t r_iov0_len = 4;
  std::memcpy(bridge::native_span(rt, iov_read, 8, true).data(), &r_iov0_base, 8);
  std::memcpy(bridge::native_span(rt, iov_read + 8, 8, true).data(), &r_iov0_len, 8);

  uint64_t r_iov1_base = uint64_t(rd2);
  uint64_t r_iov1_len = 4;
  std::memcpy(bridge::native_span(rt, iov_read + 16, 8, true).data(), &r_iov1_base, 8);
  std::memcpy(bridge::native_span(rt, iov_read + 24, 8, true).data(), &r_iov1_len, 8);

  uint64_t read_v0 = native_call(rt, "Lsun/nio/ch/FileDispatcherImpl;", "readv0",
                                 "(Ljava/io/FileDescriptor;JI)J",
                                 {fd, uint32_t(iov_read), uint32_t(uint64_t(iov_read) >> 32), 2});
  CHECK_EQ(read_v0, 8u);
  CHECK(std::memcmp(bridge::native_span(rt, rd1, 4).data(), "ABC-", 4) == 0);
  CHECK(std::memcmp(bridge::native_span(rt, rd2, 4).data(), "1234", 4) == 0);

  // EOF test on readv0
  cfg.vfs->lseek(raw_fd, 11, 0);
  uint64_t eof_v0 = native_call(rt, "Lsun/nio/ch/FileDispatcherImpl;", "readv0",
                                "(Ljava/io/FileDescriptor;JI)J",
                                {fd, uint32_t(iov_read), uint32_t(uint64_t(iov_read) >> 32), 2});
  CHECK_EQ(int64_t(eof_v0), -1);

  // Clean up
  bridge::native_free(rt, n_mem);
  bridge::native_free(rt, w1);
  bridge::native_free(rt, w2);
  bridge::native_free(rt, iov_write);
  bridge::native_free(rt, rd1);
  bridge::native_free(rt, rd2);
  bridge::native_free(rt, iov_read);

  cfg.vfs->close(raw_fd);
  cfg.vfs = saved_vfs;
  std::filesystem::remove_all(temp_dir);
}

TEST(libcore_methodhandle_invoke_and_invoke_exact) {
  LibcoreVm vm;
  rt::Runtime& rt = *vm.rt;
  rt::Class* app = rt.find_class("LApp;");
  rt::Class* mhc = rt.find_class("Ljava/lang/invoke/MethodHandle;");
  rt::Field* afom = mhc->declared_field("artFieldOrMethod", "J");
  CHECK(afom != nullptr);

  // 1. Static method: App.addNumbers(II)I
  rt::Method* add_m = app->declared_method("addNumbers", "(II)I");
  CHECK(add_m != nullptr);
  rt::Handle mh_add = rt.alloc(mhc);
  uint64_t add_cookie = rt.method_cookie(add_m);
  rt.object(mh_add)->fields.at(afom->slot) = uint32_t(add_cookie);
  rt.object(mh_add)->fields.at(afom->slot + 1) = uint32_t(add_cookie >> 32);

  // invokeExact matching callsite
  uint64_t res = mh_call(rt, mh_add, "invokeExact", "(II)I", {15, 27});
  CHECK_EQ(res, 42u);

  // invokeExact mismatching callsite -> WrongMethodTypeException (JavaThrow)
  CHECK_THROWS(mh_call(rt, mh_add, "invokeExact", "(I)I", {15}), rt::JavaThrow);
  CHECK_THROWS(mh_call(rt, mh_add, "invokeExact", "(II)V", {15, 27}), rt::JavaThrow);

  // invoke with primitive widening conversion (short -> int)
  uint64_t res_conv = mh_call(rt, mh_add, "invoke", "(SS)I", {10, 20});
  CHECK_EQ(res_conv, 30u);

  // 2. Virtual method: App.multiply(I)I
  rt::Method* mul_m = app->declared_method("multiply", "(I)I");
  CHECK(mul_m != nullptr);
  rt::Handle mh_mul = rt.alloc(mhc);
  uint64_t mul_cookie = rt.method_cookie(mul_m);
  rt.object(mh_mul)->fields.at(afom->slot) = uint32_t(mul_cookie);
  rt.object(mh_mul)->fields.at(afom->slot + 1) = uint32_t(mul_cookie >> 32);

  rt::Handle obj = rt.alloc(app);
  rt.object(obj)->fields.at(app->declared_field("count", "I")->slot) = 7;

  // invokeExact with receiver as first argument
  uint64_t mul_res = mh_call(rt, mh_mul, "invokeExact", "(LApp;I)I", {obj, 6});
  CHECK_EQ(mul_res, 42u);

  // invokeExact with null receiver -> NullPointerException
  CHECK_THROWS(mh_call(rt, mh_mul, "invokeExact", "(LApp;I)I", {rt::kNull, 6}), rt::JavaThrow);
}

