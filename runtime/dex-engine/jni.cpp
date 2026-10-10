// JNI 1.6 for the AnyAndroid runtime (System V ABI, see jni.h).
//
// References: a jobject encodes a runtime handle. Local references are kept
// in VmThread::jni_locals (GC roots) and dropped when the native method
// returns; global references are runtime globals; weak global references are
// plain handles whose generation makes them invalid once collected.
//
// Exceptions never cross native frames: a Java exception raised inside a JNI
// function becomes the "pending exception" of the JNIEnv and is rethrown when
// the native method returns to the VM.
//
// Threads: native methods run with the GIL released (other Java threads keep
// running); each JNI function re-acquires it for its duration.

#include "dex-engine/jni.h"

#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <utility>

#include "common/log.h"
#include "common/text.h"
#include "dex-engine/runtime.h"

namespace aa::rt {

// Grants the JNI implementation access to runtime internals.
struct JniBridge {
  static VmThread* running(Runtime& rt) { return rt.running_; }
  static void acquire(Runtime& rt, VmThread* t) { rt.acquire_gil(t); }
  static void unlock(Runtime& rt, VmThread* t) {
    if (rt.running_ == t) rt.running_ = nullptr;
    t->gil.unlock();
  }
  static VmThread* attached(Runtime& rt) { return rt.attached_thread(); }
  static VmThread* attach(Runtime& rt, const std::string& name) { return rt.attach_current_thread(name); }
  static void detach(Runtime& rt) { rt.detach_current_thread(); }
  static std::vector<std::shared_ptr<void>>& state(Runtime& rt) { return rt.native_state_; }
  static void*& vm(Runtime& rt) { return rt.java_vm_; }
  static bool is_global(Runtime& rt, Handle h) { return rt.globals_.count(h) > 0; }
};

namespace {

struct JniEnvImpl {
  JNIEnv env;  // must stay first: JNIEnv* == JniEnvImpl*
  Runtime* rt = nullptr;
  VmThread* thread = nullptr;
  Handle pending = kNull;
  bool pending_kill = false;
  bool has_exit = false;
  int exit_code = 0;
  std::vector<size_t> frames;  // PushLocalFrame marks
};

struct JavaVMImpl {
  JavaVM vm;  // must stay first
  Runtime* rt = nullptr;
  std::unordered_set<Handle> weak_globals;
};

const JNINativeInterface& jni_table();
const JNIInvokeInterface& invoke_table();

// A jobject is the handle in the low 32 bits plus a tag telling global and
// weak global references apart from local ones (JNI GetObjectRefType).
constexpr uintptr_t kGlobalTag = uintptr_t(1) << 32;
constexpr uintptr_t kWeakTag = uintptr_t(1) << 33;

JniEnvImpl* impl(JNIEnv* env) { return reinterpret_cast<JniEnvImpl*>(env); }
Handle H(const void* o) { return Handle(reinterpret_cast<uintptr_t>(o) & 0xFFFFFFFFu); }
jobject as_ref(Handle h, uintptr_t tag = 0) { return h ? reinterpret_cast<jobject>(uintptr_t(h) | tag) : nullptr; }

JavaVMImpl* vm_of(Runtime& rt) {
  void*& slot = JniBridge::vm(rt);
  if (!slot) {
    auto vm = std::make_shared<JavaVMImpl>();
    vm->vm.functions = &invoke_table();
    vm->rt = &rt;
    slot = vm.get();
    JniBridge::state(rt).push_back(vm);
  }
  return static_cast<JavaVMImpl*>(slot);
}

JniEnvImpl* env_for(Runtime& rt, VmThread* t) {
  if (!t->jni_env) {
    auto e = std::make_shared<JniEnvImpl>();
    e->env.functions = &jni_table();
    e->rt = &rt;
    e->thread = t;
    t->jni_env = e.get();
    JniBridge::state(rt).push_back(e);
  }
  return static_cast<JniEnvImpl*>(t->jni_env);
}

// Holds the GIL while a JNI function runs on a thread that is in native code.
class JniAccess {
 public:
  explicit JniAccess(JniEnvImpl* e) : e_(e) {
    VmThread* t = e->thread;
    if (JniBridge::running(*e->rt) != t) {
      saved_sp_ = t->parked_sp;
      saved_in_vm_ = t->in_vm;
      JniBridge::acquire(*e->rt, t);
      acquired_ = true;
    }
  }
  ~JniAccess() {
    if (!acquired_) return;
    VmThread* t = e_->thread;
    t->parked_sp = saved_sp_;
    t->in_vm = saved_in_vm_;
    JniBridge::unlock(*e_->rt, t);
  }

 private:
  JniEnvImpl* e_;
  bool acquired_ = false;
  char* saved_sp_ = nullptr;
  bool saved_in_vm_ = false;
};

jobject local(JniEnvImpl& e, Handle h) {
  if (h == kNull) return nullptr;
  e.thread->jni_locals.push_back(h);
  return as_ref(h);
}

void set_pending(JniEnvImpl& e, Handle ex) {
  e.pending = ex;
  e.thread->jni_locals.push_back(ex);  // keep it alive until the native returns
}

// Every JNI entry point (System V ABI) only calls such guarded, noexcept,
// Windows-ABI helpers: GCC/MinGW cannot emit SEH unwind data for sysv_abi
// functions that need exception landing pads, and C++ exceptions must never
// cross native frames anyway.
template <typename R, typename F>
[[gnu::noinline]] R guarded(JNIEnv* raw, R fallback, F&& body) noexcept {
  JniEnvImpl* e = impl(raw);
  try {
    JniAccess access(e);
    try {
      return body(*e, *e->rt);
    } catch (const JavaThrow& t) {
      set_pending(*e, t.exception);
    } catch (const ThreadKill&) {
      e->pending_kill = true;
    } catch (const ExitRequest& x) {
      e->has_exit = true;
      e->exit_code = x.code;
    } catch (const std::exception& x) {
      log::error("jni", std::string("erreur interne : ") + x.what());
    }
  } catch (...) {
    log::error("jni", "erreur interne inattendue");
  }
  return fallback;
}

template <typename F>
void guarded_void(JNIEnv* raw, F&& body) noexcept {
  guarded(raw, 0, [&](JniEnvImpl& e, Runtime& rt) {
    body(e, rt);
    return 0;
  });
}

Class* class_ref(Runtime& rt, jclass c, const char* what) {
  Class* cls = rt.mirror_class(H(c));
  if (!cls) rt.throw_npe(std::string(what) + " : classe null ou invalide");
  return cls;
}

std::string mutf8_of(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) {
    if (c != 0 && c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

std::u16string from_mutf8(const char* s) {
  size_t n = std::strlen(s);
  return text::mutf8_to_utf16(std::span(reinterpret_cast<const uint8_t*>(s), n + 1));
}

std::string class_descriptor_of(const char* name) {
  std::string n = name;
  if (!n.empty() && n[0] == '[') return n;
  return "L" + n + ";";
}

Method* find_instance_method(Runtime& rt, Class* cls, const char* name, const char* sig) {
  if (std::strcmp(name, "<init>") == 0) return cls->declared_method(name, sig);
  for (Class* c = cls; c; c = c->super) {
    if (Method* m = c->declared_method(name, sig); m && !m->is_static()) return m;
  }
  return rt.resolve_virtual(cls, name, sig);
}

Field* find_field(Class* cls, const char* name, const char* sig, bool is_static) {
  for (Class* c = cls; c; c = c->super) {
    if (Field* f = c->declared_field(name, sig); f && f->is_static() == is_static) return f;
    if (is_static) {
      for (Class* i : c->interfaces) {
        if (Field* f = i->declared_field(name, sig); f && f->is_static()) return f;
      }
    }
  }
  return nullptr;
}

// --- Method calls ----------------------------------------------------------------------

enum class CallKind { kVirtual, kNonvirtual, kStatic };

// Reads the Java arguments of `m` from a va_list or a jvalue array and runs it.
template <typename Next>
uint64_t do_call(JniEnvImpl& e, Runtime& rt, CallKind kind, jobject target, jmethodID mid, Next next) {
  Method* m = reinterpret_cast<Method*>(mid);
  if (!m) rt.throw_npe("jmethodID null");
  std::vector<uint32_t> words;
  Handle receiver = kNull;
  if (kind != CallKind::kStatic) {
    receiver = H(target);
    rt.nonnull(receiver, "appel JNI sur un objet null");
    words.push_back(receiver);
  }
  std::vector<std::string> params;
  std::string ret;
  dex::split_signature(m->signature, params, ret);
  for (const auto& p : params) {
    uint64_t v = next(p[0]);
    if (p[0] == 'J' || p[0] == 'D') {
      words.push_back(uint32_t(v));
      words.push_back(uint32_t(v >> 32));
    } else {
      words.push_back(uint32_t(v));
    }
  }
  Method* callee = m;
  if (kind == CallKind::kVirtual && !(m->access & dex::kAccPrivate) && m->name != "<init>") {
    if (Method* v = rt.resolve_virtual(rt.class_of(receiver), m->name, m->signature)) callee = v;
  }
  if (kind == CallKind::kStatic) rt.ensure_initialized(m->owner);
  uint64_t raw = rt.invoke(callee, words.data(), uint32_t(words.size()));
  char rt_type = m->return_type();
  if (rt_type == 'L' || rt_type == '[') return reinterpret_cast<uint64_t>(local(e, Handle(raw)));
  return raw;
}

uint64_t float_bits_of_double(double d) {
  float f = float(d);
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

uint64_t double_bits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

// Variadic arguments (C default promotions apply: float -> double, small ints -> int).
[[gnu::noinline]] uint64_t call_va(JNIEnv* env, CallKind kind, jobject target, jmethodID mid, jni_va_list ap) noexcept {
  return guarded(env, uint64_t(0), [&](JniEnvImpl& e, Runtime& rt) {
    return do_call(e, rt, kind, target, mid, [&](char t) -> uint64_t {
      switch (t) {
        case 'J': return uint64_t(__builtin_va_arg(ap, jlong));
        case 'D': return double_bits(__builtin_va_arg(ap, double));
        case 'F': return float_bits_of_double(__builtin_va_arg(ap, double));
        case 'L':
        case '[': return H(__builtin_va_arg(ap, jobject));
        case 'Z': return uint32_t(__builtin_va_arg(ap, int) & 0xFF) ? 1 : 0;
        case 'C': return uint32_t(__builtin_va_arg(ap, int) & 0xFFFF);
        default: return uint32_t(__builtin_va_arg(ap, int));
      }
    });
  });
}

[[gnu::noinline]] uint64_t call_a(JNIEnv* env, CallKind kind, jobject target, jmethodID mid, const jvalue* args) noexcept {
  return guarded(env, uint64_t(0), [&](JniEnvImpl& e, Runtime& rt) {
    size_t i = 0;
    return do_call(e, rt, kind, target, mid, [&](char t) -> uint64_t {
      const jvalue& v = args[i++];
      switch (t) {
        case 'J': return uint64_t(v.j);
        case 'D': return double_bits(v.d);
        case 'F': {
          uint32_t b;
          std::memcpy(&b, &v.f, 4);
          return b;
        }
        case 'L':
        case '[': return H(v.l);
        case 'Z': return v.z ? 1 : 0;
        case 'B': return uint32_t(int32_t(v.b));
        case 'C': return v.c;
        case 'S': return uint32_t(int32_t(v.s));
        default: return uint32_t(v.i);
      }
    });
  });
}

// Conversions of the raw 64-bit result.
inline jobject to_object(uint64_t r) { return reinterpret_cast<jobject>(r); }
inline jboolean to_boolean(uint64_t r) { return jboolean(r & 0xFF); }
inline jbyte to_byte(uint64_t r) { return jbyte(r); }
inline jchar to_char(uint64_t r) { return jchar(r); }
inline jshort to_short(uint64_t r) { return jshort(r); }
inline jint to_int(uint64_t r) { return jint(r); }
inline jlong to_long(uint64_t r) { return jlong(r); }
inline jfloat to_float(uint64_t r) {
  uint32_t b = uint32_t(r);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
inline jdouble to_double(uint64_t r) {
  double d;
  std::memcpy(&d, &r, 8);
  return d;
}
inline void to_void(uint64_t) {}

#define AA_JNI_DEFINE_CALLS(T, Name, Conv)                                                                        \
  JNI_ABI T Call##Name##Method(JNIEnv* env, jobject obj, jmethodID mid, ...) {                                     \
    jni_va_list ap;                                                                                               \
    __builtin_sysv_va_start(ap, mid);                                                                             \
    uint64_t r = call_va(env, CallKind::kVirtual, obj, mid, ap);                                                  \
    __builtin_sysv_va_end(ap);                                                                                    \
    return Conv(r);                                                                                               \
  }                                                                                                               \
  JNI_ABI T Call##Name##MethodV(JNIEnv* env, jobject obj, jmethodID mid, jni_va_list ap) {                         \
    return Conv(call_va(env, CallKind::kVirtual, obj, mid, ap));                                                  \
  }                                                                                                               \
  JNI_ABI T Call##Name##MethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {                     \
    return Conv(call_a(env, CallKind::kVirtual, obj, mid, args));                                                 \
  }                                                                                                               \
  JNI_ABI T CallNonvirtual##Name##Method(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {                   \
    jni_va_list ap;                                                                                               \
    __builtin_sysv_va_start(ap, mid);                                                                             \
    uint64_t r = call_va(env, CallKind::kNonvirtual, obj, mid, ap);                                               \
    __builtin_sysv_va_end(ap);                                                                                    \
    return Conv(r);                                                                                               \
  }                                                                                                               \
  JNI_ABI T CallNonvirtual##Name##MethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid, jni_va_list ap) {       \
    return Conv(call_va(env, CallKind::kNonvirtual, obj, mid, ap));                                               \
  }                                                                                                               \
  JNI_ABI T CallNonvirtual##Name##MethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid, const jvalue* args) {   \
    return Conv(call_a(env, CallKind::kNonvirtual, obj, mid, args));                                              \
  }                                                                                                               \
  JNI_ABI T CallStatic##Name##Method(JNIEnv* env, jclass cls, jmethodID mid, ...) {                                \
    jni_va_list ap;                                                                                               \
    __builtin_sysv_va_start(ap, mid);                                                                             \
    uint64_t r = call_va(env, CallKind::kStatic, cls, mid, ap);                                                   \
    __builtin_sysv_va_end(ap);                                                                                    \
    return Conv(r);                                                                                               \
  }                                                                                                               \
  JNI_ABI T CallStatic##Name##MethodV(JNIEnv* env, jclass cls, jmethodID mid, jni_va_list ap) {                    \
    return Conv(call_va(env, CallKind::kStatic, cls, mid, ap));                                                   \
  }                                                                                                               \
  JNI_ABI T CallStatic##Name##MethodA(JNIEnv* env, jclass cls, jmethodID mid, const jvalue* args) {                \
    return Conv(call_a(env, CallKind::kStatic, cls, mid, args));                                                  \
  }

AA_JNI_DEFINE_CALLS(jobject, Object, to_object)
AA_JNI_DEFINE_CALLS(jboolean, Boolean, to_boolean)
AA_JNI_DEFINE_CALLS(jbyte, Byte, to_byte)
AA_JNI_DEFINE_CALLS(jchar, Char, to_char)
AA_JNI_DEFINE_CALLS(jshort, Short, to_short)
AA_JNI_DEFINE_CALLS(jint, Int, to_int)
AA_JNI_DEFINE_CALLS(jlong, Long, to_long)
AA_JNI_DEFINE_CALLS(jfloat, Float, to_float)
AA_JNI_DEFINE_CALLS(jdouble, Double, to_double)
AA_JNI_DEFINE_CALLS(void, Void, to_void)

// --- Fields ----------------------------------------------------------------------------------

uint32_t* field_slot(Runtime& rt, jobject obj, jfieldID fid, bool is_static) {
  Field* f = reinterpret_cast<Field*>(fid);
  if (!f) rt.throw_npe("jfieldID null");
  if (is_static) {
    rt.ensure_initialized(f->owner);
    return &f->owner->statics.at(f->slot);
  }
  Object& o = rt.nonnull(H(obj), "accès JNI à un champ d'un objet null");
  if (!rt.is_assignable(o.klass, f->owner) || f->slot + (f->wide() ? 2u : 1u) > o.fields.size()) {
    rt.throw_new("Ljava/lang/IncompatibleClassChangeError;", "champ " + f->name + " absent de " + o.klass->java_name());
  }
  return &o.fields[f->slot];
}

uint64_t get_field(JNIEnv* env, jobject obj, jfieldID fid, bool is_static) noexcept {
  return guarded(env, uint64_t(0), [&](JniEnvImpl& e, Runtime& rt) -> uint64_t {
    uint32_t* s = field_slot(rt, obj, fid, is_static);
    Field* f = reinterpret_cast<Field*>(fid);
    if (f->is_reference()) return reinterpret_cast<uint64_t>(local(e, s[0]));
    return f->wide() ? uint64_t(s[0]) | (uint64_t(s[1]) << 32) : s[0];
  });
}

void set_field(JNIEnv* env, jobject obj, jfieldID fid, bool is_static, uint64_t v) noexcept {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    uint32_t* s = field_slot(rt, obj, fid, is_static);
    Field* f = reinterpret_cast<Field*>(fid);
    if (f->is_reference()) {
      Handle h = H(reinterpret_cast<void*>(v));
      if (h != kNull && !rt.instance_of(h, rt.find_class(f->type))) {
        rt.throw_new("Ljava/lang/IllegalArgumentException;", "valeur incompatible avec le champ " + f->name);
      }
      s[0] = h;
    } else if (f->wide()) {
      s[0] = uint32_t(v);
      s[1] = uint32_t(v >> 32);
    } else {
      s[0] = uint32_t(v);
    }
  });
}

#define AA_JNI_DEFINE_FIELDS(T, Name, Conv, ToRaw)                                                                 \
  JNI_ABI T Get##Name##Field(JNIEnv* env, jobject obj, jfieldID fid) { return Conv(get_field(env, obj, fid, false)); } \
  JNI_ABI void Set##Name##Field(JNIEnv* env, jobject obj, jfieldID fid, T v) { set_field(env, obj, fid, false, ToRaw(v)); } \
  JNI_ABI T GetStatic##Name##Field(JNIEnv* env, jclass, jfieldID fid) { return Conv(get_field(env, nullptr, fid, true)); } \
  JNI_ABI void SetStatic##Name##Field(JNIEnv* env, jclass, jfieldID fid, T v) {                                    \
    set_field(env, nullptr, fid, true, ToRaw(v));                                                                  \
  }

inline uint64_t raw_object(jobject v) { return reinterpret_cast<uint64_t>(v); }
inline uint64_t raw_int(int64_t v) { return uint64_t(v); }
inline uint64_t raw_bool(jboolean v) { return v ? 1 : 0; }
inline uint64_t raw_char(jchar v) { return v; }
inline uint64_t raw_float(jfloat v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  return b;
}
inline uint64_t raw_double(jdouble v) { return double_bits(v); }
inline uint64_t raw_small(int32_t v) { return uint32_t(v); }

AA_JNI_DEFINE_FIELDS(jobject, Object, to_object, raw_object)
AA_JNI_DEFINE_FIELDS(jboolean, Boolean, to_boolean, raw_bool)
AA_JNI_DEFINE_FIELDS(jbyte, Byte, to_byte, raw_small)
AA_JNI_DEFINE_FIELDS(jchar, Char, to_char, raw_char)
AA_JNI_DEFINE_FIELDS(jshort, Short, to_short, raw_small)
AA_JNI_DEFINE_FIELDS(jint, Int, to_int, raw_small)
AA_JNI_DEFINE_FIELDS(jlong, Long, to_long, raw_int)
AA_JNI_DEFINE_FIELDS(jfloat, Float, to_float, raw_float)
AA_JNI_DEFINE_FIELDS(jdouble, Double, to_double, raw_double)

// --- Arrays -----------------------------------------------------------------------------------

template <char P>
JNI_ABI jobject new_primitive_array(JNIEnv* env, jsize n) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    return local(e, rt.new_array(std::string("[") + P, n));
  });
}

template <char P, typename T>
JNI_ABI T* array_elements(JNIEnv* env, jobject arr, jboolean* is_copy) {
  return guarded(env, static_cast<T*>(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    ArrayObject& a = rt.array(H(arr), "tableau JNI");
    if (a.klass->component->primitive != P) rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de tableau JNI incorrect");
    if (is_copy) *is_copy = JNI_FALSE;  // objects never move: direct pointer
    return reinterpret_cast<T*>(a.data.data());
  });
}

template <typename T>
JNI_ABI void release_elements(JNIEnv*, jobject, T*, jint) {}

template <char P, typename T>
JNI_ABI void get_region(JNIEnv* env, jobject arr, jsize start, jsize len, T* buf) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    ArrayObject& a = rt.array(H(arr), "tableau JNI");
    if (a.klass->component->primitive != P) rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de tableau JNI incorrect");
    if (start < 0 || len < 0 || int64_t(start) + len > a.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "région " + std::to_string(start) + "+" + std::to_string(len));
    }
    std::memcpy(buf, a.data.data() + size_t(start) * sizeof(T), size_t(len) * sizeof(T));
  });
}

template <char P, typename T>
JNI_ABI void set_region(JNIEnv* env, jobject arr, jsize start, jsize len, const T* buf) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    ArrayObject& a = rt.array(H(arr), "tableau JNI");
    if (a.klass->component->primitive != P) rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de tableau JNI incorrect");
    if (start < 0 || len < 0 || int64_t(start) + len > a.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "région " + std::to_string(start) + "+" + std::to_string(len));
    }
    std::memcpy(a.data.data() + size_t(start) * sizeof(T), buf, size_t(len) * sizeof(T));
  });
}

// --- The rest of the interface ---------------------------------------------------------------

JNI_ABI jint GetVersion(JNIEnv*) { return JNI_VERSION_1_6; }

JNI_ABI jclass DefineClass(JNIEnv* env, const char*, jobject, const jbyte*, jsize) {
  return guarded(env, jclass(nullptr), [&](JniEnvImpl&, Runtime& rt) -> jclass {
    rt.throw_new("Ljava/lang/UnsupportedOperationException;", "[AnyAndroid] DefineClass non pris en charge (pas de chargement de bytecode JVM)");
  });
}

JNI_ABI jclass FindClass(JNIEnv* env, const char* name) {
  return guarded(env, jclass(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    if (!name) rt.throw_npe("FindClass(null)");
    Class* cls = rt.lookup_class(class_descriptor_of(name));
    if (!cls) rt.throw_new("Ljava/lang/NoClassDefFoundError;", name);
    return local(e, rt.class_mirror(cls));
  });
}

JNI_ABI jmethodID FromReflectedMethod(JNIEnv* env, jobject method) {
  return guarded(env, jmethodID(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    auto* p = rt.peer<MethodPeer>(H(method));
    if (!p) rt.throw_new("Ljava/lang/IllegalArgumentException;", "objet Method attendu");
    return reinterpret_cast<jmethodID>(p->method);
  });
}

JNI_ABI jfieldID FromReflectedField(JNIEnv* env, jobject field) {
  return guarded(env, jfieldID(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    auto* p = rt.peer<FieldPeer>(H(field));
    if (!p) rt.throw_new("Ljava/lang/IllegalArgumentException;", "objet Field attendu");
    return reinterpret_cast<jfieldID>(p->field);
  });
}

JNI_ABI jobject ToReflectedMethod(JNIEnv* env, jclass, jmethodID mid, jboolean) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    Method* m = reinterpret_cast<Method*>(mid);
    if (!m) rt.throw_npe("jmethodID null");
    Handle h = rt.alloc(rt.find_class(m->name == "<init>" ? "Ljava/lang/reflect/Constructor;" : "Ljava/lang/reflect/Method;"));
    auto p = std::make_unique<MethodPeer>();
    p->method = m;
    rt.object(h)->peer = std::move(p);
    return local(e, h);
  });
}

JNI_ABI jobject ToReflectedField(JNIEnv* env, jclass, jfieldID fid, jboolean) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    Field* f = reinterpret_cast<Field*>(fid);
    if (!f) rt.throw_npe("jfieldID null");
    Handle h = rt.alloc(rt.find_class("Ljava/lang/reflect/Field;"));
    auto p = std::make_unique<FieldPeer>();
    p->field = f;
    rt.object(h)->peer = std::move(p);
    return local(e, h);
  });
}

JNI_ABI jclass GetSuperclass(JNIEnv* env, jclass c) {
  return guarded(env, jclass(nullptr), [&](JniEnvImpl& e, Runtime& rt) -> jclass {
    Class* cls = class_ref(rt, c, "GetSuperclass");
    if (cls->is_interface() || !cls->super) return nullptr;
    return local(e, rt.class_mirror(cls->super));
  });
}

JNI_ABI jboolean IsAssignableFrom(JNIEnv* env, jclass a, jclass b) {
  return guarded(env, jboolean(JNI_FALSE), [&](JniEnvImpl&, Runtime& rt) {
    return jboolean(rt.is_assignable(class_ref(rt, a, "IsAssignableFrom"), class_ref(rt, b, "IsAssignableFrom")));
  });
}

JNI_ABI jint Throw(JNIEnv* env, jthrowable ex) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl& e, Runtime& rt) {
    if (!rt.instance_of(H(ex), rt.find_class("Ljava/lang/Throwable;"))) return jint(JNI_ERR);
    set_pending(e, H(ex));
    return jint(JNI_OK);
  });
}

JNI_ABI jint ThrowNew(JNIEnv* env, jclass c, const char* message) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl& e, Runtime& rt) {
    Class* cls = class_ref(rt, c, "ThrowNew");
    Handle ex = rt.make_throwable(cls, message ? from_mutf8(message) : u"", message != nullptr);
    set_pending(e, ex);
    return jint(JNI_OK);
  });
}

JNI_ABI jthrowable ExceptionOccurred(JNIEnv* env) {
  return guarded(env, jthrowable(nullptr), [&](JniEnvImpl& e, Runtime&) { return local(e, e.pending); });
}

JNI_ABI void ExceptionDescribe(JNIEnv* env) {
  guarded_void(env, [&](JniEnvImpl& e, Runtime& rt) {
    if (e.pending) log::warn("jni", "exception en attente :\n" + rt.describe_throwable(e.pending));
    e.pending = kNull;
  });
}

JNI_ABI void ExceptionClear(JNIEnv* env) { impl(env)->pending = kNull; }
JNI_ABI jboolean ExceptionCheck(JNIEnv* env) { return impl(env)->pending != kNull ? JNI_TRUE : JNI_FALSE; }

[[gnu::noinline]] void fatal_error(const char* msg) noexcept {
  try {
    log::error("jni", std::string("FatalError : ") + (msg ? msg : "(sans message)"));
  } catch (...) {
  }
  std::abort();
}

JNI_ABI void FatalError(JNIEnv*, const char* msg) { fatal_error(msg); }

JNI_ABI jint PushLocalFrame(JNIEnv* env, jint) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl& e, Runtime&) {
    e.frames.push_back(e.thread->jni_locals.size());
    return jint(JNI_OK);
  });
}

JNI_ABI jobject PopLocalFrame(JNIEnv* env, jobject result) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime&) {
    Handle keep = H(result);
    if (!e.frames.empty()) {
      size_t mark = e.frames.back();
      e.frames.pop_back();
      if (mark <= e.thread->jni_locals.size()) e.thread->jni_locals.resize(mark);
    }
    return local(e, keep);
  });
}

JNI_ABI jobject NewGlobalRef(JNIEnv* env, jobject obj) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl&, Runtime& rt) -> jobject {
    Handle h = H(obj);
    if (!rt.is_valid_handle(h)) return nullptr;
    rt.add_global(h);
    return as_ref(h, kGlobalTag);
  });
}

JNI_ABI void DeleteGlobalRef(JNIEnv* env, jobject obj) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    if (obj) rt.remove_global(H(obj));
  });
}

JNI_ABI void DeleteLocalRef(JNIEnv* env, jobject obj) {
  guarded_void(env, [&](JniEnvImpl& e, Runtime&) {
    auto& locals = e.thread->jni_locals;
    Handle h = H(obj);
    for (size_t i = locals.size(); i-- > 0;) {
      if (locals[i] == h) {
        locals.erase(locals.begin() + long(i));
        return;
      }
    }
  });
}

JNI_ABI jboolean IsSameObject(JNIEnv* env, jobject a, jobject b) {
  return guarded(env, jboolean(JNI_FALSE), [&](JniEnvImpl&, Runtime& rt) {
    auto norm = [&](jobject o) { return rt.is_valid_handle(H(o)) ? H(o) : kNull; };
    return jboolean(norm(a) == norm(b));
  });
}

JNI_ABI jobject NewLocalRef(JNIEnv* env, jobject obj) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    return local(e, rt.is_valid_handle(H(obj)) ? H(obj) : kNull);
  });
}

JNI_ABI jint EnsureLocalCapacity(JNIEnv*, jint) { return JNI_OK; }

JNI_ABI jobject AllocObject(JNIEnv* env, jclass c) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    Class* cls = class_ref(rt, c, "AllocObject");
    if (cls->is_abstract() || cls->is_interface()) rt.throw_new("Ljava/lang/InstantiationException;", cls->java_name());
    rt.ensure_initialized(cls);
    return local(e, rt.alloc(cls));
  });
}


JNI_ABI jobject NewObject(JNIEnv* env, jclass c, jmethodID ctor, ...) {
  jobject obj = AllocObject(env, c);
  if (!obj) return nullptr;
  jni_va_list ap;
  __builtin_sysv_va_start(ap, ctor);
  call_va(env, CallKind::kNonvirtual, obj, ctor, ap);
  __builtin_sysv_va_end(ap);
  return impl(env)->pending ? nullptr : obj;
}

JNI_ABI jobject NewObjectV(JNIEnv* env, jclass c, jmethodID ctor, jni_va_list ap) {
  jobject obj = AllocObject(env, c);
  if (!obj) return nullptr;
  call_va(env, CallKind::kNonvirtual, obj, ctor, ap);
  return impl(env)->pending ? nullptr : obj;
}

JNI_ABI jobject NewObjectA(JNIEnv* env, jclass c, jmethodID ctor, const jvalue* args) {
  jobject obj = AllocObject(env, c);
  if (!obj) return nullptr;
  call_a(env, CallKind::kNonvirtual, obj, ctor, args);
  return impl(env)->pending ? nullptr : obj;
}

JNI_ABI jclass GetObjectClass(JNIEnv* env, jobject obj) {
  return guarded(env, jclass(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    return local(e, rt.class_mirror(rt.nonnull(H(obj), "GetObjectClass(null)").klass));
  });
}

JNI_ABI jboolean IsInstanceOf(JNIEnv* env, jobject obj, jclass c) {
  return guarded(env, jboolean(JNI_FALSE), [&](JniEnvImpl&, Runtime& rt) {
    if (!obj) return jboolean(JNI_TRUE);  // null can be cast to any class
    return jboolean(rt.instance_of(H(obj), class_ref(rt, c, "IsInstanceOf")));
  });
}

JNI_ABI jmethodID GetMethodID(JNIEnv* env, jclass c, const char* name, const char* sig) {
  return guarded(env, jmethodID(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    Class* cls = class_ref(rt, c, "GetMethodID");
    rt.ensure_initialized(cls);
    Method* m = find_instance_method(rt, cls, name, sig);
    if (!m) rt.throw_new("Ljava/lang/NoSuchMethodError;", "no non-static method \"" + cls->java_name() + "." + name + sig + "\"");
    return reinterpret_cast<jmethodID>(m);
  });
}

JNI_ABI jmethodID GetStaticMethodID(JNIEnv* env, jclass c, const char* name, const char* sig) {
  return guarded(env, jmethodID(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    Class* cls = class_ref(rt, c, "GetStaticMethodID");
    rt.ensure_initialized(cls);
    Method* m = rt.find_static(cls, name, sig);
    if (!m) rt.throw_new("Ljava/lang/NoSuchMethodError;", "no static method \"" + cls->java_name() + "." + name + sig + "\"");
    return reinterpret_cast<jmethodID>(m);
  });
}

JNI_ABI jfieldID GetFieldID(JNIEnv* env, jclass c, const char* name, const char* sig) {
  return guarded(env, jfieldID(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    Class* cls = class_ref(rt, c, "GetFieldID");
    rt.ensure_initialized(cls);
    Field* f = find_field(cls, name, sig, false);
    if (!f) rt.throw_new("Ljava/lang/NoSuchFieldError;", "no \"" + std::string(sig) + "\" field \"" + name + "\" in " + cls->java_name());
    return reinterpret_cast<jfieldID>(f);
  });
}

JNI_ABI jfieldID GetStaticFieldID(JNIEnv* env, jclass c, const char* name, const char* sig) {
  return guarded(env, jfieldID(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    Class* cls = class_ref(rt, c, "GetStaticFieldID");
    rt.ensure_initialized(cls);
    Field* f = find_field(cls, name, sig, true);
    if (!f) rt.throw_new("Ljava/lang/NoSuchFieldError;", "no static \"" + std::string(sig) + "\" field \"" + name + "\" in " + cls->java_name());
    return reinterpret_cast<jfieldID>(f);
  });
}

JNI_ABI jstring NewString(JNIEnv* env, const jchar* chars, jsize len) {
  return guarded(env, jstring(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    if (len < 0) rt.throw_new("Ljava/lang/NegativeArraySizeException;", std::to_string(len));
    return local(e, rt.new_string(std::u16string(reinterpret_cast<const char16_t*>(chars), size_t(len))));
  });
}

std::u16string string_of(Runtime& rt, jstring s) {
  StringObject* so = rt.as_string(H(s));
  if (!so) rt.throw_npe("chaîne JNI null ou invalide");
  return so->value;
}

JNI_ABI jsize GetStringLength(JNIEnv* env, jstring s) {
  return guarded(env, jsize(0), [&](JniEnvImpl&, Runtime& rt) { return jsize(string_of(rt, s).size()); });
}

JNI_ABI const jchar* GetStringChars(JNIEnv* env, jstring s, jboolean* is_copy) {
  return guarded(env, static_cast<const jchar*>(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    std::u16string v = string_of(rt, s);
    auto* buf = new jchar[v.size() + 1];
    std::memcpy(buf, v.data(), v.size() * 2);
    buf[v.size()] = 0;
    if (is_copy) *is_copy = JNI_TRUE;
    return static_cast<const jchar*>(buf);
  });
}

JNI_ABI void ReleaseStringChars(JNIEnv*, jstring, const jchar* chars) { delete[] chars; }

JNI_ABI jstring NewStringUTF(JNIEnv* env, const char* utf) {
  return guarded(env, jstring(nullptr), [&](JniEnvImpl& e, Runtime& rt) -> jstring {
    if (!utf) return nullptr;
    return local(e, rt.new_string(from_mutf8(utf)));
  });
}

JNI_ABI jsize GetStringUTFLength(JNIEnv* env, jstring s) {
  return guarded(env, jsize(0), [&](JniEnvImpl&, Runtime& rt) { return jsize(mutf8_of(string_of(rt, s)).size()); });
}

JNI_ABI const char* GetStringUTFChars(JNIEnv* env, jstring s, jboolean* is_copy) {
  return guarded(env, static_cast<const char*>(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    std::string m = mutf8_of(string_of(rt, s));
    auto* buf = new char[m.size() + 1];
    std::memcpy(buf, m.data(), m.size() + 1);
    if (is_copy) *is_copy = JNI_TRUE;
    return static_cast<const char*>(buf);
  });
}

JNI_ABI void ReleaseStringUTFChars(JNIEnv*, jstring, const char* chars) { delete[] chars; }

JNI_ABI jsize GetArrayLength(JNIEnv* env, jarray arr) {
  return guarded(env, jsize(0), [&](JniEnvImpl&, Runtime& rt) { return jsize(rt.array(H(arr), "GetArrayLength").length); });
}

JNI_ABI jobjectArray NewObjectArray(JNIEnv* env, jsize n, jclass element, jobject init) {
  return guarded(env, jobjectArray(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    Class* cls = class_ref(rt, element, "NewObjectArray");
    Handle arr = rt.new_array("[" + cls->descriptor, n);
    if (Handle v = H(init)) {
      ArrayObject& a = rt.array(arr, "NewObjectArray");
      for (jsize i = 0; i < n; ++i) std::memcpy(&a.data[size_t(i) * 4], &v, 4);
    }
    return local(e, arr);
  });
}

JNI_ABI jobject GetObjectArrayElement(JNIEnv* env, jobjectArray arr, jsize i) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl& e, Runtime& rt) {
    ArrayObject& a = rt.array(H(arr), "GetObjectArrayElement");
    if (a.klass->component->primitive) rt.throw_new("Ljava/lang/IllegalArgumentException;", "tableau d'objets attendu");
    if (i < 0 || uint32_t(i) >= a.length) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(i));
    Handle h;
    std::memcpy(&h, &a.data[size_t(i) * 4], 4);
    return local(e, h);
  });
}

JNI_ABI void SetObjectArrayElement(JNIEnv* env, jobjectArray arr, jsize i, jobject v) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    ArrayObject& a = rt.array(H(arr), "SetObjectArrayElement");
    if (a.klass->component->primitive) rt.throw_new("Ljava/lang/IllegalArgumentException;", "tableau d'objets attendu");
    if (i < 0 || uint32_t(i) >= a.length) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(i));
    Handle h = H(v);
    if (h != kNull && !rt.is_assignable(rt.class_of(h), a.klass->component)) {
      rt.throw_new("Ljava/lang/ArrayStoreException;", rt.class_of(h)->java_name());
    }
    std::memcpy(&a.data[size_t(i) * 4], &h, 4);
  });
}

JNI_ABI jint RegisterNatives(JNIEnv* env, jclass c, const JNINativeMethod* methods, jint n) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl&, Runtime& rt) {
    Class* cls = class_ref(rt, c, "RegisterNatives");
    for (jint i = 0; i < n; ++i) {
      Method* m = cls->declared_method(methods[i].name, methods[i].signature);
      if (!m || !(m->access & dex::kAccNative)) {
        rt.throw_new("Ljava/lang/NoSuchMethodError;", "no native method \"" + cls->java_name() + "." + methods[i].name +
                                                          methods[i].signature + "\"");
      }
      m->jni_fn = methods[i].fnPtr;
    }
    return jint(JNI_OK);
  });
}

JNI_ABI jint UnregisterNatives(JNIEnv* env, jclass c) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl&, Runtime& rt) {
    for (auto& m : class_ref(rt, c, "UnregisterNatives")->methods) m->jni_fn = nullptr;
    return jint(JNI_OK);
  });
}

JNI_ABI jint MonitorEnter(JNIEnv* env, jobject obj) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl&, Runtime& rt) {
    rt.monitor_enter(H(obj));
    return jint(JNI_OK);
  });
}

JNI_ABI jint MonitorExit(JNIEnv* env, jobject obj) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl&, Runtime& rt) {
    rt.monitor_exit(H(obj));
    return jint(JNI_OK);
  });
}

JNI_ABI jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
  return guarded(env, jint(JNI_ERR), [&](JniEnvImpl& e, Runtime& rt) {
    *vm = &vm_of(rt)->vm;
    return jint(JNI_OK);
  });
}

JNI_ABI void GetStringRegion(JNIEnv* env, jstring s, jsize start, jsize len, jchar* buf) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    std::u16string v = string_of(rt, s);
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > v.size()) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "région " + std::to_string(start) + "+" + std::to_string(len));
    }
    std::memcpy(buf, v.data() + start, size_t(len) * 2);
  });
}

JNI_ABI void GetStringUTFRegion(JNIEnv* env, jstring s, jsize start, jsize len, char* buf) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) {
    std::u16string v = string_of(rt, s);
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > v.size()) {
      rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "région " + std::to_string(start) + "+" + std::to_string(len));
    }
    std::string m = mutf8_of(v.substr(size_t(start), size_t(len)));
    std::memcpy(buf, m.data(), m.size() + 1);
  });
}

JNI_ABI void* GetPrimitiveArrayCritical(JNIEnv* env, jarray arr, jboolean* is_copy) {
  return guarded(env, static_cast<void*>(nullptr), [&](JniEnvImpl&, Runtime& rt) {
    ArrayObject& a = rt.array(H(arr), "GetPrimitiveArrayCritical");
    if (!a.klass->component->primitive) rt.throw_new("Ljava/lang/IllegalArgumentException;", "tableau primitif attendu");
    if (is_copy) *is_copy = JNI_FALSE;
    return static_cast<void*>(a.data.data());
  });
}

JNI_ABI void ReleasePrimitiveArrayCritical(JNIEnv*, jarray, void*, jint) {}
JNI_ABI const jchar* GetStringCritical(JNIEnv* env, jstring s, jboolean* is_copy) { return GetStringChars(env, s, is_copy); }
JNI_ABI void ReleaseStringCritical(JNIEnv* env, jstring s, const jchar* chars) { ReleaseStringChars(env, s, chars); }

JNI_ABI jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
  return guarded(env, jweak(nullptr), [&](JniEnvImpl&, Runtime& rt) -> jweak {
    Handle h = H(obj);
    if (!rt.is_valid_handle(h)) return nullptr;
    vm_of(rt)->weak_globals.insert(h);
    return as_ref(h, kWeakTag);
  });
}

JNI_ABI void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
  guarded_void(env, [&](JniEnvImpl&, Runtime& rt) { vm_of(rt)->weak_globals.erase(H(obj)); });
}

JNI_ABI jobject NewDirectByteBuffer(JNIEnv* env, void*, jlong) {
  return guarded(env, jobject(nullptr), [&](JniEnvImpl&, Runtime& rt) -> jobject {
    rt.throw_new("Ljava/lang/UnsupportedOperationException;", "[AnyAndroid] java.nio.ByteBuffer direct non implémenté");
  });
}

JNI_ABI void* GetDirectBufferAddress(JNIEnv*, jobject) { return nullptr; }
JNI_ABI jlong GetDirectBufferCapacity(JNIEnv*, jobject) { return -1; }

JNI_ABI jobjectRefType GetObjectRefType(JNIEnv* env, jobject obj) {
  return guarded(env, JNIInvalidRefType, [&](JniEnvImpl& e, Runtime& rt) {
    Handle h = H(obj);
    uintptr_t tag = reinterpret_cast<uintptr_t>(obj) & (kGlobalTag | kWeakTag);
    if (!rt.is_valid_handle(h)) return JNIInvalidRefType;
    if (tag == kGlobalTag) return JniBridge::is_global(rt, h) ? JNIGlobalRefType : JNIInvalidRefType;
    if (tag == kWeakTag) return vm_of(rt)->weak_globals.count(h) ? JNIWeakGlobalRefType : JNIInvalidRefType;
    for (Handle l : e.thread->jni_locals) {
      if (l == h) return JNILocalRefType;
    }
    return JNIInvalidRefType;
  });
}

// --- JavaVM ----------------------------------------------------------------------------------

JNI_ABI jint DestroyJavaVM(JavaVM*) { return JNI_ERR; }

[[gnu::noinline]] jint attach(JavaVM* vm, JNIEnv** penv, void* args, bool daemon) noexcept try {
  Runtime& rt = *reinterpret_cast<JavaVMImpl*>(vm)->rt;
  if (VmThread* t = JniBridge::attached(rt)) {
    *penv = &env_for(rt, t)->env;
    return JNI_OK;
  }
  std::string name = "Thread-natif";
  if (args) {
    auto* a = static_cast<JavaVMAttachArgs*>(args);
    if (a->name) name = a->name;
  }
  VmThread* t = JniBridge::attach(rt, name);  // returns with the GIL held
  t->daemon = daemon;
  t->in_vm = false;  // native code: its stack is not scanned, its references live in jni_locals
  JniEnvImpl* e = env_for(rt, t);
  JniBridge::unlock(rt, t);
  *penv = &e->env;
  return JNI_OK;
} catch (...) {
  return JNI_ERR;
}

JNI_ABI jint AttachCurrentThread(JavaVM* vm, JNIEnv** penv, void* args) { return attach(vm, penv, args, false); }
JNI_ABI jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** penv, void* args) { return attach(vm, penv, args, true); }

[[gnu::noinline]] jint detach(JavaVM* vm) noexcept try {
  Runtime& rt = *reinterpret_cast<JavaVMImpl*>(vm)->rt;
  VmThread* t = JniBridge::attached(rt);
  if (!t) return JNI_EDETACHED;
  if (t->scope_depth > 0 || !t->frames.empty()) return JNI_ERR;  // Java frames still on this thread
  t->jni_locals.clear();
  JniBridge::detach(rt);
  return JNI_OK;
} catch (...) {
  return JNI_ERR;
}

JNI_ABI jint DetachCurrentThread(JavaVM* vm) { return detach(vm); }

[[gnu::noinline]] jint get_env(JavaVM* vm, void** penv, jint version) noexcept try {
  if (version < JNI_VERSION_1_1 || version > JNI_VERSION_1_6) return JNI_EVERSION;
  Runtime& rt = *reinterpret_cast<JavaVMImpl*>(vm)->rt;
  VmThread* t = JniBridge::attached(rt);
  if (!t) {
    *penv = nullptr;
    return JNI_EDETACHED;
  }
  *penv = &env_for(rt, t)->env;
  return JNI_OK;
} catch (...) {
  return JNI_ERR;
}

JNI_ABI jint GetEnv(JavaVM* vm, void** penv, jint version) { return get_env(vm, penv, version); }

const JNINativeInterface& jni_table() {
  static const JNINativeInterface table = [] {
    JNINativeInterface t{};
#define S(name) t.name = name
#define S3(prefix, Name) \
  t.prefix##Name##Method = prefix##Name##Method; \
  t.prefix##Name##MethodV = prefix##Name##MethodV; \
  t.prefix##Name##MethodA = prefix##Name##MethodA
#define CALLS(Name) S3(Call, Name); S3(CallNonvirtual, Name); S3(CallStatic, Name)
#define FIELDS(Name) \
  t.Get##Name##Field = Get##Name##Field; t.Set##Name##Field = Set##Name##Field; \
  t.GetStatic##Name##Field = GetStatic##Name##Field; t.SetStatic##Name##Field = SetStatic##Name##Field
#define ARRAYS(Name, P, T) \
  t.New##Name##Array = new_primitive_array<P>; \
  t.Get##Name##ArrayElements = array_elements<P, T>; \
  t.Release##Name##ArrayElements = release_elements<T>; \
  t.Get##Name##ArrayRegion = get_region<P, T>; \
  t.Set##Name##ArrayRegion = set_region<P, T>
    S(GetVersion); S(DefineClass); S(FindClass); S(FromReflectedMethod); S(FromReflectedField);
    S(ToReflectedMethod); S(GetSuperclass); S(IsAssignableFrom); S(ToReflectedField); S(Throw); S(ThrowNew);
    S(ExceptionOccurred); S(ExceptionDescribe); S(ExceptionClear); S(FatalError); S(PushLocalFrame);
    S(PopLocalFrame); S(NewGlobalRef); S(DeleteGlobalRef); S(DeleteLocalRef); S(IsSameObject); S(NewLocalRef);
    S(EnsureLocalCapacity); S(AllocObject); S(NewObject); S(NewObjectV); S(NewObjectA); S(GetObjectClass);
    S(IsInstanceOf); S(GetMethodID); S(GetFieldID); S(GetStaticMethodID); S(GetStaticFieldID);
    CALLS(Object); CALLS(Boolean); CALLS(Byte); CALLS(Char); CALLS(Short); CALLS(Int); CALLS(Long);
    CALLS(Float); CALLS(Double); CALLS(Void);
    FIELDS(Object); FIELDS(Boolean); FIELDS(Byte); FIELDS(Char); FIELDS(Short); FIELDS(Int); FIELDS(Long);
    FIELDS(Float); FIELDS(Double);
    S(NewString); S(GetStringLength); S(GetStringChars); S(ReleaseStringChars); S(NewStringUTF);
    S(GetStringUTFLength); S(GetStringUTFChars); S(ReleaseStringUTFChars); S(GetArrayLength); S(NewObjectArray);
    S(GetObjectArrayElement); S(SetObjectArrayElement);
    ARRAYS(Boolean, 'Z', jboolean); ARRAYS(Byte, 'B', jbyte); ARRAYS(Char, 'C', jchar); ARRAYS(Short, 'S', jshort);
    ARRAYS(Int, 'I', jint); ARRAYS(Long, 'J', jlong); ARRAYS(Float, 'F', jfloat); ARRAYS(Double, 'D', jdouble);
    S(RegisterNatives); S(UnregisterNatives); S(MonitorEnter); S(MonitorExit); S(GetJavaVM); S(GetStringRegion);
    S(GetStringUTFRegion); S(GetPrimitiveArrayCritical); S(ReleasePrimitiveArrayCritical); S(GetStringCritical);
    S(ReleaseStringCritical); S(NewWeakGlobalRef); S(DeleteWeakGlobalRef); S(ExceptionCheck);
    S(NewDirectByteBuffer); S(GetDirectBufferAddress); S(GetDirectBufferCapacity); S(GetObjectRefType);
#undef S
#undef S3
#undef CALLS
#undef FIELDS
#undef ARRAYS
    return t;
  }();
  return table;
}

const JNIInvokeInterface& invoke_table() {
  static const JNIInvokeInterface table = [] {
    JNIInvokeInterface t{};
    t.DestroyJavaVM = DestroyJavaVM;
    t.AttachCurrentThread = AttachCurrentThread;
    t.DetachCurrentThread = DetachCurrentThread;
    t.GetEnv = GetEnv;
    t.AttachCurrentThreadAsDaemon = AttachCurrentThreadAsDaemon;
    return t;
  }();
  return table;
}

// Standard JNI layout (one pointer per entry): checked against well-known indices.
constexpr size_t kPtr = sizeof(void*);
static_assert(offsetof(JNINativeInterface, GetVersion) == 4 * kPtr);
static_assert(offsetof(JNINativeInterface, FindClass) == 6 * kPtr);
static_assert(offsetof(JNINativeInterface, GetMethodID) == 33 * kPtr);
static_assert(offsetof(JNINativeInterface, CallObjectMethod) == 34 * kPtr);
static_assert(offsetof(JNINativeInterface, CallNonvirtualObjectMethod) == 64 * kPtr);
static_assert(offsetof(JNINativeInterface, GetFieldID) == 94 * kPtr);
static_assert(offsetof(JNINativeInterface, GetStaticMethodID) == 113 * kPtr);
static_assert(offsetof(JNINativeInterface, GetStaticFieldID) == 144 * kPtr);
static_assert(offsetof(JNINativeInterface, NewStringUTF) == 167 * kPtr);
static_assert(offsetof(JNINativeInterface, GetArrayLength) == 171 * kPtr);
static_assert(offsetof(JNINativeInterface, RegisterNatives) == 215 * kPtr);
static_assert(offsetof(JNINativeInterface, GetObjectRefType) == 232 * kPtr);
static_assert(sizeof(JNINativeInterface) == 233 * kPtr);
static_assert(sizeof(JNIInvokeInterface) == 8 * kPtr);

// --- Calling native methods (System V AMD64) ------------------------------------------------
//
// Integer-class arguments go to rdi, rsi, rdx, rcx, r8, r9, floating-point
// ones to xmm0-7, the rest on the stack in parameter order. Declaring the
// callee as (6 integers, 8 doubles, K stack words) reproduces exactly that
// assignment for any signature; floats travel in the low 32 bits.

template <typename R, size_t... I>
R sysv_call(void* fn, const uint64_t* r, const double* f, const uint64_t* s, std::index_sequence<I...>) {
  using Fn = R(JNI_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double,
                         double, double, double, double, decltype((void)I, uint64_t{})...);
  return reinterpret_cast<Fn>(fn)(r[0], r[1], r[2], r[3], r[4], r[5], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                  s[I]...);
}

template <typename R>
R sysv_dispatch(Runtime& rt, void* fn, const uint64_t* r, const double* f, const std::vector<uint64_t>& stack) {
  const uint64_t* s = stack.data();
  switch (stack.size()) {
#define AA_CASE(N) \
  case N: return sysv_call<R>(fn, r, f, s, std::make_index_sequence<N>{});
    AA_CASE(0) AA_CASE(1) AA_CASE(2) AA_CASE(3) AA_CASE(4) AA_CASE(5) AA_CASE(6) AA_CASE(7) AA_CASE(8)
    AA_CASE(9) AA_CASE(10) AA_CASE(11) AA_CASE(12) AA_CASE(13) AA_CASE(14) AA_CASE(15) AA_CASE(16)
#undef AA_CASE
    default: break;
  }
  rt.throw_unsupported("JNI", "méthode native avec plus de 16 arguments passés sur la pile");
}

// Kept out of line: combining setjmp and System V calls in one function
// triggers an internal error in GCC 15 (choose_baseaddr).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
[[gnu::noinline]] void spill_registers(VmThread* t) {
  setjmp(t->parked_regs);
  volatile char marker = 0;
  t->parked_sp = const_cast<char*>(&marker);  // only compared, never dereferenced after return
}
#pragma GCC diagnostic pop

[[gnu::noinline]] uint64_t call_native(Runtime& rt, void* fn, const uint64_t* ints, const double* fps,
                                       const std::vector<uint64_t>& stack, char rtype) {
  if (rtype == 'F' || rtype == 'D') {
    double d = sysv_dispatch<double>(rt, fn, ints, fps, stack);
    uint64_t raw;
    std::memcpy(&raw, &d, 8);
    return rtype == 'F' ? raw & 0xFFFFFFFFull : raw;
  }
  return sysv_dispatch<uint64_t>(rt, fn, ints, fps, stack);
}

}  // namespace

uint64_t Runtime::invoke_jni(Method* m, const uint32_t* args, uint32_t nargs) {
  VmThread* t = running_;
  JniEnvImpl* env = env_for(*this, t);
  t->frames.push_back({m, kNativePc, args, nargs});
  struct Pop {
    std::vector<Frame>& f;
    ~Pop() { f.pop_back(); }
  } pop{t->frames};
  const size_t mark = t->jni_locals.size();
  const size_t frames_mark = env->frames.size();

  uint64_t ints[6] = {};
  uint64_t fpbits[8] = {};
  size_t ni = 0, nf = 0;
  std::vector<uint64_t> stack;
  auto push_int = [&](uint64_t v) {
    if (ni < 6) ints[ni++] = v;
    else stack.push_back(v);
  };
  auto push_fp = [&](uint64_t bits) {
    if (nf < 8) fpbits[nf++] = bits;
    else stack.push_back(bits);
  };
  push_int(reinterpret_cast<uint64_t>(&env->env));
  uint32_t i = 0;
  if (m->is_static()) {
    Handle cls = class_mirror(m->owner);
    t->jni_locals.push_back(cls);
    push_int(cls);
  } else {
    t->jni_locals.push_back(args[0]);
    push_int(args[0]);
    i = 1;
  }
  std::vector<std::string> params;
  std::string ret;
  dex::split_signature(m->signature, params, ret);
  for (const auto& p : params) {
    switch (p[0]) {
      case 'J': push_int(uint64_t(args[i]) | (uint64_t(args[i + 1]) << 32)); i += 2; break;
      case 'D': push_fp(uint64_t(args[i]) | (uint64_t(args[i + 1]) << 32)); i += 2; break;
      case 'F': push_fp(args[i++]); break;
      case 'Z': push_int(args[i++] & 0xFF); break;
      case 'B': push_int(uint64_t(int64_t(int8_t(args[i++])))); break;
      case 'C': push_int(args[i++] & 0xFFFF); break;
      case 'S': push_int(uint64_t(int64_t(int16_t(args[i++])))); break;
      case 'L':
      case '[':
        if (args[i]) t->jni_locals.push_back(args[i]);
        push_int(args[i++]);
        break;
      default: push_int(uint64_t(int64_t(int32_t(args[i++])))); break;
    }
  }
  double fps[8];
  std::memcpy(fps, fpbits, sizeof fps);
  char rtype = m->return_type();
  stats_.native_calls++;
  if (options_.trace) log::write(log::Level::Trace, "jni", "appel natif", m->pretty());

  // Native code runs without the GIL; the VM frames above stay scannable.
  spill_registers(t);
  running_ = nullptr;
  t->gil.unlock();
  uint64_t raw = 0;
  struct Reacquire {
    Runtime& rt;
    VmThread* t;
    ~Reacquire() { rt.acquire_gil(t); }
  };
  {
    Reacquire again{*this, t};
    raw = call_native(*this, m->jni_fn, ints, fps, stack, rtype);
  }

  Handle result_ref = (rtype == 'L' || rtype == '[') ? H(reinterpret_cast<void*>(raw)) : kNull;
  if (t->jni_locals.size() > mark) t->jni_locals.resize(mark);
  env->frames.resize(std::min(env->frames.size(), frames_mark));
  if (env->pending_kill) {
    env->pending_kill = false;
    throw ThreadKill{};
  }
  if (env->has_exit) {
    env->has_exit = false;
    throw ExitRequest{env->exit_code};
  }
  if (env->pending) {
    Handle ex = env->pending;
    env->pending = kNull;
    throw JavaThrow{ex};
  }
  switch (rtype) {
    case 'V': return 0;
    case 'L':
    case '[':
      if (result_ref && !is_valid_handle(result_ref)) {
        throw_new("Ljava/lang/VerifyError;", "référence invalide renvoyée par " + m->pretty());
      }
      return result_ref;
    case 'Z': return raw & 0xFF ? 1 : 0;
    case 'B': return uint32_t(int32_t(int8_t(raw)));
    case 'C': return uint32_t(raw & 0xFFFF);
    case 'S': return uint32_t(int32_t(int16_t(raw)));
    case 'J':
    case 'D': return raw;
    default: return uint32_t(raw);
  }
}

JNIEnv* jni_env(Runtime& rt) {
  VmThread* t = JniBridge::running(rt);
  if (!t) return nullptr;
  return &env_for(rt, t)->env;
}

JavaVM* java_vm(Runtime& rt) { return &vm_of(rt)->vm; }

}  // namespace aa::rt
