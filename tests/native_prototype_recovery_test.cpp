#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name) {
  llvm::LLVMContext &context = module.getContext();
  auto *type = llvm::Type::getInt64Ty(context);
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=" + name),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

notdec::bin2llvm::NativeAbiParamEntry inputRegister(const std::string &name) {
  notdec::bin2llvm::NativeAbiParamEntry entry;
  entry.MinSize = 1;
  entry.MaxSize = 8;
  entry.Align = 8;
  entry.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  entry.Storage.Name = name;
  return entry;
}

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

void attachTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__stdcall";
  abi.Inputs.push_back(inputRegister("RDI"));
  abi.Inputs.push_back(inputRegister("RSI"));

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Align = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(std::move(output));

  notdec::bin2llvm::NativeAbiParamEntry secondOutput;
  secondOutput.MinSize = 1;
  secondOutput.MaxSize = 8;
  secondOutput.Align = 8;
  secondOutput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  secondOutput.Storage.Name = "RDX";
  abi.Outputs.push_back(std::move(secondOutput));

  notdec::bin2llvm::NativeAbiEffect unaffected;
  unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
  unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  unaffected.Storage.Name = "RBX";
  abi.Effects.push_back(std::move(unaffected));

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

llvm::Function *createFunction(llvm::Module &module, const std::string &name) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createUnusedExternalInputFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load =
      builder.CreateLoad(global->getValueType(), global,
                         registerName + ".external_input");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + registerName),
      llvm::ValueAsMetadata::get(global),
  };
  load->setMetadata("notdec.register.external_input",
                    llvm::MDNode::get(context, fields));
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createReturnStoreFunction(llvm::Module &module,
                                          const std::string &name,
                                          llvm::GlobalVariable *global,
                                          const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x1234), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createTwoReturnStoreFunction(llvm::Module &module,
                                             const std::string &name,
                                             llvm::GlobalVariable *global,
                                             const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x1111), global);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x1111), global);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createPartialReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *withReturn =
      llvm::BasicBlock::Create(context, "with_return", function);
  llvm::BasicBlock *withoutReturn =
      llvm::BasicBlock::Create(context, "without_return", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), withReturn,
                       withoutReturn);

  builder.SetInsertPoint(withReturn);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x3333), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(withoutReturn);
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createUniquePredecessorReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x4444), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateBr(exit);

  builder.SetInsertPoint(exit);
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createConflictingReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x5555), global);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x6666), global);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createTwoOutputReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *first,
    const std::string &firstRegisterName, llvm::GlobalVariable *second,
    const std::string &secondRegisterName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::StoreInst *firstStore = builder.CreateStore(
      llvm::ConstantInt::get(first->getValueType(), 0x7777), first);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstRegisterName));

  llvm::StoreInst *secondStore = builder.CreateStore(
      llvm::ConstantInt::get(second->getValueType(), 0x8888), second);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondRegisterName));
  builder.CreateRetVoid();
  return function;
}

void attachExternalInputs(llvm::Function &function,
                          llvm::ArrayRef<std::pair<std::string,
                                                   llvm::GlobalVariable *>>
                              inputs) {
  llvm::LLVMContext &context = function.getContext();
  std::vector<llvm::Metadata *> entries;
  for (const auto &[name, global] : inputs) {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "name=" + name),
        llvm::ValueAsMetadata::get(global),
    };
    entries.push_back(llvm::MDNode::get(context, fields));
  }
  function.setMetadata("notdec.register.external_inputs",
                       llvm::MDNode::get(context, entries));
}

bool metadataHasRegister(const llvm::Function &function, llvm::StringRef kind,
                         llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr) {
    return false;
  }
  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == prefix) {
        return true;
      }
    }
  }
  return false;
}

bool metadataRegisterAt(const llvm::Function &function, llvm::StringRef kind,
                        uint64_t index, llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr || index >= node->getNumOperands()) {
    return false;
  }
  auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(index));
  if (entry == nullptr) {
    return false;
  }
  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &fieldOperand : entry->operands()) {
    auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
    if (field != nullptr && field->getString() == prefix) {
      return true;
    }
  }
  return false;
}

uint64_t countMetadataRegister(const llvm::Function &function,
                               llvm::StringRef kind, llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr) {
    return 0;
  }
  uint64_t count = 0;
  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == prefix) {
        ++count;
      }
    }
  }
  return count;
}

bool recoveredHasField(const llvm::Function &function, llvm::StringRef field) {
  llvm::MDNode *node = function.getMetadata("notdec.prototype.recovered");
  if (node == nullptr) {
    return false;
  }
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *metadata = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (metadata != nullptr && metadata->getString() == field) {
      return true;
    }
  }
  return false;
}

bool recoveredRegisterAt(const llvm::Function &function, uint64_t listIndex,
                         uint64_t paramIndex, llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata("notdec.prototype.recovered");
  if (node == nullptr || listIndex >= node->getNumOperands()) {
    return false;
  }
  auto *list = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(listIndex));
  if (list == nullptr || paramIndex >= list->getNumOperands()) {
    return false;
  }
  auto *param = llvm::dyn_cast_or_null<llvm::MDNode>(list->getOperand(paramIndex));
  if (param == nullptr) {
    return false;
  }

  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &fieldOperand : param->operands()) {
    auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
    if (field != nullptr && field->getString() == prefix) {
      return true;
    }
  }
  return false;
}

bool recoveredPrototypeParamAt(
    const std::vector<notdec::bin2llvm::NativeRecoveredPrototypeParam> &params,
    uint64_t index, llvm::StringRef name) {
  return index < params.size() && params[index].RegisterName == name;
}

bool functionTypeShape(llvm::FunctionType &type, llvm::Type *returnType,
                       llvm::ArrayRef<llvm::Type *> params) {
  if (type.getReturnType() != returnType || type.getNumParams() != params.size()) {
    return false;
  }
  for (unsigned index = 0; index < params.size(); ++index) {
    if (type.getParamType(index) != params[index]) {
      return false;
    }
  }
  return true;
}

bool expect(bool condition, const std::string &message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

} // namespace

int main() {
  llvm::LLVMContext context;
  llvm::Module module("native-prototype-recovery-test", context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  attachTestAbi(module);

  llvm::Function *inputFunction = createFunction(module, "input_rdi");
  attachExternalInputs(*inputFunction, {{"RDI", rdi}});

  llvm::Function *reversedInputFunction =
      createFunction(module, "input_reversed");
  attachExternalInputs(*reversedInputFunction, {{"RSI", rdi}, {"RDI", rdi}});

  llvm::Function *duplicateInputFunction =
      createFunction(module, "input_duplicate");
  attachExternalInputs(*duplicateInputFunction, {{"RDI", rdi}, {"RDI", rdi}});

  llvm::Function *unusedInputFunction =
      createUnusedExternalInputFunction(module, "unused_rdi", rdi, "RDI");
  attachExternalInputs(*unusedInputFunction, {{"RDI", rdi}});

  llvm::Function *savedRegisterFunction = createFunction(module, "saved_rbx");
  attachExternalInputs(*savedRegisterFunction, {{"RBX", rbx}});
  llvm::Function *returnFunction =
      createReturnStoreFunction(module, "return_rax", rax, "RAX");
  llvm::Function *nonReturnFunction =
      createReturnStoreFunction(module, "return_rbx", rbx, "RBX");
  llvm::Function *twoReturnFunction =
      createTwoReturnStoreFunction(module, "return_rax_twice", rax, "RAX");
  llvm::Function *partialReturnFunction =
      createPartialReturnStoreFunction(module, "return_rax_partial", rax,
                                       "RAX");
  llvm::Function *uniquePredReturnFunction =
      createUniquePredecessorReturnStoreFunction(module,
                                                 "return_rax_unique_pred", rax,
                                                 "RAX");
  llvm::Function *conflictingReturnFunction =
      createConflictingReturnStoreFunction(module, "return_rax_conflict", rax,
                                           "RAX");
  llvm::Function *twoOutputReturnFunction =
      createTwoOutputReturnStoreFunction(module, "return_rdx_rax_order", rdx,
                                         "RDX", rax, "RAX");

  notdec::bin2llvm::NativePrototypeRecoveryOptions options;
  notdec::bin2llvm::NativePrototypeRecoverySummary summary =
      notdec::bin2llvm::runNativePrototypeRecovery(module, options);

  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after prototype recovery\n";
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok &= expect(summary.FunctionsSeen == 12, "unexpected function count");
  ok &= expect(summary.ExternalInputsSeen == 7,
               "unexpected external input count");
  ok &= expect(summary.InputCandidates == 4,
               "unexpected input candidate count");
  ok &= expect(summary.ReturnCandidates == 5,
               "unexpected return candidate count");
  ok &= expect(metadataHasRegister(*inputFunction,
                                   "notdec.prototype.input_candidates", "RDI"),
               "RDI was not marked as an input candidate");
  ok &= expect(metadataRegisterAt(*reversedInputFunction,
                                  "notdec.prototype.input_candidates", 0,
                                  "RDI"),
               "RDI input candidate was not sorted before RSI");
  ok &= expect(metadataRegisterAt(*reversedInputFunction,
                                  "notdec.prototype.input_candidates", 1,
                                  "RSI"),
               "RSI input candidate was not sorted after RDI");
  ok &= expect(countMetadataRegister(*duplicateInputFunction,
                                     "notdec.prototype.input_candidates",
                                     "RDI") == 1,
               "duplicate RDI input candidate was not deduplicated");
  ok &= expect(!metadataHasRegister(*unusedInputFunction,
                                    "notdec.prototype.input_candidates", "RDI"),
               "unused RDI was incorrectly marked as an input candidate");
  ok &= expect(!metadataHasRegister(*savedRegisterFunction,
                                    "notdec.prototype.input_candidates", "RBX"),
               "RBX was incorrectly marked as an input candidate");
  ok &= expect(metadataHasRegister(*returnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "RAX was not marked as a return candidate");
  ok &= expect(!metadataHasRegister(*nonReturnFunction,
                                    "notdec.prototype.return_candidates", "RBX"),
               "RBX was incorrectly marked as a return candidate");
  ok &= expect(countMetadataRegister(*twoReturnFunction,
                                     "notdec.prototype.return_candidates",
                                     "RAX") == 1,
               "RAX return candidate was not deduplicated");
  ok &= expect(!metadataHasRegister(*partialReturnFunction,
                                    "notdec.prototype.return_candidates",
                                    "RAX"),
               "partial RAX return was incorrectly marked as a candidate");
  ok &= expect(metadataHasRegister(*uniquePredReturnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "unique predecessor RAX return was not marked as a candidate");
  ok &= expect(!metadataHasRegister(*conflictingReturnFunction,
                                    "notdec.prototype.return_candidates",
                                    "RAX"),
               "conflicting RAX return was incorrectly marked as a candidate");
  ok &= expect(metadataRegisterAt(*twoOutputReturnFunction,
                                  "notdec.prototype.return_candidates", 0,
                                  "RAX"),
               "RAX return candidate was not sorted before RDX");
  ok &= expect(metadataRegisterAt(*twoOutputReturnFunction,
                                  "notdec.prototype.return_candidates", 1,
                                  "RDX"),
               "RDX return candidate was not sorted after RAX");
  ok &= expect(recoveredHasField(*inputFunction, "model=__stdcall"),
               "recovered prototype model was not written");
  ok &= expect(recoveredHasField(*inputFunction, "input_count=1"),
               "recovered prototype input count was not written");
  ok &= expect(recoveredHasField(*inputFunction, "return_count=0"),
               "recovered prototype return count was not written");
  ok &= expect(recoveredRegisterAt(*inputFunction, 3, 0, "RDI"),
               "recovered prototype input register was not written");
  std::optional<notdec::bin2llvm::NativeRecoveredPrototype> inputPrototype =
      notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(*inputFunction);
  ok &= expect(inputPrototype.has_value(),
               "recovered input prototype was not readable");
  if (inputPrototype) {
    ok &= expect(inputPrototype->ModelName == "__stdcall",
                 "recovered input prototype model was not read");
    ok &= expect(inputPrototype->Inputs.size() == 1,
                 "recovered input prototype input count was not read");
    ok &= expect(inputPrototype->Returns.empty(),
                 "recovered input prototype return count was not read");
    ok &= expect(recoveredPrototypeParamAt(inputPrototype->Inputs, 0, "RDI"),
                 "recovered input prototype register was not read");
    std::optional<llvm::FunctionType *> type =
        notdec::bin2llvm::buildNativeRecoveredPrototypeFunctionType(
            context, *inputPrototype);
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *voidType = llvm::Type::getVoidTy(context);
    ok &= expect(type.has_value() &&
                     functionTypeShape(**type, voidType, llvm::ArrayRef(i64)),
                 "recovered input prototype type was not void(i64)");
  }
  ok &= expect(recoveredHasField(*returnFunction, "input_count=0"),
               "return-only recovered prototype input count was not written");
  ok &= expect(recoveredHasField(*returnFunction, "return_count=1"),
               "return-only recovered prototype return count was not written");
  ok &= expect(recoveredRegisterAt(*returnFunction, 4, 0, "RAX"),
               "recovered prototype return register was not written");
  std::optional<notdec::bin2llvm::NativeRecoveredPrototype> returnPrototype =
      notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(*returnFunction);
  ok &= expect(returnPrototype.has_value(),
               "recovered return prototype was not readable");
  if (returnPrototype) {
    ok &= expect(returnPrototype->Inputs.empty(),
                 "recovered return prototype input count was not read");
    ok &= expect(returnPrototype->Returns.size() == 1,
                 "recovered return prototype return count was not read");
    ok &= expect(recoveredPrototypeParamAt(returnPrototype->Returns, 0, "RAX"),
                 "recovered return prototype register was not read");
    std::optional<llvm::FunctionType *> type =
        notdec::bin2llvm::buildNativeRecoveredPrototypeFunctionType(
            context, *returnPrototype);
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    ok &= expect(type.has_value() &&
                     functionTypeShape(**type, i64, llvm::ArrayRef<llvm::Type *>{}),
                 "recovered return prototype type was not i64()");
  }
  ok &= expect(recoveredRegisterAt(*twoOutputReturnFunction, 4, 0, "RAX"),
               "recovered RAX return was not sorted before RDX");
  ok &= expect(recoveredRegisterAt(*twoOutputReturnFunction, 4, 1, "RDX"),
               "recovered RDX return was not sorted after RAX");
  std::optional<notdec::bin2llvm::NativeRecoveredPrototype> twoOutputPrototype =
      notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
          *twoOutputReturnFunction);
  ok &= expect(twoOutputPrototype.has_value(),
               "recovered two-output prototype was not readable");
  if (twoOutputPrototype) {
    ok &= expect(twoOutputPrototype->Returns.size() == 2,
                 "recovered two-output prototype return count was not read");
    ok &= expect(
        recoveredPrototypeParamAt(twoOutputPrototype->Returns, 0, "RAX"),
        "recovered RAX return was not read before RDX");
    ok &= expect(
        recoveredPrototypeParamAt(twoOutputPrototype->Returns, 1, "RDX"),
        "recovered RDX return was not read after RAX");
    ok &= expect(!notdec::bin2llvm::buildNativeRecoveredPrototypeFunctionType(
                     context, *twoOutputPrototype),
                 "multi-return recovered prototype type was incorrectly built");
  }
  ok &= expect(unusedInputFunction->getMetadata("notdec.prototype.recovered") ==
                   nullptr,
               "empty recovered prototype metadata was not cleared");
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *unusedInputFunction),
               "empty recovered prototype metadata was incorrectly read");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
