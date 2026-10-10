#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Structured logging: human-readable lines on stderr and, optionally, one
// JSON object per line (JSONL) in a file. Logs never leave the machine.
namespace aa::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

void set_level(Level level);
Level level();
bool enabled(Level level);
bool parse_level(std::string_view name, Level& out);

// Adds a JSONL sink. Returns false if the file cannot be opened.
bool open_jsonl(const std::filesystem::path& path);
void close_jsonl();

// Context attached to every subsequent record (e.g. the running package).
void set_package(std::string package);

void write(Level level, std::string_view component, std::string_view message, std::string_view api = {});

inline void trace(std::string_view c, std::string_view m) { if (enabled(Level::Trace)) write(Level::Trace, c, m); }
inline void debug(std::string_view c, std::string_view m) { if (enabled(Level::Debug)) write(Level::Debug, c, m); }
inline void info(std::string_view c, std::string_view m) { write(Level::Info, c, m); }
inline void warn(std::string_view c, std::string_view m, std::string_view api = {}) { write(Level::Warn, c, m, api); }
inline void error(std::string_view c, std::string_view m, std::string_view api = {}) { write(Level::Error, c, m, api); }

}  // namespace aa::log
