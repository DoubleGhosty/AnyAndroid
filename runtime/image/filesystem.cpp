#include "image/filesystem.h"

#include "common/bytes.h"
#include "common/text.h"

namespace aa::image {

std::unique_ptr<FileSystem> open_ext4(std::shared_ptr<const Source> src);
std::unique_ptr<FileSystem> open_erofs(std::shared_ptr<const Source> src);

namespace {
constexpr uint64_t kSuperOffset = 1024;
constexpr int kMaxSymlinkDepth = 40;
}  // namespace

std::string detect_filesystem(const Source& src) {
  if (src.size() < kSuperOffset + 128) return "";
  if (src.u32_at(kSuperOffset) == 0xE0F5E1E2) return "erofs";
  auto magic = src.read(kSuperOffset + 0x38, 2);
  if (load_u16(magic.data()) == 0xEF53) return "ext4";
  return "";
}

std::unique_ptr<FileSystem> open_filesystem(std::shared_ptr<const Source> src) {
  std::string kind = detect_filesystem(*src);
  if (kind == "ext4") return open_ext4(std::move(src));
  if (kind == "erofs") return open_erofs(std::move(src));
  throw_format("système de fichiers non reconnu (ni ext4 ni EROFS)");
}

std::optional<uint64_t> FileSystem::resolve(std::string_view path, bool follow_last, int depth) const {
  if (depth > kMaxSymlinkDepth) throw_format("trop de liens symboliques imbriqués : " + text::printable(path));
  std::vector<uint64_t> stack{root()};
  auto parts = text::split(path, '/');
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (stack.size() > 1) stack.pop_back();
      continue;
    }
    Stat dir = stat_inode(stack.back());
    if (dir.type != FileType::kDirectory) return std::nullopt;
    std::optional<DirEntry> found;
    for (auto& e : read_dir(stack.back())) {
      if (e.name == part) {
        found = std::move(e);
        break;
      }
    }
    if (!found) return std::nullopt;
    bool last = i + 1 == parts.size();
    Stat st = stat_inode(found->inode);
    if (st.type == FileType::kSymlink && (!last || follow_last)) {
      std::string target = read_link(found->inode);
      if (target.empty() || target[0] != '/') {
        // Relative target: resolve it from the directory holding the link.
        std::string walked;
        for (size_t k = 0; k < i; ++k) {
          if (!parts[k].empty() && parts[k] != ".") walked += "/" + parts[k];
        }
        target = walked + "/" + target;
      }
      std::string rest;
      for (size_t k = i + 1; k < parts.size(); ++k) rest += "/" + parts[k];
      return resolve(target + rest, follow_last, depth + 1);
    }
    stack.push_back(found->inode);
  }
  return stack.back();
}

std::optional<Stat> FileSystem::stat(std::string_view path, bool follow_last) const {
  auto ino = resolve(path, follow_last, 0);
  if (!ino) return std::nullopt;
  return stat_inode(*ino);
}

std::vector<DirEntry> FileSystem::list(std::string_view path) const {
  auto st = stat(path);
  if (!st) throw_format("chemin introuvable dans l'image : " + text::printable(path));
  if (st->type != FileType::kDirectory) throw_format("ce n'est pas un répertoire : " + text::printable(path));
  return read_dir(st->inode);
}

std::vector<uint8_t> FileSystem::read_file(std::string_view path, uint64_t max_size) const {
  auto st = stat(path);
  if (!st) throw_format("fichier introuvable dans l'image : " + text::printable(path));
  if (st->type != FileType::kRegular) throw_format("ce n'est pas un fichier ordinaire : " + text::printable(path));
  return read_inode(st->inode, max_size);
}

std::string FileSystem::read_link(uint64_t inode) const {
  auto data = read_inode(inode, 4096);
  return std::string(data.begin(), data.end());
}

}  // namespace aa::image
