#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "image/aosp.h"
#include "image/filesystem.h"

// Virtual file system of an emulated Android process (ADR-005).
//
// The Android tree is assembled from mounts, matched by longest prefix:
//  - the system image, read-only, as the base layer ("/");
//  - APEX modules under /apex/<name>, opened on demand from /system/apex;
//  - the application's APK under /data/app/<package>, read-only, from memory;
//  - a host directory per application for writable areas (/data, /sdcard),
//    sandboxed: Linux names are encoded, reparse points refused;
//  - /dev (null, zero, random, urandom).
// Symbolic links are resolved at the VFS level, so they may cross mounts.
// Errors are Linux errno values (VfsError), never host error codes.
namespace aa::vfs {

// Linux errno values used by the VFS.
enum : int32_t {
  kEPERM = 1, kENOENT = 2, kEIO = 5, kEBADF = 9, kENOMEM = 12, kEACCES = 13, kEEXIST = 17, kEXDEV = 18, kENOTDIR = 20,
  kEISDIR = 21, kEINVAL = 22, kEMFILE = 24, kENOTTY = 25, kEFBIG = 27, kENOSPC = 28, kESPIPE = 29,
  kEROFS = 30, kENAMETOOLONG = 36, kENOTEMPTY = 39, kELOOP = 40, kENODATA = 61,
};

struct VfsError {
  int32_t err;
  std::string context;
};

enum class NodeType { kFile, kDirectory, kSymlink, kCharDevice };

// Linux open(2) flags (x86_64 values, as published by OsConstants).
constexpr int32_t kO_ACCMODE = 03, kO_RDONLY = 0, kO_WRONLY = 01, kO_RDWR = 02, kO_CREAT = 0100, kO_EXCL = 0200,
                  kO_TRUNC = 01000, kO_APPEND = 02000, kO_DIRECTORY = 0200000, kO_NOFOLLOW = 0400000;

struct VStat {
  NodeType type = NodeType::kFile;
  uint32_t mode = 0;  // type and permission bits (S_IFREG | 0644...)
  uint64_t size = 0;
  uint64_t ino = 0;
  uint64_t nlink = 1;
  int64_t mtime = 0;  // seconds since the epoch
  uint32_t uid = 0, gid = 0;
  uint64_t dev = 0;
};

class OpenFile {
 public:
  virtual ~OpenFile() = default;
  virtual VStat stat() = 0;
  // Reads at `offset`; returns the number of bytes read (0 at the end).
  virtual size_t read(uint64_t offset, std::span<uint8_t> out) = 0;
  virtual size_t write(uint64_t offset, std::span<const uint8_t> data) = 0;
  virtual void truncate(uint64_t size) = 0;
  virtual void sync() {}
  virtual bool seekable() const { return true; }
};

class Mount {
 public:
  virtual ~Mount() = default;
  // `rel` is the path inside the mount, "/"-separated and normalised ("/" is the root).
  virtual std::optional<VStat> lstat(const std::string& rel) = 0;
  virtual std::string readlink(const std::string& rel) = 0;
  virtual std::vector<std::string> list(const std::string& rel) = 0;
  virtual std::unique_ptr<OpenFile> open(const std::string& rel, int32_t flags, uint32_t mode) = 0;
  virtual void mkdir(const std::string& rel, uint32_t mode) { throw VfsError{kEROFS, rel}; }
  virtual void remove(const std::string& rel, bool directory) { throw VfsError{kEROFS, rel}; }
  virtual void rename(const std::string& from, const std::string& to) { throw VfsError{kEROFS, from}; }
  virtual void set_mtime(const std::string& rel, int64_t seconds) { throw VfsError{kEROFS, rel}; }
  virtual bool writable() const { return false; }
};

// Read-only view of a file system from an Android image.
std::unique_ptr<Mount> image_mount(std::shared_ptr<image::FileSystem> fs, std::string root = "/");
// APEX modules of a system image (/apex/<module>/...), opened on demand.
std::unique_ptr<Mount> apex_mount(std::shared_ptr<image::SystemImage> system);
// Read-only directory whose entries are regular files of an image under other
// names: `files` maps an entry name to a path in `fs` (missing ones are skipped).
std::unique_ptr<Mount> alias_mount(std::shared_ptr<image::FileSystem> fs, const std::map<std::string, std::string>& files);
// Read-only flat directory of files held in memory (`files` maps an entry
// name to its bytes), such as an application's APK under /data/app/<package>.
std::unique_ptr<Mount> memory_mount(std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files);
// A host directory (created if needed). Writable unless read_only.
std::unique_ptr<Mount> host_mount(const std::filesystem::path& dir, bool read_only);
// /dev: null, zero, random, urandom.
std::unique_ptr<Mount> dev_mount();

// Host encoding of a Linux file name (reversible): characters Windows refuses,
// '%' itself, trailing dots/spaces and reserved device names become %XX.
std::string encode_host_name(std::string_view name);
std::string decode_host_name(std::string_view name);

class Vfs {
 public:
  void mount(const std::string& prefix, std::unique_ptr<Mount> m);

  // Absolute, normalised path ("." and ".." resolved lexically).
  static std::string normalize(std::string_view path, std::string_view cwd = "/");
  // Resolves symbolic links (all components, the last one when follow_last).
  std::string resolve(std::string_view path, bool follow_last = true);

  VStat stat(std::string_view path, bool follow = true);
  bool exists(std::string_view path);
  std::string readlink(std::string_view path);
  std::vector<std::string> list(std::string_view path);
  void mkdir(std::string_view path, uint32_t mode);
  void unlink(std::string_view path);
  void rmdir(std::string_view path);
  void rename(std::string_view from, std::string_view to);
  void set_mtime(std::string_view path, int64_t seconds);
  bool writable(std::string_view path);

  // File descriptors (3 and above; 0-2 are the standard streams).
  int32_t open(std::string_view path, int32_t flags, uint32_t mode);
  void close(int32_t fd);
  bool is_open(int32_t fd) const { return fds_.count(fd) != 0; }
  size_t read(int32_t fd, std::span<uint8_t> out);
  size_t write(int32_t fd, std::span<const uint8_t> data);
  size_t pread(int32_t fd, std::span<uint8_t> out, uint64_t offset);
  size_t pwrite(int32_t fd, std::span<const uint8_t> data, uint64_t offset);
  int64_t lseek(int32_t fd, int64_t offset, int32_t whence);
  VStat fstat(int32_t fd);
  void ftruncate(int32_t fd, uint64_t size);
  void fsync(int32_t fd);
  int32_t open_flags(int32_t fd);
  std::string fd_path(int32_t fd);
  // The open file behind `fd`; stays usable after close (file mappings).
  std::shared_ptr<OpenFile> open_file(int32_t fd);

  // Whole-file helpers (tests, host code).
  std::vector<uint8_t> read_file(std::string_view path, uint64_t max_size = 64u << 20);
  void write_file(std::string_view path, std::span<const uint8_t> data);

 private:
  struct Located {
    Mount* mount;
    std::string rel;
  };
  struct Fd {
    std::shared_ptr<OpenFile> file;
    uint64_t pos = 0;
    int32_t flags = 0;
    std::string path;
  };
  Located locate(const std::string& resolved);
  bool mount_ancestor(const std::string& resolved) const;  // strict parent of a mount point
  Fd& fd_entry(int32_t fd);

  std::map<std::string, std::unique_ptr<Mount>, std::greater<>> mounts_;  // longest prefix first
  std::map<int32_t, Fd> fds_;
};

// Standard layout of an Android process: base layer (system image or a
// read-only host directory such as an extracted boot class path), APEX
// modules, /data/app/<package>, /data/data/<package>, /data/user/0/<package>, /data/local/tmp,
// /sdcard and /dev, the writable areas living under `data_root`.
struct LayoutOptions {
  std::shared_ptr<image::SystemImage> system;  // optional
  std::filesystem::path base_dir;              // used when no system image
  std::filesystem::path data_root;             // host directory for writable areas
  std::string package = "exec";
  // Files of /data/app/<package> (base.apk), served read-only from memory.
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> app_files;
};
std::shared_ptr<Vfs> make_android_vfs(const LayoutOptions& options);

}  // namespace aa::vfs
