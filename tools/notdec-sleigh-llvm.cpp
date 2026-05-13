#include "SleighBytes.h"
#include "notdec-bin2llvm/PcodeToLLVM.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <optional>
#include <string>
#include <system_error>

namespace {

struct CliOptions {
  notdec::bin2llvm::SleighBytesOptions SleighOptions;
  std::string OutputPath;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " <sla-file> <hex-bytes> -o <output.ll> [-a address] "
               "[-p root-sla-dir] [-s pspec-file]\n";
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc < 5) {
    return std::nullopt;
  }

  CliOptions options;
  options.SleighOptions.SlaFileName = argv[1];
  options.SleighOptions.HexBytes = argv[2];
  if (options.SleighOptions.HexBytes.size() % 2 != 0) {
    std::cerr << "hex-bytes must contain an even number of hex digits\n";
    return std::nullopt;
  }

  for (int argIndex = 3; argIndex < argc; ++argIndex) {
    std::string flag = argv[argIndex];
    if (argIndex + 1 >= argc) {
      std::cerr << "flag has no value: " << flag << '\n';
      return std::nullopt;
    }

    std::string value = argv[++argIndex];
    if (flag == "-o") {
      options.OutputPath = std::move(value);
    } else if (flag == "-a") {
      try {
        options.SleighOptions.Address = std::stoull(value, nullptr, 0);
      } catch (const std::exception &) {
        std::cerr << "invalid address: " << value << '\n';
        return std::nullopt;
      }
    } else if (flag == "-p") {
      options.SleighOptions.RootSlaDir = std::move(value);
    } else if (flag == "-s") {
      options.SleighOptions.PspecFileName = std::move(value);
    } else {
      std::cerr << "unknown flag: " << flag << '\n';
      return std::nullopt;
    }
  }

  if (options.OutputPath.empty()) {
    std::cerr << "missing -o <output.ll>\n";
    return std::nullopt;
  }
  return options;
}

int writeModule(const llvm::Module &module, const std::string &outputPath) {
  std::error_code errorCode;
  llvm::raw_fd_ostream output(outputPath, errorCode);
  if (errorCode) {
    std::cerr << "failed to open output file: " << outputPath << ": "
              << errorCode.message() << '\n';
    return 1;
  }

  module.print(output, nullptr);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  auto options = parseArgs(argc, argv);
  if (!options) {
    printUsage(argv[0]);
    return 1;
  }

  auto program =
      notdec::bin2llvm::collectSleighPcode(options->SleighOptions, std::cerr);
  if (program.Ops.empty()) {
    return 1;
  }

  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeLoweringConfig config;
  std::string errorMessage;
  auto module = notdec::bin2llvm::buildPcodeModule(context, program, config,
                                                   errorMessage);
  if (!module) {
    std::cerr << "failed to lower p-code: " << errorMessage << '\n';
    return 1;
  }

  if (llvm::verifyModule(*module, &llvm::errs())) {
    std::cerr << "module verification failed\n";
    return 1;
  }

  return writeModule(*module, options->OutputPath);
}
