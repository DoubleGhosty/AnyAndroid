#include "apk-inspector/elf.h"

#include "common/bytes.h"

namespace aa::apk {

ElfInfo inspect_elf(std::span<const uint8_t> data) {
  ElfInfo info;
  if (data.size() < 20 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
    info.error = "signature ELF absente";
    return info;
  }
  if (data[5] != 1) {
    info.error = "ELF big-endian non pris en charge";
    return info;
  }
  info.bits = data[4] == 2 ? 64 : (data[4] == 1 ? 32 : 0);
  if (info.bits == 0) {
    info.error = "classe ELF inconnue";
    return info;
  }
  uint16_t type = load_u16(data.data() + 16);
  uint16_t machine = load_u16(data.data() + 18);
  switch (type) {
    case 2: info.type = "executable"; break;
    case 3: info.type = "shared object"; break;
    default: info.type = "type " + std::to_string(type);
  }
  switch (machine) {
    case 3: info.machine = "x86"; break;
    case 40: info.machine = "arm"; break;
    case 62: info.machine = "x86_64"; break;
    case 183: info.machine = "aarch64"; break;
    case 243: info.machine = "riscv"; break;
    default: info.machine = "machine " + std::to_string(machine);
  }
  info.valid = true;
  return info;
}

std::string abi_machine(const std::string& abi) {
  if (abi == "arm64-v8a") return "aarch64";
  if (abi == "armeabi-v7a" || abi == "armeabi") return "arm";
  if (abi == "x86") return "x86";
  if (abi == "x86_64") return "x86_64";
  if (abi == "riscv64") return "riscv";
  return "";
}

}  // namespace aa::apk
