#include "SleighBytes.h"

#include <sleigh/Support.h>
#include <sleigh/libsleigh.hh>

#include <cassert>
#include <cstdlib>
#include <exception>
#include <sstream>
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

std::optional<std::filesystem::path>
findSpecPath(const std::string &fileName,
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

std::string parseHexBytes(std::string_view hexBytes, uint64_t address,
                          uint64_t addressSize, std::ostream &errorStream) {
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

    const uint64_t addressMask = ~0ULL >> (64UL - addressSize * 8);
    uint64_t byteAddress = address + (index / 2);
    uint64_t maskedAddress = byteAddress & addressMask;
    if (maskedAddress < byteAddress || maskedAddress < address) {
      errorStream << "hex bytes exceed address width\n";
      return "";
    }

    buffer.push_back(static_cast<char>(byteValue));
  }
  return buffer;
}

VarnodeView convertVarnode(const ghidra::VarnodeData &data) {
  VarnodeView result;
  result.Space = data.space ? data.space->getName() : "";
  result.Offset = data.offset;
  result.Size = static_cast<uint32_t>(data.size);
  return result;
}

PcodeOpcode convertOpcode(ghidra::OpCode opcode) {
  switch (opcode) {
  case ghidra::CPUI_COPY:
    return PcodeOpcode::Copy;
  case ghidra::CPUI_LOAD:
    return PcodeOpcode::Load;
  case ghidra::CPUI_STORE:
    return PcodeOpcode::Store;
  case ghidra::CPUI_INT_EQUAL:
    return PcodeOpcode::IntEqual;
  case ghidra::CPUI_INT_NOTEQUAL:
    return PcodeOpcode::IntNotEqual;
  case ghidra::CPUI_INT_SLESS:
    return PcodeOpcode::IntSLess;
  case ghidra::CPUI_INT_LESS:
    return PcodeOpcode::IntLess;
  case ghidra::CPUI_INT_ZEXT:
    return PcodeOpcode::IntZExt;
  case ghidra::CPUI_INT_SEXT:
    return PcodeOpcode::IntSExt;
  case ghidra::CPUI_INT_ADD:
    return PcodeOpcode::IntAdd;
  case ghidra::CPUI_INT_SUB:
    return PcodeOpcode::IntSub;
  case ghidra::CPUI_INT_SBORROW:
    return PcodeOpcode::IntSBorrow;
  case ghidra::CPUI_INT_XOR:
    return PcodeOpcode::IntXor;
  case ghidra::CPUI_INT_AND:
    return PcodeOpcode::IntAnd;
  case ghidra::CPUI_INT_OR:
    return PcodeOpcode::IntOr;
  case ghidra::CPUI_INT_LEFT:
    return PcodeOpcode::IntLeft;
  case ghidra::CPUI_INT_RIGHT:
    return PcodeOpcode::IntRight;
  case ghidra::CPUI_INT_SRIGHT:
    return PcodeOpcode::IntSRight;
  case ghidra::CPUI_INT_MULT:
    return PcodeOpcode::IntMult;
  case ghidra::CPUI_PIECE:
    return PcodeOpcode::Piece;
  case ghidra::CPUI_SUBPIECE:
    return PcodeOpcode::Subpiece;
  case ghidra::CPUI_POPCOUNT:
    return PcodeOpcode::Popcount;
  default:
    return PcodeOpcode::Unsupported;
  }
}

void printVarData(std::ostream &os, const VarnodeView &data) {
  os << '(' << data.Space << ",0x" << std::hex << data.Offset << ',' << std::dec
     << data.Size << ')';
}

class PcodeCollector : public ghidra::PcodeEmit {
public:
  explicit PcodeCollector(PcodeProgram &program) : Program(program) {}

  void dump(const ghidra::Address &, ghidra::OpCode op,
            ghidra::VarnodeData *outVar, ghidra::VarnodeData *vars,
            int32_t inputCount) override {
    PcodeOpView view;
    view.Opcode = convertOpcode(op);
    view.OpcodeName = ghidra::get_opname(op);
    if (outVar) {
      view.Output = convertVarnode(*outVar);
    }
    view.Inputs.reserve(inputCount);
    for (int32_t index = 0; index < inputCount; ++index) {
      view.Inputs.push_back(convertVarnode(vars[index]));
    }
    Program.Ops.push_back(std::move(view));
  }

private:
  PcodeProgram &Program;
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

std::optional<std::filesystem::path>
findPspecPath(const SleighBytesOptions &options,
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
  auto slaFilePath = findSpecPath(options.SlaFileName, options.RootSlaDir);
  if (!slaFilePath) {
    errorStream << "could not find sla file: " << options.SlaFileName << '\n';
    return program;
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
    errorStream << "could not find pspec file: " << *options.PspecFileName
                << '\n';
    return program;
  }
  if (pspecPath) {
    ghidra::Element *pspecRoot =
        storage.openDocument(pspecPath->string())->getRoot();
    storage.registerTag(pspecRoot);
  }

  engine.initialize(storage);
  engine.allowContextSet(false);
  loadProcessorSpecContext(engine, context, storage);

  std::string imageBuffer = parseHexBytes(options.HexBytes, address,
                                          engine.getDefaultSize(), errorStream);
  if (imageBuffer.empty() && !options.HexBytes.empty()) {
    return program;
  }

  size_t length = imageBuffer.size();
  loadImage.setImageBuffer(std::move(imageBuffer));

  PcodeCollector collector(program);
  ghidra::Address current(engine.getDefaultCodeSpace(), address);
  ghidra::Address end(engine.getDefaultCodeSpace(), address + length);
  while (current < end) {
    try {
      int32_t instructionLength = engine.oneInstruction(collector, current);
      current = current + instructionLength;
    } catch (ghidra::UnimplError &error) {
      errorStream << "UnimplError @ " << current << ": " << error.explain
                  << '\n';
      program.Ops.clear();
      return program;
    } catch (ghidra::BadDataError &error) {
      errorStream << "BadDataError @ " << current << ": " << error.explain
                  << '\n';
      program.Ops.clear();
      return program;
    }
  }

  return program;
}

void printPcodeProgram(const PcodeProgram &program, std::ostream &os) {
  for (const PcodeOpView &op : program.Ops) {
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
