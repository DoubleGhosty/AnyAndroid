#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct hb_language_impl_t;

// minikin's Locale, LocaleList and LocaleListCache: the locales of a Paint
// (Paint.setTextLocales) and of the font families of fonts.xml, compared to
// choose fallback fonts and HarfBuzz languages.
namespace aa::text {

constexpr size_t kFontLocaleLimit = 12;

enum class EmojiStyle : uint8_t { kEmpty = 0, kDefault = 1, kEmoji = 2, kText = 3 };

class LocaleList;

class Locale {
 public:
  enum class Variant : uint16_t { kNone = 0, kGerman1901 = 1, kGerman1996 = 2 };

  Locale() = default;
  // A BCP 47 tag as minikin parses it (language, script, region, German
  // orthography variants, -u-em- emoji style).
  explicit Locale(std::string_view tag);

  bool operator==(const Locale& o) const {
    return supported() && script_ == o.script_ && language_ == o.language_ && region_ == o.region_ &&
           variant_ == o.variant_ && emoji_style_ == o.emoji_style_;
  }
  bool supported() const {
    return language_ != kNoLanguage || script_ != kNoScript || region_ != kNoRegion ||
           variant_ != Variant::kNone || emoji_style_ != EmojiStyle::kEmpty;
  }
  EmojiStyle emoji_style() const { return emoji_style_; }
  bool same_script(const Locale& o) const { return script_ == o.script_; }
  // hb_script_t (ISO 15924 tag): whether text of that script suits this
  // locale (Japanese covers Han, Hiragana and Katakana...).
  bool supports_script(uint32_t script_tag) const;
  int score_for(const LocaleList& supported) const;
  std::string str() const;
  uint64_t identifier() const {
    return uint64_t(language_) << 49 | uint64_t(script_) << 29 | uint64_t(region_) << 14 |
           uint64_t(emoji_style_) << 12 | uint64_t(variant_);
  }

 private:
  friend class LocaleList;
  static constexpr uint16_t kNoLanguage = 0x7fff;
  static constexpr uint16_t kNoRegion = 0x7fff;
  static constexpr uint32_t kNoScript = 0xfffff;
  static uint8_t sub_script_bits(uint32_t packed_script);
  static bool supports_script(uint8_t provided, uint8_t requested) {
    return requested != 0 && (provided & requested) == requested;
  }

  uint32_t script_ = kNoScript;
  uint16_t language_ = kNoLanguage;
  uint16_t region_ = kNoRegion;
  uint8_t sub_script_bits_ = 0;
  Variant variant_ = Variant::kNone;
  EmojiStyle emoji_style_ = EmojiStyle::kEmpty;
};

class LocaleList {
 public:
  LocaleList() = default;
  explicit LocaleList(std::vector<Locale> locales);
  size_t size() const { return locales_.size(); }
  bool empty() const { return locales_.empty(); }
  const Locale& operator[](size_t i) const { return locales_[i]; }
  const hb_language_impl_t* hb_language(size_t i) const { return hb_langs_[i]; }
  EmojiStyle emoji_style() const { return emoji_style_; }

 private:
  friend class Locale;
  std::vector<Locale> locales_;
  std::vector<const hb_language_impl_t*> hb_langs_;
  uint8_t union_of_sub_script_bits_ = 0;
  bool all_same_language_ = false;
  EmojiStyle emoji_style_ = EmojiStyle::kEmpty;
};

// LocaleListCache: ids handed to Java (Paint.nSetTextLocales). 0 is the empty
// list; kInvalidLocaleListId never names a list.
constexpr uint32_t kEmptyLocaleListId = 0;
constexpr uint32_t kInvalidLocaleListId = 0xFFFFFFFF;
// The id of a comma-separated list of locales; equal lists share an id.
uint32_t locale_list_id(std::string_view tags);
// The list of an id (the empty list for an unknown id).
const LocaleList& locale_list(uint32_t id);
bool locale_list_id_valid(uint32_t id);

}  // namespace aa::text
