// Dalvik bytecode interpreter: a plain switch over pre-decoded instructions.
// Semantics follow the Dalvik bytecode reference
// (https://source.android.com/docs/core/runtime/dalvik-bytecode).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/bytes.h"
#include "common/text.h"
#include "dex-engine/opcodes.h"
#include "dex-engine/runtime.h"

namespace aa::rt {

namespace {

struct FramePop {
  std::vector<Frame>& frames;
  ~FramePop() { frames.pop_back(); }
};

inline float as_float(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
inline uint32_t float_bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
inline double as_double(uint64_t b) {
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
inline uint64_t double_bits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

// Java narrowing of floating-point values: NaN -> 0, saturating.
template <typename I, typename F>
I java_f2i(F v) {
  if (std::isnan(v)) return 0;
  if (v >= F(std::numeric_limits<I>::max())) return std::numeric_limits<I>::max();
  if (v <= F(std::numeric_limits<I>::min())) return std::numeric_limits<I>::min();
  return I(v);
}

template <typename F>
int32_t compare_fp(F x, F y, int32_t nan_result) {
  if (std::isnan(x) || std::isnan(y)) return nan_result;
  return x < y ? -1 : (x > y ? 1 : 0);
}

// Integer operations shared by the 23x, 2addr and literal forms.
// kind: 0 add, 1 sub, 2 mul, 3 div, 4 rem, 5 and, 6 or, 7 xor, 8 shl, 9 shr, 10 ushr.
int32_t int_op(Runtime& rt, int kind, int32_t x, int32_t y) {
  uint32_t ux = uint32_t(x), uy = uint32_t(y);
  switch (kind) {
    case 0: return int32_t(ux + uy);
    case 1: return int32_t(ux - uy);
    case 2: return int32_t(ux * uy);
    case 3:
      if (y == 0) rt.throw_new("Ljava/lang/ArithmeticException;", "divide by zero");
      if (x == std::numeric_limits<int32_t>::min() && y == -1) return x;
      return x / y;
    case 4:
      if (y == 0) rt.throw_new("Ljava/lang/ArithmeticException;", "divide by zero");
      if (y == -1) return 0;
      return x % y;
    case 5: return x & y;
    case 6: return x | y;
    case 7: return x ^ y;
    case 8: return int32_t(ux << (uy & 31));
    case 9: return x >> (uy & 31);
    case 10: return int32_t(ux >> (uy & 31));
  }
  return 0;
}

int64_t long_op(Runtime& rt, int kind, int64_t x, int64_t y) {
  uint64_t ux = uint64_t(x), uy = uint64_t(y);
  switch (kind) {
    case 0: return int64_t(ux + uy);
    case 1: return int64_t(ux - uy);
    case 2: return int64_t(ux * uy);
    case 3:
      if (y == 0) rt.throw_new("Ljava/lang/ArithmeticException;", "divide by zero");
      if (x == std::numeric_limits<int64_t>::min() && y == -1) return x;
      return x / y;
    case 4:
      if (y == 0) rt.throw_new("Ljava/lang/ArithmeticException;", "divide by zero");
      if (y == -1) return 0;
      return x % y;
    case 5: return x & y;
    case 6: return x | y;
    case 7: return x ^ y;
    case 8: return int64_t(ux << (uy & 63));
    case 9: return x >> (uy & 63);
    case 10: return int64_t(ux >> (uy & 63));
  }
  return 0;
}

template <typename F>
F fp_op(int kind, F x, F y) {
  switch (kind) {
    case 0: return x + y;
    case 1: return x - y;
    case 2: return x * y;
    case 3: return x / y;
    case 4: return std::fmod(x, y);
  }
  return 0;
}

// Expected element width for aget/aput variants (index 0..6).
constexpr uint8_t kArrayWidth[7] = {4, 8, 4, 1, 1, 2, 2};

bool array_kind_ok(int k, const Class* array_class) {
  char c = array_class->component->primitive;
  switch (k) {
    case 0: return c == 'I' || c == 'F';
    case 1: return c == 'J' || c == 'D';
    case 2: return c == 0;
    case 3:
    case 4: return c == 'Z' || c == 'B';
    case 5: return c == 'C';
    case 6: return c == 'S';
  }
  return false;
}

uint32_t narrow_store(int k, uint32_t v) {
  switch (k) {
    case 3: return v & 0xFF;                      // boolean
    case 4: return uint32_t(int32_t(int8_t(v)));  // byte
    case 5: return v & 0xFFFF;                    // char
    case 6: return uint32_t(int32_t(int16_t(v))); // short
  }
  return v;
}

}  // namespace

uint64_t Runtime::execute(Method* m, const uint32_t* args, uint32_t nargs) {
  PreparedCode& code = prepared(m);
  if (nargs != code.ins) {
    throw_new("Ljava/lang/VerifyError;", m->pretty() + " : ins_size incohérent avec la signature");
  }
  DexCache& cache = *m->owner->dex_cache;
  const dex::DexFile& dex = *cache.dex;

  // One spare register so that a wide access to the last register stays in bounds.
  std::vector<uint32_t> regs(size_t(code.registers) + 1, 0);
  if (nargs) std::memcpy(&regs[code.registers - code.ins], args, nargs * sizeof(uint32_t));

  VmThread* const t = running_;
  t->frames.push_back({m, 0, regs.data(), uint32_t(regs.size())});
  FramePop pop{t->frames};
  const size_t fi = t->frames.size() - 1;
  // Safepoint: collection, GIL hand-off to other threads, shutdown.
  auto poll = [this] {
    if (gc_requested_.load(std::memory_order_relaxed) || gil_waiters_.load(std::memory_order_relaxed) > 0 ||
        shutting_down_.load(std::memory_order_relaxed)) {
      safepoint();
    }
  };
  poll();

  auto wide = [&](uint32_t r) { return uint64_t(regs[r]) | (uint64_t(regs[r + 1]) << 32); };
  auto set_wide = [&](uint32_t r, uint64_t v) {
    regs[r] = uint32_t(v);
    regs[r + 1] = uint32_t(v >> 32);
  };

  const std::vector<Insn>& insns = code.insns;
  uint64_t result = 0;
  Handle pending_exception = kNull;
  size_t i = 0;
  uint32_t argbuf[256];
  const bool profile = options_.profile;

  for (;;) {
    try {
      for (;;) {
        if (i >= insns.size()) throw_new("Ljava/lang/VerifyError;", m->pretty() + " : sortie de la fin du code");
        const Insn& in = insns[i];
        t->frames[fi].pc = in.pc;
        stats_.instructions++;
        if (profile) m->profile_instructions++;
        if (options_.max_instructions && ++t->budget_used > options_.max_instructions) {
          throw_new("Ljava/lang/Error;", "[AnyAndroid] budget d'instructions dépassé (boucle infinie ?)");
        }
        const uint8_t op = in.op;
        switch (op) {
          case 0x00:  // nop
            ++i;
            break;
          case 0x01: case 0x02: case 0x03:
          case 0x07: case 0x08: case 0x09:
            regs[in.a] = regs[in.b];
            ++i;
            break;
          case 0x04: case 0x05: case 0x06:
            set_wide(in.a, wide(in.b));
            ++i;
            break;
          case 0x0a:  // move-result
          case 0x0c:  // move-result-object
            regs[in.a] = uint32_t(result);
            ++i;
            break;
          case 0x0b:
            set_wide(in.a, result);
            ++i;
            break;
          case 0x0d:  // move-exception
            regs[in.a] = pending_exception;
            pending_exception = kNull;
            ++i;
            break;
          case 0x0e:
            return 0;
          case 0x0f:
          case 0x11:
            return regs[in.a];
          case 0x10:
            return wide(in.a);
          case 0x12: case 0x13: case 0x14:
            regs[in.a] = uint32_t(int32_t(in.lit));
            ++i;
            break;
          case 0x15:
            regs[in.a] = uint32_t(in.lit) << 16;
            ++i;
            break;
          case 0x16: case 0x17: case 0x18:
            set_wide(in.a, uint64_t(in.lit));
            ++i;
            break;
          case 0x19:
            set_wide(in.a, uint64_t(in.lit) << 48);
            ++i;
            break;
          case 0x1a: case 0x1b:  // const-string
            regs[in.a] = resolve_string(cache, in.index);
            ++i;
            break;
          case 0x1c:  // const-class
            regs[in.a] = class_mirror(resolve_type(cache, in.index));
            ++i;
            break;
          case 0x1d:  // monitor-enter
            monitor_enter(regs[in.a]);
            ++i;
            break;
          case 0x1e:  // monitor-exit
            monitor_exit(regs[in.a]);
            ++i;
            break;
          case 0x1f: {  // check-cast
            Handle h = regs[in.a];
            if (h != kNull) {
              Class* target = resolve_type(cache, in.index);
              Class* actual = class_of(h);
              if (!is_assignable(actual, target)) {
                throw_new("Ljava/lang/ClassCastException;",
                          actual->java_name() + " cannot be cast to " + target->java_name());
              }
            }
            ++i;
            break;
          }
          case 0x20: {  // instance-of
            Class* target = resolve_type(cache, in.index);
            regs[in.a] = instance_of(regs[in.b], target) ? 1 : 0;
            ++i;
            break;
          }
          case 0x21:
            regs[in.a] = array(regs[in.b], "array-length sur null").length;
            ++i;
            break;
          case 0x22: {  // new-instance
            Class* cls = resolve_type(cache, in.index);
            ensure_initialized(cls);
            regs[in.a] = alloc(cls);
            ++i;
            break;
          }
          case 0x23: {  // new-array
            Class* cls = resolve_type(cache, in.index);
            regs[in.a] = new_array(cls, int32_t(regs[in.b]));
            ++i;
            break;
          }
          case 0x24: case 0x25: {  // filled-new-array
            Class* cls = resolve_type(cache, in.index);
            if (!cls->is_array() || (cls->component->primitive && cls->component->primitive != 'I')) {
              throw_new("Ljava/lang/InternalError;", "filled-new-array : type " + cls->java_name() + " non pris en charge");
            }
            Handle h = new_array(cls, in.argc);
            ArrayObject& a = array(h, "filled-new-array");
            for (uint32_t k = 0; k < in.argc; ++k) {
              uint32_t v = op == 0x24 ? regs[in.args[k]] : regs[in.c + k];
              std::memcpy(&a.data[k * 4], &v, 4);
            }
            result = h;
            ++i;
            break;
          }
          case 0x26: {  // fill-array-data
            ArrayObject& a = array(regs[in.a], "fill-array-data sur null");
            const ArrayPayload& p = code.arrays.at(size_t(in.target));
            if (p.width != a.width) throw_new("Ljava/lang/VerifyError;", "fill-array-data : largeur incohérente");
            if (p.count > a.length) {
              throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
                        "length=" + std::to_string(a.length) + "; index=" + std::to_string(p.count - 1));
            }
            std::memcpy(a.data.data(), p.data.data(), p.data.size());
            ++i;
            break;
          }
          case 0x27: {  // throw
            Handle h = regs[in.a];
            if (h == kNull) throw_npe("throw de null");
            if (!instance_of(h, find_class("Ljava/lang/Throwable;"))) {
              throw_new("Ljava/lang/VerifyError;", "throw d'un objet qui n'est pas Throwable");
            }
            throw JavaThrow{h};
          }
          case 0x28: case 0x29: case 0x2a:
            if (size_t(in.target) <= i) poll();
            i = size_t(in.target);
            break;
          case 0x2b: case 0x2c: {
            const SwitchTable& st = code.switches.at(size_t(in.target));
            int32_t v = int32_t(regs[in.a]);
            int32_t dest = -1;
            if (st.packed) {
              int64_t idx = int64_t(v) - st.first_key;
              if (idx >= 0 && idx < int64_t(st.targets.size())) dest = st.targets[size_t(idx)];
            } else {
              auto it = std::lower_bound(st.keys.begin(), st.keys.end(), v);
              if (it != st.keys.end() && *it == v) dest = st.targets[size_t(it - st.keys.begin())];
            }
            if (dest >= 0 && size_t(dest) <= i) poll();
            i = dest >= 0 ? size_t(dest) : i + 1;
            break;
          }
          case 0x2d: regs[in.a] = uint32_t(compare_fp(as_float(regs[in.b]), as_float(regs[in.c]), -1)); ++i; break;
          case 0x2e: regs[in.a] = uint32_t(compare_fp(as_float(regs[in.b]), as_float(regs[in.c]), 1)); ++i; break;
          case 0x2f: regs[in.a] = uint32_t(compare_fp(as_double(wide(in.b)), as_double(wide(in.c)), -1)); ++i; break;
          case 0x30: regs[in.a] = uint32_t(compare_fp(as_double(wide(in.b)), as_double(wide(in.c)), 1)); ++i; break;
          case 0x31: {
            int64_t x = int64_t(wide(in.b)), y = int64_t(wide(in.c));
            regs[in.a] = uint32_t(x < y ? -1 : (x > y ? 1 : 0));
            ++i;
            break;
          }
          case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
            int32_t x = int32_t(regs[in.a]), y = int32_t(regs[in.b]);
            bool taken = false;
            switch (op) {
              case 0x32: taken = x == y; break;
              case 0x33: taken = x != y; break;
              case 0x34: taken = x < y; break;
              case 0x35: taken = x >= y; break;
              case 0x36: taken = x > y; break;
              case 0x37: taken = x <= y; break;
            }
            if (taken && size_t(in.target) <= i) poll();
            i = taken ? size_t(in.target) : i + 1;
            break;
          }
          case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
            int32_t x = int32_t(regs[in.a]);
            bool taken = false;
            switch (op) {
              case 0x38: taken = x == 0; break;
              case 0x39: taken = x != 0; break;
              case 0x3a: taken = x < 0; break;
              case 0x3b: taken = x >= 0; break;
              case 0x3c: taken = x > 0; break;
              case 0x3d: taken = x <= 0; break;
            }
            if (taken && size_t(in.target) <= i) poll();
            i = taken ? size_t(in.target) : i + 1;
            break;
          }
          case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:
          case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {
            const bool is_put = op >= 0x4b;
            const int k = op - (is_put ? 0x4b : 0x44);
            ArrayObject& a = array(regs[in.b], is_put ? "aput sur un tableau null" : "aget sur un tableau null");
            if (a.width != kArrayWidth[k] || !array_kind_ok(k, a.klass)) {
              throw_new("Ljava/lang/VerifyError;", std::string(dex::op_info(op).name) + " sur " + a.klass->java_name());
            }
            int32_t idx = int32_t(regs[in.c]);
            if (idx < 0 || uint32_t(idx) >= a.length) {
              throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
                        "length=" + std::to_string(a.length) + "; index=" + std::to_string(idx));
            }
            uint8_t* slot = a.data.data() + size_t(idx) * a.width;
            if (!is_put) {
              switch (k) {
                case 1: {
                  uint64_t v;
                  std::memcpy(&v, slot, 8);
                  set_wide(in.a, v);
                  break;
                }
                case 3: regs[in.a] = slot[0]; break;
                case 4: regs[in.a] = uint32_t(int32_t(int8_t(slot[0]))); break;
                case 5: {
                  uint16_t v;
                  std::memcpy(&v, slot, 2);
                  regs[in.a] = v;
                  break;
                }
                case 6: {
                  int16_t v;
                  std::memcpy(&v, slot, 2);
                  regs[in.a] = uint32_t(int32_t(v));
                  break;
                }
                default: std::memcpy(&regs[in.a], slot, 4);
              }
            } else {
              switch (k) {
                case 1: {
                  uint64_t v = wide(in.a);
                  std::memcpy(slot, &v, 8);
                  break;
                }
                case 2: {
                  Handle v = regs[in.a];
                  if (v != kNull && !is_assignable(class_of(v), a.klass->component)) {
                    throw_new("Ljava/lang/ArrayStoreException;",
                              class_of(v)->java_name() + " cannot be stored in an array of type " + a.klass->java_name());
                  }
                  std::memcpy(slot, &v, 4);
                  break;
                }
                case 3:
                case 4: slot[0] = uint8_t(regs[in.a]); break;
                case 5:
                case 6: {
                  uint16_t v = uint16_t(regs[in.a]);
                  std::memcpy(slot, &v, 2);
                  break;
                }
                default: std::memcpy(slot, &regs[in.a], 4);
              }
            }
            ++i;
            break;
          }
          case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
          case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
            const bool is_put = op >= 0x59;
            const int k = op - (is_put ? 0x59 : 0x52);
            Field* f = resolve_field(cache, in.index, false);
            if (f->wide() != (k == 1)) throw_new("Ljava/lang/VerifyError;", "accès de largeur incohérente au champ " + f->name);
            Object& o = nonnull(regs[in.b], is_put ? "écriture d'un champ sur null" : "lecture d'un champ sur null");
            if (!is_assignable(o.klass, f->owner) || f->slot + (k == 1 ? 2u : 1u) > o.fields.size()) {
              throw_new("Ljava/lang/IncompatibleClassChangeError;",
                        "champ " + f->owner->java_name() + "." + f->name + " sur " + o.klass->java_name());
            }
            if (!is_put) {
              if (k == 1) {
                set_wide(in.a, uint64_t(o.fields[f->slot]) | (uint64_t(o.fields[f->slot + 1]) << 32));
              } else {
                regs[in.a] = o.fields[f->slot];
              }
            } else if (k == 1) {
              uint64_t v = wide(in.a);
              o.fields[f->slot] = uint32_t(v);
              o.fields[f->slot + 1] = uint32_t(v >> 32);
            } else {
              o.fields[f->slot] = narrow_store(k, regs[in.a]);
            }
            ++i;
            break;
          }
          case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:
          case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: {
            const bool is_put = op >= 0x67;
            const int k = op - (is_put ? 0x67 : 0x60);
            Field* f = resolve_field(cache, in.index, true);
            if (f->wide() != (k == 1)) throw_new("Ljava/lang/VerifyError;", "accès de largeur incohérente au champ " + f->name);
            ensure_initialized(f->owner);
            std::vector<uint32_t>& s = f->owner->statics;
            if (f->slot + (k == 1 ? 2u : 1u) > s.size()) throw_new("Ljava/lang/VerifyError;", "champ statique invalide");
            if (!is_put) {
              if (k == 1) {
                set_wide(in.a, uint64_t(s[f->slot]) | (uint64_t(s[f->slot + 1]) << 32));
              } else {
                regs[in.a] = s[f->slot];
              }
            } else if (k == 1) {
              uint64_t v = wide(in.a);
              s[f->slot] = uint32_t(v);
              s[f->slot + 1] = uint32_t(v >> 32);
            } else {
              s[f->slot] = narrow_store(k, regs[in.a]);
            }
            ++i;
            break;
          }
          case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
          case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
            const bool range = op >= 0x74;
            const int kind = op - (range ? 0x74 : 0x6e);  // 0 virtual, 1 super, 2 direct, 3 static, 4 interface
            const uint32_t n = in.argc;
            for (uint32_t k = 0; k < n; ++k) argbuf[k] = range ? regs[in.c + k] : regs[in.args[k]];
            const dex::MethodId& mid = dex.method(in.index);
            const std::string& name = dex.string(mid.name_idx);
            const std::string& sig = dex.proto_signature(mid.proto_idx);
            Method* target = nullptr;
            if (kind == 3) {
              target = cache.methods[in.index];
              if (!target) {
                Class* c = resolve_type(cache, mid.class_idx);
                target = find_static(c, name, sig);
                if (!target) missing_method(c, name, sig, true);
                cache.methods[in.index] = target;
              }
              ensure_initialized(target->owner);
            } else {
              if (n == 0) throw_new("Ljava/lang/VerifyError;", "appel d'instance sans receveur");
              if (argbuf[0] == kNull) {
                Class* c = resolve_type(cache, mid.class_idx);
                throw_npe(std::string("Attempt to invoke ") + (kind == 4 ? "interface" : "virtual") + " method '" +
                          c->java_name() + "." + name + sig + "' on a null object reference");
              }
              if (kind == 2) {
                target = cache.methods[in.index];
                if (!target) {
                  Class* c = resolve_type(cache, mid.class_idx);
                  target = c->declared_method(name, sig);
                  if (!target || target->is_static()) missing_method(c, name, sig, false);
                  cache.methods[in.index] = target;
                }
              } else if (kind == 1) {
                Class* ref = resolve_type(cache, mid.class_idx);
                Class* base = ref->is_interface() ? ref : m->owner->super;
                if (!base) missing_method(ref, name, sig, false);
                target = resolve_virtual(base, name, sig);
                if (!target) missing_method(base, name, sig, false);
              } else {
                Object& recv = nonnull(argbuf[0], "receveur");
                target = resolve_virtual(recv.klass, name, sig);
                if (!target) missing_method(resolve_type(cache, mid.class_idx), name, sig, false);
              }
            }
            result = invoke(target, argbuf, n);
            ++i;
            break;
          }
          // Unary operations (12x).
          case 0x7b: regs[in.a] = uint32_t(-int64_t(int32_t(regs[in.b]))); ++i; break;
          case 0x7c: regs[in.a] = ~regs[in.b]; ++i; break;
          case 0x7d: set_wide(in.a, uint64_t(0) - wide(in.b)); ++i; break;
          case 0x7e: set_wide(in.a, ~wide(in.b)); ++i; break;
          case 0x7f: regs[in.a] = float_bits(-as_float(regs[in.b])); ++i; break;
          case 0x80: set_wide(in.a, double_bits(-as_double(wide(in.b)))); ++i; break;
          case 0x81: set_wide(in.a, uint64_t(int64_t(int32_t(regs[in.b])))); ++i; break;
          case 0x82: regs[in.a] = float_bits(float(int32_t(regs[in.b]))); ++i; break;
          case 0x83: set_wide(in.a, double_bits(double(int32_t(regs[in.b])))); ++i; break;
          case 0x84: regs[in.a] = uint32_t(wide(in.b)); ++i; break;
          case 0x85: regs[in.a] = float_bits(float(int64_t(wide(in.b)))); ++i; break;
          case 0x86: set_wide(in.a, double_bits(double(int64_t(wide(in.b))))); ++i; break;
          case 0x87: regs[in.a] = uint32_t(java_f2i<int32_t>(as_float(regs[in.b]))); ++i; break;
          case 0x88: set_wide(in.a, uint64_t(java_f2i<int64_t>(as_float(regs[in.b])))); ++i; break;
          case 0x89: set_wide(in.a, double_bits(double(as_float(regs[in.b])))); ++i; break;
          case 0x8a: regs[in.a] = uint32_t(java_f2i<int32_t>(as_double(wide(in.b)))); ++i; break;
          case 0x8b: set_wide(in.a, uint64_t(java_f2i<int64_t>(as_double(wide(in.b))))); ++i; break;
          case 0x8c: regs[in.a] = float_bits(float(as_double(wide(in.b)))); ++i; break;
          case 0x8d: regs[in.a] = uint32_t(int32_t(int8_t(regs[in.b]))); ++i; break;
          case 0x8e: regs[in.a] = regs[in.b] & 0xFFFF; ++i; break;
          case 0x8f: regs[in.a] = uint32_t(int32_t(int16_t(regs[in.b]))); ++i; break;
          default:
            if ((op >= 0x90 && op <= 0xaf) || (op >= 0xb0 && op <= 0xcf)) {
              const bool two_addr = op >= 0xb0;
              const int k = op - (two_addr ? 0xb0 : 0x90);
              const uint32_t dst = in.a;
              const uint32_t src1 = two_addr ? in.a : in.b;
              const uint32_t src2 = two_addr ? in.b : in.c;
              if (k <= 10) {
                regs[dst] = uint32_t(int_op(*this, k, int32_t(regs[src1]), int32_t(regs[src2])));
              } else if (k <= 21) {
                const int lk = k - 11;
                // Shift amounts are a single int register.
                int64_t y = lk >= 8 ? int64_t(int32_t(regs[src2])) : int64_t(wide(src2));
                set_wide(dst, uint64_t(long_op(*this, lk, int64_t(wide(src1)), y)));
              } else if (k <= 26) {
                regs[dst] = float_bits(fp_op<float>(k - 22, as_float(regs[src1]), as_float(regs[src2])));
              } else {
                set_wide(dst, double_bits(fp_op<double>(k - 27, as_double(wide(src1)), as_double(wide(src2)))));
              }
              ++i;
            } else if (op >= 0xd0 && op <= 0xe2) {
              const int idx = op >= 0xd8 ? op - 0xd8 : op - 0xd0;
              const int32_t x = int32_t(regs[in.b]);
              const int32_t lit = int32_t(in.lit);
              regs[in.a] = idx == 1 ? uint32_t(lit) - uint32_t(x) : uint32_t(int_op(*this, idx, x, lit));
              ++i;
            } else if (op == 0xfa || op == 0xfb) {
              // invoke-polymorphic: signature-polymorphic methods of VarHandle
              // and MethodHandle, typed by the call-site prototype.
              const bool range = op == 0xfb;
              const uint32_t n = in.argc;
              for (uint32_t k = 0; k < n; ++k) argbuf[k] = range ? regs[in.c + k] : regs[in.args[k]];
              const dex::MethodId& mid = dex.method(in.index);
              Class* declared = resolve_type(cache, mid.class_idx);
              if (n == 0 || argbuf[0] == kNull) {
                throw_npe("Attempt to invoke " + declared->java_name() + "." + dex.string(mid.name_idx) +
                          " on a null object reference");
              }
              result = invoke_polymorphic(declared, dex.string(mid.name_idx), dex.proto_signature(in.index2), argbuf, n);
              ++i;
            } else if (op == 0xfc || op == 0xfd) {
              // invoke-custom: the target of the call site, bound once by its
              // bootstrap method, invoked exactly.
              const bool range = op == 0xfd;
              const uint32_t n = in.argc;
              for (uint32_t k = 0; k < n; ++k) argbuf[k] = range ? regs[in.c + k] : regs[in.args[k]];
              result = invoke_custom(m, in.index, argbuf, n);
              ++i;
            } else if (op == 0xfe) {  // const-method-handle
              regs[in.a] = resolve_method_handle(m, in.index);
              ++i;
            } else if (op == 0xff) {  // const-method-type
              regs[in.a] = resolve_method_type(m, in.index);
              ++i;
            } else {
              throw_new("Ljava/lang/VerifyError;", "opcode inattendu " + hex(op));
            }
            break;
        }
      }
    } catch (const JavaThrow& thrown) {
      const uint32_t pc = t->frames[fi].pc;
      int32_t target = -1;
      for (const TryBlock& tb : code.tries) {
        if (pc < tb.start || pc >= tb.end) continue;
        for (const CatchClause& cl : tb.clauses) {
          if (cl.type_idx == dex::kNoIndex) {
            target = cl.target;
            break;
          }
          Class* c = nullptr;
          try {
            c = lookup_class(dex.type_descriptor(cl.type_idx));
          } catch (const JavaThrow&) {
            c = nullptr;
          }
          if (c && instance_of(thrown.exception, c)) {
            target = cl.target;
            break;
          }
        }
        break;
      }
      if (target < 0) throw;
      pending_exception = thrown.exception;
      i = size_t(target);
    }
  }
}

}  // namespace aa::rt
