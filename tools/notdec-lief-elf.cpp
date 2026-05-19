#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <iostream>
#include <memory>
#include <string>

namespace {

int printElfSummary(const LIEF::ELF::Binary &binary,
                    const std::string &inputPath) {
  const auto &header = binary.header();

  std::cout << "lief-elf parse\n";
  std::cout << "  path: " << inputPath << '\n';
  std::cout << "  file type: " << LIEF::ELF::to_string(header.file_type())
            << '\n';
  std::cout << "  arch: " << static_cast<int>(header.machine_type()) << '\n';
  std::cout << "  entrypoint: 0x" << std::hex << binary.entrypoint()
            << std::dec << '\n';
  std::cout << "  sections: " << binary.sections().size() << '\n';
  std::cout << "  segments: " << binary.segments().size() << '\n';
  std::cout << "  symbols: " << binary.symbols().size() << '\n';
  std::cout << "  dynamic entries: " << binary.dynamic_entries().size()
            << '\n';

  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <elf-file>\n";
    return 1;
  }

  const std::string inputPath = argv[1];
  std::unique_ptr<LIEF::ELF::Binary> binary = LIEF::ELF::Parser::parse(inputPath);
  if (!binary) {
    std::cerr << "failed to parse ELF: " << inputPath << '\n';
    return 1;
  }

  return printElfSummary(*binary, inputPath);
}
