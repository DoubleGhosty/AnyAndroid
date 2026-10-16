// File natives of libcore (libcore.io.Linux, java.io.UnixFileSystem) on top
// of the virtual file system of the emulated process (ADR-005). Descriptors
// 0-2 are the host's standard streams; every other descriptor and every path
// goes through the Vfs: the code never reaches a host path directly.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <new>
#include <vector>

#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"
#include "vfs/vfs.h"

namespace aa::bridge {

vfs::Vfs& libcore_vfs() {
  auto& cfg = libcore_config();
  if (!cfg.vfs) {
    // Without a configured layout, only /dev exists.
    cfg.vfs = std::make_shared<vfs::Vfs>();
    cfg.vfs->mount("/dev", vfs::dev_mount());
  }
  return *cfg.vfs;
}

namespace {

FILE* std_stream(int32_t fd) { return fd == 1 ? stdout : fd == 2 ? stderr : nullptr; }

constexpr uint64_t kMaxMapping = uint64_t(2) << 30;

// Zeroed bytes for a mapping of `length` bytes, or ENOMEM (size limit, budget
// of the emulated native memory, host memory) before anything is published.
std::vector<uint8_t> mapping_bytes(uint64_t length) {
  if (length > kMaxMapping || !native_memory_fits(length)) throw vfs::VfsError{vfs::kENOMEM, "mmap"};
  std::vector<uint8_t> data;
  try {
    data.assign(size_t(length), 0);
  } catch (const std::bad_alloc&) {
    throw vfs::VfsError{vfs::kENOMEM, "mmap"};
  }
  return data;
}

}  // namespace

size_t read_fd(int32_t fd, std::span<uint8_t> out) {
  if (fd == 0) return std::fread(out.data(), 1, out.size(), stdin);  // console programs (blocks this thread)
  if (fd == 1 || fd == 2) throw vfs::VfsError{vfs::kEBADF, "read"};
  return libcore_vfs().read(fd, out);
}

size_t write_fd(int32_t fd, std::span<const uint8_t> data) {
  if (FILE* out = std_stream(fd)) {
    size_t n = std::fwrite(data.data(), 1, data.size(), out);
    std::fflush(out);
    return n;
  }
  if (fd == 0) throw vfs::VfsError{vfs::kEBADF, "write"};
  return libcore_vfs().write(fd, data);
}

vfs::VStat fstat_fd(int32_t fd) {
  if (fd >= 0 && fd <= 2) {  // the host's standard streams, seen as a terminal
    vfs::VStat st;
    st.type = vfs::NodeType::kCharDevice;
    st.mode = 020620;
    st.ino = uint64_t(fd) + 1;
    return st;
  }
  return libcore_vfs().fstat(fd);
}

int64_t map_fd_copy(rt::Runtime& rt, int32_t fd, uint64_t offset, uint64_t length, bool writable) {
  if (fd >= 0 && fd <= 2) throw vfs::VfsError{vfs::kEACCES, "mmap"};  // not a regular file
  libcore_vfs().fstat(fd);  // EBADF before allocating
  if ((libcore_vfs().open_flags(fd) & vfs::kO_ACCMODE) == vfs::kO_WRONLY) throw vfs::VfsError{vfs::kEACCES, "mmap"};
  // Past the end of the file the mapping reads as zeros.
  auto fill = [offset](vfs::OpenFile& file, std::vector<uint8_t>& data) {
    size_t done = 0;
    while (done < data.size()) {
      size_t n = file.read(offset + done, std::span<uint8_t>(data).subspan(done));
      if (n == 0) break;
      done += n;
    }
  };
  std::shared_ptr<vfs::OpenFile> file = libcore_vfs().open_file(fd);  // outlives a close of fd
  if (!writable) {
    if (length > kMaxMapping) throw vfs::VfsError{vfs::kENOMEM, "mmap"};
    return native_wrap_lazy(rt, length, [file, length, fill] {
      std::vector<uint8_t> data(size_t(length), 0);
      fill(*file, data);
      return data;
    });
  }
  std::vector<uint8_t> data = mapping_bytes(length);
  fill(*file, data);
  return native_wrap(rt, std::move(data), writable);
}

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

vfs::Vfs& the_vfs() { return libcore_vfs(); }

// Runs a Vfs operation, turning its errors into ErrnoException.
template <typename F>
auto guarded(Runtime& rt, const char* function, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const vfs::VfsError& e) {
    throw_errno(rt, function, e.err);
  }
}

std::string path_arg(Runtime& rt, Handle h) {
  if (h == kNull) rt.throw_npe("path == null");
  return rt.string_utf8(h);
}

Handle new_file_descriptor(Runtime& rt, int32_t fd) {
  Handle h = construct(rt, "Ljava/io/FileDescriptor;", "()V", {});
  rt::Field* f = find_field(rt.class_of(h), "descriptor", "I");
  if (!f) rt.throw_new("Ljava/lang/InternalError;", "FileDescriptor sans champ descriptor");
  rt.object(h)->fields.at(f->slot) = uint32_t(fd);
  return h;
}

// byte[] buffer, bounds-checked. Direct ByteBuffers are not supported yet.
std::span<uint8_t> byte_range(Runtime& rt, Handle buffer, int32_t offset, int32_t count) {
  rt::Object& o = rt.nonnull(buffer, "buffer");
  auto* arr = dynamic_cast<rt::ArrayObject*>(&o);
  if (!arr || arr->width != 1 || !arr->klass->component->primitive) {
    rt.throw_unsupported(o.klass->descriptor + " comme tampon d'E/S", "seuls les byte[] sont pris en charge");
  }
  if (offset < 0 || count < 0 || int64_t(offset) + count > arr->length) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "offset=" + std::to_string(offset) + " count=" +
                                                                   std::to_string(count) + " length=" +
                                                                   std::to_string(arr->length));
  }
  return std::span<uint8_t>(arr->data.data() + offset, size_t(count));
}

// Wide arguments for Java constructors.
void push_long(std::vector<uint32_t>& v, int64_t x) {
  v.push_back(uint32_t(uint64_t(x)));
  v.push_back(uint32_t(uint64_t(x) >> 32));
}

Handle struct_stat(Runtime& rt, const vfs::VStat& s) {
  std::vector<uint32_t> a;
  push_long(a, int64_t(s.dev ? s.dev : 0xFD00));  // st_dev
  push_long(a, int64_t(s.ino));                    // st_ino
  a.push_back(s.mode);                             // st_mode
  push_long(a, int64_t(s.nlink));                  // st_nlink
  a.push_back(s.uid);                              // st_uid
  a.push_back(s.gid);                              // st_gid
  push_long(a, 0);                                 // st_rdev
  push_long(a, int64_t(s.size));                   // st_size
  push_long(a, s.mtime);                           // st_atime
  push_long(a, s.mtime);                           // st_mtime
  push_long(a, s.mtime);                           // st_ctime
  push_long(a, 4096);                              // st_blksize
  push_long(a, int64_t((s.size + 511) / 512));     // st_blocks
  return construct(rt, "Landroid/system/StructStat;", "(JJIJIIJJJJJJJ)V", a);
}

Handle struct_statvfs(Runtime& rt, bool writable) {
  std::vector<uint32_t> a;
  for (int64_t v : {int64_t(4096), int64_t(4096), int64_t(1) << 22, int64_t(1) << 21, int64_t(1) << 21,
                    int64_t(1) << 20, int64_t(1) << 19, int64_t(1) << 19, int64_t(0),
                    int64_t(writable ? 0 : 1 /* ST_RDONLY */), int64_t(255)}) {
    push_long(a, v);
  }
  return construct(rt, "Landroid/system/StructStatVfs;", "(JJJJJJJJJJJ)V", a);
}

void register_linux(rt::NativeRegistry& r) {
  // IoBridge.closeAndSignalBlockedThreads(): wakes threads blocked in I/O on
  // the descriptor. Virtual files never block, so there is nobody to wake.
  r.bind("Llibcore/io/AsynchronousCloseMonitor;", "signalBlockedThreads", "(Ljava/io/FileDescriptor;)V",
         [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial, "aucune E/S bloquante sur les fichiers virtuels");
  const char* lx = "Llibcore/io/Linux;";  // instance methods: argument 0 is the Linux object
  r.bind(lx, "open", "(Ljava/lang/String;II)Ljava/io/FileDescriptor;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    int32_t fd = guarded(rt, "open", [&] { return the_vfs().open(path, a.i32(2), uint32_t(a.i32(3))); });
    return new_file_descriptor(rt, fd);
  }, S::kPartial, "système de fichiers virtuel (ADR-005)");
  r.bind(lx, "close", "(Ljava/io/FileDescriptor;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    if (fd >= 0 && fd <= 2) return 0;  // the host's standard streams stay open
    guarded(rt, "close", [&] { the_vfs().close(fd); });
    return 0;
  });
  r.bind(lx, "isatty", "(Ljava/io/FileDescriptor;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    return rt::ret_bool(fd >= 0 && fd <= 2 && _isatty(fd));
  });
  r.bind(lx, "readBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    auto buf = byte_range(rt, a.h(2), a.i32(3), a.i32(4));
    if (buf.empty()) return 0;
    // 0 at the end, as read(2); IoBridge maps it to -1.
    return rt::ret_int(int32_t(guarded(rt, "read", [&] { return read_fd(fd, buf); })));
  }, S::kPartial, "système de fichiers virtuel ; tampons byte[] uniquement");
  r.bind(lx, "readNoThrow", "(Ljava/io/FileDescriptor;[BII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    auto buf = byte_range(rt, a.h(2), a.i32(3), a.i32(4));
    try {
      return rt::ret_int(int32_t(read_fd(fd, buf)));
    } catch (const vfs::VfsError&) {
      return rt::ret_int(-1);
    }
  });
  r.bind(lx, "writeBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    auto buf = byte_range(rt, a.h(2), a.i32(3), a.i32(4));
    return rt::ret_int(int32_t(guarded(rt, "write", [&] { return write_fd(fd, buf); })));
  },S::kPartial, "sorties standard et système de fichiers virtuel ; tampons byte[] uniquement");
  r.bind(lx, "preadBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    auto buf = byte_range(rt, a.h(2), a.i32(3), a.i32(4));
    int64_t off = a.i64(5);
    if (off < 0) throw_errno(rt, "pread", vfs::kEINVAL);
    return rt::ret_int(int32_t(guarded(rt, "pread", [&] { return the_vfs().pread(fd, buf, uint64_t(off)); })));
  });
  r.bind(lx, "pwriteBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    auto buf = byte_range(rt, a.h(2), a.i32(3), a.i32(4));
    int64_t off = a.i64(5);
    if (off < 0) throw_errno(rt, "pwrite", vfs::kEINVAL);
    return rt::ret_int(int32_t(guarded(rt, "pwrite", [&] { return the_vfs().pwrite(fd, buf, uint64_t(off)); })));
  });
  r.bind(lx, "lseek", "(Ljava/io/FileDescriptor;JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    if (fd >= 0 && fd <= 2) throw_errno(rt, "lseek", vfs::kESPIPE);
    return rt::ret_long(guarded(rt, "lseek", [&] { return the_vfs().lseek(fd, a.i64(2), a.i32(4)); }));
  });
  r.bind(lx, "fstat", "(Ljava/io/FileDescriptor;)Landroid/system/StructStat;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    return struct_stat(rt, guarded(rt, "fstat", [&] { return fstat_fd(fd); }));
  });
  for (bool follow : {true, false}) {
    r.bind(lx, follow ? "stat" : "lstat", "(Ljava/lang/String;)Landroid/system/StructStat;",
           [follow](Runtime& rt, NativeArgs& a) -> uint64_t {
             std::string path = path_arg(rt, a.h(1));
             vfs::VStat st = guarded(rt, follow ? "stat" : "lstat", [&] { return the_vfs().stat(path, follow); });
             return struct_stat(rt, st);
           });
  }
  r.bind(lx, "access", "(Ljava/lang/String;I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    int32_t mode = a.i32(2);
    guarded(rt, "access", [&] {
      the_vfs().stat(path);
      if ((mode & 2 /* W_OK */) && !the_vfs().writable(path)) throw vfs::VfsError{vfs::kEROFS, path};
    });
    return 1;
  });
  r.bind(lx, "mkdir", "(Ljava/lang/String;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "mkdir", [&] { the_vfs().mkdir(path, uint32_t(a.i32(2))); });
    return 0;
  });
  r.bind(lx, "unlink", "(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "unlink", [&] { the_vfs().unlink(path); });
    return 0;
  });
  r.bind(lx, "remove", "(Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "remove", [&] {
      if (the_vfs().stat(path, false).type == vfs::NodeType::kDirectory) {
        the_vfs().rmdir(path);
      } else {
        the_vfs().unlink(path);
      }
    });
    return 0;
  });
  r.bind(lx, "rename", "(Ljava/lang/String;Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string from = path_arg(rt, a.h(1)), to = path_arg(rt, a.h(2));
    guarded(rt, "rename", [&] { the_vfs().rename(from, to); });
    return 0;
  });
  r.bind(lx, "readlink", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    return rt.new_string_utf8(guarded(rt, "readlink", [&] { return the_vfs().readlink(path); }));
  });
  r.bind(lx, "realpath", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    return rt.new_string_utf8(guarded(rt, "realpath", [&] {
      std::string p = the_vfs().resolve(path);
      the_vfs().stat(p);
      return p;
    }));
  });
  // mmap: a private copy of the file range in emulated native memory. Writes
  // are not carried back to the file (MAP_SHARED is not emulated).
  // MAP_ANONYMOUS: zeroed memory, the descriptor (null or -1) is ignored. The
  // address hint is ignored too, and MAP_FIXED, which would impose it, fails.
  r.bind(lx, "mmap", "(JJIILjava/io/FileDescriptor;J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t byte_count = a.i64(3);
    int32_t prot = a.i32(5), flags = a.i32(6);
    int64_t offset = a.i64(8);
    constexpr int32_t kProtWrite = 0x2, kMapFixed = 0x10, kMapAnonymous = 0x20;
    if (byte_count <= 0 || offset < 0 || (flags & kMapFixed)) throw_errno(rt, "mmap", vfs::kEINVAL);
    bool writable = (prot & kProtWrite) != 0;
    if (flags & kMapAnonymous) {
      return rt::ret_long(guarded(rt, "mmap", [&] {
        return native_wrap(rt, mapping_bytes(uint64_t(byte_count)), writable);
      }));
    }
    int32_t fd = fd_of(rt, a.h(7));
    return rt::ret_long(guarded(rt, "mmap", [&] {
      return map_fd_copy(rt, fd, uint64_t(offset), uint64_t(byte_count), writable);
    }));
  },S::kPartial, "copie privée du fichier (MAP_SHARED non émulé)");
  // EINVAL, as munmap(2), unless `address` is the start of a mapping, which is
  // released whole (partial unmapping is not emulated).
  r.bind(lx, "munmap", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.i64(3) <= 0 || !native_try_free(a.i64(1))) throw_errno(rt, "munmap", vfs::kEINVAL);
    return 0;
  });
  for (const char* name : {"fsync", "fdatasync"}) {
    r.bind(lx, name, "(Ljava/io/FileDescriptor;)V", [name](Runtime& rt, NativeArgs& a) -> uint64_t {
      int32_t fd = fd_of(rt, a.h(1));
      if (FILE* out = std_stream(fd)) {
        std::fflush(out);
        return 0;
      }
      guarded(rt, name, [&] { the_vfs().fsync(fd); });
      return 0;
    });
  }
  r.bind(lx, "ftruncate", "(Ljava/io/FileDescriptor;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    int64_t size = a.i64(2);
    if (size < 0) throw_errno(rt, "ftruncate", vfs::kEINVAL);
    guarded(rt, "ftruncate", [&] { the_vfs().ftruncate(fd, uint64_t(size)); });
    return 0;
  });
  r.bind(lx, "posix_fallocate", "(Ljava/io/FileDescriptor;JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    int64_t offset = a.i64(2), length = a.i64(4);
    if (offset < 0 || length <= 0) throw_errno(rt, "posix_fallocate", vfs::kEINVAL);
    if (length > INT64_MAX - offset) throw_errno(rt, "posix_fallocate", vfs::kEFBIG);
    int64_t end = offset + length;
    guarded(rt, "posix_fallocate", [&] {
      if (the_vfs().fstat(fd).size < uint64_t(end)) the_vfs().ftruncate(fd, uint64_t(end));
    });
    return 0;
  });
  // Permissions and owners: the emulated process owns its files; no effect.
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"chmod", "(Ljava/lang/String;I)V"}, {"chown", "(Ljava/lang/String;II)V"}, {"lchown", "(Ljava/lang/String;II)V"}}) {
    r.bind(lx, name, sig, [name](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::string path = path_arg(rt, a.h(1));
      guarded(rt, name, [&] { the_vfs().stat(path, std::string(name) != "lchown"); });
      return 0;
    }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  }
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"fchmod", "(Ljava/io/FileDescriptor;I)V"}, {"fchown", "(Ljava/io/FileDescriptor;II)V"}}) {
    r.bind(lx, name, sig, [name](Runtime& rt, NativeArgs& a) -> uint64_t {
      int32_t fd = fd_of(rt, a.h(1));
      if (fd > 2) guarded(rt, name, [&] { the_vfs().fstat(fd); });
      return 0;
    }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  }
  r.bind(lx, "symlink", "(Ljava/lang/String;Ljava/lang/String;)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    throw_errno(rt, "symlink", vfs::kEPERM);
  }, S::kUnsupported, "aucun lien créé par l'application (bac à sable)");
  r.bind(lx, "link", "(Ljava/lang/String;Ljava/lang/String;)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    throw_errno(rt, "link", vfs::kEPERM);
  }, S::kUnsupported, "aucun lien créé par l'application (bac à sable)");
  r.bind(lx, "getxattr", "(Ljava/lang/String;Ljava/lang/String;)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "getxattr", [&] {
      the_vfs().stat(path);
      throw vfs::VfsError{vfs::kENODATA, path};
    });
    return kNull;
  });
  r.bind(lx, "listxattr", "(Ljava/lang/String;)[Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "listxattr", [&] { the_vfs().stat(path); });
    return string_array(rt, {});
  });
  // The host areas (NTFS) keep no Linux extended attributes: writing one fails
  // as on a file system without xattr support.
  r.bind(lx, "setxattr", "(Ljava/lang/String;Ljava/lang/String;[BI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "setxattr", [&] {
      the_vfs().stat(path);
      throw vfs::VfsError{vfs::kEOPNOTSUPP, path};
    });
    return 0;
  }, S::kUnsupported, "pas d'attributs étendus dans le bac à sable (EOPNOTSUPP)");
  r.bind(lx, "removexattr", "(Ljava/lang/String;Ljava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    guarded(rt, "removexattr", [&] {
      the_vfs().stat(path);
      throw vfs::VfsError{vfs::kENODATA, path};
    });
    return 0;
  });
  r.bind(lx, "statvfs", "(Ljava/lang/String;)Landroid/system/StructStatVfs;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    bool w = guarded(rt, "statvfs", [&] { return the_vfs().writable(path); });
    return struct_statvfs(rt, w);
  }, S::kPartial, "valeurs de capacité fixes");
  r.bind(lx, "fstatvfs", "(Ljava/io/FileDescriptor;)Landroid/system/StructStatVfs;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    bool w = fd > 2 && guarded(rt, "fstatvfs", [&] { return the_vfs().writable(the_vfs().fd_path(fd)); });
    return struct_statvfs(rt, w);
  }, S::kPartial, "valeurs de capacité fixes");
  r.bind(lx, "fcntlVoid", "(Ljava/io/FileDescriptor;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    int32_t cmd = a.i32(2);
    if (cmd == 1) return 0;  // F_GETFD: no FD_CLOEXEC bookkeeping
    if (cmd == 3) {          // F_GETFL
      if (fd >= 0 && fd <= 2) return rt::ret_int(fd == 0 ? vfs::kO_RDONLY : vfs::kO_WRONLY);
      return rt::ret_int(guarded(rt, "fcntl", [&] { return the_vfs().open_flags(fd); }));
    }
    throw_errno(rt, "fcntl", vfs::kEINVAL);
  }, S::kPartial, "F_GETFD et F_GETFL");
  r.bind(lx, "fcntlInt", "(Ljava/io/FileDescriptor;II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t cmd = a.i32(2);
    if (cmd == 2 || cmd == 4) return 0;  // F_SETFD, F_SETFL: accepted without effect
    throw_errno(rt, "fcntl", vfs::kEINVAL);
  }, S::kPartial, "F_SETFD et F_SETFL sans effet");
}

// java.io.FileInputStream natives (instance methods on the stream's `fd`).
void register_file_input_stream(rt::NativeRegistry& r) {
  const char* fis = "Ljava/io/FileInputStream;";
  auto stream_fd = [](Runtime& rt, Handle stream) {
    rt::Object& o = rt.nonnull(stream, "FileInputStream");
    rt::Field* f = find_field(o.klass, "fd", "Ljava/io/FileDescriptor;");
    if (!f) rt.throw_new("Ljava/lang/InternalError;", "FileInputStream sans champ fd");
    Handle fd = o.fields.at(f->slot);
    if (fd == kNull) rt.throw_new("Ljava/io/IOException;", "Stream Closed");
    return fd_of(rt, fd);
  };
  r.bind(fis, "available0", "()I", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = stream_fd(rt, a.h(0));
    if (fd >= 0 && fd <= 2) return 0;
    int64_t left = guarded(rt, "available", [&] {
      int64_t pos = the_vfs().lseek(fd, 0, 1);
      return int64_t(the_vfs().fstat(fd).size) - pos;
    });
    return rt::ret_int(int32_t(std::clamp<int64_t>(left, 0, INT32_MAX)));
  });
  r.bind(fis, "length0", "()J", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = stream_fd(rt, a.h(0));
    if (fd >= 0 && fd <= 2) return 0;
    return rt::ret_long(int64_t(guarded(rt, "length", [&] { return the_vfs().fstat(fd).size; })));
  });
  r.bind(fis, "position0", "()J", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = stream_fd(rt, a.h(0));
    if (fd >= 0 && fd <= 2) return 0;
    return rt::ret_long(guarded(rt, "position", [&] { return the_vfs().lseek(fd, 0, 1); }));
  });
  r.bind(fis, "skip0", "(J)J", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = stream_fd(rt, a.h(0));
    int64_t n = a.i64(1);
    if (fd >= 0 && fd <= 2) throw_errno(rt, "lseek", vfs::kESPIPE);
    return rt::ret_long(guarded(rt, "skip", [&] {
      int64_t before = the_vfs().lseek(fd, 0, 1);
      return the_vfs().lseek(fd, n, 1) - before;
    }));
  });

  const char* console = "Ljava/io/Console;";
  r.bind(console, "istty", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_bool(_isatty(0) && _isatty(1)); });
  r.bind(console, "encoding", "()Ljava/lang/String;", [](Runtime&, NativeArgs&) -> uint64_t { return kNull; });
  r.bind(console, "echo", "(Z)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 1; }, S::kStub, "écho du terminal inchangé");
}

// java.io.UnixFileSystem (java.io.File), instance methods: argument 0 is the file system.
void register_unix_file_system(rt::NativeRegistry& r) {
  const char* ufs = "Ljava/io/UnixFileSystem;";
  auto file_path = [](Runtime& rt, Handle file) {
    if (file == kNull) rt.throw_npe("File");
    return rt.string_utf8(Handle(rt.call_virtual(file, "getPath", "()Ljava/lang/String;")));
  };
  r.bind(ufs, "initIDs", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ufs, "getBooleanAttributes0", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    try {
      vfs::VStat st = the_vfs().stat(path);
      int32_t attrs = 0x01;  // BA_EXISTS
      if (st.type == vfs::NodeType::kDirectory) attrs |= 0x04;   // BA_DIRECTORY
      else if (st.type == vfs::NodeType::kFile) attrs |= 0x02;   // BA_REGULAR
      size_t slash = path.find_last_of('/');
      if (path.size() > slash + 1 && path[slash + 1] == '.') attrs |= 0x08;  // BA_HIDDEN
      return rt::ret_int(attrs);
    } catch (const vfs::VfsError&) {
      return 0;
    }
  });
  r.bind(ufs, "canonicalize0", "(Ljava/lang/String;Z)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    std::string out = guarded(rt, "canonicalize", [&] {
      try {
        return the_vfs().resolve(path);
      } catch (const vfs::VfsError&) {
        return vfs::Vfs::normalize(path);  // like realpath on the existing prefix
      }
    });
    return rt.new_string_utf8(out);
  });
  r.bind(ufs, "createDirectory0", "(Ljava/io/File;)Z", [file_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = file_path(rt, a.h(1));
    try {
      the_vfs().mkdir(path, 0777);
      return 1;
    } catch (const vfs::VfsError&) {
      return 0;
    }
  });
  r.bind(ufs, "createFileExclusively0", "(Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_arg(rt, a.h(1));
    try {
      int32_t fd = the_vfs().open(path, vfs::kO_RDWR | vfs::kO_CREAT | vfs::kO_EXCL, 0666);
      the_vfs().close(fd);
      return 1;
    } catch (const vfs::VfsError& e) {
      if (e.err == vfs::kEEXIST) return 0;
      rt.throw_new("Ljava/io/IOException;", std::string("createFileExclusively : errno ") + std::to_string(e.err));
    }
  });
  r.bind(ufs, "list0", "(Ljava/io/File;)[Ljava/lang/String;", [file_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = file_path(rt, a.h(1));
    try {
      return string_array(rt, the_vfs().list(path));
    } catch (const vfs::VfsError&) {
      return kNull;
    }
  });
  r.bind(ufs, "getLastModifiedTime0", "(Ljava/io/File;)J", [file_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = file_path(rt, a.h(1));
    try {
      return rt::ret_long(the_vfs().stat(path).mtime * 1000);
    } catch (const vfs::VfsError&) {
      return 0;
    }
  });
  r.bind(ufs, "setLastModifiedTime0", "(Ljava/io/File;J)Z", [file_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = file_path(rt, a.h(1));
    try {
      the_vfs().set_mtime(path, a.i64(2) / 1000);
      return 1;
    } catch (const vfs::VfsError&) {
      return 0;
    }
  });
  r.bind(ufs, "setPermission0", "(Ljava/io/File;IZZ)Z", [file_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = file_path(rt, a.h(1));
    return rt::ret_bool(the_vfs().exists(path));
  }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  r.bind(ufs, "setReadOnly0", "(Ljava/io/File;)Z", [file_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = file_path(rt, a.h(1));
    return rt::ret_bool(the_vfs().exists(path));
  }, S::kStub, "droits fixes dans le système de fichiers virtuel");
  r.bind(ufs, "getNameMax0", "(Ljava/lang/String;)J", [](Runtime&, NativeArgs&) -> uint64_t { return 255; });
  r.bind(ufs, "getSpace0", "(Ljava/io/File;I)J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(int64_t(8) << 30);
  }, S::kPartial, "capacité fixe");
}

// libcore.io.Memory: peek/poke on native addresses (emulated native memory,
// see native_memory.h) and bulk copies between primitive arrays.
template <typename T>
T swapped(T v) {
  if constexpr (sizeof(T) == 2) return T(__builtin_bswap16(uint16_t(v)));
  if constexpr (sizeof(T) == 4) return T(__builtin_bswap32(uint32_t(v)));
  if constexpr (sizeof(T) == 8) return T(__builtin_bswap64(uint64_t(v)));
  return v;
}

template <typename T>
T peek(Runtime& rt, int64_t address, bool swap) {
  T v;
  std::memcpy(&v, native_span(rt, address, sizeof(T)).data(), sizeof(T));
  return swap ? swapped(v) : v;
}

template <typename T>
void poke(Runtime& rt, int64_t address, T v, bool swap) {
  if (swap) v = swapped(v);
  std::memcpy(native_span(rt, address, sizeof(T), true).data(), &v, sizeof(T));
}

// Bytes [offset, offset + count) of a primitive array, bounds-checked. Arrays
// of references are refused: copying bytes into one would forge handles.
std::span<uint8_t> array_bytes(Runtime& rt, Handle array, int64_t offset, int64_t count, uint32_t width = 0) {
  rt::ArrayObject& arr = rt.array(array, "tableau");
  if (!arr.klass->component || !arr.klass->component->primitive || (width && arr.width != width)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;",
                 "Memory : " + arr.klass->java_name() + " au lieu du tableau primitif attendu");
  }
  if (offset < 0 || count < 0 || uint64_t(offset) + uint64_t(count) > arr.data.size()) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "Memory : plage hors du tableau");
  }
  return std::span<uint8_t>(arr.data).subspan(size_t(offset), size_t(count));
}

// Elements [offset, offset + count) of a primitive array of `width`-byte elements.
std::span<uint8_t> array_elements(Runtime& rt, Handle array, int32_t offset, int32_t count, uint32_t width) {
  if (offset < 0 || count < 0) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "Memory : plage hors du tableau");
  }
  return array_bytes(rt, array, int64_t(offset) * width, int64_t(count) * width, width);
}

// The bytes behind Memory.memmove's objects: a primitive array, or a direct
// java.nio.Buffer, whose `address` field is an emulated native address (as
// GetDirectBufferAddress in ART's ScopedBytes).
std::span<uint8_t> memmove_bytes(Runtime& rt, Handle object, int32_t offset, int64_t count, bool write) {
  rt::Object& o = rt.nonnull(object, "Memory.memmove");
  if (dynamic_cast<rt::ArrayObject*>(&o)) return array_bytes(rt, object, offset, count);
  for (rt::Class* c = o.klass; c; c = c->super) {
    if (c->descriptor != "Ljava/nio/Buffer;") continue;
    rt::Field* f = find_field(o.klass, "address", "J");
    if (!f || f->is_static()) break;
    uint64_t address = uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32);
    if (address == 0) break;  // heap buffer
    if (offset < 0 || count < 0) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "Memory.memmove");
    if (count == 0) return {};
    return native_span(rt, int64_t(address + uint32_t(offset)), uint64_t(count), write);
  }
  rt.throw_new("Ljava/lang/IllegalArgumentException;", "Memory.memmove : " + o.klass->java_name() +
                                                            " n'est ni un tableau primitif ni un tampon direct");
}

// Copies `bytes` bytes of `width`-byte elements, reversing the bytes of each
// element when `swap` (whole elements only, as libcore's swap routines).
void copy_elements(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t width, bool swap) {
  if (!swap || width == 1) {
    std::memmove(dst, src, bytes);
    return;
  }
  for (size_t i = 0; i + width <= bytes; i += width) {
    uint8_t e[8];
    std::memcpy(e, src + i, width);
    std::reverse(e, e + width);
    std::memcpy(dst + i, e, width);
  }
}

void register_libcore_memory(rt::NativeRegistry& r) {
  const char* mem = "Llibcore/io/Memory;";
  r.bind(mem, "peekByte", "(J)B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return uint32_t(int32_t(peek<int8_t>(rt, a.i64(0), false)));
  });
  r.bind(mem, "pokeByte", "(JB)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint8_t>(rt, a.i64(0), uint8_t(a.raw(2)), false);
    return 0;
  });
  r.bind(mem, "peekByteArray", "(J[BII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t count = a.i32(4);
    auto dst = array_bytes(rt, a.h(2), a.i32(3), count);
    if (count > 0) std::memcpy(dst.data(), native_span(rt, a.i64(0), uint64_t(count)).data(), size_t(count));
    return 0;
  });
  r.bind(mem, "pokeByteArray", "(J[BII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t count = a.i32(4);
    auto src = array_bytes(rt, a.h(2), a.i32(3), count);
    if (count > 0) std::memcpy(native_span(rt, a.i64(0), uint64_t(count), true).data(), src.data(), size_t(count));
    return 0;
  });
  // peek/poke<Type>Array(long address, <type>[] array, int offset, int count,
  // boolean swap): offset and count in elements.
  struct ArrayKind {
    const char* name;
    char type;
    uint32_t width;
  };
  for (ArrayKind k : {ArrayKind{"Char", 'C', 2}, ArrayKind{"Short", 'S', 2}, ArrayKind{"Int", 'I', 4},
                      ArrayKind{"Float", 'F', 4}, ArrayKind{"Long", 'J', 8}, ArrayKind{"Double", 'D', 8}}) {
    std::string sig = std::string("(J[") + k.type + "IIZ)V";
    r.bind(mem, std::string("peek") + k.name + "Array", sig, [width = k.width](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto dst = array_elements(rt, a.h(2), a.i32(3), a.i32(4), width);
      if (dst.empty()) return 0;
      copy_elements(dst.data(), native_span(rt, a.i64(0), dst.size()).data(), dst.size(), width, a.z(5));
      return 0;
    });
    r.bind(mem, std::string("poke") + k.name + "Array", sig, [width = k.width](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto src = array_elements(rt, a.h(2), a.i32(3), a.i32(4), width);
      if (src.empty()) return 0;
      copy_elements(native_span(rt, a.i64(0), src.size(), true).data(), src.data(), src.size(), width, a.z(5));
      return 0;
    });
  }
  r.bind(mem, "peekShortNative", "(J)S", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return uint32_t(int32_t(peek<int16_t>(rt, a.i64(0), false)));
  });
  r.bind(mem, "pokeShortNative", "(JS)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint16_t>(rt, a.i64(0), uint16_t(a.raw(2)), false);
    return 0;
  });
  r.bind(mem, "peekIntNative", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return peek<uint32_t>(rt, a.i64(0), false);
  });
  r.bind(mem, "pokeIntNative", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint32_t>(rt, a.i64(0), a.raw(2), false);
    return 0;
  });
  r.bind(mem, "peekLongNative", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return peek<uint64_t>(rt, a.i64(0), false);
  });
  r.bind(mem, "pokeLongNative", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint64_t>(rt, a.i64(0), uint64_t(a.i64(2)), false);
    return 0;
  });
  // peek/poke<Type>(long address[, value], boolean swap) are bytecode around
  // the natives above (call, then reverseBytes): intrinsics save both calls.
  const char* note = "accès direct à la mémoire native émulée";
  r.intrinsic(mem, "peekShort", "(JZ)S", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return uint32_t(int32_t(peek<int16_t>(rt, a.i64(0), a.z(2))));
  }, note);
  r.intrinsic(mem, "pokeShort", "(JSZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint16_t>(rt, a.i64(0), uint16_t(a.raw(2)), a.z(3));
    return 0;
  }, note);
  r.intrinsic(mem, "peekInt", "(JZ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return peek<uint32_t>(rt, a.i64(0), a.z(2));
  }, note);
  r.intrinsic(mem, "pokeInt", "(JIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint32_t>(rt, a.i64(0), a.raw(2), a.z(3));
    return 0;
  }, note);
  r.intrinsic(mem, "peekLong", "(JZ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return peek<uint64_t>(rt, a.i64(0), a.z(2));
  }, note);
  r.intrinsic(mem, "pokeLong", "(JJZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    poke<uint64_t>(rt, a.i64(0), uint64_t(a.i64(2)), a.z(4));
    return 0;
  }, note);
  // memmove(Object dst, int dstOffset, Object src, int srcOffset, long byteCount).
  r.bind(mem, "memmove", "(Ljava/lang/Object;ILjava/lang/Object;IJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t count = a.i64(4);
    auto dst = memmove_bytes(rt, a.h(0), a.i32(1), count, true);
    auto src = memmove_bytes(rt, a.h(2), a.i32(3), count, false);
    if (count > 0) std::memmove(dst.data(), src.data(), size_t(count));
    return 0;
  });
  // unsafeBulkGet(Object dst, int dstOffset, int byteCount, byte[] src, int srcOffset,
  // int sizeofElements, boolean swap) and unsafeBulkPut(byte[] dst, int dstOffset,
  // int byteCount, Object src, int srcOffset, int sizeofElements, boolean swap):
  // heap ByteBuffers' bulk transfers. The offset in the typed array is in elements.
  auto element_width = [](Runtime& rt, int32_t width) {
    if (width != 1 && width != 2 && width != 4 && width != 8) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "Memory : taille d'élément " + std::to_string(width));
    }
    return uint32_t(width);
  };
  r.bind(mem, "unsafeBulkGet", "(Ljava/lang/Object;II[BIIZ)V", [element_width](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint32_t width = element_width(rt, a.i32(5));
    int32_t count = a.i32(2);
    if (a.i32(1) < 0) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "Memory : plage hors du tableau");
    auto dst = array_bytes(rt, a.h(0), int64_t(a.i32(1)) * width, count, width);
    auto src = array_bytes(rt, a.h(3), a.i32(4), count, 1);
    if (count > 0) copy_elements(dst.data(), src.data(), size_t(count), width, a.z(6));
    return 0;
  });
  r.bind(mem, "unsafeBulkPut", "([BIILjava/lang/Object;IIZ)V", [element_width](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint32_t width = element_width(rt, a.i32(5));
    int32_t count = a.i32(2);
    if (a.i32(4) < 0) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "Memory : plage hors du tableau");
    auto dst = array_bytes(rt, a.h(0), a.i32(1), count, 1);
    auto src = array_bytes(rt, a.h(3), int64_t(a.i32(4)) * width, count, width);
    if (count > 0) copy_elements(dst.data(), src.data(), size_t(count), width, a.z(6));
    return 0;
  });
}

}  // namespace

void register_libcore_files(rt::NativeRegistry& r) {
  register_linux(r);
  register_unix_file_system(r);
  register_file_input_stream(r);
  register_libcore_memory(r);
}

}  // namespace aa::bridge
