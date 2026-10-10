#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace aa::bridge {

// Native objects handed to Java as `long` values (AssetManager.mObject,
// XmlBlock.mNative...). Handles are never pointers: they carry a kind tag, a
// generation and a slot index, so a forged, stale or mistyped value is
// rejected instead of reaching memory.
template <typename T>
class HandleTable {
 public:
  explicit HandleTable(uint16_t kind) : kind_(kind & 0x7fff) {}

  int64_t add(std::shared_ptr<T> value) {
    std::lock_guard<std::mutex> guard(lock_);
    uint32_t index;
    if (!free_.empty()) {
      index = free_.back();
      free_.pop_back();
    } else {
      index = uint32_t(slots_.size());
      slots_.emplace_back();
    }
    slots_[index].value = std::move(value);
    ++live_;
    return (int64_t(kind_) << 48) | (int64_t(slots_[index].gen) << 32) | int64_t(index + 1);
  }

  std::shared_ptr<T> get(int64_t handle) const {
    std::lock_guard<std::mutex> guard(lock_);
    const Slot* s = slot(handle);
    return s ? s->value : nullptr;
  }

  bool remove(int64_t handle) {
    std::lock_guard<std::mutex> guard(lock_);
    Slot* s = const_cast<Slot*>(slot(handle));
    if (!s) return false;
    s->value.reset();
    ++s->gen;
    free_.push_back(uint32_t(handle & 0xffffffff) - 1);
    --live_;
    return true;
  }

  size_t live() const {
    std::lock_guard<std::mutex> guard(lock_);
    return live_;
  }

 private:
  struct Slot {
    std::shared_ptr<T> value;
    uint16_t gen = 1;
  };

  const Slot* slot(int64_t handle) const {
    uint64_t h = uint64_t(handle);
    if ((h >> 48) != kind_) return nullptr;
    uint32_t index = uint32_t(h & 0xffffffff);
    if (index == 0 || index > slots_.size()) return nullptr;
    const Slot& s = slots_[index - 1];
    if (s.gen != uint16_t(h >> 32) || !s.value) return nullptr;
    return &s;
  }

  uint16_t kind_;
  mutable std::mutex lock_;
  std::vector<Slot> slots_;
  std::vector<uint32_t> free_;
  size_t live_ = 0;
};

}  // namespace aa::bridge
