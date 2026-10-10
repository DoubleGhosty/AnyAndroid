#include "common/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

#include "common/json.h"

namespace aa::log {

namespace {

struct State {
  std::mutex mu;
  Level level = Level::Info;
  std::ofstream jsonl;
  std::string package;
};

State& state() {
  static State s;
  return s;
}

const char* level_name(Level l) {
  switch (l) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
  }
  return "?";
}

std::string timestamp(bool iso) {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  if (iso) gmtime_s(&tm, &t); else localtime_s(&tm, &t);
#else
  if (iso) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
#endif
  char buf[64];
  if (iso) {
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
  } else {
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
  }
  return buf;
}

}  // namespace

void set_level(Level level) { state().level = level; }
Level level() { return state().level; }
bool enabled(Level l) { return l != Level::Off && l >= state().level; }

bool parse_level(std::string_view name, Level& out) {
  for (Level l : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off}) {
    if (name == level_name(l)) {
      out = l;
      return true;
    }
  }
  return false;
}

bool open_jsonl(const std::filesystem::path& path) {
  std::lock_guard lock(state().mu);
  state().jsonl.open(path, std::ios::app | std::ios::binary);
  return state().jsonl.is_open();
}

void close_jsonl() {
  std::lock_guard lock(state().mu);
  state().jsonl.close();
}

void set_package(std::string package) {
  std::lock_guard lock(state().mu);
  state().package = std::move(package);
}

void write(Level l, std::string_view component, std::string_view message, std::string_view api) {
  if (!enabled(l)) return;
  State& s = state();
  std::lock_guard lock(s.mu);
  static const char* tags[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR"};
  std::string line = timestamp(false) + " " + tags[int(l)] + " [" + std::string(component) + "] ";
  line += message;
  if (!api.empty()) line += " {" + std::string(api) + "}";
  line.push_back('\n');
  std::fwrite(line.data(), 1, line.size(), stderr);

  if (s.jsonl.is_open()) {
    JsonWriter w(false);
    w.begin_object();
    w.field("ts", timestamp(true));
    w.field("level", level_name(l));
    w.field("component", component);
    if (!s.package.empty()) w.field("package", s.package);
    w.field("thread", uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffff));
    if (!api.empty()) w.field("api", api);
    w.field("message", message);
    w.end_object();
    s.jsonl << w.str() << '\n';
    s.jsonl.flush();
  }
}

}  // namespace aa::log
