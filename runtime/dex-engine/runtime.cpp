#include "dex-engine/runtime.h"

#include <algorithm>
#include <iterator>
#include <cstdio>
#include <deque>

#include "common/bytes.h"
#include "common/log.h"
#include "common/text.h"
#include "dex-engine/opcodes.h"

namespace aa::rt {

namespace {

struct LoadingGuard {
  std::unordered_set<std::string>& set;
  std::string key;
  LoadingGuard(std::unordered_set<std::string>& s, std::string k) : set(s), key(std::move(k)) { set.insert(key); }
  ~LoadingGuard() { set.erase(key); }
};

struct FramePop {
  std::vector<Frame>& frames;
  ~FramePop() { frames.pop_back(); }
};

bool looks_like_platform(std::string_view d) {
  for (std::string_view p : {"Landroid/", "Ljava/", "Ljavax/", "Ldalvik/", "Lcom/android/", "Lsun/", "Llibcore/"}) {
    if (text::starts_with(d, p)) return true;
  }
  return false;
}

std::string java(std::string_view descriptor) { return text::descriptor_to_java(descriptor); }

}  // namespace

Object::~Object() = default;

std::string Method::pretty() const { return (owner ? owner->descriptor : std::string("?")) + "->" + name + signature; }

Method* Class::declared_method(std::string_view n, std::string_view sig) const {
  for (const auto& m : methods) {
    if (m->name == n && m->signature == sig) return m.get();
  }
  return nullptr;
}

Field* Class::declared_field(std::string_view n, std::string_view type) const {
  for (const auto& f : fields) {
    if (f->name == n && (type.empty() || f->type == type)) return f.get();
  }
  return nullptr;
}

std::string Class::java_name() const {
  if (is_array()) {
    std::string s = descriptor;
    std::replace(s.begin(), s.end(), '/', '.');
    return s;
  }
  return java(descriptor);
}

const ClassAnnotations& Class::annotations() {
  if (annotations_) return *annotations_;
  annotations_ = std::make_unique<ClassAnnotations>();
  if (!dex_cache || is_array() || primitive) return *annotations_;
  const dex::DexFile& dex = *dex_cache->dex;
  ClassAnnotations& out = *annotations_;
  try {
    for (const dex::Annotation& a : dex.class_annotations(dex.class_def(class_def_idx))) {
      if (a.visibility != 2) continue;  // system annotations only
      const std::string& type = dex.type_descriptor(a.type_idx);
      auto value = [&](std::string_view name) -> const dex::EncodedValue* {
        for (const auto& [n, v] : a.elements) {
          if (dex.string(n) == name) return &v;
        }
        return nullptr;
      };
      using V = dex::EncodedValue;
      if (type == "Ldalvik/annotation/InnerClass;") {
        out.inner = true;
        const V* name = value("name");
        out.anonymous = !name || name->type == V::kNull;
        if (name && name->type == V::kString && name->bits < dex.num_strings()) out.inner_name = dex.string(uint32_t(name->bits));
        const V* flags = value("accessFlags");
        if (flags && flags->type == V::kInt) out.inner_flags = int32_t(flags->bits);
      } else if (type == "Ldalvik/annotation/EnclosingClass;") {
        const V* v = value("value");
        if (v && v->type == V::kType && v->bits < dex.num_types()) out.enclosing_class = dex.type_descriptor(uint32_t(v->bits));
      } else if (type == "Ldalvik/annotation/EnclosingMethod;") {
        const V* v = value("value");
        if (v && v->type == V::kMethod && v->bits < dex.num_methods()) out.enclosing_method = uint32_t(v->bits);
      } else if (type == "Ldalvik/annotation/MemberClasses;") {
        const V* v = value("value");
        if (v && v->type == V::kArray) {
          for (const V& e : v->array) {
            if (e.type == V::kType && e.bits < dex.num_types()) out.member_classes.push_back(dex.type_descriptor(uint32_t(e.bits)));
          }
        }
      }
    }
  } catch (const FormatError& e) {
    log::warn("runtime", "annotations illisibles pour " + java_name() + " : " + e.what());
    out = ClassAnnotations{};
  }
  return out;
}

std::string Class::simple_name() {
  if (component) return component->simple_name() + "[]";
  const ClassAnnotations& a = annotations();
  if (a.anonymous) return "";
  if (a.inner) return a.inner_name;
  std::string n = java_name();
  size_t dot = n.find_last_of('.');
  return dot == std::string::npos ? n : n.substr(dot + 1);
}

// --- NativeArgs --------------------------------------------------------------

uint32_t NativeArgs::raw(uint32_t i) const {
  if (i >= count_) rt_.throw_new("Ljava/lang/InternalError;", "argument natif " + std::to_string(i) + " absent");
  return regs_[i];
}

float NativeArgs::f32(uint32_t i) const {
  uint32_t b = raw(i);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

int64_t NativeArgs::i64(uint32_t i) const { return int64_t(uint64_t(raw(i)) | (uint64_t(raw(i + 1)) << 32)); }

double NativeArgs::f64(uint32_t i) const {
  uint64_t b = uint64_t(i64(i));
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}

Object* NativeArgs::obj(uint32_t i) const { return rt_.object(raw(i)); }
Object& NativeArgs::self() const { return rt_.nonnull(raw(0), "this"); }

std::u16string NativeArgs::str(uint32_t i) const {
  Handle h = raw(i);
  if (h == kNull) rt_.throw_npe("argument de type chaîne null");
  return rt_.string_value(h);
}

std::u16string NativeArgs::str_or(uint32_t i, std::u16string_view fallback) const {
  Handle h = raw(i);
  return h == kNull ? std::u16string(fallback) : rt_.string_value(h);
}

// --- Runtime -----------------------------------------------------------------

Runtime::Runtime(RuntimeOptions options) : options_(options) {
  slots_.emplace_back();  // index 0: null
  gc_object_threshold_ = options_.gc_min_objects;
  gc_byte_threshold_ = options_.gc_min_bytes;
  register_core_classes(registry_);
}

Runtime::~Runtime() { shutdown_threads(); }

std::vector<Class*> Runtime::loaded_classes() const {
  std::vector<Class*> out;
  for (const auto& [k, c] : classes_) out.push_back(c.get());
  return out;
}

void Runtime::reset_budget() {
  if (running_) running_->budget_used = 0;
}

// --- Roots held outside the heap --------------------------------------------------

GlobalRef::GlobalRef(Runtime& rt, Handle h) : rt_(&rt), h_(h) {
  if (h_) rt_->add_global(h_);
}

GlobalRef::GlobalRef(const GlobalRef& other) : rt_(other.rt_), h_(other.h_) {
  if (rt_ && h_) rt_->add_global(h_);
}

GlobalRef& GlobalRef::operator=(const GlobalRef& other) {
  if (this == &other) return *this;
  if (rt_ && h_) rt_->remove_global(h_);
  rt_ = other.rt_;
  h_ = other.h_;
  if (rt_ && h_) rt_->add_global(h_);
  return *this;
}

GlobalRef::~GlobalRef() {
  if (rt_ && h_) rt_->remove_global(h_);
}

void Runtime::add_global(Handle h) { globals_[h]++; }

void Runtime::remove_global(Handle h) {
  auto it = globals_.find(h);
  if (it != globals_.end() && --it->second == 0) globals_.erase(it);
}

size_t Runtime::add_root_provider(std::function<void(std::vector<Handle>&)> provider) {
  size_t id = next_root_provider_++;
  root_providers_[id] = std::move(provider);
  return id;
}

void Runtime::remove_root_provider(size_t id) { root_providers_.erase(id); }

void Runtime::add_dex(const dex::DexFile* dex) {
  auto cache = std::make_unique<DexCache>();
  cache->dex = dex;
  cache->types.resize(dex->num_types());
  cache->fields.resize(dex->num_fields());
  cache->methods.resize(dex->num_methods());
  cache->strings.resize(dex->num_strings());
  dex_caches_.push_back(std::move(cache));
}

void Runtime::add_boot_dex(const dex::DexFile* dex) {
  add_dex(dex);
  auto cache = std::move(dex_caches_.back());
  dex_caches_.pop_back();
  cache->boot = true;
  // Boot caches come first, in class path order.
  auto pos = std::find_if(dex_caches_.begin(), dex_caches_.end(), [](const auto& c) { return !c->boot; });
  dex_caches_.insert(pos, std::move(cache));
  boot_mode_ = true;
}

void Runtime::add_host_dex(const dex::DexFile* dex) {
  add_boot_dex(dex);  // last of the boot caches
  auto it = std::find_if(dex_caches_.begin(), dex_caches_.end(), [](const auto& c) { return !c->boot; });
  (*std::prev(it))->host = true;
}

Class* Runtime::lookup_class(std::string_view descriptor) {
  auto it = classes_.find(std::string(descriptor));
  if (it != classes_.end()) return it->second.get();
  return load_class(descriptor);
}

Class* Runtime::loaded_class(std::string_view descriptor) const {
  auto it = classes_.find(std::string(descriptor));
  return it == classes_.end() ? nullptr : it->second.get();
}

bool Runtime::in_boot_space(const Class* cls) {
  while (cls->component) cls = cls->component;
  return !cls->dex_cache || cls->dex_cache->boot;
}

bool Runtime::defined_in_boot_space(std::string_view descriptor) const {
  if (registry_.find(descriptor)) return true;
  for (const auto& cache : dex_caches_) {
    if (!cache->boot) break;
    if (cache->dex->find_class_def(descriptor)) return true;
  }
  return false;
}

Class* Runtime::lookup_boot_class(std::string_view descriptor) {
  size_t dims = descriptor.find_first_not_of('[');
  if (dims == std::string_view::npos) return nullptr;
  std::string_view base = descriptor.substr(dims);
  if (base.size() > 1) {
    if (Class* c = loaded_class(base)) {
      if (!in_boot_space(c)) return nullptr;
    } else if (!defined_in_boot_space(base)) {
      return nullptr;
    }
  }
  return lookup_class(descriptor);
}

Class* Runtime::define_class(const dex::DexFile* dex, std::string_view descriptor) {
  if (Class* c = loaded_class(descriptor)) return c;
  if (descriptor.size() < 3 || descriptor.front() != 'L' || descriptor.back() != ';') return nullptr;
  if (defined_in_boot_space(descriptor)) return lookup_class(descriptor);
  DexCache* cache = find_dex_cache(dex);
  if (!cache || cache->boot) return nullptr;
  auto idx = dex->find_class_def(descriptor);
  if (!idx) return nullptr;
  if (loading_.count(std::string(descriptor))) throw_new("Ljava/lang/ClassCircularityError;", java(descriptor));
  return define_from_dex(*cache, *idx);
}

std::vector<const dex::DexFile*> Runtime::app_dex_files() const {
  std::vector<const dex::DexFile*> out;
  for (const auto& cache : dex_caches_) {
    if (!cache->boot) out.push_back(cache->dex);
  }
  return out;
}

DexCache* Runtime::find_dex_cache(const dex::DexFile* dex) {
  for (auto& cache : dex_caches_) {
    if (cache->dex == dex) return cache.get();
  }
  return nullptr;
}

Class* Runtime::find_class(std::string_view descriptor) {
  if (Class* c = lookup_class(descriptor)) return c;
  std::string d(descriptor);
  size_t dims = d.find_first_not_of('[');
  std::string base = dims == std::string::npos ? d : d.substr(dims);
  if (looks_like_platform(base)) {
    const char* what = boot_mode_ ? "classe absente du chemin de classes d'amorçage et du pont : "
                                  : "classe de la plateforme non implémentée : ";
    if (unsupported_.insert(base).second) log::error("bridge", what + java(base), base);
    throw_new("Ljava/lang/NoClassDefFoundError;", std::string("[AnyAndroid] ") + what + java(base));
  }
  throw_new("Ljava/lang/NoClassDefFoundError;", java(d));
}

Class* Runtime::load_class(std::string_view descriptor) {
  if (descriptor.empty()) return nullptr;
  if (descriptor[0] == '[') return define_array(descriptor);
  if (descriptor.size() == 1) return define_primitive(descriptor[0]);
  if (descriptor.front() != 'L' || descriptor.back() != ';') return nullptr;
  if (loading_.count(std::string(descriptor))) {
    throw_new("Ljava/lang/ClassCircularityError;", java(descriptor));
  }
  // Parent-first delegation: VM core, boot class path, bridge, application.
  const NativeClassSpec* spec = registry_.find(descriptor);
  if (spec && (!boot_mode_ || is_vm_core_class(descriptor))) return define_from_registry(*spec);
  for (auto& cache : dex_caches_) {
    if (!cache->boot) break;
    if (auto idx = cache->dex->find_class_def(descriptor)) return define_from_dex(*cache, *idx);
  }
  if (spec) return define_from_registry(*spec);
  for (auto& cache : dex_caches_) {
    if (cache->boot) continue;
    if (auto idx = cache->dex->find_class_def(descriptor)) return define_from_dex(*cache, *idx);
  }
  return nullptr;
}

Class* Runtime::define_from_registry(const NativeClassSpec& spec) {
  LoadingGuard guard(loading_, spec.descriptor);
  auto cls = std::make_unique<Class>();
  cls->descriptor = spec.descriptor;
  cls->access = spec.access;
  cls->framework = true;
  cls->status = spec.status;
  if (!spec.super.empty()) cls->super = find_class(spec.super);
  for (const auto& i : spec.interfaces) cls->interfaces.push_back(find_class(i));
  cls->instance_slots = cls->super ? cls->super->instance_slots : 0;
  for (const auto& ms : spec.methods) {
    auto m = std::make_unique<Method>();
    m->owner = cls.get();
    m->name = ms.name;
    m->signature = ms.signature;
    m->access = ms.access;
    m->arg_words = dex::arg_words(ms.signature) + ((ms.access & dex::kAccStatic) ? 0 : 1);
    m->native = ms.fn;
    m->status = ms.status;
    m->note = ms.note;
    cls->methods.push_back(std::move(m));
  }
  for (const auto& fs : spec.static_fields) {
    auto f = std::make_unique<Field>();
    f->owner = cls.get();
    f->name = fs.name;
    f->type = fs.type;
    f->access = fs.access | dex::kAccStatic;
    f->slot = uint32_t(cls->statics.size());
    f->native_init = fs.init;
    cls->statics.resize(cls->statics.size() + (f->wide() ? 2 : 1));
    cls->fields.push_back(std::move(f));
  }
  for (const auto& fs : spec.instance_fields) {
    auto f = std::make_unique<Field>();
    f->owner = cls.get();
    f->name = fs.name;
    f->type = fs.type;
    f->access = fs.access;
    f->slot = cls->instance_slots;
    cls->instance_slots += f->wide() ? 2 : 1;
    cls->fields.push_back(std::move(f));
  }
  if (boot_mode_) merge_boot_bytecode(cls.get());
  Class* raw = cls.get();
  compute_ref_slots(raw);
  classes_.emplace(spec.descriptor, std::move(cls));
  return raw;
}

// A VM core class in libcore mode: the native implementation is completed by
// the bytecode of the same class in the boot class path (methods without a
// native counterpart, static fields, <clinit>, extra interfaces). Instance
// fields stay those of the native layout: bytecode touching others fails
// explicitly (NoSuchFieldError).
void Runtime::merge_boot_bytecode(Class* cls) {
  for (auto& cache : dex_caches_) {
    if (!cache->boot) break;
    auto idx = cache->dex->find_class_def(cls->descriptor);
    if (!idx) continue;
    const dex::DexFile& dex = *cache->dex;
    const dex::ClassDef& def = dex.class_def(*idx);
    cls->dex_cache = cache.get();
    cls->class_def_idx = *idx;
    cls->boot = true;
    try {
      if (def.interfaces_off) {
        for (uint32_t t : dex.type_list(def.interfaces_off)) {
          const std::string& idesc = dex.type_descriptor(t);
          bool known = false;
          for (Class* i : cls->interfaces) known |= i->descriptor == idesc;
          if (known) continue;
          Class* ic = lookup_class(idesc);
          if (ic && ic->is_interface()) cls->interfaces.push_back(ic);
        }
      }
      dex::ClassData cd = dex.class_data(def);
      for (const auto& ef : cd.static_fields) {
        const dex::FieldId& fid = dex.field(ef.field_idx);
        const std::string& name = dex.string(fid.name_idx);
        const std::string& type = dex.type_descriptor(fid.type_idx);
        if (cls->declared_field(name, type)) continue;
        auto f = std::make_unique<Field>();
        f->owner = cls;
        f->name = name;
        f->type = type;
        f->access = ef.access_flags | dex::kAccStatic;
        f->slot = uint32_t(cls->statics.size());
        cls->statics.resize(cls->statics.size() + (f->wide() ? 2 : 1));
        cls->fields.push_back(std::move(f));
      }
      auto add_method = [&](const dex::EncodedMethod& em) {
        const dex::MethodId& mid = dex.method(em.method_idx);
        const std::string& name = dex.string(mid.name_idx);
        const std::string& sig = dex.proto_signature(mid.proto_idx);
        if (cls->declared_method(name, sig)) return;
        auto m = std::make_unique<Method>();
        m->owner = cls;
        m->name = name;
        m->signature = sig;
        m->access = em.access_flags;
        m->arg_words = dex::arg_words(sig) + ((em.access_flags & dex::kAccStatic) ? 0 : 1);
        m->dex = &dex;
        m->method_idx = em.method_idx;
        m->code_off = em.code_off;
        if (const NativeBinding* b = registry_.binding(cls->descriptor, name, sig)) {
          if ((m->access & dex::kAccNative) || b->replaces_bytecode) {
            m->native = b->fn;
            m->status = b->status;
            m->note = b->note;
          }
        }
        if (!m->native && name == "<init>" && cls->descriptor == "Ljava/lang/String;") {
          // As in ART, String constructors are never run: each one maps to
          // the StringFactory method taking the same parameters.
          std::string params = sig.substr(0, sig.find(')') + 1);
          m->native = [params](Runtime& rt, NativeArgs& a) -> uint64_t {
            Class* factory = rt.find_class("Ljava/lang/StringFactory;");
            rt.ensure_initialized(factory);
            std::string want = params + "Ljava/lang/String;";
            Method* target = nullptr;
            for (const auto& fm : factory->methods) {
              if (fm->is_static() && fm->signature == want && text::starts_with(fm->name, "new")) {
                target = fm.get();
                break;
              }
            }
            if (!target) rt.throw_unsupported("Ljava/lang/String;-><init>" + params + "V", "aucune méthode StringFactory équivalente");
            std::vector<uint32_t> args;
            for (uint32_t i = 1; i < a.count(); ++i) args.push_back(a.raw(i));
            Handle made = Handle(rt.invoke(target, args.data(), uint32_t(args.size())));
            StringObject* src = rt.as_string(made);
            StringObject* self = rt.as_string(a.h(0));
            if (!src || !self) rt.throw_npe("StringFactory");
            rt.set_string_value(*self, src->value);
            return 0;
          };
          m->note = "redirigé vers StringFactory, comme ART";
        }
        cls->methods.push_back(std::move(m));
      };
      for (const auto& m : cd.direct_methods) add_method(m);
      for (const auto& m : cd.virtual_methods) add_method(m);
    } catch (const FormatError& e) {
      throw_new("Ljava/lang/ClassFormatError;", cls->java_name() + " : " + e.what());
    }
    return;
  }
}

void Runtime::compute_ref_slots(Class* cls) {
  if (cls->super) cls->ref_slots = cls->super->ref_slots;
  for (const auto& f : cls->fields) {
    if (!f->is_static() && f->is_reference()) cls->ref_slots.push_back(f->slot);
  }
}

Class* Runtime::define_from_dex(DexCache& cache, uint32_t idx) {
  const dex::DexFile& dex = *cache.dex;
  const dex::ClassDef& def = dex.class_def(idx);
  const std::string& desc = dex.type_descriptor(def.class_idx);
  LoadingGuard guard(loading_, desc);

  auto cls = std::make_unique<Class>();
  cls->descriptor = desc;
  cls->access = def.access_flags;
  cls->dex_cache = &cache;
  cls->class_def_idx = idx;
  cls->boot = cache.boot;
  if (def.superclass_idx == dex::kNoIndex) {
    throw_new("Ljava/lang/ClassFormatError;", "classe sans superclasse : " + java(desc));
  }
  const std::string& super_desc = dex.type_descriptor(def.superclass_idx);
  Class* super = lookup_class(super_desc);
  if (!super) {
    bool platform = looks_like_platform(super_desc);
    if (platform && unsupported_.insert(super_desc).second) {
      log::error("bridge", "superclasse de la plateforme non implémentée : " + java(super_desc), super_desc);
    }
    throw_new("Ljava/lang/NoClassDefFoundError;", java(desc) + " : superclasse introuvable " + java(super_desc) +
                                                       (platform ? " (non implémentée par AnyAndroid)" : ""));
  }
  if (super->is_interface()) throw_new("Ljava/lang/IncompatibleClassChangeError;", java(desc) + " étend une interface");
  if (super->is_array() || super->primitive) throw_new("Ljava/lang/ClassFormatError;", java(desc) + " : superclasse invalide");
  cls->super = super;
  cls->host = cache.host || super->host;

  try {
    if (def.interfaces_off) {
      for (uint32_t t : dex.type_list(def.interfaces_off)) {
        const std::string& idesc = dex.type_descriptor(t);
        Class* ic = lookup_class(idesc);
        if (!ic) ic = define_phantom_interface(idesc);
        if (!ic->is_interface()) throw_new("Ljava/lang/IncompatibleClassChangeError;", java(idesc) + " n'est pas une interface");
        cls->interfaces.push_back(ic);
      }
    }
    dex::ClassData cd = dex.class_data(def);
    cls->instance_slots = super->instance_slots;
    auto add_field = [&](const dex::EncodedField& ef, bool is_static) {
      const dex::FieldId& fid = dex.field(ef.field_idx);
      auto f = std::make_unique<Field>();
      f->owner = cls.get();
      f->name = dex.string(fid.name_idx);
      f->type = dex.type_descriptor(fid.type_idx);
      f->access = ef.access_flags | (is_static ? uint32_t(dex::kAccStatic) : 0u);
      uint32_t width = f->wide() ? 2 : 1;
      if (is_static) {
        f->slot = uint32_t(cls->statics.size());
        cls->statics.resize(cls->statics.size() + width);
      } else {
        f->slot = cls->instance_slots;
        cls->instance_slots += width;
      }
      cls->fields.push_back(std::move(f));
    };
    for (const auto& f : cd.static_fields) add_field(f, true);
    for (const auto& f : cd.instance_fields) add_field(f, false);
    auto add_method = [&](const dex::EncodedMethod& em) {
      const dex::MethodId& mid = dex.method(em.method_idx);
      auto m = std::make_unique<Method>();
      m->owner = cls.get();
      m->name = dex.string(mid.name_idx);
      m->signature = dex.proto_signature(mid.proto_idx);
      m->access = em.access_flags;
      m->arg_words = dex::arg_words(m->signature) + ((em.access_flags & dex::kAccStatic) ? 0 : 1);
      m->dex = &dex;
      m->method_idx = em.method_idx;
      m->code_off = em.code_off;
      // Native methods of the boot class path and VM intrinsics.
      if (const NativeBinding* b = registry_.binding(desc, m->name, m->signature)) {
        if ((m->access & dex::kAccNative) || b->replaces_bytecode) {
          m->native = b->fn;
          m->status = b->status;
          m->note = b->note;
        }
      }
      cls->methods.push_back(std::move(m));
    };
    for (const auto& m : cd.direct_methods) add_method(m);
    for (const auto& m : cd.virtual_methods) add_method(m);
  } catch (const FormatError& e) {
    throw_new("Ljava/lang/ClassFormatError;", java(desc) + " : " + e.what());
  }

  Class* raw = cls.get();
  compute_ref_slots(raw);
  classes_.emplace(desc, std::move(cls));
  log::trace("linker", "classe chargée : " + java(desc));
  return raw;
}

Class* Runtime::define_array(std::string_view descriptor) {
  size_t dims = descriptor.find_first_not_of('[');
  if (dims == std::string_view::npos || dims > 255) return nullptr;
  Class* component = lookup_class(descriptor.substr(1));
  if (!component || component->primitive == 'V') return nullptr;
  auto cls = std::make_unique<Class>();
  cls->descriptor = std::string(descriptor);
  cls->access = dex::kAccPublic | dex::kAccFinal | dex::kAccAbstract;
  cls->framework = true;
  cls->super = find_class("Ljava/lang/Object;");
  cls->interfaces = {find_class("Ljava/lang/Cloneable;"), find_class("Ljava/io/Serializable;")};
  cls->component = component;
  Class* raw = cls.get();
  classes_.emplace(raw->descriptor, std::move(cls));
  return raw;
}

Class* Runtime::define_primitive(char c) {
  if (std::string_view("ZBSCIJFDV").find(c) == std::string_view::npos) return nullptr;
  auto cls = std::make_unique<Class>();
  cls->descriptor = std::string(1, c);
  cls->access = dex::kAccPublic | dex::kAccFinal | dex::kAccAbstract;
  cls->framework = true;
  cls->primitive = c;
  Class* raw = cls.get();
  classes_.emplace(raw->descriptor, std::move(cls));
  return raw;
}

Class* Runtime::define_phantom_interface(std::string_view descriptor) {
  auto cls = std::make_unique<Class>();
  cls->descriptor = std::string(descriptor);
  cls->access = dex::kAccPublic | dex::kAccInterface | dex::kAccAbstract;
  cls->framework = looks_like_platform(descriptor);
  cls->phantom = true;
  cls->status = ApiStatus::kUnsupported;
  cls->super = find_class("Ljava/lang/Object;");
  phantoms_.push_back(cls->descriptor);
  log::warn("linker", "interface inconnue remplacée par une interface vide : " + java(descriptor));
  Class* raw = cls.get();
  classes_.emplace(raw->descriptor, std::move(cls));
  return raw;
}

void Runtime::ensure_initialized(Class* cls) {
  if (cls->state == Class::State::kInitialized) return;
  if (cls->state == Class::State::kInitializing) {
    if (cls->init_thread == running_) return;  // recursive initialisation by the same thread
    // Another thread runs <clinit>: wait for it (JLS 12.4.2).
    park(running_, threads_cv_, [cls] { return cls->state != Class::State::kInitializing; }, 0);
    if (cls->state == Class::State::kInitialized) return;
  }
  if (cls->state == Class::State::kFailed) {
    throw_new("Ljava/lang/NoClassDefFoundError;", "initialisation précédemment échouée : " + cls->java_name());
  }
  cls->state = Class::State::kInitializing;
  cls->init_thread = running_;
  try {
    if (cls->super) ensure_initialized(cls->super);
    initialize_statics(cls);
    if (Method* clinit = cls->declared_method("<clinit>", "()V")) invoke(clinit, nullptr, 0);
  } catch (const JavaThrow& t) {
    cls->state = Class::State::kFailed;
    threads_cv_.notify_all();
    // Later uses only see NoClassDefFoundError: keep the original cause.
    log::warn("linker", "échec de l'initialisation de " + cls->java_name() + " :\n" + describe_throwable(t.exception));
    Class* thrown = class_of(t.exception);
    if (thrown && is_assignable(thrown, find_class("Ljava/lang/Error;"))) throw;
    Handle err = make_throwable(find_class("Ljava/lang/ExceptionInInitializerError;"), u"", false);
    if (auto* p = peer<ThrowablePeer>(err)) p->cause = t.exception;
    throw JavaThrow{err};
  } catch (const ThreadKill&) {
    cls->state = Class::State::kFailed;
    threads_cv_.notify_all();
    throw;
  }
  cls->state = Class::State::kInitialized;
  threads_cv_.notify_all();
}

void Runtime::initialize_statics(Class* cls) {
  auto store = [&](Field& f, uint64_t v) {
    cls->statics.at(f.slot) = uint32_t(v);
    if (f.wide()) cls->statics.at(f.slot + 1) = uint32_t(v >> 32);
  };
  if (cls->framework) {
    for (auto& f : cls->fields) {
      if (f->is_static() && f->native_init) store(*f, f->native_init(*this));
    }
    if (!cls->dex_cache) return;  // merged VM core classes continue with their bytecode statics
  }
  if (!cls->dex_cache) return;
  const dex::DexFile& dex = *cls->dex_cache->dex;
  std::vector<dex::EncodedValue> values;
  std::vector<Field*> targets;  // static fields in class_data order
  try {
    const dex::ClassDef& def = dex.class_def(cls->class_def_idx);
    values = dex.encoded_array(def.static_values_off);
    for (const auto& ef : dex.class_data(def).static_fields) {
      const dex::FieldId& fid = dex.field(ef.field_idx);
      Field* f = cls->declared_field(dex.string(fid.name_idx), dex.type_descriptor(fid.type_idx));
      targets.push_back(f && f->is_static() && !f->native_init ? f : nullptr);
    }
  } catch (const FormatError& e) {
    throw_new("Ljava/lang/ClassFormatError;", cls->java_name() + " : " + e.what());
  }
  for (size_t i = 0; i < values.size() && i < targets.size(); ++i) {
    if (!targets[i]) continue;
    Field* f = targets[i];
    const dex::EncodedValue& v = values[i];
    switch (v.type) {
      case dex::EncodedValue::kByte:
      case dex::EncodedValue::kShort:
      case dex::EncodedValue::kChar:
      case dex::EncodedValue::kInt:
      case dex::EncodedValue::kBoolean:
      case dex::EncodedValue::kFloat:
      case dex::EncodedValue::kLong:
      case dex::EncodedValue::kDouble: store(*f, v.bits); break;
      case dex::EncodedValue::kString:
        if (v.bits >= dex.num_strings()) throw_new("Ljava/lang/ClassFormatError;", "valeur statique invalide");
        store(*f, resolve_string(*cls->dex_cache, uint32_t(v.bits)));
        break;
      case dex::EncodedValue::kType:
        if (v.bits >= dex.num_types()) throw_new("Ljava/lang/ClassFormatError;", "valeur statique invalide");
        store(*f, class_mirror(resolve_type(*cls->dex_cache, uint32_t(v.bits))));
        break;
      case dex::EncodedValue::kNull: store(*f, 0); break;
      default:
        log::warn("linker", "valeur statique initiale non prise en charge pour " + cls->java_name() + "." + f->name);
    }
  }
}

bool Runtime::is_assignable(const Class* from, const Class* to) const {
  if (!from || !to) return false;
  if (from == to) return true;
  if (from->primitive || to->primitive) return false;
  if (to->descriptor == "Ljava/lang/Object;") return true;
  if (from->is_array()) {
    if (to->is_array()) {
      if (from->component->primitive || to->component->primitive) return from->component == to->component;
      return is_assignable(from->component, to->component);
    }
    return to->descriptor == "Ljava/lang/Cloneable;" || to->descriptor == "Ljava/io/Serializable;";
  }
  if (to->is_interface()) {
    std::vector<const Class*> pending;
    for (const Class* c = from; c; c = c->super) {
      for (const Class* i : c->interfaces) pending.push_back(i);
    }
    if (from->is_interface()) pending.push_back(from);
    std::unordered_set<const Class*> seen;
    while (!pending.empty()) {
      const Class* i = pending.back();
      pending.pop_back();
      if (i == to) return true;
      if (!seen.insert(i).second) continue;
      for (const Class* s : i->interfaces) pending.push_back(s);
    }
    return false;
  }
  for (const Class* c = from->super; c; c = c->super) {
    if (c == to) return true;
  }
  return false;
}

bool Runtime::instance_of(Handle h, const Class* to) {
  Object* o = object(h);
  return o && is_assignable(o->klass, to);
}

Class* Runtime::class_of(Handle h) {
  Object* o = object(h);
  return o ? o->klass : nullptr;
}

bool Runtime::is_valid_handle(Handle h) const {
  uint32_t idx = h & 0xFFFFFF;
  return idx != 0 && idx < slots_.size() && slots_[idx].obj && slots_[idx].gen == (h >> 24);
}

Object* Runtime::object(Handle h) {
  if (h == kNull) return nullptr;
  if (!is_valid_handle(h)) throw_new("Ljava/lang/VerifyError;", "référence invalide ou périmée " + hex(h));
  return slots_[h & 0xFFFFFF].obj.get();
}

Handle Runtime::store(std::unique_ptr<Object> obj, size_t bytes) {
  if (live_objects_ >= options_.max_objects && !in_gc_ && running_) {
    collect_garbage();  // last resort before OutOfMemoryError
  }
  if (live_objects_ >= options_.max_objects || (free_slots_.empty() && slots_.size() > 0xFFFFFF)) {
    if (live_objects_ > options_.max_objects + 1024) throw std::runtime_error("tas épuisé");
    throw_new("Ljava/lang/OutOfMemoryError;", "limite d'objets du prototype atteinte (" + std::to_string(live_objects_) + ")");
  }
  uint32_t idx;
  if (!free_slots_.empty()) {
    idx = free_slots_.back();
    free_slots_.pop_back();
  } else {
    idx = uint32_t(slots_.size());
    slots_.emplace_back();
    // Spread initial generations so that handles do not look like small
    // integers or flag bytes to the conservative stack scan.
    slots_.back().gen = uint8_t(1 + (idx * 97u + 61u) % 255u);
  }
  slots_[idx].obj = std::move(obj);
  slots_[idx].bytes = uint32_t(std::min<size_t>(bytes, UINT32_MAX));
  ++live_objects_;
  heap_bytes_ += bytes;
  if (live_objects_ >= gc_object_threshold_ || heap_bytes_ >= gc_byte_threshold_) gc_requested_ = true;
  return (uint32_t(slots_[idx].gen) << 24) | idx;
}

Object& Runtime::nonnull(Handle h, std::string_view what) {
  Object* o = object(h);
  if (!o) throw_npe(what);
  return *o;
}

Handle Runtime::alloc(Class* cls) {
  if (cls->is_interface() || cls->is_abstract() || cls->primitive || cls->is_array()) {
    throw_new("Ljava/lang/InstantiationError;", cls->java_name());
  }
  std::unique_ptr<Object> o;
  if (cls->descriptor == "Ljava/lang/String;") {
    o = std::make_unique<StringObject>();
  } else {
    o = std::make_unique<Object>();
  }
  o->klass = cls;
  o->fields.assign(cls->instance_slots, 0);
  return store(std::move(o), 32 + 4 * size_t(cls->instance_slots));
}

Handle Runtime::new_string(std::u16string value) {
  if (!string_class_) string_class_ = find_class("Ljava/lang/String;");
  auto s = std::make_unique<StringObject>();
  s->klass = string_class_;
  s->fields.assign(string_class_->instance_slots, 0);
  size_t bytes = 48 + 2 * value.size();
  set_string_value(*s, std::move(value));
  return store(std::move(s), bytes);
}

void Runtime::set_string_value(StringObject& s, std::u16string value) {
  s.value = std::move(value);
  if (string_count_slot_ == -2) {
    Field* f = s.klass->declared_field("count", "I");
    string_count_slot_ = f && !f->is_static() ? int64_t(f->slot) : -1;
  }
  if (string_count_slot_ < 0 || size_t(string_count_slot_) >= s.fields.size()) return;
  // ART compresses strings made only of ASCII characters 1..0x7F.
  bool compressed = std::all_of(s.value.begin(), s.value.end(), [](char16_t c) { return c - 1u < 0x7Fu; });
  s.fields[size_t(string_count_slot_)] = (uint32_t(s.value.size()) << 1) | (compressed ? 0u : 1u);
}

Handle Runtime::new_string_utf8(std::string_view utf8) { return new_string(text::utf8_to_utf16(utf8)); }

Handle Runtime::intern(const std::u16string& value) {
  auto it = interned_.find(value);
  if (it != interned_.end()) return it->second;
  Handle h = new_string(value);
  interned_.emplace(value, h);
  return h;
}

Handle Runtime::new_array(Class* ac, int32_t length) {
  if (!ac->is_array()) throw_new("Ljava/lang/InternalError;", ac->java_name() + " n'est pas un type tableau");
  if (length < 0) throw_new("Ljava/lang/NegativeArraySizeException;", std::to_string(length));
  if (uint32_t(length) > options_.max_array_length) throw_new("Ljava/lang/OutOfMemoryError;", "tableau trop grand");
  auto a = std::make_unique<ArrayObject>();
  a->klass = ac;
  switch (ac->component->primitive) {
    case 'Z': case 'B': a->width = 1; break;
    case 'C': case 'S': a->width = 2; break;
    case 'J': case 'D': a->width = 8; break;
    default: a->width = 4;
  }
  a->length = uint32_t(length);
  a->data.assign(size_t(length) * a->width, 0);
  size_t bytes = 48 + a->data.size();
  return store(std::move(a), bytes);
}

Handle Runtime::new_array(std::string_view descriptor, int32_t length) { return new_array(find_class(descriptor), length); }

ArrayObject& Runtime::array(Handle h, std::string_view what) {
  Object& o = nonnull(h, what);
  auto* a = dynamic_cast<ArrayObject*>(&o);
  if (!a) throw_new("Ljava/lang/ClassCastException;", o.klass->java_name() + " n'est pas un tableau");
  return *a;
}

StringObject* Runtime::as_string(Handle h) { return dynamic_cast<StringObject*>(object(h)); }

std::u16string Runtime::string_value(Handle h) {
  if (h == kNull) throw_npe("chaîne null");
  if (StringObject* s = as_string(h)) return s->value;
  Handle r = Handle(call_virtual(h, "toString", "()Ljava/lang/String;"));
  StringObject* s = as_string(r);
  return s ? s->value : u"null";
}

std::string Runtime::string_utf8(Handle h) { return text::utf16_to_utf8(string_value(h)); }

Handle Runtime::class_mirror(Class* cls) {
  if (cls->mirror == kNull) {
    Handle h = alloc(find_class("Ljava/lang/Class;"));
    auto p = std::make_unique<ClassPeer>();
    p->cls = cls;
    object(h)->peer = std::move(p);
    cls->mirror = h;
  }
  return cls->mirror;
}

Class* Runtime::mirror_class(Handle h) {
  auto* p = peer<ClassPeer>(h);
  return p ? p->cls : nullptr;
}

// --- Invocation --------------------------------------------------------------

namespace {
struct MonitorGuard {
  Runtime& rt;
  Handle h;
  MonitorGuard(Runtime& r, Handle obj) : rt(r), h(obj) { rt.monitor_enter(h); }
  ~MonitorGuard() {
    try {
      rt.monitor_exit(h);
    } catch (...) {
    }
  }
};
}  // namespace

uint64_t Runtime::invoke(Method* m, const uint32_t* args, uint32_t nargs) {
  VmThread* t = running_;
  if (!t) throw std::logic_error("appel au runtime hors d'un Runtime::Scope");
  if (nargs != m->arg_words) {
    throw_new("Ljava/lang/IncompatibleClassChangeError;", "nombre d'arguments incohérent pour " + m->pretty());
  }
  if (t->frames.size() >= options_.max_depth) {
    throw_new("Ljava/lang/StackOverflowError;", "profondeur d'appel supérieure à " + std::to_string(options_.max_depth));
  }
  stats_.invocations++;
  if (options_.profile) m->profile_calls++;
  // ACC_SYNCHRONIZED is honoured by the VM; javac/d8 "declared synchronized"
  // methods already contain explicit monitor instructions.
  std::unique_ptr<MonitorGuard> sync;
  if (m->access & dex::kAccSynchronized) {
    sync = std::make_unique<MonitorGuard>(*this, m->is_static() ? class_mirror(m->owner) : args[0]);
  }
  if (options_.service_trace && !m->is_static() && nargs > 0) {
    trace_service_call(t, m, args[0], m->code_off != 0 || m->native);
  }
  if (m->native) {
    note_native_call(*m);
    t->frames.push_back({m, kNativePc, args, nargs});
    FramePop pop{t->frames};
    NativeArgs a(*this, args, nargs);
    return m->native(*this, a);
  }
  if (m->jni_fn) return invoke_jni(m, args, nargs);
  if (m->access & dex::kAccNative) {
    std::string api = m->pretty();
    if (m->owner->boot) {
      if (unsupported_.insert(api).second) log::error("libcore", "méthode native de la plateforme non implémentée : " + api, api);
      throw_new("Ljava/lang/UnsatisfiedLinkError;", "[AnyAndroid] méthode native de la plateforme non implémentée : " + api);
    }
    if (unsupported_.insert(api).second) log::error("bridge", "méthode JNI appelée (code natif non pris en charge) : " + api, api);
    throw_new("Ljava/lang/UnsatisfiedLinkError;", "[AnyAndroid] méthode native JNI non prise en charge : " + api);
  }
  if (m->code_off == 0) {
    // ADR-006: host classes (system services) implement only the part of the
    // framework interfaces they need; the rest answers a neutral value.
    Object* self = !m->is_static() && nargs > 0 ? object(args[0]) : nullptr;
    if (self && self->klass->host) {
      std::string api = self->klass->java_name() + " : " + m->pretty();
      if (unsupported_.insert(api).second) log::warn("services", "méthode non implémentée par l'hôte AnyAndroid : " + api, m->pretty());
      stats_.native_calls++;
      return 0;
    }
    throw_new("Ljava/lang/AbstractMethodError;", m->pretty());
  }
  return execute(m, args, nargs);
}

void Runtime::trace_service_call(VmThread* t, Method* m, Handle self, bool implemented) {
  Object* o = object(self);
  if (!o || !o->klass->host || m->name == "<init>") return;
  // Only the framework's calls: not the host's own, nor the Binder plumbing
  // inherited from the boot class path (queryLocalInterface, asBinder...).
  if (!t->frames.empty() && t->frames.back().method->owner->host) return;
  if (!m->owner->host && implemented) return;
  std::string cls = o->klass->java_name();
  cls = cls.substr(cls.find_last_of('.') + 1);
  std::string line = "service : " + cls + "." + m->name + (implemented ? "" : " (non implémentée)") + "\n";
  std::fwrite(line.data(), 1, line.size(), options_.service_trace);
  std::fflush(options_.service_trace);
}

void Runtime::note_native_call(Method& m) {
  stats_.native_calls++;
  if (options_.trace) log::write(log::Level::Trace, "bridge", std::string("appel (") + api_status_id(m.status) + ")", m.pretty());
  if (m.status == ApiStatus::kStub) {
    stubs_used_.insert(m.pretty());
    if (!m.reported) {
      m.reported = true;
      log::warn("bridge", "API simulée sans effet : " + m.pretty() + (m.note.empty() ? "" : " (" + m.note + ")"));
    }
  } else if (m.status == ApiStatus::kPartial && !m.reported) {
    m.reported = true;
    log::debug("bridge", "API partiellement prise en charge : " + m.pretty() + (m.note.empty() ? "" : " (" + m.note + ")"));
  }
}

uint64_t Runtime::invoke_polymorphic(Class* declared, const std::string& name, const std::string& callsite,
                                     const uint32_t* args, uint32_t count) {
  const PolymorphicFn& fn = registry_.polymorphic_handler();
  std::string api = declared->descriptor + "->" + name + "(polymorphe)" + callsite;
  if (!fn) throw_unsupported(api, "invoke-polymorphic sans gestionnaire");
  stats_.native_calls++;
  return fn(*this, declared, name, callsite, args, count);
}

uint64_t Runtime::field_cookie(Field* f) {
  auto it = field_cookie_of_.find(f);
  if (it != field_cookie_of_.end()) return it->second;
  field_cookies_.push_back(f);
  uint64_t cookie = field_cookies_.size();  // 0 stays invalid
  field_cookie_of_.emplace(f, cookie);
  return cookie;
}

Field* Runtime::field_from_cookie(uint64_t cookie) {
  if (cookie == 0 || cookie > field_cookies_.size()) {
    throw_new("Ljava/lang/IllegalArgumentException;", "identifiant de champ invalide " + std::to_string(cookie));
  }
  return field_cookies_[cookie - 1];
}

Method* Runtime::resolve_virtual(Class* cls, std::string_view name, std::string_view sig) {
  std::string key = std::string(name) + std::string(sig);
  auto it = cls->vcache.find(key);
  if (it != cls->vcache.end()) return it->second;
  auto concrete = [](const Method* m) { return !m->is_abstract() || (m->access & dex::kAccNative); };
  Method* found = nullptr;
  Method* abstract_decl = nullptr;
  for (Class* c = cls; c && !found; c = c->super) {
    Method* m = c->declared_method(name, sig);
    if (!m || m->is_static() || m->name == "<init>") continue;
    if (concrete(m)) {
      found = m;
    } else if (!abstract_decl) {
      abstract_decl = m;
    }
  }
  if (!found) {
    // Default methods and abstract declarations inherited from interfaces.
    std::deque<Class*> pending;
    for (Class* c = cls; c; c = c->super) {
      for (Class* i : c->interfaces) pending.push_back(i);
    }
    if (cls->is_interface()) pending.push_back(cls);
    std::unordered_set<Class*> seen;
    while (!pending.empty() && !found) {
      Class* i = pending.front();
      pending.pop_front();
      if (!seen.insert(i).second) continue;
      if (Method* m = i->declared_method(name, sig); m && !m->is_static()) {
        if (concrete(m)) {
          found = m;
        } else if (!abstract_decl) {
          abstract_decl = m;
        }
      }
      for (Class* s : i->interfaces) pending.push_back(s);
    }
  }
  Method* result = found ? found : abstract_decl;
  if (result) cls->vcache.emplace(std::move(key), result);
  return result;
}

Method* Runtime::find_static(Class* cls, std::string_view name, std::string_view sig) {
  for (Class* c = cls; c; c = c->super) {
    if (Method* m = c->declared_method(name, sig); m && m->is_static()) return m;
  }
  return nullptr;
}

uint64_t Runtime::call_virtual(Handle receiver, std::string_view name, std::string_view sig,
                               std::vector<uint32_t> extra_args) {
  Object& o = nonnull(receiver, "this");
  Method* m = resolve_virtual(o.klass, name, sig);
  if (!m) missing_method(o.klass, name, sig, false);
  std::vector<uint32_t> args;
  args.reserve(extra_args.size() + 1);
  args.push_back(receiver);
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  return invoke(m, args.data(), uint32_t(args.size()));
}

uint64_t Runtime::call_static(std::string_view cls_desc, std::string_view name, std::string_view sig,
                              std::vector<uint32_t> args) {
  Class* cls = find_class(cls_desc);
  ensure_initialized(cls);
  Method* m = find_static(cls, name, sig);
  if (!m) missing_method(cls, name, sig, true);
  return invoke(m, args.data(), uint32_t(args.size()));
}

Class* Runtime::first_framework_ancestor(Class* cls) {
  for (Class* c = cls; c; c = c->super) {
    if (c->framework && !c->is_array()) return c;
  }
  return nullptr;
}

void Runtime::missing_method(Class* referenced, std::string_view name, std::string_view sig, bool is_static) {
  if (Class* fw = first_framework_ancestor(referenced)) {
    throw_unsupported(fw->descriptor + "->" + std::string(name) + std::string(sig),
                      is_static ? "méthode statique absente du pont" : "méthode absente du pont");
  }
  throw_new("Ljava/lang/NoSuchMethodError;", referenced->java_name() + "." + std::string(name) + std::string(sig));
}

// --- Resolution ----------------------------------------------------------------

Class* Runtime::resolve_type(DexCache& cache, uint32_t type_idx) {
  Class*& slot = cache.types.at(type_idx);
  if (!slot) slot = find_class(cache.dex->type_descriptor(type_idx));
  return slot;
}

Handle Runtime::resolve_string(DexCache& cache, uint32_t string_idx) {
  Handle& slot = cache.strings.at(string_idx);
  if (slot == kNull) slot = intern(text::utf8_to_utf16(cache.dex->string(string_idx)));
  return slot;
}

Field* Runtime::resolve_field(DexCache& cache, uint32_t field_idx, bool is_static) {
  Field* f = cache.fields.at(field_idx);
  if (!f) {
    const dex::FieldId& fid = cache.dex->field(field_idx);
    Class* cls = resolve_type(cache, fid.class_idx);
    const std::string& name = cache.dex->string(fid.name_idx);
    const std::string& type = cache.dex->type_descriptor(fid.type_idx);
    for (Class* c = cls; c && !f; c = c->super) f = c->declared_field(name, type);
    if (!f && is_static) {
      std::deque<Class*> pending;
      for (Class* c = cls; c; c = c->super) {
        for (Class* i : c->interfaces) pending.push_back(i);
      }
      while (!pending.empty() && !f) {
        Class* i = pending.front();
        pending.pop_front();
        f = i->declared_field(name, type);
        for (Class* s : i->interfaces) pending.push_back(s);
      }
    }
    if (!f) {
      if (Class* fw = first_framework_ancestor(cls)) {
        throw_unsupported(fw->descriptor + "->" + name + ":" + type, "champ absent du pont");
      }
      throw_new("Ljava/lang/NoSuchFieldError;", cls->java_name() + "." + name);
    }
    cache.fields[field_idx] = f;
  }
  if (f->is_static() != is_static) {
    throw_new("Ljava/lang/IncompatibleClassChangeError;", "champ " + f->owner->java_name() + "." + f->name +
                                                               (is_static ? " n'est pas statique" : " est statique"));
  }
  return f;
}

PreparedCode& Runtime::prepared(Method* m) {
  if (!m->code) {
    if (m->code_failed) throw_new("Ljava/lang/VerifyError;", m->pretty());
    try {
      m->code = prepare_code(*m->dex, m->dex->code_item(m->code_off));
    } catch (const FormatError& e) {
      m->code_failed = true;
      log::error("verifier", m->pretty() + " rejetée : " + e.what());
      throw_new("Ljava/lang/VerifyError;", m->pretty() + " : " + e.what());
    }
  }
  return *m->code;
}

std::string Runtime::disassemble(Method* m) {
  if (m->native) return "    (implémentation native AnyAndroid)\n";
  if (m->code_off == 0) return "    (abstraite)\n";
  std::string out;
  dex::CodeItem item = m->dex->code_item(m->code_off);
  std::span<const uint16_t> insns(item.insns);
  out += "    .registers " + std::to_string(item.registers_size) + "  (ins " + std::to_string(item.ins_size) + ")\n";
  for (uint32_t pc = 0; pc < insns.size();) {
    if (uint32_t pu = dex::payload_units(insns, pc)) {
      char buf[48];
      std::snprintf(buf, sizeof buf, "    %04x: <données %u unités>\n", pc, pu);
      out += buf;
      pc += pu;
      continue;
    }
    dex::Decoded d = dex::decode(insns, pc);
    char buf[16];
    std::snprintf(buf, sizeof buf, "    %04x: ", pc);
    out += buf + dex::disassemble(m->dex, d, pc) + "\n";
    pc += d.units;
  }
  return out;
}

// --- Exceptions ------------------------------------------------------------------

Handle Runtime::make_throwable(Class* cls, std::u16string_view message, bool has_message) {
  Handle h = alloc(cls);
  auto p = std::make_unique<ThrowablePeer>();
  p->message = std::u16string(message);
  p->has_message = has_message;
  p->stack = capture_stack();
  object(h)->peer = std::move(p);
  return h;
}

void Runtime::throw_new(std::string_view descriptor, std::u16string_view message) {
  Class* cls = lookup_class(descriptor);
  if (!cls) cls = find_class("Ljava/lang/Error;");
  throw JavaThrow{make_throwable(cls, message, true)};
}

void Runtime::throw_new(std::string_view descriptor, std::string_view utf8_message) {
  Class* cls = lookup_class(descriptor);
  if (!cls) cls = find_class("Ljava/lang/Error;");
  throw JavaThrow{make_throwable(cls, text::utf8_to_utf16(utf8_message), !utf8_message.empty())};
}

void Runtime::throw_npe(std::string_view what) { throw_new("Ljava/lang/NullPointerException;", what); }

void Runtime::throw_unsupported(std::string_view api, std::string_view reason) {
  std::string a(api);
  if (unsupported_.insert(a).second) {
    log::error("bridge", "API non implémentée appelée : " + a + (reason.empty() ? "" : " — " + std::string(reason)), a);
  }
  throw_new("Ljava/lang/NoSuchMethodError;", "[AnyAndroid] API non implémentée : " + a);
}

std::vector<std::string> Runtime::capture_stack() const {
  std::vector<std::string> out;
  if (!running_) return out;
  const std::vector<Frame>& frames_ = running_->frames;
  Class* throwable = nullptr;
  auto it = classes_.find("Ljava/lang/Throwable;");
  if (it != classes_.end()) throwable = it->second.get();
  bool skipping = true;
  for (auto f = frames_.rbegin(); f != frames_.rend() && out.size() < 64; ++f) {
    // Like fillInStackTrace(), omit the constructors of the throwable itself.
    if (skipping && f->method->name == "<init>" && is_assignable(f->method->owner, throwable)) continue;
    skipping = false;
    std::string line = f->method->owner->java_name() + "." + f->method->name;
    if (f->pc == kNativePc) {
      line += "(Native Method)";
    } else {
      char buf[24];
      std::snprintf(buf, sizeof buf, "(pc 0x%04x)", f->pc);
      line += buf;
    }
    out.push_back(std::move(line));
  }
  return out;
}

std::string Runtime::describe_throwable(Handle h) {
  std::string out;
  for (int depth = 0; h != kNull && depth < 8; ++depth) {
    Object* o = object(h);
    if (!o) break;
    if (depth > 0) out += "Caused by: ";
    out += o->klass->java_name();
    auto* p = dynamic_cast<ThrowablePeer*>(o->peer.get());
    if (p && p->has_message) out += ": " + text::utf16_to_utf8(p->message);
    out += "\n";
    if (p) {
      for (const auto& line : p->stack) out += "    at " + line + "\n";
      h = p->cause;
    } else {
      break;
    }
  }
  return out;
}

}  // namespace aa::rt
