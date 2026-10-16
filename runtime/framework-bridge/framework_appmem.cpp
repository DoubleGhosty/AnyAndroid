// com.android.internal.os.ApplicationSharedMemory and the nonce block of
// android.app.PropertyInvalidatedCache (ADR-006).
//
// On a phone, system_server creates an ashmem region that every application
// maps read-only: flags, the animator scale, the network time and the nonces
// that invalidate binder caches. AnyAndroid's process plays both roles, so
// the region is ordinary process memory behind bounded handles (one per
// mapping, one for its nonce block); the "file descriptor" is /dev/zero of
// the VFS, as an anonymous mapping's would be, and names the region: every
// mapping of it, or of a descriptor duplicated from it, shares its contents.
#include "framework-bridge/internal.h"

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr int32_t kMaxFlags = 32;             // ApplicationSharedMemory.MAX_FLAGS
constexpr int32_t kMaxNonce = 64;             // nonces of the PropertyInvalidatedCache block
constexpr int32_t kMaxByte = 8192;            // their name block
constexpr size_t kMaxFeatureVersions = 1024;  // SystemFeaturesCache (191 entries in Android 17)

struct SharedState {
  std::mutex lock;
  int32_t device_upgrading = -1;  // PM_DEVICE_UPGRADING_UNSET
  int64_t network_time = -1;      // INVALID_NETWORK_TIME
  float animator_scale = 1.0f;
  uint32_t bit_flags = 0;
  std::vector<int32_t> features;
  std::array<int64_t, kMaxNonce> nonces{};
  std::array<int8_t, kMaxByte> bytes{};
  int32_t byte_hash = 0;
  int64_t nonce_handle = 0;  // handle of this state in the nonce table, once asked for
  int32_t mapped = 0;        // live mappings
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<SharedState> mappings{0x1001};
  HandleTable<SharedState> nonce_blocks{0x1002};
  std::mutex lock;
  std::map<int32_t, std::shared_ptr<SharedState>> regions;  // by descriptor
};


Tables& tables() {
  static auto* t = new Tables;
  return *t;
}

// The region a descriptor names (a new one for an unknown descriptor).
std::shared_ptr<SharedState> region_of(int32_t fd) {
  Tables& t = tables();
  std::lock_guard<std::mutex> guard(t.lock);
  auto& region = t.regions[fd];
  if (!region) region = std::make_shared<SharedState>();
  return region;
}

std::shared_ptr<SharedState> mapping(Runtime& rt, int64_t h) {
  auto s = tables().mappings.get(h);
  if (!s) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] ApplicationSharedMemory non projetée");
  return s;
}

std::shared_ptr<SharedState> nonce_block(Runtime& rt, int64_t h) {
  auto s = tables().nonce_blocks.get(h);
  if (!s) rt.throw_new("Ljava/lang/IllegalArgumentException;", "[AnyAndroid] bloc de nonces invalide");
  return s;
}

int32_t open_zero(Runtime& rt, int32_t flags) {
  try {
    return libcore_vfs().open("/dev/zero", flags, 0);
  } catch (const vfs::VfsError& e) {
    throw_errno(rt, "open", e.err);
  }
}

void check_flag(Runtime& rt, int32_t bit) {
  if (bit < 0 || bit >= kMaxFlags) rt.throw_new("Ljava/lang/IllegalArgumentException;", "drapeau hors limites");
}

}  // namespace

void register_appmem_natives(rt::NativeRegistry& r) {
  const char* asm_ = "Lcom/android/internal/os/ApplicationSharedMemory;";
  r.bind(asm_, "nativeCreate", "()I", [](Runtime& rt, NativeArgs&) -> uint64_t {
    int32_t fd = open_zero(rt, vfs::kO_RDWR);
    Tables& t = tables();
    std::lock_guard<std::mutex> guard(t.lock);
    t.regions[fd] = std::make_shared<SharedState>();
    return rt::ret_int(fd);
  }, S::kPartial, "mémoire du processus (pas d'ashmem)");
  r.bind(asm_, "nativeDupAsReadOnly", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::shared_ptr<SharedState> region = region_of(a.i32(0));
    int32_t fd = open_zero(rt, vfs::kO_RDONLY);
    Tables& t = tables();
    std::lock_guard<std::mutex> guard(t.lock);
    t.regions[fd] = std::move(region);
    return rt::ret_int(fd);
  });
  r.bind(asm_, "nativeMap", "(IZ)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    std::shared_ptr<SharedState> region = region_of(a.i32(0));
    {
      std::lock_guard<std::mutex> guard(region->lock);
      ++region->mapped;
    }
    return uint64_t(tables().mappings.add(std::move(region)));
  });
  r.bind(asm_, "nativeUnmap", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    if (auto s = tables().mappings.get(a.i64(0))) {
      std::lock_guard<std::mutex> guard(s->lock);
      if (--s->mapped == 0 && s->nonce_handle) {
        tables().nonce_blocks.remove(s->nonce_handle);
        s->nonce_handle = 0;
      }
    }
    tables().mappings.remove(a.i64(0));
    return 0;
  });
  r.bind(asm_, "nativeInit", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    s->device_upgrading = -1;
    s->network_time = -1;
    s->animator_scale = 1.0f;
    s->bit_flags = 0;
    return 0;
  });
  r.bind(asm_, "nativeGetBitFlag", "(JI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    check_flag(rt, a.i32(2));
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    return rt::ret_bool((s->bit_flags >> a.i32(2)) & 1);
  });
  r.bind(asm_, "nativeSetBitFlag", "(JIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    check_flag(rt, a.i32(2));
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    uint32_t bit = 1u << a.i32(2);
    s->bit_flags = a.z(3) ? (s->bit_flags | bit) : (s->bit_flags & ~bit);
    return 0;
  });
  r.bind(asm_, "nativeGetCurrentAnimatorScale", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    return rt::ret_float(s->animator_scale);
  });
  r.bind(asm_, "nativeSetCurrentAnimatorScale", "(JF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    s->animator_scale = a.f32(2);
    return 0;
  });
  r.bind(asm_, "nativeGetIsDeviceUpgrading", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    return rt::ret_int(s->device_upgrading);
  });
  r.bind(asm_, "nativeSetIsDeviceUpgrading", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    s->device_upgrading = a.z(2) ? 1 : 0;
    return 0;
  });
  r.bind(asm_, "nativeGetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis", "(J)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = mapping(rt, a.i64(0));
           std::lock_guard<std::mutex> guard(s->lock);
           return rt::ret_long(s->network_time);
         });
  r.bind(asm_, "nativeSetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis", "(JJ)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = mapping(rt, a.i64(0));
           std::lock_guard<std::mutex> guard(s->lock);
           s->network_time = a.i64(2);
           return 0;
         });
  r.bind(asm_, "nativeGetSystemNonceBlock", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    if (!s->nonce_handle) s->nonce_handle = tables().nonce_blocks.add(s);
    return uint64_t(s->nonce_handle);
  });
  r.bind(asm_, "nativeWriteSystemFeaturesCache", "(J[I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    if (a.h(2) == kNull) rt.throw_npe("features");
    rt::ArrayObject& arr = rt.array(a.h(2), "int[]");
    if (arr.length > kMaxFeatureVersions) rt.throw_new("Ljava/lang/IllegalArgumentException;", "trop de fonctionnalités");
    std::lock_guard<std::mutex> guard(s->lock);
    s->features.resize(arr.length);
    if (arr.length) std::memcpy(s->features.data(), arr.data.data(), size_t(arr.length) * 4);
    return 0;
  });
  r.bind(asm_, "nativeReadSystemFeaturesCache", "(J)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = mapping(rt, a.i64(0));
    std::vector<int32_t> features;
    {
      std::lock_guard<std::mutex> guard(s->lock);
      features = s->features;
    }
    Handle out = rt.new_array("[I", int32_t(features.size()));
    if (!features.empty()) std::memcpy(rt.array(out, "int[]").data.data(), features.data(), features.size() * 4);
    return out;
  });

  // PropertyInvalidatedCache's nonce block: nonces by index, and the block of
  // names with the hash its writer computed (Arrays.hashCode).
  const char* pic = "Landroid/app/PropertyInvalidatedCache;";
  r.bind(pic, "nativeGetMaxNonce", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    nonce_block(rt, a.i64(0));
    return rt::ret_int(kMaxNonce);
  });
  r.bind(pic, "nativeGetMaxByte", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    nonce_block(rt, a.i64(0));
    return rt::ret_int(kMaxByte);
  });
  r.bind(pic, "nativeGetNonce", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = nonce_block(rt, a.i64(0));
    int32_t i = a.i32(2);
    if (i < 0 || i >= kMaxNonce) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(i));
    std::lock_guard<std::mutex> guard(s->lock);
    return rt::ret_long(s->nonces[size_t(i)]);
  });
  r.bind(pic, "nativeSetNonce", "(JIJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = nonce_block(rt, a.i64(0));
    int32_t i = a.i32(2);
    if (i < 0 || i >= kMaxNonce) return rt::ret_bool(false);
    std::lock_guard<std::mutex> guard(s->lock);
    s->nonces[size_t(i)] = a.i64(3);
    return rt::ret_bool(true);
  });
  r.bind(pic, "nativeGetByteBlockHash", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = nonce_block(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(s->lock);
    return rt::ret_int(s->byte_hash);
  });
  r.bind(pic, "nativeGetByteBlock", "(JI[B)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = nonce_block(rt, a.i64(0));
    if (a.h(3) == kNull) rt.throw_npe("block");
    rt::ArrayObject& arr = rt.array(a.h(3), "byte[]");
    std::lock_guard<std::mutex> guard(s->lock);
    std::memcpy(arr.data.data(), s->bytes.data(), std::min<size_t>(arr.length, s->bytes.size()));
    return rt::ret_int(s->byte_hash);
  });
  r.bind(pic, "nativeSetByteBlock", "(JI[B)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = nonce_block(rt, a.i64(0));
    if (a.h(3) == kNull) rt.throw_npe("block");
    rt::ArrayObject& arr = rt.array(a.h(3), "byte[]");
    std::lock_guard<std::mutex> guard(s->lock);
    s->bytes.fill(0);
    std::memcpy(s->bytes.data(), arr.data.data(), std::min<size_t>(arr.length, s->bytes.size()));
    s->byte_hash = a.i32(2);
    return 0;
  });
}

}  // namespace aa::bridge
