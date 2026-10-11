// Native methods of AOSP's framework.jar (G3) used by its basic services:
// logging, clocks, tracing, message queues (Looper), system properties,
// process and Binder calling identity. In Android they live in
// libandroid_runtime.so, libutils and liblog.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/text.h"
#include "framework-bridge/internal.h"
#include "image/aosp.h"
#include "vfs/vfs.h"

#include <windows.h>

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

int64_t monotonic_nanos() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return int64_t(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return int64_t(c.QuadPart / freq * 1000000000 + (c.QuadPart % freq) * 1000000000 / freq);
}

int64_t thread_cpu_nanos() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](const FILETIME& f) { return (int64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) * 100;
}

int64_t process_cpu_millis() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](const FILETIME& f) { return (int64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10000;
}

// --- android.util.Log, android.os.Trace, SystemClock -------------------------------------------

void register_log_clock_trace(rt::NativeRegistry& r) {
  const char* log = "Landroid/util/Log;";
  r.bind(log, "println_native", "(IILjava/lang/String;Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    static const char levels[] = "??VDIWEA";
    int32_t prio = a.i32(1);
    char level = prio >= 0 && prio < 8 ? levels[prio] : '?';
    std::string tag = a.h(2) ? rt.string_utf8(a.h(2)) : "";
    std::string msg = a.h(3) ? rt.string_utf8(a.h(3)) : "";
    std::string line = std::string(1, level) + "/" + tag + ": " + msg + "\n";
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    return rt::ret_int(int32_t(line.size()));
  }, S::kPartial, "journal écrit sur la sortie d'erreur (format logcat)");
  r.bind(log, "isLoggable", "(Ljava/lang/String;I)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(a.i32(1) >= 4);  // Android's default threshold: INFO
  });
  r.bind(log, "logger_entry_max_payload_native", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 4068; });

  const char* stats = "Landroid/util/StatsLog;";
  r.bind(stats, "writeImpl", "([BII)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kStub,
         "écriture des métriques StatsLog ignorée");

  const char* clock = "Landroid/os/SystemClock;";
  r.bind(clock, "uptimeMillis", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(monotonic_nanos() / 1000000); });
  r.bind(clock, "uptimeNanos", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(monotonic_nanos()); });
  r.bind(clock, "elapsedRealtime", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(monotonic_nanos() / 1000000); });
  r.bind(clock, "elapsedRealtimeNanos", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(monotonic_nanos()); });
  r.bind(clock, "currentThreadTimeMillis", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(thread_cpu_nanos() / 1000000); });
  r.bind(clock, "currentThreadTimeMicro", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(thread_cpu_nanos() / 1000); });
  r.bind(clock, "currentTimeMicro", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return rt::ret_long(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  });

  // Tracing (Perfetto/atrace) is not available: tags are never enabled.
  const char* trace = "Landroid/os/Trace;";
  auto noop = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"nativeAsyncTraceBegin", "(JLjava/lang/String;I)V"}, {"nativeAsyncTraceEnd", "(JLjava/lang/String;I)V"},
           {"nativeAsyncTraceForTrackBegin", "(JLjava/lang/String;Ljava/lang/String;I)V"},
           {"nativeAsyncTraceForTrackEnd", "(JLjava/lang/String;I)V"}, {"nativeInstant", "(JLjava/lang/String;)V"},
           {"nativeInstantForTrack", "(JLjava/lang/String;Ljava/lang/String;)V"}, {"nativeIsTagEnabled", "(J)Z"},
           {"nativeRegisterWithPerfetto", "()V"}, {"nativeSetAppTracingAllowed", "(Z)V"},
           {"nativeSetTracingEnabled", "(Z)V"}, {"nativeTraceBegin", "(JLjava/lang/String;)V"},
           {"nativeTraceCounter", "(JLjava/lang/String;J)V"}, {"nativeTraceEnd", "(J)V"}}) {
    r.bind(trace, name, sig, noop, S::kPartial, "traçage désactivé");
  }
}

// --- android.os.MessageQueue (Looper) -------------------------------------------------------------
//
// ART's NativeMessageQueue waits on epoll with an eventfd for nativeWake().
// Here a queue remembers the thread polling it; the wait is a LockSupport-
// style park of that thread (the GIL is released), nativeWake() unparks it.

struct NativeQueue {
  Handle poller = kNull;  // java.lang.Thread blocked in nativePollOnce
  bool polling = false;
  bool woken = false;  // wake requested before or during the poll
};

std::vector<std::unique_ptr<NativeQueue>>& queues() {
  static std::vector<std::unique_ptr<NativeQueue>> table;
  return table;
}

NativeQueue& queue(Runtime& rt, int64_t ptr) {
  auto& t = queues();
  if (ptr <= 0 || uint64_t(ptr) > t.size() || !t[size_t(ptr - 1)]) {
    rt.throw_new("Ljava/lang/IllegalStateException;", "file de messages native invalide");
  }
  return *t[size_t(ptr - 1)];
}

void register_message_queue(rt::NativeRegistry& r) {
  const char* mq = "Landroid/os/MessageQueue;";
  r.bind(mq, "nativeInit", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    queues().push_back(std::make_unique<NativeQueue>());
    return uint64_t(queues().size());
  });
  r.bind(mq, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    int64_t ptr = a.i64(0);
    if (ptr > 0 && uint64_t(ptr) <= queues().size()) queues()[size_t(ptr - 1)].reset();
    return 0;
  });
  // Instance method: argument 0 is the MessageQueue.
  r.bind(mq, "nativePollOnce", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeQueue& q = queue(rt, a.i64(1));
    int32_t timeout = a.i32(3);
    if (q.woken) {
      q.woken = false;
      return 0;
    }
    if (timeout == 0) return 0;
    q.poller = rt.current_thread_peer();
    q.polling = true;
    // park_current: relative nanoseconds, 0 = until unparked.
    int64_t remaining = timeout < 0 ? 0 : int64_t(timeout) * 1000000;
    int64_t deadline = monotonic_nanos() + remaining;
    while (!q.woken) {
      rt.park_current(false, remaining);
      if (timeout >= 0) {
        remaining = deadline - monotonic_nanos();
        if (remaining <= 0) break;
      }
    }
    q.polling = false;
    q.woken = false;
    return 0;
  }, S::kPartial, "attente sans descripteurs de fichiers surveillés");
  r.bind(mq, "nativeWake", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeQueue& q = queue(rt, a.i64(0));
    q.woken = true;
    if (q.polling && q.poller != kNull) rt.unpark(q.poller);
    return 0;
  });
  r.bind(mq, "nativeIsPolling", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(queue(rt, a.i64(0)).polling);
  });
  r.bind(mq, "nativeSetFileDescriptorEvents", "(JII)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.throw_unsupported("Landroid/os/MessageQueue;->nativeSetFileDescriptorEvents(JII)V",
                         "surveillance de descripteurs par la file de messages non prise en charge");
  }, S::kUnsupported, "surveillance de descripteurs non prise en charge");
  r.bind(mq, "nativeSetSkipEpollWaitForZeroTimeout", "(J)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
}

// --- android.os.SystemProperties ---------------------------------------------------------------------
//
// Read from the build.prop files of the system image (through the VFS), plus
// an honest identity for the emulated device: AnyAndroid never presents
// itself as a certified device (ADR-002 guardrails).

struct PropertyStore {
  bool loaded = false;
  std::map<std::string, std::string> values;
  std::vector<std::string> keys;  // handles returned by native_find (index + 1)
};

PropertyStore& properties() {
  static PropertyStore store;
  if (!store.loaded) {
    store.loaded = true;
    for (const char* file : {"/system/etc/prop.default", "/system/build.prop", "/system_ext/etc/build.prop",
                             "/product/etc/build.prop", "/vendor/build.prop", "/odm/etc/build.prop"}) {
      try {
        auto data = libcore_vfs().read_file(file, 4u << 20);
        for (auto& [k, v] : image::parse_build_prop(data)) {
          if (!text::starts_with(k, "import")) store.values[k] = v;
        }
      } catch (const vfs::VfsError&) {
        // absent from this image
      }
    }
    const std::pair<const char*, const char*> device[] = {
        {"ro.product.manufacturer", "AnyAndroid"}, {"ro.product.brand", "anyandroid"},
        {"ro.product.model", "AnyAndroid (Windows)"}, {"ro.product.device", "anyandroid_x86_64"},
        {"ro.product.name", "anyandroid_x86_64"}, {"ro.hardware", "anyandroid"},
        {"ro.product.cpu.abi", "x86_64"}, {"ro.product.cpu.abilist", "x86_64"},
        {"ro.product.cpu.abilist64", "x86_64"}, {"ro.product.cpu.abilist32", ""},
        {"ro.debuggable", "0"}, {"ro.secure", "1"}, {"ro.boot.hardware", "anyandroid"},
        {"ro.serialno", "unknown"}, {"ro.build.characteristics", "default"}};
    for (const auto& [k, v] : device) store.values[k] = v;  // the device is AnyAndroid, whatever the image says
    // Generic values from the system partition when the global ones are absent.
    for (const char* k : {"version.sdk", "version.release", "version.codename", "version.incremental", "id",
                          "type", "tags", "fingerprint", "display.id", "version.security_patch"}) {
      std::string global = std::string("ro.build.") + k, sys = std::string("ro.system.build.") + k;
      if (!store.values.count(global) && store.values.count(sys)) store.values[global] = store.values[sys];
    }
  }
  return store;
}

std::optional<std::string> property(const std::string& key) {
  auto& p = properties();
  auto it = p.values.find(key);
  if (it == p.values.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> property_by_handle(int64_t h) {
  auto& p = properties();
  if (h <= 0 || uint64_t(h) > p.keys.size()) return std::nullopt;
  return property(p.keys[size_t(h - 1)]);
}

bool parse_bool(const std::string& v, bool def) {
  if (v == "1" || v == "y" || v == "yes" || v == "on" || v == "true") return true;
  if (v == "0" || v == "n" || v == "no" || v == "off" || v == "false") return false;
  return def;
}

int64_t parse_long(const std::string& v, int64_t def) {
  try {
    size_t used = 0;
    int64_t x = std::stoll(v, &used, 0);
    return used == v.size() ? x : def;
  } catch (...) {
    return def;
  }
}

void register_system_properties(rt::NativeRegistry& r) {
  const char* sp = "Landroid/os/SystemProperties;";
  r.bind(sp, "native_find", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string key = rt.string_utf8(a.h(0));
    if (!property(key)) return 0;
    auto& p = properties();
    auto it = std::find(p.keys.begin(), p.keys.end(), key);
    if (it != p.keys.end()) return uint64_t(it - p.keys.begin() + 1);
    p.keys.push_back(key);
    return uint64_t(p.keys.size());
  });
  r.bind(sp, "native_get", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(property_by_handle(a.i64(0)).value_or(""));
  });
  r.bind(sp, "native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto v = property(rt.string_utf8(a.h(0)));
    if (v && !v->empty()) return rt.new_string_utf8(*v);
    return a.h(1);
  });
  r.bind(sp, "native_get_int", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto v = property(rt.string_utf8(a.h(0)));
    return rt::ret_int(int32_t(v ? parse_long(*v, a.i32(1)) : a.i32(1)));
  });
  r.bind(sp, "native_get_int", "(JI)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto v = property_by_handle(a.i64(0));
    return rt::ret_int(int32_t(v ? parse_long(*v, a.i32(2)) : a.i32(2)));
  });
  r.bind(sp, "native_get_long", "(Ljava/lang/String;J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto v = property(rt.string_utf8(a.h(0)));
    return rt::ret_long(v ? parse_long(*v, a.i64(1)) : a.i64(1));
  });
  r.bind(sp, "native_get_long", "(JJ)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto v = property_by_handle(a.i64(0));
    return rt::ret_long(v ? parse_long(*v, a.i64(2)) : a.i64(2));
  });
  r.bind(sp, "native_get_boolean", "(Ljava/lang/String;Z)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto v = property(rt.string_utf8(a.h(0)));
    return rt::ret_bool(v ? parse_bool(*v, a.z(1)) : a.z(1));
  });
  r.bind(sp, "native_get_boolean", "(JZ)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto v = property_by_handle(a.i64(0));
    return rt::ret_bool(v ? parse_bool(*v, a.z(2)) : a.z(2));
  });
  r.bind(sp, "native_set", "(Ljava/lang/String;Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string key = rt.string_utf8(a.h(0));
    std::string value = a.h(1) ? rt.string_utf8(a.h(1)) : "";
    if (text::starts_with(key, "ro.") && property(key)) {
      rt.throw_new("Ljava/lang/RuntimeException;", "failed to set system property \"" + key + "\" (ro.* déjà défini)");
    }
    properties().values[key] = value;  // process-local
    return 0;
  }, S::kPartial, "propriétés locales au processus");
  r.bind(sp, "native_add_change_callback", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(sp, "native_report_sysprop_change", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
}

// --- android.os.Process, android.os.Binder calling identity ----------------------------------------

constexpr int32_t kAppUid = 10000;

// ADR-006: the process's service manager is AnyAndroid's host object
// aa.host.services.LocalServiceManager, created once. ServiceManager reaches it
// through BinderInternal.getContextObject / ServiceManagerProxy.getNativeServiceManager,
// then IServiceManager.Stub.asInterface finds it as a local interface: calls are
// direct Java calls, without Parcel.
Handle local_service_manager(Runtime& rt) {
  auto& cache = rt.object_cache();
  if (auto it = cache.find("host:servicemanager"); it != cache.end()) return it->second;
  constexpr const char* kClass = "Laa/host/services/LocalServiceManager;";
  rt::Class* c = rt.lookup_class(kClass);
  if (!c || !c->host) {  // never a class of the application's own DEX
    rt.throw_new("Ljava/lang/IllegalStateException;",
                 "[AnyAndroid] services système indisponibles : DEX d'hôte absent (ADR-006, option --host-dex)");
  }
  Handle h = construct(rt, kClass, "()V", {});
  cache["host:servicemanager"] = h;
  return h;
}

void register_process_binder(rt::NativeRegistry& r) {
  const char* proc = "Landroid/os/Process;";
  r.bind(proc, "getElapsedCpuTime", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(process_cpu_millis()); });
  r.bind(proc, "setThreadPriorityNative", "(II)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kStub,
         "priorités Linux sans effet");
  r.bind(proc, "getThreadPriority", "(I)I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(proc, "getThreadScheduler", "(I)I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(proc, "setArgV0Native", "(Ljava/lang/String;)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(proc, "getUidForName", "(Ljava/lang/String;)I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); });
  r.bind(proc, "getGidForName", "(Ljava/lang/String;)I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); });
  r.bind(proc, "getTotalMemory", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(int64_t(4) << 30); },
         S::kPartial, "valeur fixe");
  r.bind(proc, "getFreeMemory", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(int64_t(2) << 30); },
         S::kPartial, "valeur fixe");
  // Signals: only the process's own death (Process.killProcess(myPid())) has an effect.
  auto send = [](Runtime&, NativeArgs& a) -> uint64_t {
    if (a.i32(0) == int32_t(GetCurrentProcessId()) && a.i32(1) == 9) throw rt::ExitRequest{137};
    return 0;
  };
  for (const char* name : {"sendSignal", "sendSignalQuiet", "sendSignalThrows"}) {
    r.bind(proc, name, "(II)V", send, S::kPartial, "seul l'arrêt du processus courant est pris en compte");
  }

  const char* binder = "Landroid/os/Binder;";
  r.bind(binder, "getCallingUid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(kAppUid); });
  r.bind(binder, "getCallingPid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(int32_t(GetCurrentProcessId())); });
  r.bind(binder, "clearCallingIdentity", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long((int64_t(kAppUid) << 32) | int64_t(GetCurrentProcessId()));
  });
  r.bind(binder, "restoreCallingIdentity", "(J)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "hasExplicitIdentity", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "getCallingWorkSourceUid", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); });
  r.bind(binder, "clearCallingWorkSource", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "restoreCallingWorkSource", "(J)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "setCallingWorkSourceUid", "(I)J", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "getThreadStrictModePolicy", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "setThreadStrictModePolicy", "(I)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "flushPendingCommands", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(binder, "isDirectlyHandlingTransactionNative", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  // Local Binder objects. Binder.transact calls onTransact in Java and
  // Parcel keeps the objects themselves, so the native holder (BBinder in
  // Android) is only an identity: a counter, freed by a no-op.
  static std::atomic<int64_t> holders{0};
  int64_t finalizer = register_free_function([](int64_t) {});
  r.bind(binder, "getNativeBBinderHolder", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(++holders); });
  r.bind(binder, "getNativeFinalizer", "()J", [finalizer](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(finalizer); });
  auto noop = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
  r.bind(binder, "blockUntilThreadAvailable", "()V", noop);
  r.bind(binder, "setGlobalInheritRt", "(Z)V", noop);
  r.bind(binder, "setInheritRt", "(Z)V", noop);
  r.bind(binder, "markVintfStability", "()V", noop);
  r.bind(binder, "forceDowngradeToSystemStability", "()V", noop);
  r.bind(binder, "setExtensionNative", "(Landroid/os/IBinder;)V", noop, S::kStub, "extensions de Binder ignorées");

  auto service_manager = [](Runtime& rt, NativeArgs&) -> uint64_t { return local_service_manager(rt); };
  r.bind("Lcom/android/internal/os/BinderInternal;", "getContextObject", "()Landroid/os/IBinder;", service_manager,
         S::kPartial, "gestionnaire de services local d'AnyAndroid (ADR-006)");
  r.bind("Landroid/os/ServiceManagerProxy;", "getNativeServiceManager", "()Landroid/os/IBinder;", service_manager,
         S::kPartial, "gestionnaire de services local d'AnyAndroid (ADR-006)");
}

}  // namespace

std::optional<std::string> system_property(const std::string& key) { return property(key); }

void register_framework_natives(rt::NativeRegistry& r) {
  register_log_clock_trace(r);
  register_message_queue(r);
  register_system_properties(r);
  register_process_binder(r);
  register_parcel_natives(r);
  register_resource_natives(r);
}

}  // namespace aa::bridge
