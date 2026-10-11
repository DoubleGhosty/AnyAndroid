#define _CRT_RAND_S  // rand_s(): the system's cryptographic generator
#include "vfs/vfs.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include "common/bytes.h"
#include "common/file_io.h"
#include "common/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace aa::vfs {

namespace {

constexpr int kMaxSymlinks = 40;
constexpr size_t kMaxPath = 4096;
constexpr size_t kMaxName = 255;
constexpr size_t kMaxOpenFiles = 1024;
// Timestamp of the files of an Android system image (2009-01-01, as built).
constexpr int64_t kImageTime = 1230768000;

constexpr uint32_t kIfReg = 0100000, kIfDir = 040000, kIfLnk = 0120000, kIfChr = 020000;

std::vector<std::string> components(std::string_view path) {
  std::vector<std::string> out;
  for (auto& p : text::split(path, '/')) {
    if (!p.empty()) out.push_back(std::move(p));
  }
  return out;
}

// --- Image mount ---------------------------------------------------------------

class ImageFile final : public OpenFile {
 public:
  ImageFile(std::shared_ptr<const std::vector<uint8_t>> data, VStat st) : data_(std::move(data)), st_(st) {}
  VStat stat() override { return st_; }
  size_t read(uint64_t offset, std::span<uint8_t> out) override {
    if (st_.type == NodeType::kDirectory) throw VfsError{kEISDIR, "read"};
    if (offset >= data_->size()) return 0;
    size_t n = std::min<size_t>(out.size(), data_->size() - size_t(offset));
    std::memcpy(out.data(), data_->data() + offset, n);
    return n;
  }
  size_t write(uint64_t, std::span<const uint8_t>) override { throw VfsError{kEBADF, "write"}; }
  void truncate(uint64_t) override { throw VfsError{kEROFS, "truncate"}; }

 private:
  std::shared_ptr<const std::vector<uint8_t>> data_;
  VStat st_;
};

VStat to_vstat(const image::Stat& s) {
  VStat v;
  v.ino = s.inode;
  v.size = s.size;
  v.mtime = kImageTime;
  v.mode = s.mode;
  switch (s.type) {
    case image::FileType::kDirectory: v.type = NodeType::kDirectory; v.nlink = 2; break;
    case image::FileType::kSymlink: v.type = NodeType::kSymlink; break;
    case image::FileType::kRegular: v.type = NodeType::kFile; break;
    default: v.type = NodeType::kCharDevice; break;
  }
  if ((v.mode & 0170000) == 0) {
    v.mode |= v.type == NodeType::kDirectory ? kIfDir : v.type == NodeType::kSymlink ? kIfLnk : kIfReg;
  }
  return v;
}

class ImageMount final : public Mount {
 public:
  ImageMount(std::shared_ptr<image::FileSystem> fs, std::string root) : fs_(std::move(fs)), root_(std::move(root)) {}

  std::optional<VStat> lstat(const std::string& rel) override {
    try {
      auto st = fs_->stat(path(rel), false);
      if (!st) return std::nullopt;
      return to_vstat(*st);
    } catch (const FormatError& e) {
      throw VfsError{kEIO, e.what()};
    }
  }
  std::string readlink(const std::string& rel) override {
    auto st = lstat(rel);
    if (!st) throw VfsError{kENOENT, rel};
    if (st->type != NodeType::kSymlink) throw VfsError{kEINVAL, rel};
    try {
      return fs_->read_link(st->ino);
    } catch (const FormatError& e) {
      throw VfsError{kEIO, e.what()};
    }
  }
  std::vector<std::string> list(const std::string& rel) override {
    auto st = lstat(rel);
    if (!st) throw VfsError{kENOENT, rel};
    if (st->type != NodeType::kDirectory) throw VfsError{kENOTDIR, rel};
    std::vector<std::string> out;
    try {
      for (const auto& e : fs_->read_dir(st->ino)) out.push_back(e.name);
    } catch (const FormatError& e) {
      throw VfsError{kEIO, e.what()};
    }
    return out;
  }
  std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t) override {
    if ((flags & kO_ACCMODE) != kO_RDONLY || (flags & (kO_TRUNC | kO_APPEND))) throw VfsError{kEROFS, rel};
    auto st = lstat(rel);
    if (!st) {
      if (flags & kO_CREAT) throw VfsError{kEROFS, rel};
      throw VfsError{kENOENT, rel};
    }
    if ((flags & kO_CREAT) && (flags & kO_EXCL)) throw VfsError{kEEXIST, rel};
    if (st->type == NodeType::kDirectory) {
      return std::make_unique<ImageFile>(std::make_shared<const std::vector<uint8_t>>(), *st);
    }
    if ((flags & kO_DIRECTORY)) throw VfsError{kENOTDIR, rel};
    return std::make_unique<ImageFile>(content(*st), *st);
  }

 private:
  std::string path(const std::string& rel) const {
    if (root_ == "/") return rel;
    return rel == "/" ? root_ : root_ + rel;
  }
  // File contents are cached per inode (images are immutable).
  std::shared_ptr<const std::vector<uint8_t>> content(const VStat& st) {
    auto it = cache_.find(st.ino);
    if (it != cache_.end()) return it->second;
    std::shared_ptr<const std::vector<uint8_t>> data;
    try {
      data = std::make_shared<const std::vector<uint8_t>>(fs_->read_inode(st.ino, 1ull << 30));
    } catch (const FormatError& e) {
      throw VfsError{kEIO, e.what()};
    }
    cached_bytes_ += data->size();
    if (cached_bytes_ > (512ull << 20)) {  // keep the cache bounded
      cache_.clear();
      cached_bytes_ = data->size();
    }
    cache_[st.ino] = data;
    return data;
  }

  std::shared_ptr<image::FileSystem> fs_;
  std::string root_;
  std::map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> cache_;
  uint64_t cached_bytes_ = 0;
};

// --- Alias mount -------------------------------------------------------------------------

// A flat, read-only directory of image files exposed under other names.
class AliasMount final : public Mount {
 public:
  AliasMount(std::shared_ptr<image::FileSystem> fs, std::map<std::string, std::string> files)
      : image_(std::move(fs), "/"), files_(std::move(files)) {}

  std::optional<VStat> lstat(const std::string& rel) override {
    if (rel == "/") return root_stat();
    const std::string* target = find(rel);
    if (!target) return std::nullopt;
    return image_.lstat(*target);
  }
  std::string readlink(const std::string& rel) override { throw VfsError{lstat(rel) ? kEINVAL : kENOENT, rel}; }
  std::vector<std::string> list(const std::string& rel) override {
    if (rel != "/") throw VfsError{lstat(rel) ? kENOTDIR : kENOENT, rel};
    std::vector<std::string> names;
    for (const auto& [name, target] : files_) names.push_back(name);
    return names;
  }
  std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t mode) override {
    if (rel == "/") {
      if ((flags & kO_ACCMODE) != kO_RDONLY) throw VfsError{kEISDIR, rel};
      return std::make_unique<ImageFile>(std::make_shared<const std::vector<uint8_t>>(), root_stat());
    }
    const std::string* target = find(rel);
    if (!target) throw VfsError{(flags & kO_CREAT) ? kEROFS : kENOENT, rel};
    return image_.open(*target, flags, mode);
  }

 private:
  static VStat root_stat() {
    VStat v;
    v.type = NodeType::kDirectory;
    v.mode = kIfDir | 0755;
    v.nlink = 2;
    v.mtime = kImageTime;
    v.ino = 1;
    return v;
  }
  const std::string* find(const std::string& rel) const {
    auto it = files_.find(rel.substr(1));
    return it == files_.end() ? nullptr : &it->second;
  }

  ImageMount image_;
  std::map<std::string, std::string> files_;
};

// --- Memory mount ----------------------------------------------------------------------

// A flat, read-only directory of files held in memory.
class MemoryMount final : public Mount {
 public:
  using Files = std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>;
  explicit MemoryMount(Files files) : files_(std::move(files)) {}

  std::optional<VStat> lstat(const std::string& rel) override {
    if (rel == "/") return root_stat();
    auto it = files_.find(rel.substr(1));
    if (it == files_.end()) return std::nullopt;
    return file_stat(it);
  }
  std::string readlink(const std::string& rel) override { throw VfsError{lstat(rel) ? kEINVAL : kENOENT, rel}; }
  std::vector<std::string> list(const std::string& rel) override {
    if (rel != "/") throw VfsError{lstat(rel) ? kENOTDIR : kENOENT, rel};
    std::vector<std::string> names;
    for (const auto& [name, data] : files_) names.push_back(name);
    return names;
  }
  std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t) override {
    if (rel == "/") {
      if ((flags & kO_ACCMODE) != kO_RDONLY) throw VfsError{kEISDIR, rel};
      return std::make_unique<ImageFile>(std::make_shared<const std::vector<uint8_t>>(), root_stat());
    }
    auto it = files_.find(rel.substr(1));
    if (it == files_.end()) throw VfsError{(flags & kO_CREAT) ? kEROFS : kENOENT, rel};
    if ((flags & kO_ACCMODE) != kO_RDONLY || (flags & kO_TRUNC)) throw VfsError{kEROFS, rel};
    if (flags & kO_DIRECTORY) throw VfsError{kENOTDIR, rel};
    return std::make_unique<ImageFile>(it->second, file_stat(it));
  }

 private:
  static VStat root_stat() {
    VStat v;
    v.type = NodeType::kDirectory;
    v.mode = kIfDir | 0755;
    v.nlink = 2;
    v.mtime = kImageTime;
    v.ino = 1;
    return v;
  }
  VStat file_stat(Files::const_iterator it) const {
    VStat v;
    v.mode = kIfReg | 0644;
    v.size = it->second->size();
    v.mtime = kImageTime;
    v.ino = 2 + uint64_t(std::distance(files_.begin(), it));
    return v;
  }

  Files files_;
};

// --- APEX mount ------------------------------------------------------------------------

class ApexMount final : public Mount {
 public:
  explicit ApexMount(std::shared_ptr<image::SystemImage> system) : system_(std::move(system)) {}

  std::optional<VStat> lstat(const std::string& rel) override {
    if (rel == "/") return root_stat();
    auto [mod, inner] = split(rel);
    Mount* m = module(mod);
    if (!m) return std::nullopt;
    return m->lstat(inner);
  }
  std::string readlink(const std::string& rel) override {
    auto [mod, inner] = split(rel);
    Mount* m = module(mod);
    if (!m) throw VfsError{kENOENT, rel};
    return m->readlink(inner);
  }
  std::vector<std::string> list(const std::string& rel) override {
    if (rel == "/") {
      std::vector<std::string> names;
      for (const auto& e : system_->apex_entries()) {
        std::string n = e.name;
        for (const char* ext : {".capex", ".apex"}) {
          if (text::ends_with(n, ext)) n.resize(n.size() - std::strlen(ext));
        }
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
      }
      return names;
    }
    auto [mod, inner] = split(rel);
    Mount* m = module(mod);
    if (!m) throw VfsError{kENOENT, rel};
    return m->list(inner);
  }
  std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t mode) override {
    if (rel == "/") {
      if ((flags & kO_ACCMODE) != kO_RDONLY) throw VfsError{kEISDIR, rel};
      return std::make_unique<ImageFile>(std::make_shared<const std::vector<uint8_t>>(), root_stat());
    }
    auto [mod, inner] = split(rel);
    Mount* m = module(mod);
    if (!m) throw VfsError{(flags & kO_CREAT) ? kEROFS : kENOENT, rel};
    return m->open(inner, flags, mode);
  }

 private:
  static VStat root_stat() {
    VStat v;
    v.type = NodeType::kDirectory;
    v.mode = kIfDir | 0755;
    v.nlink = 2;
    v.mtime = kImageTime;
    v.ino = 1;
    return v;
  }
  static std::pair<std::string, std::string> split(const std::string& rel) {
    size_t slash = rel.find('/', 1);
    if (slash == std::string::npos) return {rel.substr(1), "/"};
    return {rel.substr(1, slash - 1), rel.substr(slash)};
  }
  // Opens (once) the APEX whose manifest name is `name`.
  Mount* module(const std::string& name) {
    auto it = modules_.find(name);
    if (it != modules_.end()) return it->second.get();
    if (name.empty() || name.size() > kMaxName) return nullptr;
    std::unique_ptr<Mount> found;
    std::string dir = system_->fs_path("/system/apex");
    auto entries = system_->apex_entries();
    // Most likely files first: the module name itself, then the rest.
    std::stable_sort(entries.begin(), entries.end(), [&](const image::DirEntry& a, const image::DirEntry& b) {
      auto score = [&](const std::string& n) {
        return n == name + ".apex" || n == name + ".capex" || n == name ? 0 : 1;
      };
      return score(a.name) < score(b.name);
    });
    for (const auto& e : entries) {
      try {
        image::Stat st = system_->fs().stat_inode(e.inode);
        if (st.type == image::FileType::kDirectory) {
          auto manifest = image::parse_apex_manifest(system_->fs().read_file(dir + "/" + e.name + "/apex_manifest.pb", 1u << 20));
          if (manifest.name != name) continue;
          // Flattened APEX: a directory of the system image.
          found = image_mount(std::shared_ptr<image::FileSystem>(system_, const_cast<image::FileSystem*>(&system_->fs())),
                              dir + "/" + e.name);
          break;
        }
        if (!text::ends_with(e.name, ".apex") && !text::ends_with(e.name, ".capex")) continue;
        auto apex = std::make_shared<image::Apex>(image::open_apex(system_->fs().read_file(dir + "/" + e.name, 1ull << 30)));
        if (apex->manifest.name != name) continue;
        found = image_mount(std::shared_ptr<image::FileSystem>(apex, apex->fs.get()), "/");
        break;
      } catch (const FormatError&) {
        continue;
      }
    }
    Mount* raw = found.get();
    modules_[name] = std::move(found);  // remember misses too
    return raw;
  }

  std::shared_ptr<image::SystemImage> system_;
  std::map<std::string, std::unique_ptr<Mount>> modules_;
};

// --- Host mount ----------------------------------------------------------------------------

bool reserved_device_name(std::string_view name) {
  std::string base(name.substr(0, name.find('.')));
  for (auto& c : base) c = char(std::toupper(static_cast<unsigned char>(c)));
  static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
  for (const char* r : reserved) {
    if (base == r) return true;
  }
  if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '0' &&
      base[3] <= '9') {
    return true;
  }
  return false;
}

class HostFile final : public OpenFile {
 public:
  HostFile(void* handle, bool readable, bool writable) : h_(handle), readable_(readable), writable_(writable) {}
  ~HostFile() override {
#ifdef _WIN32
    if (h_) CloseHandle(static_cast<HANDLE>(h_));
#endif
  }
  VStat stat() override {
    VStat v;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(static_cast<HANDLE>(h_), &info)) throw VfsError{kEIO, "fstat"};
    v.size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    v.ino = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    uint64_t t = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    v.mtime = int64_t((t - 116444736000000000ull) / 10000000);
    v.type = NodeType::kFile;
    v.mode = kIfReg | 0660;
    v.uid = v.gid = 10000;
#endif
    return v;
  }
  size_t read(uint64_t offset, std::span<uint8_t> out) override {
    if (!readable_) throw VfsError{kEBADF, "read"};
#ifdef _WIN32
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(h_), out.data(), DWORD(std::min<size_t>(out.size(), 1u << 30)), &got, &ov)) {
      if (GetLastError() == ERROR_HANDLE_EOF) return 0;
      throw VfsError{kEIO, "read"};
    }
    return got;
#else
    return 0;
#endif
  }
  size_t write(uint64_t offset, std::span<const uint8_t> data) override {
    if (!writable_) throw VfsError{kEBADF, "write"};
#ifdef _WIN32
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD done = 0;
    if (!WriteFile(static_cast<HANDLE>(h_), data.data(), DWORD(std::min<size_t>(data.size(), 1u << 30)), &done, &ov)) {
      throw VfsError{GetLastError() == ERROR_DISK_FULL ? kENOSPC : kEIO, "write"};
    }
    return done;
#else
    return 0;
#endif
  }
  void truncate(uint64_t size) override {
    if (!writable_) throw VfsError{kEINVAL, "truncate"};
#ifdef _WIN32
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = LONGLONG(size);
    if (!SetFileInformationByHandle(static_cast<HANDLE>(h_), FileEndOfFileInfo, &info, sizeof info)) {
      throw VfsError{kEIO, "truncate"};
    }
#endif
  }
  void sync() override {
#ifdef _WIN32
    if (writable_) FlushFileBuffers(static_cast<HANDLE>(h_));
#endif
  }

 private:
  void* h_;
  bool readable_, writable_;
};

class HostMount final : public Mount {
 public:
  HostMount(std::filesystem::path dir, bool read_only) : root_(std::move(dir)), read_only_(read_only) {
    std::error_code ec;
    if (!read_only_) std::filesystem::create_directories(root_, ec);
  }
  bool writable() const override { return !read_only_; }

  std::optional<VStat> lstat(const std::string& rel) override {
    std::filesystem::path p = host(rel);
    std::error_code ec;
    auto st = std::filesystem::symlink_status(p, ec);
    if (ec || !std::filesystem::exists(st)) return std::nullopt;
    if (std::filesystem::is_symlink(st) || is_reparse_point(p)) throw VfsError{kEACCES, rel};  // never followed
    VStat v;
    v.uid = v.gid = 10000;
    auto t = std::filesystem::last_write_time(p, ec);
    if (!ec) {
      v.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch()).count();
    }
    if (std::filesystem::is_directory(st)) {
      v.type = NodeType::kDirectory;
      v.mode = kIfDir | (read_only_ ? 0755 : 0771);
      v.nlink = 2;
    } else {
      v.type = NodeType::kFile;
      v.mode = kIfReg | (read_only_ ? 0644 : 0660);
      v.size = std::filesystem::file_size(p, ec);
    }
    v.ino = std::hash<std::string>{}(rel);
    return v;
  }
  std::string readlink(const std::string& rel) override {
    if (!lstat(rel)) throw VfsError{kENOENT, rel};
    throw VfsError{kEINVAL, rel};  // no symbolic links in host areas
  }
  std::vector<std::string> list(const std::string& rel) override {
    auto st = lstat(rel);
    if (!st) throw VfsError{kENOENT, rel};
    if (st->type != NodeType::kDirectory) throw VfsError{kENOTDIR, rel};
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(host(rel), ec)) {
      out.push_back(decode_host_name(path_utf8(e.path().filename())));
    }
    return out;
  }
  std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t mode) override {
    int32_t acc = flags & kO_ACCMODE;
    bool want_write = acc == kO_WRONLY || acc == kO_RDWR || (flags & (kO_TRUNC | kO_CREAT));
    if (want_write && read_only_) throw VfsError{kEROFS, rel};
    auto st = lstat(rel);
    if (st && (flags & kO_CREAT) && (flags & kO_EXCL)) throw VfsError{kEEXIST, rel};
    if (!st && !(flags & kO_CREAT)) throw VfsError{kENOENT, rel};
    if (st && st->type == NodeType::kDirectory) {
      if (acc != kO_RDONLY) throw VfsError{kEISDIR, rel};
      VStat dir = *st;
      return std::make_unique<ImageFile>(std::make_shared<const std::vector<uint8_t>>(), dir);
    }
    if (flags & kO_DIRECTORY) throw VfsError{kENOTDIR, rel};
    if (!st) check_parent(rel);
#ifdef _WIN32
    DWORD access = acc == kO_RDONLY ? GENERIC_READ : acc == kO_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    DWORD disposition = (flags & kO_CREAT) ? ((flags & kO_EXCL) ? CREATE_NEW : (flags & kO_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                           : ((flags & kO_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING);
    HANDLE h = CreateFileW(host(rel).wstring().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      DWORD e = GetLastError();
      throw VfsError{e == ERROR_FILE_EXISTS ? kEEXIST : e == ERROR_ACCESS_DENIED ? kEACCES : kENOENT, rel};
    }
    return std::make_unique<HostFile>(h, acc != kO_WRONLY, acc != kO_RDONLY);
#else
    (void)mode;
    throw VfsError{kEIO, rel};
#endif
  }
  void mkdir(const std::string& rel, uint32_t) override {
    if (read_only_) throw VfsError{kEROFS, rel};
    if (lstat(rel)) throw VfsError{kEEXIST, rel};
    check_parent(rel);
    std::error_code ec;
    if (!std::filesystem::create_directory(host(rel), ec)) throw VfsError{kEIO, rel};
  }
  void remove(const std::string& rel, bool directory) override {
    if (read_only_) throw VfsError{kEROFS, rel};
    if (rel == "/") throw VfsError{kEACCES, rel};
    auto st = lstat(rel);
    if (!st) throw VfsError{kENOENT, rel};
    if (directory && st->type != NodeType::kDirectory) throw VfsError{kENOTDIR, rel};
    if (!directory && st->type == NodeType::kDirectory) throw VfsError{kEISDIR, rel};
    std::error_code ec;
    if (directory && !std::filesystem::is_empty(host(rel), ec)) throw VfsError{kENOTEMPTY, rel};
    if (!std::filesystem::remove(host(rel), ec)) throw VfsError{kEIO, rel};
  }
  void rename(const std::string& from, const std::string& to) override {
    if (read_only_) throw VfsError{kEROFS, from};
    if (!lstat(from)) throw VfsError{kENOENT, from};
    check_parent(to);
    std::error_code ec;
    std::filesystem::rename(host(from), host(to), ec);
    if (ec) throw VfsError{kEIO, from};
  }
  void set_mtime(const std::string& rel, int64_t seconds) override {
    if (read_only_) throw VfsError{kEROFS, rel};
    if (!lstat(rel)) throw VfsError{kENOENT, rel};
    std::error_code ec;
    auto t = std::chrono::clock_cast<std::filesystem::file_time_type::clock>(
        std::chrono::system_clock::time_point(std::chrono::seconds(seconds)));
    std::filesystem::last_write_time(host(rel), t, ec);
  }

 private:
  std::filesystem::path host(const std::string& rel) const {
    std::filesystem::path p = root_;
    for (const auto& c : components(rel)) {
      if (c == "." || c == "..") throw VfsError{kEINVAL, rel};  // normalised paths only
      if (c.size() > kMaxName) throw VfsError{kENAMETOOLONG, rel};
      p /= path_from_utf8(encode_host_name(c));
    }
    return p;
  }
  void check_parent(const std::string& rel) {
    size_t slash = rel.find_last_of('/');
    std::string parent = slash == 0 ? "/" : rel.substr(0, slash);
    auto st = lstat(parent);
    if (!st) throw VfsError{kENOENT, rel};
    if (st->type != NodeType::kDirectory) throw VfsError{kENOTDIR, rel};
  }
  // Junctions and symbolic links created on the host are never traversed.
  bool is_reparse_point(const std::filesystem::path& p) const {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
  }

  std::filesystem::path root_;
  bool read_only_;
};

// --- /dev -------------------------------------------------------------------------------

class DevFile final : public OpenFile {
 public:
  explicit DevFile(std::string name) : name_(std::move(name)) {}
  VStat stat() override {
    VStat v;
    v.type = NodeType::kCharDevice;
    v.mode = kIfChr | 0666;
    v.ino = std::hash<std::string>{}(name_);
    return v;
  }
  size_t read(uint64_t, std::span<uint8_t> out) override {
    if (name_ == "null") return 0;
    if (name_ == "zero") {
      std::memset(out.data(), 0, out.size());
      return out.size();
    }
    // random / urandom: the operating system's cryptographic generator.
    for (size_t i = 0; i < out.size();) {
      unsigned int r = 0;
      if (rand_s(&r) != 0) throw VfsError{kEIO, "urandom"};
      for (int k = 0; k < 4 && i < out.size(); ++k, ++i) out[i] = uint8_t(r >> (8 * k));
    }
    return out.size();
  }
  size_t write(uint64_t, std::span<const uint8_t> data) override { return data.size(); }
  void truncate(uint64_t) override {}
  bool seekable() const override { return false; }

 private:
  std::string name_;
};

class DevMount final : public Mount {
 public:
  std::optional<VStat> lstat(const std::string& rel) override {
    if (rel == "/") {
      VStat v;
      v.type = NodeType::kDirectory;
      v.mode = kIfDir | 0755;
      v.nlink = 2;
      return v;
    }
    std::string name = rel.substr(1);
    if (!known(name)) return std::nullopt;
    return DevFile(name).stat();
  }
  std::string readlink(const std::string& rel) override { throw VfsError{lstat(rel) ? kEINVAL : kENOENT, rel}; }
  std::vector<std::string> list(const std::string& rel) override {
    if (rel != "/") throw VfsError{kENOTDIR, rel};
    return {"null", "zero", "random", "urandom"};
  }
  std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t) override {
    std::string name = rel.substr(1);
    if (!known(name)) throw VfsError{(flags & kO_CREAT) ? kEACCES : kENOENT, rel};
    return std::make_unique<DevFile>(name);
  }

 private:
  static bool known(const std::string& n) { return n == "null" || n == "zero" || n == "random" || n == "urandom"; }
};

}  // namespace

// --- Host name encoding --------------------------------------------------------------------

std::string encode_host_name(std::string_view name) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    bool trailing = i + 1 == name.size() && (c == '.' || c == ' ');
    bool bad = c < 0x20 || c == '%' || std::strchr("<>:\"\\|?*", c) != nullptr || trailing;
    if (i == 0 && reserved_device_name(name)) bad = true;
    if (bad) {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += char(c);
    }
  }
  return out;
}

std::string decode_host_name(std::string_view name) {
  std::string out;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '%' && i + 2 < name.size() && val(name[i + 1]) >= 0 && val(name[i + 2]) >= 0) {
      out += char(val(name[i + 1]) * 16 + val(name[i + 2]));
      i += 2;
    } else {
      out += name[i];
    }
  }
  return out;
}

std::unique_ptr<Mount> image_mount(std::shared_ptr<image::FileSystem> fs, std::string root) {
  return std::make_unique<ImageMount>(std::move(fs), std::move(root));
}
std::unique_ptr<Mount> apex_mount(std::shared_ptr<image::SystemImage> system) {
  return std::make_unique<ApexMount>(std::move(system));
}
std::unique_ptr<Mount> alias_mount(std::shared_ptr<image::FileSystem> fs, const std::map<std::string, std::string>& files) {
  std::map<std::string, std::string> present;
  for (const auto& [name, target] : files) {
    if (name.empty() || name.size() > kMaxName || name.find('/') != std::string::npos) continue;
    try {
      auto st = fs->stat(target, false);
      if (st && st->type == image::FileType::kRegular) present[name] = target;
    } catch (const FormatError&) {
      continue;  // unreadable entry: left out, as a missing one
    }
  }
  return std::make_unique<AliasMount>(std::move(fs), std::move(present));
}
std::unique_ptr<Mount> memory_mount(std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files) {
  for (auto it = files.begin(); it != files.end();) {
    const std::string& name = it->first;
    bool bad = name.empty() || name == "." || name == ".." || name.size() > kMaxName ||
               name.find_first_of(std::string_view("/\0", 2)) != std::string::npos || !it->second;
    it = bad ? files.erase(it) : std::next(it);
  }
  return std::make_unique<MemoryMount>(std::move(files));
}
std::unique_ptr<Mount> host_mount(const std::filesystem::path& dir, bool read_only) {
  return std::make_unique<HostMount>(dir, read_only);
}
std::unique_ptr<Mount> dev_mount() { return std::make_unique<DevMount>(); }

// --- Vfs ---------------------------------------------------------------------------------------

void Vfs::mount(const std::string& prefix, std::unique_ptr<Mount> m) { mounts_[normalize(prefix)] = std::move(m); }

std::string Vfs::normalize(std::string_view path, std::string_view cwd) {
  if (path.find('\0') != std::string_view::npos) throw VfsError{kEINVAL, "NUL dans un chemin"};
  if (path.size() > kMaxPath) throw VfsError{kENAMETOOLONG, std::string(path.substr(0, 64))};
  std::string full = !path.empty() && path[0] == '/' ? std::string(path) : std::string(cwd) + "/" + std::string(path);
  std::vector<std::string> out;
  for (auto& c : components(full)) {
    if (c == ".") continue;
    if (c == "..") {
      if (!out.empty()) out.pop_back();
      continue;
    }
    if (c.size() > kMaxName) throw VfsError{kENAMETOOLONG, c};
    out.push_back(std::move(c));
  }
  std::string s;
  for (const auto& c : out) s += "/" + c;
  return s.empty() ? "/" : s;
}

Vfs::Located Vfs::locate(const std::string& p) {
  Mount* best = nullptr;
  size_t best_len = 0;
  std::string prefix;
  for (auto& [pre, m] : mounts_) {
    bool match = pre == "/" || p == pre || (p.size() > pre.size() && p.compare(0, pre.size(), pre) == 0 && p[pre.size()] == '/');
    if (match && (best == nullptr || pre.size() > best_len)) {
      best = m.get();
      best_len = pre.size();
      prefix = pre;
    }
  }
  if (!best) throw VfsError{kENOENT, p};
  std::string rel = prefix == "/" ? p : p.substr(prefix.size());
  if (rel.empty()) rel = "/";
  return {best, rel};
}

std::string Vfs::resolve(std::string_view path, bool follow_last) {
  std::string p = normalize(path);
  for (int links = 0; links <= kMaxSymlinks; ++links) {
    auto parts = components(p);
    std::string cur;
    bool restarted = false;
    for (size_t i = 0; i < parts.size(); ++i) {
      std::string cand = cur + "/" + parts[i];
      bool last = i + 1 == parts.size();
      if (!last || follow_last) {
        Located l = locate(cand);
        auto st = l.mount->lstat(l.rel);
        if (st && st->type == NodeType::kSymlink) {
          std::string target = l.mount->readlink(l.rel);
          std::string rest;
          for (size_t k = i + 1; k < parts.size(); ++k) rest += "/" + parts[k];
          p = normalize(target[0] == '/' ? target + rest : (cur.empty() ? "" : cur) + "/" + target + rest);
          restarted = true;
          break;
        }
      }
      cur = cand;
    }
    if (!restarted) return cur.empty() ? "/" : cur;
  }
  throw VfsError{kELOOP, std::string(path)};
}

bool Vfs::mount_ancestor(const std::string& p) const {
  for (const auto& [pre, m] : mounts_) {
    if (pre != "/" && pre.size() > p.size() && pre.compare(0, p.size(), p) == 0 && (p == "/" || pre[p.size()] == '/')) {
      return true;
    }
  }
  return false;
}

VStat Vfs::stat(std::string_view path, bool follow) {
  std::string p = resolve(path, follow);
  Located l = locate(p);
  auto st = l.mount->lstat(l.rel);
  if (!st) {
    if (!mount_ancestor(p)) throw VfsError{kENOENT, p};
    // Parent directories of mount points exist, as on a real system.
    VStat dir;
    dir.type = NodeType::kDirectory;
    dir.mode = kIfDir | 0755;
    dir.nlink = 2;
    dir.mtime = kImageTime;
    dir.ino = std::hash<std::string>{}(p);
    return dir;
  }
  return *st;
}

bool Vfs::exists(std::string_view path) {
  try {
    stat(path);
    return true;
  } catch (const VfsError&) {
    return false;
  }
}

std::string Vfs::readlink(std::string_view path) {
  std::string p = resolve(path, false);
  Located l = locate(p);
  return l.mount->readlink(l.rel);
}

std::vector<std::string> Vfs::list(std::string_view path) {
  std::string p = resolve(path);
  Located l = locate(p);
  std::vector<std::string> out;
  try {
    out = l.mount->list(l.rel);
  } catch (const VfsError& e) {
    if (e.err != kENOENT || !mount_ancestor(p)) throw;
  }
  // Mount points appear in their parent directory.
  for (const auto& [pre, m] : mounts_) {
    if (pre == "/" || pre == p) continue;
    size_t slash = pre.find_last_of('/');
    std::string parent = slash == 0 ? "/" : pre.substr(0, slash);
    std::string name = pre.substr(slash + 1);
    if (parent == p && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void Vfs::mkdir(std::string_view path, uint32_t mode) {
  std::string p = resolve(path, false);
  Located l = locate(p);
  if (l.rel == "/") throw VfsError{kEEXIST, p};
  l.mount->mkdir(l.rel, mode);
}

void Vfs::unlink(std::string_view path) {
  std::string p = resolve(path, false);
  Located l = locate(p);
  l.mount->remove(l.rel, false);
}

void Vfs::rmdir(std::string_view path) {
  std::string p = resolve(path, false);
  Located l = locate(p);
  if (l.rel == "/") throw VfsError{kEACCES, p};
  l.mount->remove(l.rel, true);
}

void Vfs::rename(std::string_view from, std::string_view to) {
  std::string a = resolve(from, false), b = resolve(to, false);
  Located la = locate(a), lb = locate(b);
  if (la.mount != lb.mount) throw VfsError{kEXDEV, a};
  la.mount->rename(la.rel, lb.rel);
}

void Vfs::set_mtime(std::string_view path, int64_t seconds) {
  std::string p = resolve(path);
  Located l = locate(p);
  l.mount->set_mtime(l.rel, seconds);
}

bool Vfs::writable(std::string_view path) {
  std::string p = resolve(path);
  return locate(p).mount->writable();
}

int32_t Vfs::open(std::string_view path, int32_t flags, uint32_t mode) {
  if (fds_.size() >= kMaxOpenFiles) throw VfsError{kEMFILE, std::string(path)};
  std::string p = resolve(path, !(flags & kO_NOFOLLOW));
  Located l = locate(p);
  if (flags & kO_NOFOLLOW) {
    auto st = l.mount->lstat(l.rel);
    if (st && st->type == NodeType::kSymlink) throw VfsError{kELOOP, p};
  }
  Fd entry;
  entry.file = std::shared_ptr<OpenFile>(l.mount->open(l.rel, flags, mode));
  entry.flags = flags;
  entry.path = p;
  int32_t fd = 3;
  while (fds_.count(fd)) ++fd;
  fds_[fd] = std::move(entry);
  return fd;
}

Vfs::Fd& Vfs::fd_entry(int32_t fd) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) throw VfsError{kEBADF, "fd " + std::to_string(fd)};
  return it->second;
}

void Vfs::close(int32_t fd) {
  if (!fds_.erase(fd)) throw VfsError{kEBADF, "fd " + std::to_string(fd)};
}

size_t Vfs::read(int32_t fd, std::span<uint8_t> out) {
  Fd& e = fd_entry(fd);
  if ((e.flags & kO_ACCMODE) == kO_WRONLY) throw VfsError{kEBADF, e.path};
  size_t n = e.file->read(e.pos, out);
  e.pos += n;
  return n;
}

size_t Vfs::write(int32_t fd, std::span<const uint8_t> data) {
  Fd& e = fd_entry(fd);
  if ((e.flags & kO_ACCMODE) == kO_RDONLY) throw VfsError{kEBADF, e.path};
  if (e.flags & kO_APPEND) e.pos = e.file->stat().size;
  size_t n = e.file->write(e.pos, data);
  e.pos += n;
  return n;
}

size_t Vfs::pread(int32_t fd, std::span<uint8_t> out, uint64_t offset) {
  Fd& e = fd_entry(fd);
  if (!e.file->seekable()) throw VfsError{kESPIPE, e.path};
  if ((e.flags & kO_ACCMODE) == kO_WRONLY) throw VfsError{kEBADF, e.path};
  return e.file->read(offset, out);
}

size_t Vfs::pwrite(int32_t fd, std::span<const uint8_t> data, uint64_t offset) {
  Fd& e = fd_entry(fd);
  if (!e.file->seekable()) throw VfsError{kESPIPE, e.path};
  if ((e.flags & kO_ACCMODE) == kO_RDONLY) throw VfsError{kEBADF, e.path};
  return e.file->write(offset, data);
}

int64_t Vfs::lseek(int32_t fd, int64_t offset, int32_t whence) {
  Fd& e = fd_entry(fd);
  if (!e.file->seekable()) return 0;
  int64_t base = whence == 0 ? 0 : whence == 1 ? int64_t(e.pos) : whence == 2 ? int64_t(e.file->stat().size) : -1;
  if (base < 0) throw VfsError{kEINVAL, e.path};
  int64_t pos = base + offset;
  if (pos < 0) throw VfsError{kEINVAL, e.path};
  e.pos = uint64_t(pos);
  return pos;
}

VStat Vfs::fstat(int32_t fd) { return fd_entry(fd).file->stat(); }

void Vfs::ftruncate(int32_t fd, uint64_t size) {
  Fd& e = fd_entry(fd);
  if ((e.flags & kO_ACCMODE) == kO_RDONLY) throw VfsError{kEINVAL, e.path};
  e.file->truncate(size);
}

void Vfs::fsync(int32_t fd) { fd_entry(fd).file->sync(); }
int32_t Vfs::open_flags(int32_t fd) { return fd_entry(fd).flags; }
std::string Vfs::fd_path(int32_t fd) { return fd_entry(fd).path; }

std::shared_ptr<OpenFile> Vfs::open_file(int32_t fd) { return fd_entry(fd).file; }

std::vector<uint8_t> Vfs::read_file(std::string_view path, uint64_t max_size) {
  int32_t fd = open(path, kO_RDONLY, 0);
  std::vector<uint8_t> out;
  try {
    VStat st = fstat(fd);
    if (st.type == NodeType::kDirectory) throw VfsError{kEISDIR, std::string(path)};
    if (st.size > max_size) throw VfsError{kEFBIG, std::string(path)};
    out.resize(size_t(st.size));
    size_t got = 0;
    while (got < out.size()) {
      size_t n = read(fd, std::span(out).subspan(got));
      if (n == 0) break;
      got += n;
    }
    out.resize(got);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
  return out;
}

void Vfs::write_file(std::string_view path, std::span<const uint8_t> data) {
  int32_t fd = open(path, kO_WRONLY | kO_CREAT | kO_TRUNC, 0600);
  try {
    size_t done = 0;
    while (done < data.size()) done += write(fd, data.subspan(done));
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
}

// --- Standard Android layout ---------------------------------------------------------------------

std::shared_ptr<Vfs> make_android_vfs(const LayoutOptions& o) {
  // The package name comes from the application's manifest and becomes a path
  // component of the mounts below: only Java package names are accepted.
  bool valid = !o.package.empty() && o.package.size() <= kMaxName;
  bool segment_start = true;
  for (char c : o.package) {
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool digit = c >= '0' && c <= '9';
    if (c == '.') {
      valid &= !segment_start;
      segment_start = true;
    } else {
      valid &= letter || (digit && !segment_start);
      segment_start = false;
    }
  }
  if (!valid || segment_start) throw std::invalid_argument("nom de paquet invalide : " + o.package.substr(0, 64));
  auto vfs = std::make_shared<Vfs>();
  if (o.system) {
    std::shared_ptr<image::FileSystem> fs(o.system, const_cast<image::FileSystem*>(&o.system->fs()));
    // A system-as-root image provides "/" (with /system inside); otherwise
    // the partition is the /system tree.
    bool as_root = o.system->fs_path("/system") == "/system";
    if (as_root) {
      vfs->mount("/", image_mount(fs, "/"));
    } else {
      vfs->mount("/", host_mount(o.data_root / "root", false));
      vfs->mount("/system", image_mount(fs, "/"));
    }
    vfs->mount("/apex", apex_mount(o.system));
    // aconfig flag storage, laid out by aconfigd at boot from each partition's
    // etc/aconfig: /metadata/aconfig/maps/<container>.package.map and
    // .flag.map, /metadata/aconfig/boot/<container>.val and .info (the
    // image's default values; no overrides are applied).
    std::string sys = as_root ? "/system" : "";
    std::map<std::string, std::string> maps, boot;
    for (const auto& [container, dir] : {std::pair<std::string, std::string>{"system", sys + "/etc/aconfig"},
                                         {"system_ext", sys + "/system_ext/etc/aconfig"},
                                         {"product", sys + "/product/etc/aconfig"}}) {
      maps[container + ".package.map"] = dir + "/package.map";
      maps[container + ".flag.map"] = dir + "/flag.map";
      boot[container + ".val"] = dir + "/flag.val";
      boot[container + ".info"] = dir + "/flag.info";
    }
    vfs->mount("/metadata/aconfig/maps", alias_mount(fs, maps));
    vfs->mount("/metadata/aconfig/boot", alias_mount(fs, boot));
  } else if (!o.base_dir.empty()) {
    vfs->mount("/", host_mount(o.base_dir, true));
  } else {
    vfs->mount("/", host_mount(o.data_root / "root", true));
  }
  std::filesystem::path app = o.data_root / encode_host_name(o.package);
  if (!o.app_files.empty()) vfs->mount("/data/app/" + o.package, memory_mount(o.app_files));
  vfs->mount("/data/data/" + o.package, host_mount(app / "data", false));
  vfs->mount("/data/user/0/" + o.package, host_mount(app / "data", false));
  vfs->mount("/data/user_de/0/" + o.package, host_mount(app / "data_de", false));
  vfs->mount("/data/local/tmp", host_mount(app / "tmp", false));
  vfs->mount("/storage/emulated/0", host_mount(app / "sdcard", false));
  vfs->mount("/sdcard", host_mount(app / "sdcard", false));
  vfs->mount("/dev", dev_mount());
  return vfs;
}

}  // namespace aa::vfs
