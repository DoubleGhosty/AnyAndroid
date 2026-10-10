#include "resources/res_config.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "common/bytes.h"

namespace aa::res {

namespace {

constexpr char kEnglish[2] = {'e', 'n'};
constexpr char kUnitedStates[2] = {'U', 'S'};
constexpr char kTagalog[2] = {'t', 'l'};
constexpr char kFilipino[2] = {char(0xAD), char(0x05)};  // "fil" packed

bool identical(const char a[2], const char b[2]) { return a[0] == b[0] && a[1] == b[1]; }

bool langs_equivalent(const char a[2], const char b[2]) {
  return identical(a, b) || (identical(a, kTagalog) && identical(b, kFilipino)) ||
         (identical(a, kFilipino) && identical(b, kTagalog));
}

bool in_list(std::string_view value, std::initializer_list<std::string_view> list) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

// Likely script of a language (a small subset of CLDR's likely subtags, which
// AOSP compiles into LocaleDataTables.cpp). "" when unknown: matching then
// falls back to comparing countries, as AOSP does for unknown languages.
std::string likely_script(std::string_view lang, std::string_view region) {
  if (lang == "zh") return in_list(region, {"TW", "HK", "MO"}) ? "Hant" : "Hans";
  if (lang == "sr") return region == "ME" ? "Latn" : "Cyrl";
  if (lang == "pa") return region == "PK" ? "Arab" : "Guru";
  if (lang == "uz") return region == "AF" ? "Arab" : "Latn";
  if (lang == "az") return region == "IR" ? "Arab" : "Latn";
  if (lang == "mn") return region == "CN" ? "Mong" : "Cyrl";
  if (lang == "sd") return region == "IN" ? "Deva" : "Arab";
  struct Script {
    std::string_view script;
    std::initializer_list<std::string_view> langs;
  };
  static const Script kScripts[] = {
      {"Arab", {"ar", "fa", "ur", "ps", "ug", "ckb", "ks", "ku"}},
      {"Cyrl", {"ru", "uk", "be", "bg", "mk", "kk", "ky", "tg", "tt", "ba", "cv", "ce", "os", "ab", "sah"}},
      {"Grek", {"el"}},
      {"Hebr", {"he", "iw", "yi"}},
      {"Deva", {"hi", "mr", "ne", "sa", "kok", "mai", "doi", "brx", "bho"}},
      {"Beng", {"bn", "as", "mni"}},
      {"Gujr", {"gu"}},
      {"Orya", {"or"}},
      {"Taml", {"ta"}},
      {"Telu", {"te"}},
      {"Knda", {"kn"}},
      {"Mlym", {"ml"}},
      {"Sinh", {"si"}},
      {"Thai", {"th"}},
      {"Laoo", {"lo"}},
      {"Mymr", {"my"}},
      {"Khmr", {"km"}},
      {"Geor", {"ka"}},
      {"Armn", {"hy"}},
      {"Ethi", {"am", "ti"}},
      {"Tibt", {"bo", "dz"}},
      {"Thaa", {"dv"}},
      {"Kore", {"ko"}},
      {"Jpan", {"ja"}},
      {"Cher", {"chr"}},
      {"Olck", {"sat"}},
      {"Latn", {"en", "fr", "de", "es", "it", "pt", "nl", "sv", "da", "no", "nb", "nn", "fi", "is", "et", "lv",
                "lt", "pl", "cs", "sk", "sl", "hr", "bs", "hu", "ro", "sq", "tr", "ms", "id", "in", "tl", "fil",
                "vi", "sw", "zu", "xh", "af", "ca", "eu", "gl", "ga", "cy", "mt", "lb", "fo", "rm", "br", "co",
                "eo", "la", "ha", "yo", "ig", "so", "om", "rw", "mg", "ny", "sn", "st", "tn", "ts", "ve", "ss",
                "nr", "kl", "gd", "fy", "ht", "mi", "sm", "to", "qu", "ay", "gn", "jv", "su", "ceb", "haw",
                "hmn", "tk", "wo", "ff", "ln", "lg", "ak", "ee", "ki", "kab", "ast", "fur", "sc", "se", "smn"}},
  };
  for (const Script& s : kScripts) {
    if (in_list(lang, s.langs)) return std::string(s.script);
  }
  return {};
}

void compute_script(const char language[2], const char country[2], char out[4]) {
  std::memset(out, 0, 4);
  std::string script = likely_script(unpack_locale_field(language, 'a'), unpack_locale_field(country, '0'));
  if (script.size() == 4) std::memcpy(out, script.data(), 4);
}

bool close_to_us_english(const char region[2]) {
  std::string r = unpack_locale_field(region, '0');
  return in_list(r, {"", "US", "AS", "GU", "MH", "MP", "PR", "UM", "VI"});
}

// Ancestors of a requested region in CLDR's parent locales, closest first,
// "" standing for the language alone (a subset of AOSP's tables).
std::vector<std::string> region_ancestors(std::string_view lang, std::string_view region) {
  std::vector<std::string> out;
  if (!region.empty()) out.emplace_back(region);
  if (lang == "en" && !in_list(region, {"", "US", "AS", "GU", "MH", "MP", "PR", "UM", "VI"})) {
    if (in_list(region, {"AT", "BE", "CH", "DE", "DK", "FI", "NL", "SE", "SI"})) out.emplace_back("150");
    out.emplace_back("001");
  } else if (lang == "es" && in_list(region, {"AR", "BO", "BR", "BZ", "CL", "CO", "CR", "CU", "DO", "EC", "GT",
                                              "HN", "MX", "NI", "PA", "PE", "PR", "PY", "SV", "US", "UY", "VE"})) {
    out.emplace_back("419");
  } else if (lang == "pt" && in_list(region, {"AO", "CH", "CV", "FR", "GQ", "GW", "LU", "MO", "MZ", "ST", "TL"})) {
    out.emplace_back("PT");
  }
  out.emplace_back("");
  return out;
}

int compare_regions(const char left[2], const char right[2], const ResConfig& requested) {
  if (identical(left, right)) return 0;
  std::string l = unpack_locale_field(left, '0'), r = unpack_locale_field(right, '0');
  auto ancestors = region_ancestors(unpack_locale_field(requested.language, 'a'),
                                    unpack_locale_field(requested.country, '0'));
  auto index = [&](const std::string& v) {
    auto it = std::find(ancestors.begin(), ancestors.end(), v);
    return it == ancestors.end() ? ancestors.size() + 1 : size_t(it - ancestors.begin());
  };
  size_t li = index(l), ri = index(r);
  if (li != ri) return li < ri ? 1 : -1;
  // Neither is an ancestor: keep a stable order (two-letter codes, then
  // dictionary order), as AOSP does as a last resort.
  if (l.size() != r.size()) return l.size() < r.size() ? 1 : -1;
  return l < r ? 1 : -1;
}

}  // namespace

void pack_locale_field(std::string_view in, char base, char out[2]) {
  out[0] = out[1] = 0;
  if (in.size() == 2) {
    out[0] = in[0];
    out[1] = in[1];
  } else if (in.size() == 3) {
    uint8_t first = uint8_t(in[0] - base) & 0x7f;
    uint8_t second = uint8_t(in[1] - base) & 0x7f;
    uint8_t third = uint8_t(in[2] - base) & 0x7f;
    out[0] = char(0x80 | (third << 2) | (second >> 3));
    out[1] = char((second << 5) | first);
  }
}

std::string unpack_locale_field(const char in[2], char base) {
  if (in[0] == 0) return {};
  if (uint8_t(in[0]) & 0x80) {
    uint8_t b0 = uint8_t(in[0]), b1 = uint8_t(in[1]);
    std::string out(3, '\0');
    out[0] = char(base + (b1 & 0x1f));
    out[1] = char(base + (((b1 & 0xe0) >> 5) | ((b0 & 0x03) << 3)));
    out[2] = char(base + ((b0 & 0x7c) >> 2));
    return out;
  }
  return std::string(in, in[1] ? 2 : 1);
}

ResConfig ResConfig::parse(std::span<const uint8_t> bytes) {
  if (bytes.size() < 4) throw_format("ResTable_config tronquée");
  uint32_t size = load_u32(bytes.data());
  if (size < 4 || size > bytes.size()) throw_format("taille de ResTable_config invalide");
  uint8_t b[64] = {};
  std::memcpy(b, bytes.data(), std::min<size_t>(size, sizeof(b)));
  ResConfig c;
  c.mcc = load_u16(b + 4);
  c.mnc = load_u16(b + 6);
  std::memcpy(c.language, b + 8, 2);
  std::memcpy(c.country, b + 10, 2);
  c.orientation = b[12];
  c.touchscreen = b[13];
  c.density = load_u16(b + 14);
  c.keyboard = b[16];
  c.navigation = b[17];
  c.input_flags = b[18];
  c.grammatical_inflection = b[19];
  c.screen_width = load_u16(b + 20);
  c.screen_height = load_u16(b + 22);
  c.sdk_version = load_u16(b + 24);
  c.minor_version = load_u16(b + 26);
  c.screen_layout = b[28];
  c.ui_mode = b[29];
  c.smallest_screen_width_dp = load_u16(b + 30);
  c.screen_width_dp = load_u16(b + 32);
  c.screen_height_dp = load_u16(b + 34);
  std::memcpy(c.locale_script, b + 36, 4);
  std::memcpy(c.locale_variant, b + 40, 8);
  c.screen_layout2 = b[48];
  c.color_mode = b[49];
  c.locale_script_was_computed = b[52] != 0;
  std::memcpy(c.locale_numbering_system, b + 53, 8);
  return c;
}

uint32_t ResConfig::locale_key() const {
  return uint32_t(uint8_t(language[0])) | (uint32_t(uint8_t(language[1])) << 8) |
         (uint32_t(uint8_t(country[0])) << 16) | (uint32_t(uint8_t(country[1])) << 24);
}

bool ResConfig::operator==(const ResConfig& o) const {
  return mcc == o.mcc && mnc == o.mnc && identical(language, o.language) && identical(country, o.country) &&
         orientation == o.orientation && touchscreen == o.touchscreen && density == o.density &&
         keyboard == o.keyboard && navigation == o.navigation && input_flags == o.input_flags &&
         grammatical_inflection == o.grammatical_inflection && screen_width == o.screen_width &&
         screen_height == o.screen_height && sdk_version == o.sdk_version && minor_version == o.minor_version &&
         screen_layout == o.screen_layout && ui_mode == o.ui_mode &&
         smallest_screen_width_dp == o.smallest_screen_width_dp && screen_width_dp == o.screen_width_dp &&
         screen_height_dp == o.screen_height_dp && std::memcmp(locale_script, o.locale_script, 4) == 0 &&
         std::memcmp(locale_variant, o.locale_variant, 8) == 0 && screen_layout2 == o.screen_layout2 &&
         color_mode == o.color_mode && std::memcmp(locale_numbering_system, o.locale_numbering_system, 8) == 0;
}

bool ResConfig::match(const ResConfig& settings) const {
  if (has_imsi()) {
    if (mcc != 0 && mcc != settings.mcc) return false;
    if (mnc != 0 && mnc != settings.mnc) return false;
  }
  if (locale_key() != 0) {
    // Countries and variants are weeded out by is_better_than; the script
    // must match when both are known.
    if (!langs_equivalent(language, settings.language)) return false;
    bool countries_must_match = false;
    char computed[4] = {};
    const char* script = nullptr;
    if (settings.locale_script[0] == 0) {
      countries_must_match = true;
    } else if (locale_script[0] == 0 && !locale_script_was_computed) {
      compute_script(language, country, computed);
      if (computed[0] == 0) {
        countries_must_match = true;
      } else {
        script = computed;
      }
    } else {
      script = locale_script;
    }
    if (countries_must_match) {
      if (country[0] != 0 && !identical(country, settings.country)) return false;
    } else if (std::memcmp(script, settings.locale_script, 4) != 0) {
      return false;
    }
  }
  if (has_screen_config()) {
    int layout_dir = screen_layout & kMaskLayoutDir;
    if (layout_dir != 0 && layout_dir != (settings.screen_layout & kMaskLayoutDir)) return false;
    int size = screen_layout & kMaskScreenSize;
    if (size != 0 && size > (settings.screen_layout & kMaskScreenSize)) return false;
    int screen_long = screen_layout & kMaskScreenLong;
    if (screen_long != 0 && screen_long != (settings.screen_layout & kMaskScreenLong)) return false;
    int ui_type = ui_mode & kMaskUiModeType;
    if (ui_type != 0 && ui_type != (settings.ui_mode & kMaskUiModeType)) return false;
    int night = ui_mode & kMaskUiModeNight;
    if (night != 0 && night != (settings.ui_mode & kMaskUiModeNight)) return false;
    if (smallest_screen_width_dp != 0 && smallest_screen_width_dp > settings.smallest_screen_width_dp) return false;
  }
  if (has_screen_config2()) {
    int round = screen_layout2 & kMaskScreenRound;
    if (round != 0 && round != (settings.screen_layout2 & kMaskScreenRound)) return false;
    int hdr = color_mode & kMaskHdr;
    if (hdr != 0 && hdr != (settings.color_mode & kMaskHdr)) return false;
    int wide = color_mode & kMaskWideColorGamut;
    if (wide != 0 && wide != (settings.color_mode & kMaskWideColorGamut)) return false;
  }
  if (has_screen_size_dp()) {
    if (screen_width_dp != 0 && screen_width_dp > settings.screen_width_dp) return false;
    if (screen_height_dp != 0 && screen_height_dp > settings.screen_height_dp) return false;
  }
  if (has_screen_type()) {
    if (orientation != 0 && orientation != settings.orientation) return false;
    // Any density matches: resources are scaled (see is_better_than).
    if (touchscreen != 0 && touchscreen != settings.touchscreen) return false;
  }
  if (has_input()) {
    int keys = input_flags & kMaskKeysHidden;
    int set_keys = settings.input_flags & kMaskKeysHidden;
    if (keys != 0 && keys != set_keys) {
      // KEYSHIDDEN_NO also matches the more recent KEYSHIDDEN_SOFT.
      if (keys != kKeysHiddenNo || set_keys != kKeysHiddenSoft) return false;
    }
    int nav = input_flags & kMaskNavHidden;
    if (nav != 0 && nav != (settings.input_flags & kMaskNavHidden)) return false;
    if (keyboard != 0 && keyboard != settings.keyboard) return false;
    if (navigation != 0 && navigation != settings.navigation) return false;
  }
  if (has_screen_size()) {
    if (screen_width != 0 && screen_width > settings.screen_width) return false;
    if (screen_height != 0 && screen_height > settings.screen_height) return false;
  }
  if (has_version()) {
    if (sdk_version != 0 && sdk_version > settings.sdk_version) return false;
    if (minor_version != 0 && minor_version != settings.minor_version) return false;
  }
  if (grammatical_inflection != 0 && grammatical_inflection != settings.grammatical_inflection) return false;
  return true;
}

int ResConfig::locale_importance() const {
  return (locale_variant[0] ? 4 : 0) + (locale_script[0] && !locale_script_was_computed ? 2 : 0) +
         (locale_numbering_system[0] ? 1 : 0);
}

int ResConfig::is_locale_more_specific_than(const ResConfig& o) const {
  if (locale_key() || o.locale_key()) {
    if (language[0] != o.language[0]) {
      if (!language[0]) return -1;
      if (!o.language[0]) return 1;
    }
    if (country[0] != o.country[0]) {
      if (!country[0]) return -1;
      if (!o.country[0]) return 1;
    }
  }
  return locale_importance() - o.locale_importance();
}

bool ResConfig::is_locale_better_than(const ResConfig& o, const ResConfig* requested) const {
  if (requested->locale_key() == 0) return false;
  if (locale_key() == 0 && o.locale_key() == 0 && !locale_numbering_system[0] && !o.locale_numbering_system[0]) {
    return false;
  }
  if (!langs_equivalent(language, o.language)) {
    // One has no language. It is the better match, except for US English
    // and close locales, whose resources traditionally live without one.
    if (identical(requested->language, kEnglish)) {
      if (identical(requested->country, kUnitedStates)) {
        if (language[0] != 0) return country[0] == 0 || identical(country, kUnitedStates);
        return !(o.country[0] == 0 || identical(o.country, kUnitedStates));
      }
      if (close_to_us_english(requested->country)) {
        if (language[0] != 0) return close_to_us_english(country);
        return !close_to_us_english(o.country);
      }
    }
    return language[0] != 0;
  }
  int regions = compare_regions(country, o.country, *requested);
  if (regions != 0) return regions > 0;
  bool variant = std::strncmp(locale_variant, requested->locale_variant, 8) == 0;
  bool o_variant = std::strncmp(o.locale_variant, requested->locale_variant, 8) == 0;
  if (variant != o_variant) return variant;
  bool numsys = std::strncmp(locale_numbering_system, requested->locale_numbering_system, 8) == 0;
  bool o_numsys = std::strncmp(o.locale_numbering_system, requested->locale_numbering_system, 8) == 0;
  if (numsys != o_numsys) return numsys;
  return identical(language, requested->language) && !identical(o.language, requested->language);
}

bool ResConfig::is_more_specific_than(const ResConfig& o) const {
  if (has_imsi() || o.has_imsi()) {
    if (mcc != o.mcc) {
      if (!mcc) return false;
      if (!o.mcc) return true;
    }
    if (mnc != o.mnc) {
      if (!mnc) return false;
      if (!o.mnc) return true;
    }
  }
  if (locale_key() || o.locale_key()) {
    int diff = is_locale_more_specific_than(o);
    if (diff < 0) return false;
    if (diff > 0) return true;
  }
  if ((grammatical_inflection || o.grammatical_inflection) && grammatical_inflection != o.grammatical_inflection) {
    if (!grammatical_inflection) return false;
    if (!o.grammatical_inflection) return true;
  }
  // Qualifiers in decreasing order of importance: the first that differs
  // decides (a set qualifier is more specific than an unset one).
  const std::pair<uint32_t, uint32_t> order[] = {
      {screen_layout & kMaskLayoutDir, o.screen_layout & kMaskLayoutDir},
      {smallest_screen_width_dp, o.smallest_screen_width_dp},
      {screen_width_dp, o.screen_width_dp},
      {screen_height_dp, o.screen_height_dp},
      {screen_layout & kMaskScreenSize, o.screen_layout & kMaskScreenSize},
      {screen_layout & kMaskScreenLong, o.screen_layout & kMaskScreenLong},
      {screen_layout2 & kMaskScreenRound, o.screen_layout2 & kMaskScreenRound},
      {color_mode & kMaskHdr, o.color_mode & kMaskHdr},
      {color_mode & kMaskWideColorGamut, o.color_mode & kMaskWideColorGamut},
      {orientation, o.orientation},
      {ui_mode & kMaskUiModeType, o.ui_mode & kMaskUiModeType},
      {ui_mode & kMaskUiModeNight, o.ui_mode & kMaskUiModeNight},
      // Density is never "more specific": the default equals 160.
      {touchscreen, o.touchscreen},
      {input_flags & kMaskKeysHidden, o.input_flags & kMaskKeysHidden},
      {input_flags & kMaskNavHidden, o.input_flags & kMaskNavHidden},
      {keyboard, o.keyboard},
      {navigation, o.navigation},
      {screen_width, o.screen_width},
      {screen_height, o.screen_height},
      {sdk_version, o.sdk_version},
      {minor_version, o.minor_version},
  };
  for (const auto& [mine, other] : order) {
    // Two different set values do not decide: only set versus unset counts.
    if (mine != other && (mine == 0 || other == 0)) return mine != 0;
  }
  return false;
}

bool ResConfig::is_better_than(const ResConfig& o, const ResConfig* requested) const {
  if (!requested) return is_more_specific_than(o);
  const ResConfig& req = *requested;
  if (has_imsi() || o.has_imsi()) {
    if (mcc != o.mcc && req.mcc) return mcc != 0;
    if (mnc != o.mnc && req.mnc) return mnc != 0;
  }
  if (is_locale_better_than(o, requested)) return true;
  if (o.is_locale_better_than(*this, requested)) return false;
  if ((grammatical_inflection || o.grammatical_inflection) && grammatical_inflection != o.grammatical_inflection &&
      req.grammatical_inflection) {
    return grammatical_inflection != 0;
  }
  if ((screen_layout || o.screen_layout) && ((screen_layout ^ o.screen_layout) & kMaskLayoutDir) &&
      (req.screen_layout & kMaskLayoutDir)) {
    return (screen_layout & kMaskLayoutDir) > (o.screen_layout & kMaskLayoutDir);
  }
  if ((smallest_screen_width_dp || o.smallest_screen_width_dp) &&
      smallest_screen_width_dp != o.smallest_screen_width_dp) {
    // Larger configurations were filtered out by match(): the largest wins.
    return smallest_screen_width_dp > o.smallest_screen_width_dp;
  }
  if (has_screen_size_dp() || o.has_screen_size_dp()) {
    int mine = 0, other = 0;
    if (req.screen_width_dp) {
      mine += req.screen_width_dp - screen_width_dp;
      other += req.screen_width_dp - o.screen_width_dp;
    }
    if (req.screen_height_dp) {
      mine += req.screen_height_dp - screen_height_dp;
      other += req.screen_height_dp - o.screen_height_dp;
    }
    if (mine != other) return mine < other;
  }
  if (screen_layout || o.screen_layout) {
    if (((screen_layout ^ o.screen_layout) & kMaskScreenSize) && (req.screen_layout & kMaskScreenSize)) {
      // Undefined counts as normal when at least normal is requested.
      int my_size = screen_layout & kMaskScreenSize, o_size = o.screen_layout & kMaskScreenSize;
      int fixed_mine = my_size, fixed_other = o_size;
      if ((req.screen_layout & kMaskScreenSize) >= kScreenSizeNormal) {
        if (fixed_mine == 0) fixed_mine = kScreenSizeNormal;
        if (fixed_other == 0) fixed_other = kScreenSizeNormal;
      }
      if (fixed_mine == fixed_other) return my_size != 0;
      return fixed_mine > fixed_other;
    }
    if (((screen_layout ^ o.screen_layout) & kMaskScreenLong) && (req.screen_layout & kMaskScreenLong)) {
      return (screen_layout & kMaskScreenLong) != 0;
    }
  }
  if ((screen_layout2 || o.screen_layout2) && ((screen_layout2 ^ o.screen_layout2) & kMaskScreenRound) &&
      (req.screen_layout2 & kMaskScreenRound)) {
    return (screen_layout2 & kMaskScreenRound) != 0;
  }
  if (color_mode || o.color_mode) {
    if (((color_mode ^ o.color_mode) & kMaskHdr) && (req.color_mode & kMaskHdr)) return (color_mode & kMaskHdr) != 0;
    if (((color_mode ^ o.color_mode) & kMaskWideColorGamut) && (req.color_mode & kMaskWideColorGamut)) {
      return (color_mode & kMaskWideColorGamut) != 0;
    }
  }
  if (orientation != o.orientation && req.orientation) return orientation != 0;
  if (ui_mode || o.ui_mode) {
    if (((ui_mode ^ o.ui_mode) & kMaskUiModeType) && (req.ui_mode & kMaskUiModeType)) {
      return (ui_mode & kMaskUiModeType) != 0;
    }
    if (((ui_mode ^ o.ui_mode) & kMaskUiModeNight) && (req.ui_mode & kMaskUiModeNight)) {
      return (ui_mode & kMaskUiModeNight) != 0;
    }
  }
  if (has_screen_type() || o.has_screen_type()) {
    if (density != o.density) {
      int mine = density ? density : kDensityMedium;
      int other = o.density ? o.density : kDensityMedium;
      // DENSITY_ANY always beats scaling a bucket.
      if (mine == kDensityAny) return true;
      if (other == kDensityAny) return false;
      int wanted = req.density;
      if (wanted == 0 || wanted == kDensityAny) wanted = kDensityMedium;
      int h = mine, l = other;
      bool i_am_bigger = true;
      if (l > h) {
        std::swap(l, h);
        i_am_bigger = false;
      }
      if (wanted >= h) return i_am_bigger;
      if (l >= wanted) return !i_am_bigger;
      // Scaling down is considered twice as good as scaling up.
      if (((2 * l) - wanted) * h > wanted * wanted) return !i_am_bigger;
      return i_am_bigger;
    }
    if (touchscreen != o.touchscreen && req.touchscreen) return touchscreen != 0;
  }
  if (has_input() || o.has_input()) {
    int keys = input_flags & kMaskKeysHidden, o_keys = o.input_flags & kMaskKeysHidden;
    if (keys != o_keys) {
      int req_keys = req.input_flags & kMaskKeysHidden;
      if (req_keys) {
        if (!keys) return false;
        if (!o_keys) return true;
        if (req_keys == keys) return true;
        if (req_keys == o_keys) return false;
      }
    }
    int nav = input_flags & kMaskNavHidden, o_nav = o.input_flags & kMaskNavHidden;
    if (nav != o_nav && (req.input_flags & kMaskNavHidden)) {
      if (!nav) return false;
      if (!o_nav) return true;
    }
    if (keyboard != o.keyboard && req.keyboard) return keyboard != 0;
    if (navigation != o.navigation && req.navigation) return navigation != 0;
  }
  if (has_screen_size() || o.has_screen_size()) {
    int mine = 0, other = 0;
    if (req.screen_width) {
      mine += req.screen_width - screen_width;
      other += req.screen_width - o.screen_width;
    }
    if (req.screen_height) {
      mine += req.screen_height - screen_height;
      other += req.screen_height - o.screen_height;
    }
    if (mine != other) return mine < other;
  }
  if (has_version() || o.has_version()) {
    if (sdk_version != o.sdk_version && req.sdk_version) return sdk_version > o.sdk_version;
    if (minor_version != o.minor_version && req.minor_version) return minor_version != 0;
  }
  return false;
}

void ResConfig::set_bcp47_locale(std::string_view tag) {
  std::memset(language, 0, 2);
  std::memset(country, 0, 2);
  std::memset(locale_script, 0, 4);
  std::memset(locale_variant, 0, 8);
  std::memset(locale_numbering_system, 0, 8);
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= tag.size(); ++i) {
    if (i == tag.size() || tag[i] == '-' || tag[i] == '_') {
      parts.emplace_back(tag.substr(start, i - start));
      start = i + 1;
    }
  }
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
  };
  auto all = [](const std::string& s, int (*pred)(int)) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [pred](char c) { return pred(uint8_t(c)) != 0; });
  };
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string p = parts[i];
    if (i == 0) {
      if ((p.size() == 2 || p.size() == 3) && all(p, isalpha)) pack_locale_field(lower(p), 'a', language);
      continue;
    }
    std::string l = lower(p);
    if (l == "x") break;  // private use
    if (l == "u") {
      // Unicode extension: only the numbering system ("nu") is kept.
      for (size_t k = i + 1; k + 1 < parts.size(); ++k) {
        if (lower(parts[k]) == "nu" && parts[k + 1].size() >= 3 && parts[k + 1].size() <= 8) {
          std::memcpy(locale_numbering_system, lower(parts[k + 1]).data(), parts[k + 1].size());
        }
      }
      break;
    }
    if (p.size() == 4 && all(p, isalpha)) {
      locale_script[0] = char(std::toupper(uint8_t(p[0])));
      for (int k = 1; k < 4; ++k) locale_script[k] = char(std::tolower(uint8_t(p[size_t(k)])));
    } else if ((p.size() == 2 && all(p, isalpha)) || (p.size() == 3 && all(p, isdigit))) {
      std::string up = p;
      for (char& c : up) c = char(std::toupper(uint8_t(c)));
      pack_locale_field(up, '0', country);
    } else if ((p.size() >= 5 && p.size() <= 8) || (p.size() == 4 && std::isdigit(uint8_t(p[0])))) {
      std::memcpy(locale_variant, l.data(), l.size());
    }
  }
  locale_script_was_computed = locale_script[0] == 0;
  if (locale_script_was_computed) compute_script(language, country, locale_script);
}

std::string ResConfig::bcp47_locale() const {
  if (language[0] == 0 && country[0] == 0) return {};
  std::string out;
  if (language[0] != 0) out = identical(language, kTagalog) ? "fil" : unpack_locale_field(language, 'a');
  if (locale_script[0] != 0 && !locale_script_was_computed) {
    if (!out.empty()) out += '-';
    out.append(locale_script, 4);
  }
  if (country[0] != 0) {
    if (!out.empty()) out += '-';
    out += unpack_locale_field(country, '0');
  }
  if (locale_variant[0] != 0) {
    if (!out.empty()) out += '-';
    out.append(locale_variant, strnlen(locale_variant, 8));
  }
  if (locale_numbering_system[0] != 0 && !out.empty()) {
    out += "-u-nu-";
    out.append(locale_numbering_system, strnlen(locale_numbering_system, 8));
  }
  return out;
}

std::string ResConfig::to_string() const {
  std::vector<std::string> q;
  if (mcc) q.push_back("mcc" + std::to_string(mcc));
  if (mnc) q.push_back("mnc" + std::to_string(mnc));
  if (language[0]) {
    if (locale_script[0] && !locale_script_was_computed) {
      q.push_back("b+" + unpack_locale_field(language, 'a') + "+" + std::string(locale_script, 4) +
                  (country[0] ? "+" + unpack_locale_field(country, '0') : ""));
    } else {
      q.push_back(unpack_locale_field(language, 'a'));
      if (country[0]) q.push_back("r" + unpack_locale_field(country, '0'));
    }
  }
  switch (screen_layout & kMaskLayoutDir) {
    case 0x40: q.push_back("ldltr"); break;
    case 0x80: q.push_back("ldrtl"); break;
  }
  if (smallest_screen_width_dp) q.push_back("sw" + std::to_string(smallest_screen_width_dp) + "dp");
  if (screen_width_dp) q.push_back("w" + std::to_string(screen_width_dp) + "dp");
  if (screen_height_dp) q.push_back("h" + std::to_string(screen_height_dp) + "dp");
  static const char* kSizes[] = {"", "small", "normal", "large", "xlarge"};
  if (int s = screen_layout & kMaskScreenSize; s > 0 && s <= 4) q.emplace_back(kSizes[s]);
  if ((screen_layout & kMaskScreenLong) == 0x10) q.emplace_back("notlong");
  if ((screen_layout & kMaskScreenLong) == 0x20) q.emplace_back("long");
  if ((screen_layout2 & kMaskScreenRound) == 1) q.emplace_back("notround");
  if ((screen_layout2 & kMaskScreenRound) == 2) q.emplace_back("round");
  if ((color_mode & kMaskWideColorGamut) == 1) q.emplace_back("nowidecg");
  if ((color_mode & kMaskWideColorGamut) == 2) q.emplace_back("widecg");
  if ((color_mode & kMaskHdr) == 4) q.emplace_back("lowdr");
  if ((color_mode & kMaskHdr) == 8) q.emplace_back("highdr");
  static const char* kOrientations[] = {"", "port", "land", "square"};
  if (orientation > 0 && orientation <= 3) q.emplace_back(kOrientations[orientation]);
  static const char* kUiModes[] = {"", "", "desk", "car", "television", "appliance", "watch", "vrheadset"};
  if (int t = ui_mode & kMaskUiModeType; t >= 2 && t <= 7) q.emplace_back(kUiModes[t]);
  if ((ui_mode & kMaskUiModeNight) == 0x10) q.emplace_back("notnight");
  if ((ui_mode & kMaskUiModeNight) == 0x20) q.emplace_back("night");
  switch (density) {
    case 0: break;
    case 120: q.emplace_back("ldpi"); break;
    case 160: q.emplace_back("mdpi"); break;
    case 213: q.emplace_back("tvdpi"); break;
    case 240: q.emplace_back("hdpi"); break;
    case 320: q.emplace_back("xhdpi"); break;
    case 480: q.emplace_back("xxhdpi"); break;
    case 640: q.emplace_back("xxxhdpi"); break;
    case kDensityAny: q.emplace_back("anydpi"); break;
    case kDensityNone: q.emplace_back("nodpi"); break;
    default: q.push_back(std::to_string(density) + "dpi"); break;
  }
  static const char* kTouch[] = {"", "notouch", "stylus", "finger"};
  if (touchscreen > 0 && touchscreen <= 3) q.emplace_back(kTouch[touchscreen]);
  static const char* kKeys[] = {"", "keysexposed", "keyshidden", "keyssoft"};
  if (int k = input_flags & kMaskKeysHidden; k > 0) q.emplace_back(kKeys[k]);
  static const char* kKeyboards[] = {"", "nokeys", "qwerty", "12key"};
  if (keyboard > 0 && keyboard <= 3) q.emplace_back(kKeyboards[keyboard]);
  if ((input_flags & kMaskNavHidden) == 0x04) q.emplace_back("navexposed");
  if ((input_flags & kMaskNavHidden) == 0x08) q.emplace_back("navhidden");
  static const char* kNavs[] = {"", "nonav", "dpad", "trackball", "wheel"};
  if (navigation > 0 && navigation <= 4) q.emplace_back(kNavs[navigation]);
  if (screen_width || screen_height) q.push_back(std::to_string(screen_width) + "x" + std::to_string(screen_height));
  if (sdk_version) q.push_back("v" + std::to_string(sdk_version) + (minor_version ? "." + std::to_string(minor_version) : ""));
  std::string out;
  for (const auto& s : q) {
    if (!out.empty()) out += '-';
    out += s;
  }
  return out;
}

}  // namespace aa::res
