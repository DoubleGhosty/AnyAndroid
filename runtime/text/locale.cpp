// minikin's Locale.cpp and LocaleListCache.cpp (frameworks/minikin).
#include "text/locale.h"

#include <hb.h>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_set>

#include "text/unicode.h"

namespace aa::text {

namespace {

constexpr uint32_t kFiveBits = 0x1f;

// Two-letter codes use 0x1f as their first letter.
uint16_t pack_language_or_region(std::string_view in, char two_base, char three_base) {
  if (in.size() == 2) {
    return uint16_t(0x7c00u | uint16_t(in[0] - two_base) << 5 | uint16_t(in[1] - two_base));
  }
  return uint16_t(uint16_t(in[0] - three_base) << 10 | uint16_t(in[1] - three_base) << 5 |
                  uint16_t(in[2] - three_base));
}

size_t unpack_language_or_region(uint16_t in, char* out, char two_base, char three_base) {
  uint8_t first = (in >> 10) & kFiveBits, second = (in >> 5) & kFiveBits, third = in & kFiveBits;
  if (first == 0x1f) {
    out[0] = char(second + two_base);
    out[1] = char(third + two_base);
    return 2;
  }
  out[0] = char(first + three_base);
  out[1] = char(second + three_base);
  out[2] = char(third + three_base);
  return 3;
}

constexpr uint32_t pack_script(char c1, char c2, char c3, char c4) {
  return uint32_t(c1 - 'A') << 15 | uint32_t(c2 - 'a') << 10 | uint32_t(c3 - 'a') << 5 | uint32_t(c4 - 'a');
}

constexpr uint32_t pack_script(uint32_t tag) {
  return pack_script(char(tag >> 24), char((tag >> 16) & 0xff), char((tag >> 8) & 0xff), char(tag & 0xff));
}

constexpr uint32_t unpack_script(uint32_t packed) {
  return (((packed >> 15) + 'A') << 24) | ((((packed >> 10) & kFiveBits) + 'a') << 16) |
         ((((packed >> 5) & kFiveBits) + 'a') << 8) | ((packed & kFiveBits) + 'a');
}

static_assert(unpack_script(pack_script('J', 'p', 'a', 'n')) == HB_TAG('J', 'p', 'a', 'n'));

bool lower(char c) { return 'a' <= c && c <= 'z'; }
bool upper(char c) { return 'A' <= c && c <= 'Z'; }
bool digit(char c) { return '0' <= c && c <= '9'; }

bool valid_language(std::string_view b) {
  return (b.size() == 2 || b.size() == 3) && lower(b[0]) && lower(b[1]) && (b.size() == 2 || lower(b[2]));
}
bool valid_script(std::string_view b) {
  return b.size() == 4 && upper(b[0]) && lower(b[1]) && lower(b[2]) && lower(b[3]);
}
bool valid_region(std::string_view b) {
  return (b.size() == 2 && upper(b[0]) && upper(b[1])) ||
         (b.size() == 3 && digit(b[0]) && digit(b[1]) && digit(b[2]));
}

// isSubtag: `subtag` at the start of `buf`, ended by its end, NUL, '-' or '_'.
bool is_subtag(std::string_view buf, std::string_view subtag) {
  if (buf.size() < subtag.size() || buf.substr(0, subtag.size()) != subtag) return false;
  return buf.size() == subtag.size() || buf[subtag.size()] == '\0' || buf[subtag.size()] == '-' ||
         buf[subtag.size()] == '_';
}

EmojiStyle emoji_style_of_extension(std::string_view buf) {
  if (buf.size() >= 7) {
    size_t pos = buf.find("em-");
    if (pos != std::string_view::npos) {
      std::string_view rest = buf.substr(pos + 3);
      if (is_subtag(rest, "emoji")) return EmojiStyle::kEmoji;
      if (is_subtag(rest, "text")) return EmojiStyle::kText;
      if (is_subtag(rest, "default")) return EmojiStyle::kDefault;
    }
  }
  return EmojiStyle::kEmpty;
}

constexpr uint8_t kBopomofo = 1u, kHan = 1u << 1, kHangul = 1u << 2, kHiragana = 1u << 3, kKatakana = 1u << 4,
                  kSimplifiedChinese = 1u << 5, kTraditionalChinese = 1u << 6;

}  // namespace

uint8_t Locale::sub_script_bits(uint32_t script) {
  switch (script) {
    case pack_script('B', 'o', 'p', 'o'): return kBopomofo;
    case pack_script('H', 'a', 'n', 'g'): return kHangul;
    case pack_script('H', 'a', 'n', 'b'): return kHan | kBopomofo;  // Bopomofo: almost only Taiwan
    case pack_script('H', 'a', 'n', 'i'): return kHan;
    case pack_script('H', 'a', 'n', 's'): return kHan | kSimplifiedChinese;
    case pack_script('H', 'a', 'n', 't'): return kHan | kTraditionalChinese;
    case pack_script('H', 'i', 'r', 'a'): return kHiragana;
    case pack_script('H', 'r', 'k', 't'): return kKatakana | kHiragana;
    case pack_script('J', 'p', 'a', 'n'): return kHan | kKatakana | kHiragana;
    case pack_script('K', 'a', 'n', 'a'): return kKatakana;
    case pack_script('K', 'o', 'r', 'e'): return kHan | kHangul;
    default: return 0;
  }
}

Locale::Locale(std::string_view input) {
  std::vector<std::string_view> tokens;
  for (size_t start = 0;;) {
    size_t dash = input.find('-', start);
    tokens.push_back(input.substr(start, dash == std::string_view::npos ? std::string_view::npos : dash - start));
    if (dash == std::string_view::npos) break;
    start = dash + 1;
  }
  size_t t = 0;
  std::string_view language = tokens[t++];
  if (!valid_language(language)) return;  // only two- and three-letter languages are understood
  language_ = pack_language_or_region(language, 'a', 'a');
  // Each subtag is optional; the tag may end after any of them.
  if (t < tokens.size()) {
    std::string_view token = tokens[t++];
    bool more = true;
    auto next = [&] {
      if (t == tokens.size()) {
        more = false;
      } else {
        token = tokens[t++];
      }
    };
    if (valid_script(token)) {
      script_ = pack_script(token[0], token[1], token[2], token[3]);
      sub_script_bits_ = sub_script_bits(script_);
      next();
    }
    if (more && valid_region(token)) {
      region_ = pack_language_or_region(token, 'A', '0');
      next();
    }
    if (more && language == "de") {  // only German orthography variants matter
      if (token == "1901") variant_ = Variant::kGerman1901;
      if (token == "1996") variant_ = Variant::kGerman1996;
      if (variant_ != Variant::kNone && t == tokens.size()) more = false;
    }
    if (more) {
      // resolveUnicodeExtension: the emoji style of a -u- extension.
      size_t pos = input.find("-u-");
      if (pos != std::string_view::npos) emoji_style_ = emoji_style_of_extension(input.substr(pos + 3));
    }
  }
  if (emoji_style_ == EmojiStyle::kEmpty) {
    if (script_ == pack_script('Z', 's', 'y', 'e')) emoji_style_ = EmojiStyle::kEmoji;
    if (script_ == pack_script('Z', 's', 'y', 'm')) emoji_style_ = EmojiStyle::kText;
  }
}

bool Locale::supports_script(uint32_t script_tag) const {
  uint32_t packed = pack_script(script_tag);
  if (packed == script_) return true;
  return supports_script(sub_script_bits_, sub_script_bits(packed));
}

int Locale::score_for(const LocaleList& supported) const {
  bool language_script_match = false, subtag_match = false, script_match = false;
  for (size_t i = 0; i < supported.size(); ++i) {
    const Locale& s = supported[i];
    if (emoji_style_ != EmojiStyle::kEmpty && emoji_style_ == s.emoji_style_) {
      subtag_match = true;
      if (language_ == s.language_) return 4;
    }
    if (same_script(s) || supports_script(s.sub_script_bits_, sub_script_bits_)) {
      script_match = true;
      if (language_ == s.language_) language_script_match = true;
    }
  }
  if (supports_script(supported.union_of_sub_script_bits_, sub_script_bits_)) {
    script_match = true;
    if (language_ == supported[0].language_ && supported.all_same_language_) return 3;
  }
  if (language_script_match) return 3;
  if (subtag_match) return 2;
  if (script_match) return 1;
  return 0;
}

std::string Locale::str() const {
  char buf[32];
  size_t i;
  if (language_ == kNoLanguage) {
    buf[0] = 'u', buf[1] = 'n', buf[2] = 'd';
    i = 3;
  } else {
    i = unpack_language_or_region(language_, buf, 'a', 'a');
  }
  if (script_ != kNoScript) {
    uint32_t raw = unpack_script(script_);
    buf[i++] = '-';
    buf[i++] = char(raw >> 24);
    buf[i++] = char((raw >> 16) & 0xff);
    buf[i++] = char((raw >> 8) & 0xff);
    buf[i++] = char(raw & 0xff);
  }
  if (region_ != kNoRegion) {
    buf[i++] = '-';
    i += unpack_language_or_region(region_, buf + i, 'A', '0');
  }
  if (variant_ != Variant::kNone) {
    buf[i++] = '-';
    buf[i++] = '1';
    buf[i++] = '9';
    buf[i++] = variant_ == Variant::kGerman1901 ? '0' : '9';
    buf[i++] = variant_ == Variant::kGerman1901 ? '1' : '6';
  }
  return std::string(buf, i);
}

LocaleList::LocaleList(std::vector<Locale> locales) : locales_(std::move(locales)) {
  all_same_language_ = true;
  uint16_t first = locales_.empty() ? Locale::kNoLanguage : locales_[0].language_;
  for (const Locale& l : locales_) {
    union_of_sub_script_bits_ |= l.sub_script_bits_;
    if (all_same_language_ && first != l.language_) all_same_language_ = false;
    hb_langs_.push_back(l.supported() ? hb_language_from_string(l.str().c_str(), -1) : HB_LANGUAGE_INVALID);
    if (emoji_style_ == EmojiStyle::kEmpty) emoji_style_ = l.emoji_style();
  }
}

namespace {

// LocaleListCache.
struct Cache {
  std::mutex lock;
  std::deque<LocaleList> lists;  // stable addresses
  std::map<std::vector<uint64_t>, uint32_t> by_locales;
  std::map<std::string, uint32_t, std::less<>> by_string;
  Cache() {
    lists.emplace_back();
    by_locales.emplace(std::vector<uint64_t>{}, kEmptyLocaleListId);
    by_string.emplace("", kEmptyLocaleListId);
  }
};

Cache& cache() {
  static auto* c = new Cache;
  return *c;
}

// Bounded: the strings come from applications.
constexpr size_t kMaxLists = 4096;

std::vector<Locale> parse_list(std::string_view input) {
  std::vector<Locale> result;
  std::unordered_set<uint64_t> seen;
  for (size_t start = 0; start <= input.size();) {
    size_t comma = input.find(',', start);
    std::string_view item = input.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    start = comma == std::string_view::npos ? input.size() + 1 : comma + 1;
    if (item.empty()) continue;
    Locale locale(unicode_services().language_tag(item));
    if (!locale.supported() || !seen.insert(locale.identifier()).second) continue;
    result.push_back(locale);
    if (result.size() >= kFontLocaleLimit) break;
  }
  return result;
}

}  // namespace

uint32_t locale_list_id(std::string_view tags) {
  Cache& c = cache();
  {
    std::lock_guard<std::mutex> guard(c.lock);
    if (auto it = c.by_string.find(tags); it != c.by_string.end()) return it->second;
  }
  std::vector<Locale> locales = parse_list(tags);  // outside the lock: may call into Java
  std::vector<uint64_t> key;
  for (const Locale& l : locales) key.push_back(l.identifier());
  std::lock_guard<std::mutex> guard(c.lock);
  uint32_t id;
  if (auto it = c.by_locales.find(key); it != c.by_locales.end()) {
    id = it->second;
  } else {
    if (c.lists.size() >= kMaxLists) return kInvalidLocaleListId;
    id = uint32_t(c.lists.size());
    c.lists.emplace_back(std::move(locales));
    c.by_locales.emplace(std::move(key), id);
  }
  if (c.by_string.size() < kMaxLists * 4) c.by_string.emplace(std::string(tags), id);
  return id;
}

const LocaleList& locale_list(uint32_t id) {
  Cache& c = cache();
  std::lock_guard<std::mutex> guard(c.lock);
  return id < c.lists.size() ? c.lists[id] : c.lists[0];
}

bool locale_list_id_valid(uint32_t id) {
  Cache& c = cache();
  std::lock_guard<std::mutex> guard(c.lock);
  return id < c.lists.size();
}

}  // namespace aa::text
