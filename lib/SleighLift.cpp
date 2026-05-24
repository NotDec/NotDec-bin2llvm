#include "notdec-bin2llvm/SleighLift.h"

#include <sleigh/Support.h>
#include <sleigh/libsleigh.hh>

#include <fstream>
#include <limits>
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
  case ghidra::CPUI_CALL:
    return PcodeOpcode::Call;
  case ghidra::CPUI_CALLIND:
    return PcodeOpcode::CallInd;
  case ghidra::CPUI_CALLOTHER:
    return PcodeOpcode::CallOther;
  case ghidra::CPUI_BRANCH:
    return PcodeOpcode::Branch;
  case ghidra::CPUI_BRANCHIND:
    return PcodeOpcode::BranchInd;
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
  case ghidra::CPUI_INT_SLESSEQUAL:
    return PcodeOpcode::IntSLessEqual;
  case ghidra::CPUI_INT_LESS:
    return PcodeOpcode::IntLess;
  case ghidra::CPUI_INT_LESSEQUAL:
    return PcodeOpcode::IntLessEqual;
  case ghidra::CPUI_INT_ZEXT:
    return PcodeOpcode::IntZExt;
  case ghidra::CPUI_INT_SEXT:
    return PcodeOpcode::IntSExt;
  case ghidra::CPUI_INT_ADD:
    return PcodeOpcode::IntAdd;
  case ghidra::CPUI_INT_CARRY:
    return PcodeOpcode::IntCarry;
  case ghidra::CPUI_INT_SCARRY:
    return PcodeOpcode::IntSCarry;
  case ghidra::CPUI_INT_SUB:
    return PcodeOpcode::IntSub;
  case ghidra::CPUI_INT_SBORROW:
    return PcodeOpcode::IntSBorrow;
  case ghidra::CPUI_INT_2COMP:
    return PcodeOpcode::Int2Comp;
  case ghidra::CPUI_INT_NEGATE:
    return PcodeOpcode::IntNegate;
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
  case ghidra::CPUI_INT_DIV:
    return PcodeOpcode::IntDiv;
  case ghidra::CPUI_INT_SDIV:
    return PcodeOpcode::IntSDiv;
  case ghidra::CPUI_INT_REM:
    return PcodeOpcode::IntRem;
  case ghidra::CPUI_INT_SREM:
    return PcodeOpcode::IntSRem;
  case ghidra::CPUI_BOOL_NEGATE:
    return PcodeOpcode::BoolNegate;
  case ghidra::CPUI_BOOL_AND:
    return PcodeOpcode::BoolAnd;
  case ghidra::CPUI_BOOL_OR:
    return PcodeOpcode::BoolOr;
  case ghidra::CPUI_BOOL_XOR:
    return PcodeOpcode::BoolXor;
  case ghidra::CPUI_FLOAT_EQUAL:
    return PcodeOpcode::FloatEqual;
  case ghidra::CPUI_FLOAT_NOTEQUAL:
    return PcodeOpcode::FloatNotEqual;
  case ghidra::CPUI_FLOAT_LESS:
    return PcodeOpcode::FloatLess;
  case ghidra::CPUI_FLOAT_LESSEQUAL:
    return PcodeOpcode::FloatLessEqual;
  case ghidra::CPUI_FLOAT_NAN:
    return PcodeOpcode::FloatNan;
  case ghidra::CPUI_FLOAT_ADD:
    return PcodeOpcode::FloatAdd;
  case ghidra::CPUI_FLOAT_DIV:
    return PcodeOpcode::FloatDiv;
  case ghidra::CPUI_FLOAT_MULT:
    return PcodeOpcode::FloatMult;
  case ghidra::CPUI_FLOAT_SUB:
    return PcodeOpcode::FloatSub;
  case ghidra::CPUI_FLOAT_NEG:
    return PcodeOpcode::FloatNeg;
  case ghidra::CPUI_FLOAT_ABS:
    return PcodeOpcode::FloatAbs;
  case ghidra::CPUI_FLOAT_SQRT:
    return PcodeOpcode::FloatSqrt;
  case ghidra::CPUI_FLOAT_INT2FLOAT:
    return PcodeOpcode::FloatInt2Float;
  case ghidra::CPUI_FLOAT_FLOAT2FLOAT:
    return PcodeOpcode::FloatFloat2Float;
  case ghidra::CPUI_FLOAT_TRUNC:
    return PcodeOpcode::FloatTrunc;
  case ghidra::CPUI_FLOAT_CEIL:
    return PcodeOpcode::FloatCeil;
  case ghidra::CPUI_FLOAT_FLOOR:
    return PcodeOpcode::FloatFloor;
  case ghidra::CPUI_FLOAT_ROUND:
    return PcodeOpcode::FloatRound;
  case ghidra::CPUI_MULTIEQUAL:
    return PcodeOpcode::Multiequal;
  case ghidra::CPUI_INDIRECT:
    return PcodeOpcode::Indirect;
  case ghidra::CPUI_PIECE:
    return PcodeOpcode::Piece;
  case ghidra::CPUI_SUBPIECE:
    return PcodeOpcode::Subpiece;
  case ghidra::CPUI_CAST:
    return PcodeOpcode::Cast;
  case ghidra::CPUI_PTRADD:
    return PcodeOpcode::PtrAdd;
  case ghidra::CPUI_PTRSUB:
    return PcodeOpcode::PtrSub;
  case ghidra::CPUI_SEGMENTOP:
    return PcodeOpcode::SegmentOp;
  case ghidra::CPUI_CPOOLREF:
    return PcodeOpcode::CpoolRef;
  case ghidra::CPUI_NEW:
    return PcodeOpcode::New;
  case ghidra::CPUI_INSERT:
    return PcodeOpcode::Insert;
  case ghidra::CPUI_EXTRACT:
    return PcodeOpcode::Extract;
  case ghidra::CPUI_POPCOUNT:
    return PcodeOpcode::Popcount;
  case ghidra::CPUI_LZCOUNT:
    return PcodeOpcode::Lzcount;
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

class AssemblyCollector : public ghidra::AssemblyEmit {
public:
  void dump(const ghidra::Address &address, const std::string &mnemonic,
            const std::string &body) override {
    Last.Address = address.getOffset();
    Last.Mnemonic = mnemonic;
    Last.Body = body;
  }

  SleighInstructionSummary take(uint64_t size) {
    Last.Size = size;
    SleighInstructionSummary result = std::move(Last);
    Last = {};
    return result;
  }

private:
  SleighInstructionSummary Last;
};

class XmlCapableSleigh : public ghidra::Sleigh {
public:
  XmlCapableSleigh(ghidra::LoadImage *loadImage,
                   ghidra::ContextDatabase *context)
      : ghidra::Sleigh(loadImage, context) {}

  void decodeXmlSla(const ghidra::Element *element) {
    ghidra::XmlDecode decoder(this, element);
    decode(decoder);
  }
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

bool appendInstructionPcode(ghidra::Sleigh &engine, PcodeCollector &collector,
                            ghidra::Address &current,
                            std::ostream &errorStream, PcodeProgram &program) {
  try {
    int32_t instructionLength = engine.oneInstruction(collector, current);
    current = current + instructionLength;
    return true;
  } catch (ghidra::UnimplError &error) {
    errorStream << "UnimplError @ " << current << ": " << error.explain
                << '\n';
  } catch (ghidra::BadDataError &error) {
    errorStream << "BadDataError @ " << current << ": " << error.explain
                << '\n';
  }
  program.Ops.clear();
  return false;
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

bool initializeSleighEngine(ghidra::LoadImage &loadImage,
                            const SleighSpecOptions &options,
                            std::ostream &errorStream,
                            ghidra::ContextInternal &context,
                            XmlCapableSleigh &engine,
                            ghidra::DocumentStorage &storage,
                            std::filesystem::path &slaFilePath) {
  auto foundSlaFilePath =
      findSleighSpecPath(options.SlaFileName, options.RootSlaDir);
  if (!foundSlaFilePath) {
    errorStream << "could not find sla file: " << options.SlaFileName << '\n';
    return false;
  }
  slaFilePath = *foundSlaFilePath;
  bool isXmlSla = isXmlSlaFile(slaFilePath);

  ghidra::AttributeId::initialize();
  ghidra::ElementId::initialize();

  if (isXmlSla) {
    try {
      engine.decodeXmlSla(
          storage.openDocument(slaFilePath.string())->getRoot());
    } catch (ghidra::LowlevelError &error) {
      errorStream << "failed to decode XML .sla: " << error.explain << '\n';
      return false;
    }
  } else {
    std::istringstream sleighXml("<sleigh>" + slaFilePath.string() +
                                 "</sleigh>");
    ghidra::Element *root = storage.parseDocument(sleighXml)->getRoot();
    storage.registerTag(root);
  }

  auto pspecPath = findPspecPath(options, slaFilePath);
  if (options.PspecFileName && !pspecPath) {
    errorStream << "could not find pspec file: " << *options.PspecFileName
                << '\n';
    return false;
  }
  if (pspecPath) {
    ghidra::Element *pspecRoot =
        storage.openDocument(pspecPath->string())->getRoot();
    storage.registerTag(pspecRoot);
  }

  engine.initialize(storage);
  engine.allowContextSet(false);
  loadProcessorSpecContext(engine, context, storage);
  (void)loadImage;
  return true;
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
  ghidra::ContextInternal context;
  XmlCapableSleigh engine(&loadImage, &context);
  ghidra::DocumentStorage storage;
  std::filesystem::path slaFilePath;
  if (!initializeSleighEngine(loadImage, options, errorStream, context, engine,
                              storage, slaFilePath)) {
    return program;
  }
  program.IsBigEndian = engine.isBigEndian();
  collectRegisters(engine, program);

  PcodeCollector collector(engine, program);
  ghidra::Address current(engine.getDefaultCodeSpace(), address);
  ghidra::Address end(engine.getDefaultCodeSpace(), address + length);
  while (current < end) {
    if (!appendInstructionPcode(engine, collector, current, errorStream,
                                program)) {
      return program;
    }
  }

  return program;
}

PcodeProgram
collectSleighPcodeRanges(ghidra::LoadImage &loadImage,
                         const SleighSpecOptions &options,
                         const std::vector<std::pair<uint64_t, uint64_t>>
                             &ranges,
                         std::ostream &errorStream) {
  PcodeProgram program;
  ghidra::ContextInternal context;
  XmlCapableSleigh engine(&loadImage, &context);
  ghidra::DocumentStorage storage;
  std::filesystem::path slaFilePath;
  if (!initializeSleighEngine(loadImage, options, errorStream, context, engine,
                              storage, slaFilePath)) {
    return program;
  }
  program.IsBigEndian = engine.isBigEndian();
  collectRegisters(engine, program);

  PcodeCollector collector(engine, program);
  for (const auto &[start, endOffset] : ranges) {
    if (start >= endOffset) {
      continue;
    }

    ghidra::Address current(engine.getDefaultCodeSpace(), start);
    ghidra::Address end(engine.getDefaultCodeSpace(), endOffset);
    while (current < end) {
      if (!appendInstructionPcode(engine, collector, current, errorStream,
                                  program)) {
        return program;
      }
    }
  }

  return program;
}

std::vector<SleighInstructionSummary>
collectSleighInstructionSummaries(ghidra::LoadImage &loadImage,
                                  const SleighSpecOptions &options,
                                  uint64_t address, uint64_t maxInstructions,
                                  uint64_t maxBytes,
                                  std::ostream &errorStream) {
  return collectSleighInstructionDecode(loadImage, options, address,
                                        maxInstructions, maxBytes, errorStream)
      .Instructions;
}

SleighInstructionDecode
collectSleighInstructionDecode(ghidra::LoadImage &loadImage,
                               const SleighSpecOptions &options,
                               uint64_t address, uint64_t maxInstructions,
                               uint64_t maxBytes, std::ostream &errorStream) {
  SleighInstructionDecode decode;
  if (maxInstructions == 0 || maxBytes == 0) {
    return decode;
  }

  ghidra::ContextInternal context;
  XmlCapableSleigh engine(&loadImage, &context);
  ghidra::DocumentStorage storage;
  std::filesystem::path slaFilePath;
  if (!initializeSleighEngine(loadImage, options, errorStream, context, engine,
                              storage, slaFilePath)) {
    return decode;
  }
  decode.Pcode.IsBigEndian = engine.isBigEndian();
  collectRegisters(engine, decode.Pcode);

  AssemblyCollector collector;
  PcodeCollector pcodeCollector(engine, decode.Pcode);
  ghidra::Address current(engine.getDefaultCodeSpace(), address);
  if (address > std::numeric_limits<uint64_t>::max() - maxBytes) {
    maxBytes = std::numeric_limits<uint64_t>::max() - address;
  }
  ghidra::Address end(engine.getDefaultCodeSpace(), address + maxBytes);
  while (decode.Instructions.size() < maxInstructions && current < end) {
    try {
      int32_t instructionLength = engine.printAssembly(collector, current);
      if (instructionLength <= 0 ||
          static_cast<uint64_t>(instructionLength) >
              end.getOffset() - current.getOffset()) {
        break;
      }
      int32_t pcodeLength = engine.oneInstruction(pcodeCollector, current);
      if (pcodeLength != instructionLength) {
        errorStream << "Sleigh decode length mismatch @ " << current << ": "
                    << instructionLength << " vs " << pcodeLength << '\n';
        break;
      }
      decode.Instructions.push_back(
          collector.take(static_cast<uint64_t>(instructionLength)));
      current = current + instructionLength;
    } catch (ghidra::UnimplError &error) {
      errorStream << "UnimplError @ " << current << ": " << error.explain
                  << '\n';
      return decode;
    } catch (ghidra::BadDataError &error) {
      errorStream << "BadDataError @ " << current << ": " << error.explain
                  << '\n';
      return decode;
    }
  }

  return decode;
}

} // namespace notdec::bin2llvm
