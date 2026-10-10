// Character set converters behind com.android.icu.charset.NativeConverter
// (ICU4C in Android). AnyAndroid implements the charsets every Java program
// relies on (StandardCharsets) without ICU4C: UTF-8, US-ASCII, ISO-8859-1,
// UTF-16, UTF-16BE and UTF-16LE. Other names are reported as unsupported
// (charsetForName returns null), as ICU does for unknown charsets.
//
// Contract of encode/decode (from CharsetEncoderICU / CharsetDecoderICU):
//  - data[0], data[1]: input and output offsets on entry; on return data[0]
//    is the number of input units consumed, data[1] the absolute output end;
//  - on an error the offending units are consumed and counted in data[2],
//    which the Java side backs out;
//  - result: 0 (U_ZERO_ERROR), 15 (U_BUFFER_OVERFLOW_ERROR), 10
//    (U_INVALID_CHAR_FOUND: unmappable), 12 (U_ILLEGAL_CHAR_FOUND: malformed)
//    or 11 (U_TRUNCATED_CHAR_FOUND: incomplete input at flush).
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

constexpr int32_t kOk = 0;
constexpr int32_t kUnmappable = 10;
constexpr int32_t kTruncated = 11;
constexpr int32_t kMalformed = 12;
constexpr int32_t kOverflow = 15;

// CodingErrorAction as translated by the Java side.
constexpr int32_t kReport = 0;
constexpr int32_t kIgnore = 1;
constexpr int32_t kReplace = 2;

enum class Codec { kUtf8, kAscii, kLatin1, kUtf16, kUtf16Be, kUtf16Le, kUtf32, kUtf32Be, kUtf32Le };

struct CodecInfo {
  Codec codec;
  const char* java_name;
  const char* icu_name;
  std::vector<const char*> aliases;
  int max_bytes;
  int min_bytes;
};

const std::vector<CodecInfo>& codecs() {
  static const std::vector<CodecInfo> all = {
      {Codec::kUtf8, "UTF-8", "UTF-8", {"UTF8", "unicode-1-1-utf-8", "cp1208"}, 3, 1},
      {Codec::kAscii, "US-ASCII", "US-ASCII",
       {"ASCII", "ANSI_X3.4-1968", "ANSI_X3.4-1986", "ISO_646.irv:1991", "ISO646-US", "us", "IBM367", "cp367",
        "csASCII", "646", "ascii7"},
       1, 1},
      {Codec::kLatin1, "ISO-8859-1", "ISO-8859-1",
       {"ISO_8859-1:1987", "iso-ir-100", "ISO_8859-1", "latin1", "l1", "IBM819", "CP819", "csISOLatin1", "8859_1",
        "819", "ISO8859_1"},
       1, 1},
      {Codec::kUtf16, "UTF-16", "UTF-16", {"UTF16", "ISO-10646-UCS-2", "csUnicode", "ucs-2", "unicode"}, 4, 2},
      {Codec::kUtf16Be, "UTF-16BE", "UTF-16BE", {"UTF16_BigEndian", "x-utf-16be", "UnicodeBigUnmarked"}, 2, 2},
      {Codec::kUtf16Le, "UTF-16LE", "UTF-16LE", {"UTF16_LittleEndian", "x-utf-16le", "UnicodeLittleUnmarked"}, 2, 2},
      {Codec::kUtf32, "UTF-32", "UTF-32", {"UTF32", "ISO-10646-UCS-4", "ucs-4"}, 8, 4},
      {Codec::kUtf32Be, "UTF-32BE", "UTF-32BE", {"UTF32_BigEndian", "X-UTF-32BE"}, 4, 4},
      {Codec::kUtf32Le, "UTF-32LE", "UTF-32LE", {"UTF32_LittleEndian", "X-UTF-32LE"}, 4, 4},
  };
  return all;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

const CodecInfo* find_codec(std::string_view name) {
  for (const auto& c : codecs()) {
    if (iequals(name, c.java_name) || iequals(name, c.icu_name)) return &c;
    for (const char* a : c.aliases) {
      if (iequals(name, a)) return &c;
    }
  }
  return nullptr;
}

struct Converter {
  const CodecInfo* info = nullptr;
  int32_t enc_malformed = kReport, enc_unmappable = kReport;
  std::vector<uint8_t> enc_replacement{'?'};
  int32_t dec_malformed = kReport, dec_unmappable = kReport;
  std::u16string dec_replacement = u"�";
  bool enc_bom_done = false;   // UTF-16: byte order mark written
  bool dec_bom_done = false;   // UTF-16: byte order detected
  bool dec_big_endian = true;
};

// Handles are bounded indices (never pointers): bytecode can forge longs.
std::vector<std::unique_ptr<Converter>>& converters() {
  static std::vector<std::unique_ptr<Converter>> table;
  return table;
}

Converter& converter(Runtime& rt, int64_t handle) {
  auto& table = converters();
  if (handle <= 0 || uint64_t(handle) > table.size() || !table[size_t(handle - 1)]) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "convertisseur invalide " + std::to_string(handle));
  }
  return *table[size_t(handle - 1)];
}

bool high(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool low(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

struct Result {
  int32_t code = kOk;
  int32_t consumed = 0;
  int32_t out = 0;
  int32_t invalid = 0;
};

Result encode(Converter& c, const char16_t* src, int32_t i, int32_t end, uint8_t* dst, int32_t o, int32_t dst_end,
              bool flush) {
  const int32_t start = i;
  Result r;
  auto finish = [&](int32_t code) {
    r.code = code;
    r.consumed = i - start;
    r.out = o;
    return r;
  };
  const Codec codec = c.info->codec;
  if (codec == Codec::kUtf16 && !c.enc_bom_done && i < end) {
    if (o + 2 > dst_end) return finish(kOverflow);
    dst[o++] = 0xFE;
    dst[o++] = 0xFF;
    c.enc_bom_done = true;
  }
  if (codec == Codec::kUtf32 && !c.enc_bom_done && i < end) {
    if (o + 4 > dst_end) return finish(kOverflow);
    dst[o++] = 0x00;
    dst[o++] = 0x00;
    dst[o++] = 0xFE;
    dst[o++] = 0xFF;
    c.enc_bom_done = true;
  }
  while (i < end) {
    char16_t ch = src[i];
    uint32_t cp = ch;
    int32_t units = 1;
    int32_t error = kOk;
    if (high(ch)) {
      if (i + 1 < end) {
        if (low(src[i + 1])) {
          cp = 0x10000 + ((uint32_t(ch) - 0xD800) << 10) + (uint32_t(src[i + 1]) - 0xDC00);
          units = 2;
        } else {
          error = kMalformed;
        }
      } else if (flush) {
        error = kTruncated;
      } else {
        break;  // wait for the trail surrogate
      }
    } else if (low(ch)) {
      error = kMalformed;
    }
    if (error == kOk) {
      if ((codec == Codec::kAscii && cp > 0x7F) || (codec == Codec::kLatin1 && cp > 0xFF)) error = kUnmappable;
    }
    if (error != kOk) {
      int32_t action = error == kUnmappable ? c.enc_unmappable : c.enc_malformed;
      if (action == kReport || error == kTruncated) {
        i += units;
        r.invalid = units;
        return finish(error);
      }
      if (action == kReplace) {
        if (o + int32_t(c.enc_replacement.size()) > dst_end) return finish(kOverflow);
        for (uint8_t b : c.enc_replacement) dst[o++] = b;
      }
      i += units;
      continue;
    }
    uint8_t buf[4];
    int32_t n = 0;
    switch (codec) {
      case Codec::kAscii:
      case Codec::kLatin1: buf[n++] = uint8_t(cp); break;
      case Codec::kUtf8:
        if (cp < 0x80) {
          buf[n++] = uint8_t(cp);
        } else if (cp < 0x800) {
          buf[n++] = uint8_t(0xC0 | (cp >> 6));
          buf[n++] = uint8_t(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          buf[n++] = uint8_t(0xE0 | (cp >> 12));
          buf[n++] = uint8_t(0x80 | ((cp >> 6) & 0x3F));
          buf[n++] = uint8_t(0x80 | (cp & 0x3F));
        } else {
          buf[n++] = uint8_t(0xF0 | (cp >> 18));
          buf[n++] = uint8_t(0x80 | ((cp >> 12) & 0x3F));
          buf[n++] = uint8_t(0x80 | ((cp >> 6) & 0x3F));
          buf[n++] = uint8_t(0x80 | (cp & 0x3F));
        }
        break;
      case Codec::kUtf16:
      case Codec::kUtf16Be:
      case Codec::kUtf16Le:
        for (int32_t k = 0; k < units; ++k) {
          char16_t u = src[i + k];
          if (codec == Codec::kUtf16Le) {
            buf[n++] = uint8_t(u);
            buf[n++] = uint8_t(u >> 8);
          } else {
            buf[n++] = uint8_t(u >> 8);
            buf[n++] = uint8_t(u);
          }
        }
        break;
      case Codec::kUtf32:
      case Codec::kUtf32Be:
        buf[n++] = uint8_t(cp >> 24);
        buf[n++] = uint8_t(cp >> 16);
        buf[n++] = uint8_t(cp >> 8);
        buf[n++] = uint8_t(cp);
        break;
      case Codec::kUtf32Le:
        buf[n++] = uint8_t(cp);
        buf[n++] = uint8_t(cp >> 8);
        buf[n++] = uint8_t(cp >> 16);
        buf[n++] = uint8_t(cp >> 24);
        break;
    }
    if (o + n > dst_end) return finish(kOverflow);
    std::memcpy(dst + o, buf, size_t(n));
    o += n;
    i += units;
  }
  return finish(kOk);
}

Result decode(Converter& c, const uint8_t* src, int32_t i, int32_t end, char16_t* dst, int32_t o, int32_t dst_end,
              bool flush) {
  const int32_t start = i;
  Result r;
  auto finish = [&](int32_t code) {
    r.code = code;
    r.consumed = i - start;
    r.out = o;
    return r;
  };
  const Codec codec = c.info->codec;
  // Handles an invalid sequence of `len` bytes; returns false to stop.
  auto invalid = [&](int32_t error, int32_t len, int32_t& code) {
    int32_t action = error == kUnmappable ? c.dec_unmappable : c.dec_malformed;
    if (action == kReport || error == kTruncated) {
      i += len;
      r.invalid = len;
      code = error;
      return false;
    }
    if (action == kReplace) {
      if (o + int32_t(c.dec_replacement.size()) > dst_end) {
        code = kOverflow;
        return false;
      }
      for (char16_t u : c.dec_replacement) dst[o++] = u;
    }
    i += len;
    return true;
  };
  if (codec == Codec::kUtf16 && !c.dec_bom_done) {
    if (end - i < 2) {
      if (!flush || end == i) return finish(kOk);
    } else {
      if (src[i] == 0xFE && src[i + 1] == 0xFF) {
        c.dec_big_endian = true;
        i += 2;
      } else if (src[i] == 0xFF && src[i + 1] == 0xFE) {
        c.dec_big_endian = false;
        i += 2;
      }
      c.dec_bom_done = true;
    }
  }
  if (codec == Codec::kUtf32 && !c.dec_bom_done) {
    if (end - i < 4) {
      if (!flush || end == i) return finish(kOk);
    } else {
      if (src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 0xFE && src[i + 3] == 0xFF) {
        c.dec_big_endian = true;
        i += 4;
      } else if (src[i] == 0xFF && src[i + 1] == 0xFE && src[i + 2] == 0 && src[i + 3] == 0) {
        c.dec_big_endian = false;
        i += 4;
      }
      c.dec_bom_done = true;
    }
  }
  while (i < end) {
    int32_t code = kOk;
    uint32_t cp = 0;
    int32_t len = 1;
    switch (codec) {
      case Codec::kLatin1: cp = src[i]; break;
      case Codec::kAscii:
        cp = src[i];
        if (cp > 0x7F) {
          if (!invalid(kMalformed, 1, code)) return finish(code);
          continue;
        }
        break;
      case Codec::kUtf8: {
        uint8_t b = src[i];
        int32_t need;
        uint32_t min;
        if (b < 0x80) {
          need = 0, cp = b, min = 0;
        } else if (b >= 0xC2 && b <= 0xDF) {
          need = 1, cp = b & 0x1F, min = 0x80;
        } else if (b >= 0xE0 && b <= 0xEF) {
          need = 2, cp = b & 0x0F, min = 0x800;
        } else if (b >= 0xF0 && b <= 0xF4) {
          need = 3, cp = b & 0x07, min = 0x10000;
        } else {
          if (!invalid(kMalformed, 1, code)) return finish(code);
          continue;
        }
        int32_t k = 1;
        bool bad = false;
        for (; k <= need; ++k) {
          if (i + k >= end) break;
          uint8_t t = src[i + k];
          if ((t & 0xC0) != 0x80) {
            bad = true;
            break;
          }
          cp = (cp << 6) | (t & 0x3F);
          // Reject overlong forms, surrogates and values above U+10FFFF as
          // soon as the second byte shows them (maximal subpart rule).
          if (k == 1 && ((need == 2 && ((b == 0xE0 && t < 0xA0) || (b == 0xED && t > 0x9F))) ||
                         (need == 3 && ((b == 0xF0 && t < 0x90) || (b == 0xF4 && t > 0x8F))))) {
            bad = true;
            break;
          }
        }
        if (bad) {
          if (!invalid(kMalformed, k, code)) return finish(code);
          continue;
        }
        if (k <= need) {  // incomplete at the end of the input
          if (!flush) return finish(kOk);
          if (!invalid(kTruncated, end - i, code)) return finish(code);
          continue;
        }
        if (cp < min) {
          if (!invalid(kMalformed, need + 1, code)) return finish(code);
          continue;
        }
        len = need + 1;
        break;
      }
      case Codec::kUtf16:
      case Codec::kUtf16Be:
      case Codec::kUtf16Le: {
        bool be = codec == Codec::kUtf16Be || (codec == Codec::kUtf16 && c.dec_big_endian);
        auto unit = [&](int32_t at) -> char16_t {
          return be ? char16_t((src[at] << 8) | src[at + 1]) : char16_t(src[at] | (src[at + 1] << 8));
        };
        if (end - i < 2) {
          if (!flush) return finish(kOk);
          if (!invalid(kTruncated, end - i, code)) return finish(code);
          continue;
        }
        char16_t u = unit(i);
        if (high(u)) {
          if (end - i < 4) {
            if (!flush) return finish(kOk);
            if (!invalid(kTruncated, end - i, code)) return finish(code);
            continue;
          }
          char16_t v = unit(i + 2);
          if (!low(v)) {
            if (!invalid(kMalformed, 2, code)) return finish(code);
            continue;
          }
          cp = 0x10000 + ((uint32_t(u) - 0xD800) << 10) + (uint32_t(v) - 0xDC00);
          len = 4;
        } else if (low(u)) {
          if (!invalid(kMalformed, 2, code)) return finish(code);
          continue;
        } else {
          cp = u;
          len = 2;
        }
        break;
      }
      case Codec::kUtf32:
      case Codec::kUtf32Be:
      case Codec::kUtf32Le: {
        if (end - i < 4) {
          if (!flush) return finish(kOk);
          if (!invalid(kTruncated, end - i, code)) return finish(code);
          continue;
        }
        bool be = codec == Codec::kUtf32Be || (codec == Codec::kUtf32 && c.dec_big_endian);
        cp = be ? (uint32_t(src[i]) << 24) | (uint32_t(src[i + 1]) << 16) | (uint32_t(src[i + 2]) << 8) | src[i + 3]
                : (uint32_t(src[i + 3]) << 24) | (uint32_t(src[i + 2]) << 16) | (uint32_t(src[i + 1]) << 8) | src[i];
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
          if (!invalid(kMalformed, 4, code)) return finish(code);
          continue;
        }
        len = 4;
        break;
      }
    }
    int32_t n = cp >= 0x10000 ? 2 : 1;
    if (o + n > dst_end) return finish(kOverflow);
    if (n == 2) {
      dst[o++] = char16_t(0xD800 + ((cp - 0x10000) >> 10));
      dst[o++] = char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF));
    } else {
      dst[o++] = char16_t(cp);
    }
    i += len;
  }
  return finish(kOk);
}

// Validated views of the arrays passed to encode/decode.
struct ArrayView {
  uint8_t* data = nullptr;
  int32_t length = 0;  // in elements
};

ArrayView view(Runtime& rt, Handle h, uint8_t width, const char* what) {
  if (h == kNull) return {};
  rt::ArrayObject& a = rt.array(h, what);
  if (a.width != width || !a.klass->component->primitive) rt.throw_new("Ljava/lang/IllegalArgumentException;", what);
  return {a.data.data(), int32_t(a.length)};
}

uint64_t run_codec(Runtime& rt, NativeArgs& a, bool encoding) {
  Converter& c = converter(rt, a.i64(0));
  ArrayView in = view(rt, a.h(2), encoding ? 2 : 1, "entrée");
  int32_t in_end = a.i32(3);
  ArrayView out = view(rt, a.h(4), encoding ? 1 : 2, "sortie");
  int32_t out_end = a.i32(5);
  rt::ArrayObject& data = rt.array(a.h(6), "data");
  if (data.width != 4 || data.length < 3) rt.throw_new("Ljava/lang/IllegalArgumentException;", "data");
  int32_t d[3];
  std::memcpy(d, data.data.data(), 12);
  bool flush = a.z(7);
  if (in_end < 0 || in_end > in.length || d[0] < 0 || d[0] > in_end || out_end < 0 || out_end > out.length || d[1] < 0 ||
      d[1] > out_end) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "positions de conversion invalides");
  }
  Result r = encoding ? encode(c, reinterpret_cast<const char16_t*>(in.data), d[0], in_end, out.data, d[1], out_end, flush)
                      : decode(c, in.data, d[0], in_end, reinterpret_cast<char16_t*>(out.data), d[1], out_end, flush);
  d[0] = r.consumed;
  d[1] = r.out;
  if (r.invalid) d[2] = r.invalid;
  std::memcpy(rt.array(a.h(6), "data").data.data(), d, 12);
  return rt::ret_int(r.code);
}

}  // namespace

void register_libcore_charsets(rt::NativeRegistry& r) {
  const char* nc = "Lcom/android/icu/charset/NativeConverter;";
  r.bind(nc, "charsetForName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("charsetName");
    const CodecInfo* info = find_codec(rt.string_utf8(a.h(0)));
    if (!info) return kNull;
    Handle aliases = rt.new_array("[Ljava/lang/String;", int32_t(info->aliases.size()));
    rt::GlobalRef keep(rt, aliases);
    for (size_t i = 0; i < info->aliases.size(); ++i) {
      Handle s = rt.new_string_utf8(info->aliases[i]);
      std::memcpy(&rt.array(aliases, "aliases").data[i * 4], &s, 4);
    }
    rt::Class* cls = rt.find_class("Lcom/android/icu/charset/CharsetICU;");
    rt.ensure_initialized(cls);
    rt::Method* ctor = cls->declared_method("<init>", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)V");
    if (!ctor) rt.throw_new("Ljava/lang/NoSuchMethodError;", "CharsetICU.<init>");
    Handle cs = rt.alloc(cls);
    rt::GlobalRef keep_cs(rt, cs);
    uint32_t args[] = {cs, rt.new_string_utf8(info->java_name), 0, aliases};
    args[2] = rt.new_string_utf8(info->icu_name);
    rt.invoke(ctor, args, 4);
    return cs;
  }, S::kPartial, "UTF-8, US-ASCII, ISO-8859-1, UTF-16 (BE/LE) ; autres jeux non pris en charge");
  r.bind(nc, "getAvailableCharsetNames", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    Handle arr = rt.new_array("[Ljava/lang/String;", int32_t(codecs().size()));
    rt::GlobalRef keep(rt, arr);
    for (size_t i = 0; i < codecs().size(); ++i) {
      Handle s = rt.new_string_utf8(codecs()[i].java_name);
      std::memcpy(&rt.array(arr, "noms").data[i * 4], &s, 4);
    }
    return arr;
  });
  r.bind(nc, "contains", "(Ljava/lang/String;Ljava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const CodecInfo* x = find_codec(rt.string_utf8(a.h(0)));
    const CodecInfo* y = find_codec(rt.string_utf8(a.h(1)));
    if (!x || !y) return 0;
    if (x == y) return 1;
    switch (x->codec) {
      case Codec::kUtf8: case Codec::kUtf16: case Codec::kUtf16Be: case Codec::kUtf16Le:
      case Codec::kUtf32: case Codec::kUtf32Be: case Codec::kUtf32Le: return 1;  // all of Unicode
      case Codec::kLatin1: return rt::ret_bool(y->codec == Codec::kAscii);
      default: return 0;
    }
  });
  r.bind(nc, "openConverter", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const CodecInfo* info = find_codec(rt.string_utf8(a.h(0)));
    if (!info) rt.throw_new("Ljava/nio/charset/UnsupportedCharsetException;", std::u16string_view(rt.string_value(a.h(0))));
    auto conv = std::make_unique<Converter>();
    conv->info = info;
    converters().push_back(std::move(conv));
    return uint64_t(converters().size());
  });
  r.bind(nc, "closeConverter", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t h = a.i64(0);
    if (h > 0 && uint64_t(h) <= converters().size()) converters()[size_t(h - 1)].reset();
    return 0;
  });
  r.bind(nc, "getNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial,
         "convertisseurs libérés par closeConverter uniquement");
  r.bind(nc, "resetByteToChar", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Converter& c = converter(rt, a.i64(0));
    c.dec_bom_done = false;
    c.dec_big_endian = true;
    return 0;
  });
  r.bind(nc, "resetCharToByte", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    converter(rt, a.i64(0)).enc_bom_done = false;
    return 0;
  });
  r.bind(nc, "getMaxBytesPerChar", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(converter(rt, a.i64(0)).info->max_bytes);
  });
  r.bind(nc, "getAveBytesPerChar", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const CodecInfo* i = converter(rt, a.i64(0)).info;
    return rt::ret_float(float(i->max_bytes + i->min_bytes) / 2.0f);
  });
  r.bind(nc, "getAveCharsPerByte", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_float(1.0f / float(converter(rt, a.i64(0)).info->max_bytes));
  });
  r.bind(nc, "getSubstitutionBytes", "(J)[B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Codec c = converter(rt, a.i64(0)).info->codec;
    std::vector<uint8_t> sub = c == Codec::kUtf16 || c == Codec::kUtf16Be ? std::vector<uint8_t>{0xFF, 0xFD}
                               : c == Codec::kUtf16Le                     ? std::vector<uint8_t>{0xFD, 0xFF}
                                                                          : std::vector<uint8_t>{'?'};
    Handle arr = rt.new_array("[B", int32_t(sub.size()));
    std::memcpy(rt.array(arr, "byte[]").data.data(), sub.data(), sub.size());
    return arr;
  });
  r.bind(nc, "setCallbackEncode", "(JII[B)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Converter& c = converter(rt, a.i64(0));
    c.enc_malformed = a.i32(2);
    c.enc_unmappable = a.i32(3);
    if (a.h(4) != kNull) {
      rt::ArrayObject& rep = rt.array(a.h(4), "replacement");
      c.enc_replacement.assign(rep.data.begin(), rep.data.end());
    }
    return 0;
  });
  r.bind(nc, "setCallbackDecode", "(JIILjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Converter& c = converter(rt, a.i64(0));
    c.dec_malformed = a.i32(2);
    c.dec_unmappable = a.i32(3);
    if (a.h(4) != kNull) c.dec_replacement = rt.string_value(a.h(4));
    return 0;
  });
  r.bind(nc, "encode", "(J[CI[BI[IZ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return run_codec(rt, a, true); });
  r.bind(nc, "decode", "(J[BI[CI[IZ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return run_codec(rt, a, false); });
}

}  // namespace aa::bridge
