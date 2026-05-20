#include "notdec-bin2llvm/NativeAnalysis.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

struct CliOptions {
  std::string ElfPath;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " <elf-file>\n";
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc != 2) {
    return std::nullopt;
  }

  CliOptions options;
  options.ElfPath = argv[1];
  return options;
}

} // namespace

int main(int argc, char **argv) {
  try {
    std::optional<CliOptions> options = parseArgs(argc, argv);
    if (!options) {
      printUsage(argv[0]);
      return 1;
    }

    std::unique_ptr<LIEF::ELF::Binary> binary =
        LIEF::ELF::Parser::parse(options->ElfPath);
    if (!binary) {
      std::cerr << "failed to parse ELF: " << options->ElfPath << '\n';
      return 1;
    }

    notdec::bin2llvm::NativeProgramState state(*binary);
    notdec::bin2llvm::NativeAnalysisManager manager;
    manager.addAnalyzer(notdec::bin2llvm::createElfLoadAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createRelocationPltAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createElfEntryAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createElfSymbolAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createReportAnalyzer(std::cout));
    manager.run(state);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
