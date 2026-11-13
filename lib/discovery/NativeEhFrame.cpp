#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

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

std::unique_ptr<NativeAnalyzer> createEhFrameAnalyzer() {
  return std::make_unique<EhFrameAnalyzer>();
}

} // namespace notdec::bin2llvm
