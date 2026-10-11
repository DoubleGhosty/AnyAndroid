// java.util.regex natives of Android's i18n module (com.android.icu.util.regex
// PatternNative and MatcherNative, ICU4C's uregex on a phone) on top of
// AnyAndroid's ICU-compatible engine (regex/regex.h).
//
// Patterns and matchers live in bounded handle tables. The static factories
// PatternNative.create and MatcherNative.create are replaced so that each
// handle is owned by its Java object and released when the collector frees
// it (AnyAndroid never runs the NativeAllocationRegistry cleaners). Offsets
// arrays are bounds-checked; a match that exhausts its step or backtracking
// budget throws RuntimeException, as an ICU error would.
#include "framework-bridge/internal.h"

#include <cstring>
#include <memory>
#include <string>

#include "framework-bridge/handles.h"
#include "regex/regex.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr size_t kMaxLive = 1u << 20;  // patterns, and matchers, alive at once

struct PatternHolder {
  std::shared_ptr<const regex::Pattern> pattern;
};
struct MatcherHolder {
  explicit MatcherHolder(std::shared_ptr<const regex::Pattern> p) : matcher(std::move(p)) {}
  regex::Matcher matcher;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<PatternHolder> patterns{0x0f01};
  HandleTable<MatcherHolder> matchers{0x0f02};
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed: owners may be freed at exit
  return *t;
}

// Releases a handle when the collector frees the Java object owning it.
struct HandleOwner final : rt::NativePeer {
  HandleOwner(bool is_matcher, int64_t h) : matcher(is_matcher), handle(h) {}
  ~HandleOwner() override {
    if (matcher) {
      tables().matchers.remove(handle);
    } else {
      tables().patterns.remove(handle);
    }
  }
  bool matcher;
  int64_t handle;
};

std::shared_ptr<PatternHolder> pattern(Runtime& rt, int64_t h) {
  auto p = tables().patterns.get(h);
  if (!p) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] motif d'expression régulière invalide");
  return p;
}
std::shared_ptr<MatcherHolder> matcher(Runtime& rt, int64_t h) {
  auto m = tables().matchers.get(h);
  if (!m) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] matcher d'expression régulière invalide");
  return m;
}

int64_t compile(Runtime& rt, Handle regex, int32_t flags) {
  if (regex == kNull) rt.throw_npe("regex");
  if (tables().patterns.live() >= kMaxLive) {
    rt.throw_new("Ljava/lang/OutOfMemoryError;", "[AnyAndroid] trop d'expressions régulières vivantes");
  }
  std::u16string text = rt.string_value(regex);
  std::shared_ptr<const regex::Pattern> p;
  try {
    p = regex::Pattern::compile(text, uint32_t(flags) & (regex::kUnixLines | regex::kCaseInsensitive |
                                                         regex::kComments | regex::kMultiline | regex::kDotAll));
  } catch (const regex::SyntaxError& e) {
    Handle desc = rt.new_string_utf8(e.what());
    rt::GlobalRef keep(rt, desc);
    Handle ex = construct(rt, "Ljava/util/regex/PatternSyntaxException;", "(Ljava/lang/String;Ljava/lang/String;I)V",
                          {desc, regex, uint32_t(e.index)});
    throw rt::JavaThrow{ex};
  }
  auto holder = std::make_shared<PatternHolder>();
  holder->pattern = std::move(p);
  return tables().patterns.add(std::move(holder));
}

int64_t open_matcher(Runtime& rt, int64_t pattern_handle) {
  auto p = pattern(rt, pattern_handle);
  if (tables().matchers.live() >= kMaxLive) {
    rt.throw_new("Ljava/lang/OutOfMemoryError;", "[AnyAndroid] trop de matchers d'expressions régulières vivants");
  }
  return tables().matchers.add(std::make_shared<MatcherHolder>(p->pattern));
}

void set_long_field(Runtime& rt, Handle obj, const char* name, int64_t value) {
  rt::Object& o = rt.nonnull(obj, "objet");
  rt::Field* f = find_field(o.klass, name, "J");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/InternalError;", std::string("champ absent : ") + name);
  o.fields.at(f->slot) = uint32_t(uint64_t(value));
  o.fields.at(f->slot + 1) = uint32_t(uint64_t(value) >> 32);
}

// Creates an object of `cls` owning `h` (its constructor is not run: the
// fields it would set are set here).
Handle owning_object(Runtime& rt, const char* cls, bool is_matcher, int64_t h) {
  rt::Class* c = rt.find_class(cls);
  rt.ensure_initialized(c);
  Handle obj = rt.alloc(c);
  set_long_field(rt, obj, "address", h);
  rt.object(obj)->peer = std::make_unique<HandleOwner>(is_matcher, h);
  return obj;
}

// Writes the group offsets of the last match into `offsets` (start/end pairs).
void write_offsets(Runtime& rt, Handle offsets, const regex::Matcher& m) {
  if (offsets == kNull) rt.throw_npe("offsets");
  rt::ArrayObject& arr = rt.array(offsets, "int[]");
  const auto& g = m.groups();
  size_t n = std::min<size_t>(arr.length, g.size());
  for (size_t i = 0; i < n; ++i) std::memcpy(arr.data.data() + 4 * i, &g[i], 4);
}

template <typename F>
uint64_t run_match(Runtime& rt, int64_t h, Handle offsets, F op) {
  auto m = matcher(rt, h);
  bool found;
  try {
    found = op(m->matcher);
  } catch (const regex::ResourceError& e) {
    rt.throw_new("Ljava/lang/RuntimeException;", std::string("[AnyAndroid] ") + e.what());
  }
  if (found) write_offsets(rt, offsets, m->matcher);
  return rt::ret_bool(found);
}

}  // namespace

void register_libcore_regex(rt::NativeRegistry& r) {
  const char* pn = "Lcom/android/icu/util/regex/PatternNative;";
  const char* mn = "Lcom/android/icu/util/regex/MatcherNative;";
  static const int64_t pattern_free = register_free_function([](int64_t h) { tables().patterns.remove(h); });
  static const int64_t matcher_free = register_free_function([](int64_t h) { tables().matchers.remove(h); });

  r.intrinsic(pn, "create", "(Ljava/lang/String;I)Lcom/android/icu/util/regex/PatternNative;",
              [pn](Runtime& rt, NativeArgs& a) -> uint64_t {
                return owning_object(rt, pn, false, compile(rt, a.h(0), a.i32(1)));
              },
              "motif possédé par l'objet Java, libéré par le ramasse-miettes");
  r.bind(pn, "compileImpl", "(Ljava/lang/String;I)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return uint64_t(compile(rt, a.h(0), a.i32(1))); });
  r.bind(pn, "getNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(pattern_free); });
  r.bind(pn, "openMatcherImpl", "(J)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return uint64_t(open_matcher(rt, a.i64(0))); });
  r.bind(pn, "getMatchedGroupIndexImpl", "(JLjava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(2) == kNull) rt.throw_npe("name");
    return rt::ret_int(pattern(rt, a.i64(0))->pattern->group_index(rt.string_value(a.h(2))));
  });

  r.intrinsic(mn, "create", "(Lcom/android/icu/util/regex/PatternNative;)Lcom/android/icu/util/regex/MatcherNative;",
              [mn](Runtime& rt, NativeArgs& a) -> uint64_t {
                rt::Object& p = rt.nonnull(a.h(0), "PatternNative");
                rt::Field* addr = find_field(p.klass, "address", "J");
                if (!addr) rt.throw_new("Ljava/lang/InternalError;", "PatternNative.address absent");
                int64_t ph = int64_t(uint64_t(p.fields.at(addr->slot)) | (uint64_t(p.fields.at(addr->slot + 1)) << 32));
                Handle obj = owning_object(rt, mn, true, open_matcher(rt, ph));
                rt::Object& o = *rt.object(obj);
                if (rt::Field* f = find_field(o.klass, "nativePattern", "Lcom/android/icu/util/regex/PatternNative;")) {
                  o.fields.at(f->slot) = a.h(0);
                }
                return obj;
              },
              "matcher possédé par l'objet Java, libéré par le ramasse-miettes");
  r.bind(mn, "getNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(matcher_free); });
  r.bind(mn, "setInputImpl", "(JLjava/lang/String;II)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(2) == kNull) rt.throw_npe("input");
    matcher(rt, a.i64(0))->matcher.set_input(rt.string_value(a.h(2)), a.i32(3), a.i32(4));
    return 0;
  });
  r.bind(mn, "findImpl", "(JI[I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t start = a.i32(2);
    return run_match(rt, a.i64(0), a.h(3), [start](regex::Matcher& m) { return m.find(start); });
  });
  r.bind(mn, "findNextImpl", "(J[I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return run_match(rt, a.i64(0), a.h(2), [](regex::Matcher& m) { return m.find_next(); });
  });
  r.bind(mn, "matchesImpl", "(J[I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return run_match(rt, a.i64(0), a.h(2), [](regex::Matcher& m) { return m.matches(); });
  });
  r.bind(mn, "lookingAtImpl", "(J[I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return run_match(rt, a.i64(0), a.h(2), [](regex::Matcher& m) { return m.looking_at(); });
  });
  r.bind(mn, "groupCountImpl", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(matcher(rt, a.i64(0))->matcher.group_count());
  });
  r.bind(mn, "hitEndImpl", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(matcher(rt, a.i64(0))->matcher.hit_end());
  });
  r.bind(mn, "requireEndImpl", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(matcher(rt, a.i64(0))->matcher.require_end());
  });
  r.bind(mn, "useAnchoringBoundsImpl", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    matcher(rt, a.i64(0))->matcher.use_anchoring_bounds(a.z(2));
    return 0;
  });
  r.bind(mn, "useTransparentBoundsImpl", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    matcher(rt, a.i64(0))->matcher.use_transparent_bounds(a.z(2));
    return 0;
  });
}

}  // namespace aa::bridge
