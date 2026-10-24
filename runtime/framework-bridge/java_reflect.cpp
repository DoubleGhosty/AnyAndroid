// java.lang.reflect and the reflective parts of java.lang.Class.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common/log.h"
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
  if (uint32_t* cause = rt::throwable_field(rt, ite, "cause", "Ljava/lang/Throwable;")) *cause = target;
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

// --- Annotations ---------------------------------------------------------------------------------
//
// As art/runtime/dex/dex_file_annotations.cc (android-17.0.0_r1): with libcore,
// an annotation is made by libcore.reflect.AnnotationFactory.createAnnotation
// from AnnotationMember objects, the values converted by ProcessAnnotationValue;
// its Java code supplies the defaults, toString, equals and hashCode. Without a
// boot class path (the bridge's own classes) a minimal synthetic class stands in.

using rt::AnnotationSet;
using rt::EncodedAnnotation;
using V = dex::EncodedValue;

constexpr uint8_t kVisibilityBuild = 0;
constexpr uint8_t kVisibilityRuntime = 1;

// A class named by a DEX file, or nullptr when it does not resolve.
rt::Class* resolve_or_null(Runtime& rt, const std::string& descriptor) {
  try {
    return rt.find_class(descriptor);
  } catch (const rt::JavaThrow&) {
    return nullptr;
  }
}

// IsVisibilityCompatible: applications targeting Android 6.0 or older also
// see build-visibility annotations.
bool visibility_compatible(Runtime& rt, uint8_t actual) {
  if (actual == kVisibilityRuntime) return true;
  if (actual != kVisibilityBuild) return false;
  Handle vm = Handle(rt.call_static("Ldalvik/system/VMRuntime;", "getRuntime", "()Ldalvik/system/VMRuntime;"));
  int32_t target = vm == kNull ? 10000 : int32_t(rt.call_virtual(vm, "getTargetSdkVersion", "()I"));
  return target > 0 && target <= 23;
}

// DexFile::AnnotationValue: a primitive (its type and register bits) or an
// object (primitive 0, the handle in `bits`).
struct AnnotationValue {
  bool ok = false;
  char primitive = 0;
  uint64_t bits = 0;
};

Handle process_encoded_annotation(Runtime& rt, const dex::DexFile& dex, const std::string& type_descriptor,
                                  const std::vector<std::pair<std::string, V>>& elements);

rt::Method* resolve_method_id(Runtime& rt, const dex::DexFile& dex, uint32_t idx) {
  if (idx >= dex.num_methods()) return nullptr;
  const dex::MethodId& mid = dex.method(idx);
  rt::Class* c = resolve_or_null(rt, dex.type_descriptor(mid.class_idx));
  for (rt::Class* k = c; k; k = k->super) {
    if (rt::Method* m = k->declared_method(dex.string(mid.name_idx), dex.proto_signature(mid.proto_idx))) return m;
  }
  return nullptr;
}

rt::Field* resolve_field_id(Runtime& rt, const dex::DexFile& dex, uint32_t idx) {
  if (idx >= dex.num_fields()) return nullptr;
  const dex::FieldId& fid = dex.field(idx);
  rt::Class* c = resolve_or_null(rt, dex.type_descriptor(fid.class_idx));
  for (rt::Class* k = c; k; k = k->super) {
    if (rt::Field* f = k->declared_field(dex.string(fid.name_idx), dex.type_descriptor(fid.type_idx))) return f;
  }
  return nullptr;
}

// ProcessAnnotationValue. `all_objects` boxes primitives (kAllObjects);
// otherwise they stay raw (kPrimitivesOrObjects, the elements of an array).
AnnotationValue process_annotation_value(Runtime& rt, const dex::DexFile& dex, const V& v, rt::Class* array_class,
                                         bool all_objects) {
  AnnotationValue out;
  auto object = [&](Handle h) {
    out.ok = true;
    out.bits = h;
    return out;
  };
  switch (v.type) {
    case V::kByte: out.primitive = 'B'; out.bits = uint32_t(int32_t(int8_t(v.bits))); break;
    case V::kShort: out.primitive = 'S'; out.bits = uint32_t(int32_t(int16_t(v.bits))); break;
    case V::kChar: out.primitive = 'C'; out.bits = uint16_t(v.bits); break;
    case V::kInt: out.primitive = 'I'; out.bits = uint32_t(v.bits); break;
    case V::kLong: out.primitive = 'J'; out.bits = v.bits; break;
    case V::kFloat: out.primitive = 'F'; out.bits = uint32_t(v.bits); break;
    case V::kDouble: out.primitive = 'D'; out.bits = v.bits; break;
    case V::kBoolean: out.primitive = 'Z'; out.bits = v.bits & 1; break;
    case V::kString:
      if (v.bits >= dex.num_strings()) return out;
      return object(rt.intern(text::utf8_to_utf16(dex.string(uint32_t(v.bits)))));
    case V::kType: {
      if (v.bits >= dex.num_types()) return out;
      const std::string& d = dex.type_descriptor(uint32_t(v.bits));
      try {
        return object(rt.class_mirror(rt.find_class(d)));
      } catch (const rt::JavaThrow& t) {
        if (!all_objects) return out;
        // The value is a TypeNotPresentException, thrown by the element's accessor.
        Handle cause = t.exception;
        return object(construct(rt, "Ljava/lang/TypeNotPresentException;", "(Ljava/lang/String;Ljava/lang/Throwable;)V",
                                {rt.new_string_utf8(d), cause}));
      }
    }
    case V::kMethod: {
      rt::Method* m = resolve_method_id(rt, dex, uint32_t(v.bits));
      if (!m) return out;
      return object(rt.reflect_method(m));
    }
    case V::kField: {
      rt::Field* f = resolve_field_id(rt, dex, uint32_t(v.bits));
      if (!f) return out;
      return object(rt.reflect_field(f));
    }
    case V::kEnum: {
      rt::Field* f = resolve_field_id(rt, dex, uint32_t(v.bits));
      if (!f || !f->is_static()) return out;
      rt.ensure_initialized(f->owner);
      return object(Handle(f->owner->statics.at(f->slot)));
    }
    case V::kArray: {
      if (!array_class || !array_class->component) return out;
      rt::Class* component = array_class->component;
      Handle arr = rt.new_array(array_class, int32_t(v.array.size()));
      rt::GlobalRef keep(rt, arr);
      for (size_t i = 0; i < v.array.size(); ++i) {
        AnnotationValue e = process_annotation_value(rt, dex, v.array[i], component, false);
        if (!e.ok) return out;
        rt::ArrayObject& a = rt.array(arr, "annotation");
        if (bool(component->primitive) != bool(e.primitive)) return out;  // a value of another kind
        std::memcpy(&a.data[i * a.width], &e.bits, a.width);
      }
      return object(arr);
    }
    case V::kAnnotation: {
      if (v.bits >= dex.num_types()) return out;
      std::vector<std::pair<std::string, V>> elements;
      for (size_t i = 0; i < v.names.size() && i < v.array.size(); ++i) {
        if (v.names[i] >= dex.num_strings()) return out;
        elements.emplace_back(dex.string(v.names[i]), v.array[i]);
      }
      Handle h = process_encoded_annotation(rt, dex, dex.type_descriptor(uint32_t(v.bits)), elements);
      if (h == kNull) return out;
      return object(h);
    }
    case V::kNull: return object(kNull);
    default: return out;  // method types and handles are unexpected here
  }
  out.ok = true;
  if (all_objects) {
    out.bits = box(rt, out.primitive, out.bits);
    out.primitive = 0;
  }
  return out;
}

// The bridge's synthetic annotation class (no boot class path): its accessors
// read the encoded values, without defaults.
void setup_annotation_proxy_methods(Runtime&, rt::Class* proxy_cls, rt::Class* anno_cls) {
  auto add = [&](std::string name, std::string sig, uint32_t words, rt::NativeFn fn) {
    auto m = std::make_unique<rt::Method>();
    m->owner = proxy_cls;
    m->name = std::move(name);
    m->signature = std::move(sig);
    m->access = dex::kAccPublic | (m->name == "<init>" ? uint32_t(dex::kAccConstructor) : 0u);
    m->arg_words = words;
    m->native = std::move(fn);
    proxy_cls->methods.push_back(std::move(m));
  };
  add("<init>", "()V", 1, [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  add("annotationType", "()Ljava/lang/Class;", 1,
      [anno_cls](Runtime& rt, NativeArgs&) -> uint64_t { return rt.class_mirror(anno_cls); });
  for (const auto& method : anno_cls->methods) {
    if (method->name == "<clinit>" || method->name == "<init>") continue;
    std::string m_name = method->name;
    rt::Class* ret = nullptr;
    std::string ret_desc = return_of(method.get());
    add(m_name, method->signature, 1, [m_name, ret_desc, ret](Runtime& rt, NativeArgs& a) mutable -> uint64_t {
      auto* peer = rt.peer<rt::AnnotationPeer>(a.h(0));
      if (!peer) rt.throw_new("Ljava/lang/InternalError;", "AnnotationPeer manquant");
      auto it = peer->elements.find(m_name);
      if (it == peer->elements.end()) rt.throw_new("Ljava/lang/annotation/IncompleteAnnotationException;", m_name);
      if (!ret) ret = rt.find_class(ret_desc);
      AnnotationValue v = process_annotation_value(rt, *peer->dex, it->second, ret, false);
      if (!v.ok) rt.throw_new("Ljava/lang/annotation/AnnotationFormatError;", m_name);
      return v.bits;
    });
  }
  add("equals", "(Ljava/lang/Object;)Z", 2, [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto* p1 = rt.peer<rt::AnnotationPeer>(a.h(0));
    auto* p2 = rt.peer<rt::AnnotationPeer>(a.h(1));
    if (a.h(0) == a.h(1)) return rt::ret_bool(true);
    if (!p1 || !p2 || p1->anno_class != p2->anno_class || p1->elements.size() != p2->elements.size()) return rt::ret_bool(false);
    for (const auto& [k, v1] : p1->elements) {
      auto it2 = p2->elements.find(k);
      if (it2 == p2->elements.end() || v1.type != it2->second.type || v1.bits != it2->second.bits) return rt::ret_bool(false);
    }
    return rt::ret_bool(true);
  });
  add("hashCode", "()I", 1, [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto* p = rt.peer<rt::AnnotationPeer>(a.h(0));
    int32_t hash = 0;
    if (p) {
      for (const auto& [k, v] : p->elements) hash += (127 * rt::java_string_hash(rt::u16(k))) ^ int32_t(v.bits ^ (v.bits >> 32));
    }
    return rt::ret_int(hash);
  });
  add("toString", "()Ljava/lang/String;", 1, [anno_cls](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8("@" + anno_cls->java_name() + "()");
  });
}

Handle legacy_annotation(Runtime& rt, rt::Class* anno_cls, const dex::DexFile& dex,
                         const std::vector<std::pair<std::string, V>>& elements) {
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
  peer->dex = &dex;
  for (const auto& [n, v] : elements) peer->elements.emplace(n, v);
  rt.object(h)->peer = std::move(peer);
  return h;
}

// CreateAnnotationMember: an AnnotationMember of the annotation type's
// element `name`; kNull when the element or its value does not resolve.
Handle create_annotation_member(Runtime& rt, rt::Class* anno_cls, const dex::DexFile& dex, const std::string& name,
                                const V& value) {
  rt::Method* method = nullptr;  // FindDeclaredVirtualMethodByName
  for (const auto& m : anno_cls->methods) {
    if (m->name == name && !m->is_static() && m->name != "<init>") {
      method = m.get();
      break;
    }
  }
  if (!method) return kNull;
  Handle string_name = rt.new_string_utf8(name);
  rt::Class* ret = resolve_or_null(rt, return_of(method));
  if (!ret) return kNull;
  AnnotationValue v = process_annotation_value(rt, dex, value, ret, true);
  if (!v.ok) return kNull;
  Handle value_object = Handle(v.bits);
  Handle method_object = rt.reflect_method(method);
  return construct(rt, "Llibcore/reflect/AnnotationMember;",
                   "(Ljava/lang/String;Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/reflect/Method;)V",
                   {string_name, value_object, rt.class_mirror(ret), method_object});
}

// ProcessEncodedAnnotation.
Handle process_encoded_annotation(Runtime& rt, const dex::DexFile& dex, const std::string& type_descriptor,
                                  const std::vector<std::pair<std::string, V>>& elements) {
  rt::Class* anno_cls = resolve_or_null(rt, type_descriptor);
  if (!anno_cls) {
    log::info("libcore", "classe d'annotation introuvable : " + type_descriptor);
    return kNull;
  }
  if (!rt.libcore_mode()) return legacy_annotation(rt, anno_cls, dex, elements);
  const char* desc = "[Llibcore/reflect/AnnotationMember;";
  Handle members = elements.empty() ? kNull : rt.new_array(desc, int32_t(elements.size()));
  rt::GlobalRef keep(rt, members);
  for (size_t i = 0; i < elements.size(); ++i) {
    Handle member = create_annotation_member(rt, anno_cls, dex, elements[i].first, elements[i].second);
    if (member == kNull) return kNull;
    std::memcpy(&rt.array(members, desc).data[i * 4], &member, 4);
  }
  return Handle(rt.call_static("Llibcore/reflect/AnnotationFactory;", "createAnnotation",
                               "(Ljava/lang/Class;[Llibcore/reflect/AnnotationMember;)Ljava/lang/annotation/Annotation;",
                               {rt.class_mirror(anno_cls), members}));
}

Handle annotation_object(Runtime& rt, const EncodedAnnotation& ea) {
  return process_encoded_annotation(rt, *ea.dex, ea.type_descriptor, ea.elements);
}

// ProcessAnnotationSet: the runtime-visible annotations of a set, in its
// order, without those that do not resolve.
Handle process_annotation_set(Runtime& rt, const AnnotationSet& set) {
  const char* desc = "[Ljava/lang/annotation/Annotation;";
  Handle result = rt.new_array(desc, int32_t(set.items.size()));
  rt::GlobalRef keep(rt, result);
  int32_t n = 0;
  for (const EncodedAnnotation& ea : set.items) {
    if (ea.visibility != kVisibilityRuntime) continue;
    Handle h = annotation_object(rt, ea);
    if (h != kNull) std::memcpy(&rt.array(result, desc).data[size_t(n++) * 4], &h, 4);
  }
  if (size_t(n) == set.items.size()) return result;
  Handle trimmed = rt.new_array(desc, n);
  std::memcpy(rt.array(trimmed, desc).data.data(), rt.array(result, desc).data.data(), size_t(n) * 4);
  return trimmed;
}

// GetAnnotationItemFromAnnotationSet: the first runtime-visible annotation of
// the set whose type resolves to `anno`.
const EncodedAnnotation* annotation_item(Runtime& rt, const AnnotationSet& set, rt::Class* anno) {
  for (const EncodedAnnotation& ea : set.items) {
    if (!visibility_compatible(rt, ea.visibility)) continue;
    if (resolve_or_null(rt, ea.type_descriptor) == anno) return &ea;
  }
  return nullptr;
}

Handle annotation_from_set(Runtime& rt, const AnnotationSet& set, rt::Class* anno) {
  const EncodedAnnotation* ea = annotation_item(rt, set, anno);
  return ea ? annotation_object(rt, *ea) : kNull;
}

// Class.getAnnotation and Class.isAnnotationPresent: the declared annotation,
// else, for an @Inherited type, the first superclass declaring one.
bool inherited_annotation_type(Runtime& rt, rt::Class* anno) {
  rt::Class* inherited = resolve_or_null(rt, "Ljava/lang/annotation/Inherited;");
  return inherited && annotation_item(rt, anno->annotations().annotations, inherited);
}

Handle get_annotation_for_class(Runtime& rt, rt::Class* cls, rt::Class* anno_cls) {
  if (!cls || !anno_cls) return kNull;
  Handle h = annotation_from_set(rt, cls->annotations().annotations, anno_cls);
  if (h != kNull || !inherited_annotation_type(rt, anno_cls)) return h;
  for (rt::Class* k = cls->super; k; k = k->super) {
    h = annotation_from_set(rt, k->annotations().annotations, anno_cls);
    if (h != kNull) return h;
  }
  return kNull;
}

bool is_annotation_present_for_class(Runtime& rt, rt::Class* cls, rt::Class* anno_cls) {
  if (!cls || !anno_cls) return false;
  if (annotation_item(rt, cls->annotations().annotations, anno_cls)) return true;
  if (!inherited_annotation_type(rt, anno_cls)) return false;
  for (rt::Class* k = cls->super; k; k = k->super) {
    if (annotation_item(rt, k->annotations().annotations, anno_cls)) return true;
  }
  return false;
}

Handle get_signature_annotation_array(Runtime& rt, const std::vector<std::string>& sig) {
  if (sig.empty()) return kNull;
  std::vector<Handle> items;
  items.reserve(sig.size());
  for (const auto& s : sig) items.push_back(rt.new_string_utf8(s));
  return object_array(rt, "[Ljava/lang/String;", items);
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
  // libcore's Class.getModifiers over ART's access flags: an array has its
  // component's visibility, abstract and final; a member class the flags of
  // its InnerClass annotation.
  c.method("getModifiers", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::function<int32_t(rt::Class*)> modifiers = [&](rt::Class* k) -> int32_t {
      if (k->component) {
        int32_t m = modifiers(k->component);
        if (m & dex::kAccInterface) m &= ~int32_t(dex::kAccInterface | dex::kAccStatic);
        return m | dex::kAccAbstract | dex::kAccFinal;
      }
      if (k->primitive) return dex::kAccPublic | dex::kAccFinal | dex::kAccAbstract;
      int32_t flags = int32_t(k->access & 0xFFFF);
      const rt::ClassAnnotations& an = k->annotations();
      return (an.inner ? an.inner_flags : flags) & 0xFFFF;
    };
    return rt::ret_int(modifiers(class_arg(rt, a.h(0), "Class")));
  });
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
    return process_annotation_set(rt, class_arg(rt, a.h(0), "Class")->annotations().annotations);
  });
  c.method("getAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    // libcore's Class.getAnnotations: the declared annotations, then those of
    // the superclasses whose type is @Inherited and not yet present.
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    AnnotationSet all = cls->annotations().annotations;
    for (rt::Class* k = cls->super; k; k = k->super) {
      for (const EncodedAnnotation& ea : k->annotations().annotations.items) {
        if (ea.visibility != kVisibilityRuntime || all.find(ea.type_descriptor, kVisibilityRuntime)) continue;
        if (rt::Class* ac = resolve_or_null(rt, ea.type_descriptor); ac && inherited_annotation_type(rt, ac)) all.items.push_back(ea);
      }
    }
    return process_annotation_set(rt, all);
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
    return annotation_from_set(rt, cls->annotations().annotations, class_arg(rt, a.h(1), "annotationClass"));
  });
  c.method("isDeclaredAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* cls = class_arg(rt, a.h(0), "Class");
    return rt::ret_bool(annotation_item(rt, cls->annotations().annotations, class_arg(rt, a.h(1), "annotationClass")));
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

// The annotations of the member of a bridge Method, Constructor or Field.
const AnnotationSet* member_annotations(Runtime& rt, Handle h) {
  if (auto* mp = rt.peer<rt::MethodPeer>(h)) return &mp->method->annotations().annotations;
  if (auto* fp = rt.peer<rt::FieldPeer>(h)) return &fp->field->annotations().annotations;
  return nullptr;
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
        const AnnotationSet* set = member_annotations(rt, a.h(0));
        return set ? annotation_from_set(rt, *set, class_arg(rt, a.h(1), "annotationClass")) : kNull;
      })
      .method("isAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        const AnnotationSet* set = member_annotations(rt, a.h(0));
        return rt::ret_bool(set && annotation_item(rt, *set, class_arg(rt, a.h(1), "annotationClass")));
      })
      .method("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        const AnnotationSet* set = member_annotations(rt, a.h(0));
        return set ? process_annotation_set(rt, *set) : rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
      })
      .method("getAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        const AnnotationSet* set = member_annotations(rt, a.h(0));
        return set ? process_annotation_set(rt, *set) : rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
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
          inner = process_annotation_set(rt, param_annos[i]);
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
namespace {

// mirror::Class::GetAccessFlags as ClassLinker sets them: the class
// definition's flags; an array, its component's Java flags, abstract and
// final, never interface (CreateArrayClass); a primitive, public, final and
// abstract. ART's internal flags (verification, finalizable...) are not kept.
int32_t art_access_flags(rt::Class* k) {
  if (k->component) {
    return (art_access_flags(k->component) & 0xFFFF & ~int32_t(dex::kAccInterface)) | dex::kAccAbstract | dex::kAccFinal;
  }
  if (k->primitive) return dex::kAccPublic | dex::kAccFinal | dex::kAccAbstract;
  return int32_t(k->access & 0xFFFF);
}

// Class.ifTable's interfaces in ART's order (SetupInterfaceLookupTable,
// FindNewIfTableInterfaces): the superclass's, then each direct interface
// after its own superinterfaces, each once.
void iftable_of(rt::Class* k, std::vector<rt::Class*>& out) {
  auto add = [&out](rt::Class* i) {
    if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
  };
  if (k->super) iftable_of(k->super, out);
  for (rt::Class* i : k->interfaces) {
    std::vector<rt::Class*> inherited;
    iftable_of(i, inherited);
    for (rt::Class* s : inherited) add(s);
    add(i);
  }
}

}  // namespace

void register_art_reflection_natives(rt::NativeRegistry& r) {
  const char* cls = "Ljava/lang/Class;";
  // The bytecode of these Class methods reads ART fields that the core's
  // Class does not have (accessFlags, componentType, ifTable, superClass):
  // they answer from the class itself, as that bytecode would.
  r.intrinsic(cls, "getAccessFlags", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(art_access_flags(class_arg(rt, a.h(0), "Class")));
  }, "sans les drapeaux internes d'ART (vérification, finalisable…)");
  r.intrinsic(cls, "componentType", "()Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    return c->component ? rt.class_mirror(c->component) : kNull;
  });
  // "[" + componentType.descriptorString(), "L" + name with '/' + ";", or the
  // primitive's letter: the class's descriptor.
  r.intrinsic(cls, "descriptorString", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(class_arg(rt, a.h(0), "Class")->descriptor);
  });
  // The public member classes of the class and of its superclasses.
  r.intrinsic(cls, "getClasses", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<Handle> found;
    for (rt::Class* k = class_arg(rt, a.h(0), "Class"); k; k = k->super) {
      rt::GlobalRef declared(rt, Handle(rt.call_virtual(rt.class_mirror(k), "getDeclaredClasses", "()[Ljava/lang/Class;")));
      uint32_t n = rt.array(declared.get(), "Class[]").length;
      for (uint32_t i = 0; i < n; ++i) {
        Handle member;
        std::memcpy(&member, &rt.array(declared.get(), "Class[]").data[size_t(i) * 4], 4);
        if (int32_t(rt.call_virtual(member, "getModifiers", "()I")) & dex::kAccPublic) found.push_back(member);
      }
    }
    return object_array(rt, "[Ljava/lang/Class;", found);
  });
  // The most specific interface first (the table searched backwards), the
  // first public method that getPublicMethodRecursive finds.
  r.intrinsic(cls, "findInterfaceMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
              [](Runtime& rt, NativeArgs& a) -> uint64_t {
                std::vector<rt::Class*> iftable;
                iftable_of(class_arg(rt, a.h(0), "Class"), iftable);
                rt::Method* recursive = rt.find_class("Ljava/lang/Class;")->declared_method(
                    "getPublicMethodRecursive", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
                if (!recursive) rt.throw_unsupported("Ljava/lang/Class;->getPublicMethodRecursive", "absente de la libcore");
                for (auto it = iftable.rbegin(); it != iftable.rend(); ++it) {
                  uint32_t args[3] = {rt.class_mirror(*it), a.h(1), a.h(2)};
                  Handle m = Handle(rt.invoke(recursive, args, 3));
                  if (m != kNull && (int32_t(rt.call_virtual(m, "getAccessFlags", "()I")) & dex::kAccPublic)) return m;
                }
                return kNull;
              });
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
    if (is_proxy_class(c)) return kNull;
    return annotation_from_set(rt, c->annotations().annotations, class_arg(rt, a.h(1), "annotationClass"));
  });
  r.bind(cls, "isDeclaredAnnotationPresent", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    if (is_proxy_class(c)) return rt::ret_bool(false);
    return rt::ret_bool(annotation_item(rt, c->annotations().annotations, class_arg(rt, a.h(1), "annotationClass")));
  });
  r.bind(cls, "getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Class* c = class_arg(rt, a.h(0), "Class");
    if (is_proxy_class(c)) return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
    return process_annotation_set(rt, c->annotations().annotations);
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

// GetCallingClass (NthCallerVisitor): frame 0 is the native method itself,
// frame 1 its caller. Null when the stack is not that deep (an attached
// native thread).
rt::Class* calling_class(Runtime& rt, size_t num_frames) {
  const auto& frames = rt.current_thread()->frames;
  if (frames.size() <= num_frames) return nullptr;
  return frames[frames.size() - 1 - num_frames].method->owner;
}

// VerifyAccess (reflection.cc): may `calling` reach a member of `declaring`
// with `access_flags` through `obj` (the receiver, or for a static field the
// declaring class's mirror)?
bool verify_access(Runtime& rt, Handle obj, rt::Class* declaring, uint32_t access_flags, rt::Class* calling) {
  if (calling == declaring) return true;
  if (access_flags & dex::kAccPrivate) return false;
  if (access_flags & dex::kAccProtected) {
    if (obj != kNull && !rt.instance_of(obj, calling) && !in_same_package(rt, declaring, calling)) return false;
    if (rt.is_assignable(calling, declaring)) return true;
  }
  return in_same_package(rt, declaring, calling);
}

// VerifyAccess with the calling class found `num_frames` up the stack; the
// class is returned through `calling` for the message.
bool verify_caller_access(Runtime& rt, Handle obj, rt::Class* declaring, uint32_t access_flags, rt::Class** calling,
                          size_t num_frames) {
  if (access_flags & dex::kAccPublic) return true;
  *calling = calling_class(rt, num_frames);
  return *calling && verify_access(rt, obj, declaring, access_flags, *calling);
}

std::string pretty_class_or_null(const rt::Class* c) { return c ? pretty_class(c) : "null"; }

// VerifyFieldAccess<kIsSet> (java_lang_reflect_Field.cc), for a field that
// setAccessible(true) did not open: a final field is read-only, and the
// caller must see the field. `obj` is what CheckReceiver produced.
void verify_field_access(Runtime& rt, Handle field_obj, const rt::Field* f, Handle receiver, bool set) {
  if (is_accessible(rt, field_obj)) return;
  if (set && (f->access & dex::kAccFinal)) {
    rt.throw_new("Ljava/lang/IllegalAccessException;", "Cannot set " + pretty_java_access_flags(f->access) +
                                                            " field " + pretty_field(f) + " of class " +
                                                            pretty_class(f->owner));
  }
  Handle obj = f->is_static() ? rt.class_mirror(f->owner) : receiver;
  rt::Class* calling = nullptr;
  if (verify_caller_access(rt, obj, f->owner, f->access, &calling, 1)) return;
  rt.throw_new("Ljava/lang/IllegalAccessException;", "Class " + pretty_class_or_null(calling) + " cannot access " +
                                                          pretty_java_access_flags(f->access) + " field " +
                                                          pretty_field(f) + " of class " + pretty_class(f->owner));
}

// The access check of InvokeMethod for `m` (the implementation found for the
// receiver) declared by `declaring`, `num_frames` above the native.
void verify_method_access(Runtime& rt, Handle executable, const rt::Method* m, rt::Class* declaring, Handle receiver,
                          size_t num_frames) {
  if (is_accessible(rt, executable)) return;
  rt::Class* calling = nullptr;
  if (verify_caller_access(rt, receiver, declaring, m->access, &calling, num_frames)) return;
  rt.throw_new("Ljava/lang/IllegalAccessException;", "Class " + pretty_class_or_null(calling) + " cannot access " +
                                                          pretty_java_access_flags(m->access) + " method " +
                                                          pretty_method(m, true) + " of class " +
                                                          pretty_class(m->owner));
}

// InvokeMethod (reflection.cc), behind Method.invoke: the caller whose access
// is checked is `num_frames` above the native.
uint64_t invoke_method(Runtime& rt, Handle method_obj, Handle receiver, Handle args_array, size_t num_frames = 1) {
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
  verify_method_access(rt, method_obj, m, declaring, receiver, num_frames);
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

// --- java.lang.reflect.Proxy ---------------------------------------------------------------------
//
// ClassLinker::CreateProxyClass and InvokeProxyInvocationHandler
// (art/runtime/class_linker.cc, entrypoints/entrypoint_utils.cc): a final
// subclass of Proxy with the static fields `interfaces` and `throws`, a public
// copy of Proxy(InvocationHandler), and, for each proxied interface method, a
// final method that calls Proxy.invoke(proxy, method, args).

// Class::GetProxyThrowsIndex: the rank of a proxy method among its class's
// virtual methods, which is its row in `throws`.
size_t proxy_throws_index(const rt::Method* m) {
  size_t index = 0;
  for (const auto& k : m->owner->methods) {
    if (k.get() == m) return index;
    if (k->name != "<init>") ++index;
  }
  return index;
}

// The exceptions a proxy method declares (Class::GetProxyThrows()[index]).
Handle proxy_throws(Runtime& rt, const rt::Method* m) {
  rt::Field* f = m->owner->declared_field("throws", "[[Ljava/lang/Class;");
  Handle throws = f ? Handle(m->owner->statics.at(f->slot)) : kNull;
  if (throws == kNull) return kNull;
  Handle row;
  std::memcpy(&row, &rt.array(throws, "throws").data[proxy_throws_index(m) * 4], 4);
  return row;
}

// mirror::Throwable::IsCheckedException.
bool is_checked_exception(Runtime& rt, Handle e) {
  return !rt.instance_of(e, rt.find_class("Ljava/lang/Error;")) &&
         !rt.instance_of(e, rt.find_class("Ljava/lang/RuntimeException;"));
}

uint64_t invoke_proxy_handler(Runtime& rt, rt::Method* proxy_method, NativeArgs& a) {
  rt::Method* iface = proxy_method->interface_method;
  Handle receiver = a.h(0);
  Handle method_obj = rt.reflect_method(iface);
  std::vector<std::string> params = params_of(iface);
  Handle args = kNull;
  // No empty array, except for applications targeting Android 5.0 or older (Dalvik's behaviour).
  if (!params.empty() || app_target_sdk(rt) <= 21) {
    const char* desc = "[Ljava/lang/Object;";
    args = rt.new_array(desc, int32_t(params.size()));
    rt::GlobalRef keep(rt, args);
    uint32_t w = 1;
    for (size_t i = 0; i < params.size(); ++i) {
      const std::string& p = params[i];
      Handle value;
      if (p[0] == 'L' || p[0] == '[') {
        value = a.h(w++);
      } else if (p == "J" || p == "D") {
        value = box(rt, p[0], uint64_t(a.i64(w)));
        w += 2;
      } else {
        value = box(rt, p[0], a.raw(w++));
      }
      std::memcpy(&rt.array(args, desc).data[i * 4], &value, 4);
    }
  }
  Handle result;
  try {
    result = Handle(rt.call_static("Ljava/lang/reflect/Proxy;", "invoke",
                                   "(Ljava/lang/reflect/Proxy;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;",
                                   {receiver, method_obj, args}));
  } catch (const rt::JavaThrow& t) {
    // A checked exception the method does not declare is wrapped.
    if (is_checked_exception(rt, t.exception)) {
      bool declared = false;
      for (Handle c : array_items(rt, proxy_throws(rt, proxy_method))) {
        declared = declared || rt.is_assignable(rt.class_of(t.exception), rt.mirror_class(c));
      }
      if (!declared) {
        Handle cause = t.exception;
        throw rt::JavaThrow{construct(rt, "Ljava/lang/reflect/UndeclaredThrowableException;", "(Ljava/lang/Throwable;)V", {cause})};
      }
    }
    throw;
  }
  std::string ret = return_of(iface);
  if (ret == "V" || ((ret[0] == 'L' || ret[0] == '[') && result == kNull)) return 0;
  return unbox_primitive(rt, result, ret, nullptr);
}

Handle generate_proxy(Runtime& rt, NativeArgs& a) {
  std::string name = rt.string_utf8(a.h(0));
  std::string descriptor = "L" + name + ";";
  std::replace(descriptor.begin(), descriptor.end(), '.', '/');
  std::vector<rt::Class*> interfaces;
  for (Handle c : array_items(rt, a.h(1))) interfaces.push_back(class_arg(rt, c, "interface"));
  rt::Class* proxy = rt.find_class("Ljava/lang/reflect/Proxy;");
  if (rt.loaded_class(descriptor)) rt.throw_new("Ljava/lang/InternalError;", "classe proxy déjà définie : " + name);
  rt::Class* cls = rt.define_synthetic_class(descriptor, proxy, interfaces, kAccClassIsProxy | dex::kAccPublic | dex::kAccFinal);

  // Static fields 0 and 1: the declared interfaces and the exceptions of each method.
  for (const char* f_name : {"interfaces", "throws"}) {
    auto f = std::make_unique<rt::Field>();
    f->owner = cls;
    f->name = f_name;
    f->type = std::string(f_name) == "interfaces" ? "[Ljava/lang/Class;" : "[[Ljava/lang/Class;";
    f->access = dex::kAccStatic | dex::kAccPublic | dex::kAccFinal;
    f->slot = uint32_t(cls->statics.size());
    cls->statics.push_back(0);
    cls->fields.push_back(std::move(f));
  }

  // The constructor: Proxy(InvocationHandler), made public.
  rt::Method* proxy_init = proxy->declared_method("<init>", "(Ljava/lang/reflect/InvocationHandler;)V");
  if (!proxy_init) rt.throw_new("Ljava/lang/InternalError;", "Proxy(InvocationHandler) absent");
  auto ctor = std::make_unique<rt::Method>();
  ctor->owner = cls;
  ctor->name = "<init>";
  ctor->signature = proxy_init->signature;
  ctor->access = (proxy_init->access & ~uint32_t(dex::kAccProtected)) | dex::kAccPublic;
  ctor->arg_words = proxy_init->arg_words;
  ctor->interface_method = proxy_init;
  ctor->native = [proxy_init](Runtime& rt2, NativeArgs& args) -> uint64_t {
    uint32_t words[2] = {args.h(0), args.h(1)};
    return rt2.invoke(proxy_init, words, 2);
  };
  cls->methods.push_back(std::move(ctor));

  // The proxied methods: the non-private virtual ones, with their row of `throws`.
  std::vector<Handle> methods = array_items(rt, a.h(3));
  std::vector<Handle> all_throws = array_items(rt, a.h(4));
  std::vector<Handle> proxied_throws;
  for (size_t i = 0; i < methods.size(); ++i) {
    rt::Method* prototype = executable_arg(rt, methods[i]);
    if (prototype->access & (dex::kAccPrivate | dex::kAccStatic)) continue;
    auto m = std::make_unique<rt::Method>();
    m->owner = cls;
    m->name = prototype->name;
    m->signature = prototype->signature;
    m->access = (prototype->access & ~uint32_t(dex::kAccAbstract)) | dex::kAccFinal;
    m->arg_words = prototype->arg_words;
    m->method_idx = prototype->method_idx;
    m->interface_method = prototype;
    rt::Method* self = m.get();
    m->native = [self](Runtime& rt2, NativeArgs& args) -> uint64_t { return invoke_proxy_handler(rt2, self, args); };
    cls->methods.push_back(std::move(m));
    proxied_throws.push_back(i < all_throws.size() ? all_throws[i] : kNull);
  }
  Handle throws = proxied_throws.size() == all_throws.size() ? a.h(4)
                                                             : object_array(rt, "[[Ljava/lang/Class;", proxied_throws);
  cls->statics.at(0) = a.h(1);
  cls->statics.at(1) = throws;
  rt.ensure_initialized(cls);
  return rt.class_mirror(cls);
}

void register_libcore_members(rt::NativeRegistry& r) {
  r.bind("Ljava/lang/reflect/Proxy;", "generateProxy",
         "(Ljava/lang/String;[Ljava/lang/Class;Ljava/lang/ClassLoader;[Ljava/lang/reflect/Method;[[Ljava/lang/Class;)Ljava/lang/Class;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return generate_proxy(rt, a); },
         S::kPartial, "un seul espace de classes : le chargeur de classes est ignoré");
  const char* ex = "Ljava/lang/reflect/Executable;";
  r.bind(ex, "getAnnotationNative", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (m->interface_method) return kNull;
    return annotation_from_set(rt, m->annotations().annotations, class_arg(rt, a.h(1), "annotationType"));
  });
  r.bind(ex, "getDeclaredAnnotationsNative", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
    return process_annotation_set(rt, m->annotations().annotations);
  });
  // java_lang_reflect_Parameter.cc: one annotation of one parameter, with the
  // same allowance as getParameterAnnotationsNative for the implicit
  // parameters of constructors that dexers leave unannotated (b/68033708).
  r.bind("Ljava/lang/reflect/Parameter;", "getAnnotationNative",
         "(Ljava/lang/reflect/Executable;ILjava/lang/Class;)Ljava/lang/annotation/Annotation;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe("javaMethod == null");
           rt::Method* m = executable_arg(rt, a.h(0));
           if (is_proxy_class(m->owner)) return kNull;
           int64_t index = a.i32(1);
           const int64_t count = int64_t(params_of(m).size());
           if (index < 0 || index >= count) {
             throw_iae(rt, "Illegal parameterIndex " + std::to_string(index) + " for " + pretty_method(m, true) +
                               ", parameter_count is " + std::to_string(count));
           }
           const auto& lists = m->annotations().parameter_annotations;
           const int64_t annotated = int64_t(lists.size());
           if (annotated == 0) return kNull;
           if (m->name == "<init>" && !m->owner->annotations().enclosing_method && !(m->owner->access & dex::kAccEnum)) {
             if (annotated <= count) {
               if (index < count - annotated) return kNull;
               index -= count - annotated;
             } else {
               index += annotated - count;
             }
           }
           if (index >= annotated) return kNull;
           return annotation_from_set(rt, lists[size_t(index)], class_arg(rt, a.h(2), "annotationType"));
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
      Handle set = src >= 0 ? process_annotation_set(rt, lists[size_t(src)])
                            : rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
      std::memcpy(&rt.array(outer, desc).data[i * 4], &set, 4);
    }
    return outer;
  });
  r.bind(ex, "getParameters0", "()[Ljava/lang/reflect/Parameter;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) return kNull;
    // The MethodParameters system annotation (javac -parameters):
    // GetParametersMetadataForMethod.
    const EncodedAnnotation* ea = m->annotations().annotations.find("Ldalvik/annotation/MethodParameters;", 2);
    if (!ea) return kNull;
    const V* names = ea->element("names");
    const V* flags = ea->element("accessFlags");
    if (!names || !flags || names->type != V::kArray || flags->type != V::kArray) return kNull;
    AnnotationValue names_value = process_annotation_value(rt, *ea->dex, *names, rt.find_class("[Ljava/lang/String;"), true);
    if (!names_value.ok) return kNull;
    rt::GlobalRef keep_names(rt, Handle(names_value.bits));
    size_t count = names->array.size();
    if (count != flags->array.size()) {
      throw_iae(rt, "Inconsistent parameter metadata for " + pretty_method(m, true) + ". names length: " +
                        std::to_string(count) + ", access flags length: " + std::to_string(flags->array.size()));
    }
    rt::Class* parameter = rt.find_class("Ljava/lang/reflect/Parameter;");
    rt::Method* init = parameter->declared_method("<init>", "(Ljava/lang/String;ILjava/lang/reflect/Executable;I)V");
    if (!init) rt.throw_new("Ljava/lang/InternalError;", "Parameter.<init> absent");
    const char* desc = "[Ljava/lang/reflect/Parameter;";
    Handle out = rt.new_array(desc, int32_t(count));
    rt::GlobalRef keep(rt, out);
    for (size_t i = 0; i < count; ++i) {
      Handle name;
      std::memcpy(&name, &rt.array(Handle(names_value.bits), "names").data[i * 4], 4);
      Handle p = rt.alloc(parameter);
      uint32_t args[5] = {p, name, uint32_t(flags->array[i].bits), a.h(0), uint32_t(i)};
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
    return rt::ret_bool(annotation_item(rt, m->annotations().annotations, class_arg(rt, a.h(1), "annotationType")));
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
    // GetAnnotationDefaultValue: the AnnotationDefault system annotation of
    // the annotation type, an annotation holding the default of each element.
    const EncodedAnnotation* ea = m->owner->annotations().annotations.find("Ldalvik/annotation/AnnotationDefault;", 2);
    const V* defaults = ea ? ea->element("value") : nullptr;
    if (!defaults || defaults->type != V::kAnnotation) return kNull;
    for (size_t i = 0; i < defaults->names.size() && i < defaults->array.size(); ++i) {
      if (defaults->names[i] >= ea->dex->num_strings() || ea->dex->string(defaults->names[i]) != m->name) continue;
      rt::Class* ret = resolve_or_null(rt, return_of(m));
      if (!ret) return kNull;
      AnnotationValue v = process_annotation_value(rt, *ea->dex, defaults->array[i], ret, true);
      return v.ok ? v.bits : kNull;
    }
    return kNull;
  });
  r.bind(method, "getExceptionTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Method* m = executable_arg(rt, a.h(0));
    if (is_proxy_class(m->owner)) {  // a copy of the method's row of `throws`
      std::vector<Handle> objs = array_items(rt, proxy_throws(rt, m));
      return object_array(rt, "[Ljava/lang/Class;", objs);
    }
    return exception_types(rt, m);
  });
  r.bind(method, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return invoke_method(rt, a.h(0), a.h(1), a.h(2));
  });

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
    // Only the class is checked, against the caller of Constructor.newInstance
    // (2 frames up); a caller-less (JNI) call skips the check.
    if (!is_accessible(rt, a.h(0)) && !(c->access & dex::kAccPublic)) {
      rt::Class* caller = calling_class(rt, 2);
      if (caller && !in_same_package(rt, caller, c)) {  // Class::CanAccess
        if (pretty_descriptor(c->descriptor) == "dalvik.system.DexPathList$Element") {
          log::warn("reflect", "The dalvik.system.DexPathList$Element constructor is not accessible by default. "
                               "This is a temporary workaround for backwards compatibility with class-loader hacks. "
                               "Please update your application.");
        } else {
          rt.throw_new("Ljava/lang/IllegalAccessException;",
                       pretty_class(c) + " is not accessible from " + pretty_class(caller));
        }
      }
    }
    rt.ensure_initialized(c);
    std::vector<Handle> args = checked_args(rt, m, a.h(1));
    if (c->descriptor == "Ljava/lang/String;") {
      // InvokeMethod(..., 2) on the StringFactory method that replaces the constructor.
      rt::Method* factory = string_factory_for(rt, m);
      verify_method_access(rt, a.h(0), factory, c, kNull, 2);
      std::vector<uint32_t> words = build_arg_array(rt, m, kNull, args);
      return invoke_wrapped(rt, factory, words);
    }
    Handle receiver = rt.alloc(c);
    std::vector<uint32_t> words = build_arg_array(rt, m, receiver, args);
    invoke_wrapped(rt, m, words);
    return receiver;
  });
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
    return annotation_from_set(rt, f->annotations().annotations, class_arg(rt, a.h(1), "annotationType"));
  });
  r.bind(field, "isAnnotationPresentNative", "(Ljava/lang/Class;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    if (is_proxy_class(f->owner)) return rt::ret_bool(false);
    return rt::ret_bool(annotation_item(rt, f->annotations().annotations, class_arg(rt, a.h(1), "annotationType")));
  });
  r.bind(field, "getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    if (is_proxy_class(f->owner)) return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
    return process_annotation_set(rt, f->annotations().annotations);
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
    uint32_t* s = checked_field_storage(rt, f, a.h(1));
    verify_field_access(rt, a.h(0), f, a.h(1), false);
    uint64_t v = read_storage(f, s);
    return f->is_reference() ? v : box_result(rt, f->type[0], v);
  });
  for (char t : std::string("ZBCSIJFD")) {
    static const std::map<char, std::string> names = {{'Z', "Boolean"}, {'B', "Byte"},  {'C', "Char"},  {'S', "Short"},
                                                      {'I', "Int"},     {'J', "Long"},  {'F', "Float"}, {'D', "Double"}};
    std::string type(1, t);
    r.bind(field, "get" + names.at(t), "(Ljava/lang/Object;)" + type, [t](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Field* f = field_arg(rt, a.h(0));
      uint32_t* s = checked_field_storage(rt, f, a.h(1));
      verify_field_access(rt, a.h(0), f, a.h(1), false);
      if (f->is_reference()) throw_iae(rt, "Not a primitive field: " + pretty_field(f));
      uint64_t v = read_storage(f, s);
      return convert_primitive_value(rt, false, f->type[0], t, v);
    });
    r.bind(field, "set" + names.at(t), "(Ljava/lang/Object;" + type + ")V", [t](Runtime& rt, NativeArgs& a) -> uint64_t {
      rt::Field* f = field_arg(rt, a.h(0));
      uint32_t* s = checked_field_storage(rt, f, a.h(1));
      check_overwritable(rt, f);
      if (f->is_reference()) throw_iae(rt, "Not a primitive field: " + pretty_field(f));
      uint64_t in = (t == 'J' || t == 'D') ? a.i64(2) : a.raw(2);
      uint64_t v = convert_primitive_value(rt, false, t, f->type[0], in);
      verify_field_access(rt, a.h(0), f, a.h(1), true);
      write_storage(f, s, v);
      return 0;
    });
  }
  r.bind(field, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Field* f = field_arg(rt, a.h(0));
    uint32_t* s = checked_field_storage(rt, f, a.h(1));
    check_overwritable(rt, f);
    uint64_t v = unbox_primitive(rt, a.h(2), f->type, f);
    verify_field_access(rt, a.h(0), f, a.h(1), true);
    if (!f->is_static()) s = &rt.object(a.h(1))->fields.at(f->slot);  // unboxing may have run Java code
    write_storage(f, s, v);
    return 0;
  });
}

}  // namespace

// Class::IsInSamePackage: the same defining loader and the same package part
// of the descriptor (arrays by their element classes).
bool in_same_package(Runtime& rt, rt::Class* a, rt::Class* b) {
  if (a == b) return true;
  if (class_loader_of(rt, a, true) != class_loader_of(rt, b, true)) return false;
  while (a->component) a = a->component;
  while (b->component) b = b->component;
  if (a == b) return true;
  const std::string& d1 = a->descriptor;
  const std::string& d2 = b->descriptor;
  size_t i = 0;
  while (i < std::min(d1.size(), d2.size()) && d1[i] == d2[i]) ++i;
  return d1.find('/', i) == std::string::npos && d2.find('/', i) == std::string::npos;
}

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
