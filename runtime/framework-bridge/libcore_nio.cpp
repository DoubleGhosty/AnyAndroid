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

void register_unix_native_dispatcher(rt::NativeRegistry& r) {
  const char* und = "Lsun/nio/fs/UnixNativeDispatcher;";
  // Capabilities: no openat()/futimes()/birth time, so the portable code paths are used.
  r.bind(und, "init", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

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
  // read0/pread0 return IOStatus.EOF (-1) at the end of the file, as the real natives.
  r.bind(fdi, "read0", "(Ljava/io/FileDescriptor;JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3), true);
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
    return rt::ret_int(int32_t(io_call(rt, [&] { return write_fd(fd, buf); })));
  }, S::kPartial, "sorties standard et système de fichiers virtuel");
  r.bind(fdi, "writev0", "(Ljava/io/FileDescriptor;JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    int64_t address = a.i64(1);
    int32_t len = a.i32(3);
    if (len < 0) throw_io(rt, vfs::kEINVAL);
    if (len == 0) return rt::ret_long(0);
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

void register_ch_misc(rt::NativeRegistry& r) {
  r.bind("Lsun/nio/ch/IOUtil;", "iovMax", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 1024; });
  r.bind("Lsun/nio/ch/NativeThread;", "current", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return 1; }, S::kStub);
  r.bind("Lsun/nio/ch/NativeThread;", "signal", "(J)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kStub);
}

}  // namespace

void register_libcore_nio(rt::NativeRegistry& r) {
  register_unix_native_dispatcher(r);
  register_file_dispatcher(r);
  register_ch_misc(r);
}

}  // namespace aa::bridge
