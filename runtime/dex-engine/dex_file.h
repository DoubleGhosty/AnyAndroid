#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Read-only, validated view of a DEX file (format 035-039). Every index and
// offset read from the file is checked before use; violations raise
// aa::FormatError. Reference: https://source.android.com/docs/core/runtime/dex-format
namespace aa::dex {

constexpr uint32_t kNoIndex = 0xFFFFFFFF;

enum AccessFlags : uint32_t {
  kAccPublic = 0x1,
  kAccPrivate = 0x2,
  kAccProtected = 0x4,
  kAccStatic = 0x8,
  kAccFinal = 0x10,
  kAccSynchronized = 0x20,
  kAccVolatile = 0x40,
  kAccBridge = 0x40,
  kAccTransient = 0x80,
  kAccVarargs = 0x80,
  kAccNative = 0x100,
  kAccInterface = 0x200,
  kAccAbstract = 0x400,
  kAccStrict = 0x800,
  kAccSynthetic = 0x1000,
  kAccAnnotation = 0x2000,
  kAccEnum = 0x4000,
  kAccConstructor = 0x10000,
  kAccDeclaredSynchronized = 0x20000,
};

struct Header {
  std::string version;  // "035", "037", ...
  uint32_t checksum = 0;
  std::array<uint8_t, 20> signature{};
  uint32_t file_size = 0, header_size = 0, endian_tag = 0;
  uint32_t link_size = 0, link_off = 0, map_off = 0;
  uint32_t string_ids_size = 0, string_ids_off = 0;
  uint32_t type_ids_size = 0, type_ids_off = 0;
  uint32_t proto_ids_size = 0, proto_ids_off = 0;
  uint32_t field_ids_size = 0, field_ids_off = 0;
  uint32_t method_ids_size = 0, method_ids_off = 0;
  uint32_t class_defs_size = 0, class_defs_off = 0;
  uint32_t data_size = 0, data_off = 0;
  // DEX 041 containers: file_size is the size of this section, offsets are
  // relative to the start of the container.
  uint32_t container_size = 0, header_offset = 0;
};

struct ProtoId {
  uint32_t shorty_idx;
  uint32_t return_type_idx;
  uint32_t parameters_off;
};

struct FieldId {
  uint16_t class_idx;
  uint16_t type_idx;
  uint32_t name_idx;
};

struct MethodId {
  uint16_t class_idx;
  uint16_t proto_idx;
  uint32_t name_idx;
};

struct ClassDef {
  uint32_t class_idx;
  uint32_t access_flags;
  uint32_t superclass_idx;
  uint32_t interfaces_off;
  uint32_t source_file_idx;
  uint32_t annotations_off;
  uint32_t class_data_off;
  uint32_t static_values_off;
};

struct EncodedField {
  uint32_t field_idx;
  uint32_t access_flags;
};

struct EncodedMethod {
  uint32_t method_idx;
  uint32_t access_flags;
  uint32_t code_off;
};

struct ClassData {
  std::vector<EncodedField> static_fields;
  std::vector<EncodedField> instance_fields;
  std::vector<EncodedMethod> direct_methods;
  std::vector<EncodedMethod> virtual_methods;
};

struct CatchHandler {
  std::vector<std::pair<uint32_t, uint32_t>> typed;  // (type_idx, handler address)
  std::optional<uint32_t> catch_all;
};

struct TryItem {
  uint32_t start_addr;
  uint32_t insn_count;
  uint32_t handler;  // index into CodeItem::handlers
};

struct CodeItem {
  uint16_t registers_size = 0;
  uint16_t ins_size = 0;
  uint16_t outs_size = 0;
  uint32_t debug_info_off = 0;
  std::vector<uint16_t> insns;
  std::vector<TryItem> tries;
  std::vector<CatchHandler> handlers;
};

struct EncodedValue {
  enum Type : uint8_t {
    kByte = 0x00,
    kShort = 0x02,
    kChar = 0x03,
    kInt = 0x04,
    kLong = 0x06,
    kFloat = 0x10,
    kDouble = 0x11,
    kMethodType = 0x15,
    kMethodHandle = 0x16,
    kString = 0x17,
    kType = 0x18,
    kField = 0x19,
    kMethod = 0x1a,
    kEnum = 0x1b,
    kArray = 0x1c,
    kAnnotation = 0x1d,
    kNull = 0x1e,
    kBoolean = 0x1f,
  };
  uint8_t type = kNull;
  // Integers are sign/zero-extended, floats hold raw IEEE bits, references
  // hold their index; an annotation holds its type index.
  uint64_t bits = 0;
  // Elements of an array; values of the elements of an annotation, named by
  // the string indices of `names`.
  std::vector<EncodedValue> array;
  std::vector<uint32_t> names;
};

// An annotation_item: visibility (0 build, 1 runtime, 2 system), type and
// elements (name string index, value).
struct Annotation {
  uint8_t visibility = 0;
  uint32_t type_idx = 0;
  std::vector<std::pair<uint32_t, EncodedValue>> elements;
};

struct FieldAnnotation {
  uint32_t field_idx = 0;
  std::vector<Annotation> annotations;
};

struct MethodAnnotation {
  uint32_t method_idx = 0;
  std::vector<Annotation> annotations;
};

struct ParameterAnnotation {
  uint32_t method_idx = 0;
  std::vector<std::vector<Annotation>> annotations;
};

struct AnnotationsDirectory {
  std::vector<Annotation> class_annotations;
  std::vector<FieldAnnotation> field_annotations;
  std::vector<MethodAnnotation> method_annotations;
  std::vector<ParameterAnnotation> parameter_annotations;
};

class DexFile {
 public:
  // Opens a single DEX file (035 to 040, or a DEX 041 container holding one
  // section). A container with several sections needs open_all().
  static std::unique_ptr<DexFile> open(std::vector<uint8_t> data, std::string location);
  // Opens every DEX of a file: one for 035-040, one per section for a DEX 041
  // container (as written by recent d8 for the platform jars). Sections share
  // the buffer and, when they do on disk, the string table.
  static std::vector<std::unique_ptr<DexFile>> open_all(std::vector<uint8_t> data, std::string location);

  const std::string& location() const { return location_; }
  const Header& header() const { return header_; }
  // Whole file (the whole container for DEX 041): every offset of the format
  // is relative to its start.
  std::span<const uint8_t> bytes() const { return data_; }
  uint32_t section_offset() const { return base_; }
  bool checksum_valid() const;
  bool signature_valid() const;

  uint32_t num_strings() const { return header_.string_ids_size; }
  const std::string& string(uint32_t idx) const;
  uint32_t num_types() const { return header_.type_ids_size; }
  const std::string& type_descriptor(uint32_t idx) const;
  uint32_t num_protos() const { return header_.proto_ids_size; }
  const ProtoId& proto(uint32_t idx) const;
  const std::vector<uint32_t>& proto_params(uint32_t idx) const;
  const std::string& proto_signature(uint32_t idx) const;  // "(ILjava/lang/String;)V"
  const std::string& proto_shorty(uint32_t idx) const;
  uint32_t num_fields() const { return header_.field_ids_size; }
  const FieldId& field(uint32_t idx) const;
  uint32_t num_methods() const { return header_.method_ids_size; }
  const MethodId& method(uint32_t idx) const;
  uint32_t num_class_defs() const { return header_.class_defs_size; }
  const ClassDef& class_def(uint32_t idx) const;
  std::optional<uint32_t> find_class_def(std::string_view descriptor) const;

  ClassData class_data(const ClassDef& def) const;
  CodeItem code_item(uint32_t offset) const;
  std::vector<uint32_t> type_list(uint32_t offset) const;
  std::vector<EncodedValue> encoded_array(uint32_t offset) const;
  // Complete annotations directory (class, fields, methods, parameters).
  AnnotationsDirectory annotations_directory(const ClassDef& def) const;
  // Annotations of the class itself (annotations_directory_item), such as the
  // system annotations dalvik.annotation.InnerClass, EnclosingClass,
  // EnclosingMethod and MemberClasses.
  std::vector<Annotation> class_annotations(const ClassDef& def) const;

  // Human-readable references: "Lcom/x/A;->f:I", "Lcom/x/A;->m(I)V".
  std::string pretty_field(uint32_t idx) const;
  std::string pretty_method(uint32_t idx) const;

 private:
  DexFile() = default;
  // Parses the header at base_. `shared` is the string table of a previous
  // section of the same container, reused when the tables coincide.
  void parse(const DexFile* shared);

  std::shared_ptr<const std::vector<uint8_t>> storage_;
  std::span<const uint8_t> data_;
  uint32_t base_ = 0;    // offset of this DEX (container section) in data_
  uint64_t limit_ = 0;   // end of the addressable data (file or container size)
  std::string location_;
  Header header_;
  std::shared_ptr<const std::vector<std::string>> strings_;
  std::vector<ProtoId> protos_;
  std::vector<std::vector<uint32_t>> proto_params_;
  std::vector<std::string> proto_signatures_;
  std::vector<FieldId> fields_;
  std::vector<MethodId> methods_;
  std::vector<ClassDef> class_defs_;
  std::unordered_map<std::string, uint32_t> class_def_by_descriptor_;
};

// Number of 32-bit argument words for a method signature such as "(IJ)V"
// (long/double take two words), not counting the receiver.
uint32_t arg_words(std::string_view signature);
// Splits "(I[Ljava/lang/String;)V" into parameter descriptors and return type.
bool split_signature(std::string_view signature, std::vector<std::string>& params, std::string& ret);

}  // namespace aa::dex
