// apkgen: builds reproducible test APKs from text sources (AndroidManifest.xml
// and smali), without the Android SDK. Also fabricates malformed archives for
// robustness tests.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "apk-inspector/zip_reader.h"
#include "axml_writer.h"
#include "common/bytes.h"
#include "common/file_io.h"
#include "common/text.h"
#include "dex_writer.h"
#include "smali.h"
#include "xml_text.h"
#include "zip_writer.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace aa;

namespace {

void usage() {
  std::puts(
      "apkgen — génération d'APK de test pour AnyAndroid\n"
      "\n"
      "  apkgen build --manifest <AndroidManifest.xml> --smali <dossier> [--smali <dossier>...]\n"
      "               [--stub-lib <lib/abi/libx.so>:<aarch64|arm|x86|x86_64>] [--asset <fichier>=<chemin>]\n"
      "               -o <sortie.apk>\n"
      "  apkgen dex --smali <dossier> [...] -o <classes.dex>\n"
      "  apkgen corrupt <truncated|bad-crc|traversal|duplicate|bomb|bad-manifest|bad-dex> <entrée.apk> -o <sortie.apk>\n"
      "  apkgen bundle -o <sortie.apks> <base.apk> [<split.apk>...]\n"
      "\n"
      "  --smali est facultatif pour un APK de configuration (split) sans code.\n");
}

std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<tool::ClassDef> assemble_dirs(const std::vector<std::string>& dirs) {
  std::vector<fs::path> files;
  for (const auto& d : dirs) {
    fs::path root = path_from_utf8(d);
    if (fs::is_regular_file(root)) {
      files.push_back(root);
      continue;
    }
    if (!fs::is_directory(root)) throw std::runtime_error("dossier smali introuvable : " + d);
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      if (e.is_regular_file() && e.path().extension() == ".smali") files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());  // deterministic output
  std::vector<tool::ClassDef> classes;
  for (const auto& f : files) classes.push_back(tool::assemble_smali(read_text_file(f), path_utf8(f.filename())));
  if (classes.empty()) throw std::runtime_error("aucun fichier .smali trouvé");
  return classes;
}

std::vector<uint8_t> stub_elf(const std::string& machine) {
  uint16_t m;
  int bits;
  if (machine == "aarch64") m = 183, bits = 64;
  else if (machine == "arm") m = 40, bits = 32;
  else if (machine == "x86_64") m = 62, bits = 64;
  else if (machine == "x86") m = 3, bits = 32;
  else throw std::runtime_error("architecture inconnue : " + machine);
  ByteWriter w;
  const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', uint8_t(bits == 64 ? 2 : 1), 1, 1, 0};
  w.bytes(ident);
  w.u16(3);  // ET_DYN
  w.u16(m);
  w.u32(1);
  w.zeros(bits == 64 ? 64 - 24 : 52 - 24);
  std::string marker = "AnyAndroid stub library: not executable";
  w.bytes(std::span(reinterpret_cast<const uint8_t*>(marker.data()), marker.size()));
  return w.take();
}

int cmd_build(const std::vector<std::string>& args, bool dex_only) {
  std::string manifest, output;
  std::vector<std::string> smali_dirs, stub_libs, assets;
  for (size_t i = 0; i < args.size(); ++i) {
    auto next = [&]() -> const std::string& {
      if (i + 1 >= args.size()) throw std::runtime_error("valeur manquante après " + args[i]);
      return args[++i];
    };
    if (args[i] == "--manifest") manifest = next();
    else if (args[i] == "--smali") smali_dirs.push_back(next());
    else if (args[i] == "--stub-lib") stub_libs.push_back(next());
    else if (args[i] == "--asset") assets.push_back(next());
    else if (args[i] == "-o") output = next();
    else throw std::runtime_error("option inconnue : " + args[i]);
  }
  if (output.empty() || (dex_only && smali_dirs.empty()) || (!dex_only && manifest.empty())) {
    usage();
    return 2;
  }
  std::vector<uint8_t> dex;
  if (!smali_dirs.empty()) dex = tool::write_dex(assemble_dirs(smali_dirs));
  if (dex_only) {
    write_file(path_from_utf8(output), dex);
    return 0;
  }
  tool::XmlNode root = tool::parse_xml_text(read_text_file(path_from_utf8(manifest)));
  tool::ZipWriter zip;
  zip.add("AndroidManifest.xml", tool::compile_axml(root));
  if (!dex.empty()) zip.add("classes.dex", dex);  // split APKs carry no code
  for (const auto& a : assets) {
    size_t eq = a.find('=');
    if (eq == std::string::npos) throw std::runtime_error("--asset attend <fichier>=<chemin dans l'APK>");
    zip.add(a.substr(eq + 1), read_file(path_from_utf8(a.substr(0, eq)), 64u << 20));
  }
  for (const auto& s : stub_libs) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("--stub-lib attend <chemin>:<architecture>");
    zip.add(s.substr(0, colon), stub_elf(s.substr(colon + 1)), false);
  }
  write_file(path_from_utf8(output), zip.finish());
  return 0;
}

int cmd_corrupt(const std::vector<std::string>& args) {
  if (args.size() != 4 || args[2] != "-o") {
    usage();
    return 2;
  }
  const std::string& kind = args[0];
  std::vector<uint8_t> input = read_file(path_from_utf8(args[1]), 256u << 20);
  fs::path out = path_from_utf8(args[3]);
  if (kind == "truncated") {
    input.resize(input.size() / 2);
    write_file(out, input);
    return 0;
  }
  auto zip = apk::ZipArchive::open(input);
  tool::ZipWriter w;
  for (const auto& e : zip->entries()) {
    std::vector<uint8_t> data = zip->read(e);
    tool::ZipWriter::Options o;
    if (e.name == "classes.dex" && kind == "bad-crc") o.corrupt_crc = true;
    if (e.name == "classes.dex" && kind == "bad-dex") data.resize(0x70);
    if (e.name == "AndroidManifest.xml" && kind == "bad-manifest") data = bytes_of("ceci n'est pas du XML binaire Android");
    w.add(e.name, data, o);
  }
  if (kind == "traversal") w.add("../../evil.txt", bytes_of("traversée de chemin"));
  else if (kind == "duplicate") w.add("classes.dex", bytes_of("doublon"));
  else if (kind == "bomb") {
    tool::ZipWriter::Options o;
    o.declared_size_override = 3ull << 30;
    std::vector<uint8_t> zeros(1 << 20, 0);
    w.add("assets/bomb.bin", zeros, o);
  } else if (kind != "bad-crc" && kind != "bad-dex" && kind != "bad-manifest") {
    throw std::runtime_error("type de corruption inconnu : " + kind);
  }
  write_file(out, w.finish());
  return 0;
}

// App bundle archive (.apks): a ZIP of APKs, as produced by bundletool/SAI.
int cmd_bundle(const std::vector<std::string>& args) {
  if (args.size() < 3 || args[0] != "-o") {
    usage();
    return 2;
  }
  tool::ZipWriter zip;
  for (size_t i = 2; i < args.size(); ++i) {
    fs::path p = path_from_utf8(args[i]);
    zip.add(path_utf8(p.filename()), read_file(p, 1u << 30), false);
  }
  write_file(path_from_utf8(args[1]), zip.finish());
  return 0;
}

int run(const std::vector<std::string>& argv) {
  if (argv.size() < 2) {
    usage();
    return 2;
  }
  std::vector<std::string> rest(argv.begin() + 2, argv.end());
  if (argv[1] == "build") return cmd_build(rest, false);
  if (argv[1] == "dex") return cmd_build(rest, true);
  if (argv[1] == "corrupt") return cmd_corrupt(rest);
  if (argv[1] == "bundle") return cmd_bundle(rest);
  usage();
  return 2;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  auto args = utf8_args(argc, argv);
#else
int main(int argc, char** argv) {
  std::vector<std::string> args(argv, argv + argc);
#endif
  try {
    return run(args);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "apkgen : erreur : %s\n", e.what());
    return 1;
  }
}
