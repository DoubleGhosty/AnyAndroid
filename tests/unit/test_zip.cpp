// java.util.zip natives (libcore_zip.cpp, on miniz) called as libcore's
// Deflater, Inflater, CRC32 and Adler32 call them.
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dex-engine/runtime.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/native_memory.h"
#include "test.h"

using namespace aa;

namespace {

const char* kDeflater = "Ljava/util/zip/Deflater;";
const char* kInflater = "Ljava/util/zip/Inflater;";
constexpr int32_t kNoFlush = 0, kSyncFlush = 2, kFullFlush = 3, kFinish = 4;

uint32_t lo(int64_t v) { return uint32_t(uint64_t(v)); }
uint32_t hi(int64_t v) { return uint32_t(uint64_t(v) >> 32); }

// Inflater/Deflater.*Bytes*'s packed result.
struct Step {
  uint32_t read, written;
  bool finished;  // bit 62
  bool more;      // bit 63: dictionary needed (Inflater), parameters not applied yet (Deflater)
};

Step unpack(uint64_t r) {
  return {uint32_t(r & 0x7fffffff), uint32_t((r >> 31) & 0x7fffffff), ((r >> 62) & 1) != 0, (r >> 63) != 0};
}

struct ZipVm {
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;

  ZipVm() {
    rt = std::make_unique<rt::Runtime>();
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
  ~ZipVm() {
    scope.reset();
    rt.reset();
  }

  uint64_t call(const char* cls, const char* name, const char* sig, std::vector<uint32_t> args) {
    const rt::NativeBinding* b = rt->registry().binding(cls, name, sig);
    CHECK(b != nullptr);
    rt::NativeArgs a(*rt, args.data(), uint32_t(args.size()));
    return b->fn(*rt, a);
  }
  // Class of the Java exception thrown by `f`, empty if none.
  template <typename F>
  std::string thrown(F f) {
    try {
      f();
    } catch (const rt::JavaThrow& t) {
      return rt->class_of(t.exception)->java_name();
    }
    return "";
  }
  rt::Handle bytes(std::string_view s) {
    rt::Handle h = rt->new_array("[B", int32_t(s.size()));
    if (!s.empty()) std::memcpy(rt->array(h, "b").data.data(), s.data(), s.size());
    return h;
  }

  int64_t new_deflater() { return int64_t(call(kDeflater, "init", "(IIZ)J", {uint32_t(-1), 0, 0})); }
  int64_t new_inflater() { return int64_t(call(kInflater, "init", "(Z)J", {0})); }
  void end(const char* cls, int64_t h) { call(cls, "end", "(J)V", {lo(h), hi(h)}); }
  Step deflate(int64_t h, rt::Handle in, int32_t in_off, int32_t in_len, rt::Handle out, int32_t out_off,
               int32_t out_len, int32_t flush, int32_t params = 0) {
    return unpack(call(kDeflater, "deflateBytesBytes", "(J[BII[BIIII)J",
                       {0, lo(h), hi(h), in, uint32_t(in_off), uint32_t(in_len), out, uint32_t(out_off),
                        uint32_t(out_len), uint32_t(flush), uint32_t(params)}));
  }
  Step inflate(int64_t h, rt::Handle in, int32_t in_len, rt::Handle out, int32_t out_len) {
    return unpack(call(kInflater, "inflateBytesBytes", "(J[BII[BII)J",
                       {0, lo(h), hi(h), in, 0, uint32_t(in_len), out, 0, uint32_t(out_len)}));
  }

  // The whole zlib stream in[0, len), inflated.
  std::string inflate_all(rt::Handle in, int32_t len) {
    int64_t h = new_inflater();
    rt::Handle out = rt->new_array("[B", 16384);
    Step s = inflate(h, in, len, out, 16384);
    end(kInflater, h);
    CHECK(s.finished);
    CHECK_EQ(s.read, uint32_t(len));
    return std::string(reinterpret_cast<const char*>(rt->array(out, "b").data.data()), s.written);
  }
};

// A Deflater writing into one Java byte[], as DeflaterOutputStream does.
struct Sink {
  static constexpr int32_t kCapacity = 8192;
  ZipVm& vm;
  int64_t h;
  rt::Handle out;
  int32_t size = 0;

  explicit Sink(ZipVm& v) : vm(v), h(v.new_deflater()), out(v.rt->new_array("[B", kCapacity)) {}
  ~Sink() { vm.end(kDeflater, h); }
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  // Deflates in[off, off + len) into at most `room` more bytes.
  Step deflate(rt::Handle in, int32_t off, int32_t len, int32_t flush, int32_t room = kCapacity, int32_t params = 0) {
    Step s = vm.deflate(h, in, off, len, out, size, std::min(room, kCapacity - size), flush, params);
    size += int32_t(s.written);
    return s;
  }
  std::string inflated() { return vm.inflate_all(out, size); }
};

std::string repeated(std::string_view s, int n) {
  std::string r;
  for (int i = 0; i < n; ++i) r += s;
  return r;
}

}  // namespace

TEST(zip_repeated_flush_without_input_writes_nothing) {
  ZipVm vm;
  std::string text = repeated("Bonjour zlib ! ", 20);
  int32_t len = int32_t(text.size());
  rt::Handle in = vm.bytes(text);
  Sink d(vm);
  Step s = d.deflate(in, 0, len, kSyncFlush);
  CHECK_EQ(s.read, uint32_t(len));
  CHECK(s.written > 0);
  // As zlib (deflate()'s last_flush): no new input and nothing pending, no
  // new empty block.
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush).written, 0u);
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush).written, 0u);
  CHECK_EQ(d.deflate(in, 0, 0, kNoFlush).written, 0u);
  // A stronger flush than the last one still writes its block, once.
  CHECK(d.deflate(in, 0, 0, kFullFlush).written > 0);
  CHECK_EQ(d.deflate(in, 0, 0, kFullFlush).written, 0u);
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush).written, 0u);

  // okio's DeflaterSink.flush(): deflate(SYNC_FLUSH) while it writes something.
  CHECK_EQ(d.deflate(in, 0, len, kNoFlush).read, uint32_t(len));
  int calls = 0;
  while (calls < 10 && d.deflate(in, 0, 0, kSyncFlush).written > 0) ++calls;
  CHECK_EQ(calls, 1);

  CHECK(d.deflate(in, 0, 0, kFinish).finished);
  CHECK_EQ(d.deflate(in, 0, 0, kFinish).written, 0u);
  CHECK(d.inflated() == text + text);
}

TEST(zip_flush_through_a_small_output_buffer_ends) {
  ZipVm vm;
  // Poorly compressible text: the flushed data spans many 7-byte windows.
  std::string text;
  uint32_t seed = 12345;
  for (int i = 0; i < 2000; ++i) {
    seed = seed * 1103515245u + 12345u;
    text += char('a' + (seed >> 16) % 26);
  }
  int32_t len = int32_t(text.size());
  rt::Handle in = vm.bytes(text);
  Sink d(vm);
  int32_t off = 0;
  int calls = 0;
  for (;; ++calls) {
    CHECK(calls < 1000);
    Step s = d.deflate(in, off, len - off, kSyncFlush, 7);
    off += int32_t(s.read);
    if (s.written == 0) break;
  }
  CHECK_EQ(off, len);
  CHECK(calls > 100);
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush, 7).written, 0u);
  for (calls = 0; !d.deflate(in, 0, 0, kFinish, 7).finished; ++calls) CHECK(calls < 10);
  CHECK(d.inflated() == text);
}

TEST(zip_reset_and_params_restart_flush_tracking) {
  ZipVm vm;
  std::string text = repeated("Le vif zéphyr jubile sur les kumquats du clown. ", 10);
  int32_t len = int32_t(text.size());
  rt::Handle in = vm.bytes(text);
  Sink d(vm);
  CHECK(d.deflate(in, 0, len, kSyncFlush).written > 0);
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush).written, 0u);

  // reset(): a new stream, whose first flush writes its header and a block.
  vm.call(kDeflater, "reset", "(J)V", {lo(d.h), hi(d.h)});
  d.size = 0;
  CHECK(d.deflate(in, 0, 0, kSyncFlush).written > 0);
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush).written, 0u);

  // setLevel(9) after some input: params = 1 | strategy << 1 | level << 3.
  CHECK_EQ(d.deflate(in, 0, len, kNoFlush).read, uint32_t(len));
  CHECK(!d.deflate(in, 0, 0, kNoFlush, Sink::kCapacity, 1 | (9 << 3)).more);  // applied
  // The input given so far is out; flushes repeat at most once more.
  d.deflate(in, 0, 0, kSyncFlush);
  CHECK_EQ(d.deflate(in, 0, 0, kSyncFlush).written, 0u);
  CHECK(d.deflate(in, 0, len, kFinish).finished);
  CHECK(d.inflated() == text + text);
}

TEST(zip_checksums_keep_their_value_on_empty_input) {
  ZipVm vm;
  const char* crc = "Ljava/util/zip/CRC32;";
  const char* adler = "Ljava/util/zip/Adler32;";
  rt::Handle digits = vm.bytes("123456789"), wiki = vm.bytes("Wikipedia"), none = vm.bytes("");
  auto crc_bytes = [&](uint32_t v, rt::Handle a, int32_t off, int32_t len) {
    return uint32_t(vm.call(crc, "updateBytes0", "(I[BII)I", {v, a, uint32_t(off), uint32_t(len)}));
  };
  auto adler_bytes = [&](uint32_t v, rt::Handle a, int32_t off, int32_t len) {
    return uint32_t(vm.call(adler, "updateBytes", "(I[BII)I", {v, a, uint32_t(off), uint32_t(len)}));
  };
  uint32_t c = crc_bytes(0, digits, 0, 9);
  CHECK_EQ(c, 0xCBF43926u);
  CHECK_EQ(crc_bytes(c, none, 0, 0), c);
  CHECK_EQ(crc_bytes(c, digits, 9, 0), c);
  uint32_t a = adler_bytes(1, wiki, 0, 9);
  CHECK_EQ(a, 0x11E60398u);
  CHECK_EQ(adler_bytes(a, none, 0, 0), a);
  CHECK_EQ(adler_bytes(a, wiki, 4, 0), a);
  CHECK_EQ(uint32_t(vm.call(adler, "update", "(II)I", {1, 'W'})), adler_bytes(1, wiki, 0, 1));
  CHECK_EQ(vm.thrown([&] { crc_bytes(c, digits, 5, 5); }), std::string("java.lang.ArrayIndexOutOfBoundsException"));

  // Direct buffers: emulated native addresses (native_memory.h).
  int64_t buf = bridge::native_alloc(*vm.rt, 18);
  std::memcpy(bridge::native_span(*vm.rt, buf, 18, true).data(), "123456789Wikipedia", 18);
  auto crc_buffer = [&](uint32_t v, int32_t off, int32_t len) {
    return uint32_t(vm.call(crc, "updateByteBuffer0", "(IJII)I", {v, lo(buf), hi(buf), uint32_t(off), uint32_t(len)}));
  };
  auto adler_buffer = [&](uint32_t v, int32_t off, int32_t len) {
    return uint32_t(vm.call(adler, "updateByteBuffer", "(IJII)I", {v, lo(buf), hi(buf), uint32_t(off), uint32_t(len)}));
  };
  CHECK_EQ(crc_buffer(0, 0, 9), c);
  CHECK_EQ(crc_buffer(c, 9, 0), c);
  CHECK_EQ(adler_buffer(1, 9, 9), a);
  CHECK_EQ(adler_buffer(a, 18, 0), a);
  CHECK_EQ(vm.thrown([&] { adler_buffer(1, 10, 9); }), std::string("java.lang.IndexOutOfBoundsException"));
  bridge::native_free(*vm.rt, buf);
}

TEST(zip_stream_handles_are_checked) {
  ZipVm vm;
  int64_t inf = vm.new_inflater(), def = vm.new_deflater();
  CHECK((uint64_t(inf) >> 48) != (uint64_t(def) >> 48));
  rt::Handle in = vm.bytes("abc"), out = vm.rt->new_array("[B", 64);
  // An Inflater's handle is not a Deflater's, and an ended stream is gone.
  CHECK_EQ(vm.thrown([&] { vm.deflate(inf, in, 0, 3, out, 0, 64, kSyncFlush); }),
           std::string("java.lang.NullPointerException"));
  CHECK(vm.deflate(def, in, 0, 3, out, 0, 64, kSyncFlush).written > 0);
  vm.end(kDeflater, def);
  vm.end(kDeflater, def);  // a second end() is a no-op
  CHECK_EQ(vm.thrown([&] { vm.deflate(def, in, 0, 3, out, 0, 64, kSyncFlush); }),
           std::string("java.lang.NullPointerException"));
  vm.end(kInflater, inf);
}

TEST(zip_live_streams_are_capped) {
  ZipVm vm;
  for (const char* cls : {kDeflater, kInflater}) {
    auto open = [&] { return cls == kDeflater ? vm.new_deflater() : vm.new_inflater(); };
    std::vector<int64_t> live;
    for (;;) {
      std::string error = vm.thrown([&] { live.push_back(open()); });
      if (!error.empty()) {
        CHECK_EQ(error, std::string("java.lang.OutOfMemoryError"));
        break;
      }
      CHECK(live.size() <= 1024);  // ~312 KiB per Deflater, ~40 KiB per Inflater
    }
    CHECK(live.size() >= 64);
    // end() gives the place back.
    vm.end(cls, live.back());
    live.pop_back();
    live.push_back(open());
    for (int64_t h : live) vm.end(cls, h);
  }
}

// zlib's and Deflater.deflate's contract: a SYNC_FLUSH returning less than the
// room offered has written the whole flush, so everything given so far can be
// inflated. miniz stops after copying out output left by earlier calls.
TEST(zip_sync_flush_short_return_is_complete) {
  ZipVm vm;
  constexpr int32_t kInput = 100000, kRoom = 512, kOut = 1 << 20;
  std::string text(kInput, ' ');
  uint32_t seed = 12345;
  for (char& c : text) {
    seed = seed * 1103515245u + 12345u;
    c = "abcdefgh ijkl\n"[(seed >> 16) % 14];
  }
  rt::Handle in = vm.bytes(text);
  rt::Handle out = vm.rt->new_array("[B", kOut);
  int64_t h = vm.new_deflater();
  int32_t size = 0;
  // One byte at a time, each call deflating until the byte is taken but never
  // draining the compressor (OpenJDK's DeflaterOutputStream.write pattern):
  // miniz keeps output pending between calls.
  // Feeding stops right after a byte that made miniz emit a block larger than
  // the room offered: the rest of that block is still pending.
  int32_t fed = 0;
  bool pending = false;
  while (fed < kInput && !pending) {
    for (uint32_t taken = 0; taken == 0;) {
      CHECK(size + kRoom <= kOut);
      Step s = vm.deflate(h, in, fed, 1, out, size, kRoom, kNoFlush);
      taken = s.read;
      size += int32_t(s.written);
      pending = taken == 1 && s.written == uint32_t(kRoom);
    }
    ++fed;
  }
  CHECK(pending);
  for (int round = 0;; ++round) {
    CHECK(round < 4096);
    Step s = vm.deflate(h, in, 0, 0, out, size, kRoom, kSyncFlush);
    size += int32_t(s.written);
    if (s.written < uint32_t(kRoom)) break;  // flush complete, per the contract
  }
  vm.end(kDeflater, h);
  int64_t inf = vm.new_inflater();
  rt::Handle plain = vm.rt->new_array("[B", kInput + 16);
  Step s = vm.inflate(inf, out, size, plain, kInput + 16);
  vm.end(kInflater, inf);
  CHECK(!s.finished);
  CHECK_EQ(s.written, uint32_t(fed));
  CHECK(std::memcmp(vm.rt->array(plain, "b").data.data(), text.data(), size_t(fed)) == 0);
}

// Streams never ended are freed with the Inflater that used them (its Cleaner
// on Android): opening more than the cap does not run out.
TEST(zip_unended_streams_are_freed_with_their_owner) {
  ZipVm vm;
  rt::Handle gz = vm.bytes(std::string("\x78\x9c\x4b\x4c\x4a\x06\x00\x02\x4d\x01\x27", 11));  // zlib("abc")
  rt::GlobalRef keep(*vm.rt, gz);
  rt::Handle out = vm.rt->new_array("[B", 16);
  rt::GlobalRef keep_out(*vm.rt, out);
  for (int i = 0; i < 1100; ++i) {
    int64_t h = vm.new_inflater();
    rt::Handle owner = vm.rt->alloc(vm.rt->find_class("Ljava/lang/Object;"));
    Step s = unpack(vm.call(kInflater, "inflateBytesBytes", "(J[BII[BII)J",
                            {owner, lo(h), hi(h), gz, 0, 11, out, 0, 16}));
    CHECK(s.finished);
    CHECK_EQ(s.written, 3u);
  }
}

// zlib never updates adler on raw (nowrap) streams: 1 for a Deflater, 0 for
// an Inflater.
TEST(zip_nowrap_streams_keep_zlib_adler) {
  ZipVm vm;
  int64_t d = int64_t(vm.call(kDeflater, "init", "(IIZ)J", {uint32_t(-1), 0, 1}));
  rt::Handle in = vm.bytes("abc");
  rt::Handle out = vm.rt->new_array("[B", 64);
  Step s = vm.deflate(d, in, 0, 3, out, 0, 64, kFinish);
  CHECK(s.finished);
  CHECK_EQ(int32_t(vm.call(kDeflater, "getAdler", "(J)I", {lo(d), hi(d)})), 1);
  vm.end(kDeflater, d);
  int64_t i = int64_t(vm.call(kInflater, "init", "(Z)J", {1}));
  CHECK_EQ(int32_t(vm.call(kInflater, "getAdler", "(J)I", {lo(i), hi(i)})), 0);
  rt::Handle plain = vm.rt->new_array("[B", 16);
  Step t = vm.inflate(i, out, int32_t(s.written), plain, 16);
  CHECK(t.finished);
  CHECK_EQ(t.written, 3u);
  CHECK_EQ(int32_t(vm.call(kInflater, "getAdler", "(J)I", {lo(i), hi(i)})), 0);
  vm.end(kInflater, i);
}
