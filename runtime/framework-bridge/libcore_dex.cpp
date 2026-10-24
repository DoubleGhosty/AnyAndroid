// Class loaders of libcore (ADR-006 J2): dalvik.system.DexFile natives, and
// the class lookups of ART that depend on a loader (VMClassLoader,
// Class.classForName, Class.getClassLoader, VMStack).
//
// There is one class namespace: a DEX file registered by a loader
// (DexFile.defineClassNative) records that loader in its DexCache, and every
// class of that file reports it as its defining loader. A class already
// defined is shared by all loaders that reach it, and the boot space always
// wins (an application cannot replace a framework or host class).
//
// DexFile cookies are long[]{0, handle}: index 0 is ART's oat file slot,
// unused; the handle is kind-tagged and bounded (handles.h). DEX files are
// read through the process VFS, never from a host path, and the bytes kept
// alive for opened files are capped.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "apk-inspector/zip_reader.h"
#include "common/text.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/native_memory.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr uint64_t kMaxSourceSize = 1ull << 30;  // an APK or DEX file read by openDexFileNative
constexpr uint64_t kMaxOpenedBytes = 1ull << 30;  // DEX bytes kept for all opened files
constexpr int kMaxLoaderDepth = 32;               // parent chain walked by the fast path
constexpr size_t kMaxDexSets = 4096;              // DexFile objects open at once

// The DEX files behind one DexFile object (one per classesN.dex of an APK).
struct DexSet {
  std::string path;
  std::vector<const dex::DexFile*> files;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<DexSet> sets{0x0e01};
  std::mutex lock;
  // DEX files opened at run time. Never freed: a runtime's classes point into
  // them until it ends, and Runtime::add_dex requires them to outlive it.
  std::vector<std::unique_ptr<dex::DexFile>> owned;
  uint64_t owned_bytes = 0;
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed, as the DEX files it owns
  return *t;
}

bool is_dex(const std::vector<uint8_t>& b) { return b.size() >= 4 && std::memcmp(b.data(), "dex\n", 4) == 0; }

bool same_bytes(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// Adds the DEX file(s) of `bytes` to the set, reusing the files the runtime
// already has for the same bytes: the application's, added at start-up, or
// those an earlier loader opened. A DEX 041 container holds several files
// that share its bytes (DexFile::bytes() is the whole container); they are
// told apart by their section offset.
void add_dex_bytes(Runtime& rt, DexSet& set, std::vector<uint8_t> bytes, const std::string& location) {
  std::vector<const dex::DexFile*> known = rt.app_dex_files();
  std::vector<const dex::DexFile*> same;
  for (const dex::DexFile* d : known) {
    bool seen = std::any_of(same.begin(), same.end(), [&](const dex::DexFile* s) {
      return s->section_offset() == d->section_offset();
    });
    if (!seen && same_bytes(d->bytes(), bytes)) same.push_back(d);
  }
  if (!same.empty()) {
    std::sort(same.begin(), same.end(),
              [](const dex::DexFile* x, const dex::DexFile* y) { return x->section_offset() < y->section_offset(); });
    set.files.insert(set.files.end(), same.begin(), same.end());
    return;
  }
  auto parts = dex::DexFile::open_all(std::move(bytes), location);  // FormatError on a malformed file
  Tables& t = tables();
  std::lock_guard<std::mutex> guard(t.lock);
  uint64_t size = parts.empty() ? 0 : parts.front()->bytes().size();  // the parts share one buffer
  if (t.owned_bytes + size > kMaxOpenedBytes) {
    throw std::runtime_error("[AnyAndroid] quota des fichiers DEX chargés atteint");
  }
  t.owned_bytes += size;
  for (auto& part : parts) {
    rt.add_dex(part.get());
    set.files.push_back(part.get());
    t.owned.push_back(std::move(part));
  }
}

std::shared_ptr<DexSet> dex_set_or_null(Runtime& rt, Handle cookie) {
  if (cookie == kNull) return nullptr;
  rt::Class* cls = rt.class_of(cookie);
  if (!cls || cls->descriptor != "[J") return nullptr;
  rt::ArrayObject& arr = rt.array(cookie, "cookie");
  if (arr.length < 2) return nullptr;
  int64_t h;
  std::memcpy(&h, arr.data.data() + 8, 8);
  return tables().sets.get(h);
}

std::shared_ptr<DexSet> dex_set(Runtime& rt, Handle cookie) {
  if (cookie == kNull) rt.throw_npe("cookie");
  auto set = dex_set_or_null(rt, cookie);
  if (!set) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] DexFile fermé ou invalide");
  return set;
}

// Records `loader` as the defining loader of the set's DEX files that have
// none yet (ART registers a DEX file with the loader that defines from it).
void claim(Runtime& rt, const DexSet& set, Handle loader) {
  if (loader == kNull) return;
  for (const dex::DexFile* d : set.files) {
    rt::DexCache* cache = rt.find_dex_cache(d);
    if (cache && !cache->boot && cache->loader == kNull) cache->loader = loader;
  }
}

// Defines `descriptor` from the first file of the set that has it.
rt::Class* define_from_set(Runtime& rt, const DexSet& set, const std::string& descriptor, Handle loader) {
  for (const dex::DexFile* d : set.files) {
    if (!d->find_class_def(descriptor)) continue;
    rt::Class* c = rt.define_class(d, descriptor);
    if (!c) continue;
    if (!Runtime::in_boot_space(c)) claim(rt, set, loader);
    return c;
  }
  return nullptr;
}

// "com.example.Foo" (binary name) or "[Ljava.lang.String;" -> descriptor;
// empty when the name cannot name a class.
std::string binary_name_descriptor(const std::string& name) {
  if (name.empty() || name.find('/') != std::string::npos) return {};
  if (name[0] == '[') {
    std::string d = name;
    std::replace(d.begin(), d.end(), '.', '/');
    return d;
  }
  if (name.find_first_of(";[") != std::string::npos) return {};
  return text::java_to_descriptor(name);
}

Handle object_field(Runtime& rt, Handle obj, std::string_view name, std::string_view type, bool& found) {
  found = false;
  rt::Object* o = rt.object(obj);
  if (!o) return kNull;
  rt::Field* f = find_field(o->klass, name, type);
  if (!f || f->is_static()) return kNull;
  found = true;
  return o->fields.at(f->slot);
}

bool is_boot_loader(Runtime& rt, Handle loader) {
  return loader == kNull || rt.class_of(loader)->descriptor == "Ljava/lang/BootClassLoader;";
}

struct Lookup {
  bool supported = false;  // false: a loader the fast path does not model
  rt::Class* cls = nullptr;
};

// ART's ClassLinker::FindClassInBaseDexClassLoader: for the standard loaders,
// the parent chain then the loader's own DEX files, resolved directly instead
// of through loadClass() and its ClassNotFoundExceptions.
Lookup find_in_loader_chain(Runtime& rt, const std::string& descriptor, Handle loader, int depth) {
  if (is_boot_loader(rt, loader)) return {true, rt.lookup_boot_class(descriptor)};
  if (depth >= kMaxLoaderDepth) return {};
  const std::string& kind = rt.class_of(loader)->descriptor;
  if (kind != "Ldalvik/system/PathClassLoader;" && kind != "Ldalvik/system/DexClassLoader;" &&
      kind != "Ldalvik/system/InMemoryDexClassLoader;") {
    return {};
  }
  bool found = false;
  for (const char* shared : {"sharedLibraryLoaders", "sharedLibraryLoadersAfter"}) {
    if (object_field(rt, loader, shared, "[Ljava/lang/ClassLoader;", found) != kNull || !found) return {};
  }
  Handle parent = object_field(rt, loader, "parent", "Ljava/lang/ClassLoader;", found);
  if (!found) return {};
  Lookup up = find_in_loader_chain(rt, descriptor, parent, depth + 1);
  if (!up.supported || up.cls) return up;
  if (descriptor[0] == '[') return {true, nullptr};  // arrays of application classes: loadClass()
  Handle path_list = object_field(rt, loader, "pathList", "Ldalvik/system/DexPathList;", found);
  if (!found) return {};
  if (path_list == kNull) return {true, nullptr};
  Handle elements = object_field(rt, path_list, "dexElements", "[Ldalvik/system/DexPathList$Element;", found);
  if (!found) return {};
  if (elements == kNull) return {true, nullptr};
  rt::ArrayObject& arr = rt.array(elements, "dexElements");
  for (uint32_t i = 0; i < arr.length; ++i) {
    Handle element;
    std::memcpy(&element, arr.data.data() + 4 * size_t(i), 4);
    Handle dex_file = object_field(rt, element, "dexFile", "Ldalvik/system/DexFile;", found);
    if (dex_file == kNull) continue;
    Handle cookie = object_field(rt, dex_file, "mCookie", "Ljava/lang/Object;", found);
    auto set = dex_set_or_null(rt, cookie);
    if (!set) continue;
    if (rt::Class* c = define_from_set(rt, *set, descriptor, loader)) return {true, c};
  }
  return {true, nullptr};
}

// A cookie (DexFile.mCookie): a long[] whose element 1 is the DexSet handle.
Handle cookie_for(Runtime& rt, std::shared_ptr<DexSet> set) {
  if (tables().sets.live() >= kMaxDexSets) rt.throw_new("Ljava/io/IOException;", "[AnyAndroid] trop de DexFile ouverts");
  int64_t h = tables().sets.add(std::move(set));
  Handle cookie = rt.new_array("[J", 2);
  std::memcpy(rt.array(cookie, "cookie").data.data() + 8, &h, 8);
  return cookie;
}

Handle open_dex_file(Runtime& rt, NativeArgs& a) {
  if (a.h(0) == kNull) rt.throw_npe("sourceName");
  std::string path = rt.string_utf8(a.h(0));
  std::vector<uint8_t> bytes;
  try {
    bytes = libcore_vfs().read_file(path, kMaxSourceSize);
  } catch (const vfs::VfsError& e) {
    rt.throw_new("Ljava/io/IOException;", "Failed to open dex files from " + path + " because: " + strerror_text(e.err));
  }
  auto set = std::make_shared<DexSet>();
  set->path = path;
  try {
    if (is_dex(bytes)) {
      add_dex_bytes(rt, *set, std::move(bytes), path);
    } else {
      auto zip = apk::ZipArchive::open(std::move(bytes));
      for (int i = 1;; ++i) {
        std::string name = i == 1 ? "classes.dex" : "classes" + std::to_string(i) + ".dex";
        const apk::ZipEntry* e = zip->find(name);
        if (!e) break;
        add_dex_bytes(rt, *set, zip->read(*e), i == 1 ? path : path + "!" + name);
      }
    }
  } catch (const std::exception& e) {
    rt.throw_new("Ljava/io/IOException;", "Failed to open dex files from " + path + " because: " + e.what());
  }
  if (set->files.empty()) rt.throw_new("Ljava/io/IOException;", "No original dex files found for dex location " + path);
  return cookie_for(rt, set);
}

// DexFile_openInMemoryDexFilesNative: each DEX is copied from a byte array or
// a direct ByteBuffer, between its start and end.
Handle open_in_memory_dex_files(Runtime& rt, NativeArgs& a) {
  auto element = [&](Handle array, uint32_t i) {
    Handle h;
    std::memcpy(&h, &rt.array(array, "Object[]").data[size_t(i) * 4], 4);
    return h;
  };
  auto int_at = [&](Handle array, uint32_t i) {
    int32_t v;
    std::memcpy(&v, &rt.array(array, "int[]").data[size_t(i) * 4], 4);
    return v;
  };
  for (int i = 0; i < 4; ++i) {
    if (a.h(i) == kNull) rt.throw_npe({});
  }
  uint32_t count = rt.array(a.h(0), "ByteBuffer[]").length;
  for (int i = 1; i < 4; ++i) {
    if (rt.array(a.h(i), "array").length != count) rt.throw_new("Ljava/lang/IllegalArgumentException;", "longueurs incohérentes");
  }
  auto set = std::make_shared<DexSet>();
  for (uint32_t i = 0; i < count; ++i) {
    int32_t start = int_at(a.h(2), i), end = int_at(a.h(3), i);
    if (end <= start) rt.throw_new("Ljava/io/IOException;", "Bad range");  // AllocateDexMemoryMap
    std::vector<uint8_t> bytes(size_t(end - start));
    if (Handle array = element(a.h(1), i); array != kNull) {
      rt::ArrayObject& arr = rt.array(array, "byte[]");
      if (start < 0 || uint32_t(end) > arr.length) {  // GetByteArrayRegion
        rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "length=" + std::to_string(arr.length) +
                                                                        "; regionStart=" + std::to_string(start) +
                                                                        "; regionLength=" + std::to_string(end - start));
      }
      std::memcpy(bytes.data(), arr.data.data() + start, bytes.size());
    } else {
      Handle buffer = element(a.h(0), i);
      rt::Field* address = buffer != kNull ? find_field(rt.class_of(buffer), "address", "J") : nullptr;
      int64_t base = 0;
      if (address) {
        rt::Object& o = *rt.object(buffer);
        base = int64_t(uint64_t(o.fields.at(address->slot)) | (uint64_t(o.fields.at(address->slot + 1)) << 32));
      }
      if (base == 0) rt.throw_new("Ljava/io/IOException;", "dexFileBuffer not direct");  // GetDirectBufferAddress
      std::span<uint8_t> src = native_span(rt, base + start, bytes.size());
      std::memcpy(bytes.data(), src.data(), bytes.size());
    }
    // OatFileManager names in-memory DEX files after their memory.
    std::string location = "Anonymous-DexFile@" + std::to_string(i);
    try {
      add_dex_bytes(rt, *set, std::move(bytes), location);
    } catch (const std::exception& e) {
      rt.throw_new("Ljava/io/IOException;", "Failed to open dex files from memory: " + std::string(e.what()));
    }
  }
  if (set->files.empty()) rt.throw_new("Ljava/io/IOException;", "No dex files in memory");
  return cookie_for(rt, set);
}

// libartbase/base/compiler_filter.cc: the filters, in their order of quality.
enum class Filter { kAssumeVerified, kVerify, kSpaceProfile, kSpace, kSpeedProfile, kSpeed, kEverythingProfile, kEverything };

std::optional<Filter> parse_compiler_filter(const std::string& s) {
  static const std::map<std::string, Filter, std::less<>> kNames = {
      {"verify-none", Filter::kAssumeVerified}, {"interpret-only", Filter::kVerify},  // obsolete names
      {"verify-profile", Filter::kVerify},      {"verify-at-runtime", Filter::kVerify},
      {"balanced", Filter::kSpeed},             {"time", Filter::kSpace},
      {"extract", Filter::kVerify},             {"quicken", Filter::kVerify},
      {"assume-verified", Filter::kAssumeVerified}, {"verify", Filter::kVerify},
      {"space", Filter::kSpace},                {"space-profile", Filter::kSpaceProfile},
      {"speed", Filter::kSpeed},                {"speed-profile", Filter::kSpeedProfile},
      {"everything", Filter::kEverything},      {"everything-profile", Filter::kEverythingProfile},
  };
  auto it = kNames.find(s);
  if (it == kNames.end()) return std::nullopt;
  return it->second;
}

const char* filter_name(Filter f) {
  switch (f) {
    case Filter::kAssumeVerified: return "assume-verified";
    case Filter::kVerify: return "verify";
    case Filter::kSpaceProfile: return "space-profile";
    case Filter::kSpace: return "space";
    case Filter::kSpeedProfile: return "speed-profile";
    case Filter::kSpeed: return "speed";
    case Filter::kEverythingProfile: return "everything-profile";
    default: return "everything";
  }
}

bool depends_on_profile(Filter f) {
  return f == Filter::kSpaceProfile || f == Filter::kSpeedProfile || f == Filter::kEverythingProfile;
}

// OatFileAssistant::HasDexFiles: the file holds DEX code (nullopt: unreadable).
std::optional<bool> has_dex_files(const std::string& path) {
  std::vector<uint8_t> bytes;
  try {
    bytes = libcore_vfs().read_file(path, kMaxSourceSize);
  } catch (const vfs::VfsError&) {
    return std::nullopt;
  }
  if (is_dex(bytes)) return true;
  try {
    return apk::ZipArchive::open(std::move(bytes))->find("classes.dex") != nullptr;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// GetInstructionSetFromString: IllegalArgumentException for an unknown name.
void check_instruction_set(Runtime& rt, Handle isa) {
  if (isa == kNull) rt.throw_npe({});
  std::string name = rt.string_utf8(isa);
  if (name != "arm" && name != "arm64" && name != "riscv64" && name != "x86" && name != "x86_64") {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "Instruction set " + name + " is invalid.");
  }
}

void register_dex_file(rt::NativeRegistry& r) {
  const char* df = "Ldalvik/system/DexFile;";
  r.bind(df, "openInMemoryDexFilesNative",
         "([Ljava/nio/ByteBuffer;[[B[I[ILjava/lang/ClassLoader;[Ldalvik/system/DexPathList$Element;)Ljava/lang/Object;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return open_in_memory_dex_files(rt, a); }, S::kPartial,
         "sans fichier vdex anonyme ni vérification du contexte du chargeur");
  // The compiler filters (dalvik_system_DexFile.cc over compiler_filter.cc);
  // the filter itself when it does not change.
  auto filter_predicate = [](bool (*pred)(Filter)) {
    return [pred](Runtime& rt, NativeArgs& a) -> uint64_t {
      if (a.h(0) == kNull) rt.throw_npe({});
      auto f = parse_compiler_filter(rt.string_utf8(a.h(0)));
      return rt::ret_bool(f && pred(*f));
    };
  };
  r.bind(df, "isValidCompilerFilter", "(Ljava/lang/String;)Z", filter_predicate([](Filter) { return true; }));
  r.bind(df, "isProfileGuidedCompilerFilter", "(Ljava/lang/String;)Z", filter_predicate(depends_on_profile));
  r.bind(df, "isVerifiedCompilerFilter", "(Ljava/lang/String;)Z",
         filter_predicate([](Filter f) { return f != Filter::kAssumeVerified; }));
  r.bind(df, "isOptimizedCompilerFilter", "(Ljava/lang/String;)Z",
         filter_predicate([](Filter f) { return f != Filter::kAssumeVerified && f != Filter::kVerify; }));
  auto filter_mapping = [](Filter (*map)(Filter)) {
    return [map](Runtime& rt, NativeArgs& a) -> uint64_t {
      if (a.h(0) == kNull) rt.throw_npe({});
      auto f = parse_compiler_filter(rt.string_utf8(a.h(0)));
      if (!f || map(*f) == *f) return a.h(0);
      return rt.new_string_utf8(filter_name(map(*f)));
    };
  };
  r.bind(df, "getNonProfileGuidedCompilerFilter", "(Ljava/lang/String;)Ljava/lang/String;", filter_mapping([](Filter f) {
    return f == Filter::kSpaceProfile ? Filter::kSpace
           : f == Filter::kSpeedProfile ? Filter::kSpeed
           : f == Filter::kEverythingProfile ? Filter::kEverything
                                             : f;
  }));
  r.bind(df, "getSafeModeCompilerFilter", "(Ljava/lang/String;)Ljava/lang/String;", filter_mapping([](Filter f) {
    return f == Filter::kAssumeVerified || f == Filter::kVerify ? f : Filter::kVerify;
  }));
  // isReadOnlyJavaDclChecked (an application uid) and isReadOnlyJavaDclEnforced:
  // the compatibility change kEnforceReadOnlyJavaDcl, as AppCompatCallbacks decides it.
  r.bind(df, "isReadOnlyJavaDclEnforced", "()Z", [](Runtime& rt, NativeArgs&) -> uint64_t {
    constexpr uint64_t kEnforceReadOnlyJavaDcl = 218865702;
    return rt.call_static("Landroid/compat/Compatibility;", "isChangeEnabled", "(J)Z",
                          {uint32_t(kEnforceReadOnlyJavaDcl), uint32_t(kEnforceReadOnlyJavaDcl >> 32)});
  });
  // The dexopt queries, as OatFileAssistant answers them without any oat or
  // vdex file (AnyAndroid interprets the DEX code and never compiles it).
  auto existing_file = [](Runtime& rt, Handle name) {
    if (name == kNull) rt.throw_npe({});
    std::string path = rt.string_utf8(name);
    if (!libcore_vfs().exists(path)) rt.throw_new("Ljava/io/FileNotFoundException;", path);
    return path;
  };
  r.bind(df, "getDexOptNeeded", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZZ)I",
         [existing_file](Runtime& rt, NativeArgs& a) -> uint64_t {
           std::string path = existing_file(rt, a.h(0));
           check_instruction_set(rt, a.h(1));
           if (a.h(2) == kNull) rt.throw_npe({});
           std::string filter = rt.string_utf8(a.h(2));
           if (!parse_compiler_filter(filter)) {
             rt.throw_new("Ljava/lang/IllegalArgumentException;", "Compiler filter " + filter + " is invalid.");
           }
           if (a.z(5)) return 0;  // downgrade: kNoDexOptNeeded without a usable oat file
           auto dex = has_dex_files(path);
           return rt::ret_int(dex && *dex ? 1 : 0);  // kDex2OatFromScratch
         }, S::kPartial, "aucun fichier oat ; le contexte du chargeur n'est pas analysé");
  r.bind(df, "isDexOptNeeded", "(Ljava/lang/String;)Z", [existing_file](Runtime& rt, NativeArgs& a) -> uint64_t {
    existing_file(rt, a.h(0));
    return rt::ret_bool(true);  // never up to date without an oat file
  });
  r.bind(df, "getDexFileOutputPaths", "(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe({});
           check_instruction_set(rt, a.h(1));
           return kNull;  // no best oat file
         });
  r.bind(df, "getDexFileOptimizationStatus", "(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe({});
           check_instruction_set(rt, a.h(1));
           auto dex = has_dex_files(rt.string_utf8(a.h(0)));
           return string_array(rt, {dex && *dex ? "run-from-apk" : "unknown", "unknown"});
         });
  r.bind(df, "openDexFileNative",
         "(Ljava/lang/String;Ljava/lang/String;ILjava/lang/ClassLoader;[Ldalvik/system/DexPathList$Element;)"
         "Ljava/lang/Object;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return open_dex_file(rt, a); }, S::kPartial,
         "fichiers lus par le VFS, sans fichier oat ni vérification du contexte du chargeur");
  r.bind(df, "getClassNameList", "(Ljava/lang/Object;)[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto set = dex_set(rt, a.h(0));
    std::set<std::string> descriptors;
    for (const dex::DexFile* d : set->files) {
      for (uint32_t i = 0; i < d->num_class_defs(); ++i) descriptors.insert(d->type_descriptor(d->class_def(i).class_idx));
    }
    std::vector<std::string> names;
    for (const auto& desc : descriptors) names.push_back(text::descriptor_to_java(desc));
    return string_array(rt, names);
  });
  r.bind(df, "defineClassNative",
         "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Object;Ldalvik/system/DexFile;)Ljava/lang/Class;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe("name");
           std::string desc = binary_name_descriptor(rt.string_utf8(a.h(0)));
           auto set = dex_set(rt, a.h(2));
           if (desc.empty() || desc[0] == '[') return kNull;
           rt::Class* c = define_from_set(rt, *set, desc, a.h(1));
           return c ? rt.class_mirror(c) : kNull;
         },
         S::kPartial, "un seul espace de classes : une classe déjà définie est partagée");
  r.bind(df, "closeDexFile", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull || rt.class_of(a.h(0))->descriptor != "[J") return rt::ret_bool(false);
    rt::ArrayObject& arr = rt.array(a.h(0), "cookie");
    if (arr.length < 2) return rt::ret_bool(false);
    int64_t h;
    std::memcpy(&h, arr.data.data() + 8, 8);
    // The DEX files stay: classes defined from them keep running.
    return rt::ret_bool(tables().sets.remove(h));
  });
  r.bind(df, "isBackedByOatFile", "(Ljava/lang/Object;)Z", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_bool(false);
  });
  r.bind(df, "getStaticSizeOfDexFile", "(Ljava/lang/Object;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint64_t size = 0;
    for (const dex::DexFile* d : dex_set(rt, a.h(0))->files) size += d->bytes().size();
    return size;
  });
  r.bind(df, "setTrusted", "(Ljava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    dex_set(rt, a.h(0));
    return 0;
  }, S::kPartial, "pas de restriction des API cachées à lever");
  // Class loader contexts are reported to dexopt only: nothing to report.
  r.bind("Ldalvik/system/BaseDexClassLoader;", "computeClassLoaderContextsNative", "()[Ljava/lang/String;",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return string_array(rt, {}); }, S::kPartial,
         "pas de dexopt : aucun contexte à signaler");
  // The linker namespace of an application's native libraries (G4): none yet,
  // which is a success (null).
  r.bind("Lcom/android/internal/os/ClassLoaderFactory;", "createClassloaderNamespace",
         "(Ljava/lang/ClassLoader;ILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;)"
         "Ljava/lang/String;",
         [](Runtime&, NativeArgs&) -> uint64_t { return kNull; }, S::kPartial,
         "pas encore d'espace de noms du lieur (bibliothèques natives : G4)");
  r.bind(df, "verifyInBackgroundNative", "(Ljava/lang/Object;Ljava/lang/ClassLoader;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           dex_set(rt, a.h(0));
           return 0;
         }, S::kPartial, "vérification faite au chargement de chaque classe");
}

}  // namespace

rt::Handle class_loader_of(rt::Runtime& rt, rt::Class* cls, bool boot_as_null) {
  while (cls->component) cls = cls->component;
  if (cls->primitive) return kNull;
  if (Runtime::in_boot_space(cls)) {
    if (boot_as_null) return kNull;
    // Android (unlike the RI) answers BootClassLoader.getInstance() for the
    // boot class path; without libcore there is no such loader.
    if (rt.libcore_mode() && rt.lookup_class("Ljava/lang/BootClassLoader;")) {
      return rt.call_static("Ljava/lang/BootClassLoader;", "getInstance", "()Ljava/lang/BootClassLoader;");
    }
    return kNull;
  }
  if (cls->dex_cache->loader != kNull) return cls->dex_cache->loader;
  // Not registered by any loader yet: loaded by the runtime itself (the main
  // class of `exec`), as the system class loader would have.
  return rt.call_static("Ljava/lang/ClassLoader;", "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
}

rt::Class* find_loaded_class(rt::Runtime& rt, rt::Handle loader, const std::string& name) {
  std::string desc = binary_name_descriptor(name);
  if (desc.empty()) return nullptr;
  if (is_boot_loader(rt, loader)) return rt.lookup_boot_class(desc);
  if (rt::Class* c = rt.loaded_class(desc)) {
    if (Runtime::in_boot_space(c) || class_loader_of(rt, c, true) == loader) return c;
  }
  // Like ART, resolve through the standard loaders directly, so that
  // loadClass() does not go through a ClassNotFoundException per class.
  return find_in_loader_chain(rt, desc, loader, 0).cls;
}

rt::Class* class_for_name(rt::Runtime& rt, rt::Handle name, rt::Handle loader) {
  std::string n = rt.string_utf8(name);
  std::string desc = binary_name_descriptor(n);
  if (desc.empty()) rt.throw_new("Ljava/lang/ClassNotFoundException;", n);
  rt::Class* c = nullptr;
  try {
    if (!rt.libcore_mode()) {
      c = rt.lookup_class(desc);  // bridge mode: no loaders
    } else {
      Lookup r = find_in_loader_chain(rt, desc, loader, 0);
      if (r.supported) {
        c = r.cls;
      } else {
        // Any other loader: its loadClass(), as ART's ClassLinker::FindClass.
        Handle m = Handle(rt.call_virtual(loader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", {name}));
        c = m == kNull ? nullptr : rt.mirror_class(m);
      }
    }
  } catch (const rt::JavaThrow& t) {
    if (!rt.libcore_mode()) rt.throw_new("Ljava/lang/ClassNotFoundException;", n);
    // As ART: a ClassNotFoundException naming the class, caused by the error.
    rt::GlobalRef cause(rt, t.exception);
    Handle cnfe = construct(rt, "Ljava/lang/ClassNotFoundException;", "(Ljava/lang/String;Ljava/lang/Throwable;)V",
                            {name, t.exception});
    throw rt::JavaThrow{cnfe};
  }
  if (!c || c->primitive) rt.throw_new("Ljava/lang/ClassNotFoundException;", n);
  return c;
}

void register_libcore_dex(rt::NativeRegistry& r) { register_dex_file(r); }

}  // namespace aa::bridge
