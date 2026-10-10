// java.util.zip natives (libcore's Inflater.c, Deflater.c, CRC32.c,
// Adler32.c) on top of miniz.
//
// The zlib streams live in bounded handle tables: Java only ever holds a
// kind-tagged handle (InflaterZStreamRef/DeflaterZStreamRef.address), and every
// call keeps its stream alive with a shared_ptr, so a concurrent or repeated
// end() cannot free it under the call. The number of live streams is capped.
// Byte arrays are bounds-checked in 64 bits; direct buffers go through the
// emulated native memory.
#include "framework-bridge/internal.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "framework-bridge/handles.h"
#include "framework-bridge/native_memory.h"
#include "miniz.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr int kWindowBits = MZ_DEFAULT_WINDOW_BITS;  // 15, as zlib's MAX_WBITS
constexpr int kMemLevel = 8;                         // zlib's DEF_MEM_LEVEL (unused by miniz)
constexpr int kNoFlushYet = -2;                      // zlib's last_flush after deflateInit/deflateReset

// Live streams: a Deflater holds about 312 KiB of miniz state, an Inflater
// about 40 KiB. Only end() frees them (the collector never runs libcore's
// Cleaners), so a program that forgets end() runs out of streams
// (OutOfMemoryError) rather than of host memory.
constexpr size_t kMaxDeflaters = 256;   // ~78 MiB
constexpr size_t kMaxInflaters = 1024;  // ~40 MiB

// One zlib stream of an Inflater or a Deflater.
struct ZStream {
  explicit ZStream(bool deflater) : deflate(deflater) {}
  ~ZStream() {
    if (deflate) {
      mz_deflateEnd(&strm);
    } else {
      mz_inflateEnd(&strm);
    }
  }
  ZStream(const ZStream&) = delete;
  ZStream& operator=(const ZStream&) = delete;

  mz_stream strm{};
  bool deflate;
  int window_bits = kWindowBits;  // negative: raw deflate (nowrap)
  int last_flush = kNoFlushYet;   // Deflater: flush mode of the previous deflate (zlib's last_flush)
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<ZStream> inflaters{0x0d01};
  HandleTable<ZStream> deflaters{0x0d02};
  std::mutex open_lock;  // keeps the caps exact under concurrent init()
};

Tables& tables() {
  static Tables t;
  return t;
}

std::shared_ptr<ZStream> need(Runtime& rt, const HandleTable<ZStream>& table, int64_t handle, const char* what) {
  auto s = table.get(handle);
  if (!s) rt.throw_new("Ljava/lang/NullPointerException;", std::string(what) + " : flux zlib natif invalide ou fermé");
  return s;
}

std::shared_ptr<ZStream> inflater(Runtime& rt, int64_t h) { return need(rt, tables().inflaters, h, "Inflater"); }
std::shared_ptr<ZStream> deflater(Runtime& rt, int64_t h) { return need(rt, tables().deflaters, h, "Deflater"); }

// Inflater/Deflater.init: the handle of the stream `make` creates, or
// OutOfMemoryError when the cap is reached (checked before miniz allocates).
template <typename Make>
int64_t open_stream(Runtime& rt, bool deflate, Make make) {
  Tables& t = tables();
  HandleTable<ZStream>& table = deflate ? t.deflaters : t.inflaters;
  size_t max = deflate ? kMaxDeflaters : kMaxInflaters;
  std::lock_guard<std::mutex> guard(t.open_lock);
  if (table.live() >= max) {
    rt.throw_new("Ljava/lang/OutOfMemoryError;", std::string(deflate ? "Deflater" : "Inflater") +
                                                     " : trop de flux zlib ouverts (" + std::to_string(max) +
                                                     ") ; end() libère les flux inutilisés");
  }
  return table.add(make());
}

// --- Java memory -------------------------------------------------------------------------

[[noreturn]] void out_of_bounds(Runtime& rt, const char* what, int64_t off, int64_t len, int64_t size) {
  rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
               std::string(what) + " : décalage " + std::to_string(off) + ", longueur " + std::to_string(len) +
                   ", taille " + std::to_string(size));
}

// The `len` bytes at `off` of a Java byte[]: NullPointerException for null,
// ArrayIndexOutOfBoundsException when the range leaves the array.
std::span<uint8_t> byte_range(Runtime& rt, Handle array, int32_t off, int32_t len, const char* what) {
  if (array == kNull) rt.throw_npe(what);
  rt::ArrayObject& a = rt.array(array, what);
  if (a.width != 1 || a.data.size() < a.length) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string(what) + " : tableau d'octets attendu");
  }
  if (off < 0 || len < 0 || int64_t(off) + int64_t(len) > int64_t(a.length)) out_of_bounds(rt, what, off, len, a.length);
  return {a.data.data() + off, size_t(len)};
}

// The `len` bytes at a native address (DirectByteBuffer.address + position),
// checked by the emulated native memory. An empty range is never dereferenced.
std::span<uint8_t> native_range(Runtime& rt, int64_t address, int32_t len, bool write, const char* what) {
  if (len < 0) out_of_bounds(rt, what, 0, len, 0);
  if (len == 0) return {};
  return native_span(rt, address, uint64_t(len), write);
}

// address + off (CRC32/Adler32.updateByteBuffer): the offset may not carry
// out of the offset bits of the address (see native_memory.h).
int64_t offset_address(Runtime& rt, int64_t address, int32_t off, const char* what) {
  if (off < 0 || (uint64_t(address) & 0xffffffffull) + uint64_t(off) > 0xffffffffull) {
    out_of_bounds(rt, what, off, 0, 0);
  }
  return address + off;
}

void set_int_field(Runtime& rt, Handle obj, const char* name, int32_t v) {
  rt::Object& o = rt.nonnull(obj, "Inflater");
  rt::Field* f = find_field(o.klass, name, "I");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/InternalError;", std::string("champ ") + name + " introuvable");
  o.fields.at(f->slot) = uint32_t(v);
}

// CRC-32 and Adler-32 of `bytes`, continuing `v`. An empty range leaves the
// checksum unchanged, as zlib with Java's non-null array pointer: miniz would
// return the initial value for the null pointer of an empty span.
uint32_t crc32(uint32_t v, std::span<const uint8_t> bytes) {
  return bytes.empty() ? v : uint32_t(mz_crc32(v, bytes.data(), bytes.size()));
}
uint32_t adler32(uint32_t v, std::span<const uint8_t> bytes) {
  return bytes.empty() ? v : uint32_t(mz_adler32(v, bytes.data(), bytes.size()));
}

// --- Streams -----------------------------------------------------------------------------

// Points the stream at the Java buffers for one call. miniz needs a non-null
// output pointer even for an empty buffer.
struct Window {
  Window(mz_stream& s, std::span<const uint8_t> in, std::span<uint8_t> out) : s_(s), in_(in), out_(out) {
    s.next_in = in.empty() ? &dummy_ : in.data();
    s.avail_in = uint32_t(in.size());
    s.next_out = out.empty() ? &dummy_ : out.data();
    s.avail_out = uint32_t(out.size());
  }
  // No pointer into Java memory survives the call.
  ~Window() {
    s_.next_in = nullptr;
    s_.next_out = nullptr;
    s_.avail_in = 0;
    s_.avail_out = 0;
  }
  uint64_t read() const { return in_.size() - s_.avail_in; }
  uint64_t written() const { return out_.size() - s_.avail_out; }

 private:
  mz_stream& s_;
  std::span<const uint8_t> in_;
  std::span<uint8_t> out_;
  uint8_t dummy_ = 0;
};

// Inflater.inflate*'s packed result: bits 0-30 bytes read, 31-61 bytes
// written, 62 finished, 63 needs a dictionary.
uint64_t pack(uint64_t read, uint64_t written, bool bit62, bool bit63) {
  return (read & 0x7fffffffull) | ((written & 0x7fffffffull) << 31) | (bit62 ? 1ull << 62 : 0) |
         (bit63 ? 1ull << 63 : 0);
}

uint64_t do_inflate(Runtime& rt, Handle self, ZStream& z, std::span<const uint8_t> in, std::span<uint8_t> out) {
  uint64_t read, written;
  int ret;
  {
    Window w(z.strm, in, out);
    ret = mz_inflate(&z.strm, MZ_SYNC_FLUSH);
    read = w.read();
    written = w.written();
  }
  switch (ret) {
    case MZ_OK:
    case MZ_STREAM_END:
    case MZ_NEED_DICT:
    case MZ_BUF_ERROR:  // no progress possible; miniz may still have flushed buffered output
      break;
    case MZ_DATA_ERROR:
      // Inflater.inflate's handler reads these to advance its input position.
      set_int_field(rt, self, "inputConsumed", int32_t(read));
      set_int_field(rt, self, "outputConsumed", int32_t(written));
      rt.throw_new("Ljava/util/zip/DataFormatException;",
                   z.strm.msg ? std::string(z.strm.msg) : std::string("données compressées invalides"));
    case MZ_MEM_ERROR:
      rt.throw_new("Ljava/lang/OutOfMemoryError;", "inflate : mémoire insuffisante");
    default:
      rt.throw_new("Ljava/lang/InternalError;", "inflate : erreur zlib " + std::to_string(ret));
  }
  return rt::ret_long(int64_t(pack(read, written, ret == MZ_STREAM_END, ret == MZ_NEED_DICT)));
}

int zlib_level(int32_t level) { return level == -1 ? MZ_DEFAULT_LEVEL : level; }

// zlib's deflateParams on miniz. Before any output the compressor simply
// restarts with the new parameters. Otherwise the input given so far is
// compressed with the old ones up to a full flush (a block boundary with an
// empty dictionary, so the matcher can change), then the new ones apply.
// MZ_BUF_ERROR: not done yet (output full), Java calls again.
int deflate_params(ZStream& z, int32_t level, int32_t strategy) {
  auto* d = reinterpret_cast<tdefl_compressor*>(z.strm.state);
  if (!d) return MZ_STREAM_ERROR;
  if (level < -1 || level > 9 || strategy < 0 || strategy > MZ_FIXED) return MZ_STREAM_ERROR;
  mz_uint flags = TDEFL_COMPUTE_ADLER32 | tdefl_create_comp_flags_from_zip_params(zlib_level(level), z.window_bits, strategy);
  if (flags == d->m_flags) return MZ_OK;
  if (z.strm.total_in == 0 && z.strm.total_out == 0) {
    tdefl_init(d, nullptr, nullptr, int(flags));
    z.last_flush = kNoFlushYet;
    return MZ_OK;
  }
  if (d->m_prev_return_status != TDEFL_STATUS_DONE) {
    int ret = mz_deflate(&z.strm, MZ_FULL_FLUSH);
    if (ret != MZ_OK && ret != MZ_BUF_ERROR) return ret;
    if (z.strm.avail_in != 0 || d->m_lookahead_size != 0 || d->m_output_flush_remaining != 0) return MZ_BUF_ERROR;
  }
  // Same derived fields as tdefl_init(); the rest of the state is kept.
  d->m_flags = flags;
  d->m_max_probes[0] = 1 + ((flags & 0xFFF) + 2) / 3;
  d->m_max_probes[1] = 1 + (((flags & 0xFFF) >> 2) + 2) / 3;
  d->m_greedy_parsing = (flags & TDEFL_GREEDY_PARSING_FLAG) != 0;
  // zlib's deflateParams ends with deflate(Z_BLOCK), weaker than SYNC_FLUSH
  // and FULL_FLUSH: the next of these writes its block again.
  z.last_flush = kNoFlushYet;
  return MZ_OK;
}

// zlib's RANK(): flush modes ordered by strength.
int flush_rank(int flush) { return flush * 2 - (flush > 4 ? 9 : 0); }

// Nothing buffered in the compressor: no output left to copy out, no input
// left to compress, no symbol in the current block.
bool nothing_pending(const tdefl_compressor& d) {
  return d.m_output_flush_remaining == 0 && d.m_lookahead_size == 0 && d.m_pLZ_code_buf == d.m_lz_code_buf + 1;
}

// Deflater.deflate*'s packed result: bits 0-30 bytes read, 31-61 bytes
// written, 62 finished, 63 parameters still to apply (setParams).
uint64_t do_deflate(Runtime& rt, ZStream& z, std::span<const uint8_t> in, std::span<uint8_t> out, int32_t flush,
                    int32_t params) {
  bool set_params = (params & 1) != 0;
  if (!set_params && (flush < MZ_NO_FLUSH || flush > MZ_FINISH)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "deflate : mode de vidage invalide " + std::to_string(flush));
  }
  if (!set_params && !out.empty()) {
    // As zlib's deflate(): a flush no stronger than the previous one, with no
    // new input and nothing pending, writes nothing (Z_BUF_ERROR). miniz would
    // write one more empty block each time, and a caller that flushes until
    // nothing comes out (okio's DeflaterSink) would never stop.
    int previous = z.last_flush;
    z.last_flush = flush;
    auto* d = reinterpret_cast<const tdefl_compressor*>(z.strm.state);
    if (in.empty() && flush != MZ_FINISH && flush_rank(flush) <= flush_rank(previous) && d && nothing_pending(*d)) {
      return rt::ret_long(int64_t(pack(0, 0, false, false)));
    }
  }
  uint64_t read, written;
  int ret;
  {
    Window w(z.strm, in, out);
    ret = set_params ? deflate_params(z, params >> 3, (params >> 1) & 3) : mz_deflate(&z.strm, flush);
    read = w.read();
    written = w.written();
  }
  if (set_params) {
    if (ret != MZ_OK && ret != MZ_BUF_ERROR) rt.throw_new("Ljava/lang/InternalError;", "deflateParams a échoué");
    return rt::ret_long(int64_t(pack(read, written, false, ret != MZ_OK)));
  }
  switch (ret) {
    case MZ_OK:
    case MZ_STREAM_END:
    case MZ_BUF_ERROR:
      break;
    default:
      rt.throw_new("Ljava/lang/InternalError;", "deflate : erreur zlib " + std::to_string(ret));
  }
  return rt::ret_long(int64_t(pack(read, written, ret == MZ_STREAM_END, false)));
}

[[noreturn]] void no_dictionary(Runtime& rt, const char* cls) {
  rt.throw_new("Ljava/lang/UnsupportedOperationException;",
               std::string("[AnyAndroid] ") + cls + ".setDictionary : dictionnaire zlib prédéfini non pris en charge (miniz)");
}

}  // namespace

void register_libcore_zip(rt::NativeRegistry& r) {
  // --- Inflater ----------------------------------------------------------------------------
  // Instance natives: argument 0 is the Inflater, 1-2 the stream handle.
  const char* inf = "Ljava/util/zip/Inflater;";

  r.bind(inf, "init", "(Z)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(open_stream(rt, false, [&] {
      auto z = std::make_shared<ZStream>(false);
      z->window_bits = a.z(0) ? -kWindowBits : kWindowBits;
      int ret = mz_inflateInit2(&z->strm, z->window_bits);
      if (ret == MZ_MEM_ERROR) rt.throw_new("Ljava/lang/OutOfMemoryError;", "Inflater : mémoire insuffisante");
      if (ret != MZ_OK) rt.throw_new("Ljava/lang/InternalError;", "mz_inflateInit2 a échoué");
      z->strm.adler = MZ_ADLER32_INIT;
      return z;
    }));
  }, S::kSupported,
     "flux miniz (sans dictionnaire prédéfini ; " + std::to_string(kMaxInflaters) + " flux vivants au plus)");

  r.bind(inf, "end", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().inflaters.remove(a.i64(0));  // a second end() is a no-op
    return 0;
  });

  r.bind(inf, "reset", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = inflater(rt, a.i64(0));
    mz_inflateReset(&z->strm);
    z->strm.adler = MZ_ADLER32_INIT;
    return 0;
  });

  r.bind(inf, "getAdler", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(inflater(rt, a.i64(0))->strm.adler));
  });

  r.bind(inf, "setDictionary", "(J[BII)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    no_dictionary(rt, "Inflater");
  }, S::kUnsupported, "dictionnaire zlib prédéfini : non pris en charge par miniz");
  r.bind(inf, "setDictionaryBuffer", "(JJI)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    no_dictionary(rt, "Inflater");
  }, S::kUnsupported, "dictionnaire zlib prédéfini : non pris en charge par miniz");

  r.bind(inf, "inflateBytesBytes", "(J[BII[BII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = inflater(rt, a.i64(1));
    auto in = byte_range(rt, a.h(3), a.i32(4), a.i32(5), "inflate (entrée)");
    auto out = byte_range(rt, a.h(6), a.i32(7), a.i32(8), "inflate (sortie)");
    return do_inflate(rt, a.h(0), *z, in, out);
  });

  r.bind(inf, "inflateBytesBuffer", "(J[BIIJI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = inflater(rt, a.i64(1));
    auto in = byte_range(rt, a.h(3), a.i32(4), a.i32(5), "inflate (entrée)");
    auto out = native_range(rt, a.i64(6), a.i32(8), true, "inflate (sortie)");
    return do_inflate(rt, a.h(0), *z, in, out);
  });

  r.bind(inf, "inflateBufferBytes", "(JJI[BII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = inflater(rt, a.i64(1));
    auto in = native_range(rt, a.i64(3), a.i32(5), false, "inflate (entrée)");
    auto out = byte_range(rt, a.h(6), a.i32(7), a.i32(8), "inflate (sortie)");
    return do_inflate(rt, a.h(0), *z, in, out);
  });

  r.bind(inf, "inflateBufferBuffer", "(JJIJI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = inflater(rt, a.i64(1));
    auto in = native_range(rt, a.i64(3), a.i32(5), false, "inflate (entrée)");
    auto out = native_range(rt, a.i64(6), a.i32(8), true, "inflate (sortie)");
    return do_inflate(rt, a.h(0), *z, in, out);
  });

  // --- Deflater ----------------------------------------------------------------------------
  const char* def = "Ljava/util/zip/Deflater;";

  r.bind(def, "init", "(IIZ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t level = a.i32(0), strategy = a.i32(1);
    if (level < -1 || level > 9) rt.throw_new("Ljava/lang/IllegalArgumentException;", "niveau de compression invalide");
    if (strategy < 0 || strategy > MZ_FIXED) rt.throw_new("Ljava/lang/IllegalArgumentException;", "stratégie invalide");
    return rt::ret_long(open_stream(rt, true, [&] {
      auto z = std::make_shared<ZStream>(true);
      z->window_bits = a.z(2) ? -kWindowBits : kWindowBits;
      int ret = mz_deflateInit2(&z->strm, zlib_level(level), MZ_DEFLATED, z->window_bits, kMemLevel, strategy);
      if (ret == MZ_MEM_ERROR) rt.throw_new("Ljava/lang/OutOfMemoryError;", "Deflater : mémoire insuffisante");
      if (ret != MZ_OK) rt.throw_new("Ljava/lang/IllegalArgumentException;", "mz_deflateInit2 a échoué");
      return z;
    }));
  }, S::kSupported,
     "flux miniz (sans dictionnaire prédéfini ; flux valide mais octets différents de zlib ; " +
         std::to_string(kMaxDeflaters) + " flux vivants au plus)");

  r.bind(def, "end", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().deflaters.remove(a.i64(0));
    return 0;
  });

  r.bind(def, "reset", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = deflater(rt, a.i64(0));
    mz_deflateReset(&z->strm);
    z->strm.adler = MZ_ADLER32_INIT;
    z->last_flush = kNoFlushYet;
    return 0;
  });

  r.bind(def, "getAdler", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(deflater(rt, a.i64(0))->strm.adler));
  });

  r.bind(def, "setDictionary", "(J[BII)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    no_dictionary(rt, "Deflater");
  }, S::kUnsupported, "dictionnaire zlib prédéfini : non pris en charge par miniz");
  r.bind(def, "setDictionaryBuffer", "(JJI)V", [](Runtime& rt, NativeArgs&) -> uint64_t {
    no_dictionary(rt, "Deflater");
  }, S::kUnsupported, "dictionnaire zlib prédéfini : non pris en charge par miniz");

  // (this, stream, input, output, flush, params)
  r.bind(def, "deflateBytesBytes", "(J[BII[BIIII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = deflater(rt, a.i64(1));
    auto in = byte_range(rt, a.h(3), a.i32(4), a.i32(5), "deflate (entrée)");
    auto out = byte_range(rt, a.h(6), a.i32(7), a.i32(8), "deflate (sortie)");
    return do_deflate(rt, *z, in, out, a.i32(9), a.i32(10));
  });

  r.bind(def, "deflateBytesBuffer", "(J[BIIJIII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = deflater(rt, a.i64(1));
    auto in = byte_range(rt, a.h(3), a.i32(4), a.i32(5), "deflate (entrée)");
    auto out = native_range(rt, a.i64(6), a.i32(8), true, "deflate (sortie)");
    return do_deflate(rt, *z, in, out, a.i32(9), a.i32(10));
  });

  r.bind(def, "deflateBufferBytes", "(JJI[BIIII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = deflater(rt, a.i64(1));
    auto in = native_range(rt, a.i64(3), a.i32(5), false, "deflate (entrée)");
    auto out = byte_range(rt, a.h(6), a.i32(7), a.i32(8), "deflate (sortie)");
    return do_deflate(rt, *z, in, out, a.i32(9), a.i32(10));
  });

  r.bind(def, "deflateBufferBuffer", "(JJIJIII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto z = deflater(rt, a.i64(1));
    auto in = native_range(rt, a.i64(3), a.i32(5), false, "deflate (entrée)");
    auto out = native_range(rt, a.i64(6), a.i32(8), true, "deflate (sortie)");
    return do_deflate(rt, *z, in, out, a.i32(9), a.i32(10));
  });

  // --- Checksums (static natives) ---------------------------------------------------------
  const char* crc = "Ljava/util/zip/CRC32;";
  r.bind(crc, "update", "(II)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    uint8_t b = uint8_t(a.raw(1));
    return rt::ret_int(int32_t(crc32(a.raw(0), {&b, 1})));
  });
  r.bind(crc, "updateBytes0", "(I[BII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(crc32(a.raw(0), byte_range(rt, a.h(1), a.i32(2), a.i32(3), "CRC32"))));
  });
  r.bind(crc, "updateByteBuffer0", "(IJII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t at = offset_address(rt, a.i64(1), a.i32(3), "CRC32");
    return rt::ret_int(int32_t(crc32(a.raw(0), native_range(rt, at, a.i32(4), false, "CRC32"))));
  });

  const char* adl = "Ljava/util/zip/Adler32;";
  r.bind(adl, "update", "(II)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    uint8_t b = uint8_t(a.raw(1));
    return rt::ret_int(int32_t(adler32(a.raw(0), {&b, 1})));
  });
  r.bind(adl, "updateBytes", "(I[BII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(adler32(a.raw(0), byte_range(rt, a.h(1), a.i32(2), a.i32(3), "Adler32"))));
  });
  r.bind(adl, "updateByteBuffer", "(IJII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t at = offset_address(rt, a.i64(1), a.i32(3), "Adler32");
    return rt::ret_int(int32_t(adler32(a.raw(0), native_range(rt, at, a.i32(4), false, "Adler32"))));
  });
}

}  // namespace aa::bridge
