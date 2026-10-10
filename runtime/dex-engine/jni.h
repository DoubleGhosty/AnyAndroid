#pragma once

// JNI 1.6 interface of the AnyAndroid runtime.
//
// Android native libraries (x86_64 ELF) follow the System V AMD64 calling
// convention, not the Windows x64 one: every JNI function and every native
// method is therefore called with JNI_ABI (sysv_abi). Requires GCC/Clang.
//
// The function table layout is the standard JNI one (233 entries, see the
// static_asserts in jni.cpp); object references are opaque pointers that
// encode runtime handles.

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__) && defined(__x86_64__)
#define JNI_ABI __attribute__((sysv_abi))
#else
#error "La couche JNI d'AnyAndroid requiert GCC ou Clang en x86_64 (convention System V)."
#endif

extern "C" {

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

struct _jobject;
typedef _jobject* jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jarray;
typedef jobject jobjectArray;
typedef jobject jbooleanArray;
typedef jobject jbyteArray;
typedef jobject jcharArray;
typedef jobject jshortArray;
typedef jobject jintArray;
typedef jobject jlongArray;
typedef jobject jfloatArray;
typedef jobject jdoubleArray;
typedef jobject jthrowable;
typedef jobject jweak;

struct _jfieldID;
typedef _jfieldID* jfieldID;
struct _jmethodID;
typedef _jmethodID* jmethodID;

union jvalue {
  jboolean z;
  jbyte b;
  jchar c;
  jshort s;
  jint i;
  jlong j;
  jfloat f;
  jdouble d;
  jobject l;
};

enum jobjectRefType { JNIInvalidRefType = 0, JNILocalRefType = 1, JNIGlobalRefType = 2, JNIWeakGlobalRefType = 3 };

struct JNINativeMethod {
  const char* name;
  const char* signature;
  void* fnPtr;
};

#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_EDETACHED (-2)
#define JNI_EVERSION (-3)
#define JNI_ENOMEM (-4)
#define JNI_EEXIST (-5)
#define JNI_EINVAL (-6)
#define JNI_COMMIT 1
#define JNI_ABORT 2
#define JNI_VERSION_1_1 0x00010001
#define JNI_VERSION_1_2 0x00010002
#define JNI_VERSION_1_4 0x00010004
#define JNI_VERSION_1_6 0x00010006

typedef __builtin_sysv_va_list jni_va_list;

struct JNINativeInterface;
struct JNIInvokeInterface;

struct JNIEnv {
  const JNINativeInterface* functions;
};

struct JavaVM {
  const JNIInvokeInterface* functions;
};

struct JavaVMAttachArgs {
  jint version;
  const char* name;
  jobject group;
};

#define AA_JNI_CALL3(T, Name)                                                      \
  T(JNI_ABI* Call##Name##Method)(JNIEnv*, jobject, jmethodID, ...);                \
  T(JNI_ABI* Call##Name##MethodV)(JNIEnv*, jobject, jmethodID, jni_va_list);       \
  T(JNI_ABI* Call##Name##MethodA)(JNIEnv*, jobject, jmethodID, const jvalue*);
#define AA_JNI_CALLNV3(T, Name)                                                           \
  T(JNI_ABI* CallNonvirtual##Name##Method)(JNIEnv*, jobject, jclass, jmethodID, ...);     \
  T(JNI_ABI* CallNonvirtual##Name##MethodV)(JNIEnv*, jobject, jclass, jmethodID, jni_va_list); \
  T(JNI_ABI* CallNonvirtual##Name##MethodA)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*);
#define AA_JNI_CALLS3(T, Name)                                                         \
  T(JNI_ABI* CallStatic##Name##Method)(JNIEnv*, jclass, jmethodID, ...);               \
  T(JNI_ABI* CallStatic##Name##MethodV)(JNIEnv*, jclass, jmethodID, jni_va_list);      \
  T(JNI_ABI* CallStatic##Name##MethodA)(JNIEnv*, jclass, jmethodID, const jvalue*);

struct JNINativeInterface {
  void* reserved0;
  void* reserved1;
  void* reserved2;
  void* reserved3;

  jint(JNI_ABI* GetVersion)(JNIEnv*);
  jclass(JNI_ABI* DefineClass)(JNIEnv*, const char*, jobject, const jbyte*, jsize);
  jclass(JNI_ABI* FindClass)(JNIEnv*, const char*);
  jmethodID(JNI_ABI* FromReflectedMethod)(JNIEnv*, jobject);
  jfieldID(JNI_ABI* FromReflectedField)(JNIEnv*, jobject);
  jobject(JNI_ABI* ToReflectedMethod)(JNIEnv*, jclass, jmethodID, jboolean);
  jclass(JNI_ABI* GetSuperclass)(JNIEnv*, jclass);
  jboolean(JNI_ABI* IsAssignableFrom)(JNIEnv*, jclass, jclass);
  jobject(JNI_ABI* ToReflectedField)(JNIEnv*, jclass, jfieldID, jboolean);
  jint(JNI_ABI* Throw)(JNIEnv*, jthrowable);
  jint(JNI_ABI* ThrowNew)(JNIEnv*, jclass, const char*);
  jthrowable(JNI_ABI* ExceptionOccurred)(JNIEnv*);
  void(JNI_ABI* ExceptionDescribe)(JNIEnv*);
  void(JNI_ABI* ExceptionClear)(JNIEnv*);
  void(JNI_ABI* FatalError)(JNIEnv*, const char*);
  jint(JNI_ABI* PushLocalFrame)(JNIEnv*, jint);
  jobject(JNI_ABI* PopLocalFrame)(JNIEnv*, jobject);
  jobject(JNI_ABI* NewGlobalRef)(JNIEnv*, jobject);
  void(JNI_ABI* DeleteGlobalRef)(JNIEnv*, jobject);
  void(JNI_ABI* DeleteLocalRef)(JNIEnv*, jobject);
  jboolean(JNI_ABI* IsSameObject)(JNIEnv*, jobject, jobject);
  jobject(JNI_ABI* NewLocalRef)(JNIEnv*, jobject);
  jint(JNI_ABI* EnsureLocalCapacity)(JNIEnv*, jint);
  jobject(JNI_ABI* AllocObject)(JNIEnv*, jclass);
  jobject(JNI_ABI* NewObject)(JNIEnv*, jclass, jmethodID, ...);
  jobject(JNI_ABI* NewObjectV)(JNIEnv*, jclass, jmethodID, jni_va_list);
  jobject(JNI_ABI* NewObjectA)(JNIEnv*, jclass, jmethodID, const jvalue*);
  jclass(JNI_ABI* GetObjectClass)(JNIEnv*, jobject);
  jboolean(JNI_ABI* IsInstanceOf)(JNIEnv*, jobject, jclass);
  jmethodID(JNI_ABI* GetMethodID)(JNIEnv*, jclass, const char*, const char*);

  AA_JNI_CALL3(jobject, Object)
  AA_JNI_CALL3(jboolean, Boolean)
  AA_JNI_CALL3(jbyte, Byte)
  AA_JNI_CALL3(jchar, Char)
  AA_JNI_CALL3(jshort, Short)
  AA_JNI_CALL3(jint, Int)
  AA_JNI_CALL3(jlong, Long)
  AA_JNI_CALL3(jfloat, Float)
  AA_JNI_CALL3(jdouble, Double)
  AA_JNI_CALL3(void, Void)

  AA_JNI_CALLNV3(jobject, Object)
  AA_JNI_CALLNV3(jboolean, Boolean)
  AA_JNI_CALLNV3(jbyte, Byte)
  AA_JNI_CALLNV3(jchar, Char)
  AA_JNI_CALLNV3(jshort, Short)
  AA_JNI_CALLNV3(jint, Int)
  AA_JNI_CALLNV3(jlong, Long)
  AA_JNI_CALLNV3(jfloat, Float)
  AA_JNI_CALLNV3(jdouble, Double)
  AA_JNI_CALLNV3(void, Void)

  jfieldID(JNI_ABI* GetFieldID)(JNIEnv*, jclass, const char*, const char*);
  jobject(JNI_ABI* GetObjectField)(JNIEnv*, jobject, jfieldID);
  jboolean(JNI_ABI* GetBooleanField)(JNIEnv*, jobject, jfieldID);
  jbyte(JNI_ABI* GetByteField)(JNIEnv*, jobject, jfieldID);
  jchar(JNI_ABI* GetCharField)(JNIEnv*, jobject, jfieldID);
  jshort(JNI_ABI* GetShortField)(JNIEnv*, jobject, jfieldID);
  jint(JNI_ABI* GetIntField)(JNIEnv*, jobject, jfieldID);
  jlong(JNI_ABI* GetLongField)(JNIEnv*, jobject, jfieldID);
  jfloat(JNI_ABI* GetFloatField)(JNIEnv*, jobject, jfieldID);
  jdouble(JNI_ABI* GetDoubleField)(JNIEnv*, jobject, jfieldID);
  void(JNI_ABI* SetObjectField)(JNIEnv*, jobject, jfieldID, jobject);
  void(JNI_ABI* SetBooleanField)(JNIEnv*, jobject, jfieldID, jboolean);
  void(JNI_ABI* SetByteField)(JNIEnv*, jobject, jfieldID, jbyte);
  void(JNI_ABI* SetCharField)(JNIEnv*, jobject, jfieldID, jchar);
  void(JNI_ABI* SetShortField)(JNIEnv*, jobject, jfieldID, jshort);
  void(JNI_ABI* SetIntField)(JNIEnv*, jobject, jfieldID, jint);
  void(JNI_ABI* SetLongField)(JNIEnv*, jobject, jfieldID, jlong);
  void(JNI_ABI* SetFloatField)(JNIEnv*, jobject, jfieldID, jfloat);
  void(JNI_ABI* SetDoubleField)(JNIEnv*, jobject, jfieldID, jdouble);

  jmethodID(JNI_ABI* GetStaticMethodID)(JNIEnv*, jclass, const char*, const char*);

  AA_JNI_CALLS3(jobject, Object)
  AA_JNI_CALLS3(jboolean, Boolean)
  AA_JNI_CALLS3(jbyte, Byte)
  AA_JNI_CALLS3(jchar, Char)
  AA_JNI_CALLS3(jshort, Short)
  AA_JNI_CALLS3(jint, Int)
  AA_JNI_CALLS3(jlong, Long)
  AA_JNI_CALLS3(jfloat, Float)
  AA_JNI_CALLS3(jdouble, Double)
  AA_JNI_CALLS3(void, Void)

  jfieldID(JNI_ABI* GetStaticFieldID)(JNIEnv*, jclass, const char*, const char*);
  jobject(JNI_ABI* GetStaticObjectField)(JNIEnv*, jclass, jfieldID);
  jboolean(JNI_ABI* GetStaticBooleanField)(JNIEnv*, jclass, jfieldID);
  jbyte(JNI_ABI* GetStaticByteField)(JNIEnv*, jclass, jfieldID);
  jchar(JNI_ABI* GetStaticCharField)(JNIEnv*, jclass, jfieldID);
  jshort(JNI_ABI* GetStaticShortField)(JNIEnv*, jclass, jfieldID);
  jint(JNI_ABI* GetStaticIntField)(JNIEnv*, jclass, jfieldID);
  jlong(JNI_ABI* GetStaticLongField)(JNIEnv*, jclass, jfieldID);
  jfloat(JNI_ABI* GetStaticFloatField)(JNIEnv*, jclass, jfieldID);
  jdouble(JNI_ABI* GetStaticDoubleField)(JNIEnv*, jclass, jfieldID);
  void(JNI_ABI* SetStaticObjectField)(JNIEnv*, jclass, jfieldID, jobject);
  void(JNI_ABI* SetStaticBooleanField)(JNIEnv*, jclass, jfieldID, jboolean);
  void(JNI_ABI* SetStaticByteField)(JNIEnv*, jclass, jfieldID, jbyte);
  void(JNI_ABI* SetStaticCharField)(JNIEnv*, jclass, jfieldID, jchar);
  void(JNI_ABI* SetStaticShortField)(JNIEnv*, jclass, jfieldID, jshort);
  void(JNI_ABI* SetStaticIntField)(JNIEnv*, jclass, jfieldID, jint);
  void(JNI_ABI* SetStaticLongField)(JNIEnv*, jclass, jfieldID, jlong);
  void(JNI_ABI* SetStaticFloatField)(JNIEnv*, jclass, jfieldID, jfloat);
  void(JNI_ABI* SetStaticDoubleField)(JNIEnv*, jclass, jfieldID, jdouble);

  jstring(JNI_ABI* NewString)(JNIEnv*, const jchar*, jsize);
  jsize(JNI_ABI* GetStringLength)(JNIEnv*, jstring);
  const jchar*(JNI_ABI* GetStringChars)(JNIEnv*, jstring, jboolean*);
  void(JNI_ABI* ReleaseStringChars)(JNIEnv*, jstring, const jchar*);
  jstring(JNI_ABI* NewStringUTF)(JNIEnv*, const char*);
  jsize(JNI_ABI* GetStringUTFLength)(JNIEnv*, jstring);
  const char*(JNI_ABI* GetStringUTFChars)(JNIEnv*, jstring, jboolean*);
  void(JNI_ABI* ReleaseStringUTFChars)(JNIEnv*, jstring, const char*);
  jsize(JNI_ABI* GetArrayLength)(JNIEnv*, jarray);
  jobjectArray(JNI_ABI* NewObjectArray)(JNIEnv*, jsize, jclass, jobject);
  jobject(JNI_ABI* GetObjectArrayElement)(JNIEnv*, jobjectArray, jsize);
  void(JNI_ABI* SetObjectArrayElement)(JNIEnv*, jobjectArray, jsize, jobject);
  jbooleanArray(JNI_ABI* NewBooleanArray)(JNIEnv*, jsize);
  jbyteArray(JNI_ABI* NewByteArray)(JNIEnv*, jsize);
  jcharArray(JNI_ABI* NewCharArray)(JNIEnv*, jsize);
  jshortArray(JNI_ABI* NewShortArray)(JNIEnv*, jsize);
  jintArray(JNI_ABI* NewIntArray)(JNIEnv*, jsize);
  jlongArray(JNI_ABI* NewLongArray)(JNIEnv*, jsize);
  jfloatArray(JNI_ABI* NewFloatArray)(JNIEnv*, jsize);
  jdoubleArray(JNI_ABI* NewDoubleArray)(JNIEnv*, jsize);
  jboolean*(JNI_ABI* GetBooleanArrayElements)(JNIEnv*, jbooleanArray, jboolean*);
  jbyte*(JNI_ABI* GetByteArrayElements)(JNIEnv*, jbyteArray, jboolean*);
  jchar*(JNI_ABI* GetCharArrayElements)(JNIEnv*, jcharArray, jboolean*);
  jshort*(JNI_ABI* GetShortArrayElements)(JNIEnv*, jshortArray, jboolean*);
  jint*(JNI_ABI* GetIntArrayElements)(JNIEnv*, jintArray, jboolean*);
  jlong*(JNI_ABI* GetLongArrayElements)(JNIEnv*, jlongArray, jboolean*);
  jfloat*(JNI_ABI* GetFloatArrayElements)(JNIEnv*, jfloatArray, jboolean*);
  jdouble*(JNI_ABI* GetDoubleArrayElements)(JNIEnv*, jdoubleArray, jboolean*);
  void(JNI_ABI* ReleaseBooleanArrayElements)(JNIEnv*, jbooleanArray, jboolean*, jint);
  void(JNI_ABI* ReleaseByteArrayElements)(JNIEnv*, jbyteArray, jbyte*, jint);
  void(JNI_ABI* ReleaseCharArrayElements)(JNIEnv*, jcharArray, jchar*, jint);
  void(JNI_ABI* ReleaseShortArrayElements)(JNIEnv*, jshortArray, jshort*, jint);
  void(JNI_ABI* ReleaseIntArrayElements)(JNIEnv*, jintArray, jint*, jint);
  void(JNI_ABI* ReleaseLongArrayElements)(JNIEnv*, jlongArray, jlong*, jint);
  void(JNI_ABI* ReleaseFloatArrayElements)(JNIEnv*, jfloatArray, jfloat*, jint);
  void(JNI_ABI* ReleaseDoubleArrayElements)(JNIEnv*, jdoubleArray, jdouble*, jint);
  void(JNI_ABI* GetBooleanArrayRegion)(JNIEnv*, jbooleanArray, jsize, jsize, jboolean*);
  void(JNI_ABI* GetByteArrayRegion)(JNIEnv*, jbyteArray, jsize, jsize, jbyte*);
  void(JNI_ABI* GetCharArrayRegion)(JNIEnv*, jcharArray, jsize, jsize, jchar*);
  void(JNI_ABI* GetShortArrayRegion)(JNIEnv*, jshortArray, jsize, jsize, jshort*);
  void(JNI_ABI* GetIntArrayRegion)(JNIEnv*, jintArray, jsize, jsize, jint*);
  void(JNI_ABI* GetLongArrayRegion)(JNIEnv*, jlongArray, jsize, jsize, jlong*);
  void(JNI_ABI* GetFloatArrayRegion)(JNIEnv*, jfloatArray, jsize, jsize, jfloat*);
  void(JNI_ABI* GetDoubleArrayRegion)(JNIEnv*, jdoubleArray, jsize, jsize, jdouble*);
  void(JNI_ABI* SetBooleanArrayRegion)(JNIEnv*, jbooleanArray, jsize, jsize, const jboolean*);
  void(JNI_ABI* SetByteArrayRegion)(JNIEnv*, jbyteArray, jsize, jsize, const jbyte*);
  void(JNI_ABI* SetCharArrayRegion)(JNIEnv*, jcharArray, jsize, jsize, const jchar*);
  void(JNI_ABI* SetShortArrayRegion)(JNIEnv*, jshortArray, jsize, jsize, const jshort*);
  void(JNI_ABI* SetIntArrayRegion)(JNIEnv*, jintArray, jsize, jsize, const jint*);
  void(JNI_ABI* SetLongArrayRegion)(JNIEnv*, jlongArray, jsize, jsize, const jlong*);
  void(JNI_ABI* SetFloatArrayRegion)(JNIEnv*, jfloatArray, jsize, jsize, const jfloat*);
  void(JNI_ABI* SetDoubleArrayRegion)(JNIEnv*, jdoubleArray, jsize, jsize, const jdouble*);
  jint(JNI_ABI* RegisterNatives)(JNIEnv*, jclass, const JNINativeMethod*, jint);
  jint(JNI_ABI* UnregisterNatives)(JNIEnv*, jclass);
  jint(JNI_ABI* MonitorEnter)(JNIEnv*, jobject);
  jint(JNI_ABI* MonitorExit)(JNIEnv*, jobject);
  jint(JNI_ABI* GetJavaVM)(JNIEnv*, JavaVM**);
  void(JNI_ABI* GetStringRegion)(JNIEnv*, jstring, jsize, jsize, jchar*);
  void(JNI_ABI* GetStringUTFRegion)(JNIEnv*, jstring, jsize, jsize, char*);
  void*(JNI_ABI* GetPrimitiveArrayCritical)(JNIEnv*, jarray, jboolean*);
  void(JNI_ABI* ReleasePrimitiveArrayCritical)(JNIEnv*, jarray, void*, jint);
  const jchar*(JNI_ABI* GetStringCritical)(JNIEnv*, jstring, jboolean*);
  void(JNI_ABI* ReleaseStringCritical)(JNIEnv*, jstring, const jchar*);
  jweak(JNI_ABI* NewWeakGlobalRef)(JNIEnv*, jobject);
  void(JNI_ABI* DeleteWeakGlobalRef)(JNIEnv*, jweak);
  jboolean(JNI_ABI* ExceptionCheck)(JNIEnv*);
  jobject(JNI_ABI* NewDirectByteBuffer)(JNIEnv*, void*, jlong);
  void*(JNI_ABI* GetDirectBufferAddress)(JNIEnv*, jobject);
  jlong(JNI_ABI* GetDirectBufferCapacity)(JNIEnv*, jobject);
  jobjectRefType(JNI_ABI* GetObjectRefType)(JNIEnv*, jobject);
};

struct JNIInvokeInterface {
  void* reserved0;
  void* reserved1;
  void* reserved2;
  jint(JNI_ABI* DestroyJavaVM)(JavaVM*);
  jint(JNI_ABI* AttachCurrentThread)(JavaVM*, JNIEnv**, void*);
  jint(JNI_ABI* DetachCurrentThread)(JavaVM*);
  jint(JNI_ABI* GetEnv)(JavaVM*, void**, jint);
  jint(JNI_ABI* AttachCurrentThreadAsDaemon)(JavaVM*, JNIEnv**, void*);
};

#undef AA_JNI_CALL3
#undef AA_JNI_CALLNV3
#undef AA_JNI_CALLS3

}  // extern "C"

namespace aa::rt {
class Runtime;
struct VmThread;
// JNIEnv of the calling thread (attached to `rt`); valid inside a Scope.
JNIEnv* jni_env(Runtime& rt);
JavaVM* java_vm(Runtime& rt);
}  // namespace aa::rt
