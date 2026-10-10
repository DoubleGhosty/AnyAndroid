#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "apk-inspector/elf.h"
#include "apk-inspector/manifest.h"
#include "apk-inspector/zip_reader.h"
#include "dex-engine/dex_file.h"

// Static analysis of an APK. Nothing in this module executes code from the
// package: it only parses the archive, the manifest, the DEX files and the
// headers of native libraries.
namespace aa::apk {

struct NativeLib {
  std::string path;
  std::string abi;
  uint64_t size = 0;
  ElfInfo elf;
  bool abi_matches = false;  // ELF machine consistent with the directory
};

struct DexPart {
  std::string entry;  // classes.dex, classes2.dex, ...
  uint64_t size = 0;
  std::unique_ptr<dex::DexFile> file;
  std::string error;  // set when the file could not be parsed
};

struct LoadedApk {
  std::filesystem::path path;
  uint64_t file_size = 0;
  std::string sha256;
  std::unique_ptr<ZipArchive> zip;

  std::optional<Manifest> manifest;
  std::string manifest_error;
  std::string manifest_xml;  // decoded text form, for diagnostics

  std::vector<DexPart> dex;
  std::vector<NativeLib> native_libs;
  std::set<std::string> abis;
  bool has_resources_arsc = false;
  size_t resource_entries = 0;
  size_t asset_entries = 0;
  bool signed_v1 = false;      // META-INF/*.RSA|DSA|EC present
  bool signed_v2plus = false;  // APK Signing Block present
  std::vector<std::string> warnings;
};

struct LoadOptions {
  ZipLimits zip;
  uint64_t max_file_size = 1ull << 30;
};

// Throws std::runtime_error / FormatError when the file is not a readable
// archive. Problems in individual parts (manifest, DEX) are recorded in the
// result so that the inspector can still report everything else.
std::unique_ptr<LoadedApk> load_apk(const std::filesystem::path& path, const LoadOptions& options = {});
std::unique_ptr<LoadedApk> load_apk_bytes(std::vector<uint8_t> data, const std::filesystem::path& display_path,
                                          const LoadOptions& options = {});

// Loads a single APK, or every APK of an app bundle archive (.apks, .xapk,
// unencrypted .apkm: a ZIP whose entries are base.apk + split_*.apk). Split
// APKs come back as separate entries; the base APK is listed first.
std::vector<std::unique_ptr<LoadedApk>> load_package(const std::filesystem::path& path, const LoadOptions& options = {});

// A reference from app bytecode to a class member the app does not define
// itself (framework or missing library).
struct ExternalRef {
  std::string owner;      // first non-app class in the hierarchy
  std::string name;
  std::string signature;  // "(I)V" for methods, "I" for fields
  bool is_field = false;
  std::string via;        // class named in the bytecode reference
  uint64_t uses = 0;      // number of instructions referencing it
};

struct CodeAnalysis {
  std::set<std::string> app_classes;
  std::set<std::string> external_classes;
  std::vector<ExternalRef> refs;  // sorted, unique
  uint64_t methods_with_code = 0;
  uint64_t code_units = 0;
  uint64_t native_methods = 0;       // JNI methods declared by the app
  uint64_t invoke_custom_sites = 0;  // invoke-custom / invoke-polymorphic
  uint64_t undecodable_methods = 0;
  std::vector<std::string> native_method_samples;
  // Well-known package names found as string constants (GMS, GSF, Play Store...).
  std::set<std::string> package_strings;
};

CodeAnalysis analyze_code(const LoadedApk& apk);

}  // namespace aa::apk
