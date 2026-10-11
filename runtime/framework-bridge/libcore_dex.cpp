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
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "apk-inspector/zip_reader.h"
#include "common/text.h"
#include "framework-bridge/handles.h"

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

bool same_bytes(const dex::DexFile* d, const std::vector<uint8_t>& b) {
  auto own = d->bytes();
  return own.size() == b.size() && std::equal(own.begin(), own.end(), b.begin());
}

// Adds the DEX file(s) of `bytes` to the set, reusing a file the runtime
// already has with the same bytes: the application's, added at start-up, or
// one an earlier loader opened.
void add_dex_bytes(Runtime& rt, DexSet& set, std::vector<uint8_t> bytes, const std::string& location) {
  std::vector<const dex::DexFile*> known = rt.app_dex_files();
  for (const dex::DexFile* d : known) {
    if (same_bytes(d, bytes)) {
      set.files.push_back(d);
      return;
    }
  }
  auto parts = dex::DexFile::open_all(std::move(bytes), location);  // FormatError on a malformed file
  Tables& t = tables();
  for (auto& part : parts) {
    auto own = part->bytes();
    auto same = std::find_if(known.begin(), known.end(), [&](const dex::DexFile* d) {
      auto b = d->bytes();
      return b.size() == own.size() && std::equal(b.begin(), b.end(), own.begin());
    });
    if (same != known.end()) {
      set.files.push_back(*same);
      continue;
    }
    std::lock_guard<std::mutex> guard(t.lock);
    if (t.owned_bytes + own.size() > kMaxOpenedBytes) {
      throw std::runtime_error("[AnyAndroid] quota des fichiers DEX chargés atteint");
    }
    t.owned_bytes += own.size();
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
  if (tables().sets.live() >= kMaxDexSets) rt.throw_new("Ljava/io/IOException;", "[AnyAndroid] trop de DexFile ouverts");
  int64_t h = tables().sets.add(set);
  Handle cookie = rt.new_array("[J", 2);
  std::memcpy(rt.array(cookie, "cookie").data.data() + 8, &h, 8);
  return cookie;
}

void register_dex_file(rt::NativeRegistry& r) {
  const char* df = "Ldalvik/system/DexFile;";
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
