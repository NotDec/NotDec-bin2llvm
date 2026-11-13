#pragma once

#include "notdec-bin2llvm/abi/NativeAbi.h"
#include "notdec-bin2llvm/abi/NativeExternalPrototype.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterFinalCleanup.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterPreSummaryPeephole.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterPostRewritePeephole.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterElimination.h"
#include "notdec-bin2llvm/passes/stack/NativeStackFrame.h"
#include "notdec-bin2llvm/passes/vararg/NativeVarArgRewrite.h"

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
#include <map>
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

void attachMultiOutputTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_multi_output_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(input);

  auto addOutput = [&](llvm::StringRef name, uint32_t minSize,
                       uint32_t maxSize) {
    notdec::bin2llvm::NativeAbiParamEntry output;
    output.MinSize = minSize;
    output.MaxSize = maxSize;
    output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    output.Storage.Name = name.str();
    abi.Outputs.push_back(output);

    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  };
  addOutput("RAX", 1, 8);
  addOutput("RDX", 1, 8);
  addOutput("XMM0", 1, 16);
  addOutput("ST0", 10, 16);

  for (llvm::StringRef name : {"RBX", "RBP"}) {
    notdec::bin2llvm::NativeAbiEffect unaffected;
    unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
    unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    unaffected.Storage.Name = name.str();
    abi.Effects.push_back(unaffected);
  }

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
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

unsigned countStoresTo(const llvm::Function &function,
                       llvm::GlobalVariable *global) {
  unsigned count = 0;
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store != nullptr &&
        store->getPointerOperand()->stripPointerCasts() == global) {
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

bool functionHasPoisonZeroDemandOperand(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    const llvm::MDNode *metadata =
        inst.getMetadata("notdec.register.summary_ssa.zero_demand_operand");
    if (metadata == nullptr) {
      continue;
    }
    bool hasPoisonReplacementMetadata = false;
    for (const llvm::MDOperand &entryOperand : metadata->operands()) {
      const auto *entry =
          llvm::dyn_cast_or_null<llvm::MDNode>(entryOperand.get());
      if (metadataHasField(entry, "replacement=poison")) {
        hasPoisonReplacementMetadata = true;
        break;
      }
    }
    if (!hasPoisonReplacementMetadata) {
      continue;
    }
    for (const llvm::Use &operand : inst.operands()) {
      if (llvm::isa<llvm::PoisonValue>(operand.get())) {
        return true;
      }
    }
  }
  return false;
}

bool valueGraphContainsPoison(
    const llvm::Value *value,
    llvm::SmallPtrSetImpl<const llvm::Value *> &visiting) {
  if (value == nullptr || !visiting.insert(value).second) {
    return false;
  }
  if (llvm::isa<llvm::PoisonValue>(value)) {
    return true;
  }
  auto *user = llvm::dyn_cast<llvm::User>(value);
  if (user == nullptr) {
    return false;
  }
  for (const llvm::Use &operand : user->operands()) {
    if (valueGraphContainsPoison(operand.get(), visiting)) {
      return true;
    }
  }
  return false;
}

bool valueGraphContainsPoison(const llvm::Value *value) {
  llvm::SmallPtrSet<const llvm::Value *, 16> visiting;
  return valueGraphContainsPoison(value, visiting);
}

bool functionReturnGraphsContainPoison(const llvm::Function &function) {
  for (const llvm::BasicBlock &block : function) {
    auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
    if (ret != nullptr && valueGraphContainsPoison(ret->getReturnValue())) {
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

bool isLoadFromValue(llvm::Value *value, const llvm::GlobalVariable *global) {
  while (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    value = cast->getOperand(0);
  }
  auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
  return load != nullptr &&
         load->getPointerOperand()->stripPointerCasts() == global;
}

uint32_t countLoadsFromGlobal(const llvm::Function &function,
                              const llvm::GlobalVariable *global) {
  uint32_t count = 0;
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == global) {
          ++count;
        }
      }
    }
  }
  return count;
}

bool functionReadsRegisterGlobal(const llvm::Function &function,
                                 const llvm::GlobalVariable *global) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == global) {
          return true;
        }
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      std::optional<notdec::bin2llvm::NativeRegisterPartialReadInfo> read =
          notdec::bin2llvm::parseNativeRegisterPartialRead(*call);
      if (read && read->Global == global) {
        return true;
      }
    }
  }
  return false;
}

enum class ByteIndexShape {
  // 正例：区间内只有 8 位读。
  ByteOnly,
  // 反例：区间内还有一次 32 位读（标志位物化那种真用途）。
  WideReadInRegion,
  // 反例：区间内有真实 call（callee 可能把该寄存器当参数读走）。
  RealCallInRegion,
  // 反例：区间里只有写、没有任何 8 位读（低字节没被用）。
  NoByteRead,
  // 反例：宽读有别的 user。
  WideMultipleUses,
};

llvm::Function *buildByteIndexFunction(llvm::Module &module,
                                       llvm::StringRef name,
                                       ByteIndexShape shape) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *source = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *destination = createRegisterGlobal(module, "RDX");
  llvm::Function *read32 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, source->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *read8 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, source->getType(), llvm::Type::getInt8Ty(context), 64, 8);
  llvm::Function *write32 = notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
      module, source->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *external = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "byte_index_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *hit = llvm::BasicBlock::Create(context, "hit", function);
  llvm::BasicBlock *miss = llvm::BasicBlock::Create(context, "miss", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
  llvm::Value *wide = builder.CreateCall(read32, {source, zero}, "wide");
  llvm::Value *index = builder.CreateAdd(
      wide, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), -9), "index");
  if (shape == ByteIndexShape::WideMultipleUses) {
    builder.CreateCall(write32,
                       {destination, index, zero});  // 宽读/下标多一个 user
  }
  builder.CreateCall(write32, {destination, index, zero});
  llvm::Value *back = builder.CreateCall(read32, {destination, zero}, "back");
  llvm::Value *extended = builder.CreateZExt(back, llvm::Type::getInt64Ty(context));
  builder.CreateStore(extended, destination);
  llvm::Value *compare = nullptr;
  if (shape == ByteIndexShape::NoByteRead) {
    llvm::Value *other = builder.CreateCall(read8, {source, zero}, "other");
    compare = builder.CreateICmpEQ(
        other, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0));
  } else {
    llvm::Value *byte = builder.CreateCall(read8, {destination, zero}, "byte");
    compare = builder.CreateICmpULT(
        byte, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 35));
  }
  builder.CreateCondBr(compare, hit, miss);

  builder.SetInsertPoint(hit);
  if (shape == ByteIndexShape::WideReadInRegion) {
    llvm::Value *flag = builder.CreateCall(read32, {destination, zero}, "flag");
    llvm::Value *isZero = builder.CreateICmpEQ(
        flag, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    builder.CreateStore(
        builder.CreateZExt(isZero, llvm::Type::getInt8Ty(context)),
        new llvm::GlobalVariable(module, llvm::Type::getInt8Ty(context), false,
                                 llvm::GlobalValue::ExternalLinkage, nullptr,
                                 name.str() + "_zf"));
  }
  if (shape == ByteIndexShape::RealCallInRegion) {
    builder.CreateCall(external, {});
  }
  if (shape != ByteIndexShape::NoByteRead) {
    llvm::Value *byte2 = builder.CreateCall(read8, {destination, zero}, "byte2");
    llvm::Value *extended2 =
        builder.CreateZExt(byte2, llvm::Type::getInt32Ty(context));
    builder.CreateCall(write32, {destination, extended2, zero});
  } else {
    builder.CreateCall(write32, {destination,
                                 llvm::ConstantInt::get(
                                     llvm::Type::getInt32Ty(context), 0),
                                 zero});
  }
  builder.CreateRetVoid();

  builder.SetInsertPoint(miss);
  builder.CreateCall(write32, {destination,
                               llvm::ConstantInt::get(
                                   llvm::Type::getInt32Ty(context), 0),
                               zero});
  builder.CreateRetVoid();
  return function;
}

enum class WidthNarrowShape { MoveByte, MoveHalf, ShiftRight, Division };

llvm::Function *buildWidthNarrowFunction(llvm::Module &module,
                                         llvm::StringRef name,
                                         WidthNarrowShape shape) {
  llvm::LLVMContext &context = module.getContext();
  llvm::GlobalVariable *source = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *destination = createRegisterGlobal(module, "RDX");
  llvm::Function *read8 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, source->getType(), llvm::Type::getInt8Ty(context), 64, 8);
  llvm::Function *read16 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, source->getType(), llvm::Type::getInt16Ty(context), 64, 16);
  llvm::Function *read32 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, source->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *write32 = notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
      module, source->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *hit = llvm::BasicBlock::Create(context, "hit", function);
  llvm::BasicBlock *miss = llvm::BasicBlock::Create(context, "miss", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
  llvm::Value *value = builder.CreateCall(read32, {source, zero}, "value");
  llvm::Value *written = value;
  if (shape == WidthNarrowShape::ShiftRight) {
    written = builder.CreateLShr(
        value, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 8),
        "shifted");
  } else if (shape == WidthNarrowShape::Division) {
    written = builder.CreateUDiv(
        value, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 10),
        "quotient");
  }
  builder.CreateCall(write32, {destination, written, zero});
  llvm::Value *back = builder.CreateCall(read32, {destination, zero}, "back");
  llvm::Value *extended =
      builder.CreateZExt(back, llvm::Type::getInt64Ty(context));
  builder.CreateStore(extended, destination);
  llvm::Value *observed =
      shape == WidthNarrowShape::MoveHalf
          ? builder.CreateCall(read16, {destination, zero}, "half")
          : builder.CreateCall(read8, {destination, zero}, "byte");
  llvm::Value *compare = builder.CreateICmpULT(
      observed, llvm::ConstantInt::get(observed->getType(), 35));
  builder.CreateCondBr(compare, hit, miss);
  for (llvm::BasicBlock *block : {hit, miss}) {
    builder.SetInsertPoint(block);
    builder.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0),
        destination);
    builder.CreateRetVoid();
  }
  return function;
}

struct MagicDivisionCase {
  // 识别器应该写出的除数。带预移位时它是内层除数（x/10^9 写成 (x>>9)/1953125），
  // 后续 InstCombine 会把 `udiv (x>>9), 1953125` 折回 `udiv x, 10^9`。
  uint64_t ExpectedDivisor;
  uint64_t Magic;
  unsigned PostShift;
  unsigned PreShift = 0;
};

llvm::Function *buildMagicDivisionFunction(llvm::Module &module,
                                           llvm::StringRef name,
                                           const MagicDivisionCase &testCase,
                                           bool foldCorrectionShift,
                                           unsigned width = 64) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *i64 = llvm::IntegerType::get(context, width);
  llvm::Type *i128 = llvm::IntegerType::get(context, 2 * width);
  auto *type = llvm::FunctionType::get(i64, {i64}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *dividend = function->getArg(0);
  if (testCase.PreShift != 0) {
    dividend = builder.CreateLShr(
        dividend, llvm::ConstantInt::get(i64, testCase.PreShift), "prescaled");
  }
  llvm::Value *wide = builder.CreateZExt(dividend, i128, "wide");
  llvm::Value *product =
      builder.CreateMul(wide, llvm::ConstantInt::get(i128, testCase.Magic));
  if (foldCorrectionShift) {
    // InstCombine 会把 `mulhi >> W` + trunc + `>> s` 折成 `mulhi >> (W+s)`
    // + trunc，两种形态都要能认。
    llvm::Value *shifted = builder.CreateLShr(
        product, llvm::ConstantInt::get(i128, width + testCase.PostShift));
    builder.CreateRet(builder.CreateTrunc(shifted, i64, "quotient"));
    return function;
  }
  llvm::Value *high =
      builder.CreateLShr(product, llvm::ConstantInt::get(i128, width));
  llvm::Value *truncated = builder.CreateTrunc(high, i64);
  builder.CreateRet(builder.CreateLShr(
      truncated, llvm::ConstantInt::get(i64, testCase.PostShift), "quotient"));
  return function;
}

bool functionHasUdivBy(const llvm::Function &function, uint64_t divisor) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.getOpcode() != llvm::Instruction::UDiv) {
      continue;
    }
    auto *constant = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1));
    auto *type = llvm::dyn_cast<llvm::IntegerType>(inst.getType());
    if (constant != nullptr && type != nullptr &&
        constant->getValue() == llvm::APInt(type->getBitWidth(), divisor)) {
      return true;
    }
  }
  return false;
}

bool functionHasMul(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.getOpcode() == llvm::Instruction::Mul) {
      return true;
    }
  }
  return false;
}

llvm::Function *buildSignedMagicDivisionFunction(llvm::Module &module,
                                                 llvm::StringRef name,
                                                 const MagicDivisionCase &testCase,
                                                 unsigned width) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *valueType = llvm::IntegerType::get(context, width);
  llvm::Type *wideType = llvm::IntegerType::get(context, 2 * width);
  auto *type = llvm::FunctionType::get(valueType, {valueType}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *wide = builder.CreateSExt(function->getArg(0), wideType, "wide");
  llvm::Value *product = builder.CreateNSWMul(
      wide, llvm::ConstantInt::get(wideType, testCase.Magic));
  llvm::Value *high = builder.CreateLShr(
      product, llvm::ConstantInt::get(wideType, width));
  llvm::Value *truncated = builder.CreateTrunc(high, valueType);
  llvm::Value *shifted = truncated;
  if (testCase.PostShift != 0) {
    shifted = builder.CreateAShr(
        truncated, llvm::ConstantInt::get(valueType, testCase.PostShift));
  }
  llvm::Value *sign = builder.CreateLShr(
      truncated, llvm::ConstantInt::get(valueType, width - 1));
  builder.CreateRet(builder.CreateAdd(shifted, sign, "quotient"));
  return function;
}

bool functionHasSdivBy(const llvm::Function &function, uint64_t divisor) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.getOpcode() != llvm::Instruction::SDiv) {
      continue;
    }
    auto *constant = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1));
    auto *type = llvm::dyn_cast<llvm::IntegerType>(inst.getType());
    if (constant != nullptr && type != nullptr &&
        constant->getValue() == llvm::APInt(type->getBitWidth(), divisor)) {
      return true;
    }
  }
  return false;
}

llvm::Function *buildUnsignedAddFormFunction(llvm::Module &module,
                                             llvm::StringRef name,
                                             const MagicDivisionCase &testCase,
                                             unsigned width) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *valueType = llvm::IntegerType::get(context, width);
  llvm::Type *wideType = llvm::IntegerType::get(context, 2 * width);
  auto *type = llvm::FunctionType::get(valueType, {valueType}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *x = function->getArg(0);
  llvm::Value *wide = builder.CreateZExt(x, wideType);
  llvm::Value *product = builder.CreateNUWMul(
      wide, llvm::ConstantInt::get(wideType, testCase.Magic));
  llvm::Value *high = builder.CreateLShr(
      product, llvm::ConstantInt::get(wideType, width));
  llvm::Value *h = builder.CreateTrunc(high, valueType, "high_half");
  llvm::Value *difference = builder.CreateSub(x, h);
  llvm::Value *halved = builder.CreateLShr(
      difference, llvm::ConstantInt::get(valueType, 1));
  llvm::Value *sum = builder.CreateAdd(halved, h);
  builder.CreateRet(builder.CreateLShr(
      sum, llvm::ConstantInt::get(valueType, testCase.PostShift), "quotient"));
  return function;
}

llvm::Value *wrapRegisterWidthRoundTrip(llvm::IRBuilder<> &builder,
                                        llvm::Value *value, unsigned width) {
  llvm::LLVMContext &context = builder.getContext();
  llvm::Type *valueType = llvm::IntegerType::get(context, width);
  llvm::Type *wideType = llvm::IntegerType::get(context, 2 * width);
  return builder.CreateTrunc(builder.CreateZExt(value, wideType), valueType);
}

llvm::Value *emitFramePointer(llvm::IRBuilder<> &builder, llvm::AllocaInst *frame,
                              uint64_t offset) {
  llvm::LLVMContext &context = builder.getContext();
  return builder.CreateGEP(
      llvm::Type::getInt8Ty(context), frame,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), offset));
}

llvm::Value *emitManualVaListInit(llvm::IRBuilder<> &builder,
                                  llvm::AllocaInst *frame, uint64_t vaListOffset,
                                  uint64_t gpOffsetValue,
                                  uint64_t saveAreaOffset) {
  llvm::LLVMContext &context = builder.getContext();
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *i32Type = llvm::Type::getInt32Ty(context);
  llvm::Value *vaList = emitFramePointer(builder, frame, vaListOffset);
  builder.CreateStore(llvm::ConstantInt::get(i32Type, gpOffsetValue), vaList);
  builder.CreateStore(llvm::ConstantInt::get(i32Type, 48),
                      emitFramePointer(builder, frame, vaListOffset + 4));
  builder.CreateStore(
      builder.CreatePtrToInt(
          emitFramePointer(builder, frame, saveAreaOffset), i64Type),
      emitFramePointer(builder, frame, vaListOffset + 16));
  return vaList;
}

unsigned countVaStartCalls(llvm::Function &function) {
  unsigned count = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName().starts_with("llvm.va_start")) {
      ++count;
    }
  }
  return count;
}

unsigned countManualVaListConstantStores(llvm::Function &function,
                                         uint64_t vaListOffset) {
  unsigned count = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr ||
        !llvm::isa<llvm::ConstantInt>(store->getValueOperand())) {
      continue;
    }
    // 注意：stripPointerCasts 会把"全零 GEP"折成 alloca 本身（偏移 0 的
    // gp_offset store 就是这样），所以 GEP 解不出来时按 alloca+0 处理。
    llvm::Value *pointer = store->getPointerOperand()->stripPointerCasts();
    uint64_t offset = 0;
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
      auto *index = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
      if (index == nullptr) {
        continue;
      }
      offset = index->getZExtValue();
    } else if (!llvm::isa<llvm::AllocaInst>(pointer)) {
      continue;
    }
    if (offset >= vaListOffset && offset < vaListOffset + 24) {
      ++count;
    }
  }
  return count;
}

struct VarArgTestFixture {
  llvm::Function *Function = nullptr;
  llvm::AllocaInst *Frame = nullptr;
  llvm::GlobalVariable *Rax = nullptr;
  llvm::GlobalVariable *Zmm0 = nullptr;
  llvm::GlobalVariable *Zmm1 = nullptr;
  llvm::Function *UseVaList = nullptr;
};

VarArgTestFixture buildVarArgTestFixture(llvm::Module &module,
                                         const llvm::Twine &name,
                                         llvm::IRBuilder<> &builder,
                                         bool withGpSaves = true) {
  llvm::LLVMContext &context = module.getContext();
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *i512Type = llvm::IntegerType::get(context, 512);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "vararg_sink");
  VarArgTestFixture fixture;
  fixture.Rax = createRegisterGlobal(module, "RAX");
  fixture.Zmm0 = createRegisterGlobal(module, "ZMM0", i512Type, 0, 64);
  fixture.Zmm1 = createRegisterGlobal(module, "ZMM1", i512Type, 0, 64);
  fixture.UseVaList = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::PointerType::get(context, 0)}, false),
      llvm::GlobalValue::ExternalLinkage, "vararg_use_va_list", module);
  fixture.Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64Type}, false),
      llvm::GlobalValue::InternalLinkage, name, module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", fixture.Function);
  builder.SetInsertPoint(entry);
  fixture.Frame = builder.CreateAlloca(
      llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 400), nullptr,
      "frame");
  builder.CreateStore(loadRegister(builder, rdi, "RDI", "fixed"), sink);
  if (withGpSaves) {
    builder.CreateStore(loadRegister(builder, r8, "R8", "saved_r8"),
                        emitFramePointer(builder, fixture.Frame, 240));
    builder.CreateStore(loadRegister(builder, r9, "R9", "saved_r9"),
                        emitFramePointer(builder, fixture.Frame, 248));
  }
  return fixture;
}

llvm::BasicBlock *emitVarArgAlTestAndXmmSave(llvm::IRBuilder<> &builder,
                                             llvm::Module &module,
                                             llvm::AllocaInst *frame,
                                             llvm::GlobalVariable *rax,
                                             llvm::GlobalVariable *zmm0,
                                             llvm::GlobalVariable *zmm1,
                                             const llvm::Twine &doneName) {
  llvm::LLVMContext &context = module.getContext();
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *i128Type = llvm::Type::getInt128Ty(context);
  llvm::Function *function = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *xmmSave =
      llvm::BasicBlock::Create(context, "xmm_save", function);
  llvm::BasicBlock *done =
      llvm::BasicBlock::Create(context, doneName, function);
  llvm::Value *al = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0),
          llvm::Type::getInt8Ty(context), 64, 8),
      {rax, llvm::ConstantInt::get(i64Type, 0)}, "al");
  llvm::Value *alZero = builder.CreateICmpEQ(
      al, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0));
  builder.CreateCondBr(alZero, done, xmmSave);
  builder.SetInsertPoint(xmmSave);
  llvm::Value *savedZmm = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128Type, 512, 128),
      {zmm0, llvm::ConstantInt::get(i64Type, 0)}, "saved_zmm0");
  builder.CreateStore(savedZmm, emitFramePointer(builder, frame, 256));
  llvm::Value *savedZmm1 = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128Type, 512, 128),
      {zmm1, llvm::ConstantInt::get(i64Type, 0)}, "saved_zmm1");
  builder.CreateStore(savedZmm1, emitFramePointer(builder, frame, 272));
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  return done;
}

} // namespace
