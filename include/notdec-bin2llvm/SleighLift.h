#pragma once

#include "notdec-bin2llvm/Pcode.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

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

// SleighInstructionSummary is the smallest decode result used by native
// recursive analysis.  It intentionally keeps operands as display text for now:
// CFG and xref recovery need stricter target handling before operands become
// shared state.
struct SleighInstructionSummary {
  uint64_t Address = 0;
  uint64_t Size = 0;
  std::string Mnemonic;
  std::string Body;
};

std::optional<std::filesystem::path>
findSleighSpecPath(const std::string &fileName,
                   const std::optional<std::string> &rootDir);

PcodeProgram collectSleighPcode(ghidra::LoadImage &loadImage,
                                const SleighSpecOptions &options,
                                uint64_t address, uint64_t length,
                                std::ostream &errorStream);

std::vector<SleighInstructionSummary>
collectSleighInstructionSummaries(ghidra::LoadImage &loadImage,
                                  const SleighSpecOptions &options,
                                  uint64_t address, uint64_t maxInstructions,
                                  uint64_t maxBytes,
                                  std::ostream &errorStream);

} // namespace notdec::bin2llvm
