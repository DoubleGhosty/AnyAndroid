// libinput's KeyCharacterMap (framework-bridge/key_character_map.h): the .kcm
// syntax of Android's keyboards, modifier matching, typed characters as key
// strokes, overlays and the parcel layout. Then the input dispatcher's
// decisions (input_routing.h), the input scripts of `exec --input` and the
// window's letterbox mapping.
#include <map>
#include <string>
#include <vector>

#include "framework-bridge/input_routing.h"
#include "framework-bridge/key_character_map.h"
#include "headless/input_script.h"
#include "test.h"
#include "windows/windowing/letterbox.h"

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

namespace {

// A window of the dispatcher's list (from the top), its touchable region
// being its frame unless given.
bridge::InputWindow window(uint64_t connection, int32_t type, int32_t flags, bridge::InputRect frame,
                           bool visible = true) {
  bridge::InputWindow w;
  w.connection = connection;
  w.type = type;
  w.layout_flags = flags;
  w.frame = frame;
  w.touchable = frame;
  w.visible = visible;
  w.focusable = visible && (flags & bridge::kWindowFlagNotFocusable) == 0;
  return w;
}

constexpr bridge::InputRect kScreen{0, 0, 1080, 1920};

}  // namespace

// InputDispatcher: a touch goes to the topmost visible window that accepts
// it (a toast, NOT_TOUCHABLE, never takes it); keys to the topmost focusable
// window (a toast is not focusable either).
TEST(input_routing_follows_input_dispatcher) {
  using bridge::find_focused_window;
  using bridge::find_outside_targets;
  using bridge::find_touched_window;
  const int32_t toast_flags = bridge::kWindowFlagNotTouchable | bridge::kWindowFlagNotFocusable;
  std::vector<bridge::InputWindow> windows = {
      window(3, 2005, toast_flags, {240, 1600, 840, 1720}),  // TYPE_TOAST
      window(1, 1, 0, kScreen),                              // the activity
  };
  CHECK_EQ(find_touched_window(windows, 540, 1650), 1);  // through the toast
  CHECK_EQ(find_focused_window(windows), 1);
  CHECK_EQ(find_touched_window(windows, 1080, 10), -1);  // right edge excluded

  // A touch-modal dialog: the window manager makes its touchable region the
  // display, so a touch outside its frame is still its own.
  bridge::InputWindow dialog = window(2, 2, 0, {140, 760, 940, 1160});
  dialog.touchable = kScreen;
  windows.insert(windows.begin(), dialog);
  CHECK_EQ(find_touched_window(windows, 540, 900), 0);
  CHECK_EQ(find_touched_window(windows, 10, 10), 0);
  CHECK_EQ(find_focused_window(windows), 0);

  // NOT_TOUCH_MODAL, watching outside touches: a touch outside goes below,
  // the popup gets ACTION_OUTSIDE.
  windows[0] = window(2, 1000, bridge::kWindowFlagNotTouchModal | bridge::kWindowFlagWatchOutsideTouch,
                      {140, 760, 940, 1160});
  CHECK_EQ(find_touched_window(windows, 10, 10), 2);
  CHECK(find_outside_targets(windows, 2) == std::vector<size_t>{0});
  CHECK(find_outside_targets(windows, 0).empty());

  // Invisible windows take nothing.
  windows[0].visible = windows[0].focusable = false;
  CHECK_EQ(find_touched_window(windows, 540, 900), 2);
  CHECK(find_outside_targets(windows, 2).empty());
  windows[2].visible = windows[2].focusable = false;
  CHECK_EQ(find_focused_window(windows), -1);
  CHECK_EQ(find_touched_window(windows, 540, 900), -1);
}

// Typed text: the strokes of the key character map (KeyCharacterMap.getEvents),
// characters without a key in one ACTION_MULTIPLE.
TEST(input_text_becomes_key_strokes) {
  std::string error;
  auto map = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  CHECK(map != nullptr);
  auto strokes = bridge::text_to_strokes(map.get(), u"aA€\U0001F600!");
  const int32_t shift = bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn;
  std::vector<bridge::TextStroke> expected;
  auto key = [&](int32_t code, int32_t meta, bool down) {
    bridge::TextStroke s;
    s.key_code = code;
    s.meta_state = meta;
    s.down = down;
    expected.push_back(s);
  };
  key(kA, 0, true);
  key(kA, 0, false);
  key(kShiftLeft, shift, true);
  key(kA, shift, true);
  key(kA, shift, false);
  key(kShiftLeft, 0, false);
  bridge::TextStroke multiple;
  multiple.multiple = true;
  multiple.characters = u"€\U0001F600";
  expected.push_back(multiple);
  key(kShiftLeft, shift, true);
  key(k1, shift, true);
  key(k1, shift, false);
  key(kShiftLeft, 0, false);
  CHECK(strokes == expected);
  // Without a map, everything in one ACTION_MULTIPLE.
  auto plain = bridge::text_to_strokes(nullptr, u"ab");
  CHECK_EQ(plain.size(), size_t(1));
  CHECK(plain[0].multiple && plain[0].characters == u"ab");
}

TEST(input_key_code_labels) {
  CHECK(bridge::key_code_from_label("BACK") == std::optional<int32_t>(4));
  CHECK(bridge::key_code_from_label("keycode_enter") == std::optional<int32_t>(66));
  CHECK(bridge::key_code_from_label("CONTEXTUAL_QUERY") == std::optional<int32_t>(341));  // the last of Android 17
  CHECK(!bridge::key_code_from_label("RETOUR").has_value());
  CHECK(!bridge::key_code_from_label("KEYCODE_").has_value());
  CHECK_EQ(bridge::key_code_label(67), std::string("DEL"));
  CHECK_EQ(bridge::key_code_label(9999), std::string("9999"));
}

TEST(input_scripts_are_parsed_up_front) {
  using Kind = backend::InputStep::Kind;
  std::string error;
  auto steps = backend::parse_input_script(
      "after-frames 1; settle 1500\ntap 540 127.5;key back;text Été ; sleep 20;quit", 1080, 1920, &error);
  CHECK(steps.has_value());
  CHECK_EQ(steps->size(), size_t(7));
  CHECK((*steps)[0].kind == Kind::kAfterFrames && (*steps)[0].count == 1u);
  CHECK((*steps)[1].kind == Kind::kSettle && (*steps)[1].count == 1500u);
  CHECK((*steps)[2].kind == Kind::kTap && (*steps)[2].x == 540.0f && (*steps)[2].y == 127.5f);
  CHECK((*steps)[3].kind == Kind::kKey && (*steps)[3].key_code == 4);
  CHECK((*steps)[4].kind == Kind::kText && (*steps)[4].text == u"Été");
  CHECK((*steps)[5].kind == Kind::kSleep && (*steps)[5].count == 20u);
  CHECK((*steps)[6].kind == Kind::kQuit);
  auto fails = [&](const char* script, const char* why) {
    std::string e;
    bool failed = !backend::parse_input_script(script, 1080, 1920, &e).has_value();
    return failed && e.find(why) != std::string::npos;
  };
  CHECK(fails("tap 1080 10", "hors de l'écran"));
  CHECK(fails("tap 10", "attendu « tap X Y »"));
  CHECK(fails("key RETOUR", "touche inconnue"));
  CHECK(fails("key 0", "touche inconnue"));
  CHECK(fails("swipe 1 2", "étape inconnue"));
  CHECK(fails("after-frames 1; text", "étape 2"));
  CHECK(fails(" ; \n", "vide"));
}

// The display (1080 x 1920) in a square window: bands on the sides, the
// window's pixels mapped back to the display's, presses in the bands ignored.
TEST(input_letterbox_maps_window_pixels_to_the_display) {
  backend::Box image = backend::fit_letterbox({0, 0, 800, 800}, 1080, 1920);
  CHECK(image == (backend::Box{175, 0, 625, 800}));
  auto centre = backend::window_to_image(image, 1080, 1920, 400, 400);
  CHECK(centre.has_value());
  CHECK(centre->x > 539.0f && centre->x < 542.0f && centre->y > 959.0f && centre->y < 962.0f);
  CHECK(!backend::window_to_image(image, 1080, 1920, 100, 400).has_value());  // left band
  auto corner = backend::window_to_image(image, 1080, 1920, 624, 799);
  CHECK(corner.has_value() && corner->x <= 1079.0f && corner->y <= 1919.0f);
  // Dragged out of the image, a finger stays on its edge.
  CHECK(backend::window_to_image_clamped(image, 1080, 1920, 900, -50) == (backend::ImagePoint{1079.0f, 0.0f}));
  // At scale 1, the identity.
  backend::Box same = backend::fit_letterbox({0, 0, 1080, 1920}, 1080, 1920);
  CHECK(backend::window_to_image(same, 1080, 1920, 37, 1200) == std::optional<backend::ImagePoint>({37.0f, 1200.0f}));
}
