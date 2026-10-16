#pragma once

// Minimal self-contained test harness (no third-party dependency).

#include <cstdint>
#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace aa::test {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

class Failure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// UTF-16 text as an ASCII literal: code units outside printable ASCII are
// written \uXXXX, so failures stay readable whatever the console encoding.
inline std::string escape_utf16(std::u16string_view s) {
  std::string out = "u\"";
  for (char16_t c : s) {
    if (c >= 0x20 && c < 0x7f && c != u'"' && c != u'\\') {
      out += char(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
      out += buf;
    }
  }
  out += '"';
  return out;
}

template <typename T>
constexpr bool kIsUtf16Text =
    std::is_same_v<T, std::u16string> || std::is_same_v<T, std::u16string_view> ||
    (std::is_pointer_v<std::decay_t<T>> &&
     std::is_same_v<std::remove_cv_t<std::remove_pointer_t<std::decay_t<T>>>, char16_t>);

template <typename T>
struct IsVector : std::false_type {};
template <typename E, typename A>
struct IsVector<std::vector<E, A>> : std::true_type {};

// What check_eq prints for a value: UTF-16 text escaped, enumerations and
// byte-sized integers (uint8_t would print as a raw char) as numbers,
// vectors as [a, b...], anything else unchanged.
template <typename T>
decltype(auto) printable(const T& v) {
  if constexpr (IsVector<T>::value) {
    std::ostringstream os;
    os << '[';
    for (size_t i = 0; i < v.size(); ++i) os << (i ? ", " : "") << printable(v[i]);
    os << ']';
    return os.str();
  } else if constexpr (kIsUtf16Text<T>) {
    if constexpr (std::is_pointer_v<std::decay_t<T>>) {
      const char16_t* p = v;
      return p ? escape_utf16(p) : std::string("nullptr");
    } else {
      return escape_utf16(v);
    }
  } else if constexpr (std::is_same_v<T, char16_t>) {
    return escape_utf16(std::u16string(1, v));
  } else if constexpr (std::is_same_v<T, char32_t>) {
    return uint32_t(v);  // ostream's char32_t overload is deleted
  } else if constexpr (std::is_enum_v<T>) {
    return +static_cast<std::underlying_type_t<T>>(v);  // unary + promotes bytes to int
  } else if constexpr (std::is_integral_v<T> && sizeof(T) == 1 && !std::is_same_v<T, char> &&
                       !std::is_same_v<T, bool>) {
    return int(v);
  } else {
    return (v);  // const T&
  }
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (!(a == b)) {
    std::ostringstream os;
    os << file << ":" << line << " : " << ea << " == " << eb << " a échoué\n    gauche : " << printable(a)
       << "\n    droite : " << printable(b);
    throw Failure(os.str());
  }
}

}  // namespace aa::test

#define AA_CAT2(a, b) a##b
#define AA_CAT(a, b) AA_CAT2(a, b)
#define TEST(name)                                                                     \
  static void name();                                                                  \
  static ::aa::test::Registrar AA_CAT(registrar_, name)(#name, name);                  \
  static void name()

#define CHECK(cond)                                                                                   \
  do {                                                                                                \
    if (!(cond)) throw ::aa::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                           " : CHECK(" #cond ") a échoué");                           \
  } while (0)

#define CHECK_EQ(a, b) ::aa::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_THROWS(expr, Type)                                                                      \
  do {                                                                                                \
    bool thrown_ = false;                                                                             \
    try {                                                                                             \
      expr;                                                                                           \
    } catch (const Type&) {                                                                           \
      thrown_ = true;                                                                                 \
    }                                                                                                 \
    if (!thrown_) throw ::aa::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                            " : " #expr " n'a pas levé " #Type);                      \
  } while (0)
