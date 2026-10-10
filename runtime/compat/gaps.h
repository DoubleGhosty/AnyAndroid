#pragma once

#include <map>
#include <string>
#include <vector>

#include "apk-inspector/apk.h"
#include "compat/compat.h"
#include "dex-engine/native_registry.h"

// Gap analysis across one or more real-world APKs (typically Google Mobile
// Services: GmsCore, Google Services Framework, Play Store). It answers
// "what is missing, ranked by how much the code relies on it", and profiles
// the platform capabilities those packages require. Static only: nothing is
// executed.
namespace aa::compat {

struct GapItem {
  std::string name;   // API, class or Java package
  rt::ApiStatus status = rt::ApiStatus::kUnsupported;
  std::string note;
  uint64_t apis = 0;  // distinct APIs (for classes/packages)
  uint64_t uses = 0;  // referencing instructions
};

struct Capability {
  std::string name;
  std::string meaning;
  uint64_t apis = 0;
  uint64_t uses = 0;
  uint64_t missing = 0;  // APIs of this capability AnyAndroid does not implement yet
  std::vector<std::string> evidence;  // a few concrete examples
};

struct PackageSummary {
  std::string path;
  std::string package;
  std::string version;
  std::string split;
  uint64_t size = 0;
  uint64_t dex_files = 0;
  uint64_t methods_with_code = 0;
  uint64_t bytecode = 0;
  std::vector<std::string> abis;
  std::string error;
};

struct GapReport {
  std::vector<PackageSummary> packages;
  std::map<rt::ApiStatus, uint64_t> apis_by_status;
  std::map<rt::ApiStatus, uint64_t> uses_by_status;
  std::vector<GapItem> missing_apis;      // unsupported or stub, by uses
  std::vector<GapItem> missing_classes;   // by uses
  std::vector<GapItem> missing_packages;  // Java packages, by uses
  std::vector<Capability> capabilities;
  std::vector<std::string> platform_requirements;  // manifest-level needs
  std::vector<std::string> prerequisites;          // what AnyAndroid must build first
};

GapReport analyze_gaps(const std::vector<const apk::LoadedApk*>& apks, const rt::NativeRegistry& registry);
std::string render_gaps(const GapReport& report, bool markdown, size_t top);

}  // namespace aa::compat
