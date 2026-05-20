#include "notdec-bin2llvm/NativeAnalysis.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/DynamicEntry.hpp>
#include <LIEF/ELF/Relocation.hpp>
#include <LIEF/ELF/Section.hpp>
#include <LIEF/ELF/Segment.hpp>
#include <LIEF/ELF/Symbol.hpp>

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <set>
#include <sstream>
#include <utility>

namespace notdec::bin2llvm {
namespace {

bool containsAddress(uint64_t start, uint64_t size, uint64_t address) {
  return size != 0 && address >= start && address - start < size;
}

std::string hexAddress(uint64_t address) {
  std::ostringstream stream;
  stream << "0x" << std::hex << address;
  return stream.str();
}

NativeFunctionConfidence mergeConfidence(NativeFunctionConfidence lhs,
                                         NativeFunctionConfidence rhs) {
  return static_cast<int>(lhs) < static_cast<int>(rhs) ? lhs : rhs;
}

std::string relocationTypeName(const LIEF::ELF::Relocation &relocation) {
  const char *name = LIEF::ELF::to_string(relocation.type());
  if (name == nullptr) {
    return "unknown";
  }
  return name;
}

std::string relocationPurposeName(const LIEF::ELF::Relocation &relocation) {
  switch (relocation.purpose()) {
  case LIEF::ELF::Relocation::PURPOSE::PLTGOT:
    return "pltgot";
  case LIEF::ELF::Relocation::PURPOSE::DYNAMIC:
    return "dynamic";
  case LIEF::ELF::Relocation::PURPOSE::OBJECT:
    return "object";
  case LIEF::ELF::Relocation::PURPOSE::NONE:
    return "none";
  }
  return "unknown";
}

std::optional<NativeSectionInfo> findSection(const NativeProgramState &state,
                                             const std::string &name) {
  for (const NativeSectionInfo &section : state.sections()) {
    if (section.Name == name) {
      return section;
    }
  }
  return std::nullopt;
}

void addDynamicScalarEntry(NativeProgramState &state, uint64_t address,
                           const std::string &source) {
  if (address == 0) {
    return;
  }
  if (!state.isExecutableAddress(address)) {
    state.addNote(source +
                  " points outside executable memory: " + hexAddress(address));
    return;
  }
  state.addFunctionSeed(address, 0, "", source, NativeFunctionConfidence::High);
}

void addDynamicArrayEntries(NativeProgramState &state, uint64_t arrayAddress,
                            uint64_t arraySize, const std::string &source) {
  if (arrayAddress == 0 || arraySize == 0) {
    return;
  }

  uint8_t pointerSize = state.pointerSize();
  if (pointerSize == 0 || arraySize < pointerSize) {
    return;
  }

  for (uint64_t offset = 0; offset + pointerSize <= arraySize;
       offset += pointerSize) {
    uint64_t pointerAddress = arrayAddress + offset;
    std::optional<uint64_t> target = state.readPointer(pointerAddress);
    if (!target) {
      state.addNote(source + " pointer is unreadable at " +
                    hexAddress(pointerAddress));
      continue;
    }
    addDynamicScalarEntry(state, *target, source);
  }
}

class ElfLoadAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "ElfLoadAnalyzer"; }
  int priority() const override { return 10; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    uint64_t executableRanges = 0;
    for (const NativeMemoryRange &range : state.memoryRanges()) {
      if (range.Executable) {
        ++executableRanges;
      }
    }
    if (executableRanges == 0) {
      state.addNote("ELF has no executable PT_LOAD segment");
    }
  }
};

class RelocationPltAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "RelocationPltAnalyzer"; }
  int priority() const override { return 20; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (state.binary().header().machine_type() != LIEF::ELF::ARCH::X86_64) {
      state.addNote("relocation analysis currently supports x86-64 ELF only");
      return;
    }

    std::vector<NativeRelocationInfo> jumpSlots;
    for (const LIEF::ELF::Relocation &relocation :
         state.binary().relocations()) {
      NativeRelocationInfo info;
      info.Address = relocation.address();
      info.Type = LIEF::ELF::Relocation::to_value(relocation.type());
      info.TypeName = relocationTypeName(relocation);
      info.TableKind = relocationPurposeName(relocation);
      info.Addend = relocation.addend();

      if (const LIEF::ELF::Symbol *symbol = relocation.symbol()) {
        info.SymbolName = symbol->name();
        info.SymbolValue = symbol->value();
      }

      switch (relocation.type()) {
      case LIEF::ELF::Relocation::TYPE::X86_64_RELATIVE:
      case LIEF::ELF::Relocation::TYPE::X86_64_RELATIVE64:
        info.ComputedValue = static_cast<uint64_t>(relocation.addend());
        info.Status = "applied";
        state.addRelocatedPointer(info.Address, *info.ComputedValue);
        break;
      case LIEF::ELF::Relocation::TYPE::X86_64_GLOB_DAT:
        if (info.SymbolValue != 0) {
          info.ComputedValue =
              info.SymbolValue + static_cast<uint64_t>(relocation.addend());
          state.addRelocatedPointer(info.Address, *info.ComputedValue);
          info.Status = "applied";
        } else {
          info.Status = "external";
        }
        break;
      case LIEF::ELF::Relocation::TYPE::X86_64_JUMP_SLOT:
        info.Status = "external";
        jumpSlots.push_back(info);
        break;
      case LIEF::ELF::Relocation::TYPE::X86_64_IRELATIVE:
        info.ComputedValue = static_cast<uint64_t>(relocation.addend());
        info.Status = "resolver";
        break;
      default:
        info.Status = "unsupported";
        break;
      }

      state.addRelocation(std::move(info));
    }

    addPltEntries(state, jumpSlots);
  }

private:
  void addPltEntries(NativeProgramState &state,
                     std::vector<NativeRelocationInfo> jumpSlots) {
    if (jumpSlots.empty()) {
      return;
    }

    std::sort(
        jumpSlots.begin(), jumpSlots.end(),
        [](const NativeRelocationInfo &lhs, const NativeRelocationInfo &rhs) {
          return lhs.Address < rhs.Address;
        });

    std::optional<NativeSectionInfo> pltSec = findSection(state, ".plt.sec");
    std::optional<NativeSectionInfo> plt = findSection(state, ".plt");
    uint64_t stubStart = 0;
    bool usesLegacyPlt = false;

    if (pltSec && pltSec->Size / 16 >= jumpSlots.size()) {
      stubStart = pltSec->Address;
    } else if (plt && plt->Size >= 32 &&
               (plt->Size / 16) - 1 >= jumpSlots.size()) {
      stubStart = plt->Address + 16;
      usesLegacyPlt = true;
    } else {
      state.addNote("could not match PLT stubs to jump-slot relocations");
      return;
    }

    for (size_t index = 0; index < jumpSlots.size(); ++index) {
      NativePltEntry entry;
      entry.StubAddress = stubStart + index * 16;
      entry.GotAddress = jumpSlots[index].Address;
      entry.SymbolName = jumpSlots[index].SymbolName;
      state.addPltEntry(std::move(entry));
    }

    if (usesLegacyPlt) {
      state.addNote("PLT external mapping uses legacy .plt entries");
    }
  }
};

class ElfEntryAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "ElfEntryAnalyzer"; }
  int priority() const override { return 30; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    const LIEF::ELF::Binary &binary = state.binary();
    addDynamicScalarEntry(state, binary.entrypoint(), "elf-entry");

    uint64_t initArray = 0;
    uint64_t initArraySize = 0;
    uint64_t preinitArray = 0;
    uint64_t preinitArraySize = 0;
    uint64_t finiArray = 0;
    uint64_t finiArraySize = 0;

    for (const LIEF::ELF::DynamicEntry &entry : binary.dynamic_entries()) {
      switch (entry.tag()) {
      case LIEF::ELF::DynamicEntry::TAG::INIT:
        addDynamicScalarEntry(state, entry.value(), "dt-init");
        break;
      case LIEF::ELF::DynamicEntry::TAG::FINI:
        addDynamicScalarEntry(state, entry.value(), "dt-fini");
        break;
      case LIEF::ELF::DynamicEntry::TAG::INIT_ARRAY:
        initArray = entry.value();
        break;
      case LIEF::ELF::DynamicEntry::TAG::INIT_ARRAYSZ:
        initArraySize = entry.value();
        break;
      case LIEF::ELF::DynamicEntry::TAG::PREINIT_ARRAY:
        preinitArray = entry.value();
        break;
      case LIEF::ELF::DynamicEntry::TAG::PREINIT_ARRAYSZ:
        preinitArraySize = entry.value();
        break;
      case LIEF::ELF::DynamicEntry::TAG::FINI_ARRAY:
        finiArray = entry.value();
        break;
      case LIEF::ELF::DynamicEntry::TAG::FINI_ARRAYSZ:
        finiArraySize = entry.value();
        break;
      default:
        break;
      }
    }

    addDynamicArrayEntries(state, initArray, initArraySize, "dt-init-array");
    addDynamicArrayEntries(state, preinitArray, preinitArraySize,
                           "dt-preinit-array");
    addDynamicArrayEntries(state, finiArray, finiArraySize, "dt-fini-array");
  }
};

class ElfSymbolAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "ElfSymbolAnalyzer"; }
  int priority() const override { return 40; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    std::set<std::pair<uint64_t, std::string>> seenSymbols;
    for (const LIEF::ELF::Symbol &symbol : state.binary().symbols()) {
      if (symbol.type() != LIEF::ELF::Symbol::TYPE::FUNC ||
          symbol.shndx() == LIEF::ELF::Symbol::SECTION_INDEX::UNDEF ||
          symbol.value() == 0) {
        continue;
      }
      uint64_t address = symbol.value();
      if (!state.isExecutableAddress(address)) {
        continue;
      }
      std::string name = symbol.name();
      if (!seenSymbols.insert({address, name}).second) {
        continue;
      }
      state.addFunctionSeed(address, symbol.size(), std::move(name),
                            "elf-symbol", NativeFunctionConfidence::High);
    }
  }
};

class ReportAnalyzer final : public NativeAnalyzer {
public:
  explicit ReportAnalyzer(std::ostream &output) : Output(output) {}

  std::string name() const override { return "ReportAnalyzer"; }
  int priority() const override { return 1000; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    std::map<NativeFunctionConfidence, uint64_t> confidenceCounts;
    for (const auto &[address, seed] : state.functionSeeds()) {
      (void)address;
      ++confidenceCounts[seed.Confidence];
    }

    Output << "native discovery report\n";
    Output << "  pointer size: " << static_cast<unsigned>(state.pointerSize())
           << '\n';
    Output << "  memory ranges: " << state.memoryRanges().size() << '\n';
    Output << "  sections: " << state.sections().size() << '\n';
    Output << "  function seeds: " << state.functionSeeds().size() << '\n';

    Output << "  sources:\n";
    for (const auto &[source, count] : state.sourceCounts()) {
      Output << "    " << source << ": " << count << '\n';
    }

    Output << "  confidence:\n";
    for (NativeFunctionConfidence confidence :
         {NativeFunctionConfidence::High, NativeFunctionConfidence::Medium,
          NativeFunctionConfidence::Low}) {
      Output << "    " << toString(confidence) << ": "
             << confidenceCounts[confidence] << '\n';
    }

    Output << "  executable sections:\n";
    for (const NativeSectionInfo &section : state.sections()) {
      if (!section.Executable) {
        continue;
      }
      Output << "    " << section.Name << " " << hexAddress(section.Address)
             << " size " << section.Size << '\n';
    }

    Output << "  relocations:\n";
    Output << "    total: " << state.relocations().size() << '\n';
    Output << "    relocated pointers: " << state.relocatedPointers().size()
           << '\n';
    std::map<std::string, uint64_t> relocationStatusCounts;
    std::map<std::string, uint64_t> relocationTypeCounts;
    for (const NativeRelocationInfo &relocation : state.relocations()) {
      ++relocationStatusCounts[relocation.Status];
      ++relocationTypeCounts[relocation.TypeName];
    }
    Output << "    by status:\n";
    for (const auto &[status, count] : relocationStatusCounts) {
      Output << "      " << status << ": " << count << '\n';
    }
    Output << "    by type:\n";
    for (const auto &[typeName, count] : relocationTypeCounts) {
      Output << "      " << typeName << ": " << count << '\n';
    }

    Output << "  plt:\n";
    Output << "    external symbols: " << state.pltEntries().size() << '\n';
    size_t shownPltEntries = 0;
    for (const NativePltEntry &entry : state.pltEntries()) {
      if (shownPltEntries == 8) {
        break;
      }
      Output << "    " << hexAddress(entry.StubAddress) << " -> "
             << entry.SymbolName << " via GOT " << hexAddress(entry.GotAddress)
             << '\n';
      ++shownPltEntries;
    }

    if (!state.notes().empty()) {
      Output << "  notes:\n";
      for (const std::string &note : state.notes()) {
        Output << "    " << note << '\n';
      }
    }

    Output << "  seeds:\n";
    for (const auto &[address, seed] : state.functionSeeds()) {
      Output << "    " << hexAddress(address);
      if (!seed.PrimaryName.empty()) {
        Output << " " << seed.PrimaryName;
      }
      if (seed.Size != 0) {
        Output << " size " << seed.Size;
      }
      Output << " [" << toString(seed.Confidence) << "] sources:";
      for (const std::string &source : seed.Sources) {
        Output << " " << source;
      }
      if (!seed.Aliases.empty()) {
        Output << " aliases:";
        for (const std::string &alias : seed.Aliases) {
          Output << " " << alias;
        }
      }
      Output << '\n';
    }
  }

private:
  std::ostream &Output;
};

} // namespace

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
  } else {
    if (seed.Size == 0 && size != 0) {
      seed.Size = size;
    }
    if (!name.empty() && seed.PrimaryName.empty()) {
      seed.PrimaryName = std::move(name);
    } else if (!name.empty() && name != seed.PrimaryName &&
               std::find(seed.Aliases.begin(), seed.Aliases.end(), name) ==
                   seed.Aliases.end()) {
      seed.Aliases.push_back(std::move(name));
    }
    seed.Confidence = mergeConfidence(seed.Confidence, confidence);
  }

  if (std::find(seed.Sources.begin(), seed.Sources.end(), source) ==
      seed.Sources.end()) {
    seed.Sources.push_back(std::move(source));
  }
  return inserted;
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

std::unique_ptr<NativeAnalyzer> createElfLoadAnalyzer() {
  return std::make_unique<ElfLoadAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createRelocationPltAnalyzer() {
  return std::make_unique<RelocationPltAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createElfEntryAnalyzer() {
  return std::make_unique<ElfEntryAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createElfSymbolAnalyzer() {
  return std::make_unique<ElfSymbolAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output) {
  return std::make_unique<ReportAnalyzer>(output);
}

} // namespace notdec::bin2llvm
