#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "resources/res_config.h"

// resources.arsc (ResTable) and the string pools shared with compiled XML,
// read in place from an immutable buffer. Every offset is bounds-checked:
// tables come from untrusted APKs.
namespace aa::res {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

// Res_value data types.
enum : uint8_t {
  kTypeNull = 0x00,
  kTypeReference = 0x01,
  kTypeAttribute = 0x02,
  kTypeString = 0x03,
  kTypeFloat = 0x04,
  kTypeDimension = 0x05,
  kTypeFraction = 0x06,
  kTypeDynamicReference = 0x07,
  kTypeDynamicAttribute = 0x08,
  kTypeFirstInt = 0x10,
  kTypeIntBoolean = 0x12,
  kTypeLastInt = 0x1f,
};
constexpr uint32_t kDataNullUndefined = 0;
constexpr uint32_t kDataNullEmpty = 1;

struct Value {
  uint8_t type = kTypeNull;
  uint32_t data = 0;
};

// ResStringPool: UTF-8 or UTF-16 strings and their style spans, decoded on
// demand.
class StringPool {
 public:
  // The chunk is [offset, offset + size) of `bytes`. Throws FormatError.
  static std::shared_ptr<StringPool> parse(Bytes bytes, size_t offset, size_t size);

  uint32_t size() const { return count_; }
  bool utf8() const { return (flags_ & 0x100) != 0; }
  std::optional<std::u16string> string16(uint32_t index) const;
  std::optional<std::string> string8(uint32_t index) const;
  // Spans of a styled string as (name, first char, last char) triples.
  std::vector<uint32_t> style(uint32_t index) const;
  // Index of an exact string, through an index built on first use.
  std::optional<uint32_t> find(std::u16string_view s) const;

 private:
  StringPool() = default;
  std::optional<std::span<const uint8_t>> raw(uint32_t index, size_t* chars) const;

  Bytes bytes_;
  std::span<const uint8_t> chunk_;
  uint32_t count_ = 0, style_count_ = 0, flags_ = 0, strings_start_ = 0, styles_start_ = 0;
  size_t index_offset_ = 0;  // offset of the string offsets array in chunk_
  mutable std::mutex lock_;
  mutable std::unique_ptr<std::unordered_map<std::u16string, uint32_t>> lookup_;
};

// ResTable_entry, decoded.
struct Entry {
  uint32_t key = 0;  // index in the package's key strings
  uint16_t flags = 0;
  bool complex = false;
  Value value;  // simple entries
  uint32_t parent = 0;  // complex entries (bags)
  std::vector<std::pair<uint32_t, Value>> map;
};

// One ResTable_type chunk: the entries of a type for one configuration.
struct TypeVariant {
  ResConfig config;
  uint8_t flags = 0;  // 0x01 sparse, 0x02 16-bit offsets
  uint32_t entry_count = 0;
  std::span<const uint8_t> offsets;
  std::span<const uint8_t> entries;
  // Offset of an entry in `entries`, if this variant defines it.
  std::optional<uint32_t> entry_offset(uint32_t index) const;
};

struct TypeGroup {
  uint8_t id = 0;
  std::vector<uint32_t> spec_flags;  // per entry (ResTable_typeSpec)
  std::vector<TypeVariant> variants;
};

class LoadedPackage {
 public:
  uint32_t id = 0;  // 0x01 framework, 0x7f applications, 0 shared library
  std::u16string name;
  std::shared_ptr<StringPool> type_strings, key_strings;
  uint32_t type_id_offset = 0;
  std::vector<TypeGroup> types;  // indexed by type id - 1 (empty groups for gaps)
  std::vector<std::pair<uint32_t, std::u16string>> libraries;  // ResTable_lib_entry

  const TypeGroup* type(uint8_t type_id) const;
  // Decodes the entry at `offset` of a variant (nullopt if malformed).
  std::optional<Entry> read_entry(const TypeVariant& v, uint32_t offset) const;
  std::optional<std::u16string> type_name(uint8_t type_id) const;
  // Resource id (with this package's id) of type/entry, through a name index
  // built on first use.
  std::optional<uint32_t> find_entry_by_name(std::u16string_view type, std::u16string_view entry) const;

 private:
  mutable std::mutex lock_;
  mutable std::unordered_map<std::u16string, std::unordered_map<std::u16string, uint32_t>> names_;
  mutable bool names_built_ = false;
};

class LoadedArsc {
 public:
  // Throws FormatError on a malformed table.
  static std::shared_ptr<LoadedArsc> load(Bytes data);

  const std::shared_ptr<StringPool>& values() const { return values_; }
  const std::vector<std::unique_ptr<LoadedPackage>>& packages() const { return packages_; }

 private:
  Bytes data_;
  std::shared_ptr<StringPool> values_;
  std::vector<std::unique_ptr<LoadedPackage>> packages_;
};

}  // namespace aa::res
