#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativePrototypeModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

std::optional<std::string> metadataField(const llvm::MDNode &node,
                                         llvm::StringRef key) {
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node.operands()) {
    auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (field == nullptr || !field->getString().starts_with(prefix)) {
      continue;
    }
    return field->getString().substr(prefix.size()).str();
  }
  return std::nullopt;
}

std::optional<uint64_t> parseUint64Field(const llvm::MDNode &node,
                                         llvm::StringRef key);

llvm::MDNode *inputCandidateMetadata(llvm::LLVMContext &context,
                                     const NativeParamActive &active) {
  if (active.Trials.empty()) {
    return nullptr;
  }

  std::vector<llvm::Metadata *> entries;
  for (const NativeParamTrial &trial : active.Trials) {
    std::vector<llvm::Metadata *> fields;
    if (trial.StorageKind == "stack") {
      fields = {
          llvm::MDString::get(context, "storage=stack"),
          llvm::MDString::get(context, "space=" + trial.StackSpace),
          llvm::MDString::get(context,
                              "offset=" + std::to_string(trial.StackOffset)),
          llvm::MDString::get(context, "size=" + std::to_string(trial.Size)),
          llvm::MDString::get(context, "slot=" + std::to_string(trial.Slot)),
      };
    } else {
      fields = {
          llvm::MDString::get(context, "storage=register"),
          llvm::MDString::get(context, "name=" + trial.RegisterName),
          llvm::MDString::get(context, "size=" + std::to_string(trial.Size)),
          llvm::MDString::get(context, "slot=" + std::to_string(trial.Slot)),
      };
    }
    entries.push_back(llvm::MDNode::get(context, fields));
  }
  return llvm::MDNode::get(context, entries);
}

std::vector<NativeRecoveredPrototypeParam> recoveredParams(
    const NativeParamActive &active) {
  std::vector<NativeRecoveredPrototypeParam> params;
  for (const NativeParamTrial &trial : active.Trials) {
    NativeRecoveredPrototypeParam param;
    param.RegisterName = trial.RegisterName;
    param.StorageKind = trial.StorageKind;
    param.StackSpace = trial.StackSpace;
    param.StackOffset = trial.StackOffset;
    param.Size = trial.Size;
    param.Slot = trial.Slot;
    params.push_back(std::move(param));
  }
  return params;
}

llvm::MDNode *recoveredParamListMetadata(
    llvm::LLVMContext &context,
    const std::vector<NativeRecoveredPrototypeParam> &params) {
  std::vector<llvm::Metadata *> entries;
  for (const NativeRecoveredPrototypeParam &param : params) {
    std::vector<llvm::Metadata *> fields;
    if (param.StorageKind == "stack") {
      fields = {
          llvm::MDString::get(context, "storage=stack"),
          llvm::MDString::get(context, "space=" + param.StackSpace),
          llvm::MDString::get(context,
                              "offset=" + std::to_string(param.StackOffset)),
          llvm::MDString::get(context, "size=" + std::to_string(param.Size)),
          llvm::MDString::get(context, "slot=" + std::to_string(param.Slot)),
      };
    } else {
      fields = {
          llvm::MDString::get(context, "storage=register"),
          llvm::MDString::get(context, "name=" + param.RegisterName),
          llvm::MDString::get(context, "size=" + std::to_string(param.Size)),
          llvm::MDString::get(context, "slot=" + std::to_string(param.Slot)),
      };
    }
    entries.push_back(llvm::MDNode::get(context, fields));
  }
  return llvm::MDNode::get(context, entries);
}

llvm::MDNode *recoveredPrototypeMetadata(
    llvm::LLVMContext &context, const NativeRecoveredPrototype &prototype) {
  std::vector<llvm::Metadata *> fields = {
      llvm::MDString::get(context, "model=" + prototype.ModelName),
      llvm::MDString::get(context,
                          "input_count=" +
                              std::to_string(prototype.Inputs.size())),
      llvm::MDString::get(context,
                          "return_count=" +
                              std::to_string(prototype.Returns.size())),
      recoveredParamListMetadata(context, prototype.Inputs),
      recoveredParamListMetadata(context, prototype.Returns),
  };
  return llvm::MDNode::get(context, fields);
}

void clearPrototypeRecoveryMetadata(llvm::Function &function) {
  function.setMetadata("notdec.prototype.input_candidates", nullptr);
  function.setMetadata("notdec.prototype.return_candidates", nullptr);
  function.setMetadata("notdec.prototype.recovered", nullptr);
}

std::optional<std::string> returnValueKey(llvm::Value &value) {
  if (auto *constantInt = llvm::dyn_cast<llvm::ConstantInt>(&value)) {
    llvm::SmallString<32> text;
    constantInt->getValue().toString(text, 10, false);
    return ("const:" + text).str();
  }
  if (value.hasName()) {
    return ("value:" + value.getName()).str();
  }
  return std::nullopt;
}

bool sameReturnStoreValue(llvm::Value &first, llvm::Value &second,
                          unsigned depth = 0) {
  if (&first == &second) {
    return true;
  }
  std::optional<std::string> firstKey = returnValueKey(first);
  std::optional<std::string> secondKey = returnValueKey(second);
  if (firstKey && secondKey && *firstKey == *secondKey) {
    return true;
  }
  if (depth >= 4) {
    return false;
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&first)) {
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (incoming == nullptr ||
          !sameReturnStoreValue(*incoming, second, depth + 1)) {
        return false;
      }
    }
    return phi->getNumIncomingValues() > 0;
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&second)) {
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (incoming == nullptr ||
          !sameReturnStoreValue(first, *incoming, depth + 1)) {
        return false;
      }
    }
    return phi->getNumIncomingValues() > 0;
  }
  return false;
}

bool hasActiveExternalInputUse(llvm::Function &function,
                               llvm::StringRef registerName) {
  bool sawExternalInputLoad = false;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      llvm::MDNode *metadata = load->getMetadata("notdec.register.external_input");
      if (metadata == nullptr) {
        continue;
      }
      std::optional<std::string> name = metadataField(*metadata, "name");
      if (!name || *name != registerName) {
        continue;
      }
      sawExternalInputLoad = true;
      if (!load->use_empty()) {
        return true;
      }
    }
  }
  return !sawExternalInputLoad;
}

bool hasActiveUse(const llvm::Instruction &instruction) {
  return !instruction.use_empty();
}

std::optional<llvm::AllocaInst *> functionStackAlloca(llvm::Function &function) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
      if (alloca == nullptr || !alloca->hasName() ||
          !alloca->getName().starts_with("notdec_stack")) {
        continue;
      }
      return alloca;
    }
  }
  return std::nullopt;
}

std::optional<int64_t> constantByteOffsetFromBase(llvm::Value *pointer,
                                                  llvm::Value *base,
                                                  const llvm::DataLayout &layout) {
  auto *gep = llvm::dyn_cast_or_null<llvm::GEPOperator>(pointer);
  if (gep == nullptr || gep->getPointerOperand()->stripPointerCasts() != base) {
    return std::nullopt;
  }

  llvm::APInt offset(64, 0, true);
  if (!gep->accumulateConstantOffset(layout, offset)) {
    return std::nullopt;
  }
  return offset.getSExtValue();
}

std::vector<NativeParamTrial> stackInputTrials(llvm::Function &function,
                                               const NativePrototypeModel &model) {
  std::vector<NativeParamTrial> trials;
  std::optional<llvm::AllocaInst *> stackBase = functionStackAlloca(function);
  const llvm::Module *module = function.getParent();
  if (!stackBase || module == nullptr) {
    return trials;
  }

  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr || !hasActiveUse(*load)) {
        continue;
      }
      llvm::MDNode *metadata = load->getMetadata("notdec.stack.input");
      if (metadata == nullptr) {
        continue;
      }
      // Stack candidates come from HeritageToLLVM metadata, not from pointer
      // arithmetic alone.  The GEP/base check keeps the metadata tied to the
      // current function's stack object.
      std::optional<std::string> space = metadataField(*metadata, "space");
      std::optional<uint64_t> offset = parseUint64Field(*metadata, "offset");
      std::optional<uint64_t> size = parseUint64Field(*metadata, "size");
      if (!space || !offset || !size ||
          *size > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        continue;
      }
      if (!constantByteOffsetFromBase(load->getPointerOperand(), *stackBase,
                                      module->getDataLayout())) {
        continue;
      }
      std::optional<NativeStorageMatch> match =
          model.findInputStack(*space, *offset, static_cast<uint32_t>(*size));
      if (!match) {
        continue;
      }

      NativeParamTrial trial;
      trial.StorageKind = "stack";
      trial.StackSpace = *space;
      trial.StackOffset = *offset;
      trial.Size = static_cast<uint32_t>(*size);
      trial.Slot = match->Slot;
      trial.Active = true;
      trials.push_back(std::move(trial));
    }
  }
  return trials;
}

std::optional<llvm::LoadInst *> uniqueExternalInputLoad(
    llvm::Function &function, llvm::StringRef registerName) {
  llvm::LoadInst *result = nullptr;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      llvm::MDNode *metadata = load->getMetadata("notdec.register.external_input");
      if (metadata == nullptr) {
        continue;
      }
      std::optional<std::string> name = metadataField(*metadata, "name");
      if (!name || *name != registerName) {
        continue;
      }
      if (result != nullptr) {
        return std::nullopt;
      }
      result = load;
    }
  }
  if (result == nullptr) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::string> registerAccessName(llvm::Instruction &instruction) {
  llvm::MDNode *metadata = instruction.getMetadata("notdec.register.access");
  if (metadata == nullptr) {
    return std::nullopt;
  }
  return metadataField(*metadata, "name");
}

bool isDeclarationCallOutputLoad(llvm::LoadInst &load) {
  std::optional<std::string> registerName = registerAccessName(load);
  if (!registerName) {
    return false;
  }

  for (auto iter = llvm::BasicBlock::reverse_iterator(load.getIterator()),
            end = load.getParent()->rend();
       iter != end; ++iter) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter)) {
      if (registerAccessName(*store) == registerName) {
        return false;
      }
      continue;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&*iter);
    if (call == nullptr) {
      continue;
    }
    llvm::Function *callee = call->getCalledFunction();
    return callee != nullptr && !callee->isIntrinsic() &&
           callee->isDeclaration();
  }
  return false;
}

bool hasUnsafeReturnValueLoad(
    llvm::ArrayRef<NativePrototypeReturnBinding> returnBindings) {
  // A return value that is still a register load can also be a direct
  // callsite's old return load.  Batch rewriting another callee may erase that
  // load later, so skip it until rewrite ordering tracks that dependency.
  for (const NativePrototypeReturnBinding &binding : returnBindings) {
    auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(binding.ReturnValue);
    if (load != nullptr && load->getMetadata("notdec.register.access") != nullptr &&
        !isDeclarationCallOutputLoad(*load)) {
      return true;
    }
    auto *trunc = llvm::dyn_cast_or_null<llvm::TruncInst>(binding.ReturnValue);
    auto *truncLoad = trunc != nullptr
                          ? llvm::dyn_cast_or_null<llvm::LoadInst>(
                                trunc->getOperand(0))
                          : nullptr;
    if (truncLoad != nullptr &&
        truncLoad->getMetadata("notdec.register.access") != nullptr &&
        !isDeclarationCallOutputLoad(*truncLoad)) {
      return true;
    }
  }
  return false;
}

bool hasAnyPrototypeCandidateMetadata(const llvm::Function &function) {
  return function.getMetadata("notdec.prototype.recovered") != nullptr ||
         function.getMetadata("notdec.prototype.input_candidates") != nullptr ||
         function.getMetadata("notdec.prototype.return_candidates") != nullptr;
}

std::optional<llvm::Value *> registerStoreValueInReverseRange(
    llvm::BasicBlock::reverse_iterator iter, llvm::BasicBlock::reverse_iterator end,
    llvm::StringRef registerName, llvm::Type *valueType) {
  for (; iter != end; ++iter) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter);
    if (store == nullptr) {
      continue;
    }
    llvm::MDNode *metadata = store->getMetadata("notdec.register.access");
    if (metadata == nullptr) {
      continue;
    }
    std::optional<std::string> name = metadataField(*metadata, "name");
    if (!name || *name != registerName) {
      continue;
    }
    llvm::Value *value = store->getValueOperand();
    if (value == nullptr || value->getType() != valueType) {
      return std::nullopt;
    }
    return value;
  }
  return std::nullopt;
}

struct RegisterValueLookup {
  bool Unsafe = false;
  llvm::Value *Value = nullptr;
};

// Resolves a register-global load back to the current register value when the
// path is local to this function and no non-intrinsic call can clobber it.
RegisterValueLookup registerValueInReverseRange(
    llvm::BasicBlock::reverse_iterator iter,
    llvm::BasicBlock::reverse_iterator end, llvm::StringRef registerName,
    llvm::Type *valueType) {
  for (; iter != end; ++iter) {
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&*iter)) {
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr || !callee->isIntrinsic()) {
        return RegisterValueLookup{true, nullptr};
      }
      continue;
    }

    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter);
    if (store == nullptr) {
      continue;
    }
    llvm::MDNode *storeMetadata =
        store->getMetadata("notdec.register.access");
    if (storeMetadata == nullptr ||
        metadataField(*storeMetadata, "name") != registerName) {
      continue;
    }
    llvm::Value *value = store->getValueOperand();
    if (value == nullptr || value->getType() != valueType) {
      return RegisterValueLookup{true, nullptr};
    }
    return RegisterValueLookup{false, value};
  }
  return RegisterValueLookup{};
}

RegisterValueLookup registerValueAtBlockEntry(
    llvm::BasicBlock &block, llvm::StringRef registerName, llvm::Type *valueType,
    std::set<llvm::BasicBlock *> &visited) {
  if (!visited.insert(&block).second) {
    return RegisterValueLookup{};
  }

  llvm::Value *result = nullptr;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    RegisterValueLookup local = registerValueInReverseRange(
        predecessor->rbegin(), predecessor->rend(), registerName, valueType);
    if (local.Unsafe) {
      return local;
    }

    llvm::Value *value = local.Value;
    if (value == nullptr) {
      RegisterValueLookup incoming = registerValueAtBlockEntry(
          *predecessor, registerName, valueType, visited);
      if (incoming.Unsafe) {
        return incoming;
      }
      value = incoming.Value;
    }
    if (value == nullptr) {
      continue;
    }
    if (result == nullptr) {
      result = value;
      continue;
    }
    if (!sameReturnStoreValue(*result, *value)) {
      return RegisterValueLookup{true, nullptr};
    }
  }
  return RegisterValueLookup{false, result};
}

std::optional<llvm::Value *> registerStoreValueBeforeLoad(llvm::LoadInst &load) {
  llvm::MDNode *metadata = load.getMetadata("notdec.register.access");
  if (metadata == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> registerName = metadataField(*metadata, "name");
  if (!registerName) {
    return std::nullopt;
  }

  RegisterValueLookup local = registerValueInReverseRange(
      llvm::BasicBlock::reverse_iterator(load.getIterator()),
      load.getParent()->rend(), *registerName, load.getType());
  if (local.Unsafe) {
    return std::nullopt;
  }
  if (local.Value != nullptr) {
    return local.Value;
  }

  std::set<llvm::BasicBlock *> visited;
  RegisterValueLookup incoming = registerValueAtBlockEntry(
      *load.getParent(), *registerName, load.getType(), visited);
  if (incoming.Unsafe || incoming.Value == nullptr) {
    return std::nullopt;
  }
  return incoming.Value;
}

bool hasCallInReverseRange(llvm::BasicBlock::reverse_iterator iter,
                           llvm::BasicBlock::reverse_iterator end) {
  for (; iter != end; ++iter) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&*iter);
    if (call == nullptr) {
      continue;
    }
    llvm::Function *callee = call->getCalledFunction();
    if (callee != nullptr && callee->isIntrinsic()) {
      continue;
    }
    if (llvm::isa<llvm::CallInst>(&*iter)) {
      return true;
    }
  }
  return false;
}

std::optional<llvm::Argument *> functionArgumentForRecoveredInput(
    llvm::Function &function, llvm::StringRef registerName, llvm::Type *paramType) {
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype || prototype->Inputs.empty()) {
    return std::nullopt;
  }
  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(), *prototype);
  if (!recoveredType || function.getFunctionType() != *recoveredType) {
    return std::nullopt;
  }

  for (uint64_t index = 0; index < prototype->Inputs.size(); ++index) {
    if (prototype->Inputs[index].RegisterName != registerName) {
      continue;
    }
    if (index >= function.arg_size() ||
        function.getFunctionType()->getParamType(index) != paramType) {
      return std::nullopt;
    }
    return function.getArg(index);
  }
  return std::nullopt;
}

std::optional<llvm::Value *> functionEntryValueForRegister(
    llvm::Function &function, llvm::StringRef registerName, llvm::Type *paramType) {
  if (std::optional<llvm::Argument *> argument =
          functionArgumentForRecoveredInput(function, registerName, paramType)) {
    return *argument;
  }
  std::optional<llvm::LoadInst *> inputLoad =
      uniqueExternalInputLoad(function, registerName);
  if (!inputLoad || (*inputLoad)->getType() != paramType) {
    return std::nullopt;
  }
  return *inputLoad;
}

std::optional<llvm::Value *> registerPhiValueAtBlockEntry(
    llvm::BasicBlock &block, llvm::StringRef registerName, llvm::Type *paramType) {
  llvm::Value *result = nullptr;
  std::string prefix = (registerName + ".regssa").str();
  for (llvm::PHINode &phi : block.phis()) {
    if (!phi.getName().starts_with(prefix) || phi.getType() != paramType) {
      continue;
    }
    if (result != nullptr) {
      return std::nullopt;
    }
    result = &phi;
  }
  if (result == nullptr) {
    return std::nullopt;
  }
  return result;
}

std::optional<llvm::GlobalVariable *> registerGlobalForName(
    llvm::Module &module, llvm::StringRef registerName, llvm::Type *paramType) {
  llvm::GlobalVariable *result = nullptr;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (global.getValueType() != paramType) {
      continue;
    }
    llvm::MDNode *metadata = global.getMetadata("notdec.register");
    if (metadata == nullptr) {
      continue;
    }
    std::optional<std::string> name = metadataField(*metadata, "name");
    if (!name || *name != registerName) {
      continue;
    }
    if (result != nullptr) {
      return std::nullopt;
    }
    result = &global;
  }
  if (result == nullptr) {
    return std::nullopt;
  }
  return result;
}

llvm::MDNode *registerGlobalAccessMetadata(llvm::GlobalVariable &global) {
  llvm::MDNode *metadata = global.getMetadata("notdec.register");
  if (metadata == nullptr) {
    return nullptr;
  }
  llvm::LLVMContext &context = global.getContext();
  std::string name = metadataField(*metadata, "name").value_or(
      global.getName().str());
  std::string space =
      metadataField(*metadata, "space").value_or("register");
  std::string offset = metadataField(*metadata, "offset").value_or("0");
  std::string size = metadataField(*metadata, "size").value_or("0");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + name),
      llvm::MDString::get(context, "space=" + space),
      llvm::MDString::get(context, "offset=" + offset),
      llvm::MDString::get(context, "size=" + size),
      llvm::MDString::get(context, "name=" + name),
  };
  return llvm::MDNode::get(context, fields);
}

std::optional<llvm::Value *> registerGlobalValueBeforeCall(
    llvm::CallInst &call, llvm::StringRef registerName, llvm::Type *paramType) {
  std::optional<llvm::GlobalVariable *> global =
      registerGlobalForName(*call.getModule(), registerName, paramType);
  if (!global) {
    return std::nullopt;
  }

  // Old callees loaded ABI input registers from globals at entry.  After
  // signature rewrite, this load moves to the caller side for callsites where
  // no SSA store/entry value is available.
  llvm::IRBuilder<> builder(&call);
  llvm::LoadInst *load =
      builder.CreateLoad(paramType, *global, (registerName + ".callsite_input").str());
  if (llvm::MDNode *metadata = registerGlobalAccessMetadata(**global)) {
    load->setMetadata("notdec.register.access", metadata);
  }
  return load;
}

std::optional<llvm::Value *> equivalentInputValueFromPredecessors(
    llvm::BasicBlock &block, llvm::StringRef registerName,
    llvm::Type *paramType) {
  llvm::Value *result = nullptr;
  uint64_t predecessorCount = 0;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    ++predecessorCount;
    std::optional<llvm::Value *> value = registerStoreValueInReverseRange(
        predecessor->rbegin(), predecessor->rend(), registerName, paramType);
    if (!value) {
      return std::nullopt;
    }
    if (result == nullptr) {
      result = *value;
      continue;
    }
    if (!sameReturnStoreValue(*result, **value)) {
      return std::nullopt;
    }
  }
  if (predecessorCount < 2) {
    return std::nullopt;
  }
  return result;
}

std::optional<llvm::Value *> callsiteInputValueBeforeCall(
    llvm::CallInst &call, llvm::StringRef registerName, llvm::Type *paramType) {
  bool sawInterveningCall = hasCallInReverseRange(
      llvm::BasicBlock::reverse_iterator(call.getIterator()),
      call.getParent()->rend());
  std::optional<llvm::Value *> localValue = registerStoreValueInReverseRange(
      llvm::BasicBlock::reverse_iterator(call.getIterator()),
      call.getParent()->rend(), registerName, paramType);
  if (localValue) {
    return localValue;
  }

  std::set<llvm::BasicBlock *> visited;
  llvm::BasicBlock *current = call.getParent();
  while (visited.insert(current).second) {
    llvm::BasicBlock *predecessor = nullptr;
    bool sawMultiplePredecessors = false;
    for (llvm::BasicBlock *candidate : llvm::predecessors(current)) {
      if (predecessor != nullptr) {
        sawMultiplePredecessors = true;
        break;
      }
      predecessor = candidate;
    }
    if (sawMultiplePredecessors) {
      if (std::optional<llvm::Value *> value =
              equivalentInputValueFromPredecessors(*current, registerName,
                                                   paramType)) {
        return value;
      }
      return registerGlobalValueBeforeCall(call, registerName, paramType);
    }
    if (predecessor == nullptr) {
      if (!sawInterveningCall) {
        if (std::optional<llvm::Value *> entryValue =
                functionEntryValueForRegister(*call.getFunction(), registerName,
                                              paramType)) {
          return entryValue;
        }
      }
      return registerGlobalValueBeforeCall(call, registerName, paramType);
    }

    bool reachesCurrent = false;
    for (llvm::BasicBlock *candidate : llvm::successors(predecessor)) {
      if (candidate == current) {
        reachesCurrent = true;
        break;
      }
    }
    if (!reachesCurrent) {
      return std::nullopt;
    }

    std::optional<llvm::Value *> predecessorValue =
        registerStoreValueInReverseRange(predecessor->rbegin(),
                                         predecessor->rend(), registerName,
                                         paramType);
    if (predecessorValue) {
      return predecessorValue;
    }
    std::optional<llvm::Value *> predecessorPhi =
        registerPhiValueAtBlockEntry(*predecessor, registerName, paramType);
    if (predecessorPhi) {
      return predecessorPhi;
    }
    sawInterveningCall |=
        hasCallInReverseRange(predecessor->rbegin(), predecessor->rend());
    current = predecessor;
  }
  return registerGlobalValueBeforeCall(call, registerName, paramType);
}

// Multi-input form of Ghidra FuncCallSpecs::buildInputFromTrials(...): keep
// the call and ABI-ordered argument values together so the old void call can be
// replaced after every callsite has been checked.
struct MultiInputCallsiteRewrite {
  llvm::CallInst *Call = nullptr;
  std::vector<llvm::Value *> Arguments;
};

struct MultiInputCallsiteCollectionResult {
  std::vector<MultiInputCallsiteRewrite> Rewrites;
  std::string FailureReason;
};

MultiInputCallsiteCollectionResult collectMultiInputDirectCallsiteRewrites(
    llvm::Function &function,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> inputs,
    llvm::FunctionType &recoveredType) {
  MultiInputCallsiteCollectionResult result;
  for (llvm::User *user : function.users()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(user);
    if (call == nullptr || call->getCalledFunction() != &function ||
        call->arg_size() != 0 || !call->getType()->isVoidTy()) {
      result.FailureReason = "function has uses";
      return result;
    }

    MultiInputCallsiteRewrite rewrite;
    rewrite.Call = call;
    rewrite.Arguments.reserve(inputs.size());
    for (uint64_t index = 0; index < inputs.size(); ++index) {
      std::optional<llvm::Value *> argument = callsiteInputValueBeforeCall(
          *call, inputs[index].RegisterName, recoveredType.getParamType(index));
      if (!argument) {
        result.FailureReason = "unsafe callsite input value";
        return result;
      }
      rewrite.Arguments.push_back(*argument);
    }
    result.Rewrites.push_back(std::move(rewrite));
  }
  return result;
}

void rewriteMultiInputDirectCallsites(
    llvm::Function &rewritten,
    llvm::ArrayRef<MultiInputCallsiteRewrite> callsites) {
  for (const MultiInputCallsiteRewrite &callsite : callsites) {
    llvm::IRBuilder<> builder(callsite.Call);
    llvm::CallInst *newCall = builder.CreateCall(
        rewritten.getFunctionType(), &rewritten, callsite.Arguments);
    newCall->setCallingConv(callsite.Call->getCallingConv());
    callsite.Call->eraseFromParent();
  }
}

struct ReturnLoadSearchResult {
  llvm::LoadInst *Load = nullptr;
  llvm::BasicBlock *SharedSuccessor = nullptr;
  llvm::BasicBlock *CallPredecessor = nullptr;
  bool Blocked = false;
  bool Clobbered = false;
};

ReturnLoadSearchResult foundReturnLoad(llvm::LoadInst *load) {
  ReturnLoadSearchResult result;
  result.Load = load;
  return result;
}

ReturnLoadSearchResult blockedReturnLoadSearch() {
  ReturnLoadSearchResult result;
  result.Blocked = true;
  return result;
}

ReturnLoadSearchResult clobberedReturnLoadSearch() {
  ReturnLoadSearchResult result;
  result.Clobbered = true;
  return result;
}

struct ReturnOnlyCallsiteCollectionResult {
  std::vector<llvm::CallInst *> Callsites;
  std::string FailureReason;
};

struct MultiReturnCallsiteRewrite {
  llvm::CallInst *Call = nullptr;
  // Kept ABI-slot aligned.  A null entry means the caller does not use that
  // return component before it is overwritten or before control leaves the path.
  std::vector<llvm::LoadInst *> ReturnLoads;
  std::vector<ReturnLoadSearchResult> ReturnLoadResults;
  std::vector<std::string> ReturnRegisterNames;
};

struct MultiReturnCallsiteCollectionResult {
  std::vector<MultiReturnCallsiteRewrite> Rewrites;
  std::string FailureReason;
};

// Combined callsite rewrite for input + multi-return shapes.  It keeps the
// ABI-ordered argument values and old return loads together because the old void
// call is replaced by one typed call.
struct InputMultiReturnCallsiteRewrite {
  llvm::CallInst *Call = nullptr;
  std::vector<llvm::Value *> Arguments;
  // Same slot order as the recovered returns; null entries are unused results.
  std::vector<llvm::LoadInst *> ReturnLoads;
  std::vector<ReturnLoadSearchResult> ReturnLoadResults;
  std::vector<std::string> ReturnRegisterNames;
};

struct InputMultiReturnCallsiteCollectionResult {
  std::vector<InputMultiReturnCallsiteRewrite> Rewrites;
  std::string FailureReason;
};

// This local call-effect check is only used to stop return-load search at an
// intermediate call that overwrites the ABI return register.
bool functionMetadataHasRegister(const llvm::Function &function,
                                 llvm::StringRef metadataName,
                                 llvm::StringRef registerName) {
  llvm::MDNode *node = function.getMetadata(metadataName);
  if (node == nullptr) {
    return false;
  }
  std::string expected = ("name=" + registerName).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == expected) {
        return true;
      }
    }
  }
  return false;
}

std::set<std::string> collectAbiUnaffectedRegisters(llvm::Module &module) {
  std::set<std::string> unaffected;
  llvm::NamedMDNode *abiMetadata = module.getNamedMetadata("notdec.abi");
  if (abiMetadata == nullptr) {
    return unaffected;
  }
  for (llvm::MDNode *abiNode : abiMetadata->operands()) {
    for (const llvm::MDOperand &operand : abiNode->operands()) {
      auto *effectList = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
      if (effectList == nullptr) {
        continue;
      }
      for (const llvm::MDOperand &effectOperand : effectList->operands()) {
        auto *effectNode =
            llvm::dyn_cast_or_null<llvm::MDNode>(effectOperand.get());
        if (effectNode == nullptr ||
            metadataField(*effectNode, "effect") !=
                std::optional<std::string>("unaffected")) {
          continue;
        }
        for (const llvm::MDOperand &storageOperand : effectNode->operands()) {
          auto *storageNode =
              llvm::dyn_cast_or_null<llvm::MDNode>(storageOperand.get());
          if (storageNode == nullptr ||
              metadataField(*storageNode, "kind") !=
                  std::optional<std::string>("register")) {
            continue;
          }
          std::optional<std::string> name = metadataField(*storageNode, "name");
          if (name && !name->empty()) {
            unaffected.insert(*name);
          }
        }
      }
    }
  }
  return unaffected;
}

bool callClobbersRegister(llvm::CallBase &call, llvm::StringRef registerName) {
  llvm::Function *callee = call.getCalledFunction();
  if (callee != nullptr && callee->isIntrinsic()) {
    return false;
  }
  if (callee != nullptr && !callee->isDeclaration()) {
    if (functionMetadataHasRegister(*callee, "notdec.register.preserves",
                                    registerName)) {
      return false;
    }
    if (functionMetadataHasRegister(*callee, "notdec.register.clobbers",
                                    registerName)) {
      return true;
    }
  }
  llvm::Module *module = call.getModule();
  if (module == nullptr) {
    return true;
  }
  std::set<std::string> unaffected = collectAbiUnaffectedRegisters(*module);
  return unaffected.count(registerName.str()) == 0;
}

ReturnLoadSearchResult findReturnLoadBeforeStoreInRange(
    llvm::BasicBlock::iterator iter, llvm::BasicBlock::iterator end,
    llvm::StringRef returnRegisterName) {
  for (; iter != end; ++iter) {
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&*iter)) {
      if (callClobbersRegister(*call, returnRegisterName)) {
        return clobberedReturnLoadSearch();
      }
    }
    std::optional<std::string> name;
    if (llvm::MDNode *metadata =
            iter->getMetadata("notdec.register.access")) {
      name = metadataField(*metadata, "name");
    } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&*iter)) {
      if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
              load->getPointerOperand()->stripPointerCasts())) {
        if (llvm::MDNode *metadata = global->getMetadata("notdec.register")) {
          name = metadataField(*metadata, "name");
        }
      }
    } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter)) {
      if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
              store->getPointerOperand()->stripPointerCasts())) {
        if (llvm::MDNode *metadata = global->getMetadata("notdec.register")) {
          name = metadataField(*metadata, "name");
        }
      }
    }
    if (!name || *name != returnRegisterName) {
      continue;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&*iter)) {
      return foundReturnLoad(load);
    }
    if (llvm::isa<llvm::StoreInst>(&*iter)) {
      return clobberedReturnLoadSearch();
    }
  }
  return {};
}

ReturnLoadSearchResult findSharedSuccessorUnusedReturn(
    llvm::BasicBlock &block, llvm::StringRef returnRegisterName,
    std::set<llvm::BasicBlock *> &visitedBlocks);

bool hasUnvisitedPredecessor(llvm::BasicBlock &block,
                             const std::set<llvm::BasicBlock *> &visited) {
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    if (visited.count(predecessor) == 0) {
      return true;
    }
  }
  return false;
}

ReturnLoadSearchResult findDominatedSuccessorReturnLoad(
    llvm::BasicBlock &block, llvm::StringRef returnRegisterName) {
  std::vector<llvm::BasicBlock *> worklist = {&block};
  std::set<llvm::BasicBlock *> visited;
  while (!worklist.empty()) {
    llvm::BasicBlock *current = worklist.back();
    worklist.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }

    ReturnLoadSearchResult blockResult = findReturnLoadBeforeStoreInRange(
        current->begin(), current->end(), returnRegisterName);
    if (blockResult.Load != nullptr || blockResult.Blocked ||
        blockResult.Clobbered) {
      return blockResult;
    }

    llvm::Instruction *terminator = current->getTerminator();
    if (terminator == nullptr) {
      return blockedReturnLoadSearch();
    }
    for (llvm::BasicBlock *successor : llvm::successors(current)) {
      if (hasUnvisitedPredecessor(*successor, visited)) {
        return blockedReturnLoadSearch();
      }
      worklist.push_back(successor);
    }
  }
  return {};
}

ReturnLoadSearchResult findMixedSuccessorReturnLoad(
    llvm::BasicBlock &block, llvm::StringRef returnRegisterName) {
  llvm::LoadInst *load = nullptr;
  uint64_t successorCount = 0;
  // A call result dominates each direct successor.  One successor may read the
  // return register; all other reachable paths must prove the value is unused.
  for (llvm::BasicBlock *successor : llvm::successors(&block)) {
    ++successorCount;
    ReturnLoadSearchResult successorResult = findReturnLoadBeforeStoreInRange(
        successor->begin(), successor->end(), returnRegisterName);
    if (successorResult.Blocked) {
      return blockedReturnLoadSearch();
    }
    if (successorResult.Load != nullptr) {
      if (load != nullptr) {
        return blockedReturnLoadSearch();
      }
      load = successorResult.Load;
      continue;
    }
    if (successorResult.Clobbered) {
      continue;
    }
    ReturnLoadSearchResult nestedResult =
        findDominatedSuccessorReturnLoad(*successor, returnRegisterName);
    if (nestedResult.Load != nullptr) {
      if (load != nullptr) {
        return blockedReturnLoadSearch();
      }
      load = nestedResult.Load;
      continue;
    }
    if (nestedResult.Clobbered) {
      continue;
    }
    std::set<llvm::BasicBlock *> activeBlocks;
    ReturnLoadSearchResult unusedResult = findSharedSuccessorUnusedReturn(
        *successor, returnRegisterName, activeBlocks);
    if (unusedResult.Blocked || unusedResult.Load != nullptr) {
      return blockedReturnLoadSearch();
    }
  }
  if (successorCount <= 1) {
    return {};
  }
  return foundReturnLoad(load);
}

ReturnLoadSearchResult findSharedSuccessorUnusedReturn(
    llvm::BasicBlock &block, llvm::StringRef returnRegisterName,
    std::set<llvm::BasicBlock *> &visitedBlocks) {
  std::vector<llvm::BasicBlock *> worklist = {&block};
  while (!worklist.empty()) {
    llvm::BasicBlock *current = worklist.back();
    worklist.pop_back();
    if (!visitedBlocks.insert(current).second) {
      continue;
    }

    ReturnLoadSearchResult blockResult = findReturnLoadBeforeStoreInRange(
        current->begin(), current->end(), returnRegisterName);
    if (blockResult.Load != nullptr || blockResult.Blocked) {
      return blockedReturnLoadSearch();
    }
    if (blockResult.Clobbered) {
      continue;
    }

    llvm::Instruction *terminator = current->getTerminator();
    if (terminator == nullptr) {
      return blockedReturnLoadSearch();
    }
    for (llvm::BasicBlock *successor : llvm::successors(current)) {
      worklist.push_back(successor);
    }
  }
  return {};
}

ReturnLoadSearchResult
findCallsiteReturnLoad(llvm::CallInst &oldCall,
                       llvm::StringRef returnRegisterName,
                       bool allowSharedSuccessorLoad = false) {
  llvm::BasicBlock::iterator localIter(oldCall.getIterator());
  ReturnLoadSearchResult localResult = findReturnLoadBeforeStoreInRange(
      ++localIter, oldCall.getParent()->end(), returnRegisterName);
  if (localResult.Load != nullptr || localResult.Blocked ||
      localResult.Clobbered) {
    return localResult;
  }

  std::set<llvm::BasicBlock *> visited;
  llvm::BasicBlock *current = oldCall.getParent();
  while (visited.insert(current).second) {
    llvm::BasicBlock *successor = nullptr;
    for (llvm::BasicBlock *candidate : llvm::successors(current)) {
      if (successor != nullptr) {
        return findMixedSuccessorReturnLoad(*current, returnRegisterName);
      }
      successor = candidate;
    }
    if (successor == nullptr) {
      return {};
    }

    llvm::BasicBlock *predecessor = nullptr;
    bool hasMultiplePredecessors = false;
    bool hasCurrentPredecessor = false;
    for (llvm::BasicBlock *candidate : llvm::predecessors(successor)) {
      if (predecessor != nullptr) {
        hasMultiplePredecessors = true;
      }
      if (candidate == current) {
        hasCurrentPredecessor = true;
      }
      predecessor = candidate;
    }
    if (!hasCurrentPredecessor) {
      return blockedReturnLoadSearch();
    }

    ReturnLoadSearchResult successorResult = findReturnLoadBeforeStoreInRange(
        successor->begin(), successor->end(), returnRegisterName);
    if (hasMultiplePredecessors) {
      if (successorResult.Load != nullptr) {
        if (!allowSharedSuccessorLoad) {
          return blockedReturnLoadSearch();
        }
        successorResult.SharedSuccessor = successor;
        successorResult.CallPredecessor = current;
        return successorResult;
      }
      if (successorResult.Blocked) {
        return blockedReturnLoadSearch();
      }
      if (!successorResult.Clobbered) {
        std::set<llvm::BasicBlock *> activeBlocks;
        ReturnLoadSearchResult unusedResult = findSharedSuccessorUnusedReturn(
            *successor, returnRegisterName, activeBlocks);
        if (unusedResult.Blocked || unusedResult.Load != nullptr) {
          return blockedReturnLoadSearch();
        }
      }
      return {};
    }
    if (successorResult.Load != nullptr || successorResult.Blocked ||
        successorResult.Clobbered) {
      return successorResult;
    }
    current = successor;
  }
  std::set<llvm::BasicBlock *> activeBlocks;
  ReturnLoadSearchResult unusedResult =
      findSharedSuccessorUnusedReturn(*current, returnRegisterName,
                                      activeBlocks);
  if (unusedResult.Blocked || unusedResult.Load != nullptr) {
    return blockedReturnLoadSearch();
  }
  return {};
}

bool replaceSharedSuccessorReturnLoad(llvm::LoadInst &load,
                                      llvm::Value &callPathValue,
                                      const ReturnLoadSearchResult &result,
                                      llvm::StringRef returnRegisterName) {
  if (result.SharedSuccessor == nullptr || result.CallPredecessor == nullptr) {
    return false;
  }
  uint64_t predecessorCount = 0;
  for (llvm::BasicBlock *predecessor :
       llvm::predecessors(result.SharedSuccessor)) {
    (void)predecessor;
    ++predecessorCount;
  }
  llvm::IRBuilder<> builder(&*result.SharedSuccessor->getFirstInsertionPt());
  llvm::PHINode *phi =
      builder.CreatePHI(load.getType(), predecessorCount,
                        returnRegisterName + ".return_phi");
  for (llvm::BasicBlock *predecessor :
       llvm::predecessors(result.SharedSuccessor)) {
    if (predecessor == result.CallPredecessor) {
      phi->addIncoming(&callPathValue, predecessor);
      continue;
    }
    llvm::Instruction *terminator = predecessor->getTerminator();
    if (terminator == nullptr) {
      phi->eraseFromParent();
      return false;
    }
    llvm::IRBuilder<> predecessorBuilder(terminator);
    llvm::LoadInst *incomingLoad = predecessorBuilder.CreateLoad(
        load.getType(), load.getPointerOperand(),
        (returnRegisterName + ".return_incoming").str());
    if (llvm::MDNode *metadata = load.getMetadata("notdec.register.access")) {
      incomingLoad->setMetadata("notdec.register.access", metadata);
    }
    phi->addIncoming(incomingLoad, predecessor);
  }
  load.replaceAllUsesWith(phi);
  if (load.use_empty()) {
    load.eraseFromParent();
  }
  return true;
}

void rewriteCallsiteReturnLoad(llvm::CallInst &oldCall, llvm::CallInst &newCall,
                               llvm::StringRef returnRegisterName,
                               bool allowSharedSuccessorLoad = false) {
  ReturnLoadSearchResult result =
      findCallsiteReturnLoad(oldCall, returnRegisterName,
                             allowSharedSuccessorLoad);
  llvm::LoadInst *load = result.Load;
  if (load == nullptr) {
    return;
  }
  if (load->getType() != newCall.getType()) {
    return;
  }

  if (result.SharedSuccessor != nullptr && result.CallPredecessor != nullptr) {
    replaceSharedSuccessorReturnLoad(*load, newCall, result, returnRegisterName);
    return;
  }

  load->replaceAllUsesWith(&newCall);
  if (load->use_empty()) {
    load->eraseFromParent();
  }
}

void rewriteInputReturnDirectCallsites(
    llvm::Function &rewritten,
    llvm::ArrayRef<MultiInputCallsiteRewrite> callsites,
    llvm::StringRef returnRegisterName) {
  for (const MultiInputCallsiteRewrite &callsite : callsites) {
    llvm::IRBuilder<> builder(callsite.Call);
    llvm::CallInst *newCall = builder.CreateCall(
        rewritten.getFunctionType(), &rewritten, callsite.Arguments);
    newCall->setCallingConv(callsite.Call->getCallingConv());
    rewriteCallsiteReturnLoad(*callsite.Call, *newCall, returnRegisterName,
                              true);
    callsite.Call->eraseFromParent();
  }
}

bool callsiteHasMismatchedReturnLoad(llvm::CallInst &callsite,
                                     llvm::StringRef returnRegisterName,
                                     llvm::Type *returnType,
                                     bool allowSharedSuccessorLoad = false) {
  ReturnLoadSearchResult result =
      findCallsiteReturnLoad(callsite, returnRegisterName,
                             allowSharedSuccessorLoad);
  return result.Blocked || (result.Load != nullptr &&
                            result.Load->getType() != returnType);
}

ReturnOnlyCallsiteCollectionResult
collectReturnOnlyDirectCallsites(llvm::Function &function,
                                 llvm::StringRef returnRegisterName,
                                 llvm::Type *returnType) {
  ReturnOnlyCallsiteCollectionResult result;
  for (llvm::User *user : function.users()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(user);
    if (call == nullptr || call->getCalledFunction() != &function ||
        call->arg_size() != 0 || !call->getType()->isVoidTy()) {
      result.FailureReason = "function has uses";
      return result;
    }
    if (callsiteHasMismatchedReturnLoad(*call, returnRegisterName, returnType,
                                        true)) {
      result.FailureReason = "unsafe callsite return load";
      return result;
    }
    result.Callsites.push_back(call);
  }
  return result;
}

void rewriteReturnOnlyDirectCallsites(llvm::Function &rewritten,
                                      llvm::ArrayRef<llvm::CallInst *> callsites,
                                      llvm::StringRef returnRegisterName) {
  for (llvm::CallInst *callsite : callsites) {
    llvm::IRBuilder<> builder(callsite);
    llvm::CallInst *newCall =
        builder.CreateCall(rewritten.getFunctionType(), &rewritten, {});
    newCall->setCallingConv(callsite->getCallingConv());
    rewriteCallsiteReturnLoad(*callsite, *newCall, returnRegisterName, true);
    callsite->eraseFromParent();
  }
}

MultiReturnCallsiteCollectionResult collectMultiReturnDirectCallsites(
    llvm::Function &function,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> returns,
    llvm::StructType &returnType) {
  MultiReturnCallsiteCollectionResult result;
  for (llvm::User *user : function.users()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(user);
    if (call == nullptr || call->getCalledFunction() != &function ||
        call->arg_size() != 0 || !call->getType()->isVoidTy()) {
      result.FailureReason = "function has uses";
      return result;
    }

    MultiReturnCallsiteRewrite rewrite;
    rewrite.Call = call;
    rewrite.ReturnLoads.reserve(returns.size());
    rewrite.ReturnLoadResults.reserve(returns.size());
    rewrite.ReturnRegisterNames.reserve(returns.size());
    for (uint64_t index = 0; index < returns.size(); ++index) {
      ReturnLoadSearchResult loadResult =
          findCallsiteReturnLoad(*call, returns[index].RegisterName, true);
      if (loadResult.Blocked ||
          (loadResult.Load != nullptr &&
           loadResult.Load->getType() != returnType.getElementType(index))) {
        result.FailureReason = "unsafe callsite return load";
        return result;
      }
      rewrite.ReturnLoads.push_back(loadResult.Load);
      rewrite.ReturnLoadResults.push_back(loadResult);
      rewrite.ReturnRegisterNames.push_back(returns[index].RegisterName);
    }
    result.Rewrites.push_back(std::move(rewrite));
  }
  return result;
}

void rewriteMultiReturnDirectCallsites(
    llvm::Function &rewritten,
    llvm::ArrayRef<MultiReturnCallsiteRewrite> callsites) {
  for (const MultiReturnCallsiteRewrite &callsite : callsites) {
    llvm::IRBuilder<> builder(callsite.Call);
    llvm::CallInst *newCall =
        builder.CreateCall(rewritten.getFunctionType(), &rewritten, {});
    newCall->setCallingConv(callsite.Call->getCallingConv());
    for (uint64_t index = 0; index < callsite.ReturnLoads.size(); ++index) {
      llvm::LoadInst *load = callsite.ReturnLoads[index];
      if (load == nullptr) {
        continue;
      }
      llvm::Value *field =
          builder.CreateExtractValue(newCall, {static_cast<unsigned>(index)});
      if (index < callsite.ReturnLoadResults.size() &&
          callsite.ReturnLoadResults[index].SharedSuccessor != nullptr) {
        llvm::StringRef registerName =
            index < callsite.ReturnRegisterNames.size()
                ? llvm::StringRef(callsite.ReturnRegisterNames[index])
                : llvm::StringRef();
        replaceSharedSuccessorReturnLoad(*load, *field,
                                         callsite.ReturnLoadResults[index],
                                         registerName);
        continue;
      }
      load->replaceAllUsesWith(field);
      if (load->use_empty()) {
        load->eraseFromParent();
      }
    }
    callsite.Call->eraseFromParent();
  }
}

InputMultiReturnCallsiteCollectionResult
collectInputMultiReturnDirectCallsites(
    llvm::Function &function,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> inputs,
    llvm::FunctionType &recoveredType,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> returns,
    llvm::StructType &returnType) {
  InputMultiReturnCallsiteCollectionResult result;
  for (llvm::User *user : function.users()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(user);
    if (call == nullptr || call->getCalledFunction() != &function ||
        call->arg_size() != 0 || !call->getType()->isVoidTy()) {
      result.FailureReason = "function has uses";
      return result;
    }

    InputMultiReturnCallsiteRewrite rewrite;
    rewrite.Call = call;
    rewrite.Arguments.reserve(inputs.size());
    for (uint64_t index = 0; index < inputs.size(); ++index) {
      std::optional<llvm::Value *> argument = callsiteInputValueBeforeCall(
          *call, inputs[index].RegisterName, recoveredType.getParamType(index));
      if (!argument) {
        result.FailureReason = "unsafe callsite input value";
        return result;
      }
      rewrite.Arguments.push_back(*argument);
    }
    rewrite.ReturnLoads.reserve(returns.size());
    rewrite.ReturnLoadResults.reserve(returns.size());
    rewrite.ReturnRegisterNames.reserve(returns.size());
    for (uint64_t index = 0; index < returns.size(); ++index) {
      ReturnLoadSearchResult loadResult =
          findCallsiteReturnLoad(*call, returns[index].RegisterName, true);
      if (loadResult.Blocked ||
          (loadResult.Load != nullptr &&
           loadResult.Load->getType() != returnType.getElementType(index))) {
        result.FailureReason = "unsafe callsite return load";
        return result;
      }
      rewrite.ReturnLoads.push_back(loadResult.Load);
      rewrite.ReturnLoadResults.push_back(loadResult);
      rewrite.ReturnRegisterNames.push_back(returns[index].RegisterName);
    }
    result.Rewrites.push_back(std::move(rewrite));
  }
  return result;
}

void rewriteInputMultiReturnDirectCallsites(
    llvm::Function &rewritten,
    llvm::ArrayRef<InputMultiReturnCallsiteRewrite> callsites) {
  for (const InputMultiReturnCallsiteRewrite &callsite : callsites) {
    llvm::IRBuilder<> builder(callsite.Call);
    llvm::CallInst *newCall = builder.CreateCall(
        rewritten.getFunctionType(), &rewritten, callsite.Arguments);
    newCall->setCallingConv(callsite.Call->getCallingConv());
    for (uint64_t index = 0; index < callsite.ReturnLoads.size(); ++index) {
      llvm::LoadInst *load = callsite.ReturnLoads[index];
      if (load == nullptr) {
        continue;
      }
      llvm::Value *field =
          builder.CreateExtractValue(newCall, {static_cast<unsigned>(index)});
      if (index < callsite.ReturnLoadResults.size() &&
          callsite.ReturnLoadResults[index].SharedSuccessor != nullptr) {
        llvm::StringRef registerName =
            index < callsite.ReturnRegisterNames.size()
                ? llvm::StringRef(callsite.ReturnRegisterNames[index])
                : llvm::StringRef();
        replaceSharedSuccessorReturnLoad(*load, *field,
                                         callsite.ReturnLoadResults[index],
                                         registerName);
        continue;
      }
      load->replaceAllUsesWith(field);
      if (load->use_empty()) {
        load->eraseFromParent();
      }
    }
    callsite.Call->eraseFromParent();
  }
}

std::vector<NativeParamTrial> returnTrialsBeforeInstruction(
    llvm::Instruction &instruction, const NativePrototypeModel &model) {
  std::vector<NativeParamTrial> trials;
  std::set<uint64_t> seenSlots;
  llvm::BasicBlock::reverse_iterator iter(instruction.getIterator());
  llvm::BasicBlock::reverse_iterator end = instruction.getParent()->rend();
  for (; iter != end; ++iter) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter);
    if (store == nullptr) {
      continue;
    }

    llvm::MDNode *access = store->getMetadata("notdec.register.access");
    if (access == nullptr) {
      continue;
    }
    std::optional<std::string> name = metadataField(*access, "name");
    std::optional<std::string> base = metadataField(*access, "base");
    std::optional<std::string> outputName = name;
    std::optional<NativeStorageMatch> match;
    if (outputName) {
      match = model.findOutputRegister(*outputName);
    }
    if (!match && base && store->getValueOperand() != nullptr &&
        store->getMetadata("notdec.register.synthetic") == nullptr &&
        store->getValueOperand()->getType()->isIntegerTy(64)) {
      auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
          store->getPointerOperand()->stripPointerCasts());
      if (global != nullptr && global->getValueType()->isIntegerTy(64)) {
        llvm::MDNode *globalMetadata = global->getMetadata("notdec.register");
        if (globalMetadata != nullptr &&
            metadataField(*globalMetadata, "name") == *base) {
          outputName = base;
          match = model.findOutputRegister(*outputName);
        }
      }
    }
    if (!outputName) {
      continue;
    }
    if (!match) {
      continue;
    }
    if (!seenSlots.insert(match->Slot).second) {
      continue;
    }

    NativeParamTrial trial;
    trial.RegisterName = *outputName;
    trial.Slot = match->Slot;
    trial.Store = store;
    trial.Value = store->getValueOperand();
    if (trial.Value != nullptr) {
      trial.ValueKey = returnValueKey(*trial.Value);
    }
    trial.Active = true;
    trials.push_back(std::move(trial));
  }
  return trials;
}

bool hasConflictingReturnTrialValue(
    llvm::ArrayRef<NativeParamTrial> trials,
    const std::map<uint64_t, NativeParamTrial> &firstTrialsBySlot,
    uint64_t slot) {
  auto first = firstTrialsBySlot.find(slot);
  if (first == firstTrialsBySlot.end() || first->second.Value == nullptr) {
    return false;
  }
  for (const NativeParamTrial &trial : trials) {
    if (trial.Slot != slot || trial.Value == nullptr) {
      continue;
    }
    if (!sameReturnStoreValue(*first->second.Value, *trial.Value)) {
      return true;
    }
  }
  return false;
}

llvm::BasicBlock *uniquePredecessor(llvm::BasicBlock &block) {
  llvm::BasicBlock *result = nullptr;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    if (result != nullptr) {
      return nullptr;
    }
    result = predecessor;
  }
  return result;
}

std::vector<NativeParamTrial> returnTrialsBefore(
    llvm::ReturnInst &ret, const NativePrototypeModel &model) {
  std::vector<NativeParamTrial> trials =
      returnTrialsBeforeInstruction(ret, model);
  if (!trials.empty()) {
    return trials;
  }

  std::set<llvm::BasicBlock *> visited;
  llvm::BasicBlock *block = ret.getParent();
  while (block != nullptr && visited.insert(block).second) {
    llvm::BasicBlock *predecessor = uniquePredecessor(*block);
    if (predecessor == nullptr) {
      return {};
    }
    llvm::Instruction *terminator = predecessor->getTerminator();
    if (terminator == nullptr) {
      return {};
    }
    trials = returnTrialsBeforeInstruction(*terminator, model);
    if (!trials.empty()) {
      return trials;
    }
    block = predecessor;
  }
  return {};
}

std::optional<std::vector<llvm::StoreInst *>> returnPointStores(
    llvm::Function &function, const NativePrototypeModel &model,
    llvm::StringRef registerName) {
  std::vector<llvm::StoreInst *> stores;
  std::set<llvm::StoreInst *> seenStores;
  uint64_t returnCount = 0;
  for (llvm::BasicBlock &block : function) {
    auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
    if (ret == nullptr) {
      continue;
    }
    ++returnCount;
    llvm::StoreInst *storeForReturn = nullptr;
    for (const NativeParamTrial &trial : returnTrialsBefore(*ret, model)) {
      if (trial.RegisterName == registerName && trial.Store != nullptr) {
        storeForReturn = trial.Store;
        break;
      }
    }
    if (storeForReturn == nullptr) {
      return std::nullopt;
    }
    if (seenStores.insert(storeForReturn).second) {
      stores.push_back(storeForReturn);
    }
  }
  if (returnCount == 0 || stores.empty()) {
    return std::nullopt;
  }
  return stores;
}

void addFunctionSummary(NativePrototypeRecoverySummary &total,
                        const NativePrototypeRecoveryFunctionSummary &function) {
  ++total.FunctionsSeen;
  total.ExternalInputsSeen += function.ExternalInputsSeen;
  total.InputCandidates += function.InputCandidates;
  total.ReturnCandidates += function.ReturnCandidates;
  if (function.RewriteEligible) {
    ++total.RewriteEligibleFunctions;
  }
  if (function.NeedsSignatureRewrite) {
    ++total.SignatureRewriteNeededFunctions;
  }
  total.Functions.push_back(function);
}

void addUniqueTrialBySlot(NativeParamActive &active,
                          std::set<uint64_t> &seenSlots,
                          NativeParamTrial trial) {
  if (!seenSlots.insert(trial.Slot).second) {
    return;
  }
  active.Trials.push_back(std::move(trial));
}

void sortTrialsBySlot(NativeParamActive &active) {
  std::stable_sort(active.Trials.begin(), active.Trials.end(),
                   [](const NativeParamTrial &lhs,
                      const NativeParamTrial &rhs) {
                     return lhs.Slot < rhs.Slot;
                   });
}

std::optional<uint64_t> parseUint64Field(const llvm::MDNode &node,
                                         llvm::StringRef key) {
  std::optional<std::string> text = metadataField(node, key);
  if (!text) {
    return std::nullopt;
  }
  try {
    size_t parsed = 0;
    uint64_t value = std::stoull(*text, &parsed, 0);
    if (parsed != text->size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

llvm::Value *returnValueForStore(llvm::StoreInst &store) {
  llvm::Value *value = store.getValueOperand();
  if (auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(value)) {
    if (std::optional<llvm::Value *> storedValue =
            registerStoreValueBeforeLoad(*load)) {
      value = *storedValue;
    }
  }
  if (value == nullptr || value->getType()->isIntegerTy(64)) {
    return value;
  }

  llvm::MDNode *access = store.getMetadata("notdec.register.access");
  if (access == nullptr) {
    return value;
  }
  std::optional<uint64_t> size = parseUint64Field(*access, "size");
  if (!size || *size != 8) {
    return value;
  }

  auto *integerType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
  if (integerType == nullptr || integerType->getBitWidth() <= 64) {
    return value;
  }

  llvm::IRBuilder<> builder(&store);
  return builder.CreateTrunc(value, llvm::Type::getInt64Ty(store.getContext()),
                             "notdec.return.slice");
}

std::optional<std::vector<llvm::LoadInst *>>
returnPointDeclarationCallOutputLoads(llvm::Function &function,
                                      llvm::StringRef registerName) {
  std::vector<llvm::LoadInst *> loads;
  std::set<llvm::LoadInst *> seenLoads;
  uint64_t returnCount = 0;
  for (llvm::BasicBlock &block : function) {
    auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
    if (ret == nullptr) {
      continue;
    }
    ++returnCount;

    llvm::LoadInst *loadForReturn = nullptr;
    for (auto iter = llvm::BasicBlock::reverse_iterator(ret->getIterator()),
              end = block.rend();
         iter != end; ++iter) {
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter)) {
        if (registerAccessName(*store) == registerName) {
          return std::nullopt;
        }
        continue;
      }
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&*iter);
      if (load == nullptr || registerAccessName(*load) != registerName) {
        continue;
      }
      if (!isDeclarationCallOutputLoad(*load)) {
        return std::nullopt;
      }
      loadForReturn = load;
      break;
    }
    if (loadForReturn == nullptr) {
      return std::nullopt;
    }
    if (seenLoads.insert(loadForReturn).second) {
      loads.push_back(loadForReturn);
    }
  }
  if (returnCount == 0 || loads.empty()) {
    return std::nullopt;
  }
  return loads;
}

void applyDeclarationCallOutputAliases(
    std::vector<NativePrototypeReturnBinding> &bindings) {
  std::map<std::string, llvm::LoadInst *> callOutputsByRegister;
  for (const NativePrototypeReturnBinding &binding : bindings) {
    auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(binding.ReturnValue);
    if (load == nullptr || !isDeclarationCallOutputLoad(*load)) {
      continue;
    }
    std::optional<std::string> registerName = registerAccessName(*load);
    if (!registerName) {
      continue;
    }
    callOutputsByRegister[*registerName] = load;
  }

  for (NativePrototypeReturnBinding &binding : bindings) {
    auto found = callOutputsByRegister.find(binding.Param.RegisterName);
    if (found == callOutputsByRegister.end()) {
      continue;
    }
    binding.ReturnValue = found->second;
    binding.EraseReturnStores = false;
  }
}

std::optional<std::vector<NativeRecoveredPrototypeParam>>
readRecoveredParamList(const llvm::MDNode &node) {
  std::vector<NativeRecoveredPrototypeParam> params;
  std::set<std::string> seenNames;
  std::set<std::string> seenStorage;
  std::optional<uint64_t> previousSlot;
  for (const llvm::MDOperand &operand : node.operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      return std::nullopt;
    }
    std::optional<uint64_t> slot = parseUint64Field(*entry, "slot");
    if (!slot) {
      return std::nullopt;
    }
    std::string storage = metadataField(*entry, "storage").value_or("register");
    NativeRecoveredPrototypeParam param;
    param.StorageKind = storage;
    param.Slot = *slot;
    if (storage == "register") {
      if (entry->getNumOperands() != 2 && entry->getNumOperands() != 4) {
        return std::nullopt;
      }
      std::optional<std::string> name = metadataField(*entry, "name");
      if (!name || name->empty()) {
        return std::nullopt;
      }
      if (entry->getNumOperands() == 4 &&
          metadataField(*entry, "storage") != "register") {
        return std::nullopt;
      }
      if (!seenNames.insert(*name).second) {
        return std::nullopt;
      }
      param.RegisterName = *name;
      if (std::optional<uint64_t> size = parseUint64Field(*entry, "size")) {
        if (*size == 0 ||
            *size > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
          return std::nullopt;
        }
        param.Size = static_cast<uint32_t>(*size);
      }
    } else if (storage == "stack") {
      if (entry->getNumOperands() != 5) {
        return std::nullopt;
      }
      std::optional<std::string> space = metadataField(*entry, "space");
      std::optional<uint64_t> offset = parseUint64Field(*entry, "offset");
      std::optional<uint64_t> size = parseUint64Field(*entry, "size");
      if (!space || space->empty() || !offset || !size || *size == 0 ||
          *size > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::nullopt;
      }
      std::string key = *space + ":" + std::to_string(*offset) + ":" +
                        std::to_string(*size);
      if (!seenStorage.insert(key).second) {
        return std::nullopt;
      }
      param.StackSpace = *space;
      param.StackOffset = *offset;
      param.Size = static_cast<uint32_t>(*size);
    } else {
      return std::nullopt;
    }
    if (previousSlot && *slot <= *previousSlot) {
      return std::nullopt;
    }
    params.push_back(std::move(param));
    previousSlot = *slot;
  }
  return params;
}

} // namespace

NativePrototypeRecoverySummary runNativePrototypeRecovery(
    llvm::Module &module, const NativePrototypeRecoveryOptions &options) {
  NativePrototypeRecoverySummary summary;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi) {
    for (llvm::Function &function : module) {
      if (!function.isDeclaration()) {
        clearPrototypeRecoveryMetadata(function);
      }
    }
    return summary;
  }

  NativePrototypeModel model(*abi);
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    NativePrototypeRecoveryFunctionSummary functionSummary;
    functionSummary.FunctionName = function.getName().str();
    llvm::MDNode *previousRecoveredMetadata =
        function.getMetadata("notdec.prototype.recovered");
    std::optional<NativeRecoveredPrototype> previousRecovered =
        readNativeRecoveredPrototypeMetadata(function);

    NativeParamActive active;
    std::set<uint64_t> inputSlots;
    llvm::MDNode *externalInputs =
        function.getMetadata("notdec.register.external_inputs");
    if (externalInputs != nullptr) {
      for (const llvm::MDOperand &operand : externalInputs->operands()) {
        auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
        if (entry == nullptr) {
          continue;
        }
        std::optional<std::string> name = metadataField(*entry, "name");
        if (!name) {
          continue;
        }
        ++functionSummary.ExternalInputsSeen;

        std::optional<NativeStorageMatch> match =
            model.findInputRegister(*name);
        if (!match) {
          continue;
        }
        if (nativeAbiHasEffectRegister(*abi, NativeAbiEffectKind::Unaffected,
                                       *name)) {
          continue;
        }
        if (!hasActiveExternalInputUse(function, *name)) {
          continue;
        }
        if (!inputSlots.insert(match->Slot).second) {
          continue;
        }

        NativeParamTrial trial;
        trial.RegisterName = *name;
        trial.Size = 8;
        trial.Slot = match->Slot;
        trial.Active = true;
        active.Trials.push_back(std::move(trial));
      }
    }
    for (NativeParamTrial &trial : stackInputTrials(function, model)) {
      addUniqueTrialBySlot(active, inputSlots, std::move(trial));
    }
    sortTrialsBySlot(active);

    NativeParamActive returns;
    std::map<uint64_t, NativeParamTrial> returnTrialsBySlot;
    std::map<uint64_t, uint64_t> returnSlotCounts;
    std::vector<NativeParamTrial> returnTrials;
    uint64_t returnCount = 0;
    for (llvm::BasicBlock &block : function) {
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
        ++returnCount;
        for (NativeParamTrial &trial : returnTrialsBefore(*ret, model)) {
          uint64_t slot = trial.Slot;
          ++returnSlotCounts[slot];
          returnTrialsBySlot.try_emplace(slot, trial);
          returnTrials.push_back(std::move(trial));
        }
      }
    }
    std::set<uint64_t> returnSlots;
    for (const auto &[slot, count] : returnSlotCounts) {
      if (count != returnCount) {
        continue;
      }
      if (hasConflictingReturnTrialValue(returnTrials, returnTrialsBySlot,
                                         slot)) {
        continue;
      }
      addUniqueTrialBySlot(returns, returnSlots,
                           std::move(returnTrialsBySlot[slot]));
    }
    sortTrialsBySlot(returns);

    functionSummary.InputCandidates = active.Trials.size();
    functionSummary.ReturnCandidates = returns.Trials.size();
    if (llvm::MDNode *node =
            inputCandidateMetadata(module.getContext(), active)) {
      function.setMetadata("notdec.prototype.input_candidates", node);
    } else {
      function.setMetadata("notdec.prototype.input_candidates", nullptr);
    }
    if (llvm::MDNode *node =
            inputCandidateMetadata(module.getContext(), returns)) {
      function.setMetadata("notdec.prototype.return_candidates", node);
    } else {
      function.setMetadata("notdec.prototype.return_candidates", nullptr);
    }
    NativeRecoveredPrototype recovered;
    recovered.ModelName = abi->PrototypeName;
    recovered.Inputs = recoveredParams(active);
    recovered.Returns = recoveredParams(returns);
    // A rerun after signature rewrite may not have register candidates anymore.
    // Keep the old recovered prototype if it still matches the current type.
    bool hasRecoveredCandidates =
        !recovered.Inputs.empty() || !recovered.Returns.empty();
    if (!hasRecoveredCandidates && previousRecovered &&
        previousRecoveredMetadata != nullptr &&
        previousRecovered->ModelName == abi->PrototypeName) {
      std::optional<llvm::FunctionType *> previousType =
          buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                                   *previousRecovered);
      if (previousType && function.getFunctionType() == *previousType) {
        function.setMetadata("notdec.prototype.recovered",
                             previousRecoveredMetadata);
      } else {
        function.setMetadata(
            "notdec.prototype.recovered",
            recoveredPrototypeMetadata(module.getContext(), recovered));
      }
    } else {
      function.setMetadata(
          "notdec.prototype.recovered",
          recoveredPrototypeMetadata(module.getContext(), recovered));
    }
    NativePrototypeRewriteEligibility rewrite =
        getNativePrototypeRewriteEligibility(function);
    functionSummary.RewriteEligible = rewrite.Eligible;
    functionSummary.NeedsSignatureRewrite = rewrite.NeedsRewrite;
    addFunctionSummary(summary, functionSummary);
  }

  if (options.RewriteSignatures) {
    NativePrototypeModuleRewriteSummary rewriteSummary =
        rewriteNativeRecoveredPrototypes(module);
    summary.SignatureRewriteFunctionsSeen = rewriteSummary.FunctionsSeen;
    summary.SignatureRewriteFunctionsRewritten =
        rewriteSummary.FunctionsRewritten;
    summary.SignatureRewriteFunctionsSkipped =
        rewriteSummary.FunctionsSkipped;
    summary.SignatureRewriteSkippedByReason =
        rewriteSummary.SkippedByReason;
    summary.SignatureRewriteFunctions = rewriteSummary.Functions;
  }

  if (options.PrintSummary) {
    printNativePrototypeRecoverySummary(summary, llvm::errs());
  }
  return summary;
}

std::optional<NativeRecoveredPrototype>
readNativeRecoveredPrototypeMetadata(const llvm::Function &function) {
  llvm::MDNode *node = function.getMetadata("notdec.prototype.recovered");
  if (node == nullptr || node->getNumOperands() != 5) {
    return std::nullopt;
  }

  std::optional<std::string> model = metadataField(*node, "model");
  if (!model || model->empty()) {
    return std::nullopt;
  }
  std::optional<uint64_t> inputCount = parseUint64Field(*node, "input_count");
  std::optional<uint64_t> returnCount =
      parseUint64Field(*node, "return_count");
  if (!inputCount || !returnCount) {
    return std::nullopt;
  }

  auto *inputsNode = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(3));
  auto *returnsNode = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(4));
  if (inputsNode == nullptr || returnsNode == nullptr) {
    return std::nullopt;
  }

  std::optional<std::vector<NativeRecoveredPrototypeParam>> inputs =
      readRecoveredParamList(*inputsNode);
  std::optional<std::vector<NativeRecoveredPrototypeParam>> returns =
      readRecoveredParamList(*returnsNode);
  if (!inputs || !returns) {
    return std::nullopt;
  }
  if (*inputCount != inputs->size() || *returnCount != returns->size()) {
    return std::nullopt;
  }
  NativeRecoveredPrototype prototype;
  prototype.ModelName = *model;
  prototype.Inputs = std::move(*inputs);
  prototype.Returns = std::move(*returns);
  return prototype;
}

std::optional<llvm::FunctionType *> buildNativeRecoveredPrototypeFunctionType(
    llvm::LLVMContext &context, const NativeRecoveredPrototype &prototype) {
  std::vector<llvm::Type *> paramTypes;
  llvm::Type *registerType = llvm::Type::getInt64Ty(context);
  paramTypes.reserve(prototype.Inputs.size());
  for (const NativeRecoveredPrototypeParam &param : prototype.Inputs) {
    if (param.StorageKind != "register") {
      return std::nullopt;
    }
    paramTypes.push_back(registerType);
  }

  llvm::Type *returnType = llvm::Type::getVoidTy(context);
  for (const NativeRecoveredPrototypeParam &param : prototype.Returns) {
    if (param.StorageKind != "register") {
      return std::nullopt;
    }
  }
  if (prototype.Returns.size() == 1) {
    returnType = registerType;
  } else if (prototype.Returns.size() > 1) {
    std::vector<llvm::Type *> returnElements(prototype.Returns.size(),
                                             registerType);
    returnType = llvm::StructType::get(context, returnElements);
  }
  return llvm::FunctionType::get(returnType, paramTypes, false);
}

NativePrototypeRewriteEligibility
getNativePrototypeRewriteEligibility(const llvm::Function &function) {
  NativePrototypeRewriteEligibility result;
  if (function.isDeclaration()) {
    result.Reason = "declaration";
    return result;
  }
  if (function.isVarArg()) {
    result.Reason = "vararg function";
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = hasAnyPrototypeCandidateMetadata(function)
                        ? "missing recovered prototype"
                        : "no recovered prototype candidates";
    return result;
  }
  if (const llvm::Module *module = function.getParent()) {
    std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(*module);
    if (abi && prototype->ModelName != abi->PrototypeName) {
      result.Reason = "recovered prototype ABI model mismatch";
      return result;
    }
  }

  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                               *prototype);
  if (!recoveredType) {
    result.Reason = "unsupported recovered prototype type";
    return result;
  }

  result.Eligible = true;
  result.RecoveredType = *recoveredType;
  result.NeedsRewrite = function.getFunctionType() != *recoveredType;
  result.Reason = result.NeedsRewrite ? "needs rewrite" : "already matches";
  return result;
}

std::optional<std::vector<NativePrototypeInputBinding>>
getNativePrototypeInputBindings(llvm::Function &function) {
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype || prototype->Inputs.empty()) {
    return std::nullopt;
  }

  std::vector<NativePrototypeInputBinding> bindings;
  bindings.reserve(prototype->Inputs.size());
  for (const NativeRecoveredPrototypeParam &param : prototype->Inputs) {
    std::optional<llvm::LoadInst *> load =
        uniqueExternalInputLoad(function, param.RegisterName);
    if (!load) {
      return std::nullopt;
    }

    NativePrototypeInputBinding binding;
    binding.Param = param;
    binding.ExternalInputLoad = *load;
    bindings.push_back(std::move(binding));
  }
  return bindings;
}

std::optional<std::vector<NativePrototypeReturnBinding>>
getNativePrototypeReturnBindings(llvm::Function &function) {
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype || prototype->Returns.empty()) {
    return std::nullopt;
  }
  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    return std::nullopt;
  }
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(*module);
  if (!abi) {
    return std::nullopt;
  }
  NativePrototypeModel model(*abi);

  std::vector<NativePrototypeReturnBinding> bindings;
  bindings.reserve(prototype->Returns.size());
  for (const NativeRecoveredPrototypeParam &param : prototype->Returns) {
    std::optional<std::vector<llvm::StoreInst *>> stores =
        returnPointStores(function, model, param.RegisterName);
    std::optional<std::vector<llvm::LoadInst *>> callOutputLoads =
        returnPointDeclarationCallOutputLoads(function, param.RegisterName);
    if (callOutputLoads) {
      NativePrototypeReturnBinding binding;
      binding.Param = param;
      binding.ReturnValue = callOutputLoads->front();
      binding.EraseReturnStores = false;
      bindings.push_back(std::move(binding));
      continue;
    }
    if (!stores || stores->empty()) {
      return std::nullopt;
    }

    NativePrototypeReturnBinding binding;
    binding.Param = param;
    binding.ReturnStore = stores->front();
    binding.ReturnStores = *stores;
    llvm::Value *rawReturnValue = stores->front()->getValueOperand();
    binding.ReturnValue = returnValueForStore(*stores->front());
    if (rawReturnValue == nullptr || binding.ReturnValue == nullptr) {
      return std::nullopt;
    }
    if (stores->size() == 1) {
      bindings.push_back(std::move(binding));
      continue;
    }
    for (llvm::StoreInst *store : *stores) {
      llvm::Value *value = store->getValueOperand();
      if (value == nullptr || value->getType() != rawReturnValue->getType()) {
        return std::nullopt;
      }
      if (!sameReturnStoreValue(*rawReturnValue, *value)) {
        return std::nullopt;
      }
    }
    bindings.push_back(std::move(binding));
  }
  applyDeclarationCallOutputAliases(bindings);
  return bindings;
}

void eraseReturnBindingStores(
    llvm::ArrayRef<NativePrototypeReturnBinding> returnBindings) {
  for (const NativePrototypeReturnBinding &binding : returnBindings) {
    if (!binding.EraseReturnStores) {
      continue;
    }
    if (!binding.ReturnStores.empty()) {
      for (llvm::StoreInst *store : binding.ReturnStores) {
        if (store != nullptr) {
          store->eraseFromParent();
        }
      }
    } else if (binding.ReturnStore != nullptr) {
      binding.ReturnStore->eraseFromParent();
    }
  }
}

void clearTransientPrototypeRecoveryMetadata(llvm::Function &function) {
  function.setMetadata("notdec.register.external_inputs", nullptr);
  function.setMetadata("notdec.prototype.input_candidates", nullptr);
  function.setMetadata("notdec.prototype.return_candidates", nullptr);
}

void appendCalleeFirstFunction(llvm::Function &function,
                               std::set<llvm::Function *> &visiting,
                               std::set<llvm::Function *> &visited,
                               std::vector<llvm::Function *> &ordered) {
  if (!visited.insert(&function).second) {
    return;
  }
  if (!visiting.insert(&function).second) {
    return;
  }

  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      llvm::Function *callee = call != nullptr ? call->getCalledFunction()
                                               : nullptr;
      if (callee == nullptr || callee == &function ||
          callee->getParent() != function.getParent()) {
        continue;
      }
      appendCalleeFirstFunction(*callee, visiting, visited, ordered);
    }
  }

  visiting.erase(&function);
  ordered.push_back(&function);
}

std::vector<llvm::Function *> calleeFirstFunctions(llvm::Module &module) {
  std::vector<llvm::Function *> ordered;
  std::set<llvm::Function *> visiting;
  std::set<llvm::Function *> visited;
  for (llvm::Function &function : module) {
    appendCalleeFirstFunction(function, visiting, visited, ordered);
  }
  return ordered;
}

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeReturnOnly(llvm::Function &function) {
  NativePrototypeRewriteResult result;
  result.Function = &function;
  if (function.getFunctionType()->getNumParams() != 0 ||
      !function.getReturnType()->isVoidTy()) {
    result.Reason = "original function is not void()";
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = "missing recovered prototype";
    return result;
  }
  if (!prototype->Inputs.empty() || prototype->Returns.size() != 1) {
    result.Reason = "not return-only prototype";
    return result;
  }

  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                               *prototype);
  if (!recoveredType || (*recoveredType)->getNumParams() != 0 ||
      !(*recoveredType)->getReturnType()->isIntegerTy(64)) {
    result.Reason = "unsupported recovered prototype type";
    return result;
  }

  std::optional<std::vector<NativePrototypeReturnBinding>> returnBindings =
      getNativePrototypeReturnBindings(function);
  if (!returnBindings || returnBindings->size() != 1) {
    result.Reason = "missing return binding";
    return result;
  }
  llvm::Value *returnValue = (*returnBindings)[0].ReturnValue;
  if (returnValue == nullptr ||
      returnValue->getType() != (*recoveredType)->getReturnType()) {
    result.Reason = "return value type mismatch";
    return result;
  }
  if (hasUnsafeReturnValueLoad(*returnBindings)) {
    result.Reason = "unsafe return value load";
    return result;
  }
  std::optional<std::vector<llvm::CallInst *>> callsiteRewrites;
  if (!function.use_empty()) {
    ReturnOnlyCallsiteCollectionResult callsiteCollection =
        collectReturnOnlyDirectCallsites(function,
                                         prototype->Returns[0].RegisterName,
                                         (*recoveredType)->getReturnType());
    if (!callsiteCollection.FailureReason.empty()) {
      result.Reason = callsiteCollection.FailureReason;
      return result;
    }
    callsiteRewrites = std::move(callsiteCollection.Callsites);
  }

  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    result.Reason = "function has no module";
    return result;
  }

  std::string originalName = function.getName().str();
  function.setName(originalName + ".old");
  llvm::Function *rewritten = llvm::Function::Create(
      *recoveredType, function.getLinkage(), originalName, module);
  rewritten->copyAttributesFrom(&function);
  rewritten->copyMetadata(&function, 0);
  clearTransientPrototypeRecoveryMetadata(*rewritten);
  rewritten->setCallingConv(function.getCallingConv());
  rewritten->splice(rewritten->end(), &function);

  for (llvm::BasicBlock &block : *rewritten) {
    auto *ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (ret == nullptr) {
      continue;
    }
    llvm::IRBuilder<> builder(ret);
    builder.CreateRet(returnValue);
    ret->eraseFromParent();
  }
  eraseReturnBindingStores(*returnBindings);
  if (callsiteRewrites) {
    rewriteReturnOnlyDirectCallsites(*rewritten, *callsiteRewrites,
                                     prototype->Returns[0].RegisterName);
  }

  function.eraseFromParent();
  result.Rewritten = true;
  result.Reason = "rewritten";
  result.Function = rewritten;
  return result;
}

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeInputOnly(llvm::Function &function) {
  NativePrototypeRewriteResult result;
  result.Function = &function;
  if (function.getFunctionType()->getNumParams() != 0 ||
      !function.getReturnType()->isVoidTy()) {
    result.Reason = "original function is not void()";
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = "missing recovered prototype";
    return result;
  }
  if (prototype->Inputs.empty() || !prototype->Returns.empty()) {
    result.Reason = "not input-only prototype";
    return result;
  }

  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                               *prototype);
  if (!recoveredType || (*recoveredType)->getNumParams() == 0 ||
      !(*recoveredType)->getReturnType()->isVoidTy()) {
    result.Reason = "unsupported recovered prototype type";
    return result;
  }
  for (llvm::Type *paramType : (*recoveredType)->params()) {
    if (!paramType->isIntegerTy(64)) {
      result.Reason = "unsupported recovered prototype type";
      return result;
    }
  }

  std::optional<std::vector<NativePrototypeInputBinding>> inputBindings =
      getNativePrototypeInputBindings(function);
  if (!inputBindings || inputBindings->size() != prototype->Inputs.size()) {
    result.Reason = "missing input binding";
    return result;
  }
  for (uint64_t index = 0; index < inputBindings->size(); ++index) {
    llvm::LoadInst *inputLoad = (*inputBindings)[index].ExternalInputLoad;
    if (inputLoad == nullptr ||
        inputLoad->getType() != (*recoveredType)->getParamType(index)) {
      result.Reason = "input load type mismatch";
      return result;
    }
  }
  std::optional<std::vector<MultiInputCallsiteRewrite>> callsiteRewrites;
  if (!function.use_empty()) {
    MultiInputCallsiteCollectionResult callsiteCollection =
        collectMultiInputDirectCallsiteRewrites(function, prototype->Inputs,
                                                **recoveredType);
    if (!callsiteCollection.FailureReason.empty()) {
      result.Reason = callsiteCollection.FailureReason;
      return result;
    }
    callsiteRewrites = std::move(callsiteCollection.Rewrites);
  }

  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    result.Reason = "function has no module";
    return result;
  }

  std::string originalName = function.getName().str();
  function.setName(originalName + ".old");
  llvm::Function *rewritten = llvm::Function::Create(
      *recoveredType, function.getLinkage(), originalName, module);
  rewritten->copyAttributesFrom(&function);
  rewritten->copyMetadata(&function, 0);
  clearTransientPrototypeRecoveryMetadata(*rewritten);
  rewritten->setCallingConv(function.getCallingConv());
  rewritten->splice(rewritten->end(), &function);

  auto argument = rewritten->arg_begin();
  for (const NativePrototypeInputBinding &binding : *inputBindings) {
    llvm::LoadInst *inputLoad = binding.ExternalInputLoad;
    argument->setName(inputLoad->getName());
    inputLoad->replaceAllUsesWith(&*argument);
    if (inputLoad->use_empty()) {
      inputLoad->eraseFromParent();
    }
    ++argument;
  }
  if (callsiteRewrites) {
    rewriteMultiInputDirectCallsites(*rewritten, *callsiteRewrites);
  }

  function.eraseFromParent();
  result.Rewritten = true;
  result.Reason = "rewritten";
  result.Function = rewritten;
  return result;
}

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeInputReturn(llvm::Function &function) {
  NativePrototypeRewriteResult result;
  result.Function = &function;
  if (function.getFunctionType()->getNumParams() != 0 ||
      !function.getReturnType()->isVoidTy()) {
    result.Reason = "original function is not void()";
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = "missing recovered prototype";
    return result;
  }
  if (prototype->Inputs.empty() || prototype->Returns.size() != 1) {
    result.Reason = "not input-return prototype";
    return result;
  }

  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                               *prototype);
  if (!recoveredType ||
      (*recoveredType)->getNumParams() != prototype->Inputs.size() ||
      !(*recoveredType)->getReturnType()->isIntegerTy(64)) {
    result.Reason = "unsupported recovered prototype type";
    return result;
  }
  for (llvm::Type *paramType : (*recoveredType)->params()) {
    if (!paramType->isIntegerTy(64)) {
      result.Reason = "unsupported recovered prototype type";
      return result;
    }
  }

  std::optional<std::vector<NativePrototypeInputBinding>> inputBindings =
      getNativePrototypeInputBindings(function);
  if (!inputBindings || inputBindings->size() != prototype->Inputs.size()) {
    result.Reason = "missing input binding";
    return result;
  }
  for (uint64_t index = 0; index < inputBindings->size(); ++index) {
    llvm::LoadInst *inputLoad = (*inputBindings)[index].ExternalInputLoad;
    if (inputLoad == nullptr ||
        inputLoad->getType() != (*recoveredType)->getParamType(index)) {
      result.Reason = "input load type mismatch";
      return result;
    }
  }

  std::optional<std::vector<NativePrototypeReturnBinding>> returnBindings =
      getNativePrototypeReturnBindings(function);
  if (!returnBindings || returnBindings->size() != 1) {
    result.Reason = "missing return binding";
    return result;
  }
  llvm::Value *returnValue = (*returnBindings)[0].ReturnValue;
  if (returnValue == nullptr ||
      returnValue->getType() != (*recoveredType)->getReturnType()) {
    result.Reason = "return value type mismatch";
    return result;
  }
  if (hasUnsafeReturnValueLoad(*returnBindings)) {
    result.Reason = "unsafe return value load";
    return result;
  }
  std::optional<std::vector<MultiInputCallsiteRewrite>> callsiteRewrites;
  if (!function.use_empty()) {
    MultiInputCallsiteCollectionResult callsiteCollection =
        collectMultiInputDirectCallsiteRewrites(function, prototype->Inputs,
                                                **recoveredType);
    if (!callsiteCollection.FailureReason.empty()) {
      result.Reason = callsiteCollection.FailureReason;
      return result;
    }
    callsiteRewrites = std::move(callsiteCollection.Rewrites);
    for (const MultiInputCallsiteRewrite &callsite : *callsiteRewrites) {
      if (callsiteHasMismatchedReturnLoad(
              *callsite.Call, prototype->Returns[0].RegisterName,
              (*recoveredType)->getReturnType(), true)) {
        result.Reason = "unsafe callsite return load";
        return result;
      }
    }
  }

  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    result.Reason = "function has no module";
    return result;
  }

  std::string originalName = function.getName().str();
  function.setName(originalName + ".old");
  llvm::Function *rewritten = llvm::Function::Create(
      *recoveredType, function.getLinkage(), originalName, module);
  rewritten->copyAttributesFrom(&function);
  rewritten->copyMetadata(&function, 0);
  clearTransientPrototypeRecoveryMetadata(*rewritten);
  rewritten->setCallingConv(function.getCallingConv());
  rewritten->splice(rewritten->end(), &function);

  auto argument = rewritten->arg_begin();
  for (const NativePrototypeInputBinding &binding : *inputBindings) {
    llvm::LoadInst *inputLoad = binding.ExternalInputLoad;
    argument->setName(inputLoad->getName());
    if (returnValue == inputLoad) {
      returnValue = &*argument;
    }
    inputLoad->replaceAllUsesWith(&*argument);
    if (inputLoad->use_empty()) {
      inputLoad->eraseFromParent();
    }
    ++argument;
  }

  for (llvm::BasicBlock &block : *rewritten) {
    auto *ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (ret == nullptr) {
      continue;
    }
    llvm::IRBuilder<> builder(ret);
    builder.CreateRet(returnValue);
    ret->eraseFromParent();
  }
  eraseReturnBindingStores(*returnBindings);
  if (callsiteRewrites) {
    rewriteInputReturnDirectCallsites(*rewritten, *callsiteRewrites,
                                      prototype->Returns[0].RegisterName);
  }

  function.eraseFromParent();
  result.Rewritten = true;
  result.Reason = "rewritten";
  result.Function = rewritten;
  return result;
}

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeMultiReturn(llvm::Function &function) {
  NativePrototypeRewriteResult result;
  result.Function = &function;
  if (function.getFunctionType()->getNumParams() != 0 ||
      !function.getReturnType()->isVoidTy()) {
    result.Reason = "original function is not void()";
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = "missing recovered prototype";
    return result;
  }
  if (!prototype->Inputs.empty() || prototype->Returns.size() <= 1) {
    result.Reason = "not multi-return prototype";
    return result;
  }
  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                               *prototype);
  auto *returnStruct =
      recoveredType
          ? llvm::dyn_cast<llvm::StructType>((*recoveredType)->getReturnType())
          : nullptr;
  if (!recoveredType || (*recoveredType)->getNumParams() != 0 ||
      returnStruct == nullptr ||
      returnStruct->getNumElements() != prototype->Returns.size()) {
    result.Reason = "unsupported recovered prototype type";
    return result;
  }

  std::optional<std::vector<NativePrototypeReturnBinding>> returnBindings =
      getNativePrototypeReturnBindings(function);
  if (!returnBindings || returnBindings->size() != prototype->Returns.size()) {
    result.Reason = "missing return binding";
    return result;
  }
  for (uint64_t index = 0; index < returnBindings->size(); ++index) {
    llvm::Value *returnValue = (*returnBindings)[index].ReturnValue;
    if (returnValue == nullptr ||
        returnValue->getType() != returnStruct->getElementType(index)) {
      result.Reason = "return value type mismatch";
      return result;
    }
  }
  if (hasUnsafeReturnValueLoad(*returnBindings)) {
    result.Reason = "unsafe return value load";
    return result;
  }
  std::optional<std::vector<MultiReturnCallsiteRewrite>> callsiteRewrites;
  if (!function.use_empty()) {
    MultiReturnCallsiteCollectionResult callsiteCollection =
        collectMultiReturnDirectCallsites(function, prototype->Returns,
                                          *returnStruct);
    if (!callsiteCollection.FailureReason.empty()) {
      result.Reason = callsiteCollection.FailureReason;
      return result;
    }
    callsiteRewrites = std::move(callsiteCollection.Rewrites);
  }

  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    result.Reason = "function has no module";
    return result;
  }

  std::string originalName = function.getName().str();
  function.setName(originalName + ".old");
  llvm::Function *rewritten = llvm::Function::Create(
      *recoveredType, function.getLinkage(), originalName, module);
  rewritten->copyAttributesFrom(&function);
  rewritten->copyMetadata(&function, 0);
  clearTransientPrototypeRecoveryMetadata(*rewritten);
  rewritten->setCallingConv(function.getCallingConv());
  rewritten->splice(rewritten->end(), &function);

  for (llvm::BasicBlock &block : *rewritten) {
    auto *ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (ret == nullptr) {
      continue;
    }
    llvm::IRBuilder<> builder(ret);
    llvm::Value *aggregate = llvm::UndefValue::get(returnStruct);
    for (uint64_t index = 0; index < returnBindings->size(); ++index) {
      aggregate = builder.Insert(llvm::InsertValueInst::Create(
          aggregate, (*returnBindings)[index].ReturnValue,
          {static_cast<unsigned>(index)}));
    }
    builder.CreateRet(aggregate);
    ret->eraseFromParent();
  }
  eraseReturnBindingStores(*returnBindings);
  if (callsiteRewrites) {
    rewriteMultiReturnDirectCallsites(*rewritten, *callsiteRewrites);
  }

  function.eraseFromParent();
  result.Rewritten = true;
  result.Reason = "rewritten";
  result.Function = rewritten;
  return result;
}

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeInputMultiReturn(llvm::Function &function) {
  NativePrototypeRewriteResult result;
  result.Function = &function;
  if (function.getFunctionType()->getNumParams() != 0 ||
      !function.getReturnType()->isVoidTy()) {
    result.Reason = "original function is not void()";
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = "missing recovered prototype";
    return result;
  }
  if (prototype->Inputs.empty() || prototype->Returns.size() <= 1) {
    result.Reason = "not input multi-return prototype";
    return result;
  }
  std::optional<llvm::FunctionType *> recoveredType =
      buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                               *prototype);
  auto *returnStruct =
      recoveredType
          ? llvm::dyn_cast<llvm::StructType>((*recoveredType)->getReturnType())
          : nullptr;
  if (!recoveredType ||
      (*recoveredType)->getNumParams() != prototype->Inputs.size() ||
      returnStruct == nullptr ||
      returnStruct->getNumElements() != prototype->Returns.size()) {
    result.Reason = "unsupported recovered prototype type";
    return result;
  }
  for (llvm::Type *paramType : (*recoveredType)->params()) {
    if (!paramType->isIntegerTy(64)) {
      result.Reason = "unsupported recovered prototype type";
      return result;
    }
  }

  std::optional<std::vector<NativePrototypeInputBinding>> inputBindings =
      getNativePrototypeInputBindings(function);
  if (!inputBindings || inputBindings->size() != prototype->Inputs.size()) {
    result.Reason = "missing input binding";
    return result;
  }
  for (uint64_t index = 0; index < inputBindings->size(); ++index) {
    llvm::LoadInst *inputLoad = (*inputBindings)[index].ExternalInputLoad;
    if (inputLoad == nullptr ||
        inputLoad->getType() != (*recoveredType)->getParamType(index)) {
      result.Reason = "input load type mismatch";
      return result;
    }
  }

  std::optional<std::vector<NativePrototypeReturnBinding>> returnBindings =
      getNativePrototypeReturnBindings(function);
  if (!returnBindings || returnBindings->size() != prototype->Returns.size()) {
    result.Reason = "missing return binding";
    return result;
  }
  for (uint64_t index = 0; index < returnBindings->size(); ++index) {
    llvm::Value *returnValue = (*returnBindings)[index].ReturnValue;
    if (returnValue == nullptr ||
        returnValue->getType() != returnStruct->getElementType(index)) {
      result.Reason = "return value type mismatch";
      return result;
    }
  }
  if (hasUnsafeReturnValueLoad(*returnBindings)) {
    result.Reason = "unsafe return value load";
    return result;
  }

  std::optional<std::vector<InputMultiReturnCallsiteRewrite>> callsiteRewrites;
  if (!function.use_empty()) {
    InputMultiReturnCallsiteCollectionResult callsiteCollection =
        collectInputMultiReturnDirectCallsites(
            function, prototype->Inputs, **recoveredType, prototype->Returns,
            *returnStruct);
    if (!callsiteCollection.FailureReason.empty()) {
      result.Reason = callsiteCollection.FailureReason;
      return result;
    }
    callsiteRewrites = std::move(callsiteCollection.Rewrites);
  }

  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    result.Reason = "function has no module";
    return result;
  }

  std::string originalName = function.getName().str();
  function.setName(originalName + ".old");
  llvm::Function *rewritten = llvm::Function::Create(
      *recoveredType, function.getLinkage(), originalName, module);
  rewritten->copyAttributesFrom(&function);
  rewritten->copyMetadata(&function, 0);
  clearTransientPrototypeRecoveryMetadata(*rewritten);
  rewritten->setCallingConv(function.getCallingConv());
  rewritten->splice(rewritten->end(), &function);

  auto argument = rewritten->arg_begin();
  for (const NativePrototypeInputBinding &binding : *inputBindings) {
    llvm::LoadInst *inputLoad = binding.ExternalInputLoad;
    argument->setName(inputLoad->getName());
    for (NativePrototypeReturnBinding &returnBinding : *returnBindings) {
      if (returnBinding.ReturnValue == inputLoad) {
        returnBinding.ReturnValue = &*argument;
      }
    }
    inputLoad->replaceAllUsesWith(&*argument);
    if (inputLoad->use_empty()) {
      inputLoad->eraseFromParent();
    }
    ++argument;
  }

  for (llvm::BasicBlock &block : *rewritten) {
    auto *ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (ret == nullptr) {
      continue;
    }
    llvm::IRBuilder<> builder(ret);
    llvm::Value *aggregate = llvm::UndefValue::get(returnStruct);
    for (uint64_t index = 0; index < returnBindings->size(); ++index) {
      aggregate = builder.Insert(llvm::InsertValueInst::Create(
          aggregate, (*returnBindings)[index].ReturnValue,
          {static_cast<unsigned>(index)}));
    }
    builder.CreateRet(aggregate);
    ret->eraseFromParent();
  }
  eraseReturnBindingStores(*returnBindings);
  if (callsiteRewrites) {
    rewriteInputMultiReturnDirectCallsites(*rewritten, *callsiteRewrites);
  }

  function.eraseFromParent();
  result.Rewritten = true;
  result.Reason = "rewritten";
  result.Function = rewritten;
  return result;
}

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototype(llvm::Function &function) {
  NativePrototypeRewriteResult result;
  result.Function = &function;

  NativePrototypeRewriteEligibility eligibility =
      getNativePrototypeRewriteEligibility(function);
  if (!eligibility.Eligible) {
    result.Reason = eligibility.Reason;
    return result;
  }
  if (eligibility.Eligible && !eligibility.NeedsRewrite) {
    result.Reason = eligibility.Reason;
    return result;
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype) {
    result.Reason = "missing recovered prototype";
    return result;
  }

  if (prototype->Inputs.empty() && prototype->Returns.size() == 1) {
    return rewriteNativeRecoveredPrototypeReturnOnly(function);
  }
  if (!prototype->Inputs.empty() && prototype->Returns.empty()) {
    return rewriteNativeRecoveredPrototypeInputOnly(function);
  }
  if (!prototype->Inputs.empty() && prototype->Returns.size() == 1) {
    return rewriteNativeRecoveredPrototypeInputReturn(function);
  }
  if (prototype->Inputs.empty() && prototype->Returns.size() > 1) {
    return rewriteNativeRecoveredPrototypeMultiReturn(function);
  }
  if (!prototype->Inputs.empty() && prototype->Returns.size() > 1) {
    return rewriteNativeRecoveredPrototypeInputMultiReturn(function);
  }

  result.Reason = "unsupported recovered prototype shape";
  return result;
}

NativePrototypeModuleRewriteSummary
rewriteNativeRecoveredPrototypes(llvm::Module &module) {
  NativePrototypeModuleRewriteSummary summary;

  std::vector<llvm::Function *> functions = calleeFirstFunctions(module);

  for (llvm::Function *function : functions) {
    ++summary.FunctionsSeen;
    NativePrototypeModuleRewriteFunctionSummary functionSummary;
    functionSummary.FunctionName = function->getName().str();
    NativePrototypeRewriteResult result =
        rewriteNativeRecoveredPrototype(*function);
    functionSummary.Rewritten = result.Rewritten;
    functionSummary.Reason = result.Rewritten ? "rewritten" : result.Reason;
    summary.Functions.push_back(std::move(functionSummary));
    if (result.Rewritten) {
      ++summary.FunctionsRewritten;
      continue;
    }
    ++summary.FunctionsSkipped;
    ++summary.SkippedByReason[result.Reason];
  }

  return summary;
}

void printNativePrototypeRecoverySummary(
    const NativePrototypeRecoverySummary &summary, llvm::raw_ostream &os) {
  os << "native prototype recovery summary\n";
  os << "  functions: " << summary.FunctionsSeen << '\n';
  os << "  external inputs: " << summary.ExternalInputsSeen << '\n';
  os << "  input candidates: " << summary.InputCandidates << '\n';
  os << "  return candidates: " << summary.ReturnCandidates << '\n';
  os << "  rewrite eligible functions: " << summary.RewriteEligibleFunctions
     << '\n';
  os << "  signature rewrite needed functions: "
     << summary.SignatureRewriteNeededFunctions << '\n';
  os << "  signature rewrite seen functions: "
     << summary.SignatureRewriteFunctionsSeen << '\n';
  os << "  signature rewrite rewritten functions: "
     << summary.SignatureRewriteFunctionsRewritten << '\n';
  os << "  signature rewrite skipped functions: "
     << summary.SignatureRewriteFunctionsSkipped << '\n';
  for (const auto &[reason, count] : summary.SignatureRewriteSkippedByReason) {
    os << "  signature rewrite skipped reason " << reason << ": " << count
       << '\n';
  }
  for (const NativePrototypeModuleRewriteFunctionSummary &function :
       summary.SignatureRewriteFunctions) {
    os << "  signature rewrite function " << function.FunctionName
       << ": rewritten=" << (function.Rewritten ? 1 : 0)
       << " reason=" << function.Reason << '\n';
  }
  for (const NativePrototypeRecoveryFunctionSummary &function :
       summary.Functions) {
    os << "  function " << function.FunctionName
       << ": external_inputs=" << function.ExternalInputsSeen
       << " input_candidates=" << function.InputCandidates
       << " return_candidates=" << function.ReturnCandidates
       << " rewrite_eligible=" << (function.RewriteEligible ? 1 : 0)
       << " needs_signature_rewrite="
       << (function.NeedsSignatureRewrite ? 1 : 0) << '\n';
  }
}

} // namespace notdec::bin2llvm
