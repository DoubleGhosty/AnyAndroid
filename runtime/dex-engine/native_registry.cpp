#include "dex-engine/native_registry.h"

#include <set>
#include <stdexcept>

#include "dex-engine/runtime.h"

namespace aa::rt {

const char* api_status_id(ApiStatus s) {
  switch (s) {
    case ApiStatus::kSupported: return "supported";
    case ApiStatus::kPartial: return "partial";
    case ApiStatus::kStub: return "stub";
    case ApiStatus::kUnsupported: return "unsupported";
    case ApiStatus::kInvestigate: return "investigate";
  }
  return "?";
}

const char* api_status_label(ApiStatus s) {
  switch (s) {
    case ApiStatus::kSupported: return "Supportée";
    case ApiStatus::kPartial: return "Partielle";
    case ApiStatus::kStub: return "Stub";
    case ApiStatus::kUnsupported: return "Non supportée";
    case ApiStatus::kInvestigate: return "À investiguer";
  }
  return "?";
}

ClassBuilder& ClassBuilder::method(std::string name, std::string sig, NativeFn fn, ApiStatus status, std::string note) {
  spec_->methods.push_back({std::move(name), std::move(sig), 0x1, std::move(fn), status, std::move(note)});
  return *this;
}

ClassBuilder& ClassBuilder::static_method(std::string name, std::string sig, NativeFn fn, ApiStatus status,
                                          std::string note) {
  spec_->methods.push_back({std::move(name), std::move(sig), 0x1 | 0x8, std::move(fn), status, std::move(note)});
  return *this;
}

ClassBuilder& ClassBuilder::ctor(std::string sig, NativeFn fn, ApiStatus status, std::string note) {
  spec_->methods.push_back({"<init>", std::move(sig), 0x1 | 0x10000, std::move(fn), status, std::move(note)});
  return *this;
}

ClassBuilder& ClassBuilder::bytecode_with_libcore(std::initializer_list<std::string_view> names) {
  for (std::string_view name : names) {
    bool found = false;
    for (auto& m : spec_->methods) {
      if (m.name == name) {
        m.bytecode_with_libcore = true;
        found = true;
      }
    }
    if (!found) throw std::logic_error("bytecode_with_libcore : méthode inconnue " + std::string(name));
  }
  return *this;
}

ClassBuilder& ClassBuilder::abstract_method(std::string name, std::string sig) {
  spec_->methods.push_back({std::move(name), std::move(sig), 0x1 | 0x400, nullptr, ApiStatus::kSupported, {}});
  return *this;
}

ClassBuilder& ClassBuilder::stub(std::string name, std::string sig, std::string note) {
  spec_->methods.push_back({std::move(name), std::move(sig), 0x1,
                            [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, ApiStatus::kStub, std::move(note)});
  return *this;
}

ClassBuilder& ClassBuilder::unsupported(std::string name, std::string sig, std::string note) {
  std::string api = spec_->descriptor + "->" + name + sig;
  std::string reason = note;
  spec_->methods.push_back({std::move(name), std::move(sig), 0x1,
                            [api, reason](Runtime& rt, NativeArgs&) -> uint64_t { rt.throw_unsupported(api, reason); },
                            ApiStatus::kUnsupported, std::move(note)});
  return *this;
}

ClassBuilder& ClassBuilder::static_field(std::string name, std::string type, std::function<uint64_t(Runtime&)> init,
                                         ApiStatus status, std::string note) {
  spec_->static_fields.push_back({std::move(name), std::move(type), 0x1 | 0x8 | 0x10, std::move(init), status,
                                  std::move(note)});
  return *this;
}

ClassBuilder& ClassBuilder::field(std::string name, std::string type, uint32_t access) {
  spec_->instance_fields.push_back({std::move(name), std::move(type), access & ~0x8u, nullptr, ApiStatus::kSupported, {}});
  return *this;
}

void NativeRegistry::bind(std::string_view cls, std::string_view name, std::string_view sig, NativeFn fn,
                          ApiStatus status, std::string note) {
  std::string key = std::string(cls) + "->" + std::string(name) + std::string(sig);
  if (!bindings_.emplace(key, NativeBinding{std::move(fn), status, std::move(note), false}).second) {
    throw std::logic_error("méthode native liée deux fois : " + key);
  }
}

void NativeRegistry::intrinsic(std::string_view cls, std::string_view name, std::string_view sig, NativeFn fn,
                               std::string note) {
  std::string key = std::string(cls) + "->" + std::string(name) + std::string(sig);
  if (!bindings_.emplace(key, NativeBinding{std::move(fn), ApiStatus::kSupported, std::move(note), true}).second) {
    throw std::logic_error("intrinsèque défini deux fois : " + key);
  }
}

const NativeBinding* NativeRegistry::binding(std::string_view cls, std::string_view name, std::string_view sig) const {
  std::string key;
  key.reserve(cls.size() + name.size() + sig.size() + 2);
  key.append(cls).append("->").append(name).append(sig);
  auto it = bindings_.find(key);
  return it == bindings_.end() ? nullptr : &it->second;
}

bool is_vm_core_class(std::string_view d) {
  // Only the classes whose objects carry VM state (peers). Pure Java helpers
  // (Modifier, Proxy, Member...) come from the boot class path.
  static const std::set<std::string, std::less<>> core = {
      "Ljava/lang/Object;",
      "Ljava/lang/String;",
      "Ljava/lang/Class;",
      "Ljava/lang/Throwable;",
      "Ljava/lang/Thread;",
      "Ljava/lang/ref/Reference;",
      "Ljava/lang/ref/WeakReference;",
      "Ljava/lang/ref/SoftReference;",
      "Ljava/lang/reflect/Array;",
      "Ljava/lang/reflect/InvocationTargetException;",
  };
  return core.count(d) != 0;
}

ClassBuilder NativeRegistry::define(std::string descriptor, std::string super, std::vector<std::string> interfaces,
                                    uint32_t access, ApiStatus status, std::string note) {
  auto spec = std::make_unique<NativeClassSpec>();
  spec->descriptor = descriptor;
  spec->super = std::move(super);
  spec->interfaces = std::move(interfaces);
  spec->access = access;
  spec->status = status;
  spec->note = std::move(note);
  auto* raw = spec.get();
  if (!classes_.emplace(descriptor, std::move(spec)).second) {
    throw std::logic_error("classe native définie deux fois : " + descriptor);
  }
  return ClassBuilder(raw);
}

ClassBuilder NativeRegistry::define_interface(std::string descriptor, std::vector<std::string> super_interfaces,
                                              ApiStatus status, std::string note) {
  return define(std::move(descriptor), "Ljava/lang/Object;", std::move(super_interfaces), 0x1 | 0x200 | 0x400, status,
                std::move(note));
}

ClassBuilder NativeRegistry::extend(std::string_view descriptor) {
  auto it = classes_.find(descriptor);
  if (it == classes_.end()) throw std::logic_error("classe native absente : " + std::string(descriptor));
  return ClassBuilder(it->second.get());
}

const NativeClassSpec* NativeRegistry::find(std::string_view descriptor) const {
  auto it = classes_.find(descriptor);
  return it == classes_.end() ? nullptr : it->second.get();
}

NativeRegistry::Lookup NativeRegistry::lookup_method(std::string_view cls, std::string_view name,
                                                     std::string_view sig) const {
  Lookup out;
  std::vector<std::string> pending = {std::string(cls)};
  std::set<std::string> seen;
  while (!pending.empty()) {
    std::string c = pending.front();
    pending.erase(pending.begin());
    if (!seen.insert(c).second) continue;
    const NativeClassSpec* spec = find(c);
    if (!spec) continue;
    out.class_known = true;
    for (const auto& m : spec->methods) {
      if (m.name == name && m.signature == sig) {
        out.declaring = spec;
        out.method = &m;
        return out;
      }
    }
    if (!spec->super.empty()) pending.push_back(spec->super);
    for (const auto& i : spec->interfaces) pending.push_back(i);
  }
  return out;
}

NativeRegistry::Lookup NativeRegistry::lookup_field(std::string_view cls, std::string_view name,
                                                    std::string_view type) const {
  Lookup out;
  std::string c(cls);
  for (int depth = 0; depth < 64 && !c.empty(); ++depth) {
    const NativeClassSpec* spec = find(c);
    if (!spec) break;
    out.class_known = true;
    for (const auto& f : spec->static_fields) {
      if (f.name == name && f.type == type) {
        out.declaring = spec;
        out.field = &f;
        return out;
      }
    }
    c = spec->super;
  }
  return out;
}

}  // namespace aa::rt
