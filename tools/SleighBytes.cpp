#include "SleighBytes.h"
#include "notdec-bin2llvm/SleighLift.h"

#include <sleigh/libsleigh.hh>

#include <cassert>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <utility>

namespace notdec::bin2llvm {
namespace {

class InMemoryLoadImage : public ghidra::LoadImage {
public:
  explicit InMemoryLoadImage(uint64_t baseAddress)
      : ghidra::LoadImage("notdec-sleigh-pcode"), baseAddress(baseAddress) {}

  void setImageBuffer(std::string &&buffer) {
    assert(imageBuffer.empty());
    imageBuffer = std::move(buffer);
  }

  void loadFill(unsigned char *ptr, int size,
                const ghidra::Address &addr) override {
    uint64_t start = addr.getOffset();
    for (int i = 0; i < size; ++i) {
      uint64_t offset = start + i;
      if (offset >= baseAddress) {
        offset -= baseAddress;
        ptr[i] = offset < imageBuffer.size() ? imageBuffer[offset] : 0;
      } else {
        ptr[i] = 0;
      }
    }
  }

  std::string getArchType(void) const override { return "memory"; }

  void adjustVma(long) override {}

private:
  uint64_t baseAddress;
  std::string imageBuffer;
};

std::string parseHexBytes(std::string_view hexBytes, uint64_t address,
                          std::ostream &errorStream) {
  std::string buffer;
  buffer.reserve(hexBytes.size() / 2);
  for (size_t index = 0; index < hexBytes.size(); index += 2) {
    char nibbleText[] = {hexBytes[index], hexBytes[index + 1], '\0'};
    char *parseEnd = nullptr;
    long byteValue = std::strtol(nibbleText, &parseEnd, 16);
    if (parseEnd != &nibbleText[2]) {
      errorStream << "invalid hex byte: " << nibbleText << '\n';
      return "";
    }

    uint64_t byteAddress = address + (index / 2);
    if (byteAddress < address) {
      errorStream << "hex bytes overflow address range\n";
      return "";
    }

    buffer.push_back(static_cast<char>(byteValue));
  }
  return buffer;
}

void printVarData(std::ostream &os, const VarnodeView &data) {
  os << '(' << data.Space << ",0x" << std::hex << data.Offset << ',' << std::dec
     << data.Size << ')';
}

} // namespace

std::optional<SleighBytesOptions>
parseSleighBytesOptions(int argc, char **argv, std::ostream &errorStream) {
  if (argc < 3) {
    return std::nullopt;
  }

  SleighBytesOptions options;
  options.SlaFileName = argv[1];
  options.HexBytes = argv[2];
  if (options.HexBytes.size() % 2 != 0) {
    errorStream << "hex-bytes must contain an even number of hex digits\n";
    return std::nullopt;
  }

  for (int argIndex = 3; argIndex < argc; ++argIndex) {
    std::string flag = argv[argIndex];
    if (argIndex + 1 >= argc) {
      errorStream << "flag has no value: " << flag << '\n';
      return std::nullopt;
    }

    std::string value = argv[++argIndex];
    if (flag == "-a") {
      try {
        options.Address = std::stoull(value, nullptr, 0);
      } catch (const std::exception &) {
        errorStream << "invalid address: " << value << '\n';
        return std::nullopt;
      }
    } else if (flag == "-p") {
      options.RootSlaDir = std::move(value);
    } else if (flag == "-s") {
      options.PspecFileName = std::move(value);
    } else {
      errorStream << "unknown flag: " << flag << '\n';
      return std::nullopt;
    }
  }

  return options;
}

PcodeProgram collectSleighPcode(const SleighBytesOptions &options,
                                std::ostream &errorStream) {
  PcodeProgram program;
  uint64_t address = options.Address.value_or(0);

  std::string imageBuffer = parseHexBytes(options.HexBytes, address,
                                          errorStream);
  if (imageBuffer.empty() && !options.HexBytes.empty()) {
    return program;
  }

  size_t length = imageBuffer.size();
  InMemoryLoadImage loadImage(address);
  loadImage.setImageBuffer(std::move(imageBuffer));

  SleighSpecOptions specOptions;
  specOptions.SlaFileName = options.SlaFileName;
  specOptions.RootSlaDir = options.RootSlaDir;
  specOptions.PspecFileName = options.PspecFileName;
  return collectSleighPcode(loadImage, specOptions, address, length,
                            errorStream);
}

void printPcodeProgram(const PcodeProgram &program, std::ostream &os, bool withAddress) {
  for (const PcodeOpView &op : program.Ops) {
    if (withAddress) {
      os << "0x" << std::hex << op.Address << std::dec << ": ";
    }
    if (op.Output) {
      printVarData(os, *op.Output);
      os << " = ";
    }

    os << op.OpcodeName;
    for (const VarnodeView &input : op.Inputs) {
      os << ' ';
      printVarData(os, input);
    }
    os << '\n';
  }
}

} // namespace notdec::bin2llvm
