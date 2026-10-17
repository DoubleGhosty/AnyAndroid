// android.os.Parcel natives (libbinder's Parcel in Android), for in-process
// use: Bundle/Intent marshalling, AIDL stubs called locally.
//
// Layout follows libbinder: every write is padded to 4 bytes; int64/double
// take 8; String16 = int32 length (-1 for null) + UTF-16 + NUL; String8 =
// int32 length + bytes + NUL; byte arrays = int32 length (-1 for null) +
// bytes; interface tokens = strict mode policy, work source, 'SYST' header,
// String16. Binders are kept in a side table of global references; a parcel
// holding binders cannot be marshalled to bytes (as in Android).
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/text.h"
#include "framework-bridge/internal.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr uint32_t kMaxParcel = 64u << 20;
constexpr int32_t kBinderTag = 0x73622a85;  // BINDER_TYPE_BINDER
constexpr int32_t kInterfaceHeader = 0x53595354;  // 'SYST'

struct NativeParcel {
  std::vector<uint8_t> data;  // capacity = data.size()
  uint32_t size = 0;
  uint32_t pos = 0;
  std::vector<rt::GlobalRef> binders;
  bool allow_fds = true;
};

struct ParcelTable {
  std::vector<std::unique_ptr<NativeParcel>> slots;
  std::vector<size_t> free;
  int64_t allocated = 0;
};

ParcelTable& table() {
  static ParcelTable t;
  return t;
}

NativeParcel& parcel(Runtime& rt, int64_t ptr) {
  auto& t = table();
  if (ptr <= 0 || uint64_t(ptr) > t.slots.size() || !t.slots[size_t(ptr - 1)]) {
    rt.throw_new("Ljava/lang/IllegalStateException;", "Parcel natif invalide");
  }
  return *t.slots[size_t(ptr - 1)];
}

uint32_t pad4(uint32_t n) { return (n + 3) & ~3u; }

// Makes room for `n` bytes at the current position and returns them.
uint8_t* grow(Runtime& rt, NativeParcel& p, uint32_t n) {
  uint64_t end = uint64_t(p.pos) + n;
  if (end > kMaxParcel) rt.throw_new("Ljava/lang/OutOfMemoryError;", "Parcel trop grand");
  if (end > p.data.size()) p.data.resize(size_t(std::max<uint64_t>(end, p.data.size() * 2)), 0);
  uint8_t* at = p.data.data() + p.pos;
  p.pos = uint32_t(end);
  p.size = std::max(p.size, p.pos);
  return at;
}

void write_raw(Runtime& rt, NativeParcel& p, const void* src, uint32_t n) {
  uint32_t padded = pad4(n);
  uint8_t* at = grow(rt, p, padded);
  if (n) std::memcpy(at, src, n);
  if (padded > n) std::memset(at + n, 0, padded - n);
}

void write_i32(Runtime& rt, NativeParcel& p, int32_t v) { write_raw(rt, p, &v, 4); }
void write_i64(Runtime& rt, NativeParcel& p, int64_t v) { write_raw(rt, p, &v, 8); }

// Reads `n` bytes (advancing by the padded size); nullptr when not enough data.
const uint8_t* read_raw(NativeParcel& p, uint32_t n) {
  uint32_t padded = pad4(n);
  if (uint64_t(p.pos) + padded > p.size || padded < n) return nullptr;
  const uint8_t* at = p.data.data() + p.pos;
  p.pos += padded;
  return at;
}

int32_t read_i32(NativeParcel& p) {
  const uint8_t* at = read_raw(p, 4);
  int32_t v = 0;
  if (at) std::memcpy(&v, at, 4);
  return v;
}

int64_t read_i64(NativeParcel& p) {
  const uint8_t* at = read_raw(p, 8);
  int64_t v = 0;
  if (at) std::memcpy(&v, at, 8);
  return v;
}

void write_string16(Runtime& rt, NativeParcel& p, Handle s) {
  if (s == kNull) {
    write_i32(rt, p, -1);
    return;
  }
  std::u16string v = rt.string_value(s);
  write_i32(rt, p, int32_t(v.size()));
  std::u16string with_nul = v + u'\0';
  write_raw(rt, p, with_nul.data(), uint32_t(with_nul.size() * 2));
}

Handle read_string16(Runtime& rt, NativeParcel& p) {
  int32_t len = read_i32(p);
  if (len < 0 || uint64_t(len) * 2 + 2 > uint64_t(p.size - std::min(p.pos, p.size))) return kNull;
  const uint8_t* at = read_raw(p, uint32_t(len) * 2 + 2);
  if (!at) return kNull;
  std::u16string v(size_t(len), u'\0');
  if (len) std::memcpy(v.data(), at, size_t(len) * 2);
  return rt.new_string(std::move(v));
}

std::vector<uint8_t> byte_slice(Runtime& rt, Handle arr, int32_t off, int32_t len) {
  rt::ArrayObject& a = rt.array(arr, "byte[]");
  if (a.width != 1 || off < 0 || len < 0 || int64_t(off) + len > a.length) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "Parcel : tableau hors limites");
  }
  return std::vector<uint8_t>(a.data.begin() + off, a.data.begin() + off + len);
}

Handle new_bytes(Runtime& rt, const uint8_t* src, uint32_t n) {
  Handle h = rt.new_array("[B", int32_t(n));
  if (n) std::memcpy(rt.array(h, "byte[]").data.data(), src, n);
  return h;
}

}  // namespace

// The native parcel of a Java Parcel (Parcel.mNativePtr).
static NativeParcel& parcel_of_object(Runtime& rt, Handle parcel_object) {
  rt::Object& o = rt.nonnull(parcel_object, "Parcel");
  rt::Field* f = find_field(o.klass, "mNativePtr", "J");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/IllegalStateException;", "Parcel sans natif");
  int64_t ptr = int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
  return parcel(rt, ptr);
}

int32_t parcel_read_i32(Runtime& rt, Handle parcel_object, bool* ok) {
  NativeParcel& p = parcel_of_object(rt, parcel_object);
  const uint8_t* at = read_raw(p, 4);
  if (!at) {
    *ok = false;
    return 0;
  }
  int32_t v;
  std::memcpy(&v, at, 4);
  return v;
}

void parcel_write_i32(Runtime& rt, Handle parcel_object, int32_t v) {
  write_i32(rt, parcel_of_object(rt, parcel_object), v);
}

std::string parcel_read_string8(Runtime& rt, Handle parcel_object, bool* ok) {
  NativeParcel& p = parcel_of_object(rt, parcel_object);
  int32_t len = read_i32(p);
  const uint8_t* at = len < 0 ? nullptr : read_raw(p, uint32_t(len) + 1);
  if (!at) {
    *ok = false;
    return {};
  }
  return std::string(reinterpret_cast<const char*>(at), size_t(len));
}

void parcel_write_string8(Runtime& rt, Handle parcel_object, const std::string& v) {
  NativeParcel& p = parcel_of_object(rt, parcel_object);
  write_i32(rt, p, int32_t(v.size()));
  write_raw(rt, p, v.c_str(), uint32_t(v.size() + 1));
}

int64_t parcel_read_i64(Runtime& rt, Handle parcel_object, bool* ok) {
  NativeParcel& p = parcel_of_object(rt, parcel_object);
  const uint8_t* at = read_raw(p, 8);
  if (!at) {
    *ok = false;
    return 0;
  }
  int64_t v;
  std::memcpy(&v, at, 8);
  return v;
}

void parcel_write_i64(Runtime& rt, Handle parcel_object, int64_t v) {
  write_i64(rt, parcel_of_object(rt, parcel_object), v);
}

std::vector<uint8_t> parcel_read_byte_vector(Runtime& rt, Handle parcel_object, bool* ok) {
  NativeParcel& p = parcel_of_object(rt, parcel_object);
  const uint8_t* at = read_raw(p, 4);
  int32_t len = 0;
  if (at) std::memcpy(&len, at, 4);
  const uint8_t* bytes = !at || len < 0 ? nullptr : read_raw(p, uint32_t(len));
  if (!bytes) {
    *ok = false;
    return {};
  }
  return std::vector<uint8_t>(bytes, bytes + len);
}

void parcel_write_byte_vector(Runtime& rt, Handle parcel_object, const std::vector<uint8_t>& v) {
  NativeParcel& p = parcel_of_object(rt, parcel_object);
  write_i32(rt, p, int32_t(v.size()));
  write_raw(rt, p, v.data(), uint32_t(v.size()));
}

void register_parcel_natives(rt::NativeRegistry& r) {
  const char* pc = "Landroid/os/Parcel;";
  r.bind(pc, "nativeCreate", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    auto& t = table();
    ++t.allocated;
    if (!t.free.empty()) {
      size_t i = t.free.back();
      t.free.pop_back();
      t.slots[i] = std::make_unique<NativeParcel>();
      return uint64_t(i + 1);
    }
    t.slots.push_back(std::make_unique<NativeParcel>());
    return uint64_t(t.slots.size());
  });
  r.bind(pc, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto& t = table();
    int64_t ptr = a.i64(0);
    if (ptr > 0 && uint64_t(ptr) <= t.slots.size() && t.slots[size_t(ptr - 1)]) {
      t.slots[size_t(ptr - 1)].reset();
      t.free.push_back(size_t(ptr - 1));
      --t.allocated;
    }
    return 0;
  });
  r.bind(pc, "nativeFreeBuffer", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    p = NativeParcel{};
    return 0;
  });
  r.bind(pc, "getGlobalAllocCount", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(table().allocated); });
  r.bind(pc, "getGlobalAllocSize", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    int64_t total = 0;
    for (const auto& p : table().slots) total += p ? int64_t(p->data.size()) : 0;
    return rt::ret_long(total);
  });

  // Sizes and positions.
  r.bind(pc, "nativeDataSize", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return parcel(rt, a.i64(0)).size; });
  r.bind(pc, "nativeDataAvail", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    return p.pos < p.size ? p.size - p.pos : 0;
  });
  r.bind(pc, "nativeDataPosition", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return parcel(rt, a.i64(0)).pos; });
  r.bind(pc, "nativeDataCapacity", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return uint32_t(parcel(rt, a.i64(0)).data.size());
  });
  r.bind(pc, "nativeSetDataSize", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t n = a.i32(2);
    if (n < 0 || uint32_t(n) > kMaxParcel) rt.throw_new("Ljava/lang/IllegalArgumentException;", "taille de Parcel invalide");
    if (uint32_t(n) > p.data.size()) p.data.resize(size_t(n), 0);
    p.size = uint32_t(n);
    p.pos = std::min(p.pos, p.size);
    return 0;
  });
  r.bind(pc, "nativeSetDataPosition", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t n = a.i32(2);
    if (n < 0 || uint32_t(n) > kMaxParcel) rt.throw_new("Ljava/lang/IllegalArgumentException;", "position de Parcel invalide");
    p.pos = uint32_t(n);
    return 0;
  });
  r.bind(pc, "nativeSetDataCapacity", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t n = a.i32(2);
    if (n < 0 || uint32_t(n) > kMaxParcel) rt.throw_new("Ljava/lang/IllegalArgumentException;", "capacité de Parcel invalide");
    if (uint32_t(n) > p.data.size()) p.data.resize(size_t(n), 0);
    return 0;
  });

  // Primitives (the int returned by writes is a status: 0 = OK).
  r.bind(pc, "nativeWriteInt", "(JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    write_i32(rt, parcel(rt, a.i64(0)), a.i32(2));
    return 0;
  });
  r.bind(pc, "nativeWriteLong", "(JJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    write_i64(rt, parcel(rt, a.i64(0)), a.i64(2));
    return 0;
  });
  r.bind(pc, "nativeWriteFloat", "(JF)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    write_i32(rt, parcel(rt, a.i64(0)), int32_t(a.raw(2)));
    return 0;
  });
  r.bind(pc, "nativeWriteDouble", "(JD)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    write_i64(rt, parcel(rt, a.i64(0)), a.i64(2));
    return 0;
  });
  r.bind(pc, "nativeReadInt", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(read_i32(parcel(rt, a.i64(0)))); });
  r.bind(pc, "nativeReadLong", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_long(read_i64(parcel(rt, a.i64(0)))); });
  r.bind(pc, "nativeReadFloat", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t { return uint32_t(read_i32(parcel(rt, a.i64(0)))); });
  r.bind(pc, "nativeReadDouble", "(J)D", [](Runtime& rt, NativeArgs& a) -> uint64_t { return uint64_t(read_i64(parcel(rt, a.i64(0)))); });

  // Strings.
  r.bind(pc, "nativeWriteString16", "(JLjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    write_string16(rt, parcel(rt, a.i64(0)), a.h(2));
    return 0;
  });
  r.bind(pc, "nativeReadString16", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return read_string16(rt, parcel(rt, a.i64(0)));
  });
  r.bind(pc, "nativeWriteString8", "(JLjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    if (a.h(2) == kNull) {
      write_i32(rt, p, -1);
      return 0;
    }
    std::string v = rt.string_utf8(a.h(2));
    write_i32(rt, p, int32_t(v.size()));
    write_raw(rt, p, v.c_str(), uint32_t(v.size() + 1));
    return 0;
  });
  r.bind(pc, "nativeReadString8", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t len = read_i32(p);
    if (len < 0) return kNull;
    const uint8_t* at = read_raw(p, uint32_t(len) + 1);
    if (!at) return kNull;
    return rt.new_string_utf8(std::string_view(reinterpret_cast<const char*>(at), size_t(len)));
  });

  // Byte arrays and blobs (always in place: no shared memory).
  r.bind(pc, "nativeWriteByteArray", "(J[BII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    if (a.h(2) == kNull) {
      write_i32(rt, p, -1);
      return 0;
    }
    auto bytes = byte_slice(rt, a.h(2), a.i32(3), a.i32(4));
    write_i32(rt, p, int32_t(bytes.size()));
    write_raw(rt, p, bytes.data(), uint32_t(bytes.size()));
    return 0;
  });
  r.bind(pc, "nativeCreateByteArray", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t len = read_i32(p);
    if (len < 0) return kNull;
    const uint8_t* at = read_raw(p, uint32_t(len));
    if (!at) return kNull;
    return new_bytes(rt, at, uint32_t(len));
  });
  r.bind(pc, "nativeReadByteArray", "(J[BI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t len = read_i32(p);
    if (a.h(2) == kNull || len != a.i32(3)) return 0;
    const uint8_t* at = read_raw(p, uint32_t(len));
    if (!at) return 0;
    rt::ArrayObject& dst = rt.array(a.h(2), "byte[]");
    if (dst.width != 1 || uint32_t(len) > dst.length) return 0;
    if (len) std::memcpy(dst.data.data(), at, size_t(len));
    return 1;
  });
  r.bind(pc, "nativeWriteBlob", "(J[BII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    if (a.h(2) == kNull) {
      write_i32(rt, p, -1);
      return 0;
    }
    auto bytes = byte_slice(rt, a.h(2), a.i32(3), a.i32(4));
    write_i32(rt, p, int32_t(bytes.size()));
    write_i32(rt, p, 0);  // BLOB_INPLACE
    write_raw(rt, p, bytes.data(), uint32_t(bytes.size()));
    return 0;
  }, S::kPartial, "toujours en place (pas de mémoire partagée)");
  r.bind(pc, "nativeReadBlob", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t len = read_i32(p);
    if (len < 0) return kNull;
    if (read_i32(p) != 0) rt.throw_unsupported("Landroid/os/Parcel;->readBlob (mémoire partagée)", "blob hors du Parcel");
    const uint8_t* at = read_raw(p, uint32_t(len));
    if (!at) return kNull;
    return new_bytes(rt, at, uint32_t(len));
  });

  // Interface tokens.
  r.bind(pc, "nativeWriteInterfaceToken", "(JLjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    write_i32(rt, p, 0);   // strict mode policy
    write_i32(rt, p, -1);  // work source uid
    write_i32(rt, p, kInterfaceHeader);
    write_string16(rt, p, a.h(2));
    return 0;
  });
  r.bind(pc, "nativeEnforceInterface", "(JLjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    read_i32(p);
    read_i32(p);
    int32_t header = read_i32(p);
    Handle got = read_string16(rt, p);
    std::u16string want = a.h(2) ? rt.string_value(a.h(2)) : u"";
    if (header != kInterfaceHeader || got == kNull || rt.string_value(got) != want) {
      rt.throw_new("Ljava/lang/SecurityException;", "Binder invocation to an incorrect interface");
    }
    return 0;
  });
  r.bind(pc, "nativeReadCallingWorkSourceUid", "(J)I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); });
  r.bind(pc, "nativeReplaceCallingWorkSourceUid", "(JI)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  // Binders: kept in the parcel (in-process only).
  r.bind(pc, "nativeWriteStrongBinder", "(JLandroid/os/IBinder;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t index = -1;
    if (a.h(2) != kNull) {
      p.binders.emplace_back(rt, a.h(2));
      index = int32_t(p.binders.size() - 1);
    }
    write_i32(rt, p, kBinderTag);
    write_i32(rt, p, index);
    write_i64(rt, p, 0);
    return 0;
  }, S::kPartial, "objets Binder du processus uniquement");
  r.bind(pc, "nativeReadStrongBinder", "(J)Landroid/os/IBinder;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    int32_t tag = read_i32(p);
    int32_t index = read_i32(p);
    read_i64(p);
    if (tag != kBinderTag || index < 0 || size_t(index) >= p.binders.size()) return kNull;
    return p.binders[size_t(index)].get();
  }, S::kPartial, "objets Binder du processus uniquement");
  r.bind(pc, "nativeHasBinders", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(!parcel(rt, a.i64(0)).binders.empty());
  });
  r.bind(pc, "nativeHasBindersInRange", "(JII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(!parcel(rt, a.i64(0)).binders.empty());
  }, S::kPartial, "approximation : présence dans tout le Parcel");
  r.bind(pc, "nativeMarkForBinder", "(JLandroid/os/IBinder;)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(pc, "nativeMarkSensitive", "(J)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(pc, "nativeIsForRpc", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  // File descriptors are not transported.
  r.bind(pc, "nativeHasFileDescriptors", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(pc, "nativeHasFileDescriptorsInRange", "(JII)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(pc, "nativePushAllowFds", "(JZ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    bool previous = p.allow_fds;
    p.allow_fds = p.allow_fds && a.z(2);
    return rt::ret_bool(previous);
  });
  r.bind(pc, "nativeRestoreAllowFds", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    parcel(rt, a.i64(0)).allow_fds = a.z(2);
    return 0;
  });
  r.bind(pc, "nativeGetOpenAshmemSize", "(J)J", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  // Marshalling.
  r.bind(pc, "nativeMarshall", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    if (!p.binders.empty()) {
      rt.throw_new("Ljava/lang/RuntimeException;", "Tried to marshall a Parcel that contains objects (binders or FDs).");
    }
    return new_bytes(rt, p.data.data(), p.size);
  });
  r.bind(pc, "nativeUnmarshall", "(J[BII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& p = parcel(rt, a.i64(0));
    auto bytes = byte_slice(rt, a.h(2), a.i32(3), a.i32(4));
    p.data = std::move(bytes);
    p.size = uint32_t(p.data.size());
    p.pos = 0;
    p.binders.clear();
    return 0;
  });
  r.bind(pc, "nativeAppendFrom", "(JJII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& dst = parcel(rt, a.i64(0));
    NativeParcel& src = parcel(rt, a.i64(2));
    int32_t off = a.i32(4), len = a.i32(5);
    if (off < 0 || len < 0 || uint64_t(off) + uint64_t(len) > src.size) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "appendFrom : plage invalide");
    }
    if (!src.binders.empty()) rt.throw_unsupported("Landroid/os/Parcel;->appendFrom (Binder)", "Parcel contenant des Binder");
    std::vector<uint8_t> copy(src.data.begin() + off, src.data.begin() + off + len);
    uint8_t* at = grow(rt, dst, uint32_t(len));
    if (len) std::memcpy(at, copy.data(), size_t(len));
    return 0;
  });
  r.bind(pc, "nativeCompareData", "(JJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& x = parcel(rt, a.i64(0));
    NativeParcel& y = parcel(rt, a.i64(2));
    if (x.size != y.size) return rt::ret_int(x.size < y.size ? -1 : 1);
    int c = x.size ? std::memcmp(x.data.data(), y.data.data(), x.size) : 0;
    return rt::ret_int(c < 0 ? -1 : c > 0 ? 1 : 0);
  });
  r.bind(pc, "nativeCompareDataInRange", "(JIJII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    NativeParcel& x = parcel(rt, a.i64(0));
    int32_t xo = a.i32(2);
    NativeParcel& y = parcel(rt, a.i64(3));
    int32_t yo = a.i32(5), len = a.i32(6);
    if (xo < 0 || yo < 0 || len < 0 || uint64_t(xo) + len > x.size || uint64_t(yo) + len > y.size) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "compareData : plage invalide");
    }
    return rt::ret_bool(len == 0 || std::memcmp(x.data.data() + xo, y.data.data() + yo, size_t(len)) == 0);
  });
  r.bind(pc, "nativeSignalExceptionForError", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    rt.throw_new("Ljava/lang/RuntimeException;", "Parcel : erreur " + std::to_string(a.i32(0)));
  });
}

}  // namespace aa::bridge
