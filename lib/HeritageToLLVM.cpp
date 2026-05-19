#include "notdec-bin2llvm/HeritageToLLVM.h"
#include "notdec-bin2llvm/RegisterStorage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace notdec::bin2llvm {
namespace {

unsigned bitWidth(uint32_t byteSize) {
  return byteSize == 0 ? 1 : byteSize * 8;
}

bool isIntLikeType(const std::string &type) {
  return type == "int" || type == "uint" || type == "undefined4";
}

bool isLikelyX86_64Language(const std::string &language) {
  return language.find("x86") != std::string::npos &&
         language.find("64") != std::string::npos;
}

struct X86_64RegisterAlias {
  const char *Name;
  const char *Base;
  uint32_t BaseSize;
  uint32_t ByteOffset;
};

const X86_64RegisterAlias *findX86_64RegisterAlias(const std::string &name) {
  static const X86_64RegisterAlias aliases[] = {
      {"EAX", "RAX", 8, 0},  {"AX", "RAX", 8, 0},   {"AL", "RAX", 8, 0},
      {"AH", "RAX", 8, 1},   {"EBX", "RBX", 8, 0},  {"BX", "RBX", 8, 0},
      {"BL", "RBX", 8, 0},   {"BH", "RBX", 8, 1},   {"ECX", "RCX", 8, 0},
      {"CX", "RCX", 8, 0},   {"CL", "RCX", 8, 0},   {"CH", "RCX", 8, 1},
      {"EDX", "RDX", 8, 0},  {"DX", "RDX", 8, 0},   {"DL", "RDX", 8, 0},
      {"DH", "RDX", 8, 1},   {"ESI", "RSI", 8, 0},  {"SI", "RSI", 8, 0},
      {"SIL", "RSI", 8, 0},  {"EDI", "RDI", 8, 0},  {"DI", "RDI", 8, 0},
      {"DIL", "RDI", 8, 0},  {"ESP", "RSP", 8, 0},  {"SP", "RSP", 8, 0},
      {"SPL", "RSP", 8, 0},  {"EBP", "RBP", 8, 0},  {"BP", "RBP", 8, 0},
      {"BPL", "RBP", 8, 0},  {"R8D", "R8", 8, 0},   {"R8W", "R8", 8, 0},
      {"R8B", "R8", 8, 0},   {"R9D", "R9", 8, 0},   {"R9W", "R9", 8, 0},
      {"R9B", "R9", 8, 0},   {"R10D", "R10", 8, 0}, {"R10W", "R10", 8, 0},
      {"R10B", "R10", 8, 0}, {"R11D", "R11", 8, 0}, {"R11W", "R11", 8, 0},
      {"R11B", "R11", 8, 0}, {"R12D", "R12", 8, 0}, {"R12W", "R12", 8, 0},
      {"R12B", "R12", 8, 0}, {"R13D", "R13", 8, 0}, {"R13W", "R13", 8, 0},
      {"R13B", "R13", 8, 0}, {"R14D", "R14", 8, 0}, {"R14W", "R14", 8, 0},
      {"R14B", "R14", 8, 0}, {"R15D", "R15", 8, 0}, {"R15W", "R15", 8, 0},
      {"R15B", "R15", 8, 0},
  };
  for (const X86_64RegisterAlias &alias : aliases) {
    if (name == alias.Name) {
      return &alias;
    }
  }
  return nullptr;
}

void printPoisonFallbackError(llvm::StringRef functionName,
                              const std::string &reason) {
  llvm::errs() << "Error: heritage lowering fell back to poison in "
               << functionName << ": " << reason << '\n';
}

llvm::Type *typeForSourceType(llvm::LLVMContext &context,
                              const std::string &type) {
  if (type == "void") {
    return llvm::Type::getVoidTy(context);
  }
  if (isIntLikeType(type)) {
    return llvm::IntegerType::get(context, 32);
  }
  if (type == "long" || type == "ulong" || type == "undefined8" ||
      type.find('*') != std::string::npos) {
    return llvm::IntegerType::get(context, 64);
  }
  if (type == "short" || type == "ushort" || type == "undefined2") {
    return llvm::IntegerType::get(context, 16);
  }
  if (type == "char" || type == "byte" || type == "undefined1") {
    return llvm::IntegerType::get(context, 8);
  }
  return llvm::IntegerType::get(context, 32);
}

std::string sanitizeSymbolName(const std::string &name) {
  std::string result;
  result.reserve(name.size());
  for (char ch : name) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) || ch == '_' || ch == '$' || ch == '.') {
      result.push_back(ch);
    } else {
      result.push_back('_');
    }
  }
  if (result.empty()) {
    return "";
  }
  if (std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

std::string addressSuffix(const std::string &address) {
  std::string text = address;
  size_t colon = text.rfind(':');
  if (colon != std::string::npos) {
    text = text.substr(colon + 1);
  }

  std::string result;
  for (char ch : text) {
    if (std::isxdigit(static_cast<unsigned char>(ch))) {
      result.push_back(static_cast<char>(std::tolower(ch)));
    }
  }
  return result.empty() ? "unknown" : result;
}

std::string uniqueSymbolName(const std::string &preferred,
                             const std::string &entry,
                             std::set<std::string> &usedNames) {
  std::string base = sanitizeSymbolName(preferred);
  if (base.empty()) {
    base = "sub_" + addressSuffix(entry);
  }
  if (usedNames.insert(base).second) {
    return base;
  }

  std::string withAddress = base + "_" + addressSuffix(entry);
  if (usedNames.insert(withAddress).second) {
    return withAddress;
  }

  unsigned index = 1;
  while (true) {
    std::string candidate = withAddress + "_" + std::to_string(index++);
    if (usedNames.insert(candidate).second) {
      return candidate;
    }
  }
}

llvm::FunctionType *
functionTypeForHeritageFunction(llvm::LLVMContext &context,
                                const HeritageFunction &function) {
  std::vector<llvm::Type *> paramTypes;
  for (const HeritageParam &param : function.Params) {
    paramTypes.push_back(typeForSourceType(context, param.Type));
  }

  llvm::Type *returnType = typeForSourceType(context, function.ReturnType);
  return llvm::FunctionType::get(returnType, paramTypes, false);
}

llvm::FunctionType *varargFunctionType(llvm::LLVMContext &context,
                                       const std::string &returnType) {
  return llvm::FunctionType::get(typeForSourceType(context, returnType), {},
                                 true);
}

struct HeritageModuleSymbolPlan {
  std::vector<std::string> InternalNames;
  std::vector<std::string> ExternalNames;
  std::unordered_map<std::string, std::string> NameByEntry;
  std::unordered_map<std::string, std::string> NameByOriginalName;
};

HeritageModuleSymbolPlan planModuleSymbols(const HeritageModule &module) {
  HeritageModuleSymbolPlan plan;
  std::set<std::string> usedNames;

  for (const HeritageModuleFunction &function : module.Functions) {
    const HeritageFunction &heritageFunction = function.Program.Function;
    std::string name = uniqueSymbolName(heritageFunction.Name,
                                        heritageFunction.Entry, usedNames);
    plan.NameByEntry.emplace(heritageFunction.Entry, name);
    plan.NameByOriginalName.emplace(heritageFunction.Name, name);
    plan.InternalNames.push_back(std::move(name));
  }

  for (const HeritageExternalFunction &external : module.Externals) {
    std::string name =
        uniqueSymbolName(external.Name, external.Address, usedNames);
    plan.NameByOriginalName.emplace(external.Name, name);
    plan.ExternalNames.push_back(std::move(name));
  }

  return plan;
}

std::vector<RegisterInfo>
registerInfosForHeritageProgram(const HeritageProgram &program) {
  std::vector<RegisterInfo> registers;
  bool useX86_64Aliases = isLikelyX86_64Language(program.Program.Language);
  for (const HeritageVarnode &varnode : program.Varnodes) {
    if (!varnode.IsRegister || !varnode.RegisterName) {
      continue;
    }
    RegisterInfo info;
    info.Space = varnode.Space;
    info.Offset = varnode.Offset;
    info.Size = varnode.Size;
    info.Name = *varnode.RegisterName;
    registers.push_back(std::move(info));

    // Build register storage from the largest architectural register.  P-Code
    // already models effects such as EAX zeroing the upper half of RAX; this
    // only decides which LLVM global backs the byte range.
    if (!useX86_64Aliases) {
      continue;
    }
    const X86_64RegisterAlias *alias =
        findX86_64RegisterAlias(*varnode.RegisterName);
    if (alias == nullptr || varnode.Offset < alias->ByteOffset) {
      continue;
    }
    RegisterInfo base;
    base.Space = varnode.Space;
    base.Offset = varnode.Offset - alias->ByteOffset;
    base.Size = alias->BaseSize;
    base.Name = alias->Base;
    registers.push_back(std::move(base));
  }
  return registers;
}

std::vector<RegisterInfo>
registerInfosForHeritageModule(const HeritageModule &module) {
  std::vector<RegisterInfo> registers;
  for (const HeritageModuleFunction &function : module.Functions) {
    std::vector<RegisterInfo> functionRegisters =
        registerInfosForHeritageProgram(function.Program);
    registers.insert(registers.end(), functionRegisters.begin(),
                     functionRegisters.end());
  }
  return registers;
}

std::string resolveCallTargetName(const HeritageOp &op,
                                  const HeritageModuleSymbolPlan *symbols) {
  if (symbols == nullptr) {
    return op.CallTargetName.value_or("");
  }
  if (op.CallTarget) {
    auto it = symbols->NameByEntry.find(*op.CallTarget);
    if (it != symbols->NameByEntry.end()) {
      return it->second;
    }
  }
  if (op.CallTargetName) {
    auto it = symbols->NameByOriginalName.find(*op.CallTargetName);
    if (it != symbols->NameByOriginalName.end()) {
      return it->second;
    }
    return *op.CallTargetName;
  }
  return "";
}

llvm::Function *declareInternalFunction(llvm::Module &module,
                                        const HeritageFunction &function,
                                        const std::string &name) {
  auto *functionType =
      functionTypeForHeritageFunction(module.getContext(), function);
  auto *llvmFunction = llvm::Function::Create(
      functionType, llvm::GlobalValue::ExternalLinkage, name, &module);

  unsigned index = 0;
  for (llvm::Argument &argument : llvmFunction->args()) {
    if (index < function.Params.size()) {
      argument.setName(sanitizeSymbolName(function.Params[index].Name));
    }
    ++index;
  }
  return llvmFunction;
}

class HeritageLowerer {
public:
  HeritageLowerer(llvm::LLVMContext &context, llvm::Module &module,
                  const HeritageProgram &program,
                  const HeritageModuleSymbolPlan *symbols = nullptr,
                  llvm::Function *function = nullptr,
                  RegisterStorage *registers = nullptr)
      : Context(context), Module(module), Program(program), Symbols(symbols),
        Builder(context), Function(function), Registers(registers) {
    if (Registers == nullptr) {
      OwnedRegisters = std::make_unique<RegisterStorage>(
          context, module, registerInfosForHeritageProgram(program), false);
      Registers = OwnedRegisters.get();
    }
  }

  bool lower(std::string &errorMessage) {
    if (!createFunction(errorMessage)) {
      return false;
    }
    createBlocks();
    createStackFrame();
    attachParameterMetadata();
    mapParameters(errorMessage);
    if (!errorMessage.empty()) {
      return false;
    }

    for (const HeritageBlock &block : Program.Blocks) {
      Builder.SetInsertPoint(BlockMap.at(block.Id));
      if (!lowerBlock(block, errorMessage)) {
        return false;
      }
    }
    return finalizePendingPhis(errorMessage);
  }

private:
  struct PendingPhi {
    llvm::PHINode *Phi = nullptr;
    std::vector<std::string> Predecessors;
    std::vector<std::string> Inputs;
    uint32_t OutputSize = 0;
  };

  // Many Ghidra INDIRECT ops appear before the STORE/CALL that their second
  // input references. Keep the lowered instruction until the effect op is
  // lowered, then attach metadata to both sides.
  struct PendingIndirectMetadata {
    llvm::Instruction *Instruction = nullptr;
    std::string EffectOp;
    std::string Input0;
  };

  // Ghidra heritage already rewrites normal RSP prologue/epilogue traffic into
  // frame-relative stack varnodes.  Keep those locals as one byte-addressed
  // alloca instead of reconstructing them through the current RSP value.
  struct StackFrame {
    int64_t Low = 0;
    int64_t High = 0;
    llvm::AllocaInst *Storage = nullptr;
  };

  llvm::Type *intType(uint32_t byteSize) {
    return llvm::IntegerType::get(Context, bitWidth(byteSize));
  }

  void warnPoisonFallback(const std::string &reason) {
    if (PoisonFallbackWarnings.insert(reason).second) {
      printPoisonFallbackError(Program.Function.Name, reason);
    }
  }

  void warnMissingParameterVarnode(const HeritageParam &param) {
    std::ostringstream os;
    os << "parameter " << param.Name << " has no varnode";
    if (param.RegisterName) {
      os << " (register " << *param.RegisterName << ")";
    }
    if (MissingParameterWarnings.insert(os.str()).second) {
      llvm::errs() << "Warning: " << os.str() << '\n';
    }
  }

  std::string describeCurrentOp() const {
    if (CurrentOp == nullptr) {
      return "";
    }
    std::ostringstream os;
    os << " op=" << CurrentOp->Id << " mnemonic=" << CurrentOp->Mnemonic
       << " block=" << CurrentOp->Parent;
    return os.str();
  }

  std::string describeVarnode(const HeritageVarnode &varnode) const {
    std::ostringstream os;
    os << varnode.Id << " space=" << varnode.Space
       << " address=" << varnode.Address << " offset=" << varnode.Offset
       << " size=" << varnode.Size << " isInput=" << varnode.IsInput
       << " isAddressTied=" << varnode.IsAddressTied;
    if (varnode.RegisterName) {
      os << " register=" << *varnode.RegisterName;
    }
    if (varnode.HighVariable) {
      os << " highVariable=" << *varnode.HighVariable;
    }
    if (varnode.HighType) {
      os << " highType=" << *varnode.HighType;
    }
    os << describeCurrentOp();
    return os.str();
  }

  llvm::Type *floatType(uint32_t byteSize) {
    if (byteSize == 4) {
      return llvm::Type::getFloatTy(Context);
    }
    if (byteSize == 8) {
      return llvm::Type::getDoubleTy(Context);
    }
    return nullptr;
  }

  uint32_t floatByteSize(llvm::Type *type) const {
    return type->isDoubleTy() ? 8 : 4;
  }

  llvm::Type *typeForSourceType(const std::string &type) {
    return ::notdec::bin2llvm::typeForSourceType(Context, type);
  }

  bool createFunction(std::string &errorMessage) {
    if (Function == nullptr) {
      Function = declareInternalFunction(Module, Program.Function,
                                         Program.Function.Name);
    }

    if (Program.Blocks.empty()) {
      errorMessage = "heritage program has no blocks";
      return false;
    }
    return true;
  }

  void createBlocks() {
    for (const HeritageBlock &block : Program.Blocks) {
      std::string name =
          block.Id == Program.Blocks.front().Id ? "entry" : block.Id;
      BlockMap.emplace(block.Id,
                       llvm::BasicBlock::Create(Context, name, Function));
    }
  }

  static int64_t signedOffset(uint64_t offset) {
    return static_cast<int64_t>(offset);
  }

  void createStackFrame() {
    int64_t low = 0;
    int64_t high = 0;
    bool hasLocalStack = false;
    for (const HeritageVarnode &varnode : Program.Varnodes) {
      if (varnode.Space != "stack" || varnode.Size == 0) {
        continue;
      }
      int64_t offset = signedOffset(varnode.Offset);
      if (offset >= 0) {
        continue;
      }
      int64_t end = offset + static_cast<int64_t>(varnode.Size);
      if (!hasLocalStack) {
        low = offset;
        high = std::max<int64_t>(0, end);
        hasLocalStack = true;
      } else {
        low = std::min(low, offset);
        high = std::max(high, end);
      }
    }
    if (!hasLocalStack || high <= low) {
      return;
    }

    uint64_t size = static_cast<uint64_t>(high - low);
    if (size > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
      return;
    }
    llvm::IRBuilder<> entryBuilder(&Function->getEntryBlock(),
                                   Function->getEntryBlock().begin());
    auto *byteType = llvm::Type::getInt8Ty(Context);
    auto *arrayType = llvm::ArrayType::get(byteType, size);
    Stack.Low = low;
    Stack.High = high;
    Stack.Storage =
        entryBuilder.CreateAlloca(arrayType, nullptr, "notdec_stack");
    Stack.Storage->setAlignment(llvm::Align(16));
  }

  void mapParameters(std::string &errorMessage) {
    auto arg = Function->arg_begin();
    for (const HeritageParam &param : Program.Function.Params) {
      if (arg == Function->arg_end()) {
        errorMessage = "LLVM function argument mismatch";
        return;
      }
      if (param.Varnode) {
        Values[*param.Varnode] = &*arg;
      } else {
        warnMissingParameterVarnode(param);
      }
      ++arg;
    }
  }

  void attachParameterMetadata() {
    std::vector<llvm::Metadata *> entries;
    for (const HeritageParam &param : Program.Function.Params) {
      if (param.Varnode || !param.RegisterName) {
        continue;
      }
      llvm::Metadata *fields[] = {
          llvm::MDString::get(Context, "index=" + std::to_string(param.Index)),
          llvm::MDString::get(Context, "name=" + param.Name),
          llvm::MDString::get(Context, "type=" + param.Type),
          llvm::MDString::get(Context, "storage=" + param.Storage),
          llvm::MDString::get(Context, "register=" + *param.RegisterName),
      };
      entries.push_back(llvm::MDNode::get(Context, fields));
    }
    if (!entries.empty()) {
      Function->setMetadata("notdec.param.register",
                            llvm::MDNode::get(Context, entries));
    }
  }

  llvm::Value *resize(llvm::Value *value, uint32_t byteSize) {
    llvm::Type *targetType = intType(byteSize);
    if (value->getType() == targetType) {
      return value;
    }

    unsigned sourceBits = value->getType()->getIntegerBitWidth();
    unsigned targetBits = targetType->getIntegerBitWidth();
    if (sourceBits < targetBits) {
      return Builder.CreateZExt(value, targetType);
    }
    return Builder.CreateTrunc(value, targetType);
  }

  llvm::Value *resizeToIntegerType(llvm::Value *value,
                                   llvm::Type *targetType) {
    if (value->getType() == targetType) {
      return value;
    }

    unsigned sourceBits = value->getType()->getIntegerBitWidth();
    unsigned targetBits = targetType->getIntegerBitWidth();
    if (sourceBits < targetBits) {
      return Builder.CreateZExt(value, targetType);
    }
    return Builder.CreateTrunc(value, targetType);
  }

  const HeritageVarnode *varnodeFor(const std::string &id) const {
    auto it = Program.VarnodeById.find(id);
    return it != Program.VarnodeById.end() ? it->second : nullptr;
  }

  llvm::Value *read(const std::string &id) {
    if (auto it = Values.find(id); it != Values.end()) {
      const HeritageVarnode *varnode = varnodeFor(id);
      return varnode ? resize(it->second, varnode->Size) : it->second;
    }

    const HeritageVarnode *varnode = varnodeFor(id);
    if (varnode == nullptr) {
      return nullptr;
    }
    if (varnode->IsConstant) {
      return llvm::ConstantInt::get(intType(varnode->Size), varnode->Offset);
    }
    if (varnode->IsRegister && varnode->RegisterName) {
      RegisterAccess access{varnode->Space, varnode->Offset, varnode->Size,
                            varnode->RegisterName};
      if (llvm::Value *value = Registers->read(Builder, access)) {
        return value;
      }
    }
    if (llvm::Value *value = readAddressTiedInput(Builder, *varnode)) {
      return value;
    }

    warnPoisonFallback("read unmodeled varnode " + describeVarnode(*varnode));
    return Builder.CreateFreeze(llvm::PoisonValue::get(intType(varnode->Size)),
                                id + "_in");
  }

  bool write(const std::string &id, llvm::Value *value,
             std::string &errorMessage) {
    const HeritageVarnode *varnode = varnodeFor(id);
    if (varnode == nullptr) {
      errorMessage = "unknown output varnode: " + id;
      return false;
    }
    llvm::Value *resized = resize(value, varnode->Size);
    Values[id] = resized;
    if (varnode->IsRegister && varnode->RegisterName) {
      RegisterAccess access{varnode->Space, varnode->Offset, varnode->Size,
                            varnode->RegisterName};
      if (Registers->hasRegister(access)) {
        Registers->write(Builder, access, resized);
      }
    }
    if (llvm::Value *pointer = pointerForStackVarnode(Builder, *varnode)) {
      auto *store = Builder.CreateStore(resized, pointer);
      store->setAlignment(llvm::Align(1));
    }
    return true;
  }

  void rememberOpInstruction(const HeritageOp &op, llvm::Instruction *inst) {
    if (inst == nullptr) {
      return;
    }
    OpInstructionById[op.Id] = inst;
    llvm::Metadata *metadata[] = {
        llvm::MDString::get(Context, "effectOp"),
        llvm::MDString::get(Context, op.Id),
        llvm::MDString::get(Context, "mnemonic"),
        llvm::MDString::get(Context, op.Mnemonic),
    };
    llvm::MDNode *node = llvm::MDNode::getDistinct(Context, metadata);
    EffectMetadataByOpId[op.Id] = node;
    inst->setMetadata("notdec.effect", node);
    attachPendingIndirectMetadata(op.Id);
  }

  void attachIndirectMetadata(const HeritageOp &op, llvm::Value *value) {
    if (!op.EffectOp || value == nullptr) {
      return;
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) {
      return;
    }
    auto effectIt = OpInstructionById.find(*op.EffectOp);
    if (effectIt == OpInstructionById.end() || effectIt->second == nullptr) {
      PendingIndirectsByEffect[*op.EffectOp].push_back(
          PendingIndirectMetadata{inst, *op.EffectOp,
                                  op.Inputs.empty() ? "" : op.Inputs[0]});
      return;
    }

    auto metadataIt = EffectMetadataByOpId.find(*op.EffectOp);
    if (metadataIt == EffectMetadataByOpId.end() || metadataIt->second == nullptr) {
      return;
    }

    std::vector<llvm::Metadata *> metadata = {
        llvm::MDString::get(Context, "effect"),
        metadataIt->second,
        llvm::MDString::get(Context, "effectOp"),
        llvm::MDString::get(Context, *op.EffectOp),
        llvm::MDString::get(Context, "input0"),
        llvm::MDString::get(Context, op.Inputs.empty() ? "" : op.Inputs[0]),
    };
    inst->setMetadata("notdec.indirect",
                      llvm::MDNode::get(Context, metadata));
  }

  void attachPendingIndirectMetadata(const std::string &effectOp) {
    auto pendingIt = PendingIndirectsByEffect.find(effectOp);
    if (pendingIt == PendingIndirectsByEffect.end()) {
      return;
    }
    for (const PendingIndirectMetadata &pending : pendingIt->second) {
      attachIndirectMetadata(pending.Instruction, pending.EffectOp,
                             pending.Input0);
    }
    PendingIndirectsByEffect.erase(pendingIt);
  }

  void attachIndirectMetadata(llvm::Instruction *inst,
                              const std::string &effectOp,
                              const std::string &input0) {
    auto effectIt = OpInstructionById.find(effectOp);
    if (effectIt == OpInstructionById.end() || effectIt->second == nullptr) {
      return;
    }
    auto metadataIt = EffectMetadataByOpId.find(effectOp);
    if (metadataIt == EffectMetadataByOpId.end() || metadataIt->second == nullptr) {
      return;
    }

    std::vector<llvm::Metadata *> metadata = {
        llvm::MDString::get(Context, "effect"),
        metadataIt->second,
        llvm::MDString::get(Context, "effectOp"),
        llvm::MDString::get(Context, effectOp),
        llvm::MDString::get(Context, "input0"),
        llvm::MDString::get(Context, input0),
    };
    inst->setMetadata("notdec.indirect",
                      llvm::MDNode::get(Context, metadata));
  }

  llvm::Value *readFloatBits(const std::string &id, llvm::Type *floatTy,
                             std::string &errorMessage) {
    llvm::Value *bits = read(id);
    if (bits == nullptr) {
      errorMessage = "FLOAT op reads unknown varnode: " + id;
      return nullptr;
    }
    return Builder.CreateBitCast(resize(bits, floatByteSize(floatTy)), floatTy);
  }

  bool writeFloatBits(const std::string &id, llvm::Value *value,
                      std::string &errorMessage) {
    llvm::Type *bitsTy = intType(floatByteSize(value->getType()));
    return write(id, Builder.CreateBitCast(value, bitsTy), errorMessage);
  }

  bool requireInputs(const HeritageOp &op, size_t count,
                     std::string &errorMessage) {
    if (op.Inputs.size() == count) {
      return true;
    }
    std::ostringstream os;
    os << op.Mnemonic << " needs " << count << " inputs, got "
       << op.Inputs.size();
    errorMessage = os.str();
    return false;
  }

  bool requireOutput(const HeritageOp &op, const HeritageVarnode *&output,
                     std::string &errorMessage) {
    if (!op.Output) {
      errorMessage = op.Mnemonic + " has no output";
      return false;
    }
    output = varnodeFor(*op.Output);
    if (output == nullptr) {
      errorMessage = op.Mnemonic + " output is unknown";
      return false;
    }
    return true;
  }

  bool constInput(const HeritageOp &op, size_t index, uint64_t &value,
                  std::string &errorMessage) {
    if (op.Inputs.size() <= index) {
      errorMessage = op.Mnemonic + " missing constant input";
      return false;
    }
    const HeritageVarnode *varnode = varnodeFor(op.Inputs[index]);
    if (varnode == nullptr || !varnode->IsConstant) {
      errorMessage =
          op.Mnemonic + " input " + std::to_string(index) + " must be constant";
      return false;
    }
    value = varnode->Offset;
    return true;
  }

  bool lowerCopy(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 1, errorMessage)) {
      return false;
    }
    llvm::Value *input = read(op.Inputs[0]);
    if (input == nullptr) {
      errorMessage = "COPY reads unknown varnode: " + op.Inputs[0];
      return false;
    }
    return write(*op.Output, input, errorMessage);
  }

  bool lowerCopyLike(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || op.Inputs.empty()) {
      errorMessage = op.Mnemonic + " needs output and at least one input";
      return false;
    }
    llvm::Value *input = read(op.Inputs[0]);
    if (input == nullptr) {
      errorMessage = op.Mnemonic + " reads unknown varnode: " + op.Inputs[0];
      return false;
    }
    return write(*op.Output, input, errorMessage);
  }

  bool lowerIndirect(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || op.Inputs.empty()) {
      errorMessage = "INDIRECT needs output and at least one input";
      return false;
    }
    llvm::Value *input = read(op.Inputs[0]);
    if (input == nullptr) {
      errorMessage = "INDIRECT reads unknown varnode: " + op.Inputs[0];
      return false;
    }
    if (!write(*op.Output, input, errorMessage)) {
      return false;
    }
    auto valueIt = Values.find(*op.Output);
    if (valueIt != Values.end()) {
      attachIndirectMetadata(op, valueIt->second);
    }
    return true;
  }

  bool lowerBinary(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    llvm::Value *lhs = resize(read(op.Inputs[0]), output->Size);
    llvm::Value *rhs = resize(read(op.Inputs[1]), output->Size);
    llvm::Value *result = nullptr;
    if (op.Mnemonic == "INT_ADD") {
      result = Builder.CreateAdd(lhs, rhs);
    } else if (op.Mnemonic == "INT_SUB") {
      result = Builder.CreateSub(lhs, rhs);
    } else if (op.Mnemonic == "INT_MULT") {
      result = Builder.CreateMul(lhs, rhs);
    } else if (op.Mnemonic == "INT_AND") {
      result = Builder.CreateAnd(lhs, rhs);
    } else if (op.Mnemonic == "INT_OR") {
      result = Builder.CreateOr(lhs, rhs);
    } else if (op.Mnemonic == "INT_XOR") {
      result = Builder.CreateXor(lhs, rhs);
    } else if (op.Mnemonic == "INT_LEFT") {
      result = Builder.CreateShl(lhs, rhs);
    } else if (op.Mnemonic == "INT_RIGHT") {
      result = Builder.CreateLShr(lhs, rhs);
    } else if (op.Mnemonic == "INT_SRIGHT") {
      result = Builder.CreateAShr(lhs, rhs);
    } else if (op.Mnemonic == "INT_DIV") {
      result = Builder.CreateUDiv(lhs, rhs);
    } else if (op.Mnemonic == "INT_SDIV") {
      result = Builder.CreateSDiv(lhs, rhs);
    } else if (op.Mnemonic == "INT_REM") {
      result = Builder.CreateURem(lhs, rhs);
    } else if (op.Mnemonic == "INT_SREM") {
      result = Builder.CreateSRem(lhs, rhs);
    } else {
      errorMessage = "unsupported binary opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerCompare(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    const HeritageVarnode *lhsVarnode = varnodeFor(op.Inputs[0]);
    if (lhsVarnode == nullptr) {
      errorMessage = "compare input is unknown";
      return false;
    }
    llvm::Value *lhs = resize(read(op.Inputs[0]), lhsVarnode->Size);
    llvm::Value *rhs = resize(read(op.Inputs[1]), lhsVarnode->Size);
    llvm::Value *result = nullptr;
    if (op.Mnemonic == "INT_SLESSEQUAL") {
      result = Builder.CreateICmpSLE(lhs, rhs);
    } else if (op.Mnemonic == "INT_SLESS") {
      result = Builder.CreateICmpSLT(lhs, rhs);
    } else if (op.Mnemonic == "INT_LESSEQUAL") {
      result = Builder.CreateICmpULE(lhs, rhs);
    } else if (op.Mnemonic == "INT_LESS") {
      result = Builder.CreateICmpULT(lhs, rhs);
    } else if (op.Mnemonic == "INT_EQUAL") {
      result = Builder.CreateICmpEQ(lhs, rhs);
    } else if (op.Mnemonic == "INT_NOTEQUAL") {
      result = Builder.CreateICmpNE(lhs, rhs);
    } else {
      errorMessage = "unsupported compare opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerOverflow(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 2, errorMessage)) {
      return false;
    }

    const HeritageVarnode *lhsVarnode = varnodeFor(op.Inputs[0]);
    if (lhsVarnode == nullptr) {
      errorMessage = op.Mnemonic + " left input is unknown";
      return false;
    }
    llvm::Value *lhs = read(op.Inputs[0]);
    llvm::Value *rhs = resize(read(op.Inputs[1]), lhsVarnode->Size);
    llvm::Intrinsic::ID intrinsicId = llvm::Intrinsic::not_intrinsic;
    if (op.Mnemonic == "INT_CARRY") {
      intrinsicId = llvm::Intrinsic::uadd_with_overflow;
    } else if (op.Mnemonic == "INT_SCARRY") {
      intrinsicId = llvm::Intrinsic::sadd_with_overflow;
    } else if (op.Mnemonic == "INT_SBORROW") {
      intrinsicId = llvm::Intrinsic::ssub_with_overflow;
    } else {
      errorMessage = "unsupported overflow opcode: " + op.Mnemonic;
      return false;
    }

    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, intrinsicId, {lhs->getType()});
    llvm::Value *call = Builder.CreateCall(intrinsic, {lhs, rhs});
    return write(*op.Output, Builder.CreateExtractValue(call, 1), errorMessage);
  }

  bool lowerCountBits(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Function *intrinsic = nullptr;
    llvm::Value *result = nullptr;
    if (op.Mnemonic == "POPCOUNT") {
      intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::ctpop, {input->getType()});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Mnemonic == "LZCOUNT") {
      intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::ctlz, {input->getType()});
      result = Builder.CreateCall(
          intrinsic, {input, llvm::ConstantInt::getFalse(Context)});
    } else {
      errorMessage = "unsupported bit count opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerCast(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Type *outputType = intType(output->Size);
    llvm::Value *result = nullptr;
    if (op.Mnemonic == "INT_ZEXT") {
      result = Builder.CreateZExt(input, outputType);
    } else if (op.Mnemonic == "INT_SEXT") {
      result = Builder.CreateSExt(input, outputType);
    } else {
      errorMessage = "unsupported cast opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerBoolNegate(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 1, errorMessage)) {
      return false;
    }
    llvm::Value *input = read(op.Inputs[0]);
    if (!input->getType()->isIntegerTy(1)) {
      input = Builder.CreateICmpNE(input,
                                   llvm::ConstantInt::get(input->getType(), 0));
    }
    return write(*op.Output, Builder.CreateNot(input), errorMessage);
  }

  bool lowerUnary(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = resize(read(op.Inputs[0]), output->Size);
    llvm::Value *result = nullptr;
    if (op.Mnemonic == "INT_NEGATE") {
      result = Builder.CreateNot(input);
    } else if (op.Mnemonic == "INT_2COMP") {
      result = Builder.CreateNeg(input);
    } else {
      errorMessage = "unsupported unary opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerBoolBinary(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = read(op.Inputs[0]);
    llvm::Value *rhs = read(op.Inputs[1]);
    if (!lhs->getType()->isIntegerTy(1)) {
      lhs =
          Builder.CreateICmpNE(lhs, llvm::ConstantInt::get(lhs->getType(), 0));
    }
    if (!rhs->getType()->isIntegerTy(1)) {
      rhs =
          Builder.CreateICmpNE(rhs, llvm::ConstantInt::get(rhs->getType(), 0));
    }

    llvm::Value *result = nullptr;
    if (op.Mnemonic == "BOOL_AND") {
      result = Builder.CreateAnd(lhs, rhs);
    } else if (op.Mnemonic == "BOOL_OR") {
      result = Builder.CreateOr(lhs, rhs);
    } else if (op.Mnemonic == "BOOL_XOR") {
      result = Builder.CreateXor(lhs, rhs);
    } else {
      errorMessage = "unsupported bool opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerFloatBinary(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(output->Size);
    if (floatTy == nullptr) {
      errorMessage = op.Mnemonic + " only supports 4/8-byte floats";
      return false;
    }

    llvm::Value *lhs = readFloatBits(op.Inputs[0], floatTy, errorMessage);
    llvm::Value *rhs = readFloatBits(op.Inputs[1], floatTy, errorMessage);
    if (lhs == nullptr || rhs == nullptr) {
      return false;
    }

    llvm::Value *result = nullptr;
    if (op.Mnemonic == "FLOAT_ADD") {
      result = Builder.CreateFAdd(lhs, rhs);
    } else if (op.Mnemonic == "FLOAT_SUB") {
      result = Builder.CreateFSub(lhs, rhs);
    } else if (op.Mnemonic == "FLOAT_MULT") {
      result = Builder.CreateFMul(lhs, rhs);
    } else if (op.Mnemonic == "FLOAT_DIV") {
      result = Builder.CreateFDiv(lhs, rhs);
    } else {
      errorMessage = "unsupported float binary opcode: " + op.Mnemonic;
      return false;
    }
    return writeFloatBits(*op.Output, result, errorMessage);
  }

  bool lowerFloatCompare(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    const HeritageVarnode *lhsVarnode = varnodeFor(op.Inputs[0]);
    if (lhsVarnode == nullptr) {
      errorMessage = op.Mnemonic + " input is unknown";
      return false;
    }
    llvm::Type *floatTy = floatType(lhsVarnode->Size);
    if (floatTy == nullptr) {
      errorMessage = op.Mnemonic + " only supports 4/8-byte floats";
      return false;
    }

    llvm::Value *lhs = readFloatBits(op.Inputs[0], floatTy, errorMessage);
    llvm::Value *rhs = readFloatBits(op.Inputs[1], floatTy, errorMessage);
    if (lhs == nullptr || rhs == nullptr) {
      return false;
    }

    llvm::Value *result = nullptr;
    if (op.Mnemonic == "FLOAT_EQUAL") {
      result = Builder.CreateFCmpOEQ(lhs, rhs);
    } else if (op.Mnemonic == "FLOAT_NOTEQUAL") {
      result = Builder.CreateFCmpUNE(lhs, rhs);
    } else if (op.Mnemonic == "FLOAT_LESS") {
      result = Builder.CreateFCmpOLT(lhs, rhs);
    } else if (op.Mnemonic == "FLOAT_LESSEQUAL") {
      result = Builder.CreateFCmpOLE(lhs, rhs);
    } else {
      errorMessage = "unsupported float compare opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerFloatUnary(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 1, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(output->Size);
    if (floatTy == nullptr) {
      errorMessage = op.Mnemonic + " only supports 4/8-byte floats";
      return false;
    }

    llvm::Value *input = readFloatBits(op.Inputs[0], floatTy, errorMessage);
    if (input == nullptr) {
      return false;
    }

    llvm::Value *result = nullptr;
    if (op.Mnemonic == "FLOAT_NEG") {
      result = Builder.CreateFNeg(input);
    } else if (op.Mnemonic == "FLOAT_ABS") {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::fabs, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Mnemonic == "FLOAT_SQRT") {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::sqrt, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Mnemonic == "CEIL" || op.Mnemonic == "FLOOR" ||
               op.Mnemonic == "ROUND") {
      llvm::Intrinsic::ID intrinsicId = llvm::Intrinsic::not_intrinsic;
      if (op.Mnemonic == "CEIL") {
        intrinsicId = llvm::Intrinsic::ceil;
      } else if (op.Mnemonic == "FLOOR") {
        intrinsicId = llvm::Intrinsic::floor;
      } else {
        intrinsicId = llvm::Intrinsic::round;
      }
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, intrinsicId, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else {
      errorMessage = "unsupported float unary opcode: " + op.Mnemonic;
      return false;
    }
    return writeFloatBits(*op.Output, result, errorMessage);
  }

  bool lowerFloatNan(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 1, errorMessage)) {
      return false;
    }
    const HeritageVarnode *inputVarnode = varnodeFor(op.Inputs[0]);
    if (inputVarnode == nullptr) {
      errorMessage = "FLOAT_NAN input is unknown";
      return false;
    }
    llvm::Type *floatTy = floatType(inputVarnode->Size);
    if (floatTy == nullptr) {
      errorMessage = "FLOAT_NAN only supports 4/8-byte floats";
      return false;
    }
    llvm::Value *input = readFloatBits(op.Inputs[0], floatTy, errorMessage);
    if (input == nullptr) {
      return false;
    }
    return write(*op.Output, Builder.CreateFCmpUNO(input, input), errorMessage);
  }

  bool lowerFloatCast(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 1, errorMessage)) {
      return false;
    }
    llvm::Type *outputFloatTy = floatType(output->Size);
    if (op.Mnemonic == "INT2FLOAT") {
      if (outputFloatTy == nullptr) {
        errorMessage = "INT2FLOAT only supports 4/8-byte float outputs";
        return false;
      }
      llvm::Value *input = read(op.Inputs[0]);
      if (input == nullptr) {
        errorMessage = "INT2FLOAT reads unknown varnode: " + op.Inputs[0];
        return false;
      }
      return writeFloatBits(
          *op.Output, Builder.CreateSIToFP(input, outputFloatTy), errorMessage);
    }

    const HeritageVarnode *inputVarnode = varnodeFor(op.Inputs[0]);
    if (inputVarnode == nullptr) {
      errorMessage = op.Mnemonic + " input is unknown";
      return false;
    }
    llvm::Type *inputFloatTy = floatType(inputVarnode->Size);
    if (inputFloatTy == nullptr) {
      errorMessage = op.Mnemonic + " only supports 4/8-byte float inputs";
      return false;
    }
    llvm::Value *input =
        readFloatBits(op.Inputs[0], inputFloatTy, errorMessage);
    if (input == nullptr) {
      return false;
    }

    if (op.Mnemonic == "FLOAT2FLOAT") {
      if (outputFloatTy == nullptr) {
        errorMessage = "FLOAT2FLOAT only supports 4/8-byte float outputs";
        return false;
      }
      return writeFloatBits(
          *op.Output, Builder.CreateFPCast(input, outputFloatTy), errorMessage);
    }
    if (op.Mnemonic == "TRUNC") {
      return write(*op.Output,
                   Builder.CreateFPToSI(input, intType(output->Size)),
                   errorMessage);
    }

    errorMessage = "unsupported float cast opcode: " + op.Mnemonic;
    return false;
  }

  bool lowerSubpiece(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    const HeritageVarnode *offset = varnodeFor(op.Inputs[1]);
    if (offset == nullptr || !offset->IsConstant) {
      errorMessage = "SUBPIECE offset must be constant";
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Value *shift =
        llvm::ConstantInt::get(input->getType(), offset->Offset * 8);
    return write(*op.Output, Builder.CreateLShr(input, shift), errorMessage);
  }

  bool lowerPiece(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 2, errorMessage)) {
      return false;
    }

    llvm::Type *outputType = intType(output->Size);
    llvm::Value *high = Builder.CreateZExt(read(op.Inputs[0]), outputType);
    llvm::Value *low = Builder.CreateZExt(read(op.Inputs[1]), outputType);
    const HeritageVarnode *lowVarnode = varnodeFor(op.Inputs[1]);
    if (lowVarnode == nullptr) {
      errorMessage = "PIECE low input is unknown";
      return false;
    }
    llvm::Value *shift =
        llvm::ConstantInt::get(outputType, bitWidth(lowVarnode->Size));
    return write(*op.Output,
                 Builder.CreateOr(Builder.CreateShl(high, shift), low),
                 errorMessage);
  }

  bool lowerPtrAdd(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 3, errorMessage)) {
      return false;
    }

    uint64_t elementSize = 0;
    if (!constInput(op, 2, elementSize, errorMessage)) {
      return false;
    }
    llvm::Value *base = resize(read(op.Inputs[0]), output->Size);
    llvm::Value *index = resize(read(op.Inputs[1]), output->Size);
    llvm::Value *scale =
        llvm::ConstantInt::get(intType(output->Size), elementSize);
    return write(*op.Output,
                 Builder.CreateAdd(base, Builder.CreateMul(index, scale)),
                 errorMessage);
  }

  bool lowerPtrSub(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 2, errorMessage)) {
      return false;
    }

    uint64_t offset = 0;
    if (!constInput(op, 1, offset, errorMessage)) {
      return false;
    }
    llvm::Value *base = resize(read(op.Inputs[0]), output->Size);
    llvm::Value *constant =
        llvm::ConstantInt::get(intType(output->Size), offset);
    return write(*op.Output, Builder.CreateAdd(base, constant), errorMessage);
  }

  bool lowerInsert(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 4, errorMessage)) {
      return false;
    }

    uint64_t bitOffset = 0;
    uint64_t bitSize = 0;
    if (!constInput(op, 2, bitOffset, errorMessage) ||
        !constInput(op, 3, bitSize, errorMessage)) {
      return false;
    }

    unsigned width = bitWidth(output->Size);
    if (bitOffset >= width || bitSize > width - bitOffset) {
      errorMessage = "INSERT bit range exceeds output width";
      return false;
    }

    llvm::Type *type = intType(output->Size);
    llvm::APInt rangeMask =
        llvm::APInt::getLowBitsSet(width, bitSize).shl(bitOffset);
    llvm::Value *base = resize(read(op.Inputs[0]), output->Size);
    llvm::Value *inserted = resize(read(op.Inputs[1]), output->Size);
    llvm::Value *cleared =
        Builder.CreateAnd(base, llvm::ConstantInt::get(type, ~rangeMask));
    llvm::Value *shifted = Builder.CreateAnd(
        Builder.CreateShl(inserted, llvm::ConstantInt::get(type, bitOffset)),
        llvm::ConstantInt::get(type, rangeMask));
    return write(*op.Output, Builder.CreateOr(cleared, shifted), errorMessage);
  }

  bool lowerExtract(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 3, errorMessage)) {
      return false;
    }

    uint64_t bitOffset = 0;
    uint64_t bitSize = 0;
    if (!constInput(op, 1, bitOffset, errorMessage) ||
        !constInput(op, 2, bitSize, errorMessage)) {
      return false;
    }
    const HeritageVarnode *inputVarnode = varnodeFor(op.Inputs[0]);
    if (inputVarnode == nullptr) {
      errorMessage = op.Mnemonic + " input is unknown";
      return false;
    }
    unsigned inputWidth = bitWidth(inputVarnode->Size);
    if (bitOffset >= inputWidth || bitSize > inputWidth - bitOffset) {
      errorMessage = op.Mnemonic + " bit range exceeds input width";
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Value *shifted = Builder.CreateLShr(
        input, llvm::ConstantInt::get(input->getType(), bitOffset));
    llvm::Value *masked = Builder.CreateAnd(
        shifted,
        llvm::ConstantInt::get(
            input->getType(), llvm::APInt::getLowBitsSet(inputWidth, bitSize)));
    if (op.Mnemonic == "SPULL") {
      unsigned shift = inputWidth - bitSize;
      llvm::Value *left = Builder.CreateShl(
          masked, llvm::ConstantInt::get(input->getType(), shift));
      masked = Builder.CreateAShr(
          left, llvm::ConstantInt::get(input->getType(), shift));
      masked = Builder.CreateSExtOrTrunc(masked, intType(output->Size));
    }
    return write(*op.Output, masked, errorMessage);
  }

  llvm::Value *pointerForStackVarnode(llvm::IRBuilderBase &builder,
                                      const HeritageVarnode &varnode) {
    if (Stack.Storage == nullptr || varnode.Space != "stack") {
      return nullptr;
    }
    int64_t offset = signedOffset(varnode.Offset);
    int64_t end = offset + static_cast<int64_t>(varnode.Size);
    if (offset < Stack.Low || end > Stack.High) {
      return nullptr;
    }
    auto *byteOffset = llvm::ConstantInt::get(
        intType(8), static_cast<uint64_t>(offset - Stack.Low));
    return builder.CreateInBoundsGEP(llvm::Type::getInt8Ty(Context),
                                     Stack.Storage, byteOffset,
                                     varnode.Id + ".stack");
  }

  llvm::Value *pointerForAddressTiedInput(llvm::IRBuilderBase &builder,
                                          const HeritageVarnode &varnode) {
    if (!varnode.IsInput || !varnode.IsAddressTied) {
      return nullptr;
    }
    if (varnode.Space == "ram") {
      llvm::Value *address = llvm::ConstantInt::get(intType(8), varnode.Offset);
      return memoryPointer(builder, address);
    }
    if (varnode.Space == "stack") {
      return pointerForStackVarnode(builder, varnode);
    }
    return nullptr;
  }

  llvm::Value *readAddressTiedInput(llvm::IRBuilderBase &builder,
                                    const HeritageVarnode &varnode) {
    llvm::Value *pointer = pointerForAddressTiedInput(builder, varnode);
    if (pointer == nullptr) {
      return nullptr;
    }
    auto *load =
        builder.CreateLoad(intType(varnode.Size), pointer, varnode.Id + ".mem");
    load->setAlignment(llvm::Align(1));
    return load;
  }

  llvm::GlobalVariable *memoryGlobal() {
    if (Memory) {
      return Memory;
    }

    // Temporary heritage memory model.  This keeps LOAD/STORE explicit in LLVM
    // IR until binary sections and stack objects are exported.
    auto *byteType = llvm::Type::getInt8Ty(Context);
    auto *arrayType = llvm::ArrayType::get(byteType, 1024 * 1024);
    Memory = new llvm::GlobalVariable(Module, arrayType, false,
                                      llvm::GlobalValue::ExternalLinkage,
                                      nullptr, "notdec_ram");
    return Memory;
  }

  llvm::Value *memoryPointer(llvm::IRBuilderBase &builder,
                             llvm::Value *address) {
    return builder.CreateIntToPtr(address, llvm::PointerType::get(Context, 0),
                                  "notdec_mem_ptr");
  }

  bool requireConstSpaceSelector(const HeritageOp &op,
                                 std::string &errorMessage) {
    const HeritageVarnode *selector =
        op.Inputs.empty() ? nullptr : varnodeFor(op.Inputs[0]);
    if (selector != nullptr && selector->IsConstant) {
      return true;
    }

    errorMessage = op.Mnemonic + " address space selector must be const";
    return false;
  }

  bool lowerLoad(const HeritageOp &op, std::string &errorMessage) {
    const HeritageVarnode *output = nullptr;
    if (!requireOutput(op, output, errorMessage) ||
        !requireInputs(op, 2, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), 8);
    auto *load = Builder.CreateLoad(intType(output->Size),
                                    memoryPointer(Builder, address),
                                    *op.Output);
    load->setAlignment(llvm::Align(1));
    return write(*op.Output, load, errorMessage);
  }

  bool lowerStore(const HeritageOp &op, std::string &errorMessage) {
    if (!requireInputs(op, 3, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), 8);
    llvm::Value *value = read(op.Inputs[2]);
    auto *store = Builder.CreateStore(value, memoryPointer(Builder, address));
    store->setAlignment(llvm::Align(1));
    rememberOpInstruction(op, store);
    return true;
  }

  bool lowerPhi(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output) {
      errorMessage = "MULTIEQUAL has no output";
      return false;
    }
    const HeritageBlock *block = Program.BlockById.at(op.Parent);
    const HeritageVarnode *output = varnodeFor(*op.Output);
    if (output == nullptr) {
      errorMessage = "MULTIEQUAL output is unknown";
      return false;
    }
    if (block->In.size() != op.Inputs.size()) {
      errorMessage = "MULTIEQUAL input count does not match predecessor count";
      return false;
    }

    auto *phi =
        Builder.CreatePHI(intType(output->Size), op.Inputs.size(), *op.Output);
    Values[*op.Output] = phi;
    PendingPhis.push_back(PendingPhi{phi, block->In, op.Inputs, output->Size});
    return true;
  }

  llvm::Value *resizeForPhiIncoming(llvm::Value *value, uint32_t byteSize,
                                    llvm::BasicBlock *incomingBlock) {
    llvm::Type *targetType = intType(byteSize);
    if (value->getType() == targetType) {
      return value;
    }

    if (auto *constantInt = llvm::dyn_cast<llvm::ConstantInt>(value)) {
      return llvm::ConstantInt::get(
          targetType, constantInt->getValue().zextOrTrunc(bitWidth(byteSize)));
    }
    if (llvm::isa<llvm::Constant>(value)) {
      warnPoisonFallback("PHI incoming constant has unsupported resize");
      return llvm::PoisonValue::get(targetType);
    }

    llvm::Instruction *terminator = incomingBlock->getTerminator();
    if (terminator == nullptr) {
      warnPoisonFallback("PHI incoming block has no terminator for resize");
      return llvm::PoisonValue::get(targetType);
    }

    llvm::IRBuilder<> edgeBuilder(terminator);
    unsigned sourceBits = value->getType()->getIntegerBitWidth();
    unsigned targetBits = targetType->getIntegerBitWidth();
    if (sourceBits < targetBits) {
      return edgeBuilder.CreateZExt(value, targetType);
    }
    return edgeBuilder.CreateTrunc(value, targetType);
  }

  llvm::Value *readPhiIncoming(const std::string &id, uint32_t byteSize,
                               llvm::BasicBlock *incomingBlock) {
    if (auto it = Values.find(id); it != Values.end()) {
      return resizeForPhiIncoming(it->second, byteSize, incomingBlock);
    }

    const HeritageVarnode *varnode = varnodeFor(id);
    if (varnode != nullptr && varnode->IsConstant) {
      return llvm::ConstantInt::get(intType(byteSize), varnode->Offset);
    }
    if (varnode != nullptr && varnode->IsRegister && varnode->RegisterName) {
      llvm::Instruction *terminator = incomingBlock->getTerminator();
      if (terminator != nullptr) {
        llvm::IRBuilder<> edgeBuilder(terminator);
        RegisterAccess access{varnode->Space, varnode->Offset, varnode->Size,
                              varnode->RegisterName};
        if (llvm::Value *value = Registers->read(edgeBuilder, access)) {
          return resizeForPhiIncoming(value, byteSize, incomingBlock);
        }
      }
    }
    if (varnode != nullptr) {
      llvm::Instruction *terminator = incomingBlock->getTerminator();
      if (terminator != nullptr) {
        llvm::IRBuilder<> edgeBuilder(terminator);
        if (llvm::Value *value = readAddressTiedInput(edgeBuilder, *varnode)) {
          return resizeForPhiIncoming(value, byteSize, incomingBlock);
        }
      }
    }

    if (varnode != nullptr) {
      warnPoisonFallback("PHI incoming varnode is unavailable: " +
                         describeVarnode(*varnode));
    } else {
      warnPoisonFallback("PHI incoming varnode is unknown: " + id);
    }
    return llvm::PoisonValue::get(intType(byteSize));
  }

  bool finalizePendingPhis(std::string &errorMessage) {
    for (const PendingPhi &pending : PendingPhis) {
      for (size_t index = 0; index < pending.Inputs.size(); ++index) {
        auto blockIt = BlockMap.find(pending.Predecessors[index]);
        if (blockIt == BlockMap.end()) {
          errorMessage = "MULTIEQUAL references unknown predecessor block: " +
                         pending.Predecessors[index];
          return false;
        }
        llvm::Value *incoming = readPhiIncoming(
            pending.Inputs[index], pending.OutputSize, blockIt->second);
        pending.Phi->addIncoming(incoming, blockIt->second);
      }
    }
    return true;
  }

  bool lowerCall(const HeritageOp &op, std::string &errorMessage) {
    std::string calleeName = resolveCallTargetName(op, Symbols);
    if (calleeName.empty() || op.Inputs.empty()) {
      errorMessage = "CALL needs resolvable target and target input";
      return false;
    }

    std::vector<llvm::Value *> args;
    for (size_t index = 1; index < op.Inputs.size(); ++index) {
      llvm::Value *arg = read(op.Inputs[index]);
      if (arg == nullptr) {
        errorMessage = "CALL reads unknown argument varnode";
        return false;
      }
      args.push_back(arg);
    }

    llvm::Type *returnType = llvm::Type::getVoidTy(Context);
    if (op.Output) {
      const HeritageVarnode *output = varnodeFor(*op.Output);
      if (output == nullptr) {
        errorMessage = "CALL output is unknown";
        return false;
      }
      returnType = intType(output->Size);
    }
    // HighFunction can omit or vary call-site arguments while prototype
    // recovery is still incomplete.  Use a vararg declaration until the export
    // schema carries stable callee prototypes.
    auto *calleeType = llvm::FunctionType::get(returnType, {}, true);
    llvm::FunctionCallee callee =
        Module.getOrInsertFunction(calleeName, calleeType);
    llvm::CallInst *call = Builder.CreateCall(callee, args);
    rememberOpInstruction(op, call);
    if (!op.Output) {
      return true;
    }
    return write(*op.Output, call, errorMessage);
  }

  bool lowerHelperCall(const HeritageOp &op, std::string &errorMessage) {
    llvm::Type *returnType = llvm::Type::getVoidTy(Context);
    std::string helperName = "notdec_heritage_" + op.Mnemonic + "_void";
    if (op.Output) {
      const HeritageVarnode *output = varnodeFor(*op.Output);
      if (output == nullptr) {
        errorMessage = op.Mnemonic + " output is unknown";
        return false;
      }
      returnType = intType(output->Size);
      helperName = "notdec_heritage_" + op.Mnemonic + "_i" +
                   std::to_string(bitWidth(output->Size));
    }

    std::vector<llvm::Value *> args;
    for (const std::string &inputId : op.Inputs) {
      llvm::Value *arg = read(inputId);
      if (arg == nullptr) {
        errorMessage = op.Mnemonic + " reads unknown argument varnode";
        return false;
      }
      args.push_back(arg);
    }

    auto *helperType = llvm::FunctionType::get(returnType, {}, true);
    llvm::FunctionCallee helper =
        Module.getOrInsertFunction(helperName, helperType);
    llvm::CallInst *call = Builder.CreateCall(helper, args);
    rememberOpInstruction(op, call);
    if (!op.Output) {
      return true;
    }
    return write(*op.Output, call, errorMessage);
  }

  llvm::Value *returnValueFor(const HeritageOp &op, std::string &errorMessage) {
    llvm::Type *returnType = Function->getReturnType();
    if (returnType->isVoidTy()) {
      return nullptr;
    }
    if (op.Inputs.size() < 2) {
      llvm::errs() << "Warning: RETURN op " << op.Id << " in "
                   << Program.Function.Name << " block " << op.Parent
                   << " has no value input; returning undef for "
                   << Program.Function.ReturnType << '\n';
      return llvm::UndefValue::get(returnType);
    }
    llvm::Value *value = read(op.Inputs[1]);
    if (value == nullptr) {
      errorMessage = "RETURN reads unknown varnode";
      return nullptr;
    }
    if (!returnType->isIntegerTy() || !value->getType()->isIntegerTy()) {
      errorMessage = "RETURN value type is not integer";
      return nullptr;
    }
    return resizeToIntegerType(value, returnType);
  }

  const HeritageBlock *
  chooseBlockByAddress(const std::vector<const HeritageBlock *> &candidates,
                       const std::string &address,
                       std::string &errorMessage) const {
    if (candidates.empty()) {
      return nullptr;
    }
    if (candidates.size() == 1) {
      return candidates.front();
    }

    const HeritageBlock *nonEmpty = nullptr;
    for (const HeritageBlock *candidate : candidates) {
      if (candidate != nullptr && !candidate->Ops.empty()) {
        if (nonEmpty != nullptr) {
          errorMessage = "branch target address is ambiguous: " + address;
          return nullptr;
        }
        nonEmpty = candidate;
      }
    }
    return nonEmpty != nullptr ? nonEmpty : candidates.front();
  }

  const HeritageBlock *resolveSuccessorByAddress(
      const HeritageBlock &block, const std::string &address,
      std::string &errorMessage) const {
    // Ghidra can emit an empty entry trampoline and a real code block with the
    // same start address.  Prefer the current CFG edge, otherwise a branch back
    // to the function entry may accidentally target the empty entry block.
    std::vector<const HeritageBlock *> successorCandidates;
    for (const std::string &successor : block.Out) {
      auto blockIt = Program.BlockById.find(successor);
      if (blockIt != Program.BlockById.end() &&
          blockIt->second->Start == address) {
        successorCandidates.push_back(blockIt->second);
      }
    }
    if (!successorCandidates.empty()) {
      return chooseBlockByAddress(successorCandidates, address, errorMessage);
    }

    auto globalCandidates = Program.BlockByStart.find(address);
    if (globalCandidates == Program.BlockByStart.end()) {
      return nullptr;
    }
    return chooseBlockByAddress(globalCandidates->second, address,
                                errorMessage);
  }

  bool lowerBranch(const HeritageOp &op, const HeritageBlock &block,
                   std::string &errorMessage) {
    if (op.Mnemonic == "CBRANCH") {
      if (!requireInputs(op, 2, errorMessage)) {
        return false;
      }
      const HeritageVarnode *target = varnodeFor(op.Inputs[0]);
      if (target == nullptr) {
        errorMessage = "CBRANCH target varnode is unknown";
        return false;
      }
      const HeritageBlock *trueHeritageBlock =
          resolveSuccessorByAddress(block, target->Address, errorMessage);
      if (!errorMessage.empty()) {
        return false;
      }
      if (trueHeritageBlock == nullptr && block.Out.size() < 2) {
        errorMessage = "CBRANCH target block is unknown: " + target->Address;
        return false;
      }
      llvm::BasicBlock *falseBlock = nullptr;
      llvm::BasicBlock *trueBlock = nullptr;
      if (trueHeritageBlock != nullptr) {
        trueBlock = BlockMap.at(trueHeritageBlock->Id);
        for (const std::string &successor : block.Out) {
          if (successor != trueHeritageBlock->Id) {
            falseBlock = BlockMap.at(successor);
            break;
          }
        }
      } else {
        trueBlock = BlockMap.at(block.Out[0]);
        falseBlock = BlockMap.at(block.Out[1]);
      }
      if (falseBlock == nullptr) {
        errorMessage = "CBRANCH false successor is unknown";
        return false;
      }
      llvm::Value *condition = read(op.Inputs[1]);
      if (!condition->getType()->isIntegerTy(1)) {
        condition = Builder.CreateICmpNE(
            condition, llvm::ConstantInt::get(condition->getType(), 0));
      }
      Builder.CreateCondBr(condition, trueBlock, falseBlock);
      return true;
    }

    if (op.Mnemonic == "BRANCH") {
      if (!requireInputs(op, 1, errorMessage)) {
        return false;
      }
      const HeritageVarnode *target = varnodeFor(op.Inputs[0]);
      if (target == nullptr) {
        errorMessage = "BRANCH target varnode is unknown";
        return false;
      }
      const HeritageBlock *targetBlock =
          resolveSuccessorByAddress(block, target->Address, errorMessage);
      if (!errorMessage.empty()) {
        return false;
      }
      if (targetBlock == nullptr) {
        if (block.Out.size() != 1) {
          errorMessage = "BRANCH target block is unknown: " + target->Address;
          return false;
        }
        Builder.CreateBr(BlockMap.at(block.Out.front()));
        return true;
      }
      Builder.CreateBr(BlockMap.at(targetBlock->Id));
      return true;
    }

    if (op.Mnemonic == "RETURN") {
      llvm::Value *value = returnValueFor(op, errorMessage);
      if (!errorMessage.empty()) {
        return false;
      }
      if (value == nullptr) {
        Builder.CreateRetVoid();
      } else {
        Builder.CreateRet(value);
      }
      return true;
    }

    if (op.Mnemonic == "BRANCHIND") {
      if (!requireInputs(op, 1, errorMessage)) {
        return false;
      }
      llvm::Value *target = read(op.Inputs[0]);
      if (target == nullptr) {
        errorMessage = "BRANCHIND target varnode is unknown";
        return false;
      }
      if (block.Out.empty()) {
        if (Function->getReturnType()->isVoidTy()) {
          Builder.CreateRetVoid();
        } else {
          warnPoisonFallback("BRANCHIND without successors returns poison");
          Builder.CreateRet(llvm::PoisonValue::get(Function->getReturnType()));
        }
        return true;
      }
      llvm::Value *address =
          Builder.CreateIntToPtr(target, llvm::PointerType::get(Context, 0));
      llvm::IndirectBrInst *branch =
          Builder.CreateIndirectBr(address, block.Out.size());
      for (const std::string &successor : block.Out) {
        branch->addDestination(BlockMap.at(successor));
      }
      return true;
    }

    errorMessage = "unsupported terminator: " + op.Mnemonic;
    return false;
  }

  bool lowerOp(const HeritageOp &op, std::string &errorMessage) {
    if (op.Mnemonic == "COPY") {
      return lowerCopy(op, errorMessage);
    }
    if (op.Mnemonic == "CAST") {
      return lowerCopyLike(op, errorMessage);
    }
    if (op.Mnemonic == "INDIRECT") {
      return lowerIndirect(op, errorMessage);
    }
    if (op.Mnemonic == "LOAD") {
      return lowerLoad(op, errorMessage);
    }
    if (op.Mnemonic == "STORE") {
      return lowerStore(op, errorMessage);
    }
    if (op.Mnemonic == "INT_ADD" || op.Mnemonic == "INT_SUB" ||
        op.Mnemonic == "INT_MULT" || op.Mnemonic == "INT_AND" ||
        op.Mnemonic == "INT_OR" || op.Mnemonic == "INT_XOR" ||
        op.Mnemonic == "INT_LEFT" || op.Mnemonic == "INT_RIGHT" ||
        op.Mnemonic == "INT_SRIGHT" || op.Mnemonic == "INT_DIV" ||
        op.Mnemonic == "INT_SDIV" || op.Mnemonic == "INT_REM" ||
        op.Mnemonic == "INT_SREM") {
      return lowerBinary(op, errorMessage);
    }
    if (op.Mnemonic == "INT_CARRY" || op.Mnemonic == "INT_SCARRY" ||
        op.Mnemonic == "INT_SBORROW") {
      return lowerOverflow(op, errorMessage);
    }
    if (op.Mnemonic == "POPCOUNT" || op.Mnemonic == "LZCOUNT") {
      return lowerCountBits(op, errorMessage);
    }
    if (op.Mnemonic == "INT_SLESSEQUAL" || op.Mnemonic == "INT_SLESS" ||
        op.Mnemonic == "INT_LESSEQUAL" || op.Mnemonic == "INT_LESS" ||
        op.Mnemonic == "INT_EQUAL" || op.Mnemonic == "INT_NOTEQUAL") {
      return lowerCompare(op, errorMessage);
    }
    if (op.Mnemonic == "INT_ZEXT" || op.Mnemonic == "INT_SEXT") {
      return lowerCast(op, errorMessage);
    }
    if (op.Mnemonic == "INT_NEGATE" || op.Mnemonic == "INT_2COMP") {
      return lowerUnary(op, errorMessage);
    }
    if (op.Mnemonic == "BOOL_NEGATE") {
      return lowerBoolNegate(op, errorMessage);
    }
    if (op.Mnemonic == "BOOL_AND" || op.Mnemonic == "BOOL_OR" ||
        op.Mnemonic == "BOOL_XOR") {
      return lowerBoolBinary(op, errorMessage);
    }
    if (op.Mnemonic == "FLOAT_ADD" || op.Mnemonic == "FLOAT_SUB" ||
        op.Mnemonic == "FLOAT_MULT" || op.Mnemonic == "FLOAT_DIV") {
      return lowerFloatBinary(op, errorMessage);
    }
    if (op.Mnemonic == "FLOAT_EQUAL" || op.Mnemonic == "FLOAT_NOTEQUAL" ||
        op.Mnemonic == "FLOAT_LESS" || op.Mnemonic == "FLOAT_LESSEQUAL") {
      return lowerFloatCompare(op, errorMessage);
    }
    if (op.Mnemonic == "FLOAT_NEG" || op.Mnemonic == "FLOAT_ABS" ||
        op.Mnemonic == "FLOAT_SQRT" || op.Mnemonic == "CEIL" ||
        op.Mnemonic == "FLOOR" || op.Mnemonic == "ROUND") {
      return lowerFloatUnary(op, errorMessage);
    }
    if (op.Mnemonic == "FLOAT_NAN") {
      return lowerFloatNan(op, errorMessage);
    }
    if (op.Mnemonic == "INT2FLOAT" || op.Mnemonic == "FLOAT2FLOAT" ||
        op.Mnemonic == "TRUNC") {
      return lowerFloatCast(op, errorMessage);
    }
    if (op.Mnemonic == "SUBPIECE") {
      return lowerSubpiece(op, errorMessage);
    }
    if (op.Mnemonic == "PIECE") {
      return lowerPiece(op, errorMessage);
    }
    if (op.Mnemonic == "PTRADD") {
      return lowerPtrAdd(op, errorMessage);
    }
    if (op.Mnemonic == "PTRSUB") {
      return lowerPtrSub(op, errorMessage);
    }
    if (op.Mnemonic == "INSERT") {
      return lowerInsert(op, errorMessage);
    }
    if (op.Mnemonic == "EXTRACT" || op.Mnemonic == "ZPULL" ||
        op.Mnemonic == "SPULL") {
      return lowerExtract(op, errorMessage);
    }
    if (op.Mnemonic == "MULTIEQUAL") {
      return lowerPhi(op, errorMessage);
    }
    if (op.Mnemonic == "CALL") {
      return lowerCall(op, errorMessage);
    }
    if (op.Mnemonic == "UNIMPLEMENTED" || op.Mnemonic == "CALLIND" ||
        op.Mnemonic == "CALLOTHER" || op.Mnemonic == "SEGMENTOP" ||
        op.Mnemonic == "CPOOLREF" || op.Mnemonic == "NEW") {
      return lowerHelperCall(op, errorMessage);
    }

    errorMessage = "unsupported heritage opcode: " + op.Mnemonic;
    return false;
  }

  bool lowerBlock(const HeritageBlock &block, std::string &errorMessage) {
    for (const std::string &opId : block.Ops) {
      const HeritageOp *op = Program.OpById.at(opId);
      if (op->Mnemonic == "MULTIEQUAL") {
        CurrentOp = op;
        bool ok = lowerPhi(*op, errorMessage);
        CurrentOp = nullptr;
        if (!ok) {
          return false;
        }
      }
    }

    for (const std::string &opId : block.Ops) {
      const HeritageOp *op = Program.OpById.at(opId);
      if (op->Mnemonic == "MULTIEQUAL") {
        continue;
      }
      if (op->Mnemonic == "BRANCH" || op->Mnemonic == "CBRANCH" ||
          op->Mnemonic == "BRANCHIND" || op->Mnemonic == "RETURN") {
        CurrentOp = op;
        bool ok = lowerBranch(*op, block, errorMessage);
        CurrentOp = nullptr;
        return ok;
      }
      CurrentOp = op;
      bool ok = lowerOp(*op, errorMessage);
      CurrentOp = nullptr;
      if (!ok) {
        return false;
      }
    }

    if (!block.Out.empty()) {
      Builder.CreateBr(BlockMap.at(block.Out.front()));
    } else if (Function->getReturnType()->isVoidTy()) {
      Builder.CreateRetVoid();
    } else {
      warnPoisonFallback("fallthrough block without successors returns poison");
      Builder.CreateRet(llvm::PoisonValue::get(Function->getReturnType()));
    }
    return true;
  }

  llvm::LLVMContext &Context;
  llvm::Module &Module;
  const HeritageProgram &Program;
  const HeritageModuleSymbolPlan *Symbols = nullptr;
  llvm::IRBuilder<> Builder;
  llvm::Function *Function = nullptr;
  std::unique_ptr<RegisterStorage> OwnedRegisters;
  RegisterStorage *Registers = nullptr;
  std::unordered_map<std::string, llvm::BasicBlock *> BlockMap;
  std::unordered_map<std::string, llvm::Value *> Values;
  // Maps exported P-Code op ids to the LLVM instruction produced from them.
  // This lets INDIRECT metadata point at an IR-level effect instead of only
  // preserving the original P-Code text.
  std::unordered_map<std::string, llvm::Instruction *> OpInstructionById;
  std::unordered_map<std::string, llvm::MDNode *> EffectMetadataByOpId;
  std::unordered_map<std::string, std::vector<PendingIndirectMetadata>>
      PendingIndirectsByEffect;
  std::unordered_set<std::string> PoisonFallbackWarnings;
  std::unordered_set<std::string> MissingParameterWarnings;
  std::vector<PendingPhi> PendingPhis;
  llvm::GlobalVariable *Memory = nullptr;
  StackFrame Stack;
  const HeritageOp *CurrentOp = nullptr;
};

} // namespace

std::unique_ptr<llvm::Module>
buildHeritageModule(llvm::LLVMContext &context, const HeritageProgram &program,
                    const HeritageLoweringConfig &config,
                    std::string &errorMessage) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  HeritageLowerer lowerer(context, *module, program);
  if (!lowerer.lower(errorMessage)) {
    return nullptr;
  }
  return module;
}

std::unique_ptr<llvm::Module> buildHeritageDeclarationModule(
    llvm::LLVMContext &context, const HeritageModule &heritageModule,
    const HeritageLoweringConfig &config, std::string &errorMessage) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  HeritageModuleSymbolPlan symbols = planModuleSymbols(heritageModule);

  for (size_t index = 0; index < heritageModule.Functions.size(); ++index) {
    declareInternalFunction(*module,
                            heritageModule.Functions[index].Program.Function,
                            symbols.InternalNames[index]);
  }

  for (size_t index = 0; index < heritageModule.Externals.size(); ++index) {
    const HeritageExternalFunction &external = heritageModule.Externals[index];
    module->getOrInsertFunction(
        symbols.ExternalNames[index],
        varargFunctionType(context, external.ReturnType));
  }

  return module;
}

std::unique_ptr<llvm::Module> buildHeritageModuleWithBodies(
    llvm::LLVMContext &context, const HeritageModule &heritageModule,
    const HeritageLoweringConfig &config, HeritageModuleLoweringStats &stats,
    std::string &errorMessage) {
  stats = HeritageModuleLoweringStats{};
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  HeritageModuleSymbolPlan symbols = planModuleSymbols(heritageModule);
  RegisterStorage registers(
      context, *module, registerInfosForHeritageModule(heritageModule), false);

  for (size_t index = 0; index < heritageModule.Functions.size(); ++index) {
    declareInternalFunction(*module,
                            heritageModule.Functions[index].Program.Function,
                            symbols.InternalNames[index]);
    stats.DeclaredInternalFunctions++;
  }

  for (size_t index = 0; index < heritageModule.Externals.size(); ++index) {
    const HeritageExternalFunction &external = heritageModule.Externals[index];
    module->getOrInsertFunction(
        symbols.ExternalNames[index],
        varargFunctionType(context, external.ReturnType));
    stats.DeclaredExternalFunctions++;
  }

  auto restoreDeclaration = [&](size_t index) {
    const HeritageFunction &function =
        heritageModule.Functions[index].Program.Function;
    if (llvm::Function *existing =
            module->getFunction(symbols.InternalNames[index])) {
      existing->eraseFromParent();
    }
    declareInternalFunction(*module, function, symbols.InternalNames[index]);
  };

  for (size_t index = 0; index < heritageModule.Functions.size(); ++index) {
    const HeritageModuleFunction &function = heritageModule.Functions[index];
    if (function.Status != "ok") {
      continue;
    }

    llvm::Function *llvmFunction =
        module->getFunction(symbols.InternalNames[index]);
    std::string functionError;
    HeritageLowerer lowerer(context, *module, function.Program, &symbols,
                            llvmFunction, &registers);
    if (!lowerer.lower(functionError)) {
      restoreDeclaration(index);
      stats.Failures.push_back({function.Program.Function.Name,
                                function.Program.Function.Entry,
                                functionError});
      continue;
    }

    std::string verifierError;
    llvm::raw_string_ostream verifierStream(verifierError);
    if (llvm::verifyFunction(*llvmFunction, &verifierStream)) {
      restoreDeclaration(index);
      verifierStream.flush();
      stats.Failures.push_back({function.Program.Function.Name,
                                function.Program.Function.Entry,
                                verifierError});
      continue;
    }

    stats.LoweredFunctions++;
  }

  return module;
}

} // namespace notdec::bin2llvm
