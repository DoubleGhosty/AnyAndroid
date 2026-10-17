#pragma once

#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dex-engine/code.h"
#include "dex-engine/dex_file.h"
#include "dex-engine/native_registry.h"

// Object model, class linker, threads and invocation for the AnyAndroid DEX
// runtime.
//
// References are 32-bit handles (8-bit generation + 24-bit slot index, 0 is
// null), which keeps Dalvik's 32-bit register model intact and lets the
// collector detect stale references. Memory is reclaimed by a mark-sweep
// collector (see gc.cpp) whose roots are precise for heap objects and
// conservative for native stacks.
//
// Threads: Java threads are Windows threads. A global interpreter lock (the
// "GIL", see ADR-003) serialises execution of Java code; it is released at
// blocking points (sleep, wait, join, contended monitors) and periodically at
// safepoints. Every call into the runtime must happen inside a Runtime::Scope.
namespace aa::rt {

using Handle = uint32_t;
constexpr Handle kNull = 0;

struct Class;
struct Method;
struct VmThread;
class Runtime;

// Native state attached to an object by framework classes (e.g. the window
// node behind a TextView). Peers are only accessed through checked casts.
struct NativePeer {
  virtual ~NativePeer() = default;
  // Reports the Java objects this peer keeps alive (GC strong references).
  virtual void trace(std::vector<Handle>& out) const {}
  // Clears references to objects that did not survive (weak references).
  virtual void sweep_weak(const std::function<bool(Handle)>& is_live) {}
  // Called on objects about to be freed, before any of them is destroyed.
  virtual void on_collect(const std::function<bool(Handle)>& is_live) {}
};

struct Monitor;

struct Object {
  Class* klass = nullptr;
  std::vector<uint32_t> fields;
  std::unique_ptr<NativePeer> peer;
  std::unique_ptr<Monitor> monitor;  // inflated on first use
  virtual ~Object();
};

struct StringObject final : Object {
  std::u16string value;
};

struct ArrayObject final : Object {
  uint32_t length = 0;
  uint8_t width = 4;  // bytes per element
  std::vector<uint8_t> data;
};

struct ClassPeer final : NativePeer {
  Class* cls = nullptr;
};

struct VmThread;

// java.lang.Thread state.
struct ThreadPeer final : NativePeer {
  VmThread* vm = nullptr;  // set once started (or for attached threads)
  Handle target = kNull;   // Runnable
  Handle handler = kNull;  // UncaughtExceptionHandler
  std::u16string name;
  bool daemon = false;
  bool started = false;
  bool pending_interrupt = false;
  int32_t priority = 5;
  void trace(std::vector<Handle>& out) const override {
    out.push_back(target);
    out.push_back(handler);
  }
};

struct Method;
struct Field;

// java.lang.reflect.Method/Constructor and Field state.
struct MethodPeer final : NativePeer {
  Method* method = nullptr;
};
struct FieldPeer final : NativePeer {
  Field* field = nullptr;
};

struct AnnotationPeer final : NativePeer {
  Class* anno_class = nullptr;
  const dex::DexFile* dex = nullptr;
  std::unordered_map<std::string, dex::EncodedValue> elements;
  std::vector<Handle> allocated;
  void trace(std::vector<Handle>& out) const override {
    out.insert(out.end(), allocated.begin(), allocated.end());
  }
};

// java.lang.ref.Reference state. Soft references are kept strong.
struct ReferencePeer final : NativePeer {
  Handle self = kNull;
  Handle referent = kNull;
  bool weak = true;
  void trace(std::vector<Handle>& out) const override {
    if (!weak) out.push_back(referent);
  }
  void sweep_weak(const std::function<bool(Handle)>& is_live) override {
    if (weak && referent && !is_live(referent)) referent = kNull;
  }
};

struct ReferenceQueuePeer final : NativePeer {
  std::vector<Handle> queued;
  void trace(std::vector<Handle>& out) const override {
    for (Handle h : queued) out.push_back(h);
  }
};

struct ThrowablePeer final : NativePeer {
  std::u16string message;
  bool has_message = false;
  Handle cause = kNull;
  std::vector<std::string> stack;
  Handle stack_trace = kNull;     // StackTraceElement[] once built or set (libcore mode)
  std::vector<Handle> suppressed;  // addSuppressed
  // Throwable(String, Throwable, boolean, boolean): suppression disabled, no
  // stack trace (neither captured nor settable).
  bool suppression_enabled = true, stack_writable = true;
  void trace(std::vector<Handle>& out) const override {
    out.push_back(cause);
    out.push_back(stack_trace);
    out.insert(out.end(), suppressed.begin(), suppressed.end());
  }
};

// Encoded annotation (visibility 1 = RUNTIME, 2 = SYSTEM) read from DEX.
struct EncodedAnnotation {
  uint8_t visibility = 0;
  std::string type_descriptor;
  std::unordered_map<std::string, dex::EncodedValue> elements;
  const dex::DexFile* dex = nullptr;
};

struct FieldAnnotations {
  std::vector<std::string> signature;  // dalvik.annotation.Signature
  std::unordered_map<std::string, EncodedAnnotation> annotations;
};

struct MethodAnnotations {
  std::vector<std::string> signature;       // dalvik.annotation.Signature
  std::vector<std::string> throws_classes;  // dalvik.annotation.Throws (class descriptors)
  std::unordered_map<std::string, EncodedAnnotation> annotations;
  std::vector<std::unordered_map<std::string, EncodedAnnotation>> parameter_annotations;
};

struct ClassAnnotations {
  bool inner = false;      // InnerClass: a member, local or anonymous class
  bool anonymous = false;  // InnerClass with a null name
  std::string inner_name;  // simple name of a member or local class
  int32_t inner_flags = 0;
  std::string enclosing_class;               // EnclosingClass (descriptor), or empty
  std::optional<uint32_t> enclosing_method;  // EnclosingMethod: method index in the class's DEX
  std::vector<std::string> member_classes;   // MemberClasses (descriptors)
  std::vector<std::string> signature;        // dalvik.annotation.Signature
  std::unordered_map<std::string, EncodedAnnotation> annotations;
};

struct Field {
  Class* owner = nullptr;
  std::string name;
  std::string type;
  uint32_t access = 0;
  uint32_t slot = 0;
  uint32_t field_idx = dex::kNoIndex;
  std::function<uint64_t(Runtime&)> native_init;
  std::unique_ptr<FieldAnnotations> annotations_;
  bool is_static() const { return access & dex::kAccStatic; }
  bool wide() const { return type == "J" || type == "D"; }
  bool is_reference() const { return !type.empty() && (type[0] == 'L' || type[0] == '['); }
  const FieldAnnotations& annotations();
};

struct Method {
  Class* owner = nullptr;
  std::string name;
  std::string signature;
  uint32_t access = 0;
  uint32_t arg_words = 0;  // including the receiver
  // Bytecode implementation.
  const dex::DexFile* dex = nullptr;
  uint32_t method_idx = 0;
  uint32_t code_off = 0;
  std::unique_ptr<PreparedCode> code;
  bool code_failed = false;
  // Native implementation (framework).
  NativeFn native;
  ApiStatus status = ApiStatus::kSupported;
  std::string note;
  bool reported = false;  // stub/partial usage already logged
  // Profile (`exec --profile`, RuntimeOptions::profile): calls and
  // instructions executed in this method's own frames (callees excluded).
  uint64_t profile_calls = 0;
  uint64_t profile_instructions = 0;
  // JNI implementation registered with RegisterNatives (System V ABI).
  void* jni_fn = nullptr;
  std::unique_ptr<MethodAnnotations> annotations_;

  bool is_static() const { return access & dex::kAccStatic; }
  bool is_abstract() const { return !native && !jni_fn && code_off == 0; }
  char return_type() const { return signature.empty() ? 'V' : signature[signature.find(')') + 1]; }
  std::string pretty() const;
  const MethodAnnotations& annotations();
};

struct DexCache {
  const dex::DexFile* dex = nullptr;
  bool boot = false;  // boot class path (libcore, framework)
  bool host = false;  // AnyAndroid's host code (ADR-006), after the boot class path
  // Class loader that registered this application DEX file (DexFile natives,
  // ADR-006 J2): the defining loader of its classes. kNull until one does.
  Handle loader = kNull;
  std::vector<Class*> types;
  std::vector<Field*> fields;
  std::vector<Method*> methods;  // static/direct resolution
  std::vector<Handle> strings;
};

struct Class {
  enum class State { kLinked, kInitializing, kInitialized, kFailed };

  std::string descriptor;
  Class* super = nullptr;
  std::vector<Class*> interfaces;
  uint32_t access = 0;
  bool framework = false;  // provided by the native registry
  bool boot = false;       // defined by the boot class path (libcore mode)
  bool host = false;       // defined by the host DEX, or derived from such a class (ADR-006)
  bool phantom = false;    // unknown interface replaced by an empty one
  char primitive = 0;      // 'I', 'Z', ... for primitive classes
  Class* component = nullptr;
  DexCache* dex_cache = nullptr;
  uint32_t class_def_idx = 0;
  std::vector<std::unique_ptr<Field>> fields;
  std::vector<std::unique_ptr<Method>> methods;
  uint32_t instance_slots = 0;
  std::vector<uint32_t> ref_slots;  // instance slots holding references (incl. superclasses)
  std::vector<uint32_t> statics;
  State state = State::kLinked;
  VmThread* init_thread = nullptr;  // thread running <clinit>
  Handle mirror = kNull;
  std::unordered_map<std::string, Method*> vcache;
  ApiStatus status = ApiStatus::kSupported;

  bool is_interface() const { return access & dex::kAccInterface; }
  bool is_abstract() const { return access & dex::kAccAbstract; }
  bool is_array() const { return component != nullptr; }
  Method* declared_method(std::string_view name, std::string_view sig) const;
  Field* declared_field(std::string_view name, std::string_view type) const;
  std::string java_name() const;
  // System and runtime annotations.
  const ClassAnnotations& annotations();
  ClassAnnotations& mutable_annotations();
  // Class.getSimpleName(): "" for an anonymous class, the InnerClass name for
  // a member or local class, "T[]" for arrays.
  std::string simple_name();

 private:
  void load_annotations();
  std::unique_ptr<ClassAnnotations> annotations_;
};

// A Java exception in flight, propagated with C++ exceptions.
struct JavaThrow {
  Handle exception;
};

// System.exit(): unwinds to the host.
struct ExitRequest {
  int code;
};

// Unwinds a Java thread when the runtime shuts down. Never caught by Java code.
struct ThreadKill {};

struct Frame {
  Method* method;
  uint32_t pc;                    // kNativePc for native frames
  const uint32_t* regs = nullptr;  // register file or native arguments (GC roots)
  uint32_t nregs = 0;
};
constexpr uint32_t kNativePc = 0xFFFFFFFF;

struct Monitor {
  struct Waiter {
    bool notified = false;
  };
  VmThread* owner = nullptr;
  uint32_t count = 0;
  std::condition_variable entry;    // threads waiting to acquire
  std::condition_variable waitset;  // threads in Object.wait()
  std::vector<Waiter*> waiters;
};

// A Java thread: either a Windows thread started by Thread.start(), or an
// existing thread attached to the runtime (main, UI).
struct VmThread {
  Runtime* rt = nullptr;
  uint32_t id = 0;
  std::string name;
  Handle peer = kNull;  // java.lang.Thread (created lazily for attached threads)
  bool daemon = false;
  bool attached = false;
  std::vector<Frame> frames;
  uint64_t budget_used = 0;
  uint32_t poll_count = 0;
  // GIL state.
  std::unique_lock<std::mutex> gil;
  int scope_depth = 0;
  bool in_vm = false;          // inside a Scope (running or parked)
  char* parked_sp = nullptr;   // stack position while parked (conservative scan)
  const char* stack_high = nullptr;
  std::jmp_buf parked_regs;    // callee-saved registers spilled while parked
  // Blocking state.
  bool interrupted = false;
  bool park_permit = false;  // LockSupport.unpark() permit
  bool alive = true;
  std::condition_variable sleep_cv;
  Monitor* waiting_on = nullptr;
  std::thread os;
  // JNI local references of the native frames of this thread.
  std::vector<Handle> jni_locals;
  void* jni_env = nullptr;
};

// Strong reference held by native code outside the Java heap (tasks, host
// state). Copyable; the object stays alive while one copy exists.
class GlobalRef {
 public:
  GlobalRef() = default;
  GlobalRef(Runtime& rt, Handle h);
  GlobalRef(const GlobalRef& other);
  GlobalRef& operator=(const GlobalRef& other);
  ~GlobalRef();
  Handle get() const { return h_; }

 private:
  Runtime* rt_ = nullptr;
  Handle h_ = kNull;
};

struct RuntimeOptions {
  bool trace = false;
  // Per-method counters (Method::profile_*, `exec --profile`). Off, they are
  // not touched: the interpreter tests a flag hoisted out of its loop.
  bool profile = false;
  uint32_t max_depth = 1000;
  size_t max_objects = 4'000'000;
  uint32_t max_array_length = 64u << 20;
  // Instruction budget per top-level call (0 = unlimited). Protects the UI
  // thread against runaway loops in tests and scripted runs.
  uint64_t max_instructions = 0;
  // Collection is triggered when the live heap doubles, but never below these.
  size_t gc_min_objects = 200'000;
  size_t gc_min_bytes = 32u << 20;
  bool verbose_gc = false;
  // ADR-006: when set, each call from the framework into an object of the
  // host DEX (a local system service) is written here, one line per call
  // ("service : PackageService.getApplicationInfo"), for reference traces.
  std::FILE* service_trace = nullptr;
};

// Arguments of a native method, as raw 32-bit registers.
class NativeArgs {
 public:
  NativeArgs(Runtime& rt, const uint32_t* regs, uint32_t count) : rt_(rt), regs_(regs), count_(count) {}

  uint32_t count() const { return count_; }
  uint32_t raw(uint32_t i) const;
  int32_t i32(uint32_t i) const { return int32_t(raw(i)); }
  bool z(uint32_t i) const { return raw(i) != 0; }
  float f32(uint32_t i) const;
  int64_t i64(uint32_t i) const;
  double f64(uint32_t i) const;
  Handle h(uint32_t i) const { return raw(i); }
  Object* obj(uint32_t i) const;      // may be null
  Object& self() const;               // argument 0, null-checked
  std::u16string str(uint32_t i) const;  // NullPointerException if null
  std::u16string str_or(uint32_t i, std::u16string_view fallback) const;

 private:
  Runtime& rt_;
  const uint32_t* regs_;
  uint32_t count_;
};

inline uint64_t ret_int(int32_t v) { return uint32_t(v); }
inline uint64_t ret_bool(bool v) { return v ? 1 : 0; }
inline uint64_t ret_obj(Handle h) { return h; }
inline uint64_t ret_long(int64_t v) { return uint64_t(v); }
inline uint64_t ret_float(float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  return b;
}
inline uint64_t ret_double(double v) {
  uint64_t b;
  std::memcpy(&b, &v, 8);
  return b;
}

struct GcStats {
  uint64_t collections = 0;
  uint64_t freed = 0;
  uint64_t last_live = 0;
  double last_ms = 0;
};

class Runtime {
 public:
  explicit Runtime(RuntimeOptions options = {});
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Holds the GIL for the calling OS thread, attaching it as a Java thread
  // on first use. Scopes nest; only the outermost one acquires/releases.
  class Scope {
   public:
    explicit Scope(Runtime& rt, const char* thread_name = "main");
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    Runtime& rt_;
    VmThread* thread_;
  };

  NativeRegistry& registry() { return registry_; }
  const RuntimeOptions& options() const { return options_; }
  RuntimeOptions& mutable_options() { return options_; }
  // Makes the classes of a DEX file loadable. The file must outlive the runtime.
  void add_dex(const dex::DexFile* dex);
  // Adds a DEX file of the boot class path (AOSP libcore and framework, G2).
  // Its classes take precedence over the bridge and over application code,
  // except the VM core classes (is_vm_core_class), which stay native.
  void add_boot_dex(const dex::DexFile* dex);
  // Adds AnyAndroid's host DEX (system services, ADR-006) after the boot class
  // path: its classes cannot be replaced by the application's. An abstract
  // method a host class leaves unimplemented answers a neutral value (null, 0,
  // false) and is logged once, instead of AbstractMethodError.
  void add_host_dex(const dex::DexFile* dex);
  bool libcore_mode() const { return boot_mode_; }

  // --- Classes -------------------------------------------------------------
  Class* find_class(std::string_view descriptor);    // NoClassDefFoundError if absent
  Class* lookup_class(std::string_view descriptor);  // nullptr if absent
  // Class already defined, without loading it.
  Class* loaded_class(std::string_view descriptor) const;
  // Class of the boot space (VM core, boot class path, host DEX, bridge),
  // loaded if needed; nullptr when only application DEX files define it.
  Class* lookup_boot_class(std::string_view descriptor);
  // Defines (without initialising) a class from an application DEX file, as
  // DexFile.defineClassNative does. There is one class namespace (ADR-006 J2):
  // the boot space wins and a class already defined is returned as is.
  Class* define_synthetic_class(std::string descriptor, Class* super, std::vector<Class*> interfaces,
                                uint32_t access = dex::kAccPublic | dex::kAccFinal);
  Class* define_class(const dex::DexFile* dex, std::string_view descriptor);
  // Classes defined by the boot space rather than by application DEX files
  // (arrays: by their element type).
  static bool in_boot_space(const Class* cls);
  // Application DEX files (add_dex), in order, and their caches.
  std::vector<const dex::DexFile*> app_dex_files() const;
  DexCache* find_dex_cache(const dex::DexFile* dex);
  void ensure_initialized(Class* cls);
  bool is_assignable(const Class* from, const Class* to) const;
  bool instance_of(Handle h, const Class* to);
  Class* class_of(Handle h);
  std::vector<Class*> loaded_classes() const;

  // --- Objects -------------------------------------------------------------
  Object* object(Handle h);                          // nullptr for null
  Object& nonnull(Handle h, std::string_view what);  // NullPointerException
  bool is_valid_handle(Handle h) const;
  template <typename P>
  P* peer(Handle h) {
    Object* o = object(h);
    return o ? dynamic_cast<P*>(o->peer.get()) : nullptr;
  }
  Handle alloc(Class* cls);
  Handle new_string(std::u16string value);
  // Replaces the characters of a string and keeps ART's `count` field
  // ((length << 1) | uncompressed flag), read directly by libcore, in sync.
  void set_string_value(StringObject& s, std::u16string value);
  Handle new_string_utf8(std::string_view utf8);
  Handle intern(const std::u16string& value);
  Handle new_array(Class* array_class, int32_t length);
  Handle new_array(std::string_view descriptor, int32_t length);
  ArrayObject& array(Handle h, std::string_view what);
  StringObject* as_string(Handle h);
  std::u16string string_value(Handle h);  // toString() for non-strings, NPE for null
  std::string string_utf8(Handle h);
  Handle class_mirror(Class* cls);
  Class* mirror_class(Handle h);
  size_t object_count() const { return live_objects_; }

  // --- Invocation ----------------------------------------------------------
  uint64_t invoke(Method* method, const uint32_t* args, uint32_t nargs);
  Method* resolve_virtual(Class* cls, std::string_view name, std::string_view sig);
  Method* find_static(Class* cls, std::string_view name, std::string_view sig);
  uint64_t call_virtual(Handle receiver, std::string_view name, std::string_view sig,
                        std::vector<uint32_t> extra_args = {});
  uint64_t call_static(std::string_view cls, std::string_view name, std::string_view sig,
                       std::vector<uint32_t> args = {});
  // Resets the instruction budget of the current thread (new top-level event).
  void reset_budget();
  // invoke-polymorphic (VarHandle, MethodHandle): handled by the registry's
  // polymorphic handler, typed by the call-site prototype.
  uint64_t invoke_polymorphic(Class* declared, const std::string& name, const std::string& callsite,
                              const uint32_t* args, uint32_t count);
  // Opaque, bounds-checked identifiers of fields for code that stores them in
  // Java objects (ART's Field.getArtField(), VarHandle): never raw pointers.
  uint64_t field_cookie(Field* f);
  Field* field_from_cookie(uint64_t cookie);
  uint64_t method_cookie(Method* m);
  Method* method_from_cookie(uint64_t cookie);

  // --- Threads and monitors ---------------------------------------------------
  VmThread* current_thread() const { return running_; }
  Handle current_thread_peer();
  // Thread.start(): creates a Windows thread that runs `peer.run()`.
  VmThread* start_thread(Handle peer, const std::string& name, bool daemon);
  void monitor_enter(Handle h);
  void monitor_exit(Handle h);
  bool holds_lock(Handle h);
  // Object.wait(millis): 0 = forever. Throws InterruptedException.
  void monitor_wait(Handle h, int64_t millis);
  void monitor_notify(Handle h, bool all);
  void sleep(int64_t millis);  // Thread.sleep, interruptible
  // LockSupport.park/unpark (Unsafe.park): one permit per thread. `time` is
  // relative nanoseconds (0 = no timeout) or, when absolute, epoch milliseconds.
  void park_current(bool absolute, int64_t time);
  void unpark(Handle thread_peer);
  void yield_thread();
  void interrupt(VmThread* t);
  bool join(VmThread* t, int64_t millis);  // false on timeout
  VmThread* thread_of_peer(Handle peer);
  // Waits until every non-daemon thread started by Thread.start() has ended.
  void wait_for_non_daemon_threads();
  // Stops every started thread (ThreadKill) and joins them. Called by ~Runtime.
  void shutdown_threads();
  // Called by the interpreter: GC, GIL hand-off and shutdown checks.
  void safepoint();
  // Uncaught exception in a started thread: Thread handlers, then this hook.
  std::function<void(VmThread*, Handle)> on_uncaught_exception;
  // System.exit() called from a started thread.
  std::function<void(int)> on_exit_request;

  // --- Garbage collection -----------------------------------------------------
  void collect_garbage();
  void request_gc() { gc_requested_ = true; }
  const GcStats& gc_stats() const { return gc_stats_; }
  // Extra roots supplied by the host (activities, pending intents...).
  size_t add_root_provider(std::function<void(std::vector<Handle>&)> provider);
  void remove_root_provider(size_t id);
  void add_global(Handle h);
  void remove_global(Handle h);

  // --- Exceptions ----------------------------------------------------------
  [[noreturn]] void throw_new(std::string_view descriptor, std::u16string_view message);
  [[noreturn]] void throw_new(std::string_view descriptor, std::string_view utf8_message = {});
  [[noreturn]] void throw_npe(std::string_view what);
  // An Android/Java API the prototype does not implement was called. Logs it,
  // records it and raises java.lang.NoSuchMethodError with an explicit message.
  [[noreturn]] void throw_unsupported(std::string_view api, std::string_view reason = {});
  Handle make_throwable(Class* cls, std::u16string_view message, bool has_message);
  std::string describe_throwable(Handle h);  // "java.lang.X: message\n  at ..."
  std::vector<std::string> capture_stack() const;

  // --- Diagnostics ---------------------------------------------------------
  struct Stats {
    uint64_t instructions = 0;
    uint64_t invocations = 0;
    uint64_t native_calls = 0;
    uint64_t threads_started = 0;
  };
  const Stats& stats() const { return stats_; }
  const std::set<std::string>& unsupported_apis() const { return unsupported_; }
  const std::set<std::string>& stub_apis() const { return stubs_used_; }
  const std::vector<std::string>& phantom_classes() const { return phantoms_; }

  // Disassembly of a method's prepared code, for debugging.
  std::string disassemble(Method* method);

  // Canonical objects owned by native code (boxed-value cache, singletons).
  std::unordered_map<std::string, Handle>& object_cache() { return object_cache_; }

 private:
  friend class Scope;
  friend class NativeArgs;
  friend struct JniBridge;

  struct Slot {
    std::unique_ptr<Object> obj;
    uint32_t bytes = 0;
    uint8_t gen = 1;
  };

  Class* load_class(std::string_view descriptor);
  bool defined_in_boot_space(std::string_view descriptor) const;
  Class* define_from_registry(const NativeClassSpec& spec);
  void merge_boot_bytecode(Class* cls);
  Class* define_from_dex(DexCache& cache, uint32_t class_def_idx);
  Class* define_array(std::string_view descriptor);
  Class* define_primitive(char c);
  Class* define_phantom_interface(std::string_view descriptor);
  void compute_ref_slots(Class* cls);
  void initialize_statics(Class* cls);
  void note_native_call(Method& m);
  void trace_service_call(VmThread* t, Method* m, Handle self, bool implemented);
  Handle store(std::unique_ptr<Object> obj, size_t bytes);

  uint64_t execute(Method* method, const uint32_t* args, uint32_t nargs);
  uint64_t invoke_jni(Method* method, const uint32_t* args, uint32_t nargs);
  PreparedCode& prepared(Method* m);

  // Resolution from bytecode.
  Class* resolve_type(DexCache& cache, uint32_t type_idx);
  Handle resolve_string(DexCache& cache, uint32_t string_idx);
  Field* resolve_field(DexCache& cache, uint32_t field_idx, bool is_static);
  [[noreturn]] void missing_method(Class* referenced, std::string_view name, std::string_view sig, bool is_static);
  Class* first_framework_ancestor(Class* cls);

  // Threads (threads.cpp).
  VmThread* attach_current_thread(const std::string& name);
  void acquire_gil(VmThread* t);
  void release_gil(VmThread* t);
  // Blocks on `cv` with the GIL released until pred() is true or the timeout
  // (ms, 0 = none) expires; returns pred(). Throws ThreadKill on shutdown.
  bool park(VmThread* t, std::condition_variable& cv, const std::function<bool()>& pred, int64_t timeout_ms);
  void thread_main(VmThread* t);
  VmThread* attached_thread();  // VmThread of the calling OS thread, if attached
  void detach_current_thread();
  Monitor& monitor_of(Object& o);
  void check_interrupted(VmThread* t);


  RuntimeOptions options_;
  NativeRegistry registry_;
  std::unordered_map<std::string, std::unique_ptr<Class>> classes_;
  std::unordered_set<std::string> loading_;
  std::vector<std::unique_ptr<DexCache>> dex_caches_;
  std::vector<Slot> slots_;
  std::vector<uint32_t> free_slots_;
  size_t live_objects_ = 0;
  size_t heap_bytes_ = 0;
  size_t gc_object_threshold_ = 0;
  size_t gc_byte_threshold_ = 0;
  std::atomic<bool> gc_requested_{false};
  bool in_gc_ = false;
  GcStats gc_stats_;
  std::unordered_map<Handle, uint32_t> globals_;
  std::map<size_t, std::function<void(std::vector<Handle>&)>> root_providers_;
  size_t next_root_provider_ = 1;
  std::unordered_map<std::u16string, Handle> interned_;
  std::unordered_map<std::string, Handle> object_cache_;

  // Threads.
  std::mutex gil_;
  std::condition_variable gil_handoff_;
  std::atomic<int> gil_waiters_{0};
  uint64_t gil_generation_ = 0;
  VmThread* running_ = nullptr;
  std::vector<std::unique_ptr<VmThread>> threads_;
  std::condition_variable threads_cv_;  // a started thread ended
  uint32_t next_thread_id_ = 1;
  std::atomic<bool> shutting_down_{false};
  uint64_t instance_id_ = 0;
  // Native-side state owned by the runtime (JNI environments, JavaVM).
  std::vector<std::shared_ptr<void>> native_state_;
  void* java_vm_ = nullptr;

  Stats stats_;
  bool boot_mode_ = false;
  std::vector<Field*> field_cookies_;
  std::unordered_map<Field*, uint64_t> field_cookie_of_;
  std::vector<Method*> method_cookies_;
  std::unordered_map<Method*, uint64_t> method_cookie_of_;
  std::set<std::string> unsupported_;
  std::set<std::string> stubs_used_;
  std::vector<std::string> phantoms_;
  Class* string_class_ = nullptr;
  int64_t string_count_slot_ = -2;  // -2: not looked up yet, -1: no such field
};

// Registers java.lang.Object, String, Class, Thread, the Throwable hierarchy,
// java.lang.ref and the few interfaces the runtime itself depends on.
void register_core_classes(NativeRegistry& registry);
// What libcore's Thread constructors set up besides the native peer (field
// initializers such as blockerLock), for every Thread object, including those
// the runtime creates for threads started outside Java.
void init_thread_object(Runtime& rt, Handle thread);

}  // namespace aa::rt
