#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// ResTable_config (frameworks/base/libs/androidfw/ResourceTypes.h): the
// qualifiers of a resource variant (values-fr-land, drawable-xhdpi...) and of
// the device configuration it is matched against. The selection rules
// (match, isBetterThan, isMoreSpecificThan) follow ResourceTypes.cpp.
namespace aa::res {

struct ResConfig {
  // Constants of ResTable_config.
  static constexpr uint8_t kMaskKeysHidden = 0x03, kKeysHiddenNo = 0x01, kKeysHiddenSoft = 0x03;
  static constexpr uint8_t kMaskNavHidden = 0x0c;
  static constexpr uint8_t kMaskScreenSize = 0x0f, kScreenSizeNormal = 0x02;
  static constexpr uint8_t kMaskScreenLong = 0x30, kMaskLayoutDir = 0xc0;
  static constexpr uint8_t kMaskUiModeType = 0x0f, kMaskUiModeNight = 0x30;
  static constexpr uint8_t kMaskScreenRound = 0x03;
  static constexpr uint8_t kMaskWideColorGamut = 0x03, kMaskHdr = 0x0c;
  static constexpr uint16_t kDensityMedium = 160, kDensityAny = 0xfffe, kDensityNone = 0xffff;

  uint16_t mcc = 0, mnc = 0;
  char language[2] = {}, country[2] = {};  // packed when 3 letters/digits
  uint8_t orientation = 0, touchscreen = 0;
  uint16_t density = 0;
  uint8_t keyboard = 0, navigation = 0, input_flags = 0, grammatical_inflection = 0;
  uint16_t screen_width = 0, screen_height = 0;
  uint16_t sdk_version = 0, minor_version = 0;
  uint8_t screen_layout = 0, ui_mode = 0;
  uint16_t smallest_screen_width_dp = 0;
  uint16_t screen_width_dp = 0, screen_height_dp = 0;
  char locale_script[4] = {};
  char locale_variant[8] = {};
  uint8_t screen_layout2 = 0, color_mode = 0;
  bool locale_script_was_computed = false;
  char locale_numbering_system[8] = {};

  // Reads a size-prefixed ResTable_config; fields beyond its size stay zero.
  // Throws FormatError when the size is impossible.
  static ResConfig parse(std::span<const uint8_t> bytes);

  bool match(const ResConfig& settings) const;
  bool is_better_than(const ResConfig& o, const ResConfig* requested) const;
  bool is_more_specific_than(const ResConfig& o) const;

  // BCP 47 tags ("fr-FR", "zh-Hant-TW", "en-US-u-nu-latn"). The script is
  // computed from the language when the tag has none.
  void set_bcp47_locale(std::string_view tag);
  std::string bcp47_locale() const;  // "" without a language
  // aapt-style qualifiers ("fr-rFR-land-xhdpi-v21"), "" for the default.
  std::string to_string() const;

  uint32_t locale_key() const;  // language and country, 0 when unset
  bool has_imsi() const { return mcc || mnc; }
  bool has_screen_type() const { return orientation || touchscreen || density; }
  bool has_input() const { return keyboard || navigation || input_flags; }
  bool has_screen_size() const { return screen_width || screen_height; }
  bool has_version() const { return sdk_version || minor_version; }
  bool has_screen_config() const { return screen_layout || ui_mode || smallest_screen_width_dp; }
  bool has_screen_size_dp() const { return screen_width_dp || screen_height_dp; }
  bool has_screen_config2() const { return screen_layout2 || color_mode; }

  bool operator==(const ResConfig& o) const;

 private:
  bool is_locale_better_than(const ResConfig& o, const ResConfig* requested) const;
  int is_locale_more_specific_than(const ResConfig& o) const;
  int locale_importance() const;
};

// Packs a 2- or 3-letter language ('a' base) or region ('0' base) code the
// way ResTable_config stores it; unpack returns "" for an empty field.
void pack_locale_field(std::string_view in, char base, char out[2]);
std::string unpack_locale_field(const char in[2], char base);

}  // namespace aa::res
