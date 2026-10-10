#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dex-engine/dex_file.h"
#include "image/filesystem.h"

// Android system image contents: APEX packages, classpath configuration and
// boot class path extraction (G2: run AOSP's libcore on the AnyAndroid VM).
namespace aa::image {

// --- APEX ----------------------------------------------------------------------

struct ApexManifest {
  std::string name;
  int64_t version = 0;
};
ApexManifest parse_apex_manifest(std::span<const uint8_t> pb);

// An APEX package (or compressed .capex) opened from its bytes.
struct Apex {
  ApexManifest manifest;
  bool compressed = false;
  std::string payload_fs;  // "ext4" or "erofs"
  std::unique_ptr<FileSystem> fs;
};
Apex open_apex(std::vector<uint8_t> bytes);

// --- classpaths.pb (packages/modules/common/proto/classpaths.proto) ----------------

enum class Classpath { kUnknown = 0, kBoot = 1, kDex2oatBoot = 2, kSystemServer = 3, kStandaloneSystemServer = 4 };

struct ClasspathJar {
  std::string path;  // device path, e.g. /apex/com.android.art/javalib/core-oj.jar
  Classpath classpath = Classpath::kUnknown;
  std::string min_sdk;
  std::string max_sdk;
};
std::vector<ClasspathJar> parse_classpaths(std::span<const uint8_t> pb);

// --- System image ------------------------------------------------------------------

std::map<std::string, std::string> parse_build_prop(std::span<const uint8_t> text);

// A system partition: handles system-as-root layouts (/system/...) and plain
// system partitions (/framework/...). Device paths are always "/system/...".
class SystemImage {
 public:
  explicit SystemImage(std::unique_ptr<FileSystem> fs);
  const FileSystem& fs() const { return *fs_; }
  // File-system path of a device path under /system, or "" if outside it.
  std::string fs_path(const std::string& device_path) const;
  const std::map<std::string, std::string>& build_props() const { return props_; }
  std::string prop(const std::string& key) const;
  // APEX files and flattened APEX directories under /system/apex.
  std::vector<DirEntry> apex_entries() const;

 private:
  std::unique_ptr<FileSystem> fs_;
  bool system_as_root_ = false;
  std::map<std::string, std::string> props_;
};

struct ExtractedFile {
  std::string device_path;  // /apex/com.android.art/javalib/core-oj.jar
  std::string relative;     // apex/com.android.art/javalib/core-oj.jar (in the output directory)
  std::string origin;       // APEX file name or "system"
  uint64_t size = 0;
  std::string sha256;
};

struct BootClasspathExtraction {
  std::map<std::string, std::string> props;  // fingerprint, release, sdk
  std::vector<ExtractedFile> jars;            // boot class path, in order
  std::vector<ExtractedFile> data;            // ICU and time zone data
  std::vector<std::string> warnings;
};

// Extracts the boot class path jars (and the libcore data files) of a system
// image into out_dir, and writes out_dir/bootclasspath.txt describing them.
BootClasspathExtraction extract_boot_classpath(const SystemImage& system, const std::filesystem::path& out_dir);

// Reads out_dir/bootclasspath.txt: the extracted jars, in class path order.
struct BootJar {
  std::string device_path;      // /apex/com.android.art/javalib/core-oj.jar
  std::filesystem::path file;  // extracted copy
};
std::vector<BootJar> read_boot_classpath_list(const std::filesystem::path& dir);

// A boot class path loaded in memory: the DEX files of each jar, in order.
struct LoadedBootClassPath {
  std::vector<std::string> device_paths;  // one per jar
  std::vector<std::unique_ptr<dex::DexFile>> dex;
  std::vector<std::string> warnings;
};
// `spec` is a directory holding bootclasspath.txt (see extract_boot_classpath)
// or a list of .jar / .dex files separated by ';'. With libcore_only, only the
// jars of the ART and i18n modules are kept (G2: libcore without framework).
LoadedBootClassPath load_boot_classpath(const std::string& spec, bool libcore_only = false);

}  // namespace aa::image
