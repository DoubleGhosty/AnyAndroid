// libcore.math.NativeBN (libcore_math_NativeBN.cpp, android-17.0.0_r1): the
// BoringSSL big numbers that java.math.BigInteger uses for its large
// multiplications, divisions and modular exponentiations. A number is a sign
// and a magnitude of 32-bit little-endian limbs; the results are BoringSSL's
// (BN_mul, BN_div truncating towards zero, BN_mod_exp in [0, m)), its
// failures the ArithmeticException messages of throwException.
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/internal.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using Limbs = std::vector<uint32_t>;

struct BigNum {
  bool neg = false;  // never set for zero (BN_set_negative)
  Limbs mag;         // no leading zero limb
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<BigNum>& numbers() {
  static auto* t = new HandleTable<BigNum>(0x112B);  // never destroyed
  return *t;
}

std::shared_ptr<BigNum> number(Runtime& rt, int64_t handle) {
  auto n = numbers().get(handle);
  if (!n) rt.throw_new("Ljava/lang/IllegalArgumentException;", "BIGNUM invalide");
  return n;
}

void trim(Limbs& v) {
  while (!v.empty() && v.back() == 0) v.pop_back();
}

int ucmp(const Limbs& a, const Limbs& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

Limbs umul(const Limbs& a, const Limbs& b) {
  if (a.empty() || b.empty()) return {};
  Limbs r(a.size() + b.size(), 0);
  for (size_t i = 0; i < a.size(); ++i) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.size(); ++j) {
      uint64_t t = uint64_t(a[i]) * b[j] + r[i + j] + carry;
      r[i + j] = uint32_t(t);
      carry = t >> 32;
    }
    r[i + b.size()] = uint32_t(carry);
  }
  trim(r);
  return r;
}

// Unsigned division (Knuth, The Art of Computer Programming, 4.3.1, algorithm D).
void udivmod(const Limbs& u, const Limbs& v, Limbs* q, Limbs* r) {
  if (ucmp(u, v) < 0) {
    if (q) q->clear();
    if (r) *r = u;
    return;
  }
  if (v.size() == 1) {
    Limbs quot(u.size(), 0);
    uint64_t rem = 0;
    for (size_t i = u.size(); i-- > 0;) {
      uint64_t cur = (rem << 32) | u[i];
      quot[i] = uint32_t(cur / v[0]);
      rem = cur % v[0];
    }
    trim(quot);
    if (q) *q = std::move(quot);
    if (r) {
      r->assign(1, uint32_t(rem));
      trim(*r);
    }
    return;
  }
  // Normalize so that the divisor's top limb has its high bit set.
  const size_t n = v.size(), m = u.size() - n;
  int shift = 0;
  for (uint32_t top = v.back(); !(top & 0x80000000u); top <<= 1) ++shift;
  Limbs vn(n), un(u.size() + 1);
  for (size_t i = n; i-- > 0;) {
    vn[i] = (v[i] << shift) | (shift && i ? v[i - 1] >> (32 - shift) : 0);
  }
  un[u.size()] = shift ? u.back() >> (32 - shift) : 0;
  for (size_t i = u.size(); i-- > 0;) {
    un[i] = (u[i] << shift) | (shift && i ? u[i - 1] >> (32 - shift) : 0);
  }
  Limbs quot(m + 1, 0);
  for (size_t j = m + 1; j-- > 0;) {
    // Estimate the quotient digit from the top two limbs.
    uint64_t num = (uint64_t(un[j + n]) << 32) | un[j + n - 1];
    uint64_t qhat = num / vn[n - 1], rhat = num % vn[n - 1];
    while (qhat >> 32 || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
      --qhat;
      rhat += vn[n - 1];
      if (rhat >> 32) break;
    }
    // Multiply and subtract.
    int64_t borrow = 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
      uint64_t p = qhat * vn[i] + carry;
      carry = p >> 32;
      int64_t t = int64_t(un[i + j]) - borrow - int64_t(uint32_t(p));
      un[i + j] = uint32_t(t);
      borrow = t < 0 ? 1 : 0;
    }
    int64_t t = int64_t(un[j + n]) - borrow - int64_t(carry);
    un[j + n] = uint32_t(t);
    if (t < 0) {  // estimate one too large: add the divisor back
      --qhat;
      uint64_t c = 0;
      for (size_t i = 0; i < n; ++i) {
        uint64_t s = uint64_t(un[i + j]) + vn[i] + c;
        un[i + j] = uint32_t(s);
        c = s >> 32;
      }
      un[j + n] += uint32_t(c);
    }
    quot[j] = uint32_t(qhat);
  }
  trim(quot);
  if (q) *q = std::move(quot);
  if (r) {
    Limbs rem(n);
    for (size_t i = 0; i < n; ++i) rem[i] = (un[i] >> shift) | (shift ? un[i + 1] << (32 - shift) : 0);
    trim(rem);
    *r = std::move(rem);
  }
}

[[noreturn]] void division_by_zero(Runtime& rt) {
  rt.throw_new("Ljava/lang/ArithmeticException;", "BigInteger division by zero");
}

void set(BigNum& n, Limbs mag, bool neg) {
  trim(mag);
  n.neg = neg && !mag.empty();
  n.mag = std::move(mag);
}

// BN_mod_exp: a^p mod m in [0, m), by left-to-right square-and-multiply.
Limbs mod_exp(Runtime& rt, const BigNum& a, const BigNum& p, const BigNum& m) {
  if (m.mag.empty()) division_by_zero(rt);
  if (m.neg || p.neg) rt.throw_new("Ljava/lang/ArithmeticException;", "Operation failed");
  Limbs base;
  udivmod(a.mag, m.mag, nullptr, &base);
  if (a.neg && !base.empty()) {  // BN_nnmod: the non-negative residue
    Limbs diff = m.mag;
    int64_t borrow = 0;
    for (size_t i = 0; i < diff.size(); ++i) {
      int64_t t = int64_t(diff[i]) - borrow - (i < base.size() ? int64_t(base[i]) : 0);
      diff[i] = uint32_t(t);
      borrow = t < 0 ? 1 : 0;
    }
    trim(diff);
    base = std::move(diff);
  }
  Limbs result;
  udivmod(Limbs{1}, m.mag, nullptr, &result);  // 1 mod m: 0 when m is 1
  for (size_t i = p.mag.size(); i-- > 0;) {
    for (int bit = 31; bit >= 0; --bit) {
      udivmod(umul(result, result), m.mag, nullptr, &result);
      if ((p.mag[i] >> bit) & 1) udivmod(umul(result, base), m.mag, nullptr, &result);
    }
  }
  return result;
}

}  // namespace

void register_libcore_math(rt::NativeRegistry& r) {
  const char* bn = "Llibcore/math/NativeBN;";
  r.bind(bn, "BN_new", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(numbers().add(std::make_shared<BigNum>()));
  });
  r.bind(bn, "BN_free", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    if (a.i64(0) != 0) numbers().remove(a.i64(0));  // nothing on a zero argument
    return 0;
  });
  // BN_le2bn of the first `len` ints read as little-endian bytes, then BN_set_negative.
  r.bind(bn, "litEndInts2bn", "([IIZJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe({});
    rt::ArrayObject& ints = rt.array(a.h(0), "int[]");
    int32_t len = a.i32(1);
    if (len < 0 || uint32_t(len) > ints.length) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;",
                   "length=" + std::to_string(ints.length) + "; index=" + std::to_string(len));
    }
    Limbs mag(static_cast<size_t>(len));
    if (len) std::memcpy(mag.data(), ints.data.data(), size_t(len) * 4);
    set(*number(rt, a.i64(3)), std::move(mag), a.z(2));
    return 0;
  });
  // BN_bn2le_padded: the magnitude as little-endian ints, BN_num_bytes rounded up.
  r.bind(bn, "bn2litEndInts", "(J)[I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = number(rt, a.i64(0));
    Handle result = rt.new_array("[I", int32_t(n->mag.size()));
    if (!n->mag.empty()) std::memcpy(rt.array(result, "int[]").data.data(), n->mag.data(), n->mag.size() * 4);
    return result;
  });
  r.bind(bn, "BN_mul", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto x = number(rt, a.i64(2)), y = number(rt, a.i64(4));
    Limbs product = umul(x->mag, y->mag);
    set(*number(rt, a.i64(0)), std::move(product), x->neg != y->neg);
    return 0;
  });
  // BN_div: q = num / divisor rounded towards zero, rem = num - q * divisor.
  r.bind(bn, "BN_div", "(JJJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto num = number(rt, a.i64(4)), divisor = number(rt, a.i64(6));
    if (divisor->mag.empty()) division_by_zero(rt);
    Limbs q, rem;
    udivmod(num->mag, divisor->mag, &q, &rem);
    bool num_neg = num->neg, q_neg = num->neg != divisor->neg;
    set(*number(rt, a.i64(0)), std::move(q), q_neg);
    set(*number(rt, a.i64(2)), std::move(rem), num_neg);
    return 0;
  });
  r.bind(bn, "BN_mod_exp", "(JJJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto x = number(rt, a.i64(2)), p = number(rt, a.i64(4)), m = number(rt, a.i64(6));
    Limbs result = mod_exp(rt, *x, *p, *m);
    set(*number(rt, a.i64(0)), std::move(result), false);
    return 0;
  });
}

}  // namespace aa::bridge
