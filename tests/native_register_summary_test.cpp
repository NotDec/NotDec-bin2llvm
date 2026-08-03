#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

llvm::MDNode *registerAccessMetadata(llvm::LLVMContext &context,
                                     const std::string &name) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + name),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
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

void attachTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(std::move(input));

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);
  output.Storage.Name = "RDX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(killed);
  killed.Storage.Name = "RDX";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::NativeAbiEffect unaffected;
  unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
  unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  unaffected.Storage.Name = "RBX";
  abi.Effects.push_back(unaffected);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachTestFloatOnlyAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_float_only_test";

  notdec::bin2llvm::NativeAbiParamEntry floatOutput;
  floatOutput.MinSize = 1;
  floatOutput.MaxSize = 8;
  floatOutput.MetaType = "float";
  floatOutput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  floatOutput.Storage.Name = "XMM0_Qa";
  abi.Outputs.push_back(floatOutput);

  notdec::bin2llvm::NativeAbiParamEntry integerOutput;
  integerOutput.MinSize = 1;
  integerOutput.MaxSize = 8;
  integerOutput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  integerOutput.Storage.Name = "RAX";
  abi.Outputs.push_back(integerOutput);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachExternalEvidenceAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_external_evidence_test";
  for (const char *name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name;
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
  killed.Storage.Name = "RDX";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachI386StackEvidenceAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_i386_stack_evidence_test";
  abi.StackPointerRegister = "ESP";
  abi.StackPointerSpace = "register";
  abi.StackShift = 4;

  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 64;
  input.Align = 4;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Stack;
  input.Storage.Space = "stack";
  input.Storage.Offset = 4;
  abi.Inputs.push_back(input);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

notdec::bin2llvm::NativeRegisterCallInputSlot
registerInputSlot(const std::string &name, unsigned offsetBits = 0,
                  unsigned sizeBits = 64) {
  notdec::bin2llvm::NativeRegisterCallInputSlot slot;
  slot.Kind = notdec::bin2llvm::NativeRegisterCallInputSlotKind::Register;
  slot.UnitName = name;
  slot.OffsetBits = offsetBits;
  slot.SizeBits = sizeBits;
  return slot;
}

notdec::bin2llvm::NativeRegisterCallInputSlot stackInputSlot(uint64_t offset) {
  notdec::bin2llvm::NativeRegisterCallInputSlot slot;
  slot.Kind = notdec::bin2llvm::NativeRegisterCallInputSlotKind::Stack;
  slot.StackSpace = "stack";
  slot.StackOffset = offset;
  slot.StackSize = 4;
  slot.StackAlign = 4;
  slot.SizeBits = 32;
  return slot;
}

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name,
                                           llvm::Type *type, uint64_t offset,
                                           uint64_t size) {
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(module.getContext(), "space=register"),
      llvm::MDString::get(module.getContext(),
                          "offset=" + std::to_string(offset)),
      llvm::MDString::get(module.getContext(), "size=" + std::to_string(size)),
      llvm::MDString::get(module.getContext(), "name=" + name),
  };
  global->setMetadata("notdec.register",
                      llvm::MDNode::get(module.getContext(), fields));
  return global;
}

llvm::StoreInst *storeRegister(llvm::IRBuilder<> &builder,
                               llvm::GlobalVariable *reg, uint64_t value,
                               const std::string &name) {
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(reg->getValueType(), value), reg);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(reg->getContext(), name));
  return store;
}

llvm::StoreInst *storeRegisterValue(llvm::IRBuilder<> &builder,
                                    llvm::GlobalVariable *reg,
                                    llvm::Value *value,
                                    const std::string &name) {
  llvm::StoreInst *store = builder.CreateStore(value, reg);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(reg->getContext(), name));
  return store;
}

llvm::LoadInst *loadRegister(llvm::IRBuilder<> &builder,
                             llvm::GlobalVariable *reg, const std::string &name,
                             const std::string &valueName = "") {
  llvm::LoadInst *load =
      builder.CreateLoad(reg->getValueType(), reg, valueName);
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(reg->getContext(), name));
  return load;
}

const notdec::bin2llvm::NativeRegisterSummaryFunction *
functionSummary(const notdec::bin2llvm::NativeRegisterSummary &summary,
                const std::string &name) {
  for (const auto &function : summary.Functions) {
    if (function.FunctionName == name) {
      return &function;
    }
  }
  return nullptr;
}

const notdec::bin2llvm::NativeRegisterSummaryRegister *
registerSummary(const notdec::bin2llvm::NativeRegisterSummaryFunction &function,
                const std::string &name) {
  for (const auto &reg : function.Registers) {
    if (reg.Name == name) {
      return &reg;
    }
  }
  return nullptr;
}

const notdec::bin2llvm::NativeRegisterExternalCallsite *
unknownExternalCallsite(const notdec::bin2llvm::NativeRegisterSummary &summary,
                        const std::string &calleeName) {
  for (const auto &callsite : summary.ExternalCallsites) {
    if (callsite.Kind == notdec::bin2llvm::NativeRegisterExternalCallsiteKind::
                             UnknownExternal &&
        callsite.CalleeName == calleeName) {
      return &callsite;
    }
  }
  return nullptr;
}

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool testKilledReadDoesNotBecomeInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-killed-read", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "killed_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, 7, "RDI");
  (void)loadRegister(builder, rdi, "RDI", "after_kill");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "killed_read");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");
  return expect(rdiSummary != nullptr, "missing RDI summary") &&
         expect(!rdiSummary->ReadEntry,
                "killed RDI read was incorrectly marked readEntry") &&
         expect(rdiSummary->MayNonEntry, "RDI write was not marked modified");
}

bool testPartialWriteDoesNotReadEntryByItself() {
  llvm::LLVMContext context;
  llvm::Module module("summary-partial-write", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "partial_write_rax", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "partial_write_rax");
  const auto *raxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RAX");
  return expect(raxSummary != nullptr, "missing partial write RAX summary") &&
         expect(!raxSummary->ReadEntry,
                "partial write helper was incorrectly marked readEntry") &&
         expect(raxSummary->MayEntry,
                "partial write did not preserve untouched RAX bits") &&
         expect(raxSummary->MayNonEntry,
                "partial write was not marked as modifying RAX");
}

bool testPartialReadReadsEntry() {
  llvm::LLVMContext context;
  llvm::Module module("summary-partial-read", context);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "partial_read_rax", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *value = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "partial_read_rax");
  const auto *raxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RAX");
  std::string entryMask =
      raxSummary == nullptr ? "" : raxSummary->EntryDemandMaskHex;
  std::transform(
      entryMask.begin(), entryMask.end(), entryMask.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return expect(raxSummary != nullptr, "missing partial read RAX summary") &&
         expect(raxSummary->ReadEntry,
                "partial read helper was not marked readEntry") &&
         expect(entryMask == "ffffffff",
                "partial read did not keep low 32-bit demand mask") &&
         expect(raxSummary->MayEntry,
                "partial read lost entry value preservation") &&
         expect(!raxSummary->MayNonEntry,
                "partial read was incorrectly marked as modifying RAX");
}

bool testKnownVarArgExternalKeepsFullInputDemand() {
  llvm::LLVMContext context;
  llvm::Module module("summary-vararg-full-input-demand", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_vararg_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (const char *name : {"RDI", "RSI", "RDX", "RCX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name;
    abi.Inputs.push_back(input);
  }
  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rcx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  auto *openType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *open = llvm::Function::Create(
      openType, llvm::GlobalValue::ExternalLinkage, "open", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_rcx_before_open", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *low = builder.CreateCall(
      partialRead,
      {rcx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateStore(low, sink);
  builder.CreateCall(openType, open);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "partial_rcx_before_open");
  const auto *rcxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RCX");
  std::string entryMask =
      rcxSummary == nullptr ? "" : rcxSummary->EntryDemandMaskHex;
  std::transform(
      entryMask.begin(), entryMask.end(), entryMask.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return expect(rcxSummary != nullptr, "missing vararg RCX summary") &&
         expect(rcxSummary->ReadEntry,
                "known vararg call did not read entry RCX") &&
         expect(entryMask == "ffffffffffffffff",
                "known vararg call did not keep full RCX demand mask");
}

bool testUnknownExternalNoInputsDoesNotReadAbiArguments() {
  llvm::LLVMContext context;
  llvm::Module module("summary-unknown-external-no-inputs", context);
  attachExternalEvidenceAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  auto *callType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);
  llvm::Function *function = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "calls_unknown", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callType, external);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.UnknownExternalInputPolicy =
      notdec::bin2llvm::NativeRegisterUnknownExternalInputPolicy::NoInputs;
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *fn = functionSummary(summary, "calls_unknown");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, rdi->getName().str());
  const auto *rsiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, rsi->getName().str());
  return expect(rdiSummary != nullptr, "missing no-input RDI summary") &&
         expect(rsiSummary != nullptr, "missing no-input RSI summary") &&
         expect(!rdiSummary->ReadEntry,
                "unknown no-input call read entry RDI") &&
         expect(!rsiSummary->ReadEntry, "unknown no-input call read entry RSI");
}

bool testKnownExternalShapeReadsOnlyDeclaredRegister() {
  llvm::LLVMContext context;
  llvm::Module module("summary-known-external-shape", context);
  attachExternalEvidenceAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  auto *callType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "known_external", module);
  llvm::Function *function = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "calls_known", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callType, external);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  notdec::bin2llvm::NativeExternalCallShape shape;
  shape.Inputs.push_back(registerInputSlot("RSI"));
  options.ExternalCallShapes.emplace("known_external", std::move(shape));
  options.UnknownExternalInputPolicy =
      notdec::bin2llvm::NativeRegisterUnknownExternalInputPolicy::NoInputs;
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *fn = functionSummary(summary, "calls_known");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, rdi->getName().str());
  const auto *rsiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, rsi->getName().str());
  return expect(rdiSummary != nullptr, "missing known-shape RDI summary") &&
         expect(rsiSummary != nullptr, "missing known-shape RSI summary") &&
         expect(!rdiSummary->ReadEntry,
                "known external shape read undeclared RDI") &&
         expect(rsiSummary->ReadEntry,
                "known external shape did not read declared RSI");
}

bool testKnownVarArgUsesFixedThenCallsiteInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-known-vararg-callsite-shape", context);
  attachExternalEvidenceAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *callType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "known_vararg", module);
  llvm::Function *function =
      llvm::Function::Create(callType, llvm::GlobalValue::ExternalLinkage,
                             "calls_known_vararg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::CallInst *call = builder.CreateCall(callType, external);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeExternalCallShape fixedShape;
  fixedShape.Inputs.push_back(registerInputSlot("RDI"));
  fixedShape.VarArgInputs.push_back(registerInputSlot("RSI"));
  fixedShape.VarArgInputs.push_back(registerInputSlot("RDX"));
  fixedShape.FixedArgs = 1;
  fixedShape.VarArg = true;

  notdec::bin2llvm::NativeRegisterSummaryOptions baseOptions;
  baseOptions.AttachMetadata = false;
  baseOptions.ExternalCallShapes.emplace("known_vararg", fixedShape);
  baseOptions.UnknownExternalInputPolicy =
      notdec::bin2llvm::NativeRegisterUnknownExternalInputPolicy::NoInputs;
  baseOptions.CollectExternalCallsiteEvidence = true;
  auto baseSummary =
      notdec::bin2llvm::runNativeRegisterSummary(module, baseOptions);
  const auto *baseFn = functionSummary(baseSummary, "calls_known_vararg");
  const auto *baseRdi = baseFn == nullptr
                            ? nullptr
                            : registerSummary(*baseFn, rdi->getName().str());
  const auto *baseRsi = baseFn == nullptr
                            ? nullptr
                            : registerSummary(*baseFn, rsi->getName().str());

  const notdec::bin2llvm::NativeRegisterExternalCallsite *evidence = nullptr;
  for (const auto &candidate : baseSummary.ExternalCallsites) {
    if (candidate.Call == call &&
        candidate.Kind ==
            notdec::bin2llvm::NativeRegisterExternalCallsiteKind::KnownVarArg) {
      evidence = &candidate;
    }
  }
  if (!expect(baseRdi != nullptr && baseRdi->ReadEntry,
              "known vararg base summary did not read fixed RDI") ||
      !expect(baseRsi != nullptr && !baseRsi->ReadEntry,
              "known vararg base summary read RSI tail") ||
      !expect(evidence != nullptr && evidence->Slots.size() == 2,
              "known vararg base summary did not collect tail evidence")) {
    return false;
  }

  notdec::bin2llvm::NativeExternalCallShape callsiteShape = fixedShape;
  callsiteShape.Inputs.push_back(registerInputSlot("RSI"));
  notdec::bin2llvm::NativeRegisterSummaryOptions finalOptions = baseOptions;
  finalOptions.CollectExternalCallsiteEvidence = false;
  finalOptions.ExternalCallsiteShapes.emplace(call, std::move(callsiteShape));
  auto finalSummary =
      notdec::bin2llvm::runNativeRegisterSummary(module, finalOptions);
  const auto *finalFn = functionSummary(finalSummary, "calls_known_vararg");
  const auto *finalRsi = finalFn == nullptr
                             ? nullptr
                             : registerSummary(*finalFn, rsi->getName().str());
  const auto *finalRdx = finalFn == nullptr
                             ? nullptr
                             : registerSummary(*finalFn, rdx->getName().str());
  return expect(finalRsi != nullptr && finalRsi->ReadEntry,
                "known vararg final summary did not read inferred RSI") &&
         expect(finalRdx != nullptr && !finalRdx->ReadEntry,
                "known vararg final summary read uninferred RDX");
}

bool testUnknownExternalCallsiteEvidenceClassifiesOrigins() {
  llvm::LLVMContext context;
  llvm::Module module("summary-unknown-external-evidence", context);
  attachExternalEvidenceAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  (void)createRegisterGlobal(module, "RDX");

  auto *callType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *producer = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "unknown_producer", module);
  llvm::Function *target = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "unknown_target", module);
  llvm::Function *function = llvm::Function::Create(
      callType, llvm::GlobalValue::ExternalLinkage, "evidence_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callType, producer);
  llvm::LoadInst *produced = loadRegister(builder, rax, "RAX", "produced");
  storeRegisterValue(builder, rsi, produced, "RSI");
  storeRegister(builder, rdi, 7, "RDI");
  builder.CreateCall(callType, target);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.UnknownExternalInputPolicy =
      notdec::bin2llvm::NativeRegisterUnknownExternalInputPolicy::NoInputs;
  options.CollectExternalCallsiteEvidence = true;
  options.ExternalEvidenceSlots = {
      registerInputSlot("RDI"),
      registerInputSlot("RSI"),
      registerInputSlot("RDX"),
  };
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *callsite = unknownExternalCallsite(summary, "unknown_target");
  if (!expect(callsite != nullptr, "missing unknown target evidence") ||
      !expect(callsite->Slots.size() == 3,
              "unknown target evidence slot count mismatch")) {
    return false;
  }
  using Origin = notdec::bin2llvm::NativeRegisterCallsiteValueOrigin;
  return expect(callsite->Slots[0].Origin == Origin::LocalDefinition,
                "RDI evidence was not classified as local") &&
         expect(callsite->Slots[1].Origin == Origin::LocalDefinition,
                "RSI explicit write from a call value was not local") &&
         expect(callsite->Slots[2].Origin == Origin::CallClobber,
                "RDX evidence was not classified as call clobber");
}

bool testI386AlignedStackCallsiteEvidenceUsesCurrentEsp() {
  llvm::LLVMContext context;
  llvm::Module module("summary-i386-aligned-stack-evidence", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackEvidenceAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);

  auto *callType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external =
      llvm::Function::Create(callType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_stack_target", module);
  llvm::Function *function =
      llvm::Function::Create(callType, llvm::GlobalValue::ExternalLinkage,
                             "aligned_stack_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryEsp = loadRegister(builder, esp, "ESP", "entry.esp");
  llvm::Value *alignedEsp = builder.CreateAnd(
      entryEsp, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegisterValue(builder, esp, alignedEsp, "ESP");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(alignedEsp, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 55), argPointer);
  builder.CreateCall(callType, external);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.AttachMetadata = false;
  options.IgnoredRegisters.insert("ESP");
  options.UnknownExternalInputPolicy =
      notdec::bin2llvm::NativeRegisterUnknownExternalInputPolicy::NoInputs;
  options.CollectExternalCallsiteEvidence = true;
  options.ExternalEvidenceSlots = {stackInputSlot(4)};
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *callsite =
      unknownExternalCallsite(summary, "unknown_stack_target");
  using Origin = notdec::bin2llvm::NativeRegisterCallsiteValueOrigin;
  return expect(callsite != nullptr,
                "missing aligned i386 stack callsite evidence") &&
         expect(callsite->Slots.size() == 1,
                "aligned i386 stack evidence slot count mismatch") &&
         expect(callsite->Slots[0].Origin == Origin::LocalDefinition,
                "aligned i386 stack store was not matched at current ESP");
}

bool testCalleeReadPropagatesToCallerEntry() {
  llvm::LLVMContext context;
  llvm::Module module("summary-call-read", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "reads_rdi", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *arg = loadRegister(calleeBuilder, rdi, "RDI", "arg");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");
  calleeBuilder.CreateStore(arg, sink);
  calleeBuilder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "caller_entry_forwards_rdi", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(type, callee);
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "caller_entry_forwards_rdi");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");
  return expect(rdiSummary != nullptr, "missing caller RDI summary") &&
         expect(rdiSummary->ReadEntry,
                "callee readEntry did not propagate to caller entry RDI");
}

bool testSparseJoinKeepsUntouchedPath() {
  llvm::LLVMContext context;
  llvm::Module module("summary-sparse-join", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "branch_write_then_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *writeBlock =
      llvm::BasicBlock::Create(context, "write", function);
  llvm::BasicBlock *keepBlock =
      llvm::BasicBlock::Create(context, "keep", function);
  llvm::BasicBlock *joinBlock =
      llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), writeBlock,
                       keepBlock);
  builder.SetInsertPoint(writeBlock);
  storeRegister(builder, rdi, 7, "RDI");
  builder.CreateBr(joinBlock);
  builder.SetInsertPoint(keepBlock);
  builder.CreateBr(joinBlock);
  builder.SetInsertPoint(joinBlock);
  (void)loadRegister(builder, rdi, "RDI", "maybe_entry");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "branch_write_then_read");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");
  return expect(rdiSummary != nullptr, "missing branch RDI summary") &&
         expect(rdiSummary->ReadEntry,
                "sparse join lost untouched path before read") &&
         expect(rdiSummary->MayEntry,
                "sparse join lost mayEntry from untouched path") &&
         expect(rdiSummary->MayNonEntry,
                "sparse join lost mayNonEntry from write path");
}

bool testTopDownDemandKeepsOnlyUsedReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-demanded-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "writes_rax_rdx", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  storeRegister(calleeBuilder, rax, 1, "RAX");
  storeRegister(calleeBuilder, rdx, 2, "RDX");
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "uses_only_rax", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(callerBuilder, rax, "RAX", "ret_rax");
  callerBuilder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "writes_rax_rdx");
  const auto *raxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RAX");
  const auto *rdxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDX");
  return expect(raxSummary != nullptr, "missing callee RAX summary") &&
         expect(rdxSummary != nullptr, "missing callee RDX summary") &&
         expect(raxSummary->MayNonEntry, "RAX write was not marked modified") &&
         expect(rdxSummary->MayNonEntry, "RDX write was not marked modified") &&
         expect(raxSummary->ExitDemand,
                "used RAX return was not marked demanded") &&
         expect(!rdxSummary->ExitDemand,
                "unused RDX return was incorrectly marked demanded");
}

bool testRootDemandSkipsFloatOnlyAbiOutput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-root-demand-skips-float-output", context);
  attachTestFloatOnlyAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *zmm0 = createRegisterGlobal(
      module, "ZMM0", llvm::IntegerType::get(context, 512), 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "root_writes_rax_zmm0", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, 1, "RAX");
  builder.CreateStore(llvm::ConstantInt::get(zmm0->getValueType(), 2), zmm0);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "root_writes_rax_zmm0");
  const auto *raxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RAX");
  const auto *zmmSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "ZMM0");
  return expect(raxSummary != nullptr, "missing root RAX summary") &&
         expect(zmmSummary != nullptr, "missing root ZMM0 summary") &&
         expect(raxSummary->ExitDemand,
                "root integer ABI return was not demanded") &&
         expect(!zmmSummary->ExitDemand,
                "root float ABI output was incorrectly demanded");
}

bool testSavedRegisterRestoreIsPreserved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-saved-register-restore", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "save_clobber_restore_rbx", module);
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
  storeRegister(builder, rbx, 42, "RBX");
  llvm::LoadInst *saved = builder.CreateLoad(rbx->getValueType(), slot);
  storeRegisterValue(builder, rbx, saved, "RBX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "save_clobber_restore_rbx");
  const auto *rbxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RBX");
  return expect(rbxSummary != nullptr, "missing RBX summary") &&
         expect(rbxSummary->MayEntry, "restored RBX lost entry value") &&
         expect(!rbxSummary->MayNonEntry,
                "restored RBX was still marked modified");
}

bool testOverwrittenSavedRegisterSlotIsNotPreserved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-saved-register-overwritten", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "overwrite_saved_rbx_slot", module);
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
  builder.CreateStore(llvm::ConstantInt::get(rbx->getValueType(), 7), slot);
  storeRegister(builder, rbx, 42, "RBX");
  llvm::LoadInst *saved = builder.CreateLoad(rbx->getValueType(), slot);
  storeRegisterValue(builder, rbx, saved, "RBX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "overwrite_saved_rbx_slot");
  const auto *rbxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RBX");
  return expect(rbxSummary != nullptr, "missing overwritten RBX summary") &&
         expect(rbxSummary->MayNonEntry,
                "overwritten saved RBX slot was incorrectly preserved");
}

bool testImplicitCalleeSavedRestoreIsPreserved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-implicit-callee-saved-restore", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "implicit_restore_rbx", module);
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
  storeRegister(builder, rbx, 42, "RBX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "implicit_restore_rbx");
  const auto *rbxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RBX");
  return expect(rbxSummary != nullptr, "missing implicit RBX summary") &&
         expect(rbxSummary->MayEntry,
                "implicit restored RBX lost entry value") &&
         expect(!rbxSummary->MayNonEntry,
                "implicit restored RBX was still marked modified");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testKilledReadDoesNotBecomeInput();
  ok &= testPartialWriteDoesNotReadEntryByItself();
  ok &= testPartialReadReadsEntry();
  ok &= testKnownVarArgExternalKeepsFullInputDemand();
  ok &= testUnknownExternalNoInputsDoesNotReadAbiArguments();
  ok &= testKnownExternalShapeReadsOnlyDeclaredRegister();
  ok &= testKnownVarArgUsesFixedThenCallsiteInputs();
  ok &= testUnknownExternalCallsiteEvidenceClassifiesOrigins();
  ok &= testI386AlignedStackCallsiteEvidenceUsesCurrentEsp();
  ok &= testCalleeReadPropagatesToCallerEntry();
  ok &= testSparseJoinKeepsUntouchedPath();
  ok &= testTopDownDemandKeepsOnlyUsedReturn();
  ok &= testRootDemandSkipsFloatOnlyAbiOutput();
  ok &= testSavedRegisterRestoreIsPreserved();
  ok &= testOverwrittenSavedRegisterSlotIsNotPreserved();
  ok &= testImplicitCalleeSavedRestoreIsPreserved();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
