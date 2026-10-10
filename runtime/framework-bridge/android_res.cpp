// Application resources in bridge mode: android.content.res.Resources and
// android.view.LayoutInflater over the resources.arsc of the APK (aa::res),
// plus the resource-id overloads of Context, Activity, TextView and Toast.
//
// Layouts are inflated into the bridge's widgets the way LayoutInflater does
// it: one view per start tag (android.widget, then android.view, or an
// application class), framework attributes applied through the views' public
// setters, <include> and <merge> supported.

#include <cmath>
#include <cstring>

#include "common/bytes.h"
#include "common/log.h"
#include "common/text.h"
#include "dex-engine/jlang.h"
#include "framework-bridge/internal.h"
#include "resources/asset_manager.h"

namespace aa::bridge {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

namespace {

// android.R.attr ids read by the inflater.
enum : uint32_t {
  kAttrEnabled = 0x0101000e,
  kAttrTextSize = 0x01010095,
  kAttrTextColor = 0x01010098,
  kAttrOrientation = 0x010100c4,
  kAttrId = 0x010100d0,
  kAttrBackground = 0x010100d4,
  kAttrVisibility = 0x010100dc,
  kAttrLayoutWidth = 0x010100f4,
  kAttrLayoutHeight = 0x010100f5,
  kAttrText = 0x0101014f,
  kAttrHint = 0x01010150,
  kAttrLayoutWeight = 0x01010181,
  kAttrOnClick = 0x0101026f,
};

constexpr int kMaxIncludeDepth = 16;

struct InflaterPeer final : rt::NativePeer {
  Handle context = kNull;
  void trace(std::vector<Handle>& out) const override { out.push_back(context); }
};

// View$DeclaredOnClickListener: android:onClick="method" calls method(View)
// on the view's context.
struct DeclaredClickPeer final : rt::NativePeer {
  std::string method;
  Handle view = kNull;
  void trace(std::vector<Handle>& out) const override { out.push_back(view); }
};

std::string hex_id(uint32_t id) { return hex(id, 8); }

[[noreturn]] void not_found(Runtime& rt, const std::string& what, uint32_t id) {
  rt.throw_new("Landroid/content/res/Resources$NotFoundException;", what + " resource ID #" + hex_id(id));
}

[[noreturn]] void inflate_error(Runtime& rt, int32_t line, const std::string& message) {
  rt.throw_new("Landroid/view/InflateException;", "Binary XML file line #" + std::to_string(line) + ": " + message);
}

// A resource value with its references followed.
struct Resolved {
  uint8_t type = res::kTypeNull;
  uint32_t data = 0;
  int32_t cookie = -1;
  std::optional<std::u16string> string;  // kTypeString
};

std::optional<Resolved> resolve(const res::AssetManager* am, uint32_t id) {
  if (!am || id == 0) return std::nullopt;
  auto v = am->get_resource(id, false);
  if (!v || !am->resolve_reference(*v)) return std::nullopt;
  Resolved r{v->type, v->data, v->cookie, std::nullopt};
  if (r.type == res::kTypeString) {
    if (auto pool = am->string_pool(v->cookie)) r.string = pool->string16(v->data);
    if (!r.string) return std::nullopt;
  }
  return r;
}

Resolved require(Runtime& rt, Host& host, uint32_t id, const char* what) {
  auto r = resolve(host.resources(), id);
  if (!r) not_found(rt, what, id);
  return *r;
}

float complex_to_float(uint32_t data) {
  static constexpr float kMults[4] = {1.0f / 256, 1.0f / 32768, 1.0f / 8388608, 1.0f / 2147483648.0f};
  return float(int32_t(data & 0xffffff00u)) * kMults[(data >> 4) & 3];
}

float density_of(Host& host) {
  const res::AssetManager* am = host.resources();
  uint16_t d = am ? am->configuration().density : 0;
  return d == 0 || d >= res::ResConfig::kDensityAny ? 160.0f : float(d);
}

// TypedValue.complexToDimension with this device's metrics (scaledDensity =
// density, xdpi = densityDpi).
float dimension_px(Host& host, uint32_t data) {
  float v = complex_to_float(data), dpi = density_of(host);
  switch (data & 0xf) {
    case 0: return v;
    case 1:
    case 2: return v * dpi / 160.0f;
    case 3: return v * dpi / 72.0f;
    case 4: return v * dpi;
    case 5: return v * dpi / 25.4f;
    default: return 0;
  }
}

bool is_int(uint8_t type) { return type >= res::kTypeFirstInt && type <= res::kTypeLastInt; }

// TypedValue.coerceToString for the types an application meets.
std::u16string coerce_to_string(const Resolved& r) {
  if (r.string) return *r.string;
  char buf[48];
  switch (r.type) {
    case 0x12: return r.data ? u"true" : u"false";
    case 0x11: std::snprintf(buf, sizeof buf, "0x%08x", r.data); break;
    case 0x1c: case 0x1d: case 0x1e: case 0x1f: std::snprintf(buf, sizeof buf, "#%08x", r.data); break;
    case res::kTypeFloat: {
      float f;
      std::memcpy(&f, &r.data, 4);
      std::snprintf(buf, sizeof buf, "%g", double(f));
      break;
    }
    case res::kTypeDimension: {
      static const char* kUnits[] = {"px", "dip", "sp", "pt", "in", "mm"};
      unsigned u = r.data & 0xf;
      std::snprintf(buf, sizeof buf, "%g%s", double(complex_to_float(r.data)), u < 6 ? kUnits[u] : "");
      break;
    }
    default:
      if (is_int(r.type)) {
        std::snprintf(buf, sizeof buf, "%d", int32_t(r.data));
      } else {
        return u"";
      }
  }
  return text::utf8_to_utf16(buf);
}

std::u16string get_text(Runtime& rt, Host& host, uint32_t id) {
  Resolved r = require(rt, host, id, "String");
  if (!r.string) not_found(rt, "String", id);
  return *r.string;
}

int32_t get_color(Runtime& rt, Host& host, uint32_t id) {
  Resolved r = require(rt, host, id, "Color");
  if (r.type < 0x1c || r.type > 0x1f) not_found(rt, "Color", id);
  return int32_t(r.data);
}

// --- Inflation -------------------------------------------------------------------

Handle resources_object(Runtime& rt) {
  auto& cache = rt.object_cache();
  auto it = cache.find("bridge:resources");
  if (it != cache.end()) return it->second;
  Handle h = rt.alloc(rt.find_class("Landroid/content/res/Resources;"));
  cache["bridge:resources"] = h;
  return h;
}

Handle new_inflater(Runtime& rt, Handle context) {
  Handle h = rt.alloc(rt.find_class("Landroid/view/LayoutInflater;"));
  auto p = std::make_unique<InflaterPeer>();
  p->context = context;
  rt.object(h)->peer = std::move(p);
  return h;
}

bool instance_of(Runtime& rt, Handle h, const char* descriptor) {
  rt::Class* c = rt.lookup_class(descriptor);
  return c && h != kNull && rt.is_assignable(rt.class_of(h), c);
}

uint32_t float_bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

class Inflater {
 public:
  Inflater(Runtime& rt, Host& host, Handle context) : rt_(rt), host_(host), context_(context) {}

  Handle inflate(uint32_t layout, Handle parent, bool attach, int depth = 0) {
    if (depth > kMaxIncludeDepth) rt_.throw_new("Landroid/view/InflateException;", "<include /> imbriqués trop profondément");
    const res::AssetManager* am = host_.resources();
    Resolved file = require(rt_, host_, layout, "Layout");
    if (!file.string) not_found(rt_, "Layout", layout);
    std::string path = text::utf16_to_utf8(*file.string);
    auto bytes = am->open_non_asset(path, file.cookie, nullptr);
    if (!bytes) not_found(rt_, "File " + path + " from xml type layout", layout);
    std::shared_ptr<res::XmlTree> tree;
    try {
      tree = res::XmlTree::parse(std::move(*bytes), am->package_id_of_cookie(file.cookie));
    } catch (const std::exception& e) {
      rt_.throw_new("Landroid/view/InflateException;", path + " : XML binaire invalide : " + e.what());
    }
    res::XmlParser p(tree, layout);

    rt::GlobalRef root;
    std::vector<rt::GlobalRef> views;  // open view elements
    std::vector<bool> is_view;         // per open element
    bool merged = false;
    for (int32_t ev = p.next(); ev != res::XmlParser::kEndDocument; ev = p.next()) {
      if (ev == res::XmlParser::kBadDocument) inflate_error(rt_, p.line(), "document XML invalide");
      if (ev == res::XmlParser::kEndTag) {
        if (!is_view.empty()) {
          if (is_view.back()) views.pop_back();
          is_view.pop_back();
        }
        continue;
      }
      if (ev != res::XmlParser::kStartTag) continue;
      std::u16string name = pool_string(p, p.element_name());
      bool first = root.get() == kNull && !merged;
      if (name == u"merge") {
        if (!first || parent == kNull || !attach) {
          inflate_error(rt_, p.line(), "<merge /> can be used only with a valid ViewGroup root and attachToRoot=true");
        }
        merged = true;
        views.emplace_back(rt_, parent);
        is_view.push_back(true);
        continue;
      }
      if (name == u"include") {
        if (views.empty()) inflate_error(rt_, p.line(), "<include /> cannot be the root element");
        include(p, views.back().get(), depth);
        is_view.push_back(false);
        continue;
      }
      if (!views.empty() && !instance_of(rt_, views.back().get(), "Landroid/view/ViewGroup;")) {
        inflate_error(rt_, p.line(), "la vue parente n'est pas un ViewGroup : <" + text::utf16_to_utf8(name) + "> refusé");
      }
      rt::GlobalRef view(rt_, create_view(name, p.line()));
      apply_attributes(view.get(), p);
      if (first) {
        root = view;
      } else {
        rt_.call_virtual(views.back().get(), "addView", "(Landroid/view/View;)V", {view.get()});
      }
      views.push_back(view);
      is_view.push_back(true);
    }
    if (merged) return parent;
    if (root.get() == kNull) rt_.throw_new("Landroid/view/InflateException;", path + " : aucun élément racine");
    if (parent != kNull && attach) {
      rt_.call_virtual(parent, "addView", "(Landroid/view/View;)V", {root.get()});
      return parent;
    }
    return root.get();
  }

 private:
  std::u16string pool_string(const res::XmlParser& p, int32_t index) const {
    if (index < 0) return u"";
    return p.tree()->strings()->string16(uint32_t(index)).value_or(u"");
  }

  // Value of attribute i, references resolved; nullopt (logged) for theme
  // attributes and references the bridge cannot resolve (framework resources).
  std::optional<Resolved> value(const res::XmlParser& p, int32_t i) {
    res::Value v = p.attribute_value(i);
    if (v.type == res::kTypeReference) {
      auto r = resolve(host_.resources(), v.data);
      if (!r) log::warn("inflate", "ligne " + std::to_string(p.line()) + " : ressource " + hex_id(v.data) + " introuvable, attribut ignoré");
      return r;
    }
    if (v.type == res::kTypeAttribute) {
      log::warn("inflate", "ligne " + std::to_string(p.line()) + " : attribut de thème ?" + hex_id(v.data) + " ignoré (pas de thèmes en mode pont)");
      return std::nullopt;
    }
    Resolved r{v.type, v.data, -1, std::nullopt};
    if (v.type == res::kTypeString) r.string = pool_string(p, int32_t(v.data));
    return r;
  }

  Handle create_view(const std::u16string& tag, int32_t line) {
    std::string name = text::utf16_to_utf8(tag);
    std::vector<std::string> candidates;
    if (name.find('.') == std::string::npos) {
      candidates = {"android.widget." + name, "android.view." + name};
    } else {
      candidates = {name};
    }
    rt::Class* cls = nullptr;
    for (const auto& c : candidates) {
      if ((cls = rt_.lookup_class(text::java_to_descriptor(c)))) break;
    }
    if (!cls) inflate_error(rt_, line, "Error inflating class " + name + " (classe absente du pont et de l'application)");
    if (!rt_.is_assignable(cls, rt_.find_class("Landroid/view/View;"))) {
      inflate_error(rt_, line, "Error inflating class " + name + " : ce n'est pas une vue");
    }
    const char* two = "(Landroid/content/Context;Landroid/util/AttributeSet;)V";
    if (cls->declared_method("<init>", two)) return construct(rt_, cls->descriptor, two, {context_, kNull});
    return construct(rt_, cls->descriptor, "(Landroid/content/Context;)V", {context_});
  }

  void apply_attributes(Handle view, const res::XmlParser& p) {
    int32_t width = -2, height = -2;
    float weight = 0;
    bool text_view = instance_of(rt_, view, "Landroid/widget/TextView;");
    for (int32_t i = 0; i < p.attribute_count(); ++i) {
      uint32_t attr = p.attribute_name_resid(i);
      if (attr == kAttrId) {  // the reference itself is the id (@+id/x, @android:id/list)
        rt_.call_virtual(view, "setId", "(I)V", {p.attribute_value(i).data});
        continue;
      }
      if (!handled(attr)) {
        if (attr) log::debug("inflate", "attribut " + hex_id(attr) + " ignoré");
        continue;
      }
      auto resolved = value(p, i);
      if (!resolved) continue;
      const Resolved& v = *resolved;
      switch (attr) {
        case kAttrText:
        case kAttrHint:
          if (text_view) {
            Handle s = rt_.new_string(coerce_to_string(v));
            rt_.call_virtual(view, attr == kAttrText ? "setText" : "setHint", "(Ljava/lang/CharSequence;)V", {s});
          }
          break;
        case kAttrOrientation:
          if (instance_of(rt_, view, "Landroid/widget/LinearLayout;")) {
            rt_.call_virtual(view, "setOrientation", "(I)V", {v.data});
          }
          break;
        case kAttrTextSize:
          if (text_view && v.type == res::kTypeDimension) {
            float sp = dimension_px(host_, v.data) * 160.0f / density_of(host_);
            rt_.call_virtual(view, "setTextSize", "(F)V", {float_bits(sp)});
          }
          break;
        case kAttrTextColor:
          if (text_view && v.type >= 0x1c && v.type <= 0x1f) rt_.call_virtual(view, "setTextColor", "(I)V", {v.data});
          break;
        case kAttrBackground:
          if (v.type >= 0x1c && v.type <= 0x1f) {
            rt_.call_virtual(view, "setBackgroundColor", "(I)V", {v.data});
          } else {
            log::debug("inflate", "android:background ignoré (seules les couleurs unies sont prises en charge)");
          }
          break;
        case kAttrVisibility:
          rt_.call_virtual(view, "setVisibility", "(I)V", {v.data == 2 ? 8u : v.data == 1 ? 4u : 0u});
          break;
        case kAttrEnabled:
          rt_.call_virtual(view, "setEnabled", "(Z)V", {v.data ? 1u : 0u});
          break;
        case kAttrOnClick:
          if (v.string) set_declared_click(view, text::utf16_to_utf8(*v.string));
          break;
        case kAttrLayoutWidth:
        case kAttrLayoutHeight: {
          int32_t px = v.type == res::kTypeDimension ? int32_t(std::lround(dimension_px(host_, v.data))) : int32_t(v.data);
          (attr == kAttrLayoutWidth ? width : height) = px;
          break;
        }
        case kAttrLayoutWeight:
          if (v.type == res::kTypeFloat) std::memcpy(&weight, &v.data, 4);
          else if (is_int(v.type)) weight = float(int32_t(v.data));
          break;
        default:
          break;
      }
    }
    Handle params = construct(rt_, "Landroid/widget/LinearLayout$LayoutParams;", "(IIF)V",
                              {uint32_t(width), uint32_t(height), float_bits(weight)});
    rt_.call_virtual(view, "setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V", {params});
  }

  static bool handled(uint32_t attr) {
    switch (attr) {
      case kAttrEnabled: case kAttrTextSize: case kAttrTextColor: case kAttrOrientation:
      case kAttrBackground: case kAttrVisibility: case kAttrLayoutWidth: case kAttrLayoutHeight:
      case kAttrText: case kAttrHint: case kAttrLayoutWeight: case kAttrOnClick:
        return true;
      default:
        return false;
    }
  }

  void set_declared_click(Handle view, const std::string& method) {
    Handle listener = rt_.alloc(rt_.find_class("Landroid/view/View$DeclaredOnClickListener;"));
    auto peer = std::make_unique<DeclaredClickPeer>();
    peer->method = method;
    peer->view = view;
    rt_.object(listener)->peer = std::move(peer);
    rt_.call_virtual(view, "setOnClickListener", "(Landroid/view/View$OnClickListener;)V", {listener});
  }

  void include(const res::XmlParser& p, Handle parent, int depth) {
    static const std::u16string kLayout = u"layout";
    int32_t i = p.index_of_attribute(nullptr, kLayout);
    res::Value v = i >= 0 ? p.attribute_value(i) : res::Value{};
    if (v.type != res::kTypeReference || v.data == 0) {
      inflate_error(rt_, p.line(), "You must specify a valid layout reference. The layout ID @layout/... is not valid.");
    }
    rt::GlobalRef view(rt_, inflate(v.data, kNull, false, depth + 1));
    int32_t id = p.index_of_resid(kAttrId);
    if (id >= 0) rt_.call_virtual(view.get(), "setId", "(I)V", {uint32_t(p.attribute_value(id).data)});
    rt_.call_virtual(parent, "addView", "(Landroid/view/View;)V", {view.get()});
  }

  Runtime& rt_;
  Host& host_;
  Handle context_;
};

Handle inflate(Runtime& rt, Host& host, Handle context, uint32_t layout, Handle parent, bool attach) {
  return Inflater(rt, host, context).inflate(layout, parent, attach);
}

// --- Registration -------------------------------------------------------------------

void register_resources_class(rt::NativeRegistry& r, Host& host) {
  r.define("Landroid/content/res/Resources$NotFoundException;", "Landroid/util/AndroidRuntimeException;");
  r.define("Landroid/view/InflateException;", "Ljava/lang/RuntimeException;");

  auto res = r.define("Landroid/content/res/Resources;", "Ljava/lang/Object;");
  res.method("getText", "(I)Ljava/lang/CharSequence;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(get_text(rt, host, a.raw(1)));
  }, S::kPartial, "texte sans styles (balises <b>, <i>... ignorées)");
  res.method("getText", "(ILjava/lang/CharSequence;)Ljava/lang/CharSequence;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto v = resolve(host.resources(), a.raw(1));
    return v && v->string ? rt.new_string(*v->string) : a.h(2);
  });
  res.method("getString", "(I)Ljava/lang/String;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(get_text(rt, host, a.raw(1)));
  });
  res.method("getString", "(I[Ljava/lang/Object;)Ljava/lang/String;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(java_format(rt, get_text(rt, host, a.raw(1)), a.h(2)));
  });
  res.method("getColor", "(I)I", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get_color(rt, host, a.raw(1)));
  });
  res.method("getColor", "(ILandroid/content/res/Resources$Theme;)I", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get_color(rt, host, a.raw(1)));
  }, S::kPartial, "thème ignoré");
  res.method("getInteger", "(I)I", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    Resolved v = require(rt, host, a.raw(1), "Integer");
    if (!is_int(v.type)) not_found(rt, "Integer", a.raw(1));
    return rt::ret_int(int32_t(v.data));
  });
  res.method("getBoolean", "(I)Z", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    Resolved v = require(rt, host, a.raw(1), "Boolean");
    if (!is_int(v.type)) not_found(rt, "Boolean", a.raw(1));
    return rt::ret_bool(v.data != 0);
  });
  auto dimension = [&host](Runtime& rt, uint32_t id) {
    Resolved v = require(rt, host, id, "Dimension");
    if (v.type != res::kTypeDimension) not_found(rt, "Dimension", id);
    return dimension_px(host, v.data);
  };
  res.method("getDimension", "(I)F", [dimension](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_float(dimension(rt, a.raw(1)));
  });
  res.method("getDimensionPixelOffset", "(I)I", [dimension](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(dimension(rt, a.raw(1))));
  });
  res.method("getDimensionPixelSize", "(I)I", [dimension](Runtime& rt, NativeArgs& a) -> uint64_t {
    float f = dimension(rt, a.raw(1));
    int32_t px = int32_t(f + 0.5f);
    return rt::ret_int(px != 0 ? px : f == 0 ? 0 : f > 0 ? 1 : -1);
  });
  res.method("getIdentifier", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    const res::AssetManager* am = host.resources();
    if (!am || a.h(1) == kNull) return rt::ret_int(0);
    std::string name = text::utf16_to_utf8(a.str(1));
    std::string type = text::utf16_to_utf8(a.str_or(2, u""));
    std::string package = text::utf16_to_utf8(a.str_or(3, u""));
    return rt::ret_int(int32_t(am->resource_id(name, type, package)));
  });
  auto name_part = [&host](Runtime& rt, uint32_t id, int part) -> Handle {
    const res::AssetManager* am = host.resources();
    auto n = am ? am->resource_name(id) : std::nullopt;
    if (!n) not_found(rt, "Unable to find", id);
    switch (part) {
      case 0: return rt.new_string(n->package + u":" + n->type + u"/" + n->entry);
      case 1: return rt.new_string(n->package);
      case 2: return rt.new_string(n->type);
      default: return rt.new_string(n->entry);
    }
  };
  res.method("getResourceName", "(I)Ljava/lang/String;", [name_part](Runtime& rt, NativeArgs& a) -> uint64_t { return name_part(rt, a.raw(1), 0); });
  res.method("getResourcePackageName", "(I)Ljava/lang/String;", [name_part](Runtime& rt, NativeArgs& a) -> uint64_t { return name_part(rt, a.raw(1), 1); });
  res.method("getResourceTypeName", "(I)Ljava/lang/String;", [name_part](Runtime& rt, NativeArgs& a) -> uint64_t { return name_part(rt, a.raw(1), 2); });
  res.method("getResourceEntryName", "(I)Ljava/lang/String;", [name_part](Runtime& rt, NativeArgs& a) -> uint64_t { return name_part(rt, a.raw(1), 3); });
  res.unsupported("getLayout", "(I)Landroid/content/res/XmlResourceParser;", "XmlPullParser non disponible en mode pont");
  res.unsupported("getDrawable", "(I)Landroid/graphics/drawable/Drawable;", "images (drawables) non prises en charge en mode pont");
  res.unsupported("getStringArray", "(I)[Ljava/lang/String;", "tableaux de ressources non pris en charge en mode pont");
}

void register_layout_inflater(rt::NativeRegistry& r, Host& host) {
  auto li = r.define("Landroid/view/LayoutInflater;", "Ljava/lang/Object;");
  li.static_method("from", "(Landroid/content/Context;)Landroid/view/LayoutInflater;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("LayoutInflater.from(null)");
    return new_inflater(rt, a.h(0));
  });
  auto ctx_of = [](Runtime& rt, Handle h) { return peer_or_throw<InflaterPeer>(rt, h, "LayoutInflater").context; };
  li.method("getContext", "()Landroid/content/Context;", [ctx_of](Runtime& rt, NativeArgs& a) -> uint64_t { return ctx_of(rt, a.h(0)); });
  li.method("cloneInContext", "(Landroid/content/Context;)Landroid/view/LayoutInflater;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return new_inflater(rt, a.h(1));
  });
  li.method("inflate", "(ILandroid/view/ViewGroup;)Landroid/view/View;", [&host, ctx_of](Runtime& rt, NativeArgs& a) -> uint64_t {
    return inflate(rt, host, ctx_of(rt, a.h(0)), a.raw(1), a.h(2), a.h(2) != kNull);
  }, S::kPartial, "vues du pont, attributs courants (texte, id, couleurs, tailles, poids, visibilité, onClick)");
  li.method("inflate", "(ILandroid/view/ViewGroup;Z)Landroid/view/View;", [&host, ctx_of](Runtime& rt, NativeArgs& a) -> uint64_t {
    return inflate(rt, host, ctx_of(rt, a.h(0)), a.raw(1), a.h(2), a.z(3));
  }, S::kPartial, "vues du pont, attributs courants (texte, id, couleurs, tailles, poids, visibilité, onClick)");

  r.define("Landroid/view/View$DeclaredOnClickListener;", "Ljava/lang/Object;", {"Landroid/view/View$OnClickListener;"})
      .method("onClick", "(Landroid/view/View;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
        auto& p = peer_or_throw<DeclaredClickPeer>(rt, a.h(0), "View$DeclaredOnClickListener");
        Handle context = rt.call_virtual(p.view, "getContext", "()Landroid/content/Context;");
        const char* sig = "(Landroid/view/View;)V";
        if (context == kNull || !rt.resolve_virtual(rt.class_of(context), p.method, sig)) {
          rt.throw_new("Ljava/lang/IllegalStateException;",
                       "Could not find method " + p.method + "(View) in a parent or ancestor Context for "
                       "android:onClick attribute defined on view class " + rt.class_of(p.view)->java_name());
        }
        return rt.call_virtual(context, p.method, sig, {a.h(1)});
      });
}

void register_resource_overloads(rt::NativeRegistry& r, Host& host) {
  auto ctx = r.extend("Landroid/content/Context;");
  ctx.method("getResources", "()Landroid/content/res/Resources;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return resources_object(rt);
  });
  ctx.method("getString", "(I)Ljava/lang/String;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(get_text(rt, host, a.raw(1)));
  });
  ctx.method("getString", "(I[Ljava/lang/Object;)Ljava/lang/String;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(java_format(rt, get_text(rt, host, a.raw(1)), a.h(2)));
  });
  ctx.method("getText", "(I)Ljava/lang/CharSequence;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string(get_text(rt, host, a.raw(1)));
  }, S::kPartial, "texte sans styles");
  ctx.method("getColor", "(I)I", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get_color(rt, host, a.raw(1)));
  });
  ctx.method("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = text::utf16_to_utf8(a.str_or(1, u""));
    if (name == "layout_inflater") return new_inflater(rt, a.h(0));
    log::warn("services", "getSystemService(\"" + name + "\") : service non émulé en mode pont, renvoie null");
    return kNull;
  }, S::kPartial, "seul layout_inflater est disponible en mode pont");

  auto act = r.extend("Landroid/app/Activity;");
  act.method("setContentView", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::GlobalRef view(rt, inflate(rt, host, a.h(0), a.raw(1), kNull, false));
    return rt.call_virtual(a.h(0), "setContentView", "(Landroid/view/View;)V", {view.get()});
  });
  act.method("getLayoutInflater", "()Landroid/view/LayoutInflater;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return new_inflater(rt, a.h(0));
  });
  act.method("setTitle", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle title = rt.new_string(get_text(rt, host, a.raw(1)));
    return rt.call_virtual(a.h(0), "setTitle", "(Ljava/lang/CharSequence;)V", {title});
  });

  auto tv = r.extend("Landroid/widget/TextView;");
  tv.method("setText", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle s = rt.new_string(get_text(rt, host, a.raw(1)));
    return rt.call_virtual(a.h(0), "setText", "(Ljava/lang/CharSequence;)V", {s});
  });
  tv.method("setHint", "(I)V", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle s = rt.new_string(get_text(rt, host, a.raw(1)));
    return rt.call_virtual(a.h(0), "setHint", "(Ljava/lang/CharSequence;)V", {s});
  });

  r.extend("Landroid/widget/Toast;")
      .static_method("makeText", "(Landroid/content/Context;II)Landroid/widget/Toast;", [&host](Runtime& rt, NativeArgs& a) -> uint64_t {
        Handle s = rt.new_string(get_text(rt, host, a.raw(1)));
        return rt.call_static("Landroid/widget/Toast;", "makeText",
                              "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;",
                              {a.h(0), s, a.raw(2)});
      });
}

}  // namespace

void register_android_res(rt::NativeRegistry& r, Host& host) {
  register_resources_class(r, host);
  register_layout_inflater(r, host);
  register_resource_overloads(r, host);
}

}  // namespace aa::bridge
