// Natives of android.content.res (ApkAssets, AssetManager, StringBlock,
// XmlBlock) over aa::res, the AnyAndroid counterpart of libandroidfw's
// AssetManager2. APKs are read through the process's VFS (/system/framework/
// framework-res.apk, /data/app/...). Native objects are HandleTable handles.
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/text.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"
#include "resources/asset_manager.h"
#include "vfs/vfs.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr uint64_t kMaxApkSize = 1ull << 30;

struct ApkHolder {
  std::shared_ptr<res::ApkAssets> apk;
  int64_t pool = 0;  // StringBlock handle of the value pool, created on demand
};

struct ManagerHolder {
  res::AssetManager am;
};

struct ThemeHolder {
  std::shared_ptr<ManagerHolder> manager;  // keeps the AssetManager alive
  std::unique_ptr<res::Theme> theme;
};

struct TreeHolder {
  std::shared_ptr<res::XmlTree> tree;
  int64_t pool = 0;
};

struct AssetStream {
  std::vector<uint8_t> data;
  uint64_t pos = 0;
};

struct Tables {
  HandleTable<ApkHolder> apks{0x0a01};
  HandleTable<ManagerHolder> managers{0x0a02};
  HandleTable<ThemeHolder> themes{0x0a03};
  HandleTable<res::StringPool> pools{0x0a04};
  HandleTable<TreeHolder> trees{0x0a05};
  HandleTable<res::XmlParser> parsers{0x0a06};
  HandleTable<AssetStream> assets{0x0a07};
};

Tables& tables() {
  static Tables t;
  return t;
}

template <typename T>
std::shared_ptr<T> need(Runtime& rt, const HandleTable<T>& table, int64_t handle, const char* what) {
  auto v = table.get(handle);
  if (!v) rt.throw_new("Ljava/lang/IllegalStateException;", std::string(what) + " natif invalide ou fermé");
  return v;
}

std::shared_ptr<ManagerHolder> manager(Runtime& rt, int64_t h) { return need(rt, tables().managers, h, "AssetManager"); }
std::shared_ptr<res::XmlParser> parser(Runtime& rt, int64_t h) { return need(rt, tables().parsers, h, "XmlBlock.Parser"); }

[[noreturn]] void file_not_found(Runtime& rt, const std::string& path) {
  rt.throw_new("Ljava/io/FileNotFoundException;", path);
}

// --- Java objects -------------------------------------------------------------------------

void set_int_field(Runtime& rt, Handle obj, std::string_view name, int32_t v) {
  rt::Object& o = rt.nonnull(obj, "objet");
  rt::Field* f = find_field(o.klass, name, "I");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/InternalError;", "champ " + std::string(name) + " introuvable");
  o.fields.at(f->slot) = uint32_t(v);
}

int64_t long_field(Runtime& rt, Handle obj, std::string_view name) {
  rt::Object& o = rt.nonnull(obj, "objet");
  rt::Field* f = find_field(o.klass, name, "J");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/InternalError;", "champ " + std::string(name) + " introuvable");
  return int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
}

// AssetManager's CopyValue: fills a TypedValue and returns the Java cookie.
int32_t copy_value(Runtime& rt, const res::SelectedValue& v, Handle typed_value) {
  rt::Object& o = rt.nonnull(typed_value, "TypedValue");
  set_int_field(rt, typed_value, "type", v.type);
  set_int_field(rt, typed_value, "data", int32_t(v.data));
  set_int_field(rt, typed_value, "assetCookie", res::java_cookie(v.cookie));
  set_int_field(rt, typed_value, "resourceId", int32_t(v.resid));
  set_int_field(rt, typed_value, "changingConfigurations", int32_t(v.flags));
  set_int_field(rt, typed_value, "density", v.density);
  if (rt::Field* s = find_field(o.klass, "string", "Ljava/lang/CharSequence;")) o.fields.at(s->slot) = kNull;
  return res::java_cookie(v.cookie);
}

std::vector<uint32_t> int_array(Runtime& rt, Handle h, const char* what) {
  rt::ArrayObject& a = rt.array(h, what);
  if (a.width != 4) rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string(what) + " : int[] attendu");
  std::vector<uint32_t> out(a.length);
  if (a.length) std::memcpy(out.data(), a.data.data(), size_t(a.length) * 4);
  return out;
}

Handle new_int_array(Runtime& rt, const std::vector<uint32_t>& values) {
  Handle h = rt.new_array("[I", int32_t(values.size()));
  if (!values.empty()) std::memcpy(rt.array(h, "int[]").data.data(), values.data(), values.size() * 4);
  return h;
}

// Writable view of a Java int[] (by handle or by VMRuntime.addressOf value).
std::span<uint32_t> int_span(rt::ArrayObject& a, const char* what, Runtime& rt) {
  if (a.width != 4) rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string(what) + " : int[] attendu");
  return {reinterpret_cast<uint32_t*>(a.data.data()), a.length};
}

Handle java_string(Runtime& rt, const std::optional<std::u16string>& s) { return s ? rt.new_string(*s) : kNull; }

std::vector<uint8_t> byte_range(Runtime& rt, Handle arr, int32_t off, int32_t len) {
  rt::ArrayObject& a = rt.array(arr, "byte[]");
  if (a.width != 1 || off < 0 || len < 0 || int64_t(off) + len > a.length) {
    rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "plage d'octets invalide");
  }
  return std::vector<uint8_t>(a.data.begin() + off, a.data.begin() + off + len);
}

// Reads a whole file of the emulated process (path or open descriptor).
std::vector<uint8_t> read_path(Runtime& rt, const std::string& path) {
  try {
    return libcore_vfs().read_file(path, kMaxApkSize);
  } catch (const vfs::VfsError&) {
    rt.throw_new("Ljava/io/IOException;", "Failed to load asset path " + path);
  }
}

std::vector<uint8_t> read_fd(Runtime& rt, int32_t fd, uint64_t offset, uint64_t length, bool whole) {
  try {
    vfs::Vfs& v = libcore_vfs();
    uint64_t size = v.fstat(fd).size;
    if (whole) {
      offset = 0;
      length = size;
    }
    if (offset > size || length > size - offset || length > kMaxApkSize) {
      rt.throw_new("Ljava/io/IOException;", "plage de fichier invalide");
    }
    std::vector<uint8_t> out(size_t(length), 0);
    size_t done = 0;
    while (done < out.size()) {
      size_t n = v.pread(fd, std::span<uint8_t>(out).subspan(done), offset + done);
      if (n == 0) break;
      done += n;
    }
    out.resize(done);
    return out;
  } catch (const vfs::VfsError& e) {
    throw_errno(rt, "read", e.err);
  }
}

// --- ApkAssets ------------------------------------------------------------------------------

constexpr int32_t kFormatApk = 0, kFormatArsc = 2;

int64_t load_apk_assets(Runtime& rt, int32_t format, const std::string& path, std::vector<uint8_t> bytes,
                        int32_t flags) {
  std::shared_ptr<res::ApkAssets> apk;
  try {
    if (format == kFormatApk) {
      apk = res::ApkAssets::load_apk(path, std::move(bytes), uint32_t(flags));
    } else if (format == kFormatArsc) {
      apk = res::ApkAssets::load_table(path, std::move(bytes), uint32_t(flags));
    } else {
      rt.throw_new("Ljava/io/IOException;", "format d'ApkAssets non pris en charge (" + std::to_string(format) + ") : " + path);
    }
  } catch (const FormatError& e) {
    rt.throw_new("Ljava/io/IOException;", "Failed to load asset path " + path + " (" + e.what() + ")");
  }
  auto holder = std::make_shared<ApkHolder>();
  holder->apk = std::move(apk);
  return tables().apks.add(std::move(holder));
}

void register_apk_assets(rt::NativeRegistry& r) {
  const char* c = "Landroid/content/res/ApkAssets;";
  r.bind(c, "nativeLoad", "(ILjava/lang/String;ILandroid/content/res/loader/AssetsProvider;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           std::string path = rt.string_utf8(a.h(1));
           return uint64_t(load_apk_assets(rt, a.i32(0), path, read_path(rt, path), a.i32(2)));
         }, S::kPartial, "APK et resources.arsc ; pas de dossier ni d'overlay");
  r.bind(c, "nativeLoadEmpty", "(ILandroid/content/res/loader/AssetsProvider;)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto holder = std::make_shared<ApkHolder>();
    holder->apk = res::ApkAssets::load_empty("empty", uint32_t(a.i32(0)));
    return uint64_t(tables().apks.add(std::move(holder)));
  });
  r.bind(c, "nativeLoadFd", "(ILjava/io/FileDescriptor;Ljava/lang/String;ILandroid/content/res/loader/AssetsProvider;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto bytes = read_fd(rt, fd_of(rt, a.h(1)), 0, 0, true);
           return uint64_t(load_apk_assets(rt, a.i32(0), rt.string_utf8(a.h(2)), std::move(bytes), a.i32(3)));
         });
  r.bind(c, "nativeLoadFdOffsets",
         "(ILjava/io/FileDescriptor;Ljava/lang/String;JJILandroid/content/res/loader/AssetsProvider;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int64_t offset = a.i64(3), length = a.i64(5);
           if (offset < 0 || length < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "plage négative");
           auto bytes = read_fd(rt, fd_of(rt, a.h(1)), uint64_t(offset), uint64_t(length), false);
           return uint64_t(load_apk_assets(rt, a.i32(0), rt.string_utf8(a.h(2)), std::move(bytes), a.i32(7)));
         });
  r.bind(c, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    if (auto h = tables().apks.get(a.i64(0))) {
      if (h->pool) tables().pools.remove(h->pool);
      tables().apks.remove(a.i64(0));
    }
    return 0;
  });
  auto path = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.new_string_utf8(need(rt, tables().apks, a.i64(0), "ApkAssets")->apk->path());
  };
  r.bind(c, "nativeGetAssetPath", "(J)Ljava/lang/String;", path);
  r.bind(c, "nativeGetDebugName", "(J)Ljava/lang/String;", path);
  r.bind(c, "nativeGetStringBlock", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto h = need(rt, tables().apks, a.i64(0), "ApkAssets");
    if (!h->pool) {
      if (!h->apk->table()) return 0;
      h->pool = tables().pools.add(h->apk->table()->values());
    }
    return uint64_t(h->pool);
  });
  // 2 = always up to date: APKs of the emulated system do not change.
  r.bind(c, "nativeIsUpToDate", "(J)I", [](Runtime&, NativeArgs&) -> uint64_t { return 2; });
  r.bind(c, "nativeOpenXml", "(JLjava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto h = need(rt, tables().apks, a.i64(0), "ApkAssets");
    std::string file = rt.string_utf8(a.h(2));
    auto data = h->apk->open(file);
    if (!data) file_not_found(rt, file);
    uint8_t pkg = 0x7f;
    if (h->apk->table() && !h->apk->table()->packages().empty()) pkg = uint8_t(h->apk->table()->packages()[0]->id);
    try {
      auto tree = std::make_shared<TreeHolder>();
      tree->tree = res::XmlTree::parse(std::move(*data), pkg ? pkg : 0x7f);
      return uint64_t(tables().trees.add(std::move(tree)));
    } catch (const FormatError&) {
      rt.throw_new("Ljava/io/FileNotFoundException;", "Corrupt XML binary file");
    }
  });
  r.bind(c, "nativeDefinesOverlayable", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(c, "nativeGetOverlayableInfo", "(JLjava/lang/String;)Landroid/content/om/OverlayableInfo;",
         [](Runtime&, NativeArgs&) -> uint64_t { return kNull; }, S::kStub, "pas d'overlays");
}

// --- AssetManager: configuration and lookups -------------------------------------------------

void register_asset_manager(rt::NativeRegistry& r) {
  const char* c = "Landroid/content/res/AssetManager;";
  r.bind(c, "nativeCreate", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return uint64_t(tables().managers.add(std::make_shared<ManagerHolder>()));
  });
  r.bind(c, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().managers.remove(a.i64(0));
    return 0;
  });
  r.bind(c, "getGlobalAssetManagerCount", "()I", [](Runtime&, NativeArgs&) -> uint64_t {
    return uint32_t(tables().managers.live());
  });
  r.bind(c, "getGlobalAssetCount", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return uint32_t(tables().assets.live()); });
  r.bind(c, "getAssetAllocations", "()Ljava/lang/String;", [](Runtime&, NativeArgs&) -> uint64_t { return kNull; });
  r.bind(c, "nativeContainsAllocatedTable", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  r.bind(c, "nativeSetApkAssets", "(J[Landroid/content/res/ApkAssets;ZZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    rt::ArrayObject& arr = rt.array(a.h(2), "ApkAssets[]");
    std::vector<std::shared_ptr<res::ApkAssets>> apks;
    for (uint32_t i = 0; i < arr.length; ++i) {
      Handle e = 0;
      std::memcpy(&e, arr.data.data() + size_t(i) * 4, 4);
      if (e == kNull) rt.throw_npe("ApkAssets at index " + std::to_string(i) + " is null");
      apks.push_back(need(rt, tables().apks, long_field(rt, e, "mNativePtr"), "ApkAssets")->apk);
    }
    m->am.set_apk_assets(std::move(apks));
    return 0;
  });

  r.bind(c, "nativeSetConfiguration", "(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIIZ)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto m = manager(rt, a.i64(0));
           res::ResConfig base;
           base.mcc = uint16_t(a.i32(2));
           base.mnc = uint16_t(a.i32(3));
           base.orientation = uint8_t(a.i32(6));
           base.touchscreen = uint8_t(a.i32(7));
           base.density = uint16_t(a.i32(8));
           base.keyboard = uint8_t(a.i32(9));
           base.input_flags = uint8_t(a.i32(10));
           base.navigation = uint8_t(a.i32(11));
           base.screen_width = uint16_t(a.i32(12));
           base.screen_height = uint16_t(a.i32(13));
           base.smallest_screen_width_dp = uint16_t(a.i32(14));
           base.screen_width_dp = uint16_t(a.i32(15));
           base.screen_height_dp = uint16_t(a.i32(16));
           int32_t layout = a.i32(17);
           base.screen_layout = uint8_t(layout);
           base.screen_layout2 = uint8_t((layout & 0x300) >> 8);  // round qualifier
           base.ui_mode = uint8_t(a.i32(18));
           base.color_mode = uint8_t(a.i32(19));
           base.grammatical_inflection = uint8_t(a.i32(20));
           base.sdk_version = uint16_t(a.i32(21));
           base.minor_version = uint16_t(a.i32(22));
           std::vector<res::ResConfig> configs;
           if (a.h(5) != kNull) {
             rt::ArrayObject& locales = rt.array(a.h(5), "String[]");
             for (uint32_t i = 0; i < locales.length; ++i) {
               Handle s = 0;
               std::memcpy(&s, locales.data.data() + size_t(i) * 4, 4);
               res::ResConfig cfg = base;
               if (s != kNull) cfg.set_bcp47_locale(rt.string_utf8(s));
               configs.push_back(cfg);
             }
           }
           if (configs.empty()) configs.push_back(base);
           m->am.set_configurations(std::move(configs));
           return 0;
         });

  r.bind(c, "nativeGetAssignedPackageIdentifiers", "(JZZ)Landroid/util/SparseArray;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto m = manager(rt, a.i64(0));
           Handle sa = construct(rt, "Landroid/util/SparseArray;", "()V", {});
           rt::GlobalRef keep(rt, sa);
           for (const auto& [id, name] : m->am.packages()) {
             Handle s = rt.new_string(name);
             rt.call_virtual(sa, "put", "(ILjava/lang/Object;)V", {id, s});
           }
           return sa;
         });

  r.bind(c, "nativeGetResourceValue", "(JISLandroid/util/TypedValue;Z)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto v = m->am.get_resource(uint32_t(a.i32(2)), false, uint16_t(a.i32(3)));
    if (!v) return rt::ret_int(-1);
    if (a.z(5) && !m->am.resolve_reference(*v)) return rt::ret_int(-1);
    return rt::ret_int(copy_value(rt, *v, a.h(4)));
  });
  r.bind(c, "nativeGetResourceBagValue", "(JIILandroid/util/TypedValue;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto bag = m->am.get_bag(uint32_t(a.i32(2)));
    if (!bag) return rt::ret_int(-1);
    const res::BagEntry* e = bag->find(uint32_t(a.i32(3)));
    if (!e) return rt::ret_int(-1);
    res::SelectedValue v;
    v.type = e->value.type;
    v.data = e->value.data;
    v.cookie = e->cookie;
    v.flags = bag->type_spec_flags;
    if (!m->am.resolve_reference(v)) return rt::ret_int(-1);
    return rt::ret_int(copy_value(rt, v, a.h(4)));
  });
  r.bind(c, "nativeGetStyleAttributes", "(JI)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto bag = manager(rt, a.i64(0))->am.get_bag(uint32_t(a.i32(2)));
    if (!bag) return kNull;
    std::vector<uint32_t> keys;
    for (const auto& e : bag->entries) keys.push_back(e.key);
    return new_int_array(rt, keys);
  });
  r.bind(c, "nativeGetParentThemeIdentifier", "(JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto bag = manager(rt, a.i64(0))->am.get_bag(uint32_t(a.i32(2)));
    return bag ? bag->parent : 0;
  });

  // Arrays (<array>, <string-array>, <integer-array>).
  r.bind(c, "nativeGetResourceArraySize", "(JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto bag = manager(rt, a.i64(0))->am.get_bag(uint32_t(a.i32(2)));
    return rt::ret_int(bag ? int32_t(bag->entries.size()) : -1);
  });
  r.bind(c, "nativeGetResourceArray", "(JI[I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto bag = m->am.get_bag(uint32_t(a.i32(2)));
    if (!bag) return rt::ret_int(-1);
    rt::ArrayObject& out = rt.array(a.h(3), "int[]");
    auto values = int_span(out, "outData", rt);
    if (bag->entries.size() * res::kStyleNumEntries > values.size()) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "Input array is not large enough");
    }
    size_t i = 0;
    for (const auto& e : bag->entries) {
      res::SelectedValue v;
      v.type = e.value.type;
      v.data = e.value.data;
      v.cookie = e.cookie;
      v.flags = bag->type_spec_flags;
      if (!m->am.resolve_reference(v)) return rt::ret_int(-1);
      if (v.type == res::kTypeReference && v.data == 0) {
        v.type = res::kTypeNull;
        v.data = res::kDataNullUndefined;
      }
      uint32_t* cur = values.data() + i * res::kStyleNumEntries;
      cur[0] = v.type;
      cur[1] = v.data;
      cur[2] = uint32_t(res::java_cookie(v.cookie));
      cur[3] = v.resid;
      cur[4] = v.flags;
      cur[5] = v.density;
      cur[6] = 0;
      ++i;
    }
    return rt::ret_int(int32_t(bag->entries.size()));
  });
  r.bind(c, "nativeGetResourceStringArray", "(JI)[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto bag = m->am.get_bag(uint32_t(a.i32(2)));
    if (!bag) return kNull;
    Handle arr = rt.new_array("[Ljava/lang/String;", int32_t(bag->entries.size()));
    rt::GlobalRef keep(rt, arr);
    for (size_t i = 0; i < bag->entries.size(); ++i) {
      const auto& e = bag->entries[i];
      res::SelectedValue v{e.value.type, e.value.data, e.cookie, bag->type_spec_flags, 0, 0};
      if (!m->am.resolve_reference(v)) return kNull;
      if (v.type != res::kTypeString) continue;
      auto pool = m->am.string_pool(v.cookie);
      Handle s = pool ? java_string(rt, pool->string16(v.data)) : kNull;
      std::memcpy(rt.array(arr, "String[]").data.data() + i * 4, &s, 4);
    }
    return arr;
  });
  r.bind(c, "nativeGetResourceStringArrayInfo", "(JI)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto bag = m->am.get_bag(uint32_t(a.i32(2)));
    if (!bag) return kNull;
    std::vector<uint32_t> out;
    for (const auto& e : bag->entries) {
      res::SelectedValue v{e.value.type, e.value.data, e.cookie, bag->type_spec_flags, 0, 0};
      if (!m->am.resolve_reference(v)) return kNull;
      out.push_back(uint32_t(res::java_cookie(v.cookie)));
      out.push_back(v.type == res::kTypeString ? v.data : uint32_t(-1));
    }
    return new_int_array(rt, out);
  });
  r.bind(c, "nativeGetResourceIntArray", "(JI)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto bag = m->am.get_bag(uint32_t(a.i32(2)));
    if (!bag) return kNull;
    std::vector<uint32_t> out;
    for (const auto& e : bag->entries) {
      res::SelectedValue v{e.value.type, e.value.data, e.cookie, bag->type_spec_flags, 0, 0};
      if (!m->am.resolve_reference(v)) return kNull;
      out.push_back(v.type >= res::kTypeFirstInt && v.type <= res::kTypeLastInt ? v.data : 0);
    }
    return new_int_array(rt, out);
  });

  // Names.
  r.bind(c, "nativeGetResourceIdentifier", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto m = manager(rt, a.i64(0));
           if (a.h(2) == kNull) rt.throw_npe("name");
           std::string type = a.h(3) ? rt.string_utf8(a.h(3)) : "";
           std::string package = a.h(4) ? rt.string_utf8(a.h(4)) : "";
           return m->am.resource_id(rt.string_utf8(a.h(2)), type, package);
         });
  r.bind(c, "nativeGetResourceName", "(JI)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = manager(rt, a.i64(0))->am.resource_name(uint32_t(a.i32(2)));
    if (!n) return kNull;
    std::u16string s = n->package;
    if (!n->type.empty()) s += (s.empty() ? u"" : u":") + n->type;
    if (!n->entry.empty()) s += (s.empty() ? u"" : u"/") + n->entry;
    return rt.new_string(s);
  });
  r.bind(c, "nativeGetResourcePackageName", "(JI)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = manager(rt, a.i64(0))->am.resource_name(uint32_t(a.i32(2)));
    return n && !n->package.empty() ? rt.new_string(n->package) : kNull;
  });
  r.bind(c, "nativeGetResourceTypeName", "(JI)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = manager(rt, a.i64(0))->am.resource_name(uint32_t(a.i32(2)));
    return n && !n->type.empty() ? rt.new_string(n->type) : kNull;
  });
  r.bind(c, "nativeGetResourceEntryName", "(JI)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = manager(rt, a.i64(0))->am.resource_name(uint32_t(a.i32(2)));
    return n && !n->entry.empty() ? rt.new_string(n->entry) : kNull;
  });
  r.bind(c, "nativeGetLocales", "(JZ)[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return string_array(rt, manager(rt, a.i64(0))->am.locales(a.z(2)));
  });
  r.bind(c, "nativeGetResourceConfigurations", "(J)[Landroid/content/res/Configuration;",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return rt.new_array("[Landroid/content/res/Configuration;", 0); },
         S::kStub, "liste vide");
  r.bind(c, "nativeGetLastResourceResolution", "(J)Ljava/lang/String;", [](Runtime&, NativeArgs&) -> uint64_t { return kNull; });
  r.bind(c, "nativeSetResourceResolutionLoggingEnabled", "(JZ)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(c, "nativeGetOverlayableMap", "(JLjava/lang/String;)Ljava/util/Map;", [](Runtime&, NativeArgs&) -> uint64_t {
    return kNull;
  }, S::kStub, "pas d'overlays");
  r.bind(c, "nativeGetOverlayablesToString", "(JLjava/lang/String;)Ljava/lang/String;",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return rt.new_string(u""); }, S::kStub, "pas d'overlays");
  r.bind(c, "nativeSetOverlayConstraints", "(JII)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
}

// --- AssetManager: files and assets ------------------------------------------------------------

int64_t open_stream(Runtime& rt, std::optional<std::vector<uint8_t>> data, const std::string& path) {
  if (!data) file_not_found(rt, path);
  auto s = std::make_shared<AssetStream>();
  s->data = std::move(*data);
  return tables().assets.add(std::move(s));
}

int64_t open_tree(Runtime& rt, const res::AssetManager& am, std::vector<uint8_t> data, int32_t cookie) {
  try {
    auto tree = std::make_shared<TreeHolder>();
    tree->tree = res::XmlTree::parse(std::move(data), am.package_id_of_cookie(cookie));
    return tables().trees.add(std::move(tree));
  } catch (const FormatError&) {
    rt.throw_new("Ljava/io/FileNotFoundException;", "Corrupt XML binary file");
  }
}

void register_asset_files(rt::NativeRegistry& r) {
  const char* c = "Landroid/content/res/AssetManager;";
  r.bind(c, "nativeList", "(JLjava/lang/String;)[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    return string_array(rt, m->am.list_assets(rt.string_utf8(a.h(2))));
  });
  r.bind(c, "nativeOpenAsset", "(JLjava/lang/String;I)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    std::string name = rt.string_utf8(a.h(2));
    return uint64_t(open_stream(rt, m->am.open_asset(name, nullptr), name));
  });
  r.bind(c, "nativeOpenNonAsset", "(JILjava/lang/String;I)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    std::string name = rt.string_utf8(a.h(3));
    return uint64_t(open_stream(rt, m->am.open_non_asset(name, a.i32(2) > 0 ? a.i32(2) - 1 : -1, nullptr), name));
  });
  auto no_fd = [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt.throw_new("Ljava/io/FileNotFoundException;",
                 "This file can not be opened as a file descriptor; it is probably compressed");
  };
  r.bind(c, "nativeOpenAssetFd", "(JLjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;", no_fd, S::kStub,
         "pas de descripteur sur une entrée d'APK");
  r.bind(c, "nativeOpenNonAssetFd", "(JILjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;", no_fd, S::kStub,
         "pas de descripteur sur une entrée d'APK");
  r.bind(c, "nativeOpenXmlAsset", "(JILjava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    std::string name = rt.string_utf8(a.h(3));
    int32_t cookie = a.i32(2) > 0 ? a.i32(2) - 1 : -1;
    auto data = m->am.open_non_asset(name, cookie, &cookie);
    if (!data) file_not_found(rt, name);
    return uint64_t(open_tree(rt, m->am, std::move(*data), cookie));
  });
  r.bind(c, "nativeOpenXmlAssetFd", "(JILjava/io/FileDescriptor;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    return uint64_t(open_tree(rt, m->am, read_fd(rt, fd_of(rt, a.h(3)), 0, 0, true), a.i32(2) > 0 ? a.i32(2) - 1 : -1));
  });

  r.bind(c, "nativeAssetDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().assets.remove(a.i64(0));
    return 0;
  });
  r.bind(c, "nativeAssetReadChar", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = need(rt, tables().assets, a.i64(0), "Asset");
    if (s->pos >= s->data.size()) return rt::ret_int(-1);
    return s->data[size_t(s->pos++)];
  });
  r.bind(c, "nativeAssetRead", "(J[BII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = need(rt, tables().assets, a.i64(0), "Asset");
    int32_t off = a.i32(3), len = a.i32(4);
    if (len == 0) return 0;
    rt::ArrayObject& b = rt.array(a.h(2), "byte[]");
    int32_t blen = int32_t(b.length);
    if (b.width != 1 || off < 0 || off >= blen || len < 0 || len > blen || off > blen - len) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "");
    }
    if (s->pos >= s->data.size()) return rt::ret_int(-1);
    size_t n = std::min<size_t>(size_t(len), s->data.size() - size_t(s->pos));
    std::memcpy(b.data.data() + off, s->data.data() + s->pos, n);
    s->pos += n;
    return uint32_t(n);
  });
  r.bind(c, "nativeAssetSeek", "(JJI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = need(rt, tables().assets, a.i64(0), "Asset");
    int64_t offset = a.i64(2);
    int32_t whence = a.i32(4);
    int64_t base = whence > 0 ? int64_t(s->data.size()) : whence < 0 ? 0 : int64_t(s->pos);
    int64_t target = base + offset;
    if (target < 0 || target > int64_t(s->data.size())) return rt::ret_long(-1);
    s->pos = uint64_t(target);
    return rt::ret_long(target);
  });
  r.bind(c, "nativeAssetGetLength", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(int64_t(need(rt, tables().assets, a.i64(0), "Asset")->data.size()));
  });
  r.bind(c, "nativeAssetGetRemainingLength", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = need(rt, tables().assets, a.i64(0), "Asset");
    return rt::ret_long(int64_t(s->data.size() - std::min<uint64_t>(s->pos, s->data.size())));
  });
}

// --- Themes and attribute resolution --------------------------------------------------------

std::shared_ptr<ThemeHolder> theme(Runtime& rt, int64_t h) { return need(rt, tables().themes, h, "Theme"); }

void register_themes(rt::NativeRegistry& r) {
  const char* c = "Landroid/content/res/AssetManager;";
  r.bind(c, "nativeThemeCreate", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto holder = std::make_shared<ThemeHolder>();
    holder->manager = manager(rt, a.i64(0));
    holder->theme = std::make_unique<res::Theme>(&holder->manager->am);
    return uint64_t(tables().themes.add(std::move(holder)));
  });
  r.bind(c, "nativeThemeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().themes.remove(a.i64(0));
    return 0;
  });
  r.bind(c, "nativeThemeApplyStyle", "(JJIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    theme(rt, a.i64(2))->theme->apply_style(uint32_t(a.i32(4)), a.z(5));
    return 0;
  });
  r.bind(c, "nativeThemeRebase", "(JJ[I[ZI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = theme(rt, a.i64(2));
    std::vector<uint32_t> styles = int_array(rt, a.h(4), "styleIds");
    rt::ArrayObject& force = rt.array(a.h(5), "boolean[]");
    int32_t count = a.i32(6);
    if (count < 0 || size_t(count) > styles.size() || uint32_t(count) > force.length) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "nativeThemeRebase : tailles incohérentes");
    }
    t->theme->clear();
    for (int32_t i = 0; i < count; ++i) t->theme->apply_style(styles[size_t(i)], force.data[size_t(i) * force.width] != 0);
    return 0;
  });
  r.bind(c, "nativeThemeCopy", "(JJJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto dst = theme(rt, a.i64(2));
    auto src = theme(rt, a.i64(6));
    dst->theme->set_to(*src->theme);
    return 0;
  });
  r.bind(c, "nativeThemeGetAttributeValue", "(JJILandroid/util/TypedValue;Z)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = theme(rt, a.i64(2));
    auto v = t->theme->get_attribute(uint32_t(a.i32(4)));
    if (!v) return rt::ret_int(-1);
    if (a.z(6) && !t->manager->am.resolve_reference(*v)) return rt::ret_int(-1);
    return rt::ret_int(copy_value(rt, *v, a.h(5)));
  });
  r.bind(c, "nativeThemeGetChangingConfigurations", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return theme(rt, a.i64(0))->theme->changing_configurations();
  });
  r.bind(c, "nativeThemeDump", "(JJILjava/lang/String;Ljava/lang/String;)V", [](Runtime&, NativeArgs&) -> uint64_t {
    return 0;
  }, S::kStub, "pas de vidage du thème");
  r.bind(c, "nativeAttributeResolutionStack", "(JJIII)[I", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_array("[I", 0);
  }, S::kStub, "pile de résolution non suivie");

  // TypedArray buffers come as VMRuntime.addressOf values (nativeApplyStyle)
  // or as arrays (the other variants).
  r.bind(c, "nativeApplyStyle", "(JJIIJ[IJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = theme(rt, a.i64(2));
    std::shared_ptr<res::XmlParser> p = a.i64(6) ? parser(rt, a.i64(6)) : nullptr;
    std::vector<uint32_t> attrs = int_array(rt, a.h(8), "attrs");
    auto values = int_span(native_array(rt, a.i64(9)), "outValues", rt);
    auto indices = int_span(native_array(rt, a.i64(11)), "outIndices", rt);
    if (values.size() < attrs.size() * res::kStyleNumEntries || indices.size() < attrs.size() + 1) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "tableaux de TypedArray trop petits");
    }
    res::apply_style(*t->theme, p.get(), uint32_t(a.i32(4)), uint32_t(a.i32(5)), attrs, values, indices);
    return 0;
  });
  r.bind(c, "nativeApplyStyleWithArray", "(JJIIJ[I[I[I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = theme(rt, a.i64(2));
    std::shared_ptr<res::XmlParser> p = a.i64(6) ? parser(rt, a.i64(6)) : nullptr;
    std::vector<uint32_t> attrs = int_array(rt, a.h(8), "attrs");
    auto values = int_span(rt.array(a.h(9), "outValues"), "outValues", rt);
    auto indices = int_span(rt.array(a.h(10), "outIndices"), "outIndices", rt);
    if (values.size() < attrs.size() * res::kStyleNumEntries || indices.size() < attrs.size() + 1) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "tableaux de TypedArray trop petits");
    }
    res::apply_style(*t->theme, p.get(), uint32_t(a.i32(4)), uint32_t(a.i32(5)), attrs, values, indices);
    return 0;
  });
  r.bind(c, "nativeResolveAttrs", "(JJII[I[I[I[I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = theme(rt, a.i64(2));
    std::vector<uint32_t> src = a.h(6) ? int_array(rt, a.h(6), "inValues") : std::vector<uint32_t>{};
    std::vector<uint32_t> attrs = int_array(rt, a.h(7), "attrs");
    auto values = int_span(rt.array(a.h(8), "outValues"), "outValues", rt);
    std::span<uint32_t> indices;
    if (a.h(9)) indices = int_span(rt.array(a.h(9), "outIndices"), "outIndices", rt);
    if (values.size() < attrs.size() * res::kStyleNumEntries || (a.h(9) && indices.size() < attrs.size() + 1) ||
        (!src.empty() && src.size() < attrs.size())) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "tableaux trop petits");
    }
    res::resolve_attrs(*t->theme, uint32_t(a.i32(4)), uint32_t(a.i32(5)), src, attrs, values, indices);
    return 1;
  });
  r.bind(c, "nativeRetrieveAttributes", "(JJ[I[I[I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = manager(rt, a.i64(0));
    auto p = parser(rt, a.i64(2));
    std::vector<uint32_t> attrs = int_array(rt, a.h(4), "attrs");
    auto values = int_span(rt.array(a.h(5), "outValues"), "outValues", rt);
    std::span<uint32_t> indices;
    if (a.h(6)) indices = int_span(rt.array(a.h(6), "outIndices"), "outIndices", rt);
    if (values.size() < attrs.size() * res::kStyleNumEntries || (a.h(6) && indices.size() < attrs.size() + 1)) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "tableaux trop petits");
    }
    res::retrieve_attributes(m->am, p.get(), attrs, values, indices);
    return 1;
  });
}

// --- StringBlock ------------------------------------------------------------------------------

void register_string_block(rt::NativeRegistry& r) {
  const char* c = "Landroid/content/res/StringBlock;";
  r.bind(c, "nativeCreate", "([BII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto bytes = std::make_shared<const std::vector<uint8_t>>(byte_range(rt, a.h(0), a.i32(1), a.i32(2)));
    try {
      return uint64_t(tables().pools.add(res::StringPool::parse(bytes, 0, bytes->size())));
    } catch (const FormatError&) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "pool de chaînes invalide");
    }
  });
  r.bind(c, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().pools.remove(a.i64(0));
    return 0;
  });
  r.bind(c, "nativeGetSize", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return need(rt, tables().pools, a.i64(0), "StringBlock")->size();
  });
  r.bind(c, "nativeGetString", "(JI)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pool = need(rt, tables().pools, a.i64(0), "StringBlock");
    int32_t idx = a.i32(2);
    auto s = idx >= 0 ? pool->string16(uint32_t(idx)) : std::nullopt;
    if (!s) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", std::to_string(idx));
    return rt.new_string(*s);
  });
  r.bind(c, "nativeGetStyle", "(JI)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pool = need(rt, tables().pools, a.i64(0), "StringBlock");
    int32_t idx = a.i32(2);
    if (idx < 0) return kNull;
    auto spans = pool->style(uint32_t(idx));
    return spans.empty() ? kNull : new_int_array(rt, spans);
  });
}

// --- XmlBlock ---------------------------------------------------------------------------------

void register_xml_block(rt::NativeRegistry& r) {
  const char* c = "Landroid/content/res/XmlBlock;";
  r.bind(c, "nativeCreate", "([BII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    try {
      auto tree = std::make_shared<TreeHolder>();
      tree->tree = res::XmlTree::parse(byte_range(rt, a.h(0), a.i32(1), a.i32(2)));
      return uint64_t(tables().trees.add(std::move(tree)));
    } catch (const FormatError&) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "XML compilé invalide");
    }
  });
  r.bind(c, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    if (auto t = tables().trees.get(a.i64(0))) {
      if (t->pool) tables().pools.remove(t->pool);
      tables().trees.remove(a.i64(0));
    }
    return 0;
  });
  r.bind(c, "nativeGetStringBlock", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = need(rt, tables().trees, a.i64(0), "XmlBlock");
    if (!t->pool) t->pool = tables().pools.add(t->tree->strings());
    return uint64_t(t->pool);
  });
  r.bind(c, "nativeCreateParseState", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = need(rt, tables().trees, a.i64(0), "XmlBlock");
    return uint64_t(tables().parsers.add(std::make_shared<res::XmlParser>(t->tree, uint32_t(a.i32(2)))));
  });
  r.bind(c, "nativeDestroyParseState", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().parsers.remove(a.i64(0));
    return 0;
  });
  r.bind(c, "nativeNext", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = parser(rt, a.i64(0));
    for (;;) {
      switch (p->next()) {
        case res::XmlParser::kStartTag: return 2;
        case res::XmlParser::kEndTag: return 3;
        case res::XmlParser::kText: return 4;
        case res::XmlParser::kStartDocument: return 0;
        case res::XmlParser::kEndDocument: return 1;
        case res::XmlParser::kStartNamespace:
        case res::XmlParser::kEndNamespace:
          continue;
        default:
          rt.throw_new("Lorg/xmlpull/v1/XmlPullParserException;", "Corrupt XML binary file");
      }
    }
  });
  auto by_state = [&r, c](const char* name, int32_t (*fn)(const res::XmlParser&)) {
    r.bind(c, name, "(J)I", [fn](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(fn(*parser(rt, a.i64(0)))); });
  };
  by_state("nativeGetNamespace", [](const res::XmlParser& p) { return p.element_namespace(); });
  by_state("nativeGetName", [](const res::XmlParser& p) { return p.element_name(); });
  by_state("nativeGetText", [](const res::XmlParser& p) { return p.text(); });
  by_state("nativeGetLineNumber", [](const res::XmlParser& p) { return p.line(); });
  by_state("nativeGetAttributeCount", [](const res::XmlParser& p) { return p.attribute_count(); });
  by_state("nativeGetSourceResId", [](const res::XmlParser& p) { return int32_t(p.source_resid()); });
  by_state("nativeGetIdAttribute", [](const res::XmlParser& p) {
    int32_t i = p.index_of_id();
    return i >= 0 ? p.attribute_raw_value(i) : -1;
  });
  by_state("nativeGetClassAttribute", [](const res::XmlParser& p) {
    int32_t i = p.index_of_class();
    return i >= 0 ? p.attribute_raw_value(i) : -1;
  });
  by_state("nativeGetStyleAttribute", [](const res::XmlParser& p) {
    int32_t i = p.index_of_style();
    if (i < 0) return 0;
    res::Value v = p.attribute_value(i);
    return v.type == res::kTypeReference || v.type == res::kTypeAttribute ? int32_t(v.data) : 0;
  });
  auto by_index = [&r, c](const char* name, int32_t (*fn)(const res::XmlParser&, int32_t)) {
    r.bind(c, name, "(JI)I", [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_int(fn(*parser(rt, a.i64(0)), a.i32(2)));
    });
  };
  by_index("nativeGetAttributeNamespace", [](const res::XmlParser& p, int32_t i) { return p.attribute_namespace(i); });
  by_index("nativeGetAttributeName", [](const res::XmlParser& p, int32_t i) { return p.attribute_name(i); });
  by_index("nativeGetAttributeResource", [](const res::XmlParser& p, int32_t i) {
    return int32_t(p.attribute_name_resid(i));
  });
  by_index("nativeGetAttributeDataType", [](const res::XmlParser& p, int32_t i) {
    return int32_t(p.attribute_value(i).type);
  });
  by_index("nativeGetAttributeData", [](const res::XmlParser& p, int32_t i) { return int32_t(p.attribute_value(i).data); });
  by_index("nativeGetAttributeStringValue", [](const res::XmlParser& p, int32_t i) { return p.attribute_raw_value(i); });
  r.bind(c, "nativeGetAttributeIndex", "(JLjava/lang/String;Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = parser(rt, a.i64(0));
    if (a.h(3) == kNull) rt.throw_npe("name");
    std::optional<std::u16string> ns;
    if (a.h(2) != kNull) ns = rt.string_value(a.h(2));
    return rt::ret_int(p->index_of_attribute(ns ? &*ns : nullptr, rt.string_value(a.h(3))));
  });
  r.bind(c, "nativeGetFlagInfo", "(J)Landroid/content/res/XmlBlock$FlagInfo;", [](Runtime&, NativeArgs&) -> uint64_t {
    return kNull;
  }, S::kPartial, "pas de ressources conditionnées par des drapeaux");
}

}  // namespace

void register_resource_natives(rt::NativeRegistry& r) {
  register_apk_assets(r);
  register_asset_manager(r);
  register_asset_files(r);
  register_themes(r);
  register_string_block(r);
  register_xml_block(r);
}

}  // namespace aa::bridge
