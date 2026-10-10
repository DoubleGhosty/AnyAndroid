#pragma once

#include <map>
#include <string>
#include <vector>

#include "apk-inspector/apk.h"
#include "dex-engine/native_registry.h"

// Static compatibility evaluation: compares what an APK needs (manifest,
// code references, native libraries) with what AnyAndroid implements, before
// anything is executed.
namespace aa::compat {

enum class Severity { kInfo, kWarning, kBlocking };
enum class Verdict { kCompatible, kPartial, kIncompatible };

struct Finding {
  Severity severity;
  std::string code;  // stable identifier, e.g. NATIVE_LIBS
  std::string message;
};

struct ApiUse {
  std::string api;  // "Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V"
  rt::ApiStatus status;
  std::string note;
  bool is_class = false;
  uint64_t uses = 0;   // instructions referencing it
  std::string owner;   // class descriptor
};

struct Report {
  Verdict verdict = Verdict::kCompatible;
  std::vector<Finding> findings;
  std::vector<ApiUse> apis;
  std::map<rt::ApiStatus, int> counts;
  apk::CodeAnalysis code;
  std::string launch_activity;  // Java class name, empty if none
};

enum class Mode { kApp, kExec };

// `registry` must contain the core classes and the installed bridge.
Report evaluate(const apk::LoadedApk& apk, const rt::NativeRegistry& registry, Mode mode = Mode::kApp);

const char* verdict_label(Verdict v);
const char* severity_label(Severity s);

struct RenderOptions {
  bool all_apis = false;
  bool manifest_xml = false;
  bool classes = false;
};
std::string render_text(const apk::LoadedApk& apk, const Report& report, const RenderOptions& options = {});
std::string render_json(const apk::LoadedApk& apk, const Report& report);
// Published compatibility matrix (docs/compatibility/api-matrix.md).
std::string render_api_matrix(const rt::NativeRegistry& registry);

}  // namespace aa::compat
