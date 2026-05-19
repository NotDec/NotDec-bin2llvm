#pragma once

#include "notdec-bin2llvm/Pcode.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace ghidra {
class LoadImage;
} // namespace ghidra

namespace notdec::bin2llvm {

// SleighSpecOptions names the architecture files needed by libsla.  Native
// binary loading and instruction selection stay outside this struct, so the
// same Sleigh setup can be reused by byte-level tests and real ELF lifting.
struct SleighSpecOptions {
  std::string SlaFileName;
  std::optional<std::string> RootSlaDir;
  std::optional<std::string> PspecFileName;
};

std::optional<std::filesystem::path>
findSleighSpecPath(const std::string &fileName,
                   const std::optional<std::string> &rootDir);

PcodeProgram collectSleighPcode(ghidra::LoadImage &loadImage,
                                const SleighSpecOptions &options,
                                uint64_t address, uint64_t length,
                                std::ostream &errorStream);

} // namespace notdec::bin2llvm
