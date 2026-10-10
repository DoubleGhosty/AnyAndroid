#include <cstdio>
#include <cstring>
#include <exception>

#include "common/log.h"
#include "dex-engine/runtime.h"
#include "test.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace aa::test {
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
}  // namespace aa::test

// Usage: aa_unit_tests [filtre]  — runs the tests whose name contains the filter.
int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  aa::log::set_level(aa::log::Level::Off);
  const char* filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0;
  for (const auto& t : aa::test::registry()) {
    if (*filter && !std::strstr(t.name, filter)) continue;
    try {
      t.fn();
      ++passed;
    } catch (const std::exception& e) {
      ++failed;
      std::printf("ÉCHEC %s\n  %s\n", t.name, e.what());
    } catch (const aa::rt::JavaThrow&) {
      ++failed;
      std::printf("ÉCHEC %s\n  exception Java non rattrapée\n", t.name);
    } catch (...) {
      ++failed;
      std::printf("ÉCHEC %s\n  exception inconnue\n", t.name);
    }
  }
  std::printf("%d test(s) réussi(s), %d échec(s)\n", passed, failed);
  return failed ? 1 : 0;
}
