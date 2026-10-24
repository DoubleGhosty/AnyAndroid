#include "compat/compat.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "common/file_io.h"
#include "common/json.h"
#include "common/text.h"

namespace aa::compat {

using rt::ApiStatus;

namespace {

bool feature_supported(const std::string& name) {
  static const std::set<std::string> ok = {"android.hardware.touchscreen", "android.hardware.faketouch",
                                           "android.hardware.screen.portrait", "android.hardware.screen.landscape"};
  return ok.count(name) > 0;
}

std::string permission_note(const std::string& p) {
  // The verdict is the bridge mode's (`run`): the real libcore's sockets
  // (`exec --bootclasspath`) are not reachable from it.
  if (p == "android.permission.INTERNET") return "réseau absent du mode pont ; seule la vraie libcore (exec --bootclasspath) a des sockets";
  if (p == "android.permission.ACCESS_NETWORK_STATE") return "aucun service connectivity";
  if (p.find("LOCATION") != std::string::npos) return "localisation non disponible";
  if (p.find("CAMERA") != std::string::npos) return "caméra non disponible";
  if (p.find("STORAGE") != std::string::npos) return "stockage partagé non disponible";
  return "aucun service correspondant";
}

std::string human_size(uint64_t n) {
  char buf[32];
  if (n < 1024) {
    std::snprintf(buf, sizeof buf, "%llu o", static_cast<unsigned long long>(n));
  } else if (n < (1u << 20)) {
    std::snprintf(buf, sizeof buf, "%.1f Kio", double(n) / 1024);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f Mio", double(n) / (1 << 20));
  }
  return buf;
}

const char* status_symbol(ApiStatus s) {
  switch (s) {
    case ApiStatus::kSupported: return "✓";
    case ApiStatus::kPartial: return "~";
    case ApiStatus::kStub: return "○";
    case ApiStatus::kUnsupported: return "✗";
    case ApiStatus::kInvestigate: return "?";
  }
  return "?";
}

}  // namespace

const char* verdict_label(Verdict v) {
  switch (v) {
    case Verdict::kCompatible: return "COMPATIBLE";
    case Verdict::kPartial: return "PARTIEL";
    case Verdict::kIncompatible: return "INCOMPATIBLE";
  }
  return "?";
}

const char* severity_label(Severity s) {
  switch (s) {
    case Severity::kInfo: return "INFO";
    case Severity::kWarning: return "AVERTISSEMENT";
    case Severity::kBlocking: return "BLOQUANT";
  }
  return "?";
}

Report evaluate(const apk::LoadedApk& apk, const rt::NativeRegistry& registry, Mode mode) {
  Report rep;
  auto add = [&](Severity s, const char* code, const std::string& msg) { rep.findings.push_back({s, code, msg}); };

  for (const auto& w : apk.warnings) add(Severity::kWarning, "ARCHIVE", w);

  if (!apk.manifest) add(Severity::kBlocking, "MANIFEST", "manifeste illisible : " + apk.manifest_error);

  if (apk.dex.empty()) add(Severity::kBlocking, "NO_CODE", "aucun fichier classes.dex : rien à exécuter");
  for (const auto& part : apk.dex) {
    if (!part.file) {
      add(Severity::kBlocking, "DEX_INVALID", part.entry + " invalide : " + part.error);
      continue;
    }
    if (!part.file->checksum_valid()) add(Severity::kBlocking, "DEX_CHECKSUM", part.entry + " : somme de contrôle Adler-32 invalide");
    if (!part.file->signature_valid()) add(Severity::kWarning, "DEX_SIGNATURE", part.entry + " : empreinte SHA-1 invalide");
  }

  if (!apk.native_libs.empty()) {
    std::string abis;
    for (const auto& a : apk.abis) abis += (abis.empty() ? "" : ", ") + a;
    add(Severity::kBlocking, "NATIVE_LIBS",
        std::to_string(apk.native_libs.size()) + " bibliothèque(s) native(s) ELF (" + abis +
            ") : le code natif n'est pas pris en charge par le MVP (aucune bibliothèque ne sera chargée)");
  }

  rep.code = apk::analyze_code(apk);

  if (apk.manifest) {
    const apk::Manifest& m = *apk.manifest;
    if (mode == Mode::kApp) {
      if (auto launcher = m.launcher_activity()) {
        rep.launch_activity = *launcher;
        if (!rep.code.app_classes.count(text::java_to_descriptor(*launcher))) {
          add(Severity::kBlocking, "LAUNCHER_MISSING", "classe de l'activité principale absente du code : " + *launcher);
        }
      } else {
        add(Severity::kBlocking, "NO_LAUNCHER", "aucune activité MAIN/LAUNCHER déclarée");
      }
    }
    for (const auto& f : m.features) {
      if (f.gl_es_version && f.name.empty()) {
        if (f.required) {
          char buf[96];
          std::snprintf(buf, sizeof buf, "OpenGL ES %u.%u requis : rendu GPU non pris en charge", f.gl_es_version >> 16,
                        f.gl_es_version & 0xFFFF);
          add(Severity::kWarning, "GLES", buf);
        }
      } else if (feature_supported(f.name)) {
        add(Severity::kInfo, "FEATURE", f.name + " : émulé (souris/clavier)");
      } else if (f.required) {
        add(Severity::kBlocking, "FEATURE", "fonctionnalité matérielle requise non disponible : " + f.name);
      } else {
        add(Severity::kInfo, "FEATURE", "fonctionnalité optionnelle non disponible : " + f.name);
      }
    }
    for (const auto& p : m.permissions) add(Severity::kWarning, "PERMISSION", p + " non accordée (" + permission_note(p) + ")");
    size_t components = m.services.size() + m.receivers.size() + m.providers.size();
    if (components) {
      add(Severity::kWarning, "COMPONENTS",
          std::to_string(m.services.size()) + " service(s), " + std::to_string(m.receivers.size()) + " récepteur(s), " +
              std::to_string(m.providers.size()) + " fournisseur(s) déclarés : jamais démarrés par le prototype");
    }
    for (const auto& l : m.required_libraries) add(Severity::kWarning, "USES_LIBRARY", "bibliothèque partagée requise non fournie : " + l);
    if (!m.application_class.empty()) add(Severity::kInfo, "APPLICATION", "classe Application personnalisée : " + m.application_class);
    if (m.debuggable) add(Severity::kInfo, "DEBUGGABLE", "application marquée debuggable");
  }
  if (!apk.signed_v1 && !apk.signed_v2plus) add(Severity::kInfo, "UNSIGNED", "APK non signé (normal pour un APK de test)");

  // Platform API references.
  std::set<std::string> owners_with_members;
  for (const auto& ref : rep.code.refs) {
    owners_with_members.insert(ref.owner);
    ApiUse u;
    u.api = ref.owner + "->" + ref.name + (ref.is_field ? ":" + ref.signature : ref.signature);
    u.uses = ref.uses;
    u.owner = ref.owner;
    auto lookup = ref.is_field ? registry.lookup_field(ref.owner, ref.name, ref.signature)
                               : registry.lookup_method(ref.owner, ref.name, ref.signature);
    if (!lookup.class_known) {
      u.status = ApiStatus::kUnsupported;
      u.note = "classe non implémentée";
    } else if (lookup.method) {
      u.status = lookup.method->status;
      u.note = lookup.method->note;
    } else if (lookup.field) {
      u.status = lookup.field->status;
      u.note = lookup.field->note;
    } else {
      u.status = ApiStatus::kUnsupported;
      u.note = ref.is_field ? "champ non implémenté" : "méthode non implémentée";
    }
    rep.apis.push_back(std::move(u));
  }
  for (const auto& c : rep.code.external_classes) {
    if (owners_with_members.count(c) || registry.contains(c)) continue;
    rep.apis.push_back({c, ApiStatus::kUnsupported, "classe non implémentée (utilisée comme type)", true, 0, c});
  }
  std::sort(rep.apis.begin(), rep.apis.end(), [](const ApiUse& a, const ApiUse& b) { return a.api < b.api; });
  for (const auto& u : rep.apis) rep.counts[u.status]++;

  if (int n = rep.counts[ApiStatus::kUnsupported]) {
    add(Severity::kWarning, "UNSUPPORTED_APIS",
        std::to_string(n) + " référence(s) à des API non implémentées : l'application échouera explicitement si elle les appelle");
  }
  if (int n = rep.counts[ApiStatus::kStub]) {
    add(Severity::kInfo, "STUB_APIS", std::to_string(n) + " API simulée(s) sans effet");
  }

  bool blocking = false, warning = false;
  for (const auto& f : rep.findings) {
    blocking |= f.severity == Severity::kBlocking;
    warning |= f.severity == Severity::kWarning;
  }
  rep.verdict = blocking ? Verdict::kIncompatible : (warning ? Verdict::kPartial : Verdict::kCompatible);
  return rep;
}

std::string render_text(const apk::LoadedApk& apk, const Report& rep, const RenderOptions& opt) {
  std::string o;
  auto line = [&](const std::string& label, const std::string& value) {
    std::string l = label;
    size_t width = 0;
    for (unsigned char c : l) width += (c & 0xC0) != 0x80;  // UTF-8 aware padding
    while (width++ < 22) l += ' ';
    o += l + ": " + value + "\n";
  };
  line("APK", path_utf8(apk.path));
  line("SHA-256", apk.sha256);
  line("Taille", human_size(apk.file_size) + ", " + std::to_string(apk.zip->entries().size()) + " entrées");
  if (apk.manifest) {
    const apk::Manifest& m = *apk.manifest;
    line("Paquet", m.package + " (version " + (m.version_name.empty() ? "?" : m.version_name) + ", code " +
                       std::to_string(m.version_code) + ")");
    line("Libellé", m.app_label.empty() ? "(aucun)" : m.app_label);
    line("SDK", "min " + std::to_string(m.min_sdk) + ", cible " + std::to_string(m.target_sdk));
    line("Activité principale", rep.launch_activity.empty() ? "(aucune)" : rep.launch_activity);
    line("Composants", std::to_string(m.activities.size()) + " activité(s), " + std::to_string(m.services.size()) +
                           " service(s), " + std::to_string(m.receivers.size()) + " récepteur(s), " +
                           std::to_string(m.providers.size()) + " fournisseur(s)");
    std::string perms;
    for (const auto& p : m.permissions) perms += (perms.empty() ? "" : ", ") + p;
    line("Permissions", perms.empty() ? "aucune" : perms);
  } else {
    line("Manifeste", "ILLISIBLE — " + apk.manifest_error);
  }
  for (const auto& part : apk.dex) {
    if (part.file) {
      const auto& h = part.file->header();
      line("Code DEX", part.entry + " (DEX " + h.version + ", " + std::to_string(h.class_defs_size) + " classe(s), " +
                           std::to_string(h.method_ids_size) + " référence(s) de méthode, somme de contrôle " +
                           (part.file->checksum_valid() ? "OK" : "INVALIDE") + ")");
    } else {
      line("Code DEX", part.entry + " INVALIDE — " + part.error);
    }
  }
  if (apk.dex.empty()) line("Code DEX", "aucun");
  line("Code exécutable", std::to_string(rep.code.methods_with_code) + " méthode(s), " +
                              std::to_string(rep.code.code_units * 2) + " octets de bytecode");
  if (apk.native_libs.empty()) {
    line("Bibliothèques natives", "aucune");
  } else {
    for (const auto& lib : apk.native_libs) {
      line("Bibliothèque native", lib.path + " — " +
                                      (lib.elf.valid ? "ELF " + std::to_string(lib.elf.bits) + " bits " + lib.elf.machine +
                                                           (lib.abi_matches ? "" : " (ABI incohérente)")
                                                     : "en-tête invalide : " + lib.elf.error));
    }
  }
  line("Ressources", std::string(apk.has_resources_arsc ? "resources.arsc présent" : "pas de resources.arsc") + ", " +
                         std::to_string(apk.resource_entries) + " fichier(s) res/, " + std::to_string(apk.asset_entries) +
                         " fichier(s) assets/");
  line("Signature", apk.signed_v2plus ? "bloc de signature v2+ présent (non vérifié)"
                    : apk.signed_v1   ? "signature v1 (JAR) présente (non vérifiée)"
                                      : "absente");

  auto count = [&](ApiStatus s) {
    auto it = rep.counts.find(s);
    return it == rep.counts.end() ? 0 : it->second;
  };
  o += "\nAPI de la plateforme référencées : " + std::to_string(rep.apis.size()) + "\n";
  o += "  ✓ supportées " + std::to_string(count(ApiStatus::kSupported)) + "   ~ partielles " +
       std::to_string(count(ApiStatus::kPartial)) + "   ○ stubs " + std::to_string(count(ApiStatus::kStub)) +
       "   ✗ non supportées " + std::to_string(count(ApiStatus::kUnsupported)) + "\n";
  for (const auto& u : rep.apis) {
    if (!opt.all_apis && (u.status == ApiStatus::kSupported)) continue;
    o += std::string("  ") + status_symbol(u.status) + " " + u.api + (u.note.empty() ? "" : "  — " + u.note) + "\n";
  }

  if (opt.classes) {
    o += "\nClasses de l'application (" + std::to_string(rep.code.app_classes.size()) + ") :\n";
    for (const auto& c : rep.code.app_classes) o += "  " + text::descriptor_to_java(c) + "\n";
  }
  if (opt.manifest_xml && !apk.manifest_xml.empty()) o += "\nAndroidManifest.xml (décodé) :\n" + apk.manifest_xml;

  o += "\nConstats :\n";
  if (rep.findings.empty()) o += "  (aucun)\n";
  for (const auto& f : rep.findings) o += std::string("  [") + severity_label(f.severity) + "] " + f.message + "\n";
  o += std::string("\nVerdict : ") + verdict_label(rep.verdict);
  if (rep.verdict == Verdict::kCompatible) o += " (dans le périmètre testé du prototype)";
  if (rep.verdict == Verdict::kPartial) o += " (lancement possible, échecs explicites probables)";
  o += "\n";
  return o;
}

std::string render_json(const apk::LoadedApk& apk, const Report& rep) {
  JsonWriter w;
  w.begin_object();
  w.field("schema", "anyandroid.inspect/1");
  w.field("path", path_utf8(apk.path));
  w.field("sha256", apk.sha256);
  w.field("size", apk.file_size);
  w.field("entries", uint64_t(apk.zip->entries().size()));
  w.key("manifest");
  if (apk.manifest) {
    const apk::Manifest& m = *apk.manifest;
    w.begin_object();
    w.field("package", m.package).field("versionCode", int64_t(m.version_code)).field("versionName", m.version_name);
    w.field("label", m.app_label).field("minSdk", m.min_sdk).field("targetSdk", m.target_sdk);
    w.field("launcher", rep.launch_activity);
    w.key("permissions").begin_array();
    for (const auto& p : m.permissions) w.value(p);
    w.end_array();
    w.key("activities").begin_array();
    for (const auto& a : m.activities) w.value(a.name);
    w.end_array();
    w.field("services", uint64_t(m.services.size()));
    w.field("receivers", uint64_t(m.receivers.size()));
    w.field("providers", uint64_t(m.providers.size()));
    w.end_object();
  } else {
    w.begin_object().field("error", apk.manifest_error).end_object();
  }
  w.key("dex").begin_array();
  for (const auto& part : apk.dex) {
    w.begin_object().field("entry", part.entry).field("size", part.size);
    if (part.file) {
      w.field("version", part.file->header().version);
      w.field("classes", part.file->num_class_defs());
      w.field("methodRefs", part.file->num_methods());
      w.field("checksumValid", part.file->checksum_valid());
      w.field("signatureValid", part.file->signature_valid());
    } else {
      w.field("error", part.error);
    }
    w.end_object();
  }
  w.end_array();
  w.key("nativeLibraries").begin_array();
  for (const auto& lib : apk.native_libs) {
    w.begin_object().field("path", lib.path).field("abi", lib.abi).field("valid", lib.elf.valid);
    w.field("machine", lib.elf.machine).field("bits", lib.elf.bits).field("abiMatches", lib.abi_matches).end_object();
  }
  w.end_array();
  w.field("resourcesArsc", apk.has_resources_arsc);
  w.field("signed", apk.signed_v1 || apk.signed_v2plus);
  w.key("apis").begin_array();
  for (const auto& u : rep.apis) {
    w.begin_object().field("api", u.api).field("status", rt::api_status_id(u.status));
    if (!u.note.empty()) w.field("note", u.note);
    w.end_object();
  }
  w.end_array();
  w.key("findings").begin_array();
  for (const auto& f : rep.findings) {
    const char* sev = f.severity == Severity::kBlocking ? "blocking" : f.severity == Severity::kWarning ? "warning" : "info";
    w.begin_object().field("severity", sev).field("code", f.code).field("message", f.message).end_object();
  }
  w.end_array();
  const char* verdict = rep.verdict == Verdict::kCompatible ? "compatible" : rep.verdict == Verdict::kPartial ? "partial" : "incompatible";
  w.field("verdict", verdict);
  w.end_object();
  return w.str() + "\n";
}

std::string render_api_matrix(const rt::NativeRegistry& registry) {
  std::map<ApiStatus, int> totals;
  for (const auto& [desc, spec] : registry.classes()) {
    for (const auto& m : spec->methods) totals[m.status]++;
    for (const auto& f : spec->static_fields) totals[f.status]++;
  }
  std::string o;
  o += "# Matrice de compatibilité des API\n\n";
  o += "> Fichier généré par `anyandroid compat --markdown` à partir du registre du pont : ne pas modifier à la main.\n";
  o += "> Une API absente de ce tableau n'est **pas** implémentée : tout appel échoue avec une erreur explicite.\n\n";
  o += "| Statut | Signification | Membres |\n|---|---|---|\n";
  struct Row {
    ApiStatus s;
    const char* meaning;
  };
  for (Row r : {Row{ApiStatus::kSupported, "comportement testé et documenté pour le périmètre annoncé"},
                Row{ApiStatus::kPartial, "chemin courant pris en charge, limites connues"},
                Row{ApiStatus::kStub, "API reconnue mais sans effet réel"},
                Row{ApiStatus::kUnsupported, "reconnue mais refusée avec une erreur explicite"}}) {
    o += std::string("| ") + rt::api_status_label(r.s) + " | " + r.meaning + " | " + std::to_string(totals[r.s]) + " |\n";
  }
  o += "\n";
  for (const auto& [desc, spec] : registry.classes()) {
    o += "## `" + text::descriptor_to_java(desc) + "`";
    if ((spec->access & 0x200) != 0) o += " (interface)";
    o += "\n\n";
    if (!spec->note.empty() || spec->status != ApiStatus::kSupported) {
      o += std::string("Classe : **") + rt::api_status_label(spec->status) + "**" + (spec->note.empty() ? "" : " — " + spec->note) + "\n\n";
    }
    if (spec->methods.empty() && spec->static_fields.empty()) {
      o += "_Aucun membre propre (type déclaré pour la résolution des classes)._\n\n";
      continue;
    }
    o += "| Membre | Statut | Note |\n|---|---|---|\n";
    for (const auto& f : spec->static_fields) {
      o += "| `" + f.name + ":" + f.type + "` (statique) | " + rt::api_status_label(f.status) + " | " + f.note + " |\n";
    }
    for (const auto& m : spec->methods) {
      std::string kind = (m.access & 0x8) ? " (statique)" : (!m.fn ? " (abstraite)" : "");
      o += "| `" + m.name + m.signature + "`" + kind + " | " + rt::api_status_label(m.status) + " | " + m.note + " |\n";
    }
    o += "\n";
  }
  return o;
}

}  // namespace aa::compat
