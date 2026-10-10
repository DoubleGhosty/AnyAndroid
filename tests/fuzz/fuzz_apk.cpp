// Mutation fuzzer for the static analysis and the interpreter.
//
//   aa_fuzz <fichier.apk> <itérations> <graine> [classe principale à exécuter]
//
// Each iteration mutates either the whole archive, the manifest or the DEX
// file, then runs the parsers (and, if a class is given, its main() under a
// tight instruction budget). Java exceptions and FormatError are expected;
// anything else (crash, std::bad_alloc, hang) is a bug.

#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "apk-inspector/apk.h"
#include "apk-inspector/axml.h"
#include "apk-inspector/manifest.h"
#include "common/file_io.h"
#include "common/log.h"
#include "dex-engine/code.h"
#include "dex-engine/runtime.h"

using namespace aa;

namespace {

void mutate(std::vector<uint8_t>& d, std::mt19937& rng) {
  if (d.empty()) return;
  std::uniform_int_distribution<size_t> pos(0, d.size() - 1);
  switch (rng() % 4) {
    case 0: {  // bit flips
      int n = 1 + int(rng() % 8);
      for (int i = 0; i < n; ++i) d[pos(rng)] ^= uint8_t(1u << (rng() % 8));
      break;
    }
    case 1: {  // interesting 32-bit values
      static const uint32_t values[] = {0, 1, 0x7f, 0x80, 0xff, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff};
      size_t p = pos(rng) & ~size_t(3);
      if (p + 4 <= d.size()) {
        uint32_t v = values[rng() % (sizeof values / sizeof values[0])];
        std::memcpy(&d[p], &v, 4);
      }
      break;
    }
    case 2:  // truncation
      d.resize(pos(rng));
      break;
    default: {  // random bytes
      int n = 1 + int(rng() % 16);
      for (int i = 0; i < n; ++i) d[pos(rng)] = uint8_t(rng());
    }
  }
}

void exercise_dex(const std::vector<uint8_t>& bytes) {
  auto dex = dex::DexFile::open(bytes, "fuzz.dex");
  for (uint32_t c = 0; c < dex->num_class_defs(); ++c) {
    const auto& def = dex->class_def(c);
    try {
      dex->encoded_array(def.static_values_off);
    } catch (const FormatError&) {
    }
    auto cd = dex->class_data(def);
    for (const auto* list : {&cd.direct_methods, &cd.virtual_methods}) {
      for (const auto& m : *list) {
        if (!m.code_off) continue;
        try {
          rt::prepare_code(*dex, dex->code_item(m.code_off));
        } catch (const FormatError&) {
        }
      }
    }
  }
}

void execute(const std::vector<uint8_t>& bytes, const std::string& main_class) {
  auto dex = dex::DexFile::open(bytes, "fuzz.dex");
  rt::RuntimeOptions opts;
  opts.max_instructions = 200000;
  opts.max_depth = 64;
  opts.max_objects = 100000;
  opts.max_array_length = 1 << 16;
  rt::Runtime runtime(opts);
  runtime.add_dex(dex.get());
  rt::Runtime::Scope scope(runtime, "fuzz");
  try {
    rt::Class* c = runtime.find_class(main_class);
    runtime.ensure_initialized(c);
    if (rt::Method* m = runtime.find_static(c, "main", "([Ljava/lang/String;)V")) {
      uint32_t args[1] = {runtime.new_array("[Ljava/lang/String;", 0)};
      runtime.invoke(m, args, 1);
    }
  } catch (const rt::JavaThrow&) {
  } catch (const rt::ExitRequest&) {
  }
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
  std::vector<std::string> argv = utf8_args(argc, wargv);
  if (argc < 4) {
    std::fprintf(stderr, "usage : aa_fuzz <apk> <itérations> <graine> [classe]\n");
    return 2;
  }
  log::set_level(log::Level::Off);
  auto original = read_file(path_from_utf8(argv[1]), 64u << 20);
  long iterations = std::stol(argv[2]);
  std::mt19937 rng(uint32_t(std::stoul(argv[3])));
  std::string main_class = argc > 4 ? argv[4] : "";

  auto base = apk::load_apk_bytes(original, "fuzz.apk");
  std::vector<uint8_t> manifest = base->zip->read("AndroidManifest.xml");
  std::vector<uint8_t> dex = base->zip->read("classes.dex");

  long expected_errors = 0;
  for (long i = 0; i < iterations; ++i) {
    try {
      switch (i % 3) {
        case 0: {
          auto d = original;
          mutate(d, rng);
          auto apk = apk::load_apk_bytes(std::move(d), "fuzz.apk");
          apk::analyze_code(*apk);
          break;
        }
        case 1: {
          auto m = manifest;
          mutate(m, rng);
          apk::parse_manifest(apk::parse_axml(m));
          break;
        }
        default: {
          auto d = dex;
          mutate(d, rng);
          exercise_dex(d);
          if (!main_class.empty()) execute(d, main_class);
        }
      }
    } catch (const FormatError&) {
      ++expected_errors;
    } catch (const rt::JavaThrow&) {
      ++expected_errors;
    } catch (const std::runtime_error& e) {
      ++expected_errors;  // e.g. heap limit reached
    }
  }
  std::printf("%ld itérations, %ld entrées rejetées proprement, aucun plantage\n", iterations, expected_errors);
  return 0;
}
