#include "notdec-bin2llvm/HeritagePcode.h"
#include "notdec-bin2llvm/HeritageToLLVM.h"

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
  std::string InputPath;
  std::string OutputPath;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " <heritage-module.json> -o <output.ll>\n";
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc != 4) {
    return std::nullopt;
  }
  CliOptions options;
  options.InputPath = argv[1];
  std::string flag = argv[2];
  if (flag != "-o") {
    std::cerr << "unknown flag: " << flag << '\n';
    return std::nullopt;
  }
  options.OutputPath = argv[3];
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

  notdec::bin2llvm::HeritageModule heritageModule;
  std::string errorMessage;
  if (!notdec::bin2llvm::loadHeritageModuleFromJson(
          options->InputPath, heritageModule, errorMessage)) {
    std::cerr << errorMessage << '\n';
    return 1;
  }

  llvm::LLVMContext context;
  notdec::bin2llvm::HeritageLoweringConfig config;
  config.ModuleName = "notdec.bin2llvm.heritage.module";
  auto module = notdec::bin2llvm::buildHeritageDeclarationModule(
      context, heritageModule, config, errorMessage);
  if (!module) {
    std::cerr << "failed to build heritage module declarations: "
              << errorMessage << '\n';
    return 1;
  }

  if (llvm::verifyModule(*module, &llvm::errs())) {
    std::cerr << "module verification failed\n";
    return 1;
  }

  return writeModule(*module, options->OutputPath);
}
