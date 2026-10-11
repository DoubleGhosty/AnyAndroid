#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "dex-engine/runtime.h"

// Emulated native memory. Whatever Java code holds as a "native address"
// (Unsafe.allocateMemory, VMRuntime.addressOf, DirectByteBuffer.address,
// the C strings and buffers that NIO passes to its natives, Os.mmap) is a
// tagged value naming a region and an offset, never a host pointer:
//
//   bits 60-63  0x5 (tag; the value stays positive)
//   bits 52-59  generation of the region slot (stale addresses are rejected)
//   bits 32-51  region id
//   bits  0-31  offset in the region
//
// Java code may do pointer arithmetic (address + offset) within a region;
// every access is checked against the region's bounds. A forged, stale or
// out-of-range address raises a Java exception instead of reaching memory
// (a freed slot is reused only after thousands of others, so that its 8-bit
// generation does not soon revive stale addresses).
// The bytes owned by regions count against a global budget (1 GiB by
// default, set_native_memory_limit): beyond it, as when the host itself runs
// out of memory, allocations fail with OutOfMemoryError.
namespace aa::bridge {

// Region of `size` zeroed bytes owned by native code (Unsafe.allocateMemory).
int64_t native_alloc(rt::Runtime& rt, uint64_t size);
// Frees a region; `address` must be its base (offset 0).
void native_free(rt::Runtime& rt, int64_t address);
// Same without exception: false (nothing freed) unless `address` is the base
// of a live region owned by native code (munmap reports EINVAL instead).
bool native_try_free(int64_t address);
// Unsafe.reallocateMemory: a new region holding the old bytes; 0 frees.
int64_t native_realloc(rt::Runtime& rt, int64_t address, uint64_t size);
// Region over the elements of a primitive Java array (VMRuntime.addressOf).
// The same array always gets the same address while it is alive; the region
// is released with the array.
int64_t native_address_of(rt::Runtime& rt, rt::Handle array);
// Region holding `data` (mappings: Os.mmap, FileChannel.map).
int64_t native_wrap(rt::Runtime& rt, std::vector<uint8_t> data, bool writable);
// Read-only region of `size` bytes produced by `load` on first access, as the
// pages of a file mapping are read when touched (and only then counted
// against the budget). `load` returns exactly `size` bytes or throws; a
// failure is reported to the accessing Java code as InternalError, as a
// fault in an unsafe memory access.
int64_t native_wrap_lazy(rt::Runtime& rt, uint64_t size, std::function<std::vector<uint8_t>()> load);

bool is_native_address(int64_t address);
// `size` bytes at `address`; throws (IllegalArgumentException for an unknown
// address, IndexOutOfBoundsException past the end of its region,
// ReadOnlyBufferException when `write` targets a read-only region).
std::span<uint8_t> native_span(rt::Runtime& rt, int64_t address, uint64_t size, bool write = false);
// Bytes from `address` up to the first NUL (at most `max`, within the region).
std::string native_cstring(rt::Runtime& rt, int64_t address, size_t max = 4096);
// The Java array behind a VMRuntime.addressOf value (offset 0 only).
rt::ArrayObject& native_array(rt::Runtime& rt, int64_t address);

// Global budget of the bytes owned by regions (arrays are not counted: they
// belong to the Java heap).
struct NativeMemoryStats {
  uint64_t used = 0;   // bytes owned by live regions
  uint64_t limit = 0;  // budget
  size_t regions = 0;  // live regions, arrays included
};
NativeMemoryStats native_memory_stats();
void set_native_memory_limit(uint64_t bytes);
// Whether `size` more bytes fit in the budget now (lets a caller report ENOMEM
// before building a large buffer).
bool native_memory_fits(uint64_t size);

}  // namespace aa::bridge
