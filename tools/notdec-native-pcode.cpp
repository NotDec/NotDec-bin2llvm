#include "SleighBytes.h"
#include "notdec-bin2llvm/LiefElfLoadImage.h"
#include "notdec-bin2llvm/SleighLift.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

struct CliOptions {
  std::string ElfPath;
  notdec::bin2llvm::SleighSpecOptions SpecOptions;
  uint64_t Address = 0;
  uint64_t Length = 0;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " <elf-file> <sla-file> -a <address> -l <length> "
               "[-p root-sla-dir] [-s pspec-file]\n";
}

bool parseUint64(const std::string &text, uint64_t &value) {
  try {
    size_t parsed = 0;
    value = std::stoull(text, &parsed, 0);
    return parsed == text.size();
  } catch (const std::exception &) {
    return false;
  }
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc < 6) {
    return std::nullopt;
  }

  CliOptions options;
  options.ElfPath = argv[1];
  options.SpecOptions.SlaFileName = argv[2];
  bool hasAddress = false;
  bool hasLength = false;

  for (int argIndex = 3; argIndex < argc; ++argIndex) {
    std::string flag = argv[argIndex];
    if (argIndex + 1 >= argc) {
      std::cerr << "flag has no value: " << flag << '\n';
      return std::nullopt;
    }

    std::string value = argv[++argIndex];
    if (flag == "-a") {
      if (!parseUint64(value, options.Address)) {
        std::cerr << "invalid address: " << value << '\n';
        return std::nullopt;
      }
      hasAddress = true;
    } else if (flag == "-l") {
      if (!parseUint64(value, options.Length) || options.Length == 0) {
        std::cerr << "invalid length: " << value << '\n';
        return std::nullopt;
      }
      hasLength = true;
    } else if (flag == "-p") {
      options.SpecOptions.RootSlaDir = std::move(value);
    } else if (flag == "-s") {
      options.SpecOptions.PspecFileName = std::move(value);
    } else {
      std::cerr << "unknown flag: " << flag << '\n';
      return std::nullopt;
    }
  }

  if (!hasAddress) {
    std::cerr << "missing -a <address>\n";
    return std::nullopt;
  }
  if (!hasLength) {
    std::cerr << "missing -l <length>\n";
    return std::nullopt;
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  try {
    auto options = parseArgs(argc, argv);
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

    notdec::bin2llvm::LiefElfLoadImage loadImage(*binary, std::cerr);
    if (!loadImage.hasExecutableBytes()) {
      return 1;
    }

    auto program = notdec::bin2llvm::collectSleighPcode(
        loadImage, options->SpecOptions, options->Address, options->Length,
        std::cerr);
    if (program.Ops.empty()) {
      return 1;
    }

    notdec::bin2llvm::printPcodeProgram(program, std::cout);
    return 0;
  } catch (const ghidra::LowlevelError &error) {
    std::cerr << "libsla error: " << error.explain << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
