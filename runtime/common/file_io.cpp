#include "common/file_io.h"

#include <fstream>
#include <stdexcept>

#include "common/text.h"

namespace aa {

std::string path_utf8(const std::filesystem::path& path) {
  auto s = path.u8string();
  return std::string(s.begin(), s.end());
}

std::filesystem::path path_from_utf8(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::vector<std::string> utf8_args(int argc, wchar_t** argv) {
  std::vector<std::string> out;
  for (int i = 0; i < argc; ++i) {
    std::wstring w(argv[i]);
    out.push_back(text::utf16_to_utf8(std::u16string(w.begin(), w.end())));
  }
  return out;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path, uint64_t max_size) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec) throw std::runtime_error("fichier illisible : " + path_utf8(path) + " (" + ec.message() + ")");
  if (size > max_size) {
    throw std::runtime_error("fichier trop volumineux : " + path_utf8(path) + " (" + std::to_string(size) +
                             " octets, limite " + std::to_string(max_size) + ")");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("ouverture impossible : " + path_utf8(path));
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
    throw std::runtime_error("lecture incomplète : " + path_utf8(path));
  }
  return data;
}

std::string read_text_file(const std::filesystem::path& path, uint64_t max_size) {
  auto bytes = read_file(path, max_size);
  std::string s(bytes.begin(), bytes.end());
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    s.erase(0, 3);  // UTF-8 BOM
  }
  return s;
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> data) {
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("écriture impossible : " + path_utf8(path));
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("écriture incomplète : " + path_utf8(path));
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
  write_file(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

}  // namespace aa
