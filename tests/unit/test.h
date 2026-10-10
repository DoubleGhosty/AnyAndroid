#pragma once

// Minimal self-contained test harness (no third-party dependency).

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
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

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (!(a == b)) {
    std::ostringstream os;
    os << file << ":" << line << " : " << ea << " == " << eb << " a échoué\n    gauche : " << a << "\n    droite : " << b;
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
