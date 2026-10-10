#include "framework-bridge/internal.h"
#include "vfs/vfs.h"

#include <cstring>
#include <string>

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

vfs::Vfs& the_vfs() { return libcore_vfs(); }

template <typename F>
auto guarded(Runtime& rt, const char* function, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const vfs::VfsError& e) {
    if (rt.lookup_class("Lsun/nio/fs/UnixException;")) {
      throw rt::JavaThrow{construct(rt, "Lsun/nio/fs/UnixException;", "(I)V", {uint32_t(e.err)})};
    }
    throw_errno(rt, function, e.err);
  }
}

std::string path_from_address(Runtime& rt, int64_t address) {
  rt::ArrayObject& arr = array_at_address(rt, address);
  if (arr.width != 1) rt.throw_new("Ljava/lang/IllegalArgumentException;", "path buffer is not byte[]");
  const char* data = reinterpret_cast<const char*>(arr.data.data());
  size_t len = 0;
  while (len < arr.length && data[len] != '\0') len++;
  return std::string(data, len);
}

std::span<uint8_t> buffer_from_address(Runtime& rt, int64_t address, int32_t len) {
  rt::ArrayObject& arr = array_at_address(rt, address);
  if (arr.width != 1) rt.throw_new("Ljava/lang/IllegalArgumentException;", "buffer is not byte[]");
  if (len < 0 || uint32_t(len) > arr.length) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "buffer length");
  return std::span<uint8_t>(reinterpret_cast<uint8_t*>(arr.data.data()), len);
}

Handle make_byte_array(Runtime& rt, const std::string& str) {
  Handle h = rt.new_array("[B", int32_t(str.size()));
  rt::ArrayObject& arr = rt.array(h, "byte[]");
  std::memcpy(arr.data.data(), str.data(), str.size());
  return h;
}

void set_unix_attributes(Runtime& rt, Handle attrs, const vfs::VStat& st) {
  rt::Class* c = rt.class_of(attrs);
  rt::Object* o = rt.object(attrs);
  
  auto set_int = [&](const char* name, int32_t v) {
    rt::Field* f = find_field(c, name, "I");
    if (f) o->fields.at(f->slot) = uint32_t(v);
  };
  auto set_long = [&](const char* name, int64_t v) {
    rt::Field* f = find_field(c, name, "J");
    if (f) {
      o->fields.at(f->slot) = uint32_t(v);
      o->fields.at(f->slot + 1) = uint32_t(v >> 32);
    }
  };
  
  set_int("st_mode", st.mode);
  set_long("st_ino", st.ino);
  set_long("st_dev", st.dev ? st.dev : 0xFD00);
  set_long("st_rdev", 0);
  set_int("st_nlink", int32_t(st.nlink));
  set_int("st_uid", st.uid);
  set_int("st_gid", st.gid);
  set_long("st_size", st.size);
  set_long("st_atime", st.mtime);
  set_long("st_mtime", st.mtime);
  set_long("st_ctime", st.mtime);
}

void register_unix_native_dispatcher(rt::NativeRegistry& r) {
  const char* und = "Lsun/nio/fs/UnixNativeDispatcher;";
  r.bind(und, "init", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  
  auto do_stat = [](Runtime& rt, NativeArgs& a, bool follow) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    vfs::VStat st = guarded(rt, "stat", [&] { return the_vfs().stat(path, follow); });
    set_unix_attributes(rt, a.h(2), st);
    return 0;
  };
  r.bind(und, "stat0", "(JLsun/nio/fs/UnixFileAttributes;)V", [do_stat](Runtime& rt, NativeArgs& a) -> uint64_t {
    return do_stat(rt, a, true);
  });
  r.bind(und, "lstat0", "(JLsun/nio/fs/UnixFileAttributes;)V", [do_stat](Runtime& rt, NativeArgs& a) -> uint64_t {
    return do_stat(rt, a, false);
  });
  r.bind(und, "fstat0", "(ILsun/nio/fs/UnixFileAttributes;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    vfs::VStat st;
    if (fd >= 0 && fd <= 2) {
      st.type = vfs::NodeType::kCharDevice;
      st.mode = 020620;
      st.ino = uint64_t(fd) + 1;
    } else {
      st = guarded(rt, "fstat", [&] { return the_vfs().fstat(fd); });
    }
    set_unix_attributes(rt, a.h(1), st);
    return 0;
  });

  r.bind(und, "open0", "(JII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    return rt::ret_int(guarded(rt, "open", [&] { return the_vfs().open(path, a.i32(2), a.i32(3)); }));
  });
  r.bind(und, "close0", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (fd > 2) guarded(rt, "close", [&] { the_vfs().close(fd); });
    return 0;
  });

  r.bind(und, "read", "(IJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3));
    if (buf.empty()) return 0;
    if (fd == 0) return rt::ret_int(int32_t(std::fread(buf.data(), 1, buf.size(), stdin)));
    if (fd == 1 || fd == 2) throw_errno(rt, "read", vfs::kEBADF);
    return rt::ret_int(int32_t(guarded(rt, "read", [&] { return the_vfs().read(fd, buf); })));
  });
  r.bind(und, "write", "(IJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3));
    if (fd == 1) { std::fwrite(buf.data(), 1, buf.size(), stdout); std::fflush(stdout); return rt::ret_int(int32_t(buf.size())); }
    if (fd == 2) { std::fwrite(buf.data(), 1, buf.size(), stderr); std::fflush(stderr); return rt::ret_int(int32_t(buf.size())); }
    if (fd == 0) throw_errno(rt, "write", vfs::kEBADF);
    return rt::ret_int(int32_t(guarded(rt, "write", [&] { return the_vfs().write(fd, buf); })));
  });

  // Directory iteration stubs (AnyAndroid doesn't support DIR* properly in NIO yet, but we can fake it or fail gracefully)
  // Actually, VFS provides `list`. 
  // Let's implement it with a global map or simply not support it for now, as AnyAndroid's libcore_files.cpp doesn't implement opendir.
  r.bind(und, "opendir0", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    throw_errno(rt, "opendir", vfs::kENOSPC); // Use kENOSPC as dummy
  }, S::kUnsupported);
  r.bind(und, "readdir", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return kNull;
  }, S::kUnsupported);

  r.bind(und, "mkdir0", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    guarded(rt, "mkdir", [&] { the_vfs().mkdir(path, uint32_t(a.i32(2))); });
    return 0;
  });
  r.bind(und, "unlink0", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    guarded(rt, "unlink", [&] { the_vfs().unlink(path); });
    return 0;
  });
  r.bind(und, "rename0", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string from = path_from_address(rt, a.i64(0)), to = path_from_address(rt, a.i64(2));
    guarded(rt, "rename", [&] { the_vfs().rename(from, to); });
    return 0;
  });
  r.bind(und, "readlink0", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    std::string target = guarded(rt, "readlink", [&] { return the_vfs().readlink(path); });
    return make_byte_array(rt, target);
  });
  r.bind(und, "realpath0", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    std::string real = guarded(rt, "realpath", [&] {
      std::string p = the_vfs().resolve(path);
      the_vfs().stat(p);
      return p;
    });
    return make_byte_array(rt, real);
  });
  r.bind(und, "access0", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string path = path_from_address(rt, a.i64(0));
    int32_t mode = a.i32(2);
    guarded(rt, "access", [&] {
      the_vfs().stat(path);
      if ((mode & 2) && !the_vfs().writable(path)) throw vfs::VfsError{vfs::kEROFS, path};
    });
    return 0;
  });
  r.bind(und, "getcwd", "()[B", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return make_byte_array(rt, "/");
  }, S::kPartial, "dossier courant fixe");
  r.bind(und, "strerror", "(I)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return make_byte_array(rt, "Error " + std::to_string(a.i32(0)));
  });
}

void register_file_dispatcher(rt::NativeRegistry& r) {
  const char* fd = "Lsun/nio/ch/FileDispatcherImpl;";
  auto stream_fd = [](Runtime& rt, Handle fdo) {
    rt::Object& o = rt.nonnull(fdo, "FileDescriptor");
    rt::Field* f = find_field(o.klass, "descriptor", "I");
    if (!f) rt.throw_new("Ljava/lang/InternalError;", "FileDescriptor sans champ descriptor");
    return int32_t(o.fields.at(f->slot));
  };
  r.bind(fd, "read0", "(Ljava/io/FileDescriptor;JI)I", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3));
    if (buf.empty()) return 0;
    if (f == 0) return rt::ret_int(int32_t(std::fread(buf.data(), 1, buf.size(), stdin)));
    if (f == 1 || f == 2) throw_errno(rt, "read", vfs::kEBADF);
    return rt::ret_int(int32_t(guarded(rt, "read", [&] { return the_vfs().read(f, buf); })));
  });
  r.bind(fd, "write0", "(Ljava/io/FileDescriptor;JI)I", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3));
    if (f == 1) { std::fwrite(buf.data(), 1, buf.size(), stdout); std::fflush(stdout); return rt::ret_int(int32_t(buf.size())); }
    if (f == 2) { std::fwrite(buf.data(), 1, buf.size(), stderr); std::fflush(stderr); return rt::ret_int(int32_t(buf.size())); }
    if (f == 0) throw_errno(rt, "write", vfs::kEBADF);
    return rt::ret_int(int32_t(guarded(rt, "write", [&] { return the_vfs().write(f, buf); })));
  });
  r.bind(fd, "pread0", "(Ljava/io/FileDescriptor;JIJ)I", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3));
    int64_t off = a.i64(4);
    if (off < 0) throw_errno(rt, "pread", vfs::kEINVAL);
    return rt::ret_int(int32_t(guarded(rt, "pread", [&] { return the_vfs().pread(f, buf, uint64_t(off)); })));
  });
  r.bind(fd, "pwrite0", "(Ljava/io/FileDescriptor;JIJ)I", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    auto buf = buffer_from_address(rt, a.i64(1), a.i32(3));
    int64_t off = a.i64(4);
    if (off < 0) throw_errno(rt, "pwrite", vfs::kEINVAL);
    return rt::ret_int(int32_t(guarded(rt, "pwrite", [&] { return the_vfs().pwrite(f, buf, uint64_t(off)); })));
  });
  r.bind(fd, "close0", "(Ljava/io/FileDescriptor;)V", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    if (f > 2) guarded(rt, "close", [&] { the_vfs().close(f); });
    return 0;
  });
  r.bind(fd, "size0", "(Ljava/io/FileDescriptor;)J", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    if (f >= 0 && f <= 2) return 0;
    return rt::ret_long(int64_t(guarded(rt, "size", [&] { return the_vfs().fstat(f).size; })));
  });
  r.bind(fd, "truncate0", "(Ljava/io/FileDescriptor;J)I", [stream_fd](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t f = stream_fd(rt, a.h(0));
    int64_t size = a.i64(1);
    if (size < 0) throw_errno(rt, "truncate", vfs::kEINVAL);
    if (f > 2) guarded(rt, "truncate", [&] { the_vfs().ftruncate(f, uint64_t(size)); });
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
