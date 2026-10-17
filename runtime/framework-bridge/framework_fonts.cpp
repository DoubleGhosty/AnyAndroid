// android.graphics.Typeface and android.graphics.fonts natives (G3, ADR-006
// J4), each signature checked against the framework.jar of Android 17,
// written from what the real start-up path asks for. Typeface's static
// initialisation and Typeface.loadPreinstalledSystemFontMap read the system
// font configuration (/system/etc/fonts.xml) in Java; the native side builds
// minikin's objects (Font, FontFamily, FontCollection behind a Typeface) in
// the text engine (runtime/text, ADR-007) and keeps HWUI's registries, with
// its rules for styles, weights and fallback.
//
// The font data stays where Java mapped it (a direct ByteBuffer, kept alive
// by the font). The engine reads the tables it needs from it (cmap for
// fallback, fvar for variations) and opens a font with FreeType only when it
// measures or draws with it, as minikin leaves fonts mapped until used.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/text.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/native_memory.h"
#include "framework-bridge/skia_bridge.h"
#include "text/font_collection.h"
#include "text/locale.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

struct Axis {
  int32_t tag = 0;
  float value = 0;
};

// minikin::Font over the font data of a direct ByteBuffer.
struct FontData {
  rt::GlobalRef buffer;  // keeps the mapping alive
  int64_t address = 0;   // emulated native address of the data
  int32_t size = 0;
  std::string path;  // "" when the font does not come from a file
  int32_t source_id = 0;
};

// The engine's view of a font buffer: parts read in place, the whole pinned
// when FreeType opens the font. A read that fails reads as missing bytes.
class BufferSource final : public text::FontSource {
 public:
  BufferSource(Runtime& rt, std::shared_ptr<const FontData> data) : rt_(&rt), data_(std::move(data)) {}
  size_t size() const override { return size_t(data_->size); }
  size_t read(size_t offset, std::span<uint8_t> out) const override {
    if (offset >= size()) return 0;
    try {
      return native_read(*rt_, data_->address, offset, out.first(std::min(out.size(), size() - offset)));
    } catch (const rt::JavaThrow&) {
      return 0;
    }
  }
  text::FontData bytes() const override {
    try {
      PinnedBytes p = native_pin(*rt_, data_->address, uint64_t(data_->size));
      return text::FontData{std::move(p.owner), p.data, p.size};
    } catch (const rt::JavaThrow&) {
      return text::FontData{};
    }
  }

 private:
  Runtime* rt_;
  std::shared_ptr<const FontData> data_;
};

struct Font {
  std::shared_ptr<const FontData> data;
  std::u16string locales;
  int32_t weight = 400;
  bool italic = false;
  int32_t ttc_index = 0;
  std::vector<Axis> axes;
  std::shared_ptr<const text::Font> engine;  // minikin::Font
};

struct FontBuilder {
  std::mutex lock;
  std::vector<Axis> axes;
};

struct Family {
  std::vector<std::shared_ptr<const Font>> fonts;
  std::u16string lang_tags;
  int32_t variant = 0;
  bool custom_fallback = false, default_fallback = false;
  int32_t variation_family_type = 0;
  std::shared_ptr<const text::FontFamily> engine;  // minikin::FontFamily
};

struct FamilyBuilder {
  std::mutex lock;
  std::vector<std::shared_ptr<const Font>> fonts;
};

struct Collection {
  std::vector<std::shared_ptr<const Family>> families;
  std::shared_ptr<const text::FontCollection> engine;  // minikin::FontCollection
};

// HWUI's Typeface: a font collection, its base weight, the style asked
// through the API (NORMAL, BOLD, ITALIC, BOLD_ITALIC) and the minikin style.
struct Typeface {
  std::shared_ptr<const Collection> collection;
  int32_t base_weight = 400;
  int32_t api_style = 0;
  int32_t weight = 400;
  bool italic = false;
};

constexpr int32_t kBold = 1, kItalic = 2;
constexpr int32_t kResolveByFontTable = -1;

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<FontBuilder> font_builders{0x1104};
  HandleTable<const Font> fonts{0x1105};
  HandleTable<FamilyBuilder> family_builders{0x1106};
  HandleTable<const Family> families{0x1107};
  HandleTable<const Typeface> typefaces{0x1108};
  std::atomic<int32_t> next_source_id{1};
  std::mutex lock;  // the registries below
  std::shared_ptr<const Typeface> default_typeface;
  std::vector<std::pair<std::u16string, std::shared_ptr<const Collection>>> generic_families;
  std::vector<std::shared_ptr<const Collection>> font_map;
  std::unordered_map<const text::Font*, std::shared_ptr<const Font>> engine_to_font;
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed: handles may be freed at exit
  return *t;
}

template <typename T>
std::shared_ptr<T> get(Runtime& rt, HandleTable<T>& table, int64_t h, const char* what) {
  auto v = table.get(h);
  if (!v) rt.throw_new("Ljava/lang/IllegalStateException;", std::string("[AnyAndroid] ") + what + " invalide");
  return v;
}

// A builder is consumed by the native that builds from it (HWUI deletes it).
template <typename T>
std::shared_ptr<T> take(Runtime& rt, HandleTable<T>& table, int64_t h, const char* what) {
  auto v = get(rt, table, h, what);
  table.remove(h);
  return v;
}

std::u16string optional_string(Runtime& rt, Handle h) { return h == kNull ? std::u16string() : rt.string_value(h); }

int64_t long_of(const rt::Object& o, const rt::Field& f) {
  return int64_t(uint64_t(o.fields.at(f.slot)) | (uint64_t(o.fields.at(f.slot + 1)) << 32));
}

// JNI GetDirectBufferAddress / GetDirectBufferCapacity on `buffer`.
std::shared_ptr<FontData> font_data(Runtime& rt, Handle buffer, std::string path) {
  rt::Object& o = rt.nonnull(buffer, "buffer");
  rt::Field* address = find_field(o.klass, "address", "J");
  rt::Field* capacity = find_field(o.klass, "capacity", "I");
  int64_t at = address && !address->is_static() ? long_of(o, *address) : 0;
  if (!is_native_address(at) || !capacity || capacity->is_static()) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "Not a direct buffer");
  }
  int32_t size = int32_t(o.fields.at(capacity->slot));
  if (size <= 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "buffer size must not be zero or negative");
  auto d = std::make_shared<FontData>();
  d->buffer = rt::GlobalRef(rt, buffer);
  d->address = at;
  d->size = size;
  d->path = std::move(path);
  d->source_id = tables().next_source_id++;
  return d;
}

int32_t clamp_weight(int32_t w) { return std::max(1, std::min(1000, w)); }

// HWUI's computeAPIStyle.
int32_t api_style(int32_t weight, bool italic) {
  if (weight < 600) return italic ? kItalic : 0;
  return italic ? kBold | kItalic : kBold;
}

// minikin's FontFamily::getClosestMatch score.
int32_t match_score(const Font& f, int32_t weight, bool italic) {
  int32_t score = std::abs(f.weight / 100 - weight / 100);
  if (f.italic != italic) score += 2;
  return score;
}

std::shared_ptr<const Typeface> resolve_default(Runtime& rt, int64_t h) {
  if (h != 0) return get(rt, tables().typefaces, h, "Typeface");
  std::lock_guard<std::mutex> guard(tables().lock);
  if (!tables().default_typeface) {
    rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] pas de Typeface par défaut (setDefault non appelé)");
  }
  return tables().default_typeface;
}

uint64_t add_typeface(Typeface&& t) {
  return uint64_t(tables().typefaces.add(std::make_shared<const Typeface>(std::move(t))));
}

// HWUI's computeRelativeStyle.
void set_relative_style(Typeface& t, int32_t base_weight, int32_t style) {
  t.weight = clamp_weight((style & kBold) ? base_weight + 300 : base_weight);
  t.italic = (style & kItalic) != 0;
}

// Font_Builder_build: the minikin font of a bridge font.
std::shared_ptr<const text::Font> engine_font(Runtime& rt, const Font& f) {
  std::vector<text::Variation> axes;
  for (const Axis& a : f.axes) axes.push_back({uint32_t(a.tag), a.value});
  uint32_t locales = uint32_t(minikin_locale_list_id(rt, f.locales));
  text::FontStyle style{uint16_t(std::clamp(f.weight, 0, 1000)), f.italic};
  return std::make_shared<const text::Font>(std::make_shared<BufferSource>(rt, f.data), uint32_t(std::max(0, f.ttc_index)),
                                            std::move(axes), style, locales);
}

void register_fonts(rt::NativeRegistry& r) {
  const char* fb = "Landroid/graphics/fonts/Font$Builder;";
  r.bind(fb, "nInitBuilder", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return uint64_t(tables().font_builders.add(std::make_shared<FontBuilder>()));
  });
  r.bind(fb, "nAddAxis", "(JIF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = get(rt, tables().font_builders, a.i64(0), "Font.Builder");
    std::lock_guard<std::mutex> guard(b->lock);
    if (b->axes.size() >= 64) rt.throw_new("Ljava/lang/IllegalArgumentException;", "trop d'axes de variation");
    b->axes.push_back({a.i32(2), a.f32(3)});
    return 0;
  });
  r.bind(fb, "nBuild", "(JLjava/nio/ByteBuffer;Ljava/lang/String;Ljava/lang/String;IZI)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(2) == kNull) rt.throw_npe("buffer");
           auto b = take(rt, tables().font_builders, a.i64(0), "Font.Builder");
           auto f = std::make_shared<Font>();
           f->data = font_data(rt, a.h(2), a.h(3) == kNull ? std::string() : rt.string_utf8(a.h(3)));
           f->locales = optional_string(rt, a.h(4));
           minikin_locale_list_id(rt, f->locales);
           f->weight = a.i32(5);
           f->italic = a.z(6);
           f->ttc_index = a.i32(7);
           {
             std::lock_guard<std::mutex> guard(b->lock);
             f->axes = b->axes;
           }
           f->engine = engine_font(rt, *f);
           {
             std::lock_guard<std::mutex> guard(tables().lock);
             tables().engine_to_font[f->engine.get()] = f;
           }
           return uint64_t(tables().fonts.add(f));
         });
  // A font over the same data with other parameters (Font.Builder(Font)).
  r.bind(fb, "nClone", "(JJIZI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = get(rt, tables().fonts, a.i64(0), "Font");
    auto b = take(rt, tables().font_builders, a.i64(2), "Font.Builder");
    auto f = std::make_shared<Font>(*src);
    f->weight = a.i32(4);
    f->italic = a.z(5);
    f->ttc_index = a.i32(6);
    {
      std::lock_guard<std::mutex> guard(b->lock);
      f->axes = b->axes;
    }
    f->engine = engine_font(rt, *f);
    {
      std::lock_guard<std::mutex> guard(tables().lock);
      tables().engine_to_font[f->engine.get()] = f;
    }
    return uint64_t(tables().fonts.add(f));
  });

  const char* fo = "Landroid/graphics/fonts/Font;";
  static const int64_t font_free = register_free_function([](int64_t h) { tables().fonts.remove(h); });
  r.bind(fo, "nGetReleaseNativeFont", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(font_free); });
  r.bind(fo, "nCloneFont", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return uint64_t(tables().fonts.add(get(rt, tables().fonts, a.i64(0), "Font")));
  });
  r.bind(fo, "nGetFontPath", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = get(rt, tables().fonts, a.i64(0), "Font");
    return f->data->path.empty() ? kNull : rt::ret_obj(rt.new_string_utf8(f->data->path));
  });
  r.bind(fo, "nGetLocaleList", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_obj(rt.new_string(get(rt, tables().fonts, a.i64(0), "Font")->locales));
  });
  r.bind(fo, "nGetIndex", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get(rt, tables().fonts, a.i64(0), "Font")->ttc_index);
  });
  // Weight in the low 16 bits, italic in bit 16.
  r.bind(fo, "nGetPackedStyle", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = get(rt, tables().fonts, a.i64(0), "Font");
    return rt::ret_int((f->weight & 0xFFFF) | (f->italic ? 1 << 16 : 0));
  });
  r.bind(fo, "nGetAxisCount", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(get(rt, tables().fonts, a.i64(0), "Font")->axes.size()));
  });
  // The axis tag in the high 32 bits, the bits of its float value below.
  r.bind(fo, "nGetAxisInfo", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = get(rt, tables().fonts, a.i64(0), "Font");
    int32_t i = a.i32(2);
    if (i < 0 || size_t(i) >= f->axes.size()) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "axe");
    uint32_t bits;
    std::memcpy(&bits, &f->axes[size_t(i)].value, 4);
    return rt::ret_long(int64_t((uint64_t(uint32_t(f->axes[size_t(i)].tag)) << 32) | bits));
  });
  r.bind(fo, "nGetBufferAddress", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(get(rt, tables().fonts, a.i64(0), "Font")->data->address);
  });
  r.bind(fo, "nGetSourceId", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get(rt, tables().fonts, a.i64(0), "Font")->data->source_id);
  });
}

void register_families(rt::NativeRegistry& r) {
  const char* fb = "Landroid/graphics/fonts/FontFamily$Builder;";
  static const int64_t family_free = register_free_function([](int64_t h) { tables().families.remove(h); });
  r.bind(fb, "nGetReleaseNativeFamily", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(family_free);
  });
  r.bind(fb, "nInitBuilder", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return uint64_t(tables().family_builders.add(std::make_shared<FamilyBuilder>()));
  });
  r.bind(fb, "nAddFont", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = get(rt, tables().family_builders, a.i64(0), "FontFamily.Builder");
    auto f = get(rt, tables().fonts, a.i64(2), "Font");
    std::lock_guard<std::mutex> guard(b->lock);
    if (b->fonts.size() >= 1024) rt.throw_new("Ljava/lang/IllegalArgumentException;", "trop de polices dans la famille");
    b->fonts.push_back(std::move(f));
    return 0;
  });
  r.bind(fb, "nBuild", "(JLjava/lang/String;IZZI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = take(rt, tables().family_builders, a.i64(0), "FontFamily.Builder");
    auto f = std::make_shared<Family>();
    f->lang_tags = optional_string(rt, a.h(2));
    minikin_locale_list_id(rt, f->lang_tags);
    f->variant = a.i32(3);
    f->custom_fallback = a.z(4);
    f->default_fallback = a.z(5);
    f->variation_family_type = a.i32(6);
    {
      std::lock_guard<std::mutex> guard(b->lock);
      f->fonts = b->fonts;
    }
    // minikin rejects a family without coverage; with no font there is none.
    if (f->fonts.empty()) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "Failed to create internal object. maybe invalid font data");
    }
    std::vector<std::shared_ptr<const text::Font>> fonts;
    for (const auto& font : f->fonts) fonts.push_back(font->engine);
    f->engine = std::make_shared<const text::FontFamily>(
        uint32_t(minikin_locale_list_id(rt, f->lang_tags)), text::FamilyVariant(std::clamp(f->variant, 0, 2)),
        std::move(fonts), f->custom_fallback, f->default_fallback,
        text::VariationFamilyType(std::clamp(f->variation_family_type, 0, 3)));
    return uint64_t(tables().families.add(std::move(f)));
  }, S::kPartial, "couverture lue de la table cmap au premier usage ; la famille n'est pas refusée sans couverture");

  const char* fa = "Landroid/graphics/fonts/FontFamily;";
  // A new native font object each time (Java wraps and registers it).
  r.bind(fa, "nGetFont", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = get(rt, tables().families, a.i64(0), "FontFamily");
    int32_t i = a.i32(2);
    if (i < 0 || size_t(i) >= f->fonts.size()) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "police");
    return uint64_t(tables().fonts.add(f->fonts[size_t(i)]));
  });
  r.bind(fa, "nGetFontSize", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(get(rt, tables().families, a.i64(0), "FontFamily")->fonts.size()));
  });
  r.bind(fa, "nGetLangTags", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_obj(rt.new_string(get(rt, tables().families, a.i64(0), "FontFamily")->lang_tags));
  });
  r.bind(fa, "nGetVariant", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get(rt, tables().families, a.i64(0), "FontFamily")->variant);
  });
}

void register_typefaces(rt::NativeRegistry& r) {
  const char* tf = "Landroid/graphics/Typeface;";
  static const int64_t typeface_free = register_free_function([](int64_t h) { tables().typefaces.remove(h); });
  r.bind(tf, "nativeGetReleaseFunc", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(typeface_free); });
  // HWUI maps the file into its font data cache ahead of use: an optimisation
  // with no observable effect, so nothing to do.
  r.bind(tf, "nativeWarmUpCache", "(Ljava/lang/String;)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(tf, "nativeRegisterLocaleList", "(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    minikin_locale_list_id(rt, optional_string(rt, a.h(0)));
    return 0;
  });

  // Typeface::createFromFamilies: the families, then the fallback's.
  r.bind(tf, "nativeCreateFromArray", "([JJII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("familyArray");
    rt::ArrayObject& arr = rt.array(a.h(0), "long[]");
    if (arr.width != 8) rt.throw_new("Ljava/lang/IllegalArgumentException;", "long[] attendu");
    auto collection = std::make_shared<Collection>();
    for (uint32_t i = 0; i < arr.length; ++i) {
      int64_t h;
      std::memcpy(&h, arr.data.data() + size_t(i) * 8, 8);
      collection->families.push_back(get(rt, tables().families, h, "FontFamily"));
    }
    if (int64_t fallback = a.i64(1)) {
      auto fb = get(rt, tables().typefaces, fallback, "Typeface");
      const auto& more = fb->collection->families;
      collection->families.insert(collection->families.end(), more.begin(), more.end());
    }
    std::vector<std::shared_ptr<const text::FontFamily>> engines;
    for (const auto& family : collection->families) engines.push_back(family->engine);
    collection->engine = std::make_shared<const text::FontCollection>(std::move(engines));
    int32_t weight = a.i32(3), italic = a.i32(4);
    if (weight == kResolveByFontTable || italic == kResolveByFontTable) {
      // The style of the first family's font closest to the default one.
      int32_t font_weight = 400;
      bool font_italic = false;
      if (!collection->families.empty() && !collection->families.front()->fonts.empty()) {
        const Font* best = nullptr;
        for (const auto& f : collection->families.front()->fonts) {
          if (!best || match_score(*f, 400, false) < match_score(*best, 400, false)) best = f.get();
        }
        font_weight = best->weight;
        font_italic = best->italic;
      }
      if (weight == kResolveByFontTable) weight = font_weight;
      if (italic == kResolveByFontTable) italic = font_italic;
    }
    Typeface t;
    t.collection = std::move(collection);
    t.base_weight = weight;
    t.api_style = api_style(weight, italic == 1);
    t.weight = clamp_weight(weight);
    t.italic = italic == 1;
    return add_typeface(std::move(t));
  });
  // Typeface::createRelative.
  r.bind(tf, "nativeCreateFromTypeface", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = resolve_default(rt, a.i64(0));
    Typeface t;
    t.collection = src->collection;
    t.base_weight = src->base_weight;
    t.api_style = a.i32(2);
    set_relative_style(t, t.base_weight, t.api_style);
    return add_typeface(std::move(t));
  });
  // Typeface::createAbsolute.
  r.bind(tf, "nativeCreateFromTypefaceWithExactStyle", "(JIZ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = resolve_default(rt, a.i64(0));
    Typeface t;
    t.collection = src->collection;
    t.base_weight = src->base_weight;
    t.api_style = api_style(a.i32(2), a.z(3));
    t.weight = clamp_weight(a.i32(2));
    t.italic = a.z(3);
    return add_typeface(std::move(t));
  });
  // Typeface::createWithDifferentBaseWeight.
  r.bind(tf, "nativeCreateWeightAlias", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = resolve_default(rt, a.i64(0));
    Typeface t;
    t.collection = src->collection;
    t.base_weight = a.i32(2);
    t.api_style = src->api_style;
    set_relative_style(t, t.base_weight, t.api_style);
    return add_typeface(std::move(t));
  });
  r.bind(tf, "nativeGetStyle", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get(rt, tables().typefaces, a.i64(0), "Typeface")->api_style);
  });
  r.bind(tf, "nativeGetWeight", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(get(rt, tables().typefaces, a.i64(0), "Typeface")->weight);
  });

  // minikin's SystemFonts registries.
  r.bind(tf, "nativeSetDefault", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = get(rt, tables().typefaces, a.i64(0), "Typeface");
    std::lock_guard<std::mutex> guard(tables().lock);
    tables().default_typeface = std::move(t);
    return 0;
  });
  r.bind(tf, "nativeRegisterGenericFamily", "(Ljava/lang/String;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string name = optional_string(rt, a.h(0));
    auto t = get(rt, tables().typefaces, a.i64(1), "Typeface");
    std::lock_guard<std::mutex> guard(tables().lock);
    auto& families = tables().generic_families;
    for (auto& [n, c] : families) {
      if (n == name) {
        c = t->collection;
        return 0;
      }
    }
    families.emplace_back(std::move(name), t->collection);
    return 0;
  });
  r.bind(tf, "nativeAddFontCollections", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = get(rt, tables().typefaces, a.i64(0), "Typeface");
    std::lock_guard<std::mutex> guard(tables().lock);
    tables().font_map.push_back(t->collection);
    return 0;
  });
  // Sets one of Typeface's static final fields (DEFAULT, SANS_SERIF...).
  r.bind(tf, "nativeForceSetStaticFinalField", "(Ljava/lang/String;Landroid/graphics/Typeface;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe("fieldName");
           rt::Class* c = rt.find_class("Landroid/graphics/Typeface;");
           rt::Field* f = c ? c->declared_field(rt.string_utf8(a.h(0)), "Landroid/graphics/Typeface;") : nullptr;
           if (!f || !f->is_static()) rt.throw_new("Ljava/lang/RuntimeException;", "Unable to find field");
           c->statics.at(f->slot) = a.h(1);
           return 0;
         });
}

}  // namespace

int32_t minikin_locale_list_id(rt::Runtime& rt, const std::u16string& tags) {
  ensure_text_services(rt);
  uint32_t id = text::locale_list_id(text::utf16_to_utf8(tags));
  if (id == text::kInvalidLocaleListId) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "trop de listes de locales distinctes");
  }
  return int32_t(id);
}

bool minikin_locale_list_valid(int32_t id) { return id >= 0 && text::locale_list_id_valid(uint32_t(id)); }

TypefaceInfo resolve_typeface(rt::Runtime& rt, int64_t handle) {
  auto t = resolve_default(rt, handle);
  return TypefaceInfo{t->collection->engine, text::FontStyle{uint16_t(t->weight), t->italic}, t->api_style};
}

int64_t font_handle_from_engine(rt::Runtime&, const std::shared_ptr<const text::Font>& engine_font) {
  if (!engine_font) return 0;
  std::lock_guard<std::mutex> guard(tables().lock);
  auto it = tables().engine_to_font.find(engine_font.get());
  if (it != tables().engine_to_font.end()) {
    return tables().fonts.add(it->second);
  }
  auto f = std::make_shared<Font>();
  f->engine = engine_font;
  f->weight = engine_font->style().weight;
  f->italic = engine_font->style().italic;
  auto d = std::make_shared<FontData>();
  d->source_id = tables().next_source_id++;
  f->data = std::move(d);
  tables().engine_to_font[engine_font.get()] = f;
  return tables().fonts.add(f);
}

int64_t font_clone_handle(rt::Runtime& rt, int64_t font_handle) {
  auto f = get(rt, tables().fonts, font_handle, "Font");
  return tables().fonts.add(f);
}

sk_sp<SkTypeface> font_skia_typeface(Runtime& rt, int64_t handle) {
  auto f = get(rt, tables().fonts, handle, "Font");
  if (!f || !f->engine) return nullptr;
  return skia_typeface(f->engine->face());
}

void register_framework_fonts(rt::NativeRegistry& r) {
  register_fonts(r);
  register_families(r);
  register_typefaces(r);
}

}  // namespace aa::bridge
