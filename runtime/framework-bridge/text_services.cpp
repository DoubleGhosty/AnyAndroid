// The text engine's Unicode services (runtime/text/unicode.h) over the ICU4J
// of the libcore (android.icu, the i18n module): the same ICU version and
// data as the ICU4C that minikin uses on Android. Each answer is cached: a
// call into the interpreter costs far more than a lookup.
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework-bridge/internal.h"
#include "text/unicode.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::Runtime;

// android.icu.lang.UProperty, checked against core-icu4j.jar of Android 17.
constexpr int32_t kUPropertyEmoji = 57;
constexpr int32_t kUPropertyEmojiModifier = 59;
constexpr int32_t kUPropertyEmojiModifierBase = 60;
constexpr int32_t kUPropertyExtendedPictographic = 64;
constexpr int32_t kUPropertyGraphemeClusterBreak = 4114;

class IcuServices final : public text::UnicodeServices {
 public:
  // Answers come from the runtime of the latest caller (one at a time: the
  // unit tests create several in turn); a new runtime starts a new cache.
  void bind(Runtime& rt) {
    std::lock_guard<std::mutex> guard(lock_);
    if (rt_ == &rt) return;
    rt_ = &rt;
    tags_.clear();
    properties_.clear();
    graphemes_.clear();
  }

  // minikin's toLanguageTag (LocaleListCache.cpp): uloc_canonicalize, then
  // "und" for an empty locale or a bare region, else uloc_addLikelySubtags
  // and uloc_toLanguageTag.
  std::string language_tag(std::string_view locale) override {
    std::string key(locale);
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (auto it = tags_.find(key); it != tags_.end()) return it->second;
    }
    std::string result;
    try {
      Handle in = rt_->new_string_utf8(key);
      Handle canon = Handle(rt_->call_static("Landroid/icu/util/ULocale;", "canonicalize",
                                            "(Ljava/lang/String;)Ljava/lang/String;", {in}));
      std::string c = canon == kNull ? std::string() : rt_->string_utf8(canon);
      if (c.empty() || (c.size() == 5 && c[0] == '_')) {
        if (!c.empty()) c[0] = '-';
        result = "und" + c;
      } else {
        Handle ul = construct(*rt_, "Landroid/icu/util/ULocale;", "(Ljava/lang/String;)V", {canon});
        Handle likely = Handle(rt_->call_static("Landroid/icu/util/ULocale;", "addLikelySubtags",
                                               "(Landroid/icu/util/ULocale;)Landroid/icu/util/ULocale;", {ul}));
        Handle tag = likely == kNull ? kNull
                                     : Handle(rt_->call_virtual(likely, "toLanguageTag", "()Ljava/lang/String;"));
        result = tag == kNull ? std::string() : rt_->string_utf8(tag);
      }
    } catch (const rt::JavaThrow&) {
      // No ICU4J (the bridge without the libcore): the tag as written.
      result = key;
      for (char& ch : result) {
        if (ch == '_') ch = '-';
      }
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (tags_.size() < 4096) tags_.emplace(std::move(key), result);
    return result;
  }

  bool has_property(char32_t c, text::EmojiProperty p) override {
    int32_t property = 0;
    switch (p) {
      case text::EmojiProperty::kEmoji: property = kUPropertyEmoji; break;
      case text::EmojiProperty::kEmojiModifier: property = kUPropertyEmojiModifier; break;
      case text::EmojiProperty::kEmojiModifierBase: property = kUPropertyEmojiModifierBase; break;
      case text::EmojiProperty::kExtendedPictographic: property = kUPropertyExtendedPictographic; break;
    }
    uint64_t key = uint64_t(c) << 8 | uint8_t(p);
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (auto it = properties_.find(key); it != properties_.end()) return it->second;
    }
    bool value = false;
    try {
      value = rt_->call_static("Landroid/icu/lang/UCharacter;", "hasBinaryProperty", "(II)Z",
                              {uint32_t(c), uint32_t(property)}) != 0;
    } catch (const rt::JavaThrow&) {
      value = false;
    }
    std::lock_guard<std::mutex> guard(lock_);
    properties_.emplace(key, value);
    return value;
  }

  text::GraphemeBreakProp grapheme_break(char32_t c) override {
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (auto it = graphemes_.find(c); it != graphemes_.end()) return it->second;
    }
    text::GraphemeBreakProp value = text::GraphemeBreakProp::kOther;
    try {
      int32_t v = int32_t(rt_->call_static("Landroid/icu/lang/UCharacter;", "getIntPropertyValue", "(II)I",
                                          {uint32_t(c), uint32_t(kUPropertyGraphemeClusterBreak)}));
      if (v >= 0 && v <= int32_t(text::GraphemeBreakProp::kZwj)) value = text::GraphemeBreakProp(v);
    } catch (const rt::JavaThrow&) {
    }
    std::lock_guard<std::mutex> guard(lock_);
    graphemes_.emplace(c, value);
    return value;
  }

  // ubidi_setPara and ubidi_getVisualRun through android.icu.text.Bidi.
  bool bidi_runs(std::u16string_view text, uint8_t level, std::vector<text::BidiRun>* runs, bool* para_rtl) override {
    try {
      Handle bidi = construct(*rt_, "Landroid/icu/text/Bidi;", "()V", {});
      Handle str = rt_->new_string(std::u16string(text));
      rt_->call_virtual(bidi, "setPara", "(Ljava/lang/String;B[B)V", {str, uint32_t(int32_t(int8_t(level))), kNull});
      *para_rtl = (int8_t(rt_->call_virtual(bidi, "getParaLevel", "()B")) & 1) != 0;
      int32_t count = int32_t(rt_->call_virtual(bidi, "countRuns", "()I"));
      runs->clear();
      for (int32_t i = 0; i < count; ++i) {
        Handle run = Handle(rt_->call_virtual(bidi, "getVisualRun", "(I)Landroid/icu/text/BidiRun;", {uint32_t(i)}));
        if (run == kNull) return false;
        text::BidiRun r;
        r.start = uint32_t(rt_->call_virtual(run, "getStart", "()I"));
        r.length = uint32_t(rt_->call_virtual(run, "getLength", "()I"));
        r.rtl = (int8_t(rt_->call_virtual(run, "getDirection", "()B")) & 1) != 0;
        runs->push_back(r);
      }
      return true;
    } catch (const rt::JavaThrow&) {
      return false;
    }
  }

 private:
  Runtime* rt_ = nullptr;
  std::mutex lock_;
  std::map<std::string, std::string> tags_;
  std::unordered_map<uint64_t, bool> properties_;
  std::unordered_map<char32_t, text::GraphemeBreakProp> graphemes_;
};

}  // namespace

void ensure_text_services(Runtime& rt) {
  static auto* services = [] {
    auto* s = new IcuServices;  // lives as long as the process
    text::set_unicode_services(s);
    return s;
  }();
  services->bind(rt);
}

}  // namespace aa::bridge
