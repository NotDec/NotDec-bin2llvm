#pragma once

#include "notdec-bin2llvm/Pcode.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace notdec::bin2llvm {

struct SleighBytesOptions {
  std::string SlaFileName;
  std::string HexBytes;
  std::optional<uint64_t> Address;
  std::optional<std::string> RootSlaDir;
  std::optional<std::string> PspecFileName;
};

std::optional<SleighBytesOptions>
parseSleighBytesOptions(int argc, char **argv, std::ostream &errorStream);

PcodeProgram collectSleighPcode(const SleighBytesOptions &options,
                                std::ostream &errorStream);

void printPcodeProgram(const PcodeProgram &program, std::ostream &os, bool withAddress = false);

} // namespace notdec::bin2llvm
