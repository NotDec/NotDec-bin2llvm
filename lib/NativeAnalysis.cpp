#include "notdec-bin2llvm/NativeAnalysis.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/DynamicEntry.hpp>
#include <LIEF/ELF/Relocation.hpp>
#include <LIEF/ELF/Section.hpp>
#include <LIEF/ELF/Segment.hpp>
#include <LIEF/ELF/Symbol.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
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

bool readBytes(const NativeProgramState &state, uint64_t address, uint64_t size,
               std::vector<uint8_t> &bytes) {
  for (const NativeMemoryRange &range : state.memoryRanges()) {
    if (range.Start > address) {
      break;
    }
    if (!range.Readable || !containsAddress(range.Start, range.Size, address)) {
      continue;
    }
    uint64_t offset = address - range.Start;
    if (size > range.Bytes.size() || offset > range.Bytes.size() - size) {
      return false;
    }
    bytes.assign(range.Bytes.begin() + offset,
                 range.Bytes.begin() + offset + size);
    return true;
  }
  return false;
}

bool executableRangeContains(const NativeProgramState &state, uint64_t start,
                             uint64_t end) {
  if (start >= end) {
    return false;
  }
  for (const NativeMemoryRange &range : state.memoryRanges()) {
    if (!range.Executable) {
      continue;
    }
    if (start >= range.Start && end - range.Start <= range.Size) {
      return true;
    }
  }
  return false;
}

void addUnsupportedEhFrameSample(NativeEhFrameStats &stats,
                                 const std::string &sample) {
  ++stats.UnsupportedCount;
  if (stats.UnsupportedSamples.size() < 8) {
    stats.UnsupportedSamples.push_back(sample);
  }
}

void addEhFrameHdrUnsupportedSample(NativeEhFrameStats &stats,
                                    const std::string &sample) {
  ++stats.HdrUnsupportedCount;
  if (stats.UnsupportedSamples.size() < 8) {
    stats.UnsupportedSamples.push_back(".eh_frame_hdr " + sample);
  }
}

void addEhFrameHdrMismatchSample(NativeEhFrameStats &stats,
                                 const std::string &sample) {
  if (stats.HdrMismatchSamples.size() < 8) {
    stats.HdrMismatchSamples.push_back(sample);
  }
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

class EhFrameAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "EhFrameAnalyzer"; }
  int priority() const override { return 50; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    NativeEhFrameStats &stats = state.ehFrameStats();
    std::optional<NativeSectionInfo> hdrSection =
        findSection(state, ".eh_frame_hdr");
    stats.HasEhFrameHdr = hdrSection.has_value();

    std::optional<NativeSectionInfo> section = findSection(state, ".eh_frame");
    if (!section) {
      return;
    }
    stats.HasEhFrame = true;

    std::vector<uint8_t> bytes;
    if (!readBytes(state, section->Address, section->Size, bytes)) {
      ++stats.InvalidCount;
      state.addNote(".eh_frame section is not readable");
      return;
    }

    EhFrameReader reader(state, *section, bytes);
    reader.parse();

    if (hdrSection) {
      std::vector<uint8_t> hdrBytes;
      if (!readBytes(state, hdrSection->Address, hdrSection->Size, hdrBytes)) {
        ++stats.HdrInvalidCount;
        state.addNote(".eh_frame_hdr section is not readable");
      } else {
        EhFrameHdrReader hdrReader(state, *hdrSection, hdrBytes);
        hdrReader.parse();
        if (stats.ParsedEhFrameHdr) {
          compareEhFrameHdr(state);
        }
      }
    }
  }

private:
  static constexpr uint8_t DW_EH_PE_absptr = 0x00;
  static constexpr uint8_t DW_EH_PE_udata4 = 0x03;
  static constexpr uint8_t DW_EH_PE_udata8 = 0x04;
  static constexpr uint8_t DW_EH_PE_sdata4 = 0x0b;
  static constexpr uint8_t DW_EH_PE_sdata8 = 0x0c;
  static constexpr uint8_t DW_EH_PE_pcrel = 0x10;
  static constexpr uint8_t DW_EH_PE_datarel = 0x30;
  static constexpr uint8_t DW_EH_PE_omit = 0xff;

  struct CieInfo {
    uint64_t Address = 0;
    uint8_t FdeEncoding = DW_EH_PE_absptr;
  };

  class EhFrameReader {
  public:
    EhFrameReader(NativeProgramState &state, const NativeSectionInfo &section,
                  const std::vector<uint8_t> &bytes)
        : State(state), Section(section), Bytes(bytes) {}

    void parse() {
      size_t offset = 0;
      while (offset + 4 <= Bytes.size()) {
        size_t recordOffset = offset;
        uint32_t length = 0;
        if (!readU32(offset, length)) {
          ++stats().InvalidCount;
          return;
        }
        if (length == 0) {
          return;
        }
        if (length == 0xffffffffu) {
          addUnsupportedEhFrameSample(stats(),
                                      ".eh_frame 64-bit record length");
          return;
        }
        if (length > Bytes.size() || offset > Bytes.size() - length) {
          ++stats().InvalidCount;
          State.addNote(".eh_frame record extends past section end at " +
                        hexAddress(recordAddress(recordOffset)));
          return;
        }

        size_t contentOffset = offset;
        size_t recordEnd = offset + length;
        uint32_t ciePointer = 0;
        if (!readU32(offset, ciePointer)) {
          ++stats().InvalidCount;
          return;
        }
        if (ciePointer == 0) {
          parseCie(recordOffset, contentOffset + 4, recordEnd);
        } else {
          parseFde(recordOffset, contentOffset, ciePointer, offset, recordEnd);
        }
        offset = recordEnd;
      }
    }

  private:
    NativeEhFrameStats &stats() { return State.ehFrameStats(); }

    uint64_t recordAddress(size_t offset) const {
      return Section.Address + static_cast<uint64_t>(offset);
    }

    bool readU8(size_t &offset, uint8_t &value) const {
      if (offset >= Bytes.size()) {
        return false;
      }
      value = Bytes[offset++];
      return true;
    }

    bool readUnsigned(size_t &offset, uint8_t size, uint64_t &value) const {
      if (size > 8 || size > Bytes.size() || offset > Bytes.size() - size) {
        return false;
      }
      value = 0;
      for (uint8_t index = 0; index < size; ++index) {
        value |= static_cast<uint64_t>(Bytes[offset + index]) << (index * 8);
      }
      offset += size;
      return true;
    }

    bool readSigned(size_t &offset, uint8_t size, int64_t &value) const {
      uint64_t raw = 0;
      if (!readUnsigned(offset, size, raw)) {
        return false;
      }
      if (size == 8) {
        value = static_cast<int64_t>(raw);
        return true;
      }
      uint64_t signBit = uint64_t{1} << (size * 8 - 1);
      if ((raw & signBit) != 0) {
        raw |= (~uint64_t{0}) << (size * 8);
      }
      value = static_cast<int64_t>(raw);
      return true;
    }

    bool readU32(size_t &offset, uint32_t &value) const {
      uint64_t raw = 0;
      if (!readUnsigned(offset, 4, raw)) {
        return false;
      }
      value = static_cast<uint32_t>(raw);
      return true;
    }

    bool readString(size_t &offset, size_t end, std::string &value) const {
      value.clear();
      while (offset < end && Bytes[offset] != 0) {
        value.push_back(static_cast<char>(Bytes[offset++]));
      }
      if (offset >= end) {
        return false;
      }
      ++offset;
      return true;
    }

    bool readUleb128(size_t &offset, size_t end, uint64_t &value) const {
      value = 0;
      unsigned shift = 0;
      while (offset < end) {
        uint8_t byte = Bytes[offset++];
        if (shift >= 64 && (byte & 0x7f) != 0) {
          return false;
        }
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
          return true;
        }
        shift += 7;
      }
      return false;
    }

    bool readSleb128(size_t &offset, size_t end, int64_t &value) const {
      value = 0;
      unsigned shift = 0;
      uint8_t byte = 0;
      do {
        if (offset >= end || shift >= 64) {
          return false;
        }
        byte = Bytes[offset++];
        value |= static_cast<int64_t>(byte & 0x7f) << shift;
        shift += 7;
      } while ((byte & 0x80) != 0);
      if (shift < 64 && (byte & 0x40) != 0) {
        value |= -static_cast<int64_t>(uint64_t{1} << shift);
      }
      return true;
    }

    std::string encodingName(uint8_t encoding) const {
      std::ostringstream stream;
      stream << "0x" << std::hex << static_cast<unsigned>(encoding);
      return stream.str();
    }

    bool encodedSize(uint8_t encoding, uint8_t &size) const {
      if (encoding == DW_EH_PE_omit) {
        size = 0;
        return true;
      }
      switch (encoding & 0x0f) {
      case DW_EH_PE_absptr:
        size = State.pointerSize();
        return size == 4 || size == 8;
      case DW_EH_PE_udata4:
      case DW_EH_PE_sdata4:
        size = 4;
        return true;
      case DW_EH_PE_udata8:
      case DW_EH_PE_sdata8:
        size = 8;
        return true;
      default:
        return false;
      }
    }

    bool skipEncoded(size_t &offset, size_t end, uint8_t encoding) {
      uint8_t size = 0;
      if (!encodedSize(encoding, size) || offset > end || size > end - offset) {
        addUnsupportedEhFrameSample(stats(), "unsupported encoding " +
                                                 encodingName(encoding));
        return false;
      }
      offset += size;
      return true;
    }

    bool decodeEncodedAddress(size_t &offset, size_t end, uint8_t encoding,
                              uint64_t &value) {
      if (encoding == DW_EH_PE_omit) {
        return false;
      }
      uint8_t size = 0;
      if (!encodedSize(encoding, size) || offset > end || size > end - offset) {
        addUnsupportedEhFrameSample(stats(), "unsupported encoding " +
                                                 encodingName(encoding));
        return false;
      }

      uint64_t fieldAddress = recordAddress(offset);
      uint64_t rawUnsigned = 0;
      int64_t rawSigned = 0;
      switch (encoding & 0x0f) {
      case DW_EH_PE_absptr:
      case DW_EH_PE_udata4:
      case DW_EH_PE_udata8:
        if (!readUnsigned(offset, size, rawUnsigned)) {
          return false;
        }
        value = rawUnsigned;
        break;
      case DW_EH_PE_sdata4:
      case DW_EH_PE_sdata8:
        if (!readSigned(offset, size, rawSigned)) {
          return false;
        }
        value = static_cast<uint64_t>(rawSigned);
        break;
      default:
        return false;
      }

      if ((encoding & 0x70) == DW_EH_PE_pcrel) {
        int64_t delta =
            (encoding & 0x08) != 0 ? rawSigned : static_cast<int64_t>(value);
        if (delta < 0 &&
            fieldAddress < static_cast<uint64_t>(-delta)) {
          return false;
        }
        if (delta > 0 &&
            fieldAddress >
                std::numeric_limits<uint64_t>::max() -
                    static_cast<uint64_t>(delta)) {
          return false;
        }
        value = delta < 0 ? fieldAddress - static_cast<uint64_t>(-delta)
                          : fieldAddress + static_cast<uint64_t>(delta);
      } else if ((encoding & 0x70) == DW_EH_PE_datarel) {
        int64_t delta =
            (encoding & 0x08) != 0 ? rawSigned : static_cast<int64_t>(value);
        if (delta < 0 && Section.Address < static_cast<uint64_t>(-delta)) {
          return false;
        }
        if (delta > 0 &&
            Section.Address >
                std::numeric_limits<uint64_t>::max() -
                    static_cast<uint64_t>(delta)) {
          return false;
        }
        value = delta < 0 ? Section.Address - static_cast<uint64_t>(-delta)
                          : Section.Address + static_cast<uint64_t>(delta);
      } else if ((encoding & 0x70) != 0) {
        addUnsupportedEhFrameSample(stats(), "unsupported encoding " +
                                                 encodingName(encoding));
        return false;
      }
      return true;
    }

    bool decodeEncodedRange(size_t &offset, size_t end, uint8_t encoding,
                            uint64_t &value) {
      uint8_t size = 0;
      if (!encodedSize(encoding, size) || offset > end || size > end - offset) {
        addUnsupportedEhFrameSample(stats(), "unsupported encoding " +
                                                 encodingName(encoding));
        return false;
      }
      uint64_t rawUnsigned = 0;
      int64_t rawSigned = 0;
      switch (encoding & 0x0f) {
      case DW_EH_PE_absptr:
      case DW_EH_PE_udata4:
      case DW_EH_PE_udata8:
        if (!readUnsigned(offset, size, rawUnsigned)) {
          return false;
        }
        value = rawUnsigned;
        return true;
      case DW_EH_PE_sdata4:
      case DW_EH_PE_sdata8:
        if (!readSigned(offset, size, rawSigned) || rawSigned < 0) {
          return false;
        }
        value = static_cast<uint64_t>(rawSigned);
        return true;
      default:
        return false;
      }
    }

    void parseCie(size_t recordOffset, size_t offset, size_t recordEnd) {
      ++stats().CieCount;
      uint8_t version = 0;
      std::string augmentation;
      uint64_t ignoredUleb = 0;
      int64_t ignoredSleb = 0;
      if (!readU8(offset, version) ||
          !readString(offset, recordEnd, augmentation) ||
          !readUleb128(offset, recordEnd, ignoredUleb) ||
          !readSleb128(offset, recordEnd, ignoredSleb)) {
        ++stats().InvalidCount;
        return;
      }
      if (version == 1) {
        uint8_t ignoredRegister = 0;
        if (!readU8(offset, ignoredRegister)) {
          ++stats().InvalidCount;
          return;
        }
      } else {
        if (!readUleb128(offset, recordEnd, ignoredUleb)) {
          ++stats().InvalidCount;
          return;
        }
      }

      CieInfo cie;
      cie.Address = recordAddress(recordOffset);
      if (!augmentation.empty() && augmentation[0] == 'z') {
        uint64_t augmentationLength = 0;
        if (!readUleb128(offset, recordEnd, augmentationLength) ||
            augmentationLength > recordEnd ||
            offset > recordEnd - augmentationLength) {
          ++stats().InvalidCount;
          return;
        }
        size_t augmentationEnd = offset + static_cast<size_t>(augmentationLength);
        for (size_t index = 1; index < augmentation.size(); ++index) {
          switch (augmentation[index]) {
          case 'P': {
            uint8_t encoding = 0;
            if (!readU8(offset, encoding) ||
                !skipEncoded(offset, augmentationEnd, encoding)) {
              return;
            }
            break;
          }
          case 'L': {
            uint8_t ignoredEncoding = 0;
            if (!readU8(offset, ignoredEncoding)) {
              ++stats().InvalidCount;
              return;
            }
            break;
          }
          case 'R':
            if (!readU8(offset, cie.FdeEncoding)) {
              ++stats().InvalidCount;
              return;
            }
            break;
          default:
            addUnsupportedEhFrameSample(
                stats(), std::string(".eh_frame CIE augmentation ") +
                             augmentation[index]);
            return;
          }
        }
      }
      Cies[cie.Address] = cie;
    }

    void parseFde(size_t recordOffset, size_t ciePointerOffset,
                  uint32_t ciePointer, size_t offset, size_t recordEnd) {
      ++stats().FdeCount;
      uint64_t ciePointerAddress = recordAddress(ciePointerOffset);
      if (ciePointerAddress < ciePointer) {
        ++stats().InvalidCount;
        return;
      }
      uint64_t cieAddress = ciePointerAddress - ciePointer;
      auto cie = Cies.find(cieAddress);
      if (cie == Cies.end()) {
        ++stats().InvalidCount;
        State.addNote(".eh_frame FDE references unknown CIE at " +
                      hexAddress(cieAddress));
        return;
      }

      uint64_t pcBegin = 0;
      uint64_t pcRange = 0;
      if (!decodeEncodedAddress(offset, recordEnd, cie->second.FdeEncoding,
                                pcBegin) ||
          !decodeEncodedRange(offset, recordEnd, cie->second.FdeEncoding,
                              pcRange)) {
        ++stats().InvalidCount;
        return;
      }
      if (pcBegin == 0 || pcRange == 0) {
        ++stats().InvalidCount;
        return;
      }
      if (!State.isExecutableAddress(pcBegin)) {
        ++stats().InvalidCount;
        return;
      }

      bool inserted = State.addFunctionSeed(pcBegin, 0, "", "eh-frame",
                                            NativeFunctionConfidence::High);
      if (inserted) {
        ++stats().AddedSeedCount;
      } else {
        ++stats().OverlappedSeedCount;
      }

      if (pcBegin > std::numeric_limits<uint64_t>::max() - pcRange) {
        ++stats().InvalidCount;
        return;
      }
      uint64_t pcEnd = pcBegin + pcRange;
      if (!executableRangeContains(State, pcBegin, pcEnd)) {
        ++stats().InvalidCount;
        State.addNote(".eh_frame range crosses executable segment at " +
                      hexAddress(pcBegin) + " size " +
                      std::to_string(pcRange));
        return;
      }

      State.addFunctionRange(pcBegin, pcBegin, pcEnd, "eh-frame");
      stats().FrameFdes.push_back({pcBegin, recordAddress(recordOffset)});
      ++stats().ParsedFdeCount;
    }

    NativeProgramState &State;
    const NativeSectionInfo &Section;
    const std::vector<uint8_t> &Bytes;
    std::map<uint64_t, CieInfo> Cies;
  };

  class EhFrameHdrReader {
  public:
    EhFrameHdrReader(NativeProgramState &state,
                     const NativeSectionInfo &section,
                     const std::vector<uint8_t> &bytes)
        : State(state), Section(section), Bytes(bytes) {}

    void parse() {
      size_t offset = 0;
      uint8_t version = 0;
      uint8_t ehFramePtrEncoding = 0;
      uint8_t fdeCountEncoding = 0;
      uint8_t tableEncoding = 0;
      if (!readU8(offset, version) || !readU8(offset, ehFramePtrEncoding) ||
          !readU8(offset, fdeCountEncoding) || !readU8(offset, tableEncoding)) {
        ++stats().HdrInvalidCount;
        return;
      }
      if (version != 1) {
        ++stats().HdrInvalidCount;
        State.addNote(".eh_frame_hdr has unsupported version " +
                      std::to_string(version));
        return;
      }

      uint64_t ignoredEhFramePtr = 0;
      if (!decodeEncodedAddress(offset, ehFramePtrEncoding,
                                ignoredEhFramePtr)) {
        ++stats().HdrInvalidCount;
        return;
      }
      uint64_t fdeCount = 0;
      if (!decodeEncodedScalar(offset, fdeCountEncoding, fdeCount)) {
        ++stats().HdrInvalidCount;
        return;
      }
      stats().HdrFdeCount = fdeCount;

      if (tableEncoding == DW_EH_PE_omit) {
        addEhFrameHdrUnsupportedSample(stats(), "omitted FDE table");
        return;
      }
      for (uint64_t index = 0; index < fdeCount; ++index) {
        uint64_t initialLocation = 0;
        uint64_t fdeAddress = 0;
        if (!decodeEncodedAddress(offset, tableEncoding, initialLocation) ||
            !decodeEncodedAddress(offset, tableEncoding, fdeAddress)) {
          ++stats().HdrInvalidCount;
          return;
        }
        stats().HdrEntries.push_back({initialLocation, fdeAddress});
      }
      stats().HdrTableEntries = stats().HdrEntries.size();
      stats().ParsedEhFrameHdr = true;
    }

  private:
    NativeEhFrameStats &stats() { return State.ehFrameStats(); }

    uint64_t fieldAddress(size_t offset) const {
      return Section.Address + static_cast<uint64_t>(offset);
    }

    bool readU8(size_t &offset, uint8_t &value) const {
      if (offset >= Bytes.size()) {
        return false;
      }
      value = Bytes[offset++];
      return true;
    }

    bool readUnsigned(size_t &offset, uint8_t size, uint64_t &value) const {
      if (size > 8 || size > Bytes.size() || offset > Bytes.size() - size) {
        return false;
      }
      value = 0;
      for (uint8_t index = 0; index < size; ++index) {
        value |= static_cast<uint64_t>(Bytes[offset + index]) << (index * 8);
      }
      offset += size;
      return true;
    }

    bool readSigned(size_t &offset, uint8_t size, int64_t &value) const {
      uint64_t raw = 0;
      if (!readUnsigned(offset, size, raw)) {
        return false;
      }
      if (size == 8) {
        value = static_cast<int64_t>(raw);
        return true;
      }
      uint64_t signBit = uint64_t{1} << (size * 8 - 1);
      if ((raw & signBit) != 0) {
        raw |= (~uint64_t{0}) << (size * 8);
      }
      value = static_cast<int64_t>(raw);
      return true;
    }

    std::string encodingName(uint8_t encoding) const {
      std::ostringstream stream;
      stream << "0x" << std::hex << static_cast<unsigned>(encoding);
      return stream.str();
    }

    bool encodedSize(uint8_t encoding, uint8_t &size) {
      if (encoding == DW_EH_PE_omit) {
        size = 0;
        return true;
      }
      switch (encoding & 0x0f) {
      case DW_EH_PE_absptr:
        size = State.pointerSize();
        return size == 4 || size == 8;
      case DW_EH_PE_udata4:
      case DW_EH_PE_sdata4:
        size = 4;
        return true;
      case DW_EH_PE_udata8:
      case DW_EH_PE_sdata8:
        size = 8;
        return true;
      default:
        addEhFrameHdrUnsupportedSample(stats(), "unsupported encoding " +
                                                    encodingName(encoding));
        return false;
      }
    }

    bool readEncodedRaw(size_t &offset, uint8_t encoding, uint64_t &value,
                        int64_t &signedValue, uint64_t &address) {
      if (encoding == DW_EH_PE_omit) {
        return false;
      }
      uint8_t size = 0;
      if (!encodedSize(encoding, size) || offset > Bytes.size() ||
          size > Bytes.size() - offset) {
        return false;
      }

      address = fieldAddress(offset);
      value = 0;
      signedValue = 0;
      switch (encoding & 0x0f) {
      case DW_EH_PE_absptr:
      case DW_EH_PE_udata4:
      case DW_EH_PE_udata8:
        if (!readUnsigned(offset, size, value)) {
          return false;
        }
        signedValue = static_cast<int64_t>(value);
        return true;
      case DW_EH_PE_sdata4:
      case DW_EH_PE_sdata8:
        if (!readSigned(offset, size, signedValue)) {
          return false;
        }
        value = static_cast<uint64_t>(signedValue);
        return true;
      default:
        return false;
      }
    }

    bool applyRelative(uint8_t encoding, uint64_t rawValue,
                       int64_t signedValue, uint64_t fieldAddr,
                       uint64_t &value) {
      uint8_t relative = encoding & 0x70;
      if (relative == 0) {
        value = rawValue;
        return true;
      }
      if (relative != DW_EH_PE_pcrel && relative != DW_EH_PE_datarel) {
        addEhFrameHdrUnsupportedSample(stats(), "unsupported encoding " +
                                                    encodingName(encoding));
        return false;
      }

      uint64_t base = relative == DW_EH_PE_pcrel ? fieldAddr : Section.Address;
      int64_t delta =
          (encoding & 0x08) != 0 ? signedValue : static_cast<int64_t>(rawValue);
      if (delta < 0 && base < static_cast<uint64_t>(-delta)) {
        return false;
      }
      if (delta > 0 &&
          base > std::numeric_limits<uint64_t>::max() -
                     static_cast<uint64_t>(delta)) {
        return false;
      }
      value = delta < 0 ? base - static_cast<uint64_t>(-delta)
                        : base + static_cast<uint64_t>(delta);
      return true;
    }

    bool decodeEncodedAddress(size_t &offset, uint8_t encoding,
                              uint64_t &value) {
      uint64_t rawValue = 0;
      int64_t signedValue = 0;
      uint64_t address = 0;
      return readEncodedRaw(offset, encoding, rawValue, signedValue, address) &&
             applyRelative(encoding, rawValue, signedValue, address, value);
    }

    bool decodeEncodedScalar(size_t &offset, uint8_t encoding,
                             uint64_t &value) {
      uint64_t rawValue = 0;
      int64_t signedValue = 0;
      uint64_t address = 0;
      if (!readEncodedRaw(offset, encoding, rawValue, signedValue, address)) {
        return false;
      }
      if ((encoding & 0x08) != 0 && signedValue < 0) {
        return false;
      }
      value = (encoding & 0x08) != 0 ? static_cast<uint64_t>(signedValue)
                                     : rawValue;
      return true;
    }

    NativeProgramState &State;
    const NativeSectionInfo &Section;
    const std::vector<uint8_t> &Bytes;
  };

  static void compareEhFrameHdr(NativeProgramState &state) {
    NativeEhFrameStats &stats = state.ehFrameStats();
    std::map<uint64_t, uint64_t> fdeByStart;
    std::set<uint64_t> hdrStarts;
    for (const NativeEhFrameFdeInfo &fde : stats.FrameFdes) {
      fdeByStart[fde.PcBegin] = fde.FdeAddress;
    }

    for (const NativeEhFrameHdrEntry &entry : stats.HdrEntries) {
      hdrStarts.insert(entry.InitialLocation);
      auto frameFde = fdeByStart.find(entry.InitialLocation);
      if (frameFde == fdeByStart.end()) {
        ++stats.HdrMissingInFrame;
        addEhFrameHdrMismatchSample(
            stats, "hdr start missing in .eh_frame: " +
                       hexAddress(entry.InitialLocation));
        continue;
      }
      ++stats.HdrMatchedStarts;
      if (frameFde->second == entry.FdeAddress) {
        ++stats.HdrFdeAddressMatches;
      } else {
        ++stats.HdrFdeAddressMismatches;
        addEhFrameHdrMismatchSample(
            stats, "hdr FDE address mismatch for " +
                       hexAddress(entry.InitialLocation) + ": hdr " +
                       hexAddress(entry.FdeAddress) + ", frame " +
                       hexAddress(frameFde->second));
      }
    }

    for (const NativeEhFrameFdeInfo &fde : stats.FrameFdes) {
      if (hdrStarts.find(fde.PcBegin) == hdrStarts.end()) {
        ++stats.HdrExtraFrameFdes;
        addEhFrameHdrMismatchSample(
            stats, ".eh_frame FDE missing in hdr: " + hexAddress(fde.PcBegin));
      }
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

    const NativeEhFrameStats &ehFrame = state.ehFrameStats();
    Output << "  eh_frame:\n";
    Output << "    .eh_frame_hdr: " << (ehFrame.HasEhFrameHdr ? "yes" : "no")
           << '\n';
    Output << "    .eh_frame: " << (ehFrame.HasEhFrame ? "yes" : "no")
           << '\n';
    Output << "    parsed hdr: "
           << (ehFrame.ParsedEhFrameHdr ? "yes" : "no") << '\n';
    Output << "    CIE: " << ehFrame.CieCount << '\n';
    Output << "    FDE: " << ehFrame.FdeCount << '\n';
    Output << "    parsed FDE: " << ehFrame.ParsedFdeCount << '\n';
    Output << "    hdr FDE count: " << ehFrame.HdrFdeCount << '\n';
    Output << "    hdr table entries: " << ehFrame.HdrTableEntries << '\n';
    Output << "    hdr matched starts: " << ehFrame.HdrMatchedStarts << '\n';
    Output << "    hdr missing in frame: " << ehFrame.HdrMissingInFrame
           << '\n';
    Output << "    frame FDEs missing in hdr: " << ehFrame.HdrExtraFrameFdes
           << '\n';
    Output << "    hdr FDE address matches: "
           << ehFrame.HdrFdeAddressMatches << '\n';
    Output << "    hdr FDE address mismatches: "
           << ehFrame.HdrFdeAddressMismatches << '\n';
    Output << "    added seeds: " << ehFrame.AddedSeedCount << '\n';
    Output << "    overlapped seeds: " << ehFrame.OverlappedSeedCount << '\n';
    Output << "    invalid: " << ehFrame.InvalidCount << '\n';
    Output << "    unsupported: " << ehFrame.UnsupportedCount << '\n';
    Output << "    hdr invalid: " << ehFrame.HdrInvalidCount << '\n';
    Output << "    hdr unsupported: " << ehFrame.HdrUnsupportedCount << '\n';
    if (!ehFrame.HdrMismatchSamples.empty()) {
      Output << "    hdr mismatch samples:\n";
      for (const std::string &sample : ehFrame.HdrMismatchSamples) {
        Output << "      " << sample << '\n';
      }
    }
    if (!ehFrame.UnsupportedSamples.empty()) {
      Output << "    unsupported samples:\n";
      for (const std::string &sample : ehFrame.UnsupportedSamples) {
        Output << "      " << sample << '\n';
      }
    }

    std::map<std::string, uint64_t> rangeSourceCounts;
    for (const auto &[address, seed] : state.functionSeeds()) {
      (void)address;
      if (!seed.RangeSource.empty()) {
        ++rangeSourceCounts[seed.RangeSource];
      }
    }
    Output << "  range sources:\n";
    for (const auto &[source, count] : rangeSourceCounts) {
      Output << "    " << source << ": " << count << '\n';
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
      if (seed.RangeStart != 0 || seed.RangeEnd != 0) {
        Output << " range [" << hexAddress(seed.RangeStart) << ", "
               << hexAddress(seed.RangeEnd) << ") from " << seed.RangeSource;
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
    if (size != 0 && address <= std::numeric_limits<uint64_t>::max() - size) {
      seed.RangeStart = address;
      seed.RangeEnd = address + size;
      seed.RangeSource = source;
    }
  } else {
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
  }

  if (std::find(seed.Sources.begin(), seed.Sources.end(), source) ==
      seed.Sources.end()) {
    seed.Sources.push_back(std::move(source));
  }
  return inserted;
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

std::unique_ptr<NativeAnalyzer> createEhFrameAnalyzer() {
  return std::make_unique<EhFrameAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output) {
  return std::make_unique<ReportAnalyzer>(output);
}

} // namespace notdec::bin2llvm
