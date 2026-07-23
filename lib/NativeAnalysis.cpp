#include "notdec-bin2llvm/NativeAnalysis.h"

#include "notdec-bin2llvm/LiefElfLoadImage.h"
#include "notdec-bin2llvm/SleighLift.h"

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

bool isInstructionFallthroughTo(const NativeInstruction &instruction,
                                uint64_t target) {
  return instruction.Fallthrough == target && instruction.end() == target;
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

bool supportsRelocationPltAnalysis(LIEF::ELF::ARCH arch) {
  return arch == LIEF::ELF::ARCH::X86_64 || arch == LIEF::ELF::ARCH::I386;
}

bool isRelativeRelocation(LIEF::ELF::ARCH arch,
                          LIEF::ELF::Relocation::TYPE type) {
  if (arch == LIEF::ELF::ARCH::X86_64) {
    return type == LIEF::ELF::Relocation::TYPE::X86_64_RELATIVE ||
           type == LIEF::ELF::Relocation::TYPE::X86_64_RELATIVE64;
  }
  if (arch == LIEF::ELF::ARCH::I386) {
    return type == LIEF::ELF::Relocation::TYPE::X86_RELATIVE;
  }
  return false;
}

bool isGlobDatRelocation(LIEF::ELF::ARCH arch,
                         LIEF::ELF::Relocation::TYPE type) {
  if (arch == LIEF::ELF::ARCH::X86_64) {
    return type == LIEF::ELF::Relocation::TYPE::X86_64_GLOB_DAT;
  }
  if (arch == LIEF::ELF::ARCH::I386) {
    return type == LIEF::ELF::Relocation::TYPE::X86_GLOB_DAT;
  }
  return false;
}

bool isJumpSlotRelocation(LIEF::ELF::ARCH arch,
                          LIEF::ELF::Relocation::TYPE type) {
  if (arch == LIEF::ELF::ARCH::X86_64) {
    return type == LIEF::ELF::Relocation::TYPE::X86_64_JUMP_SLOT;
  }
  if (arch == LIEF::ELF::ARCH::I386) {
    return type == LIEF::ELF::Relocation::TYPE::X86_JUMP_SLOT;
  }
  return false;
}

bool isAbsoluteExternalFunctionPointerRelocation(
    LIEF::ELF::ARCH arch, LIEF::ELF::Relocation::TYPE type) {
  if (arch == LIEF::ELF::ARCH::X86_64) {
    return type == LIEF::ELF::Relocation::TYPE::X86_64_64;
  }
  if (arch == LIEF::ELF::ARCH::I386) {
    return type == LIEF::ELF::Relocation::TYPE::X86_32;
  }
  return false;
}

bool isIRelativeRelocation(LIEF::ELF::ARCH arch,
                           LIEF::ELF::Relocation::TYPE type) {
  if (arch == LIEF::ELF::ARCH::X86_64) {
    return type == LIEF::ELF::Relocation::TYPE::X86_64_IRELATIVE;
  }
  if (arch == LIEF::ELF::ARCH::I386) {
    return type == LIEF::ELF::Relocation::TYPE::X86_IRELATIVE;
  }
  return false;
}

std::optional<int64_t>
relocationAddendForPointerValue(const NativeProgramState &state,
                                const LIEF::ELF::Relocation &relocation) {
  if (state.binary().header().machine_type() != LIEF::ELF::ARCH::I386) {
    return relocation.addend();
  }

  // i386 uses REL relocations: the addend is stored in the relocated word.
  std::optional<uint64_t> rawAddend =
      state.readRawPointer(relocation.address());
  if (!rawAddend) {
    return std::nullopt;
  }
  return static_cast<int64_t>(*rawAddend);
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

std::optional<uint64_t> parseHex(const std::string &text) {
  if (text.empty()) {
    return std::nullopt;
  }
  for (char ch : text) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
  }
  uint64_t value = 0;
  std::istringstream stream(text);
  stream >> std::hex >> value;
  if (!stream || !stream.eof()) {
    return std::nullopt;
  }
  return value;
}

std::optional<uint64_t> parseUnsignedNumber(const std::string &text) {
  if (text.empty()) {
    return std::nullopt;
  }
  try {
    size_t parsedLength = 0;
    uint64_t value = std::stoull(text, &parsedLength, 0);
    if (parsedLength != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::string trimAsciiWhitespace(const std::string &text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::optional<int64_t> readSigned32(const NativeProgramState &state,
                                    uint64_t address) {
  std::vector<uint8_t> bytes;
  if (!readBytes(state, address, 4, bytes) || bytes.size() != 4) {
    return std::nullopt;
  }
  uint32_t raw = 0;
  for (uint8_t index = 0; index < 4; ++index) {
    raw |= static_cast<uint32_t>(bytes[index]) << (index * 8);
  }
  return static_cast<int32_t>(raw);
}

void addUniqueAddress(std::vector<uint64_t> &addresses, uint64_t address) {
  if (std::find(addresses.begin(), addresses.end(), address) ==
      addresses.end()) {
    addresses.push_back(address);
  }
}

const NativeMemoryRange *findReadableRangeContaining(
    const NativeProgramState &state, uint64_t address) {
  for (const NativeMemoryRange &range : state.memoryRanges()) {
    if (range.Start > address) {
      break;
    }
    if (range.Readable && containsAddress(range.Start, range.Size, address)) {
      return &range;
    }
  }
  return nullptr;
}

bool isLikelyCStringByte(uint8_t byte) {
  return (byte >= 0x20 && byte <= 0x7e) || byte == '\t' || byte == '\n' ||
         byte == '\r';
}

bool looksLikeReadOnlyCString(const NativeProgramState &state,
                              uint64_t address) {
  const NativeMemoryRange *range = findReadableRangeContaining(state, address);
  if (range == nullptr || range->Writable || range->Executable) {
    return false;
  }

  constexpr uint64_t kMaxCStringProbeBytes = 256;
  constexpr uint64_t kMinCStringBytes = 4;

  uint64_t offset = address - range->Start;
  if (offset >= range->Bytes.size()) {
    return false;
  }

  uint64_t available = range->Bytes.size() - offset;
  uint64_t limit = std::min(kMaxCStringProbeBytes, available);
  uint64_t length = 0;
  for (uint64_t index = 0; index < limit; ++index) {
    uint8_t byte = range->Bytes[offset + index];
    if (byte == 0) {
      return length >= kMinCStringBytes;
    }
    if (!isLikelyCStringByte(byte)) {
      return false;
    }
    ++length;
  }
  return false;
}

bool isMappedAddress(const NativeProgramState &state, uint64_t address) {
  for (const NativeMemoryRange &range : state.memoryRanges()) {
    if (range.Start > address) {
      break;
    }
    if (containsAddress(range.Start, range.Size, address)) {
      return true;
    }
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

bool hasSource(const NativeFunctionSeed &seed, const std::string &source) {
  return std::find(seed.Sources.begin(), seed.Sources.end(), source) !=
         seed.Sources.end();
}

bool hasBytesAt(const NativeProgramState &state, uint64_t address,
                std::initializer_list<uint8_t> expected) {
  std::vector<uint8_t> bytes;
  if (!readBytes(state, address, expected.size(), bytes)) {
    return false;
  }
  return std::equal(bytes.begin(), bytes.end(), expected.begin(),
                    expected.end());
}

bool isX86GlibcStartPattern(const NativeProgramState &state,
                            uint64_t address) {
  if (state.binary().header().machine_type() != LIEF::ELF::ARCH::X86_64) {
    return false;
  }

  uint64_t cursor = address;
  if (hasBytesAt(state, cursor, {0xf3, 0x0f, 0x1e, 0xfa})) {
    cursor += 4;
  }
  if (!hasBytesAt(state, cursor,
                  {0x31, 0xed, 0x49, 0x89, 0xd1, 0x5e, 0x48, 0x89,
                   0xe2, 0x48, 0x83, 0xe4, 0xf0, 0x50, 0x54, 0x45,
                   0x31, 0xc0, 0x31, 0xc9})) {
    return false;
  }
  cursor += 20;

  if (hasBytesAt(state, cursor, {0x48, 0x8d, 0x3d}) ||
      hasBytesAt(state, cursor, {0x48, 0x8b, 0x3d}) ||
      hasBytesAt(state, cursor, {0x48, 0xc7, 0xc7})) {
    cursor += 7;
  } else if (hasBytesAt(state, cursor, {0x48, 0xbf})) {
    cursor += 10;
  } else {
    return false;
  }

  return hasBytesAt(state, cursor, {0xe8}) ||
         hasBytesAt(state, cursor, {0xff, 0x15});
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

      bool inserted = State.addFunctionSeed(pcBegin, 0, "", "eh-frame",
                                            NativeFunctionConfidence::High);
      if (inserted) {
        ++stats().AddedSeedCount;
      } else {
        ++stats().OverlappedSeedCount;
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

// Narrow x86-64 jump-table facts used by the current native frontend.
// The matcher is intentionally based on decoded instruction text: this stage
// only needs enough machine-level facts to recover CFG edges before lowering.
struct X86PicI32JumpDispatch {
  uint64_t BlockStart = 0;
  uint64_t TableBase = 0;
  uint64_t EntryCount = 0;
};

// A narrow parsed view of the common GCC/Clang PIC table dispatch:
//   MOVSXD target, dword ptr [base + index*0x4]
//   ADD    target, base
//   JMP    target
// The concrete registers vary, so keep the matcher semantic enough to avoid
// baking in only RAX/RBX while still rejecting unrelated indirect branches.
struct X86PicI32DispatchPattern {
  std::string TargetReg;
  std::string BaseReg;
  std::string IndexReg;
};

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

// Result of scanning one block backwards for an absolute LEA base.
// "Killed" means the register was overwritten before a matching LEA was found;
// callers must not keep walking predecessors through that path.
struct LeaBaseBlockScan {
  std::optional<uint64_t> Base;
  bool Killed = false;
};

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

// Linear address order can cross unrelated blocks and hit a later clobber of
// the base register.  Walk CFG predecessors instead; accept recovered bases
// only when the predecessor paths that do find a base agree on one value.
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

struct X86IndirectMemoryTarget {
  std::string BaseReg;
  uint64_t Displacement = 0;
};

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

struct X86ExternalFunctionPointerMatch {
  uint64_t SlotAddress = 0;
  NativeXrefKind Kind = NativeXrefKind::Flow;
};

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

class GtirbFunctionFactsAnalyzer final : public NativeAnalyzer {
public:
  explicit GtirbFunctionFactsAnalyzer(NativeGtirbDecodeOptions options)
      : Options(std::move(options)) {}

  std::string name() const override { return "GtirbFunctionFactsAnalyzer"; }
  int priority() const override { return 55; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
#if NOTDEC_BIN2LLVM_ENABLE_GTIRB
    auto importGtirb = [&](const std::filesystem::path &gtirbPath,
                           const char *source) -> bool {
      std::ifstream input(gtirbPath, std::ios::binary);
      if (!input) {
        state.addNote(std::string("gtirb frontend could not open ") + source +
                      ": " + gtirbPath.string());
        return false;
      }

      gtirb::Context context;
      gtirb_pprint::registerAuxDataTypes();
      gtirb::ErrorOr<gtirb::IR *> loaded = gtirb::IR::load(context, input);
      if (!loaded) {
        state.addNote(std::string("gtirb frontend load failed from ") + source +
                      ": " + loaded.getError().message());
        return false;
      }

      uint64_t functionCount = 0;
      uint64_t blockCount = 0;
      uint64_t edgeCount = 0;
      for (gtirb::Module &module : (*loaded)->modules()) {
        importFunctions(state, context, module, Options.RuntimeFilter,
                        functionCount, blockCount);
        importCfgEdges(state, module, edgeCount);
      }
      uint64_t fallbackCount = importSeedRanges(state, Options.RuntimeFilter);
      state.setControlFlowAuthority(NativeControlFlowAuthority::Gtirb);
      state.addNote("gtirb frontend imported from " + std::string(source) +
                    ": " + std::to_string(functionCount) + " functions, " +
                    std::to_string(blockCount) + " blocks, " +
                    std::to_string(edgeCount) + " cfg edges, " +
                    std::to_string(fallbackCount) +
                    " seed-range fallbacks");
      return true;
    };

    std::optional<std::filesystem::path> gtirbPath = resolveGtirbPath(state);
    if (!gtirbPath) {
      return;
    }
    (void)importGtirb(*gtirbPath, "gtirb");
#else
    (void)state;
    state.addNote("gtirb frontend unavailable: rebuild with GTIRB");
#endif
  }

private:
  NativeGtirbDecodeOptions Options;

  static std::string shellQuote(const std::string &text) {
    std::string quoted = "'";
    for (char ch : text) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += "'";
    return quoted;
  }

  std::optional<std::filesystem::path>
  resolveGtirbPath(NativeProgramState &state) const {
    if (!Options.GtirbPath.empty()) {
      return std::filesystem::path(Options.GtirbPath);
    }
    if (!Options.GenerateIfMissing || Options.ElfPath.empty()) {
      state.addNote("gtirb frontend skipped: no .gtirb path");
      return std::nullopt;
    }

    std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("notdec-" +
         std::to_string(std::hash<std::string>{}(Options.ElfPath)) +
         ".gtirb");
    std::string command = shellQuote(Options.DdisasmPath) + " " +
                          shellQuote(Options.ElfPath) + " --ir " +
                          shellQuote(output.string()) + " >/dev/null 2>/dev/null";
    int exitCode = std::system(command.c_str());
    if (exitCode == 0 && std::filesystem::exists(output)) {
      return output;
    }

    std::filesystem::path fallback =
        std::filesystem::temp_directory_path() /
        ("notdec-" +
         std::to_string(std::hash<std::string>{}(Options.ElfPath)) +
         "-no-analysis.gtirb");
    command = shellQuote(Options.DdisasmPath) + " --no-analysis " +
              shellQuote(Options.ElfPath) + " --ir " +
              shellQuote(fallback.string()) + " >/dev/null 2>/dev/null";
    exitCode = std::system(command.c_str());
    if (exitCode == 0 && std::filesystem::exists(fallback)) {
      state.addNote("gtirb frontend ddisasm fell back to no-analysis for: " +
                    Options.ElfPath);
      return fallback;
    }

    state.addNote("gtirb frontend ddisasm failed for: " + Options.ElfPath);
    return std::nullopt;
  }

#if NOTDEC_BIN2LLVM_ENABLE_GTIRB
  static const gtirb::CodeBlock *codeBlockByUuid(const gtirb::Context &context,
                                                 const gtirb::UUID &uuid) {
    const gtirb::Node *node = gtirb::Node::getByUUID(context, uuid);
    if (node == nullptr || !gtirb::CodeBlock::classof(node)) {
      return nullptr;
    }
    return static_cast<const gtirb::CodeBlock *>(node);
  }

  static uint64_t codeBlockAddress(const gtirb::CodeBlock &block) {
    return static_cast<uint64_t>(*block.getAddress());
  }

  static std::string functionNameFor(const gtirb::Context &context,
                                     const gtirb::Module &module,
                                     const gtirb::UUID &functionUuid) {
    const auto *functionNames =
        module.getAuxData<gtirb::schema::FunctionNames>();
    if (functionNames == nullptr) {
      return "";
    }
    auto iterator = functionNames->find(functionUuid);
    if (iterator == functionNames->end()) {
      return "";
    }
    const gtirb::Node *node = gtirb::Node::getByUUID(context, iterator->second);
    if (node == nullptr || !gtirb::Symbol::classof(node)) {
      return "";
    }
    return static_cast<const gtirb::Symbol *>(node)->getName();
  }

  static void importFunctions(NativeProgramState &state,
                              const gtirb::Context &context,
                              const gtirb::Module &module,
                              const NativeRuntimeFilterOptions &runtimeFilter,
                              uint64_t &functionCount, uint64_t &blockCount) {
    const auto *functionEntries =
        module.getAuxData<gtirb::schema::FunctionEntries>();
    const auto *functionBlocks =
        module.getAuxData<gtirb::schema::FunctionBlocks>();
    if (functionEntries == nullptr || functionBlocks == nullptr) {
      state.addNote("gtirb frontend missing function auxdata");
      return;
    }

    for (const auto &[functionUuid, entryUuids] : *functionEntries) {
      if (entryUuids.empty()) {
        continue;
      }
      auto blocksIterator = functionBlocks->find(functionUuid);
      if (blocksIterator == functionBlocks->end()) {
        continue;
      }

      std::vector<NativeBasicBlock> blocks;
      uint64_t rangeStart = std::numeric_limits<uint64_t>::max();
      uint64_t rangeEnd = 0;
      for (const gtirb::UUID &blockUuid : blocksIterator->second) {
        const gtirb::CodeBlock *block = codeBlockByUuid(context, blockUuid);
        if (block == nullptr || !block->getAddress() || block->getSize() == 0) {
          continue;
        }
        uint64_t start = codeBlockAddress(*block);
        uint64_t end = start + block->getSize();
        if (!state.isExecutableAddress(start)) {
          continue;
        }
        NativeBasicBlock nativeBlock;
        nativeBlock.Start = start;
        nativeBlock.End = end;
        blocks.push_back(std::move(nativeBlock));
        rangeStart = std::min(rangeStart, start);
        rangeEnd = std::max(rangeEnd, end);
      }
      if (blocks.empty()) {
        continue;
      }

      uint64_t entryAddress = 0;
      for (const gtirb::UUID &entryUuid : entryUuids) {
        const gtirb::CodeBlock *entryBlock =
            codeBlockByUuid(context, entryUuid);
        if (entryBlock != nullptr && entryBlock->getAddress()) {
          entryAddress = codeBlockAddress(*entryBlock);
          break;
        }
      }
      if (entryAddress == 0) {
        continue;
      }

      std::sort(blocks.begin(), blocks.end(),
                [](const NativeBasicBlock &lhs,
                   const NativeBasicBlock &rhs) { return lhs.Start < rhs.Start; });
      std::string name = functionNameFor(context, module, functionUuid);
      if (runtimeFilter.SkipRuntimeFunctions &&
          (isNativeRuntimeFunctionName(name) ||
           isNativeRuntimeAddress(state, entryAddress))) {
        continue;
      }
      state.addFunctionSeed(entryAddress, rangeEnd - rangeStart, name,
                            "gtirb-ddisasm",
                            NativeFunctionConfidence::High);
      NativeFunction function;
      function.Entry = entryAddress;
      function.RangeStart = rangeStart;
      function.RangeEnd = rangeEnd;
      function.Name = std::move(name);
      function.Blocks = std::move(blocks);
      function.Source = "gtirb-ddisasm";
      if (state.addFunction(std::move(function))) {
        ++functionCount;
        blockCount += blocksIterator->second.size();
      }
    }
  }

  static void importCfgEdges(NativeProgramState &state,
                             const gtirb::Module &module,
                             uint64_t &edgeCount) {
    const gtirb::CFG &cfg = module.getIR()->getCFG();
    std::map<uint64_t, const gtirb::CodeBlock *> codeBlocksByAddress;
    for (const gtirb::CodeBlock &block : module.code_blocks()) {
      if (block.getAddress()) {
        codeBlocksByAddress.emplace(static_cast<uint64_t>(*block.getAddress()),
                                    &block);
      }
    }

    for (const auto &[entry, function] : state.functions()) {
      (void)entry;
      std::set<uint64_t> blockStarts;
      for (const NativeBasicBlock &block : function.Blocks) {
        blockStarts.insert(block.Start);
      }
      auto functionContains = [&](uint64_t address) {
        for (const NativeBasicBlock &block : function.Blocks) {
          if (block.Start <= address && address < block.End) {
            return true;
          }
        }
        return false;
      };
      auto addFlowXref = [&](uint64_t source, uint64_t target) {
        NativeXref xref;
        xref.From = source;
        xref.To = target;
        xref.Kind = NativeXrefKind::Flow;
        xref.Source = "gtirb-ddisasm-flow";
        state.addXref(std::move(xref));
      };
      auto addCallXref = [&](uint64_t source, uint64_t target) {
        NativeXref xref;
        xref.From = source;
        xref.To = target;
        xref.Kind = NativeXrefKind::Call;
        xref.Source = "gtirb-ddisasm-call";
        state.addXref(std::move(xref));
      };
      auto importBlockEdges = [&](uint64_t source,
                                  const gtirb::CodeBlock &gtirbBlock,
                                  bool attachSuccessors) {
        std::vector<uint64_t> successors;
        for (auto [successorNode, label] :
             gtirb::cfgSuccessors(cfg, &gtirbBlock)) {
          if (!label || !gtirb::CodeBlock::classof(successorNode)) {
            continue;
          }
          const auto &[conditional, direct, type] = *label;
          (void)conditional;
          (void)direct;
          const gtirb::CodeBlock *successor =
              static_cast<const gtirb::CodeBlock *>(successorNode);
          if (!successor->getAddress()) {
            continue;
          }
          uint64_t target = static_cast<uint64_t>(*successor->getAddress());
          if (type == gtirb::EdgeType::Branch ||
              type == gtirb::EdgeType::Fallthrough) {
            bool localTarget = blockStarts.count(target) != 0;
            if (attachSuccessors && localTarget) {
              successors.push_back(target);
            } else {
              addFlowXref(source, target);
            }
            ++edgeCount;
          } else if (type == gtirb::EdgeType::Call) {
            addCallXref(source, target);
          }
        }
        if (attachSuccessors) {
          state.addBasicBlockSuccessors(function.Entry, source, successors);
        }
      };

      for (const NativeBasicBlock &block : function.Blocks) {
        auto gtirbBlock = codeBlocksByAddress.find(block.Start);
        if (gtirbBlock == codeBlocksByAddress.end()) {
          continue;
        }
        importBlockEdges(block.Start, *gtirbBlock->second,
                         /*attachSuccessors=*/true);
      }

      // GTIRB CFG can contain code-block boundaries that FunctionBlocks did not
      // list for this function.  Keep those edges as flow xrefs so the normalizer
      // can split the containing NativeBasicBlock before lowering.
      for (const auto &[address, gtirbBlock] : codeBlocksByAddress) {
        if (blockStarts.count(address) != 0 || !functionContains(address)) {
          continue;
        }
        importBlockEdges(address, *gtirbBlock, /*attachSuccessors=*/false);
      }
    }
  }

  static uint64_t
  importSeedRanges(NativeProgramState &state,
                   const NativeRuntimeFilterOptions &runtimeFilter) {
    uint64_t imported = 0;
    for (const auto &[entry, seed] : state.functionSeeds()) {
      if (state.functionAt(entry) != nullptr ||
          seed.Confidence != NativeFunctionConfidence::High ||
          seed.RangeStart == 0 || seed.RangeEnd <= seed.RangeStart ||
          !state.isExecutableAddress(entry) ||
          !executableRangeContains(state, seed.RangeStart, seed.RangeEnd)) {
        continue;
      }
      if (runtimeFilter.SkipRuntimeFunctions &&
          isNativeRuntimeSeed(state, seed)) {
        continue;
      }

      NativeBasicBlock block;
      block.Start = seed.RangeStart;
      block.End = seed.RangeEnd;

      NativeFunction function;
      function.Entry = entry;
      function.RangeStart = seed.RangeStart;
      function.RangeEnd = seed.RangeEnd;
      function.Name = seed.PrimaryName;
      function.Blocks.push_back(std::move(block));
      function.Source = "gtirb-seed-range-fallback";
      function.IsExternallyVisible = seed.IsExternallyVisible;
      if (state.addFunction(std::move(function))) {
        ++imported;
      }
    }
    return imported;
  }
#endif
};

class SleighSeedInstructionAnalyzer final : public NativeAnalyzer {
public:
  explicit SleighSeedInstructionAnalyzer(NativeSleighDecodeOptions options)
      : Options(options) {}

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

    addRelocationCodeSeeds(state);
    SleighInstructionDecoder decoder(loadImage, SpecOptions, NullErrors);
    if (!decoder.isValid()) {
      state.addNote("sleigh instruction decode skipped: engine init failed");
      return;
    }
    if (Options.DecodeExistingBlocksOnly) {
      decodeExistingBlocks(state, decoder);
      return;
    }

    std::deque<DecodeQueueItem> decodeQueue;
    std::set<std::pair<uint64_t, uint64_t>> queuedSeeds;
    std::set<std::pair<uint64_t, uint64_t>> decodedSeeds;
    enqueueInitialSeeds(state, Options, decodeQueue, queuedSeeds);

    uint64_t decodedSeedCount = 0;
    while (!decodeQueue.empty()) {
      if (Options.MaxDecodedSeeds &&
          decodedSeedCount >= *Options.MaxDecodedSeeds) {
        state.addNote("sleigh instruction decode stopped after explicit seed "
                      "limit " +
                      std::to_string(*Options.MaxDecodedSeeds));
        break;
      }
      DecodeQueueItem item = decodeQueue.front();
      decodeQueue.pop_front();
      std::pair<uint64_t, uint64_t> seedKey{item.FunctionEntry,
                                            item.BlockAddress};
      if (!decodedSeeds.insert(seedKey).second ||
          !state.isExecutableAddress(item.BlockAddress)) {
        continue;
      }
      DecodeSeedResult result =
          decodeSeed(state, decoder, item.FunctionEntry, item.BlockAddress);
      ++decodedSeedCount;
      for (uint64_t target : result.CallTargets) {
        if (Options.RuntimeFilter.SkipRuntimeFunctions &&
            isNativeRuntimeAddress(state, target)) {
          continue;
        }
        state.addFunctionSeed(target, 0, "", "sleigh-direct-call",
                              NativeFunctionConfidence::High);
        enqueueSeed(state, target, target, decodeQueue, queuedSeeds);
      }
      for (uint64_t target : result.TailBranchTargets) {
        if (Options.RuntimeFilter.SkipRuntimeFunctions &&
            isNativeRuntimeAddress(state, target)) {
          continue;
        }
        state.addFunctionSeed(target, 0, "", "sleigh-tail-branch",
                              NativeFunctionConfidence::High);
        enqueueSeed(state, target, target, decodeQueue, queuedSeeds);
      }
      for (uint64_t target : result.BranchTargets) {
        if (targetBelongsToFunctionRange(state, item.FunctionEntry, target)) {
          enqueueSeed(state, item.FunctionEntry, target, decodeQueue,
                      queuedSeeds);
        }
      }
      if (result.FallthroughTarget) {
        if (targetBelongsToFunctionRange(state, item.FunctionEntry,
                                         *result.FallthroughTarget)) {
          enqueueSeed(state, item.FunctionEntry, *result.FallthroughTarget,
                      decodeQueue, queuedSeeds);
        }
      }
      enqueueRecoveredJumpTableTargets(state, item.FunctionEntry, decodeQueue,
                                       queuedSeeds);
    }
  }

private:
  // Decode every known entry seed, but keep each local decode small.  That makes
  // module-wide discovery cover real Bench2 binaries without letting one bad
  // function consume a large linear range.
  static constexpr uint64_t MaxInstructionsPerSeed = 8;
  static constexpr uint64_t MaxBytesPerSeed = 64;

  struct DecodedFlowInfo {
    std::vector<uint64_t> BranchTargets;
    std::vector<uint64_t> CallTargets;
    std::vector<uint64_t> InternalCallTargets;
    bool HasConditionalBranch = false;
    bool HasUnconditionalBranch = false;
    bool HasIndirectBranch = false;
    bool HasReturn = false;
    bool HasIndirectCall = false;
  };

  struct DirectControlFlowResult {
    std::map<uint64_t, DecodedFlowInfo> FlowInfos;
    std::vector<NativeXref> Xrefs;
    std::vector<NativeUnresolvedFlow> UnresolvedFlows;
  };

  // A seed decode is intentionally small. If the window ends without a real
  // control-flow terminator, the next address is still part of the same
  // function and should be decoded as another block.
  struct DecodeSeedResult {
    std::vector<uint64_t> CallTargets;
    // A direct jump can hand off to another function instead of staying inside
    // the current CFG.  Decode that target separately so this function does not
    // absorb the callee's blocks.
    std::vector<uint64_t> TailBranchTargets;
    std::vector<uint64_t> BranchTargets;
    std::optional<uint64_t> FallthroughTarget;
  };

  // Direct calls start a new function.  Direct branches keep the same function
  // entry and decode another block for that function.
  struct DecodeQueueItem {
    uint64_t FunctionEntry = 0;
    uint64_t BlockAddress = 0;
  };

  std::ostringstream NullErrors;
  SleighSpecOptions SpecOptions;
  NativeSleighDecodeOptions Options;

  struct ExistingDecodeRange {
    uint64_t FunctionEntry = 0;
    uint64_t Start = 0;
    uint64_t End = 0;
  };

  void decodeExistingBlocks(NativeProgramState &state,
                            SleighInstructionDecoder &decoder) {
    std::vector<ExistingDecodeRange> ranges;
    for (const auto &[entry, function] : state.functions()) {
      for (const NativeBasicBlock &block : function.Blocks) {
        ranges.push_back({entry, block.Start, block.End});
      }
    }
    for (const ExistingDecodeRange &range : ranges) {
      decodeExistingBlock(state, decoder, range.FunctionEntry, range.Start,
                          range.End);
    }
  }

  void decodeExistingBlock(NativeProgramState &state,
                           SleighInstructionDecoder &decoder,
                           uint64_t functionEntry, uint64_t start,
                           uint64_t end) {
    if (start >= end || !state.isExecutableAddress(start)) {
      return;
    }
    std::optional<uint64_t> availableBytes = executableBytesFrom(state, start);
    if (!availableBytes || *availableBytes == 0) {
      return;
    }
    uint64_t requestedBytes = std::min<uint64_t>(end - start, *availableBytes);
    SleighInstructionDecode decode =
        decoder.decode(start, std::numeric_limits<uint64_t>::max(),
                       requestedBytes, NullErrors);
    std::vector<NativeInstruction> decodedInstructions;
    for (const SleighInstructionSummary &summary : decode.Instructions) {
      if (summary.Address < start || summary.Address + summary.Size > end) {
        continue;
      }
      NativeInstruction instruction;
      instruction.Address = summary.Address;
      instruction.Size = summary.Size;
      instruction.Mnemonic = summary.Body.empty()
                                  ? summary.Mnemonic
                                  : summary.Mnemonic + " " + summary.Body;
      instruction.Source = "sleigh-gtirb-block";
      (void)readBytes(state, summary.Address, summary.Size, instruction.Bytes);
      decodedInstructions.push_back(std::move(instruction));
    }
    bool collectMachineFlow = !state.hasGtirbControlFlowAuthority();
    DirectControlFlowResult flowResult =
        collectDirectControlFlow(state, decode.Pcode, collectMachineFlow);
    annotateDecodedInstructionFlows(decodedInstructions, flowResult.FlowInfos);
    std::vector<uint64_t> tailBranchTargets;
    if (collectMachineFlow) {
      collectTailBranchTargets(state, functionEntry, decodedInstructions,
                               tailBranchTargets);
    }
    for (const NativeInstruction &instruction : decodedInstructions) {
      state.addInstruction(instruction);
    }
    for (const NativeXref &xref : flowResult.Xrefs) {
      state.addXref(xref);
    }
    for (const NativeUnresolvedFlow &flow : flowResult.UnresolvedFlows) {
      state.addUnresolvedFlow(flow);
    }
    if (collectMachineFlow && !decodedInstructions.empty()) {
      uint64_t rangeStart = decodedInstructions.front().Address;
      uint64_t rangeEnd =
          decodedInstructions.back().Address + decodedInstructions.back().Size;
      std::vector<uint64_t> branchTargets;
      addDecodedFunctionBlocks(state, functionEntry, rangeStart, rangeEnd,
                               decodedInstructions, std::nullopt,
                               branchTargets);
    }
  }

  static void addRelocationCodeSeeds(NativeProgramState &state) {
    for (const auto &[slot, target] : state.relocatedPointers()) {
      (void)slot;
      if (!state.isExecutableAddress(target)) {
        continue;
      }
      state.addFunctionSeed(target, 0, "", "elf-relocation-code",
                            NativeFunctionConfidence::Low);
    }
  }

  bool resolveSpecOptions(NativeProgramState &state) {
    const LIEF::ELF::Binary &binary = state.binary();
    std::optional<NativeElfArchitectureSpec> archSpec =
        nativeElfArchitectureSpec(binary);
    if (!archSpec) {
      state.addNote(unsupportedNativeElfArchitectureMessage(
          binary, "SLEIGH instruction decode"));
      return false;
    }

    std::filesystem::path specRoot =
        std::filesystem::path(NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR) /
        "Ghidra/Processors/x86/data/languages";
    std::filesystem::path slaPath = specRoot / archSpec->SlaFileName;
    std::filesystem::path pspecPath = specRoot / archSpec->PspecFileName;
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

  static void enqueueInitialSeeds(NativeProgramState &state,
                                  const NativeSleighDecodeOptions &options,
                                  std::deque<DecodeQueueItem> &decodeQueue,
                                  std::set<std::pair<uint64_t, uint64_t>>
                                      &queuedSeeds) {
    if (!options.InitialFunctionEntries.empty()) {
      for (uint64_t entry : options.InitialFunctionEntries) {
        enqueueSeed(state, entry, entry, decodeQueue, queuedSeeds);
      }
      return;
    }

    for (NativeFunctionConfidence confidence :
         {NativeFunctionConfidence::High, NativeFunctionConfidence::Medium,
          NativeFunctionConfidence::Low}) {
      for (const NativeFunctionWorkItem &item : state.functionWorklist()) {
        auto seedIterator = state.functionSeeds().find(item.Address);
        if (seedIterator == state.functionSeeds().end() ||
            seedIterator->second.Confidence != confidence) {
          continue;
        }
        if (options.RuntimeFilter.SkipRuntimeFunctions &&
            isNativeRuntimeSeed(state, seedIterator->second)) {
          continue;
        }
        enqueueSeed(state, item.Address, item.Address, decodeQueue,
                    queuedSeeds);
      }
    }
  }

  static bool enqueueSeed(NativeProgramState &state, uint64_t functionEntry,
                          uint64_t blockAddress,
                          std::deque<DecodeQueueItem> &decodeQueue,
                          std::set<std::pair<uint64_t, uint64_t>>
                              &queuedSeeds) {
    if (!state.isExecutableAddress(functionEntry) ||
        !state.isExecutableAddress(blockAddress) ||
        !queuedSeeds.insert({functionEntry, blockAddress}).second) {
      return false;
    }
    decodeQueue.push_back({functionEntry, blockAddress});
    return true;
  }

  static void enqueueRecoveredJumpTableTargets(
      NativeProgramState &state, uint64_t functionEntry,
      std::deque<DecodeQueueItem> &decodeQueue,
      std::set<std::pair<uint64_t, uint64_t>> &queuedSeeds) {
    const NativeFunction *function = state.functionAt(functionEntry);
    if (function == nullptr) {
      return;
    }

    for (const NativeUnresolvedFlow &flow : state.unresolvedFlows()) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch ||
          !functionContainsAddress(*function, flow.Address)) {
        continue;
      }

      std::optional<X86PicI32JumpDispatch> dispatch =
          matchX86PicI32OffsetDispatch(state, *function, flow.Address);
      if (!dispatch) {
        continue;
      }

      std::vector<uint64_t> targets;
      bool complete = false;
      if (!readX86PicI32Targets(state, dispatch->TableBase,
                                dispatch->EntryCount,
                                /*onlyExistingBlocks=*/false, targets,
                                complete)) {
        continue;
      }
      for (uint64_t target : targets) {
        if (targetBelongsToFunctionRange(state, functionEntry, target)) {
          enqueueSeed(state, functionEntry, target, decodeQueue, queuedSeeds);
        }
      }
    }
  }

  DecodeSeedResult decodeSeed(NativeProgramState &state,
                              SleighInstructionDecoder &decoder,
                              uint64_t functionEntry, uint64_t address) {
    DecodeSeedResult result;
    std::optional<uint64_t> availableBytes = executableBytesFrom(state, address);
    if (!availableBytes || *availableBytes == 0) {
      return result;
    }

    uint64_t decodeBytes = std::min(
        MaxBytesPerSeed,
        boundedBytesForFunctionSeed(state, functionEntry, address,
                                    *availableBytes));
    SleighInstructionDecode decode =
        decoder.decode(address, MaxInstructionsPerSeed, decodeBytes,
                       NullErrors);
    std::vector<NativeInstruction> decodedInstructions;
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
      decodedInstructions.push_back(std::move(instruction));
    }
    DirectControlFlowResult flowResult;
    std::map<uint64_t, DecodedFlowInfo> flowInfos;
    if (rangeStart == address && rangeStart < rangeEnd) {
      flowResult = collectDirectControlFlow(state, decode.Pcode,
                                            /*collectMachineFlow=*/true);
      flowInfos = flowResult.FlowInfos;
    }
    annotateDecodedInstructionFlows(decodedInstructions, flowInfos);
    collectTailBranchTargets(state, functionEntry, decodedInstructions,
                             result.TailBranchTargets);
    // A seed is decoded linearly, but only locally reachable instructions should
    // become block facts for this function entry.
    std::set<uint64_t> reachableStarts =
        reachableInstructionStarts(decodedInstructions, rangeStart);
    decodedInstructions.erase(
        std::remove_if(decodedInstructions.begin(), decodedInstructions.end(),
                       [&](const NativeInstruction &instruction) {
                         return reachableStarts.count(instruction.Address) == 0;
                       }),
        decodedInstructions.end());
    if (decodedInstructions.empty()) {
      return result;
    }
    for (uint64_t target : reachableCallTargets(flowResult, reachableStarts)) {
      addUniqueAddress(result.CallTargets, target);
    }
    for (const NativeXref &xref : reachableXrefs(flowResult, reachableStarts)) {
      state.addXref(xref);
    }
    std::set<uint64_t> resolvedIndirectFlowStarts =
        collectExternalFunctionPointerXrefs(state, decodedInstructions);
    std::set<uint64_t> trapStarts = trapInstructionStarts(decodedInstructions);
    for (const NativeUnresolvedFlow &flow :
         reachableUnresolvedFlows(flowResult, reachableStarts, trapStarts,
                                  resolvedIndirectFlowStarts)) {
      state.addUnresolvedFlow(flow);
    }
    rangeStart = decodedInstructions.front().Address;
    rangeEnd =
        decodedInstructions.back().Address + decodedInstructions.back().Size;
    result.FallthroughTarget = fallthroughTargetForDecodedWindow(
        state, functionEntry, rangeStart, rangeEnd, decodeBytes,
        decodedInstructions);
    if (result.FallthroughTarget) {
      decodedInstructions.back().Fallthrough = *result.FallthroughTarget;
    }
    for (const NativeInstruction &instruction : decodedInstructions) {
      state.addInstruction(instruction);
    }
    addDecodedFunctionBlocks(state, functionEntry, rangeStart, rangeEnd,
                             decodedInstructions, result.FallthroughTarget,
                             result.BranchTargets);
    return result;
  }

  static void collectTailBranchTargets(
      const NativeProgramState &state, uint64_t functionEntry,
      std::vector<NativeInstruction> &instructions,
      std::vector<uint64_t> &tailBranchTargets) {
    for (NativeInstruction &instruction : instructions) {
      std::vector<uint64_t> localTargets;
      for (uint64_t target : instruction.DirectFlowTargets) {
        if (isTailBranchTarget(state, functionEntry, instruction, target)) {
          addUniqueAddress(tailBranchTargets, target);
          addUniqueAddress(instruction.TailFlowTargets, target);
          continue;
        }
        addUniqueAddress(localTargets, target);
      }
      instruction.DirectFlowTargets = std::move(localTargets);
    }
  }

  static bool isTailBranchTarget(const NativeProgramState &state,
                                 uint64_t functionEntry,
                                 const NativeInstruction &instruction,
                                 uint64_t target) {
    if (instruction.FlowKind != NativeInstructionFlowKind::UnconditionalBranch) {
      return false;
    }
    if (state.lookupPltExternal(target)) {
      return true;
    }
    if (isKnownOtherFunctionEntry(state, functionEntry, target)) {
      return true;
    }
    if (!isDynamicArrayThunkSeed(state, functionEntry)) {
      return false;
    }
    return target < functionEntry;
  }

  static bool isDynamicArrayThunkSeed(const NativeProgramState &state,
                                      uint64_t functionEntry) {
    auto seedIterator = state.functionSeeds().find(functionEntry);
    if (seedIterator == state.functionSeeds().end()) {
      return false;
    }
    const NativeFunctionSeed &seed = seedIterator->second;
    if (seed.RangeStart != 0 && seed.RangeEnd > seed.RangeStart) {
      return false;
    }
    return std::find(seed.Sources.begin(), seed.Sources.end(),
                     "dt-init-array") != seed.Sources.end() ||
           std::find(seed.Sources.begin(), seed.Sources.end(),
                     "dt-fini-array") != seed.Sources.end();
  }

  static std::set<uint64_t> reachableInstructionStarts(
      const std::vector<NativeInstruction> &instructions, uint64_t entry) {
    std::map<uint64_t, const NativeInstruction *> instructionByAddress;
    for (const NativeInstruction &instruction : instructions) {
      instructionByAddress[instruction.Address] = &instruction;
    }

    std::set<uint64_t> reachable;
    std::vector<uint64_t> worklist;
    if (instructionByAddress.count(entry) != 0) {
      worklist.push_back(entry);
    }
    while (!worklist.empty()) {
      uint64_t address = worklist.back();
      worklist.pop_back();
      if (!reachable.insert(address).second) {
        continue;
      }

      const NativeInstruction &instruction = *instructionByAddress[address];
      for (uint64_t target : instruction.DirectFlowTargets) {
        if (instructionByAddress.count(target) != 0) {
          worklist.push_back(target);
        }
      }
      if (instruction.Fallthrough &&
          isInstructionFallthroughTo(instruction, *instruction.Fallthrough) &&
          instructionByAddress.count(*instruction.Fallthrough) != 0) {
        worklist.push_back(*instruction.Fallthrough);
      }
    }
    return reachable;
  }

  static std::vector<uint64_t>
  reachableCallTargets(const DirectControlFlowResult &flowResult,
                       const std::set<uint64_t> &reachableStarts) {
    std::vector<uint64_t> targets;
    for (const auto &[address, info] : flowResult.FlowInfos) {
      if (reachableStarts.count(address) == 0) {
        continue;
      }
      for (uint64_t target : info.InternalCallTargets) {
        addUniqueAddress(targets, target);
      }
    }
    return targets;
  }

  static std::vector<NativeXref>
  reachableXrefs(const DirectControlFlowResult &flowResult,
                 const std::set<uint64_t> &reachableStarts) {
    std::vector<NativeXref> xrefs;
    for (const NativeXref &xref : flowResult.Xrefs) {
      if (reachableStarts.count(xref.From) != 0) {
        xrefs.push_back(xref);
      }
    }
    return xrefs;
  }

  static std::set<uint64_t> collectExternalFunctionPointerXrefs(
      NativeProgramState &state,
      const std::vector<NativeInstruction> &instructions) {
    std::set<uint64_t> resolved;
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction &instruction = instructions[index];
      std::optional<X86ExternalFunctionPointerMatch> match =
          matchX86ExternalFunctionPointerInstruction(state, instructions,
                                                     index);
      if (!match) {
        continue;
      }

      NativeXref xref;
      xref.From = instruction.Address;
      xref.To = match->SlotAddress;
      xref.Kind = match->Kind;
      xref.Source = match->Kind == NativeXrefKind::Call
                        ? "x86-call-external-function-pointer"
                        : "x86-tail-branch-external-function-pointer";
      state.addXref(std::move(xref));
      resolved.insert(instruction.Address);
    }
    return resolved;
  }

  static std::set<uint64_t>
  trapInstructionStarts(const std::vector<NativeInstruction> &instructions) {
    std::set<uint64_t> starts;
    for (const NativeInstruction &instruction : instructions) {
      if (instruction.FlowKind == NativeInstructionFlowKind::Trap) {
        starts.insert(instruction.Address);
      }
    }
    return starts;
  }

  static std::vector<NativeUnresolvedFlow> reachableUnresolvedFlows(
      const DirectControlFlowResult &flowResult,
      const std::set<uint64_t> &reachableStarts,
      const std::set<uint64_t> &trapStarts,
      const std::set<uint64_t> &resolvedIndirectFlowStarts) {
    std::vector<NativeUnresolvedFlow> flows;
    for (const NativeUnresolvedFlow &flow : flowResult.UnresolvedFlows) {
      if (reachableStarts.count(flow.Address) != 0 &&
          trapStarts.count(flow.Address) == 0 &&
          resolvedIndirectFlowStarts.count(flow.Address) == 0) {
        flows.push_back(flow);
      }
    }
    return flows;
  }

  static void annotateDecodedInstructionFlows(
      std::vector<NativeInstruction> &instructions,
      const std::map<uint64_t, DecodedFlowInfo> &flowInfos) {
    for (size_t index = 0; index < instructions.size(); ++index) {
      NativeInstruction &instruction = instructions[index];
      auto flowIterator = flowInfos.find(instruction.Address);
      if (flowIterator != flowInfos.end()) {
        const DecodedFlowInfo &flowInfo = flowIterator->second;
        instruction.DirectFlowTargets = flowInfo.BranchTargets;
        instruction.DirectCallTargets = flowInfo.CallTargets;
        instruction.HasIndirectCall = flowInfo.HasIndirectCall;
        if (flowInfo.HasConditionalBranch) {
          instruction.FlowKind = NativeInstructionFlowKind::ConditionalBranch;
        } else if (flowInfo.HasUnconditionalBranch) {
          instruction.FlowKind = NativeInstructionFlowKind::UnconditionalBranch;
        } else if (flowInfo.HasIndirectBranch) {
          instruction.FlowKind = NativeInstructionFlowKind::IndirectBranch;
        } else if (flowInfo.HasReturn) {
          instruction.FlowKind = NativeInstructionFlowKind::Return;
        }
      }
      if (instruction.Mnemonic == "UD2") {
        instruction.FlowKind = NativeInstructionFlowKind::Trap;
        instruction.DirectFlowTargets.clear();
        instruction.TailFlowTargets.clear();
      }

      bool hasNextInstruction = index + 1 != instructions.size();
      bool hasFallthrough =
          instruction.FlowKind !=
              NativeInstructionFlowKind::UnconditionalBranch &&
          instruction.FlowKind != NativeInstructionFlowKind::IndirectBranch &&
          instruction.FlowKind != NativeInstructionFlowKind::Trap &&
          instruction.FlowKind != NativeInstructionFlowKind::Return;
      if (hasNextInstruction && hasFallthrough &&
          instructions[index + 1].Address == instruction.end()) {
        instruction.Fallthrough = instructions[index + 1].Address;
      } else if (!hasNextInstruction &&
                 instruction.FlowKind ==
                     NativeInstructionFlowKind::ConditionalBranch) {
        instruction.Fallthrough = instruction.end();
      }
    }
  }

  static std::optional<uint64_t> fallthroughTargetForDecodedWindow(
      NativeProgramState &state, uint64_t entry, uint64_t rangeStart,
      uint64_t rangeEnd, uint64_t decodeBytes,
      const std::vector<NativeInstruction> &instructions) {
    if (instructions.empty() || rangeStart == 0 || rangeStart >= rangeEnd) {
      return std::nullopt;
    }
    const NativeInstruction &last = instructions.back();
    if (last.Address + last.Size != rangeEnd) {
      return std::nullopt;
    }
    if (last.FlowKind == NativeInstructionFlowKind::UnconditionalBranch ||
        last.FlowKind == NativeInstructionFlowKind::IndirectBranch ||
        last.FlowKind == NativeInstructionFlowKind::Trap ||
        last.FlowKind == NativeInstructionFlowKind::Return) {
      return std::nullopt;
    }
    if (!state.isExecutableAddress(rangeEnd) ||
        isKnownOtherFunctionEntry(state, entry, rangeEnd)) {
      return std::nullopt;
    }
    auto seedIterator = state.functionSeeds().find(entry);
    if (seedIterator != state.functionSeeds().end()) {
      const NativeFunctionSeed &seed = seedIterator->second;
      if (seed.RangeStart != 0 && seed.RangeEnd > seed.RangeStart &&
          rangeEnd == seed.RangeEnd) {
        return std::nullopt;
      }
    }
    bool hitInstructionLimit = instructions.size() == MaxInstructionsPerSeed;
    bool hitByteLimit = rangeEnd == rangeStart + decodeBytes;
    if (!hitInstructionLimit && !hitByteLimit) {
      return std::nullopt;
    }
    return rangeEnd;
  }

  static void addDecodedFunctionBlocks(
      NativeProgramState &state, uint64_t entry, uint64_t rangeStart,
      uint64_t rangeEnd,
      const std::vector<NativeInstruction> &instructions,
      std::optional<uint64_t> fallthroughTarget,
      std::vector<uint64_t> &branchTargets) {
    if (rangeStart == 0 || rangeStart >= rangeEnd) {
      return;
    }

    std::vector<NativeBasicBlock> blocks =
        buildDecodedBlocks(instructions, rangeStart, rangeEnd);
    for (NativeBasicBlock &block : blocks) {
      if (fallthroughTarget && block.End == rangeEnd &&
          block.Successors.empty()) {
        addUniqueAddress(block.Successors, *fallthroughTarget);
      }
      eraseOutOfRangeFunctionSuccessors(state, entry, block.Successors);
      eraseKnownOtherFunctionSuccessors(state, entry, block.Successors);
      for (uint64_t successor : block.Successors) {
        if (successor < rangeStart || successor >= rangeEnd) {
          addUniqueAddress(branchTargets, successor);
        }
      }
    }
    if (blocks.empty()) {
      return;
    }

    if (state.functionAt(entry) != nullptr) {
      for (NativeBasicBlock &block : blocks) {
        state.addBasicBlock(entry, std::move(block));
      }
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
      function.IsExternallyVisible = seedIterator->second.IsExternallyVisible;
    }

    function.Blocks = std::move(blocks);
    state.addFunction(std::move(function));
  }

  static DirectControlFlowResult
  collectDirectControlFlow(NativeProgramState &state,
                           const PcodeProgram &program,
                           bool collectMachineFlow) {
    DirectControlFlowResult result;
    std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> seenXrefs;
    // This is intentionally local to one decoded P-Code range.  It only keeps
    // enough provenance to recognize guarded GOT indirect calls.
    std::map<std::string, uint64_t> sourceRamByVarnode;
    for (const PcodeOpView &op : program.Ops) {
      if (op.Opcode == PcodeOpcode::Load) {
        trackLoadSourceRam(sourceRamByVarnode, op);
      }
      trackCopySourceRam(sourceRamByVarnode, op);

      std::optional<uint64_t> target = directRamTarget(op);
      if (op.Opcode == PcodeOpcode::Call) {
        if (target && state.isExecutableAddress(*target)) {
          DecodedFlowInfo &info = result.FlowInfos[op.Address];
          if (state.lookupPltExternal(*target)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                           NativeXrefKind::Call, "sleigh-pcode-plt-call");
            addUniqueAddress(info.CallTargets, *target);
            continue;
          }
          addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                         NativeXrefKind::Call, "sleigh-pcode-direct-call");
          addUniqueAddress(info.CallTargets, *target);
          addUniqueAddress(info.InternalCallTargets, *target);
        }
      } else if (op.Opcode == PcodeOpcode::CallInd) {
        result.FlowInfos[op.Address].HasIndirectCall = true;
        if (op.Inputs.size() == 1 && op.Inputs[0].Space == "ram") {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, op.Inputs[0].Offset)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Call,
                           "sleigh-pcode-pointer-indirect-call");
            continue;
          }
        }
        if (auto gotAddress = callIndGotSource(sourceRamByVarnode, op)) {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Call,
                           "sleigh-pcode-pointer-indirect-call");
            continue;
          }
          if (isExternalFunctionPointerRelocationAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Call,
                           "sleigh-pcode-got-indirect-call");
            continue;
          }
        }
        NativeUnresolvedFlow flow;
        flow.Address = op.Address;
        flow.Kind = NativeUnresolvedFlowKind::IndirectCall;
        flow.Source = "sleigh-pcode-indirect-flow";
        result.UnresolvedFlows.push_back(std::move(flow));
      } else if (op.Opcode == PcodeOpcode::Branch ||
                 op.Opcode == PcodeOpcode::CBranch) {
        if (!collectMachineFlow) {
          continue;
        }
        DecodedFlowInfo &info = result.FlowInfos[op.Address];
        if (op.Opcode == PcodeOpcode::CBranch) {
          info.HasConditionalBranch = true;
        } else {
          info.HasUnconditionalBranch = true;
        }
        if (target && state.isExecutableAddress(*target)) {
          if (state.lookupPltExternal(*target)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-plt-tail-branch");
            addUniqueAddress(info.BranchTargets, *target);
            continue;
          }
          addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                         NativeXrefKind::Flow, "sleigh-pcode-direct-flow");
          addUniqueAddress(info.BranchTargets, *target);
        }
      } else if (op.Opcode == PcodeOpcode::BranchInd) {
        if (!collectMachineFlow) {
          continue;
        }
        result.FlowInfos[op.Address].HasIndirectBranch = true;
        if (op.Inputs.size() == 1 && op.Inputs[0].Space == "ram") {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, op.Inputs[0].Offset)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-pointer-indirect-branch");
            continue;
          }
        }
        if (auto gotAddress = branchIndGotTarget(op)) {
          if (isPltGotSlot(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-plt-indirect-branch");
            continue;
          }
          if (isPlt0ResolverSlot(state, op.Address, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-plt0-resolver-branch");
            continue;
          }
        }
        // Guarded external tail jumps load the GOT slot into a register before
        // BRANCHIND. Only accept slots proven by relocation as external
        // function pointers.
        if (auto gotAddress = branchIndGotSource(sourceRamByVarnode, op)) {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-pointer-indirect-branch");
            continue;
          }
          if (isExternalFunctionPointerRelocationAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-got-indirect-branch");
            continue;
          }
        }
        NativeUnresolvedFlow flow;
        flow.Address = op.Address;
        flow.Kind = NativeUnresolvedFlowKind::IndirectBranch;
        flow.Source = "sleigh-pcode-indirect-flow";
        result.UnresolvedFlows.push_back(std::move(flow));
      } else if (op.Opcode == PcodeOpcode::Return) {
        if (collectMachineFlow) {
          result.FlowInfos[op.Address].HasReturn = true;
        }
      } else {
        addPendingDirectDataXrefs(state, result.Xrefs, seenXrefs, op);
      }
    }
    return result;
  }

  static void trackCopySourceRam(std::map<std::string, uint64_t> &sources,
                                 const PcodeOpView &op) {
    if (op.Opcode != PcodeOpcode::Copy || !op.Output ||
        op.Inputs.size() != 1) {
      return;
    }

    std::string outputKey = varnodeStorageKey(*op.Output);
    if (auto source = sourceRam(sources, op.Inputs[0])) {
      sources[outputKey] = *source;
      return;
    }
    sources.erase(outputKey);
  }

  static void trackLoadSourceRam(std::map<std::string, uint64_t> &sources,
                                 const PcodeOpView &op) {
    if (op.Opcode != PcodeOpcode::Load || !op.Output ||
        op.Inputs.size() != 2 || op.Inputs[0].Space != "const") {
      return;
    }

    std::string outputKey = varnodeStorageKey(*op.Output);
    if (auto source = sourceRam(sources, op.Inputs[1])) {
      sources[outputKey] = *source;
      return;
    }
    sources.erase(outputKey);
  }

  static std::optional<uint64_t>
  callIndGotSource(const std::map<std::string, uint64_t> &sources,
                   const PcodeOpView &op) {
    if (op.Inputs.size() != 1) {
      return std::nullopt;
    }
    return sourceRam(sources, op.Inputs[0]);
  }

  static std::optional<uint64_t> branchIndGotTarget(const PcodeOpView &op) {
    if (op.Inputs.size() != 1 || op.Inputs[0].Space != "ram") {
      return std::nullopt;
    }
    return op.Inputs[0].Offset;
  }

  static std::optional<uint64_t>
  branchIndGotSource(const std::map<std::string, uint64_t> &sources,
                     const PcodeOpView &op) {
    if (op.Inputs.size() != 1) {
      return std::nullopt;
    }
    return sourceRam(sources, op.Inputs[0]);
  }

  static std::optional<uint64_t>
  resolvedExecutablePointerTargetAt(const NativeProgramState &state,
                                    uint64_t address) {
    std::optional<uint64_t> target = state.readPointer(address);
    if (!target || !state.isExecutableAddress(*target)) {
      return std::nullopt;
    }
    return target;
  }

  static std::optional<uint64_t>
  sourceRam(const std::map<std::string, uint64_t> &sources,
            const VarnodeView &varnode) {
    if (varnode.Space == "ram") {
      return varnode.Offset;
    }
    auto it = sources.find(varnodeStorageKey(varnode));
    if (it == sources.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  static std::string varnodeStorageKey(const VarnodeView &varnode) {
    std::ostringstream stream;
    stream << varnode.Space << ':' << std::hex << varnode.Offset << ':'
           << std::dec << varnode.Size;
    return stream.str();
  }

  static bool isPltGotSlot(const NativeProgramState &state, uint64_t address) {
    for (const NativePltEntry &entry : state.pltEntries()) {
      if (entry.GotAddress == address && !entry.SymbolName.empty()) {
        return true;
      }
    }
    return false;
  }

  static bool isPlt0ResolverSlot(const NativeProgramState &state,
                                 uint64_t branchAddress, uint64_t gotAddress) {
    std::optional<NativeSectionInfo> plt = findSection(state, ".plt");
    std::optional<NativeSectionInfo> got = findSection(state, ".got");
    if (!plt || !got) {
      return false;
    }
    return branchAddress == plt->Address + 6 && gotAddress == got->Address + 16;
  }

  static void addPendingDirectDataXrefs(
      NativeProgramState &state, std::vector<NativeXref> &xrefs,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      const PcodeOpView &op) {
    if (op.Output) {
      addPendingDirectDataXref(state, xrefs, seenXrefs, op.Address, *op.Output);
    }
    for (const VarnodeView &input : op.Inputs) {
      addPendingDirectDataXref(state, xrefs, seenXrefs, op.Address, input);
    }
  }

  static void addPendingDirectDataXref(
      NativeProgramState &state, std::vector<NativeXref> &xrefs,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      uint64_t from, const VarnodeView &varnode) {
    if (varnode.Space != "ram" || state.isExecutableAddress(varnode.Offset)) {
      return;
    }
    NativeXrefKind kind = looksLikeReadOnlyCString(state, varnode.Offset)
                              ? NativeXrefKind::String
                              : NativeXrefKind::Data;
    const char *source = kind == NativeXrefKind::String
                             ? "sleigh-pcode-direct-string"
                             : "sleigh-pcode-direct-data";
    addPendingXref(xrefs, seenXrefs, from, varnode.Offset, kind, source);
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

  static void addPendingXref(
      std::vector<NativeXref> &xrefs,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      uint64_t from, uint64_t to, NativeXrefKind kind, const char *source) {
    if (!seenXrefs.insert({from, to, kind}).second) {
      return;
    }
    NativeXref xref;
    xref.From = from;
    xref.To = to;
    xref.Kind = kind;
    xref.Source = source;
    xrefs.push_back(std::move(xref));
  }

  static std::vector<NativeBasicBlock>
  buildDecodedBlocks(const std::vector<NativeInstruction> &instructions,
                     uint64_t rangeStart, uint64_t rangeEnd) {
    std::vector<NativeBasicBlock> blocks;
    if (instructions.empty()) {
      return blocks;
    }

    std::set<uint64_t> instructionStarts;
    for (const NativeInstruction &instruction : instructions) {
      instructionStarts.insert(instruction.Address);
    }

    std::set<uint64_t> blockStarts;
    blockStarts.insert(rangeStart);
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction &instruction = instructions[index];
      for (uint64_t target : instruction.DirectFlowTargets) {
        if (instructionStarts.count(target) != 0) {
          blockStarts.insert(target);
        }
      }
      if (index + 1 != instructions.size() &&
          instruction.FlowKind != NativeInstructionFlowKind::None) {
        blockStarts.insert(instructions[index + 1].Address);
      }
    }

    uint64_t blockStart = rangeStart;
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction &instruction = instructions[index];
      uint64_t instructionEnd = instruction.Address + instruction.Size;
      bool isLastInstruction = index + 1 == instructions.size();
      bool nextStartsBlock =
          !isLastInstruction &&
          blockStarts.count(instructions[index + 1].Address) != 0;

      std::vector<uint64_t> successors;
      bool endBlock = isLastInstruction || nextStartsBlock;
      if (instruction.FlowKind != NativeInstructionFlowKind::None) {
        if (instruction.FlowKind ==
            NativeInstructionFlowKind::ConditionalBranch) {
          successors = instruction.DirectFlowTargets;
          if (instruction.Fallthrough &&
              isInstructionFallthroughTo(instruction,
                                         *instruction.Fallthrough)) {
            addUniqueAddress(successors, *instruction.Fallthrough);
          }
        } else if (instruction.FlowKind ==
                   NativeInstructionFlowKind::UnconditionalBranch) {
          successors = instruction.DirectFlowTargets;
        } else if (instruction.FlowKind ==
                   NativeInstructionFlowKind::IndirectBranch) {
          successors = instruction.DirectFlowTargets;
        } else if (instruction.FlowKind == NativeInstructionFlowKind::Trap) {
          successors.clear();
        }
        endBlock = true;
      }
      if (nextStartsBlock && successors.empty() &&
          instruction.FlowKind == NativeInstructionFlowKind::None &&
          isInstructionFallthroughTo(instruction,
                                     instructions[index + 1].Address)) {
        addUniqueAddress(successors, instructions[index + 1].Address);
      }

      if (!endBlock) {
        continue;
      }

      NativeBasicBlock block;
      block.Start = blockStart;
      block.End = std::min(instructionEnd, rangeEnd);
      block.Successors = std::move(successors);
      if (block.Start < block.End) {
        blocks.push_back(std::move(block));
      }
      if (!isLastInstruction) {
        blockStart = instructions[index + 1].Address;
      }
    }
    return blocks;
  }

  static void
  eraseOutOfRangeFunctionSuccessors(const NativeProgramState &state,
                                    uint64_t entry,
                                    std::vector<uint64_t> &successors) {
    successors.erase(
        std::remove_if(successors.begin(), successors.end(),
                       [&](uint64_t successor) {
                         return !targetBelongsToFunctionRange(state, entry,
                                                              successor);
                       }),
        successors.end());
  }

  static void
  eraseKnownOtherFunctionSuccessors(NativeProgramState &state, uint64_t entry,
                                    std::vector<uint64_t> &successors) {
    successors.erase(
        std::remove_if(successors.begin(), successors.end(),
                       [&](uint64_t successor) {
                         return isKnownOtherFunctionEntry(state, entry,
                                                          successor);
                       }),
        successors.end());
  }

  static bool isKnownOtherFunctionEntry(const NativeProgramState &state,
                                        uint64_t entry, uint64_t address) {
    if (address == entry) {
      return false;
    }
    auto seedIterator = state.functionSeeds().find(address);
    return seedIterator != state.functionSeeds().end() &&
           seedIterator->second.IsEntry &&
           isBoundarySeed(seedIterator->second);
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

  static bool targetBelongsToFunctionRange(const NativeProgramState &state,
                                           uint64_t functionEntry,
                                           uint64_t target) {
    auto seedIterator = state.functionSeeds().find(functionEntry);
    if (seedIterator == state.functionSeeds().end()) {
      return true;
    }

    const NativeFunctionSeed &seed = seedIterator->second;
    if (seed.RangeStart == 0 || seed.RangeEnd <= seed.RangeStart) {
      return true;
    }
    return target >= seed.RangeStart && target < seed.RangeEnd;
  }

  static uint64_t boundedBytesForFunctionSeed(const NativeProgramState &state,
                                              uint64_t functionEntry,
                                              uint64_t blockAddress,
                                              uint64_t availableBytes) {
    auto seedIterator = state.functionSeeds().find(functionEntry);
    if (seedIterator == state.functionSeeds().end()) {
      return capBytesAtNextFunctionSeed(state, blockAddress, availableBytes);
    }

    const NativeFunctionSeed &seed = seedIterator->second;
    // Known seed ranges come from stronger boundaries such as symbol sizes or
    // .eh_frame FDEs.  Use them as a decode cap, but only when the queued block
    // is actually inside the recorded half-open range.
    if (seed.RangeStart == 0 || seed.RangeEnd <= seed.RangeStart ||
        blockAddress < seed.RangeStart || blockAddress >= seed.RangeEnd) {
      return capBytesAtNextFunctionSeed(state, blockAddress, availableBytes);
    }
    return std::min(availableBytes, seed.RangeEnd - blockAddress);
  }

  static uint64_t capBytesAtNextFunctionSeed(const NativeProgramState &state,
                                             uint64_t blockAddress,
                                             uint64_t availableBytes) {
    uint64_t cappedBytes = availableBytes;
    for (auto iterator = state.functionSeeds().upper_bound(blockAddress);
         iterator != state.functionSeeds().end(); ++iterator) {
      const auto &[address, seed] = *iterator;
      if (!isBoundarySeed(seed)) {
        continue;
      }
      uint64_t distance = address - blockAddress;
      if (distance < cappedBytes) {
        cappedBytes = distance;
      }
      break;
    }
    return cappedBytes;
  }

  static bool isBoundarySeed(const NativeFunctionSeed &seed) {
    return seed.Confidence != NativeFunctionConfidence::Low;
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
    std::map<NativeUnresolvedFlowKind, uint64_t> unresolvedFlowCounts;
    for (const NativeUnresolvedFlow &flow : state.unresolvedFlows()) {
      ++unresolvedFlowCounts[flow.Kind];
    }
    Output << "  unresolved indirect flows:\n";
    Output << "    total: " << state.unresolvedFlows().size() << '\n';
    for (NativeUnresolvedFlowKind kind :
         {NativeUnresolvedFlowKind::IndirectCall,
          NativeUnresolvedFlowKind::IndirectBranch,
          NativeUnresolvedFlowKind::IndirectTailBranch}) {
      Output << "    " << toString(kind) << ": "
             << unresolvedFlowCounts[kind] << '\n';
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

class X86JumpTableAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "X86JumpTableAnalyzer"; }
  int priority() const override { return 70; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch) {
        continue;
      }
      recoverJumpTableAt(state, flow.Address);
    }
  }

private:
  static void recoverJumpTableAt(NativeProgramState &state,
                                 uint64_t branchAddress) {
    bool completeForAnyFunction = false;
    for (const auto &[entry, function] : state.functions()) {
      (void)entry;
      if (!functionHasBlockContaining(function, branchAddress)) {
        continue;
      }
      completeForAnyFunction |=
          recoverJumpTableInFunction(state, function, branchAddress);
    }
    if (completeForAnyFunction) {
      state.removeUnresolvedFlow(branchAddress,
                                 NativeUnresolvedFlowKind::IndirectBranch);
    }
  }

  static bool recoverJumpTableInFunction(NativeProgramState &state,
                                         const NativeFunction &function,
                                         uint64_t branchAddress) {
    std::optional<X86PicI32JumpDispatch> dispatch =
        matchX86PicI32OffsetDispatch(state, function, branchAddress);
    if (!dispatch) {
      return false;
    }

    std::vector<uint64_t> targets;
    bool complete = false;
    if (!readX86PicI32Targets(state, dispatch->TableBase,
                              dispatch->EntryCount,
                              /*onlyExistingBlocks=*/false, targets,
                              complete)) {
      return false;
    }
    if (targets.empty()) {
      return false;
    }

    std::vector<uint64_t> decodedTargets;
    for (uint64_t target : targets) {
      if (state.instructionAt(target) != nullptr) {
        decodedTargets.push_back(target);
      }
    }
    if (decodedTargets.empty()) {
      return false;
    }

    state.addInstructionDirectFlowTargets(branchAddress, decodedTargets);
    std::vector<uint64_t> existingBlockTargets;
    for (uint64_t target : decodedTargets) {
      if (functionContainsBlockStart(state, target)) {
        existingBlockTargets.push_back(target);
      }
    }
    state.addBasicBlockSuccessors(function.Entry, dispatch->BlockStart,
                                  existingBlockTargets);
    for (uint64_t target : decodedTargets) {
      NativeXref xref;
      xref.From = branchAddress;
      xref.To = target;
      xref.Kind = NativeXrefKind::Flow;
      xref.Source = "x86-jump-table";
      state.addXref(std::move(xref));
    }
    (void)complete;
    return true;
  }

  static bool functionHasBlockContaining(const NativeFunction &function,
                                         uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (address >= block.Start && address < block.End) {
        return true;
      }
    }
    return false;
  }
};

class FlowFactNormalizer final : public NativeAnalyzer {
public:
  std::string name() const override { return "FlowFactNormalizer"; }
  int priority() const override { return 80; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (state.hasGtirbControlFlowAuthority()) {
      foldEhFrameOnlyBranchTargets(state);
      splitGtirbFlowXrefBlocks(state);
      restoreIntraFunctionFlowXrefSuccessors(state);
      recoverExternalFunctionPointerFlows(state);
      return;
    }

    std::vector<std::pair<uint64_t, NativeBasicBlock>> missingBlocks;
    for (const auto &[entry, function] : state.functions()) {
      appendMissingBlocks(state, function, missingBlocks);
    }
    for (auto &[entry, block] : missingBlocks) {
      state.addBasicBlock(entry, std::move(block));
    }
    std::vector<std::pair<uint64_t, NativeBasicBlock>> splitBlocks;
    for (const auto &[entry, function] : state.functions()) {
      appendFlowBoundarySplitBlocks(state, function, splitBlocks);
    }
    for (auto &[entry, block] : splitBlocks) {
      state.addBasicBlock(entry, std::move(block));
    }
    std::vector<std::pair<uint64_t, NativeBasicBlock>> targetBlocks;
    for (const auto &[entry, function] : state.functions()) {
      appendDecodedDirectTargetBlocks(state, function, targetBlocks);
    }
    for (auto &[entry, block] : targetBlocks) {
      state.addBasicBlock(entry, std::move(block));
    }
    foldEhFrameOnlyBranchTargets(state);
    for (const auto &[entry, function] : state.functions()) {
      state.removeInvalidBasicBlockSuccessors(entry);
      for (const NativeBasicBlock &block : function.Blocks) {
        normalizeBlockSuccessors(state, function, block);
      }
    }
    resolveCfgBackedIndirectBranches(state);
    classifyIndirectTailExits(state);
    recoverExternalFunctionPointerFlows(state);
  }

private:
  // GTIRB/ddisasm can resolve an indirect branch through CFG edges even when
  // SLEIGH p-code still reports BRANCHIND.  Keep the instruction-level flow
  // facts and unresolved diagnostics aligned with the already-known block CFG.
  static void resolveCfgBackedIndirectBranches(NativeProgramState &state) {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch) {
        continue;
      }

      const NativeFunction *function = state.functionContaining(flow.Address);
      if (function == nullptr) {
        continue;
      }
      const NativeBasicBlock *block =
          functionBlockContaining(*function, flow.Address);
      const NativeInstruction *instruction = state.instructionAt(flow.Address);
      if (block == nullptr || instruction == nullptr ||
          instruction->FlowKind != NativeInstructionFlowKind::IndirectBranch ||
          block->Successors.empty()) {
        continue;
      }

      std::vector<uint64_t> localTargets;
      for (uint64_t successor : block->Successors) {
        if (functionHasBlockStartingAt(*function, successor)) {
          localTargets.push_back(successor);
        }
      }
      if (localTargets.empty()) {
        continue;
      }

      state.addInstructionDirectFlowTargets(flow.Address, localTargets);
      state.removeUnresolvedFlow(flow.Address, flow.Kind);
    }
  }

  static void classifyIndirectTailExits(NativeProgramState &state) {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch) {
        continue;
      }

      const NativeFunction *function = state.functionContaining(flow.Address);
      if (function == nullptr) {
        continue;
      }
      const NativeBasicBlock *block =
          functionBlockContaining(*function, flow.Address);
      const NativeInstruction *instruction = state.instructionAt(flow.Address);
      if (block == nullptr || instruction == nullptr ||
          instruction->FlowKind != NativeInstructionFlowKind::IndirectBranch ||
          instruction->end() != block->End || block->End != function->RangeEnd ||
          !block->Successors.empty()) {
        continue;
      }

      NativeUnresolvedFlow tail = flow;
      tail.Kind = NativeUnresolvedFlowKind::IndirectTailBranch;
      tail.Source = "native-block-indirect-tail-exit";
      state.removeUnresolvedFlow(flow.Address, flow.Kind);
      state.addUnresolvedFlow(std::move(tail));
    }
  }

  static void recoverExternalFunctionPointerFlows(NativeProgramState &state) {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch &&
          flow.Kind != NativeUnresolvedFlowKind::IndirectCall &&
          flow.Kind != NativeUnresolvedFlowKind::IndirectTailBranch) {
        continue;
      }

      const NativeFunction *function = state.functionContaining(flow.Address);
      if (function == nullptr) {
        continue;
      }

      std::vector<NativeInstruction> instructions;
      std::map<uint64_t, size_t> indexByAddress;
      for (const NativeInstruction *instruction :
           state.instructionsInRange(function->RangeStart, function->RangeEnd)) {
        indexByAddress[instruction->Address] = instructions.size();
        instructions.push_back(*instruction);
      }
      auto indexIterator = indexByAddress.find(flow.Address);
      if (indexIterator == indexByAddress.end()) {
        continue;
      }

      std::optional<X86ExternalFunctionPointerMatch> match =
          matchX86ExternalFunctionPointerInstruction(state, instructions,
                                                     indexIterator->second);
      if (!match) {
        continue;
      }

      NativeXref xref;
      xref.From = flow.Address;
      xref.To = match->SlotAddress;
      xref.Kind = match->Kind;
      xref.Source = match->Kind == NativeXrefKind::Call
                        ? "x86-call-external-function-pointer"
                        : "x86-tail-branch-external-function-pointer";
      state.addXref(std::move(xref));
      state.removeUnresolvedFlow(flow.Address, flow.Kind);
    }
  }

  static void foldEhFrameOnlyBranchTargets(NativeProgramState &state) {
    std::vector<std::pair<uint64_t, uint64_t>> folds;
    for (const auto &[entry, function] : state.functions()) {
      for (const NativeBasicBlock &block : function.Blocks) {
        const NativeInstruction *terminator = nullptr;
        for (const NativeInstruction *instruction :
             state.instructionsInRange(block.Start, block.End)) {
          terminator = instruction;
        }
        if (terminator == nullptr ||
            terminator->FlowKind == NativeInstructionFlowKind::UnconditionalBranch) {
          continue;
        }
        std::vector<uint64_t> targets = terminator->DirectFlowTargets;
        if (terminator->FlowKind == NativeInstructionFlowKind::ConditionalBranch) {
          for (uint64_t target : terminator->TailFlowTargets) {
            addUniqueAddress(targets, target);
          }
        }
        for (uint64_t target : targets) {
          if (target == entry || functionHasBlockStartingAt(function, target)) {
            continue;
          }
          if (!isFoldableEhFrameBranchTarget(state, entry, target)) {
            continue;
          }
          folds.push_back({entry, target});
        }
      }
    }
    for (const NativeXref &xref : state.xrefs()) {
      if (xref.Kind != NativeXrefKind::Flow) {
        continue;
      }
      const NativeFunction *owner = state.functionContaining(xref.From);
      if (owner == nullptr ||
          !isFoldableEhFrameBranchTarget(state, owner->Entry, xref.To)) {
        continue;
      }
      folds.push_back({owner->Entry, xref.To});
    }
    for (const auto &[entry, function] : state.functions()) {
      auto seedIterator = state.functionSeeds().find(entry);
      if (seedIterator == state.functionSeeds().end() ||
          !hasSource(seedIterator->second, "eh-frame") ||
          seedIterator->second.Sources.size() != 1) {
        continue;
      }
      if (function.Source != "gtirb-seed-range-fallback") {
        continue;
      }
      for (const NativeBasicBlock &block : function.Blocks) {
        const NativeInstruction *terminator = nullptr;
        for (const NativeInstruction *instruction :
             state.instructionsInRange(block.Start, block.End)) {
          terminator = instruction;
        }
        if (terminator == nullptr ||
            terminator->FlowKind !=
                NativeInstructionFlowKind::UnconditionalBranch) {
          continue;
        }
        for (uint64_t target : terminator->TailFlowTargets) {
          const NativeFunction *owner = state.functionContaining(target);
          if (owner != nullptr && owner->Entry != entry) {
            folds.push_back({owner->Entry, entry});
          }
        }
      }
    }

    for (const auto &[ownerEntry, targetEntry] : folds) {
      const NativeFunction *targetFunction = state.functionAt(targetEntry);
      if (targetFunction == nullptr) {
        continue;
      }
      std::vector<NativeBasicBlock> blocks = targetFunction->Blocks;
      for (NativeBasicBlock &block : blocks) {
        state.addBasicBlock(ownerEntry, std::move(block));
      }
      restoreFoldedEhFrameFlowTargets(state, ownerEntry, targetEntry);
      state.demoteFunctionSeedToRangeHint(targetEntry);
      state.removeFunction(targetEntry);
    }
    restoreIntraFunctionFlowXrefSuccessors(state);
  }

  static bool isFoldableEhFrameBranchTarget(const NativeProgramState &state,
                                            uint64_t ownerEntry,
                                            uint64_t target) {
    const NativeFunction *targetFunction = state.functionAt(target);
    if (targetFunction == nullptr) {
      return false;
    }
    auto seedIterator = state.functionSeeds().find(target);
    if (seedIterator == state.functionSeeds().end() ||
        !hasSource(seedIterator->second, "eh-frame")) {
      return false;
    }
    if (targetFunction->Source == "gtirb-seed-range-fallback" &&
        seedIterator->second.Sources.size() == 1) {
      return true;
    }
    if (targetFunction->Source != "gtirb-ddisasm") {
      return false;
    }
    if (isNativeRuntimeFunction(state, *targetFunction)) {
      return false;
    }

    // ddisasm may split compiler generated cold blocks into separate
    // functions when .eh_frame has a separate FDE for the block.  Treat such a
    // range as a function boundary hint only when there is no real call edge to
    // the entry; otherwise it may be an address-taken function.
    std::optional<uint64_t> flowOwner;
    for (const NativeXref *xref : state.xrefsTo(target)) {
      if (xref->Kind == NativeXrefKind::Call) {
        return false;
      }
      if (xref->Kind == NativeXrefKind::Flow) {
        const NativeFunction *owner = state.functionContaining(xref->From);
        if (owner == nullptr) {
          return false;
        }
        if (flowOwner && *flowOwner != owner->Entry) {
          return false;
        }
        flowOwner = owner->Entry;
      }
    }
    return flowOwner && *flowOwner == ownerEntry;
  }

  static void restoreFoldedEhFrameFlowTargets(NativeProgramState &state,
                                              uint64_t ownerEntry,
                                              uint64_t foldedEntry) {
    const NativeFunction *owner = state.functionAt(ownerEntry);
    if (owner == nullptr) {
      return;
    }
    // Folding can split copied blocks again, so keep terminator addresses and
    // resolve their current source blocks only after all needed target splits.
    struct FoldedEdge {
      uint64_t TerminatorAddress = 0;
      uint64_t Target = 0;
      bool WasTail = false;
    };
    std::vector<FoldedEdge> edges;
    for (const NativeBasicBlock &block : owner->Blocks) {
      const NativeInstruction *terminator = nullptr;
      for (const NativeInstruction *instruction :
           state.instructionsInRange(block.Start, block.End)) {
        terminator = instruction;
      }
      if (terminator == nullptr) {
        continue;
      }
      for (uint64_t target : terminator->DirectFlowTargets) {
        edges.push_back({terminator->Address, target, false});
      }
      const std::vector<uint64_t> tailTargets = terminator->TailFlowTargets;
      for (uint64_t target : tailTargets) {
        edges.push_back({terminator->Address, target, true});
      }
    }
    for (const FoldedEdge &edge : edges) {
      if (edge.Target != foldedEntry) {
        ensureFunctionBlockStartsAt(state, ownerEntry, edge.Target);
      }
    }
    owner = state.functionAt(ownerEntry);
    if (owner == nullptr) {
      return;
    }
    for (const FoldedEdge &edge : edges) {
      if (edge.Target != foldedEntry &&
          !functionHasBlockStartingAt(*owner, edge.Target)) {
        continue;
      }
      const NativeBasicBlock *sourceBlock =
          functionBlockContaining(*owner, edge.TerminatorAddress);
      if (sourceBlock == nullptr) {
        continue;
      }
      if (edge.WasTail) {
        state.restoreInstructionTailFlowTarget(edge.TerminatorAddress,
                                               edge.Target);
      }
      state.addBasicBlockSuccessors(ownerEntry, sourceBlock->Start,
                                    {edge.Target});
    }
    for (const NativeXref *xref : state.xrefsTo(foldedEntry)) {
      if (xref->Kind != NativeXrefKind::Flow) {
        continue;
      }
      const NativeFunction *sourceOwner = state.functionContaining(xref->From);
      if (sourceOwner == nullptr || sourceOwner->Entry != ownerEntry) {
        continue;
      }
      const NativeBasicBlock *sourceBlock =
          functionBlockContaining(*owner, xref->From);
      if (sourceBlock == nullptr) {
        continue;
      }
      state.addBasicBlockSuccessors(ownerEntry, sourceBlock->Start,
                                    {foldedEntry});
    }
  }

  static void restoreIntraFunctionFlowXrefSuccessors(
      NativeProgramState &state) {
    for (const NativeXref &xref : state.xrefs()) {
      if (xref.Kind != NativeXrefKind::Flow) {
        continue;
      }
      const NativeFunction *sourceOwner = state.functionContaining(xref.From);
      const NativeFunction *targetOwner = state.functionContaining(xref.To);
      if (sourceOwner == nullptr || targetOwner == nullptr ||
          sourceOwner->Entry != targetOwner->Entry ||
          !functionHasBlockStartingAt(*sourceOwner, xref.To)) {
        continue;
      }
      const NativeBasicBlock *sourceBlock =
          functionBlockContaining(*sourceOwner, xref.From);
      if (sourceBlock == nullptr) {
        continue;
      }
      state.addBasicBlockSuccessors(sourceOwner->Entry, sourceBlock->Start,
                                    {xref.To});
    }
  }

  static void splitGtirbFlowXrefBlocks(NativeProgramState &state) {
    std::vector<std::pair<uint64_t, uint64_t>> splits;
    auto addSplit = [&](uint64_t entry, uint64_t address) {
      if (address == 0) {
        return;
      }
      auto item = std::make_pair(entry, address);
      if (std::find(splits.begin(), splits.end(), item) == splits.end()) {
        splits.push_back(item);
      }
    };

    for (const NativeXref &xref : state.xrefs()) {
      if (xref.Kind != NativeXrefKind::Flow ||
          xref.Source != "gtirb-ddisasm-flow") {
        continue;
      }
      const NativeFunction *sourceOwner = state.functionContaining(xref.From);
      const NativeFunction *targetOwner = state.functionContaining(xref.To);
      if (sourceOwner == nullptr || targetOwner == nullptr ||
          sourceOwner->Entry != targetOwner->Entry) {
        continue;
      }
      if (!functionHasBlockStartingAt(*sourceOwner, xref.From)) {
        addSplit(sourceOwner->Entry, xref.From);
      }
      if (!functionHasBlockStartingAt(*sourceOwner, xref.To)) {
        addSplit(sourceOwner->Entry, xref.To);
      }
    }

    for (const auto &[entry, address] : splits) {
      ensureFunctionBlockStartsAt(state, entry, address);
    }
  }

  static bool ensureFunctionBlockStartsAt(NativeProgramState &state,
                                          uint64_t functionEntry,
                                          uint64_t address) {
    const NativeFunction *function = state.functionAt(functionEntry);
    if (function == nullptr) {
      return false;
    }
    if (functionHasBlockStartingAt(*function, address)) {
      return true;
    }
    const NativeBasicBlock *containing =
        functionBlockContaining(*function, address);
    if (containing == nullptr || containing->Start == address) {
      return false;
    }

    NativeBasicBlock block;
    block.Start = address;
    block.End = containing->End;
    block.Successors = containing->Successors;
    state.addBasicBlock(functionEntry, std::move(block));

    function = state.functionAt(functionEntry);
    return function != nullptr && functionHasBlockStartingAt(*function, address);
  }

  static void appendMissingBlocks(
      const NativeProgramState &state, const NativeFunction &function,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    std::vector<const NativeInstruction *> instructions =
        state.instructionsInRange(function.RangeStart, function.RangeEnd);
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction *instruction = instructions[index];
      if (functionHasBlockContaining(function, instruction->Address)) {
        continue;
      }

      NativeBasicBlock block;
      block.Start = instruction->Address;
      size_t endIndex = index;
      while (endIndex < instructions.size()) {
        const NativeInstruction *current = instructions[endIndex];
        if (endIndex != index &&
            functionHasBlockContaining(function, current->Address)) {
          break;
        }
        block.End = current->end();
        if (current->FlowKind != NativeInstructionFlowKind::None) {
          addInstructionSuccessors(function, *current, block.Successors);
          break;
        }
        if (endIndex + 1 == instructions.size() ||
            !isInstructionFallthroughTo(
                *current, instructions[endIndex + 1]->Address)) {
          addInstructionSuccessors(function, *current, block.Successors);
          break;
        }
        ++endIndex;
      }

      if (block.Start < block.End) {
        result.push_back({function.Entry, std::move(block)});
      }
      index = endIndex;
    }
  }

  static void appendFlowBoundarySplitBlocks(
      const NativeProgramState &state, const NativeFunction &function,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    for (const NativeInstruction *instruction :
         state.instructionsInRange(function.RangeStart, function.RangeEnd)) {
      for (uint64_t target : instruction->DirectFlowTargets) {
        appendSplitBlockAt(function, target, result);
      }
      if (instruction->FlowKind == NativeInstructionFlowKind::ConditionalBranch &&
          instruction->Fallthrough &&
          isInstructionFallthroughTo(*instruction, *instruction->Fallthrough)) {
        appendSplitBlockAt(function, *instruction->Fallthrough, result);
      }
    }
  }

  static void appendDecodedDirectTargetBlocks(
      const NativeProgramState &state, const NativeFunction &function,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    // Some decoded cold blocks are outside the seed range but have already been
    // reached by a direct edge. Import only unowned decoded targets.
    for (const NativeInstruction *instruction :
         state.instructionsInRange(function.RangeStart, function.RangeEnd)) {
      for (uint64_t target : instruction->DirectFlowTargets) {
        appendDecodedDirectTargetBlockAt(state, function, target, result);
      }
    }
  }

  static void appendDecodedDirectTargetBlockAt(
      const NativeProgramState &state, const NativeFunction &function,
      uint64_t address,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    if (!state.isExecutableAddress(address) ||
        state.instructionAt(address) == nullptr ||
        functionHasBlockContaining(function, address)) {
      return;
    }
    if (auto owner = state.functionContaining(address)) {
      if (owner->Entry != function.Entry) {
        return;
      }
    }
    for (const auto &[entry, block] : result) {
      if (entry == function.Entry && block.Start == address) {
        return;
      }
    }

    std::vector<const NativeInstruction *> instructions =
        state.instructionsInRange(address, executableDecodeLimit(state, address));
    if (instructions.empty() || instructions.front()->Address != address) {
      return;
    }

    NativeBasicBlock block;
    block.Start = address;
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction *current = instructions[index];
      if (index != 0 && functionHasBlockContaining(function, current->Address)) {
        break;
      }
      block.End = current->end();
      if (current->FlowKind != NativeInstructionFlowKind::None) {
        break;
      }
      if (index + 1 == instructions.size() ||
          !isInstructionFallthroughTo(*current, instructions[index + 1]->Address)) {
        break;
      }
    }
    if (block.Start < block.End) {
      result.push_back({function.Entry, std::move(block)});
    }
  }

  static uint64_t executableDecodeLimit(const NativeProgramState &state,
                                        uint64_t address) {
    for (const NativeMemoryRange &range : state.memoryRanges()) {
      if (!range.Executable ||
          !containsAddress(range.Start, range.Size, address)) {
        continue;
      }
      return range.Start + range.Size;
    }
    return address;
  }

  static void appendSplitBlockAt(
      const NativeFunction &function, uint64_t address,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    if (functionHasBlockStartingAt(function, address)) {
      return;
    }
    const NativeBasicBlock *containing =
        functionBlockContaining(function, address);
    if (containing == nullptr || address == containing->Start) {
      return;
    }
    NativeBasicBlock block;
    block.Start = address;
    block.End = containing->End;
    block.Successors = containing->Successors;
    result.push_back({function.Entry, std::move(block)});
  }

  static void normalizeBlockSuccessors(NativeProgramState &state,
                                       const NativeFunction &function,
                                       const NativeBasicBlock &block) {
    const NativeInstruction *terminator = nullptr;
    for (const NativeInstruction *instruction :
         state.instructionsInRange(block.Start, block.End)) {
      terminator = instruction;
    }
    if (terminator == nullptr) {
      return;
    }

    if (terminator->FlowKind == NativeInstructionFlowKind::None &&
        terminator->Fallthrough &&
        isInstructionFallthroughTo(*terminator, *terminator->Fallthrough) &&
        functionHasBlockStartingAt(function, *terminator->Fallthrough)) {
      state.addBasicBlockSuccessors(function.Entry, block.Start,
                                    {*terminator->Fallthrough});
    }

    for (uint64_t target : terminator->DirectFlowTargets) {
      if (std::find(block.Successors.begin(), block.Successors.end(),
                    target) != block.Successors.end()) {
        continue;
      }
      if (functionHasBlockStartingAt(function, target)) {
        state.addBasicBlockSuccessors(function.Entry, block.Start, {target});
        continue;
      }
      state.markInstructionTailFlowTarget(terminator->Address, target);
    }
  }

  static bool functionHasBlockStartingAt(const NativeFunction &function,
                                         uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (block.Start == address) {
        return true;
      }
    }
    return false;
  }

  static bool functionHasBlockContaining(const NativeFunction &function,
                                         uint64_t address) {
    return functionBlockContaining(function, address) != nullptr;
  }

  static const NativeBasicBlock *
  functionBlockContaining(const NativeFunction &function, uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (address >= block.Start && address < block.End) {
        return &block;
      }
    }
    return nullptr;
  }

  static void addInstructionSuccessors(
      const NativeFunction &function, const NativeInstruction &instruction,
      std::vector<uint64_t> &successors) {
    if (instruction.FlowKind == NativeInstructionFlowKind::ConditionalBranch) {
      addLocalSuccessors(function, instruction.DirectFlowTargets, successors);
      if (instruction.Fallthrough &&
          isInstructionFallthroughTo(instruction, *instruction.Fallthrough)) {
        addLocalSuccessor(function, *instruction.Fallthrough, successors);
      }
      return;
    }
    if (instruction.FlowKind == NativeInstructionFlowKind::UnconditionalBranch ||
        instruction.FlowKind == NativeInstructionFlowKind::IndirectBranch) {
      addLocalSuccessors(function, instruction.DirectFlowTargets, successors);
      return;
    }
    if (instruction.FlowKind == NativeInstructionFlowKind::Trap) {
      return;
    }
    if (instruction.FlowKind == NativeInstructionFlowKind::None &&
        instruction.Fallthrough &&
        isInstructionFallthroughTo(instruction, *instruction.Fallthrough)) {
      addLocalSuccessor(function, *instruction.Fallthrough, successors);
    }
  }

  static void addLocalSuccessors(const NativeFunction &function,
                                 const std::vector<uint64_t> &targets,
                                 std::vector<uint64_t> &successors) {
    for (uint64_t target : targets) {
      addLocalSuccessor(function, target, successors);
    }
  }

  static void addLocalSuccessor(const NativeFunction &function, uint64_t target,
                                std::vector<uint64_t> &successors) {
    if (!functionHasBlockStartingAt(function, target)) {
      return;
    }
    addUniqueAddress(successors, target);
  }
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

std::unique_ptr<NativeAnalyzer> createEhFrameAnalyzer() {
  return std::make_unique<EhFrameAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createGtirbFunctionFactsAnalyzer(
    NativeGtirbDecodeOptions options) {
  return std::make_unique<GtirbFunctionFactsAnalyzer>(std::move(options));
}

std::unique_ptr<NativeAnalyzer> createSleighSeedInstructionAnalyzer(
    NativeSleighDecodeOptions options) {
  return std::make_unique<SleighSeedInstructionAnalyzer>(options);
}

std::unique_ptr<NativeAnalyzer> createX86JumpTableAnalyzer() {
  return std::make_unique<X86JumpTableAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createFlowFactNormalizer() {
  return std::make_unique<FlowFactNormalizer>();
}

std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output) {
  return std::make_unique<ReportAnalyzer>(output);
}

} // namespace notdec::bin2llvm
