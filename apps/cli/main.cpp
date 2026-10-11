// anyandroid — command-line front end of the prototype.
//
//   anyandroid inspect <apk>   static analysis, never executes code
//   anyandroid run <apk>       launches the main activity in a window
//   anyandroid exec <apk|dex> <classe>   runs a console program (main)
//   anyandroid compat          prints the API compatibility matrix

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <io.h>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "apk-inspector/apk.h"
#include "common/bytes.h"
#include "common/file_io.h"
#include "common/log.h"
#include "common/text.h"
#include "compat/compat.h"
#include "compat/gaps.h"
#include "dex-engine/opcodes.h"
#include "dex-engine/runtime.h"
#include "framework-bridge/bridge.h"
#include "headless/headless_backend.h"
#include "image/aosp.h"
#include "image/filesystem.h"
#include "resources/asset_manager.h"
#include "vfs/vfs.h"
#include "lifecycle/app_host.h"
#include "windows/windowing/win32_backend.h"

#include <windows.h>

namespace fs = std::filesystem;
using namespace aa;

namespace {

// Exit codes (documented in README and `anyandroid help`).
constexpr int kOk = 0;
constexpr int kAppError = 1;     // the app crashed or the program failed
constexpr int kUsage = 2;        // bad command line or unreadable input
constexpr int kNotImplemented = 3;
constexpr int kRefused = 4;      // incompatible APK, launch refused
constexpr int kDeclined = 5;     // the user did not confirm the launch

void out(const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

void err(const std::string& s) {
  std::string line = "anyandroid : " + s + "\n";
  std::fwrite(line.data(), 1, line.size(), stderr);
}

void usage() {
  out(
      "AnyAndroid for Windows " AA_VERSION " — prototype de recherche\n"
      "Exécute des applications Android (DEX) comme des processus Windows, sans machine virtuelle.\n"
      "\n"
      "Commandes :\n"
      "  inspect <fichier.apk> [--json] [--all-apis] [--manifest] [--classes] [--disasm <classe|all>]\n"
      "      Analyse statique : manifeste, DEX, bibliothèques natives, API utilisées, verdict.\n"
      "      Aucun code de l'APK n'est exécuté.\n"
      "  run <fichier.apk> [--yes] [--force] [--activity <classe>] [--headless] [--script <fichier>]\n"
      "                    [--do \"<commandes>\"] [--max-instructions <n>] [--locale <bcp47>]\n"
      "      Lance l'activité principale dans une fenêtre Windows (ou sans fenêtre avec --headless).\n"
      "      Une confirmation est demandée avant d'exécuter le code d'un APK (sauf --yes).\n"
      "      Les ressources (resources.arsc) suivent la langue de Windows, ou --locale.\n"
      "  exec [--bootclasspath <dossier>] <fichier.apk|.dex> <classe> [arguments...]\n"
      "      Exécute la méthode statique main(String[]) d'une classe (programme console).\n"
      "      --bootclasspath : utilise la libcore d'AOSP extraite par `image bootclasspath` (G2) ;\n"
      "      --libcore-only : n'en garde que les modules ART et i18n (sans framework.jar).\n"
      "      --system-image <system.img> : sert /system et /apex depuis l'image (ADR-005) ;\n"
      "      --data-dir <dossier> : bac à sable des zones inscriptibles (/data, /sdcard) ;\n"
      "      --profile : à la fin, les méthodes les plus coûteuses (instructions, appels) sur stderr.\n"
      "  libcore natives --bootclasspath <dossier> [--libcore-only] [--all] [--core]\n"
      "      Couverture des méthodes natives de la libcore par AnyAndroid ; --core liste les membres\n"
      "      des classes du cœur de la VM référencés mais absents.\n"
      "  compat [--markdown]\n"
      "      Affiche la matrice des API Android/Java prises en charge.\n"
      "  gaps <fichier.apk|.apks|.xapk>... [--markdown] [--top <n>] [--output <fichier>]\n"
      "      Analyse d'écart sur des APK réels (ex. GmsCore, GSF, Play Store) : API manquantes classées\n"
      "      par nombre d'appels, capacités requises (natif, Binder, threads...), prérequis.\n"
      "  image info <image>                       format, partitions et propriétés d'une image système\n"
      "  image ls <image> [chemin]                liste un répertoire de l'image\n"
      "  image cat <image> <chemin> --output <f>  extrait un fichier de l'image\n"
      "  image bootclasspath <image> --output <dossier> [--partition <nom>]\n"
      "      Extrait la libcore d'AOSP (chemin de classes d'amorçage, données ICU et fuseaux horaires)\n"
      "      d'une image système : system.img brute ou sparse, super.img, ext4 ou EROFS.\n"
      "  resources <framework-res.apk> [<app.apk>...] [--get <id|nom>] [--locale <bcp47>] [--density <dpi>]\n"
      "            [--locales]\n"
      "      Lit les ressources compilées (resources.arsc) comme AssetManager2 : paquets, valeur\n"
      "      choisie pour une configuration, styles avec leurs parents.\n"
      "  version | help\n"
      "\n"
      "Options globales :\n"
      "  --log-level <trace|debug|info|warn|error|off>   (défaut : warn ; info pour run)\n"
      "  --log-file <fichier.jsonl>                       journal structuré JSONL\n"
      "  --trace                                          trace chaque appel au pont framework\n"
      "\n"
      "Codes de sortie : 0 succès, 1 erreur de l'application, 2 usage ou fichier illisible,\n"
      "  3 non implémenté, 4 APK incompatible (lancement refusé), 5 lancement non confirmé.\n"
      "\n"
      "Commandes prévues (phase 5, non implémentées) : install, list, launch, stop, logs, uninstall.\n");
}

struct Args {
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;  // "--x" -> value ("" for flags)
  bool has(const std::string& k) const { return options.count(k) > 0; }
  std::string get(const std::string& k, const std::string& def = {}) const {
    auto it = options.find(k);
    return it == options.end() ? def : it->second;
  }
};

bool parse_args(const std::vector<std::string>& argv, Args& a) {
  static const std::set<std::string> with_value = {"--log-level", "--log-file", "--disasm", "--activity",
                                                   "--script",    "--do",       "--max-instructions", "--top", "--output",
                                                   "--partition", "--bootclasspath", "--system-image", "--data-dir",
                                                   "--get",       "--locale",   "--density"};
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& s = argv[i];
    if (a.command.empty() && !text::starts_with(s, "--")) {
      a.command = s;
    } else if (text::starts_with(s, "--") && a.command != "exec") {
      if (with_value.count(s)) {
        if (i + 1 >= argv.size()) {
          err("valeur manquante après " + s);
          return false;
        }
        a.options[s] = argv[++i];
      } else {
        a.options[s] = "";
      }
    } else if (a.command == "exec" && a.positional.size() < 2 && text::starts_with(s, "--")) {
      // exec: options before the class name; everything after belongs to main().
      if (with_value.count(s) && i + 1 < argv.size()) {
        a.options[s] = argv[++i];
      } else {
        a.options[s] = "";
      }
    } else {
      a.positional.push_back(s);
    }
  }
  return true;
}

bool setup_logging(const Args& a, log::Level fallback) {
  log::Level level = fallback;
  if (a.has("--trace")) level = log::Level::Trace;
  if (a.has("--log-level") && !log::parse_level(a.get("--log-level"), level)) {
    err("niveau de journal inconnu : " + a.get("--log-level"));
    return false;
  }
  log::set_level(level);
  if (a.has("--log-file") && !log::open_jsonl(path_from_utf8(a.get("--log-file")))) {
    err("impossible d'ouvrir le journal " + a.get("--log-file"));
    return false;
  }
  return true;
}

std::unique_ptr<apk::LoadedApk> load(const std::string& path) {
  try {
    return apk::load_apk(path_from_utf8(path));
  } catch (const std::exception& e) {
    err("APK illisible : " + std::string(e.what()));
    return nullptr;
  }
}

// A phone-like configuration (AnyAndroid's default window).
res::ResConfig device_config(const std::string& locale, uint16_t density) {
  res::ResConfig config;
  config.set_bcp47_locale(locale);
  config.density = density;
  config.sdk_version = 37;
  config.screen_width_dp = 411;
  config.screen_height_dp = 914;
  config.smallest_screen_width_dp = 411;
  config.orientation = 1;
  config.touchscreen = 3;
  config.screen_layout = 0x42;  // normal, left to right
  config.ui_mode = 0x11;        // normal, not night
  return config;
}

// Language of the Windows user ("fr-FR"), as Android would take it from the
// device settings.
std::string user_locale() {
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) <= 0) return "en-US";
  return text::from_wide(name);
}

// Resources of an APK for `run`: the bridge widgets use dp as Windows units,
// the backends scale them to the monitor (hence mdpi).
std::shared_ptr<res::AssetManager> app_resources(const apk::LoadedApk& apk, const std::string& locale) {
  if (!apk.has_resources_arsc) return nullptr;
  auto bytes = read_file(apk.path, 1ull << 30);
  auto assets = res::ApkAssets::load_apk(path_utf8(apk.path), std::move(bytes), 0);
  auto am = std::make_shared<res::AssetManager>();
  am->set_apk_assets({assets});
  am->set_configurations({device_config(locale, res::ResConfig::kDensityMedium)});
  return am;
}

// Registry with every native class, for static evaluation only.
struct StaticRegistry {
  bridge::NullHost host;
  rt::NativeRegistry registry;
  StaticRegistry() {
    rt::register_core_classes(registry);
    bridge::install(registry, host);
  }
};

std::string disassemble_classes(const apk::LoadedApk& apk, const std::string& which) {
  std::string o;
  for (const auto& part : apk.dex) {
    if (!part.file) continue;
    const dex::DexFile& d = *part.file;
    for (uint32_t c = 0; c < d.num_class_defs(); ++c) {
      const dex::ClassDef& def = d.class_def(c);
      std::string desc = d.type_descriptor(def.class_idx);
      if (which != "all" && desc != which && text::descriptor_to_java(desc) != which) continue;
      o += "classe " + text::descriptor_to_java(desc);
      if (def.superclass_idx != dex::kNoIndex) o += " extends " + text::descriptor_to_java(d.type_descriptor(def.superclass_idx));
      o += "  [" + part.entry + "]\n";
      dex::ClassData cd = d.class_data(def);
      for (const auto* list : {&cd.direct_methods, &cd.virtual_methods}) {
        for (const auto& m : *list) {
          o += "  méthode " + d.pretty_method(m.method_idx) + " (accès " + hex(m.access_flags) + ")\n";
          if (!m.code_off) continue;
          try {
            dex::CodeItem code = d.code_item(m.code_off);
            std::span<const uint16_t> insns(code.insns);
            o += "    .registers " + std::to_string(code.registers_size) + "\n";
            for (uint32_t pc = 0; pc < insns.size();) {
              if (uint32_t pu = dex::payload_units(insns, pc)) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "    %04x: <données, %u unités>\n", pc, pu);
                o += buf;
                pc += pu;
                continue;
              }
              dex::Decoded dec = dex::decode(insns, pc);
              char buf[16];
              std::snprintf(buf, sizeof buf, "    %04x: ", pc);
              o += buf + dex::disassemble(&d, dec, pc) + "\n";
              pc += dec.units;
            }
            for (const auto& t : code.tries) {
              char buf[64];
              std::snprintf(buf, sizeof buf, "    try %04x..%04x -> gestionnaire #%u\n", t.start_addr,
                            t.start_addr + t.insn_count, t.handler);
              o += buf;
            }
          } catch (const FormatError& e) {
            o += std::string("    <code invalide : ") + e.what() + ">\n";
          }
        }
      }
    }
  }
  if (o.empty()) o = "aucune classe correspondante : " + which + "\n";
  return o;
}

std::vector<std::unique_ptr<apk::LoadedApk>> load_all(const std::vector<std::string>& paths) {
  std::vector<std::unique_ptr<apk::LoadedApk>> out;
  for (const auto& p : paths) {
    try {
      for (auto& apk : apk::load_package(path_from_utf8(p))) out.push_back(std::move(apk));
    } catch (const std::exception& e) {
      err("APK illisible : " + p + " : " + e.what());
      return {};
    }
  }
  return out;
}

int cmd_inspect(const Args& a) {
  if (a.positional.empty()) {
    usage();
    return kUsage;
  }
  auto apks = load_all(a.positional);
  if (apks.empty()) return kUsage;
  StaticRegistry reg;
  bool json = a.has("--json");
  if (json && apks.size() > 1) out("[\n");
  for (size_t i = 0; i < apks.size(); ++i) {
    const apk::LoadedApk& apk = *apks[i];
    bool is_split = apk.manifest && !apk.manifest->split.empty();
    compat::Report report = compat::evaluate(apk, reg.registry, is_split ? compat::Mode::kExec : compat::Mode::kApp);
    if (json) {
      if (i) out(",\n");
      out(compat::render_json(apk, report));
    } else {
      if (apks.size() > 1) out(std::string(i ? "\n" : "") + "=== " + path_utf8(apk.path) + " ===\n");
      compat::RenderOptions o;
      o.all_apis = a.has("--all-apis");
      o.manifest_xml = a.has("--manifest");
      o.classes = a.has("--classes");
      out(compat::render_text(apk, report, o));
    }
    if (a.has("--disasm")) out("\n" + disassemble_classes(apk, a.get("--disasm")));
  }
  if (json && apks.size() > 1) out("]\n");
  return kOk;
}

int cmd_gaps(const Args& a) {
  if (a.positional.empty()) {
    usage();
    return kUsage;
  }
  auto apks = load_all(a.positional);
  if (apks.empty()) return kUsage;
  std::vector<const apk::LoadedApk*> ptrs;
  for (const auto& p : apks) ptrs.push_back(p.get());
  StaticRegistry reg;
  compat::GapReport g = compat::analyze_gaps(ptrs, reg.registry);
  size_t top = a.has("--top") ? size_t(std::stoul(a.get("--top"))) : 25;
  std::string text = compat::render_gaps(g, a.has("--markdown"), top);
  if (a.has("--output")) {
    write_text_file(path_from_utf8(a.get("--output")), text);
    out("rapport écrit dans " + a.get("--output") + "\n");
  } else {
    out(text);
  }
  return kOk;
}

bool confirm_launch(const apk::LoadedApk& apk, const std::string& label) {
  std::string prompt = "Lancer « " + label + " » (SHA-256 " + apk.sha256.substr(0, 16) +
                       "…) ? Le code de cet APK va s'exécuter sur cette machine. [o/N] ";
  if (!_isatty(_fileno(stdin))) {
    err("lancement non confirmé : entrée non interactive (utilisez --yes pour confirmer explicitement)");
    return false;
  }
  out(prompt);
  std::string answer;
  std::getline(std::cin, answer);
  answer = text::trim(answer);
  return answer == "o" || answer == "O" || answer == "oui" || answer == "y" || answer == "yes";
}

void report_runtime(const rt::Runtime& rt) {
  const auto& s = rt.stats();
  log::info("runtime", "instructions exécutées : " + std::to_string(s.instructions) + ", appels : " +
                           std::to_string(s.invocations) + " (dont " + std::to_string(s.native_calls) +
                           " vers le pont), objets vivants : " + std::to_string(rt.object_count()) + ", collectes : " +
                           std::to_string(rt.gc_stats().collections) + ", threads démarrés : " +
                           std::to_string(s.threads_started));
  for (const auto& api : rt.unsupported_apis()) log::warn("runtime", "API non implémentée rencontrée : " + api, api);
  for (const auto& api : rt.stub_apis()) log::info("runtime", "API simulée utilisée : " + api);
}

// `exec --profile`: the methods that executed the most instructions in their
// own frames, then the most called ones.
void report_profile(rt::Runtime& rt) {
  std::vector<const rt::Method*> methods;
  for (rt::Class* c : rt.loaded_classes()) {
    for (const auto& m : c->methods) {
      if (m->profile_calls || m->profile_instructions) methods.push_back(m.get());
    }
  }
  auto print = [&](const char* title, auto key) {
    std::sort(methods.begin(), methods.end(), [&](const rt::Method* x, const rt::Method* y) { return key(x) > key(y); });
    err(title);
    for (size_t i = 0; i < std::min<size_t>(methods.size(), 25); ++i) {
      const rt::Method* m = methods[i];
      err("  " + std::to_string(m->profile_instructions) + " instr.  " + std::to_string(m->profile_calls) + " appels  " +
          m->pretty());
    }
  };
  print("Profil : méthodes par instructions exécutées (hors appelées)",
        [](const rt::Method* m) { return m->profile_instructions; });
  print("Profil : méthodes par nombre d'appels", [](const rt::Method* m) { return m->profile_calls; });
}

int cmd_run(const Args& a) {
  if (a.positional.size() != 1) {
    usage();
    return kUsage;
  }
  auto apk = load(a.positional[0]);
  if (!apk) return kUsage;
  StaticRegistry reg;
  compat::Report report = compat::evaluate(*apk, reg.registry);
  for (const auto& f : report.findings) {
    if (f.severity == compat::Severity::kBlocking) log::error("compat", f.message);
    else if (f.severity == compat::Severity::kWarning) log::warn("compat", f.message);
  }
  if (!apk->manifest) {
    err("manifeste illisible : lancement impossible");
    return kRefused;
  }
  std::string activity = a.get("--activity", report.launch_activity);
  if (!activity.empty()) activity = apk::resolve_class_name(apk->manifest->package, activity);
  if (report.verdict == compat::Verdict::kIncompatible && !a.has("--force")) {
    err("APK incompatible avec le prototype (voir `anyandroid inspect`) : lancement refusé. --force pour passer outre.");
    return kRefused;
  }
  if (activity.empty()) {
    err("aucune activité à lancer");
    return kRefused;
  }
  bool any_dex = false;
  for (const auto& p : apk->dex) any_dex |= p.file != nullptr;
  if (!any_dex) {
    err("aucun code DEX valide : lancement impossible");
    return kRefused;
  }
  const apk::Manifest& m = *apk->manifest;
  std::string label = m.app_label.empty() || m.app_label[0] == '@' ? m.package : m.app_label;
  if (!a.has("--yes") && !confirm_launch(*apk, label)) return kDeclined;

  rt::RuntimeOptions ro;
  ro.trace = a.has("--trace");
  rt::Runtime runtime(ro);
  for (const auto& p : apk->dex) {
    if (p.file) runtime.add_dex(p.file.get());
  }
  lifecycle::AppOptions ao;
  if (a.has("--max-instructions")) ao.max_instructions_per_event = std::stoull(a.get("--max-instructions"));
  try {
    ao.resources = app_resources(*apk, a.has("--locale") ? a.get("--locale") : user_locale());
  } catch (const std::exception& e) {
    log::warn("ressources", std::string("resources.arsc illisible, ressources indisponibles : ") + e.what());
  }

  std::unique_ptr<ui::Backend> backend;
  if (a.has("--headless")) {
    std::string script = a.get("--do");
    if (a.has("--script")) script = read_text_file(path_from_utf8(a.get("--script"))) + "\n" + script;
    backend = std::make_unique<backend::HeadlessBackend>(script, stdout);
  } else {
    backend = std::make_unique<backend::Win32Backend>();
  }
  lifecycle::AppHost host(runtime, m, *backend, ao);
  bridge::install(runtime.registry(), host);
  log::info("hôte", "lancement de " + m.package + " / " + activity);
  int code = host.run(activity);
  {
    rt::Runtime::Scope scope(runtime, "main");
    report_runtime(runtime);
  }
  return code;
}

int cmd_exec(const Args& a) {
  if (a.positional.size() < 2) {
    usage();
    return kUsage;
  }
  const std::string& file = a.positional[0];
  std::string cls = a.positional[1];
  std::unique_ptr<apk::LoadedApk> apk;
  std::vector<std::unique_ptr<dex::DexFile>> owned;
  std::vector<const dex::DexFile*> dex_files;
  std::string package = "exec";
  try {
    auto bytes = read_file(path_from_utf8(file), 512u << 20);
    if (bytes.size() >= 4 && bytes[0] == 'd' && bytes[1] == 'e' && bytes[2] == 'x' && bytes[3] == '\n') {
      for (auto& d : dex::DexFile::open_all(std::move(bytes), file)) {
        owned.push_back(std::move(d));
        dex_files.push_back(owned.back().get());
      }
    } else {
      apk = apk::load_apk_bytes(std::move(bytes), path_from_utf8(file));
      for (const auto& p : apk->dex) {
        if (p.file) dex_files.push_back(p.file.get());
        else err(p.entry + " ignoré : " + p.error);
      }
      if (apk->manifest) package = apk->manifest->package;
      if (!apk->native_libs.empty()) log::warn("compat", "bibliothèques natives présentes : elles ne seront pas chargées");
    }
  } catch (const std::exception& e) {
    err("fichier illisible : " + std::string(e.what()));
    return kUsage;
  }
  if (dex_files.empty()) {
    err("aucun code DEX exécutable");
    return kUsage;
  }
  // Libcore mode (G2): AOSP's boot class path replaces the bridge's java.*.
  image::LoadedBootClassPath boot;
  if (a.has("--bootclasspath")) {
    try {
      boot = image::load_boot_classpath(a.get("--bootclasspath"), a.has("--libcore-only"));
    } catch (const std::exception& e) {
      err("chemin de classes d'amorçage illisible : " + std::string(e.what()));
      return kUsage;
    }
    for (const auto& w : boot.warnings) log::warn("libcore", w);
    log::info("libcore", "chemin de classes d'amorçage : " + std::to_string(boot.device_paths.size()) + " archives, " +
                             std::to_string(boot.dex.size()) + " fichiers DEX");
  }
  rt::RuntimeOptions ro;
  ro.trace = a.has("--trace");
  ro.profile = a.has("--profile");
  rt::Runtime runtime(ro);
  for (const auto& d : boot.dex) runtime.add_boot_dex(d.get());
  for (const auto* d : dex_files) runtime.add_dex(d);
  lifecycle::ConsoleHost host(package);
  bridge::install(runtime.registry(), host);
  if (!boot.dex.empty()) {
    std::string bcp;
    for (const auto& p : boot.device_paths) bcp += (bcp.empty() ? "" : ":") + p;
    bridge::libcore_config().boot_class_path = bcp;
    bridge::libcore_config().class_path = "/data/app/" + package + "/base.apk";
    // File system of the process (ADR-005).
    try {
      vfs::LayoutOptions lo;
      lo.package = package;
      fs::path bcp_dir = path_from_utf8(a.get("--bootclasspath"));
      if (a.has("--system-image")) {
        image::OpenedImage img = image::open_image(path_from_utf8(a.get("--system-image")));
        lo.system = std::make_shared<image::SystemImage>(image::open_filesystem(img.source));
      } else if (fs::is_directory(bcp_dir)) {
        lo.base_dir = bcp_dir;  // the extracted boot class path, read-only
      }
      if (a.has("--data-dir")) {
        lo.data_root = path_from_utf8(a.get("--data-dir"));
      } else if (fs::is_directory(bcp_dir)) {
        lo.data_root = fs::absolute(bcp_dir).parent_path() / "appdata";
      } else {
        lo.data_root = fs::temp_directory_path() / "AnyAndroid" / "appdata";
      }
      bridge::libcore_config().vfs = vfs::make_android_vfs(lo);
      log::info("vfs", "données de l'application : " + path_utf8(lo.data_root));
    } catch (const std::exception& e) {
      err("système de fichiers de l'application : " + std::string(e.what()));
      return kUsage;
    }
  }
  std::string desc = cls[0] == 'L' && cls.back() == ';' ? cls : text::java_to_descriptor(cls);
  int code = kOk;
  rt::Runtime::Scope scope(runtime, "main");
  // Java SE semantics for console programs: report and let the thread die.
  runtime.on_uncaught_exception = [&](rt::VmThread* t, rt::Handle exception) {
    std::string d = "Exception in thread \"" + t->name + "\" " + runtime.describe_throwable(exception);
    std::fwrite(d.data(), 1, d.size(), stderr);
    code = kAppError;
  };
  int exit_code = -1;
  runtime.on_exit_request = [&](int c) { exit_code = c; };
  try {
    bridge::start_libcore(runtime);  // no-op without a boot class path
    rt::Class* c = runtime.find_class(desc);
    runtime.ensure_initialized(c);
    rt::Method* main = runtime.find_static(c, "main", "([Ljava/lang/String;)V");
    if (!main) {
      err("pas de méthode public static void main(String[]) dans " + c->java_name());
      return kUsage;
    }
    std::vector<std::string> args(a.positional.begin() + 2, a.positional.end());
    rt::Handle arr = runtime.new_array("[Ljava/lang/String;", int32_t(args.size()));
    for (size_t i = 0; i < args.size(); ++i) {
      rt::Handle s = runtime.new_string_utf8(args[i]);
      std::memcpy(&runtime.array(arr, "args").data[i * 4], &s, 4);
    }
    uint32_t argv[1] = {arr};
    runtime.invoke(main, argv, 1);
    host.run_tasks();
    // Like the JVM, wait for the non-daemon threads before exiting.
    runtime.wait_for_non_daemon_threads();
  } catch (const rt::JavaThrow& t) {
    std::string d = runtime.describe_throwable(t.exception);
    std::fwrite("Exception in thread \"main\" ", 1, 27, stderr);
    std::fwrite(d.data(), 1, d.size(), stderr);
    code = kAppError;
  } catch (const rt::ExitRequest& e) {
    code = e.code;
  }
  if (exit_code >= 0) code = exit_code;
  report_runtime(runtime);
  if (a.has("--profile")) report_profile(runtime);
  return code;
}

const char* type_label(image::FileType t) {
  switch (t) {
    case image::FileType::kRegular: return "-";
    case image::FileType::kDirectory: return "d";
    case image::FileType::kSymlink: return "l";
    default: return "?";
  }
}

int cmd_image(const Args& a) {
  if (a.positional.size() < 2) {
    usage();
    return kUsage;
  }
  const std::string& sub = a.positional[0];
  const std::string& file = a.positional[1];
  if (text::ends_with(file, ".zip")) {
    err("archive ZIP : décompressez d'abord l'image (par exemple `tar -xf " + file + " system.img`)");
    return kUsage;
  }
  try {
    image::OpenedImage img = image::open_image(path_from_utf8(file), a.get("--partition"));
    std::string fs_kind = image::detect_filesystem(*img.source);
    if (sub == "info") {
      std::string layers;
      for (const auto& l : img.layers) layers += l + " → ";
      out("Image : " + file + "\n");
      out("Format : " + layers + (fs_kind.empty() ? "inconnu" : fs_kind) + " (" + std::to_string(img.source->size()) +
          " octets)\n");
      if (fs_kind.empty()) return kOk;
      image::SystemImage sys(image::open_filesystem(img.source));
      for (const char* key : {"ro.system.build.fingerprint", "ro.system.build.version.release",
                              "ro.system.build.version.sdk", "ro.build.version.codename", "ro.system.build.type"}) {
        std::string v = sys.prop(key);
        if (!v.empty()) out(std::string("  ") + key + " = " + v + "\n");
      }
      size_t apex = 0;
      for (const auto& e : sys.apex_entries()) apex += !e.name.empty();
      out("  APEX dans /system/apex : " + std::to_string(apex) + "\n");
      return kOk;
    }
    if (fs_kind.empty()) {
      err("système de fichiers non reconnu dans " + file);
      return kUsage;
    }
    auto fsys = image::open_filesystem(img.source);
    if (sub == "ls") {
      std::string dir = a.positional.size() > 2 ? a.positional[2] : "/";
      auto entries = fsys->list(dir);
      std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) { return x.name < y.name; });
      for (const auto& e : entries) {
        image::Stat st = fsys->stat_inode(e.inode);
        std::string line = std::string(type_label(st.type)) + " " + std::to_string(st.size);
        line.resize(std::max<size_t>(line.size(), 14), ' ');
        line += " " + e.name;
        if (st.type == image::FileType::kSymlink) line += " -> " + text::printable(fsys->read_link(e.inode));
        out(line + "\n");
      }
      return kOk;
    }
    if (sub == "cat") {
      if (a.positional.size() < 3 || !a.has("--output")) {
        err("usage : anyandroid image cat <image> <chemin> --output <fichier>");
        return kUsage;
      }
      auto data = fsys->read_file(a.positional[2], 2ull << 30);
      write_file(path_from_utf8(a.get("--output")), data);
      out(std::to_string(data.size()) + " octets écrits dans " + a.get("--output") + "\n");
      return kOk;
    }
    if (sub == "bootclasspath") {
      if (!a.has("--output")) {
        err("--output <dossier> est requis (hors du dépôt : ces fichiers ne doivent pas être redistribués)");
        return kUsage;
      }
      image::SystemImage sys(std::move(fsys));
      auto result = image::extract_boot_classpath(sys, path_from_utf8(a.get("--output")));
      for (const auto& [k, v] : result.props) out(k + " = " + v + "\n");
      uint64_t total = 0;
      out("Chemin de classes d'amorçage (" + std::to_string(result.jars.size()) + " archives) :\n");
      for (const auto& j : result.jars) {
        out("  " + j.device_path + "  (" + std::to_string(j.size) + " octets, " + j.origin + ")\n");
        total += j.size;
      }
      for (const auto& d : result.data) total += d.size;
      out("Données : " + std::to_string(result.data.size()) + " fichiers ; total extrait : " + std::to_string(total) +
          " octets\n");
      for (const auto& w : result.warnings) log::warn("image", w);
      out("Liste écrite dans " + path_utf8(path_from_utf8(a.get("--output")) / "bootclasspath.txt") + "\n");
      return result.jars.empty() ? kAppError : kOk;
    }
    err("sous-commande image inconnue : " + sub);
    return kUsage;
  } catch (const FormatError& e) {
    err("image invalide : " + std::string(e.what()));
    return kUsage;
  } catch (const std::exception& e) {
    err(e.what());
    return kUsage;
  }
}

// Coverage of the native methods of a boot class path by AnyAndroid.
int cmd_libcore(const Args& a) {
  if (a.positional.empty() || a.positional[0] != "natives" || !a.has("--bootclasspath")) {
    err("usage : anyandroid libcore natives --bootclasspath <dossier|archives> [--all]");
    return kUsage;
  }
  image::LoadedBootClassPath boot;
  try {
    boot = image::load_boot_classpath(a.get("--bootclasspath"), a.has("--libcore-only"));
  } catch (const std::exception& e) {
    err("chemin de classes d'amorçage illisible : " + std::string(e.what()));
    return kUsage;
  }
  StaticRegistry reg;
  size_t total = 0, bound = 0, core = 0;
  std::map<std::string, std::vector<std::string>> missing;  // class -> methods
  for (const auto& d : boot.dex) {
    size_t file_total = 0, file_bound = 0;
    for (uint32_t c = 0; c < d->num_class_defs(); ++c) {
      const dex::ClassDef& def = d->class_def(c);
      std::string desc = d->type_descriptor(def.class_idx);
      bool vm_core = rt::is_vm_core_class(desc);
      dex::ClassData cd = d->class_data(def);
      for (const auto* list : {&cd.direct_methods, &cd.virtual_methods}) {
        for (const auto& m : *list) {
          if (!(m.access_flags & dex::kAccNative)) continue;
          const dex::MethodId& mid = d->method(m.method_idx);
          std::string name = d->string(mid.name_idx), sig = d->proto_signature(mid.proto_idx);
          ++total;
          ++file_total;
          if (vm_core && reg.registry.lookup_method(desc, name, sig).method) {
            ++core;
          } else if (reg.registry.binding(desc, name, sig)) {
            ++bound;
            ++file_bound;
          } else {
            missing[desc].push_back(name + sig + ((m.access_flags & dex::kAccStatic) ? "  [static]" : ""));
          }
        }
      }
    }
    out(d->location() + " : " + std::to_string(file_bound) + " / " + std::to_string(file_total) + " natives liées\n");
  }
  out("\nTotal : " + std::to_string(total) + " méthodes natives ; " + std::to_string(bound) + " liées par AnyAndroid, " +
      std::to_string(core) + " remplacées par le cœur de la VM (Object, String, Class, Thread, Throwable, réflexion), " +
      std::to_string(total - bound - core) + " manquantes.\n");
  // Members of the VM core classes (kept native in libcore mode) that the
  // boot class path references but AnyAndroid does not provide.
  std::set<std::string> core_missing;
  for (const auto& d : boot.dex) {
    for (uint32_t i = 0; i < d->num_methods(); ++i) {
      const dex::MethodId& mid = d->method(i);
      const std::string& cls = d->type_descriptor(mid.class_idx);
      if (!rt::is_vm_core_class(cls)) continue;
      const std::string& name = d->string(mid.name_idx);
      const std::string& sig = d->proto_signature(mid.proto_idx);
      if (!reg.registry.lookup_method(cls, name, sig).method) core_missing.insert(cls + "->" + name + sig);
    }
    for (uint32_t i = 0; i < d->num_fields(); ++i) {
      const dex::FieldId& fid = d->field(i);
      const std::string& cls = d->type_descriptor(fid.class_idx);
      if (!rt::is_vm_core_class(cls)) continue;
      const std::string& name = d->string(fid.name_idx);
      const std::string& type = d->type_descriptor(fid.type_idx);
      bool found = reg.registry.lookup_field(cls, name, type).field != nullptr;
      for (const rt::NativeClassSpec* s = reg.registry.find(cls); s && !found; s = s->super.empty() ? nullptr : reg.registry.find(s->super)) {
        for (const auto& f : s->instance_fields) found |= f.name == name && f.type == type;
      }
      if (!found) core_missing.insert(cls + "->" + name + ":" + type);
    }
  }
  out("Membres du cœur de la VM référencés par le chemin d'amorçage mais absents : " + std::to_string(core_missing.size()) +
      "\n");
  if (a.has("--core")) {
    for (const auto& m : core_missing) out("  " + m + "\n");
  }
  std::vector<std::pair<std::string, std::vector<std::string>>> by_size(missing.begin(), missing.end());
  std::stable_sort(by_size.begin(), by_size.end(), [](const auto& x, const auto& y) { return x.second.size() > y.second.size(); });
  size_t limit = a.has("--all") ? by_size.size() : std::min<size_t>(by_size.size(), 40);
  if (limit) out("\nNatives manquantes par classe" + std::string(a.has("--all") ? "" : " (40 premières, --all pour tout)") + " :\n");
  for (size_t i = 0; i < limit; ++i) {
    const auto& [cls, methods] = by_size[i];
    out("  " + text::descriptor_to_java(cls) + " (" + std::to_string(methods.size()) + ")\n");
    if (a.has("--all")) {
      for (const auto& m : methods) out("      " + m + "\n");
    }
  }
  return kOk;
}

std::string describe_value(const res::AssetManager& am, uint8_t type, uint32_t data, int32_t cookie) {
  char hex[16];
  std::snprintf(hex, sizeof(hex), "0x%08x", data);
  switch (type) {
    case res::kTypeNull: return data == res::kDataNullEmpty ? "@empty" : "@null";
    case res::kTypeReference:
    case res::kTypeAttribute: {
      auto n = am.resource_name(data);
      return std::string(type == res::kTypeReference ? "@" : "?") +
             (n ? text::utf16_to_utf8(n->package + u":" + n->type + u"/" + n->entry) : std::string(hex));
    }
    case res::kTypeString: {
      auto pool = am.string_pool(cookie);
      auto s8 = pool ? pool->string8(data) : std::nullopt;
      return s8 ? "\"" + *s8 + "\"" : "chaîne #" + std::to_string(data);
    }
    case res::kTypeIntBoolean: return data ? "true" : "false";
    case res::kTypeFirstInt: return std::to_string(int32_t(data));
    default:
      if (type >= 0x1c && type <= 0x1f) {
        std::snprintf(hex, sizeof(hex), "#%08x", data);
        return hex;
      }
      return "type " + std::to_string(type) + " " + hex;
  }
}

int cmd_resources(const Args& a) {
  if (a.positional.empty()) {
    usage();
    return kUsage;
  }
  try {
    std::vector<std::shared_ptr<res::ApkAssets>> apks;
    for (size_t i = 0; i < a.positional.size(); ++i) {
      auto bytes = read_file(path_from_utf8(a.positional[i]), 1ull << 30);
      apks.push_back(res::ApkAssets::load_apk(a.positional[i], std::move(bytes), i == 0 ? uint32_t(res::ApkAssets::kSystem) : 0u));
    }
    res::AssetManager am;
    am.set_apk_assets(apks);
    am.set_configurations({device_config(a.get("--locale", "en-US"), uint16_t(std::stoi(a.get("--density", "420"))))});

    for (const auto& apk : apks) {
      out(apk->path() + "\n");
      if (!apk->table()) {
        out("  (sans resources.arsc)\n");
        continue;
      }
      for (const auto& p : apk->table()->packages()) {
        size_t entries = 0, variants = 0;
        for (const auto& g : p->types) {
          entries += g.spec_flags.size();
          variants += g.variants.size();
        }
        char id[8];
        std::snprintf(id, sizeof(id), "0x%02x", p->id);
        out(std::string("  paquet ") + id + " " + text::utf16_to_utf8(p->name) + " : " + std::to_string(p->types.size()) +
            " types, " + std::to_string(entries) + " entrées, " + std::to_string(variants) + " variantes\n");
      }
    }
    if (a.has("--locales")) {
      auto locales = am.locales(false);
      out("Langues (" + std::to_string(locales.size()) + ") :");
      for (const auto& l : locales) out(" " + l);
      out("\n");
    }
    if (a.has("--get")) {
      std::string what = a.get("--get");
      uint32_t id = text::starts_with(what, "0x") ? uint32_t(std::stoul(what, nullptr, 16)) : am.resource_id(what, "", "android");
      if (id == 0) {
        err("ressource inconnue : " + what);
        return kUsage;
      }
      char hex[16];
      std::snprintf(hex, sizeof(hex), "0x%08x", id);
      auto name = am.resource_name(id);
      out(std::string(hex) + " " + (name ? text::utf16_to_utf8(name->package + u":" + name->type + u"/" + name->entry) : "?") +
          "\n");
      if (auto bag = am.get_bag(id)) {
        out("  bag (" + std::to_string(bag->entries.size()) + " entrées" +
            (bag->parent ? ", parent " + describe_value(am, res::kTypeReference, bag->parent, -1) : std::string()) + ")\n");
        for (const auto& e : bag->entries) {
          auto key = am.resource_name(e.key);
          std::string k = key ? text::utf16_to_utf8(key->entry) : std::to_string(e.key);
          out("    " + k + " = " + describe_value(am, e.value.type, e.value.data, e.cookie) + "\n");
        }
      } else if (auto v = am.get_resource(id, false)) {
        res::SelectedValue resolved = *v;
        am.resolve_reference(resolved);
        out("  valeur : " + describe_value(am, v->type, v->data, v->cookie));
        if (resolved.type != v->type || resolved.data != v->data) {
          out(" -> " + describe_value(am, resolved.type, resolved.data, resolved.cookie));
        }
        out("\n");
      } else {
        out("  aucune valeur pour cette configuration\n");
      }
    }
    return kOk;
  } catch (const std::exception& e) {
    err(std::string("ressources illisibles : ") + e.what());
    return kUsage;
  }
}

int cmd_compat(const Args& a) {
  StaticRegistry reg;
  if (a.has("--markdown")) {
    out(compat::render_api_matrix(reg.registry));
    return kOk;
  }
  std::map<rt::ApiStatus, int> totals;
  size_t classes = 0;
  for (const auto& [desc, spec] : reg.registry.classes()) {
    ++classes;
    for (const auto& m : spec->methods) totals[m.status]++;
    for (const auto& f : spec->static_fields) totals[f.status]++;
  }
  out("Classes implémentées par le pont : " + std::to_string(classes) + "\n");
  for (auto s : {rt::ApiStatus::kSupported, rt::ApiStatus::kPartial, rt::ApiStatus::kStub, rt::ApiStatus::kUnsupported}) {
    out(std::string("  ") + rt::api_status_label(s) + " : " + std::to_string(totals[s]) + "\n");
  }
  out("Détail : anyandroid compat --markdown\n");
  return kOk;
}

int dispatch(const std::vector<std::string>& argv) {
  Args a;
  if (!parse_args(argv, a)) return kUsage;
  if (a.command.empty() || a.command == "help" || a.has("--help")) {
    usage();
    return a.command.empty() ? kUsage : kOk;
  }
  if (a.command == "version") {
    out("AnyAndroid " AA_VERSION "\n");
    return kOk;
  }
  bool runs_app = a.command == "run" || a.command == "exec";
  if (!setup_logging(a, runs_app ? log::Level::Info : log::Level::Warn)) return kUsage;
  if (a.command == "inspect") return cmd_inspect(a);
  if (a.command == "run") return cmd_run(a);
  if (a.command == "exec") return cmd_exec(a);
  if (a.command == "compat") return cmd_compat(a);
  if (a.command == "gaps") return cmd_gaps(a);
  if (a.command == "image") return cmd_image(a);
  if (a.command == "libcore") return cmd_libcore(a);
  if (a.command == "resources") return cmd_resources(a);
  for (const char* planned : {"install", "list", "launch", "stop", "logs", "uninstall"}) {
    if (a.command == planned) {
      err("« " + a.command + " » est prévue en phase 5 (gestionnaire de paquets) et n'est pas encore implémentée");
      return kNotImplemented;
    }
  }
  err("commande inconnue : " + a.command);
  usage();
  return kUsage;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  try {
    return dispatch(utf8_args(argc, argv));
  } catch (const std::exception& e) {
    err(std::string("erreur interne : ") + e.what());
    return 70;
  }
}
