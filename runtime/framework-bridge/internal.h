#pragma once

#include <functional>
#include <optional>
#include <string>

#include "dex-engine/runtime.h"
#include "framework-bridge/bridge.h"

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

// Helpers of the libcore natives (libcore_natives.cpp).
rt::Field* find_field(rt::Class* cls, std::string_view name, std::string_view type);
int32_t fd_of(rt::Runtime& rt, rt::Handle file_descriptor);
// Creates an object of a (bytecode) class by running one of its constructors.
rt::Handle construct(rt::Runtime& rt, std::string_view cls, std::string_view sig, std::vector<uint32_t> args);
// Throws android.system.ErrnoException(function, err).
[[noreturn]] void throw_errno(rt::Runtime& rt, const char* function, int32_t err);
rt::Handle string_array(rt::Runtime& rt, const std::vector<std::string>& items);
// Native "free functions" of libcore.util.NativeAllocationRegistry: the
// getNativeFinalizer() natives return these bounded ids instead of C function
// pointers, and applyFreeFunction(id, ptr) calls the matching function.
int64_t register_free_function(std::function<void(int64_t)> fn);
// VMRuntime.addressOf: primitive arrays are given tagged fake addresses
// (never real pointers, which Java code could forge to write anywhere);
// natives taking such an address decode it back to the array, checked.
int64_t fake_address_of(rt::Runtime& rt, rt::Handle array);
rt::ArrayObject& array_at_address(rt::Runtime& rt, int64_t address);

}  // namespace aa::bridge
