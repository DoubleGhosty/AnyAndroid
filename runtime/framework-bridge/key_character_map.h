#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// libinput's KeyCharacterMap (frameworks/native/libs/input/KeyCharacterMap.cpp,
// Android 17): the .kcm files of /system/usr/keychars (keyboard type, the
// character of each key for each set of modifiers, fallbacks, replacements,
// scan code and HID usage mappings), and the queries of
// android.view.KeyCharacterMap. Independent of the Java runtime.
namespace aa::bridge {

// AMETA_* (android/input.h), KeyEvent.META_* in Java.
enum : int32_t {
  kMetaShiftOn = 0x01,
  kMetaAltOn = 0x02,
  kMetaSymOn = 0x04,
  kMetaFunctionOn = 0x08,
  kMetaAltLeftOn = 0x10,
  kMetaAltRightOn = 0x20,
  kMetaShiftLeftOn = 0x40,
  kMetaShiftRightOn = 0x80,
  kMetaCtrlOn = 0x1000,
  kMetaCtrlLeftOn = 0x2000,
  kMetaCtrlRightOn = 0x4000,
  kMetaMetaOn = 0x10000,
  kMetaMetaLeftOn = 0x20000,
  kMetaMetaRightOn = 0x40000,
  kMetaCapsLockOn = 0x100000,
  kMetaNumLockOn = 0x200000,
  kMetaScrollLockOn = 0x400000,
};

// Keyboard.cpp's normalizeMetaState and updateMetaState.
int32_t normalize_meta_state(int32_t meta_state);
int32_t update_meta_state(int32_t key_code, bool down, int32_t meta_state);

class KeyCharacterMap {
 public:
  enum class KeyboardType : int32_t { kUnknown = 0, kNumeric, kPredictive, kAlpha, kFull, kSpecialFunction, kOverlay };
  enum class Format { kBase, kOverlay, kAny };
  // KEYCODE_ labels (without the prefix) to key codes.
  using KeyCodeLookup = std::function<std::optional<int32_t>(std::string_view label)>;

  struct Behavior {
    int32_t meta_state = 0;
    char16_t character = 0;
    int32_t fallback_key_code = 0;
    int32_t replacement_key_code = 0;
    bool operator==(const Behavior&) const = default;
  };
  struct Key {
    char16_t label = 0;
    char16_t number = 0;
    std::list<Behavior> behaviors;  // the most specific first
    bool operator==(const Key&) const = default;
  };
  struct FallbackAction {
    int32_t key_code = 0, meta_state = 0;
  };
  // A key event of getEvents.
  struct KeyStroke {
    int32_t key_code = 0, meta_state = 0;
    bool down = false;
  };

  explicit KeyCharacterMap(std::string load_file_name = {}) : load_file_name_(std::move(load_file_name)) {}

  // KeyCharacterMap::loadContents: nullptr, with the reason in *error, when
  // the contents are malformed (the parser's messages).
  static std::unique_ptr<KeyCharacterMap> load_contents(const std::string& name, std::string_view contents,
                                                        Format format, const KeyCodeLookup& lookup,
                                                        std::string* error);

  KeyboardType keyboard_type() const { return type_; }
  const std::string& load_file_name() const { return load_file_name_; }
  char16_t display_label(int32_t key_code) const;
  char16_t number(int32_t key_code) const;
  char16_t character(int32_t key_code, int32_t meta_state) const;
  bool fallback_action(int32_t key_code, int32_t meta_state, FallbackAction* out) const;
  char16_t match(int32_t key_code, const char16_t* chars, size_t count, int32_t meta_state) const;
  // The key strokes that type `chars` (with their modifier keys); false when
  // a character has no key.
  bool events(const char16_t* chars, size_t count, std::vector<KeyStroke>& out) const;
  // mapKey: the key code of a scan code or HID usage; false when unmapped.
  bool map_key(int32_t scan_code, int32_t usage_code, int32_t* out_key_code) const;
  // combine: an overlay's keys and mappings over these (the base is
  // reloaded first when an overlay was already applied).
  void combine(const KeyCharacterMap& overlay);
  bool operator==(const KeyCharacterMap& o) const {
    return keys_ == o.keys_ && type_ == o.type_ && load_file_name_ == o.load_file_name_ &&
           overlay_applied_ == o.overlay_applied_ && key_remapping_ == o.key_remapping_ &&
           keys_by_scan_code_ == o.keys_by_scan_code_ && keys_by_usage_code_ == o.keys_by_usage_code_;
  }

  // Parcel layout of writeToParcel/readFromParcel, over int32 and String8
  // primitives.
  struct ParcelWriter {
    std::function<void(int32_t)> write_i32;
    std::function<void(const std::string&)> write_string8;
  };
  struct ParcelReader {
    std::function<int32_t()> read_i32;
    std::function<std::string()> read_string8;
    std::function<bool()> failed;
  };
  void write_to_parcel(const ParcelWriter& w) const;
  static std::unique_ptr<KeyCharacterMap> read_from_parcel(const ParcelReader& r);

 private:
  class Parser;
  const Key* key(int32_t key_code) const;
  const Behavior* key_behavior(int32_t key_code, int32_t meta_state) const;
  static bool matches_meta_state(int32_t event_meta_state, int32_t behavior_meta_state);
  bool find_key(char16_t ch, int32_t* out_key_code, int32_t* out_meta_state) const;

  std::map<int32_t, Key> keys_;
  KeyboardType type_ = KeyboardType::kUnknown;
  std::string load_file_name_;
  bool overlay_applied_ = false;
  std::unordered_map<int32_t, int32_t> key_remapping_;
  std::map<int32_t, int32_t> keys_by_scan_code_;
  std::map<int32_t, int32_t> keys_by_usage_code_;
  // The base contents, to reload when another overlay is combined.
  std::string base_contents_;
  KeyCodeLookup lookup_;
};

}  // namespace aa::bridge
