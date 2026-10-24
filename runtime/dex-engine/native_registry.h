#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Registry of classes implemented natively by AnyAndroid (java.lang core and
// the Android framework subset). Every member carries a compatibility status
// so that the same table drives execution, static compatibility reports and
// the published API matrix.
namespace aa::rt {

class Runtime;
class NativeArgs;
struct Class;
struct Method;

enum class ApiStatus : uint8_t { kSupported, kPartial, kStub, kUnsupported, kInvestigate };

const char* api_status_id(ApiStatus s);     // "supported", "partial", ...
const char* api_status_label(ApiStatus s);  // French label for reports

using NativeFn = std::function<uint64_t(Runtime&, NativeArgs&)>;

// Signature-polymorphic methods (VarHandle accessors, MethodHandle.invoke):
// declared class, method name, call-site prototype ("(Ljava/lang/Object;II)Z")
// and raw argument words, argument 0 being the handle.
using PolymorphicFn = std::function<uint64_t(Runtime&, Class* declared, const std::string& name,
                                             const std::string& callsite, const uint32_t* args, uint32_t count)>;

// const-method-type, const-method-handle and invoke-custom (DEX 038), which
// need java.lang.invoke: resolved against the DEX file and the class of the
// referrer (the method executing the instruction).
struct InvokeDynamicHandlers {
  std::function<uint32_t(Runtime&, Method* referrer, uint32_t proto_idx)> method_type;
  std::function<uint32_t(Runtime&, Method* referrer, uint32_t method_handle_idx)> method_handle;
  std::function<uint64_t(Runtime&, Method* referrer, uint32_t call_site_idx, const uint32_t* args, uint32_t count)>
      invoke_custom;
};

struct NativeMethodSpec {
  std::string name;
  std::string signature;  // "(Ljava/lang/String;)V"
  uint32_t access = 0x1;  // ACC_PUBLIC
  NativeFn fn;            // empty = abstract (interface callback contract)
  ApiStatus status = ApiStatus::kSupported;
  std::string note;
  // With a boot class path, the boot bytecode that declares this method with
  // code runs instead (it works on the same fields): the native
  // implementation serves only without libcore.
  bool bytecode_with_libcore = false;
};

struct NativeFieldSpec {
  std::string name;
  std::string type;
  uint32_t access = 0x1 | 0x8 | 0x10;  // public static final
  std::function<uint64_t(Runtime&)> init;
  ApiStatus status = ApiStatus::kSupported;
  std::string note;
};

struct NativeClassSpec {
  std::string descriptor;
  std::string super;  // empty only for java.lang.Object and interfaces' implicit root
  std::vector<std::string> interfaces;
  uint32_t access = 0x1;
  ApiStatus status = ApiStatus::kSupported;
  std::string note;
  std::vector<NativeMethodSpec> methods;
  std::vector<NativeFieldSpec> static_fields;
  // Instance fields read and written by bytecode (e.g. Thread.threadLocals,
  // accessed directly by libcore's ThreadLocal).
  std::vector<NativeFieldSpec> instance_fields;
};

// Native implementation of a method declared in bytecode: a native method of
// the boot class path (libcore's System.nanoTime(), Linux.writeBytes()...),
// or a VM intrinsic replacing a bytecode body.
struct NativeBinding {
  NativeFn fn;
  ApiStatus status = ApiStatus::kSupported;
  std::string note;
  bool replaces_bytecode = false;
};

class ClassBuilder {
 public:
  explicit ClassBuilder(NativeClassSpec* spec) : spec_(spec) {}

  ClassBuilder& method(std::string name, std::string sig, NativeFn fn, ApiStatus status = ApiStatus::kSupported,
                       std::string note = {});
  ClassBuilder& static_method(std::string name, std::string sig, NativeFn fn,
                              ApiStatus status = ApiStatus::kSupported, std::string note = {});
  ClassBuilder& ctor(std::string sig, NativeFn fn, ApiStatus status = ApiStatus::kSupported, std::string note = {});
  // Interface/abstract method: dispatch must find an implementation in app code.
  ClassBuilder& abstract_method(std::string name, std::string sig);
  // Recognised API without effect. Calls are logged and counted; the return
  // value is zero/null/false.
  ClassBuilder& stub(std::string name, std::string sig, std::string note);
  // Recognised API that deliberately fails with an explicit error.
  ClassBuilder& unsupported(std::string name, std::string sig, std::string note);
  ClassBuilder& static_field(std::string name, std::string type, std::function<uint64_t(Runtime&)> init,
                             ApiStatus status = ApiStatus::kSupported, std::string note = {});
  // Instance field stored in the object like a bytecode field (zero-initialised).
  ClassBuilder& field(std::string name, std::string type, uint32_t access = 0);
  // Marks the methods already declared under these names (every signature)
  // as serving only without libcore (NativeMethodSpec::bytecode_with_libcore).
  ClassBuilder& bytecode_with_libcore(std::initializer_list<std::string_view> names);

  NativeClassSpec& spec() { return *spec_; }

 private:
  NativeClassSpec* spec_;
};

class NativeRegistry {
 public:
  ClassBuilder define(std::string descriptor, std::string super, std::vector<std::string> interfaces = {},
                      uint32_t access = 0x1, ApiStatus status = ApiStatus::kSupported, std::string note = {});
  ClassBuilder define_interface(std::string descriptor, std::vector<std::string> super_interfaces = {},
                                ApiStatus status = ApiStatus::kSupported, std::string note = {});
  // Returns an existing class for extension (e.g. the bridge adding methods
  // to a core class). Creates nothing if absent.
  ClassBuilder extend(std::string_view descriptor);

  const NativeClassSpec* find(std::string_view descriptor) const;
  bool contains(std::string_view descriptor) const { return find(descriptor) != nullptr; }

  struct Lookup {
    bool class_known = false;
    const NativeClassSpec* declaring = nullptr;
    const NativeMethodSpec* method = nullptr;
    const NativeFieldSpec* field = nullptr;
  };
  // Static lookup through registered superclasses and interfaces.
  Lookup lookup_method(std::string_view cls, std::string_view name, std::string_view sig) const;
  Lookup lookup_field(std::string_view cls, std::string_view name, std::string_view type) const;

  const std::map<std::string, std::unique_ptr<NativeClassSpec>, std::less<>>& classes() const { return classes_; }

  // Implements the native method `name sig` of the bytecode class `cls`.
  void bind(std::string_view cls, std::string_view name, std::string_view sig, NativeFn fn,
            ApiStatus status = ApiStatus::kSupported, std::string note = {});
  // Replaces the bytecode body of a method by a native implementation.
  void intrinsic(std::string_view cls, std::string_view name, std::string_view sig, NativeFn fn, std::string note = {});
  const NativeBinding* binding(std::string_view cls, std::string_view name, std::string_view sig) const;
  // Keyed by "Lpkg/Class;->name(sig)".
  const std::map<std::string, NativeBinding, std::less<>>& bindings() const { return bindings_; }

  void set_polymorphic_handler(PolymorphicFn fn) { polymorphic_ = std::move(fn); }
  const PolymorphicFn& polymorphic_handler() const { return polymorphic_; }
  void set_invoke_dynamic_handlers(InvokeDynamicHandlers h) { invoke_dynamic_ = std::move(h); }
  const InvokeDynamicHandlers& invoke_dynamic_handlers() const { return invoke_dynamic_; }

 private:
  PolymorphicFn polymorphic_;
  InvokeDynamicHandlers invoke_dynamic_;
  std::map<std::string, std::unique_ptr<NativeClassSpec>, std::less<>> classes_;
  std::map<std::string, NativeBinding, std::less<>> bindings_;
};

// Classes the VM itself implements (object model, monitors, threads,
// exceptions, reflection). With a boot class path (libcore mode, ADR-004),
// they keep their native implementation, completed by the methods, static
// fields and interfaces of their bytecode that have no native counterpart;
// every other class of the boot class path comes from its bytecode.
bool is_vm_core_class(std::string_view descriptor);

}  // namespace aa::rt
