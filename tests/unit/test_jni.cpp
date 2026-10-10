// G1: JNI with the System V calling convention used by Android libraries.
//
// The native methods below are compiled as sysv_abi functions, exactly like
// code coming from an x86_64 Android .so, and registered with RegisterNatives.
// They must not use C++ exceptions (no unwind data for sysv_abi on MinGW).

#include <cstring>
#include <memory>
#include <string>

#include "dex-engine/dex_file.h"
#include "dex-engine/jni.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kJniTest = R"SMALI(
.class public LJniTest;
.super Ljava/lang/Object;
.field public counter:I

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static native add(II)I
.end method
.method public static native mix(IJFDIIIIDFJ)D
.end method
.method public native greet(Ljava/lang/String;)Ljava/lang/String;
.end method
.method public static native fail()V
.end method
.method public static native sum([I)I
.end method
.method public native bump()I
.end method
.method public static native callBoom()I
.end method
.method public static native unregistered()V
.end method

.method public static twice(I)I
    .registers 2
    mul-int/lit8 v0, p0, 0x2
    return v0
.end method

.method public static boom()V
    .registers 2
    new-instance v0, Ljava/lang/RuntimeException;
    const-string v1, "boum Java"
    invoke-direct {v0, v1}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/String;)V
    throw v0
.end method

.method public static catchFail()Ljava/lang/String;
    .registers 2
    :start
    invoke-static {}, LJniTest;->fail()V
    const-string v0, "pas d'exception"
    return-object v0
    :end
    .catch Ljava/lang/IllegalStateException; {:start .. :end} :handler
    :handler
    move-exception v0
    invoke-virtual {v0}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static sumOf123()I
    .registers 4
    const/4 v0, 0x3
    new-array v0, v0, [I
    fill-array-data v0, :data
    invoke-static {v0}, LJniTest;->sum([I)I
    move-result v1
    return v1
    :data
    .array-data 4
        0x1 0x2 0x3
    .end array-data
.end method
)SMALI";

JNI_ABI jint native_add(JNIEnv*, jclass, jint a, jint b) { return a + b; }

JNI_ABI jdouble native_mix(JNIEnv*, jclass, jint a, jlong b, jfloat c, jdouble d, jint e, jint f, jint g, jint h,
                           jdouble i, jfloat j, jlong k) {
  return double(a) + double(b) + double(c) + d + e + f + g + h + i + double(j) + double(k);
}

JNI_ABI jstring native_greet(JNIEnv* env, jobject self, jstring name) {
  const JNINativeInterface* f = env->functions;
  const char* utf = f->GetStringUTFChars(env, name, nullptr);
  jclass sbc = f->FindClass(env, "java/lang/StringBuilder");
  jmethodID ctor = f->GetMethodID(env, sbc, "<init>", "(Ljava/lang/String;)V");
  jobject sb = f->NewObject(env, sbc, ctor, f->NewStringUTF(env, "Bonjour "));
  jmethodID append_s = f->GetMethodID(env, sbc, "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
  jmethodID append_i = f->GetMethodID(env, sbc, "append", "(I)Ljava/lang/StringBuilder;");
  f->CallObjectMethod(env, sb, append_s, f->NewStringUTF(env, utf));  // MUTF-8 round trip
  f->CallObjectMethod(env, sb, append_s, f->NewStringUTF(env, " "));
  jclass self_class = f->GetObjectClass(env, self);
  jmethodID twice = f->GetStaticMethodID(env, self_class, "twice", "(I)I");
  jint t = f->CallStaticIntMethod(env, self_class, twice, 21);  // variadic call
  f->CallObjectMethod(env, sb, append_i, t);
  jmethodID to_string = f->GetMethodID(env, sbc, "toString", "()Ljava/lang/String;");
  jstring result = static_cast<jstring>(f->CallObjectMethod(env, sb, to_string));
  f->ReleaseStringUTFChars(env, name, utf);
  return result;
}

JNI_ABI void native_fail(JNIEnv* env, jclass) {
  jclass ise = env->functions->FindClass(env, "java/lang/IllegalStateException");
  env->functions->ThrowNew(env, ise, "depuis JNI");
}

JNI_ABI jint native_sum(JNIEnv* env, jclass, jintArray arr) {
  jsize n = env->functions->GetArrayLength(env, arr);
  jint* p = env->functions->GetIntArrayElements(env, arr, nullptr);
  jint s = 0;
  for (jsize i = 0; i < n; ++i) s += p[i];
  env->functions->ReleaseIntArrayElements(env, arr, p, JNI_ABORT);
  return s;
}

JNI_ABI jint native_bump(JNIEnv* env, jobject self) {
  const JNINativeInterface* f = env->functions;
  jclass cls = f->GetObjectClass(env, self);
  jfieldID fid = f->GetFieldID(env, cls, "counter", "I");
  jint v = f->GetIntField(env, self, fid) + 5;
  f->SetIntField(env, self, fid, v);
  return v;
}

JNI_ABI jint native_call_boom(JNIEnv* env, jclass cls) {
  const JNINativeInterface* f = env->functions;
  jmethodID boom = f->GetStaticMethodID(env, cls, "boom", "()V");
  f->CallStaticVoidMethod(env, cls, boom);
  if (f->ExceptionCheck(env)) {
    f->ExceptionClear(env);
    return 99;
  }
  return 0;
}

struct JniVm {
  std::unique_ptr<dex::DexFile> dex;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;
  JniVm() {
    dex = dex::DexFile::open(tool::write_dex({tool::assemble_smali(kJniTest, "jni.smali")}), "jni.dex");
    rt = std::make_unique<rt::Runtime>();
    bridge::install(rt->registry(), host);
    rt->add_dex(dex.get());
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
    JNIEnv* env = rt::jni_env(*rt);
    jclass cls = env->functions->FindClass(env, "JniTest");
    static const JNINativeMethod methods[] = {
        {"add", "(II)I", reinterpret_cast<void*>(native_add)},
        {"mix", "(IJFDIIIIDFJ)D", reinterpret_cast<void*>(native_mix)},
        {"greet", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(native_greet)},
        {"fail", "()V", reinterpret_cast<void*>(native_fail)},
        {"sum", "([I)I", reinterpret_cast<void*>(native_sum)},
        {"bump", "()I", reinterpret_cast<void*>(native_bump)},
        {"callBoom", "()I", reinterpret_cast<void*>(native_call_boom)},
    };
    registered = env->functions->RegisterNatives(env, cls, methods, 7);
  }
  ~JniVm() {
    scope.reset();
    rt.reset();
  }
  jint registered = JNI_ERR;
  uint64_t call(const char* name, const char* sig, std::vector<uint32_t> args = {}) {
    return rt->call_static("LJniTest;", name, sig, std::move(args));
  }
};

std::vector<uint32_t> w64(uint64_t v) { return {uint32_t(v), uint32_t(v >> 32)}; }
uint32_t fbits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
uint64_t dbits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

}  // namespace

TEST(jni_register_and_call_natives) {
  JniVm vm;
  CHECK_EQ(vm.registered, JNI_OK);
  CHECK_EQ(int32_t(vm.call("add", "(II)I", {40, 2})), 42);
  CHECK_EQ(int32_t(vm.call("sumOf123", "()I")), 6);
}

TEST(jni_stack_arguments_and_floats) {
  JniVm vm;
  std::vector<uint32_t> args = {1};
  for (uint32_t x : w64(10000000000ull)) args.push_back(x);
  args.push_back(fbits(0.5f));
  for (uint32_t x : w64(dbits(0.25))) args.push_back(x);
  for (uint32_t x : {2u, 3u, 4u, 5u}) args.push_back(x);
  for (uint32_t x : w64(dbits(0.125))) args.push_back(x);
  args.push_back(fbits(0.0625f));
  for (uint32_t x : w64(6)) args.push_back(x);
  uint64_t r = vm.call("mix", "(IJFDIIIIDFJ)D", args);
  double d;
  std::memcpy(&d, &r, 8);
  CHECK(d == 10000000021.9375);
}

TEST(jni_callbacks_strings_and_fields) {
  JniVm vm;
  rt::Class* cls = vm.rt->find_class("LJniTest;");
  rt::Handle obj = vm.rt->alloc(cls);
  uint32_t ctor_args[1] = {obj};
  vm.rt->invoke(cls->declared_method("<init>", "()V"), ctor_args, 1);
  rt::Handle name = vm.rt->new_string_utf8("Léo");
  uint64_t r = vm.rt->call_virtual(obj, "greet", "(Ljava/lang/String;)Ljava/lang/String;", {name});
  CHECK_EQ(vm.rt->string_utf8(rt::Handle(r)), std::string("Bonjour Léo 42"));
  CHECK_EQ(int32_t(vm.rt->call_virtual(obj, "bump", "()I")), 5);
  CHECK_EQ(int32_t(vm.rt->call_virtual(obj, "bump", "()I")), 10);
}

TEST(jni_exceptions_both_ways) {
  JniVm vm;
  CHECK_EQ(vm.rt->string_utf8(rt::Handle(vm.call("catchFail", "()Ljava/lang/String;"))), std::string("depuis JNI"));
  CHECK_EQ(int32_t(vm.call("callBoom", "()I")), 99);
  // A native method that was never registered fails explicitly.
  try {
    vm.call("unregistered", "()V");
    CHECK(false);
  } catch (const rt::JavaThrow& t) {
    CHECK_EQ(vm.rt->class_of(t.exception)->java_name(), std::string("java.lang.UnsatisfiedLinkError"));
  }
}

TEST(jni_references_and_vm) {
  JniVm vm;
  JNIEnv* env = rt::jni_env(*vm.rt);
  const JNINativeInterface* f = env->functions;
  CHECK_EQ(f->GetVersion(env), JNI_VERSION_1_6);
  jstring s = f->NewStringUTF(env, "global");
  jobject g = f->NewGlobalRef(env, s);
  CHECK_EQ(f->GetObjectRefType(env, g), JNIGlobalRefType);
  CHECK(f->IsSameObject(env, s, g));
  f->DeleteGlobalRef(env, g);
  JavaVM* jvm = nullptr;
  CHECK_EQ(f->GetJavaVM(env, &jvm), JNI_OK);
  JNIEnv* again = nullptr;
  CHECK_EQ(jvm->functions->GetEnv(jvm, reinterpret_cast<void**>(&again), JNI_VERSION_1_6), JNI_OK);
  CHECK(again == env);
  // Missing members are reported as pending exceptions, not crashes.
  jclass cls = f->FindClass(env, "JniTest");
  CHECK(f->GetMethodID(env, cls, "absente", "()V") == nullptr);
  CHECK(f->ExceptionCheck(env));
  f->ExceptionClear(env);
  CHECK(f->FindClass(env, "does/not/Exist") == nullptr);
  CHECK(f->ExceptionCheck(env));
  f->ExceptionClear(env);
}
