// The input dispatcher's decisions that need no Java runtime (input_routing.h)
// and the descriptions of input events (bridge.h).
#include "framework-bridge/input_routing.h"

#include <array>
#include <cstdio>

#include "common/text.h"
#include "framework-bridge/bridge.h"

namespace aa::bridge {

namespace {

// KeyEvent's KEYCODE_ labels by code, from 0 (InputEventLabels' KEYCODES,
// Android 17).
constexpr std::array kKeyCodeLabels = {
    "UNKNOWN", "SOFT_LEFT", "SOFT_RIGHT", "HOME", "BACK", "CALL", "ENDCALL", "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "STAR", "POUND", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "DPAD_CENTER", "VOLUME_UP",
    "VOLUME_DOWN", "POWER", "CAMERA", "CLEAR", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "COMMA", "PERIOD", "ALT_LEFT", "ALT_RIGHT",
    "SHIFT_LEFT", "SHIFT_RIGHT", "TAB", "SPACE", "SYM", "EXPLORER", "ENVELOPE", "ENTER", "DEL", "GRAVE", "MINUS",
    "EQUALS", "LEFT_BRACKET", "RIGHT_BRACKET", "BACKSLASH", "SEMICOLON", "APOSTROPHE", "SLASH", "AT", "NUM",
    "HEADSETHOOK", "FOCUS", "PLUS", "MENU", "NOTIFICATION", "SEARCH", "MEDIA_PLAY_PAUSE", "MEDIA_STOP",
    "MEDIA_NEXT", "MEDIA_PREVIOUS", "MEDIA_REWIND", "MEDIA_FAST_FORWARD", "MUTE", "PAGE_UP", "PAGE_DOWN",
    "PICTSYMBOLS", "SWITCH_CHARSET", "BUTTON_A", "BUTTON_B", "BUTTON_C", "BUTTON_X", "BUTTON_Y", "BUTTON_Z",
    "BUTTON_L1", "BUTTON_R1", "BUTTON_L2", "BUTTON_R2", "BUTTON_THUMBL", "BUTTON_THUMBR", "BUTTON_START",
    "BUTTON_SELECT", "BUTTON_MODE", "ESCAPE", "FORWARD_DEL", "CTRL_LEFT", "CTRL_RIGHT", "CAPS_LOCK", "SCROLL_LOCK",
    "META_LEFT", "META_RIGHT", "FUNCTION", "SYSRQ", "BREAK", "MOVE_HOME", "MOVE_END", "INSERT", "FORWARD",
    "MEDIA_PLAY", "MEDIA_PAUSE", "MEDIA_CLOSE", "MEDIA_EJECT", "MEDIA_RECORD", "F1", "F2", "F3", "F4", "F5", "F6",
    "F7", "F8", "F9", "F10", "F11", "F12", "NUM_LOCK", "NUMPAD_0", "NUMPAD_1", "NUMPAD_2", "NUMPAD_3", "NUMPAD_4",
    "NUMPAD_5", "NUMPAD_6", "NUMPAD_7", "NUMPAD_8", "NUMPAD_9", "NUMPAD_DIVIDE", "NUMPAD_MULTIPLY",
    "NUMPAD_SUBTRACT", "NUMPAD_ADD", "NUMPAD_DOT", "NUMPAD_COMMA", "NUMPAD_ENTER", "NUMPAD_EQUALS",
    "NUMPAD_LEFT_PAREN", "NUMPAD_RIGHT_PAREN", "VOLUME_MUTE", "INFO", "CHANNEL_UP", "CHANNEL_DOWN", "ZOOM_IN",
    "ZOOM_OUT", "TV", "WINDOW", "GUIDE", "DVR", "BOOKMARK", "CAPTIONS", "SETTINGS", "TV_POWER", "TV_INPUT",
    "STB_POWER", "STB_INPUT", "AVR_POWER", "AVR_INPUT", "PROG_RED", "PROG_GREEN", "PROG_YELLOW", "PROG_BLUE",
    "APP_SWITCH", "BUTTON_1", "BUTTON_2", "BUTTON_3", "BUTTON_4", "BUTTON_5", "BUTTON_6", "BUTTON_7", "BUTTON_8",
    "BUTTON_9", "BUTTON_10", "BUTTON_11", "BUTTON_12", "BUTTON_13", "BUTTON_14", "BUTTON_15", "BUTTON_16",
    "LANGUAGE_SWITCH", "MANNER_MODE", "3D_MODE", "CONTACTS", "CALENDAR", "MUSIC", "CALCULATOR", "ZENKAKU_HANKAKU",
    "EISU", "MUHENKAN", "HENKAN", "KATAKANA_HIRAGANA", "YEN", "RO", "KANA", "ASSIST", "BRIGHTNESS_DOWN",
    "BRIGHTNESS_UP", "MEDIA_AUDIO_TRACK", "SLEEP", "WAKEUP", "PAIRING", "MEDIA_TOP_MENU", "11", "12",
    "LAST_CHANNEL", "TV_DATA_SERVICE", "VOICE_ASSIST", "TV_RADIO_SERVICE", "TV_TELETEXT", "TV_NUMBER_ENTRY",
    "TV_TERRESTRIAL_ANALOG", "TV_TERRESTRIAL_DIGITAL", "TV_SATELLITE", "TV_SATELLITE_BS", "TV_SATELLITE_CS",
    "TV_SATELLITE_SERVICE", "TV_NETWORK", "TV_ANTENNA_CABLE", "TV_INPUT_HDMI_1", "TV_INPUT_HDMI_2",
    "TV_INPUT_HDMI_3", "TV_INPUT_HDMI_4", "TV_INPUT_COMPOSITE_1", "TV_INPUT_COMPOSITE_2", "TV_INPUT_COMPONENT_1",
    "TV_INPUT_COMPONENT_2", "TV_INPUT_VGA_1", "TV_AUDIO_DESCRIPTION", "TV_AUDIO_DESCRIPTION_MIX_UP",
    "TV_AUDIO_DESCRIPTION_MIX_DOWN", "TV_ZOOM_MODE", "TV_CONTENTS_MENU", "TV_MEDIA_CONTEXT_MENU",
    "TV_TIMER_PROGRAMMING", "HELP", "NAVIGATE_PREVIOUS", "NAVIGATE_NEXT", "NAVIGATE_IN", "NAVIGATE_OUT",
    "STEM_PRIMARY", "STEM_1", "STEM_2", "STEM_3", "DPAD_UP_LEFT", "DPAD_DOWN_LEFT", "DPAD_UP_RIGHT",
    "DPAD_DOWN_RIGHT", "MEDIA_SKIP_FORWARD", "MEDIA_SKIP_BACKWARD", "MEDIA_STEP_FORWARD", "MEDIA_STEP_BACKWARD",
    "SOFT_SLEEP", "CUT", "COPY", "PASTE", "SYSTEM_NAVIGATION_UP", "SYSTEM_NAVIGATION_DOWN",
    "SYSTEM_NAVIGATION_LEFT", "SYSTEM_NAVIGATION_RIGHT", "ALL_APPS", "REFRESH", "THUMBS_UP", "THUMBS_DOWN",
    "PROFILE_SWITCH", "VIDEO_APP_1", "VIDEO_APP_2", "VIDEO_APP_3", "VIDEO_APP_4", "VIDEO_APP_5", "VIDEO_APP_6",
    "VIDEO_APP_7", "VIDEO_APP_8", "FEATURED_APP_1", "FEATURED_APP_2", "FEATURED_APP_3", "FEATURED_APP_4",
    "DEMO_APP_1", "DEMO_APP_2", "DEMO_APP_3", "DEMO_APP_4", "KEYBOARD_BACKLIGHT_DOWN", "KEYBOARD_BACKLIGHT_UP",
    "KEYBOARD_BACKLIGHT_TOGGLE", "STYLUS_BUTTON_PRIMARY", "STYLUS_BUTTON_SECONDARY", "STYLUS_BUTTON_TERTIARY",
    "STYLUS_BUTTON_TAIL", "RECENT_APPS", "MACRO_1", "MACRO_2", "MACRO_3", "MACRO_4", "EMOJI_PICKER", "SCREENSHOT",
    "DICTATE", "NEW", "CLOSE", "DO_NOT_DISTURB", "PRINT", "LOCK", "FULLSCREEN", "F13", "F14", "F15", "F16", "F17",
    "F18", "F19", "F20", "F21", "F22", "F23", "F24", "ACCESSIBILITY", "CONTEXTUAL_SEARCH", "CONTEXTUAL_INSERT",
    "CONTEXTUAL_QUERY",
};

bool is_high_surrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool is_low_surrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// A coordinate as the traces write it: an integer when it is one, else with
// at most two decimals.
std::string coordinate(float v) {
  char buf[32];
  if (v == float(int64_t(v))) {
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    return buf;
  }
  std::snprintf(buf, sizeof buf, "%.2f", double(v));
  std::string s = buf;
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  return s;
}

std::string point(float x, float y) { return coordinate(x) + "," + coordinate(y); }

}  // namespace

int find_touched_window(const std::vector<InputWindow>& windows, float x, float y) {
  for (size_t i = 0; i < windows.size(); i++) {
    const InputWindow& w = windows[i];
    if (!w.visible || !w.touchable_flag()) continue;
    if (w.touchable.contains(x, y)) return int(i);
  }
  return -1;
}

std::vector<size_t> find_outside_targets(const std::vector<InputWindow>& windows, int touched) {
  std::vector<size_t> out;
  size_t end = touched < 0 ? windows.size() : size_t(touched);
  for (size_t i = 0; i < end && i < windows.size(); i++) {
    if (windows[i].visible && windows[i].watches_outside()) out.push_back(i);
  }
  return out;
}

int find_focused_window(const std::vector<InputWindow>& windows) {
  for (size_t i = 0; i < windows.size(); i++) {
    if (windows[i].visible && windows[i].focusable) return int(i);
  }
  return -1;
}

std::vector<TextStroke> text_to_strokes(const KeyCharacterMap* map, std::u16string_view text) {
  std::vector<TextStroke> out;
  std::u16string pending;  // characters without a key
  auto flush = [&] {
    if (pending.empty()) return;
    TextStroke s;
    s.multiple = true;
    s.characters = std::move(pending);
    out.push_back(std::move(s));
    pending.clear();
  };
  std::vector<KeyCharacterMap::KeyStroke> strokes;
  for (size_t i = 0; i < text.size(); i++) {
    char16_t c = text[i];
    bool pair = is_high_surrogate(c) && i + 1 < text.size() && is_low_surrogate(text[i + 1]);
    strokes.clear();
    if (pair || !map || !map->events(&c, 1, strokes)) {
      pending.push_back(c);
      if (pair) pending.push_back(text[++i]);
      continue;
    }
    flush();
    for (const auto& k : strokes) {
      TextStroke s;
      s.key_code = k.key_code;
      s.meta_state = k.meta_state;
      s.down = k.down;
      out.push_back(std::move(s));
    }
  }
  flush();
  return out;
}

std::optional<int32_t> key_code_from_label(std::string_view label) {
  std::string upper;
  for (char c : label) upper += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
  std::string_view name = upper;
  if (name.substr(0, 8) == "KEYCODE_") name.remove_prefix(8);
  if (name.empty()) return std::nullopt;
  for (size_t code = 0; code < kKeyCodeLabels.size(); code++) {
    if (name == kKeyCodeLabels[code]) return int32_t(code);
  }
  return std::nullopt;
}

std::string key_code_label(int32_t key_code) {
  if (key_code >= 0 && size_t(key_code) < kKeyCodeLabels.size()) return kKeyCodeLabels[size_t(key_code)];
  return std::to_string(key_code);
}

std::string describe_input_event(const InputEventInfo& e) {
  using Type = InputEventInfo::Type;
  std::string s;
  switch (e.type) {
    case Type::kTouch:
      switch (e.action) {
        case 0: s = "appui "; break;
        case 1: s = "relâchement "; break;
        case 2: s = "glissement "; break;
        case 3: s = "geste annulé en "; break;
        case 4: s = "toucher extérieur en "; break;
        default: s = "geste (action " + std::to_string(e.action) + ") en "; break;
      }
      return s + point(e.x, e.y);
    case Type::kScroll:
      return "défilement h=" + coordinate(e.horizontal) + " v=" + coordinate(e.vertical) + " en " + point(e.x, e.y);
    case Type::kCharacters:
      return "texte « " + text::utf16_to_utf8(e.characters) + " »";
    case Type::kKey:
      s = e.action == 0 ? "appui sur la touche " : e.action == 1 ? "relâchement de la touche " : "touche ";
      s += key_code_label(e.key_code);
      if (e.repeat > 0) s += " (répétition " + std::to_string(e.repeat) + ")";
      if (e.meta_state != 0) {
        char buf[24];
        std::snprintf(buf, sizeof buf, " (méta 0x%x)", unsigned(e.meta_state));
        s += buf;
      }
      return s;
  }
  return s;
}

}  // namespace aa::bridge
