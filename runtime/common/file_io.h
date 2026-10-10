#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace aa {

// Reads a whole file, refusing files larger than max_size. Throws
// std::runtime_error with a readable message on failure.
std::vector<uint8_t> read_file(const std::filesystem::path& path, uint64_t max_size);
std::string read_text_file(const std::filesystem::path& path, uint64_t max_size = 16u << 20);
void write_file(const std::filesystem::path& path, std::span<const uint8_t> data);
void write_text_file(const std::filesystem::path& path, const std::string& text);

// Path rendered as UTF-8 for messages, and the reverse.
std::string path_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(const std::string& utf8);

// Process arguments as UTF-8 (wmain on Windows keeps non-ASCII paths intact).
std::vector<std::string> utf8_args(int argc, wchar_t** argv);

}  // namespace aa
