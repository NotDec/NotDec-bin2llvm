#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

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
    LIEF::ELF::ARCH arch = state.binary().header().machine_type();
    if (!supportsRelocationPltAnalysis(arch)) {
      state.addNote("relocation/PLT analysis currently supports x86-64 and "
                    "i386 ELF only; got " +
                    nativeElfArchitectureName(state.binary()));
      return;
    }

    std::vector<NativeRelocationInfo> jumpSlots;
    std::vector<NativeRelocationInfo> globDatFunctions;
    for (const LIEF::ELF::Relocation &relocation :
         state.binary().relocations()) {
      NativeRelocationInfo info;
      info.Address = relocation.address();
      info.Type = LIEF::ELF::Relocation::to_value(relocation.type());
      info.TypeName = relocationTypeName(relocation);
      info.TableKind = relocationPurposeName(relocation);
      info.Addend = relocation.addend();

      bool symbolIsFunction = false;
      if (const LIEF::ELF::Symbol *symbol = relocation.symbol()) {
        info.SymbolName = symbol->name();
        info.SymbolValue = symbol->value();
        symbolIsFunction = symbol->type() == LIEF::ELF::Symbol::TYPE::FUNC;
      }

      if (isRelativeRelocation(arch, relocation.type())) {
        std::optional<int64_t> addend =
            relocationAddendForPointerValue(state, relocation);
        if (addend) {
          info.Addend = *addend;
          info.ComputedValue = static_cast<uint64_t>(*addend);
          info.Status = "applied";
          state.addRelocatedPointer(info.Address, *info.ComputedValue);
        } else {
          info.Status = "unsupported";
          state.addNote("could not read implicit relocation addend at " +
                        hexAddress(info.Address));
        }
      } else if (isGlobDatRelocation(arch, relocation.type())) {
        if (info.SymbolValue != 0) {
          info.ComputedValue =
              info.SymbolValue + static_cast<uint64_t>(relocation.addend());
          state.addRelocatedPointer(info.Address, *info.ComputedValue);
          info.Status = "applied";
        } else {
          info.Status = "external";
        }
        if (symbolIsFunction && !info.SymbolName.empty()) {
          globDatFunctions.push_back(info);
        }
      } else if (isJumpSlotRelocation(arch, relocation.type())) {
        info.Status = "external";
        jumpSlots.push_back(info);
      } else if (isAbsoluteExternalFunctionPointerRelocation(
                     arch, relocation.type())) {
        if (symbolIsFunction && info.SymbolValue == 0 &&
            !info.SymbolName.empty()) {
          info.Status = "external";
        } else {
          info.Status = "unsupported";
        }
      } else if (isIRelativeRelocation(arch, relocation.type())) {
        std::optional<int64_t> addend =
            relocationAddendForPointerValue(state, relocation);
        if (addend) {
          info.Addend = *addend;
          info.ComputedValue = static_cast<uint64_t>(*addend);
          info.Status = "resolver";
        } else {
          info.Status = "unsupported";
          state.addNote("could not read implicit resolver addend at " +
                        hexAddress(info.Address));
        }
      } else {
        info.Status = "unsupported";
      }

      state.addRelocation(std::move(info));
    }

    if (arch == LIEF::ELF::ARCH::X86_64) {
      addPltGotEntries(state, std::move(globDatFunctions));
    }
    addPltEntries(state, jumpSlots);
    addRelocationPointerXrefs(state);
  }

private:
  void addRelocationPointerXrefs(NativeProgramState &state) {
    std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> seenXrefs;
    for (const auto &[address, target] : state.relocatedPointers()) {
      if (!isMappedAddress(state, target)) {
        continue;
      }
      NativeXrefKind kind = NativeXrefKind::Data;
      const char *source = "elf-relocation-pointer";
      if (state.isExecutableAddress(target)) {
        kind = NativeXrefKind::Flow;
        source = "elf-relocation-code";
      } else if (looksLikeReadOnlyCString(state, target)) {
        kind = NativeXrefKind::String;
        source = "elf-relocation-string";
      }
      NativeXref xref;
      xref.From = address;
      xref.To = target;
      xref.Kind = kind;
      xref.Source = source;
      if (!seenXrefs.insert({xref.From, xref.To, xref.Kind}).second) {
        continue;
      }
      state.addXref(std::move(xref));
    }
  }

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

  void addPltGotEntries(NativeProgramState &state,
                        std::vector<NativeRelocationInfo> globDatFunctions) {
    if (globDatFunctions.empty()) {
      return;
    }

    std::optional<NativeSectionInfo> pltGot = findSection(state, ".plt.got");
    if (!pltGot) {
      return;
    }
    if (pltGot->Size % 16 != 0) {
      state.addNote("could not match .plt.got stubs: unexpected section size");
      return;
    }

    std::sort(
        globDatFunctions.begin(), globDatFunctions.end(),
        [](const NativeRelocationInfo &lhs, const NativeRelocationInfo &rhs) {
          return lhs.Address < rhs.Address;
        });

    for (size_t index = 0; index < pltGot->Size / 16; ++index) {
      uint64_t stubAddress = pltGot->Address + index * 16;
      std::vector<uint8_t> stubBytes;
      if (!readBytes(state, stubAddress, 10, stubBytes) ||
          stubBytes.size() != 10) {
        continue;
      }
      if (stubBytes[0] != 0xf3 || stubBytes[1] != 0x0f ||
          stubBytes[2] != 0x1e || stubBytes[3] != 0xfa ||
          stubBytes[4] != 0xff || stubBytes[5] != 0x25) {
        continue;
      }

      uint32_t rawDisplacement = 0;
      for (size_t byteIndex = 0; byteIndex < 4; ++byteIndex) {
        rawDisplacement |= static_cast<uint32_t>(stubBytes[6 + byteIndex])
                           << (byteIndex * 8);
      }
      int32_t displacement = static_cast<int32_t>(rawDisplacement);

      uint64_t gotAddress =
          stubAddress + 10 + static_cast<int64_t>(displacement);
      auto relocationIt = std::find_if(
          globDatFunctions.begin(), globDatFunctions.end(),
          [&](const NativeRelocationInfo &relocation) {
            return relocation.Address == gotAddress;
          });
      if (relocationIt == globDatFunctions.end()) {
        continue;
      }

      NativePltEntry entry;
      entry.StubAddress = stubAddress;
      entry.GotAddress = gotAddress;
      entry.SymbolName = relocationIt->SymbolName;
      state.addPltEntry(std::move(entry));
    }
  }
};

class ElfEntryAnalyzer final : public NativeAnalyzer {
public:
  explicit ElfEntryAnalyzer(NativeRuntimeFilterOptions options)
      : Options(options) {}

  std::string name() const override { return "ElfEntryAnalyzer"; }
  int priority() const override { return 30; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (Options.SkipRuntimeFunctions) {
      state.addNote("runtime filter skipped ELF entry/init/fini seeds");
      return;
    }

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

private:
  NativeRuntimeFilterOptions Options;
};

class ElfSymbolAnalyzer final : public NativeAnalyzer {
public:
  explicit ElfSymbolAnalyzer(NativeRuntimeFilterOptions options)
      : Options(options) {}

  std::string name() const override { return "ElfSymbolAnalyzer"; }
  int priority() const override { return 40; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    std::set<std::pair<uint64_t, std::string>> seenSymbols;
    for (const LIEF::ELF::Symbol &symbol : state.binary().symbols()) {
      addSymbolSeed(state, symbol, "elf-symbol", Options, seenSymbols);
    }

    for (const LIEF::ELF::Symbol &symbol : state.binary().dynamic_symbols()) {
      addSymbolSeed(state, symbol, "elf-dynamic-symbol", Options, seenSymbols);
    }
  }

private:
  static void
  addSymbolSeed(NativeProgramState &state, const LIEF::ELF::Symbol &symbol,
                const std::string &source,
                const NativeRuntimeFilterOptions &options,
                std::set<std::pair<uint64_t, std::string>> &seenSymbols) {
    if (symbol.type() != LIEF::ELF::Symbol::TYPE::FUNC ||
        symbol.shndx() == LIEF::ELF::Symbol::SECTION_INDEX::UNDEF ||
        symbol.value() == 0) {
      return;
    }
    uint64_t address = symbol.value();
    if (!state.isExecutableAddress(address)) {
      return;
    }
    std::string name = symbol.name();
    if (options.SkipRuntimeFunctions &&
        (isNativeRuntimeFunctionName(name) ||
         isNativeRuntimeAddress(state, address))) {
      return;
    }
    if (!seenSymbols.insert({address, source + "\n" + name}).second) {
      return;
    }
    state.addFunctionSeed(address, symbol.size(), std::move(name), source,
                          NativeFunctionConfidence::High);
    if (symbol.is_exported()) {
      state.markFunctionSeedExternallyVisible(address);
    }
  }

  NativeRuntimeFilterOptions Options;
};

std::unique_ptr<NativeAnalyzer> createElfLoadAnalyzer() {
  return std::make_unique<ElfLoadAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createRelocationPltAnalyzer() {
  return std::make_unique<RelocationPltAnalyzer>();
}

std::unique_ptr<NativeAnalyzer>
createElfEntryAnalyzer(NativeRuntimeFilterOptions options) {
  return std::make_unique<ElfEntryAnalyzer>(options);
}

std::unique_ptr<NativeAnalyzer>
createElfSymbolAnalyzer(NativeRuntimeFilterOptions options) {
  return std::make_unique<ElfSymbolAnalyzer>(options);
}

} // namespace notdec::bin2llvm
