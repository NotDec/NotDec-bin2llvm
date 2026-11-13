#include "notdec-bin2llvm/discovery/NativeAnalysis.h"

#include "notdec-bin2llvm/elf/LiefElfLoadImage.h"
#include "notdec-bin2llvm/core/SleighLift.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/DynamicEntry.hpp>
#include <LIEF/ELF/EnumToString.hpp>
#include <LIEF/ELF/Relocation.hpp>
#include <LIEF/ELF/Section.hpp>
#include <LIEF/ELF/Segment.hpp>
#include <LIEF/ELF/Symbol.hpp>

#if NOTDEC_BIN2LLVM_ENABLE_GTIRB
#include <gtirb_pprinter/PrettyPrinter.hpp>
#include <gtirb/gtirb.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <functional>
#include <ostream>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>


#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

std::string toString(NativeFunctionConfidence confidence) {
  switch (confidence) {
  case NativeFunctionConfidence::High:
    return "high";
  case NativeFunctionConfidence::Medium:
    return "medium";
  case NativeFunctionConfidence::Low:
    return "low";
  }
  return "unknown";
}

std::string toString(NativeXrefKind kind) {
  switch (kind) {
  case NativeXrefKind::Flow:
    return "flow";
  case NativeXrefKind::Call:
    return "call";
  case NativeXrefKind::Data:
    return "data";
  case NativeXrefKind::String:
    return "string";
  }
  return "unknown";
}

std::string toString(NativeUnresolvedFlowKind kind) {
  switch (kind) {
  case NativeUnresolvedFlowKind::IndirectCall:
    return "indirect call";
  case NativeUnresolvedFlowKind::IndirectBranch:
    return "indirect branch";
  case NativeUnresolvedFlowKind::IndirectTailBranch:
    return "indirect tail branch";
  }
  return "unknown";
}

std::string toString(NativeInstructionFlowKind kind) {
  switch (kind) {
  case NativeInstructionFlowKind::None:
    return "none";
  case NativeInstructionFlowKind::ConditionalBranch:
    return "conditional branch";
  case NativeInstructionFlowKind::UnconditionalBranch:
    return "unconditional branch";
  case NativeInstructionFlowKind::IndirectBranch:
    return "indirect branch";
  case NativeInstructionFlowKind::Trap:
    return "trap";
  case NativeInstructionFlowKind::Return:
    return "return";
  }
  return "unknown";
}

std::optional<NativeElfArchitectureSpec>
nativeElfArchitectureSpec(const LIEF::ELF::Binary &binary) {
  switch (binary.header().machine_type()) {
  case LIEF::ELF::ARCH::X86_64:
    return NativeElfArchitectureSpec{"x86-64.sla", "x86-64.pspec",
                                     "x86-64-gcc.cspec"};
  case LIEF::ELF::ARCH::I386:
    return NativeElfArchitectureSpec{"x86.sla", "x86.pspec",
                                     "x86gcc.cspec"};
  default:
    return std::nullopt;
  }
}

bool isSupportedNativeElfArchitecture(const LIEF::ELF::Binary &binary) {
  return nativeElfArchitectureSpec(binary).has_value();
}

std::string nativeElfArchitectureName(const LIEF::ELF::Binary &binary) {
  const char *name = LIEF::ELF::to_string(binary.header().machine_type());
  if (name != nullptr) {
    return name;
  }
  return "unknown";
}

std::string unsupportedNativeElfArchitectureMessage(
    const LIEF::ELF::Binary &binary, const std::string &component) {
  return component + " currently supports x86-64 and i386 ELF only; got " +
         nativeElfArchitectureName(binary);
}

NativeProgramState::NativeProgramState(const LIEF::ELF::Binary &binary)
    : Binary(binary), PointerSize(binary.ptr_size()) {
  for (const LIEF::ELF::Segment &segment : binary.segments()) {
    if (segment.type() != LIEF::ELF::Segment::TYPE::LOAD) {
      continue;
    }

    NativeMemoryRange range;
    range.Start = segment.virtual_address();
    range.Size = segment.virtual_size();
    range.Readable = segment.has(LIEF::ELF::Segment::FLAGS::R);
    range.Writable = segment.has(LIEF::ELF::Segment::FLAGS::W);
    range.Executable = segment.has(LIEF::ELF::Segment::FLAGS::X);
    auto content = segment.content();
    range.Bytes.assign(content.begin(), content.end());
    MemoryRanges.push_back(std::move(range));
  }

  std::sort(MemoryRanges.begin(), MemoryRanges.end(),
            [](const NativeMemoryRange &lhs, const NativeMemoryRange &rhs) {
              return lhs.Start < rhs.Start;
            });

  for (const LIEF::ELF::Section &section : binary.sections()) {
    if (section.virtual_address() == 0 || section.size() == 0) {
      continue;
    }

    NativeSectionInfo info;
    info.Name = section.name();
    info.Address = section.virtual_address();
    info.Size = section.size();
    info.Executable = section.has(LIEF::ELF::Section::FLAGS::EXECINSTR);
    Sections.push_back(std::move(info));
  }
}

bool NativeProgramState::isExecutableAddress(uint64_t address) const {
  for (const NativeMemoryRange &range : MemoryRanges) {
    if (range.Start > address) {
      break;
    }
    if (range.Executable && containsAddress(range.Start, range.Size, address)) {
      return true;
    }
  }
  return false;
}

std::optional<uint64_t>
NativeProgramState::readPointer(uint64_t address) const {
  auto relocated = RelocatedPointers.find(address);
  if (relocated != RelocatedPointers.end()) {
    return relocated->second;
  }
  return readRawPointer(address);
}

std::optional<uint64_t>
NativeProgramState::readRawPointer(uint64_t address) const {
  for (const NativeMemoryRange &range : MemoryRanges) {
    if (range.Start > address) {
      break;
    }
    if (!range.Readable || !containsAddress(range.Start, range.Size, address)) {
      continue;
    }

    uint64_t offset = address - range.Start;
    if (offset + PointerSize > range.Bytes.size()) {
      return std::nullopt;
    }

    uint64_t value = 0;
    for (uint8_t index = 0; index < PointerSize; ++index) {
      value |= static_cast<uint64_t>(range.Bytes[offset + index])
               << (index * 8);
    }
    return value;
  }
  return std::nullopt;
}

std::optional<std::string>
NativeProgramState::lookupPltExternal(uint64_t address) const {
  for (const NativePltEntry &entry : PltEntries) {
    if (entry.StubAddress == address) {
      return entry.SymbolName;
    }
  }
  return std::nullopt;
}

const NativeFunction *
NativeProgramState::functionAt(uint64_t entry) const {
  auto iterator = Functions.find(entry);
  if (iterator == Functions.end()) {
    return nullptr;
  }
  return &iterator->second;
}

const NativeFunction *
NativeProgramState::functionContaining(uint64_t address) const {
  for (const auto &[entry, function] : Functions) {
    (void)entry;
    for (const NativeBasicBlock &block : function.Blocks) {
      if (block.Start <= address && address < block.End) {
        return &function;
      }
    }
  }
  return nullptr;
}

std::vector<const NativeXref *>
NativeProgramState::xrefsFrom(uint64_t address) const {
  std::vector<const NativeXref *> result;
  auto iterator = XrefsByFrom.find(address);
  if (iterator == XrefsByFrom.end()) {
    return result;
  }
  result.reserve(iterator->second.size());
  for (size_t index : iterator->second) {
    result.push_back(&Xrefs[index]);
  }
  return result;
}

std::vector<const NativeXref *>
NativeProgramState::xrefsTo(uint64_t address) const {
  std::vector<const NativeXref *> result;
  auto iterator = XrefsByTo.find(address);
  if (iterator == XrefsByTo.end()) {
    return result;
  }
  result.reserve(iterator->second.size());
  for (size_t index : iterator->second) {
    result.push_back(&Xrefs[index]);
  }
  return result;
}

const NativeInstruction *
NativeProgramState::instructionAt(uint64_t address) const {
  auto iterator = Instructions.find(address);
  if (iterator == Instructions.end()) {
    return nullptr;
  }
  return &iterator->second;
}

std::vector<const NativeInstruction *>
NativeProgramState::instructionsInRange(uint64_t start, uint64_t end) const {
  std::vector<const NativeInstruction *> result;
  if (start >= end) {
    return result;
  }

  for (auto iterator = Instructions.lower_bound(start);
       iterator != Instructions.end() && iterator->first < end; ++iterator) {
    result.push_back(&iterator->second);
  }
  return result;
}

bool NativeProgramState::addFunctionSeed(uint64_t address, uint64_t size,
                                         std::string name, std::string source,
                                         NativeFunctionConfidence confidence) {
  if (address == 0) {
    return false;
  }

  ++SourceCounts[source];
  auto [iterator, inserted] = FunctionSeeds.try_emplace(address);
  NativeFunctionSeed &seed = iterator->second;
  if (inserted) {
    seed.Address = address;
    seed.Size = size;
    seed.PrimaryName = std::move(name);
    seed.Confidence = confidence;
    if (size != 0 && address <= std::numeric_limits<uint64_t>::max() - size) {
      seed.RangeStart = address;
      seed.RangeEnd = address + size;
      seed.RangeSource = source;
    }
  } else {
    bool promotedRangeHint = !seed.IsEntry;
    if (promotedRangeHint) {
      seed.IsEntry = true;
    }
    if (seed.Size == 0 && size != 0) {
      seed.Size = size;
    }
    if (size != 0 && address <= std::numeric_limits<uint64_t>::max() - size &&
        seed.RangeSource.empty()) {
      seed.RangeStart = address;
      seed.RangeEnd = address + size;
      seed.RangeSource = source;
    } else if (size != 0 &&
               address <= std::numeric_limits<uint64_t>::max() - size &&
               (seed.RangeStart != address || seed.RangeEnd != address + size)) {
      Notes.push_back("function seed range mismatch at " + hexAddress(address) +
                      ": existing " + seed.RangeSource + " [" +
                      hexAddress(seed.RangeStart) + ", " +
                      hexAddress(seed.RangeEnd) + "), new " + source + " [" +
                      hexAddress(address) + ", " + hexAddress(address + size) +
                      ")");
    }
    if (!name.empty() && seed.PrimaryName.empty()) {
      seed.PrimaryName = std::move(name);
    } else if (!name.empty() && name != seed.PrimaryName &&
               std::find(seed.Aliases.begin(), seed.Aliases.end(), name) ==
                   seed.Aliases.end()) {
      seed.Aliases.push_back(std::move(name));
    }
    seed.Confidence = mergeConfidence(seed.Confidence, confidence);
    if (promotedRangeHint) {
      FunctionWorklist.push_back({address, source});
    }
  }

  if (inserted) {
    FunctionWorklist.push_back({address, source});
  }
  if (std::find(seed.Sources.begin(), seed.Sources.end(), source) ==
      seed.Sources.end()) {
    seed.Sources.push_back(std::move(source));
  }
  return inserted;
}

bool NativeProgramState::addFunction(NativeFunction function) {
  if (function.Entry == 0 || function.RangeStart >= function.RangeEnd) {
    return false;
  }
  if (!isExecutableAddress(function.Entry)) {
    Notes.push_back("confirmed function entry outside executable memory: " +
                    hexAddress(function.Entry));
    return false;
  }
  if (Functions.find(function.Entry) != Functions.end()) {
    return false;
  }
  auto seedIterator = FunctionSeeds.find(function.Entry);
  if (seedIterator != FunctionSeeds.end()) {
    function.IsExternallyVisible =
        function.IsExternallyVisible || seedIterator->second.IsExternallyVisible;
  }

  for (const NativeBasicBlock &block : function.Blocks) {
    if (block.Start < function.RangeStart || block.End > function.RangeEnd ||
        block.Start >= block.End) {
      Notes.push_back("confirmed function has invalid block range at " +
                      hexAddress(function.Entry));
      return false;
    }
  }

  Functions.emplace(function.Entry, std::move(function));
  return true;
}

bool NativeProgramState::markFunctionSeedExternallyVisible(uint64_t address) {
  auto iterator = FunctionSeeds.find(address);
  if (iterator == FunctionSeeds.end()) {
    return false;
  }
  iterator->second.IsExternallyVisible = true;
  return true;
}

bool NativeProgramState::removeFunction(uint64_t entry) {
  return Functions.erase(entry) != 0;
}

bool NativeProgramState::demoteFunctionSeedToRangeHint(uint64_t address) {
  auto iterator = FunctionSeeds.find(address);
  if (iterator == FunctionSeeds.end()) {
    return false;
  }
  iterator->second.IsEntry = false;
  return true;
}

bool isNativeRuntimeFunctionName(const std::string &name) {
  static const std::set<std::string> names = {
      "_start",
      "_init",
      "_fini",
      "_DT_INIT",
      "_DT_FINI",
      "_INIT_0",
      "_FINI_0",
      "deregister_tm_clones",
      "register_tm_clones",
      "__do_global_dtors_aux",
      "frame_dummy",
      "_dl_relocate_static_pie",
  };
  return names.find(name) != names.end();
}

bool isNativeRuntimeSectionName(const std::string &name) {
  return name == ".plt" || name == ".plt.got" || name == ".plt.sec" ||
         name == ".init" || name == ".fini";
}

bool isNativeRuntimeAddress(const NativeProgramState &state,
                            uint64_t address) {
  for (const NativeSectionInfo &section : state.sections()) {
    if (isNativeRuntimeSectionName(section.Name) &&
        containsAddress(section.Address, section.Size, address)) {
      return true;
    }
  }
  return isX86GlibcStartPattern(state, address);
}

bool isNativeRuntimeSeed(const NativeProgramState &state,
                         const NativeFunctionSeed &seed) {
  if (isNativeRuntimeFunctionName(seed.PrimaryName) ||
      isNativeRuntimeAddress(state, seed.Address) ||
      hasSource(seed, "elf-entry") || hasSource(seed, "dt-init") ||
      hasSource(seed, "dt-fini") || hasSource(seed, "dt-init-array") ||
      hasSource(seed, "dt-preinit-array") || hasSource(seed, "dt-fini-array")) {
    return true;
  }
  for (const std::string &alias : seed.Aliases) {
    if (isNativeRuntimeFunctionName(alias)) {
      return true;
    }
  }
  return false;
}

bool isNativeRuntimeFunction(const NativeProgramState &state,
                             const NativeFunction &function) {
  if (isNativeRuntimeFunctionName(function.Name) ||
      isNativeRuntimeAddress(state, function.Entry)) {
    return true;
  }
  auto seed = state.functionSeeds().find(function.Entry);
  return seed != state.functionSeeds().end() &&
         isNativeRuntimeSeed(state, seed->second);
}

bool NativeProgramState::addBasicBlock(uint64_t functionEntry,
                                       NativeBasicBlock block) {
  auto iterator = Functions.find(functionEntry);
  if (iterator == Functions.end() || block.Start >= block.End) {
    return false;
  }

  // Splitting an already discovered range is only a fallthrough edge when the
  // decoded instruction facts say so.  Empty successor lists also represent
  // returns and indirect tail exits, so do not invent a successor just because
  // another block starts inside the old byte range.
  auto hasInstructionFallthroughTo = [&](uint64_t start,
                                         uint64_t target) -> bool {
    for (auto instruction = Instructions.lower_bound(start);
         instruction != Instructions.end() && instruction->first < target;
         ++instruction) {
      if (isInstructionFallthroughTo(instruction->second, target)) {
        return true;
      }
    }
    return false;
  };

  NativeFunction &function = iterator->second;
  for (NativeBasicBlock &existing : function.Blocks) {
    if (existing.Start != block.Start) {
      continue;
    }
    if (existing.End == block.End) {
      bool changed = false;
      for (uint64_t successor : block.Successors) {
        if (std::find(existing.Successors.begin(), existing.Successors.end(),
                      successor) == existing.Successors.end()) {
          existing.Successors.push_back(successor);
          changed = true;
        }
      }
      return changed;
    }
    if (block.End < existing.End) {
      existing.End = block.End;
      existing.Successors = std::move(block.Successors);
      return true;
    }
    return false;
  }

  for (NativeBasicBlock &existing : function.Blocks) {
    if (existing.Start < block.Start && block.Start < existing.End) {
      bool hadSuccessorToSplit =
          std::find(existing.Successors.begin(), existing.Successors.end(),
                    block.Start) != existing.Successors.end();
      existing.End = block.Start;
      existing.Successors.clear();
      if (hadSuccessorToSplit ||
          hasInstructionFallthroughTo(existing.Start, block.Start)) {
        existing.Successors.push_back(block.Start);
      }
    }
    if (block.Start < existing.Start && existing.Start < block.End) {
      bool hadSuccessorToSplit =
          std::find(block.Successors.begin(), block.Successors.end(),
                    existing.Start) != block.Successors.end();
      block.End = existing.Start;
      block.Successors.clear();
      if (hadSuccessorToSplit ||
          hasInstructionFallthroughTo(block.Start, existing.Start)) {
        block.Successors.push_back(existing.Start);
      }
    }
  }
  if (block.Start >= block.End) {
    return false;
  }
  function.RangeStart = std::min(function.RangeStart, block.Start);
  function.RangeEnd = std::max(function.RangeEnd, block.End);
  function.Blocks.push_back(std::move(block));
  return true;
}

bool NativeProgramState::addBasicBlockSuccessors(
    uint64_t functionEntry, uint64_t blockStart,
    const std::vector<uint64_t> &successors) {
  auto iterator = Functions.find(functionEntry);
  if (iterator == Functions.end()) {
    return false;
  }

  const NativeFunction &function = iterator->second;
  auto hasBlockStart = [&](uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (block.Start == address) {
        return true;
      }
    }
    return false;
  };
  if (!successors.empty()) {
    for (uint64_t successor : successors) {
      if (!hasBlockStart(successor)) {
        return false;
      }
    }
  }

  for (NativeBasicBlock &block : iterator->second.Blocks) {
    if (block.Start != blockStart) {
      continue;
    }
    bool changed = false;
    for (uint64_t successor : successors) {
      if (std::find(block.Successors.begin(), block.Successors.end(),
                    successor) == block.Successors.end()) {
        block.Successors.push_back(successor);
        changed = true;
      }
    }
    return changed;
  }
  return false;
}

bool NativeProgramState::removeInvalidBasicBlockSuccessors(
    uint64_t functionEntry) {
  auto iterator = Functions.find(functionEntry);
  if (iterator == Functions.end()) {
    return false;
  }

  NativeFunction &function = iterator->second;
  std::set<uint64_t> blockStarts;
  for (const NativeBasicBlock &block : function.Blocks) {
    blockStarts.insert(block.Start);
  }

  bool changed = false;
  for (NativeBasicBlock &block : function.Blocks) {
    size_t oldSize = block.Successors.size();
    block.Successors.erase(
        std::remove_if(block.Successors.begin(), block.Successors.end(),
                       [&](uint64_t successor) {
                         return blockStarts.count(successor) == 0;
                       }),
        block.Successors.end());
    changed |= block.Successors.size() != oldSize;
  }
  return changed;
}

bool NativeProgramState::addInstructionDirectFlowTargets(
    uint64_t address, const std::vector<uint64_t> &targets) {
  auto iterator = Instructions.find(address);
  if (iterator == Instructions.end()) {
    return false;
  }

  bool changed = false;
  NativeInstruction &instruction = iterator->second;
  for (uint64_t target : targets) {
    if (std::find(instruction.DirectFlowTargets.begin(),
                  instruction.DirectFlowTargets.end(),
                  target) == instruction.DirectFlowTargets.end()) {
      instruction.DirectFlowTargets.push_back(target);
      changed = true;
    }
  }
  return changed;
}

bool NativeProgramState::markInstructionTailFlowTarget(uint64_t address,
                                                       uint64_t target) {
  auto iterator = Instructions.find(address);
  if (iterator == Instructions.end()) {
    return false;
  }

  NativeInstruction &instruction = iterator->second;
  auto directTarget = std::find(instruction.DirectFlowTargets.begin(),
                                instruction.DirectFlowTargets.end(), target);
  if (directTarget == instruction.DirectFlowTargets.end()) {
    return false;
  }
  instruction.DirectFlowTargets.erase(directTarget);
  if (std::find(instruction.TailFlowTargets.begin(),
                instruction.TailFlowTargets.end(),
                target) == instruction.TailFlowTargets.end()) {
    instruction.TailFlowTargets.push_back(target);
  }
  return true;
}

bool NativeProgramState::restoreInstructionTailFlowTarget(uint64_t address,
                                                          uint64_t target) {
  auto iterator = Instructions.find(address);
  if (iterator == Instructions.end()) {
    return false;
  }

  NativeInstruction &instruction = iterator->second;
  auto tailTarget = std::find(instruction.TailFlowTargets.begin(),
                              instruction.TailFlowTargets.end(), target);
  if (tailTarget == instruction.TailFlowTargets.end()) {
    return false;
  }

  instruction.TailFlowTargets.erase(tailTarget);
  if (std::find(instruction.DirectFlowTargets.begin(),
                instruction.DirectFlowTargets.end(),
                target) == instruction.DirectFlowTargets.end()) {
    instruction.DirectFlowTargets.push_back(target);
  }
  return true;
}

void NativeProgramState::addXref(NativeXref xref) {
  if (xref.From == 0 || xref.To == 0) {
    return;
  }

  auto fromIterator = XrefsByFrom.find(xref.From);
  if (fromIterator != XrefsByFrom.end()) {
    for (size_t index : fromIterator->second) {
      const NativeXref &existing = Xrefs[index];
      if (existing.To == xref.To && existing.Kind == xref.Kind &&
          existing.Source == xref.Source) {
        return;
      }
    }
  }

  size_t index = Xrefs.size();
  Xrefs.push_back(std::move(xref));
  XrefsByFrom[Xrefs[index].From].push_back(index);
  XrefsByTo[Xrefs[index].To].push_back(index);
}

bool NativeProgramState::addUnresolvedFlow(NativeUnresolvedFlow flow) {
  if (flow.Address == 0) {
    return false;
  }
  for (const NativeUnresolvedFlow &existing : UnresolvedFlows) {
    if (existing.Address == flow.Address && existing.Kind == flow.Kind) {
      return false;
    }
  }
  UnresolvedFlows.push_back(std::move(flow));
  return true;
}

bool NativeProgramState::removeUnresolvedFlow(uint64_t address,
                                              NativeUnresolvedFlowKind kind) {
  size_t oldSize = UnresolvedFlows.size();
  UnresolvedFlows.erase(
      std::remove_if(UnresolvedFlows.begin(), UnresolvedFlows.end(),
                     [&](const NativeUnresolvedFlow &flow) {
                       return flow.Address == address && flow.Kind == kind;
                     }),
      UnresolvedFlows.end());
  return UnresolvedFlows.size() != oldSize;
}

bool NativeProgramState::addInstruction(NativeInstruction instruction) {
  if (instruction.Address == 0 || instruction.Size == 0) {
    return false;
  }
  if (instruction.Address >
      std::numeric_limits<uint64_t>::max() - instruction.Size) {
    return false;
  }
  if (!executableRangeContains(*this, instruction.Address, instruction.end())) {
    Notes.push_back("instruction outside executable range at " +
                    hexAddress(instruction.Address));
    return false;
  }
  if (!instruction.Bytes.empty() && instruction.Bytes.size() != instruction.Size) {
    Notes.push_back("instruction byte length mismatch at " +
                    hexAddress(instruction.Address));
    return false;
  }
  return Instructions.emplace(instruction.Address, std::move(instruction)).second;
}

void NativeProgramState::addFunctionRange(uint64_t address, uint64_t start,
                                          uint64_t end, std::string source) {
  auto iterator = FunctionSeeds.find(address);
  if (iterator == FunctionSeeds.end() || start >= end) {
    return;
  }

  NativeFunctionSeed &seed = iterator->second;
  if (seed.RangeSource.empty()) {
    seed.RangeStart = start;
    seed.RangeEnd = end;
    seed.RangeSource = std::move(source);
    return;
  }
  if (seed.RangeStart == start && seed.RangeEnd == end) {
    if (source == "eh-frame") {
      seed.RangeSource = std::move(source);
    }
    return;
  }

  Notes.push_back("function seed range mismatch at " + hexAddress(address) +
                  ": existing " + seed.RangeSource + " [" +
                  hexAddress(seed.RangeStart) + ", " +
                  hexAddress(seed.RangeEnd) + "), new " + source + " [" +
                  hexAddress(start) + ", " + hexAddress(end) + ")");
}

void NativeProgramState::setControlFlowAuthority(
    NativeControlFlowAuthority authority) {
  if (authority == NativeControlFlowAuthority::Unknown ||
      ControlFlowAuthority == authority) {
    return;
  }
  if (ControlFlowAuthority != NativeControlFlowAuthority::Unknown) {
    Notes.push_back("native control-flow authority already set");
    return;
  }
  ControlFlowAuthority = authority;
}

void NativeProgramState::addRelocation(NativeRelocationInfo relocation) {
  Relocations.push_back(std::move(relocation));
}

void NativeProgramState::addRelocatedPointer(uint64_t address, uint64_t value) {
  RelocatedPointers[address] = value;
}

void NativeProgramState::addPltEntry(NativePltEntry entry) {
  PltEntries.push_back(std::move(entry));
}

void NativeProgramState::addNote(std::string note) {
  Notes.push_back(std::move(note));
}

void NativeAnalysisManager::addAnalyzer(
    std::unique_ptr<NativeAnalyzer> analyzer) {
  Analyzers.push_back(std::move(analyzer));
}

void NativeAnalysisManager::run(NativeProgramState &state) {
  std::stable_sort(Analyzers.begin(), Analyzers.end(),
                   [](const std::unique_ptr<NativeAnalyzer> &lhs,
                      const std::unique_ptr<NativeAnalyzer> &rhs) {
                     return lhs->priority() < rhs->priority();
                   });
  for (std::unique_ptr<NativeAnalyzer> &analyzer : Analyzers) {
    analyzer->run(state, *this);
  }
}

} // namespace notdec::bin2llvm
