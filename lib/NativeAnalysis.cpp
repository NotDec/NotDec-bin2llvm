#include "notdec-bin2llvm/NativeAnalysis.h"

#include "notdec-bin2llvm/LiefElfLoadImage.h"
#include "notdec-bin2llvm/SleighLift.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/DynamicEntry.hpp>
#include <LIEF/ELF/Relocation.hpp>
#include <LIEF/ELF/Section.hpp>
#include <LIEF/ELF/Segment.hpp>
#include <LIEF/ELF/Symbol.hpp>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <ostream>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace notdec::bin2llvm {
namespace {

#ifndef NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR
#define NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR "/sn640/ghidra"
#endif

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
      addSymbolSeed(state, symbol, "elf-symbol", seenSymbols);
    }

    for (const LIEF::ELF::Symbol &symbol : state.binary().dynamic_symbols()) {
      addSymbolSeed(state, symbol, "elf-dynamic-symbol", seenSymbols);
    }
  }

private:
  static void
  addSymbolSeed(NativeProgramState &state, const LIEF::ELF::Symbol &symbol,
                const std::string &source,
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
    if (!seenSymbols.insert({address, source + "\n" + name}).second) {
      return;
    }
    state.addFunctionSeed(address, symbol.size(), std::move(name), source,
                          NativeFunctionConfidence::High);
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

class SleighSeedInstructionAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "SleighSeedInstructionAnalyzer"; }
  int priority() const override { return 60; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (!resolveSpecOptions(state)) {
      return;
    }

    LiefElfLoadImage loadImage(state.binary(), NullErrors);
    if (!loadImage.hasExecutableBytes()) {
      state.addNote("sleigh instruction decode skipped: no executable bytes");
      return;
    }

    uint64_t decodedSeeds = 0;
    for (const NativeFunctionWorkItem &item : state.functionWorklist()) {
      if (decodedSeeds == MaxSeeds) {
        break;
      }
      if (!state.isExecutableAddress(item.Address)) {
        continue;
      }
      decodeSeed(state, loadImage, item.Address);
      ++decodedSeeds;
    }
  }

private:
  // This analyzer is only a bounded smoke path for recursive decode.  Larger
  // limits should come with CFG stop rules first, otherwise linear decode after
  // branches can look more precise than it is.
  static constexpr uint64_t MaxSeeds = 8;
  static constexpr uint64_t MaxInstructionsPerSeed = 8;
  static constexpr uint64_t MaxBytesPerSeed = 64;

  std::ostringstream NullErrors;
  SleighSpecOptions SpecOptions;

  bool resolveSpecOptions(NativeProgramState &state) {
    const LIEF::ELF::Binary &binary = state.binary();
    if (binary.header().machine_type() != LIEF::ELF::ARCH::X86_64) {
      state.addNote("sleigh instruction decode supports x86-64 ELF only");
      return false;
    }

    std::filesystem::path specRoot =
        std::filesystem::path(NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR) /
        "Ghidra/Processors/x86/data/languages";
    std::filesystem::path slaPath = specRoot / "x86-64.sla";
    std::filesystem::path pspecPath = specRoot / "x86-64.pspec";
    if (!std::filesystem::exists(slaPath)) {
      state.addNote("sleigh instruction decode missing spec: " +
                    slaPath.string());
      return false;
    }

    SpecOptions.SlaFileName = slaPath.string();
    if (std::filesystem::exists(pspecPath)) {
      SpecOptions.PspecFileName = pspecPath.string();
    }
    return true;
  }

  void decodeSeed(NativeProgramState &state, LiefElfLoadImage &loadImage,
                  uint64_t address) {
    std::optional<uint64_t> availableBytes = executableBytesFrom(state, address);
    if (!availableBytes || *availableBytes == 0) {
      return;
    }

    uint64_t decodeBytes = std::min(MaxBytesPerSeed, *availableBytes);
    SleighInstructionDecode decode = collectSleighInstructionDecode(
        loadImage, SpecOptions, address, MaxInstructionsPerSeed, decodeBytes,
        NullErrors);
    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    for (const SleighInstructionSummary &summary : decode.Instructions) {
      if (rangeStart == 0) {
        rangeStart = summary.Address;
      }
      rangeEnd = summary.Address + summary.Size;

      NativeInstruction instruction;
      instruction.Address = summary.Address;
      instruction.Size = summary.Size;
      instruction.Mnemonic = summary.Body.empty()
                                  ? summary.Mnemonic
                                  : summary.Mnemonic + " " + summary.Body;
      instruction.Source = "sleigh-seed-linear";
      (void)readBytes(state, summary.Address, summary.Size, instruction.Bytes);
      state.addInstruction(std::move(instruction));
    }
    std::vector<uint64_t> successors;
    if (rangeStart == address && rangeStart < rangeEnd) {
      addDirectControlFlow(state, decode.Pcode, successors);
    }
    addDecodedFunctionBlock(state, address, rangeStart, rangeEnd, successors);
  }

  static void addDecodedFunctionBlock(NativeProgramState &state,
                                      uint64_t entry, uint64_t rangeStart,
                                      uint64_t rangeEnd,
                                      std::vector<uint64_t> successors) {
    if (rangeStart == 0 || rangeStart != entry || rangeStart >= rangeEnd) {
      return;
    }

    NativeFunction function;
    function.Entry = entry;
    function.RangeStart = rangeStart;
    function.RangeEnd = rangeEnd;
    function.Source = "sleigh-seed-linear";
    auto seedIterator = state.functionSeeds().find(entry);
    if (seedIterator != state.functionSeeds().end()) {
      function.Name = seedIterator->second.PrimaryName;
    }

    NativeBasicBlock block;
    block.Start = rangeStart;
    block.End = rangeEnd;
    block.Successors = std::move(successors);
    function.Blocks.push_back(std::move(block));
    state.addFunction(std::move(function));
  }

  static void addDirectControlFlow(NativeProgramState &state,
                                   const PcodeProgram &program,
                                   std::vector<uint64_t> &successors) {
    std::set<std::pair<uint64_t, uint64_t>> seenFlowSuccessors;
    std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> seenXrefs;
    for (const PcodeOpView &op : program.Ops) {
      std::optional<uint64_t> target = directRamTarget(op);
      if (!target || !state.isExecutableAddress(*target)) {
        continue;
      }

      if (op.Opcode == PcodeOpcode::Call) {
        addUniqueXref(state, seenXrefs, op.Address, *target,
                      NativeXrefKind::Call);
      } else if (op.Opcode == PcodeOpcode::Branch ||
                 op.Opcode == PcodeOpcode::CBranch) {
        addUniqueXref(state, seenXrefs, op.Address, *target,
                      NativeXrefKind::Flow);
        if (seenFlowSuccessors.insert({op.Address, *target}).second &&
            std::find(successors.begin(), successors.end(), *target) ==
                successors.end()) {
          successors.push_back(*target);
        }
      }
    }
  }

  static std::optional<uint64_t> directRamTarget(const PcodeOpView &op) {
    if (op.Inputs.empty()) {
      return std::nullopt;
    }
    if (op.Opcode != PcodeOpcode::Call && op.Opcode != PcodeOpcode::Branch &&
        op.Opcode != PcodeOpcode::CBranch) {
      return std::nullopt;
    }
    const VarnodeView &target = op.Inputs.front();
    if (target.Space != "ram") {
      return std::nullopt;
    }
    return target.Offset;
  }

  static void addUniqueXref(
      NativeProgramState &state,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      uint64_t from, uint64_t to, NativeXrefKind kind) {
    if (!seenXrefs.insert({from, to, kind}).second) {
      return;
    }
    NativeXref xref;
    xref.From = from;
    xref.To = to;
    xref.Kind = kind;
    xref.Source = "sleigh-pcode-direct-flow";
    state.addXref(std::move(xref));
  }

  static std::optional<uint64_t>
  executableBytesFrom(const NativeProgramState &state, uint64_t address) {
    for (const NativeMemoryRange &range : state.memoryRanges()) {
      if (range.Start > address) {
        break;
      }
      if (!range.Executable ||
          !containsAddress(range.Start, range.Size, address)) {
        continue;
      }
      return range.Size - (address - range.Start);
    }
    return std::nullopt;
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
    Output << "  function worklist: " << state.functionWorklist().size()
           << '\n';

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

    Output << "  confirmed functions:\n";
    Output << "    total: " << state.functions().size() << '\n';
    uint64_t blockCount = 0;
    for (const auto &[entry, function] : state.functions()) {
      (void)entry;
      blockCount += function.Blocks.size();
    }
    Output << "    basic blocks: " << blockCount << '\n';
    Output << "  instructions:\n";
    Output << "    total: " << state.instructions().size() << '\n';

    std::map<NativeXrefKind, uint64_t> xrefKindCounts;
    for (const NativeXref &xref : state.xrefs()) {
      ++xrefKindCounts[xref.Kind];
    }
    Output << "  xrefs:\n";
    Output << "    total: " << state.xrefs().size() << '\n';
    for (NativeXrefKind kind : {NativeXrefKind::Flow, NativeXrefKind::Call,
                                NativeXrefKind::Data, NativeXrefKind::String}) {
      Output << "    " << toString(kind) << ": " << xrefKindCounts[kind]
             << '\n';
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
    if (function.RangeStart <= address && address < function.RangeEnd) {
      return &function;
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

bool NativeProgramState::addBasicBlock(uint64_t functionEntry,
                                       NativeBasicBlock block) {
  auto iterator = Functions.find(functionEntry);
  if (iterator == Functions.end() || block.Start >= block.End) {
    return false;
  }

  NativeFunction &function = iterator->second;
  if (block.Start < function.RangeStart || block.End > function.RangeEnd) {
    Notes.push_back("basic block outside function range at " +
                    hexAddress(functionEntry));
    return false;
  }
  function.Blocks.push_back(std::move(block));
  return true;
}

void NativeProgramState::addXref(NativeXref xref) {
  if (xref.From == 0 || xref.To == 0) {
    return;
  }

  size_t index = Xrefs.size();
  Xrefs.push_back(std::move(xref));
  XrefsByFrom[Xrefs[index].From].push_back(index);
  XrefsByTo[Xrefs[index].To].push_back(index);
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

std::unique_ptr<NativeAnalyzer> createSleighSeedInstructionAnalyzer() {
  return std::make_unique<SleighSeedInstructionAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output) {
  return std::make_unique<ReportAnalyzer>(output);
}

} // namespace notdec::bin2llvm
