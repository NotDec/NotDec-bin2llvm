#include "SleighBytes.h"

#include <iostream>

namespace {

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " <sla-file> <hex-bytes> [-a address] [-p root-sla-dir] "
               "[-s pspec-file]\n";
}

} // namespace

int main(int argc, char **argv) {
  auto options =
      notdec::bin2llvm::parseSleighBytesOptions(argc, argv, std::cerr);
  if (!options) {
    printUsage(argv[0]);
    return 1;
  }

  auto program = notdec::bin2llvm::collectSleighPcode(*options, std::cerr);
  if (program.Ops.empty()) {
    return 1;
  }

  notdec::bin2llvm::printPcodeProgram(program, std::cout);
  return 0;
}
