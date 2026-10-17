#include "dex-engine/dex_file.h"

#include "common/bytes.h"
#include "common/hash.h"
#include "common/text.h"

namespace aa::dex {

namespace {

constexpr uint32_t kEndianConstant = 0x12345678;
constexpr int kMaxEncodedDepth = 16;

void check_section(uint64_t limit, uint32_t count, uint32_t off, uint32_t item_size, const char* name) {
  if (count == 0) return;
  if (off % 4 != 0) throw_format(std::string("section ") + name + " non alignée");
  if (uint64_t(off) + uint64_t(count) * item_size > limit) throw_format(std::string("section ") + name + " hors limites");
}

int dex_version(std::span<const uint8_t> d, size_t at) {
  if (d.size() < at + 8) throw_format("fichier DEX trop court");
  const uint8_t* m = d.data() + at;
  if (m[0] != 'd' || m[1] != 'e' || m[2] != 'x' || m[3] != '\n' || m[7] != 0) throw_format("signature DEX absente");
  int version = 0;
  for (int i = 4; i < 7; ++i) {
    if (m[i] < '0' || m[i] > '9') throw_format("version DEX invalide");
    version = version * 10 + (m[i] - '0');
  }
  return version;
}

void read_encoded_value(ByteReader& r, EncodedValue& out, int depth);

void read_encoded_array(ByteReader& r, std::vector<EncodedValue>& out, int depth) {
  if (depth > kMaxEncodedDepth) throw_format("tableau encodé trop imbriqué");
  uint32_t size = r.uleb128();
  if (size > r.remaining()) throw_format("taille de tableau encodé invalide");
  out.resize(size);
  for (auto& v : out) read_encoded_value(r, v, depth + 1);
}

void read_encoded_value(ByteReader& r, EncodedValue& out, int depth) {
  uint8_t b = r.u8();
  uint8_t arg = b >> 5;
  out.type = b & 0x1f;
  auto read_bytes = [&](uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; ++i) v |= uint64_t(r.u8()) << (8 * i);
    return v;
  };
  auto sign_extend = [](uint64_t v, uint32_t n) {
    if (n < 8 && (v >> (8 * n - 1)) & 1) v |= ~uint64_t(0) << (8 * n);
    return v;
  };
  switch (out.type) {
    case EncodedValue::kByte:
      if (arg != 0) throw_format("valeur encodée byte invalide");
      out.bits = sign_extend(read_bytes(1), 1);
      break;
    case EncodedValue::kShort:
    case EncodedValue::kInt:
    case EncodedValue::kLong: {
      uint32_t n = arg + 1u;
      if ((out.type == EncodedValue::kShort && n > 2) || (out.type == EncodedValue::kInt && n > 4)) {
        throw_format("valeur encodée trop longue");
      }
      out.bits = sign_extend(read_bytes(n), n);
      break;
    }
    case EncodedValue::kChar:
      if (arg > 1) throw_format("valeur encodée char invalide");
      out.bits = read_bytes(arg + 1u);
      break;
    case EncodedValue::kFloat:
    case EncodedValue::kDouble: {
      uint32_t width = out.type == EncodedValue::kFloat ? 4 : 8;
      uint32_t n = arg + 1u;
      if (n > width) throw_format("valeur flottante encodée trop longue");
      out.bits = read_bytes(n) << (8 * (width - n));  // zero-extended to the right
      break;
    }
    case EncodedValue::kMethodType:
    case EncodedValue::kMethodHandle:
    case EncodedValue::kString:
    case EncodedValue::kType:
    case EncodedValue::kField:
    case EncodedValue::kMethod:
    case EncodedValue::kEnum:
      if (arg > 3) throw_format("index encodé trop long");
      out.bits = read_bytes(arg + 1u);
      break;
    case EncodedValue::kArray:
      read_encoded_array(r, out.array, depth);
      break;
    case EncodedValue::kAnnotation: {
      if (depth > kMaxEncodedDepth) throw_format("annotation trop imbriquée");
      r.uleb128();  // type_idx
      uint32_t size = r.uleb128();
      if (size > r.remaining()) throw_format("annotation encodée invalide");
      for (uint32_t i = 0; i < size; ++i) {
        r.uleb128();  // name_idx
        EncodedValue ignored;
        read_encoded_value(r, ignored, depth + 1);
      }
      break;
    }
    case EncodedValue::kNull:
      break;
    case EncodedValue::kBoolean:
      out.bits = arg & 1;
      break;
    default:
      throw_format("type de valeur encodée inconnu " + hex(out.type));
  }
}

}  // namespace

std::unique_ptr<DexFile> DexFile::open(std::vector<uint8_t> data, std::string location) {
  auto all = open_all(std::move(data), std::move(location));
  if (all.size() != 1) {
    throw_format("conteneur DEX 041 de " + std::to_string(all.size()) + " sections : utiliser DexFile::open_all");
  }
  return std::move(all.front());
}

std::vector<std::unique_ptr<DexFile>> DexFile::open_all(std::vector<uint8_t> data, std::string location) {
  auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(data));
  std::span<const uint8_t> bytes(*storage);
  std::vector<std::unique_ptr<DexFile>> out;
  uint64_t base = 0;
  do {
    if (out.size() >= 256) throw_format("conteneur DEX : trop de sections");
    std::unique_ptr<DexFile> dex(new DexFile());
    dex->storage_ = storage;
    dex->data_ = bytes;
    dex->base_ = uint32_t(base);
    dex->location_ = out.empty() ? location : location + "#" + std::to_string(out.size());
    dex->parse(out.empty() ? nullptr : out.back().get());
    bool container = dex->header_.version == "041";
    base += dex->header_.file_size;
    out.push_back(std::move(dex));
    if (!container) break;
  } while (base < bytes.size());
  return out;
}

void DexFile::parse(const DexFile* shared) {
  int version = dex_version(data_, base_);
  header_.version.assign(reinterpret_cast<const char*>(data_.data()) + base_ + 4, 3);
  if (version < 35 || version > 41) throw_format("version DEX " + header_.version + " non prise en charge (035 à 041)");
  bool container = version == 41;
  size_t header_size = container ? 0x78 : 0x70;
  if (data_.size() - base_ < header_size) throw_format("fichier DEX trop court");

  ByteReader r(data_, base_ + 8);
  Header& h = header_;
  h.checksum = r.u32();
  auto sig = r.bytes(20);
  std::copy(sig.begin(), sig.end(), h.signature.begin());
  h.file_size = r.u32();
  h.header_size = r.u32();
  h.endian_tag = r.u32();
  h.link_size = r.u32();
  h.link_off = r.u32();
  h.map_off = r.u32();
  h.string_ids_size = r.u32();
  h.string_ids_off = r.u32();
  h.type_ids_size = r.u32();
  h.type_ids_off = r.u32();
  h.proto_ids_size = r.u32();
  h.proto_ids_off = r.u32();
  h.field_ids_size = r.u32();
  h.field_ids_off = r.u32();
  h.method_ids_size = r.u32();
  h.method_ids_off = r.u32();
  h.class_defs_size = r.u32();
  h.class_defs_off = r.u32();
  h.data_size = r.u32();
  h.data_off = r.u32();
  if (container) {
    h.container_size = r.u32();
    h.header_offset = r.u32();
  }

  if (h.endian_tag != kEndianConstant) throw_format("DEX big-endian non pris en charge");
  if (h.header_size != header_size) throw_format("taille d'en-tête DEX invalide");
  if (container) {
    // Every section repeats the container size and its own offset; all
    // offsets are relative to the start of the container.
    if (h.header_offset != base_ || h.container_size != data_.size()) throw_format("en-tête de conteneur DEX incohérent");
    if (h.file_size < header_size || uint64_t(base_) + h.file_size > h.container_size) {
      throw_format("taille de section DEX incohérente");
    }
    limit_ = h.container_size;
  } else {
    if (h.file_size > data_.size() || h.file_size < header_size) throw_format("taille de fichier DEX incohérente");
    data_ = data_.first(h.file_size);
    limit_ = h.file_size;
  }
  r = ByteReader(data_);

  check_section(limit_, h.string_ids_size, h.string_ids_off, 4, "string_ids");
  check_section(limit_, h.type_ids_size, h.type_ids_off, 4, "type_ids");
  check_section(limit_, h.proto_ids_size, h.proto_ids_off, 12, "proto_ids");
  check_section(limit_, h.field_ids_size, h.field_ids_off, 8, "field_ids");
  check_section(limit_, h.method_ids_size, h.method_ids_off, 8, "method_ids");
  check_section(limit_, h.class_defs_size, h.class_defs_off, 32, "class_defs");
  if (h.type_ids_size > 65535) throw_format("trop de types");
  if (h.proto_ids_size > 65535) throw_format("trop de prototypes");

  if (shared && shared->header_.string_ids_off == h.string_ids_off &&
      shared->header_.string_ids_size == h.string_ids_size) {
    strings_ = shared->strings_;  // table shared by the sections of a container
  } else {
    auto strings = std::make_shared<std::vector<std::string>>();
    strings->reserve(h.string_ids_size);
    for (uint32_t i = 0; i < h.string_ids_size; ++i) {
      uint32_t off = r.u32_at(h.string_ids_off + 4ull * i);
      if (off >= limit_) throw_format("chaîne " + std::to_string(i) + " hors limites");
      ByteReader s(data_, off);
      uint32_t utf16_size = s.uleb128();
      std::u16string u = text::mutf8_to_utf16(data_.subspan(s.pos()));
      if (u.size() != utf16_size) throw_format("longueur de chaîne " + std::to_string(i) + " incohérente");
      strings->push_back(text::utf16_to_utf8(u));
    }
    strings_ = std::move(strings);
  }

  for (uint32_t i = 0; i < h.type_ids_size; ++i) {
    if (r.u32_at(h.type_ids_off + 4ull * i) >= h.string_ids_size) throw_format("type " + std::to_string(i) + " invalide");
  }

  protos_.resize(h.proto_ids_size);
  proto_params_.resize(h.proto_ids_size);
  proto_signatures_.resize(h.proto_ids_size);
  for (uint32_t i = 0; i < h.proto_ids_size; ++i) {
    ByteReader p = r.sub(h.proto_ids_off + 12ull * i, 12);
    ProtoId& proto = protos_[i];
    proto.shorty_idx = p.u32();
    proto.return_type_idx = p.u32();
    proto.parameters_off = p.u32();
    if (proto.shorty_idx >= h.string_ids_size || proto.return_type_idx >= h.type_ids_size) {
      throw_format("prototype " + std::to_string(i) + " invalide");
    }
    if (proto.parameters_off) proto_params_[i] = type_list(proto.parameters_off);
    std::string signature = "(";
    for (uint32_t t : proto_params_[i]) signature += type_descriptor(t);
    signature += ")" + type_descriptor(proto.return_type_idx);
    proto_signatures_[i] = std::move(signature);
  }

  fields_.resize(h.field_ids_size);
  for (uint32_t i = 0; i < h.field_ids_size; ++i) {
    ByteReader f = r.sub(h.field_ids_off + 8ull * i, 8);
    FieldId& fid = fields_[i];
    fid.class_idx = f.u16();
    fid.type_idx = f.u16();
    fid.name_idx = f.u32();
    if (fid.class_idx >= h.type_ids_size || fid.type_idx >= h.type_ids_size || fid.name_idx >= h.string_ids_size) {
      throw_format("champ " + std::to_string(i) + " invalide");
    }
  }

  methods_.resize(h.method_ids_size);
  for (uint32_t i = 0; i < h.method_ids_size; ++i) {
    ByteReader m = r.sub(h.method_ids_off + 8ull * i, 8);
    MethodId& mid = methods_[i];
    mid.class_idx = m.u16();
    mid.proto_idx = m.u16();
    mid.name_idx = m.u32();
    if (mid.class_idx >= h.type_ids_size || mid.proto_idx >= h.proto_ids_size || mid.name_idx >= h.string_ids_size) {
      throw_format("méthode " + std::to_string(i) + " invalide");
    }
  }

  class_defs_.resize(h.class_defs_size);
  for (uint32_t i = 0; i < h.class_defs_size; ++i) {
    ByteReader c = r.sub(h.class_defs_off + 32ull * i, 32);
    ClassDef& d = class_defs_[i];
    d.class_idx = c.u32();
    d.access_flags = c.u32();
    d.superclass_idx = c.u32();
    d.interfaces_off = c.u32();
    d.source_file_idx = c.u32();
    d.annotations_off = c.u32();
    d.class_data_off = c.u32();
    d.static_values_off = c.u32();
    if (d.class_idx >= h.type_ids_size || (d.superclass_idx != kNoIndex && d.superclass_idx >= h.type_ids_size)) {
      throw_format("définition de classe " + std::to_string(i) + " invalide");
    }
    if (!class_def_by_descriptor_.emplace(type_descriptor(d.class_idx), i).second) {
      throw_format("classe définie deux fois : " + text::printable(type_descriptor(d.class_idx)));
    }
  }
}

bool DexFile::checksum_valid() const {
  return hash::adler32(data_.subspan(base_ + 12, header_.file_size - 12)) == header_.checksum;
}

bool DexFile::signature_valid() const {
  auto digest = hash::sha1(data_.subspan(base_ + 32, header_.file_size - 32));
  return digest == header_.signature;
}

const std::string& DexFile::string(uint32_t idx) const {
  if (idx >= strings_->size()) throw_format("index de chaîne DEX invalide " + std::to_string(idx));
  return (*strings_)[idx];
}

const std::string& DexFile::type_descriptor(uint32_t idx) const {
  if (idx >= header_.type_ids_size) throw_format("index de type DEX invalide " + std::to_string(idx));
  return (*strings_)[load_u32(data_.data() + header_.type_ids_off + 4ull * idx)];
}

const ProtoId& DexFile::proto(uint32_t idx) const {
  if (idx >= protos_.size()) throw_format("index de prototype invalide " + std::to_string(idx));
  return protos_[idx];
}

const std::vector<uint32_t>& DexFile::proto_params(uint32_t idx) const {
  proto(idx);
  return proto_params_[idx];
}

const std::string& DexFile::proto_signature(uint32_t idx) const {
  proto(idx);
  return proto_signatures_[idx];
}

const std::string& DexFile::proto_shorty(uint32_t idx) const { return string(proto(idx).shorty_idx); }

const FieldId& DexFile::field(uint32_t idx) const {
  if (idx >= fields_.size()) throw_format("index de champ invalide " + std::to_string(idx));
  return fields_[idx];
}

const MethodId& DexFile::method(uint32_t idx) const {
  if (idx >= methods_.size()) throw_format("index de méthode invalide " + std::to_string(idx));
  return methods_[idx];
}

const ClassDef& DexFile::class_def(uint32_t idx) const {
  if (idx >= class_defs_.size()) throw_format("index de classe invalide " + std::to_string(idx));
  return class_defs_[idx];
}

std::optional<uint32_t> DexFile::find_class_def(std::string_view descriptor) const {
  auto it = class_def_by_descriptor_.find(std::string(descriptor));
  if (it == class_def_by_descriptor_.end()) return std::nullopt;
  return it->second;
}

ClassData DexFile::class_data(const ClassDef& def) const {
  ClassData cd;
  if (def.class_data_off == 0) return cd;
  if (def.class_data_off >= limit_) throw_format("class_data hors limites");
  ByteReader r(std::span<const uint8_t>(data_), def.class_data_off);
  uint32_t counts[4];
  for (auto& c : counts) {
    c = r.uleb128();
    if (c > r.remaining()) throw_format("class_data : nombre de membres invalide");
  }
  auto read_fields = [&](uint32_t n, std::vector<EncodedField>& out) {
    uint32_t idx = 0;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t diff = r.uleb128();
      idx = i == 0 ? diff : idx + diff;
      if (idx >= fields_.size()) throw_format("class_data : index de champ invalide");
      out.push_back({idx, r.uleb128()});
    }
  };
  auto read_methods = [&](uint32_t n, std::vector<EncodedMethod>& out) {
    uint32_t idx = 0;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t diff = r.uleb128();
      idx = i == 0 ? diff : idx + diff;
      if (idx >= methods_.size()) throw_format("class_data : index de méthode invalide");
      EncodedMethod m{idx, r.uleb128(), r.uleb128()};
      if (m.code_off >= limit_) throw_format("class_data : code hors limites");
      out.push_back(m);
    }
  };
  read_fields(counts[0], cd.static_fields);
  read_fields(counts[1], cd.instance_fields);
  read_methods(counts[2], cd.direct_methods);
  read_methods(counts[3], cd.virtual_methods);
  return cd;
}

CodeItem DexFile::code_item(uint32_t offset) const {
  if (offset % 4 != 0) throw_format("code_item non aligné");
  ByteReader r(std::span<const uint8_t>(data_), offset);
  CodeItem code;
  code.registers_size = r.u16();
  code.ins_size = r.u16();
  code.outs_size = r.u16();
  uint16_t tries_size = r.u16();
  code.debug_info_off = r.u32();
  uint32_t insns_size = r.u32();
  if (code.ins_size > code.registers_size) throw_format("code_item : plus d'arguments que de registres");
  r.require(size_t(insns_size) * 2, "instructions");
  code.insns.resize(insns_size);
  for (auto& u : code.insns) u = r.u16();
  if (tries_size == 0) return code;

  if (insns_size % 2) r.u16();  // padding
  struct RawTry {
    uint32_t start;
    uint16_t count;
    uint16_t handler_off;
  };
  std::vector<RawTry> raw(tries_size);
  for (auto& t : raw) {
    t.start = r.u32();
    t.count = r.u16();
    t.handler_off = r.u16();
  }
  size_t list_start = r.pos();
  uint32_t list_size = r.uleb128();
  if (list_size > r.remaining()) throw_format("liste de gestionnaires invalide");
  std::unordered_map<uint32_t, uint32_t> index_by_offset;
  for (uint32_t i = 0; i < list_size; ++i) {
    index_by_offset[uint32_t(r.pos() - list_start)] = i;
    int32_t size = r.sleb128();
    uint32_t n = size < 0 ? uint32_t(-int64_t(size)) : uint32_t(size);
    if (n > r.remaining()) throw_format("gestionnaire d'exceptions invalide");
    CatchHandler h;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t type_idx = r.uleb128();
      uint32_t addr = r.uleb128();
      if (type_idx >= header_.type_ids_size) throw_format("type d'exception invalide");
      h.typed.emplace_back(type_idx, addr);
    }
    if (size <= 0) h.catch_all = r.uleb128();
    code.handlers.push_back(std::move(h));
  }
  for (const auto& t : raw) {
    auto it = index_by_offset.find(t.handler_off);
    if (it == index_by_offset.end()) throw_format("try_item : gestionnaire introuvable");
    if (uint64_t(t.start) + t.count > insns_size) throw_format("try_item hors du code");
    code.tries.push_back({t.start, t.count, it->second});
  }
  return code;
}

std::vector<uint32_t> DexFile::type_list(uint32_t offset) const {
  if (offset % 4 != 0) throw_format("type_list non alignée");
  ByteReader r(std::span<const uint8_t>(data_), offset);
  uint32_t size = r.u32();
  r.require(size_t(size) * 2, "type_list");
  std::vector<uint32_t> out(size);
  for (auto& t : out) {
    t = r.u16();
    if (t >= header_.type_ids_size) throw_format("type_list : type invalide");
  }
  return out;
}

std::vector<EncodedValue> DexFile::encoded_array(uint32_t offset) const {
  std::vector<EncodedValue> out;
  if (offset == 0) return out;
  ByteReader r(std::span<const uint8_t>(data_), offset);
  read_encoded_array(r, out, 0);
  return out;
}

namespace {

std::vector<Annotation> parse_annotation_set(std::span<const uint8_t> data, uint32_t set_off,
                                             uint32_t type_ids_size, uint32_t string_ids_size) {
  std::vector<Annotation> out;
  if (set_off == 0) return out;
  ByteReader set(data, set_off);
  uint32_t size = set.u32();
  if (size > set.remaining() / 4) throw_format("annotation_set_item hors limites");
  out.reserve(size);
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t item_off = set.u32();
    if (item_off == 0) continue;
    ByteReader item(data, item_off);
    Annotation a;
    a.visibility = item.u8();
    a.type_idx = item.uleb128();
    if (a.type_idx >= type_ids_size) throw_format("annotation : type invalide");
    uint32_t count = item.uleb128();
    if (count > item.remaining()) throw_format("annotation : taille invalide");
    for (uint32_t k = 0; k < count; ++k) {
      uint32_t name = item.uleb128();
      if (name >= string_ids_size) throw_format("annotation : nom invalide");
      EncodedValue v;
      read_encoded_value(item, v, 0);
      a.elements.emplace_back(name, std::move(v));
    }
    out.push_back(std::move(a));
  }
  return out;
}

}  // namespace

AnnotationsDirectory DexFile::annotations_directory(const ClassDef& def) const {
  AnnotationsDirectory dir;
  if (def.annotations_off == 0) return dir;
  auto data = std::span<const uint8_t>(data_);
  ByteReader r(data, def.annotations_off);
  uint32_t class_annotations_off = r.u32();
  uint32_t fields_size = r.u32();
  uint32_t annotated_methods_size = r.u32();
  uint32_t annotated_parameters_size = r.u32();

  if (class_annotations_off != 0) {
    dir.class_annotations = parse_annotation_set(data, class_annotations_off, header_.type_ids_size, header_.string_ids_size);
  }

  if (fields_size > r.remaining() / 8) throw_format("field_annotations hors limites");
  dir.field_annotations.reserve(fields_size);
  for (uint32_t i = 0; i < fields_size; ++i) {
    uint32_t field_idx = r.u32();
    uint32_t off = r.u32();
    if (field_idx >= header_.field_ids_size) throw_format("field_annotation : index invalide");
    dir.field_annotations.push_back({field_idx, parse_annotation_set(data, off, header_.type_ids_size, header_.string_ids_size)});
  }

  if (annotated_methods_size > r.remaining() / 8) throw_format("method_annotations hors limites");
  dir.method_annotations.reserve(annotated_methods_size);
  for (uint32_t i = 0; i < annotated_methods_size; ++i) {
    uint32_t method_idx = r.u32();
    uint32_t off = r.u32();
    if (method_idx >= header_.method_ids_size) throw_format("method_annotation : index invalide");
    dir.method_annotations.push_back({method_idx, parse_annotation_set(data, off, header_.type_ids_size, header_.string_ids_size)});
  }

  if (annotated_parameters_size > r.remaining() / 8) throw_format("parameter_annotations hors limites");
  dir.parameter_annotations.reserve(annotated_parameters_size);
  for (uint32_t i = 0; i < annotated_parameters_size; ++i) {
    uint32_t method_idx = r.u32();
    uint32_t off = r.u32();
    if (method_idx >= header_.method_ids_size) throw_format("parameter_annotation : index invalide");
    ParameterAnnotation pa;
    pa.method_idx = method_idx;
    if (off != 0) {
      ByteReader ref_list(data, off);
      uint32_t count = ref_list.u32();
      if (count > ref_list.remaining() / 4) throw_format("annotation_set_ref_list hors limites");
      pa.annotations.resize(count);
      for (uint32_t p = 0; p < count; ++p) {
        uint32_t param_set_off = ref_list.u32();
        if (param_set_off != 0) {
          pa.annotations[p] = parse_annotation_set(data, param_set_off, header_.type_ids_size, header_.string_ids_size);
        }
      }
    }
    dir.parameter_annotations.push_back(std::move(pa));
  }

  return dir;
}

std::vector<Annotation> DexFile::class_annotations(const ClassDef& def) const {
  if (def.annotations_off == 0) return {};
  auto data = std::span<const uint8_t>(data_);
  ByteReader dir(data, def.annotations_off);
  uint32_t set_off = dir.u32();
  return parse_annotation_set(data, set_off, header_.type_ids_size, header_.string_ids_size);
}

std::string DexFile::pretty_field(uint32_t idx) const {
  const FieldId& f = field(idx);
  return type_descriptor(f.class_idx) + "->" + string(f.name_idx) + ":" + type_descriptor(f.type_idx);
}

std::string DexFile::pretty_method(uint32_t idx) const {
  const MethodId& m = method(idx);
  return type_descriptor(m.class_idx) + "->" + string(m.name_idx) + proto_signature(m.proto_idx);
}

bool split_signature(std::string_view sig, std::vector<std::string>& params, std::string& ret) {
  params.clear();
  if (sig.empty() || sig[0] != '(') return false;
  size_t i = 1;
  auto read_type = [&](std::string& out) {
    size_t start = i;
    while (i < sig.size() && sig[i] == '[') ++i;
    if (i >= sig.size()) return false;
    if (sig[i] == 'L') {
      size_t end = sig.find(';', i);
      if (end == std::string_view::npos) return false;
      i = end + 1;
    } else {
      if (std::string_view("ZBSCIJFDV").find(sig[i]) == std::string_view::npos) return false;
      ++i;
    }
    out = std::string(sig.substr(start, i - start));
    return true;
  };
  while (i < sig.size() && sig[i] != ')') {
    std::string t;
    if (!read_type(t)) return false;
    params.push_back(std::move(t));
  }
  if (i >= sig.size()) return false;
  ++i;
  if (!read_type(ret)) return false;
  return i == sig.size();
}

uint32_t arg_words(std::string_view signature) {
  std::vector<std::string> params;
  std::string ret;
  if (!split_signature(signature, params, ret)) return 0;
  uint32_t n = 0;
  for (const auto& p : params) n += (p == "J" || p == "D") ? 2 : 1;
  return n;
}

}  // namespace aa::dex
