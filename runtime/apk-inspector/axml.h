#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/bytes.h"

// Android binary XML ("AXML", ResXMLTree) as produced by aapt/aapt2 for
// AndroidManifest.xml and compiled layouts.
namespace aa::apk {

inline constexpr std::string_view kAndroidNs = "http://schemas.android.com/apk/res/android";

// Res_value data types (frameworks/base/libs/androidfw/ResourceTypes.h).
enum ResType : uint8_t {
  kResNull = 0x00,
  kResReference = 0x01,
  kResAttribute = 0x02,
  kResString = 0x03,
  kResFloat = 0x04,
  kResDimension = 0x05,
  kResFraction = 0x06,
  kResIntDec = 0x10,
  kResIntHex = 0x11,
  kResIntBoolean = 0x12,
  kResColorArgb8 = 0x1c,
  kResColorRgb8 = 0x1d,
  kResColorArgb4 = 0x1e,
  kResColorRgb4 = 0x1f,
};

// Chunk types used by AXML and resources.arsc.
enum ChunkType : uint16_t {
  kChunkStringPool = 0x0001,
  kChunkTable = 0x0002,
  kChunkXml = 0x0003,
  kChunkXmlStartNamespace = 0x0100,
  kChunkXmlEndNamespace = 0x0101,
  kChunkXmlStartElement = 0x0102,
  kChunkXmlEndElement = 0x0103,
  kChunkXmlCdata = 0x0104,
  kChunkXmlResourceMap = 0x0180,
  kChunkTablePackage = 0x0200,
  kChunkTableType = 0x0201,
  kChunkTableTypeSpec = 0x0202,
};

struct ResValue {
  uint8_t type = kResNull;
  uint32_t data = 0;
};

// Decoded string pool (UTF-8 or UTF-16 encoded in the file; UTF-8 here).
class StringPool {
 public:
  // `chunk` covers exactly one RES_STRING_POOL_TYPE chunk.
  static StringPool parse(std::span<const uint8_t> chunk, uint32_t max_strings);
  size_t size() const { return strings_.size(); }
  const std::string& at(uint32_t index) const;  // throws FormatError
  bool valid(uint32_t index) const { return index < strings_.size(); }

 private:
  std::vector<std::string> strings_;
};

struct XmlAttribute {
  std::string ns;
  std::string name;
  uint32_t resource_id = 0;  // from the resource map, 0 if none
  std::string raw;           // raw string value if present
  ResValue value;
  std::string string_value;  // decoded string for kResString
  std::string display() const;
  bool as_bool(bool fallback) const;
  int64_t as_int(int64_t fallback) const;
};

struct XmlElement {
  std::string ns;
  std::string name;
  uint32_t line = 0;
  std::vector<XmlAttribute> attributes;
  std::vector<std::unique_ptr<XmlElement>> children;

  // Looks an attribute up by local name in the android namespace (falling
  // back to the well-known resource id when the name was stripped).
  const XmlAttribute* android_attr(std::string_view name) const;
  // Looks an attribute up by local name without namespace.
  const XmlAttribute* plain_attr(std::string_view name) const;
};

struct XmlDocument {
  std::unique_ptr<XmlElement> root;
  uint32_t element_count = 0;
};

struct AxmlLimits {
  uint32_t max_depth = 64;
  uint32_t max_elements = 200000;
  uint32_t max_strings = 500000;
  uint32_t max_attributes = 256;
};

XmlDocument parse_axml(std::span<const uint8_t> data, const AxmlLimits& limits = {});

// Resource id of the well-known android:<name> attribute, 0 if unknown.
uint32_t android_attr_id(std::string_view name);
std::string_view android_attr_name(uint32_t id);

// Renders an element tree as indented text XML (diagnostics).
std::string to_text_xml(const XmlElement& element, int indent = 0);

}  // namespace aa::apk
