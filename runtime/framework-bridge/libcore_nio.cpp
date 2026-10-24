// NIO natives of libcore (sun.nio.fs.UnixNativeDispatcher, sun.nio.ch.FileDispatcherImpl,
// sun.nio.ch.FileChannelImpl) on top of the virtual file system of the emulated
// process (ADR-005). Paths and buffers arrive as emulated native addresses
// (native_memory.h); directory streams are bounded ids, never DIR* pointers.
// Errors follow the real natives: UnixNativeDispatcher throws
// sun.nio.fs.UnixException(errno), the channel natives java.io.IOException
// carrying the strerror text.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"
#include "vfs/vfs.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

vfs::Vfs& the_vfs() { return libcore_vfs(); }

constexpr int32_t kIosEof = -1;  // sun.nio.ch.IOStatus.EOF
constexpr int32_t kSeekSet = 0, kSeekCur = 1;
constexpr int64_t kPageSize = 4096;  // sysconf(_SC_PAGESIZE), see libcore_natives.cpp

bool std_fd(int32_t fd) { return fd >= 0 && fd <= 2; }

[[noreturn]] void throw_unix(Runtime& rt, int32_t err) {
  throw rt::JavaThrow{construct(rt, "Lsun/nio/fs/UnixException;", "(I)V", {uint32_t(err)})};
}

[[noreturn]] void throw_io(Runtime& rt, int32_t err) { rt.throw_new("Ljava/io/IOException;", strerror_text(err)); }

// Runs a Vfs operation for UnixNativeDispatcher: errors become UnixException.
template <typename F>
auto unix_call(Runtime& rt, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const vfs::VfsError& e) {
    throw_unix(rt, e.err);
  }
}

// Runs a Vfs operation for the channel natives: errors become IOException.
template <typename F>
auto io_call(Runtime& rt, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const vfs::VfsError& e) {
    throw_io(rt, e.err);
  }
}

std::string path_from_address(Runtime& rt, int64_t address) {
  if (address == 0) rt.throw_npe("path address is null");
  return native_cstring(rt, address);
}

// `len` bytes at a native address; `write` when the native stores into them.
std::span<uint8_t> buffer_from_address(Runtime& rt, int64_t address, int32_t len, bool write) {
  if (len < 0) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "buffer length");
  if (len == 0) return {};
  return native_span(rt, address, uint64_t(len), write);
}

Handle make_byte_array(Runtime& rt, const std::string& str) {
  Handle h = rt.new_array("[B", int32_t(str.size()));
  rt::ArrayObject& arr = rt.array(h, "byte[]");
  std::memcpy(arr.data.data(), str.data(), str.size());
  return h;
}

// Fills a sun.nio.fs.UnixFileAttributes. The Vfs keeps whole seconds and a
// single time stamp: access and change times are the modification time.
void set_unix_attributes(Runtime& rt, Handle attrs, const vfs::VStat& st) {
  rt::Object& o = rt.nonnull(attrs, "UnixFileAttributes");
  auto slot = [&](const char* name, const char* type) {
    rt::Field* f = find_field(o.klass, name, type);
    if (!f || f->is_static()) {
      rt.throw_new("Ljava/lang/InternalError;", std::string("UnixFileAttributes sans champ ") + name);
    }
    return f->slot;
  };
  auto set_int = [&](const char* name, int32_t v) { o.fields.at(slot(name, "I")) = uint32_t(v); };
  auto set_long = [&](const char* name, int64_t v) {
    auto s = slot(name, "J");
    o.fields.at(s) = uint32_t(uint64_t(v));
    o.fields.at(s + 1) = uint32_t(uint64_t(v) >> 32);
  };
  set_int("st_mode", int32_t(st.mode));
  set_long("st_ino", int64_t(st.ino));
  set_long("st_dev", int64_t(st.dev ? st.dev : 0xFD00));
  set_long("st_rdev", 0);
  set_int("st_nlink", int32_t(st.nlink));
  set_int("st_uid", int32_t(st.uid));
  set_int("st_gid", int32_t(st.gid));
  set_long("st_size", int64_t(st.size));
  for (const char* t : {"st_atime", "st_mtime", "st_ctime"}) {
    set_long((std::string(t) + "_sec").c_str(), st.mtime);
    set_long((std::string(t) + "_nsec").c_str(), 0);
  }
}

// --- Directory streams (opendir/readdir/closedir) -------------------------------------

// A snapshot of the directory taken when it is opened (Vfs::list), read in order.
struct DirStream {
  std::mutex lock;
  std::vector<std::string> names;
  size_t next = 0;
  int32_t fd = -1;  // descriptor handed to fdopendir, closed by closedir
};

constexpr size_t kMaxOpenDirs = 256;

HandleTable<DirStream>& dir_streams() {
  static HandleTable<DirStream> table(0x0b01);
  return table;
}

int64_t open_dir_stream(Runtime& rt, std::vector<std::string> names, int32_t fd) {
  static std::mutex open_lock;  // keeps the cap exact under concurrent opens
  std::unique_lock<std::mutex> guard(open_lock);
  if (dir_streams().live() >= kMaxOpenDirs) {
    // Never hold a host lock while Java code runs (the exception's
    // constructor): it may yield the GIL to a thread waiting on this lock.
    guard.unlock();
    throw_unix(rt, vfs::kEMFILE);
  }
  auto s = std::make_shared<DirStream>();
  s->names.reserve(names.size() + 2);
  s->names.push_back(".");
  s->names.push_back("..");
  for (auto& n : names) s->names.push_back(std::move(n));
  s->fd = fd;
  return dir_streams().add(std::move(s));
}

std::shared_ptr<DirStream> dir_stream(Runtime& rt, int64_t id) {
  auto s = dir_streams().get(id);
  if (!s) throw_unix(rt, vfs::kEBADF);
  return s;
}

// --- Text streams (fopen0, setmntent0) -------------------------------------------------

// A FILE* of the natives that only read (/proc/mounts, getline): the file's
// bytes when opened and a position, behind a bounded id.
struct TextStream {
  std::mutex lock;
  std::vector<uint8_t> bytes;
  size_t pos = 0;
};

HandleTable<TextStream>& text_streams() {
  static HandleTable<TextStream> table(0x112C);
  return table;
}

int64_t open_text_stream(Runtime& rt, const std::string& path) {
  auto s = std::make_shared<TextStream>();
  s->bytes = unix_call(rt, [&] { return the_vfs().read_file(path, 16u << 20); });
  return text_streams().add(std::move(s));
}

std::shared_ptr<TextStream> text_stream(Runtime& rt, int64_t id) {
  auto s = text_streams().get(id);
  if (!s) throw_unix(rt, vfs::kEBADF);
  return s;
}

// The path of an *at call: `address` itself if absolute, else an entry of
// the directory `dfd` (AT_FDCWD -100: the current directory, "/").
std::string at_path(Runtime& rt, int32_t dfd, int64_t address) {
  std::string path = path_from_address(rt, address);
  if (!path.empty() && path[0] == '/') return path;
  constexpr int32_t kAtFdcwd = -100;
  if (dfd == kAtFdcwd) return "/" + path;
  if (dfd < 0 || std_fd(dfd)) throw_unix(rt, std_fd(dfd) ? vfs::kENOTDIR : vfs::kEBADF);
  std::string dir = unix_call(rt, [&] {
    if (the_vfs().fstat(dfd).type != vfs::NodeType::kDirectory) throw vfs::VfsError{vfs::kENOTDIR, "openat"};
    return the_vfs().fd_path(dfd);
  });
  return dir == "/" ? "/" + path : dir + "/" + path;
}

void register_unix_native_dispatcher(rt::NativeRegistry& r) {
  const char* und = "Lsun/nio/fs/UnixNativeDispatcher;";
  // Capabilities: no openat()/futimes()/birth time, so the portable code paths are used.
  // Capabilities, as bionic gives them on Android 17: openat64, fstatat64,
  // unlinkat, renameat, futimesat and fdopendir exist (SUPPORTS_OPENAT 2,
  // SUPPORTS_FUTIMES 4); no birth time.
  r.bind(und, "init", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(2 | 4); });

  for (bool follow : {true, false}) {
    r.bind(und, follow ? "stat0" : "lstat0", "(JLsun/nio/fs/UnixFileAttributes;)V",
           [follow](Runtime& rt, NativeArgs& a) -> uint64_t {
             std::string path = path_from_address(rt, a.i64(0));
             set_unix_attributes(rt, a.h(2), unix_call(rt, [&] { return the_vfs().stat(path, follow); }));
             return 0;
           });
  }
  r.bind(und, "fstat", "(ILsun/nio/fs/UnixFileAttributes;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    set_unix_attributes(rt, a.h(1), unix_call(rt, [&] { return fstat_fd(fd); }));
    return 0;
  });
  // stat1: st_mode, or 0 when the file cannot be stat'ed (never throws).
  r.bind(und, "stat1", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    try {
      return rt::ret_int(int32_t(the_vfs().stat(path).mode));
    } catch (const vfs::VfsError&) {
      return 0;
    }
  });
  r.bind(und, "exists0", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(the_vfs().exists(path_from_address(rt, a.i64(0))));
  });

  r.bind(und, "open0", "(JII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    return rt::ret_int(unix_call(rt, [&] { return the_vfs().open(path, a.i32(2), uint32_t(a.i32(3))); }));
  }, S::kPartial, "système de fichiers virtuel (ADR-005)");
  r.bind(und, "close0", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (!std_fd(fd)) unix_call(rt, [&] { the_vfs().close(fd); });  // the host's standard streams stay open
    return 0;
  });
  // read/write return the raw count (0 at the end of the file, as read(2)).
  r.bind(und, "read", "(IJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), true);
    if (buf.empty()) return 0;
    return rt::ret_int(int32_t(unix_call(rt, [&] { return read_fd(fd, buf); })));
  });
  r.bind(und, "write", "(IJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), false);
    return rt::ret_int(int32_t(unix_call(rt, [&] { return write_fd(fd, buf); })));
  });

  r.bind(und, "opendir0", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    auto names = unix_call(rt, [&] { return the_vfs().list(path); });
    return rt::ret_long(open_dir_stream(rt, std::move(names), -1));
  }, S::kPartial, "instantané du répertoire à l'ouverture");
  r.bind(und, "fdopendir", "(I)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (std_fd(fd)) throw_unix(rt, vfs::kENOTDIR);
    auto names = unix_call(rt, [&] {
      if (the_vfs().fstat(fd).type != vfs::NodeType::kDirectory) throw vfs::VfsError{vfs::kENOTDIR, "fdopendir"};
      return the_vfs().list(the_vfs().fd_path(fd));
    });
    return rt::ret_long(open_dir_stream(rt, std::move(names), fd));
  }, S::kPartial, "instantané du répertoire à l'ouverture");
  // readdir: the next name ("." and ".." included, as readdir(3)), null at the end.
  r.bind(und, "readdir", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = dir_stream(rt, a.i64(0));
    std::string name;
    {
      std::lock_guard<std::mutex> guard(s->lock);
      if (s->next >= s->names.size()) return kNull;
      name = s->names[s->next++];
    }
    return make_byte_array(rt, name);
  });
  r.bind(und, "closedir", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(0);
    auto s = dir_stream(rt, id);
    dir_streams().remove(id);
    if (s->fd >= 0) unix_call(rt, [&] { the_vfs().close(s->fd); });
    return 0;
  });

  r.bind(und, "mkdir0", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    unix_call(rt, [&] { the_vfs().mkdir(path, uint32_t(a.i32(2))); });
    return 0;
  });
  r.bind(und, "rmdir0", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    unix_call(rt, [&] { the_vfs().rmdir(path); });
    return 0;
  });
  r.bind(und, "unlink0", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    unix_call(rt, [&] { the_vfs().unlink(path); });
    return 0;
  });
  r.bind(und, "rename0", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string from = path_from_address(rt, a.i64(0)), to = path_from_address(rt, a.i64(2));
    unix_call(rt, [&] { the_vfs().rename(from, to); });
    return 0;
  });
  r.bind(und, "readlink0", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    return make_byte_array(rt, unix_call(rt, [&] { return the_vfs().readlink(path); }));
  });
  r.bind(und, "realpath0", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    return make_byte_array(rt, unix_call(rt, [&] {
      std::string p = the_vfs().resolve(path);
      the_vfs().stat(p);
      return p;
    }));
  });
  r.bind(und, "access0", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    int32_t mode = a.i32(2);
    unix_call(rt, [&] {
      the_vfs().stat(path);
      if ((mode & 2 /* W_OK */) && !the_vfs().writable(path)) throw vfs::VfsError{vfs::kEROFS, path};
    });
    return 0;
  });
  // The *at variants: a relative path names an entry of the directory `dfd`
  // (AT_FDCWD: the current directory, "/").
  r.bind(und, "dup", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (std_fd(fd)) return rt::ret_int(fd);  // the host's standard streams stay shared
    return rt::ret_int(unix_call(rt, [&] { return the_vfs().dup(fd); }));
  });
  r.bind(und, "openat0", "(IJII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = at_path(rt, a.i32(0), a.i64(1));
    return rt::ret_int(unix_call(rt, [&] { return the_vfs().open(path, a.i32(3), uint32_t(a.i32(4))); }));
  });
  r.bind(und, "fstatat0", "(IJILsun/nio/fs/UnixFileAttributes;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = at_path(rt, a.i32(0), a.i64(1));
    constexpr int32_t kAtSymlinkNofollow = 0x100;
    bool follow = (a.i32(3) & kAtSymlinkNofollow) == 0;
    set_unix_attributes(rt, a.h(4), unix_call(rt, [&] { return the_vfs().stat(path, follow); }));
    return 0;
  });
  r.bind(und, "unlinkat0", "(IJI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = at_path(rt, a.i32(0), a.i64(1));
    constexpr int32_t kAtRemovedir = 0x200;
    unix_call(rt, [&] { (a.i32(3) & kAtRemovedir) ? the_vfs().rmdir(path) : the_vfs().unlink(path); });
    return 0;
  });
  r.bind(und, "renameat0", "(IJIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string from = at_path(rt, a.i32(0), a.i64(1)), to = at_path(rt, a.i32(3), a.i64(4));
    unix_call(rt, [&] { the_vfs().rename(from, to); });
    return 0;
  });
  // utimes, futimes (microseconds): the Vfs keeps whole seconds of the
  // modification time only.
  r.bind(und, "utimes0", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    unix_call(rt, [&] { the_vfs().set_mtime(path, a.i64(4) / 1000000); });
    return 0;
  }, S::kPartial, "date de modification à la seconde");
  r.bind(und, "futimes", "(IJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (std_fd(fd)) throw_unix(rt, vfs::kEPERM);
    unix_call(rt, [&] { the_vfs().set_mtime(the_vfs().fd_path(fd), a.i64(3) / 1000000); });
    return 0;
  }, S::kPartial, "date de modification à la seconde");
  // Permissions and owners stay fixed in the Vfs, as with Linux.chmod and
  // chown; the file must exist. Links are refused as by Linux.link and symlink.
  for (const char* name : {"chmod0", "chown0", "lchown0"}) {
    r.bind(und, name, std::string_view(name) == "chmod0" ? "(JI)V" : "(JII)V", [name](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::string path = path_from_address(rt, a.i64(0));
      unix_call(rt, [&] { the_vfs().stat(path, std::string_view(name) != "lchown0"); });
      return 0;
    }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  }
  for (const char* name : {"fchmod", "fchown"}) {
    r.bind(und, name, std::string_view(name) == "fchmod" ? "(II)V" : "(III)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      int32_t fd = a.i32(0);
      if (!std_fd(fd)) unix_call(rt, [&] { return the_vfs().fstat(fd); });
      return 0;
    }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  }
  // No `link` permission for applications in Android's SELinux policy.
  r.bind(und, "link0", "(JJ)V", [](Runtime& rt, NativeArgs&) -> uint64_t { throw_unix(rt, vfs::kEACCES); });
  r.bind(und, "symlink0", "(JJ)V", [](Runtime& rt, NativeArgs&) -> uint64_t { throw_unix(rt, vfs::kEPERM); },
         S::kUnsupported, "aucun lien créé par l'application (bac à sable)");
  // mknod: a regular file (S_IFREG or no type), as open(O_CREAT|O_EXCL);
  // devices need CAP_MKNOD, FIFOs a node type the Vfs lacks.
  r.bind(und, "mknod0", "(JIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    uint32_t mode = uint32_t(a.i32(2)), type = mode & 0170000;
    if (type != 0 && type != 0100000) throw_unix(rt, vfs::kEPERM);
    unix_call(rt, [&] {
      int32_t fd = the_vfs().open(path, vfs::kO_WRONLY | vfs::kO_CREAT | vfs::kO_EXCL, mode & 07777);
      the_vfs().close(fd);
    });
    return 0;
  }, S::kPartial, "fichiers ordinaires seulement");
  r.bind(und, "statvfs0", "(JLsun/nio/fs/UnixFileStoreAttributes;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    unix_call(rt, [&] { return the_vfs().stat(path); });
    Handle attrs = a.h(2);
    rt::Object& o = rt.nonnull(attrs, "UnixFileStoreAttributes");
    for (auto [name, value] : {std::pair{"f_frsize", int64_t(4096)}, std::pair{"f_blocks", int64_t(1) << 22},
                               std::pair{"f_bfree", int64_t(1) << 21}, std::pair{"f_bavail", int64_t(1) << 21}}) {
      rt::Field* f = find_field(o.klass, name, "J");
      if (!f) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
      o.fields.at(f->slot) = uint32_t(uint64_t(value));
      o.fields.at(f->slot + 1) = uint32_t(uint64_t(value) >> 32);
    }
    return 0;
  }, S::kPartial, "valeurs de capacité fixes, comme Linux.statvfs");
  // pathconf of bionic on ext4: _PC_LINK_MAX 65000, _PC_NAME_MAX 255,
  // _PC_PATH_MAX 4096, _PC_PIPE_BUF 4096; other names EINVAL.
  auto path_limit = [](Runtime& rt, int32_t name) -> int64_t {
    switch (name) {
      case 0: return 65000;
      case 3: return 255;
      case 4: case 5: return 4096;
      default: throw_unix(rt, vfs::kEINVAL);
    }
  };
  r.bind(und, "pathconf0", "(JI)J", [path_limit](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    unix_call(rt, [&] { return the_vfs().stat(path); });
    return rt::ret_long(path_limit(rt, a.i32(2)));
  });
  r.bind(und, "fpathconf", "(II)J", [path_limit](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (!std_fd(fd)) unix_call(rt, [&] { return the_vfs().fstat(fd); });
    return rt::ret_long(path_limit(rt, a.i32(1)));
  });
  // Users and groups by bionic's databases.
  r.bind(und, "getpwuid", "(I)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto name = android_user_name(uint32_t(a.i32(0)));
    if (!name) throw_unix(rt, vfs::kENOENT);
    return make_byte_array(rt, *name);
  });
  r.bind(und, "getgrgid", "(I)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto name = android_group_name(uint32_t(a.i32(0)));
    if (!name) throw_unix(rt, vfs::kENOENT);
    return make_byte_array(rt, *name);
  });
  r.bind(und, "getpwnam0", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto id = android_user_id(native_cstring(rt, a.i64(0)));
    return rt::ret_int(id ? int32_t(*id) : -1);
  });
  r.bind(und, "getgrnam0", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto id = android_group_id(native_cstring(rt, a.i64(0)));
    return rt::ret_int(id ? int32_t(*id) : -1);
  });
  // Read-only text streams (fopen, getline): a snapshot of the file's lines.
  r.bind(und, "fopen0", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0)), mode = native_cstring(rt, a.i64(2));
    if (mode.empty() || mode[0] != 'r' || mode.find('+') != std::string::npos) throw_unix(rt, vfs::kEINVAL);  // read-only use
    return rt::ret_long(open_text_stream(rt, path));
  }, S::kPartial, "flux en lecture seule");
  r.bind(und, "fclose", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    text_stream(rt, a.i64(0));
    text_streams().remove(a.i64(0));
    return 0;
  });
  r.bind(und, "rewind", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = text_stream(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    s->pos = 0;
    return 0;
  });
  // getlinelen: the length of the next line, its '\n' included, -1 at the end.
  r.bind(und, "getlinelen", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = text_stream(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    if (s->pos >= s->bytes.size()) return rt::ret_int(-1);
    size_t end = s->pos;
    while (end < s->bytes.size() && s->bytes[end] != '\n') ++end;
    if (end < s->bytes.size()) ++end;
    int32_t length = int32_t(end - s->pos);
    s->pos = end;
    return rt::ret_int(length);
  });

  r.bind(und, "getcwd", "()[B", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return make_byte_array(rt, "/");
  }, S::kPartial, "dossier courant fixe");
  r.bind(und, "strerror", "(I)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return make_byte_array(rt, strerror_text(a.i32(0)));
  });
}

// --- Channels -----------------------------------------------------------------------------

void register_file_dispatcher(rt::NativeRegistry& r) {
  const char* fdi = "Lsun/nio/ch/FileDispatcherImpl;";  // static natives
  // read0/pread0 return IOStatus.EOF (-1) at the end of the file, as the real
  // natives. Sockets and pipes (SocketDispatcher, Pipe channels) go to
  // libcore_net.cpp, which blocks or not as their O_NONBLOCK says.
  r.bind(fdi, "read0", "(Ljava/io/FileDescriptor;JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), true);
    std::span<uint8_t> one[] = {buf};
    if (auto n = nio_socket_io(rt, fd, one, true)) return rt::ret_int(int32_t(*n));
    if (buf.empty()) return 0;
    size_t n = io_call(rt, [&] { return read_fd(fd, buf); });
    return rt::ret_int(n == 0 ? kIosEof : int32_t(n));
  }, S::kPartial, "système de fichiers virtuel (ADR-005)");
  r.bind(fdi, "pread0", "(Ljava/io/FileDescriptor;JIJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), true);
    int64_t off = a.i64(4);
    if (off < 0) throw_io(rt, vfs::kEINVAL);
    if (std_fd(fd)) throw_io(rt, vfs::kESPIPE);
    if (buf.empty()) return 0;
    size_t n = io_call(rt, [&] { return the_vfs().pread(fd, buf, uint64_t(off)); });
    return rt::ret_int(n == 0 ? kIosEof : int32_t(n));
  });
  r.bind(fdi, "readv0", "(Ljava/io/FileDescriptor;JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    int64_t address = a.i64(1);
    int32_t len = a.i32(3);
    if (len < 0) throw_io(rt, vfs::kEINVAL);
    if (len == 0) return rt::ret_long(0);
    if (auto n = nio_socket_io(rt, fd, native_iovecs(rt, address, len, true), true)) return rt::ret_long(*n);
    auto iov_span = native_span(rt, address, uint64_t(len) * 16, false);
    int64_t total = 0;
    bool any_attempted = false;
    for (int32_t i = 0; i < len; ++i) {
      uint64_t iov_base = 0, iov_len = 0;
      std::memcpy(&iov_base, iov_span.data() + size_t(i) * 16, 8);
      std::memcpy(&iov_len, iov_span.data() + size_t(i) * 16 + 8, 8);
      if (iov_len == 0) continue;
      auto buf = buffer_from_address(rt, int64_t(iov_base), int32_t(iov_len), true);
      if (buf.empty()) continue;
      any_attempted = true;
      size_t n = io_call(rt, [&] { return read_fd(fd, buf); });
      total += int64_t(n);
      if (n < buf.size()) break;
    }
    if (total == 0 && any_attempted) return rt::ret_long(kIosEof);
    return rt::ret_long(total);
  });
  r.bind(fdi, "write0", "(Ljava/io/FileDescriptor;JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), false);
    std::span<uint8_t> one[] = {buf};
    if (auto n = nio_socket_io(rt, fd, one, false)) return rt::ret_int(int32_t(*n));
    return rt::ret_int(int32_t(io_call(rt, [&] { return write_fd(fd, buf); })));
  }, S::kPartial, "sorties standard, système de fichiers virtuel et sockets");
  r.bind(fdi, "writev0", "(Ljava/io/FileDescriptor;JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    int64_t address = a.i64(1);
    int32_t len = a.i32(3);
    if (len < 0) throw_io(rt, vfs::kEINVAL);
    if (len == 0) return rt::ret_long(0);
    if (auto n = nio_socket_io(rt, fd, native_iovecs(rt, address, len, false), false)) return rt::ret_long(*n);
    auto iov_span = native_span(rt, address, uint64_t(len) * 16, false);
    int64_t total = 0;
    for (int32_t i = 0; i < len; ++i) {
      uint64_t iov_base = 0, iov_len = 0;
      std::memcpy(&iov_base, iov_span.data() + size_t(i) * 16, 8);
      std::memcpy(&iov_len, iov_span.data() + size_t(i) * 16 + 8, 8);
      if (iov_len == 0) continue;
      auto buf = buffer_from_address(rt, int64_t(iov_base), int32_t(iov_len), false);
      if (buf.empty()) continue;
      size_t n = io_call(rt, [&] { return write_fd(fd, buf); });
      total += int64_t(n);
      if (n < buf.size()) break;
    }
    return rt::ret_long(total);
  });
  r.bind(fdi, "pwrite0", "(Ljava/io/FileDescriptor;JIJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), false);
    int64_t off = a.i64(4);
    if (off < 0) throw_io(rt, vfs::kEINVAL);
    if (std_fd(fd)) throw_io(rt, vfs::kESPIPE);
    return rt::ret_int(int32_t(io_call(rt, [&] { return the_vfs().pwrite(fd, buf, uint64_t(off)); })));
  });
  r.bind(fdi, "close0", "(Ljava/io/FileDescriptor;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    if (fd != -1 && !std_fd(fd)) io_call(rt, [&] { the_vfs().close(fd); });  // standard streams stay open
    return 0;
  });
  r.bind(fdi, "size0", "(Ljava/io/FileDescriptor;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    return rt::ret_long(int64_t(io_call(rt, [&] { return fstat_fd(fd).size; })));
  });
  r.bind(fdi, "truncate0", "(Ljava/io/FileDescriptor;J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    int64_t size = a.i64(1);
    if (size < 0 || std_fd(fd)) throw_io(rt, vfs::kEINVAL);
    io_call(rt, [&] { the_vfs().ftruncate(fd, uint64_t(size)); });
    return 0;
  });
  r.bind(fdi, "force0", "(Ljava/io/FileDescriptor;Z)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    if (fd == 1 || fd == 2) {
      std::fflush(fd == 1 ? stdout : stderr);
      return 0;
    }
    if (fd == 0) throw_io(rt, vfs::kEINVAL);
    io_call(rt, [&] { the_vfs().fsync(fd); });
    return 0;
  });
  r.bind(fdi, "lock0", "(Ljava/io/FileDescriptor;ZJJZ)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t fd = fd_of(rt, a.h(0));
           if (fd > 2) io_call(rt, [&] { return fstat_fd(fd); });
           return 0; // FileDispatcher.LOCKED
         }, S::kStub, "verrou de canal exclusif simulé");
  r.bind(fdi, "release0", "(Ljava/io/FileDescriptor;JJ)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t fd = fd_of(rt, a.h(0));
           if (fd > 2) io_call(rt, [&] { return fstat_fd(fd); });
           return 0;
         }, S::kStub, "libération de verrou de canal");

  const char* fci = "Lsun/nio/ch/FileChannelImpl;";
  // initIDs: the allocation granularity of mappings (the page size).
  r.bind(fci, "initIDs", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(kPageSize); });
  // position0(fd, offset), instance method: a negative offset queries the position.
  r.bind(fci, "position0", "(Ljava/io/FileDescriptor;J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    int64_t offset = a.i64(2);
    if (std_fd(fd)) throw_io(rt, vfs::kESPIPE);
    return rt::ret_long(io_call(rt, [&] {
      return offset < 0 ? the_vfs().lseek(fd, 0, kSeekCur) : the_vfs().lseek(fd, offset, kSeekSet);
    }));
  });
  // map0(prot, position, length), instance method on the channel's `fd`: a
  // private copy in emulated native memory (MAP_SHARED is not emulated).
  r.bind(fci, "map0", "(IJJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt::Object& self = a.self();
    rt::Field* f = find_field(self.klass, "fd", "Ljava/io/FileDescriptor;");
    if (!f || f->is_static()) rt.throw_new("Ljava/lang/InternalError;", "FileChannelImpl sans champ fd");
    int32_t fd = fd_of(rt, Handle(self.fields.at(f->slot)));
    constexpr int32_t kMapReadOnly = 0;  // FileChannelImpl.MAP_RO (MAP_RW = 1, MAP_PV = 2)
    bool writable = a.i32(1) != kMapReadOnly;
    int64_t position = a.i64(2), length = a.i64(4);
    if (position < 0 || length <= 0) throw_io(rt, vfs::kEINVAL);
    try {
      return rt::ret_long(map_fd_copy(rt, fd, uint64_t(position), uint64_t(length), writable));
    } catch (const vfs::VfsError& e) {
      if (e.err == vfs::kENOMEM) rt.throw_new("Ljava/lang/OutOfMemoryError;", "Map failed");
      throw_io(rt, e.err);
    }
  }, S::kPartial, "copie privée du fichier (MAP_SHARED non émulé)");
  // unmap0(address, length), static: IOException (EINVAL) unless `address` is
  // the start of a mapping, released whole, as munmap(2) seen by the real native.
  r.bind(fci, "unmap0", "(JJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.i64(2) <= 0 || !native_try_free(a.i64(0))) throw_io(rt, vfs::kEINVAL);
    return 0;
  });
}

// --- sun.nio.fs.LinuxNativeDispatcher, UnixCopyFile; FileKey, MappedByteBuffer,
// FileChannelImpl.transferTo0, java.util.prefs.FileSystemPreferences ----------------------

// getmntent: the next line of a mount table, "fsname dir type opts freq
// passno", its fields unescaped (\040 is a space), comments skipped.
bool next_mount_entry(TextStream& s, std::vector<std::string>& fields) {
  while (s.pos < s.bytes.size()) {
    size_t end = s.pos;
    while (end < s.bytes.size() && s.bytes[end] != '\n') ++end;
    std::string line(s.bytes.begin() + ptrdiff_t(s.pos), s.bytes.begin() + ptrdiff_t(end));
    s.pos = end < s.bytes.size() ? end + 1 : end;
    fields.clear();
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      if (i >= line.size()) break;
      std::string field;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
        if (line[i] == '\\' && i + 3 < line.size() && line[i + 1] >= '0' && line[i + 1] <= '3') {
          field += char((line[i + 1] - '0') * 64 + (line[i + 2] - '0') * 8 + (line[i + 3] - '0'));
          i += 4;
        } else {
          field += line[i++];
        }
      }
      fields.push_back(std::move(field));
    }
    if (fields.empty() || fields[0][0] == '#') continue;
    while (fields.size() < 4) fields.emplace_back();
    return true;
  }
  return false;
}

void register_nio_misc(rt::NativeRegistry& r) {
  const char* lnd = "Lsun/nio/fs/LinuxNativeDispatcher;";
  r.bind(lnd, "init", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  // Extended attributes: the host areas keep none, as Linux.getxattr and setxattr say.
  r.bind(lnd, "fgetxattr0", "(IJJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (!std_fd(fd)) unix_call(rt, [&] { return the_vfs().fstat(fd); });
    throw_unix(rt, vfs::kENODATA);
  });
  r.bind(lnd, "flistxattr", "(IJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (!std_fd(fd)) unix_call(rt, [&] { return the_vfs().fstat(fd); });
    return 0;  // an empty list
  });
  r.bind(lnd, "fsetxattr0", "(IJJI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (!std_fd(fd)) unix_call(rt, [&] { return the_vfs().fstat(fd); });
    throw_unix(rt, vfs::kEOPNOTSUPP);
  }, S::kUnsupported, "pas d'attributs étendus dans le bac à sable (EOPNOTSUPP)");
  r.bind(lnd, "fremovexattr0", "(IJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (!std_fd(fd)) unix_call(rt, [&] { return the_vfs().fstat(fd); });
    throw_unix(rt, vfs::kENODATA);
  });
  r.bind(lnd, "setmntent0", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    return rt::ret_long(open_text_stream(rt, path));
  });
  // getmntent0(stream, entry, buffer, bufLen): 0 and the entry's byte arrays
  // set, -1 at the end.
  r.bind(lnd, "getmntent0", "(JLsun/nio/fs/UnixMountEntry;JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = text_stream(rt, a.i64(0));
    std::vector<std::string> fields;
    {
      std::lock_guard<std::mutex> guard(s->lock);
      if (!next_mount_entry(*s, fields)) return rt::ret_int(-1);
    }
    Handle entry = a.h(2);
    rt::Object& o = rt.nonnull(entry, "UnixMountEntry");
    const char* names[] = {"name", "dir", "fstype", "opts"};
    for (int i = 0; i < 4; ++i) {
      rt::Field* f = find_field(o.klass, names[i], "[B");
      if (!f) rt.throw_new("Ljava/lang/NoSuchFieldError;", names[i]);
      Handle bytes = make_byte_array(rt, fields[size_t(i)]);
      rt.object(entry)->fields.at(f->slot) = bytes;
    }
    return 0;
  });
  r.bind(lnd, "endmntent", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    text_streams().remove(a.i64(0));
    return 0;
  });

  // UnixCopyFile.transfer(dst, src, cancelAddress): 8 KiB at a time, until the
  // end, ECANCELED once the int at cancelAddress is set.
  r.bind("Lsun/nio/fs/UnixCopyFile;", "transfer", "(IIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t dst = a.i32(0), src = a.i32(1);
    int64_t cancel = a.i64(2);
    std::vector<uint8_t> buf(8192);
    for (;;) {
      size_t n = unix_call(rt, [&] { return read_fd(src, buf); });
      if (n == 0) return 0;
      if (cancel != 0) {
        int32_t flag = 0;
        std::memcpy(&flag, native_span(rt, cancel, 4, false).data(), 4);
        if (flag != 0) throw_unix(rt, 125);  // ECANCELED
      }
      size_t done = 0;
      while (done < n) done += unix_call(rt, [&] { return write_fd(dst, std::span<const uint8_t>(buf.data() + done, n - done)); });
    }
  });

  // FileKey.init: st_dev and st_ino of the descriptor's file.
  r.bind("Lsun/nio/ch/FileKey;", "init", "(Ljava/io/FileDescriptor;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    vfs::VStat st = io_call(rt, [&] { return fstat_fd(fd_of(rt, a.h(1))); });
    rt::Object& self = a.self();
    for (auto [name, value] : {std::pair{"st_dev", int64_t(st.dev ? st.dev : 0xFD00)}, std::pair{"st_ino", int64_t(st.ino)}}) {
      rt::Field* f = find_field(self.klass, name, "J");
      if (!f) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
      self.fields.at(f->slot) = uint32_t(uint64_t(value));
      self.fields.at(f->slot + 1) = uint32_t(uint64_t(value) >> 32);
    }
    return 0;
  });

  // MappedByteBuffer: mappings are private copies always resident; the range
  // must still be mapped (mincore, madvise, msync: ENOMEM).
  const char* mbb = "Ljava/nio/MappedByteBuffer;";  // instance natives
  auto check_mapping = [](Runtime& rt, int64_t address, int64_t length) {
    if (length > 0 && !native_range_mapped(address, uint64_t(length))) throw_io(rt, vfs::kENOMEM);
  };
  r.bind(mbb, "isLoaded0", "(JJJ)Z", [check_mapping](Runtime& rt, NativeArgs& a) -> uint64_t {
    check_mapping(rt, a.i64(1), a.i64(3));
    return 1;
  });
  r.bind(mbb, "load0", "(JJ)V", [check_mapping](Runtime& rt, NativeArgs& a) -> uint64_t {
    check_mapping(rt, a.i64(1), a.i64(3));
    return 0;
  });
  r.bind(mbb, "force0", "(Ljava/io/FileDescriptor;JJ)V", [check_mapping](Runtime& rt, NativeArgs& a) -> uint64_t {
    check_mapping(rt, a.i64(2), a.i64(4));
    return 0;
  }, S::kPartial, "copie privée : rien à écrire dans le fichier");

  // transferTo0: sendfile64 is not emulated here, which the real native
  // reports as IOStatus.UNSUPPORTED_CASE (EINVAL): FileChannelImpl then
  // copies through a mapping or a buffer.
  r.bind("Lsun/nio/ch/FileChannelImpl;", "transferTo0", "(Ljava/io/FileDescriptor;JJLjava/io/FileDescriptor;)J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(-6); }, S::kPartial, "repli Java (UNSUPPORTED_CASE)");

  // FileSystemPreferences: locks of the Vfs are not enforced (as
  // FileDispatcherImpl.lock0), so lockFile0 opens the file and succeeds.
  const char* fsp = "Ljava/util/prefs/FileSystemPreferences;";
  r.bind(fsp, "chmod", "(Ljava/lang/String;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe({});
    std::string path = rt.string_utf8(a.h(0));
    try {
      the_vfs().stat(path);
      return 0;
    } catch (const vfs::VfsError& e) {
      return rt::ret_int(e.err);
    }
  }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  r.bind(fsp, "lockFile0", "(Ljava/lang/String;IZ)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe({});
    std::string path = rt.string_utf8(a.h(0));
    bool shared = a.i32(2) != 0;
    int32_t result[2] = {0, 0};
    try {
      result[0] = shared ? the_vfs().open(path, vfs::kO_RDONLY, 0)
                         : the_vfs().open(path, vfs::kO_WRONLY | vfs::kO_CREAT, uint32_t(a.i32(1)));
    } catch (const vfs::VfsError& e) {
      result[0] = 0;
      result[1] = e.err;
    }
    Handle array = rt.new_array("[I", 2);
    std::memcpy(rt.array(array, "int[]").data.data(), result, sizeof result);
    return array;
  }, S::kPartial, "verrou non appliqué entre processus");
  r.bind(fsp, "unlockFile0", "(I)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    try {
      the_vfs().close(a.i32(0));
      return 0;
    } catch (const vfs::VfsError& e) {
      return rt::ret_int(e.err);
    }
  });
}

void register_ch_misc(rt::NativeRegistry& r) {
  r.bind("Lsun/nio/ch/IOUtil;", "iovMax", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 1024; });
  // NativeThread, IOUtil's descriptors and pipes: libcore_net.cpp.
}

}  // namespace

void register_libcore_nio(rt::NativeRegistry& r) {
  register_unix_native_dispatcher(r);
  register_file_dispatcher(r);
  register_ch_misc(r);
  register_nio_misc(r);
}

}  // namespace aa::bridge
