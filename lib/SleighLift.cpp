#include "notdec-bin2llvm/SleighLift.h"

#include <sleigh/Support.h>
#include <sleigh/libsleigh.hh>

#include <fstream>
#include <map>
#include <sstream>

namespace notdec::bin2llvm {
namespace {

VarnodeView convertVarnode(const ghidra::Sleigh &engine,
                           const ghidra::VarnodeData &data) {
  VarnodeView result;
  result.Space = data.space ? data.space->getName() : "";
  result.Offset = data.offset;
  result.Size = static_cast<uint32_t>(data.size);
  if (data.space != nullptr) {
    std::string name =
        engine.getExactRegisterName(data.space, data.offset, data.size);
    if (name.empty()) {
      name = engine.getRegisterName(data.space, data.offset, data.size);
    }
    if (!name.empty()) {
      result.IsRegister = true;
      result.RegisterName = std::move(name);
    }
  }
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
  case ghidra::CPUI_BRANCH:
    return PcodeOpcode::Branch;
  case ghidra::CPUI_CBRANCH:
    return PcodeOpcode::CBranch;
  case ghidra::CPUI_RETURN:
    return PcodeOpcode::Return;
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
  case ghidra::CPUI_BOOL_NEGATE:
    return PcodeOpcode::BoolNegate;
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

class PcodeCollector : public ghidra::PcodeEmit {
public:
  PcodeCollector(const ghidra::Sleigh &engine, PcodeProgram &program)
      : Engine(engine), Program(program) {}

  void dump(const ghidra::Address &address, ghidra::OpCode op,
            ghidra::VarnodeData *outVar, ghidra::VarnodeData *vars,
            int32_t inputCount) override {
    PcodeOpView view;
    view.Address = address.getOffset();
    view.Opcode = convertOpcode(op);
    view.OpcodeName = ghidra::get_opname(op);
    if (outVar) {
      view.Output = convertVarnode(Engine, *outVar);
    }
    view.Inputs.reserve(inputCount);
    for (int32_t index = 0; index < inputCount; ++index) {
      view.Inputs.push_back(convertVarnode(Engine, vars[index]));
    }
    Program.Ops.push_back(std::move(view));
  }

private:
  const ghidra::Sleigh &Engine;
  PcodeProgram &Program;
};

void collectRegisters(const ghidra::Sleigh &engine, PcodeProgram &program) {
  std::map<ghidra::VarnodeData, std::string> registers;
  engine.getAllRegisters(registers);
  for (const auto &[varnode, name] : registers) {
    if (varnode.space == nullptr || name.empty()) {
      continue;
    }
    RegisterInfo info;
    info.Space = varnode.space->getName();
    info.Offset = varnode.offset;
    info.Size = static_cast<uint32_t>(varnode.size);
    info.Name = name;
    program.Registers.push_back(std::move(info));
  }
}

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
findPspecPath(const SleighSpecOptions &options,
              const std::filesystem::path &slaFilePath) {
  if (options.PspecFileName) {
    return findSleighSpecPath(*options.PspecFileName, options.RootSlaDir);
  }

  std::filesystem::path sibling = slaFilePath;
  sibling.replace_extension(".pspec");
  if (std::filesystem::exists(sibling)) {
    return sibling;
  }

  return std::nullopt;
}

bool isXmlSlaFile(const std::filesystem::path &slaFilePath) {
  std::ifstream input(slaFilePath);
  char first = '\0';
  input >> first;
  return first == '<';
}

} // namespace

std::optional<std::filesystem::path>
findSleighSpecPath(const std::string &fileName,
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

PcodeProgram collectSleighPcode(ghidra::LoadImage &loadImage,
                                const SleighSpecOptions &options,
                                uint64_t address, uint64_t length,
                                std::ostream &errorStream) {
  PcodeProgram program;
  auto slaFilePath = findSleighSpecPath(options.SlaFileName, options.RootSlaDir);
  if (!slaFilePath) {
    errorStream << "could not find sla file: " << options.SlaFileName << '\n';
    return program;
  }
  if (isXmlSlaFile(*slaFilePath)) {
    errorStream << "libsla expects a compressed .sla file, got XML .sla: "
                << slaFilePath->string() << '\n';
    return program;
  }

  ghidra::AttributeId::initialize();
  ghidra::ElementId::initialize();

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
  program.IsBigEndian = engine.isBigEndian();
  collectRegisters(engine, program);

  PcodeCollector collector(engine, program);
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

} // namespace notdec::bin2llvm
