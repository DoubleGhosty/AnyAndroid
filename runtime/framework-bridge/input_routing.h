#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "framework-bridge/key_character_map.h"

// The decisions of the input dispatcher (input_dispatcher.cpp) that do not
// need the Java runtime: the window list as InputDispatcher receives it from
// the window manager (gui::WindowInfo), the window a touch or a key goes to,
// typed text as the key strokes of a key character map, and the KEYCODE_
// labels of the key codes.
namespace aa::bridge {

// WindowManager.LayoutParams flags the window manager turns into input
// configuration.
enum : int32_t {
  kWindowFlagNotFocusable = 0x00000008,
  kWindowFlagNotTouchable = 0x00000010,
  kWindowFlagNotTouchModal = 0x00000020,
  kWindowFlagWatchOutsideTouch = 0x00040000,
};

// A rectangle of display pixels: x in [left, right), y in [top, bottom), as
// the window manager's bounds are read by InputDispatcher.
struct InputRect {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
  bool operator==(const InputRect&) const = default;
};

// gui::WindowInfo of a window with an input channel, as the window manager
// fills it (InputMonitor.populateInputWindowHandle): the list is ordered from
// the topmost window down. The window's transform is the offset of its frame
// (no scaling or rotation).
struct InputWindow {
  uint64_t connection = 0;     // its input channel (both ends share it)
  std::string name;            // LayoutParams title
  int32_t type = 0;            // LayoutParams.type
  int32_t layout_flags = 0;    // LayoutParams.flags
  InputRect frame;             // display pixels
  InputRect touchable;         // bounds of the touchable region
  bool visible = false;        // not InputConfig::NOT_VISIBLE
  bool focusable = false;      // WindowState.canReceiveKeys
  bool touchable_flag() const { return (layout_flags & kWindowFlagNotTouchable) == 0; }
  bool watches_outside() const { return (layout_flags & kWindowFlagWatchOutsideTouch) != 0; }
};

// InputDispatcher::findTouchedWindowAtLocked: the topmost visible window
// that accepts a touch at (x, y) (not NOT_TOUCHABLE, its touchable region
// containing the point); -1 for none.
int find_touched_window(const std::vector<InputWindow>& windows, float x, float y);
// findOutsideTargetsLocked: the visible windows above `touched` (above all
// of them when touched is -1) that watch outside touches, from the top.
std::vector<size_t> find_outside_targets(const std::vector<InputWindow>& windows, int touched);
// The window that gets key events: the topmost visible focusable window
// (the window manager's DisplayContent.findFocusedWindow, without focused
// application nor window tokens); -1 for none.
int find_focused_window(const std::vector<InputWindow>& windows);

// One key event of typed text: a key stroke of the key character map, or
// characters no key types (KeyEvent ACTION_MULTIPLE with KEYCODE_UNKNOWN).
struct TextStroke {
  bool multiple = false;
  std::u16string characters;  // multiple
  int32_t key_code = 0, meta_state = 0;
  bool down = false;
  bool operator==(const TextStroke&) const = default;
};
// The key events that type `text` with `map` (null: every character in one
// ACTION_MULTIPLE): each character as the map's strokes
// (KeyCharacterMap.getEvents); consecutive characters that no key types, and
// supplementary characters, grouped in one ACTION_MULTIPLE.
std::vector<TextStroke> text_to_strokes(const KeyCharacterMap* map, std::u16string_view text);

// KEYCODE_ labels of android.view.KeyEvent (InputEventLabels): "BACK" for 4.
// key_code_from_label takes a label with or without the KEYCODE_ prefix, in
// any case; nullopt when unknown. key_code_label gives the number for codes
// without a label.
std::optional<int32_t> key_code_from_label(std::string_view label);
std::string key_code_label(int32_t key_code);

}  // namespace aa::bridge
