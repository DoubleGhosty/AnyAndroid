#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "apk-inspector/axml.h"

namespace aa::apk {

struct IntentFilter {
  std::vector<std::string> actions;
  std::vector<std::string> categories;
  std::vector<std::string> schemes;
};

struct Component {
  std::string kind;  // activity, activity-alias, service, receiver, provider
  std::string name;  // fully qualified class name
  std::string label;
  std::string target_activity;  // activity-alias only
  std::optional<bool> exported;
  bool enabled = true;
  std::string process;      // android:process (":name" = private process)
  std::string permission;   // permission required to start/bind
  std::string authorities;  // provider only
  std::vector<IntentFilter> filters;

  bool is_launcher() const;
};

struct Feature {
  std::string name;  // empty for glEsVersion-only declarations
  bool required = true;
  uint32_t gl_es_version = 0;
};

struct MetaData {
  std::string name;
  std::string value;
};

// <permission> declared by the package itself.
struct PermissionDecl {
  std::string name;
  uint32_t protection_level = 0;  // R.attr.protectionLevel flags
  std::string protection_text() const;  // "signature|privileged", ...
};

struct Manifest {
  std::string package;
  std::string split;           // non-empty for a split APK (config.x86_64, ...)
  bool split_required = false; // base APK that cannot run without its splits
  std::string shared_user_id;  // android:sharedUserId (deprecated, used by GMS/GSF)
  int compile_sdk = 0;
  int64_t version_code = 0;
  std::string version_name;
  int min_sdk = 1;
  int target_sdk = 0;  // 0 = not declared (defaults to min_sdk)
  std::optional<int> max_sdk;
  std::vector<std::string> permissions;
  std::vector<Feature> features;
  std::vector<std::string> required_libraries;
  std::string application_class;  // custom android.app.Application, if any
  std::string app_label;
  bool debuggable = false;
  bool has_code = true;
  bool persistent = false;
  std::string app_process;  // default process of the application
  std::vector<std::string> optional_libraries;
  std::vector<MetaData> meta_data;  // <application><meta-data>
  std::vector<PermissionDecl> declared_permissions;
  std::vector<std::string> queried_packages;  // <queries><package>
  std::vector<Component> activities;  // includes activity-alias entries
  std::vector<Component> services;
  std::vector<Component> receivers;
  std::vector<Component> providers;

  // Fully qualified name of the MAIN/LAUNCHER activity (alias resolved).
  std::optional<std::string> launcher_activity() const;
  const Component* find_activity(const std::string& name) const;
  // Every process name used by the application and its components.
  std::vector<std::string> processes() const;
  const MetaData* find_meta_data(const std::string& name) const;
};

// Extracts the manifest model from a parsed AndroidManifest.xml. Throws
// FormatError if the root element or the package name is missing.
Manifest parse_manifest(const XmlDocument& doc);

// Resolves ".Foo" / "Foo" / "com.x.Foo" against the package name.
std::string resolve_class_name(const std::string& package, const std::string& name);

}  // namespace aa::apk
