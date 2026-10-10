#include "compat/gaps.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "common/file_io.h"
#include "common/text.h"

namespace aa::compat {

using rt::ApiStatus;

namespace {

struct CapabilityDef {
  const char* name;
  const char* meaning;
  std::vector<const char*> prefixes;
};

const std::vector<CapabilityDef>& capability_defs() {
  static const std::vector<CapabilityDef> defs = {
      {"Code natif (JNI)", "chargeur ELF, couche bionic/POSIX, JNI",
       {"Ljava/lang/System;->loadLibrary", "Ljava/lang/System;->load(", "Ljava/lang/Runtime;->loadLibrary"}},
      {"Binder / AIDL", "IPC entre composants et entre paquets (bindService, Parcel)",
       {"Landroid/os/IBinder;", "Landroid/os/Binder;", "Landroid/os/Parcel;", "Landroid/os/IInterface;",
        "Landroid/content/Context;->bindService", "Landroid/content/ContextWrapper;->bindService",
        "Landroid/content/ServiceConnection;"}},
      {"Threads et concurrence", "threads Java réels, verrous, exécuteurs",
       {"Ljava/lang/Thread;", "Ljava/util/concurrent/", "Landroid/os/HandlerThread;", "Ljava/lang/Object;->wait",
        "Ljava/lang/Object;->notify"}},
      {"Réflexion", "introspection et appels dynamiques",
       {"Ljava/lang/reflect/", "Ljava/lang/Class;->forName", "Ljava/lang/Class;->getDeclared", "Ljava/lang/Class;->getMethod",
        "Ljava/lang/Class;->getField", "Ljava/lang/Class;->getConstructor"}},
      {"API internes ou cachées", "API hors SDK public, sans garantie de stabilité",
       {"Lcom/android/internal/", "Landroid/app/ActivityThread;", "Landroid/os/SystemProperties;",
        "Landroid/os/ServiceManager;", "Ldalvik/system/VMRuntime;", "Llibcore/", "Lsun/misc/Unsafe;"}},
      {"Chargement dynamique de code", "DexClassLoader, modules téléchargés à l'exécution",
       {"Ldalvik/system/DexClassLoader;", "Ldalvik/system/PathClassLoader;", "Ldalvik/system/InMemoryDexClassLoader;",
        "Ldalvik/system/BaseDexClassLoader;", "Ljava/lang/ClassLoader;"}},
      {"Réseau et TLS", "sockets, HTTP, TLS", {"Ljava/net/", "Ljavax/net/", "Landroid/net/", "Lorg/apache/http/"}},
      {"Stockage et bases de données", "fichiers privés, SharedPreferences, SQLite",
       {"Landroid/database/", "Landroid/content/SharedPreferences", "Ljava/io/File", "Ljava/nio/"}},
      {"Comptes et synchronisation", "AccountManager, ContentResolver, fournisseurs de contenu",
       {"Landroid/accounts/", "Landroid/content/ContentResolver;", "Landroid/content/ContentProvider;",
        "Landroid/content/AbstractThreadedSyncAdapter;"}},
      {"Cryptographie et Keystore", "JCA, Android Keystore", {"Ljava/security/", "Ljavax/crypto/", "Landroid/security/"}},
      {"Graphismes avancés et WebView", "Canvas/Bitmap, OpenGL ES, WebView",
       {"Landroid/graphics/", "Landroid/opengl/", "Landroid/webkit/", "Landroid/view/SurfaceView;",
        "Landroid/view/TextureView;"}},
      {"Services système", "getSystemService, JobScheduler, alarmes, notifications, PackageManager",
       {"Landroid/content/Context;->getSystemService", "Landroid/app/job/", "Landroid/app/AlarmManager;",
        "Landroid/app/NotificationManager;", "Landroid/content/pm/"}},
      {"Ressources compilées", "resources.arsc, layouts XML, chaînes localisées",
       {"Landroid/content/res/", "Landroid/view/LayoutInflater;", "Landroid/content/Context;->getString",
        "Landroid/content/Context;->getResources", "Landroid/app/Activity;->setContentView(I)"}},
  };
  return defs;
}

// Platform permissions reserved to system or privileged apps (non exhaustive).
const std::set<std::string>& privileged_permissions() {
  static const std::set<std::string> perms = {
      "INSTALL_PACKAGES", "DELETE_PACKAGES", "READ_PRIVILEGED_PHONE_STATE", "WRITE_SECURE_SETTINGS", "MANAGE_USERS",
      "INTERACT_ACROSS_USERS", "INTERACT_ACROSS_USERS_FULL", "GET_ACCOUNTS_PRIVILEGED", "UPDATE_DEVICE_STATS",
      "ACCESS_CACHE_FILESYSTEM", "REBOOT", "MASTER_CLEAR", "PACKAGE_USAGE_STATS", "CHANGE_COMPONENT_ENABLED_STATE",
      "MODIFY_PHONE_STATE", "READ_LOGS", "DEVICE_POWER", "UPDATE_APP_OPS_STATS", "MANAGE_APP_OPS_MODES",
      "GRANT_RUNTIME_PERMISSIONS", "START_ACTIVITIES_FROM_BACKGROUND", "NETWORK_SETTINGS", "LOCAL_MAC_ADDRESS",
      "BACKUP", "CAPTURE_AUDIO_OUTPUT", "OBSERVE_GRANT_REVOKE_PERMISSIONS", "WRITE_GSERVICES", "BIND_APPWIDGET",
      "MANAGE_DEVICE_ADMINS", "INSTALL_LOCATION_PROVIDER", "SUBSTITUTE_NOTIFICATION_APP_NAME"};
  return perms;
}

std::string java_package(const std::string& owner) {
  std::string j = text::descriptor_to_java(owner);
  size_t dot = j.rfind('.');
  return dot == std::string::npos ? "(défaut)" : j.substr(0, dot);
}

std::string fmt_count(uint64_t n) {
  std::string s = std::to_string(n);
  for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), " ");
  return s;
}

std::string human_size(uint64_t n) {
  char buf[32];
  if (n < (1u << 20)) {
    std::snprintf(buf, sizeof buf, "%.1f Kio", double(n) / 1024);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f Mio", double(n) / (1 << 20));
  }
  return buf;
}

template <typename T>
void sort_by_uses(std::vector<T>& v) {
  std::sort(v.begin(), v.end(), [](const T& a, const T& b) {
    if (a.uses != b.uses) return a.uses > b.uses;
    return a.name < b.name;
  });
}

}  // namespace

GapReport analyze_gaps(const std::vector<const apk::LoadedApk*>& apks, const rt::NativeRegistry& registry) {
  GapReport g;
  struct Agg {
    ApiStatus status = ApiStatus::kSupported;
    std::string note;
    std::string owner;
    uint64_t uses = 0;
  };
  std::map<std::string, Agg> apis;
  uint64_t native_methods = 0, invoke_custom = 0;
  std::set<std::string> abis, package_strings, shared_uids, privileged, signature_perms;
  std::vector<std::string> multi_process;  // "package (n processus : ...)"
  std::vector<std::string> native_samples;
  bool any_native = false;
  size_t components = 0, providers = 0;

  for (const apk::LoadedApk* apk : apks) {
    PackageSummary s;
    s.path = path_utf8(apk->path);
    s.size = apk->file_size;
    s.dex_files = apk->dex.size();
    for (const auto& a : apk->abis) s.abis.push_back(a);
    if (apk->manifest) {
      const apk::Manifest& m = *apk->manifest;
      s.package = m.package;
      s.version = m.version_name + " (" + std::to_string(m.version_code) + ")";
      s.split = m.split;
      if (!m.shared_user_id.empty()) shared_uids.insert(m.shared_user_id + " (" + m.package + ")");
      if (m.split.empty()) {
        auto procs = m.processes();
        if (procs.size() > 1) {
          std::string list;
          for (const auto& p : procs) list += (list.empty() ? "" : ", ") + p;
          multi_process.push_back(m.package + " : " + std::to_string(procs.size()) + " processus (" + list + ")");
        }
      }
      for (const auto& p : m.permissions) {
        std::string short_name = text::starts_with(p, "android.permission.") ? p.substr(19) : "";
        if (privileged_permissions().count(short_name)) privileged.insert(p);
      }
      for (const auto& d : m.declared_permissions) {
        if ((d.protection_level & 0xF) >= 2) signature_perms.insert(d.name + " [" + d.protection_text() + "]");
      }
      components += m.services.size() + m.receivers.size() + m.providers.size();
      providers += m.providers.size();
      if (m.split.empty()) {
        std::string req = m.package + " : minSdk " + std::to_string(m.min_sdk) + ", targetSdk " + std::to_string(m.target_sdk);
        if (m.persistent) req += ", application persistante";
        if (!m.app_process.empty()) req += ", processus principal " + m.app_process;
        if (const apk::MetaData* v = m.find_meta_data("com.google.android.gms.version")) {
          req += ", com.google.android.gms.version=" + v->value;
        }
        g.platform_requirements.push_back(req);
        for (const auto& l : m.required_libraries) g.platform_requirements.push_back(m.package + " requiert la bibliothèque partagée " + l);
      }
    } else {
      s.error = apk->manifest_error;
    }
    if (!apk->native_libs.empty()) any_native = true;
    for (const auto& a : apk->abis) abis.insert(a);

    Report r = evaluate(*apk, registry, Mode::kExec);
    s.methods_with_code = r.code.methods_with_code;
    s.bytecode = r.code.code_units * 2;
    native_methods += r.code.native_methods;
    invoke_custom += r.code.invoke_custom_sites;
    for (const auto& n : r.code.native_method_samples) {
      if (native_samples.size() < 8) native_samples.push_back(n);
    }
    package_strings.insert(r.code.package_strings.begin(), r.code.package_strings.end());
    for (const ApiUse& u : r.apis) {
      Agg& a = apis[u.api];
      a.status = u.status;
      a.note = u.note;
      a.owner = u.owner.empty() ? u.api : u.owner;
      a.uses += u.uses;
    }
    g.packages.push_back(std::move(s));
  }

  std::map<std::string, GapItem> classes, packages;
  for (const auto& [api, a] : apis) {
    g.apis_by_status[a.status]++;
    g.uses_by_status[a.status] += a.uses;
    if (a.status != ApiStatus::kUnsupported && a.status != ApiStatus::kStub) continue;
    g.missing_apis.push_back({api, a.status, a.note, 1, a.uses});
    GapItem& c = classes[a.owner];
    c.name = a.owner;
    c.apis++;
    c.uses += a.uses;
    GapItem& p = packages[java_package(a.owner)];
    p.name = java_package(a.owner);
    p.apis++;
    p.uses += a.uses;
  }
  for (auto& [k, v] : classes) g.missing_classes.push_back(v);
  for (auto& [k, v] : packages) g.missing_packages.push_back(v);
  sort_by_uses(g.missing_apis);
  sort_by_uses(g.missing_classes);
  sort_by_uses(g.missing_packages);

  for (const CapabilityDef& def : capability_defs()) {
    Capability cap;
    cap.name = def.name;
    cap.meaning = def.meaning;
    std::vector<std::pair<uint64_t, std::string>> hits;
    for (const auto& [api, a] : apis) {
      bool match = std::any_of(def.prefixes.begin(), def.prefixes.end(),
                               [&](const char* p) { return text::starts_with(api, p); });
      if (!match) continue;
      cap.apis++;
      cap.uses += a.uses;
      if (a.status == ApiStatus::kUnsupported || a.status == ApiStatus::kStub) cap.missing++;
      hits.emplace_back(a.uses, api);
    }
    std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < hits.size() && i < 3; ++i) cap.evidence.push_back(hits[i].second);
    g.capabilities.push_back(std::move(cap));
  }
  Capability jni;
  jni.name = "Méthodes JNI déclarées";
  jni.meaning = "méthodes Java implémentées en code natif";
  jni.apis = native_methods;
  jni.uses = native_methods;
  jni.missing = native_methods;  // no ELF loader yet
  jni.evidence = native_samples;
  g.capabilities.push_back(jni);
  Capability indy;
  indy.name = "invoke-custom / invoke-polymorphic";
  indy.meaning = "MethodHandle, LambdaMetafactory non désucrés";
  indy.uses = invoke_custom;
  indy.apis = invoke_custom;
  indy.missing = invoke_custom;
  g.capabilities.push_back(indy);

  for (const auto& u : shared_uids) g.platform_requirements.push_back("UID partagé (android:sharedUserId) : " + u);
  for (const auto& mp : multi_process) g.platform_requirements.push_back("plusieurs processus — " + mp);
  for (const auto& p : privileged) g.platform_requirements.push_back("permission réservée aux applications système/privilégiées : " + p);
  if (!signature_perms.empty()) {
    g.platform_requirements.push_back(std::to_string(signature_perms.size()) +
                                      " permission(s) de niveau signature déclarée(s) (ex. " + *signature_perms.begin() + ")");
  }
  if (components) {
    g.platform_requirements.push_back(std::to_string(components) + " composant(s) service/récepteur/fournisseur, dont " +
                                      std::to_string(providers) + " fournisseur(s) de contenu");
  }
  for (const auto& s : package_strings) g.platform_requirements.push_back("dépendance détectée (chaîne constante) : " + s);

  // What AnyAndroid has to build first, derived from the evidence.
  // Only capabilities with APIs AnyAndroid still lacks become prerequisites.
  auto cap_uses = [&](const char* name) {
    for (const auto& c : g.capabilities) {
      if (c.name == name) return c.missing;
    }
    return uint64_t(0);
  };
  if (any_native || native_methods) {
    std::string abi_note = abis.count("x86_64") ? "bibliothèques x86_64 présentes : exécution native possible sans traduction"
                                                : "aucune bibliothèque x86_64 : traduction binaire ARM64 → x64 nécessaire, "
                                                  "ou utiliser la variante x86_64 des APK";
    g.prerequisites.push_back("Couche native : chargeur ELF, traduction bionic → Windows (libc, pthread, mmap, dlopen) et JNI (" +
                              abi_note + ")");
  }
  if (cap_uses("Threads et concurrence")) {
    g.prerequisites.push_back("Concurrence : java.util.concurrent (exécuteurs, verrous, atomiques) au-dessus des threads du runtime");
  }
  if (cap_uses("Binder / AIDL")) {
    g.prerequisites.push_back("IPC : Binder/AIDL en processus (Parcel, IBinder, bindService) et gestionnaire de services");
  }
  if (cap_uses("Réflexion")) g.prerequisites.push_back("Runtime : réflexion java.lang.reflect complète");
  if (cap_uses("Ressources compilées")) g.prerequisites.push_back("Ressources : resources.arsc, layouts et chaînes compilés");
  if (!shared_uids.empty() || !multi_process.empty() || !privileged.empty()) {
    g.prerequisites.push_back("Plateforme : installation « système privilégié », UID partagé et modèle multi-processus");
  }
  if (cap_uses("Comptes et synchronisation")) {
    g.prerequisites.push_back("Services : AccountManager, ContentResolver/ContentProvider, synchronisation");
  }
  uint64_t missing = g.apis_by_status[ApiStatus::kUnsupported];
  if (missing) {
    g.prerequisites.push_back("Framework : " + fmt_count(missing) +
                              " API référencées non implémentées (voir le classement) — plaide pour l'exécution du "
                              "framework AOSP lui-même plutôt qu'une réécriture API par API");
  }
  return g;
}

std::string render_gaps(const GapReport& g, bool md, size_t top) {
  std::string o;
  auto h = [&](const std::string& title) {
    size_t width = 0;
    for (unsigned char ch : title) width += (ch & 0xC0) != 0x80;
    o += md ? "\n## " + title + "\n\n" : "\n" + title + "\n" + std::string(width, '-') + "\n";
  };
  auto label = [](ApiStatus s) { return std::string(rt::api_status_label(s)); };

  o += md ? "# Analyse d'écart AnyAndroid\n\n" : "Analyse d'écart AnyAndroid\n==========================\n";
  o += "> Analyse statique : aucun code des paquets n'a été exécuté.\n";

  h("Paquets analysés");
  if (md) o += "| Paquet | Version | Split | Taille | DEX | Méthodes | Bytecode | ABI natives |\n|---|---|---|---|---|---|---|---|\n";
  for (const auto& p : g.packages) {
    std::string abis;
    for (const auto& a : p.abis) abis += (abis.empty() ? "" : ", ") + a;
    if (abis.empty()) abis = "—";
    std::string name = p.package.empty() ? p.path + " (" + p.error + ")" : p.package;
    if (md) {
      o += "| " + name + " | " + p.version + " | " + (p.split.empty() ? "base" : p.split) + " | " + human_size(p.size) +
           " | " + std::to_string(p.dex_files) + " | " + fmt_count(p.methods_with_code) + " | " + human_size(p.bytecode) +
           " | " + abis + " |\n";
    } else {
      o += "  " + name + "  " + p.version + (p.split.empty() ? "" : "  [split " + p.split + "]") + "  " + human_size(p.size) +
           ", " + std::to_string(p.dex_files) + " DEX, " + fmt_count(p.methods_with_code) + " méthodes, ABI : " + abis + "\n";
    }
  }

  h("Couverture des API de la plateforme");
  uint64_t total_apis = 0, total_uses = 0;
  for (const auto& [s, n] : g.apis_by_status) total_apis += n;
  for (const auto& [s, n] : g.uses_by_status) total_uses += n;
  if (md) o += "| Statut | API distinctes | Appels dans le code |\n|---|---|---|\n";
  for (auto s : {ApiStatus::kSupported, ApiStatus::kPartial, ApiStatus::kStub, ApiStatus::kUnsupported}) {
    auto a = g.apis_by_status.count(s) ? g.apis_by_status.at(s) : 0;
    auto u = g.uses_by_status.count(s) ? g.uses_by_status.at(s) : 0;
    char pct[16];
    std::snprintf(pct, sizeof pct, "%.1f %%", total_uses ? 100.0 * double(u) / double(total_uses) : 0.0);
    if (md) {
      o += "| " + label(s) + " | " + fmt_count(a) + " | " + fmt_count(u) + " (" + pct + ") |\n";
    } else {
      o += "  " + label(s) + " : " + fmt_count(a) + " API, " + fmt_count(u) + " appels (" + pct + ")\n";
    }
  }
  o += (md ? "\n" : "") + std::string("  Total : ") + fmt_count(total_apis) + " API distinctes, " + fmt_count(total_uses) + " appels.\n";

  h("Besoins de la plateforme (manifestes)");
  if (g.platform_requirements.empty()) o += "  (aucun besoin particulier)\n";
  for (const auto& r : g.platform_requirements) o += (md ? "- " : "  - ") + r + "\n";

  h("Profil des capacités requises");
  if (md) o += "| Capacité | Ce que cela demande | API (manquantes) | Appels | Exemples |\n|---|---|---|---|---|\n";
  for (const auto& c : g.capabilities) {
    if (!c.uses && !c.apis) continue;
    std::string ev;
    for (const auto& e : c.evidence) ev += (ev.empty() ? "" : md ? "<br>" : "; ") + (md ? "`" + e + "`" : e);
    if (md) {
      o += "| " + c.name + " | " + c.meaning + " | " + fmt_count(c.apis) + " (" + fmt_count(c.missing) + ") | " +
           fmt_count(c.uses) + " | " + ev + " |\n";
    } else {
      o += "  " + c.name + " : " + fmt_count(c.apis) + " API dont " + fmt_count(c.missing) + " manquantes, " +
           fmt_count(c.uses) + " appels — " + c.meaning + "\n";
      if (!ev.empty()) o += "      ex. " + ev + "\n";
    }
  }

  auto table = [&](const std::string& title, const std::vector<GapItem>& items, bool with_status) {
    h(title);
    if (items.empty()) {
      o += "  (aucune)\n";
      return;
    }
    if (md) o += with_status ? "| API | Statut | Appels | Note |\n|---|---|---|---|\n" : "| Nom | API manquantes | Appels |\n|---|---|---|\n";
    for (size_t i = 0; i < items.size() && i < top; ++i) {
      const GapItem& it = items[i];
      if (md) {
        o += with_status ? "| `" + it.name + "` | " + label(it.status) + " | " + fmt_count(it.uses) + " | " + it.note + " |\n"
                         : "| `" + it.name + "` | " + fmt_count(it.apis) + " | " + fmt_count(it.uses) + " |\n";
      } else {
        o += "  " + std::string(it.status == ApiStatus::kStub ? "○ " : "✗ ") + it.name + "  (" + fmt_count(it.uses) + " appels" +
             (with_status ? "" : ", " + fmt_count(it.apis) + " API") + ")\n";
      }
    }
    if (items.size() > top) o += "  … et " + fmt_count(items.size() - top) + " autres\n";
  };
  table("Paquets Java manquants les plus utilisés", g.missing_packages, false);
  table("Classes manquantes les plus utilisées", g.missing_classes, false);
  table("API manquantes les plus appelées", g.missing_apis, true);

  h("Prérequis à construire, dans l'ordre suggéré");
  int n = 0;
  for (const auto& p : g.prerequisites) o += "  " + std::to_string(++n) + ". " + p + "\n";
  if (g.prerequisites.empty()) o += "  (aucun prérequis supplémentaire détecté)\n";
  return o;
}

}  // namespace aa::compat
