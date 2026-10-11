#include <cstring>
#include <memory>
#include <string>

#include "common/text.h"
#include "dex-engine/dex_file.h"
#include "dex-engine/jlang.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kT = R"SMALI(
.class public LT;
.super Ljava/lang/Object;
.source "T.java"

.method public static add(II)I
    .registers 3
    add-int v0, p0, p1
    return v0
.end method

.method public static arith(IIII)I
    .registers 5
    mul-int v0, p0, p1
    add-int/2addr v0, p2
    div-int/2addr v0, p3
    return v0
.end method

.method public static rem(II)I
    .registers 3
    rem-int v0, p0, p1
    return v0
.end method

.method public static lmul(JJ)J
    .registers 6
    mul-long v0, p0, p2
    return-wide v0
.end method

.method public static lshl(JI)J
    .registers 4
    shl-long v0, p0, p2
    return-wide v0
.end method

.method public static fdiv(FF)F
    .registers 3
    div-float v0, p0, p1
    return v0
.end method

.method public static d2i(D)I
    .registers 3
    double-to-int v0, p0
    return v0
.end method

.method public static sum(I)I
    .registers 4
    const/4 v0, 0x0
    const/4 v1, 0x1
    :loop
    if-gt v1, p0, :done
    add-int/2addr v0, v1
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

.method public static fib(I)I
    .registers 4
    const/4 v0, 0x2
    if-ge p0, v0, :rec
    return p0
    :rec
    add-int/lit8 v0, p0, -0x1
    invoke-static {v0}, LT;->fib(I)I
    move-result v1
    add-int/lit8 v0, p0, -0x2
    invoke-static {v0}, LT;->fib(I)I
    move-result v2
    add-int/2addr v1, v2
    return v1
.end method

.method public static sw(I)I
    .registers 2
    packed-switch p0, :pdata
    const/4 v0, -0x1
    return v0
    :c0
    const/16 v0, 0xa
    return v0
    :c1
    const/16 v0, 0x14
    return v0
    :pdata
    .packed-switch 0x5
        :c0
        :c1
    .end packed-switch
.end method

.method public static sp(I)I
    .registers 2
    sparse-switch p0, :sdata
    const/4 v0, 0x0
    return v0
    :a
    const/4 v0, 0x1
    return v0
    :b
    const/4 v0, 0x2
    return v0
    :sdata
    .sparse-switch
        -0x64 -> :a
        0x3e8 -> :b
    .end sparse-switch
.end method

.method public static arrays()I
    .registers 5
    const/4 v0, 0x3
    new-array v1, v0, [I
    fill-array-data v1, :data
    const/4 v2, 0x1
    aget v3, v1, v2
    array-length v4, v1
    add-int/2addr v3, v4
    const/4 v2, 0x2
    aget v4, v1, v2
    add-int/2addr v3, v4
    return v3
    :data
    .array-data 4
        0xa 0x14 0x1e
    .end array-data
.end method

.method public static filled()I
    .registers 4
    const/4 v0, 0x7
    const/4 v1, 0x5
    filled-new-array {v0, v1}, [I
    move-result-object v2
    const/4 v0, 0x1
    aget v3, v2, v0
    return v3
.end method

.method public static oob()I
    .registers 3
    const/4 v0, 0x2
    new-array v1, v0, [I
    const/4 v0, 0x5
    aget v2, v1, v0
    return v2
.end method

.method public static safediv(II)I
    .registers 4
    :try_start
    div-int v0, p0, p1
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :handler
    return v0
    :handler
    move-exception v1
    const/4 v0, -0x1
    return v0
.end method

.method public static thrower()Ljava/lang/String;
    .registers 3
    :start
    new-instance v0, LMyEx;
    const-string v1, "boum"
    invoke-direct {v0, v1}, LMyEx;-><init>(Ljava/lang/String;)V
    throw v0
    :end
    .catch Ljava/lang/RuntimeException; {:start .. :end} :h
    :h
    move-exception v0
    invoke-virtual {v0}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static uncaught()V
    .registers 2
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "état invalide"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
.end method

.method public static dispatch()I
    .registers 4
    new-instance v0, LDog;
    invoke-direct {v0}, LDog;-><init>()V
    invoke-interface {v0}, LSpeaker;->speak()I
    move-result v1
    invoke-virtual {v0}, LAnimal;->legs()I
    move-result v2
    mul-int/lit8 v1, v1, 0xa
    add-int/2addr v1, v2
    instance-of v3, v0, LSpeaker;
    add-int/2addr v1, v3
    return v1
.end method

.method public static hash()I
    .registers 2
    const-string v0, "hello"
    invoke-virtual {v0}, Ljava/lang/String;->hashCode()I
    move-result v1
    return v1
.end method

.method public static concat()Ljava/lang/String;
    .registers 3
    const-string v0, "Bon"
    const-string v1, "jour"
    invoke-virtual {v0, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    const/16 v1, 0x2a
    invoke-static {v1}, Ljava/lang/String;->valueOf(I)Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static recurse()V
    .registers 0
    invoke-static {}, LT;->recurse()V
    return-void
.end method

.method public static spin()V
    .registers 1
    :l
    goto :l
.end method

.method public static sleepy()V
    .registers 2
    const-wide/16 v0, 0x1
    invoke-static {v0, v1}, Landroid/os/SystemClock;->sleep(J)V
    return-void
.end method

.method public static missing()I
    .registers 2
    const-string v0, "abc"
    invoke-virtual {v0}, Ljava/lang/String;->codePointCount()I
    move-result v1
    return v1
.end method

.method public static npe()I
    .registers 2
    const/4 v0, 0x0
    invoke-virtual {v0}, Ljava/lang/String;->length()I
    move-result v1
    return v1
.end method
)SMALI";

const char* kMyEx = R"SMALI(
.class public LMyEx;
.super Ljava/lang/RuntimeException;
.method public constructor <init>(Ljava/lang/String;)V
    .registers 2
    invoke-direct {p0, p1}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/String;)V
    return-void
.end method
)SMALI";

const char* kAnimals = R"SMALI(
.class public LAnimal;
.super Ljava/lang/Object;
.implements LSpeaker;
.field protected legs:I
.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    const/4 v0, 0x4
    iput v0, p0, LAnimal;->legs:I
    return-void
.end method
.method public speak()I
    .registers 2
    const/4 v0, 0x1
    return v0
.end method
.method public legs()I
    .registers 2
    iget v0, p0, LAnimal;->legs:I
    return v0
.end method
)SMALI";

const char* kDog = R"SMALI(
.class public LDog;
.super LAnimal;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, LAnimal;-><init>()V
    return-void
.end method
.method public speak()I
    .registers 3
    invoke-super {p0}, LAnimal;->speak()I
    move-result v0
    add-int/lit8 v0, v0, 0x1
    return v0
.end method
)SMALI";

const char* kSpeaker = R"SMALI(
.class public interface abstract LSpeaker;
.super Ljava/lang/Object;
.method public abstract speak()I
.end method
)SMALI";

const char* kCounter = R"SMALI(
.class public LCounter;
.super Ljava/lang/Object;
.field public static count:I
.field public static final NAME:Ljava/lang/String; = "compteur"
.field public static final BIG:J = 0x123456789L
.method static constructor <clinit>()V
    .registers 1
    const/16 v0, 0x64
    sput v0, LCounter;->count:I
    return-void
.end method
.method public static next()I
    .registers 2
    sget v0, LCounter;->count:I
    add-int/lit8 v0, v0, 0x1
    sput v0, LCounter;->count:I
    return v0
.end method
.method public static name()Ljava/lang/String;
    .registers 1
    sget-object v0, LCounter;->NAME:Ljava/lang/String;
    return-object v0
.end method
.method public static big()J
    .registers 2
    sget-wide v0, LCounter;->BIG:J
    return-wide v0
.end method
)SMALI";

struct Vm {
  std::unique_ptr<dex::DexFile> dex;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;  // released before the runtime is destroyed

  explicit Vm(std::vector<tool::ClassDef> classes, rt::RuntimeOptions opts = {}) {
    dex = dex::DexFile::open(tool::write_dex(classes), "test.dex");
    rt = std::make_unique<rt::Runtime>(opts);
    rt->add_dex(dex.get());
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
  Vm(Vm&&) = default;
  ~Vm() {
    scope.reset();
    rt.reset();
  }
  static Vm standard(rt::RuntimeOptions opts = {}) {
    std::vector<tool::ClassDef> classes;
    for (const char* s : {kT, kMyEx, kAnimals, kDog, kSpeaker, kCounter}) classes.push_back(tool::assemble_smali(s, "t.smali"));
    return Vm(std::move(classes), opts);
  }
  uint64_t call(const char* name, const char* sig, std::vector<uint32_t> args = {}, const char* cls = "LT;") {
    return rt->call_static(cls, name, sig, std::move(args));
  }
  // Runs the call and returns the Java exception class name ("" if none).
  std::string thrown(const char* name, const char* sig, std::vector<uint32_t> args = {}) {
    try {
      call(name, sig, std::move(args));
    } catch (const rt::JavaThrow& t) {
      return rt->class_of(t.exception)->java_name();
    }
    return "";
  }
  std::string str(uint64_t h) { return rt->string_utf8(rt::Handle(h)); }
};

std::vector<uint32_t> words(int64_t v) { return {uint32_t(uint64_t(v)), uint32_t(uint64_t(v) >> 32)}; }
uint32_t fbits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

}  // namespace

TEST(dex_writer_output_is_valid) {
  std::vector<tool::ClassDef> classes;
  for (const char* s : {kT, kMyEx, kAnimals, kDog, kSpeaker, kCounter}) classes.push_back(tool::assemble_smali(s, "t.smali"));
  auto bytes = tool::write_dex(classes);
  auto dex = dex::DexFile::open(bytes, "t.dex");
  CHECK(dex->checksum_valid());
  CHECK(dex->signature_valid());
  CHECK_EQ(dex->num_class_defs(), uint32_t(6));
  CHECK(dex->find_class_def("LDog;").has_value());
  // Superclasses precede subclasses.
  CHECK(*dex->find_class_def("LAnimal;") < *dex->find_class_def("LDog;"));
  // Strings are sorted.
  for (uint32_t i = 1; i < dex->num_strings(); ++i) {
    CHECK(text::utf8_to_utf16(dex->string(i - 1)) < text::utf8_to_utf16(dex->string(i)));
  }
  // Every method body passes the structural verifier.
  for (uint32_t c = 0; c < dex->num_class_defs(); ++c) {
    auto cd = dex->class_data(dex->class_def(c));
    for (const auto* list : {&cd.direct_methods, &cd.virtual_methods}) {
      for (const auto& m : *list) {
        if (m.code_off) rt::prepare_code(*dex, dex->code_item(m.code_off));
      }
    }
  }
}

TEST(interpreter_arithmetic) {
  Vm vm = Vm::standard();
  CHECK_EQ(int32_t(vm.call("add", "(II)I", {40, 2})), 42);
  CHECK_EQ(int32_t(vm.call("add", "(II)I", {0x7fffffff, 1})), int32_t(0x80000000));  // wraps
  CHECK_EQ(int32_t(vm.call("arith", "(IIII)I", {6, 7, 8, 5})), 10);
  CHECK_EQ(int32_t(vm.call("rem", "(II)I", {uint32_t(-7), 3})), -1);
  CHECK_EQ(int32_t(vm.call("arith", "(IIII)I", {0x80000000u, 1, 0, uint32_t(-1)})), int32_t(0x80000000));
  auto l = words(3000000000LL);
  auto r = words(3);
  CHECK_EQ(int64_t(vm.call("lmul", "(JJ)J", {l[0], l[1], r[0], r[1]})), 9000000000LL);
  auto one = words(1);
  CHECK_EQ(int64_t(vm.call("lshl", "(JI)J", {one[0], one[1], 40})), int64_t(1) << 40);
  uint32_t q = uint32_t(vm.call("fdiv", "(FF)F", {fbits(1.0f), fbits(4.0f)}));
  float qf;
  std::memcpy(&qf, &q, 4);
  CHECK(qf == 0.25f);
  double big = 1e20;
  uint64_t bb;
  std::memcpy(&bb, &big, 8);
  CHECK_EQ(int32_t(vm.call("d2i", "(D)I", {uint32_t(bb), uint32_t(bb >> 32)})), 0x7fffffff);  // saturates
  CHECK_EQ(vm.thrown("arith", "(IIII)I", {1, 1, 1, 0}), std::string("java.lang.ArithmeticException"));
}

TEST(interpreter_control_flow) {
  Vm vm = Vm::standard();
  CHECK_EQ(int32_t(vm.call("sum", "(I)I", {100})), 5050);
  CHECK_EQ(int32_t(vm.call("fib", "(I)I", {20})), 6765);
  CHECK_EQ(int32_t(vm.call("sw", "(I)I", {5})), 10);
  CHECK_EQ(int32_t(vm.call("sw", "(I)I", {6})), 20);
  CHECK_EQ(int32_t(vm.call("sw", "(I)I", {7})), -1);
  CHECK_EQ(int32_t(vm.call("sp", "(I)I", {uint32_t(-100)})), 1);
  CHECK_EQ(int32_t(vm.call("sp", "(I)I", {1000})), 2);
  CHECK_EQ(int32_t(vm.call("sp", "(I)I", {5})), 0);
}

TEST(interpreter_arrays) {
  Vm vm = Vm::standard();
  CHECK_EQ(int32_t(vm.call("arrays", "()I")), 53);
  CHECK_EQ(int32_t(vm.call("filled", "()I")), 5);
  CHECK_EQ(vm.thrown("oob", "()I"), std::string("java.lang.ArrayIndexOutOfBoundsException"));
}

TEST(interpreter_exceptions) {
  Vm vm = Vm::standard();
  CHECK_EQ(int32_t(vm.call("safediv", "(II)I", {10, 2})), 5);
  CHECK_EQ(int32_t(vm.call("safediv", "(II)I", {1, 0})), -1);
  CHECK_EQ(vm.str(vm.call("thrower", "()Ljava/lang/String;")), std::string("boum"));
  try {
    vm.call("uncaught", "()V");
    CHECK(false);
  } catch (const rt::JavaThrow& t) {
    std::string d = vm.rt->describe_throwable(t.exception);
    CHECK(text::starts_with(d, "java.lang.IllegalStateException: état invalide"));
    CHECK(d.find("at T.uncaught") != std::string::npos);
  }
  CHECK_EQ(vm.thrown("npe", "()I"), std::string("java.lang.NullPointerException"));
}

TEST(interpreter_objects_and_dispatch) {
  Vm vm = Vm::standard();
  CHECK_EQ(int32_t(vm.call("dispatch", "()I")), 25);
  CHECK_EQ(int32_t(vm.call("next", "()I", {}, "LCounter;")), 101);
  CHECK_EQ(int32_t(vm.call("next", "()I", {}, "LCounter;")), 102);
  CHECK_EQ(vm.str(vm.call("name", "()Ljava/lang/String;", {}, "LCounter;")), std::string("compteur"));
  CHECK_EQ(int64_t(vm.call("big", "()J", {}, "LCounter;")), 0x123456789LL);
}

TEST(interpreter_strings) {
  Vm vm = Vm::standard();
  CHECK_EQ(int32_t(vm.call("hash", "()I")), 99162322);
  CHECK_EQ(vm.str(vm.call("concat", "()Ljava/lang/String;")), std::string("Bonjour42"));
}

TEST(interpreter_limits) {
  rt::RuntimeOptions opts;
  opts.max_depth = 200;
  opts.max_instructions = 100000;
  Vm vm = Vm::standard(opts);
  CHECK_EQ(vm.thrown("recurse", "()V"), std::string("java.lang.StackOverflowError"));
  vm.rt->reset_budget();
  CHECK_EQ(vm.thrown("spin", "()V"), std::string("java.lang.Error"));
}

// `exec --profile`: per-method counters, left untouched when profiling is off.
TEST(interpreter_profile_counters) {
  for (bool profile : {false, true}) {
    rt::RuntimeOptions opts;
    opts.profile = profile;
    Vm vm = Vm::standard(opts);
    CHECK_EQ(int32_t(vm.call("add", "(II)I", {2, 3})), 5);
    CHECK_EQ(int32_t(vm.call("add", "(II)I", {4, 5})), 9);
    rt::Method* add = vm.rt->find_class("LT;")->declared_method("add", "(II)I");
    CHECK_EQ(add->profile_calls, uint64_t(profile ? 2 : 0));
    CHECK_EQ(add->profile_instructions, uint64_t(profile ? 4 : 0));  // add-int, return
  }
}

TEST(interpreter_reports_unsupported_apis) {
  Vm vm = Vm::standard();
  CHECK_EQ(vm.thrown("sleepy", "()V"), std::string("java.lang.NoClassDefFoundError"));
  CHECK(vm.rt->unsupported_apis().count("Landroid/os/SystemClock;"));
  CHECK_EQ(vm.thrown("missing", "()I"), std::string("java.lang.NoSuchMethodError"));
  CHECK(vm.rt->unsupported_apis().count("Ljava/lang/String;->codePointCount()I"));
}

TEST(verifier_rejects_bad_register) {
  tool::ClassDef c;
  c.descriptor = "LBad;";
  c.super = "Ljava/lang/Object;";
  c.access = 1;
  tool::MethodDef m;
  m.name = "bad";
  m.signature = "()V";
  m.access = 0x9;
  m.has_code = true;
  m.registers = 1;
  m.insns = {0x5001, 0x000e};  // move v0, v5 ; return-void
  c.direct_methods.push_back(m);
  Vm vm({c});
  try {
    vm.call("bad", "()V", {}, "LBad;");
    CHECK(false);
  } catch (const rt::JavaThrow& t) {
    CHECK_EQ(vm.rt->class_of(t.exception)->java_name(), std::string("java.lang.VerifyError"));
  }
}

TEST(java_number_formatting) {
  CHECK_EQ(rt::java_fp_to_string(1.0, false), std::string("1.0"));
  CHECK_EQ(rt::java_fp_to_string(0.1f, true), std::string("0.1"));
  CHECK_EQ(rt::java_fp_to_string(1e7, false), std::string("1.0E7"));
  CHECK_EQ(rt::java_fp_to_string(123.456, false), std::string("123.456"));
  CHECK_EQ(rt::java_fp_to_string(1e-4, false), std::string("1.0E-4"));
  CHECK_EQ(rt::java_fp_to_string(-2.5, false), std::string("-2.5"));
  CHECK_EQ(rt::java_fp_to_string(100.0, false), std::string("100.0"));
  CHECK_EQ(rt::java_fp_to_string(0.001, false), std::string("0.001"));
}

// ADR-006: an abstract method that a class of the host DEX leaves unimplemented
// answers a neutral value; for application classes it stays AbstractMethodError.
TEST(host_classes_answer_neutral_values_for_unimplemented_methods) {
  const char* two = R"SMALI(
.class public interface abstract LTwo;
.super Ljava/lang/Object;
.method public abstract a()I
.end method
.method public abstract b()Ljava/lang/String;
.end method
)SMALI";
  const char* half = R"SMALI(
.class public LHalf;
.super Ljava/lang/Object;
.implements LTwo;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public a()I
    .registers 2
    const/4 v0, 0x7
    return v0
.end method
)SMALI";
  const char* use = R"SMALI(
.class public LUse;
.super Ljava/lang/Object;
.method public static run(LTwo;)I
    .registers 3
    invoke-interface {p0}, LTwo;->b()Ljava/lang/String;
    move-result-object v0
    if-nez v0, :bad
    invoke-interface {p0}, LTwo;->a()I
    move-result v1
    add-int/lit8 v1, v1, 0x64
    return v1
    :bad
    const/4 v1, -0x1
    return v1
.end method
)SMALI";
  // The same partial class in the application DEX.
  std::string app_half = std::string(half);
  app_half.replace(app_half.find("LHalf;"), 6, "LAppHalf;");
  auto host_dex = dex::DexFile::open(tool::write_dex({tool::assemble_smali(half, "h.smali")}), "host.dex");
  Vm vm({tool::assemble_smali(two, "t.smali"), tool::assemble_smali(use, "u.smali"),
         tool::assemble_smali(app_half, "a.smali")});
  vm.rt->add_host_dex(host_dex.get());
  auto make = [&](const char* cls) {
    rt::Class* c = vm.rt->find_class(cls);
    rt::Handle h = vm.rt->alloc(c);
    uint32_t args[1] = {h};
    vm.rt->invoke(c->declared_method("<init>", "()V"), args, 1);
    return h;
  };
  rt::Handle host_obj = make("LHalf;");
  CHECK(vm.rt->find_class("LHalf;")->host);
  CHECK_EQ(int32_t(vm.call("run", "(LTwo;)I", {host_obj}, "LUse;")), 107);
  rt::Handle app_obj = make("LAppHalf;");
  CHECK(!vm.rt->find_class("LAppHalf;")->host);
  std::string thrown;
  try {
    vm.call("run", "(LTwo;)I", {app_obj}, "LUse;");
  } catch (const rt::JavaThrow& t) {
    thrown = vm.rt->class_of(t.exception)->java_name();
  }
  CHECK_EQ(thrown, std::string("java.lang.AbstractMethodError"));
}

// ADR-006 J2: one class namespace in which the boot space wins; a class loader
// defines a class from one given application DEX file.
TEST(class_loaders_define_from_their_dex_and_the_boot_space_wins) {
  auto cls = [](const char* name) {
    std::string s = std::string(".class public ") + name + "\n.super Ljava/lang/Object;\n";
    return tool::assemble_smali(s, "c.smali");
  };
  auto boot = dex::DexFile::open(tool::write_dex({cls("LShared;"), cls("LBootOnly;")}), "boot.dex");
  auto second = dex::DexFile::open(tool::write_dex({cls("LApp2;")}), "classes2.dex");
  Vm vm({cls("LShared;"), cls("LApp1;")});
  vm.rt->add_boot_dex(boot.get());
  vm.rt->add_dex(second.get());
  const dex::DexFile* first = vm.dex.get();

  std::vector<const dex::DexFile*> app = vm.rt->app_dex_files();
  CHECK_EQ(app.size(), size_t(2));
  CHECK(app[0] == first && app[1] == second.get());
  // The boot space neither sees nor loads application classes.
  CHECK(vm.rt->lookup_boot_class("LApp1;") == nullptr);
  CHECK(vm.rt->lookup_boot_class("[[LApp1;") == nullptr);
  CHECK(vm.rt->loaded_class("LApp1;") == nullptr);
  rt::Class* boot_only = vm.rt->lookup_boot_class("LBootOnly;");
  CHECK(boot_only != nullptr && rt::Runtime::in_boot_space(boot_only));
  CHECK(vm.rt->lookup_boot_class("[I") != nullptr);
  // A class is defined from the DEX file asked for, and only from it.
  CHECK(vm.rt->define_class(second.get(), "LApp1;") == nullptr);
  rt::Class* app1 = vm.rt->define_class(first, "LApp1;");
  CHECK(app1 != nullptr && !rt::Runtime::in_boot_space(app1));
  CHECK(app1->dex_cache == vm.rt->find_dex_cache(first));
  CHECK(vm.rt->define_class(second.get(), "LApp1;") == app1);  // already defined: shared
  CHECK(!rt::Runtime::in_boot_space(vm.rt->find_class("[LApp1;")));
  // An application DEX file cannot replace a class of the boot space.
  rt::Class* shared = vm.rt->define_class(first, "LShared;");
  CHECK(shared != nullptr && rt::Runtime::in_boot_space(shared));
  CHECK(vm.rt->define_class(first, "Lbad") == nullptr);
  CHECK(vm.rt->find_dex_cache(second.get())->loader == rt::kNull);
}
