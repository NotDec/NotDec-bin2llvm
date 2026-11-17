#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

std::optional<uint64_t>
findNearestLeaBase(const NativeProgramState &state,
                   const NativeFunction &function, uint64_t beforeAddress,
                   const std::string &reg) {
  std::vector<const NativeInstruction *> instructions =
      state.instructionsInRange(function.RangeStart, beforeAddress);
  const std::string prefix = "LEA " + reg + ",[0x";
  for (auto iterator = instructions.rbegin(); iterator != instructions.rend();
       ++iterator) {
    const std::string &text = (*iterator)->Mnemonic;
    if (text.rfind(prefix, 0) == 0 && !text.empty() && text.back() == ']') {
      return parseHex(text.substr(prefix.size(),
                                  text.size() - prefix.size() - 1));
    }
    if (text.rfind("MOV " + reg + ",", 0) == 0 ||
        text.rfind("LEA " + reg + ",", 0) == 0 ||
        text.rfind("POP " + reg, 0) == 0) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool writesRegister(const std::string &text, const std::string &reg) {
  return text.rfind("MOV " + reg + ",", 0) == 0 ||
         text.rfind("LEA " + reg + ",", 0) == 0 ||
         text.rfind("POP " + reg, 0) == 0;
}

const NativeBasicBlock *functionBlockContaining(const NativeFunction &function,
                                                uint64_t address) {
  for (const NativeBasicBlock &block : function.Blocks) {
    if (block.Start <= address && address < block.End) {
      return &block;
    }
  }
  return nullptr;
}

std::vector<const NativeBasicBlock *>
functionPredecessorBlocks(const NativeFunction &function, uint64_t blockStart) {
  std::vector<const NativeBasicBlock *> predecessors;
  for (const NativeBasicBlock &block : function.Blocks) {
    if (std::find(block.Successors.begin(), block.Successors.end(),
                  blockStart) != block.Successors.end()) {
      predecessors.push_back(&block);
    }
  }
  return predecessors;
}

LeaBaseBlockScan findNearestLeaBaseInBlockRange(
    const NativeProgramState &state, const NativeBasicBlock &block,
    uint64_t beforeAddress, const std::string &reg) {
  std::vector<const NativeInstruction *> instructions =
      state.instructionsInRange(block.Start, beforeAddress);
  const std::string prefix = "LEA " + reg + ",[0x";
  for (auto iterator = instructions.rbegin(); iterator != instructions.rend();
       ++iterator) {
    const std::string &text = (*iterator)->Mnemonic;
    if (text.rfind(prefix, 0) == 0 && !text.empty() && text.back() == ']') {
      return {parseHex(text.substr(prefix.size(),
                                   text.size() - prefix.size() - 1)),
              false};
    }
    if (writesRegister(text, reg)) {
      return {std::nullopt, true};
    }
  }
  return {};
}

std::optional<uint64_t> findNearestLeaBaseInFunctionCFGFromBlock(
    const NativeProgramState &state, const NativeFunction &function,
    const NativeBasicBlock &block,
    uint64_t beforeAddress, const std::string &reg,
    std::set<uint64_t> &visitingBlocks,
    std::map<uint64_t, std::optional<uint64_t>> &memo) {
  const bool fullBlockScan = beforeAddress >= block.End;
  if (fullBlockScan) {
    auto memoIterator = memo.find(block.Start);
    if (memoIterator != memo.end()) {
      return memoIterator->second;
    }
  }

  uint64_t scanEnd = beforeAddress < block.End ? beforeAddress : block.End;
  LeaBaseBlockScan scan =
      findNearestLeaBaseInBlockRange(state, block, scanEnd, reg);
  if (scan.Base || scan.Killed) {
    if (fullBlockScan) {
      memo[block.Start] = scan.Base;
    }
    return scan.Base;
  }

  if (!visitingBlocks.insert(block.Start).second) {
    return std::nullopt;
  }

  std::vector<const NativeBasicBlock *> predecessors =
      functionPredecessorBlocks(function, block.Start);
  if (predecessors.empty()) {
    visitingBlocks.erase(block.Start);
    if (fullBlockScan) {
      memo[block.Start] = std::nullopt;
    }
    return std::nullopt;
  }

  std::optional<uint64_t> commonBase;
  bool sawBase = false;
  for (const NativeBasicBlock *predecessor : predecessors) {
    std::optional<uint64_t> predecessorBase =
        findNearestLeaBaseInFunctionCFGFromBlock(
            state, function, *predecessor, predecessor->End, reg,
            visitingBlocks, memo);
    if (!predecessorBase) {
      continue;
    }
    sawBase = true;
    if (!commonBase) {
      commonBase = predecessorBase;
      continue;
    }
    if (*commonBase != *predecessorBase) {
      visitingBlocks.erase(block.Start);
      if (fullBlockScan) {
        memo[block.Start] = std::nullopt;
      }
      return std::nullopt;
    }
  }
  visitingBlocks.erase(block.Start);
  if (!sawBase) {
    if (fullBlockScan) {
      memo[block.Start] = std::nullopt;
    }
    return std::nullopt;
  }
  if (fullBlockScan) {
    memo[block.Start] = commonBase;
  }
  return commonBase;
}

std::optional<uint64_t> findNearestLeaBaseInFunctionCFG(
    const NativeProgramState &state, const NativeFunction &function,
    uint64_t beforeAddress, const std::string &reg,
    std::set<uint64_t> &visitedBlocks) {
  const NativeBasicBlock *block = functionBlockContaining(function, beforeAddress);
  if (block == nullptr) {
    return std::nullopt;
  }
  std::map<uint64_t, std::optional<uint64_t>> memo;
  return findNearestLeaBaseInFunctionCFGFromBlock(
      state, function, *block, beforeAddress, reg, visitedBlocks, memo);
}

std::optional<uint64_t> parseX86LeaAbsoluteBase(const std::string &text,
                                                const std::string &reg);

std::optional<uint64_t> findNearestLeaBaseInDecodedCFG(
    const std::vector<NativeInstruction> &instructions, size_t beforeIndex,
    const std::string &reg) {
  if (instructions.empty() || beforeIndex == 0 ||
      beforeIndex > instructions.size()) {
    return std::nullopt;
  }

  std::map<uint64_t, size_t> indexByAddress;
  std::map<uint64_t, std::vector<uint64_t>> predecessors;
  for (size_t index = 0; index < instructions.size(); ++index) {
    indexByAddress[instructions[index].Address] = index;
  }
  for (const NativeInstruction &instruction : instructions) {
    for (uint64_t target : instruction.DirectFlowTargets) {
      if (indexByAddress.count(target) != 0) {
        predecessors[target].push_back(instruction.Address);
      }
    }
    if (instruction.Fallthrough &&
        isInstructionFallthroughTo(instruction, *instruction.Fallthrough) &&
        indexByAddress.count(*instruction.Fallthrough) != 0) {
      predecessors[*instruction.Fallthrough].push_back(instruction.Address);
    }
  }

  std::set<uint64_t> visited;
  std::function<std::optional<uint64_t>(uint64_t)> visit =
      [&](uint64_t address) -> std::optional<uint64_t> {
    if (!visited.insert(address).second) {
      return std::nullopt;
    }
    auto indexIterator = indexByAddress.find(address);
    if (indexIterator == indexByAddress.end()) {
      return std::nullopt;
    }
    const NativeInstruction &instruction = instructions[indexIterator->second];
    if (auto base = parseX86LeaAbsoluteBase(instruction.Mnemonic, reg)) {
      return base;
    }
    if (writesRegister(instruction.Mnemonic, reg)) {
      return std::nullopt;
    }

    auto predecessorIterator = predecessors.find(address);
    if (predecessorIterator == predecessors.end()) {
      return std::nullopt;
    }

    std::optional<uint64_t> candidate;
    std::vector<uint64_t> ordered = predecessorIterator->second;
    std::sort(ordered.begin(), ordered.end(), std::greater<uint64_t>());
    for (uint64_t predecessor : ordered) {
      std::optional<uint64_t> predecessorBase = visit(predecessor);
      if (!predecessorBase) {
        continue;
      }
      if (!candidate) {
        candidate = predecessorBase;
        continue;
      }
      if (*candidate != *predecessorBase) {
        return std::nullopt;
      }
    }
    return candidate;
  };

  return visit(instructions[beforeIndex - 1].Address);
}

std::string low32RegisterName(const std::string &reg) {
  if (reg == "RAX") {
    return "EAX";
  }
  if (reg == "RBX") {
    return "EBX";
  }
  if (reg == "RCX") {
    return "ECX";
  }
  if (reg == "RDX") {
    return "EDX";
  }
  if (reg == "RSI") {
    return "ESI";
  }
  if (reg == "RDI") {
    return "EDI";
  }
  if (reg == "RBP") {
    return "EBP";
  }
  if (reg == "RSP") {
    return "ESP";
  }
  if (reg.size() >= 2 && reg[0] == 'R' && std::isdigit(reg[1])) {
    return reg + "D";
  }
  return "";
}

std::string low8RegisterName(const std::string &reg) {
  if (reg == "RAX") {
    return "AL";
  }
  if (reg == "RBX") {
    return "BL";
  }
  if (reg == "RCX") {
    return "CL";
  }
  if (reg == "RDX") {
    return "DL";
  }
  if (reg == "RSI") {
    return "SIL";
  }
  if (reg == "RDI") {
    return "DIL";
  }
  if (reg == "RBP") {
    return "BPL";
  }
  if (reg == "RSP") {
    return "SPL";
  }
  if (reg.size() >= 2 && reg[0] == 'R' && std::isdigit(reg[1])) {
    return reg + "B";
  }
  return "";
}

std::vector<std::string> compareRegisterNamesForText(const std::string &reg) {
  if (reg == "EAX") {
    return {"EAX", "AL"};
  }
  if (reg == "EBX") {
    return {"EBX", "BL"};
  }
  if (reg == "ECX") {
    return {"ECX", "CL"};
  }
  if (reg == "EDX") {
    return {"EDX", "DL"};
  }
  if (reg == "AL" || reg == "BL" || reg == "CL" || reg == "DL" ||
      reg == "SIL" || reg == "DIL" || reg == "BPL" || reg == "SPL") {
    return {reg};
  }
  if (reg == "ESI") {
    return {"ESI"};
  }
  if (reg == "EDI") {
    return {"EDI"};
  }
  if (reg.size() >= 3 && reg[0] == 'R' && std::isdigit(reg[1]) &&
      reg.back() == 'D') {
    return {reg, reg.substr(0, reg.size() - 1) + "B"};
  }
  if (reg.size() >= 3 && reg[0] == 'R' && std::isdigit(reg[1]) &&
      reg.back() == 'B') {
    return {reg};
  }
  std::vector<std::string> names;
  names.push_back(reg);
  std::string low32 = low32RegisterName(reg);
  if (!low32.empty()) {
    names.push_back(low32);
  }
  std::string low8 = low8RegisterName(reg);
  if (!low8.empty()) {
    names.push_back(low8);
  }
  return names;
}

std::vector<std::string> compareRegisterNamesForIndex(const std::string &reg) {
  return compareRegisterNamesForText(reg);
}

void addUniqueString(std::vector<std::string> &values, std::string value) {
  if (value.empty() ||
      std::find(values.begin(), values.end(), value) != values.end()) {
    return;
  }
  values.push_back(std::move(value));
}

void addCompareRegisterNames(std::vector<std::string> &compareRegs,
                             const std::string &reg) {
  for (std::string name : compareRegisterNamesForText(reg)) {
    addUniqueString(compareRegs, std::move(name));
  }
}

std::optional<std::pair<std::string, std::string>>
parseX86MovRegReg(const std::string &text, const std::string &mnemonic) {
  const std::string prefix = mnemonic + " ";
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  size_t comma = text.find(',', prefix.size());
  if (comma == std::string::npos || comma + 1 >= text.size()) {
    return std::nullopt;
  }
  std::string dest = text.substr(prefix.size(), comma - prefix.size());
  std::string src = text.substr(comma + 1);
  if (dest.find('[') != std::string::npos ||
      src.find('[') != std::string::npos || src.find(' ') != std::string::npos) {
    return std::nullopt;
  }
  return std::make_pair(std::move(dest), std::move(src));
}

std::optional<std::pair<std::string, uint64_t>>
parseX86RegImmediate(const std::string &text, const std::string &mnemonic) {
  const std::string prefix = mnemonic + " ";
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  size_t comma = text.find(',', prefix.size());
  if (comma == std::string::npos || comma + 1 >= text.size()) {
    return std::nullopt;
  }
  std::string reg = text.substr(prefix.size(), comma - prefix.size());
  if (reg.empty() || reg.find('[') != std::string::npos ||
      reg.find(' ') != std::string::npos) {
    return std::nullopt;
  }
  std::optional<uint64_t> value =
      parseUnsignedNumber(trimAsciiWhitespace(text.substr(comma + 1)));
  if (!value) {
    return std::nullopt;
  }
  return std::make_pair(std::move(reg), *value);
}

std::optional<std::pair<std::string, std::string>>
parseX86MovRegMemory(const std::string &text) {
  const std::string prefix = "MOV ";
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  size_t comma = text.find(',', prefix.size());
  if (comma == std::string::npos || comma + 1 >= text.size()) {
    return std::nullopt;
  }
  std::string dest = text.substr(prefix.size(), comma - prefix.size());
  std::string memory = trimAsciiWhitespace(text.substr(comma + 1));
  if (dest.empty() || dest.find('[') != std::string::npos ||
      dest.find(' ') != std::string::npos ||
      memory.find(" ptr [") == std::string::npos) {
    return std::nullopt;
  }
  return std::make_pair(std::move(dest), std::move(memory));
}

std::optional<std::pair<std::string, std::string>>
parseX86MovMemoryReg(const std::string &text) {
  const std::string prefix = "MOV ";
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  size_t comma = text.find(',', prefix.size());
  if (comma == std::string::npos || comma + 1 >= text.size()) {
    return std::nullopt;
  }
  std::string memory = trimAsciiWhitespace(
      text.substr(prefix.size(), comma - prefix.size()));
  std::string src = trimAsciiWhitespace(text.substr(comma + 1));
  if (memory.find(" ptr [") == std::string::npos || src.empty() ||
      src.find('[') != std::string::npos ||
      src.find(' ') != std::string::npos) {
    return std::nullopt;
  }
  return std::make_pair(std::move(memory), std::move(src));
}

std::optional<std::pair<std::string, uint64_t>>
parseX86MemoryImmediate(const std::string &text, const std::string &mnemonic) {
  const std::string prefix = mnemonic + " ";
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  size_t comma = text.find(',', prefix.size());
  if (comma == std::string::npos || comma + 1 >= text.size()) {
    return std::nullopt;
  }
  std::string memory = trimAsciiWhitespace(
      text.substr(prefix.size(), comma - prefix.size()));
  if (memory.find(" ptr [") == std::string::npos) {
    return std::nullopt;
  }
  std::optional<uint64_t> value =
      parseUnsignedNumber(trimAsciiWhitespace(text.substr(comma + 1)));
  if (!value) {
    return std::nullopt;
  }
  return std::make_pair(std::move(memory), *value);
}

std::optional<uint64_t>
findNearestUpperBound(const NativeProgramState &state,
                      const NativeFunction &function, uint64_t beforeAddress,
                      const std::string &indexReg) {
  std::vector<const NativeInstruction *> instructions =
      state.instructionsInRange(function.RangeStart, beforeAddress);
  std::vector<std::string> compareRegs =
      compareRegisterNamesForIndex(indexReg);
  if (compareRegs.empty()) {
    return std::nullopt;
  }
  std::vector<std::string> compareMemoryOperands;

  std::vector<const NativeInstruction *> window;
  uint64_t scanned = 0;
  for (auto iterator = instructions.rbegin();
       iterator != instructions.rend() && scanned < 256; ++iterator, ++scanned) {
    window.push_back(*iterator);
  }

  for (const NativeInstruction *instruction : window) {
    const std::string &text = instruction->Mnemonic;
    for (const char *mnemonic : {"MOV", "MOVZX"}) {
      std::optional<std::pair<std::string, std::string>> move =
          parseX86MovRegReg(text, mnemonic);
      if (move && std::find(compareRegs.begin(), compareRegs.end(),
                            move->first) != compareRegs.end()) {
        addCompareRegisterNames(compareRegs, move->second);
      }
    }
    std::optional<std::pair<std::string, std::string>> memoryLoad =
        parseX86MovRegMemory(text);
    if (memoryLoad &&
        std::find(compareRegs.begin(), compareRegs.end(),
                  memoryLoad->first) != compareRegs.end()) {
      addUniqueString(compareMemoryOperands, memoryLoad->second);
    }
    // Some optimized parsers spill the switch index to a stack slot and later
    // compare that slot before reusing the live index register for dispatch.
    std::optional<std::pair<std::string, std::string>> memoryStore =
        parseX86MovMemoryReg(text);
    if (memoryStore &&
        std::find(compareRegs.begin(), compareRegs.end(),
                  memoryStore->second) != compareRegs.end()) {
      addUniqueString(compareMemoryOperands, memoryStore->first);
    }
  }

  for (const NativeInstruction *instruction : window) {
    const std::string &text = instruction->Mnemonic;
    for (const std::string &compareReg : compareRegs) {
      std::optional<std::pair<std::string, uint64_t>> andMask =
          parseX86RegImmediate(text, "AND");
      if (andMask && andMask->first == compareReg && andMask->second < 256) {
        return andMask->second + 1;
      }
      const std::string prefix = "CMP " + compareReg + ",0x";
      if (text.rfind(prefix, 0) == 0) {
        std::optional<uint64_t> maxIndex = parseHex(text.substr(prefix.size()));
        if (maxIndex) {
          return *maxIndex + 1;
        }
      }
    }
    std::optional<std::pair<std::string, uint64_t>> memoryCompare =
        parseX86MemoryImmediate(text, "CMP");
    if (memoryCompare &&
        std::find(compareMemoryOperands.begin(), compareMemoryOperands.end(),
                  memoryCompare->first) != compareMemoryOperands.end()) {
      return memoryCompare->second + 1;
    }
  }
  return std::nullopt;
}

std::optional<std::string> parseX86RegisterAfterPrefix(
    const std::string &text, const std::string &prefix) {
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::string reg = text.substr(prefix.size());
  if (reg.empty() || reg.find(' ') != std::string::npos ||
      reg.find(',') != std::string::npos || reg.find('[') != std::string::npos) {
    return std::nullopt;
  }
  return reg;
}

std::optional<X86IndirectMemoryTarget>
parseX86RegisterMemoryTarget(const std::string &text,
                             const std::string &mnemonic,
                             const std::string &destReg) {
  const std::string prefix = mnemonic + " " + destReg + ",qword ptr [";
  if (text.rfind(prefix, 0) != 0 || text.empty() || text.back() != ']') {
    return std::nullopt;
  }

  std::string inner = trimAsciiWhitespace(
      text.substr(prefix.size(), text.size() - prefix.size() - 1));
  if (inner.empty()) {
    return std::nullopt;
  }

  X86IndirectMemoryTarget target;
  size_t plus = inner.find('+');
  if (plus == std::string::npos) {
    target.BaseReg = trimAsciiWhitespace(inner);
    if (target.BaseReg.empty()) {
      return std::nullopt;
    }
    return target;
  }

  target.BaseReg = trimAsciiWhitespace(inner.substr(0, plus));
  if (target.BaseReg.empty()) {
    return std::nullopt;
  }
  std::string displacementText = trimAsciiWhitespace(inner.substr(plus + 1));
  std::optional<uint64_t> displacement = parseUnsignedNumber(displacementText);
  if (!displacement) {
    return std::nullopt;
  }
  target.Displacement = *displacement;
  return target;
}

std::optional<X86IndirectMemoryTarget>
parseX86IndirectMemoryTarget(const std::string &text,
                             const std::string &mnemonic) {
  const std::string prefix = mnemonic + " qword ptr [";
  if (text.rfind(prefix, 0) != 0 || text.empty() || text.back() != ']') {
    return std::nullopt;
  }

  std::string inner = trimAsciiWhitespace(
      text.substr(prefix.size(), text.size() - prefix.size() - 1));
  if (inner.empty()) {
    return std::nullopt;
  }

  X86IndirectMemoryTarget target;
  size_t plus = inner.find('+');
  if (plus == std::string::npos) {
    target.BaseReg = trimAsciiWhitespace(inner);
    if (target.BaseReg.empty()) {
      return std::nullopt;
    }
    return target;
  }

  target.BaseReg = trimAsciiWhitespace(inner.substr(0, plus));
  if (target.BaseReg.empty()) {
    return std::nullopt;
  }
  std::string displacementText = trimAsciiWhitespace(inner.substr(plus + 1));
  std::optional<uint64_t> displacement = parseUnsignedNumber(displacementText);
  if (!displacement) {
    return std::nullopt;
  }
  target.Displacement = *displacement;
  return target;
}

std::optional<uint64_t> parseX86LeaAbsoluteBase(const std::string &text,
                                                const std::string &reg) {
  const std::string prefix = "LEA " + reg + ",[0x";
  if (text.rfind(prefix, 0) != 0 || text.empty() || text.back() != ']') {
    return std::nullopt;
  }
  return parseHex(text.substr(prefix.size(), text.size() - prefix.size() - 1));
}

std::optional<uint64_t> findNearestLeaBaseInDecodedInstructions(
    const std::vector<NativeInstruction> &instructions, size_t beforeIndex,
    const std::string &reg) {
  for (size_t index = beforeIndex; index > 0; --index) {
    const std::string &text = instructions[index - 1].Mnemonic;
    if (auto address = parseX86LeaAbsoluteBase(text, reg)) {
      return address;
    }
    if (writesRegister(text, reg)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> findNearestLeaBaseInKnownFallthroughInstructions(
    const NativeProgramState &state, uint64_t beforeAddress,
    const std::string &reg) {
  uint64_t start = beforeAddress > 64 ? beforeAddress - 64 : 0;
  std::vector<const NativeInstruction *> instructions =
      state.instructionsInRange(start, beforeAddress);
  uint64_t expectedNext = beforeAddress;
  for (auto iterator = instructions.rbegin(); iterator != instructions.rend();
       ++iterator) {
    const NativeInstruction &instruction = **iterator;
    if (!isInstructionFallthroughTo(instruction, expectedNext)) {
      return std::nullopt;
    }
    const std::string &text = instruction.Mnemonic;
    if (auto address = parseX86LeaAbsoluteBase(text, reg)) {
      return address;
    }
    if (writesRegister(text, reg)) {
      return std::nullopt;
    }
    expectedNext = instruction.Address;
  }
  return std::nullopt;
}

std::optional<X86IndirectMemoryTarget>
findNearestRegisterLoadInDecodedInstructions(
    const std::vector<NativeInstruction> &instructions, size_t beforeIndex,
    const std::string &reg) {
  for (size_t index = beforeIndex; index > 0; --index) {
    const std::string &text = instructions[index - 1].Mnemonic;
    if (auto target = parseX86RegisterMemoryTarget(text, "MOV", reg)) {
      return target;
    }
    if (writesRegister(text, reg)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<X86IndirectMemoryTarget>
findNearestRegisterLoadInKnownFallthroughInstructions(
    const NativeProgramState &state, uint64_t beforeAddress,
    const std::string &reg) {
  uint64_t start = beforeAddress > 64 ? beforeAddress - 64 : 0;
  std::vector<const NativeInstruction *> instructions =
      state.instructionsInRange(start, beforeAddress);
  uint64_t expectedNext = beforeAddress;
  for (auto iterator = instructions.rbegin(); iterator != instructions.rend();
       ++iterator) {
    const NativeInstruction &instruction = **iterator;
    if (!isInstructionFallthroughTo(instruction, expectedNext)) {
      return std::nullopt;
    }
    const std::string &text = instruction.Mnemonic;
    if (auto target = parseX86RegisterMemoryTarget(text, "MOV", reg)) {
      return target;
    }
    if (writesRegister(text, reg)) {
      return std::nullopt;
    }
    expectedNext = instruction.Address;
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, std::string>>
parseX86AddRegReg(const std::string &text) {
  const std::string prefix = "ADD ";
  if (text.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  size_t comma = text.find(',', prefix.size());
  if (comma == std::string::npos || comma + 1 >= text.size()) {
    return std::nullopt;
  }
  return std::make_pair(text.substr(prefix.size(), comma - prefix.size()),
                        text.substr(comma + 1));
}

std::optional<X86PicI32DispatchPattern>
parseX86PicI32DispatchPattern(const NativeInstruction &load,
                              const NativeInstruction &add,
                              const NativeInstruction &branch) {
  std::optional<std::string> branchReg =
      parseX86RegisterAfterPrefix(branch.Mnemonic, "JMP ");
  if (!branchReg) {
    return std::nullopt;
  }

  std::optional<std::pair<std::string, std::string>> addRegs =
      parseX86AddRegReg(add.Mnemonic);
  if (!addRegs || addRegs->first != *branchReg) {
    return std::nullopt;
  }

  const std::string loadPrefix =
      "MOVSXD " + *branchReg + ",dword ptr [" + addRegs->second + " + ";
  const std::string loadSuffix = "*0x4]";
  if (load.Mnemonic.rfind(loadPrefix, 0) != 0 ||
      load.Mnemonic.size() <= loadPrefix.size() + loadSuffix.size() ||
      load.Mnemonic.compare(load.Mnemonic.size() - loadSuffix.size(),
                            loadSuffix.size(), loadSuffix) != 0) {
    return std::nullopt;
  }

  X86PicI32DispatchPattern pattern;
  pattern.TargetReg = *branchReg;
  pattern.BaseReg = addRegs->second;
  pattern.IndexReg = load.Mnemonic.substr(
      loadPrefix.size(),
      load.Mnemonic.size() - loadPrefix.size() - loadSuffix.size());
  if (pattern.IndexReg.empty()) {
    return std::nullopt;
  }
  return pattern;
}

std::optional<X86PicI32DispatchPattern>
findX86PicI32DispatchPattern(const NativeProgramState &state,
                             const NativeFunction &function,
                             const NativeInstruction &branch,
                             const std::string &branchReg) {
  std::vector<const NativeInstruction *> instructions =
      state.instructionsInRange(function.RangeStart, branch.Address);
  const NativeInstruction *matchedAdd = nullptr;
  size_t addIndex = 0;
  uint64_t scanned = 0;
  for (size_t index = instructions.size(); index > 0 && scanned < 8;
       --index, ++scanned) {
    const NativeInstruction &instruction = *instructions[index - 1];
    std::optional<std::pair<std::string, std::string>> addRegs =
        parseX86AddRegReg(instruction.Mnemonic);
    if (!addRegs || addRegs->first != branchReg) {
      continue;
    }
    matchedAdd = &instruction;
    addIndex = index - 1;
    break;
  }
  if (matchedAdd == nullptr) {
    return std::nullopt;
  }

  for (size_t index = addIndex; index > 0 && addIndex - (index - 1) <= 4;
       --index) {
    const NativeInstruction &load = *instructions[index - 1];
    std::optional<X86PicI32DispatchPattern> pattern =
        parseX86PicI32DispatchPattern(load, *matchedAdd, branch);
    if (pattern) {
      return pattern;
    }
  }
  return std::nullopt;
}

std::optional<X86PicI32JumpDispatch>
matchX86PicI32OffsetDispatch(const NativeProgramState &state,
                             const NativeFunction &function,
                             uint64_t branchAddress) {
  const NativeInstruction *branch = state.instructionAt(branchAddress);
  if (branch == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> branchReg =
      parseX86RegisterAfterPrefix(branch->Mnemonic, "JMP ");
  if (!branchReg) {
    return std::nullopt;
  }
  std::optional<X86PicI32DispatchPattern> pattern =
      findX86PicI32DispatchPattern(state, function, *branch, *branchReg);
  if (!pattern) {
    return std::nullopt;
  }

  std::optional<uint64_t> tableBase =
      findNearestLeaBase(state, function, branchAddress, pattern->BaseReg);
  if (!tableBase) {
    std::set<uint64_t> visitedBlocks;
    tableBase = findNearestLeaBaseInFunctionCFG(
        state, function, branchAddress, pattern->BaseReg, visitedBlocks);
  }
  std::optional<uint64_t> entryCount =
      findNearestUpperBound(state, function, branchAddress, pattern->IndexReg);
  if (!tableBase || !entryCount || *entryCount == 0 || *entryCount > 256) {
    return std::nullopt;
  }

  X86PicI32JumpDispatch dispatch;
  for (const NativeBasicBlock &block : function.Blocks) {
    if (branchAddress >= block.Start && branchAddress < block.End) {
      dispatch.BlockStart = block.Start;
      break;
    }
  }
  if (dispatch.BlockStart == 0) {
    return std::nullopt;
  }
  dispatch.TableBase = *tableBase;
  dispatch.EntryCount = *entryCount;
  return dispatch;
}

bool isExternalFunctionPointerRelocationAt(const NativeProgramState &state,
                                           uint64_t address) {
  LIEF::ELF::ARCH arch = state.binary().header().machine_type();
  if (!supportsRelocationPltAnalysis(arch)) {
    return false;
  }
  for (const NativeRelocationInfo &relocation : state.relocations()) {
    if (relocation.Address != address || relocation.Status != "external" ||
        relocation.SymbolName.empty()) {
      continue;
    }
    LIEF::ELF::Relocation::TYPE type =
        static_cast<LIEF::ELF::Relocation::TYPE>(relocation.Type);
    return isGlobDatRelocation(arch, type) ||
           isAbsoluteExternalFunctionPointerRelocation(arch, type);
  }
  return false;
}

std::optional<X86ExternalFunctionPointerMatch>
matchAnyLeaBaseInDecodedInstructions(
    const std::vector<NativeInstruction> &instructions,
    const std::string &reg, uint64_t displacement, NativeXrefKind kind,
    const NativeProgramState &state) {
  for (const NativeInstruction &instruction : instructions) {
    std::optional<uint64_t> base =
        parseX86LeaAbsoluteBase(instruction.Mnemonic, reg);
    if (!base || *base > std::numeric_limits<uint64_t>::max() - displacement) {
      continue;
    }
    X86ExternalFunctionPointerMatch match;
    match.SlotAddress = *base + displacement;
    match.Kind = kind;
    if (isExternalFunctionPointerRelocationAt(state, match.SlotAddress)) {
      return match;
    }
  }
  return std::nullopt;
}

std::optional<X86ExternalFunctionPointerMatch>
matchX86ExternalFunctionPointerInstruction(
    const NativeProgramState &state,
    const std::vector<NativeInstruction> &instructions, size_t index) {
  if (index >= instructions.size()) {
    return std::nullopt;
  }

  const NativeInstruction &instruction = instructions[index];
  auto matchExternalPointer = [&](std::optional<uint64_t> base,
                                  uint64_t displacement,
                                  NativeXrefKind kind)
      -> std::optional<X86ExternalFunctionPointerMatch> {
    if (!base || *base > std::numeric_limits<uint64_t>::max() - displacement) {
      return std::nullopt;
    }
    X86ExternalFunctionPointerMatch match;
    match.SlotAddress = *base + displacement;
    match.Kind = kind;
    if (isExternalFunctionPointerRelocationAt(state, match.SlotAddress)) {
      return match;
    }
    return std::nullopt;
  };

  std::optional<std::string> regJump =
      parseX86RegisterAfterPrefix(instruction.Mnemonic, "JMP ");
  if (!regJump) {
    regJump = parseX86RegisterAfterPrefix(instruction.Mnemonic, "CALL ");
  }
  if (regJump) {
    std::optional<X86IndirectMemoryTarget> loadTarget =
        findNearestRegisterLoadInDecodedInstructions(instructions, index,
                                                     *regJump);
    if (!loadTarget) {
      loadTarget = findNearestRegisterLoadInKnownFallthroughInstructions(
          state, instruction.Address, *regJump);
    }
    if (loadTarget) {
      if (loadTarget->BaseReg.rfind("0x", 0) == 0) {
        if (auto match = matchExternalPointer(
                parseUnsignedNumber(loadTarget->BaseReg),
                loadTarget->Displacement,
                instruction.HasIndirectCall ? NativeXrefKind::Call
                                           : NativeXrefKind::Flow)) {
          return match;
        }
      } else {
        if (auto match = matchExternalPointer(
                findNearestLeaBaseInDecodedInstructions(
                    instructions, index, loadTarget->BaseReg),
                loadTarget->Displacement,
                instruction.HasIndirectCall ? NativeXrefKind::Call
                                           : NativeXrefKind::Flow)) {
          return match;
        }
        if (const NativeFunction *function =
                state.functionContaining(instruction.Address)) {
          if (auto match = matchExternalPointer(
                  findNearestLeaBase(state, *function, instruction.Address,
                                     loadTarget->BaseReg),
                  loadTarget->Displacement,
                  instruction.HasIndirectCall ? NativeXrefKind::Call
                                             : NativeXrefKind::Flow)) {
            return match;
          }
        }
        if (auto match = matchExternalPointer(
                findNearestLeaBaseInKnownFallthroughInstructions(
                    state, instruction.Address, loadTarget->BaseReg),
                loadTarget->Displacement,
                instruction.HasIndirectCall ? NativeXrefKind::Call
                                           : NativeXrefKind::Flow)) {
          return match;
        }
        if (auto match = matchExternalPointer(
                findNearestLeaBaseInDecodedCFG(instructions, index,
                                               loadTarget->BaseReg),
                loadTarget->Displacement,
                instruction.HasIndirectCall ? NativeXrefKind::Call
                                           : NativeXrefKind::Flow)) {
          return match;
        }
        if (auto match = matchAnyLeaBaseInDecodedInstructions(
                instructions, loadTarget->BaseReg, loadTarget->Displacement,
                instruction.HasIndirectCall ? NativeXrefKind::Call
                                           : NativeXrefKind::Flow, state)) {
          return match;
        }
        if (const NativeFunction *function =
                state.functionContaining(instruction.Address)) {
          std::set<uint64_t> visitedBlocks;
          if (auto match = matchExternalPointer(
                  findNearestLeaBaseInFunctionCFG(
                      state, *function, instruction.Address,
                      loadTarget->BaseReg, visitedBlocks),
                  loadTarget->Displacement,
                  instruction.HasIndirectCall ? NativeXrefKind::Call
                                             : NativeXrefKind::Flow)) {
            return match;
          }
        }
      }
    }
  }

  std::optional<X86IndirectMemoryTarget> target;
  NativeXrefKind kind = NativeXrefKind::Flow;
  if (instruction.HasIndirectCall) {
    target = parseX86IndirectMemoryTarget(instruction.Mnemonic, "CALL");
    kind = NativeXrefKind::Call;
  } else if (instruction.FlowKind == NativeInstructionFlowKind::IndirectBranch) {
    target = parseX86IndirectMemoryTarget(instruction.Mnemonic, "JMP");
  }
  if (!target) {
    return std::nullopt;
  }

  auto matchExternalPointerForTarget =
      [&](std::optional<uint64_t> base) -> std::optional<X86ExternalFunctionPointerMatch> {
    if (!base || *base > std::numeric_limits<uint64_t>::max() -
                             target->Displacement) {
      return std::nullopt;
    }
    X86ExternalFunctionPointerMatch match;
    match.SlotAddress = *base + target->Displacement;
    match.Kind = kind;
    if (isExternalFunctionPointerRelocationAt(state, match.SlotAddress)) {
      return match;
    }
    return std::nullopt;
  };

  if (target->BaseReg.rfind("0x", 0) == 0) {
    if (auto match = matchExternalPointerForTarget(
            parseUnsignedNumber(target->BaseReg))) {
      return match;
    }
  } else {
    if (auto match = matchExternalPointerForTarget(
            findNearestLeaBaseInDecodedInstructions(instructions, index,
                                                    target->BaseReg))) {
      return match;
    }
    if (const NativeFunction *function =
            state.functionContaining(instruction.Address)) {
      if (auto match = matchExternalPointerForTarget(
              findNearestLeaBase(state, *function, instruction.Address,
                                 target->BaseReg))) {
        return match;
      }
    }
    if (auto match = matchExternalPointerForTarget(
            findNearestLeaBaseInKnownFallthroughInstructions(
                state, instruction.Address, target->BaseReg))) {
      return match;
    }
    if (auto match = matchExternalPointerForTarget(
            findNearestLeaBaseInDecodedCFG(instructions, index,
                                           target->BaseReg))) {
      return match;
    }
    if (auto match = matchAnyLeaBaseInDecodedInstructions(
            instructions, target->BaseReg, target->Displacement, kind, state)) {
      return match;
    }
    if (const NativeFunction *function =
            state.functionContaining(instruction.Address)) {
      std::set<uint64_t> visitedBlocks;
      if (auto match = matchExternalPointerForTarget(
              findNearestLeaBaseInFunctionCFG(state, *function,
                                              instruction.Address,
                                              target->BaseReg, visitedBlocks))) {
        return match;
      }
    }
  }
  return std::nullopt;
}

bool functionContainsBlockStart(const NativeProgramState &state,
                                uint64_t address) {
  const NativeFunction *function = state.functionContaining(address);
  if (function == nullptr) {
    return false;
  }
  for (const NativeBasicBlock &block : function->Blocks) {
    if (block.Start == address) {
      return true;
    }
  }
  return false;
}

bool functionContainsAddress(const NativeFunction &function, uint64_t address) {
  return address >= function.RangeStart && address < function.RangeEnd;
}

bool readX86PicI32Targets(const NativeProgramState &state, uint64_t tableBase,
                          uint64_t entryCount, bool onlyExistingBlocks,
                          std::vector<uint64_t> &targets, bool &complete) {
  complete = true;
  for (uint64_t index = 0; index < entryCount; ++index) {
    std::optional<int64_t> offset = readSigned32(state, tableBase + index * 4);
    if (!offset) {
      return false;
    }
    uint64_t target =
        static_cast<uint64_t>(static_cast<int64_t>(tableBase) + *offset);
    if (!state.isExecutableAddress(target)) {
      return false;
    }
    if (!functionContainsBlockStart(state, target)) {
      complete = false;
      if (onlyExistingBlocks) {
        continue;
      }
    }
    addUniqueAddress(targets, target);
  }
  return true;
}


} // namespace notdec::bin2llvm
