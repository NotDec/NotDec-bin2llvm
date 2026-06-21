#include "notdec-bin2llvm/NativeAnalysis.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

bool expectEqual(const std::string &actual, const std::string &expected,
                 const char *label) {
  if (actual == expected) {
    return true;
  }
  std::cerr << label << ": expected " << expected << ", got " << actual
            << '\n';
  return false;
}

bool expectTrue(bool condition, const char *label) {
  if (condition) {
    return true;
  }
  std::cerr << label << '\n';
  return false;
}

std::unique_ptr<LIEF::ELF::Binary> parseSelfBinary(const char *argv0) {
  try {
    return LIEF::ELF::Parser::parse(argv0);
  } catch (const std::exception &error) {
    std::cerr << "failed to parse test binary: " << error.what() << '\n';
    return nullptr;
  }
}

std::optional<uint64_t>
firstExecutableAddress(const notdec::bin2llvm::NativeProgramState &state) {
  for (const notdec::bin2llvm::NativeMemoryRange &range :
       state.memoryRanges()) {
    if (range.Executable && range.Size >= 0x100) {
      return range.Start;
    }
  }
  return std::nullopt;
}

notdec::bin2llvm::NativeInstruction
makeInstruction(uint64_t address, uint64_t size,
                notdec::bin2llvm::NativeInstructionFlowKind flowKind) {
  notdec::bin2llvm::NativeInstruction instruction;
  instruction.Address = address;
  instruction.Size = size;
  instruction.FlowKind = flowKind;
  instruction.Source = "native-analysis-facts-test";
  return instruction;
}

bool testInstructionFlowKindStrings() {
  using notdec::bin2llvm::NativeInstructionFlowKind;
  using notdec::bin2llvm::toString;

  bool ok = true;
  ok &= expectEqual(toString(NativeInstructionFlowKind::None), "none",
                    "none flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::ConditionalBranch),
                    "conditional branch", "conditional flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::UnconditionalBranch),
                    "unconditional branch", "unconditional flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::IndirectBranch),
                    "indirect branch", "indirect flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::Return), "return",
                    "return flow kind");
  return ok;
}

bool testFlowNormalizerMovesNonCfgTargetToTail(const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base;
  uint64_t localFallthrough = entry + 0x10;
  uint64_t externalTarget = entry + 0x80;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x20;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, entry + 0x06, {localFallthrough}});
  function.Blocks.push_back({localFallthrough, localFallthrough + 0x02, {}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function\n";
    return false;
  }

  auto branch = makeInstruction(
      entry, 0x06,
      notdec::bin2llvm::NativeInstructionFlowKind::ConditionalBranch);
  branch.DirectFlowTargets.push_back(externalTarget);
  branch.Fallthrough = localFallthrough;
  state.addInstruction(std::move(branch));
  state.addInstruction(makeInstruction(
      localFallthrough, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::Return));

  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
  manager.run(state);

  const notdec::bin2llvm::NativeInstruction *normalized =
      state.instructionAt(entry);
  bool ok = true;
  ok &= expectTrue(normalized != nullptr, "normalized branch missing");
  if (normalized != nullptr) {
    ok &= expectTrue(normalized->DirectFlowTargets.empty(),
                     "non-CFG branch target stayed direct");
    ok &= expectTrue(normalized->TailFlowTargets.size() == 1 &&
                         normalized->TailFlowTargets.front() == externalTarget,
                     "non-CFG branch target was not marked tail");
  }
  return ok;
}

bool testFlowNormalizerFillsDecodedBlockHole(const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base + 0x30;
  uint64_t hole = entry + 0x02;
  uint64_t existingTarget = entry + 0x07;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x20;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, hole, {}});
  function.Blocks.push_back({existingTarget, existingTarget + 0x02, {}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function with hole\n";
    return false;
  }

  auto first = makeInstruction(
      entry, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::None);
  first.Fallthrough = hole;
  state.addInstruction(std::move(first));

  auto holeBody = makeInstruction(
      hole, 0x03, notdec::bin2llvm::NativeInstructionFlowKind::None);
  holeBody.Fallthrough = hole + 0x03;
  state.addInstruction(std::move(holeBody));

  auto holeTerminator = makeInstruction(
      hole + 0x03, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::UnconditionalBranch);
  holeTerminator.DirectFlowTargets.push_back(existingTarget);
  state.addInstruction(std::move(holeTerminator));

  state.addInstruction(makeInstruction(
      existingTarget, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::Return));

  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
  manager.run(state);

  const notdec::bin2llvm::NativeFunction *normalized = state.functionAt(entry);
  bool sawHoleBlock = false;
  if (normalized != nullptr) {
    for (const notdec::bin2llvm::NativeBasicBlock &block :
         normalized->Blocks) {
      if (block.Start == hole && block.End == existingTarget &&
          block.Successors.size() == 1 &&
          block.Successors.front() == existingTarget) {
        sawHoleBlock = true;
      }
    }
  }
  return expectTrue(sawHoleBlock, "decoded instruction hole was not blocked");
}

bool testFlowNormalizerDoesNotJoinMissingBlocksWithoutFallthrough(
    const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base + 0x60;
  uint64_t hole = entry + 0x02;
  uint64_t target = entry + 0x10;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x20;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, hole, {}});
  function.Blocks.push_back({target, target + 0x02, {}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function with disconnected hole\n";
    return false;
  }

  auto first = makeInstruction(
      entry, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::None);
  first.Fallthrough = hole;
  state.addInstruction(std::move(first));

  auto disconnected = makeInstruction(
      hole, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::None);
  disconnected.Fallthrough = target;
  state.addInstruction(std::move(disconnected));
  state.addInstruction(makeInstruction(
      hole + 0x02, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::Return));
  state.addInstruction(makeInstruction(
      target, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::Return));

  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
  manager.run(state);

  const notdec::bin2llvm::NativeFunction *normalized = state.functionAt(entry);
  bool sawShortHoleBlock = false;
  bool sawJoinedHoleBlock = false;
  if (normalized != nullptr) {
    for (const notdec::bin2llvm::NativeBasicBlock &block :
         normalized->Blocks) {
      if (block.Start == hole && block.End == hole + 0x02) {
        sawShortHoleBlock = true;
      }
      if (block.Start == hole && block.End > hole + 0x02) {
        sawJoinedHoleBlock = true;
      }
    }
  }

  bool ok = true;
  ok &= expectTrue(sawShortHoleBlock,
                   "missing block without fallthrough was not split");
  ok &= expectTrue(!sawJoinedHoleBlock,
                   "missing block was joined by address adjacency");
  return ok;
}

bool testBasicBlockSuccessorsRequireBlockStarts(const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base + 0x90;
  uint64_t successor = entry + 0x10;
  uint64_t middleOfSuccessor = successor + 0x01;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x30;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, entry + 0x02, {}});
  function.Blocks.push_back({successor, successor + 0x04, {}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function for successor validation\n";
    return false;
  }

  bool accepted =
      state.addBasicBlockSuccessors(entry, entry, {middleOfSuccessor});
  const notdec::bin2llvm::NativeFunction *normalized = state.functionAt(entry);
  bool successorWasAdded = false;
  if (normalized != nullptr) {
    for (const notdec::bin2llvm::NativeBasicBlock &block :
         normalized->Blocks) {
      if (block.Start == entry) {
        successorWasAdded = !block.Successors.empty();
      }
    }
  }

  bool ok = true;
  ok &= expectTrue(!accepted,
                   "non-block-start successor was accepted");
  ok &= expectTrue(!successorWasAdded,
                   "non-block-start successor was added");
  return ok;
}

bool testFlowNormalizerRemovesInvalidBlockSuccessors(const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base + 0xc0;
  uint64_t invalidTarget = entry + 0x08;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x20;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, entry + 0x02, {invalidTarget}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function with invalid successor\n";
    return false;
  }

  auto branch = makeInstruction(
      entry, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::UnconditionalBranch);
  branch.DirectFlowTargets.push_back(invalidTarget);
  state.addInstruction(std::move(branch));

  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
  manager.run(state);

  const notdec::bin2llvm::NativeFunction *normalized = state.functionAt(entry);
  bool successorWasRemoved = false;
  if (normalized != nullptr) {
    for (const notdec::bin2llvm::NativeBasicBlock &block :
         normalized->Blocks) {
      if (block.Start == entry) {
        successorWasRemoved = block.Successors.empty();
      }
    }
  }

  const notdec::bin2llvm::NativeInstruction *normalizedBranch =
      state.instructionAt(entry);
  bool targetWasMovedToTail = normalizedBranch != nullptr &&
                              normalizedBranch->DirectFlowTargets.empty() &&
                              normalizedBranch->TailFlowTargets.size() == 1 &&
                              normalizedBranch->TailFlowTargets.front() ==
                                  invalidTarget;

  bool ok = true;
  ok &= expectTrue(successorWasRemoved,
                   "invalid block successor was not removed");
  ok &= expectTrue(targetWasMovedToTail,
                   "invalid direct target was not marked as tail");
  return ok;
}

bool testFlowNormalizerSplitsDirectTargetInsideBlock(const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base + 0xf0;
  uint64_t target = entry + 0x02;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x20;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, target + 0x02, {target}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function with mid-block target\n";
    return false;
  }

  auto branch = makeInstruction(
      entry, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::UnconditionalBranch);
  branch.DirectFlowTargets.push_back(target);
  state.addInstruction(std::move(branch));
  state.addInstruction(makeInstruction(
      target, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::Return));

  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
  manager.run(state);

  const notdec::bin2llvm::NativeFunction *normalized = state.functionAt(entry);
  bool sawSourceBlock = false;
  bool sawTargetBlock = false;
  if (normalized != nullptr) {
    for (const notdec::bin2llvm::NativeBasicBlock &block :
         normalized->Blocks) {
      if (block.Start == entry && block.End == target &&
          block.Successors.size() == 1 && block.Successors.front() == target) {
        sawSourceBlock = true;
      }
      if (block.Start == target && block.End == target + 0x02) {
        sawTargetBlock = true;
      }
    }
  }

  const notdec::bin2llvm::NativeInstruction *normalizedBranch =
      state.instructionAt(entry);
  bool directTargetStayedCfg =
      normalizedBranch != nullptr &&
      normalizedBranch->DirectFlowTargets.size() == 1 &&
      normalizedBranch->DirectFlowTargets.front() == target &&
      normalizedBranch->TailFlowTargets.empty();

  bool ok = true;
  ok &= expectTrue(sawSourceBlock,
                   "source block was not split at direct target");
  ok &= expectTrue(sawTargetBlock, "direct target block was not created");
  ok &= expectTrue(directTargetStayedCfg,
                   "direct target was incorrectly marked as tail");
  return ok;
}

bool testFlowNormalizerSplitsFallthroughInsideBlock(const char *argv0) {
  auto binary = parseSelfBinary(argv0);
  if (!binary) {
    return false;
  }

  notdec::bin2llvm::NativeProgramState state(*binary);
  std::optional<uint64_t> base = firstExecutableAddress(state);
  if (!base) {
    std::cerr << "test binary has no executable range\n";
    return false;
  }

  uint64_t entry = *base + 0x130;
  uint64_t fallthrough = entry + 0x02;
  uint64_t taken = entry + 0x10;

  notdec::bin2llvm::NativeFunction function;
  function.Entry = entry;
  function.RangeStart = entry;
  function.RangeEnd = entry + 0x30;
  function.Source = "native-analysis-facts-test";
  function.Blocks.push_back({entry, fallthrough + 0x02, {taken}});
  function.Blocks.push_back({taken, taken + 0x02, {}});
  if (!state.addFunction(std::move(function))) {
    std::cerr << "failed to add test function with mid-block fallthrough\n";
    return false;
  }

  auto branch = makeInstruction(
      entry, 0x02,
      notdec::bin2llvm::NativeInstructionFlowKind::ConditionalBranch);
  branch.DirectFlowTargets.push_back(taken);
  branch.Fallthrough = fallthrough;
  state.addInstruction(std::move(branch));
  state.addInstruction(makeInstruction(
      fallthrough, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::Return));
  state.addInstruction(makeInstruction(
      taken, 0x02, notdec::bin2llvm::NativeInstructionFlowKind::Return));

  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
  manager.run(state);

  const notdec::bin2llvm::NativeFunction *normalized = state.functionAt(entry);
  bool sawSourceBlock = false;
  bool sawFallthroughBlock = false;
  if (normalized != nullptr) {
    for (const notdec::bin2llvm::NativeBasicBlock &block :
         normalized->Blocks) {
      if (block.Start == entry && block.End == fallthrough &&
          block.Successors.size() == 2) {
        bool hasTaken =
            std::find(block.Successors.begin(), block.Successors.end(),
                      taken) != block.Successors.end();
        bool hasFallthrough =
            std::find(block.Successors.begin(), block.Successors.end(),
                      fallthrough) != block.Successors.end();
        sawSourceBlock = hasTaken && hasFallthrough;
      }
      if (block.Start == fallthrough &&
          block.End == fallthrough + 0x02) {
        sawFallthroughBlock = true;
      }
    }
  }

  bool ok = true;
  ok &= expectTrue(sawSourceBlock,
                   "source block did not keep taken and fallthrough edges");
  ok &= expectTrue(sawFallthroughBlock,
                   "fallthrough target block was not created");
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 1) {
    return EXIT_FAILURE;
  }
  bool ok = true;
  ok &= testInstructionFlowKindStrings();
  ok &= testFlowNormalizerMovesNonCfgTargetToTail(argv[0]);
  ok &= testFlowNormalizerFillsDecodedBlockHole(argv[0]);
  ok &= testFlowNormalizerDoesNotJoinMissingBlocksWithoutFallthrough(argv[0]);
  ok &= testBasicBlockSuccessorsRequireBlockStarts(argv[0]);
  ok &= testFlowNormalizerRemovesInvalidBlockSuccessors(argv[0]);
  ok &= testFlowNormalizerSplitsDirectTargetInsideBlock(argv[0]);
  ok &= testFlowNormalizerSplitsFallthroughInsideBlock(argv[0]);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
