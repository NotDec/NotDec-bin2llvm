#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterPeephole.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h"
#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

llvm::MDNode *registerAccessMetadata(llvm::LLVMContext &context,
                                     const std::string &name,
                                     uint64_t size = 8) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + name),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=" + std::to_string(size)),
      llvm::MDString::get(context, "name=" + name),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::MDNode *summaryPhiMetadata(llvm::LLVMContext &context,
                                 const std::string &name) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + name),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name) {
  llvm::LLVMContext &context = module.getContext();
  auto *global = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=" + name),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name,
                                           llvm::Type *type, uint64_t offset,
                                           uint64_t size) {
  llvm::LLVMContext &context = module.getContext();
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=" + std::to_string(offset)),
      llvm::MDString::get(context, "size=" + std::to_string(size)),
      llvm::MDString::get(context, "name=" + name),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

void attachTestAbiWithInputs(llvm::Module &module,
                             std::initializer_list<llvm::StringRef> inputs) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  for (llvm::StringRef name : inputs) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(killed);

  for (llvm::StringRef name : {"RBX", "RBP"}) {
    notdec::bin2llvm::NativeAbiEffect unaffected;
    unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
    unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    unaffected.Storage.Name = name.str();
    abi.Effects.push_back(unaffected);
  }

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachTestAbi(llvm::Module &module) {
  attachTestAbiWithInputs(module, {"RDI"});
}

void addStackInput(notdec::bin2llvm::NativeAbiSpec &abi,
                   const std::string &space, uint64_t offset, uint32_t align,
                   uint32_t maxSize = 64) {
  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = maxSize;
  input.Align = align;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Stack;
  input.Storage.Space = space;
  input.Storage.Offset = offset;
  abi.Inputs.push_back(input);
}

void attachI386StackTestAbi(llvm::Module &module, uint64_t offset = 4) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_i386_stack_test";
  abi.StackPointerRegister = "ESP";
  abi.StackPointerSpace = "register";
  abi.StackShift = 4;
  addStackInput(abi, "stack", offset, 4);

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 4;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "EAX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "EAX";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachX64StackTestAbi(llvm::Module &module, uint64_t offset = 8) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_x64_stack_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  abi.StackShift = 8;

  for (llvm::StringRef name : {"RDI", "RSI", "RDX", "RCX", "R8", "R9"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  addStackInput(abi, "stack", offset, 8);

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachI386TestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_i386_test";
  abi.StackPointerRegister = "ESP";
  abi.StackPointerSpace = "register";

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 4;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "EAX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "EAX";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachTestFloatAbi(llvm::Module &module, unsigned floatInputs) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_float_test";
  for (unsigned index = 0; index < floatInputs; ++index) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 4;
    input.MaxSize = 8;
    input.MetaType = "float";
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = ("XMM" + std::to_string(index) + "_Qa");
    abi.Inputs.push_back(input);
  }
  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.MetaType = "float";
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "XMM0_Qa";
  abi.Outputs.push_back(output);
  for (unsigned index = 0; index < floatInputs; ++index) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = "XMM" + std::to_string(index);
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachTestIntegerAndFloatVarArgAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_mixed_vararg_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  for (llvm::StringRef name : {"RDI", "RSI", "RDX", "RCX", "R8", "R9"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }

  notdec::bin2llvm::NativeAbiParamEntry floatInput;
  floatInput.MinSize = 4;
  floatInput.MaxSize = 8;
  floatInput.MetaType = "float";
  floatInput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  floatInput.Storage.Name = "XMM0_Qa";
  abi.Inputs.push_back(floatInput);

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(killed);
  killed.Storage.Name = "XMM0";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

llvm::StoreInst *storeRegister(llvm::IRBuilder<> &builder,
                               llvm::GlobalVariable *reg, llvm::Value *value,
                               const std::string &name, uint64_t size = 8) {
  llvm::StoreInst *store = builder.CreateStore(value, reg);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(reg->getContext(), name, size));
  return store;
}

llvm::LoadInst *loadRegister(llvm::IRBuilder<> &builder,
                             llvm::GlobalVariable *reg, const std::string &name,
                             const std::string &valueName = "",
                             uint64_t size = 8) {
  llvm::LoadInst *load =
      builder.CreateLoad(reg->getValueType(), reg, valueName);
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(reg->getContext(), name, size));
  return load;
}

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool verifyOk(llvm::Module &module, const char *message) {
  std::string error;
  llvm::raw_string_ostream os(error);
  if (!llvm::verifyModule(module, &os)) {
    return true;
  }
  os.flush();
  std::cerr << message << "\n" << error << '\n';
  return false;
}

bool hasNamedAlloca(llvm::Function &function, llvm::StringRef prefix) {
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
    if (alloca != nullptr && alloca->hasName() &&
        alloca->getName().starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

const notdec::bin2llvm::NativeRegisterSummaryFunction *
functionSummary(const notdec::bin2llvm::NativeRegisterSummary &summary,
                llvm::StringRef name) {
  for (const auto &function : summary.Functions) {
    if (function.FunctionName == name) {
      return &function;
    }
  }
  return nullptr;
}

const notdec::bin2llvm::NativeRegisterSummaryRegister *
registerSummary(const notdec::bin2llvm::NativeRegisterSummaryFunction &function,
                llvm::StringRef name) {
  for (const auto &reg : function.Registers) {
    if (reg.Name == name) {
      return &reg;
    }
  }
  return nullptr;
}

bool hasLiveReplacedRegisterLoad(llvm::Function &function) {
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
    if (load != nullptr &&
        load->getMetadata("notdec.register.summary_ssa.replaced") != nullptr &&
        !load->use_empty()) {
      return true;
    }
  }
  return false;
}

bool hasCallTo(const llvm::Function &function, llvm::StringRef calleeName) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == calleeName) {
      return true;
    }
  }
  return false;
}

bool blockEndsWithUnreachable(const llvm::Function &function,
                              llvm::StringRef blockName) {
  for (const llvm::BasicBlock &block : function) {
    if (block.getName() == blockName) {
      return llvm::isa<llvm::UnreachableInst>(block.getTerminator());
    }
  }
  return false;
}

bool hasPartialWriteCall(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr &&
        notdec::bin2llvm::parseNativeRegisterPartialWrite(*call)) {
      return true;
    }
  }
  return false;
}

bool hasPartialReadCall(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr &&
        notdec::bin2llvm::parseNativeRegisterPartialRead(*call)) {
      return true;
    }
  }
  return false;
}

unsigned countPartialReadCalls(const llvm::Function &function,
                               llvm::GlobalVariable *global, uint64_t bitOffset,
                               uint32_t readWidth) {
  unsigned count = 0;
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr) {
      continue;
    }
    std::optional<notdec::bin2llvm::NativeRegisterPartialReadInfo> read =
        notdec::bin2llvm::parseNativeRegisterPartialRead(*call);
    if (read && read->Global == global && read->BitOffset == bitOffset &&
        read->ReadWidth == readWidth) {
      ++count;
    }
  }
  return count;
}

bool hasRegisterLoad(const llvm::Function &function, llvm::StringRef name) {
  std::string wanted = ("name=" + name).str();
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
    if (load == nullptr) {
      continue;
    }
    llvm::MDNode *metadata = load->getMetadata("notdec.register.access");
    if (metadata == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &operand : metadata->operands()) {
      auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
      if (text != nullptr && text->getString() == wanted) {
        return true;
      }
    }
  }
  return false;
}

bool hasRegisterStore(const llvm::Function &function, llvm::StringRef name) {
  std::string wanted = ("name=" + name).str();
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr) {
      continue;
    }
    llvm::MDNode *metadata = store->getMetadata("notdec.register.access");
    if (metadata == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &operand : metadata->operands()) {
      auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
      if (text != nullptr && text->getString() == wanted) {
        return true;
      }
    }
  }
  return false;
}

bool hasLoadFromPhiPointer(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
    if (load != nullptr &&
        llvm::isa<llvm::PHINode>(load->getPointerOperand())) {
      return true;
    }
  }
  return false;
}

bool hasStoreInstruction(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (llvm::isa<llvm::StoreInst>(inst)) {
      return true;
    }
  }
  return false;
}

bool functionHasAnyRegisterSummaryMetadata(const llvm::Function &function) {
  return function.getMetadata("notdec.register.summary") != nullptr ||
         function.getMetadata("notdec.register.summary.read_entry") !=
             nullptr ||
         function.getMetadata("notdec.register.summary.preserves") != nullptr ||
         function.getMetadata("notdec.register.summary.modifies") != nullptr ||
         function.getMetadata("notdec.register.summary.demanded_returns") !=
             nullptr ||
         function.getMetadata("notdec.register.summary_ssa") != nullptr;
}

uint64_t summarySsaMetadataUInt(const llvm::Function &function,
                                llvm::StringRef key) {
  llvm::MDNode *metadata = function.getMetadata("notdec.register.summary_ssa");
  if (metadata == nullptr) {
    return 0;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : metadata->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (!value.consume_front(prefix)) {
      continue;
    }
    uint64_t parsed = 0;
    if (!value.getAsInteger(10, parsed)) {
      return parsed;
    }
  }
  return 0;
}

bool moduleHasFunctionNamed(const llvm::Module &module, llvm::StringRef name) {
  return module.getFunction(name) != nullptr;
}

bool moduleHasUsedFunctionNamed(const llvm::Module &module,
                                llvm::StringRef name) {
  const llvm::Function *function = module.getFunction(name);
  return function != nullptr && !function->use_empty();
}

bool isUnknownValueCall(const llvm::Value *value) {
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  if (call == nullptr) {
    return false;
  }
  const llvm::Function *callee = call->getCalledFunction();
  return callee != nullptr && callee->getName().starts_with("notdec.unknown.");
}

bool metadataHasField(const llvm::MDNode *metadata, llvm::StringRef field) {
  if (metadata == nullptr) {
    return false;
  }
  for (const llvm::MDOperand &operand : metadata->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text != nullptr && text->getString() == field) {
      return true;
    }
  }
  return false;
}

bool functionHasUsedSummaryCallValueRange(const llvm::Function &function,
                                          llvm::StringRef kind,
                                          llvm::StringRef name,
                                          uint64_t bitOffset,
                                          uint64_t bitWidth) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr || call->use_empty()) {
      continue;
    }
    llvm::MDNode *metadata =
        call->getMetadata("notdec.register.summary_ssa.call_value");
    if (metadataHasField(metadata, ("kind=" + kind).str()) &&
        metadataHasField(metadata, ("name=" + name).str()) &&
        metadataHasField(metadata, "bit_offset=" + std::to_string(bitOffset)) &&
        metadataHasField(metadata, "bit_width=" + std::to_string(bitWidth))) {
      return true;
    }
  }
  return false;
}

bool moduleHasOverflowIntrinsicDeclaration(const llvm::Module &module) {
  for (const llvm::Function &function : module) {
    if (function.isDeclaration() &&
        function.getName().contains(".with.overflow.")) {
      return true;
    }
  }
  return false;
}

bool functionHasZeroDemandOperandMetadata(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.getMetadata("notdec.register.summary_ssa.zero_demand_operand") !=
        nullptr) {
      return true;
    }
  }
  return false;
}

bool functionHasInstructionNameContaining(const llvm::Function &function,
                                          llvm::StringRef needle) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.hasName() && inst.getName().contains(needle)) {
      return true;
    }
  }
  return false;
}

bool valueNameContains(const llvm::Value *value, llvm::StringRef needle) {
  return value != nullptr && value->hasName() &&
         value->getName().contains(needle);
}

llvm::Function *createStackCanaryCheckFunction(
    llvm::Module &module, uint64_t tlsOffset, bool useZextCondition,
    bool extraFailSideEffect, bool maskSavedCanary = false,
    bool saveRealCanary = false, llvm::StringRef tlsRegisterName = "FS_OFFSET",
    unsigned tlsRegisterBytes = 8) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *tlsRegisterType = tlsRegisterBytes == 4
                                    ? llvm::Type::getInt32Ty(context)
                                    : llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *tlsBaseRegister = createRegisterGlobal(
      module, tlsRegisterName.str(), tlsRegisterType, 0, tlsRegisterBytes);

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "stack_canary_epilogue", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  llvm::Value *initialSavedCanary = llvm::ConstantInt::get(tlsRegisterType, 0);
  if (saveRealCanary) {
    llvm::LoadInst *savedTlsBase =
        loadRegister(builder, tlsBaseRegister, tlsRegisterName.str(),
                     "tls_base_save", tlsRegisterBytes);
    llvm::Value *savedTlsCanaryAddress = builder.CreateAdd(
        savedTlsBase, llvm::ConstantInt::get(tlsRegisterType, tlsOffset),
        "tls_canary_save_addr");
    llvm::Value *savedTlsCanaryPointer = builder.CreateIntToPtr(
        savedTlsCanaryAddress, llvm::PointerType::get(context, 0),
        "tls_canary_save_ptr");
    initialSavedCanary = builder.CreateLoad(
        tlsRegisterType, savedTlsCanaryPointer, "tls_canary_save");
  }
  builder.CreateStore(initialSavedCanary, savedPointer);
  llvm::LoadInst *savedCanary =
      builder.CreateLoad(tlsRegisterType, savedPointer, "saved_canary");
  llvm::LoadInst *tlsBase =
      loadRegister(builder, tlsBaseRegister, tlsRegisterName.str(), "tls_base",
                   tlsRegisterBytes);
  llvm::Value *tlsCanaryAddress = builder.CreateAdd(
      tlsBase, llvm::ConstantInt::get(tlsRegisterType, tlsOffset),
      "tls_canary_addr");
  llvm::Value *tlsCanaryPointer = builder.CreateIntToPtr(
      tlsCanaryAddress, llvm::PointerType::get(context, 0), "tls_canary_ptr");
  llvm::LoadInst *tlsCanary =
      builder.CreateLoad(tlsRegisterType, tlsCanaryPointer, "tls_canary");
  llvm::Value *savedCompareValue = savedCanary;
  if (maskSavedCanary) {
    savedCompareValue = builder.CreateAnd(
        savedCanary, llvm::ConstantInt::get(tlsRegisterType, 0xffffffffULL),
        "saved_canary_low32");
  }
  llvm::ICmpInst *same = llvm::cast<llvm::ICmpInst>(
      builder.CreateICmpEQ(savedCompareValue, tlsCanary, "canary_same"));
  llvm::Value *condition = same;
  if (useZextCondition) {
    llvm::Value *wide =
        builder.CreateZExt(same, llvm::Type::getInt8Ty(context), "wide");
    condition = builder.CreateICmpNE(
        wide, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0),
        "wide_nonzero");
  }
  builder.CreateCondBr(condition, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  if (extraFailSideEffect) {
    auto *sideEffectType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
    llvm::Function *sideEffect = llvm::Function::Create(
        sideEffectType, llvm::GlobalValue::ExternalLinkage,
        "not_a_stack_fail_side_effect", module);
    builder.CreateCall(sideEffect->getFunctionType(), sideEffect, {});
  }
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *createRawRspStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "raw_rsp_stack_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspBase = loadRegister(builder, rsp, "RSP", "rsp_base");
  llvm::Value *savedAddress = builder.CreateAdd(
      rspBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), -24),
      "saved_canary_addr");
  llvm::Value *savedPointer = builder.CreateIntToPtr(
      savedAddress, llvm::PointerType::get(context, 0), "saved_canary_ptr");
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      fsCanaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *createRawRspZfStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *zf =
      createRegisterGlobal(module, "ZF", llvm::Type::getInt8Ty(context), 0, 1);
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "raw_rsp_zf_stack_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspBase = loadRegister(builder, rsp, "RSP", "rsp_base");
  llvm::Value *savedAddress = builder.CreateAdd(
      rspBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 8200),
      "saved_canary_addr");
  llvm::Value *savedPointer = builder.CreateIntToPtr(
      savedAddress, llvm::PointerType::get(context, 0), "saved_canary_ptr");
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      fsCanaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  llvm::Value *flag =
      builder.CreateZExt(same, llvm::Type::getInt8Ty(context), "zf_value");
  storeRegister(builder, zf, flag, "ZF");
  llvm::LoadInst *flagLoad = loadRegister(builder, zf, "ZF", "zf_after_cmp");
  llvm::Value *flagIsZero = builder.CreateICmpEQ(
      flagLoad, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0),
      "zf_is_zero");
  builder.CreateCondBr(flagIsZero, failBlock, success);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *createPhiFsBaseStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context),
      {llvm::Type::getInt1Ty(context), llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "phi_fs_stack_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *directEdge =
      llvm::BasicBlock::Create(context, "direct_edge", function);
  llvm::BasicBlock *outerSelect =
      llvm::BasicBlock::Create(context, "outer_select", function);
  llvm::BasicBlock *innerSelect =
      llvm::BasicBlock::Create(context, "inner_select", function);
  llvm::BasicBlock *fsEdge =
      llvm::BasicBlock::Create(context, "fs_edge", function);
  llvm::BasicBlock *zeroEdge =
      llvm::BasicBlock::Create(context, "zero_edge", function);
  llvm::BasicBlock *innerMerge =
      llvm::BasicBlock::Create(context, "inner_merge", function);
  llvm::BasicBlock *outerZeroEdge =
      llvm::BasicBlock::Create(context, "outer_zero_edge", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), savedPointer);
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base_entry");
  builder.CreateCondBr(function->getArg(0), directEdge, outerSelect);

  builder.SetInsertPoint(directEdge);
  builder.CreateBr(merge);
  builder.SetInsertPoint(outerSelect);
  builder.CreateCondBr(function->getArg(1), innerSelect, outerZeroEdge);
  builder.SetInsertPoint(innerSelect);
  builder.CreateCondBr(function->getArg(0), fsEdge, zeroEdge);
  builder.SetInsertPoint(fsEdge);
  builder.CreateBr(innerMerge);
  builder.SetInsertPoint(zeroEdge);
  llvm::Value *unknownInnerFs = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt64Ty(context)),
      "fs_base_inner_unknown");
  builder.CreateBr(innerMerge);
  builder.SetInsertPoint(innerMerge);
  llvm::PHINode *innerPhi =
      builder.CreatePHI(llvm::Type::getInt64Ty(context), 2, "fs_base_inner");
  innerPhi->addIncoming(fsBase, fsEdge);
  innerPhi->addIncoming(unknownInnerFs, zeroEdge);
  builder.CreateBr(merge);
  builder.SetInsertPoint(outerZeroEdge);
  llvm::Value *unknownOuterFs = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt64Ty(context)),
      "fs_base_outer_unknown");
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *fsBasePhi =
      builder.CreatePHI(llvm::Type::getInt64Ty(context), 3, "fs_base_phi");
  fsBasePhi->addIncoming(fsBase, directEdge);
  fsBasePhi->addIncoming(innerPhi, innerMerge);
  fsBasePhi->addIncoming(unknownOuterFs, outerZeroEdge);
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBasePhi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      fsCanaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *createZeroBaseStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "zero_base_stack_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), savedPointer);
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *createSharedFailStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "shared_fail_stack_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *secondCheck =
      llvm::BasicBlock::Create(context, "second_check", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 96);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *firstSavedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "first_saved_canary_ptr");
  llvm::Value *secondSavedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32),
      "second_saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0),
      firstSavedPointer);
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0),
      secondSavedPointer);
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  llvm::LoadInst *firstSavedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), firstSavedPointer, "first_saved_canary");
  llvm::Value *firstCanaryPointer = builder.CreateIntToPtr(
      fsCanaryAddress, llvm::PointerType::get(context, 0),
      "first_fs_canary_ptr");
  llvm::LoadInst *firstFsCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), firstCanaryPointer, "first_fs_canary");
  llvm::Value *firstSame =
      builder.CreateICmpEQ(firstSavedCanary, firstFsCanary, "first_same");
  builder.CreateCondBr(firstSame, secondCheck, failBlock);

  builder.SetInsertPoint(secondCheck);
  llvm::Value *secondFsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "second_fs_canary_addr");
  llvm::LoadInst *secondSavedCanary =
      builder.CreateLoad(llvm::Type::getInt64Ty(context), secondSavedPointer,
                         "second_saved_canary");
  llvm::Value *secondCanaryPointer = builder.CreateIntToPtr(
      secondFsCanaryAddress, llvm::PointerType::get(context, 0),
      "second_fs_canary_ptr");
  llvm::LoadInst *secondFsCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), secondCanaryPointer, "second_fs_canary");
  llvm::Value *secondSame =
      builder.CreateICmpEQ(secondSavedCanary, secondFsCanary, "second_same");
  builder.CreateCondBr(secondSame, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  llvm::PHINode *failStack =
      builder.CreatePHI(llvm::Type::getInt64Ty(context), 2, "fail_stack");
  failStack->addIncoming(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 64), entry);
  failStack->addIncoming(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 72), secondCheck);
  llvm::Value *returnSlot = builder.CreateAdd(
      failStack, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), -8),
      "return_slot");
  llvm::Value *returnSlotPointer = builder.CreateIntToPtr(
      returnSlot, llvm::PointerType::get(context, 0), "return_slot_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1),
      returnSlotPointer);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *
createMixedFailPredecessorStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "mixed_fail_predecessor_stack_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *canaryCheck =
      llvm::BasicBlock::Create(context, "canary_check", function);
  llvm::BasicBlock *ordinaryCheck =
      llvm::BasicBlock::Create(context, "ordinary_check", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), canaryCheck, ordinaryCheck);

  builder.SetInsertPoint(canaryCheck);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), savedPointer);
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      fsCanaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(ordinaryCheck);
  llvm::Value *ordinaryError = builder.CreateICmpEQ(
      function->getArg(0), llvm::ConstantInt::getFalse(context),
      "ordinary_error");
  builder.CreateCondBr(ordinaryError, failBlock, success);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *
createPhiFsCanaryAddressStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "phi_fs_canary_address", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *directEdge =
      llvm::BasicBlock::Create(context, "direct_edge", function);
  llvm::BasicBlock *zeroBaseEdge =
      llvm::BasicBlock::Create(context, "zero_base_edge", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), savedPointer);
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  builder.CreateCondBr(function->getArg(0), directEdge, zeroBaseEdge);

  builder.SetInsertPoint(directEdge);
  builder.CreateBr(merge);
  builder.SetInsertPoint(zeroBaseEdge);
  llvm::Value *unknownCanaryAddress = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt64Ty(context)),
      "fs_canary_addr_unknown");
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *canaryAddress = builder.CreatePHI(
      llvm::Type::getInt64Ty(context), 2, "fs_canary_addr_phi");
  canaryAddress->addIncoming(fsCanaryAddress, directEdge);
  canaryAddress->addIncoming(unknownCanaryAddress, zeroBaseEdge);
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      canaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *
createSelfPhiFsCanaryAddressStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "self_phi_fs_canary_address", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::BasicBlock *check =
      llvm::BasicBlock::Create(context, "check", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), savedPointer);
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  builder.CreateBr(loop);

  builder.SetInsertPoint(loop);
  llvm::PHINode *canaryAddress = builder.CreatePHI(
      llvm::Type::getInt64Ty(context), 2, "fs_canary_addr_phi");
  canaryAddress->addIncoming(fsCanaryAddress, entry);
  builder.CreateCondBr(function->getArg(0), loop, check);
  canaryAddress->addIncoming(canaryAddress, loop);

  builder.SetInsertPoint(check);
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      canaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

llvm::Function *
createSelfPhiFsBaseStackCanaryCheckFunction(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *fsOffsetRegister =
      createRegisterGlobal(module, "FS_OFFSET");

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failFunction = llvm::Function::Create(
      failType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "self_phi_fs_base_canary", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::BasicBlock *check =
      llvm::BasicBlock::Create(context, "check", function);
  llvm::BasicBlock *success =
      llvm::BasicBlock::Create(context, "success", function);
  llvm::BasicBlock *failBlock =
      llvm::BasicBlock::Create(context, "fail", function);

  llvm::IRBuilder<> builder(entry);
  auto *slotType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(slotType, nullptr, "notdec_stack.native");
  llvm::Value *savedPointer = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 24),
      "saved_canary_ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), savedPointer);
  llvm::LoadInst *fsBase =
      loadRegister(builder, fsOffsetRegister, "FS_OFFSET", "fs_base");
  builder.CreateBr(loop);

  builder.SetInsertPoint(loop);
  llvm::PHINode *fsBasePhi =
      builder.CreatePHI(llvm::Type::getInt64Ty(context), 2, "fs_base_phi");
  fsBasePhi->addIncoming(fsBase, entry);
  builder.CreateCondBr(function->getArg(0), loop, check);
  fsBasePhi->addIncoming(fsBasePhi, loop);

  builder.SetInsertPoint(check);
  llvm::LoadInst *savedCanary = builder.CreateLoad(
      llvm::Type::getInt64Ty(context), savedPointer, "saved_canary");
  llvm::Value *fsCanaryAddress = builder.CreateAdd(
      fsBasePhi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "fs_canary_addr");
  llvm::Value *fsCanaryPointer = builder.CreateIntToPtr(
      fsCanaryAddress, llvm::PointerType::get(context, 0), "fs_canary_ptr");
  llvm::LoadInst *fsCanary = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                                fsCanaryPointer, "fs_canary");
  llvm::Value *same =
      builder.CreateICmpEQ(savedCanary, fsCanary, "canary_same");
  builder.CreateCondBr(same, success, failBlock);

  builder.SetInsertPoint(success);
  builder.CreateRetVoid();

  builder.SetInsertPoint(failBlock);
  builder.CreateCall(failFunction->getFunctionType(), failFunction, {});
  builder.CreateUnreachable();
  return function;
}

bool testPhiIncomingMatchesPredecessors() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "branch_merge", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 2),
                "RAX");
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "merged");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("branch_merge");
  return expect(summary.LoadsReplaced == 1, "branch load was not replaced") &&
         expect(summary.PhisCreated >= 1, "complete PHI was not created") &&
         expect(rewritten != nullptr, "branch function was not preserved") &&
         expect(!hasLiveReplacedRegisterLoad(*rewritten),
                "replaced load was reused by completed PHI") &&
         verifyOk(module, "module failed verifier after summary SSA PHI test");
}

bool testDuplicatePredecessorEdgesKeepPhiComplete() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-duplicate-edge-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                              {llvm::Type::getInt32Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "duplicate_edge_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *other =
      llvm::BasicBlock::Create(context, "other", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  llvm::SwitchInst *switchInst =
      builder.CreateSwitch(function->getArg(0), join, 2);
  switchInst->addCase(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1), join);
  switchInst->addCase(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 2), other);
  builder.SetInsertPoint(other);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 3),
                "RAX");
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "merged");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "duplicate-edge load was not replaced") &&
         expect(summary.PhisCreated >= 1,
                "duplicate predecessor edge PHI was not completed") &&
         verifyOk(module,
                  "module failed verifier after duplicate-edge PHI test");
}

bool testPartialReadLoopPassthroughUsesDominatorTree() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-loop-dt", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rsi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rsi->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_read_loop_passthrough", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *header =
      llvm::BasicBlock::Create(context, "header", function);
  llvm::BasicBlock *updateRcx =
      llvm::BasicBlock::Create(context, "update_rcx", function);
  llvm::BasicBlock *updateRsi =
      llvm::BasicBlock::Create(context, "update_rsi", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 0),
                "RCX");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 0),
                "RSI");
  builder.CreateBr(header);

  builder.SetInsertPoint(header);
  llvm::Value *headerRsi = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *done = builder.CreateICmpEQ(
      headerRsi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 3));
  builder.CreateCondBr(done, exit, updateRcx);

  builder.SetInsertPoint(updateRcx);
  llvm::Value *rcxLow = builder.CreateCall(
      partialRead,
      {rcx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *rcxNext =
      builder.CreateAdd(rcxLow, llvm::ConstantInt::get(rcxLow->getType(), 1));
  builder.CreateCall(partialWrite, {rcx, rcxNext,
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(context), 0)});
  builder.CreateCondBr(function->getArg(0), join, updateRsi);

  builder.SetInsertPoint(updateRsi);
  llvm::Value *rsiLow = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *rsiNext =
      builder.CreateAdd(rsiLow, llvm::ConstantInt::get(rsiLow->getType(), 1));
  builder.CreateCall(partialWrite, {rsi, rsiNext,
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  llvm::Value *joinedRsi = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *keepGoing = builder.CreateICmpULT(
      joinedRsi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 5));
  builder.CreateCondBr(keepGoing, header, exit);

  builder.SetInsertPoint(exit);
  llvm::Value *result = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRet(result);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hasUnknown = false;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    hasUnknown |= isUnknownValueCall(&inst);
  }

  return expect(summary.LoadsReplaced >= 4,
                "loop partial reads were not rewritten") &&
         expect(!hasPartialReadCall(*function),
                "loop partial read helper remained") &&
         expect(!hasUnknown,
                "loop passthrough partial read created an unknown value") &&
         verifyOk(module,
                  "module failed verifier after partial read loop DT test");
}

bool testRegisterPointerPhiLoadIsCanonicalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-register-pointer-phi-load", context);
  attachTestAbiWithInputs(module, {"R12", "R13"});
  llvm::GlobalVariable *r12 = createRegisterGlobal(module, "R12");
  llvm::GlobalVariable *r13 = createRegisterGlobal(module, "R13");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "register_pointer_phi_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::PHINode *pointerPhi =
      builder.CreatePHI(llvm::PointerType::get(context, 0), 2, "reg.ptr");
  pointerPhi->addIncoming(r12, left);
  pointerPhi->addIncoming(r13, right);
  llvm::LoadInst *loaded =
      builder.CreateLoad(llvm::Type::getInt64Ty(context), pointerPhi, "merged");
  loaded->setMetadata("notdec.register.access",
                      registerAccessMetadata(context, "R12"));
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(!hasLoadFromPhiPointer(*function),
                "register pointer PHI load remained after SummarySSA") &&
         expect(summary.LoadsReplaced >= 1,
                "canonicalized register load was not consumed") &&
         verifyOk(
             module,
             "module failed verifier after register pointer PHI load test");
}

bool testUnknownPhiIncomingUsesFrozenPoison() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-frozen-unknown", context);
  attachTestAbi(module);
  llvm::GlobalVariable *r10 = createRegisterGlobal(module, "R10");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *unknownExternal = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "unknown_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);
  builder.SetInsertPoint(left);
  storeRegister(builder, r10, llvm::ConstantInt::get(r10->getValueType(), 42),
                "R10");
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateCall(voidType, unknownExternal);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *loaded = loadRegister(builder, r10, "R10", "merged");
  builder.CreateRet(loaded);

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  bool hasClobberIncoming = false;
  bool hasUndefIncoming = false;
  bool hasZeroIncoming = false;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    for (llvm::Value *incoming : phi->incoming_values()) {
      hasClobberIncoming |= valueNameContains(incoming, "summary_clobber") ||
                            valueNameContains(incoming, "R10.clobber");
      hasUndefIncoming |= llvm::isa<llvm::UndefValue>(incoming);
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(incoming);
      hasZeroIncoming |= constant != nullptr && constant->isZero();
    }
  }

  return expect(summary.LoadsReplaced == 1,
                "unknown incoming load was not replaced") &&
         expect(summary.CallClobberValues >= 1,
                "external clobber effect was not observed") &&
         expect(hasClobberIncoming,
                "clobber incoming was not materialized explicitly") &&
         expect(!hasUndefIncoming, "unknown incoming still used bare undef") &&
         expect(!hasZeroIncoming, "unknown incoming was folded to zero") &&
         verifyOk(module,
                  "module failed verifier after opaque unknown incoming test");
}

bool testSelfOnlyPhiBecomesOpaqueUnknown() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-self-only-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *r10 = createRegisterGlobal(module, "R10");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "self_only_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::BasicBlock *unreachableLoop =
      llvm::BasicBlock::Create(context, "unreachable_loop", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), loop, exit);
  builder.SetInsertPoint(loop);
  llvm::LoadInst *loaded = loadRegister(builder, r10, "R10", "known_value");
  builder.CreateRet(loaded);
  builder.SetInsertPoint(exit);
  builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0));
  builder.SetInsertPoint(unreachableLoop);
  llvm::LoadInst *selfLoaded =
      loadRegister(builder, r10, "R10", "self_loop_value");
  llvm::Value *sinkPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096),
      llvm::PointerType::get(context, 0), "self_loop_sink");
  builder.CreateStore(selfLoaded, sinkPointer);
  builder.CreateCondBr(function->getArg(0), unreachableLoop, unreachableLoop);

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  bool hasSelfLoopUnknown = false;
  for (llvm::Instruction &inst : *unreachableLoop) {
    hasSelfLoopUnknown |= isUnknownValueCall(&inst);
  }

  return expect(summary.LoadsReplaced >= 1,
                "self-only PHI test did not replace any load") &&
         expect(summary.PhisSimplified >= 1,
                "self-only PHI was not simplified") &&
         expect(hasSelfLoopUnknown,
                "self-only PHI did not become opaque unknown") &&
         verifyOk(module,
                  "module failed verifier after self-only PHI unknown test");
}

bool testFsOffsetPreservedAcrossExternalCall() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-fs-offset-preserve", context);
  attachTestAbi(module);
  llvm::GlobalVariable *fsOffset = createRegisterGlobal(module, "FS_OFFSET");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "fs_preserved", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  llvm::LoadInst *direct =
      loadRegister(builder, fsOffset, "FS_OFFSET", "fs_direct");
  (void)direct;
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateCall(voidType, external);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *merged =
      loadRegister(builder, fsOffset, "FS_OFFSET", "fs_merged");
  builder.CreateRet(merged);

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  bool hasZeroFsPhiIncoming = false;
  bool hasFreezeFsPhiIncoming = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr ||
        phi->getMetadata("notdec.register.summary_ssa.phi") == nullptr) {
      continue;
    }
    for (llvm::Value *incoming : phi->incoming_values()) {
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(incoming);
      hasZeroFsPhiIncoming |= constant != nullptr && constant->isZero();
      hasFreezeFsPhiIncoming |= llvm::isa<llvm::FreezeInst>(incoming);
    }
  }

  return expect(summary.LoadsReplaced >= 1,
                "FS_OFFSET load was not replaced") &&
         expect(summary.UnknownCallEffects == 0,
                "external call clobbered FS_OFFSET") &&
         expect(!hasZeroFsPhiIncoming,
                "FS_OFFSET PHI used zero as unknown incoming") &&
         expect(!hasFreezeFsPhiIncoming,
                "FS_OFFSET PHI used frozen poison as incoming") &&
         verifyOk(module,
                  "module failed verifier after FS_OFFSET preserve test");
}

bool testPreservedCallKeepsPreviousValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-preserved-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "preserves_rax", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *stored = llvm::ConstantInt::get(rax->getValueType(), 42);
  storeRegister(builder, rax, stored, "RAX");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "after_call");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "preserved call load not replaced") &&
         expect(summary.DeadLoadsRemoved == 1,
                "preserved call replaced load was not removed") &&
         verifyOk(module, "module failed verifier after preserved call test");
}

bool testRewrittenExternalCallPreservesUnaffectedRegister() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-rewritten-external-preserve", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "__errno_location", module);
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "rewritten_external_preserves_rbx", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rbx, llvm::ConstantInt::get(rbx->getValueType(), 42),
                "RBX");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *raxValue = loadRegister(builder, rax, "RAX", "rax.ret");
  llvm::LoadInst *rbxValue = loadRegister(builder, rbx, "RBX", "rbx.after");
  builder.CreateRet(builder.CreateAdd(raxValue, rbxValue));

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("rewritten_external_preserves_rbx");
  llvm::Function *rewrittenCallee = module.getFunction("__errno_location");

  return expect(rewrittenCaller != nullptr, "rewritten caller missing") &&
         expect(rewrittenCallee != nullptr, "rewritten errno callee missing") &&
         expect(rewrittenCallee->getReturnType()->isIntegerTy(64),
                "errno call was not rewritten to a typed return") &&
         expect(!hasRegisterLoad(*rewrittenCaller, "RBX"),
                "RBX load remained after rewritten preserved call") &&
         expect(!hasRegisterStore(*rewrittenCaller, "RBX"),
                "RBX store remained after rewritten preserved call") &&
         expect(summary.CallsRewritten >= 1,
                "external call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after rewritten preserved call test");
}

bool testDemandedReturnCreatesCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-demanded-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "writes_rax", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  storeRegister(calleeBuilder, rax,
                llvm::ConstantInt::get(rax->getValueType(), 7), "RAX");
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "uses_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.CallReturnValues == 1,
                "demanded return helper was not created") &&
         expect(summary.DeadLoadsRemoved == 1,
                "return replaced load was not removed") &&
         verifyOk(module, "module failed verifier after demanded return test");
}

bool testExternalReturnUsesRangeCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-return-range", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_return_range", module);
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "uses_external_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(calleeType, external);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  llvm::Function *rewritten = module.getFunction("external_return_range");
  bool typedExternalReturn = false;
  for (llvm::Instruction &inst : llvm::instructions(*caller)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() == rewritten &&
        call->getType()->isIntegerTy(64)) {
      typedExternalReturn = true;
    }
  }

  return expect(summary.CallReturnValues == 1,
                "external return range helper was not created") &&
         expect(rewritten != nullptr,
                "rewritten external return callee missing") &&
         expect(!hasLiveReplacedRegisterLoad(*caller),
                "external return range left live raw RAX load") &&
         expect(typedExternalReturn,
                "external return call was not rebuilt with integer return") &&
         expect(!moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_return.i64"),
                "external return helper remained after rewrite") &&
         verifyOk(module,
                  "module failed verifier after external return range test");
}

bool testIndirectCallReturnHelperIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-indirect-call-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "uses_indirect_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 7),
                "RDI");
  llvm::Value *callee = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x1234),
      llvm::PointerType::get(context, 0), "callee.ptr");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);

  bool typedIndirectReturn = false;
  for (llvm::Instruction &inst : llvm::instructions(*caller)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() == nullptr &&
        call->getType()->isIntegerTy(64)) {
      typedIndirectReturn = true;
    }
  }

  return expect(summary.CallReturnValues == 1,
                "indirect call did not create return helper before rewrite") &&
         expect(summary.CallsRewritten >= 1,
                "indirect callsite was not rewritten") &&
         expect(typedIndirectReturn,
                "indirect call was not rebuilt with integer return type") &&
         expect(!moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_return.i64"),
                "indirect call return helper remained") &&
         verifyOk(module,
                  "module failed verifier after indirect call return rewrite");
}

bool testIntrinsicDoesNotCreateCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-intrinsic-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "intrinsic_between_registers", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *stored = llvm::ConstantInt::get(rax->getValueType(), 42);
  storeRegister(builder, rax, stored, "RAX");
  llvm::Function *ctpop = llvm::Intrinsic::getOrInsertDeclaration(
      &module, llvm::Intrinsic::ctpop, {rax->getValueType()});
  builder.CreateCall(ctpop->getFunctionType(), ctpop, {stored});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "after_intrinsic");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "intrinsic-separated load was not replaced") &&
         expect(summary.CallReturnValues == 0,
                "intrinsic call created a register return helper") &&
         verifyOk(module, "module failed verifier after intrinsic call test");
}

bool testOverwrittenStoreIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "overwritten_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 2),
                "RAX");
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "final_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1, "final load was not replaced") &&
         expect(summary.DeadLoadsRemoved == 1,
                "final replaced load was not removed") &&
         expect(summary.DeadStoresRemoved == 2,
                "overwritten stores were not removed") &&
         verifyOk(module, "module failed verifier after dead store test");
}

bool testCrossBlockDeadStoreIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-cross-block-dead-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "cross_block_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *next = llvm::BasicBlock::Create(context, "next", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  builder.CreateBr(next);
  builder.SetInsertPoint(next);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 2),
                "RAX");
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "final_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "cross-block final load was not replaced") &&
         expect(summary.DeadStoresRemoved == 2,
                "cross-block dead stores were not removed") &&
         verifyOk(module, "module failed verifier after cross-block test");
}

bool testAbiInputStoreBeforeCallIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-input-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);

  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "call_input_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 42),
                "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool callRewritten = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "external_callee" &&
        call->arg_size() == 1) {
      callRewritten = true;
    }
  }
  return expect(summary.DeadStoresRemoved == 1,
                "ABI input store before call was not removed") &&
         expect(summary.CallArgStoresMarked == 1,
                "ABI input store before call was not collected") &&
         expect(callRewritten, "ABI input call was not rewritten") &&
         verifyOk(module, "module failed verifier after call input test");
}

bool testI386KnownExternalUsesCspecStackOffset() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-stack-known-external", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module, 12);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "puts", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "i386_stack_known", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *espEntry = loadRegister(builder, esp, "ESP", "esp.entry", 4);
  llvm::Value *argAddress =
      builder.CreateAdd(espEntry, llvm::ConstantInt::get(i32, 8), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 42), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction = module.getFunction("i386_stack_known");
  if (!expect(rewrittenFunction != nullptr,
              "i386 stack external caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "puts") {
      rewritten = call;
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(0));
  return expect(rewritten != nullptr, "i386 stack external call missing") &&
         expect(rewritten->arg_size() == 1,
                "i386 stack external did not use one stack arg") &&
         expect(constant != nullptr && constant->getZExtValue() == 42,
                "i386 stack arg did not come from cspec offset") &&
         expect(summary.CallArgStoresMarked == 1,
                "i386 stack arg store was not marked") &&
         verifyOk(module,
                  "module failed verifier after i386 stack external rewrite");
}

bool testI386AlignedStackPointerOutgoingArgIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-aligned-stack-outgoing", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "puts", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_aligned_stack_outgoing", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *espEntry = loadRegister(builder, esp, "ESP", "esp.entry", 4);
  llvm::Value *alignedEsp = builder.CreateAnd(
      espEntry, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegister(builder, esp, alignedEsp, "ESP", 4);
  llvm::Value *argPointer =
      builder.CreateIntToPtr(alignedEsp, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 42), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_aligned_stack_outgoing");
  llvm::CallInst *rewritten = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "puts") {
        rewritten = call;
      }
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(0));
  return expect(rewritten != nullptr,
                "i386 aligned stack external call missing") &&
         expect(rewritten->arg_size() == 1,
                "i386 aligned stack external did not use one stack arg") &&
         expect(constant != nullptr && constant->getZExtValue() == 42,
                "i386 aligned stack arg did not bind the store") &&
         expect(summary.CallArgStoresMarked == 1,
                "i386 aligned stack arg store was not marked") &&
         verifyOk(module,
                  "module failed verifier after aligned stack argument test");
}

bool testI386UnknownExternalDoesNotReusePreviousStackArgs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-stack-evidence-lifetime", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *first = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "first_unknown", module);
  llvm::Function *second = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "second_unknown", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_stack_evidence_lifetime", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *espEntry = loadRegister(builder, esp, "ESP", "esp.entry", 4);
  llvm::Value *alignedEsp = builder.CreateAnd(
      espEntry, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegister(builder, esp, alignedEsp, "ESP", 4);
  llvm::Value *slot0 =
      builder.CreateIntToPtr(alignedEsp, llvm::PointerType::get(context, 0));
  llvm::Value *slot4Address = builder.CreateAdd(
      alignedEsp, llvm::ConstantInt::get(i32, 4), "stack.slot4.addr");
  llvm::Value *slot4 =
      builder.CreateIntToPtr(slot4Address, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 11), slot0);
  builder.CreateStore(llvm::ConstantInt::get(i32, 22), slot4);
  builder.CreateCall(calleeType, first);
  builder.CreateStore(llvm::ConstantInt::get(i32, 33), slot0);
  builder.CreateCall(calleeType, second);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_stack_evidence_lifetime");
  llvm::CallInst *firstCall = nullptr;
  llvm::CallInst *secondCall = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call == nullptr || call->getCalledFunction() == nullptr) {
        continue;
      }
      if (call->getCalledFunction()->getName() == "first_unknown") {
        firstCall = call;
      } else if (call->getCalledFunction()->getName() == "second_unknown") {
        secondCall = call;
      }
    }
  }
  return expect(firstCall != nullptr && firstCall->arg_size() == 2,
                "first unknown external did not retain its two stack args") &&
         expect(secondCall != nullptr && secondCall->arg_size() == 1,
                "second unknown external reused a stale stack arg") &&
         verifyOk(module,
                  "module failed verifier after stack evidence lifetime test");
}

bool testI386NativeFrameOutgoingStackArgIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-outgoing-stack", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "puts", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_outgoing", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(i8, 16);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *basePointer = builder.CreateInBoundsGEP(
      i8, stack, llvm::ConstantInt::get(i32, 12), "native.arg.base");
  llvm::Value *baseInteger =
      builder.CreatePtrToInt(basePointer, i32, "native.arg.base.int");
  llvm::Value *argAddress = builder.CreateSub(
      baseInteger, llvm::ConstantInt::get(i32, 12), "native.arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 55), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_outgoing");
  if (!expect(rewrittenFunction != nullptr,
              "i386 native frame outgoing caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "puts") {
      rewritten = call;
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(0));
  return expect(rewritten != nullptr,
                "i386 native frame outgoing call missing") &&
         expect(rewritten->arg_size() == 1,
                "i386 native frame outgoing arg was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 55,
                "i386 native frame outgoing arg did not bind the store") &&
         expect(summary.CallArgStoresMarked == 1,
                "i386 native frame outgoing store was not marked") &&
         verifyOk(module,
                  "module failed verifier after native frame outgoing rewrite");
}

bool testI386NativeFrameLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-load", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(i8, 16);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *slot =
      builder.CreateInBoundsGEP(i8, stack, llvm::ConstantInt::get(i32, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i32, slot);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_load");
  return expect(rewrittenFunction != nullptr,
                "i386 native frame load function missing") &&
         expect(rewrittenFunction->arg_empty(),
                "native frame load was misclassified as stack input") &&
         verifyOk(module,
                  "module failed verifier after native frame load test");
}

bool testI386NativeFrameOutgoingVarArgPrefixIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-vararg-prefix", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, true);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_vararg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(i8, 60);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *basePointer = builder.CreateInBoundsGEP(
      i8, stack, llvm::ConstantInt::get(i32, 12), "native.arg.base");
  llvm::Value *baseInteger =
      builder.CreatePtrToInt(basePointer, i32, "native.arg.base.int");
  for (auto [delta, value] :
       {std::pair<int, int>{-4, 33}, std::pair<int, int>{-8, 1},
        std::pair<int, int>{-12, 22}}) {
    llvm::Value *argAddress = builder.CreateAdd(
        baseInteger, llvm::ConstantInt::get(i32, delta), "native.arg.addr");
    llvm::Value *argPointer =
        builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
    builder.CreateStore(llvm::ConstantInt::get(i32, value), argPointer);
  }
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_vararg");
  if (!expect(rewrittenFunction != nullptr,
              "i386 native frame vararg caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "__fprintf_chk") {
      rewritten = call;
    }
  }
  auto constantArg = [&](unsigned index) -> llvm::ConstantInt * {
    return rewritten == nullptr || rewritten->arg_size() <= index
               ? nullptr
               : llvm::dyn_cast<llvm::ConstantInt>(
                     rewritten->getArgOperand(index));
  };
  llvm::ConstantInt *arg0 = constantArg(0);
  llvm::ConstantInt *arg1 = constantArg(1);
  llvm::ConstantInt *arg2 = constantArg(2);
  return expect(rewritten != nullptr,
                "i386 native frame vararg call missing") &&
         expect(rewritten->arg_size() >= 3,
                "i386 native frame vararg fixed prefix was not rewritten") &&
         expect(arg0 != nullptr && arg0->getZExtValue() == 22,
                "i386 native frame vararg first stack arg was not bound") &&
         expect(arg1 != nullptr && arg1->getZExtValue() == 1,
                "i386 native frame vararg second stack arg was not bound") &&
         expect(arg2 != nullptr && arg2->getZExtValue() == 33,
                "i386 native frame vararg third stack arg was not bound") &&
         expect(summary.CallArgStoresMarked == 3,
                "i386 native frame vararg stores were not marked") &&
         verifyOk(module,
                  "module failed verifier after native frame vararg rewrite");
}

bool testI386StackPointerSummaryOutgoingVarArgPrefixIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-esp-summary-vararg-prefix", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, true);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_esp_summary_vararg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(callBlock);
  llvm::PHINode *esp = builder.CreatePHI(i32, 1, "ESP.range_summary_ssa.test");
  esp->addIncoming(llvm::ConstantInt::get(i32, 0x1000), entry);
  esp->setMetadata("notdec.register.summary_ssa.phi",
                   summaryPhiMetadata(context, "ESP"));
  for (auto [delta, value] :
       {std::pair<int, int>{-4, 33}, std::pair<int, int>{-8, 1},
        std::pair<int, int>{-12, 22}}) {
    llvm::Value *argAddress = builder.CreateAdd(
        esp, llvm::ConstantInt::get(i32, delta), "esp.summary.arg.addr");
    llvm::Value *argPointer =
        builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
    builder.CreateStore(llvm::ConstantInt::get(i32, value), argPointer);
  }
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_esp_summary_vararg");
  if (!expect(rewrittenFunction != nullptr,
              "i386 ESP summary vararg caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "__fprintf_chk") {
      rewritten = call;
    }
  }
  auto constantArg = [&](unsigned index) -> llvm::ConstantInt * {
    return rewritten == nullptr || rewritten->arg_size() <= index
               ? nullptr
               : llvm::dyn_cast<llvm::ConstantInt>(
                     rewritten->getArgOperand(index));
  };
  llvm::ConstantInt *arg0 = constantArg(0);
  llvm::ConstantInt *arg1 = constantArg(1);
  llvm::ConstantInt *arg2 = constantArg(2);
  return expect(rewritten != nullptr, "i386 ESP summary vararg call missing") &&
         expect(rewritten->arg_size() >= 3,
                "i386 ESP summary vararg prefix was not rewritten") &&
         expect(arg0 != nullptr && arg0->getZExtValue() == 22,
                "i386 ESP summary first stack arg was not bound") &&
         expect(arg1 != nullptr && arg1->getZExtValue() == 1,
                "i386 ESP summary second stack arg was not bound") &&
         expect(arg2 != nullptr && arg2->getZExtValue() == 33,
                "i386 ESP summary third stack arg was not bound") &&
         expect(summary.CallArgStoresMarked == 3,
                "i386 ESP summary vararg stores were not marked") &&
         verifyOk(module,
                  "module failed verifier after ESP summary vararg rewrite");
}

bool testX64KnownExternalUsesCspecStackOverflowOffset() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x64-stack-known-external", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module, 16);
  auto *i64 = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");
  std::vector<llvm::GlobalVariable *> regs;
  for (const char *name : {"RDI", "RSI", "RDX", "RCX", "R8", "R9"}) {
    regs.push_back(createRegisterGlobal(module, name));
  }

  auto *calleeType = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "getnameinfo", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "x64_stack_known", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  for (auto [index, reg] : llvm::enumerate(regs)) {
    storeRegister(builder, reg, llvm::ConstantInt::get(i64, index + 1),
                  reg->getName().str());
  }
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *argAddress =
      builder.CreateAdd(rspEntry, llvm::ConstantInt::get(i64, 8), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i64, 77), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction = module.getFunction("x64_stack_known");
  if (!expect(rewrittenFunction != nullptr,
              "x64 stack external caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "getnameinfo") {
      rewritten = call;
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 7
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(6));
  return expect(rewritten != nullptr, "x64 stack external call missing") &&
         expect(rewritten->arg_size() == 7,
                "x64 stack overflow arg was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 77,
                "x64 stack overflow arg did not use cspec offset") &&
         verifyOk(module,
                  "module failed verifier after x64 stack external rewrite");
}

bool testX64AlignedStackPointerOutgoingArgIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x64-aligned-stack-outgoing", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module, 16);
  auto *i64 = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");
  std::vector<llvm::GlobalVariable *> regs;
  for (const char *name : {"RDI", "RSI", "RDX", "RCX", "R8", "R9"}) {
    regs.push_back(createRegisterGlobal(module, name));
  }

  auto *calleeType = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "getnameinfo", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "x64_aligned_stack_outgoing", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  for (auto [index, reg] : llvm::enumerate(regs)) {
    storeRegister(builder, reg, llvm::ConstantInt::get(i64, index + 1),
                  reg->getName().str());
  }
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *alignedRsp = builder.CreateAnd(
      rspEntry, llvm::ConstantInt::get(i64, -16, true), "rsp.aligned");
  storeRegister(builder, rsp, alignedRsp, "RSP");
  llvm::Value *argAddress =
      builder.CreateAdd(alignedRsp, llvm::ConstantInt::get(i64, 8), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i64, 77), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("x64_aligned_stack_outgoing");
  llvm::CallInst *rewritten = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "getnameinfo") {
        rewritten = call;
      }
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 7
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(6));
  return expect(rewritten != nullptr,
                "x64 aligned stack external call missing") &&
         expect(rewritten->arg_size() == 7,
                "x64 aligned stack overflow arg was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 77,
                "x64 aligned stack arg did not bind the store") &&
         verifyOk(module,
                  "module failed verifier after aligned x64 stack rewrite");
}

bool testI386InternalStackInputIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-internal-stack", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "stack_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *calleeEsp =
      loadRegister(calleeBuilder, esp, "ESP", "callee.esp", 4);
  llvm::Value *loadAddress = calleeBuilder.CreateAdd(
      calleeEsp, llvm::ConstantInt::get(i32, 4), "arg.addr");
  llvm::Value *loadPointer = calleeBuilder.CreateIntToPtr(
      loadAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *stackArg = calleeBuilder.CreateLoad(i32, loadPointer);
  calleeBuilder.CreateRet(stackArg);

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "stack_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::LoadInst *callerEsp =
      loadRegister(callerBuilder, esp, "ESP", "caller.esp", 4);
  llvm::Value *storePointer = callerBuilder.CreateIntToPtr(
      callerEsp, llvm::PointerType::get(context, 0));
  callerBuilder.CreateStore(llvm::ConstantInt::get(i32, 123), storePointer);
  callerBuilder.CreateCall(calleeType, callee);
  callerBuilder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCallee = module.getFunction("stack_callee");
  llvm::CallInst *rewrittenCall = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(caller)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() == rewrittenCallee) {
      rewrittenCall = call;
    }
  }
  auto *constant =
      rewrittenCall == nullptr || rewrittenCall->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewrittenCall->getArgOperand(0));
  return expect(rewrittenCallee != nullptr,
                "i386 internal stack callee missing") &&
         expect(rewrittenCallee->arg_size() == 1,
                "i386 internal stack callee was not given an arg") &&
         expect(rewrittenCall != nullptr && rewrittenCall->arg_size() == 1,
                "i386 internal stack call was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 123,
                "i386 internal stack arg did not bind caller store") &&
         verifyOk(module,
                  "module failed verifier after i386 internal stack rewrite");
}

bool testI386AdjustedEspStackInputIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-adjusted-esp-stack", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "adjusted_stack_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *calleeBody =
      llvm::BasicBlock::Create(context, "body", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *entryEsp =
      loadRegister(calleeBuilder, esp, "ESP", "callee.entry_esp", 4);
  llvm::Value *adjustedEsp = calleeBuilder.CreateSub(
      entryEsp, llvm::ConstantInt::get(i32, 20), "callee.esp.sub");
  storeRegister(calleeBuilder, esp, adjustedEsp, "ESP", 4);
  calleeBuilder.CreateBr(calleeBody);
  calleeBuilder.SetInsertPoint(calleeBody);
  llvm::LoadInst *currentEsp =
      loadRegister(calleeBuilder, esp, "ESP", "callee.current_esp", 4);
  // The prologue made ESP entry-20, so this is the ABI's entry-ESP+4 slot.
  llvm::Value *argAddress = calleeBuilder.CreateAdd(
      currentEsp, llvm::ConstantInt::get(i32, 24), "callee.arg.addr");
  llvm::Value *argPointer = calleeBuilder.CreateIntToPtr(
      argAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *stackArg = calleeBuilder.CreateLoad(i32, argPointer);
  calleeBuilder.CreateRet(stackArg);

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "adjusted_stack_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::LoadInst *callerEsp =
      loadRegister(callerBuilder, esp, "ESP", "caller.esp", 4);
  llvm::Value *storePointer = callerBuilder.CreateIntToPtr(
      callerEsp, llvm::PointerType::get(context, 0));
  callerBuilder.CreateStore(llvm::ConstantInt::get(i32, 123), storePointer);
  callerBuilder.CreateCall(calleeType, callee);
  callerBuilder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCallee = module.getFunction("adjusted_stack_callee");
  llvm::Function *rewrittenCaller = module.getFunction("adjusted_stack_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewrittenCallee) {
        rewrittenCall = call;
      }
    }
  }
  auto *constant =
      rewrittenCall == nullptr || rewrittenCall->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewrittenCall->getArgOperand(0));
  return expect(rewrittenCallee != nullptr,
                "adjusted i386 stack callee missing") &&
         expect(rewrittenCallee->arg_size() == 1,
                "adjusted i386 stack callee was not given one arg") &&
         expect(rewrittenCallee->getArg(0)->getName() == "stack+4.arg",
                "adjusted i386 ESP did not resolve to stack+4") &&
         expect(rewrittenCall != nullptr && rewrittenCall->arg_size() == 1,
                "adjusted i386 stack call was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 123,
                "adjusted i386 stack arg did not bind caller store") &&
         verifyOk(module,
                  "module failed verifier after adjusted i386 stack rewrite");
}

bool testI386NegativeEspLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-negative-esp-local", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "negative_esp_local", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryEsp = loadRegister(builder, esp, "ESP", "entry.esp", 4);
  llvm::Value *adjustedEsp =
      builder.CreateSub(entryEsp, llvm::ConstantInt::get(i32, 32), "esp.sub");
  storeRegister(builder, esp, adjustedEsp, "ESP", 4);
  llvm::LoadInst *currentEsp =
      loadRegister(builder, esp, "ESP", "current.esp", 4);
  llvm::Value *localAddress = builder.CreateAdd(
      currentEsp, llvm::ConstantInt::get(i32, 8), "local.addr");
  llvm::Value *localPointer =
      builder.CreateIntToPtr(localAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i32, localPointer);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("negative_esp_local");
  return expect(rewritten != nullptr, "negative ESP local function missing") &&
         expect(rewritten->arg_empty(),
                "negative ESP local load became a stack parameter") &&
         verifyOk(module,
                  "module failed verifier after negative ESP local test");
}

bool testI386AlignedEspLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-aligned-esp-local", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "aligned_esp_local", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryEsp = loadRegister(builder, esp, "ESP", "entry.esp", 4);
  llvm::Value *alignedEsp = builder.CreateAnd(
      entryEsp, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegister(builder, esp, alignedEsp, "ESP", 4);
  llvm::LoadInst *currentEsp =
      loadRegister(builder, esp, "ESP", "current.esp", 4);
  llvm::Value *localAddress = builder.CreateAdd(
      currentEsp, llvm::ConstantInt::get(i32, 4), "local.addr");
  llvm::Value *localPointer =
      builder.CreateIntToPtr(localAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i32, localPointer);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("aligned_esp_local");
  return expect(rewritten != nullptr, "aligned ESP local function missing") &&
         expect(rewritten->arg_empty(),
                "aligned ESP local load became a stack parameter") &&
         verifyOk(module,
                  "module failed verifier after aligned ESP local test");
}

bool testX64AlignedRspLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x64-aligned-rsp-local", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module);
  auto *i64 = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "aligned_rsp_local", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryRsp = loadRegister(builder, rsp, "RSP", "entry.rsp");
  llvm::Value *alignedRsp = builder.CreateAnd(
      entryRsp, llvm::ConstantInt::get(i64, -16, true), "rsp.aligned");
  storeRegister(builder, rsp, alignedRsp, "RSP");
  llvm::LoadInst *currentRsp =
      loadRegister(builder, rsp, "RSP", "current.rsp");
  llvm::Value *localAddress = builder.CreateAdd(
      currentRsp, llvm::ConstantInt::get(i64, 8), "local.addr");
  llvm::Value *localPointer =
      builder.CreateIntToPtr(localAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i64, localPointer);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("aligned_rsp_local");
  return expect(rewritten != nullptr, "aligned RSP local function missing") &&
         expect(rewritten->arg_empty(),
                "aligned RSP local load became a stack parameter") &&
         verifyOk(module,
                  "module failed verifier after aligned RSP local test");
}

bool testKnownZeroArgExternalDropsAbiInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-zero-arg-external", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "random", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "known_zero_arg_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 42),
                "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "random") {
        call = candidate;
      }
    }
  }

  return expect(call != nullptr, "known zero-arg external call missing") &&
         expect(call->arg_empty(),
                "known zero-arg external kept ABI arguments") &&
         expect(summary.DeadStoresRemoved == 1,
                "dead ABI store before zero-arg external was not removed") &&
         verifyOk(module,
                  "module failed verifier after zero-arg external rewrite");
}

bool testKnownZeroArgExternalTypedReturnIsMaterialized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-zero-arg-typed-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidCalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidCalleeType, llvm::GlobalValue::ExternalLinkage,
                             "__ctype_b_loc", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "known_zero_arg_typed_return", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidCalleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  builder.CreateRet(raxLoad);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("__ctype_b_loc");
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
      call = candidate;
    }
  }

  return expect(rewritten != nullptr, "typed zero-arg external missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "typed zero-arg external kept void return") &&
         expect(call != nullptr, "typed zero-arg external call missing") &&
         expect(call->getType()->isIntegerTy(64),
                "typed zero-arg external call kept void return") &&
         expect(summary.Warnings.empty(),
                "typed zero-arg external left register SSA warning") &&
         verifyOk(
             module,
             "module failed verifier after typed zero-arg external rewrite");
}

bool testKnownErrnoLocationReturnIsMaterialized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-errno-location-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidCalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidCalleeType, llvm::GlobalValue::ExternalLinkage,
                             "__errno_location", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "errno_location_return_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidCalleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  builder.CreateRet(raxLoad);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("__errno_location");
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
      call = candidate;
    }
  }

  return expect(rewritten != nullptr, "errno_location external missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "errno_location kept void return") &&
         expect(call != nullptr, "errno_location call missing") &&
         expect(call->getType()->isIntegerTy(64),
                "errno_location call kept void return") &&
         expect(summary.Warnings.empty(),
                "errno_location left register SSA warning") &&
         verifyOk(module,
                  "module failed verifier after errno_location rewrite");
}

bool testKnownErrnoLocationReturnUsesI386DataLayout() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-errno-location-i386-return", context);
  module.setDataLayout("e-p:32:32");
  attachI386TestAbi(module);
  llvm::GlobalVariable *eax = createRegisterGlobal(
      module, "EAX", llvm::Type::getInt32Ty(context), 0, 4);

  auto *voidCalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidCalleeType, llvm::GlobalValue::ExternalLinkage,
                             "__errno_location", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "errno_location_i386_return_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidCalleeType, callee);
  llvm::LoadInst *eaxLoad = loadRegister(builder, eax, "EAX", "eax.after", 4);
  builder.CreateRet(eaxLoad);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("__errno_location");
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
      call = candidate;
    }
  }

  return expect(rewritten != nullptr, "i386 errno_location external missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(32),
                "i386 errno_location did not use pointer-width return") &&
         expect(call != nullptr, "i386 errno_location call missing") &&
         expect(call->getType()->isIntegerTy(32),
                "i386 errno_location call did not use pointer-width return") &&
         expect(summary.Warnings.empty(),
                "i386 errno_location left register SSA warning") &&
         verifyOk(module,
                  "module failed verifier after i386 errno_location rewrite");
}

bool testKnownFixedArgExternalTruncatesAbiInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-fixed-arg-external", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "lseek", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "known_fixed_arg_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "lseek") {
        call = candidate;
      }
    }
  }

  return expect(call != nullptr, "known fixed-arg external call missing") &&
         expect(call->arg_size() == 3,
                "known fixed-arg external used wrong arity") &&
         expect(summary.DeadStoresRemoved == 4,
                "dead ABI stores before fixed-arg external were not removed") &&
         verifyOk(module,
                  "module failed verifier after fixed-arg external rewrite");
}

bool testCallArgUsesPartialRangeRead() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-arg-partial-range", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "call_arg_partial_range", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 42),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  function = module.getFunction("call_arg_partial_range");
  llvm::CallInst *call = nullptr;
  unsigned i64Phis = 0;
  if (function == nullptr) {
    return expect(false, "partial range argument function missing");
  }
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "external_callee") {
        call = candidate;
      }
    }
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst)) {
      if (phi->getType()->isIntegerTy(64)) {
        ++i64Phis;
      }
    }
  }

  return expect(call != nullptr, "partial range argument call missing") &&
         expect(call->arg_size() == 1,
                "partial range argument call was not rewritten") &&
         expect(call->getArgOperand(0)->getType()->isIntegerTy(64),
                "partial range argument kept wrong type") &&
         expect(!hasPartialWriteCall(*function),
                "partial range argument left partial write helper") &&
         expect(i64Phis == 0,
                "partial range argument created whole-register phi") &&
         expect(summary.CallsRewritten >= 1,
                "partial range argument call was not counted as rewritten") &&
         verifyOk(module,
                  "module failed verifier after partial range argument test");
}

bool testDeadFlagStoreBeforeCallIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-flag-before-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *of = createRegisterGlobal(
      module, "OF", llvm::Type::getInt8Ty(context), 523, 1);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);

  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "dead_flag_store_before_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, of, llvm::ConstantInt::get(of->getValueType(), 1),
                "OF");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.DeadStoresRemoved == 1,
                "dead flag store before call was not removed") &&
         verifyOk(module, "module failed verifier after dead flag store test");
}

bool testFlagStoreBeforeVolatileCallIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-clobbered-flag-after-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *of = createRegisterGlobal(
      module, "OF", llvm::Type::getInt8Ty(context), 523, 1);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);
  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                             "clobbered_flag_store_after_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, of, llvm::ConstantInt::get(of->getValueType(), 1),
                "OF");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, of, "OF", "of.after");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.DeadStoresRemoved == 1,
                "clobbered flag store before call was not removed") &&
         expect(summary.CallClobberValues >= 1,
                "flag read after call did not materialize clobber value") &&
         verifyOk(module, "module failed verifier after clobbered flag test");
}

bool testPostRewriteInstCombineExposesDeadFlagStore() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-rewrite-instcombine", context);
  attachTestAbi(module);
  llvm::GlobalVariable *of = createRegisterGlobal(
      module, "OF", llvm::Type::getInt8Ty(context), 523, 1);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);
  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                             "post_instcombine_flag_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, of, llvm::ConstantInt::get(of->getValueType(), 1),
                "OF");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, of, "OF", "of.after");
  llvm::Value *deadUse =
      builder.CreateSelect(llvm::ConstantInt::getFalse(context), loaded,
                           llvm::ConstantInt::get(of->getValueType(), 0));
  builder.CreateRet(deadUse);

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnablePostRewriteInstCombine = true;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);
  unsigned ofStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store != nullptr &&
        store->getPointerOperand()->stripPointerCasts() == of) {
      ++ofStores;
    }
  }

  return expect(summary.DeadStoresRemoved == 1,
                "post-rewrite instcombine did not expose dead flag store") &&
         expect(ofStores == 0, "dead OF store remained after cleanup loop") &&
         verifyOk(module,
                  "module failed verifier after post-rewrite cleanup test");
}

bool testKnownFiveArgExternalUsesFiveInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-five-arg-external", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "setsockopt", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "known_five_arg_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                "R8");
  storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 6),
                "R9");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "setsockopt") {
        call = candidate;
      }
    }
  }

  return expect(call != nullptr, "known five-arg external call missing") &&
         expect(call->arg_size() == 5,
                "known five-arg external used wrong arity") &&
         expect(summary.DeadStoresRemoved == 6,
                "dead ABI stores before five-arg external were not removed") &&
         verifyOk(module,
                  "module failed verifier after five-arg external rewrite");
}

bool testKnownFixedExternalArities() {
  struct KnownArityCase {
    const char *Name;
    unsigned Args;
  };
  const KnownArityCase cases[] = {
      {"__memmove_chk", 4},
      {"__open64_2", 2},
      {"__cxa_atexit", 3},
      {"__fgets_chk", 4},
      {"__getdelim", 4},
      {"__read_chk", 4},
      {"__register_atfork", 4},
      {"__sched_cpucount", 2},
      {"__isoc23_strtoll", 3},
      {"__isoc23_strtoul", 3},
      {"__isoc23_strtoull", 3},
      {"getdelim", 4},
      {"__poll_chk", 4},
      {"__longjmp_chk", 2},
      {"__sigsetjmp", 2},
      {"__strcpy_chk", 3},
      {"__sysconf", 1},
      {"__vfprintf_chk", 4},
      {"__vsnprintf_chk", 6},
      {"__xpg_strerror_r", 3},
      {"ERR_error_string_n", 3},
      {"ERR_clear_error", 0},
      {"ERR_get_error", 0},
      {"ERR_peek_last_error", 0},
      {"ERR_print_errors_fp", 1},
      {"ERR_reason_error_string", 1},
      {"OPENSSL_init_crypto", 2},
      {"OPENSSL_init_ssl", 2},
      {"BN_sub", 3},
      {"accept", 3},
      {"accept4", 4},
      {"arc4random", 0},
      {"cfmakeraw", 1},
      {"chmod", 2},
      {"chown", 3},
      {"closedir", 1},
      {"closelog", 0},
      {"fork", 0},
      {"clock_getres", 2},
      {"dlclose", 1},
      {"dlerror", 0},
      {"dlopen", 2},
      {"dup", 1},
      {"dup3", 3},
      {"endutxent", 0},
      {"epoll_create", 1},
      {"epoll_create1", 1},
      {"epoll_ctl", 4},
      {"epoll_pwait", 5},
      {"epoll_wait", 4},
      {"execv", 2},
      {"execvp", 2},
      {"eventfd", 2},
      {"event_base_free", 1},
      {"event_base_loop", 2},
      {"event_base_loopexit", 2},
      {"event_base_new_with_config", 1},
      {"event_config_free", 1},
      {"event_config_new", 0},
      {"event_config_set_flag", 2},
      {"event_get_version", 0},
      {"fdatasync", 1},
      {"freeifaddrs", 1},
      {"av_freep", 1},
      {"fgetc", 1},
      {"fileno", 1},
      {"fopen64", 2},
      {"fputs", 2},
      {"fsync", 1},
      {"fstat", 2},
      {"fstatfs64", 2},
      {"ftruncate", 2},
      {"futimens", 2},
      {"gai_strerror", 1},
      {"getaddrinfo", 4},
      {"getdelim", 4},
      {"getnameinfo", 7},
      {"getegid", 0},
      {"getentropy", 2},
      {"getgid", 0},
      {"getservbyname", 2},
      {"getgrgid", 1},
      {"getgrgid_r", 5},
      {"getgrnam", 1},
      {"gethostbyname", 1},
      {"gethostname", 2},
      {"getopt_long", 5},
      {"getifaddrs", 1},
      {"getloadavg", 2},
      {"getpagesize", 0},
      {"getpeername", 3},
      {"getpwuid", 1},
      {"getpwuid_r", 5},
      {"getrlimit", 2},
      {"getrlimit64", 2},
      {"getrusage", 2},
      {"getsockopt", 5},
      {"getsubopt", 3},
      {"getxattr", 4},
      {"getc", 1},
      {"gnu_get_libc_version", 0},
      {"glob64", 4},
      {"globfree64", 1},
      {"if_indextoname", 2},
      {"inet_ntoa", 1},
      {"inet_pton", 3},
      {"initgroups", 2},
      {"inotify_add_watch", 3},
      {"inotify_init1", 1},
      {"inotify_rm_watch", 2},
      {"isatty", 1},
      {"kill", 2},
      {"lchown", 3},
      {"link", 2},
      {"localtime", 1},
      {"localtime_r", 2},
      {"lstat", 2},
      {"lstat64", 2},
      {"madvise", 3},
      {"memchr", 3},
      {"mempcpy", 3},
      {"getline", 3},
      {"mkdir", 2},
      {"mkdtemp", 1},
      {"mmap", 6},
      {"mmap64", 6},
      {"mkostemp64", 2},
      {"mkstemp64", 1},
      {"mlockall", 1},
      {"msync", 3},
      {"nettle_arcfour_crypt", 4},
      {"nettle_arcfour_set_key", 3},
      {"nettle_knuth_lfib_get", 1},
      {"nettle_knuth_lfib_init", 2},
      {"nettle_sha256_digest", 3},
      {"nettle_sha256_init", 1},
      {"nettle_sha256_update", 3},
      {"nettle_yarrow256_init", 3},
      {"pathconf", 2},
      {"pcre2_code_free_8", 1},
      {"pcre2_get_error_message_8", 3},
      {"pcre2_get_ovector_pointer_8", 1},
      {"pcre2_jit_compile_8", 2},
      {"pcre2_match_data_create_8", 2},
      {"pcre2_match_data_create_from_pattern_8", 2},
      {"pcre2_match_data_free_8", 1},
      {"pcre2_pattern_info_8", 3},
      {"pipe", 1},
      {"pipe2", 2},
      {"posix_memalign", 3},
      {"posix_spawn_file_actions_destroy", 1},
      {"posix_spawn_file_actions_addclosefrom_np", 2},
      {"posix_spawn_file_actions_adddup2", 3},
      {"posix_spawn_file_actions_addfchdir_np", 2},
      {"posix_spawn_file_actions_init", 1},
      {"posix_spawnattr_destroy", 1},
      {"posix_spawnattr_init", 1},
      {"posix_spawnattr_setflags", 2},
      {"posix_spawnattr_setsigdefault", 2},
      {"posix_spawnattr_setsigmask", 2},
      {"poll", 3},
      {"pread", 4},
      {"pread64", 4},
      {"preadv", 4},
      {"preadv64", 4},
      {"preadv64v2", 5},
      {"av_packet_free", 1},
      {"pthread_attr_destroy", 1},
      {"pthread_attr_init", 1},
      {"pthread_attr_setstacksize", 2},
      {"pthread_barrier_destroy", 1},
      {"pthread_barrier_init", 3},
      {"pthread_barrier_wait", 1},
      {"pthread_cond_broadcast", 1},
      {"pthread_cond_destroy", 1},
      {"pthread_cond_init", 2},
      {"pthread_cond_timedwait", 3},
      {"pthread_cond_wait", 2},
      {"pthread_condattr_destroy", 1},
      {"pthread_condattr_init", 1},
      {"pthread_condattr_setclock", 2},
      {"pthread_create", 4},
      {"pthread_getaffinity_np", 3},
      {"pthread_getschedparam", 3},
      {"pthread_getspecific", 1},
      {"pthread_mutex_destroy", 1},
      {"pthread_mutex_init", 2},
      {"pthread_mutex_trylock", 1},
      {"pthread_mutexattr_destroy", 1},
      {"pthread_mutexattr_init", 1},
      {"pthread_mutexattr_settype", 2},
      {"pthread_once", 2},
      {"pthread_rwlock_destroy", 1},
      {"pthread_rwlock_init", 2},
      {"pthread_rwlock_rdlock", 1},
      {"pthread_rwlock_tryrdlock", 1},
      {"pthread_rwlock_trywrlock", 1},
      {"pthread_rwlock_unlock", 1},
      {"pthread_rwlock_wrlock", 1},
      {"pthread_self", 0},
      {"pthread_setaffinity_np", 3},
      {"pthread_setschedparam", 3},
      {"pthread_setname_np", 2},
      {"putchar", 1},
      {"pututxline", 1},
      {"pwrite", 4},
      {"pwrite64", 4},
      {"pwritev64", 4},
      {"qsort", 4},
      {"popen", 2},
      {"readv", 3},
      {"recvmmsg", 5},
      {"recv", 4},
      {"recvfrom", 6},
      {"sched_get_priority_max", 1},
      {"sched_get_priority_min", 1},
      {"sched_getaffinity", 3},
      {"sched_getcpu", 0},
      {"sched_yield", 0},
      {"sasl_dispose", 1},
      {"sasl_server_init", 2},
      {"sasl_server_start", 6},
      {"sasl_server_step", 5},
      {"scandir64", 4},
      {"select", 5},
      {"sem_destroy", 1},
      {"sem_init", 3},
      {"sem_post", 1},
      {"sem_trywait", 1},
      {"sem_wait", 1},
      {"send", 4},
      {"sendfile", 4},
      {"sendfile64", 4},
      {"sendmmsg", 4},
      {"sendmsg", 3},
      {"strchrnul", 2},
      {"av_strerror", 3},
      {"av_usleep", 1},
      {"SSL_accept", 1},
      {"SSL_clear", 1},
      {"SSL_connect", 1},
      {"SSL_CTX_check_private_key", 1},
      {"SSL_CTX_ctrl", 4},
      {"SSL_CTX_load_verify_locations", 3},
      {"SSL_CTX_new", 1},
      {"SSL_CTX_sess_set_new_cb", 2},
      {"SSL_CTX_set_cipher_list", 2},
      {"SSL_CTX_set_ciphersuites", 2},
      {"SSL_CTX_set_client_CA_list", 2},
      {"SSL_CTX_set_default_verify_paths", 1},
      {"SSL_CTX_set_options", 2},
      {"SSL_CTX_set_session_id_context", 3},
      {"SSL_CTX_set_verify", 3},
      {"SSL_CTX_set_verify_depth", 2},
      {"SSL_CTX_use_certificate_chain_file", 2},
      {"SSL_CTX_use_PrivateKey_file", 3},
      {"SSL_CTX_free", 1},
      {"RSA_set0_key", 4},
      {"setenv", 3},
      {"setgid", 1},
      {"setgroups", 2},
      {"setpriority", 3},
      {"setregid", 2},
      {"setreuid", 2},
      {"setbuf", 2},
      {"setrlimit", 2},
      {"setvbuf", 4},
      {"setuid", 1},
      {"sigaddset", 2},
      {"sigemptyset", 1},
      {"__sysv_signal", 2},
      {"socketpair", 4},
      {"SSL_free", 1},
      {"SSL_get_error", 2},
      {"SSL_load_client_CA_file", 1},
      {"SSL_new", 1},
      {"SSL_pending", 1},
      {"SSL_read", 3},
      {"SSL_set_connect_state", 1},
      {"SSL_set_fd", 2},
      {"SSL_set_info_callback", 2},
      {"SSL_shutdown", 1},
      {"SSL_write", 3},
      {"splice", 6},
      {"stat64", 2},
      {"statfs64", 2},
      {"strerror_r", 3},
      {"strndup", 2},
      {"strnlen", 2},
      {"strtok", 2},
      {"strtoll", 3},
      {"strtoul", 3},
      {"strtoull", 3},
      {"strtok_r", 3},
      {"symlink", 2},
      {"sysconf", 1},
      {"tcgetattr", 2},
      {"tcsetattr", 3},
      {"timegm", 1},
      {"TLS_client_method", 0},
      {"TLS_server_method", 0},
      {"ttyname_r", 3},
      {"tzset", 0},
      {"umask", 1},
      {"unsetenv", 1},
      {"updwtmpx", 2},
      {"utimensat", 4},
      {"usleep", 1},
      {"wait", 1},
      {"writev", 3},
  };

  for (const KnownArityCase &testCase : cases) {
    llvm::LLVMContext context;
    llvm::Module module(std::string("summary-ssa-known-") + testCase.Name,
                        context);
    attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
    llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
    llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
    llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
    llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
    llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
    llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
    llvm::Function *callee = llvm::Function::Create(
        calleeType, llvm::GlobalValue::ExternalLinkage, testCase.Name, module);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
    llvm::Function *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage,
        std::string("known_call_") + testCase.Name, module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                  "RDI");
    storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                  "RSI");
    storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                  "RDX");
    storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                  "RCX");
    storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                  "R8");
    storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 6),
                  "R9");
    builder.CreateCall(calleeType, callee);
    builder.CreateRetVoid();

    notdec::bin2llvm::runNativeRegisterSummarySSA(module);
    llvm::CallInst *call = nullptr;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (candidate->getCalledFunction() != nullptr &&
            candidate->getCalledFunction()->getName() == testCase.Name) {
          call = candidate;
        }
      }
    }

    unsigned expectedArgs = std::min<unsigned>(testCase.Args, 6);
    std::string contextMessage =
        std::string(" for known fixed external ") + testCase.Name;
    std::string missingMessage =
        "known fixed external call missing" + contextMessage;
    std::string arityMessage =
        "known fixed external used wrong arity" + contextMessage;
    if (!expect(call != nullptr, missingMessage.c_str()) ||
        !expect(call->arg_size() == expectedArgs, arityMessage.c_str()) ||
        !verifyOk(module,
                  "module failed verifier after fixed external rewrite")) {
      return false;
    }
  }
  return true;
}

bool testKnownVarArgExternalInfersDefinedAbiInputs() {
  struct KnownVarArgCase {
    const char *Name;
    unsigned FixedArgs;
    unsigned ExpectedCallArgs = 6;
  };
  const KnownVarArgCase cases[] = {
      {"__isoc23_sscanf", 2}, {"__isoc99_sscanf", 2}, {"__asprintf_chk", 3},
      {"__snprintf_chk", 4},  {"__syslog_chk", 2},    {"fscanf", 2},
      {"prctl", 1, 3},
  };

  for (const KnownVarArgCase &testCase : cases) {
    llvm::LLVMContext context;
    llvm::Module module(
        std::string("summary-ssa-known-vararg-") + testCase.Name, context);
    attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
    llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
    llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
    llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
    llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
    llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
    llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
    llvm::Function *callee = llvm::Function::Create(
        calleeType, llvm::GlobalValue::ExternalLinkage, testCase.Name, module);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
    llvm::Function *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage,
        std::string("known_vararg_call_") + testCase.Name, module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                  "RDI");
    storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                  "RSI");
    storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                  "RDX");
    storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                  "RCX");
    storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                  "R8");
    storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 6),
                  "R9");
    builder.CreateCall(calleeType, callee);
    builder.CreateRetVoid();

    auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
    llvm::CallInst *call = nullptr;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (candidate->getCalledFunction() != nullptr &&
            candidate->getCalledFunction()->getName() == testCase.Name) {
          call = candidate;
        }
      }
    }

    bool calleeIsVarArg = false;
    if (call != nullptr && call->getCalledFunction() != nullptr) {
      llvm::FunctionType *rewrittenType =
          call->getCalledFunction()->getFunctionType();
      calleeIsVarArg = rewrittenType->isVarArg() &&
                       rewrittenType->getNumParams() == testCase.FixedArgs;
    }

    if (!expect(call != nullptr, "known vararg external call missing") ||
        !expect(call->arg_size() == testCase.ExpectedCallArgs,
                "known vararg external dropped ABI varargs") ||
        !expect(calleeIsVarArg,
                "known vararg external did not keep vararg function type") ||
        !expect(summary.DeadStoresRemoved == 6,
                "dead ABI stores before vararg external were not removed") ||
        !verifyOk(module,
                  "module failed verifier after vararg external rewrite")) {
      return false;
    }
  }
  return true;
}

bool testKnownVarArgFixedOnlyCallDoesNotReadTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-fixed-only", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  (void)createRegisterGlobal(module, "RCX");
  (void)createRegisterGlobal(module, "R8");
  (void)createRegisterGlobal(module, "R9");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_fixed_only_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_fixed_only_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCaller != nullptr,
                "known vararg fixed-only caller missing") &&
         expect(
             rewrittenCaller->arg_empty(),
             "known vararg fixed-only call leaked a tail register argument") &&
         expect(rewrittenCall != nullptr,
                "known vararg fixed-only call missing") &&
         expect(rewrittenCall->arg_size() == 3,
                "known vararg fixed-only call used wrong arity") &&
         verifyOk(module,
                  "module failed verifier after fixed-only vararg rewrite");
}

bool testKnownVarArgCallsitesKeepIndependentArities() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-callsite-arities", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "vararg_two_calls", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  builder.CreateCall(calleeType, callee);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 4),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 5),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 6),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 7),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 8),
                "R8");
  storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 9),
                "R9");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller = module.getFunction("vararg_two_calls");
  std::vector<unsigned> arities;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "__fprintf_chk") {
        arities.push_back(call->arg_size());
      }
    }
  }

  return expect(arities.size() == 2,
                "known vararg callsite test lost a call") &&
         expect(arities[0] == 3,
                "known vararg fixed-only callsite used wrong arity") &&
         expect(arities[1] == 6,
                "known vararg extended callsite used wrong arity") &&
         verifyOk(module,
                  "module failed verifier after per-callsite vararg rewrite");
}

bool testKnownVarArgDoesNotReuseStaleCallerSavedTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-stale-tail", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "vararg_stale_tail", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                "R8");
  builder.CreateCall(calleeType, callee);

  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 6),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 7),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 8),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 9),
                "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller = module.getFunction("vararg_stale_tail");
  std::vector<unsigned> arities;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "__fprintf_chk") {
        arities.push_back(call->arg_size());
      }
    }
  }

  return expect(arities.size() == 2, "stale-tail test lost a call") &&
         expect(arities[0] == 5, "first vararg call used wrong arity") &&
         expect(arities[1] == 4,
                "second vararg call reused stale R8 as an argument") &&
         verifyOk(module, "module failed verifier after stale-tail rewrite");
}

bool testKnownVarArgUsesSseCountForFloatTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-float-tail", context);
  attachTestIntegerAndFloatVarArgAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_float_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 0),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 0),
                "R8");
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  storeRegister(
      builder, zmm0,
      llvm::ConstantInt::get(zmmType, llvm::APInt(512, 0x3ff0000000000000ULL)),
      "ZMM0");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_float_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCall != nullptr, "known vararg float call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "known vararg float call used wrong arity") &&
         expect(rewrittenCall->getArgOperand(3)->getType()->isDoubleTy(),
                "known vararg float tail was not rewritten to double") &&
         verifyOk(module,
                  "module failed verifier after known vararg float rewrite");
}

bool testKnownVarArgZeroTailKeepsConstantZero() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-zero-tail-constant", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_zero_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 0),
                "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_zero_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }
  return expect(rewrittenCall != nullptr, "known vararg zero call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "known vararg zero call used wrong arity") &&
         expect(
             llvm::isa<llvm::ConstantInt>(rewrittenCall->getArgOperand(3)) &&
                 llvm::cast<llvm::ConstantInt>(rewrittenCall->getArgOperand(3))
                     ->isZero(),
             "known vararg zero tail was not kept as constant zero") &&
         expect(cleanup.VarArgUnknownHelpersLowered == 0,
                "constant zero vararg used an unknown helper") &&
         verifyOk(module,
                  "module failed verifier after known vararg zero rewrite");
}

bool testKnownVarArgUnknownPhiTailUsesPoison() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-unknown-phi-tail-poison",
                      context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_unknown_phi_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *unknownBlock =
      llvm::BasicBlock::Create(context, "unknown", function);
  llvm::BasicBlock *zeroBlock =
      llvm::BasicBlock::Create(context, "zero", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  llvm::Value *cond = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt1Ty(context)), "unknown_cond");
  builder.CreateCondBr(cond, unknownBlock, zeroBlock);

  builder.SetInsertPoint(unknownBlock);
  auto *tail32Type = llvm::Type::getInt32Ty(context);
  llvm::Value *unknown =
      builder.CreateFreeze(llvm::PoisonValue::get(tail32Type), "tail_unknown");
  builder.CreateBr(merge);

  builder.SetInsertPoint(zeroBlock);
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *tail = builder.CreatePHI(tail32Type, 2, "tail");
  tail->addIncoming(unknown, unknownBlock);
  tail->addIncoming(llvm::ConstantInt::get(tail32Type, 0), zeroBlock);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, rcx->getValueType(), tail32Type, 64, 32);
  llvm::Value *tail64 = builder.CreateCall(
      insert,
      {llvm::ConstantInt::get(rcx->getValueType(), 0), tail,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "tail_insert");
  storeRegister(builder, rcx, tail64, "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_unknown_phi_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCall != nullptr,
                "known vararg unknown-phi call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "known vararg unknown-phi call used wrong arity") &&
         expect(llvm::isa<llvm::PoisonValue>(rewrittenCall->getArgOperand(3)),
                "known vararg unknown-phi tail was not rewritten to poison") &&
         expect(cleanup.VarArgUnknownHelpersLowered == 1,
                "unknown-phi vararg helper was not lowered") &&
         verifyOk(module,
                  "module failed verifier after unknown-phi vararg rewrite");
}

bool testNonX64VarArgUnknownPhiTailDoesNotUsePoisonHelper() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-non-x64-vararg-unknown-tail", context);
  attachTestAbiWithInputs(module, {"A0", "A1", "A2", "A3"});
  llvm::GlobalVariable *a0 = createRegisterGlobal(module, "A0");
  llvm::GlobalVariable *a1 = createRegisterGlobal(module, "A1");
  llvm::GlobalVariable *a2 = createRegisterGlobal(module, "A2");
  llvm::GlobalVariable *a3 = createRegisterGlobal(module, "A3");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "non_x64_vararg_unknown_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *unknownBlock =
      llvm::BasicBlock::Create(context, "unknown", function);
  llvm::BasicBlock *zeroBlock =
      llvm::BasicBlock::Create(context, "zero", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, a0, llvm::ConstantInt::get(a0->getValueType(), 1),
                "A0");
  storeRegister(builder, a1, llvm::ConstantInt::get(a1->getValueType(), 2),
                "A1");
  storeRegister(builder, a2, llvm::ConstantInt::get(a2->getValueType(), 3),
                "A2");
  llvm::Value *cond = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt1Ty(context)), "unknown_cond");
  builder.CreateCondBr(cond, unknownBlock, zeroBlock);

  builder.SetInsertPoint(unknownBlock);
  auto *tail32Type = llvm::Type::getInt32Ty(context);
  llvm::Value *unknown =
      builder.CreateFreeze(llvm::PoisonValue::get(tail32Type), "tail_unknown");
  builder.CreateBr(merge);

  builder.SetInsertPoint(zeroBlock);
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *tail = builder.CreatePHI(tail32Type, 2, "tail");
  tail->addIncoming(unknown, unknownBlock);
  tail->addIncoming(llvm::ConstantInt::get(tail32Type, 0), zeroBlock);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, a3->getValueType(), tail32Type, 64, 32);
  llvm::Value *tail64 = builder.CreateCall(
      insert,
      {llvm::ConstantInt::get(a3->getValueType(), 0), tail,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "tail_insert");
  storeRegister(builder, a3, tail64, "A3");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("non_x64_vararg_unknown_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCall != nullptr,
                "non-x64 vararg unknown call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "non-x64 vararg unknown call used wrong arity") &&
         expect(cleanup.VarArgUnknownHelpersLowered == 0,
                "non-x64 vararg used an x64-only unknown helper") &&
         expect(!moduleHasFunctionNamed(module,
                                        "notdec.register.vararg_unknown.i64"),
                "non-x64 vararg unknown helper declaration remained") &&
         verifyOk(module,
                  "module failed verifier after non-x64 vararg rewrite");
}

bool testMismatchedDirectCallUseUsesReturnExtract() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-direct-call-use-return-extract", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_multi_return_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(input);
  for (llvm::StringRef name : {"RAX", "RBX"}) {
    notdec::bin2llvm::NativeAbiParamEntry output;
    output.MinSize = 1;
    output.MaxSize = 8;
    output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    output.Storage.Name = name.str();
    abi.Outputs.push_back(output);

    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "uses_direct_call_result", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  llvm::CallInst *call = builder.CreateCall(calleeType, callee);
  storeRegister(builder, rax, call, "RAX");
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  llvm::LoadInst *rbxLoad = loadRegister(builder, rbx, "RBX", "rbx.after");
  llvm::Value *sum = builder.CreateAdd(call, raxLoad);
  sum = builder.CreateAdd(sum, rbxLoad);
  builder.CreateRet(sum);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hasStructCall = false;
  bool hasExtractedDirectUse = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate == nullptr || candidate->getCalledFunction() == nullptr ||
        candidate->getCalledFunction()->getName() != "unknown_external") {
      continue;
    }
    hasStructCall = candidate->getType()->isStructTy();
    for (llvm::User *user : candidate->users()) {
      auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
      if (extract != nullptr && extract->getType()->isIntegerTy(64)) {
        hasExtractedDirectUse = true;
      }
    }
  }

  return expect(summary.CallsRewritten >= 1,
                "multi-return direct-use call was not rewritten") &&
         expect(hasStructCall, "multi-return call did not return struct") &&
         expect(hasExtractedDirectUse,
                "old direct call use was not replaced by extract") &&
         verifyOk(module,
                  "module failed verifier after direct call use rewrite");
}

bool testKnownExternalUsesSingleIntegerReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-external-single-return", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_external_single_return_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(input);
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry output;
    output.MinSize = 1;
    output.MaxSize = 8;
    output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    output.Storage.Name = name.str();
    abi.Outputs.push_back(output);

    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "fclose", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "uses_fclose_rdx_after", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  llvm::LoadInst *rdxLoad = loadRegister(builder, rdx, "RDX", "rdx.after");
  builder.CreateRet(builder.CreateAdd(raxLoad, rdxLoad));

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
        candidate->getCalledFunction()->getName() == "fclose") {
      call = candidate;
    }
  }

  return expect(call != nullptr, "known external fclose call missing") &&
         expect(
             call->getType()->isIntegerTy(64),
             "known external fclose was widened to a multi-register return") &&
         verifyOk(module,
                  "module failed verifier after known external return rewrite");
}

bool testUnknownExternalTreatsRdxAsClobberNotReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-rdx-clobber", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_rdx_clobber_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry output;
    output.MinSize = 1;
    output.MaxSize = 8;
    output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    output.Storage.Name = name.str();
    abi.Outputs.push_back(output);

    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  auto *sinkType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);
  llvm::Function *sink = llvm::Function::Create(
      sinkType, llvm::GlobalValue::ExternalLinkage, "consume_rdx", module);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_rdx_after", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *rdxLoad = loadRegister(builder, rdx, "RDX", "rdx.after");
  builder.CreateCall(sinkType, sink, {rdxLoad});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hasRdxClobberWarning = false;
  bool hasRdxReturnWarning = false;
  for (const notdec::bin2llvm::NativeRegisterSummarySSAWarning &warning :
       summary.Warnings) {
    if (warning.RegisterName != "RDX") {
      continue;
    }
    hasRdxClobberWarning |= warning.Kind == "clobber";
    hasRdxReturnWarning |= warning.Kind == "return";
  }

  return expect(hasRdxClobberWarning,
                "unknown external RDX did not become clobber warning") &&
         expect(!hasRdxReturnWarning,
                "unknown external RDX was still treated as return") &&
         verifyOk(module,
                  "module failed verifier after unknown external RDX test");
}

bool testUnknownExternalClobberArgBecomesUnknown() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-clobber-arg", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_clobber_arg_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry output;
    output.MinSize = 1;
    output.MaxSize = 8;
    output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    output.Storage.Name = name.str();
    abi.Outputs.push_back(output);

    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *first = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "first_unknown", module);
  llvm::Function *second = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "second_unknown", module);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "clobber_arg_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, first);
  llvm::LoadInst *rdxAfter = loadRegister(builder, rdx, "RDX", "rdx.after");
  storeRegister(builder, rdx, rdxAfter, "RDX");
  builder.CreateCall(voidType, second);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool secondUsesClobber = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call == nullptr || call->getCalledFunction() == nullptr ||
        call->getCalledFunction()->getName() != "second_unknown") {
      continue;
    }
    for (llvm::Value *arg : call->args()) {
      secondUsesClobber |= valueNameContains(arg, "summary_clobber") ||
                           valueNameContains(arg, "RDX.clobber");
    }
  }

  return expect(!secondUsesClobber,
                "unknown external call kept clobber-derived argument") &&
         verifyOk(module,
                  "module failed verifier after clobber arg cleanup test");
}

bool testInternalReturnDoesNotExposeExternalClobber() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-clobber-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 7),
                "RAX");
  builder.CreateCall(voidType, external);
  llvm::LoadInst *rdxAfter = loadRegister(builder, rdx, "RDX", "rdx.after");
  storeRegister(builder, rdx, rdxAfter, "RDX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *rdxResult = loadRegister(builder, rdx, "RDX", "rdx.result");
  llvm::AllocaInst *sink = builder.CreateAlloca(rdx->getValueType());
  builder.CreateStore(rdxResult, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool helperLeft = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_clobber")) {
      helperLeft = !function.use_empty();
    }
  }

  return expect(!helperLeft,
                "internal return exposed external clobber helper") &&
         verifyOk(module,
                  "module failed verifier after clobber return cleanup test");
}

bool testClobberReturnPhiDoesNotMaterializeHelper() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-clobber-return-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return_phi", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *clobberPath =
      llvm::BasicBlock::Create(context, "clobber_path", callee);
  llvm::BasicBlock *knownPath =
      llvm::BasicBlock::Create(context, "known_path", callee);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", callee);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 7),
                "RAX");
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), clobberPath,
                       knownPath);

  builder.SetInsertPoint(clobberPath);
  builder.CreateCall(voidType, external);
  builder.CreateBr(done);

  builder.SetInsertPoint(knownPath);
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  builder.CreateBr(done);

  builder.SetInsertPoint(done);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool helperLeft = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_clobber")) {
      helperLeft = !function.use_empty();
    }
  }

  return expect(!helperLeft,
                "clobber return phi materialized summary_clobber helper") &&
         verifyOk(module,
                  "module failed verifier after clobber return phi test");
}

bool testUnknownExternalArityUsesMaxCallsitePrefix() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-max-arity", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_arity", module);
  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_arity_callers", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *shortCall =
      llvm::BasicBlock::Create(context, "short_call", function);
  llvm::BasicBlock *longCall =
      llvm::BasicBlock::Create(context, "long_call", function);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), shortCall,
                       longCall);

  builder.SetInsertPoint(shortCall);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateBr(done);

  builder.SetInsertPoint(longCall);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 2),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 3),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 4),
                "RDX");
  builder.CreateCall(calleeType, callee);
  builder.CreateBr(done);

  builder.SetInsertPoint(done);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("unknown_external_arity");
  bool hasInconsistentArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterSummarySSAWarning &warning :
       summary.Warnings) {
    hasInconsistentArityWarning |=
        warning.CalleeName == "unknown_external_arity" &&
        warning.Reason == "inconsistent_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external arity callee missing") &&
         expect(rewritten->arg_size() == 3,
                "unknown external arity did not use max callsite prefix") &&
         expect(hasInconsistentArityWarning,
                "unknown external arity mismatch warning missing") &&
         verifyOk(module,
                  "module failed verifier after unknown external arity test");
}

bool testExternalPrototypeJsonOverridesInferredArity() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-json-priority", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "json_known_external", module);
  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "json_known_external_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-prototype", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create external prototype JSON: " << error.message()
              << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_known_external":{"return":"void","fixed_args":1}})";
  }

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.ExternalPrototypeJsonPath = jsonPath.str().str();
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);
  (void)llvm::sys::fs::remove(jsonPath);

  llvm::Function *rewritten = module.getFunction("json_known_external");
  bool inferredWarning = false;
  for (const notdec::bin2llvm::NativeRegisterSummarySSAWarning &warning :
       summary.Warnings) {
    inferredWarning |= warning.CalleeName == "json_known_external" &&
                       warning.Reason == "inferred_unknown_external_arity";
  }
  return expect(rewritten != nullptr,
                "JSON prototype external callee missing") &&
         expect(rewritten->arg_size() == 1,
                "callsite inference overrode JSON prototype arity") &&
         expect(!inferredWarning,
                "JSON prototype external emitted inference warning") &&
         verifyOk(module,
                  "module failed verifier after JSON prototype priority test");
}

bool testUnknownExternalArityStopsAtClobberArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-clobber-arity", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_clobber_arity_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  (void)rdx;

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *knownClobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *unknown =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_clobber_arity", module);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_clobber_arity_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 99),
                "RDI");
  builder.CreateCall(voidType, knownClobber);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, unknown);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("unknown_external_clobber_arity");
  bool hasInferredArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterSummarySSAWarning &warning :
       summary.Warnings) {
    hasInferredArityWarning |=
        warning.CalleeName == "unknown_external_clobber_arity" &&
        warning.Reason == "inferred_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external clobber arity callee missing") &&
         expect(rewritten->arg_size() == 2,
                "unknown external arity counted clobber as argument") &&
         expect(hasInferredArityWarning,
                "unknown external clobber arity warning missing") &&
         verifyOk(module, "module failed verifier after clobber arity test");
}

bool testUnknownExternalArityStopsAtPhiClobberArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-phi-clobber-arity",
                      context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_phi_clobber_arity_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *knownClobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *unknown =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_phi_clobber_arity", module);
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage,
      "unknown_external_phi_clobber_arity_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *clobberPath =
      llvm::BasicBlock::Create(context, "clobber_path", function);
  llvm::BasicBlock *cleanPath =
      llvm::BasicBlock::Create(context, "clean_path", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), clobberPath,
                       cleanPath);

  builder.SetInsertPoint(clobberPath);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 99),
                "RDI");
  builder.CreateCall(voidType, knownClobber);
  builder.CreateBr(join);

  builder.SetInsertPoint(cleanPath);
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 0),
                "RDX");
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, unknown);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("unknown_external_phi_clobber_arity");
  bool hasInferredArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterSummarySSAWarning &warning :
       summary.Warnings) {
    hasInferredArityWarning |=
        warning.CalleeName == "unknown_external_phi_clobber_arity" &&
        warning.Reason == "inferred_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external phi clobber arity callee missing") &&
         expect(rewritten->arg_size() == 2,
                "unknown external arity counted phi clobber as argument") &&
         expect(hasInferredArityWarning,
                "unknown external phi clobber arity warning missing") &&
         verifyOk(module,
                  "module failed verifier after phi clobber arity test");
}

bool testUnknownExternalArityCountsExplicitBinaryArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-binary-clobber-arity",
                      context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName =
      "__summary_ssa_unknown_external_binary_clobber_arity_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *knownClobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *unknown =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_binary_clobber_arity", module);
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage,
      "unknown_external_binary_clobber_arity_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 99),
                "RDI");
  builder.CreateCall(voidType, knownClobber);
  llvm::LoadInst *rdxAfter = loadRegister(builder, rdx, "RDX", "rdx.after");
  llvm::Value *combined = builder.CreateOr(
      rdxAfter, llvm::ConstantInt::get(rdx->getValueType(), 0x100),
      "rdx.combined");
  storeRegister(builder, rdx, combined, "RDX");
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, unknown);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("unknown_external_binary_clobber_arity");
  bool hasInferredArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterSummarySSAWarning &warning :
       summary.Warnings) {
    hasInferredArityWarning |=
        warning.CalleeName == "unknown_external_binary_clobber_arity" &&
        warning.Reason == "inferred_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external binary clobber arity callee missing") &&
         expect(rewritten->arg_size() == 3,
                "unknown external arity missed explicit binary argument") &&
         expect(hasInferredArityWarning,
                "unknown external binary clobber arity warning missing") &&
         verifyOk(module,
                  "module failed verifier after binary clobber arity test");
}

bool testRecordedCallArgValueSurvivesDeadStoreCleanup() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-arg-value-survives-cleanup", context);
  attachTestAbiWithInputs(module, {"RDI"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "unlink", module);
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "call_arg_cleanup", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *path =
      builder.CreateAdd(function->getArg(0),
                        llvm::ConstantInt::get(rdi->getValueType(), 7), "path");
  llvm::Value *deadValue = builder.CreateXor(
      path, llvm::ConstantInt::get(rdi->getValueType(), 1), "dead.value");
  storeRegister(builder, rbx, deadValue, "RBX");
  storeRegister(builder, rdi, path, "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "unlink") {
        call = candidate;
      }
    }
  }

  bool argIsLocalInstruction = false;
  if (call != nullptr && call->arg_size() == 1) {
    auto *instruction =
        llvm::dyn_cast<llvm::Instruction>(call->getArgOperand(0));
    argIsLocalInstruction =
        instruction != nullptr && instruction->getFunction() == function;
  }

  return expect(call != nullptr, "known one-arg external call missing") &&
         expect(call->arg_size() == 1,
                "known one-arg external used wrong arity") &&
         expect(argIsLocalInstruction,
                "recorded call arg instruction was deleted before rewrite") &&
         expect(summary.DeadStoresRemoved >= 1,
                "dead ABI store before one-arg external was not removed") &&
         verifyOk(module,
                  "module failed verifier after call arg cleanup rewrite");
}

bool testInternalSignatureRewriteUsesArgsAndReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-signature", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *input = loadRegister(builder, rdi, "RDI", "input");
  storeRegister(builder, rax, input, "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 7),
                "RDI");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *result = loadRegister(builder, rax, "RAX", "result");
  (void)result;
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_callee");
  llvm::Function *rewrittenCaller = module.getFunction("notdec_native_caller");
  bool callRewritten = false;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten &&
          call->arg_size() == 1 && call->getType()->isIntegerTy(64)) {
        callRewritten = true;
      }
    }
  }

  return expect(rewritten != nullptr, "rewritten callee missing") &&
         expect(rewrittenCaller != nullptr, "rewritten caller missing") &&
         expect(rewritten->arg_size() == 1,
                "internal callee argument was not rewritten") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "internal callee return was not rewritten") &&
         expect(callRewritten, "internal callsite was not rewritten") &&
         expect(summary.FunctionsRewritten >= 1,
                "internal function rewrite was not counted") &&
         expect(summary.CallsRewritten >= 1,
                "internal call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after internal signature rewrite");
}

bool testInternalSignatureRewriteUsesNonAbiReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-non-abi-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_non_abi_return", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  storeRegister(builder, rbx, llvm::ConstantInt::get(rbx->getValueType(), 9),
                "RBX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_non_abi_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *result = loadRegister(builder, rbx, "RBX", "result");
  (void)result;
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_non_abi_return");
  bool helperLeft = false;
  bool callReturnsRBX = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_return")) {
      helperLeft = !function.use_empty();
    }
  }
  if (llvm::Function *rewrittenCaller =
          module.getFunction("notdec_native_non_abi_caller")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten &&
          call->getType() == rbx->getValueType()) {
        callReturnsRBX = true;
      }
    }
  }

  return expect(rewritten != nullptr, "non-ABI return callee missing") &&
         expect(rewritten->getReturnType() == rbx->getValueType(),
                "internal non-ABI return was not added to signature") &&
         expect(callReturnsRBX,
                "internal non-ABI return callsite was not rewritten") &&
         expect(!helperLeft, "non-ABI return helper was left in IR") &&
         expect(summary.CallsRewritten >= 1,
                "non-ABI return call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after non-ABI return rewrite");
}

bool testInternalSignatureRewriteUsesReadEntryReturnRegisterArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-entry-return-register-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_read_entry_rax", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *sink = builder.CreateAlloca(rax->getValueType());
  llvm::LoadInst *input = loadRegister(builder, rax, "RAX", "input");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_read_entry_rax");
  unsigned raxLoads = 0;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load != nullptr &&
          load->getPointerOperand()->stripPointerCasts() == rax) {
        ++raxLoads;
      }
    }
  }

  return expect(rewritten != nullptr, "RAX entry callee missing") &&
         expect(rewritten->arg_size() == 1,
                "read-entry return register was not added as argument") &&
         expect(rewritten->getArg(0)->getType() == rax->getValueType(),
                "RAX entry argument had wrong type") &&
         expect(raxLoads == 0, "RAX entry load remained") &&
         verifyOk(module,
                  "module failed verifier after RAX entry argument test");
}

bool testInternalSignatureRewriteUsesNarrowEntryRangeArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-narrow-entry-range-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_read_entry_rdi32", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "input32");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_read_entry_rdi32");

  return expect(rewritten != nullptr, "narrow RDI entry callee missing") &&
         expect(rewritten->arg_size() == 1,
                "narrow RDI entry was not added as argument") &&
         expect(rewritten->getArg(0)->getType()->isIntegerTy(32),
                "narrow RDI entry argument was not i32") &&
         expect(!hasRegisterLoad(*rewritten, "RDI"),
                "narrow RDI entry left raw RDI load") &&
         expect(!hasPartialReadCall(*rewritten),
                "narrow RDI entry left partial read helper") &&
         verifyOk(
             module,
             "module failed verifier after narrow RDI entry argument test");
}

bool testInternalSignatureShapeParamKeepsOverwrittenEntryRange() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-overwritten-entry-range-param", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_overwrites_rdi32", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "input32");
  builder.CreateStore(input, sink);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 0),
                "RDI");
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_overwrites_rdi32");
  bool hasUnknownValue = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasUnknownValue |=
          call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName().starts_with("notdec.unknown.");
    }
  }

  return expect(rewritten != nullptr, "overwritten RDI entry callee missing") &&
         expect(rewritten->arg_size() == 1,
                "overwritten RDI entry range was not added as argument") &&
         expect(rewritten->getArg(0)->getType()->isIntegerTy(32),
                "overwritten RDI entry argument was not i32") &&
         expect(!hasPartialReadCall(*rewritten),
                "overwritten RDI entry left partial read helper") &&
         expect(!hasUnknownValue,
                "overwritten RDI entry was materialized as unknown") &&
         verifyOk(module,
                  "module failed verifier after overwritten entry range test");
}

bool testNarrowEntryRangeDoesNotCreateWholeEntryLoad() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-narrow-entry-range-no-full-load", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_entry_rdi32_no_full_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "input32");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  return expect(summary.RangeEntryInputs == 1,
                "narrow entry range did not create one segment entry input") &&
         expect(!hasRegisterLoad(*function, "RDI"),
                "narrow entry range created whole RDI entry load") &&
         expect(!hasPartialReadCall(*function),
                "narrow entry range left partial read helper") &&
         verifyOk(module,
                  "module failed verifier after narrow entry range input test");
}

bool testPostSignatureCleanupRewritesInternalEntryRawLoad() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-signature-entry-load", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_entry_raw_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *input = loadRegister(builder, rdi, "RDI", "input");
  llvm::Value *loadedPtr = builder.CreateAdd(
      input, llvm::ConstantInt::get(rdi->getValueType(), 16), "loaded_ptr");
  llvm::Value *loadedPtrAsPointer = builder.CreateIntToPtr(
      loadedPtr, llvm::PointerType::get(context, 0), "loaded_ptr_as_pointer");
  llvm::LoadInst *loaded =
      builder.CreateLoad(rdi->getValueType(), loadedPtrAsPointer, "loaded");
  llvm::Value *same = builder.CreateICmpEQ(
      loaded, llvm::ConstantInt::get(rdi->getValueType(), 0));
  builder.CreateCondBr(same, left, right);

  builder.SetInsertPoint(left);
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *phi = builder.CreatePHI(rdi->getValueType(), 2, "merged");
  phi->addIncoming(input, left);
  phi->addIncoming(loaded, right);
  llvm::AllocaInst *sink = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(phi, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_entry_raw_load");
  unsigned rdiLoads = 0;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load != nullptr &&
          load->getPointerOperand()->stripPointerCasts() == rdi) {
        ++rdiLoads;
      }
    }
  }

  return expect(rewritten != nullptr, "post-signature callee missing") &&
         expect(rewritten->arg_size() == 1,
                "post-signature callee did not get RDI arg") &&
         expect(rdiLoads == 0, "post-signature cleanup left raw RDI load") &&
         verifyOk(
             module,
             "module failed verifier after post-signature entry load test");
}

bool testInternalSignatureRewriteUsesZmmArgAndReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-zmm-signature", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *input = loadRegister(builder, zmm0, "ZMM0", "input");
  llvm::Value *result =
      builder.CreateXor(input, llvm::ConstantInt::get(zmmType, 1), "result");
  storeRegister(builder, zmm0, result, "ZMM0");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 7), "ZMM0");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, zmm0, "ZMM0", "loaded");
  (void)loaded;
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_zmm_callee");
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_zmm_caller");
  bool callRewritten = false;
  unsigned zmmStores = 0;
  unsigned zmmLoads = 0;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() == rewritten && call->arg_size() == 1 &&
            call->getArgOperand(0)->getType() == zmmType &&
            call->getType() == zmmType) {
          callRewritten = true;
        }
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (store->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmStores;
        }
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmLoads;
        }
      }
    }
  }

  return expect(rewritten != nullptr, "ZMM internal callee missing") &&
         expect(rewrittenCaller != nullptr, "ZMM internal caller missing") &&
         expect(rewritten->arg_size() == 1,
                "ZMM internal argument was not rewritten") &&
         expect(rewritten->getArg(0)->getType() == zmmType,
                "ZMM internal argument was not i512") &&
         expect(rewritten->getReturnType() == zmmType,
                "ZMM internal return was not i512") &&
         expect(callRewritten, "ZMM internal callsite was not rewritten") &&
         expect(zmmStores == 0, "ZMM caller argument store remained") &&
         expect(zmmLoads == 0, "ZMM caller result load remained") &&
         expect(summary.CallsRewritten >= 1,
                "ZMM internal call rewrite was not counted") &&
         verifyOk(module, "module failed verifier after ZMM signature rewrite");
}

bool testPreservedZmmEntryIsPassedAsInternalArgument() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-preserved-zmm-entry-arg", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *leaf =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_leaf", module);
  llvm::BasicBlock *leafEntry =
      llvm::BasicBlock::Create(context, "entry", leaf);
  llvm::IRBuilder<> builder(leafEntry);
  llvm::AllocaInst *sink = builder.CreateAlloca(zmmType);
  llvm::LoadInst *input = loadRegister(builder, zmm0, "ZMM0", "input");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  llvm::Function *passthrough =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_passthrough", module);
  llvm::BasicBlock *passthroughEntry =
      llvm::BasicBlock::Create(context, "entry", passthrough);
  builder.SetInsertPoint(passthroughEntry);
  builder.CreateCall(voidType, leaf);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenLeaf = module.getFunction("notdec_native_zmm_leaf");
  llvm::Function *rewrittenPassthrough =
      module.getFunction("notdec_native_zmm_passthrough");
  bool callUsesPassthroughArg = false;
  unsigned zmmLoads = 0;
  if (rewrittenPassthrough != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenPassthrough)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        callUsesPassthroughArg =
            call->getCalledFunction() == rewrittenLeaf &&
            call->arg_size() == 1 && rewrittenPassthrough->arg_size() == 1 &&
            call->getArgOperand(0) == rewrittenPassthrough->getArg(0);
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmLoads;
        }
      }
    }
  }

  return expect(rewrittenLeaf != nullptr, "ZMM leaf missing") &&
         expect(rewrittenPassthrough != nullptr, "ZMM passthrough missing") &&
         expect(rewrittenLeaf->arg_size() == 1,
                "ZMM leaf argument was not rewritten") &&
         expect(rewrittenPassthrough->arg_size() == 1,
                "ZMM passthrough did not get preserved entry argument") &&
         expect(rewrittenPassthrough->getArg(0)->getType() == zmmType,
                "ZMM passthrough argument was not i512") &&
         expect(callUsesPassthroughArg,
                "ZMM passthrough call did not use its argument") &&
         expect(zmmLoads == 0, "ZMM passthrough entry load remained") &&
         verifyOk(
             module,
             "module failed verifier after preserved ZMM entry argument test");
}

bool testInternalSignatureRewriteUsesZmmLowLaneReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-zmm-low-lane-return", context);
  attachTestFloatAbi(module, 0);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_low_return", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, zmm0->getType(), llvm::Type::getInt64Ty(context), 512, 64);
  builder.CreateCall(
      partialWrite,
      {zmm0,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                              0x3ff0000000000000ULL),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_low_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, zmm0->getType(), llvm::Type::getInt64Ty(context), 512, 64);
  llvm::Value *loaded = builder.CreateCall(
      partialRead,
      {zmm0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "loaded");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "zmm_low_sink");
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_zmm_low_return");
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_zmm_low_caller");
  bool callReturnsDouble = false;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten &&
          call->arg_empty() && call->getType()->isDoubleTy()) {
        callReturnsDouble = true;
      }
    }
  }

  return expect(rewritten != nullptr, "ZMM low-lane callee missing") &&
         expect(rewrittenCaller != nullptr, "ZMM low-lane caller missing") &&
         expect(rewritten->arg_empty(),
                "ZMM low-lane return added unexpected argument") &&
         expect(rewritten->getReturnType()->isDoubleTy(),
                "ZMM low-lane return was not rewritten to double") &&
         expect(callReturnsDouble,
                "ZMM low-lane callsite was not rewritten to double") &&
         expect(!hasPartialWriteCall(*rewritten),
                "ZMM low-lane partial write helper remained") &&
         expect(summary.CallsRewritten >= 1,
                "ZMM low-lane return call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after ZMM low-lane return rewrite");
}

bool testForeignArgumentInMovedBodyIsReplaced() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-foreign-argument", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_foreign_arg", module);
  callee->getArg(0)->setName("R8.arg");
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, callee->getArg(0), "RAX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_foreign_arg");
  bool hasForeignArgumentOperand = false;
  bool hasForeignInstructionOperand = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      for (llvm::Use &operand : inst.operands()) {
        auto *argument = llvm::dyn_cast<llvm::Argument>(operand.get());
        if (argument != nullptr && argument->getParent() != rewritten) {
          hasForeignArgumentOperand = true;
        }
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(operand.get());
        if (instruction != nullptr && instruction->getFunction() != rewritten) {
          hasForeignInstructionOperand = true;
        }
      }
    }
  }

  return expect(rewritten != nullptr, "foreign-argument callee missing") &&
         expect(!hasForeignArgumentOperand,
                "moved body still referenced foreign argument") &&
         expect(!hasForeignInstructionOperand,
                "moved body still referenced foreign instruction") &&
         expect(summary.FunctionsRewritten >= 1,
                "foreign-argument function was not rewritten") &&
         verifyOk(module,
                  "module failed verifier after foreign argument rewrite");
}

bool testForeignMappedCallArgumentIsLocalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-foreign-call-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_foreign_call_arg_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *foreignInput =
      loadRegister(builder, rdi, "RDI", "foreign.input");
  storeRegister(builder, rax, foreignInput, "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_foreign_call_arg_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  // This matches real bad IR seen after summary rewrite: the recorded ABI input
  // may chase through ValueMap into the rewritten callee's argument.
  storeRegister(builder, rdi, foreignInput, "RDI");
  builder.CreateCall(voidType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_foreign_call_arg_caller");
  bool hasForeignCallOperand = false;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call == nullptr) {
        continue;
      }
      for (llvm::Value *arg : call->args()) {
        auto *argument = llvm::dyn_cast<llvm::Argument>(arg);
        if (argument != nullptr && argument->getParent() != rewrittenCaller) {
          hasForeignCallOperand = true;
        }
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(arg);
        if (instruction != nullptr &&
            instruction->getFunction() != rewrittenCaller) {
          hasForeignCallOperand = true;
        }
      }
    }
  }

  return expect(rewrittenCaller != nullptr,
                "foreign-call-arg caller missing") &&
         expect(!hasForeignCallOperand,
                "rewritten call still referenced a foreign value") &&
         expect(summary.CallsRewritten >= 1,
                "foreign-call-arg call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after foreign call arg rewrite");
}

bool testInternalCallArgBindingsKeepLaterArgsAfterEntryInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-entry-input-call-arg", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_child", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *childRdi = loadRegister(calleeBuilder, rdi, "RDI", "rdi.in");
  llvm::LoadInst *childRsi = loadRegister(calleeBuilder, rsi, "RSI", "rsi.in");
  llvm::Value *childSum = calleeBuilder.CreateAdd(childRdi, childRsi);
  llvm::AllocaInst *childSink = calleeBuilder.CreateAlloca(rdi->getValueType());
  calleeBuilder.CreateStore(childSum, childSink);
  calleeBuilder.CreateRetVoid();

  auto *parentType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *parent =
      llvm::Function::Create(parentType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_parent", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", parent);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rdiEntry = loadRegister(builder, rdi, "RDI", "rdi.entry");
  llvm::Value *rsiValue = builder.CreateAdd(
      rdiEntry, llvm::ConstantInt::get(rsi->getValueType(), 7), "rsi.value");
  storeRegister(builder, rsi, rsiValue, "RSI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_child");
  llvm::CallInst *call = nullptr;
  if (llvm::Function *rewrittenParent =
          module.getFunction("notdec_native_parent")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenParent)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
        call = candidate;
      }
    }
  }

  return expect(rewritten != nullptr, "internal child callee missing") &&
         expect(call != nullptr, "internal child call missing") &&
         expect(
             call->arg_size() == 2,
             "internal child call did not keep later arg after entry input") &&
         expect(call->getArgOperand(1) != nullptr,
                "internal child later arg was dropped") &&
         verifyOk(
             module,
             "module failed verifier after internal entry-input call arg test");
}

bool testInternalSignatureParamsUseAbiOrder() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-param-abi-order", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  llvm::GlobalVariable *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_param_order", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::Value *sum = loadRegister(builder, rcx, "RCX", "rcx.in");
  sum = builder.CreateAdd(sum, loadRegister(builder, rdx, "RDX", "rdx.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, rsi, "RSI", "rsi.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, rdi, "RDI", "rdi.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, r8, "R8", "r8.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, r9, "R9", "r9.in"));
  builder.CreateStore(sum, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_param_order");
  std::vector<llvm::StringRef> expected = {"RDI.arg", "RSI.arg", "RDX.arg",
                                           "RCX.arg", "R8.arg",  "R9.arg"};
  bool arityOk = rewritten != nullptr && rewritten->arg_size() == 6;
  bool orderOk =
      rewritten != nullptr && rewritten->arg_size() == expected.size();
  if (orderOk) {
    for (unsigned index = 0; index < expected.size(); ++index) {
      orderOk &= rewritten->getArg(index)->getName() == expected[index];
    }
  }

  return expect(rewritten != nullptr, "internal ABI order function missing") &&
         expect(arityOk, "internal ABI order function used wrong arity") &&
         expect(orderOk, "internal params were not ordered by ABI") &&
         expect(summary.FunctionsRewritten >= 1,
                "internal ABI order function was not rewritten") &&
         verifyOk(module,
                  "module failed verifier after internal ABI order param test");
}

bool testStaticRspStackRewriteKeepsSavedRegisterEvidence() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-saved-register", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "stack_save_rbx", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::LoadInst *rbxEntry = loadRegister(builder, rbx, "RBX", "rbx.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(rbxEntry, slot);
  storeRegister(builder, rbx, llvm::ConstantInt::get(rbx->getValueType(), 42),
                "RBX");
  builder.CreateRetVoid();

  auto stackSummary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.IgnoredRegisters = stackSummary.IgnoredRegisters;
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *fn = functionSummary(summary, "stack_save_rbx");
  const auto *rbxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RBX");

  return expect(stackSummary.AccessesRewritten >= 1,
                "RSP stack save was not localized") &&
         expect(stackSummary.IgnoredRegisters.count("RSP") != 0,
                "RSP was not marked ignored after stack rewrite") &&
         expect(hasNamedAlloca(*function, "notdec_stack.native"),
                "native stack alloca was not created") &&
         expect(rbxSummary != nullptr,
                "missing RBX summary after stack rewrite") &&
         expect(rbxSummary->MayEntry && !rbxSummary->MayNonEntry,
                "saved RBX was not preserved through native stack alloca") &&
         verifyOk(module, "module failed verifier after stack rewrite test");
}

bool testCallerSavedEntryStackSpillCountsAsReadEntry() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-caller-saved-stack-spill", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "caller_saved_stack_spill", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 16);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *slot = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0));
  llvm::LoadInst *entryValue = loadRegister(builder, rdi, "RDI", "rdi.entry");
  builder.CreateStore(entryValue, slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "caller_saved_stack_spill");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");

  return expect(rdiSummary != nullptr,
                "missing RDI summary for caller-saved stack spill") &&
         expect(rdiSummary->ReadEntry,
                "caller-saved stack spill did not count as ReadEntry") &&
         verifyOk(module,
                  "module failed verifier after caller-saved spill test");
}

bool testFramePointerLoadFeedsStackRewriteWithoutGlobalIgnore() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-frame-pointer-stack", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "rbp_frame_stack", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -32, true));
  storeRegister(builder, rbp, frameBase, "RBP");
  llvm::LoadInst *rbpLoad = loadRegister(builder, rbp, "RBP", "rbp.frame");
  llvm::Value *slotAddress = builder.CreateAdd(
      rbpLoad, llvm::ConstantInt::get(rbp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(rbp->getValueType(), 7), slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  return expect(summary.FramePointerLoadsReplaced == 1,
                "RBP frame load was not replaced") &&
         expect(summary.AccessesRewritten >= 1,
                "RBP-derived stack access was not localized") &&
         expect(
             summary.IgnoredRegisters.count("RBP") == 0,
             "local RBP frame-base match incorrectly ignored RBP globally") &&
         expect(hasNamedAlloca(*function, "notdec_stack.native"),
                "frame-pointer stack alloca was not created") &&
         verifyOk(module,
                  "module failed verifier after RBP stack rewrite test");
}

bool testFramePointerRewriteDoesNotHideRbpRegisterFlow() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-local-frame-pointer-ignore", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *frameFunction = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "local_rbp_frame", module);
  llvm::BasicBlock *frameEntry =
      llvm::BasicBlock::Create(context, "entry", frameFunction);
  llvm::IRBuilder<> frameBuilder(frameEntry);
  llvm::LoadInst *rspEntry =
      loadRegister(frameBuilder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = frameBuilder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -32, true));
  storeRegister(frameBuilder, rbp, frameBase, "RBP");
  llvm::LoadInst *rbpLoad = loadRegister(frameBuilder, rbp, "RBP", "rbp.frame");
  llvm::Value *slotAddress = frameBuilder.CreateAdd(
      rbpLoad, llvm::ConstantInt::get(rbp->getValueType(), -8, true));
  llvm::Value *slot = frameBuilder.CreateIntToPtr(
      slotAddress, llvm::PointerType::get(context, 0));
  frameBuilder.CreateStore(llvm::ConstantInt::get(rbp->getValueType(), 7),
                           slot);
  frameBuilder.CreateRetVoid();

  llvm::Function *registerFunction = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "rbp_register_flow", module);
  llvm::BasicBlock *registerEntry =
      llvm::BasicBlock::Create(context, "entry", registerFunction);
  llvm::IRBuilder<> registerBuilder(registerEntry);
  llvm::LoadInst *rdiEntry =
      loadRegister(registerBuilder, rdi, "RDI", "rdi.entry");
  storeRegister(registerBuilder, rbp, rdiEntry, "RBP");
  registerBuilder.CreateRetVoid();

  auto stackSummary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.IgnoredRegisters = stackSummary.IgnoredRegisters;
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *functionSummary = ::functionSummary(summary, "rbp_register_flow");
  const auto *rdiSummary = functionSummary == nullptr
                               ? nullptr
                               : registerSummary(*functionSummary, "RDI");

  return expect(stackSummary.FramePointerLoadsReplaced == 1,
                "local RBP frame load was not replaced") &&
         expect(stackSummary.IgnoredRegisters.count("RBP") == 0,
                "local RBP frame use leaked into module ignored registers") &&
         expect(rdiSummary != nullptr,
                "missing RDI summary after local RBP frame rewrite") &&
         expect(rdiSummary->ReadEntry,
                "RDI entry flow through ordinary RBP store was hidden") &&
         verifyOk(module, "module failed verifier after local RBP ignore test");
}

bool testSummarySSARemovesDeadStackFrameStore() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-stack-frame-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "dead_stack_frame_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(rsp->getValueType(), 11), slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  (void)summary;
  return expect(!hasStoreInstruction(*function),
                "dead stack-frame store remained in final IR") &&
         verifyOk(module,
                  "module failed verifier after stack-frame cleanup test");
}

bool testStackFrameAddressPassedToCallIsLocalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-frame-call-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "takes_stack_pointer", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "stack_pointer_call_arg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -64, true));
  llvm::Value *buffer = builder.CreateAdd(
      frameBase, llvm::ConstantInt::get(rsp->getValueType(), 16, true));
  builder.CreateCall(callee, {buffer});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      call = candidate;
    }
  }

  return expect(summary.AccessesRewritten >= 2,
                "stack-derived call argument was not localized") &&
         expect(call != nullptr, "missing call after stack rewrite") &&
         expect(
             llvm::isa<llvm::PtrToIntInst>(call->getArgOperand(0)),
             "stack call argument was not rewritten through native alloca") &&
         verifyOk(module,
                  "module failed verifier after stack call arg rewrite");
}

bool testPostSignatureCleanupDropsAbiStoreBeforeUnrewrittenCall() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-cleanup-abi-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unrewritten_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "abi_store_before_unrewritten_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *arg = llvm::ConstantInt::get(rdi->getValueType(), 123);
  storeRegister(builder, rdi, arg, "RDI");
  builder.CreateBr(callBlock);
  builder.SetInsertPoint(callBlock);
  builder.CreateCall(callee, {arg});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned rdiStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == rdi) {
        ++rdiStores;
      }
    }
  }

  return expect(rdiStores == 0,
                "post-signature cleanup kept a dead ABI argument store") &&
         expect(summary.DeadStoresRemoved >= 1,
                "dead ABI argument store was not counted") &&
         verifyOk(module,
                  "module failed verifier after post-signature cleanup test");
}

bool testNoReturnExternalDoesNotCreateSummaryReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-noreturn-external", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *noreturnType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *stackChkFail =
      llvm::Function::Create(noreturnType, llvm::GlobalValue::ExternalLinkage,
                             "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "calls_noreturn", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(stackChkFail->getFunctionType(), stackChkFail, {});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.CallReturnValues == 0,
                "noreturn external path created a summary return helper") &&
         verifyOk(module, "module failed verifier after noreturn cleanup test");
}

bool testExternalPrototypeJsonOverlaysDefaultNoReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-json-overlay-noreturn", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *exitType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *exit = llvm::Function::Create(
      exitType, llvm::GlobalValue::ExternalLinkage, "exit", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "json_overlay_keeps_exit", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(
      exitType, exit,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1)});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-prototype-overlay", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create external prototype JSON: " << error.message()
              << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_known_external":{"return":"void","fixed_args":0}})";
  }

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.ExternalPrototypeJsonPath = jsonPath.str().str();
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);
  (void)llvm::sys::fs::remove(jsonPath);

  return expect(summary.CallReturnValues == 0,
                "custom prototype JSON replaced default exit noreturn") &&
         expect(blockEndsWithUnreachable(*function, "entry"),
                "default exit noreturn was not preserved under JSON overlay") &&
         verifyOk(module, "module failed verifier after JSON overlay noreturn");
}

bool testInternalNoReturnCallFallthroughIsTruncated() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-noreturn-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *exitType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *exit = llvm::Function::Create(
      exitType, llvm::GlobalValue::ExternalLinkage, "exit", module);
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});

  llvm::Function *helper = llvm::Function::Create(
      voidType, llvm::GlobalValue::InternalLinkage, "local_fail", module);
  llvm::BasicBlock *helperEntry =
      llvm::BasicBlock::Create(context, "entry", helper);
  llvm::IRBuilder<> builder(helperEntry);
  builder.CreateCall(
      exitType, exit,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1)});
  builder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "calls_local_fail", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, helper, {});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.CallReturnValues == 0,
                "internal noreturn call created a summary return helper") &&
         expect(helper->hasFnAttribute(llvm::Attribute::NoReturn),
                "internal helper was not marked noreturn") &&
         expect(blockEndsWithUnreachable(*caller, "entry"),
                "internal noreturn call kept fallthrough terminator") &&
         verifyOk(module,
                  "module failed verifier after internal noreturn test");
}

bool testStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "stack canary check was not counted as removed") &&
         expect(summary.StackCanaryFailBlocksRemoved == 1,
                "stack canary fail block was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "stack canary FS_OFFSET load was kept") &&
         verifyOk(module, "module failed verifier after stack canary removal");
}

bool testI386GsStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-gs-stack-canary-remove", context);
  module.setDataLayout("e-p:32:32");
  attachI386TestAbi(module);
  llvm::Function *function = createStackCanaryCheckFunction(
      module, 20, false, false, false, false, "GS_OFFSET", 4);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "i386 GS stack canary check was not counted as removed") &&
         expect(summary.StackCanaryFailBlocksRemoved == 1,
                "i386 GS stack canary fail block was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "i386 GS stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "GS_OFFSET"),
                "i386 GS stack canary GS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after i386 GS canary removal");
}

bool testDeadStackCanaryFailThunkIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-fail-thunk-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false);
  llvm::Function *directFail = module.getFunction("__stack_chk_fail");
  if (directFail == nullptr) {
    return expect(false, "direct stack check fail declaration missing");
  }

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failThunk =
      llvm::Function::Create(failType, llvm::GlobalValue::InternalLinkage,
                             "notdec_native_4430", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", failThunk);
  llvm::IRBuilder<> thunkBuilder(entry);
  thunkBuilder.CreateCall(directFail->getFunctionType(), directFail, {});
  thunkBuilder.CreateUnreachable();

  bool rewired = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() == directFail) {
      call->setCalledFunction(failThunk->getFunctionType(), failThunk);
      rewired = true;
    }
  }
  if (!rewired) {
    return expect(false, "stack canary fail call was not rewired to thunk");
  }

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "thunk stack canary check was not removed") &&
         expect(summary.StackCanaryFailFunctionsRemoved == 1,
                "dead stack canary fail thunk was not counted as removed") &&
         expect(!moduleHasFunctionNamed(module, "notdec_native_4430"),
                "dead stack canary fail thunk was kept") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "stack canary fail thunk call was kept") &&
         verifyOk(module,
                  "module failed verifier after stack canary fail thunk test");
}

bool testStackCanaryPrologueSaveIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-prologue-save-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false, false, true);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "stack canary check with prologue save was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "stack canary fail call with prologue save was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "stack canary prologue FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after canary prologue save removal");
}

bool testStackCanaryZextConditionIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-zext-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, true, false);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "zext stack canary check was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "zext stack canary fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after zext stack canary removal");
}

bool testStackCanaryMaskedSavedLoadIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-masked-saved-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false, true);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "masked saved stack canary check was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "masked saved stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "masked saved stack canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after masked stack canary removal");
}

bool testStackCanaryFailSideEffectIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-side-effect", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, true);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 0,
                "side-effecting fail block was incorrectly removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "side-effecting stack canary fail call was removed") &&
         verifyOk(module,
                  "module failed verifier after side-effecting canary test");
}

bool testStackCanaryWrongFsOffsetIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-wrong-offset", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 32, false, false);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 0,
                "non-FS:0x28 canary-like check was incorrectly removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "non-FS:0x28 stack check fail call was removed") &&
         verifyOk(module, "module failed verifier after wrong-offset test");
}

bool testRawRspStackCanaryCheckIsRemovedAfterStackCleanup() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-raw-rsp", context);
  attachTestAbi(module);
  llvm::Function *function = createRawRspStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "raw RSP stack canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "raw RSP stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "raw RSP stack canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after raw RSP stack canary test");
}

bool testRawRspZfStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-raw-rsp-zf", context);
  attachTestAbi(module);
  llvm::Function *function = createRawRspZfStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "raw RSP ZF stack canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "raw RSP ZF stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "raw RSP ZF stack canary FS_OFFSET load was kept") &&
         expect(!hasRegisterLoad(*function, "ZF"),
                "raw RSP ZF stack canary flag load was kept") &&
         verifyOk(module,
                  "module failed verifier after raw RSP ZF stack canary test");
}

bool testPhiFsBaseStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-phi-fs", context);
  attachTestAbi(module);
  llvm::Function *function = createPhiFsBaseStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "PHI FS stack canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "PHI FS stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "PHI FS stack canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after PHI FS stack canary test");
}

bool testZeroBaseStackCanaryCheckIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-zero-base", context);
  attachTestAbi(module);
  llvm::Function *function = createZeroBaseStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 0,
                "zero-base stack canary check was incorrectly removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "zero-base stack canary fail call was incorrectly removed") &&
         verifyOk(module,
                  "module failed verifier after zero-base stack canary test");
}

bool testSharedFailStackCanaryChecksAreRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-shared-fail", context);
  attachTestAbi(module);
  llvm::Function *function = createSharedFailStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 2,
                "shared-fail stack canary checks were not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "shared-fail stack canary fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after shared-fail canary test");
}

bool testMixedFailPredecessorKeepsNonCanaryFailPath() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-mixed-fail", context);
  attachTestAbi(module);
  llvm::Function *function =
      createMixedFailPredecessorStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "mixed-fail canary predecessor was not removed") &&
         expect(summary.StackCanaryFailBlocksRemoved == 0,
                "mixed-fail block with non-canary predecessor was removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "mixed-fail non-canary fail call was removed") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "mixed-fail canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after mixed-fail canary test");
}

bool testPhiFsCanaryAddressStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-phi-address", context);
  attachTestAbi(module);
  llvm::Function *function =
      createPhiFsCanaryAddressStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "PHI FS canary address check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "PHI FS canary address fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after PHI canary address test");
}

bool testSelfPhiFsCanaryAddressStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-self-phi-address", context);
  attachTestAbi(module);
  llvm::Function *function =
      createSelfPhiFsCanaryAddressStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "self-PHI FS canary address check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "self-PHI FS canary address fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after self-PHI canary address test");
}

bool testSelfPhiFsBaseStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-self-phi-base", context);
  attachTestAbi(module);
  llvm::Function *function =
      createSelfPhiFsBaseStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "self-PHI FS base canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "self-PHI FS base canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "self-PHI FS base canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after self-PHI canary base test");
}

bool testXmmAbiEffectUsesZmmBackingWithoutSignatureReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-zmm-abi-effect", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_zmm_test";
  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MetaType = "float";
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "XMM0_Qa";
  abi.Outputs.push_back(output);
  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "XMM0";
  abi.Effects.push_back(killed);
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_float_return", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "zmm_partial_after_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(external);
  llvm::Value *old = builder.CreateLoad(zmmType, zmm0);
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 64, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  storeRegister(builder, zmm0, builder.CreateOr(keep, low), "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store != nullptr &&
        store->getPointerOperand()->stripPointerCasts() == zmm0) {
      ++zmmStores;
    }
  }

  return expect(function->getReturnType()->isVoidTy(),
                "float ABI output was rewritten as an i512 return") &&
         expect(zmmStores == 0,
                "dead ZMM partial store after external call was not removed") &&
         expect(summary.CallReturnValues == 0,
                "float ABI output created a summary return helper") &&
         verifyOk(module, "module failed verifier after ZMM ABI effect test");
}

bool testUnknownExternalFloatAbiClobberUsesWholeZmm() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-float-clobber", context);
  attachTestFloatAbi(module, 1);

  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_float_clobber", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "reads_zmm_after_unknown_external", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *sink = builder.CreateAlloca(zmmType);
  builder.CreateCall(external);
  llvm::Value *loaded = loadRegister(builder, zmm0, "ZMM0", "after");
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.CallClobberValues >= 1,
                "float ABI clobber was not modeled") &&
         expect(!moduleHasFunctionNamed(module,
                                        "notdec.register.summary_clobber.i512"),
                "whole-ZMM summary clobber declaration remained") &&
         verifyOk(
             module,
             "module failed verifier after float clobber suppression test");
}

bool testUnknownExternalIntegerClobberUsesRangeCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-integer-clobber-range", context);
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_clobber_test";
  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);
  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "R10";
  abi.Effects.push_back(killed);
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *r10 = createRegisterGlobal(module, "R10");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_integer_clobber_range", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "reads_rdx_after_unknown_external", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(calleeType, external);
  llvm::Value *loaded = loadRegister(builder, r10, "R10", "after");
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);
  llvm::Function *rewritten =
      module.getFunction("reads_rdx_after_unknown_external");

  return expect(summary.CallClobberValues >= 1,
                "integer clobber range helper was not created") &&
         expect(rewritten != nullptr,
                "integer clobber test function was not preserved") &&
         expect(!hasLiveReplacedRegisterLoad(*rewritten),
                "integer clobber range left live raw R10 load") &&
         expect(moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_clobber.i1") ||
                    moduleHasUsedFunctionNamed(
                        module, "notdec.register.summary_clobber.i64"),
                "integer clobber range helper was not used") &&
         expect((functionHasUsedSummaryCallValueRange(*function, "clobber",
                                                      "R10", 0, 64) ||
                 (functionHasUsedSummaryCallValueRange(*function, "clobber",
                                                       "R10", 0, 1) &&
                  functionHasUsedSummaryCallValueRange(*function, "clobber",
                                                       "R10", 1, 63))),
                "integer clobber helper did not carry R10 range metadata") &&
         verifyOk(module,
                  "module failed verifier after integer clobber range test");
}

bool testKnownPowUsesFloatAbiSlots() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-pow-float-abi", context);
  attachTestFloatAbi(module, 2);

  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  llvm::GlobalVariable *zmm1 =
      createRegisterGlobal(module, "ZMM1", zmmType, 4672, 64);

  auto *oldPowType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *powFunction = llvm::Function::Create(
      oldPowType, llvm::GlobalValue::ExternalLinkage, "pow", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "pow_float_slots", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 1), "ZMM0");
  storeRegister(builder, zmm1, llvm::ConstantInt::get(zmmType, 2), "ZMM1");
  builder.CreateCall(oldPowType, powFunction);
  llvm::Value *powBits =
      builder.CreateTrunc(loadRegister(builder, zmm0, "ZMM0", "pow.after"),
                          llvm::Type::getInt64Ty(context));
  builder.CreateRet(powBits);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *powCall = nullptr;
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "pow") {
        powCall = call;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == zmm0 ||
          store->getPointerOperand()->stripPointerCasts() == zmm1) {
        ++zmmStores;
      }
    }
  }

  return expect(powCall != nullptr, "pow call missing after rewrite") &&
         expect(powCall->getType()->isDoubleTy(),
                "pow return type was not rewritten to double") &&
         expect(powCall->arg_size() == 2,
                "pow did not receive two float ABI arguments") &&
         expect(powCall->getArgOperand(0)->getType()->isDoubleTy() &&
                    powCall->getArgOperand(1)->getType()->isDoubleTy(),
                "pow arguments were not rewritten to double") &&
         expect(zmmStores == 0, "pow ABI ZMM argument stores remained") &&
         expect(summary.CallsRewritten >= 1,
                "pow call was not counted as rewritten") &&
         verifyOk(module, "module failed verifier after pow ABI rewrite test");
}

bool testKnownUnaryLibmUsesFloatAbiSlots() {
  bool ok = true;
  for (const std::string &calleeName :
       std::vector<std::string>{"sqrt", "sin", "cos", "log", "exp"}) {
    llvm::LLVMContext context;
    llvm::Module module("summary-ssa-libm-float-abi", context);
    attachTestFloatAbi(module, 1);

    llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
    llvm::GlobalVariable *zmm0 =
        createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

    auto *oldCalleeType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function *callee = llvm::Function::Create(
        oldCalleeType, llvm::GlobalValue::ExternalLinkage, calleeName, module);
    auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
    llvm::Function *function =
        llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                               calleeName + "_float_slots", module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 1), "ZMM0");
    builder.CreateCall(oldCalleeType, callee);
    llvm::Value *bits =
        builder.CreateTrunc(loadRegister(builder, zmm0, "ZMM0", "libm.after"),
                            llvm::Type::getInt64Ty(context));
    builder.CreateRet(bits);

    auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
    llvm::CallInst *rewrittenCall = nullptr;
    unsigned zmmStores = 0;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() != nullptr &&
            (call->getCalledFunction()->getName() == calleeName ||
             (calleeName == "log" &&
              call->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::log))) {
          rewrittenCall = call;
        }
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (store->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmStores;
        }
      }
    }

    std::string prefix = calleeName + ": ";
    ok &= expect(rewrittenCall != nullptr,
                 (prefix + "call missing after rewrite").c_str());
    if (rewrittenCall != nullptr) {
      ok &= expect(rewrittenCall->getType()->isDoubleTy(),
                   (prefix + "return type was not double").c_str());
      ok &= expect(rewrittenCall->arg_size() == 1,
                   (prefix + "did not receive one argument").c_str());
      ok &= expect(rewrittenCall->getArgOperand(0)->getType()->isDoubleTy(),
                   (prefix + "argument was not double").c_str());
    }
    ok &= expect(zmmStores == 0,
                 (prefix + "ABI ZMM argument store remained").c_str());
    ok &= expect(
        !moduleHasFunctionNamed(module, "notdec.register.summary_return.i64"),
        (prefix + "range return helper remained").c_str());
    ok &= expect(summary.CallsRewritten >= 1,
                 (prefix + "call was not counted as rewritten").c_str());
    ok &= verifyOk(module, (prefix + "module failed verifier").c_str());
  }
  return ok;
}

bool testPartialKeepHighStoreIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "partial_keep_high", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, rdx, "RDX", "old");
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(rdx->getValueType(),
                                  llvm::APInt::getBitsSet(64, 8, 64)));
  llvm::Value *low = llvm::ConstantInt::get(rdx->getValueType(), 7);
  storeRegister(builder, rdx, builder.CreateOr(keep, low), "RDX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned rdxLoads = 0;
  unsigned rdxStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == rdx) {
        ++rdxLoads;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == rdx) {
        ++rdxStores;
      }
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial keep-high store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial keep-high store was not demand rewritten") &&
         expect(rdxLoads == 0, "partial keep-high old load was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial demand rewrite test");
}

bool testPartialWriteHelperIsConsumedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-write-helper", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(module, rax->getValueType(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_write_readback", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *after = loadRegister(builder, rax, "RAX", "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "partial write readback load was not replaced") &&
         expect(!hasPartialWriteCall(*function),
                "partial write helper call remained after summary SSA") &&
         expect(!hasRegisterLoad(*function, "RAX"),
                "raw RAX load remained after partial write rewrite") &&
         verifyOk(module,
                  "module failed verifier after partial write helper test");
}

bool testPartialReadHelperIsConsumedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-helper", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_write_then_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *after = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "partial read helper was not replaced") &&
         expect(!hasPartialReadCall(*function),
                "partial read helper call remained after summary SSA") &&
         expect(!hasRegisterLoad(*function, "RAX"),
                "raw RAX load remained after partial read rewrite") &&
         verifyOk(module,
                  "module failed verifier after partial read helper test");
}

bool testFullStoreFeedsPartialReadThroughRangeSSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-full-store-partial-read", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "full_store_partial_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  storeRegister(builder, rax,
                llvm::ConstantInt::get(rax->getValueType(), 0xb00000007ULL),
                "RAX");
  llvm::Value *low = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)}, "low");
  builder.CreateStore(low, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced >= 1,
                "full store partial read was not replaced") &&
         expect(!hasPartialReadCall(*function),
                "full store partial read helper call remained") &&
         expect(!hasRegisterLoad(*function, "RAX"),
                "full store partial read left raw RAX load") &&
         verifyOk(module,
                  "module failed verifier after full store partial read test");
}

bool testDeadPartialReadHelperIsRemovedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-partial-read-helper", context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *unknown =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "dead_partial_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  builder.CreateCall(unknown);
  builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.DeadLoadsRemoved >= 1,
                "dead partial read helper was not counted") &&
         expect(!hasPartialReadCall(*function),
                "dead partial read helper remained") &&
         verifyOk(module,
                  "module failed verifier after dead partial read helper test");
}

bool testDeadSummaryCallValueHelperIsRemovedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-summary-call-helper", context);
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *helperType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {}, false);
  llvm::Function *helper =
      llvm::Function::Create(helperType, llvm::GlobalValue::ExternalLinkage,
                             "notdec.register.summary_clobber.i64", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "dead_summary_call_helper", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::CallInst *dead = builder.CreateCall(helperType, helper, {}, "dead");
  llvm::CallInst *live = builder.CreateCall(helperType, helper, {}, "live");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=RDX"),
      llvm::MDString::get(context, "kind=clobber"),
  };
  llvm::MDNode *metadata = llvm::MDNode::get(context, fields);
  dead->setMetadata("notdec.register.summary_ssa.call_value", metadata);
  live->setMetadata("notdec.register.summary_ssa.call_value", metadata);
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);

  unsigned helperCalls = 0;
  bool deadCallRemained = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr || call->getCalledFunction() != helper) {
      continue;
    }
    ++helperCalls;
    deadCallRemained |= call->getName() == "dead";
  }

  return expect(helperCalls == 1,
                "dead summary call helper was not removed precisely") &&
         expect(!deadCallRemained, "dead summary call helper remained") &&
         expect(moduleHasFunctionNamed(module,
                                       "notdec.register.summary_clobber.i64"),
                "live summary helper declaration was removed") &&
         verifyOk(module,
                  "module failed verifier after dead summary helper test");
}

bool testConsecutivePartialReadHelpersAreConsumedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-consecutive-partial-read-helper", context);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "consecutive_partial_reads", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *first = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "first");
  llvm::Value *second = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "second");
  llvm::Value *value = builder.CreateXor(first, second, "same_xor");
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced >= 2,
                "consecutive partial reads were not both replaced") &&
         expect(!hasPartialReadCall(*function),
                "consecutive partial read helper call remained") &&
         verifyOk(module,
                  "module failed verifier after consecutive partial read test");
}

bool testConsecutivePartialReadXorIsZeroedAfterSummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-consecutive-partial-read-xor", context);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "consecutive_partial_read_xor", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *first = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "first");
  llvm::Value *second = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "second");
  llvm::Value *value = builder.CreateXor(first, second, "same_xor");
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hasNonZeroSinkStore = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr || store->getPointerOperand() != sink) {
      continue;
    }
    auto *constant =
        llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    hasNonZeroSinkStore = constant == nullptr || !constant->isZero();
  }

  return expect(summary.LoadsReplaced >= 2,
                "consecutive partial read xor inputs were not replaced") &&
         expect(!hasPartialReadCall(*function),
                "consecutive partial read xor helper call remained") &&
         expect(!hasNonZeroSinkStore,
                "consecutive partial read xor was not folded to zero") &&
         verifyOk(
             module,
             "module failed verifier after consecutive partial read xor test");
}

bool testDuplicatePartialReadXorAfterUnknownCallIsZeroed() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-call-partial-read-xor", context);
  attachTestAbi(module);
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *unknown =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "unknown_call_partial_read_xor", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, r9->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  builder.CreateCall(unknown);
  llvm::Value *first = builder.CreateCall(
      partialRead,
      {r9, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "first");
  llvm::Value *second = builder.CreateCall(
      partialRead,
      {r9, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "second");
  llvm::Value *value = builder.CreateXor(first, second, "same_xor");
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hasNonZeroSinkStore = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr || store->getPointerOperand() != sink) {
      continue;
    }
    auto *constant =
        llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    hasNonZeroSinkStore = constant == nullptr || !constant->isZero();
  }

  return expect(summary.LoadsReplaced >= 1,
                "duplicate partial read xor was not folded") &&
         expect(!hasPartialReadCall(*function),
                "duplicate partial read xor helper calls remained") &&
         expect(!hasNonZeroSinkStore,
                "duplicate partial read xor after unknown call was not zero") &&
         verifyOk(
             module,
             "module failed verifier after unknown call partial read xor test");
}

bool testBranchPartialReadUsesNarrowRangePhi() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-range-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "branch_partial_read_range_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, left, right);

  builder.SetInsertPoint(left);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 11),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::Value *after = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned i64Phis = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    if (phi->getType()->isIntegerTy(64)) {
      ++i64Phis;
    }
  }

  return expect(summary.LoadsReplaced >= 1,
                "branch partial read was not replaced") &&
         expect(!hasPartialReadCall(*function),
                "branch partial read helper call remained") &&
         expect(summary.PhisCreated >= 1,
                "branch partial read did not create a range phi") &&
         expect(i64Phis == 0,
                "branch partial read created a whole-register i64 phi") &&
         verifyOk(module, "module failed verifier after branch range phi test");
}

bool testBranchFullLoadAfterPartialWriteUsesNarrowRangePhi() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-full-load-range-phi", context);
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_no_return_test";
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(module, rax->getValueType(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "branch_full_load_range_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, left, right);

  builder.SetInsertPoint(left);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 11),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::LoadInst *after = loadRegister(builder, rax, "RAX", "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned i64Phis = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    if (phi->getType()->isIntegerTy(64)) {
      ++i64Phis;
    }
  }

  return expect(summary.LoadsReplaced >= 1,
                "branch full load was not replaced") &&
         expect(!hasRegisterLoad(*function, "RAX"),
                "branch full load left raw RAX load") &&
         expect(!hasPartialWriteCall(*function),
                "branch full load left partial write helper") &&
         expect(functionHasInstructionNameContaining(*function, "full_range"),
                "branch full load did not assemble from ranges") &&
         expect(i64Phis == 0,
                "branch full load created a whole-register i64 phi") &&
         verifyOk(
             module,
             "module failed verifier after branch full-load range phi test");
}

bool testLoopPartialWriteUsesNarrowRangePhi() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-loop-partial-write-range-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "loop_partial_write_range_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *header =
      llvm::BasicBlock::Create(context, "header", function);
  llvm::BasicBlock *body = llvm::BasicBlock::Create(context, "body", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(header);

  builder.SetInsertPoint(header);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, body, exit);

  builder.SetInsertPoint(body);
  llvm::Value *current = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "current");
  llvm::Value *next = builder.CreateAdd(
      current, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1),
      "next");
  builder.CreateCall(
      partialWrite,
      {rax, next, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(header);

  builder.SetInsertPoint(exit);
  llvm::Value *after = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "after");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  function = module.getFunction("loop_partial_write_range_phi");
  if (function == nullptr) {
    return expect(false, "loop partial write function missing after rewrite");
  }
  unsigned i32Phis = 0;
  unsigned i64Phis = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    if (phi->getType()->isIntegerTy(32)) {
      ++i32Phis;
    }
    if (phi->getType()->isIntegerTy(64)) {
      ++i64Phis;
    }
  }

  return expect(summary.LoadsReplaced >= 2,
                "loop partial reads were not replaced") &&
         expect(!hasPartialReadCall(*function),
                "loop partial read helper call remained") &&
         expect(!hasPartialWriteCall(*function),
                "loop partial write helper call remained") &&
         expect(i32Phis >= 1, "loop partial write did not create i32 phi") &&
         expect(i64Phis == 0,
                "loop partial write created a whole-register i64 phi") &&
         verifyOk(module, "module failed verifier after loop range phi test");
}

bool testDeadPartialWriteUsesRangeLiveness() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-write-range-liveness", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "dead_partial_write_range_liveness", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *high = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)},
      "high");
  builder.CreateStore(high, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.DeadStoresRemoved >= 1,
                "dead low partial write was not removed") &&
         expect(!hasPartialWriteCall(*function),
                "dead low partial write helper remained") &&
         expect(!hasPartialReadCall(*function),
                "high partial read helper remained") &&
         verifyOk(module,
                  "module failed verifier after partial write liveness test");
}

bool testFloatCallEffectUsesRangeLiveness() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-float-call-effect-range-liveness", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  llvm::Function *log = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getDoubleTy(context),
                              {llvm::Type::getDoubleTy(context)}, false),
      llvm::GlobalValue::ExternalLinkage, "log", module);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "float_call_effect_range_liveness", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 1), "ZMM0");
  builder.CreateCall(
      log, {llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 1.0)});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.DeadStoresRemoved >= 1,
                "dead full ZMM store before float return was not removed") &&
         expect(!hasRegisterStore(*function, "ZMM0"),
                "dead full ZMM store remained before float return") &&
         verifyOk(module,
                  "module failed verifier after float call liveness test");
}

bool testPlannerSplitsZmmForFloatAbiSlot() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-zmm-abi-range-planner", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "planner_zmm_abi_slot", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableRewrite = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);
  return expect(summary.RangeSegmentsPlanned >= 2,
                "float ABI slot did not split backing ZMM range") &&
         expect(summarySsaMetadataUInt(*function, "range_segments_planned") >=
                    2,
                "float ABI slot split was not exposed in metadata") &&
         verifyOk(module, "module failed verifier after zmm planner test");
}

bool testPlannerSplitsRegisterForDemandMask() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-demand-mask-range-planner", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "planner_demand_mask_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "planner_demand_mask_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *after = loadRegister(builder, rax, "RAX", "after");
  llvm::Value *low =
      builder.CreateTrunc(after, llvm::Type::getInt32Ty(context), "low");
  builder.CreateStore(low, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableRewrite = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);
  return expect(summary.RangeSegmentsPlanned >= 2,
                "demand mask did not split RAX range") &&
         expect(summarySsaMetadataUInt(*callee, "range_segments_planned") >= 2,
                "demand mask split was not exposed in callee metadata") &&
         verifyOk(module, "module failed verifier after demand planner test");
}

bool testPartialWriteHelperNameSurvivesSignatureRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-write-name", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt8Ty(context), 64, 8);

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_write_name_survives", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 11),
                "RAX");
  builder.CreateCall(externalType, external);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *keptPartialWrite =
      module.getFunction("notdec.partial_write.i64.i8");

  return expect(keptPartialWrite == partialWrite,
                "partial write helper was renamed during signature rewrite") &&
         expect(module.getFunction("1") == nullptr,
                "numeric partial write helper declaration was created") &&
         verifyOk(module,
                  "module failed verifier after partial write name test");
}

bool testPartialReadHelperNameSurvivesSignatureRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-name", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt8Ty(context), 64, 8);

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_read_name_survives", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 11),
                "RAX");
  builder.CreateCall(externalType, external);
  llvm::Value *value = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *sinkPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096),
      llvm::PointerType::get(context, 0), "sink");
  builder.CreateStore(value, sinkPointer);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *keptPartialRead =
      module.getFunction("notdec.partial_read.i64.i8");

  return expect(keptPartialRead == partialRead,
                "partial read helper was renamed during signature rewrite") &&
         expect(module.getFunction("1") == nullptr,
                "numeric partial read helper declaration was created") &&
         verifyOk(module,
                  "module failed verifier after partial read name test");
}

bool testRecordedReturnValueSurvivesPartialDemandRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-return-partial-demand", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(module, rax->getValueType(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "return_entry_or_partial", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *early = llvm::BasicBlock::Create(context, "early", callee);
  llvm::BasicBlock *write = llvm::BasicBlock::Create(context, "write", callee);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond = builder.CreateLoad(condition->getValueType(), condition);
  builder.CreateCondBr(cond, early, write);

  builder.SetInsertPoint(early);
  builder.CreateRetVoid();

  builder.SetInsertPoint(write);
  llvm::Value *old = loadRegister(builder, rax, "RAX", "old");
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(rax->getValueType(),
                                  llvm::APInt::getBitsSet(64, 8, 64)));
  llvm::Value *low = llvm::ConstantInt::get(rax->getValueType(), 1);
  storeRegister(builder, rax, builder.CreateOr(keep, low), "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "use_partial_return", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(type, callee);
  llvm::Value *returned = loadRegister(builder, rax, "RAX", "returned");
  builder.CreateStore(returned, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.FunctionsRewritten >= 1,
                "callee signature was not rewritten for RAX return") &&
         expect(summary.PartialDemandCandidates >= 1,
                "partial return path was not seen by demand rewrite") &&
         verifyOk(module,
                  "module failed verifier after return partial-demand test");
}

bool testPartialDemandZeroReplacementIsMarked() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand-zero-metadata", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zero_metadata", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, rdx, "RDX", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(rdx->getValueType(),
                             llvm::APInt::getBitsSet(64, 8, 64)),
      "keep_high");
  llvm::Value *low = llvm::ConstantInt::get(rdx->getValueType(), 7);
  storeRegister(builder, rdx, builder.CreateOr(keep, low, "merged"), "RDX");
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnablePostRewriteInstCombine = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  return expect(summary.PartialDemandMatched >= 1,
                "partial zero replacement was not applied") &&
         expect(functionHasZeroDemandOperandMetadata(*function),
                "partial zero replacement was not marked") &&
         verifyOk(module,
                  "module failed verifier after partial zero metadata test");
}

bool testPartialZmmKeepHighStoreIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-zmm-demand", context);
  attachTestAbi(module);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zmm_keep_high", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, zmm0, "ZMM0", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 128, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  storeRegister(builder, zmm0, builder.CreateOr(keep, low), "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned zmmLoads = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmLoads;
      }
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial zmm keep-high store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial zmm keep-high store was not demand rewritten") &&
         expect(zmmLoads == 0,
                "partial zmm keep-high old load was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial zmm demand test");
}

bool testPartialZmmNakedKeepHighStoreIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-zmm-naked-keep-high", context);
  attachTestAbi(module);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zmm_naked_keep_high", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, zmm0, "ZMM0", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 128, 512)));
  storeRegister(builder, zmm0, keep, "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned zmmLoads = 0;
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmLoads;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmStores;
      }
    }
  }

  return expect(
             summary.PartialDemandCandidates >= 1,
             "partial zmm naked keep-high store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial zmm naked keep-high store was not demand rewritten") &&
         expect(zmmLoads == 0,
                "partial zmm naked keep-high old load was not removed") &&
         expect(zmmStores == 0,
                "partial zmm naked keep-high store was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial zmm naked demand test");
}

bool testPartialZmmDisjointLaneChainIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-zmm-lane-chain", context);
  attachTestAbi(module);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zmm_lane_chain", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, zmm0, "ZMM0", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 128, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  llvm::Value *mid = builder.CreateOr(keep, low, "mid", /*IsDisjoint=*/true);
  llvm::Value *lane1 = builder.CreateShl(llvm::ConstantInt::get(zmmType, 11),
                                         llvm::ConstantInt::get(zmmType, 64));
  llvm::Value *combined =
      builder.CreateOr(mid, lane1, "combined", /*IsDisjoint=*/true);
  storeRegister(builder, zmm0, combined, "ZMM0");
  llvm::Value *used = builder.CreateTrunc(
      builder.CreateLShr(combined, llvm::ConstantInt::get(zmmType, 64)),
      llvm::Type::getInt64Ty(context));
  llvm::Value *slot =
      builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "slot");
  builder.CreateStore(used, slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned zmmLoads = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmLoads;
      }
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial zmm lane chain store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial zmm lane chain was not demand rewritten") &&
         expect(zmmLoads == 0,
                "partial zmm lane chain old load was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial zmm lane chain test");
}

bool testPartialDemandKeepsMemoryPointerRegisterAddress() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-memory-pointer-demand", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "ordinary_memory_sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_memory_pointer_demand", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *base = loadRegister(builder, rdi, "RDI", "rdi_base");
  llvm::Value *address = builder.CreateAdd(
      base, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "mem_addr");
  auto *pointerType = llvm::PointerType::get(context, 0);
  llvm::Value *pointer =
      builder.CreateIntToPtr(address, pointerType, "mem_ptr");
  llvm::Value *memoryValue =
      builder.CreateLoad(llvm::Type::getInt64Ty(context), pointer, "mem_value");
  builder.CreateStore(memoryValue, sink);

  llvm::Value *old = loadRegister(builder, rdx, "RDX", "old_rdx");
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(rdx->getValueType(),
                                  llvm::APInt::getBitsSet(64, 8, 64)));
  storeRegister(builder, rdx, builder.CreateOr(keep, memoryValue), "RDX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);

  llvm::IntToPtrInst *memoryPointer = nullptr;
  for (llvm::Function &rewrittenFunction : module) {
    if (rewrittenFunction.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(&inst)) {
        if (intToPtr->getName() == "mem_ptr") {
          memoryPointer = intToPtr;
          break;
        }
      }
    }
    if (memoryPointer != nullptr) {
      break;
    }
  }
  if (memoryPointer == nullptr) {
    for (llvm::Function &rewrittenFunction : module) {
      if (rewrittenFunction.isDeclaration()) {
        continue;
      }
      for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (load == nullptr || load->getName() != "mem_value") {
          continue;
        }
        memoryPointer =
            llvm::dyn_cast<llvm::IntToPtrInst>(load->getPointerOperand());
        break;
      }
    }
  }
  bool addressWasZero = false;
  if (memoryPointer != nullptr) {
    if (auto *constant =
            llvm::dyn_cast<llvm::ConstantInt>(memoryPointer->getOperand(0))) {
      addressWasZero = constant->isZero();
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial memory pointer store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial memory pointer store was not demand rewritten") &&
         expect(memoryPointer != nullptr,
                "ordinary memory inttoptr was removed") &&
         expect(!addressWasZero, "ordinary memory pointer register address was "
                                 "rewritten to zero") &&
         verifyOk(module,
                  "module failed verifier after partial memory pointer test");
}

bool testLowBitPeepholeRewritesFullLoadTrunc() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-low-bit-peephole-trunc", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "low_bit_trunc", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *full = loadRegister(builder, rsi, "RSI", "full_rsi");
  llvm::Value *low =
      builder.CreateTrunc(full, llvm::Type::getInt32Ty(context), "low_rsi");
  builder.CreateStore(low, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);

  return expect(summary.Rewrites == 1,
                "low-bit peephole did not rewrite full load trunc") &&
         expect(summary.DirectTruncRewrites == 1,
                "low-bit peephole did not count direct trunc rewrite") &&
         expect(!hasRegisterLoad(*function, "RSI"),
                "low-bit peephole left full RSI load") &&
         expect(countPartialReadCalls(*function, rsi, 0, 32) == 1,
                "low-bit peephole did not create low32 partial read") &&
         verifyOk(module, "module failed verifier after low-bit trunc test");
}

bool testLowBitPeepholeRewritesShiftTrunc() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-low-bit-peephole-shift-trunc", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "low_bit_shift_trunc", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *full = loadRegister(builder, rsi, "RSI", "full_rsi");
  llvm::Value *shifted = builder.CreateLShr(
      full, llvm::ConstantInt::get(rsi->getValueType(), 32), "high_shift");
  llvm::Value *high =
      builder.CreateTrunc(shifted, llvm::Type::getInt32Ty(context), "high_rsi");
  builder.CreateStore(high, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);

  return expect(summary.Rewrites == 1,
                "low-bit peephole did not rewrite shift trunc") &&
         expect(summary.ShiftTruncRewrites == 1,
                "low-bit peephole did not count shift trunc rewrite") &&
         expect(!hasRegisterLoad(*function, "RSI"),
                "low-bit peephole left shifted full RSI load") &&
         expect(countPartialReadCalls(*function, rsi, 32, 32) == 1,
                "low-bit peephole did not create high32 partial read") &&
         verifyOk(module,
                  "module failed verifier after low-bit shift trunc test");
}

bool testLowBitPeepholeSkipsMultiUseFullLoad() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-low-bit-peephole-multi-use", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *narrowSink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "narrow_sink");
  auto *fullSink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "full_sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "low_bit_multi_use", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *full = loadRegister(builder, rsi, "RSI", "full_rsi");
  llvm::Value *low =
      builder.CreateTrunc(full, llvm::Type::getInt32Ty(context), "low_rsi");
  builder.CreateStore(low, narrowSink);
  builder.CreateStore(full, fullSink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);

  return expect(summary.Rewrites == 0,
                "low-bit peephole rewrote a multi-use full load") &&
         expect(summary.MultiUseLoadsSkipped == 1,
                "low-bit peephole did not count multi-use skip") &&
         expect(hasRegisterLoad(*function, "RSI"),
                "low-bit peephole removed full load with live full use") &&
         expect(!hasPartialReadCall(*function),
                "low-bit peephole created partial read for multi-use load") &&
         verifyOk(module,
                  "module failed verifier after low-bit multi-use test");
}

bool testFinalCleanupDropsDeadRegisterGlobalsAndSummaryMetadata() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-clean", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "final_cleanup_clean", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = llvm::ConstantInt::get(rdi->getValueType(), 17);
  storeRegister(builder, rdi, value, "RDI");
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hadMetadata = functionHasAnyRegisterSummaryMetadata(*function);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(hadMetadata,
                "summary SSA did not attach metadata before cleanup") &&
         expect(module.getGlobalVariable("RDI") == nullptr,
                "dead RDI global remained after final cleanup") &&
         expect(!functionHasAnyRegisterSummaryMetadata(*function),
                "register summary metadata remained on clean function") &&
         expect(cleanup.FunctionMetadataCleared >= 1,
                "final cleanup did not count cleared metadata") &&
         expect(cleanup.RemainingRegisterAccesses == 0,
                "final cleanup reported register residue in clean module") &&
         verifyOk(module, "module failed verifier after final cleanup test");
}

bool testFinalCleanupKeepsMetadataWhenRegisterAccessRemains() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-residue", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_residue", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *live = loadRegister(builder, rbx, "RBX", "rbx.live");
  llvm::AllocaInst *sink = builder.CreateAlloca(rbx->getValueType());
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(module.getGlobalVariable("RBX") != nullptr,
                "live RBX global was removed") &&
         expect(function->getMetadata("notdec.register.summary_ssa") != nullptr,
                "metadata was cleared while register access remained") &&
         expect(cleanup.RemainingRegisterAccesses == 1,
                "final cleanup did not report remaining register access") &&
         verifyOk(module, "module failed verifier after residue cleanup test");
}

bool testFinalCleanupCountsPartialWriteResidue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-partial-write", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_partial_write", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(module.getGlobalVariable("RBX") != nullptr,
                "partial write RBX global was removed") &&
         expect(function->getMetadata("notdec.register.summary_ssa") != nullptr,
                "metadata was cleared while partial write remained") &&
         expect(cleanup.RemainingRegisterAccesses == 1,
                "final cleanup did not count partial write residue") &&
         verifyOk(module,
                  "module failed verifier after partial write cleanup test");
}

bool testFinalCleanupCountsPartialReadResidue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-partial-read", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_partial_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *value = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *sinkPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096),
      llvm::PointerType::get(context, 0), "sink");
  builder.CreateStore(value, sinkPointer);
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(module.getGlobalVariable("RBX") != nullptr,
                "partial read RBX global was removed") &&
         expect(function->getMetadata("notdec.register.summary_ssa") != nullptr,
                "metadata was cleared while partial read remained") &&
         expect(cleanup.RemainingRegisterAccesses == 1,
                "final cleanup did not count partial read residue") &&
         verifyOk(module,
                  "module failed verifier after partial read cleanup test");
}

bool testFinalCleanupDropsDeadPartialReadsAfterValueRangeLowering() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-dead-value-range", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_dead_value_range", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *low = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *high = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  llvm::Value *full = builder.CreateCall(
      insert, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), low,
               llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  full = builder.CreateCall(
      insert, {full, high,
               llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  (void)full;
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(cleanup.ValueRangeHelpersLowered == 2,
                "dead value range helpers were not lowered") &&
         expect(cleanup.DeadRegisterReadsRemoved == 2,
                "dead partial reads after value range lowering remained") &&
         expect(module.getGlobalVariable("RBX") == nullptr,
                "dead RBX global remained after value range cleanup") &&
         expect(!functionHasAnyRegisterSummaryMetadata(*function),
                "metadata remained after dead value range cleanup") &&
         expect(cleanup.RemainingRegisterAccesses == 0,
                "dead value range cleanup left register residue") &&
         verifyOk(module,
                  "module failed verifier after dead value range cleanup test");
}

bool testPostRewritePeepholeSimplifiesExtractFromInsertedRange() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-rewrite-peephole-inserted-extract",
                      context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt32Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_inserted_extract", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Argument *lowArg = function->getArg(0);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *high = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  llvm::Value *full = builder.CreateCall(
      insert,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), lowArg,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  full = builder.CreateCall(
      insert, {full, high,
               llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  llvm::Value *lowAgain =
      builder.CreateTrunc(full, llvm::Type::getInt32Ty(context), "low_again");
  builder.CreateStore(lowAgain, sink);
  builder.CreateRetVoid();

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  bool storeUsesArg = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    storeUsesArg |= store != nullptr && store->getPointerOperand() == sink &&
                    store->getValueOperand() == lowArg;
  }

  return expect(peephole.ValueRangeExtractsSimplified == 1,
                "inserted range extract was not simplified") &&
         expect(storeUsesArg,
                "low range did not rewrite to the inserted value") &&
         expect(
             cleanup.DeadRegisterReadsRemoved == 1,
             "dead high partial read remained after extract simplification") &&
         expect(module.getGlobalVariable("RBX") == nullptr,
                "dead RBX global remained after inserted extract cleanup") &&
         expect(cleanup.RemainingRegisterAccesses == 0,
                "inserted extract cleanup left register residue") &&
         verifyOk(module,
                  "module failed verifier after inserted extract cleanup test");
}

bool testFinalCleanupRunsGlobalDCEForUnusedIntrinsicDeclarations() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-globaldce", context);

  llvm::Intrinsic::getOrInsertDeclaration(&module,
                                          llvm::Intrinsic::ssub_with_overflow,
                                          {llvm::Type::getInt8Ty(context)});
  auto *helperType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {}, false);
  llvm::Function::Create(helperType, llvm::GlobalValue::ExternalLinkage,
                         "notdec.register.summary_return.i64", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_globaldce", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(!moduleHasOverflowIntrinsicDeclaration(module),
                "unused overflow intrinsic declaration remained") &&
         expect(!moduleHasFunctionNamed(module,
                                        "notdec.register.summary_return.i64"),
                "unused register summary helper declaration remained") &&
         verifyOk(module,
                  "module failed verifier after globaldce cleanup test");
}

bool testFinalCleanupDropsDeadUnknownValueCalls() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-dead-unknown", context);

  auto *unknownType =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {}, false);
  llvm::Function *unknown =
      llvm::Function::Create(unknownType, llvm::GlobalValue::ExternalLinkage,
                             "notdec.unknown.i32", module);
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_dead_unknown", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(unknown, {}, "dead_unknown");
  llvm::CallInst *live = builder.CreateCall(unknown, {}, "live_unknown");
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  unsigned unknownCalls = 0;
  bool storeUsesUnknown = false;
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (isUnknownValueCall(&inst)) {
      ++unknownCalls;
    }
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    storeUsesUnknown |= store != nullptr &&
                        store->getPointerOperand() == sink &&
                        isUnknownValueCall(store->getValueOperand());
  }

  return expect(unknownCalls == 1,
                "final cleanup kept wrong number of unknown value calls") &&
         expect(storeUsesUnknown, "live unknown value call was removed") &&
         expect(cleanup.DeadUnknownValuesRemoved == 1,
                "final cleanup did not count dead unknown value") &&
         expect(moduleHasUsedFunctionNamed(module, "notdec.unknown.i32"),
                "live unknown helper declaration was removed") &&
         verifyOk(module,
                  "module failed verifier after dead unknown cleanup test");
}

bool testFinalCleanupLowersValueRangeHelpers() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-value-range", context);

  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_value_range", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Argument *arg = function->getArg(0);
  llvm::Function *extract =
      notdec::bin2llvm::getOrInsertNativeRegisterValueExtract(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *low = builder.CreateCall(
      extract,
      {arg, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *full = builder.CreateCall(
      insert,
      {arg, low, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateStore(full, sink);
  builder.CreateRetVoid();

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  bool hasValueRangeCall = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    hasValueRangeCall |= call != nullptr &&
                         call->getCalledFunction() != nullptr &&
                         notdec::bin2llvm::isNativeRegisterValueRangeName(
                             call->getCalledFunction()->getName());
  }

  return expect(cleanup.ValueRangeHelpersLowered == 2,
                "value range helpers were not lowered") &&
         expect(!hasValueRangeCall, "value range helper call remained") &&
         expect(!moduleHasFunctionNamed(module, "notdec.reg.extract.i64.i32"),
                "value range extract declaration remained") &&
         expect(!moduleHasFunctionNamed(module, "notdec.reg.insert.i64.i32"),
                "value range insert declaration remained") &&
         verifyOk(module,
                  "module failed verifier after value range helper cleanup");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testPhiIncomingMatchesPredecessors();
  ok &= testDuplicatePredecessorEdgesKeepPhiComplete();
  ok &= testPartialReadLoopPassthroughUsesDominatorTree();
  ok &= testRegisterPointerPhiLoadIsCanonicalized();
  ok &= testUnknownPhiIncomingUsesFrozenPoison();
  ok &= testSelfOnlyPhiBecomesOpaqueUnknown();
  ok &= testFsOffsetPreservedAcrossExternalCall();
  ok &= testPreservedCallKeepsPreviousValue();
  ok &= testRewrittenExternalCallPreservesUnaffectedRegister();
  ok &= testDemandedReturnCreatesCallValue();
  ok &= testExternalReturnUsesRangeCallValue();
  ok &= testIndirectCallReturnHelperIsRewritten();
  ok &= testIntrinsicDoesNotCreateCallValue();
  ok &= testOverwrittenStoreIsRemoved();
  ok &= testCrossBlockDeadStoreIsRemoved();
  ok &= testAbiInputStoreBeforeCallIsKept();
  ok &= testI386KnownExternalUsesCspecStackOffset();
  ok &= testI386AlignedStackPointerOutgoingArgIsBound();
  ok &= testI386UnknownExternalDoesNotReusePreviousStackArgs();
  ok &= testI386NativeFrameOutgoingStackArgIsBound();
  ok &= testI386NativeFrameLoadDoesNotBecomeStackInput();
  ok &= testI386NativeFrameOutgoingVarArgPrefixIsBound();
  ok &= testI386StackPointerSummaryOutgoingVarArgPrefixIsBound();
  ok &= testX64KnownExternalUsesCspecStackOverflowOffset();
  ok &= testX64AlignedStackPointerOutgoingArgIsBound();
  ok &= testI386InternalStackInputIsRewritten();
  ok &= testI386AdjustedEspStackInputIsRewritten();
  ok &= testI386NegativeEspLoadDoesNotBecomeStackInput();
  ok &= testI386AlignedEspLoadDoesNotBecomeStackInput();
  ok &= testX64AlignedRspLoadDoesNotBecomeStackInput();
  ok &= testKnownZeroArgExternalDropsAbiInputs();
  ok &= testKnownFixedArgExternalTruncatesAbiInputs();
  ok &= testCallArgUsesPartialRangeRead();
  ok &= testDeadFlagStoreBeforeCallIsRemoved();
  ok &= testFlagStoreBeforeVolatileCallIsRemoved();
  ok &= testPostRewriteInstCombineExposesDeadFlagStore();
  ok &= testKnownZeroArgExternalTypedReturnIsMaterialized();
  ok &= testKnownErrnoLocationReturnIsMaterialized();
  ok &= testKnownErrnoLocationReturnUsesI386DataLayout();
  ok &= testKnownFiveArgExternalUsesFiveInputs();
  ok &= testKnownFixedExternalArities();
  ok &= testKnownVarArgExternalInfersDefinedAbiInputs();
  ok &= testKnownVarArgFixedOnlyCallDoesNotReadTail();
  ok &= testKnownVarArgCallsitesKeepIndependentArities();
  ok &= testKnownVarArgDoesNotReuseStaleCallerSavedTail();
  ok &= testKnownVarArgUsesSseCountForFloatTail();
  ok &= testKnownVarArgZeroTailKeepsConstantZero();
  ok &= testKnownVarArgUnknownPhiTailUsesPoison();
  ok &= testNonX64VarArgUnknownPhiTailDoesNotUsePoisonHelper();
  ok &= testMismatchedDirectCallUseUsesReturnExtract();
  ok &= testKnownExternalUsesSingleIntegerReturn();
  ok &= testUnknownExternalTreatsRdxAsClobberNotReturn();
  ok &= testUnknownExternalClobberArgBecomesUnknown();
  ok &= testInternalReturnDoesNotExposeExternalClobber();
  ok &= testClobberReturnPhiDoesNotMaterializeHelper();
  ok &= testUnknownExternalArityUsesMaxCallsitePrefix();
  ok &= testExternalPrototypeJsonOverridesInferredArity();
  ok &= testUnknownExternalArityStopsAtClobberArg();
  ok &= testUnknownExternalArityStopsAtPhiClobberArg();
  ok &= testUnknownExternalArityCountsExplicitBinaryArg();
  ok &= testRecordedCallArgValueSurvivesDeadStoreCleanup();
  ok &= testInternalCallArgBindingsKeepLaterArgsAfterEntryInput();
  ok &= testInternalSignatureParamsUseAbiOrder();
  ok &= testInternalSignatureRewriteUsesArgsAndReturn();
  ok &= testInternalSignatureRewriteUsesNonAbiReturn();
  ok &= testInternalSignatureRewriteUsesReadEntryReturnRegisterArg();
  ok &= testInternalSignatureRewriteUsesNarrowEntryRangeArg();
  ok &= testInternalSignatureShapeParamKeepsOverwrittenEntryRange();
  ok &= testNarrowEntryRangeDoesNotCreateWholeEntryLoad();
  ok &= testPostSignatureCleanupRewritesInternalEntryRawLoad();
  ok &= testInternalSignatureRewriteUsesZmmArgAndReturn();
  ok &= testPreservedZmmEntryIsPassedAsInternalArgument();
  ok &= testInternalSignatureRewriteUsesZmmLowLaneReturn();
  ok &= testForeignArgumentInMovedBodyIsReplaced();
  ok &= testForeignMappedCallArgumentIsLocalized();
  ok &= testStaticRspStackRewriteKeepsSavedRegisterEvidence();
  ok &= testCallerSavedEntryStackSpillCountsAsReadEntry();
  ok &= testFramePointerLoadFeedsStackRewriteWithoutGlobalIgnore();
  ok &= testFramePointerRewriteDoesNotHideRbpRegisterFlow();
  ok &= testSummarySSARemovesDeadStackFrameStore();
  ok &= testStackFrameAddressPassedToCallIsLocalized();
  ok &= testPostSignatureCleanupDropsAbiStoreBeforeUnrewrittenCall();
  ok &= testNoReturnExternalDoesNotCreateSummaryReturn();
  ok &= testExternalPrototypeJsonOverlaysDefaultNoReturn();
  ok &= testInternalNoReturnCallFallthroughIsTruncated();
  ok &= testStackCanaryCheckIsRemoved();
  ok &= testI386GsStackCanaryCheckIsRemoved();
  ok &= testDeadStackCanaryFailThunkIsRemoved();
  ok &= testStackCanaryPrologueSaveIsRemoved();
  ok &= testStackCanaryZextConditionIsRemoved();
  ok &= testStackCanaryMaskedSavedLoadIsRemoved();
  ok &= testStackCanaryFailSideEffectIsKept();
  ok &= testStackCanaryWrongFsOffsetIsKept();
  ok &= testRawRspStackCanaryCheckIsRemovedAfterStackCleanup();
  ok &= testRawRspZfStackCanaryCheckIsRemoved();
  ok &= testPhiFsBaseStackCanaryCheckIsRemoved();
  ok &= testZeroBaseStackCanaryCheckIsKept();
  ok &= testSharedFailStackCanaryChecksAreRemoved();
  ok &= testMixedFailPredecessorKeepsNonCanaryFailPath();
  ok &= testPhiFsCanaryAddressStackCanaryCheckIsRemoved();
  ok &= testSelfPhiFsCanaryAddressStackCanaryCheckIsRemoved();
  ok &= testSelfPhiFsBaseStackCanaryCheckIsRemoved();
  ok &= testXmmAbiEffectUsesZmmBackingWithoutSignatureReturn();
  ok &= testUnknownExternalFloatAbiClobberUsesWholeZmm();
  ok &= testUnknownExternalIntegerClobberUsesRangeCallValue();
  ok &= testKnownPowUsesFloatAbiSlots();
  ok &= testKnownUnaryLibmUsesFloatAbiSlots();
  ok &= testPartialKeepHighStoreIsDemandRewritten();
  ok &= testPartialWriteHelperIsConsumedBySummarySSA();
  ok &= testPartialReadHelperIsConsumedBySummarySSA();
  ok &= testFullStoreFeedsPartialReadThroughRangeSSA();
  ok &= testDeadPartialReadHelperIsRemovedBySummarySSA();
  ok &= testDeadSummaryCallValueHelperIsRemovedBySummarySSA();
  ok &= testConsecutivePartialReadHelpersAreConsumedBySummarySSA();
  ok &= testConsecutivePartialReadXorIsZeroedAfterSummarySSA();
  ok &= testDuplicatePartialReadXorAfterUnknownCallIsZeroed();
  ok &= testBranchPartialReadUsesNarrowRangePhi();
  ok &= testBranchFullLoadAfterPartialWriteUsesNarrowRangePhi();
  ok &= testLoopPartialWriteUsesNarrowRangePhi();
  ok &= testDeadPartialWriteUsesRangeLiveness();
  ok &= testFloatCallEffectUsesRangeLiveness();
  ok &= testPlannerSplitsZmmForFloatAbiSlot();
  ok &= testPlannerSplitsRegisterForDemandMask();
  ok &= testPartialWriteHelperNameSurvivesSignatureRewrite();
  ok &= testPartialReadHelperNameSurvivesSignatureRewrite();
  ok &= testRecordedReturnValueSurvivesPartialDemandRewrite();
  ok &= testPartialDemandZeroReplacementIsMarked();
  ok &= testPartialZmmKeepHighStoreIsDemandRewritten();
  ok &= testPartialZmmNakedKeepHighStoreIsDemandRewritten();
  ok &= testPartialZmmDisjointLaneChainIsDemandRewritten();
  ok &= testPartialDemandKeepsMemoryPointerRegisterAddress();
  ok &= testLowBitPeepholeRewritesFullLoadTrunc();
  ok &= testLowBitPeepholeRewritesShiftTrunc();
  ok &= testLowBitPeepholeSkipsMultiUseFullLoad();
  ok &= testFinalCleanupDropsDeadRegisterGlobalsAndSummaryMetadata();
  ok &= testFinalCleanupKeepsMetadataWhenRegisterAccessRemains();
  ok &= testFinalCleanupCountsPartialWriteResidue();
  ok &= testFinalCleanupCountsPartialReadResidue();
  ok &= testFinalCleanupDropsDeadPartialReadsAfterValueRangeLowering();
  ok &= testPostRewritePeepholeSimplifiesExtractFromInsertedRange();
  ok &= testFinalCleanupRunsGlobalDCEForUnusedIntrinsicDeclarations();
  ok &= testFinalCleanupDropsDeadUnknownValueCalls();
  ok &= testFinalCleanupLowersValueRangeHelpers();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
