#include "apk-inspector/manifest.h"

#include "common/text.h"

namespace aa::apk {

namespace {

std::string attr_string(const XmlElement& el, std::string_view name) {
  const XmlAttribute* a = el.android_attr(name);
  return a ? a->display() : std::string();
}

IntentFilter parse_filter(const XmlElement& el) {
  IntentFilter f;
  for (const auto& child : el.children) {
    std::string value = attr_string(*child, "name");
    if (child->name == "action") {
      f.actions.push_back(value);
    } else if (child->name == "category") {
      f.categories.push_back(value);
    } else if (child->name == "data") {
      if (const XmlAttribute* s = child->android_attr("scheme")) f.schemes.push_back(s->display());
    }
  }
  return f;
}

Component parse_component(const std::string& package, const XmlElement& el) {
  Component c;
  c.kind = el.name;
  c.name = resolve_class_name(package, attr_string(el, "name"));
  c.label = attr_string(el, "label");
  if (const XmlAttribute* e = el.android_attr("exported")) c.exported = e->as_bool(false);
  if (const XmlAttribute* e = el.android_attr("enabled")) c.enabled = e->as_bool(true);
  c.process = attr_string(el, "process");
  c.permission = attr_string(el, "permission");
  c.authorities = attr_string(el, "authorities");
  if (el.name == "activity-alias") {
    // android:targetActivity is not in our id table; match by name only.
    for (const auto& a : el.attributes) {
      if (a.ns == kAndroidNs && a.name == "targetActivity") c.target_activity = resolve_class_name(package, a.display());
    }
  }
  for (const auto& child : el.children) {
    if (child->name == "intent-filter") c.filters.push_back(parse_filter(*child));
  }
  return c;
}

}  // namespace

bool Component::is_launcher() const {
  for (const auto& f : filters) {
    bool main = false, launcher = false;
    for (const auto& a : f.actions) main |= a == "android.intent.action.MAIN";
    for (const auto& c : f.categories) launcher |= c == "android.intent.category.LAUNCHER";
    if (main && launcher) return true;
  }
  return false;
}

std::string resolve_class_name(const std::string& package, const std::string& name) {
  if (name.empty()) return name;
  if (name[0] == '.') return package + name;
  if (name.find('.') == std::string::npos) return package + "." + name;
  return name;
}

std::optional<std::string> Manifest::launcher_activity() const {
  for (const auto& a : activities) {
    if (!a.enabled || !a.is_launcher()) continue;
    return a.kind == "activity-alias" ? a.target_activity : a.name;
  }
  return std::nullopt;
}

std::string PermissionDecl::protection_text() const {
  static const char* base[] = {"normal", "dangerous", "signature", "signatureOrSystem", "internal"};
  uint32_t b = protection_level & 0xF;
  std::string s = b < 5 ? base[b] : "base " + std::to_string(b);
  struct Flag {
    uint32_t bit;
    const char* name;
  };
  for (Flag f : {Flag{0x10, "privileged"}, Flag{0x20, "development"}, Flag{0x40, "appop"}, Flag{0x80, "pre23"},
                 Flag{0x100, "installer"}, Flag{0x200, "verifier"}, Flag{0x400, "preinstalled"}, Flag{0x800, "setup"}}) {
    if (protection_level & f.bit) s += std::string("|") + f.name;
  }
  return s;
}

std::vector<std::string> Manifest::processes() const {
  std::vector<std::string> out;
  auto add = [&](const std::string& p) {
    std::string name = p.empty() ? (app_process.empty() ? package : app_process) : p;
    if (!name.empty() && name[0] == ':') name = package + name;
    for (const auto& o : out) {
      if (o == name) return;
    }
    out.push_back(name);
  };
  add("");
  for (const auto* list : {&activities, &services, &receivers, &providers}) {
    for (const auto& c : *list) add(c.process);
  }
  return out;
}

const MetaData* Manifest::find_meta_data(const std::string& name) const {
  for (const auto& m : meta_data) {
    if (m.name == name) return &m;
  }
  return nullptr;
}

const Component* Manifest::find_activity(const std::string& name) const {
  for (const auto& a : activities) {
    if (a.name == name && a.kind == "activity") return &a;
  }
  return nullptr;
}

Manifest parse_manifest(const XmlDocument& doc) {
  const XmlElement& root = *doc.root;
  if (root.name != "manifest") throw_format("élément racine <manifest> attendu, trouvé <" + text::printable(root.name) + ">");
  Manifest m;
  if (const XmlAttribute* p = root.plain_attr("package")) m.package = p->display();
  if (m.package.empty()) throw_format("attribut package absent du manifeste");
  if (const XmlAttribute* v = root.android_attr("versionCode")) m.version_code = v->as_int(0);
  m.version_name = attr_string(root, "versionName");
  if (const XmlAttribute* s = root.plain_attr("split")) m.split = s->display();
  if (const XmlAttribute* s = root.android_attr("isSplitRequired")) m.split_required = s->as_bool(false);
  if (const XmlAttribute* s = root.android_attr("compileSdkVersion")) m.compile_sdk = int(s->as_int(0));
  m.shared_user_id = attr_string(root, "sharedUserId");

  for (const auto& child : root.children) {
    const XmlElement& el = *child;
    if (el.name == "uses-sdk") {
      if (const XmlAttribute* a = el.android_attr("minSdkVersion")) m.min_sdk = int(a->as_int(1));
      if (const XmlAttribute* a = el.android_attr("targetSdkVersion")) m.target_sdk = int(a->as_int(0));
      if (const XmlAttribute* a = el.android_attr("maxSdkVersion")) m.max_sdk = int(a->as_int(0));
    } else if (el.name == "uses-permission" || el.name == "uses-permission-sdk-23") {
      std::string name = attr_string(el, "name");
      if (!name.empty()) m.permissions.push_back(name);
    } else if (el.name == "uses-feature") {
      Feature f;
      f.name = attr_string(el, "name");
      if (const XmlAttribute* a = el.android_attr("required")) f.required = a->as_bool(true);
      if (const XmlAttribute* a = el.android_attr("glEsVersion")) f.gl_es_version = uint32_t(a->as_int(0));
      m.features.push_back(f);
    } else if (el.name == "permission") {
      PermissionDecl p;
      p.name = attr_string(el, "name");
      if (const XmlAttribute* a = el.android_attr("protectionLevel")) p.protection_level = uint32_t(a->as_int(0));
      m.declared_permissions.push_back(p);
    } else if (el.name == "queries") {
      for (const auto& q : el.children) {
        if (q->name == "package") m.queried_packages.push_back(attr_string(*q, "name"));
      }
    } else if (el.name == "application") {
      std::string app_class = attr_string(el, "name");
      if (!app_class.empty()) m.application_class = resolve_class_name(m.package, app_class);
      m.app_label = attr_string(el, "label");
      if (const XmlAttribute* a = el.android_attr("debuggable")) m.debuggable = a->as_bool(false);
      if (const XmlAttribute* a = el.android_attr("hasCode")) m.has_code = a->as_bool(true);
      if (const XmlAttribute* a = el.android_attr("persistent")) m.persistent = a->as_bool(false);
      m.app_process = attr_string(el, "process");
      for (const auto& comp : el.children) {
        const std::string& kind = comp->name;
        if (kind == "activity" || kind == "activity-alias") {
          m.activities.push_back(parse_component(m.package, *comp));
        } else if (kind == "service") {
          m.services.push_back(parse_component(m.package, *comp));
        } else if (kind == "receiver") {
          m.receivers.push_back(parse_component(m.package, *comp));
        } else if (kind == "provider") {
          m.providers.push_back(parse_component(m.package, *comp));
        } else if (kind == "uses-library") {
          const XmlAttribute* req = comp->android_attr("required");
          if (!req || req->as_bool(true)) {
            m.required_libraries.push_back(attr_string(*comp, "name"));
          } else {
            m.optional_libraries.push_back(attr_string(*comp, "name"));
          }
        } else if (kind == "meta-data") {
          MetaData md;
          md.name = attr_string(*comp, "name");
          md.value = attr_string(*comp, "value");
          if (md.value.empty()) md.value = attr_string(*comp, "resource");
          m.meta_data.push_back(md);
        }
      }
    }
  }
  if (m.target_sdk == 0) m.target_sdk = m.min_sdk;
  return m;
}

}  // namespace aa::apk
