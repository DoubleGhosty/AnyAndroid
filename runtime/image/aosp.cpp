#include "image/aosp.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "apk-inspector/zip_reader.h"
#include "common/bytes.h"
#include "common/file_io.h"
#include "common/hash.h"
#include "common/log.h"
#include "common/text.h"

namespace aa::image {

namespace {

constexpr uint64_t kMaxApexSize = 1ull << 30;
constexpr uint64_t kMaxExtractedFile = 1ull << 30;
constexpr uint64_t kMaxExtractionTotal = 6ull << 30;

// Minimal protocol buffer wire-format reader (bounds-checked).
class PbReader {
 public:
  explicit PbReader(std::span<const uint8_t> d) : d_(d) {}
  bool done() const { return p_ >= d_.size(); }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (p_ >= d_.size()) throw_format("protobuf tronqué");
      uint8_t b = d_[p_++];
      v |= uint64_t(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    throw_format("varint protobuf invalide");
  }
  std::span<const uint8_t> bytes() {
    uint64_t n = varint();
    if (n > d_.size() - p_) throw_format("champ protobuf hors limites");
    auto s = d_.subspan(p_, size_t(n));
    p_ += size_t(n);
    return s;
  }
  std::string string() {
    auto b = bytes();
    return std::string(b.begin(), b.end());
  }
  void skip(uint32_t wire) {
    switch (wire) {
      case 0: varint(); break;
      case 1: advance(8); break;
      case 2: bytes(); break;
      case 5: advance(4); break;
      default: throw_format("type de champ protobuf non pris en charge : " + std::to_string(wire));
    }
  }

 private:
  void advance(size_t n) {
    if (n > d_.size() - p_) throw_format("protobuf tronqué");
    p_ += n;
  }
  std::span<const uint8_t> d_;
  size_t p_ = 0;
};

bool safe_relative(std::string_view rel) { return !rel.empty() && !apk::is_unsafe_entry_name(rel); }

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t b = s.find_first_not_of(" \t");
  return b == std::string::npos ? std::string() : s.substr(b);
}

}  // namespace

// --- APEX ----------------------------------------------------------------------------

ApexManifest parse_apex_manifest(std::span<const uint8_t> pb) {
  ApexManifest m;
  PbReader r(pb);
  while (!r.done()) {
    uint64_t key = r.varint();
    uint32_t field = uint32_t(key >> 3), wire = uint32_t(key & 7);
    if (field == 1 && wire == 2) {
      m.name = r.string();
    } else if (field == 2 && wire == 0) {
      m.version = int64_t(r.varint());
    } else {
      r.skip(wire);
    }
  }
  if (m.name.empty() || m.name.find('/') != std::string::npos || m.name.find("..") != std::string::npos) {
    throw_format("manifeste APEX sans nom valide");
  }
  return m;
}

Apex open_apex(std::vector<uint8_t> bytes) {
  apk::ZipLimits limits;
  limits.max_archive_size = kMaxApexSize;
  limits.max_entry_size = kMaxApexSize;
  limits.max_entries = 4096;
  auto zip = apk::ZipArchive::open(std::move(bytes), limits);
  Apex apex;
  if (zip->find("original_apex")) {  // compressed APEX (.capex)
    apex.compressed = true;
    auto inner = zip->read("original_apex");
    zip = apk::ZipArchive::open(std::move(inner), limits);
  }
  if (!zip->find("apex_manifest.pb")) throw_format("APEX sans apex_manifest.pb (format antérieur à Android 10 non pris en charge)");
  apex.manifest = parse_apex_manifest(zip->read("apex_manifest.pb"));
  if (!zip->find("apex_payload.img")) throw_format("APEX sans apex_payload.img : " + apex.manifest.name);
  auto src = std::make_shared<MemorySource>(zip->read("apex_payload.img"));
  zip.reset();
  apex.payload_fs = detect_filesystem(*src);
  apex.fs = open_filesystem(std::move(src));
  return apex;
}

// --- classpaths.pb ---------------------------------------------------------------------

std::vector<ClasspathJar> parse_classpaths(std::span<const uint8_t> pb) {
  std::vector<ClasspathJar> out;
  PbReader r(pb);
  while (!r.done()) {
    uint64_t key = r.varint();
    if ((key >> 3) != 1 || (key & 7) != 2) {
      r.skip(uint32_t(key & 7));
      continue;
    }
    PbReader jar(r.bytes());
    ClasspathJar j;
    while (!jar.done()) {
      uint64_t k = jar.varint();
      uint32_t field = uint32_t(k >> 3), wire = uint32_t(k & 7);
      if (field == 1 && wire == 2) {
        j.path = jar.string();
      } else if (field == 2 && wire == 0) {
        j.classpath = Classpath(jar.varint() & 7);
      } else if (field == 3 && wire == 2) {
        j.min_sdk = jar.string();
      } else if (field == 4 && wire == 2) {
        j.max_sdk = jar.string();
      } else {
        jar.skip(wire);
      }
    }
    if (!j.path.empty()) out.push_back(std::move(j));
  }
  return out;
}

// --- System image ---------------------------------------------------------------------------

std::map<std::string, std::string> parse_build_prop(std::span<const uint8_t> data) {
  std::map<std::string, std::string> props;
  std::string text(data.begin(), data.end());
  for (const auto& raw : text::split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    props[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return props;
}

SystemImage::SystemImage(std::unique_ptr<FileSystem> fs) : fs_(std::move(fs)) {
  auto has = [&](const char* p) {
    auto st = fs_->stat(p);
    return st && st->type == FileType::kRegular;
  };
  if (has("/system/build.prop")) {
    system_as_root_ = true;
  } else if (!has("/build.prop")) {
    throw_format("ce n'est pas une partition système Android (build.prop introuvable)");
  }
  props_ = parse_build_prop(fs_->read_file(fs_path("/system/build.prop"), 4u << 20));
}

std::string SystemImage::fs_path(const std::string& device_path) const {
  if (device_path != "/system" && !text::starts_with(device_path, "/system/")) return "";
  return system_as_root_ ? device_path : device_path.substr(7);
}

std::string SystemImage::prop(const std::string& key) const {
  auto it = props_.find(key);
  return it == props_.end() ? std::string() : it->second;
}

std::vector<DirEntry> SystemImage::apex_entries() const {
  std::string dir = fs_path("/system/apex");
  auto st = fs_->stat(dir);
  if (!st || st->type != FileType::kDirectory) return {};
  return fs_->list(dir);
}

// --- Extraction ---------------------------------------------------------------------------------

namespace {

// A file system subtree holding an APEX's contents: an APEX payload (root
// "") or a flattened APEX directory of the system image.
struct ApexView {
  std::string name;    // manifest name, e.g. com.android.art
  std::string origin;  // file or directory name in /system/apex
  const FileSystem* fs = nullptr;
  std::string root;
};

struct Extractor {
  const std::filesystem::path& out;
  BootClasspathExtraction& result;
  uint64_t total = 0;

  ExtractedFile write(const FileSystem& fs, const std::string& fs_path, const std::string& device_path,
                      const std::string& origin) {
    std::string rel = device_path.substr(1);
    if (!safe_relative(rel)) throw_format("chemin dangereux refusé : " + text::printable(device_path));
    auto data = fs.read_file(fs_path, kMaxExtractedFile);
    total += data.size();
    if (total > kMaxExtractionTotal) throw_format("extraction trop volumineuse (limite 6 Gio)");
    write_file(out / path_from_utf8(rel), data);
    ExtractedFile f;
    f.device_path = device_path;
    f.relative = rel;
    f.origin = origin;
    f.size = data.size();
    f.sha256 = hash::to_hex(hash::sha256(data));
    return f;
  }

  void write_tree(const FileSystem& fs, const std::string& fs_dir, const std::string& device_dir,
                  const std::string& origin, int depth = 0) {
    if (depth > 8) return;
    for (const auto& e : fs.list(fs_dir)) {
      Stat st = fs.stat_inode(e.inode);
      if (st.type == FileType::kDirectory) {
        write_tree(fs, fs_dir + "/" + e.name, device_dir + "/" + e.name, origin, depth + 1);
      } else if (st.type == FileType::kRegular) {
        result.data.push_back(write(fs, fs_dir + "/" + e.name, device_dir + "/" + e.name, origin));
      }
    }
  }
};

int apex_preference(const std::string& file) {
  int score = 0;
  if (file.find(".debug") != std::string::npos) score += 2;
  if (file.find(".testing") != std::string::npos) score += 4;
  return score;
}

std::vector<ClasspathJar> boot_jars_of(const FileSystem& fs, const std::string& pb_path) {
  auto st = fs.stat(pb_path);
  if (!st || st->type != FileType::kRegular) return {};
  std::vector<ClasspathJar> out;
  for (auto& j : parse_classpaths(fs.read_file(pb_path, 1u << 20))) {
    if (j.classpath == Classpath::kBoot) out.push_back(std::move(j));
  }
  return out;
}

}  // namespace

BootClasspathExtraction extract_boot_classpath(const SystemImage& system, const std::filesystem::path& out_dir) {
  BootClasspathExtraction result;
  for (const char* key : {"ro.system.build.fingerprint", "ro.build.fingerprint", "ro.system.build.version.release",
                          "ro.system.build.version.sdk", "ro.build.version.codename", "ro.system.build.type"}) {
    std::string v = system.prop(key);
    if (!v.empty()) result.props[key] = v;
  }
  Extractor ex{out_dir, result};
  const FileSystem& sfs = system.fs();

  // System jars. /system's list may also name jars of non-updatable modules
  // (e.g. /apex/com.android.i18n/javalib/core-icu4j.jar): they keep their
  // position and are read from the module below.
  std::vector<std::string> system_order;
  std::map<std::string, std::vector<std::string>> wanted_from_apex;  // module -> device paths
  std::map<std::string, ExtractedFile> extracted;                    // device path -> file
  for (const auto& j : boot_jars_of(sfs, system.fs_path("/system/etc/classpaths/bootclasspath.pb"))) {
    std::string p = system.fs_path(j.path);
    if (!p.empty()) {
      extracted[j.path] = ex.write(sfs, p, j.path, "system");
      system_order.push_back(j.path);
    } else if (text::starts_with(j.path, "/apex/") && j.path.find('/', 6) != std::string::npos) {
      wanted_from_apex[j.path.substr(6, j.path.find('/', 6) - 6)].push_back(j.path);
      system_order.push_back(j.path);
    } else {
      result.warnings.push_back("chemin de classes système non reconnu, ignoré : " + j.path);
    }
  }
  if (system_order.empty()) {
    result.warnings.push_back("/system/etc/classpaths/bootclasspath.pb absent ou vide (image antérieure à Android 12 ?)");
  }

  // APEX jars: every APEX is opened once; for a module present several times
  // (release and debug builds), the release build wins.
  std::vector<DirEntry> entries = system.apex_entries();
  std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
    int pa = apex_preference(a.name), pb = apex_preference(b.name);
    return pa != pb ? pa < pb : a.name < b.name;
  });
  std::set<std::string> seen;
  std::map<std::string, std::vector<ExtractedFile>> apex_jars;
  std::string apex_dir = system.fs_path("/system/apex");
  for (const auto& e : entries) {
    std::string path = apex_dir + "/" + e.name;
    Stat st = sfs.stat_inode(e.inode);
    Apex apex;
    ApexView view;
    view.origin = e.name;
    try {
      if (st.type == FileType::kDirectory) {
        view.fs = &sfs;
        view.root = path;
        view.name = parse_apex_manifest(sfs.read_file(path + "/apex_manifest.pb", 1u << 20)).name;
      } else if (st.type == FileType::kRegular &&
                 (text::ends_with(e.name, ".apex") || text::ends_with(e.name, ".capex"))) {
        apex = open_apex(sfs.read_file(path, kMaxApexSize));
        view.fs = apex.fs.get();
        view.name = apex.manifest.name;
      } else {
        continue;
      }
    } catch (const FormatError& err) {
      result.warnings.push_back("APEX illisible ignoré : " + e.name + " (" + err.what() + ")");
      continue;
    }
    if (!seen.insert(view.name).second) continue;
    std::string prefix = "/apex/" + view.name + "/";
    for (const auto& j : boot_jars_of(*view.fs, view.root + "/etc/classpaths/bootclasspath.pb")) {
      if (!text::starts_with(j.path, prefix)) {
        result.warnings.push_back("chemin de classes d'un autre module ignoré dans " + view.name + " : " + j.path);
        continue;
      }
      std::string inner = view.root + "/" + j.path.substr(prefix.size());
      apex_jars[view.name].push_back(ex.write(*view.fs, inner, j.path, view.origin));
    }
    for (const auto& p : wanted_from_apex[view.name]) {
      extracted[p] = ex.write(*view.fs, view.root + "/" + p.substr(prefix.size()), p, view.origin);
    }
    // Data read by libcore at run time (ICU, time zones).
    for (const char* sub : {"etc/icu", "etc/tz"}) {
      auto dst = view.fs->stat(view.root + "/" + sub);
      if (dst && dst->type == FileType::kDirectory) ex.write_tree(*view.fs, view.root + "/" + sub, prefix + sub, view.origin);
    }
  }

  // Order of derive_classpath: ART module, then /system, then the other modules.
  auto art = apex_jars.find("com.android.art");
  if (art == apex_jars.end()) {
    result.warnings.push_back("module ART (com.android.art) introuvable : pas de libcore");
  } else {
    result.jars = art->second;
    apex_jars.erase(art);
  }
  for (const auto& p : system_order) {
    auto it = extracted.find(p);
    if (it == extracted.end()) {
      result.warnings.push_back("archive du chemin de classes système introuvable : " + p);
    } else {
      result.jars.push_back(it->second);
    }
  }
  std::set<std::string> listed;
  for (const auto& j : result.jars) listed.insert(j.device_path);
  for (auto& [name, jars] : apex_jars) {
    for (const auto& j : jars) {
      if (listed.insert(j.device_path).second) result.jars.push_back(j);
    }
  }

  // Jars whose code was stripped (boot image only) cannot be interpreted.
  for (const auto& j : result.jars) {
    try {
      apk::ZipLimits limits;
      limits.max_archive_size = kMaxExtractedFile;
      auto zip = apk::ZipArchive::open(read_file(out_dir / path_from_utf8(j.relative), kMaxExtractedFile), limits);
      if (!zip->find("classes.dex")) result.warnings.push_back("archive sans classes.dex (code retiré ?) : " + j.device_path);
    } catch (const std::exception& err) {
      result.warnings.push_back("archive illisible : " + j.device_path + " (" + err.what() + ")");
    }
  }

  std::string listing;
  listing += "# AnyAndroid : chemin de classes d'amorçage extrait d'une image système Android (G2).\n";
  listing += "# Ces fichiers proviennent de l'image : ne pas les redistribuer ni les ajouter au dépôt.\n";
  for (const auto& [k, v] : result.props) listing += "# " + k + "=" + v + "\n";
  listing += "# type\tchemin-appareil\tfichier\tsha256\toctets\torigine\n";
  auto line = [&](const char* kind, const ExtractedFile& f) {
    listing += std::string(kind) + "\t" + f.device_path + "\t" + f.relative + "\t" + f.sha256 + "\t" +
               std::to_string(f.size) + "\t" + f.origin + "\n";
  };
  for (const auto& f : result.jars) line("jar", f);
  for (const auto& f : result.data) line("data", f);
  write_text_file(out_dir / "bootclasspath.txt", listing);
  return result;
}

std::vector<BootJar> read_boot_classpath_list(const std::filesystem::path& dir) {
  std::string text = read_text_file(dir / "bootclasspath.txt");
  std::vector<BootJar> out;
  for (const auto& raw : text::split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    auto cols = text::split(line, '\t');
    if (cols.size() < 3 || cols[0] != "jar") continue;
    if (!safe_relative(cols[2])) throw std::runtime_error("bootclasspath.txt : chemin dangereux " + cols[2]);
    out.push_back({cols[1], dir / path_from_utf8(cols[2])});
  }
  if (out.empty()) throw std::runtime_error("bootclasspath.txt ne liste aucune archive");
  return out;
}

LoadedBootClassPath load_boot_classpath(const std::string& spec, bool libcore_only) {
  std::vector<BootJar> jars;
  std::filesystem::path as_dir = path_from_utf8(spec);
  if (std::filesystem::is_directory(as_dir)) {
    jars = read_boot_classpath_list(as_dir);
    if (libcore_only) {
      std::erase_if(jars, [](const BootJar& j) {
        return !text::starts_with(j.device_path, "/apex/com.android.art/") &&
               !text::starts_with(j.device_path, "/apex/com.android.i18n/");
      });
      if (jars.empty()) throw std::runtime_error("aucune archive de la libcore (modules ART et i18n) dans " + spec);
    }
  } else {
    for (const auto& part : text::split(spec, ';')) {
      if (part.empty()) continue;
      std::filesystem::path p = path_from_utf8(part);
      jars.push_back({"/system/framework/" + path_utf8(p.filename()), p});
    }
  }
  LoadedBootClassPath out;
  apk::ZipLimits limits;
  limits.max_archive_size = kMaxExtractedFile;
  for (const auto& jar : jars) {
    out.device_paths.push_back(jar.device_path);
    auto bytes = read_file(jar.file, kMaxExtractedFile);
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "dex\n", 4) == 0) {
      for (auto& d : dex::DexFile::open_all(std::move(bytes), path_utf8(jar.file))) out.dex.push_back(std::move(d));
      continue;
    }
    auto zip = apk::ZipArchive::open(std::move(bytes), limits);
    size_t found = 0;
    for (int i = 1; i < 100; ++i) {
      std::string name = i == 1 ? "classes.dex" : "classes" + std::to_string(i) + ".dex";
      if (!zip->find(name)) break;
      try {
        for (auto& d : dex::DexFile::open_all(zip->read(name), jar.device_path + "!" + name)) {
          out.dex.push_back(std::move(d));
        }
        ++found;
      } catch (const FormatError& e) {
        throw_format(jar.device_path + "!" + name + " : " + e.what());
      }
    }
    if (!found) out.warnings.push_back("aucun code DEX dans " + jar.device_path);
  }
  return out;
}

}  // namespace aa::image
