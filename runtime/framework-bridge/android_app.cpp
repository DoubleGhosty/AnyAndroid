// android.content / android.app / android.os / android.util subset.

#include <algorithm>
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

constexpr const char* kNoResources =
    "ressources compilées (resources.arsc) non prises en charge dans cette version";

struct HandlerPeer final : rt::NativePeer {
  // Generation per Runnable: removeCallbacks() invalidates queued posts.
  std::shared_ptr<std::map<Handle, uint64_t>> generations = std::make_shared<std::map<Handle, uint64_t>>();
};

Extras& extras_of(Runtime& rt, Handle h) {
  rt::Object& o = rt.nonnull(h, "Intent/Bundle");
  if (auto* i = dynamic_cast<IntentPeer*>(o.peer.get())) return i->extras;
  if (auto* b = dynamic_cast<BundlePeer*>(o.peer.get())) return b->values;
  rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] état natif absent pour " + o.klass->java_name());
}

const Extra* find_extra(Runtime& rt, Handle h, NativeArgs& a, uint32_t key_index) {
  Extras& e = extras_of(rt, h);
  auto it = e.find(a.str_or(key_index, u"null"));
  return it == e.end() ? nullptr : &it->second;
}

void put_extra(Runtime& rt, NativeArgs& a, Extra v) { extras_of(rt, a.h(0))[a.str_or(1, u"null")] = std::move(v); }

std::string class_from_mirror(Runtime& rt, Handle mirror) {
  rt::Class* c = rt.mirror_class(mirror);
  if (!c) rt.throw_npe("Intent : classe null");
  return c->descriptor;
}

void register_extras_accessors(rt::ClassBuilder& b, const std::string& self_type, bool returns_self) {
  // putX returns the receiver for Intent (builder style) and void for Bundle.
  std::string ret = returns_self ? self_type : "V";
  auto put = [returns_self](Extra::Kind kind) {
    return [returns_self, kind](Runtime& rt, NativeArgs& a) -> uint64_t {
      Extra e;
      e.kind = kind;
      switch (kind) {
        case Extra::Kind::kString: e.s = a.h(2) ? rt.string_value(a.h(2)) : u""; e.i = a.h(2) == kNull; break;
        case Extra::Kind::kInt: e.i = a.i32(2); break;
        case Extra::Kind::kBool: e.i = a.z(2); break;
        case Extra::Kind::kLong: e.i = a.i64(2); break;
        case Extra::Kind::kFloat: e.d = a.f32(2); break;
        case Extra::Kind::kDouble: e.d = a.f64(2); break;
      }
      put_extra(rt, a, std::move(e));
      return returns_self ? a.h(0) : 0;
    };
  };
  const char* prefix = returns_self ? "putExtra" : nullptr;
  auto name = [&](const char* bundle_name) { return std::string(prefix ? prefix : bundle_name); };
  b.method(name("putString"), "(Ljava/lang/String;Ljava/lang/String;)" + ret, put(Extra::Kind::kString));
  if (returns_self) b.method("putExtra", "(Ljava/lang/String;Ljava/lang/CharSequence;)" + ret, put(Extra::Kind::kString));
  b.method(name("putInt"), "(Ljava/lang/String;I)" + ret, put(Extra::Kind::kInt));
  b.method(name("putBoolean"), "(Ljava/lang/String;Z)" + ret, put(Extra::Kind::kBool));
  b.method(name("putLong"), "(Ljava/lang/String;J)" + ret, put(Extra::Kind::kLong));
  b.method(name("putFloat"), "(Ljava/lang/String;F)" + ret, put(Extra::Kind::kFloat));
  b.method(name("putDouble"), "(Ljava/lang/String;D)" + ret, put(Extra::Kind::kDouble));
}

void register_bundle(rt::NativeRegistry& r) {
  auto base = r.define("Landroid/os/BaseBundle;", "Ljava/lang/Object;");
  register_extras_accessors(base, "Landroid/os/BaseBundle;", false);
  base.method("getString", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return e && e->kind == Extra::Kind::kString && !e->i ? rt.new_string(e->s) : kNull;
  });
  base.method("getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return e && e->kind == Extra::Kind::kString && !e->i ? rt.new_string(e->s) : a.h(2);
  });
  base.method("getInt", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_int(e && e->kind == Extra::Kind::kInt ? int32_t(e->i) : 0);
  });
  base.method("getInt", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_int(e && e->kind == Extra::Kind::kInt ? int32_t(e->i) : a.i32(2));
  });
  base.method("getBoolean", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_bool(e && e->kind == Extra::Kind::kBool && e->i);
  });
  base.method("getBoolean", "(Ljava/lang/String;Z)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_bool(e && e->kind == Extra::Kind::kBool ? e->i != 0 : a.z(2));
  });
  base.method("getLong", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_long(e && e->kind == Extra::Kind::kLong ? e->i : 0);
  });
  base.method("containsKey", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(find_extra(rt, a.h(0), a, 1) != nullptr);
  });
  base.method("remove", "(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    extras_of(rt, a.h(0)).erase(a.str_or(1, u"null"));
    return 0;
  });
  base.method("size", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(int32_t(extras_of(rt, a.h(0)).size())); });
  base.method("isEmpty", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_bool(extras_of(rt, a.h(0)).empty()); });
  base.unsupported("keySet", "()Ljava/util/Set;", "java.util.Set non implémenté");

  r.define("Landroid/os/Bundle;", "Landroid/os/BaseBundle;", {"Ljava/lang/Cloneable;", "Landroid/os/Parcelable;"}, 0x1 | 0x10)
      .ctor("()V", [](Runtime&, NativeArgs& a) -> uint64_t {
        a.self().peer = std::make_unique<BundlePeer>();
        return 0;
      });
  r.define_interface("Landroid/os/Parcelable;");
}

void register_intent(rt::NativeRegistry& r) {
  const std::string self = "Landroid/content/Intent;";
  auto b = r.define(self, "Ljava/lang/Object;", {"Landroid/os/Parcelable;", "Ljava/lang/Cloneable;"});
  auto ip = [](Runtime& rt, Handle h) -> IntentPeer& { return peer_or_throw<IntentPeer>(rt, h, "Intent"); };
  b.ctor("()V", [](Runtime&, NativeArgs& a) -> uint64_t {
    a.self().peer = std::make_unique<IntentPeer>();
    return 0;
  });
  b.ctor("(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<IntentPeer>();
    p->action = a.str_or(1, u"");
    a.self().peer = std::move(p);
    return 0;
  });
  b.ctor("(Landroid/content/Context;Ljava/lang/Class;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<IntentPeer>();
    p->component = class_from_mirror(rt, a.h(2));
    a.self().peer = std::move(p);
    return 0;
  });
  b.method("setClass", "(Landroid/content/Context;Ljava/lang/Class;)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    ip(rt, a.h(0)).component = class_from_mirror(rt, a.h(2));
    return a.h(0);
  });
  b.method("setClassName", "(Landroid/content/Context;Ljava/lang/String;)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    ip(rt, a.h(0)).component = text::java_to_descriptor(text::utf16_to_utf8(a.str(2)));
    return a.h(0);
  });
  b.method("setClassName", "(Ljava/lang/String;Ljava/lang/String;)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    ip(rt, a.h(0)).component = text::java_to_descriptor(text::utf16_to_utf8(a.str(2)));
    return a.h(0);
  });
  b.method("setAction", "(Ljava/lang/String;)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    ip(rt, a.h(0)).action = a.str_or(1, u"");
    return a.h(0);
  });
  b.method("getAction", "()Ljava/lang/String;", [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    IntentPeer& p = ip(rt, a.h(0));
    return p.action.empty() ? kNull : rt.new_string(p.action);
  });
  register_extras_accessors(b, self, true);
  b.method("getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return e && e->kind == Extra::Kind::kString && !e->i ? rt.new_string(e->s) : kNull;
  });
  b.method("getIntExtra", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_int(e && e->kind == Extra::Kind::kInt ? int32_t(e->i) : a.i32(2));
  });
  b.method("getBooleanExtra", "(Ljava/lang/String;Z)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_bool(e && e->kind == Extra::Kind::kBool ? e->i != 0 : a.z(2));
  });
  b.method("getLongExtra", "(Ljava/lang/String;J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const Extra* e = find_extra(rt, a.h(0), a, 1);
    return rt::ret_long(e && e->kind == Extra::Kind::kLong ? e->i : a.i64(2));
  });
  b.method("hasExtra", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(find_extra(rt, a.h(0), a, 1) != nullptr);
  });
  b.method("getExtras", "()Landroid/os/Bundle;", [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    IntentPeer& p = ip(rt, a.h(0));
    if (p.extras.empty()) return kNull;
    Handle bundle = rt.alloc(rt.find_class("Landroid/os/Bundle;"));
    auto bp = std::make_unique<BundlePeer>();
    bp->values = p.extras;
    rt.object(bundle)->peer = std::move(bp);
    return bundle;
  });
  b.method("putExtras", "(Landroid/os/Bundle;)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& dst = ip(rt, a.h(0)).extras;
    for (const auto& [k, v] : peer_or_throw<BundlePeer>(rt, a.h(1), "Bundle").values) dst[k] = v;
    return a.h(0);
  });
  b.method("addFlags", "(I)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    ip(rt, a.h(0)).flags |= a.i32(1);
    return a.h(0);
  }, S::kPartial, "drapeaux mémorisés mais sans effet");
  b.method("setFlags", "(I)" + self, [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    ip(rt, a.h(0)).flags = a.i32(1);
    return a.h(0);
  }, S::kPartial, "drapeaux mémorisés mais sans effet");
  b.method("getFlags", "()I", [ip](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(ip(rt, a.h(0)).flags); });
}

void register_context(rt::NativeRegistry& r, Host& host) {
  auto ctx = r.define("Landroid/content/Context;", "Ljava/lang/Object;", {}, 0x1 | 0x400);
  ctx.method("getPackageName", "()Ljava/lang/String;", [&host](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8(host.package_name());
  });
  ctx.method("getApplicationContext", "()Landroid/content/Context;", [&host](Runtime&, NativeArgs& a) -> uint64_t {
    Handle app = host.application();
    return app ? app : a.h(0);
  });
  ctx.method("startActivity", "(Landroid/content/Intent;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    peer_or_throw<IntentPeer>(rt, a.h(1), "Intent");
    host.start_activity(a.h(0), a.h(1));
    return 0;
  });
  ctx.method("getMainLooper", "()Landroid/os/Looper;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.call_static("Landroid/os/Looper;", "getMainLooper", "()Landroid/os/Looper;");
  });
  ctx.unsupported("getString", "(I)Ljava/lang/String;", kNoResources);
  ctx.unsupported("getText", "(I)Ljava/lang/CharSequence;", kNoResources);
  ctx.unsupported("getResources", "()Landroid/content/res/Resources;", kNoResources);
  ctx.unsupported("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", "services système non disponibles");
  ctx.unsupported("getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
                  "stockage persistant prévu en phase 5");
  ctx.unsupported("getFilesDir", "()Ljava/io/File;", "accès fichiers prévu en phase 5");
  ctx.unsupported("checkSelfPermission", "(Ljava/lang/String;)I", "modèle de permissions non implémenté");

  r.define("Landroid/content/ContextWrapper;", "Landroid/content/Context;")
      .method("getBaseContext", "()Landroid/content/Context;", [](Runtime&, NativeArgs& a) -> uint64_t { return a.h(0); },
              S::kPartial, "renvoie le contexte lui-même");
  r.define("Landroid/view/ContextThemeWrapper;", "Landroid/content/ContextWrapper;")
      .stub("setTheme", "(I)V", "thèmes non pris en charge");

  r.define("Landroid/app/Application;", "Landroid/content/ContextWrapper;")
      .ctor("()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; })
      .method("onCreate", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; })
      .stub("onTerminate", "()V", "jamais appelé");
}

void register_activity(rt::NativeRegistry& r, Host& host) {
  auto b = r.define("Landroid/app/Activity;", "Landroid/view/ContextThemeWrapper;");
  auto ap = [](Runtime& rt, Handle h) -> ActivityPeer& { return peer_or_throw<ActivityPeer>(rt, h, "Activity"); };
  b.ctor("()V", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<ActivityPeer>();
    p->class_name = a.self().klass->descriptor;
    a.self().peer = std::move(p);
    return 0;
  });
  auto lifecycle = [ap](Runtime& rt, NativeArgs& a) -> uint64_t {
    ap(rt, a.h(0)).super_called = true;
    return 0;
  };
  b.method("onCreate", "(Landroid/os/Bundle;)V", lifecycle);
  b.method("onPostCreate", "(Landroid/os/Bundle;)V", lifecycle);
  for (const char* cb : {"onStart", "onRestart", "onResume", "onPostResume", "onPause", "onStop", "onDestroy"}) {
    b.method(cb, "()V", lifecycle);
  }
  b.method("onSaveInstanceState", "(Landroid/os/Bundle;)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; },
           S::kPartial, "état non restauré (pas de recréation d'activité)");
  b.method("onBackPressed", "()V", [&host](Runtime&, NativeArgs& a) -> uint64_t {
    host.finish_activity(a.h(0));
    return 0;
  });
  b.method("setContentView", "(Landroid/view/View;)V", [&host, ap](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(1) == kNull) rt.throw_npe("setContentView(null)");
    peer_or_throw<ViewPeer>(rt, a.h(1), "View");
    ap(rt, a.h(0)).content_view = a.h(1);
    host.content_changed(a.h(0));
    return 0;
  });
  b.unsupported("setContentView", "(I)V", std::string("layouts XML : ") + kNoResources);
  b.method("findViewById", "(I)Landroid/view/View;", [ap](Runtime& rt, NativeArgs& a) -> uint64_t {
    ActivityPeer& p = ap(rt, a.h(0));
    ViewPeer* root = view_peer(rt, p.content_view);
    ui::Node* n = root ? find_view_by_id(root->node.get(), a.i32(1)) : nullptr;
    return n ? n->java_handle : kNull;
  });
  b.method("setTitle", "(Ljava/lang/CharSequence;)V", [&host, ap](Runtime& rt, NativeArgs& a) -> uint64_t {
    ap(rt, a.h(0)).title = a.str_or(1, u"");
    host.title_changed(a.h(0));
    return 0;
  });
  b.unsupported("setTitle", "(I)V", kNoResources);
  b.method("getTitle", "()Ljava/lang/CharSequence;", [ap](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(ap(rt, a.h(0)).title);
  });
  b.method("finish", "()V", [&host](Runtime&, NativeArgs& a) -> uint64_t {
    host.finish_activity(a.h(0));
    return 0;
  });
  b.method("isFinishing", "()Z", [ap](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_bool(ap(rt, a.h(0)).finishing); });
  b.method("getIntent", "()Landroid/content/Intent;", [ap](Runtime& rt, NativeArgs& a) -> uint64_t { return ap(rt, a.h(0)).intent; });
  b.method("setIntent", "(Landroid/content/Intent;)V", [ap](Runtime& rt, NativeArgs& a) -> uint64_t {
    ap(rt, a.h(0)).intent = a.h(1);
    return 0;
  });
  b.method("runOnUiThread", "(Ljava/lang/Runnable;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(1) == kNull) rt.throw_npe("runOnUiThread(null)");
    if (host.on_ui_thread()) return rt.call_virtual(a.h(1), "run", "()V");
    rt::GlobalRef keep(rt, a.h(1));
    host.post([&rt, keep] { rt.call_virtual(keep.get(), "run", "()V"); }, 0);
    return 0;
  });
  b.method("getApplication", "()Landroid/app/Application;", [&host](Runtime&, NativeArgs&) -> uint64_t { return host.application(); });
  b.method("startActivityForResult", "(Landroid/content/Intent;I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    peer_or_throw<IntentPeer>(rt, a.h(1), "Intent");
    host.start_activity(a.h(0), a.h(1));
    return 0;
  }, S::kPartial, "le résultat n'est jamais renvoyé (onActivityResult non appelé)");
  b.stub("setResult", "(I)V", "résultats d'activité non pris en charge");
  b.stub("setResult", "(ILandroid/content/Intent;)V", "résultats d'activité non pris en charge");
  b.stub("overridePendingTransition", "(II)V", "pas d'animations");
  b.unsupported("getWindow", "()Landroid/view/Window;", "API Window non implémentée");
  b.unsupported("getActionBar", "()Landroid/app/ActionBar;", "barre d'action non implémentée");
  b.unsupported("getLayoutInflater", "()Landroid/view/LayoutInflater;", std::string("inflation de layouts : ") + kNoResources);
  b.unsupported("getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
                "stockage persistant prévu en phase 5");
}

void register_os(rt::NativeRegistry& r, Host& host) {
  r.define("Landroid/os/Looper;", "Ljava/lang/Object;", {}, 0x1 | 0x10)
      .static_method("getMainLooper", "()Landroid/os/Looper;", [](Runtime& rt, NativeArgs&) -> uint64_t {
        auto& cache = rt.object_cache();
        auto it = cache.find("looper:main");
        if (it != cache.end()) return it->second;
        Handle h = rt.alloc(rt.find_class("Landroid/os/Looper;"));
        cache["looper:main"] = h;
        return h;
      })
      .static_method("myLooper", "()Landroid/os/Looper;", [&host](Runtime& rt, NativeArgs&) -> uint64_t {
        if (!host.on_ui_thread()) return kNull;
        return rt.call_static("Landroid/os/Looper;", "getMainLooper", "()Landroid/os/Looper;");
      })
      .stub("prepare", "()V", "un seul Looper (thread UI)");
  r.extend("Landroid/os/Looper;").spec().methods.back().access |= 0x8;
  r.extend("Landroid/os/Looper;").unsupported("loop", "()V", "boucle gérée par l'hôte AnyAndroid");
  r.extend("Landroid/os/Looper;").spec().methods.back().access |= 0x8;

  auto h = r.define("Landroid/os/Handler;", "Ljava/lang/Object;");
  auto init = [](Runtime&, NativeArgs& a) -> uint64_t {
    a.self().peer = std::make_unique<HandlerPeer>();
    return 0;
  };
  h.ctor("()V", [&host, init](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (!host.on_ui_thread()) {
      rt.throw_new("Ljava/lang/RuntimeException;",
                   "Can't create handler inside thread that has not called Looper.prepare() "
                   "([AnyAndroid] seul le Looper principal existe : utilisez new Handler(Looper.getMainLooper()))");
    }
    return init(rt, a);
  });
  h.ctor("(Landroid/os/Looper;)V", init, S::kPartial, "seul le Looper principal est pris en charge");
  auto post = [&host](Runtime& rt, Handle self, Handle runnable, uint32_t delay) {
    if (runnable == kNull) rt.throw_npe("Handler.post(null)");
    HandlerPeer& p = peer_or_throw<HandlerPeer>(rt, self, "Handler");
    auto gens = p.generations;
    uint64_t gen = (*gens)[runnable];
    rt::GlobalRef keep(rt, runnable);  // the queued Runnable must survive collections
    host.post([&rt, gens, keep, gen]() {
      if ((*gens)[keep.get()] != gen) return;  // removed meanwhile
      rt.call_virtual(keep.get(), "run", "()V");
    }, delay);
  };
  h.method("post", "(Ljava/lang/Runnable;)Z", [post](Runtime& rt, NativeArgs& a) -> uint64_t {
    post(rt, a.h(0), a.h(1), 0);
    return 1;
  });
  h.method("postDelayed", "(Ljava/lang/Runnable;J)Z", [post](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t d = a.i64(2);
    post(rt, a.h(0), a.h(1), uint32_t(std::clamp<int64_t>(d, 0, 24 * 3600 * 1000)));
    return 1;
  }, S::kPartial, "délai ignoré : exécuté au prochain tour de la boucle d'événements");
  h.method("removeCallbacks", "(Ljava/lang/Runnable;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    (*peer_or_throw<HandlerPeer>(rt, a.h(0), "Handler").generations)[a.h(1)]++;
    return 0;
  });

  auto log = r.define("Landroid/util/Log;", "Ljava/lang/Object;", {}, 0x1 | 0x10);
  struct Level {
    const char* name;
    char letter;
  };
  for (Level l : {Level{"v", 'V'}, Level{"d", 'D'}, Level{"i", 'I'}, Level{"w", 'W'}, Level{"e", 'E'}, Level{"wtf", 'F'}}) {
    char letter = l.letter;
    log.static_method(l.name, "(Ljava/lang/String;Ljava/lang/String;)I", [&host, letter](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::string msg = text::utf16_to_utf8(a.str_or(1, u"null"));
      host.logcat(letter, text::utf16_to_utf8(a.str_or(0, u"null")), msg);
      return rt::ret_int(int32_t(msg.size()));
    });
    log.static_method(l.name, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I",
                      [&host, letter](Runtime& rt, NativeArgs& a) -> uint64_t {
                        std::string msg = text::utf16_to_utf8(a.str_or(1, u"null"));
                        if (a.h(2)) msg += "\n" + rt.describe_throwable(a.h(2));
                        host.logcat(letter, text::utf16_to_utf8(a.str_or(0, u"null")), msg);
                        return rt::ret_int(int32_t(msg.size()));
                      });
  }
  log.static_method("isLoggable", "(Ljava/lang/String;I)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 1; });

  r.define("Landroid/util/AndroidRuntimeException;", "Ljava/lang/RuntimeException;");
  r.define("Landroid/view/ViewRootImpl$CalledFromWrongThreadException;", "Landroid/util/AndroidRuntimeException;");
  r.define("Landroid/util/SuperNotCalledException;", "Landroid/util/AndroidRuntimeException;");
  r.define("Landroid/content/ActivityNotFoundException;", "Ljava/lang/RuntimeException;");
}

}  // namespace

void register_android_app(rt::NativeRegistry& r, Host& host) {
  register_bundle(r);
  register_intent(r);
  register_context(r, host);
  register_activity(r, host);
  register_os(r, host);
}

}  // namespace aa::bridge
