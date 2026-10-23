#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aa::backend {

// A deterministic input script (J8 of ADR-006, `exec --input`): what a user
// does on the device's screen, sent through the bridge's input dispatcher.
// Steps, separated by ';' or line breaks:
//   after-frames N   wait until N frames were posted in total
//   tap X Y          a touch at (X, Y) display pixels: ACTION_DOWN then ACTION_UP
//   key NAME|CODE    a key pressed then released (KEYCODE_ label, with or
//                    without the prefix, or code)
//   text <chars>     typed text (UTF-8, up to the next ';' or line break)
//   sleep MS         wait MS milliseconds
//   settle MS        wait for the events sent to be finished, then for MS
//                    milliseconds without a new frame (the screen has settled)
//   quit             wait for the events sent to be finished, flush the
//                    standard streams and end the process (code 0)
struct InputStep {
  enum class Kind { kAfterFrames, kTap, kKey, kText, kSleep, kSettle, kQuit };
  Kind kind = Kind::kQuit;
  uint64_t count = 0;    // after-frames: frames; sleep, settle: milliseconds
  float x = 0, y = 0;    // tap
  int32_t key_code = 0;  // key
  std::u16string text;   // text
  bool operator==(const InputStep&) const = default;
};

// The steps of a script for a display of width x height pixels; nullopt with
// the reason (French) in *error when a step is malformed.
std::optional<std::vector<InputStep>> parse_input_script(std::string_view script, int32_t width, int32_t height,
                                                         std::string* error);

struct InputScriptHooks {
  // Waits until `frames` frames were posted in total; false on timeout.
  std::function<bool(uint64_t frames)> wait_for_frames;
  // Waits until no frame was posted for `quiet_ms` milliseconds; false on timeout.
  std::function<bool(uint64_t quiet_ms)> wait_for_quiet_frames;
  // Ends the process with this code (the message, if any, already reported).
  std::function<void(int code)> quit;
  // Reports an error of the script's run (French).
  std::function<void(const std::string& message)> error;
};

// Runs the steps on the calling thread; taps, keys and text go to the
// bridge's inject functions.
void run_input_script(const std::vector<InputStep>& steps, const InputScriptHooks& hooks);

}  // namespace aa::backend
