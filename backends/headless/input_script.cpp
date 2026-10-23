#include "headless/input_script.h"

#include <charconv>
#include <chrono>
#include <thread>

#include "common/text.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/input_routing.h"

namespace aa::backend {

namespace {

constexpr uint64_t kMaxFrames = 1'000'000;
constexpr uint64_t kMaxSleepMs = 3'600'000;
// What `quit` waits, at most, for the events sent to be finished.
constexpr int64_t kQuitIdleTimeoutMs = 10'000;

std::vector<std::string_view> words(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') i++;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

std::optional<uint64_t> parse_count(std::string_view s, uint64_t max) {
  uint64_t v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || v > max) return std::nullopt;
  return v;
}

std::optional<float> parse_coordinate(std::string_view s) {
  float v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
  return v;
}

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// One step, without its surrounding spaces; "" when valid.
std::string parse_step(std::string_view step, int32_t width, int32_t height, InputStep& out) {
  std::vector<std::string_view> w = words(step);
  std::string_view command = w[0];
  auto expect_args = [&](size_t n) { return w.size() == n + 1; };
  if (command == "after-frames") {
    if (!expect_args(1)) return "attendu « after-frames N »";
    auto n = parse_count(w[1], kMaxFrames);
    if (!n) return "nombre d'images invalide : " + std::string(w[1]);
    out.kind = InputStep::Kind::kAfterFrames;
    out.count = *n;
  } else if (command == "tap") {
    if (!expect_args(2)) return "attendu « tap X Y »";
    auto x = parse_coordinate(w[1]), y = parse_coordinate(w[2]);
    if (!x || !y) return "coordonnées invalides";
    if (*x < 0 || *y < 0 || *x >= float(width) || *y >= float(height)) {
      return "point hors de l'écran (" + std::to_string(width) + "x" + std::to_string(height) + ")";
    }
    out.kind = InputStep::Kind::kTap;
    out.x = *x;
    out.y = *y;
  } else if (command == "key") {
    if (!expect_args(1)) return "attendu « key NOM » ou « key CODE »";
    std::optional<int32_t> code;
    if (all_digits(w[1])) {
      auto n = parse_count(w[1], 100000);
      if (n && *n > 0 && bridge::key_code_label(int32_t(*n)) != std::to_string(*n)) code = int32_t(*n);
    } else {
      code = bridge::key_code_from_label(w[1]);
      if (code && *code == 0) code.reset();  // KEYCODE_UNKNOWN
    }
    if (!code) return "touche inconnue : " + std::string(w[1]);
    out.kind = InputStep::Kind::kKey;
    out.key_code = *code;
  } else if (command == "text") {
    std::string_view rest = step.substr(command.size());
    if (rest.empty() || (rest[0] != ' ' && rest[0] != '\t') || rest.find_first_not_of(" \t") == std::string_view::npos) {
      return "attendu « text <caractères> »";
    }
    out.kind = InputStep::Kind::kText;
    out.text = text::utf8_to_utf16(rest.substr(1));
  } else if (command == "sleep") {
    if (!expect_args(1)) return "attendu « sleep MS »";
    auto n = parse_count(w[1], kMaxSleepMs);
    if (!n) return "durée invalide : " + std::string(w[1]);
    out.kind = InputStep::Kind::kSleep;
    out.count = *n;
  } else if (command == "settle") {
    if (!expect_args(1)) return "attendu « settle MS »";
    auto n = parse_count(w[1], kMaxSleepMs);
    if (!n) return "durée invalide : " + std::string(w[1]);
    out.kind = InputStep::Kind::kSettle;
    out.count = *n;
  } else if (command == "quit") {
    if (!expect_args(0)) return "« quit » ne prend pas d'argument";
    out.kind = InputStep::Kind::kQuit;
  } else {
    return "étape inconnue « " + std::string(command) +
           " » (attendu after-frames, tap, key, text, sleep, settle ou quit)";
  }
  return {};
}

}  // namespace

std::optional<std::vector<InputStep>> parse_input_script(std::string_view script, int32_t width, int32_t height,
                                                         std::string* error) {
  std::vector<InputStep> steps;
  size_t index = 0;
  size_t start = 0;
  while (start <= script.size()) {
    size_t end = script.find_first_of(";\n", start);
    if (end == std::string_view::npos) end = script.size();
    std::string_view raw = script.substr(start, end - start);
    size_t first = raw.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos) {
      size_t last = raw.find_last_not_of(" \t\r\n");
      std::string_view step = raw.substr(first, last - first + 1);
      index++;
      InputStep s;
      std::string why = parse_step(step, width, height, s);
      if (!why.empty()) {
        *error = "script d'entrée, étape " + std::to_string(index) + " « " + std::string(step) + " » : " + why;
        return std::nullopt;
      }
      steps.push_back(std::move(s));
    }
    start = end + 1;
  }
  if (steps.empty()) {
    *error = "script d'entrée vide";
    return std::nullopt;
  }
  return steps;
}

void run_input_script(const std::vector<InputStep>& steps, const InputScriptHooks& hooks) {
  for (const InputStep& s : steps) {
    switch (s.kind) {
      case InputStep::Kind::kAfterFrames:
        if (!hooks.wait_for_frames(s.count)) {
          hooks.error("script d'entrée : délai dépassé en attendant " + std::to_string(s.count) + " images");
          hooks.quit(1);
          return;
        }
        break;
      case InputStep::Kind::kTap:
        // As the shell's `input tap`: the up right after the down, same point.
        bridge::inject_touch(bridge::TouchAction::kDown, s.x, s.y);
        bridge::inject_touch(bridge::TouchAction::kUp, s.x, s.y);
        break;
      case InputStep::Kind::kKey:
        bridge::inject_key(s.key_code, true);
        bridge::inject_key(s.key_code, false);
        break;
      case InputStep::Kind::kText:
        bridge::inject_text(s.text);
        break;
      case InputStep::Kind::kSleep:
        std::this_thread::sleep_for(std::chrono::milliseconds(s.count));
        break;
      case InputStep::Kind::kSettle:
        if (!bridge::wait_for_input_idle(kQuitIdleTimeoutMs)) {
          hooks.error("script d'entrée : des événements envoyés ne sont pas terminés");
        }
        if (!hooks.wait_for_quiet_frames(s.count)) {
          hooks.error("script d'entrée : l'écran ne se stabilise pas");
          hooks.quit(1);
          return;
        }
        break;
      case InputStep::Kind::kQuit:
        if (!bridge::wait_for_input_idle(kQuitIdleTimeoutMs)) {
          hooks.error("script d'entrée : des événements envoyés ne sont pas terminés");
        }
        hooks.quit(0);
        return;
    }
  }
}

}  // namespace aa::backend
