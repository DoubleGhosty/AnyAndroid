// android.view / android.widget / android.text subset, backed by ui::Node.

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

struct LayoutParamsPeer final : rt::NativePeer {
  int width = -2;
  int height = -2;
  float weight = 0;
};

struct TextPeer final : rt::NativePeer {
  std::u16string text;
};

struct ToastPeer final : rt::NativePeer {
  std::u16string text;
  int duration = 0;
};

ViewPeer& vp(Runtime& rt, Handle h) { return peer_or_throw<ViewPeer>(rt, h, "View"); }
ui::Node& node_of(Runtime& rt, Handle h) { return *vp(rt, h).node; }

// Android only lets the thread that created the view hierarchy touch it.
void check_ui_thread(Runtime& rt, Host& host) {
  if (!host.on_ui_thread()) {
    rt.throw_new("Landroid/view/ViewRootImpl$CalledFromWrongThreadException;",
                 "Only the original thread that created a view hierarchy can touch its views.");
  }
}

void changed(Host& host, ui::Node& n) {
  if (ui::Backend* b = host.ui()) b->node_changed(&n);
}

void make_view(NativeArgs& a, ui::NodeKind kind, const char* type_name, Handle context) {
  rt::Object& self = a.self();
  auto p = std::make_unique<ViewPeer>();
  p->node = std::make_unique<ui::Node>();
  p->node->kind = kind;
  p->node->type_name = type_name;
  p->node->java_handle = a.h(0);
  if (kind == ui::NodeKind::kButton) p->node->clickable = true;
  p->context = context;
  self.peer = std::move(p);
}

void view_ctors(rt::ClassBuilder& b, ui::NodeKind kind, const char* type_name) {
  b.ctor("(Landroid/content/Context;)V", [kind, type_name](Runtime& rt, NativeArgs& a) -> uint64_t {
    make_view(a, kind, type_name, a.h(1));
    return 0;
  });
  b.ctor("(Landroid/content/Context;Landroid/util/AttributeSet;)V", [kind, type_name](Runtime& rt, NativeArgs& a) -> uint64_t {
    make_view(a, kind, type_name, a.h(1));
    return 0;
  }, S::kPartial, "attributs XML ignorés");
}

void apply_layout_params(Runtime& rt, Host& host, Handle view, Handle params) {
  ViewPeer& v = vp(rt, view);
  v.layout_params = params;
  if (auto* lp = rt.peer<LayoutParamsPeer>(params)) {
    v.node->width = lp->width;
    v.node->height = lp->height;
    v.node->weight = lp->weight;
    changed(host, *v.node);
  }
}

void add_child(Runtime& rt, Host& host, Handle parent, Handle child, int32_t index) {
  check_ui_thread(rt, host);
  if (child == kNull) rt.throw_new("Ljava/lang/IllegalArgumentException;", "Cannot add a null child view to a ViewGroup");
  ui::Node& p = node_of(rt, parent);
  ui::Node& c = node_of(rt, child);
  if (c.parent) {
    rt.throw_new("Ljava/lang/IllegalStateException;",
                 "The specified child already has a parent. You must call removeView() on the child's parent first.");
  }
  for (ui::Node* n = &p; n; n = n->parent) {
    if (n == &c) rt.throw_new("Ljava/lang/IllegalArgumentException;", "une vue ne peut pas contenir l'un de ses ancêtres");
  }
  if (index < 0 || size_t(index) > p.children.size()) index = int32_t(p.children.size());
  p.children.insert(p.children.begin() + index, &c);
  c.parent = &p;
  if (ui::Backend* b = host.ui()) b->tree_changed(&p);
}

void remove_child_at(Runtime& rt, Host& host, ui::Node& p, size_t i) {
  check_ui_thread(rt, host);
  ui::Node* c = p.children[i];
  c->parent = nullptr;
  p.children.erase(p.children.begin() + long(i));
  if (ui::Backend* b = host.ui()) b->tree_changed(&p);
}

void register_view(rt::NativeRegistry& r, Host& host) {
  r.define_interface("Landroid/view/View$OnClickListener;").abstract_method("onClick", "(Landroid/view/View;)V");
  r.define_interface("Landroid/view/View$OnLongClickListener;").abstract_method("onLongClick", "(Landroid/view/View;)Z");
  r.define_interface("Landroid/view/ViewParent;");
  r.define_interface("Landroid/view/ViewManager;");
  r.define_interface("Landroid/util/AttributeSet;");

  auto v = r.define("Landroid/view/View;", "Ljava/lang/Object;");
  view_ctors(v, ui::NodeKind::kView, "View");
  v.method("setOnClickListener", "(Landroid/view/View$OnClickListener;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ViewPeer& p = vp(rt, a.h(0));
    p.click_listener = a.h(1);
    p.node->clickable = a.h(1) != kNull || p.node->kind == ui::NodeKind::kButton;
    changed(host, *p.node);
    return 0;
  });
  auto click = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ViewPeer& p = vp(rt, a.h(0));
    if (p.click_listener == kNull || !p.node->enabled) return 0;
    rt.call_virtual(p.click_listener, "onClick", "(Landroid/view/View;)V", {a.h(0)});
    return 1;
  };
  v.method("performClick", "()Z", click);
  v.method("callOnClick", "()Z", click);
  v.method("setId", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    node_of(rt, a.h(0)).id = a.i32(1);
    return 0;
  });
  v.method("getId", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(node_of(rt, a.h(0)).id); });
  v.method("setVisibility", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    int32_t vis = a.i32(1);
    n.visible = vis == 0;
    n.gone = vis == 8;
    changed(host, n);
    return 0;
  });
  v.method("getVisibility", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    return rt::ret_int(n.visible ? 0 : (n.gone ? 8 : 4));
  });
  v.method("setEnabled", "(Z)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.enabled = a.z(1);
    changed(host, n);
    return 0;
  });
  v.method("isEnabled", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_bool(node_of(rt, a.h(0)).enabled); });
  v.method("setClickable", "(Z)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.clickable = a.z(1);
    changed(host, n);
    return 0;
  });
  v.method("isClickable", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_bool(node_of(rt, a.h(0)).clickable); });
  v.method("getContext", "()Landroid/content/Context;", [](Runtime& rt, NativeArgs& a) -> uint64_t { return vp(rt, a.h(0)).context; });
  v.method("setTag", "(Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    vp(rt, a.h(0)).tag = a.h(1);
    return 0;
  });
  v.method("getTag", "()Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t { return vp(rt, a.h(0)).tag; });
  v.method("setBackgroundColor", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.background = a.raw(1);
    changed(host, n);
    return 0;
  }, S::kPartial, "couleur unie uniquement");
  v.method("setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    apply_layout_params(rt, host, a.h(0), a.h(1));
    return 0;
  }, S::kPartial, "largeur, hauteur et poids uniquement");
  v.method("getLayoutParams", "()Landroid/view/ViewGroup$LayoutParams;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return vp(rt, a.h(0)).layout_params;
  });
  v.method("getParent", "()Landroid/view/ViewParent;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    return n.parent ? n.parent->java_handle : kNull;
  });
  v.method("post", "(Ljava/lang/Runnable;)Z", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle runnable = a.h(1);
    if (runnable == kNull) rt.throw_npe("View.post(null)");
    rt::GlobalRef keep(rt, runnable);
    host.post([&rt, keep] { rt.call_virtual(keep.get(), "run", "()V"); }, 0);
    return 1;
  });
  v.stub("setPadding", "(IIII)V", "marges internes ignorées");
  v.stub("invalidate", "()V", "le backend redessine automatiquement");
  v.stub("requestLayout", "()V", "le backend recalcule la mise en page automatiquement");
  v.stub("setOnLongClickListener", "(Landroid/view/View$OnLongClickListener;)V", "appui long jamais déclenché");
  v.stub("getWidth", "()I", "dimensions non calculées côté pont (renvoie 0)");
  v.stub("getHeight", "()I", "dimensions non calculées côté pont (renvoie 0)");
  v.method("requestFocus", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 1; }, S::kStub, "focus géré par Windows");

  auto lp = r.define("Landroid/view/ViewGroup$LayoutParams;", "Ljava/lang/Object;");
  auto lp_ctor = [](Runtime&, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<LayoutParamsPeer>();
    p->width = a.i32(1);
    p->height = a.i32(2);
    if (a.count() > 3) p->weight = a.f32(3);
    a.self().peer = std::move(p);
    return 0;
  };
  lp.ctor("(II)V", lp_ctor);
  r.define("Landroid/view/ViewGroup$MarginLayoutParams;", "Landroid/view/ViewGroup$LayoutParams;")
      .ctor("(II)V", lp_ctor)
      .stub("setMargins", "(IIII)V", "marges ignorées");
  r.define("Landroid/widget/LinearLayout$LayoutParams;", "Landroid/view/ViewGroup$MarginLayoutParams;")
      .ctor("(II)V", lp_ctor)
      .ctor("(IIF)V", lp_ctor);

  auto g = r.define("Landroid/view/ViewGroup;", "Landroid/view/View;", {"Landroid/view/ViewParent;", "Landroid/view/ViewManager;"},
                    0x1 | 0x400);
  g.method("addView", "(Landroid/view/View;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    add_child(rt, host, a.h(0), a.h(1), -1);
    return 0;
  });
  g.method("addView", "(Landroid/view/View;I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    add_child(rt, host, a.h(0), a.h(1), a.i32(2));
    return 0;
  });
  g.method("addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    add_child(rt, host, a.h(0), a.h(1), -1);
    apply_layout_params(rt, host, a.h(1), a.h(2));
    return 0;
  }, S::kPartial, "largeur, hauteur et poids uniquement");
  g.method("removeView", "(Landroid/view/View;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& p = node_of(rt, a.h(0));
    if (a.h(1) == kNull) return 0;
    ui::Node* c = &node_of(rt, a.h(1));
    for (size_t i = 0; i < p.children.size(); ++i) {
      if (p.children[i] == c) {
        remove_child_at(rt, host, p, i);
        break;
      }
    }
    return 0;
  });
  g.method("removeViewAt", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& p = node_of(rt, a.h(0));
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= p.children.size()) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "index " + std::to_string(i));
    remove_child_at(rt, host, p, size_t(i));
    return 0;
  });
  g.method("removeAllViews", "()V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& p = node_of(rt, a.h(0));
    while (!p.children.empty()) remove_child_at(rt, host, p, p.children.size() - 1);
    return 0;
  });
  g.method("getChildCount", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(node_of(rt, a.h(0)).children.size()));
  });
  g.method("getChildAt", "(I)Landroid/view/View;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& p = node_of(rt, a.h(0));
    int32_t i = a.i32(1);
    return i >= 0 && size_t(i) < p.children.size() ? p.children[size_t(i)]->java_handle : kNull;
  });
  g.method("indexOfChild", "(Landroid/view/View;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& p = node_of(rt, a.h(0));
    if (a.h(1) == kNull) return rt::ret_int(-1);
    ui::Node* c = &node_of(rt, a.h(1));
    for (size_t i = 0; i < p.children.size(); ++i) {
      if (p.children[i] == c) return rt::ret_int(int32_t(i));
    }
    return rt::ret_int(-1);
  });

  auto ll = r.define("Landroid/widget/LinearLayout;", "Landroid/view/ViewGroup;");
  view_ctors(ll, ui::NodeKind::kGroup, "LinearLayout");
  ll.method("setOrientation", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.orientation = a.i32(1) == 1 ? ui::Orientation::kVertical : ui::Orientation::kHorizontal;
    changed(host, n);
    return 0;
  });
  ll.method("getOrientation", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(node_of(rt, a.h(0)).orientation == ui::Orientation::kVertical ? 1 : 0);
  });
  ll.stub("setGravity", "(I)V", "gravité ignorée");
  ll.stub("setWeightSum", "(F)V", "somme des poids calculée automatiquement");

  auto fl = r.define("Landroid/widget/FrameLayout;", "Landroid/view/ViewGroup;", {}, 0x1, S::kPartial,
                     "enfants empilés verticalement au lieu d'être superposés");
  fl.ctor("(Landroid/content/Context;)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    make_view(a, ui::NodeKind::kGroup, "FrameLayout", a.h(1));
    static_cast<ViewPeer*>(a.self().peer.get())->node->orientation = ui::Orientation::kVertical;
    return 0;
  }, S::kPartial, "enfants empilés verticalement");
  r.define("Landroid/widget/ScrollView;", "Landroid/widget/FrameLayout;", {}, 0x1, S::kPartial, "pas de défilement")
      .ctor("(Landroid/content/Context;)V", [](Runtime&, NativeArgs& a) -> uint64_t {
        make_view(a, ui::NodeKind::kGroup, "ScrollView", a.h(1));
        static_cast<ViewPeer*>(a.self().peer.get())->node->orientation = ui::Orientation::kVertical;
        return 0;
      }, S::kPartial, "pas de défilement");
}

void register_text(rt::NativeRegistry& r, Host& host) {
  r.define_interface("Landroid/text/Editable;", {"Ljava/lang/CharSequence;"});
  auto ssb = r.define("Landroid/text/SpannableStringBuilder;", "Ljava/lang/Object;",
                      {"Ljava/lang/CharSequence;", "Landroid/text/Editable;"}, 0x1, S::kPartial, "texte brut, sans styles");
  auto tp = [](Runtime& rt, Handle h) -> std::u16string& { return peer_or_throw<TextPeer>(rt, h, "Editable").text; };
  ssb.ctor("()V", [](Runtime&, NativeArgs& a) -> uint64_t {
    a.self().peer = std::make_unique<TextPeer>();
    return 0;
  });
  ssb.ctor("(Ljava/lang/CharSequence;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<TextPeer>();
    p->text = a.str_or(1, u"");
    a.self().peer = std::move(p);
    return 0;
  });
  ssb.method("toString", "()Ljava/lang/String;", [tp](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.new_string(tp(rt, a.h(0))); });
  ssb.method("length", "()I", [tp](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(int32_t(tp(rt, a.h(0)).size())); });
  ssb.method("charAt", "(I)C", [tp](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& s = tp(rt, a.h(0));
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= s.size()) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "index " + std::to_string(i));
    return s[size_t(i)];
  });

  auto tv = r.define("Landroid/widget/TextView;", "Landroid/view/View;");
  view_ctors(tv, ui::NodeKind::kText, "TextView");
  tv.method("setText", "(Ljava/lang/CharSequence;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    check_ui_thread(rt, host);
    ui::Node& n = node_of(rt, a.h(0));
    n.text = a.str_or(1, u"");
    changed(host, n);
    return 0;
  });
  tv.unsupported("setText", "(I)V", kNoResources);
  tv.method("getText", "()Ljava/lang/CharSequence;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(node_of(rt, a.h(0)).text);
  });
  tv.method("append", "(Ljava/lang/CharSequence;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.text += a.str_or(1, u"null");
    changed(host, n);
    return 0;
  });
  tv.method("length", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(int32_t(node_of(rt, a.h(0)).text.size())); });
  tv.method("setHint", "(Ljava/lang/CharSequence;)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.hint = a.str_or(1, u"");
    changed(host, n);
    return 0;
  });
  tv.method("getHint", "()Ljava/lang/CharSequence;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    return n.hint.empty() ? kNull : rt.new_string(n.hint);
  });
  tv.method("setTextSize", "(F)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.text_size_sp = a.f32(1);
    changed(host, n);
    return 0;
  }, S::kPartial, "taille en sp appliquée à la police Windows");
  tv.method("setTextSize", "(IF)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.text_size_sp = a.f32(2);
    changed(host, n);
    return 0;
  }, S::kPartial, "unité ignorée (traitée comme sp)");
  tv.method("setTextColor", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    ui::Node& n = node_of(rt, a.h(0));
    n.text_color = a.raw(1);
    changed(host, n);
    return 0;
  }, S::kPartial, "couleur unie uniquement");
  tv.stub("setGravity", "(I)V", "gravité ignorée");
  tv.stub("setSingleLine", "()V", "mise en forme ignorée");
  tv.stub("setMaxLines", "(I)V", "mise en forme ignorée");
  tv.stub("setInputType", "(I)V", "type de saisie ignoré");

  auto btn = r.define("Landroid/widget/Button;", "Landroid/widget/TextView;");
  view_ctors(btn, ui::NodeKind::kButton, "Button");

  auto et = r.define("Landroid/widget/EditText;", "Landroid/widget/TextView;");
  view_ctors(et, ui::NodeKind::kEdit, "EditText");
  et.method("getText", "()Landroid/text/Editable;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle h = rt.alloc(rt.find_class("Landroid/text/SpannableStringBuilder;"));
    auto p = std::make_unique<TextPeer>();
    p->text = node_of(rt, a.h(0)).text;
    rt.object(h)->peer = std::move(p);
    return h;
  }, S::kPartial, "copie du texte (les modifications de l'Editable ne sont pas répercutées)");
  et.stub("setSelection", "(I)V", "sélection gérée par Windows");

  auto toast = r.define("Landroid/widget/Toast;", "Ljava/lang/Object;");
  auto tst = [](Runtime& rt, Handle h) -> ToastPeer& { return peer_or_throw<ToastPeer>(rt, h, "Toast"); };
  toast.ctor("(Landroid/content/Context;)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    a.self().peer = std::make_unique<ToastPeer>();
    return 0;
  });
  toast.static_method("makeText", "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;",
                      [](Runtime& rt, NativeArgs& a) -> uint64_t {
                        Handle h = rt.alloc(rt.find_class("Landroid/widget/Toast;"));
                        auto p = std::make_unique<ToastPeer>();
                        p->text = a.str_or(1, u"");
                        p->duration = a.i32(2);
                        rt.object(h)->peer = std::move(p);
                        return h;
                      });
  toast.unsupported("makeText", "(Landroid/content/Context;II)Landroid/widget/Toast;", kNoResources);
  toast.spec().methods.back().access |= 0x8;
  toast.method("show", "()V", [&host, tst](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (!host.on_ui_thread()) {
      rt.throw_new("Ljava/lang/RuntimeException;", "Can't toast on a thread that has not called Looper.prepare()");
    }
    ToastPeer& p = tst(rt, a.h(0));
    if (ui::Backend* b = host.ui()) {
      b->show_toast(p.text, p.duration == 1);
    } else {
      host.logcat('I', "Toast", text::utf16_to_utf8(p.text));
    }
    return 0;
  });
  toast.method("setText", "(Ljava/lang/CharSequence;)V", [tst](Runtime& rt, NativeArgs& a) -> uint64_t {
    tst(rt, a.h(0)).text = a.str_or(1, u"");
    return 0;
  });
  toast.method("setDuration", "(I)V", [tst](Runtime& rt, NativeArgs& a) -> uint64_t {
    tst(rt, a.h(0)).duration = a.i32(1);
    return 0;
  });
  toast.stub("cancel", "()V", "le toast disparaît de lui-même");
}

}  // namespace

void register_android_widget(rt::NativeRegistry& r, Host& host) {
  register_view(r, host);
  register_text(r, host);
}

}  // namespace aa::bridge
