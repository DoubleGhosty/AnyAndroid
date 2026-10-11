#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>

#include "dex-engine/runtime.h"
#include "framework-bridge/bridge.h"
#include "vfs/vfs.h"

namespace aa::bridge {

template <typename P>
P& peer_or_throw(rt::Runtime& rt, rt::Handle h, const char* what) {
  rt::Object& o = rt.nonnull(h, what);
  auto* p = dynamic_cast<P*>(o.peer.get());
  if (!p) {
    rt.throw_new("Ljava/lang/IllegalStateException;",
                 "[AnyAndroid] état natif absent pour " + o.klass->java_name() + " (constructeur non appelé ?)");
  }
  return *p;
}

struct BoxPeer final : rt::NativePeer {
  char type = 'I';  // primitive descriptor
  uint64_t bits = 0;
};

rt::Handle box(rt::Runtime& rt, char type, uint64_t bits);
// Reads a boxed number/char/boolean as int64 / double (for String.format).
bool unbox(rt::Runtime& rt, rt::Handle h, char& type, uint64_t& bits);
std::u16string java_format(rt::Runtime& rt, const std::u16string& fmt, rt::Handle args_array);
bool java_equals(rt::Runtime& rt, rt::Handle a, rt::Handle b);

void register_java_lang(rt::NativeRegistry& r, Host& host);
void register_java_util(rt::NativeRegistry& r, Host& host);
void register_java_reflect(rt::NativeRegistry& r, Host& host);
void register_android_app(rt::NativeRegistry& r, Host& host);
void register_android_widget(rt::NativeRegistry& r, Host& host);
void register_android_res(rt::NativeRegistry& r, Host& host);
void register_libcore_charsets(rt::NativeRegistry& r);
void register_libcore_files(rt::NativeRegistry& r);
void register_framework_natives(rt::NativeRegistry& r);
void register_parcel_natives(rt::NativeRegistry& r);
void register_resource_natives(rt::NativeRegistry& r);
// Android system properties of the emulated device (build.prop and
// AnyAndroid's identity), as android.os.SystemProperties sees them.
std::optional<std::string> system_property(const std::string& key);
// File system of the emulated process (/dev only when none is configured).
vfs::Vfs& libcore_vfs();

// Descriptor I/O of the emulated process (libcore_files.cpp): 0-2 are the
// host's standard streams, the other descriptors go through the Vfs. Errors
// are thrown as vfs::VfsError, for the caller to report the way its Java API
// expects (ErrnoException, UnixException, IOException).
size_t read_fd(int32_t fd, std::span<uint8_t> out);
size_t write_fd(int32_t fd, std::span<const uint8_t> data);
vfs::VStat fstat_fd(int32_t fd);
// Private copy of `length` bytes of a file from `offset`, in emulated native
// memory (mmap without MAP_SHARED); ENOMEM beyond 2 GiB, checked first.
int64_t map_fd_copy(rt::Runtime& rt, int32_t fd, uint64_t offset, uint64_t length, bool writable);

void register_libcore_nio(rt::NativeRegistry& r);
void register_libcore_zip(rt::NativeRegistry& r);
void register_libcore_crypto(rt::NativeRegistry& r);
void register_libcore_dex(rt::NativeRegistry& r);

// Class loaders (libcore_dex.cpp, ADR-006 J2). The defining loader of a class
// as Class.getClassLoader() reports it (boot space: BootClassLoader, or null
// when boot_as_null, as ART's internal view).
rt::Handle class_loader_of(rt::Runtime& rt, rt::Class* cls, bool boot_as_null);
// VMClassLoader.findLoadedClass: the class `loader` would answer for a binary
// name without running Java code, or nullptr.
rt::Class* find_loaded_class(rt::Runtime& rt, rt::Handle loader, const std::string& name);
// Class.classForName: through `loader` (null or BootClassLoader: boot space).
// Throws ClassNotFoundException.
rt::Class* class_for_name(rt::Runtime& rt, rt::Handle name, rt::Handle loader);

// Helpers of the libcore natives (libcore_natives.cpp).
rt::Field* find_field(rt::Class* cls, std::string_view name, std::string_view type);
int32_t fd_of(rt::Runtime& rt, rt::Handle file_descriptor);
// Creates an object of a (bytecode) class by running one of its constructors.
rt::Handle construct(rt::Runtime& rt, std::string_view cls, std::string_view sig, std::vector<uint32_t> args);
// Throws android.system.ErrnoException(function, err).
[[noreturn]] void throw_errno(rt::Runtime& rt, const char* function, int32_t err);
// strerror(3) text of a Linux errno value, as bionic words it.
std::string strerror_text(int32_t err);
rt::Handle string_array(rt::Runtime& rt, const std::vector<std::string>& items);
// Native "free functions" of libcore.util.NativeAllocationRegistry: the
// getNativeFinalizer() natives return these bounded ids instead of C function
// pointers, and applyFreeFunction(id, ptr) calls the matching function.
int64_t register_free_function(std::function<void(int64_t)> fn);

}  // namespace aa::bridge
