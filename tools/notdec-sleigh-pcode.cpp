#include <sleigh/Support.h>
#include <sleigh/libsleigh.hh>

#include <cassert>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " <sla-file> <hex-bytes> [-a address] [-p root-sla-dir] "
               "[-s pspec-file]\n";
}

// Keep the first sleigh integration narrow: one buffer, one base address, and
// no file loader abstraction beyond what Sleigh requires.
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

// One plain struct keeps the CLI boundary obvious and avoids spreading optional
// parsing state across the setup code.
struct CliOptions {
  std::string SlaFileName;
  std::string HexBytes;
  std::optional<uint64_t> Address;
  std::optional<std::string> RootSlaDir;
  std::optional<std::string> PspecFileName;
};

std::optional<std::filesystem::path> findSpecPath(
    const std::string &fileName,
    const std::optional<std::string> &rootDir) {
  std::filesystem::path directPath(fileName);
  if (std::filesystem::exists(directPath)) {
    return directPath;
  }

  if (rootDir) {
    return sleigh::FindSpecFile(fileName, {*rootDir});
  }

  return sleigh::FindSpecFile(fileName);
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc < 3) {
    return std::nullopt;
  }

  CliOptions options;
  options.SlaFileName = argv[1];
  options.HexBytes = argv[2];
  if (options.HexBytes.size() % 2 != 0) {
    std::cerr << "hex-bytes must contain an even number of hex digits\n";
    return std::nullopt;
  }

  for (int argIndex = 3; argIndex < argc; ++argIndex) {
    std::string flag = argv[argIndex];
    if (argIndex + 1 >= argc) {
      std::cerr << "flag has no value: " << flag << '\n';
      return std::nullopt;
    }

    std::string value = argv[++argIndex];
    if (flag == "-a") {
      try {
        options.Address = std::stoull(value, nullptr, 0);
      } catch (const std::exception &) {
        std::cerr << "invalid address: " << value << '\n';
        return std::nullopt;
      }
    } else if (flag == "-p") {
      options.RootSlaDir = std::move(value);
    } else if (flag == "-s") {
      options.PspecFileName = std::move(value);
    } else {
      std::cerr << "unknown flag: " << flag << '\n';
      return std::nullopt;
    }
  }

  return options;
}

std::string parseHexBytes(std::string_view hexBytes, uint64_t address,
                          uint64_t addressSize) {
  std::string buffer;
  buffer.reserve(hexBytes.size() / 2);
  for (size_t index = 0; index < hexBytes.size(); index += 2) {
    char nibbleText[] = {hexBytes[index], hexBytes[index + 1], '\0'};
    char *parseEnd = nullptr;
    long byteValue = std::strtol(nibbleText, &parseEnd, 16);
    if (parseEnd != &nibbleText[2]) {
      std::cerr << "invalid hex byte: " << nibbleText << '\n';
      std::exit(EXIT_FAILURE);
    }

    const uint64_t addressMask = ~0ULL >> (64UL - addressSize * 8);
    uint64_t byteAddress = address + (index / 2);
    uint64_t maskedAddress = byteAddress & addressMask;
    if (maskedAddress < byteAddress || maskedAddress < address) {
      std::cerr << "hex bytes exceed address width\n";
      std::exit(EXIT_FAILURE);
    }

    buffer.push_back(static_cast<char>(byteValue));
  }
  return buffer;
}

void printVarData(std::ostream &os, ghidra::VarnodeData &data) {
  os << '(' << data.space->getName() << ',';
  data.space->printOffset(os, data.offset);
  os << ',' << std::dec << data.size << ')';
}

class PcodePrinter : public ghidra::PcodeEmit {
public:
  void dump(const ghidra::Address &, ghidra::OpCode op,
            ghidra::VarnodeData *outVar, ghidra::VarnodeData *vars,
            int32_t inputCount) override {
    if (outVar) {
      printVarData(std::cout, *outVar);
      std::cout << " = ";
    }

    std::cout << get_opname(op);
    for (int32_t index = 0; index < inputCount; ++index) {
      std::cout << ' ';
      printVarData(std::cout, vars[index]);
    }
    std::cout << '\n';
  }
};

void loadProcessorSpecContext(ghidra::Sleigh &engine,
                              ghidra::ContextInternal &context,
                              ghidra::DocumentStorage &storage) {
  const ghidra::Element *element = storage.getTag("processor_spec");
  if (!element) {
    return;
  }

  ghidra::XmlDecode decoder(&engine, element);
  ghidra::uint4 elementId = decoder.openElement(ghidra::ELEM_PROCESSOR_SPEC);
  for (;;) {
    ghidra::uint4 subElementId = decoder.peekElement();
    if (subElementId == 0) {
      break;
    }
    if (subElementId == ghidra::ELEM_CONTEXT_DATA) {
      context.decodeFromSpec(decoder);
      break;
    }

    decoder.openElement();
    decoder.closeElementSkipping(subElementId);
  }
  decoder.closeElement(elementId);
}

std::optional<std::filesystem::path> findPspecPath(
    const CliOptions &options,
    const std::filesystem::path &slaFilePath) {
  if (options.PspecFileName) {
    return findSpecPath(*options.PspecFileName, options.RootSlaDir);
  }

  std::filesystem::path sibling = slaFilePath;
  sibling.replace_extension(".pspec");
  if (std::filesystem::exists(sibling)) {
    return sibling;
  }

  return std::nullopt;
}

int printPcode(const CliOptions &options) {
  auto slaFilePath = findSpecPath(options.SlaFileName, options.RootSlaDir);
  if (!slaFilePath) {
    std::cerr << "could not find sla file: " << options.SlaFileName << '\n';
    return 1;
  }

  uint64_t address = options.Address.value_or(0);

  ghidra::AttributeId::initialize();
  ghidra::ElementId::initialize();

  InMemoryLoadImage loadImage(address);
  ghidra::ContextInternal context;
  ghidra::Sleigh engine(&loadImage, &context);
  ghidra::DocumentStorage storage;

  std::istringstream sleighXml("<sleigh>" + slaFilePath->string() +
                               "</sleigh>");
  ghidra::Element *root = storage.parseDocument(sleighXml)->getRoot();
  storage.registerTag(root);

  auto pspecPath = findPspecPath(options, *slaFilePath);
  if (options.PspecFileName && !pspecPath) {
    std::cerr << "could not find pspec file: " << *options.PspecFileName
              << '\n';
    return 1;
  }
  if (pspecPath) {
    ghidra::Element *pspecRoot =
        storage.openDocument(pspecPath->string())->getRoot();
    storage.registerTag(pspecRoot);
  }

  engine.initialize(storage);
  engine.allowContextSet(false);
  loadProcessorSpecContext(engine, context, storage);

  std::string imageBuffer =
      parseHexBytes(options.HexBytes, address, engine.getDefaultSize());
  size_t length = imageBuffer.size();
  loadImage.setImageBuffer(std::move(imageBuffer));

  PcodePrinter printer;
  ghidra::Address current(engine.getDefaultCodeSpace(), address);
  ghidra::Address end(engine.getDefaultCodeSpace(), address + length);
  while (current < end) {
    try {
      int32_t instructionLength = engine.oneInstruction(printer, current);
      current = current + instructionLength;
    } catch (ghidra::UnimplError &error) {
      std::cerr << "UnimplError @ " << current << ": " << error.explain
                << '\n';
      return 1;
    } catch (ghidra::BadDataError &error) {
      std::cerr << "BadDataError @ " << current << ": " << error.explain
                << '\n';
      return 1;
    }
  }

  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  auto options = parseArgs(argc, argv);
  if (!options) {
    printUsage(argv[0]);
    return 1;
  }

  return printPcode(*options);
}
