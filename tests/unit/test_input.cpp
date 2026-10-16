// libinput's KeyCharacterMap (framework-bridge/key_character_map.h): the .kcm
// syntax of Android's keyboards, modifier matching, typed characters as key
// strokes, overlays and the parcel layout.
#include <map>
#include <string>
#include <vector>

#include "framework-bridge/key_character_map.h"
#include "test.h"

using namespace aa;
using bridge::KeyCharacterMap;

namespace {

// KeyEvent's codes of the keys used below.
constexpr int32_t kA = 29, kB = 30, k1 = 8, kSpace = 62, kShiftLeft = 59, kCapsLock = 115, kDpadUp = 19;

KeyCharacterMap::KeyCodeLookup lookup() {
  static const std::map<std::string, int32_t, std::less<>> codes = {
      {"A", kA}, {"B", kB}, {"1", k1}, {"SPACE", kSpace}, {"SHIFT_LEFT", kShiftLeft},
      {"CAPS_LOCK", kCapsLock}, {"DPAD_UP", kDpadUp}};
  return [](std::string_view label) -> std::optional<int32_t> {
    auto it = codes.find(label);
    if (it == codes.end()) return std::nullopt;
    return it->second;
  };
}

// As Virtual.kcm writes its keys.
constexpr const char* kVirtual = R"(# Comment
type FULL

key A {
    label:                              'A'
    base:                               'a'
    shift, capslock:                    'A'
    shift+capslock:                     'a'
}

key 1 {
    label:                              '1'
    base:                               '1'
    shift:                              '!'
}

key SPACE {
    label:                              ' '
    base:                               ' '
    alt, meta:                          fallback SEARCH
    ctrl:                               none
}

map key 30 B
)";

std::unique_ptr<KeyCharacterMap> load(const std::string& text, KeyCharacterMap::Format format, std::string* error) {
  auto lookup_with_search = [base = lookup()](std::string_view label) -> std::optional<int32_t> {
    if (label == "SEARCH") return 84;
    return base(label);
  };
  return KeyCharacterMap::load_contents("test.kcm", text, format, lookup_with_search, error);
}

}  // namespace

TEST(input_key_character_map_parses_kcm_files) {
  std::string error;
  auto map = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  CHECK(map != nullptr);
  CHECK_EQ(error, std::string());
  CHECK(map->keyboard_type() == KeyCharacterMap::KeyboardType::kFull);
  CHECK_EQ(map->display_label(kA), char16_t('A'));
  CHECK_EQ(map->character(kA, 0), char16_t('a'));
  CHECK_EQ(map->character(kA, bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn), char16_t('A'));
  CHECK_EQ(map->character(kA, bridge::kMetaCapsLockOn), char16_t('A'));
  CHECK_EQ(map->character(kA, bridge::kMetaShiftOn | bridge::kMetaCapsLockOn), char16_t('a'));
  // CTRL, ALT and META must match exactly: no character for Ctrl+A.
  CHECK_EQ(map->character(kA, bridge::kMetaCtrlOn), char16_t(0));
  CHECK_EQ(map->number(k1), char16_t('1'));      // a digit
  CHECK_EQ(map->number(kSpace), char16_t(0));    // neither a digit nor a symbol
  CHECK_EQ(map->character(kSpace, bridge::kMetaCtrlOn), char16_t(0));  // ctrl: none
  KeyCharacterMap::FallbackAction fallback;
  CHECK(map->fallback_action(kSpace, bridge::kMetaAltOn | bridge::kMetaAltLeftOn, &fallback));
  CHECK_EQ(fallback.key_code, 84);
  CHECK_EQ(fallback.meta_state, bridge::kMetaAltLeftOn);  // the behavior's modifier removed
  int32_t mapped = 0;
  CHECK(map->map_key(30, 0, &mapped));
  CHECK_EQ(mapped, kB);
  const char16_t candidates[] = {u'!', u'x'};
  CHECK_EQ(map->match(k1, candidates, 2, 0), char16_t('!'));
}

TEST(input_key_character_map_types_characters_as_key_strokes) {
  std::string error;
  auto map = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  std::vector<KeyCharacterMap::KeyStroke> strokes;
  CHECK(map->events(u"a!", 2, strokes));
  // 'a': A down, up; '!': SHIFT_LEFT down (shift on), 1 down, 1 up, SHIFT_LEFT up.
  CHECK_EQ(strokes.size(), size_t(6));
  CHECK_EQ(strokes[0].key_code, kA);
  CHECK(strokes[0].down && !strokes[1].down);
  CHECK_EQ(strokes[2].key_code, kShiftLeft);
  CHECK_EQ(strokes[2].meta_state, bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn);
  CHECK_EQ(strokes[3].key_code, k1);
  CHECK_EQ(strokes[3].meta_state, bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn);
  CHECK_EQ(strokes[5].key_code, kShiftLeft);
  CHECK(!strokes[5].down);
  CHECK_EQ(strokes[5].meta_state, 0);
  strokes.clear();
  CHECK(!map->events(u"z", 1, strokes));  // no key types 'z'
}

TEST(input_key_character_map_overlays_and_parcels) {
  std::string error;
  auto base = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  auto overlay = load("type OVERLAY\nkey A {\n  base: 'q'\n}\n", KeyCharacterMap::Format::kOverlay, &error);
  CHECK(overlay != nullptr);
  KeyCharacterMap combined = *base;
  combined.combine(*overlay);
  CHECK_EQ(combined.character(kA, 0), char16_t('q'));
  CHECK_EQ(combined.character(k1, 0), char16_t('1'));
  CHECK_EQ(base->character(kA, 0), char16_t('a'));

  std::vector<int32_t> ints;
  std::vector<std::string> strings;
  base->write_to_parcel({[&](int32_t v) { ints.push_back(v); }, [&](const std::string& s) { strings.push_back(s); }});
  size_t i = 0, j = 0;
  bool failed = false;
  auto copy = KeyCharacterMap::read_from_parcel(
      {[&] {
         if (i < ints.size()) return ints[i++];
         failed = true;
         return 0;
       },
       [&] { return j < strings.size() ? strings[j++] : std::string(); }, [&] { return failed; }});
  CHECK(copy != nullptr);
  CHECK(*copy == *base);
  CHECK_EQ(i, ints.size());
}

TEST(input_key_character_map_rejects_malformed_files) {
  std::string error;
  CHECK(load("key A {\n base: 'a'\n}\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(error.find("missing required keyboard 'type'") != std::string::npos);
  CHECK(load("type FULL\nkey NOPE {\n}\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(error.find("test.kcm:2") != std::string::npos);
  CHECK(load("type FULL\nkey A {\n base: 'a' 'b'\n}\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(load("type FULL\nkey A {\n base: 'a'\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(error.find("Unterminated") != std::string::npos);
  CHECK(load("type OVERLAY\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(load("type FULL\nkey A {\n base: '\\u0041'\n}\n", KeyCharacterMap::Format::kBase, &error) != nullptr);
}
