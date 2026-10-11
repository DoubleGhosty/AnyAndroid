// java.lang.reflect and the reflective parts of java.lang.Class.

#include <cstring>

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

rt::Method* method_of(Runtime& rt, Handle h) { return peer_or_throw<MethodPeer>(rt, h, "Method").method; }
rt::Field* field_of(Runtime& rt, Handle h) { return peer_or_throw<FieldPeer>(rt, h, "Field").field; }

Handle make_method_object(Runtime& rt, rt::Method* m) {
  bool ctor = m->name == "<init>";
  Handle h = rt.alloc(rt.find_class(ctor ? "Ljava/lang/reflect/Constructor;" : "Ljava/lang/reflect/Method;"));
  auto p = std::make_unique<MethodPeer>();
  p->method = m;
  rt.object(h)->peer = std::move(p);
  return h;
}

Handle make_field_object(Runtime& rt, rt::Field* f) {
  Handle h = rt.alloc(rt.find_class("Ljava/lang/reflect/Field;"));
  auto p = std::make_unique<FieldPeer>();
  p->field = f;
  rt.object(h)->peer = std::move(p);
  return h;
}

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
  for (rt::Class* k = c; k; k = inherited ? k->super : nullptr) {
    for (const auto& m : k->methods) {
      bool is_ctor = m->name == "<init>";
      if (m->name == "<clinit>" || is_ctor != constructors) continue;
      if (public_only && !(m->access & dex::kAccPublic)) continue;
      if (!seen.insert(m->name + m->signature).second) continue;  // overridden
      out.push_back(m.get());
    }
    if (constructors) break;
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
  c.method("getMethods", "()[Ljava/lang/reflect/Method;", methods(true, true),
           S::kPartial, "méthodes publiques des superclasses incluses, celles des interfaces non");
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
  c.unsupported("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", kNoAnnotations);
  c.unsupported("getAnnotations", "()[Ljava/lang/annotation/Annotation;", kNoAnnotations);
  c.unsupported("isAnnotationPresent", "(Ljava/lang/Class;)Z", kNoAnnotations);
  c.unsupported("getEnclosingClass", "()Ljava/lang/Class;", "attributs InnerClass/EnclosingClass non lus");
  c.unsupported("getDeclaringClass", "()Ljava/lang/Class;", "attributs InnerClass/EnclosingClass non lus");

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
      .method("isAccessible", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 1; })
      .unsupported("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", kNoAnnotations)
      .unsupported("isAnnotationPresent", "(Ljava/lang/Class;)Z", kNoAnnotations);

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
    b.method("getParameterTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::vector<Handle> objs;
      for (const auto& p : params_of(method_of(rt, a.h(0)))) objs.push_back(class_of_descriptor(rt, p));
      return object_array(rt, "[Ljava/lang/Class;", objs);
    });
    b.method("getParameterCount", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_int(int32_t(params_of(method_of(rt, a.h(0))).size()));
    });
    b.method("getExceptionTypes", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs&) -> uint64_t {
      return object_array(rt, "[Ljava/lang/Class;", {});
    }, S::kPartial, "clause throws non lue (annotations)");
    b.method("isVarArgs", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_bool(method_of(rt, a.h(0))->access & dex::kAccVarargs);
    });
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
    std::string n = class_arg(rt, a.h(0), "Class")->java_name();
    size_t cut = n.find_last_of(".$");
    return rt.new_string_utf8(cut == std::string::npos ? n : n.substr(cut + 1));
  }, S::kPartial, "dérivé du nom binaire (attributs InnerClass non lus)");
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

  // Annotation-backed metadata: DEX annotations are not read yet; these
  // answers describe a class without annotations and are logged as stubs.
  const char* none = "annotations DEX non lues : réponse « aucune »";
  auto null_result = [](Runtime&, NativeArgs&) -> uint64_t { return kNull; };
  auto false_result = [](Runtime&, NativeArgs&) -> uint64_t { return 0; };
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"getSignatureAnnotation", "()[Ljava/lang/String;"},
           {"getEnclosingMethodNative", "()Ljava/lang/reflect/Method;"},
           {"getEnclosingConstructorNative", "()Ljava/lang/reflect/Constructor;"},
           {"getNestHostFromAnnotation", "()Ljava/lang/Class;"},
           {"getNestMembersFromAnnotation", "()[Ljava/lang/Class;"},
           {"getPermittedSubclassesFromAnnotation", "()[Ljava/lang/Class;"},
           {"getDeclaredAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"},
           {"getRecordAnnotationElement", "(Ljava/lang/String;Ljava/lang/Class;)[Ljava/lang/Object;"}}) {
    r.bind(cls, name, sig, null_result, S::kStub, none);
  }
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"isAnonymousClass", "()Z"}, {"isRecord0", "()Z"}, {"isValue", "()Z"},
           {"isDeclaredAnnotationPresent", "(Ljava/lang/Class;)Z"}}) {
    r.bind(cls, name, sig, false_result, S::kStub, none);
  }
  r.bind(cls, "getInnerClassFlags", "(I)I", [](Runtime&, NativeArgs& a) -> uint64_t { return a.raw(1); }, S::kStub, none);
  r.bind(cls, "getDeclaredClasses", "()[Ljava/lang/Class;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_array("[Ljava/lang/Class;", 0);
  }, S::kStub, none);
  r.bind(cls, "getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_array("[Ljava/lang/annotation/Annotation;", 0);
  }, S::kStub, none);

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

}  // namespace

void register_java_reflect(rt::NativeRegistry& r, Host&) {
  register_members(r);
  register_class_reflection(r);
  register_art_reflection_natives(r);
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
