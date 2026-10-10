#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace aa::apk {

// Static description of an ELF shared object found in lib/<abi>/. The file is
// only parsed, never mapped or executed.
struct ElfInfo {
  bool valid = false;
  std::string error;
  int bits = 0;              // 32 or 64
  std::string machine;       // "aarch64", "arm", "x86", "x86_64", ...
  std::string type;          // "shared object", "executable", ...
};

ElfInfo inspect_elf(std::span<const uint8_t> data);

// Machine expected for an APK ABI directory name ("arm64-v8a" -> "aarch64").
std::string abi_machine(const std::string& abi);

}  // namespace aa::apk
