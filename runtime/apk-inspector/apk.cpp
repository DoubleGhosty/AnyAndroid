#include "apk-inspector/apk.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

#include "apk-inspector/axml.h"
#include "common/bytes.h"
#include "common/file_io.h"
#include "common/hash.h"
#include "common/text.h"
#include "dex-engine/opcodes.h"

namespace aa::apk {

std::unique_ptr<LoadedApk> load_apk(const std::filesystem::path& path, const LoadOptions& options) {
  return load_apk_bytes(read_file(path, options.max_file_size), path, options);
}

std::unique_ptr<LoadedApk> load_apk_bytes(std::vector<uint8_t> data, const std::filesystem::path& display_path,
                                          const LoadOptions& options) {
  auto apk = std::make_unique<LoadedApk>();
  apk->path = display_path;
  apk->file_size = data.size();
  apk->sha256 = hash::to_hex(hash::sha256(data));
  apk->zip = ZipArchive::open(std::move(data), options.zip);
  const ZipArchive& zip = *apk->zip;
  apk->warnings = zip.warnings();

  if (const ZipEntry* e = zip.find("AndroidManifest.xml")) {
    try {
      auto bytes = zip.read(*e);
      XmlDocument doc = parse_axml(bytes);
      apk->manifest_xml = to_text_xml(*doc.root);
      apk->manifest = parse_manifest(doc);
    } catch (const FormatError& err) {
      apk->manifest_error = err.what();
    }
  } else {
    apk->manifest_error = "AndroidManifest.xml absent";
  }

  for (int i = 1;; ++i) {
    std::string name = i == 1 ? "classes.dex" : "classes" + std::to_string(i) + ".dex";
    const ZipEntry* e = zip.find(name);
    if (!e) break;
    try {
      // A DEX 041 container yields one part per section ("classes.dex#1"...).
      for (auto& file : dex::DexFile::open_all(zip.read(*e), name)) {
        DexPart part;
        part.entry = file->location();
        part.size = file->header().file_size;
        part.file = std::move(file);
        apk->dex.push_back(std::move(part));
      }
    } catch (const FormatError& err) {
      DexPart part;
      part.entry = name;
      part.size = e->uncompressed_size;
      part.error = err.what();
      apk->dex.push_back(std::move(part));
    }
  }

  for (const ZipEntry& e : zip.entries()) {
    const std::string& n = e.name;
    if (e.unsafe_name) continue;
    if (text::starts_with(n, "lib/") && text::ends_with(n, ".so")) {
      auto parts = text::split(n, '/');
      NativeLib lib;
      lib.path = n;
      lib.abi = parts.size() >= 3 ? parts[1] : "?";
      lib.size = e.uncompressed_size;
      try {
        lib.elf = inspect_elf(zip.read(e));
      } catch (const FormatError& err) {
        lib.elf.error = err.what();
      }
      lib.abi_matches = lib.elf.valid && lib.elf.machine == abi_machine(lib.abi);
      apk->abis.insert(lib.abi);
      apk->native_libs.push_back(std::move(lib));
    } else if (n == "resources.arsc") {
      apk->has_resources_arsc = true;
    } else if (text::starts_with(n, "res/")) {
      apk->resource_entries++;
    } else if (text::starts_with(n, "assets/")) {
      apk->asset_entries++;
      if (text::ends_with(n, ".dex") || text::ends_with(n, ".jar") || text::ends_with(n, ".apk")) {
        apk->warnings.push_back("code potentiellement chargé dynamiquement : " + text::printable(n));
      }
    } else if (text::starts_with(n, "META-INF/") &&
               (text::ends_with(n, ".RSA") || text::ends_with(n, ".DSA") || text::ends_with(n, ".EC"))) {
      apk->signed_v1 = true;
    } else if (text::ends_with(n, ".dex") && n.find('/') == std::string::npos &&
               std::none_of(apk->dex.begin(), apk->dex.end(), [&](const DexPart& p) { return p.entry == n; })) {
      apk->warnings.push_back("fichier DEX hors séquence ignoré : " + text::printable(n));
    }
  }

  uint64_t cd = zip.central_directory_offset();
  static const char kSigMagic[] = "APK Sig Block 42";
  if (cd >= 16 && std::memcmp(zip.raw().data() + cd - 16, kSigMagic, 16) == 0) apk->signed_v2plus = true;
  return apk;
}

namespace {

struct AppClass {
  std::string super;
  std::vector<std::string> interfaces;
  std::set<std::string> methods;  // name + signature
  std::set<std::string> fields;   // name + ":" + type
};

// Looks for `key` declared in `cls` or any app supertype. Returns true if
// found; otherwise sets `external_owner` to the first non-app superclass.
bool declared_in_app(const std::map<std::string, AppClass>& classes, const std::string& cls, const std::string& key,
                     bool field, std::string& external_owner) {
  std::vector<std::string> pending = {cls};
  std::set<std::string> seen;
  bool first_chain = true;
  external_owner.clear();
  while (!pending.empty() && seen.size() < 4096) {
    std::string c = pending.back();
    pending.pop_back();
    if (!seen.insert(c).second) continue;
    // Walk the superclass chain of `c`, queueing interfaces as we go.
    while (true) {
      auto it = classes.find(c);
      if (it == classes.end()) {
        if (first_chain && external_owner.empty()) external_owner = c;
        break;
      }
      const AppClass& ac = it->second;
      if ((field ? ac.fields : ac.methods).count(key)) return true;
      for (const auto& i : ac.interfaces) pending.push_back(i);
      if (ac.super.empty() || !seen.insert(ac.super).second) break;
      c = ac.super;
    }
    first_chain = false;
  }
  return false;
}

}  // namespace

namespace {

// Package names whose presence as string constants reveals a dependency on
// Google Mobile Services (binding to their services, content providers...).
const char* const kKnownPackages[] = {
    "com.google.android.gms",          "com.google.android.gsf",           "com.android.vending",
    "com.google.android.gms.persistent", "com.google.android.c2dm.intent.RECEIVE",
    "com.google.android.c2dm.permission.SEND", "com.google.android.gsf.login",
    "content://com.google.android.gsf.gservices",
};

// Counts, for every method/field id, the instructions that reference it.
void scan_code(const dex::DexFile& d, const dex::EncodedMethod& m, std::vector<uint64_t>& method_uses,
               std::vector<uint64_t>& field_uses, CodeAnalysis& out) {
  try {
    dex::CodeItem code = d.code_item(m.code_off);
    std::span<const uint16_t> insns(code.insns);
    for (uint32_t pc = 0; pc < insns.size();) {
      if (uint32_t pu = dex::payload_units(insns, pc)) {
        pc += pu;
        continue;
      }
      dex::Decoded dec = dex::decode(insns, pc);
      switch (dex::op_info(dec.op).index) {
        case dex::IndexKind::kMethod:
        case dex::IndexKind::kMethodProto:
          if (dec.index < method_uses.size()) method_uses[dec.index]++;
          break;
        case dex::IndexKind::kField:
          if (dec.index < field_uses.size()) field_uses[dec.index]++;
          break;
        default:
          break;
      }
      if (dec.op >= 0xfa && dec.op <= 0xfd) out.invoke_custom_sites++;
      pc += dec.units;
    }
  } catch (const FormatError&) {
    out.undecodable_methods++;
  }
}

}  // namespace

CodeAnalysis analyze_code(const LoadedApk& apk) {
  CodeAnalysis out;
  std::map<std::string, AppClass> classes;
  struct Uses {
    std::vector<uint64_t> methods, fields;
  };
  std::vector<Uses> uses(apk.dex.size());
  for (size_t p = 0; p < apk.dex.size(); ++p) {
    const auto& part = apk.dex[p];
    if (!part.file) continue;
    const dex::DexFile& d = *part.file;
    uses[p].methods.assign(d.num_methods(), 0);
    uses[p].fields.assign(d.num_fields(), 0);
    for (uint32_t i = 0; i < d.num_class_defs(); ++i) {
      const dex::ClassDef& def = d.class_def(i);
      AppClass ac;
      if (def.superclass_idx != dex::kNoIndex) ac.super = d.type_descriptor(def.superclass_idx);
      dex::ClassData cd;
      try {
        if (def.interfaces_off) {
          for (uint32_t t : d.type_list(def.interfaces_off)) ac.interfaces.push_back(d.type_descriptor(t));
        }
        cd = d.class_data(def);
      } catch (const FormatError&) {
        out.undecodable_methods++;
      }
      for (const auto* list : {&cd.static_fields, &cd.instance_fields}) {
        for (const auto& f : *list) {
          const dex::FieldId& fid = d.field(f.field_idx);
          ac.fields.insert(d.string(fid.name_idx) + ":" + d.type_descriptor(fid.type_idx));
        }
      }
      for (const auto* list : {&cd.direct_methods, &cd.virtual_methods}) {
        for (const auto& m : *list) {
          const dex::MethodId& mid = d.method(m.method_idx);
          ac.methods.insert(d.string(mid.name_idx) + d.proto_signature(mid.proto_idx));
          if (m.access_flags & dex::kAccNative) {
            out.native_methods++;
            if (out.native_method_samples.size() < 20) out.native_method_samples.push_back(d.pretty_method(m.method_idx));
          }
          if (m.code_off) {
            out.methods_with_code++;
            out.code_units += ByteReader(d.bytes()).u32_at(m.code_off + 12);
            scan_code(d, m, uses[p].methods, uses[p].fields, out);
          }
        }
      }
      std::string desc = d.type_descriptor(def.class_idx);
      out.app_classes.insert(desc);
      classes.emplace(desc, std::move(ac));
    }
    for (uint32_t i = 0; i < d.num_strings(); ++i) {
      for (const char* known : kKnownPackages) {
        if (d.string(i) == known) out.package_strings.insert(known);
      }
    }
  }

  std::map<std::tuple<std::string, std::string, std::string, bool, std::string>, uint64_t> refs;
  auto add_ref = [&](std::string cls, const std::string& name, const std::string& sig, bool field, uint64_t n) {
    std::string via = cls;
    if (!cls.empty() && cls[0] == '[') cls = "Ljava/lang/Object;";  // array methods (clone, ...)
    std::string owner;
    if (declared_in_app(classes, cls, field ? name + ":" + sig : name + sig, field, owner)) return;
    if (owner.empty()) owner = cls;
    refs[{owner, name, sig, field, via == owner ? std::string() : via}] += n;
  };

  for (size_t p = 0; p < apk.dex.size(); ++p) {
    const auto& part = apk.dex[p];
    if (!part.file) continue;
    const dex::DexFile& d = *part.file;
    for (uint32_t i = 0; i < d.num_methods(); ++i) {
      const dex::MethodId& m = d.method(i);
      add_ref(d.type_descriptor(m.class_idx), d.string(m.name_idx), d.proto_signature(m.proto_idx), false,
              uses[p].methods[i]);
    }
    for (uint32_t i = 0; i < d.num_fields(); ++i) {
      const dex::FieldId& f = d.field(i);
      add_ref(d.type_descriptor(f.class_idx), d.string(f.name_idx), d.type_descriptor(f.type_idx), true,
              uses[p].fields[i]);
    }
    for (uint32_t i = 0; i < d.num_types(); ++i) {
      std::string t = d.type_descriptor(i);
      size_t dims = t.find_first_not_of('[');
      if (dims == std::string::npos) continue;
      t = t.substr(dims);
      if (t.size() > 1 && t[0] == 'L' && !out.app_classes.count(t)) out.external_classes.insert(t);
    }
  }
  for (const auto& [key, n] : refs) {
    const auto& [owner, name, sig, field, via] = key;
    out.refs.push_back({owner, name, sig, field, via, n});
  }
  return out;
}

std::vector<std::unique_ptr<LoadedApk>> load_package(const std::filesystem::path& path, const LoadOptions& options) {
  std::vector<std::unique_ptr<LoadedApk>> out;
  auto outer = load_apk(path, options);
  bool bundle = !outer->manifest && outer->manifest_error == "AndroidManifest.xml absent";
  if (!bundle) {
    out.push_back(std::move(outer));
    return out;
  }
  std::vector<const ZipEntry*> inner;
  for (const ZipEntry& e : outer->zip->entries()) {
    if (!e.unsafe_name && text::ends_with(e.name, ".apk")) inner.push_back(&e);
  }
  if (inner.empty()) {
    out.push_back(std::move(outer));  // neither an APK nor a bundle: report the original
    return out;
  }
  for (const ZipEntry* e : inner) {
    std::filesystem::path display = path;
    display += "!/" + e->name;
    try {
      out.push_back(load_apk_bytes(outer->zip->read(*e), display, options));
    } catch (const std::exception& err) {
      throw_format(e->name + " (dans le paquet) : " + err.what());
    }
  }
  // Base APK first (the one without a split name).
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    bool base_a = a->manifest && a->manifest->split.empty();
    bool base_b = b->manifest && b->manifest->split.empty();
    return base_a && !base_b;
  });
  return out;
}
}  // namespace aa::apk
