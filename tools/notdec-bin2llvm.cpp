#include "notdec-bin2llvm/ModuleBuilder.h"

#include <iostream>
#include <string>
#include <system_error>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace {

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

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <output.ll>\n";
    return 1;
  }

  llvm::LLVMContext context;
  notdec::bin2llvm::BuildConfig config;
  auto module = notdec::bin2llvm::buildDemoModule(context, config);

  if (llvm::verifyModule(*module, &llvm::errs())) {
    std::cerr << "module verification failed\n";
    return 1;
  }

  return writeModule(*module, argv[1]);
}
