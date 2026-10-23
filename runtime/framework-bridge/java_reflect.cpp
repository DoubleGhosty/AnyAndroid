// java.lang.reflect and the reflective parts of java.lang.Class.

#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/text.h"
#include "dex-engine/jlang.h"
#include "framework-bridge/internal.h"

namespace aa::bridge {

using rt::ApiStatus;
using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = ApiStatus;

namespace {

constexpr const char* kNoAnnotations = "annotations DEX non lues dans cette version (prévu en G2)";
constexpr const char* kInvocationTarget = "Ljava/lang/reflect/InvocationTargetException;";

using rt::FieldPeer;
using rt::MethodPeer;

rt::Class* class_arg(Runtime& rt, Handle mirror, const char* what) {
  if (mirror == kNull) rt.throw_npe(what);
  rt::Class* c = rt.mirror_class(mirror);
  if (!c) rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string(what) + " : objet Class invalide");
  return c;
}

// ART's class flag of proxy classes, which Class.isProxy() reads.
constexpr uint32_t kAccClassIsProxy = 0x40000;

bool is_proxy_class(const rt::Class* c) { return c && (c->access & kAccClassIsProxy); }

rt::Method* method_of(Runtime& rt, Handle h) { return peer_or_throw<MethodPeer>(rt, h, "Method").method; }
rt::Field* field_of(Runtime& rt, Handle h) { return peer_or_throw<FieldPeer>(rt, h, "Field").field; }

Handle make_method_object(Runtime& rt, rt::Method* m) { return rt.reflect_method(m); }
Handle make_field_object(Runtime& rt, rt::Field* f) { return rt.reflect_field(f); }

Handle object_array(Runtime& rt, const char* desc, const std::vector<Handle>& items) {
  Handle arr = rt.new_array(desc, int32_t(items.size()));
  rt::ArrayObject& a = rt.array(arr, desc);
  for (size_t i = 0; i < items.size(); ++i) std::memcpy(&a.data[i * 4], &items[i], 4);
  return arr;
}

std::vector<Handle> array_items(Runtime& rt, Handle arr) {
  std::vector<Handle> out;
  if (arr == kNull) return out;
  rt::ArrayObject& a = rt.array(arr, "tableau d'arguments");
  if (a.klass->component->primitive) rt.throw_new("Ljava/lang/IllegalArgumentException;", "tableau d'objets attendu");
  for (uint32_t i = 0; i < a.length; ++i) {
    Handle h;
    std::memcpy(&h, &a.data[size_t(i) * 4], 4);
    out.push_back(h);
  }
  return out;
}

std::vector<std::string> params_of(const rt::Method* m) {
  std::vector<std::string> params;
  std::string ret;
  dex::split_signature(m->signature, params, ret);
  return params;
}

std::string return_of(const rt::Method* m) { return m->signature.substr(m->signature.find(')') + 1); }

// Parameter descriptors from a Class[] argument.
std::string signature_params(Runtime& rt, Handle classes) {
  std::string s;
  for (Handle c : array_items(rt, classes)) s += class_arg(rt, c, "type de paramètre")->descriptor;
  return s;
}

Handle class_of_descriptor(Runtime& rt, const std::string& d) { return rt.class_mirror(rt.find_class(d)); }

int32_t method_modifiers(const rt::Method* m) {
  int32_t mods = int32_t(m->access & 0xD3F);
  if (m->access & dex::kAccDeclaredSynchronized) mods |= 0x20;
  return mods;
}

// Converts a boxed argument to the raw words of a primitive parameter, with
// Java's widening conversions.
void unbox_into(Runtime& rt, Handle arg, char target, std::vector<uint32_t>& out) {
  char from = 0;
  uint64_t bits = 0;
  if (arg == kNull || !unbox(rt, arg, from, bits)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "argument type mismatch");
  }
  auto as_long = [&]() -> int64_t {
    switch (from) {
      case 'J': return int64_t(bits);
      case 'C': return int64_t(bits & 0xFFFF);
      default: return int32_t(bits);
    }
  };
  auto as_double = [&]() -> double {
    if (from == 'F') {
      uint32_t b = uint32_t(bits);
      float f;
      std::memcpy(&f, &b, 4);
      return f;
    }
    if (from == 'D') {
      double d;
      std::memcpy(&d, &bits, 8);
      return d;
    }
    return double(as_long());
  };
  static const std::string widen_ok[] = {
      // target: accepted sources
      "Z:Z", "B:B", "S:BS", "C:C", "I:BSCI", "J:BSCIJ", "F:BSCIJF", "D:BSCIJFD"};
  bool ok = false;
  for (const auto& rule : widen_ok) {
    if (rule[0] == target) ok = rule.find(from, 2) != std::string::npos;
  }
  if (!ok) rt.throw_new("Ljava/lang/IllegalArgumentException;", "argument type mismatch");
  switch (target) {
    case 'J': {
      uint64_t v = uint64_t(as_long());
      out.push_back(uint32_t(v));
      out.push_back(uint32_t(v >> 32));
      break;
    }
    case 'F': out.push_back(uint32_t(rt::ret_float(float(as_double())))); break;
    case 'D': {
      uint64_t v = rt::ret_double(as_double());
      out.push_back(uint32_t(v));
      out.push_back(uint32_t(v >> 32));
      break;
    }
    case 'Z': out.push_back(bits ? 1 : 0); break;
    default: out.push_back(uint32_t(as_long()));
  }
}

Handle box_result(Runtime& rt, char type, uint64_t raw) {
  switch (type) {
    case 'V': return kNull;
    case 'L':
    case '[': return Handle(raw);
    case 'J':
    case 'D': return box(rt, type, raw);
    default: return box(rt, type, uint32_t(raw));
  }
}

// Shared by Method.invoke and Constructor.newInstance.
std::vector<uint32_t> build_args(Runtime& rt, const rt::Method* m, Handle receiver, Handle args_array) {
  std::vector<std::string> params = params_of(m);
  std::vector<Handle> args = array_items(rt, args_array);
  if (args.size() != params.size()) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "Wrong number of arguments; expected " + std::to_string(params.size()) +
                                                              ", got " + std::to_string(args.size()));
  }
  std::vector<uint32_t> words;
  if (!m->is_static()) words.push_back(receiver);
  for (size_t i = 0; i < params.size(); ++i) {
    const std::string& p = params[i];
    if (p[0] == 'L' || p[0] == '[') {
      if (args[i] != kNull && !rt.instance_of(args[i], rt.find_class(p))) {
        rt.throw_new("Ljava/lang/IllegalArgumentException;", "argument type mismatch");
      }
      words.push_back(args[i]);
    } else {
      unbox_into(rt, args[i], p[0], words);
    }
  }
  return words;
}

// InvocationTargetException keeps its target in libcore's `target` field,
// which getTargetException() and getCause() return, and as the throwable's
// cause, which the VM's own reports ("Caused by") follow. The field is looked
// up on InvocationTargetException itself: a subclass field cannot shadow it.
uint32_t& invocation_target(Runtime& rt, Handle ite) {
  rt::Field* f = rt.find_class(kInvocationTarget)->declared_field("target", "Ljava/lang/Throwable;");
  if (!f) rt.throw_new("Ljava/lang/InternalError;", "champ InvocationTargetException.target absent");
  return rt.nonnull(ite, "InvocationTargetException").fields.at(f->slot);
}

void set_invocation_target(Runtime& rt, Handle ite, Handle target) {
  if (auto* p = rt.peer<rt::ThrowablePeer>(ite)) p->cause = target;
  invocation_target(rt, ite) = target;
}

// The constructors chain to ReflectiveOperationException(String, Throwable)
// like libcore's bytecode, so that the Throwable state (message, stack trace)
// is set up as for any other exception, then record the target. Java's
// protected InvocationTargetException() and InvocationTargetException(Throwable)
// call super((Throwable) null): no message, no cause. The message of
// InvocationTargetException(Throwable, String) may be null.
uint64_t init_invocation_target(Runtime& rt, Handle self, Handle target, Handle message) {
  rt::Class* super = rt.find_class(kInvocationTarget)->super;
  rt::Method* ctor = super ? super->declared_method("<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V") : nullptr;
  if (!ctor) rt.throw_new("Ljava/lang/InternalError;", "constructeur de ReflectiveOperationException introuvable");
  uint32_t args[3] = {self, message, kNull};
  rt.invoke(ctor, args, 3);
  set_invocation_target(rt, self, target);
  return 0;
}

uint64_t invoke_wrapped(Runtime& rt, rt::Method* m, std::vector<uint32_t>& words) {
  try {
    return rt.invoke(m, words.data(), uint32_t(words.size()));
  } catch (const rt::JavaThrow& t) {
    Handle ite = rt.make_throwable(rt.find_class(kInvocationTarget), u"", false);
    set_invocation_target(rt, ite, t.exception);
    throw rt::JavaThrow{ite};
  }
}

void collect_methods(rt::Class* c, bool public_only, bool inherited, bool constructors, std::vector<rt::Method*>& out) {
  std::set<std::string> seen;
  std::vector<rt::Class*> interfaces;
  for (rt::Class* k = c; k; k = inherited ? k->super : nullptr) {
    for (const auto& m : k->methods) {
      bool is_ctor = m->name == "<init>";
      if (m->name == "<clinit>" || is_ctor != constructors) continue;
      if (public_only && !(m->access & dex::kAccPublic)) continue;
      if (!seen.insert(m->name + m->signature).second) continue;  // overridden
      out.push_back(m.get());
    }
    if (constructors) break;
    if (inherited) interfaces.insert(interfaces.end(), k->interfaces.begin(), k->interfaces.end());
  }
  // Then the superinterfaces (Class.getMethod searches them after the
  // superclasses), without their static methods, which are not inherited.
  std::set<rt::Class*> visited;
  for (size_t i = 0; i < interfaces.size(); ++i) {
    rt::Class* k = interfaces[i];
    if (!k || !visited.insert(k).second) continue;
    for (const auto& m : k->methods) {
      if (m->name == "<clinit>" || (m->access & dex::kAccStatic)) continue;
      if (public_only && !(m->access & dex::kAccPublic)) continue;
      if (!seen.insert(m->name + m->signature).second) continue;
      out.push_back(m.get());
    }
    interfaces.insert(interfaces.end(), k->interfaces.begin(), k->interfaces.end());
  }
}

rt::Field* find_field(rt::Class* c, const std::string& name, bool public_only, bool inherited) {
  for (rt::Class* k = c; k; k = inherited ? k->super : nullptr) {
    if (rt::Field* f = k->declared_field(name, "")) {
      if (!public_only || (f->access & dex::kAccPublic)) return f;
    }
    if (!inherited) break;
  }
  return nullptr;
}

// Mirror of a class named by a DEX annotation, or null when it cannot be loaded.
Handle mirror_or_null(Runtime& rt, const std::string& descriptor) {
  rt::Class* c = nullptr;
  try {
    c = rt.lookup_class(descriptor);
  } catch (const rt::JavaThrow&) {
    c = nullptr;
  }
  return c ? rt.class_mirror(c) : kNull;
}

Handle create_annotation_instance(Runtime& rt, rt::Class* anno_cls, const rt::EncodedAnnotation& ea);

uint64_t convert_encoded_value(Runtime& rt, const dex::EncodedValue& v, const dex::DexFile* dex, std::string_view sig) {
  std::string ret = sig.find(')') != std::string::npos ? std::string(sig.substr(sig.find(')') + 1)) : std::string(sig);
  if (ret == "Z") return rt::ret_bool((v.bits & 1) != 0);
  if (ret == "B") return rt::ret_int(int8_t(v.bits));
  if (ret == "C") return rt::ret_int(uint16_t(v.bits));
  if (ret == "S") return rt::ret_int(int16_t(v.bits));
  if (ret == "I") return rt::ret_int(int32_t(v.bits));
  if (ret == "J") return v.bits;
  if (ret == "F") return uint32_t(v.bits);
  if (ret == "D") return v.bits;
  if (ret == "Ljava/lang/String;") {
    if (v.type == dex::EncodedValue::kNull) return kNull;
    if (dex && v.bits < dex->num_strings()) {
      return rt.new_string_utf8(dex->string(uint32_t(v.bits)));
    }
    return kNull;
  }
  if (ret == "Ljava/lang/Class;") {
    if (v.type == dex::EncodedValue::kNull) return kNull;
    if (dex && v.bits < dex->num_types()) {
      rt::Class* c = rt.lookup_class(dex->type_descriptor(uint32_t(v.bits)));
      return c ? rt.class_mirror(c) : kNull;
    }
    return kNull;
  }
  if (!ret.empty() && ret[0] == '[') {
    if (v.type == dex::EncodedValue::kNull) return kNull;
    std::string elem_type = ret.substr(1);
    uint32_t count = uint32_t(v.array.size());
    Handle arr = rt.new_array(ret, int32_t(count));
    auto& arr_obj = rt.array(arr, ret);
    if (elem_type == "Ljava/lang/String;") {
      for (uint32_t i = 0; i < count; ++i) {
        Handle s = kNull;
        if (dex && v.array[i].bits < dex->num_strings()) {
          s = rt.new_string_utf8(dex->string(uint32_t(v.array[i].bits)));
        }
        std::memcpy(&arr_obj.data[size_t(i) * 4], &s, 4);
      }
    } else if (elem_type == "Ljava/lang/Class;") {
      for (uint32_t i = 0; i < count; ++i) {
        Handle m = kNull;
        if (dex && v.array[i].bits < dex->num_types()) {
          if (rt::Class* c = rt.lookup_class(dex->type_descriptor(uint32_t(v.array[i].bits)))) {
            m = rt.class_mirror(c);
          }
        }
        std::memcpy(&arr_obj.data[size_t(i) * 4], &m, 4);
      }
    } else if (elem_type == "I" || elem_type == "Z" || elem_type == "B" || elem_type == "S" || elem_type == "C") {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t b = uint32_t(v.array[i].bits);
        std::memcpy(&arr_obj.data[size_t(i) * arr_obj.width], &b, arr_obj.width);
      }
    } else if (elem_type == "J" || elem_type == "D") {
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t b = v.array[i].bits;
        std::memcpy(&arr_obj.data[size_t(i) * 8], &b, 8);
      }
    }
    return arr;
  }
  // Enum
  if (v.type == dex::EncodedValue::kEnum && dex && v.bits < dex->num_fields()) {
    const dex::FieldId& fid = dex->field(uint32_t(v.bits));
    rt::Class* enum_cls = rt.lookup_class(dex->type_descriptor(fid.class_idx));
    if (enum_cls) {
      rt.ensure_initialized(enum_cls);
      rt::Field* fld = enum_cls->declared_field(dex->string(fid.name_idx), dex->type_descriptor(fid.type_idx));
      if (fld && fld->is_static()) {
        return enum_cls->statics.at(fld->slot);
      }
    }
  }
  return kNull;
}

void setup_annotation_proxy_methods(Runtime&, rt::Class* proxy_cls, rt::Class* anno_cls) {
  {
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = "<init>";
    m->signature = "()V";
    m->access = dex::kAccPublic | dex::kAccConstructor;
    m->arg_words = 1;
    m->native = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
    proxy_cls->methods.push_back(std::move(m));
  }
  // annotationType()
  {
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = "annotationType";
    m->signature = "()Ljava/lang/Class;";
    m->access = dex::kAccPublic;
    m->arg_words = 1;
    m->native = [anno_cls](Runtime& rt, NativeArgs&) -> uint64_t {
      return rt.class_mirror(anno_cls);
    };
    proxy_cls->methods.push_back(std::move(m));
  }
  // Elements of annotation
  for (const auto& method : anno_cls->methods) {
    if (method->name == "<clinit>" || method->name == "<init>") continue;
    std::string m_name = method->name;
    std::string m_sig = method->signature;
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = m_name;
    m->signature = m_sig;
    m->access = dex::kAccPublic;
    m->arg_words = 1;
    m->native = [m_name, m_sig](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto* peer = rt.peer<rt::AnnotationPeer>(a.h(0));
      if (!peer) rt.throw_new("Ljava/lang/InternalError;", "AnnotationPeer manquant");
      auto it = peer->elements.find(m_name);
      if (it == peer->elements.end()) {
        rt.throw_new("Ljava/lang/annotation/IncompleteAnnotationException;", m_name);
      }
      return convert_encoded_value(rt, it->second, peer->dex, m_sig);
    };
    proxy_cls->methods.push_back(std::move(m));
  }
  // equals
  {
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = "equals";
    m->signature = "(Ljava/lang/Object;)Z";
    m->access = dex::kAccPublic;
    m->arg_words = 2;
    m->native = [](Runtime& rt, NativeArgs& a) -> uint64_t {
      Handle self = a.h(0);
      Handle other = a.h(1);
      if (self == other) return rt::ret_bool(true);
      auto* p1 = rt.peer<rt::AnnotationPeer>(self);
      auto* p2 = rt.peer<rt::AnnotationPeer>(other);
      if (!p1 || !p2 || p1->anno_class != p2->anno_class) return rt::ret_bool(false);
      if (p1->elements.size() != p2->elements.size()) return rt::ret_bool(false);
      for (const auto& [k, v1] : p1->elements) {
        auto it2 = p2->elements.find(k);
        if (it2 == p2->elements.end()) return rt::ret_bool(false);
        if (v1.type != it2->second.type || v1.bits != it2->second.bits) return rt::ret_bool(false);
      }
      return rt::ret_bool(true);
    };
    proxy_cls->methods.push_back(std::move(m));
  }
  // hashCode
  {
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = "hashCode";
    m->signature = "()I";
    m->access = dex::kAccPublic;
    m->arg_words = 1;
    m->native = [](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto* p = rt.peer<rt::AnnotationPeer>(a.h(0));
      if (!p) return 0;
      int32_t hash = 0;
      for (const auto& [k, v] : p->elements) {
        int32_t k_hash = rt::java_string_hash(rt::u16(k));
        int32_t v_hash = int32_t(v.bits ^ (v.bits >> 32));
        hash += (127 * k_hash) ^ v_hash;
      }
      return rt::ret_int(hash);
    };
    proxy_cls->methods.push_back(std::move(m));
  }
  // toString
  {
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = "toString";
    m->signature = "()Ljava/lang/String;";
    m->access = dex::kAccPublic;
    m->arg_words = 1;
    m->native = [anno_cls](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto* p = rt.peer<rt::AnnotationPeer>(a.h(0));
      std::string s = "@" + anno_cls->java_name() + "(";
      if (p) {
        bool first = true;
        for (const auto& [k, v] : p->elements) {
          if (!first) s += ", ";
          first = false;
          s += k + "=";
          if (v.type == dex::EncodedValue::kString && p->dex) {
            s += "\"" + p->dex->string(uint32_t(v.bits)) + "\"";
          } else if (v.type == dex::EncodedValue::kBoolean) {
            s += (v.bits & 1) ? "true" : "false";
          } else {
            s += std::to_string(v.bits);
          }
        }
      }
      s += ")";
      return rt.new_string_utf8(s);
    };
    proxy_cls->methods.push_back(std::move(m));
  }
}

Handle create_annotation_instance(Runtime& rt, rt::Class* anno_cls, const rt::EncodedAnnotation& ea) {
  if (!anno_cls) return kNull;
  std::string proxy_desc = "$Anno$" + anno_cls->descriptor.substr(1);
  rt::Class* proxy_cls = rt.loaded_class(proxy_desc);
  if (!proxy_cls) {
    proxy_cls = rt.define_synthetic_class(proxy_desc, rt.find_class("Ljava/lang/Object;"),
                                          {anno_cls, rt.find_class("Ljava/lang/annotation/Annotation;")},
                                          dex::kAccPublic | dex::kAccFinal);
    setup_annotation_proxy_methods(rt, proxy_cls, anno_cls);
  }
  Handle h = rt.alloc(proxy_cls);
  auto peer = std::make_unique<rt::AnnotationPeer>();
  peer->anno_class = anno_cls;
  peer->dex = ea.dex;
  peer->elements = ea.elements;
  rt.object(h)->peer = std::move(peer);
  return h;
}

Handle get_declared_annotations(Runtime& rt, const std::unordered_map<std::string, rt::EncodedAnnotation>& map) {
  std::vector<Handle> annos;
  for (const auto& [desc, ea] : map) {
    if (ea.visibility != 1) continue;  // RUNTIME visibility only
    if (rt::Class* anno_cls = rt.lookup_class(desc)) {
      Handle inst = create_annotation_instance(rt, anno_cls, ea);
      if (inst != kNull) annos.push_back(inst);
    }
  }
  return object_array(rt, "[Ljava/lang/annotation/Annotation;", annos);
}

Handle get_signature_annotation_array(Runtime& rt, const std::vector<std::string>& sig) {
  if (sig.empty()) return kNull;
  std::vector<Handle> items;
  items.reserve(sig.size());
  for (const auto& s : sig) items.push_back(rt.new_string_utf8(s));
  return object_array(rt, "[Ljava/lang/String;", items);
}

Handle get_annotation_for_class(Runtime& rt, rt::Class* cls, rt::Class* anno_cls) {
  if (!cls || !anno_cls) return kNull;
  const auto& map = cls->annotations().annotations;
  auto it = map.find(anno_cls->descriptor);
  if (it != map.end() && it->second.visibility == 1) {
    return create_annotation_instance(rt, anno_cls, it->second);
  }
  // Inheritance if @Inherited
  const auto& meta = anno_cls->annotations().annotations;
  if (meta.find("Ljava/lang/annotation/Inherited;") != meta.end()) {
    for (rt::Class* k = cls->super; k; k = k->super) {
      const auto& smap = k->annotations().annotations;
      auto sit = smap.find(anno_cls->descriptor);
      if (sit != smap.end() && sit->second.visibility == 1) {
        return create_annotation_instance(rt, anno_cls, sit->second);
      }
    }
  }
  return kNull;
}

bool is_annotation_present_for_class(Runtime& rt, rt::Class* cls, rt::Class* anno_cls) {
  if (!cls || !anno_cls) return false;
  const auto& map = cls->annotations().annotations;
  auto it = map.find(anno_cls->descriptor);
  if (it != map.end() && it->second.visibility == 1) return true;
  const auto& meta = anno_cls->annotations().annotations;
  if (meta.find("Ljava/lang/annotation/Inherited;") != meta.end()) {
    for (rt::Class* k = cls->super; k; k = k->super) {
      const auto& smap = k->annotations().annotations;
      auto sit = smap.find(anno_cls->descriptor);
      if (sit != smap.end() && sit->second.visibility == 1) return true;
    }
  }
  return false;
}

std::string java_to_class_descriptor(const std::string& name) {
  if (!name.empty() && name[0] == '[') {
    std::string d = name;
    for (char& ch : d) {
      if (ch == '.') ch = '/';
    }
    return d;
  }
  return text::java_to_descriptor(name);
}

void register_class_reflection(rt::NativeRegistry& r) {
  auto c = r.extend("Ljava/lang/Class;");
  auto for_name = [](Runtime& rt, const std::u16string& name, bool initialize) -> Handle {
    std::string n = text::utf16_to_utf8(name);
    if (n.empty() || n.find('/') != std::string::npos || n.find(';') != std::string::npos) {
      rt.throw_new("Ljava/lang/ClassNotFoundException;", n);
    }
    rt::Class* cls = nullptr;
    try {
      cls = rt.lookup_class(java_to_class_descriptor(n));
    } catch (const rt::JavaThrow&) {
      cls = nullptr;
    }
    if (!cls || cls->primitive) rt.throw_new("Ljava/lang/ClassNotFoundException;", n);
    if (initialize) rt.ensure_initialized(cls);
    return rt.class_mirror(cls);
  };
  // Libcore mode: Class.forName of libcore (classForName through the loader;
  // a LinkageError behind the ClassNotFoundException is thrown as is).
  auto for_name_with_loader = [](Runtime& rt, Handle name, bool initialize, Handle loader) -> Handle {
    if (name == kNull) rt.throw_npe("name == null");
    rt::Class* cls = nullptr;
    try {
      cls = class_for_name(rt, name, loader);
    } catch (const rt::JavaThrow& t) {
      rt::GlobalRef keep(rt, t.exception);
      Handle cause = Handle(rt.call_virtual(t.exception, "getCause", "()Ljava/lang/Throwable;"));
      if (cause != kNull && rt.instance_of(cause, rt.find_class("Ljava/lang/LinkageError;"))) throw rt::JavaThrow{cause};
      throw;
    }
    if (initialize) rt.ensure_initialized(cls);
    return rt.class_mirror(cls);
  };
  c.static_method("forName", "(Ljava/lang/String;)Ljava/lang/Class;",
                  [for_name, for_name_with_loader](Runtime& rt, NativeArgs& a) -> uint64_t {
                    if (!rt.libcore_mode()) return for_name(rt, a.str(0), true);
                    // The caller's loader: frame n-1 is forName itself.
                    const auto& frames = rt.current_thread()->frames;
                    Handle loader =
                        frames.size() >= 2 ? class_loader_of(rt, frames[frames.size() - 2].method->owner, true) : kNull;
                    return for_name_with_loader(rt, a.h(0), true, loader);
                  });
  c.static_method("forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
                  [for_name, for_name_with_loader](Runtime& rt, NativeArgs& a) -> uint64_t {
                    if (!rt.libcore_mode()) return for_name(rt, a.str(0), a.z(1));
                    return for_name_with_loader(rt, a.h(0), a.z(1), a.h(2));
                  },
                  S::kPartial, "un seul espace de classes (ADR-006 J2) ; sans libcore, le chargeur est ignoré");

  auto methods = [](bool public_only, bool inherited) {
    return [public_only, inherited](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::vector<rt::Method*> ms;
      collect_methods(class_arg(rt, a.h(0), "Class"), public_only, inherited, false, ms);
      std::vector<Handle> objs;
      for (rt::Method* m : ms) objs.push_back(make_method_object(rt, m));
      return object_array(rt, "[Ljava/lang/reflect/Method;", objs);
    };
  };
  c.method("getDeclaredMethods", "()[Ljava/lang/reflect/Method;", methods(false, false));
  c.method("getMethods", "()[Ljava/lang/reflect/Method;", methods(true, true));
  auto find_method = [](bool public_only, bool inherited) {
    return [public_only, inherited](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* cls = class_arg(rt, a.h(0), "Class");
      std::string name = text::utf16_to_utf8(a.str(1));
      std::string params = "(" + signature_params(rt, a.h(2)) + ")";
      std::vector<rt::Method*> ms;
      collect_methods(cls, public_only, inherited, false, ms);
      for (rt::Method* m : ms) {
        if (m->name == name && m->signature.compare(0, params.size(), params) == 0) return make_method_object(rt, m);
      }
      rt.throw_new("Ljava/lang/NoSuchMethodException;", cls->java_name() + "." + name + params);
    };
  };
  c.method("getDeclaredMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;", find_method(false, false));
  c.method("getMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;", find_method(true, true));
  auto constructors = [](bool public_only) {
    return [public_only](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::vector<rt::Method*> ms;
      collect_methods(class_arg(rt, a.h(0), "Class"), public_only, false, true, ms);
      std::vector<Handle> objs;
      for (rt::Method* m : ms) objs.push_back(make_method_object(rt, m));
      return object_array(rt, "[Ljava/lang/reflect/Constructor;", objs);
    };
  };
  c.method("getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;", constructors(false));
  c.method("getConstructors", "()[Ljava/lang/reflect/Constructor;", constructors(true));
  auto find_ctor = [](bool public_only) {
    return [public_only](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* cls = class_arg(rt, a.h(0), "Class");
      std::string sig = "(" + signature_params(rt, a.h(1)) + ")V";
      if (rt::Method* m = cls->declared_method("<init>", sig); m && (!public_only || (m->access & dex::kAccPublic))) {
        return make_method_object(rt, m);
      }
      rt.throw_new("Ljava/lang/NoSuchMethodException;", cls->java_name() + ".<init>" + sig);
    };
  };
  c.method("getDeclaredConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;", find_ctor(false));
  c.method("getConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;", find_ctor(true));

  auto fields = [](bool public_only, bool inherited) {
    return [public_only, inherited](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::vector<Handle> objs;
      for (rt::Class* k = class_arg(rt, a.h(0), "Class"); k; k = inherited ? k->super : nullptr) {
        for (const auto& f : k->fields) {
          if (!public_only || (f->access & dex::kAccPublic)) objs.push_back(make_field_object(rt, f.get()));
        }
        if (!inherited) break;
      }
      return object_array(rt, "[Ljava/lang/reflect/Field;", objs);
    };
  };
  c.method("getDeclaredFields", "()[Ljava/lang/reflect/Field;", fields(false, false));
  c.method("getFields", "()[Ljava/lang/reflect/Field;", fields(true, true));
  auto get_field = [](bool public_only, bool inherited) {
    return [public_only, inherited](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* cls = class_arg(rt, a.h(0), "Class");
      std::string name = text::utf16_to_utf8(a.str(1));
      if (rt::Field* f = find_field(cls, name, public_only, inherited)) return make_field_object(rt, f);
      rt.throw_new("Ljava/lang/NoSuchFieldException;", name);
    };
  };
  c.method("getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;", get_field(false, false));
  c.method("getField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;", get_field(true, true));

  c.method("getInterfaces", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<Handle> objs;
    for (rt::Class* i : class_arg(rt, a.h(0), "Class")->interfaces) objs.push_back(rt.class_mirror(i));
    return object_array(rt, "[Ljava/lang/Class;", objs);
  });
  c.method("getModifiers", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(class_arg(rt, a.h(0), "Class")->access & 0x7FF));
  }, S::kPartial, "modificateurs des classes internes non lus (InnerClass)");
  c.method("isAssignableFrom", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(rt.is_assignable(class_arg(rt, a.h(1), "Class"), class_arg(rt, a.h(0), "Class")));
  });
  c.method("getComponentType", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    return cls->component ? rt.class_mirror(cls->component) : kNull;
  });
  c.method("isPrimitive", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(class_arg(rt, a.h(0), "Class")->primitive != 0);
  });
  c.method("isEnum", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    return rt::ret_bool((cls->access & dex::kAccEnum) && cls->super && cls->super->descriptor == "Ljava/lang/Enum;");
  });
  c.method("cast", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    if (a.h(1) != kNull && !rt.instance_of(a.h(1), cls)) {
      rt.throw_new("Ljava/lang/ClassCastException;", "Cannot cast " + rt.class_of(a.h(1))->java_name() + " to " + cls->java_name());
    }
    return a.h(1);
  });
  c.method("getClassLoader", "()Ljava/lang/ClassLoader;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return class_loader_of(rt, class_arg(rt, a.h(0), "Class"), false);
  });
  c.method("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
    return get_annotation_for_class(rt, cls, anno_cls);
  });
  c.method("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    return get_declared_annotations(rt, cls->annotations().annotations);
  });
  c.method("getAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    std::unordered_map<std::string, rt::EncodedAnnotation> all = cls->annotations().annotations;
    for (rt::Class* k = cls->super; k; k = k->super) {
      for (const auto& [desc, ea] : k->annotations().annotations) {
        if (ea.visibility != 1 || all.count(desc)) continue;
        if (rt::Class* ac = rt.lookup_class(desc)) {
          if (ac->annotations().annotations.count("Ljava/lang/annotation/Inherited;")) {
            all.emplace(desc, ea);
          }
        }
      }
    }
    return get_declared_annotations(rt, all);
  });
  c.method("isAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
    return rt::ret_bool(is_annotation_present_for_class(rt, cls, anno_cls));
  });
  // From the system annotations of the DEX (as ART's annotations::GetEnclosingClass,
  // GetDeclaringClass): a local or anonymous class is enclosed by its method's class.
  c.method("getEnclosingClass", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    const rt::ClassAnnotations& an = cls->annotations();
    if (!an.enclosing_class.empty()) return mirror_or_null(rt, an.enclosing_class);
    if (an.enclosing_method) {
      const dex::DexFile& dex = *cls->dex_cache->dex;
      return mirror_or_null(rt, dex.type_descriptor(dex.method(*an.enclosing_method).class_idx));
    }
    return kNull;
  });
  // libcore's Class.isProxy() reads ART's accessFlags, which the bridge's Class has not.
  c.method("isProxy", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(is_proxy_class(class_arg(rt, a.h(0), "Class")));
  });
  c.method("getDeclaringClass", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const rt::ClassAnnotations& an = class_arg(rt, a.h(0), "Class")->annotations();
    if (an.anonymous || an.enclosing_class.empty()) return kNull;
    return mirror_or_null(rt, an.enclosing_class);
  });
  c.method("getSignatureAnnotation", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    return get_signature_annotation_array(rt, cls->annotations().signature);
  });
  c.method("getDeclaredAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
    const auto& map = cls->annotations().annotations;
    auto it = map.find(anno_cls->descriptor);
    if (it != map.end() && it->second.visibility == 1) {
      return create_annotation_instance(rt, anno_cls, it->second);
    }
    return kNull;
  });
  c.method("isDeclaredAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
    const auto& map = cls->annotations().annotations;
    auto it = map.find(anno_cls->descriptor);
    return rt::ret_bool(it != map.end() && it->second.visibility == 1);
  });

  auto loader = r.define("Ljava/lang/ClassLoader;", "Ljava/lang/Object;", {}, 0x1, S::kPartial,
                         "un seul chargeur (classes de l'application) ; pas de chargement dynamique de DEX");
  loader.static_method("getSystemClassLoader", "()Ljava/lang/ClassLoader;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    auto& cache = rt.object_cache();
    auto it = cache.find("classloader:system");
    if (it != cache.end()) return it->second;
    Handle h = rt.alloc(rt.find_class("Ljava/lang/ClassLoader;"));
    cache["classloader:system"] = h;
    return h;
  });
  loader.method("loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", [for_name](Runtime& rt, NativeArgs& a) -> uint64_t {
    return for_name(rt, a.str(1), false);
  });
  loader.method("getParent", "()Ljava/lang/ClassLoader;", [](Runtime&, NativeArgs&) -> uint64_t { return kNull; });
}

void register_members(rt::NativeRegistry& r) {
  r.define("Ljava/lang/NoSuchMethodException;", "Ljava/lang/ReflectiveOperationException;");
  r.define("Ljava/lang/NoSuchFieldException;", "Ljava/lang/ReflectiveOperationException;");
  r.define("Ljava/lang/IllegalAccessException;", "Ljava/lang/ReflectiveOperationException;");
  r.define(kInvocationTarget, "Ljava/lang/ReflectiveOperationException;")
      .field("target", "Ljava/lang/Throwable;", 0x2)
      .ctor("()V", [](Runtime& rt, NativeArgs& a) -> uint64_t { return init_invocation_target(rt, a.h(0), kNull, kNull); })
      .ctor("(Ljava/lang/Throwable;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return init_invocation_target(rt, a.h(0), a.h(1), kNull);
      })
      .ctor("(Ljava/lang/Throwable;Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return init_invocation_target(rt, a.h(0), a.h(1), a.h(2));
      })
      .method("getTargetException", "()Ljava/lang/Throwable;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t { return invocation_target(rt, a.h(0)); })
      .method("getCause", "()Ljava/lang/Throwable;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t { return invocation_target(rt, a.h(0)); });
  r.define_interface("Ljava/lang/reflect/Member;")
      .abstract_method("getName", "()Ljava/lang/String;")
      .abstract_method("getModifiers", "()I")
      .abstract_method("getDeclaringClass", "()Ljava/lang/Class;");
  r.define("Ljava/lang/reflect/AccessibleObject;", "Ljava/lang/Object;")
      .method("setAccessible", "(Z)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; },
              S::kPartial, "aucun contrôle d'accès n'est appliqué par le prototype")
      .method("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle self = a.h(0);
        rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
        if (auto* mp = rt.peer<rt::MethodPeer>(self)) {
          const auto& map = mp->method->annotations().annotations;
          auto it = map.find(anno_cls->descriptor);
          if (it != map.end() && it->second.visibility == 1) {
            return create_annotation_instance(rt, anno_cls, it->second);
          }
          return kNull;
        }
        if (auto* fp = rt.peer<rt::FieldPeer>(self)) {
          const auto& map = fp->field->annotations().annotations;
          auto it = map.find(anno_cls->descriptor);
          if (it != map.end() && it->second.visibility == 1) {
            return create_annotation_instance(rt, anno_cls, it->second);
          }
          return kNull;
        }
        return kNull;
      })
      .method("isAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle self = a.h(0);
        rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
        if (auto* mp = rt.peer<rt::MethodPeer>(self)) {
          const auto& map = mp->method->annotations().annotations;
          auto it = map.find(anno_cls->descriptor);
          return rt::ret_bool(it != map.end() && it->second.visibility == 1);
        }
        if (auto* fp = rt.peer<rt::FieldPeer>(self)) {
          const auto& map = fp->field->annotations().annotations;
          auto it = map.find(anno_cls->descriptor);
          return rt::ret_bool(it != map.end() && it->second.visibility == 1);
        }
        return rt::ret_bool(false);
      })
      .method("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle self = a.h(0);
        if (auto* mp = rt.peer<rt::MethodPeer>(self)) {
          return get_declared_annotations(rt, mp->method->annotations().annotations);
        }
        if (auto* fp = rt.peer<rt::FieldPeer>(self)) {
          return get_declared_annotations(rt, fp->field->annotations().annotations);
        }
        return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
      })
      .method("getAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle self = a.h(0);
        if (auto* mp = rt.peer<rt::MethodPeer>(self)) {
          return get_declared_annotations(rt, mp->method->annotations().annotations);
        }
        if (auto* fp = rt.peer<rt::FieldPeer>(self)) {
          return get_declared_annotations(rt, fp->field->annotations().annotations);
        }
        return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
      });

  for (const char* desc : {"Ljava/lang/reflect/Method;", "Ljava/lang/reflect/Constructor;"}) {
    bool ctor = std::string(desc) == "Ljava/lang/reflect/Constructor;";
    auto b = r.define(desc, "Ljava/lang/reflect/AccessibleObject;", {"Ljava/lang/reflect/Member;"}, 0x1 | 0x10);
    b.method("getName", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Method* m = method_of(rt, a.h(0));
      return rt.new_string_utf8(m->name == "<init>" ? m->owner->java_name() : m->name);
    });
    b.method("getModifiers", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(method_modifiers(method_of(rt, a.h(0)))); });
    b.method("getDeclaringClass", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt.class_mirror(method_of(rt, a.h(0))->owner);
    });
    b.method("getArtMethod", "()J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt.method_cookie(method_of(rt, a.h(0)));
    });
    b.method("getParameterTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::vector<Handle> objs;
      for (const auto& p : params_of(method_of(rt, a.h(0)))) objs.push_back(class_of_descriptor(rt, p));
      return object_array(rt, "[Ljava/lang/Class;", objs);
    });
    b.method("getParameterCount", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_int(int32_t(params_of(method_of(rt, a.h(0))).size()));
    });
    b.method("getExceptionTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Method* m = method_of(rt, a.h(0));
      const auto& throws_classes = m->annotations().throws_classes;
      std::vector<Handle> objs;
      objs.reserve(throws_classes.size());
      for (const std::string& th_desc : throws_classes) {
        if (rt::Class* c = rt.lookup_class(th_desc)) {
          objs.push_back(rt.class_mirror(c));
        }
      }
      return object_array(rt, "[Ljava/lang/Class;", objs);
    });
    b.method("getParameterAnnotations", "()[[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Method* m = method_of(rt, a.h(0));
      auto params = params_of(m);
      uint32_t count = uint32_t(params.size());
      Handle outer = rt.new_array("[[Ljava/lang/annotation/Annotation;", int32_t(count));
      const auto& param_annos = m->annotations().parameter_annotations;
      for (uint32_t i = 0; i < count; ++i) {
        Handle inner = kNull;
        if (i < param_annos.size()) {
          inner = get_declared_annotations(rt, param_annos[i]);
        } else {
          inner = rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
        }
        std::memcpy(&rt.array(outer, "[[Ljava/lang/annotation/Annotation;").data[size_t(i) * 4], &inner, 4);
      }
      return outer;
    });
    b.method("getSignatureAnnotation", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Method* m = method_of(rt, a.h(0));
      return get_signature_annotation_array(rt, m->annotations().signature);
    });
    b.method("isVarArgs", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_bool(method_of(rt, a.h(0))->access & dex::kAccVarargs);
    });
    b.method("isSynthetic", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_bool(method_of(rt, a.h(0))->access & dex::kAccSynthetic);
    });
    if (!ctor) {
      b.method("isBridge", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return rt::ret_bool(method_of(rt, a.h(0))->access & dex::kAccBridge);
      });
    }
    b.method("equals", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto* other = rt.peer<MethodPeer>(a.h(1));
      return rt::ret_bool(other && other->method == method_of(rt, a.h(0)));
    });
    b.method("hashCode", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Method* m = method_of(rt, a.h(0));
      return rt::ret_int(rt::java_string_hash(rt::u16(m->owner->java_name())) ^ rt::java_string_hash(rt::u16(m->name)));
    });
    b.method("toString", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Method* m = method_of(rt, a.h(0));
      std::string s = text::descriptor_to_java(return_of(m)) + " " + m->owner->java_name() + "." + m->name + "(";
      auto params = params_of(m);
      for (size_t i = 0; i < params.size(); ++i) s += (i ? "," : "") + text::descriptor_to_java(params[i]);
      return rt.new_string_utf8(s + ")");
    });
    if (!ctor) {
      b.method("getReturnType", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        return class_of_descriptor(rt, return_of(method_of(rt, a.h(0))));
      });
      b.method("invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt::Method* m = method_of(rt, a.h(0));
        Handle receiver = a.h(1);
        if (m->is_static()) {
          rt.ensure_initialized(m->owner);
        } else {
          if (receiver == kNull) rt.throw_npe("Method.invoke : receveur null pour une méthode d'instance");
          if (!rt.instance_of(receiver, m->owner)) {
            rt.throw_new("Ljava/lang/IllegalArgumentException;", "Expected receiver of type " + m->owner->java_name() +
                                                                     ", but got " + rt.class_of(receiver)->java_name());
          }
          // Reflection dispatches virtually, except for private methods.
          if (!(m->access & dex::kAccPrivate)) {
            if (rt::Method* v = rt.resolve_virtual(rt.class_of(receiver), m->name, m->signature)) m = v;
          }
        }
        std::vector<uint32_t> words = build_args(rt, m, receiver, a.h(2));
        uint64_t result = invoke_wrapped(rt, m, words);
        return box_result(rt, m->return_type(), result);
      });
    } else {
      b.method("newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        rt::Method* m = method_of(rt, a.h(0));
        rt::Class* cls = m->owner;
        if (cls->is_abstract() || cls->is_interface()) rt.throw_new("Ljava/lang/InstantiationException;", cls->java_name());
        rt.ensure_initialized(cls);
        Handle obj = rt.alloc(cls);
        std::vector<uint32_t> words = build_args(rt, m, obj, a.h(1));
        invoke_wrapped(rt, m, words);
        return obj;
      });
    }
  }

  auto f = r.define("Ljava/lang/reflect/Field;", "Ljava/lang/reflect/AccessibleObject;", {"Ljava/lang/reflect/Member;"}, 0x1 | 0x10);
  f.method("getName", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(field_of(rt, a.h(0))->name);
  });
  f.method("getType", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return class_of_descriptor(rt, field_of(rt, a.h(0))->type);
  });
  f.method("getModifiers", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(field_of(rt, a.h(0))->access & 0xDF));
  });
  f.method("getDeclaringClass", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.class_mirror(field_of(rt, a.h(0))->owner);
  });
  f.method("getSignatureAnnotation", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* fld = field_of(rt, a.h(0));
    return get_signature_annotation_array(rt, fld->annotations().signature);
  });
  // ART's Field.getArtField(), stored by FieldVarHandle: an opaque cookie.
  f.method("getArtField", "()J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.field_cookie(field_of(rt, a.h(0)));
  }, S::kPartial, "identifiant opaque, valable pour les VarHandle");
  // ART's Field.getOffset(), used by sun.misc.Unsafe.objectFieldOffset().
  f.method("getOffset", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* fld = field_of(rt, a.h(0));
    if (fld->is_static()) rt.throw_new("Ljava/lang/IllegalArgumentException;", "champ statique : " + fld->name);
    return rt::ret_int(int32_t(unsafe_field_offset(fld->slot)));
  }, S::kPartial, "décalage opaque, valable uniquement pour sun.misc.Unsafe");
  // Storage of a field for a given receiver (statics or instance slots).
  auto storage = [](Runtime& rt, rt::Field* fld, Handle receiver) -> uint32_t* {
    if (fld->is_static()) {
      rt.ensure_initialized(fld->owner);
      return &fld->owner->statics.at(fld->slot);
    }
    rt::Object& o = rt.nonnull(receiver, "Field : receveur null");
    if (!rt.is_assignable(o.klass, fld->owner) || fld->slot + (fld->wide() ? 2u : 1u) > o.fields.size()) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "le receveur n'est pas une instance de " + fld->owner->java_name());
    }
    return &o.fields[fld->slot];
  };
  f.method("get", "(Ljava/lang/Object;)Ljava/lang/Object;", [storage](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* fld = field_of(rt, a.h(0));
    uint32_t* s = storage(rt, fld, a.h(1));
    uint64_t raw = fld->wide() ? uint64_t(s[0]) | (uint64_t(s[1]) << 32) : s[0];
    return box_result(rt, fld->type[0], raw);
  });
  f.method("set", "(Ljava/lang/Object;Ljava/lang/Object;)V", [storage](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* fld = field_of(rt, a.h(0));
    if (fld->is_static() && (fld->access & dex::kAccFinal)) {
      rt.throw_new("Ljava/lang/IllegalAccessException;", "champ statique final : " + fld->name);
    }
    uint32_t* s = storage(rt, fld, a.h(1));
    if (fld->is_reference()) {
      if (a.h(2) != kNull && !rt.instance_of(a.h(2), rt.find_class(fld->type))) {
        rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de valeur incompatible avec " + fld->type);
      }
      s[0] = a.h(2);
      return 0;
    }
    std::vector<uint32_t> words;
    unbox_into(rt, a.h(2), fld->type[0], words);
    for (size_t i = 0; i < words.size(); ++i) s[i] = words[i];
    return 0;
  });
  auto typed_get = [storage](char t) {
    return [storage, t](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Field* fld = field_of(rt, a.h(0));
      if (fld->type.size() != 1 || fld->type[0] != t) rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de champ différent");
      uint32_t* s = storage(rt, fld, a.h(1));
      return fld->wide() ? uint64_t(s[0]) | (uint64_t(s[1]) << 32) : s[0];
    };
  };
  f.method("getInt", "(Ljava/lang/Object;)I", typed_get('I'));
  f.method("getLong", "(Ljava/lang/Object;)J", typed_get('J'));
  f.method("getBoolean", "(Ljava/lang/Object;)Z", typed_get('Z'));
  f.method("getFloat", "(Ljava/lang/Object;)F", typed_get('F'));
  f.method("getDouble", "(Ljava/lang/Object;)D", typed_get('D'));
  f.method("setInt", "(Ljava/lang/Object;I)V", [storage](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* fld = field_of(rt, a.h(0));
    if (fld->type != "I") rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de champ différent");
    storage(rt, fld, a.h(1))[0] = a.raw(2);
    return 0;
  });
  f.method("setBoolean", "(Ljava/lang/Object;Z)V", [storage](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* fld = field_of(rt, a.h(0));
    if (fld->type != "Z") rt.throw_new("Ljava/lang/IllegalArgumentException;", "type de champ différent");
    storage(rt, fld, a.h(1))[0] = a.z(2) ? 1 : 0;
    return 0;
  });

  auto mod = r.define("Ljava/lang/reflect/Modifier;", "Ljava/lang/Object;");
  struct Bit {
    const char* name;
    int32_t bit;
  };
  for (Bit b : {Bit{"isPublic", 0x1}, Bit{"isPrivate", 0x2}, Bit{"isProtected", 0x4}, Bit{"isStatic", 0x8},
                Bit{"isFinal", 0x10}, Bit{"isSynchronized", 0x20}, Bit{"isVolatile", 0x40}, Bit{"isTransient", 0x80},
                Bit{"isNative", 0x100}, Bit{"isInterface", 0x200}, Bit{"isAbstract", 0x400}, Bit{"isStrict", 0x800}}) {
    int32_t bit = b.bit;
    mod.static_method(b.name, "(I)Z", [bit](Runtime&, NativeArgs& a) -> uint64_t { return rt::ret_bool(a.i32(0) & bit); });
  }

  auto arr = r.define("Ljava/lang/reflect/Array;", "Ljava/lang/Object;", {}, 0x1 | 0x10);
  arr.static_method("newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* comp = class_arg(rt, a.h(0), "Array.newInstance");
    if (comp->primitive == 'V') rt.throw_new("Ljava/lang/IllegalArgumentException;", "void");
    return rt.new_array("[" + comp->descriptor, a.i32(1));
  });
  arr.static_method("getLength", "(Ljava/lang/Object;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(rt.array(a.h(0), "Array.getLength").length));
  });
  arr.static_method("get", "(Ljava/lang/Object;I)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& arr_obj = rt.array(a.h(0), "Array.get");
    int32_t i = a.i32(1);
    if (i < 0 || uint32_t(i) >= arr_obj.length) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(i));
    char t = arr_obj.klass->component->primitive;
    uint64_t raw = 0;
    std::memcpy(&raw, &arr_obj.data[size_t(i) * arr_obj.width], arr_obj.width);
    if (!t) return Handle(raw);
    if (t == 'B') raw = uint32_t(int32_t(int8_t(raw)));
    if (t == 'S') raw = uint32_t(int32_t(int16_t(raw)));
    return box(rt, t, raw);
  });
  arr.static_method("set", "(Ljava/lang/Object;ILjava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::ArrayObject& arr_obj = rt.array(a.h(0), "Array.set");
    int32_t i = a.i32(1);
    if (i < 0 || uint32_t(i) >= arr_obj.length) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(i));
    rt::Class* comp = arr_obj.klass->component;
    uint8_t* slot = &arr_obj.data[size_t(i) * arr_obj.width];
    if (!comp->primitive) {
      if (a.h(2) != kNull && !rt.instance_of(a.h(2), comp)) rt.throw_new("Ljava/lang/IllegalArgumentException;", "array element type mismatch");
      Handle v = a.h(2);
      std::memcpy(slot, &v, 4);
      return 0;
    }
    std::vector<uint32_t> words;
    unbox_into(rt, a.h(2), comp->primitive, words);
    uint64_t v = words[0] | (words.size() > 1 ? uint64_t(words[1]) << 32 : 0);
    std::memcpy(slot, &v, arr_obj.width);
    return 0;
  });
}

// ART natives of the VM core classes, reached by the bytecode merged into
// them in libcore mode (ADR-004): libcore's Java code of Class, Reference,
// Array... calls these instead of the public API.
void register_art_reflection_natives(rt::NativeRegistry& r) {
  const char* cls = "Ljava/lang/Class;";
  r.bind(cls, "getPrimitiveClass", "(Ljava/lang/String;)Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    static const std::pair<const char*, const char*> prims[] = {
        {"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"}, {"int", "I"},
        {"long", "J"},    {"float", "F"}, {"double", "D"}, {"void", "V"}};
    std::string name = rt.string_utf8(a.h(0));
    for (const auto& [n, d] : prims) {
      if (name == n) return rt.class_mirror(rt.find_class(d));
    }
    rt.throw_new("Ljava/lang/ClassNotFoundException;", name);
  });
  r.bind(cls, "getNameNative", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.intern(text::utf8_to_utf16(class_arg(rt, a.h(0), "Class")->java_name()));
  });
  r.bind(cls, "getSimpleNameNative", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(class_arg(rt, a.h(0), "Class")->simple_name());
  });
  r.bind(cls, "getInterfacesInternal", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    if (c->interfaces.empty()) return kNull;  // ART returns null when there are none
    std::vector<Handle> objs;
    for (rt::Class* i : c->interfaces) objs.push_back(rt.class_mirror(i));
    return object_array(rt, "[Ljava/lang/Class;", objs);
  });
  r.bind(cls, "getDeclaredFieldsUnchecked", "(Z)[Ljava/lang/reflect/Field;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<Handle> objs;
    for (const auto& f : class_arg(rt, a.h(0), "Class")->fields) {
      if (!a.z(1) || (f->access & dex::kAccPublic)) objs.push_back(make_field_object(rt, f.get()));
    }
    return object_array(rt, "[Ljava/lang/reflect/Field;", objs);
  });
  r.bind(cls, "getPublicDeclaredFields", "()[Ljava/lang/reflect/Field;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<Handle> objs;
    for (const auto& f : class_arg(rt, a.h(0), "Class")->fields) {
      if (f->access & dex::kAccPublic) objs.push_back(make_field_object(rt, f.get()));
    }
    return object_array(rt, "[Ljava/lang/reflect/Field;", objs);
  });
  r.bind(cls, "getPublicFieldRecursive", "(Ljava/lang/String;)Ljava/lang/reflect/Field;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = find_field(class_arg(rt, a.h(0), "Class"), rt.string_utf8(a.h(1)), true, true);
    return f ? make_field_object(rt, f) : kNull;
  });
  r.bind(cls, "getDeclaredMethodsUnchecked", "(Z)[Ljava/lang/reflect/Method;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<rt::Method*> ms;
    collect_methods(class_arg(rt, a.h(0), "Class"), a.z(1), false, false, ms);
    std::vector<Handle> objs;
    for (rt::Method* m : ms) objs.push_back(make_method_object(rt, m));
    return object_array(rt, "[Ljava/lang/reflect/Method;", objs);
  });
  r.bind(cls, "getDeclaredConstructorsInternal", "(Z)[Ljava/lang/reflect/Constructor;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<rt::Method*> ms;
    collect_methods(class_arg(rt, a.h(0), "Class"), a.z(1), false, true, ms);
    std::vector<Handle> objs;
    for (rt::Method* m : ms) objs.push_back(make_method_object(rt, m));
    return object_array(rt, "[Ljava/lang/reflect/Constructor;", objs);
  });
  r.bind(cls, "getDeclaredMethodInternal", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           rt::Class* c = class_arg(rt, a.h(0), "Class");
           std::string name = rt.string_utf8(a.h(1));
           std::string params = "(" + signature_params(rt, a.h(2)) + ")";
           for (const auto& m : c->methods) {
             if (m->name == name && m->signature.compare(0, params.size(), params) == 0) return make_method_object(rt, m.get());
           }
           return kNull;
         });
  r.bind(cls, "getDeclaredConstructorInternal", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           rt::Class* c = class_arg(rt, a.h(0), "Class");
           rt::Method* m = c->declared_method("<init>", "(" + signature_params(rt, a.h(1)) + ")V");
           return m ? make_method_object(rt, m) : kNull;
         });
  r.bind(cls, "classForName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("name == null");
    rt::Class* c = class_for_name(rt, a.h(0), a.h(2));
    if (a.z(1)) rt.ensure_initialized(c);
    return rt.class_mirror(c);
  }, S::kPartial, "un seul espace de classes (ADR-006 J2)");

  // Class.extData (dalvik.system.ClassExt), allocated on first use as in ART.
  r.bind(cls, "ensureExtDataPresent0", "()Ldalvik/system/ClassExt;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Object& mirror = rt.nonnull(a.h(0), "Class");
    rt::Field* f = rt.find_class("Ljava/lang/Class;")->declared_field("extData", "Ldalvik/system/ClassExt;");
    if (!f) rt.throw_new("Ljava/lang/InternalError;", "Class.extData absent");
    Handle ext = mirror.fields.at(f->slot);
    if (ext == kNull) {
      rt::Class* ext_class = rt.find_class("Ldalvik/system/ClassExt;");
      rt.ensure_initialized(ext_class);
      ext = rt.alloc(ext_class);
      rt.object(a.h(0))->fields.at(f->slot) = ext;
    }
    return ext;
  });

  // Annotation-backed metadata. The system annotations that place a class
  // (InnerClass, EnclosingClass, EnclosingMethod, MemberClasses) are read
  // below; the other annotations are not yet, and these answers describe a
  // class without them (logged as stubs).
  const char* none = "annotations DEX non lues : réponse « aucune »";
  auto null_result = [](Runtime&, NativeArgs&) -> uint64_t { return kNull; };
  auto false_result = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
  // EnclosingMethod: the method or constructor a local or anonymous class is in.
  auto enclosing = [](bool constructor) {
    return [constructor](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Class* local = class_arg(rt, a.h(0), "Class");
      const rt::ClassAnnotations& an = local->annotations();
      if (!an.enclosing_method) return kNull;
      const dex::DexFile& dex = *local->dex_cache->dex;
      const dex::MethodId& mid = dex.method(*an.enclosing_method);
      const std::string& name = dex.string(mid.name_idx);
      if ((name == "<init>") != constructor || name == "<clinit>") return kNull;
      rt::Class* owner = rt.lookup_class(dex.type_descriptor(mid.class_idx));
      rt::Method* m = owner ? owner->declared_method(name, dex.proto_signature(mid.proto_idx)) : nullptr;
      return m ? make_method_object(rt, m) : kNull;
    };
  };
  r.bind(cls, "getEnclosingMethodNative", "()Ljava/lang/reflect/Method;", enclosing(false));
  r.bind(cls, "getEnclosingConstructorNative", "()Ljava/lang/reflect/Constructor;", enclosing(true));
  r.bind(cls, "isAnonymousClass", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(class_arg(rt, a.h(0), "Class")->annotations().anonymous);
  });
  r.bind(cls, "getInnerClassFlags", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const rt::ClassAnnotations& an = class_arg(rt, a.h(0), "Class")->annotations();
    return an.inner ? rt::ret_int(an.inner_flags) : a.raw(1);
  });
  r.bind(cls, "getDeclaredClasses", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<Handle> objs;
    for (const std::string& d : class_arg(rt, a.h(0), "Class")->annotations().member_classes) {
      if (rt::Class* c = rt.lookup_class(d)) objs.push_back(rt.class_mirror(c));
    }
    return object_array(rt, "[Ljava/lang/Class;", objs);
  });
  r.bind(cls, "getSignatureAnnotation", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    return get_signature_annotation_array(rt, c->annotations().signature);
  });
  r.bind(cls, "getDeclaredAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
    const auto& map = c->annotations().annotations;
    auto it = map.find(anno_cls->descriptor);
    if (it != map.end() && it->second.visibility == 1) {
      return create_annotation_instance(rt, anno_cls, it->second);
    }
    return kNull;
  });
  r.bind(cls, "isDeclaredAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    rt::Class* anno_cls = class_arg(rt, a.h(1), "annotationClass");
    const auto& map = c->annotations().annotations;
    auto it = map.find(anno_cls->descriptor);
    return rt::ret_bool(it != map.end() && it->second.visibility == 1);
  });
  r.bind(cls, "getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    return get_declared_annotations(rt, c->annotations().annotations);
  });
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"getNestHostFromAnnotation", "()Ljava/lang/Class;"},
           {"getNestMembersFromAnnotation", "()[Ljava/lang/Class;"},
           {"getPermittedSubclassesFromAnnotation", "()[Ljava/lang/Class;"},
           {"getRecordAnnotationElement", "(Ljava/lang/String;Ljava/lang/Class;)[Ljava/lang/Object;"}}) {
    r.bind(cls, name, sig, null_result, S::kStub, none);
  }
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"isRecord0", "()Z"}, {"isValue", "()Z"}}) {
    r.bind(cls, name, sig, false_result, S::kStub, none);
  }

  const char* ref = "Ljava/lang/ref/Reference;";
  r.bind(ref, "getReferent", "()Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto* p = rt.peer<rt::ReferencePeer>(a.h(0));
    return p ? p->referent : kNull;
  });
  r.bind(ref, "refersTo0", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto* p = rt.peer<rt::ReferencePeer>(a.h(0));
    return rt::ret_bool((p ? p->referent : kNull) == a.h(1));
  });
  r.bind(ref, "clearReferent", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (auto* p = rt.peer<rt::ReferencePeer>(a.h(0))) p->referent = kNull;
    return 0;
  });

  const char* arr = "Ljava/lang/reflect/Array;";
  r.bind(arr, "createObjectArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "componentType");
    if (c->primitive == 'V') rt.throw_new("Ljava/lang/IllegalArgumentException;", "void[]");
    return rt.new_array("[" + c->descriptor, a.i32(1));
  });
  r.bind(arr, "createMultiArray", "(Ljava/lang/Class;[I)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "componentType");
    rt::ArrayObject& dims = rt.array(a.h(1), "dimensions");
    if (dims.length == 0 || dims.length > 255) rt.throw_new("Ljava/lang/IllegalArgumentException;", "dimensions");
    std::vector<int32_t> d(dims.length);
    std::memcpy(d.data(), dims.data.data(), d.size() * 4);
    std::function<Handle(size_t)> build = [&](size_t level) -> Handle {
      std::string desc = std::string(d.size() - level, '[') + c->descriptor;
      Handle h = rt.new_array(desc, d[level]);
      if (level + 1 < d.size()) {
        rt::GlobalRef keep(rt, h);
        for (int32_t i = 0; i < d[level]; ++i) {
          Handle sub = build(level + 1);
          std::memcpy(&rt.array(h, desc).data[size_t(i) * 4], &sub, 4);
        }
      }
      return h;
    };
    return build(0);
  });
}

// --- libcore's reflection classes ----------------------------------------------------------------
//
// With a boot class path (ADR-004), Method, Constructor, Field and
// AccessibleObject are libcore's own classes: their Java code (names, equals,
// hashCode, toString, generic signatures, annotation lookups) runs unchanged
// and calls ART's natives, ported here from art/runtime/native/
// java_lang_reflect_{Executable,Method,Constructor,Field}.cc and
// art/runtime/reflection.cc (android-17.0.0_r1). Not applied: the access checks
// of the calling class (VerifyAccess); the reflected member is always reachable.

// ArtMethod::GetInterfaceMethodIfProxy.
rt::Method* interface_method_if_proxy(rt::Method* m) { return m->interface_method ? m->interface_method : m; }

rt::Method* executable_arg(Runtime& rt, Handle h) {
  if (h == kNull) rt.throw_npe("Executable");
  rt::Method* m = rt.reflected_method(h);
  if (!m) rt.throw_new("Ljava/lang/InternalError;", "objet Executable sans méthode");
  return m;
}

rt::Field* field_arg(Runtime& rt, Handle h) {
  if (h == kNull) rt.throw_npe("Field");
  rt::Field* f = rt.reflected_field(h);
  if (!f) rt.throw_new("Ljava/lang/InternalError;", "objet Field sans champ");
  return f;
}

// AccessibleObject.override (setAccessible(true)).
bool is_accessible(Runtime& rt, Handle h) {
  rt::Object& o = rt.nonnull(h, "AccessibleObject");
  for (rt::Class* c = o.klass; c; c = c->super) {
    if (rt::Field* f = c->declared_field("override", "Z"); f && !f->is_static()) return o.fields.at(f->slot) != 0;
  }
  return false;
}

// The Pretty* helpers of ART's messages.
std::string pretty_descriptor(const std::string& d) { return text::descriptor_to_java(d); }

std::string pretty_type_of(Runtime& rt, Handle h) {
  return h == kNull ? "null" : pretty_descriptor(rt.class_of(h)->descriptor);
}

std::string pretty_method(const rt::Method* m, bool with_signature) {
  std::vector<std::string> params;
  std::string ret;
  dex::split_signature(m->signature, params, ret);
  std::string s = with_signature ? pretty_descriptor(ret) + " " : "";
  s += pretty_descriptor(m->owner->descriptor) + "." + m->name;
  if (with_signature) {
    s += "(";
    for (size_t i = 0; i < params.size(); ++i) s += (i ? ", " : "") + pretty_descriptor(params[i]);
    s += ")";
  }
  return s;
}

std::string pretty_field(const rt::Field* f) {
  return pretty_descriptor(f->type) + " " + pretty_descriptor(f->owner->descriptor) + "." + f->name;
}

std::string pretty_class(const rt::Class* c) { return "java.lang.Class<" + pretty_descriptor(c->descriptor) + ">"; }

std::string pretty_java_access_flags(uint32_t flags) {
  std::string s;
  if (flags & dex::kAccPublic) s += "public ";
  if (flags & dex::kAccProtected) s += "protected ";
  if (flags & dex::kAccPrivate) s += "private ";
  if (flags & dex::kAccFinal) s += "final ";
  if (flags & dex::kAccStatic) s += "static ";
  if (flags & dex::kAccAbstract) s += "abstract ";
  if (flags & dex::kAccInterface) s += "interface ";
  if (flags & dex::kAccTransient) s += "transient ";
  if (flags & dex::kAccVolatile) s += "volatile ";
  if (flags & dex::kAccSynchronized) s += "synchronized ";
  return s;
}

[[noreturn]] void throw_iae(Runtime& rt, const std::string& msg) { rt.throw_new("Ljava/lang/IllegalArgumentException;", msg); }

// VerifyObjectIsClass.
void verify_object_is_class(Runtime& rt, Handle o, rt::Class* c) {
  if (o == kNull) rt.throw_npe("null receiver");
  if (!rt.instance_of(o, c)) {
    throw_iae(rt, "Expected receiver of type " + pretty_descriptor(c->descriptor) + ", but got " + pretty_type_of(rt, o));
  }
}

int64_t primitive_as_long(char t, uint64_t v) {
  switch (t) {
    case 'B': return int8_t(v);
    case 'S': return int16_t(v);
    case 'C': return uint16_t(v);
    case 'Z': return v & 1;
    case 'J': return int64_t(v);
    default: return int32_t(v);
  }
}

double primitive_as_double(char t, uint64_t v) {
  if (t == 'F') {
    uint32_t b = uint32_t(v);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
  }
  if (t == 'D') {
    double d;
    std::memcpy(&d, &v, 8);
    return d;
  }
  return double(primitive_as_long(t, v));
}

// ConvertPrimitiveValue (reflection-inl.h): the identity or a widening
// primitive conversion (JLS 5.1.2), the value in a register's representation.
uint64_t convert_primitive_value(Runtime& rt, bool for_result, char src, char dst, uint64_t v) {
  if (src == dst) return v;
  bool ok = false;
  switch (src) {
    case 'B': ok = dst == 'S' || dst == 'I' || dst == 'J' || dst == 'F' || dst == 'D'; break;
    case 'C':
    case 'S': ok = dst == 'I' || dst == 'J' || dst == 'F' || dst == 'D'; break;
    case 'I': ok = dst == 'J' || dst == 'F' || dst == 'D'; break;
    case 'J': ok = dst == 'F' || dst == 'D'; break;
    case 'F': ok = dst == 'D'; break;
    default: break;
  }
  if (!ok) {
    std::string from = pretty_descriptor(std::string(1, src)), to = pretty_descriptor(std::string(1, dst));
    if (for_result) {
      rt.throw_new("Ljava/lang/ClassCastException;", "Couldn't convert result of type " + from + " to " + to);
    }
    throw_iae(rt, "Invalid primitive conversion from " + from + " to " + to);
  }
  switch (dst) {
    case 'F': return src == 'J' ? rt::ret_float(float(int64_t(v))) : rt::ret_float(float(primitive_as_double(src, v)));
    case 'D': return src == 'J' ? rt::ret_double(double(int64_t(v))) : rt::ret_double(primitive_as_double(src, v));
    case 'J': return uint64_t(primitive_as_long(src, v));
    default: return uint32_t(int32_t(primitive_as_long(src, v)));
  }
}

// UnboxPrimitive for a field (f) or a result (f null): the raw value of `o`
// for the type `dst`.
uint64_t unbox_primitive(Runtime& rt, Handle o, const std::string& dst, const rt::Field* f) {
  std::string kind = f ? "field " + pretty_descriptor(f->owner->descriptor) + "." + f->name : "result";
  if (dst[0] == 'L' || dst[0] == '[') {
    if (o != kNull && !rt.instance_of(o, rt.find_class(dst))) {
      if (f) throw_iae(rt, kind + " has type " + pretty_descriptor(dst) + ", got " + pretty_type_of(rt, o));
      rt.throw_new("Ljava/lang/ClassCastException;",
                   "Couldn't convert result of type " + pretty_type_of(rt, o) + " to " + pretty_descriptor(dst));
    }
    return o;
  }
  if (dst == "V") throw_iae(rt, "Can't unbox " + kind + " to void");
  if (o == kNull) {
    if (f) throw_iae(rt, kind + " has type " + pretty_descriptor(dst) + ", got null");
    rt.throw_npe("Expected to unbox a '" + pretty_descriptor(dst) + "' primitive type but was returned null");
  }
  char from = 0;
  uint64_t bits = 0;
  if (!unbox(rt, o, from, bits)) throw_iae(rt, kind + " has type " + pretty_descriptor(dst) + ", got " + pretty_type_of(rt, o));
  return convert_primitive_value(rt, !f, from, dst[0], bits);
}

// ArgArray::BuildArgArrayFromObjectArray: the receiver, then each argument
// unboxed and widened to its parameter's type.
std::vector<uint32_t> build_arg_array(Runtime& rt, rt::Method* m, Handle receiver, const std::vector<Handle>& args) {
  std::vector<std::string> params = params_of(m);
  std::vector<uint32_t> words;
  if (receiver != kNull) words.push_back(receiver);
  for (size_t i = 0; i < params.size(); ++i) {
    const std::string& p = params[i];
    Handle arg = args[i];
    auto mismatch = [&](const std::string& expected) {
      throw_iae(rt, "method " + pretty_method(m, false) + " argument " + std::to_string(i + 1) + " has type " + expected +
                        ", got " + pretty_type_of(rt, arg));
    };
    if (p[0] == 'L' || p[0] == '[') {
      if (arg != kNull && !rt.instance_of(arg, rt.find_class(p))) mismatch(pretty_descriptor(p));
      words.push_back(arg);
      continue;
    }
    if (arg == kNull) mismatch(pretty_descriptor(p));
    char from = 0;
    uint64_t bits = 0;
    // The boxes each parameter type accepts (DO_FIRST_ARG / DO_ARG).
    static const std::pair<char, const char*> accepted[] = {
        {'Z', "Z"}, {'B', "B"}, {'C', "C"}, {'S', "SB"}, {'I', "ICSB"}, {'J', "JICSB"}, {'F', "FJICSB"}, {'D', "DFJICSB"}};
    const char* ok = "";
    for (const auto& [t, list] : accepted) {
      if (t == p[0]) ok = list;
    }
    if (!unbox(rt, arg, from, bits) || !std::strchr(ok, from)) mismatch(pretty_descriptor(p));
    uint64_t v = convert_primitive_value(rt, false, from, p[0], bits);
    words.push_back(uint32_t(v));
    if (p == "J" || p == "D") words.push_back(uint32_t(v >> 32));
  }
  return words;
}

// CheckArgsForInvokeMethod.
std::vector<Handle> checked_args(Runtime& rt, rt::Method* np, Handle args_array) {
  std::vector<Handle> args = array_items(rt, args_array);
  size_t expected = params_of(np).size();
  if (args.size() != expected) {
    throw_iae(rt, "Wrong number of arguments; expected " + std::to_string(expected) + ", got " + std::to_string(args.size()));
  }
  return args;
}

// WellKnownClasses::StringInitToStringFactory: the StringFactory method taking
// the parameters of a String constructor.
rt::Method* string_factory_for(Runtime& rt, const rt::Method* ctor) {
  rt::Class* factory = rt.find_class("Ljava/lang/StringFactory;");
  rt.ensure_initialized(factory);
  std::string want = ctor->signature.substr(0, ctor->signature.find(')') + 1) + "Ljava/lang/String;";
  for (const auto& fm : factory->methods) {
    if (fm->is_static() && fm->signature == want && text::starts_with(fm->name, "new")) return fm.get();
  }
  rt.throw_unsupported(ctor->pretty(), "aucune méthode StringFactory équivalente");
}

// InvokeMethod (reflection.cc), behind Method.invoke.
uint64_t invoke_method(Runtime& rt, Handle method_obj, Handle receiver, Handle args_array) {
  rt::Method* m = executable_arg(rt, method_obj);
  rt::Class* declaring = m->owner;
  rt.ensure_initialized(declaring);
  if (m->is_static()) {
    receiver = kNull;
  } else {
    verify_object_is_class(rt, receiver, declaring);
    // FindVirtualMethodForVirtualOrInterface: direct methods are themselves.
    if (!(m->access & dex::kAccPrivate) && m->name != "<init>") {
      if (rt::Method* v = rt.resolve_virtual(rt.class_of(receiver), m->name, m->signature)) m = v;
    }
  }
  rt::Method* np = interface_method_if_proxy(m);
  std::vector<Handle> args = checked_args(rt, np, args_array);
  std::vector<uint32_t> words = build_arg_array(rt, np, receiver, args);
  uint64_t result = invoke_wrapped(rt, m, words);
  return box_result(rt, np->return_type(), result);
}

// annotations::GetExceptionTypesForMethod: the Throws annotation's classes,
// an empty array without it.
Handle exception_types(Runtime& rt, rt::Method* m) {
  std::vector<Handle> objs;
  for (const std::string& d : m->annotations().throws_classes) objs.push_back(rt.class_mirror(rt.find_class(d)));
  return object_array(rt, "[Ljava/lang/Class;", objs);
}

const rt::EncodedAnnotation* runtime_annotation(const std::unordered_map<std::string, rt::EncodedAnnotation>& map,
                                                const rt::Class* anno_cls) {
  auto it = map.find(anno_cls->descriptor);
  return it != map.end() && it->second.visibility == 1 ? &it->second : nullptr;
}

// Field values (GetFieldValue / SetFieldValue): the storage of the field for
// `receiver`, after CheckReceiver (the class of a static field initialized).
uint32_t* checked_field_storage(Runtime& rt, rt::Field* f, Handle receiver) {
  if (f->is_static()) {
    rt.ensure_initialized(f->owner);
    return &f->owner->statics.at(f->slot);
  }
  verify_object_is_class(rt, receiver, f->owner);
  return &rt.object(receiver)->fields.at(f->slot);
}

uint64_t read_storage(const rt::Field* f, const uint32_t* s) {
  uint64_t raw = f->wide() ? uint64_t(s[0]) | (uint64_t(s[1]) << 32) : s[0];
  return f->is_reference() ? raw : (f->wide() ? raw : uint32_t(int32_t(primitive_as_long(f->type[0], raw))));
}

void write_storage(const rt::Field* f, uint32_t* s, uint64_t v) {
  s[0] = uint32_t(v);
  if (f->wide()) s[1] = uint32_t(v >> 32);
}

int32_t app_target_sdk(Runtime& rt) {
  Handle vm = Handle(rt.call_static("Ldalvik/system/VMRuntime;", "getRuntime", "()Ldalvik/system/VMRuntime;"));
  return vm == kNull ? 10000 : int32_t(rt.call_virtual(vm, "getTargetSdkVersion", "()I"));
}

// Android 16 (Baklava): SdkVersion::kB.
constexpr int32_t kSdkBaklava = 36;

// ArtField::IsWriteProtected.
bool is_write_protected(const rt::Field* f) {
  const std::string& d = f->owner->descriptor;
  if (d == "Ljava/lang/System;" && f->type == "Ljava/io/PrintStream;" && (f->name == "out" || f->name == "err")) return true;
  if (d == "Ljava/lang/System;" && f->type == "Ljava/io/InputStream;" && f->name == "in") return true;
  constexpr uint32_t kStaticFinal = dex::kAccStatic | dex::kAccFinal;
  return (f->access & kStaticFinal) == kStaticFinal && f->owner->boot &&
         (d == "Landroid/os/Build;" || d == "Landroid/os/Build$VERSION;");
}

// ArtField::IsUnmodifiable (kAccMonotonic is never set here). The platform
// is Android 17, past Baklava.
bool is_unmodifiable(Runtime& rt, const rt::Field* f) {
  if (!(f->access & dex::kAccFinal) || is_write_protected(f)) return false;
  if (app_target_sdk(rt) <= kSdkBaklava) return false;
  return f->is_static();  // includes the MethodHandle, VarHandle, *FieldUpdater and Unsafe types
}

// ThrowIAEIfFieldIsNotOverwritable.
void check_overwritable(Runtime& rt, const rt::Field* f) {
  if (is_write_protected(f) ? app_target_sdk(rt) <= kSdkBaklava : !is_unmodifiable(rt, f)) return;
  rt.throw_new("Ljava/lang/IllegalAccessException;", "Cannot set " + pretty_java_access_flags(f->access) + " field " +
                                                          pretty_field(f) + " of class " + pretty_class(f->owner));
}

// VerifyFieldAccess<true>: without setAccessible(true), a final field is read-only.
void check_set_access(Runtime& rt, Handle field_obj, const rt::Field* f) {
  if (is_accessible(rt, field_obj) || !(f->access & dex::kAccFinal)) return;
  rt.throw_new("Ljava/lang/IllegalAccessException;", "Cannot set " + pretty_java_access_flags(f->access) + " field " +
                                                          pretty_field(f) + " of class " + pretty_class(f->owner));
}

void register_libcore_members(rt::NativeRegistry& r) {
  const char* ex = "Ljava/lang/reflect/Executable;";
  r.bind(ex, "getAnnotationNative", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (m->interface_method) return kNull;
    rt::Class* anno = class_arg(rt, a.h(1), "annotationType");
    const rt::EncodedAnnotation* ea = runtime_annotation(m->annotations().annotations, anno);
    return ea ? create_annotation_instance(rt, anno, *ea) : kNull;
  });
  r.bind(ex, "getDeclaredAnnotationsNative", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
    return get_declared_annotations(rt, m->annotations().annotations);
  });
  r.bind(ex, "getParameterAnnotationsNative", "()[[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (m->interface_method) return kNull;
    const auto& lists = m->annotations().parameter_annotations;
    if (lists.empty()) return kNull;
    // Constructors with implicit parameters (inner classes) get empty sets in
    // front, unless the class is local, anonymous or an enum.
    const size_t n = lists.size(), params = params_of(m).size();
    const bool resize = m->name == "<init>" && n != params && !m->owner->annotations().enclosing_method &&
                        !(m->owner->access & dex::kAccEnum);
    const size_t total = resize ? params : n;
    const char* desc = "[[Ljava/lang/annotation/Annotation;";
    Handle outer = rt.new_array(desc, int32_t(total));
    rt::GlobalRef keep(rt, outer);
    for (size_t i = 0; i < total; ++i) {
      std::ptrdiff_t src = std::ptrdiff_t(i) - (std::ptrdiff_t(total) - std::ptrdiff_t(n));
      Handle set = src >= 0 ? get_declared_annotations(rt, lists[size_t(src)])
                            : rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
      std::memcpy(&rt.array(outer, desc).data[i * 4], &set, 4);
    }
    return outer;
  });
  r.bind(ex, "getParameters0", "()[Ljava/lang/reflect/Parameter;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) return kNull;
    // The MethodParameters system annotation (javac -parameters).
    const auto& map = m->annotations().annotations;
    auto it = map.find("Ldalvik/annotation/MethodParameters;");
    if (it == map.end()) return kNull;
    const rt::EncodedAnnotation& ea = it->second;
    auto names = ea.elements.find("names");
    auto flags = ea.elements.find("accessFlags");
    if (names == ea.elements.end() || flags == ea.elements.end()) {
      throw_iae(rt, "Missing parameter metadata for names or access flags for " + pretty_method(m, true));
    }
    size_t count = names->second.array.size();
    if (count != flags->second.array.size()) {
      throw_iae(rt, "Inconsistent parameter metadata for " + pretty_method(m, true) + ". names length: " +
                        std::to_string(count) + ", access flags length: " + std::to_string(flags->second.array.size()));
    }
    rt::Class* parameter = rt.find_class("Ljava/lang/reflect/Parameter;");
    rt::Method* init = parameter->declared_method("<init>", "(Ljava/lang/String;ILjava/lang/reflect/Executable;I)V");
    if (!init) rt.throw_new("Ljava/lang/InternalError;", "Parameter.<init> absent");
    const char* desc = "[Ljava/lang/reflect/Parameter;";
    Handle out = rt.new_array(desc, int32_t(count));
    rt::GlobalRef keep(rt, out);
    for (size_t i = 0; i < count; ++i) {
      const dex::EncodedValue& n = names->second.array[i];
      Handle name = n.type == dex::EncodedValue::kString && ea.dex && n.bits < ea.dex->num_strings()
                        ? rt.new_string_utf8(ea.dex->string(uint32_t(n.bits)))
                        : kNull;
      Handle p = rt.alloc(parameter);
      uint32_t args[5] = {p, name, uint32_t(flags->second.array[i].bits), a.h(0), uint32_t(i)};
      rt.invoke(init, args, 5);
      std::memcpy(&rt.array(out, desc).data[i * 4], &p, 4);
    }
    return out;
  });
  r.bind(ex, "getSignatureAnnotation", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) return kNull;
    return get_signature_annotation_array(rt, m->annotations().signature);
  });
  r.bind(ex, "isAnnotationPresentNative", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) return rt::ret_bool(false);
    return rt::ret_bool(runtime_annotation(m->annotations().annotations, class_arg(rt, a.h(1), "annotationType")));
  });
  r.bind(ex, "compareMethodParametersInternal", "(Ljava/lang/reflect/Method;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<std::string> mine = params_of(interface_method_if_proxy(executable_arg(rt, a.h(0))));
    std::vector<std::string> other = params_of(interface_method_if_proxy(executable_arg(rt, a.h(1))));
    if (mine.size() != other.size()) return rt::ret_int(int32_t(mine.size()) - int32_t(other.size()));
    for (size_t i = 0; i < mine.size(); ++i) {
      int cmp = mine[i].compare(other[i]);
      if (cmp != 0) return rt::ret_int(cmp < 0 ? -1 : 1);
    }
    return rt::ret_int(0);
  });
  r.bind(ex, "getMethodNameInternal", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.intern(text::utf8_to_utf16(interface_method_if_proxy(executable_arg(rt, a.h(0)))->name));
  });
  r.bind(ex, "getMethodReturnTypeInternal", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return class_of_descriptor(rt, return_of(interface_method_if_proxy(executable_arg(rt, a.h(0)))));
  });
  r.bind(ex, "getParameterTypesInternal", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<std::string> params = params_of(interface_method_if_proxy(executable_arg(rt, a.h(0))));
    if (params.empty()) return kNull;  // no type list
    std::vector<Handle> objs;
    for (const auto& p : params) objs.push_back(class_of_descriptor(rt, p));
    return object_array(rt, "[Ljava/lang/Class;", objs);
  });
  r.bind(ex, "getParameterCountInternal", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(params_of(interface_method_if_proxy(executable_arg(rt, a.h(0)))).size()));
  });

  const char* method = "Ljava/lang/reflect/Method;";
  r.bind(method, "getDefaultValue", "()Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (!(m->owner->access & dex::kAccAnnotation)) return kNull;
    // The AnnotationDefault system annotation of the annotation type: an
    // annotation of that type holding the default of each element.
    const auto& map = m->owner->annotations().annotations;
    auto it = map.find("Ldalvik/annotation/AnnotationDefault;");
    if (it == map.end()) return kNull;
    auto value = it->second.elements.find("value");
    if (value == it->second.elements.end() || value->second.type != dex::EncodedValue::kAnnotation) return kNull;
    const dex::EncodedValue& defaults = value->second;
    for (size_t i = 0; i < defaults.names.size(); ++i) {
      if (defaults.names[i] >= it->second.dex->num_strings() || it->second.dex->string(defaults.names[i]) != m->name) continue;
      uint64_t raw = convert_encoded_value(rt, defaults.array[i], it->second.dex, m->signature);
      return box_result(rt, m->return_type(), raw);
    }
    return kNull;
  });
  r.bind(method, "getExceptionTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return exception_types(rt, interface_method_if_proxy(executable_arg(rt, a.h(0))));
  });
  r.bind(method, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return invoke_method(rt, a.h(0), a.h(1), a.h(2));
  }, S::kPartial, "contrôle d'accès de l'appelant non appliqué");

  const char* ctor = "Ljava/lang/reflect/Constructor;";
  r.bind(ctor, "getExceptionTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return exception_types(rt, interface_method_if_proxy(executable_arg(rt, a.h(0))));
  });
  r.bind(ctor, "newInstance0", "([Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    rt::Class* c = m->owner;
    if (c->is_abstract()) {
      rt.throw_new("Ljava/lang/InstantiationException;", std::string("Can't instantiate ") +
                                                             (c->is_interface() ? "interface " : "abstract class ") +
                                                             pretty_descriptor(c->descriptor));
    }
    rt.ensure_initialized(c);
    std::vector<Handle> args = checked_args(rt, m, a.h(1));
    if (c->descriptor == "Ljava/lang/String;") {
      rt::Method* factory = string_factory_for(rt, m);
      std::vector<uint32_t> words = build_arg_array(rt, m, kNull, args);
      return invoke_wrapped(rt, factory, words);
    }
    Handle receiver = rt.alloc(c);
    std::vector<uint32_t> words = build_arg_array(rt, m, receiver, args);
    invoke_wrapped(rt, m, words);
    return receiver;
  }, S::kPartial, "contrôle d'accès de l'appelant non appliqué");
  r.bind(ctor, "newInstanceFromSerialization", "(Ljava/lang/Class;Ljava/lang/Class;)Ljava/lang/Object;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           rt::Class* ctor_class = class_arg(rt, a.h(0), "ctorClass");
           rt::Class* alloc_class = class_arg(rt, a.h(1), "allocClass");
           rt::Method* init = ctor_class->declared_method("<init>", "()V");
           if (!init) rt.throw_new("Ljava/lang/NoSuchMethodError;", "<init>");
           rt.ensure_initialized(alloc_class);
           Handle obj = rt.alloc(alloc_class);
           uint32_t self = obj;
           rt.invoke(init, &self, 1);
           return obj;
         });

  const char* field = "Ljava/lang/reflect/Field;";
  r.bind(field, "getAnnotationNative", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    if (is_proxy_class(f->owner)) return kNull;
    rt::Class* anno = class_arg(rt, a.h(1), "annotationType");
    const rt::EncodedAnnotation* ea = runtime_annotation(f->annotations().annotations, anno);
    return ea ? create_annotation_instance(rt, anno, *ea) : kNull;
  });
  r.bind(field, "isAnnotationPresentNative", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    if (is_proxy_class(f->owner)) return rt::ret_bool(false);
    return rt::ret_bool(runtime_annotation(f->annotations().annotations, class_arg(rt, a.h(1), "annotationType")));
  });
  r.bind(field, "getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    if (is_proxy_class(f->owner)) return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
    return get_declared_annotations(rt, f->annotations().annotations);
  });
  r.bind(field, "getSignatureAnnotation", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    if (is_proxy_class(f->owner)) return kNull;
    return get_signature_annotation_array(rt, f->annotations().signature);
  });
  r.bind(field, "getNameInternal", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.intern(text::utf8_to_utf16(field_arg(rt, a.h(0))->name));
  });
  r.bind(field, "getArtField", "()J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.field_cookie(field_arg(rt, a.h(0)));
  }, S::kPartial, "identifiant opaque (cookie), pas une adresse");
  r.bind(field, "isUnmodifiable0", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(is_unmodifiable(rt, field_arg(rt, a.h(0))));
  });
  r.bind(field, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    uint64_t v = read_storage(f, checked_field_storage(rt, f, a.h(1)));
    return f->is_reference() ? v : box_result(rt, f->type[0], v);
  });
  for (char t : std::string("ZBCSIJFD")) {
    static const std::map<char, std::string> names = {{'Z', "Boolean"}, {'B', "Byte"},  {'C', "Char"},  {'S', "Short"},
                                                      {'I', "Int"},     {'J', "Long"},  {'F', "Float"}, {'D', "Double"}};
    std::string type(1, t);
    r.bind(field, "get" + names.at(t), "(Ljava/lang/Object;)" + type, [t](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Field* f = field_arg(rt, a.h(0));
      uint64_t v = read_storage(f, checked_field_storage(rt, f, a.h(1)));
      if (f->is_reference()) throw_iae(rt, "Not a primitive field: " + pretty_field(f));
      return convert_primitive_value(rt, false, f->type[0], t, v);
    });
    r.bind(field, "set" + names.at(t), "(Ljava/lang/Object;" + type + ")V", [t](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Field* f = field_arg(rt, a.h(0));
      uint32_t* s = checked_field_storage(rt, f, a.h(1));
      check_overwritable(rt, f);
      if (f->is_reference()) throw_iae(rt, "Not a primitive field: " + pretty_field(f));
      uint64_t in = (t == 'J' || t == 'D') ? a.i64(2) : a.raw(2);
      uint64_t v = convert_primitive_value(rt, false, t, f->type[0], in);
      check_set_access(rt, a.h(0), f);
      write_storage(f, s, v);
      return 0;
    });
  }
  r.bind(field, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    uint32_t* s = checked_field_storage(rt, f, a.h(1));
    check_overwritable(rt, f);
    uint64_t v = unbox_primitive(rt, a.h(2), f->type, f);
    check_set_access(rt, a.h(0), f);
    if (!f->is_static()) s = &rt.object(a.h(1))->fields.at(f->slot);  // unboxing may have run Java code
    write_storage(f, s, v);
    return 0;
  });
}

}  // namespace

void register_java_reflect(rt::NativeRegistry& r, Host&) {
  register_members(r);
  register_class_reflection(r);
  register_art_reflection_natives(r);
  register_libcore_members(r);
  // Class objects for primitive types (int.class compiles to Integer.TYPE).
  for (auto [cls, prim] : {std::pair{"Ljava/lang/Integer;", "I"}, std::pair{"Ljava/lang/Long;", "J"},
                           std::pair{"Ljava/lang/Short;", "S"}, std::pair{"Ljava/lang/Byte;", "B"},
                           std::pair{"Ljava/lang/Character;", "C"}, std::pair{"Ljava/lang/Boolean;", "Z"},
                           std::pair{"Ljava/lang/Float;", "F"}, std::pair{"Ljava/lang/Double;", "D"}}) {
    std::string p = prim;
    r.extend(cls).static_field("TYPE", "Ljava/lang/Class;", [p](Runtime& rt) -> uint64_t { return rt.class_mirror(rt.find_class(p)); });
  }
  r.define("Ljava/lang/Void;", "Ljava/lang/Object;", {}, 0x1 | 0x10)
      .static_field("TYPE", "Ljava/lang/Class;", [](Runtime& rt) -> uint64_t { return rt.class_mirror(rt.find_class("V")); });
}

}  // namespace aa::bridge
