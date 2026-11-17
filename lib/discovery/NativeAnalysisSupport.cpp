#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

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

} // namespace notdec::bin2llvm
