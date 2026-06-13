#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativePrototypeModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

bool accessMatchesEffectRegister(const llvm::MDNode &access,
                                 llvm::StringRef effectName);

std::set<std::string> stackFrameRegisterNames(const NativeAbiSpec &abi);

std::set<std::string> preservedNonStackFrameRegisterNames(
    const NativeAbiSpec &abi);

llvm::LoadInst *externalInputLoadForRegister(llvm::Function &function,
                                             llvm::StringRef registerName);

std::optional<int64_t> stackOffsetFromBase(llvm::Value *value,
                                           llvm::Value *base,
                                           std::set<llvm::Value *> &seen);

std::optional<uint64_t> memoryAccessSize(const llvm::DataLayout &layout,
                                         llvm::Instruction &instruction);

bool callMayReadRegisterName(llvm::CallBase &call, llvm::StringRef registerName);

bool isNoReturnDeclarationWithExplicitArguments(const llvm::Function &function);

bool storeIsDeadOnAllReturnPaths(
    llvm::StoreInst &store, const llvm::MDNode &access,
    llvm::function_ref<bool(llvm::CallBase &, llvm::StringRef)> mayReadName);

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

llvm::MDNode *stackInputMetadata(llvm::LLVMContext &context,
                                 llvm::StringRef space, uint64_t offset,
                                 uint32_t size) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, ("space=" + space).str()),
      llvm::MDString::get(context, "offset=" + std::to_string(offset)),
      llvm::MDString::get(context, "size=" + std::to_string(size)),
  };
  return llvm::MDNode::get(context, fields);
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

bool valueUsesExternalInputRegister(llvm::Value &value,
                                    llvm::StringRef registerName,
                                    unsigned depth = 0) {
  if (depth >= 8) {
    return false;
  }
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&value)) {
    llvm::MDNode *metadata =
        load->getMetadata("notdec.register.external_input");
    return metadata != nullptr &&
           accessMatchesEffectRegister(*metadata, registerName);
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&value)) {
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (incoming != nullptr &&
          valueUsesExternalInputRegister(*incoming, registerName, depth + 1)) {
        return true;
      }
    }
    return false;
  }
  auto *op = llvm::dyn_cast<llvm::Operator>(&value);
  if (op == nullptr) {
    return false;
  }
  if (op->getOpcode() != llvm::Instruction::Add &&
      op->getOpcode() != llvm::Instruction::Sub &&
      op->getOpcode() != llvm::Instruction::And &&
      op->getOpcode() != llvm::Instruction::PtrToInt &&
      op->getOpcode() != llvm::Instruction::IntToPtr) {
    return false;
  }
  for (llvm::Value *operand : op->operands()) {
    if (operand != nullptr &&
        valueUsesExternalInputRegister(*operand, registerName, depth + 1)) {
      return true;
    }
  }
  return false;
}

bool valueIsStackFrameExternalInputDerived(llvm::Value &value,
                                           const NativeAbiSpec &abi) {
  for (const std::string &registerName : stackFrameRegisterNames(abi)) {
    if (valueUsesExternalInputRegister(value, registerName)) {
      return true;
    }
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

std::optional<int64_t> rawStackInputOffset(llvm::LoadInst &load,
                                           llvm::LoadInst &stackBase) {
  auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(
      load.getPointerOperand()->stripPointerCasts());
  if (pointer == nullptr) {
    return std::nullopt;
  }
  std::set<llvm::Value *> seen;
  return stackOffsetFromBase(pointer->getOperand(0), &stackBase, seen);
}

std::optional<std::string>
matchingStackInputSpace(const NativeAbiSpec &abi,
                        const NativePrototypeModel &model, uint64_t offset,
                        uint32_t size) {
  for (const NativeAbiParamEntry &entry : abi.Inputs) {
    if (entry.Storage.Kind != NativeAbiStorageKind::Stack ||
        entry.Storage.Space.empty()) {
      continue;
    }
    if (model.findInputStack(entry.Storage.Space, offset, size)) {
      return entry.Storage.Space;
    }
  }
  return std::nullopt;
}

bool loadOnlyFeedsDeadPreservedRegisterRestore(llvm::LoadInst &load,
                                               const NativeAbiSpec &abi) {
  std::set<std::string> registerNames =
      preservedNonStackFrameRegisterNames(abi);
  if (registerNames.empty() || load.use_empty()) {
    return false;
  }

  // Saved-register restore slots look like a stack load immediately written back
  // to an unaffected GPR in the epilogue.  Do not turn that load into a function
  // argument; after signature rewrite the later cleanup can no longer see the
  // original load/store restore pair.
  bool sawRestoreStore = false;
  for (llvm::User *user : load.users()) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
    if (store == nullptr || store->getValueOperand() != &load) {
      return false;
    }

    llvm::MDNode *access = store->getMetadata("notdec.register.access");
    if (access == nullptr ||
        !storeIsDeadOnAllReturnPaths(*store, *access, callMayReadRegisterName)) {
      return false;
    }

    bool preservedStore = false;
    for (const std::string &registerName : registerNames) {
      if (accessMatchesEffectRegister(*access, registerName)) {
        preservedStore = true;
        break;
      }
    }
    if (!preservedStore) {
      return false;
    }
    sawRestoreStore = true;
  }
  return sawRestoreStore;
}

std::vector<NativeParamTrial> stackInputTrials(llvm::Function &function,
                                               const NativePrototypeModel &model,
                                               const NativeAbiSpec &abi) {
  std::vector<NativeParamTrial> trials;
  std::optional<llvm::AllocaInst *> stackBase = functionStackAlloca(function);
  const llvm::Module *module = function.getParent();
  llvm::LoadInst *rawStackBase =
      abi.StackPointerRegister.empty()
          ? nullptr
          : externalInputLoadForRegister(function, abi.StackPointerRegister);
  if ((!stackBase && rawStackBase == nullptr) || module == nullptr) {
    return trials;
  }

  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr || !hasActiveUse(*load)) {
        continue;
      }
      llvm::MDNode *metadata = load->getMetadata("notdec.stack.input");
      std::optional<std::string> space;
      std::optional<uint64_t> offset;
      std::optional<uint64_t> size;
      if (metadata != nullptr) {
        space = metadataField(*metadata, "space");
        offset = parseUint64Field(*metadata, "offset");
        size = parseUint64Field(*metadata, "size");
        // Metadata stack inputs must still come from the current function's
        // explicit stack object.
        if (!stackBase ||
            !constantByteOffsetFromBase(load->getPointerOperand(), *stackBase,
                                        module->getDataLayout())) {
          continue;
        }
      } else if (rawStackBase != nullptr) {
        std::optional<int64_t> rawOffset =
            rawStackInputOffset(*load, *rawStackBase);
        std::optional<uint64_t> accessSize =
            memoryAccessSize(module->getDataLayout(), *load);
        if (!rawOffset || *rawOffset < 0 ||
            abi.StackShift == 0 ||
            static_cast<uint64_t>(*rawOffset) != abi.StackShift ||
            !accessSize ||
            *accessSize >
                static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
          continue;
        }
        offset = static_cast<uint64_t>(*rawOffset);
        size = *accessSize;
        space = matchingStackInputSpace(
            abi, model, *offset, static_cast<uint32_t>(*size));
      } else {
        continue;
      }
      if (!space || !offset || !size ||
          *size > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        continue;
      }
      if (loadOnlyFeedsDeadPreservedRegisterRestore(*load, abi)) {
        continue;
      }
      std::optional<NativeStorageMatch> match =
          model.findInputStack(*space, *offset, static_cast<uint32_t>(*size));
      if (!match) {
        continue;
      }
      if (metadata == nullptr) {
        load->setMetadata(
            "notdec.stack.input",
            stackInputMetadata(function.getContext(), *space, *offset,
                               static_cast<uint32_t>(*size)));
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

bool stackInputMetadataMatches(llvm::MDNode &metadata, llvm::StringRef space,
                               uint64_t offset, uint32_t size) {
  std::optional<std::string> loadSpace = metadataField(metadata, "space");
  std::optional<uint64_t> loadOffset = parseUint64Field(metadata, "offset");
  std::optional<uint64_t> loadSize = parseUint64Field(metadata, "size");
  return loadSpace && *loadSpace == space && loadOffset &&
         *loadOffset == offset && loadSize && *loadSize == size;
}

std::optional<llvm::LoadInst *> uniqueStackInputLoad(
    llvm::Function &function, llvm::StringRef space, uint64_t offset,
    uint32_t size) {
  llvm::LoadInst *result = nullptr;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      llvm::MDNode *metadata = load->getMetadata("notdec.stack.input");
      if (metadata == nullptr) {
        continue;
      }
      if (!stackInputMetadataMatches(*metadata, space, offset, size)) {
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

std::optional<std::string> registerAccessBase(llvm::Instruction &instruction) {
  llvm::MDNode *metadata = instruction.getMetadata("notdec.register.access");
  if (metadata == nullptr) {
    return std::nullopt;
  }
  return metadataField(*metadata, "base");
}

std::optional<std::string> registerStorageBase(llvm::Instruction &instruction) {
  if (std::optional<std::string> base = registerAccessBase(instruction)) {
    return base;
  }

  llvm::Value *pointer = nullptr;
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    pointer = load->getPointerOperand();
  } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    pointer = store->getPointerOperand();
  }
  auto *global = pointer == nullptr
                     ? nullptr
                     : llvm::dyn_cast<llvm::GlobalVariable>(
                           pointer->stripPointerCasts());
  if (global == nullptr) {
    return std::nullopt;
  }
  llvm::MDNode *metadata = global->getMetadata("notdec.register");
  if (metadata == nullptr) {
    return std::nullopt;
  }
  return metadataField(*metadata, "name");
}

bool isDeclarationCallOutputLoad(llvm::LoadInst &load) {
  std::optional<std::string> registerBase = registerStorageBase(load);
  if (!registerBase) {
    return false;
  }

  for (auto iter = llvm::BasicBlock::reverse_iterator(load.getIterator()),
            end = load.getParent()->rend();
       iter != end; ++iter) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter)) {
      if (registerStorageBase(*store) == registerBase) {
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

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr &&
         callee->getName().starts_with("notdec.register.");
}

llvm::CallInst *declarationCallOutputSource(llvm::LoadInst &load) {
  std::optional<std::string> registerBase = registerStorageBase(load);
  if (!registerBase) {
    return nullptr;
  }

  for (auto iter = llvm::BasicBlock::reverse_iterator(load.getIterator()),
            end = load.getParent()->rend();
       iter != end; ++iter) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter)) {
      if (registerStorageBase(*store) == registerBase) {
        return nullptr;
      }
      continue;
    }
    auto *call = llvm::dyn_cast<llvm::CallInst>(&*iter);
    if (call == nullptr) {
      continue;
    }
    llvm::Function *callee = call->getCalledFunction();
    if (callee != nullptr && !callee->isIntrinsic() &&
        callee->isDeclaration()) {
      return call;
    }
    return nullptr;
  }
  return nullptr;
}

bool canRewriteDeclarationCallOutputLoad(llvm::LoadInst &load,
                                         const NativePrototypeModel &model) {
  if (load.getType() != llvm::Type::getInt64Ty(load.getContext())) {
    return false;
  }
  std::optional<std::string> base = registerStorageBase(load);
  if (!base || !model.findOutputRegister(*base)) {
    return false;
  }
  return declarationCallOutputSource(load) != nullptr;
}

// A declaration rewrite is only safe when the exact call and its output load
// stay in the same basic block with no same-base register store between them.
struct DeclarationCallOutputRewrite {
  llvm::CallInst *Call = nullptr;
  llvm::LoadInst *Load = nullptr;
};

using DeclarationCallOutputRewrites =
    std::map<llvm::Function *, std::vector<DeclarationCallOutputRewrite>>;

DeclarationCallOutputRewrites collectDeclarationCallOutputRewrites(
    llvm::Module &module, const NativePrototypeModel &model) {
  DeclarationCallOutputRewrites rewrites;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load == nullptr ||
            !canRewriteDeclarationCallOutputLoad(*load, model)) {
          continue;
        }
        llvm::CallInst *call = declarationCallOutputSource(*load);
        if (call == nullptr || call->arg_size() != 0 ||
            !call->getType()->isVoidTy()) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || !callee->isDeclaration() ||
            callee->getFunctionType()->getReturnType() !=
                llvm::Type::getVoidTy(module.getContext()) ||
            callee->getFunctionType()->getNumParams() != 0) {
          continue;
        }
        rewrites[callee].push_back({call, load});
      }
    }
  }

  for (auto iter = rewrites.begin(); iter != rewrites.end();) {
    bool safe = true;
    for (llvm::User *user : iter->first->users()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (call == nullptr || call->getCalledFunction() != iter->first ||
          call->arg_size() != 0 || !call->getType()->isVoidTy()) {
        safe = false;
        break;
      }
    }
    if (!safe) {
      iter = rewrites.erase(iter);
    } else {
      ++iter;
    }
  }
  return rewrites;
}

void rewriteDeclarationCallOutputs(llvm::Module &module,
                                   const NativePrototypeModel &model) {
  DeclarationCallOutputRewrites rewrites =
      collectDeclarationCallOutputRewrites(module, model);
  for (auto &[callee, callRewrites] : rewrites) {
    std::string originalName = callee->getName().str();
    callee->setName(originalName + ".old");
    auto *newType = llvm::FunctionType::get(
        llvm::Type::getInt64Ty(module.getContext()), {}, false);
    llvm::Function *rewritten =
        llvm::Function::Create(newType, callee->getLinkage(), originalName,
                               module);
    rewritten->copyMetadata(callee, 0);
    rewritten->setCallingConv(callee->getCallingConv());

    for (llvm::User *user : llvm::make_early_inc_range(callee->users())) {
      auto *oldCall = llvm::dyn_cast<llvm::CallInst>(user);
      if (oldCall == nullptr || oldCall->getCalledFunction() != callee ||
          oldCall->arg_size() != 0 || !oldCall->getType()->isVoidTy()) {
        continue;
      }
      llvm::IRBuilder<> builder(oldCall);
      llvm::CallInst *newCall =
          builder.CreateCall(rewritten->getFunctionType(), rewritten, {});
      newCall->setCallingConv(oldCall->getCallingConv());
      for (const DeclarationCallOutputRewrite &rewrite : callRewrites) {
        if (rewrite.Call != oldCall) {
          continue;
        }
        rewrite.Load->replaceAllUsesWith(newCall);
        if (rewrite.Load->use_empty()) {
          rewrite.Load->eraseFromParent();
        }
      }
      oldCall->eraseFromParent();
    }
    callee->eraseFromParent();
  }
}

std::optional<llvm::Value *> callsiteInputValueBeforeCall(
    llvm::CallInst &call, const NativeRecoveredPrototypeParam &input,
    llvm::Type *paramType);

std::optional<llvm::Value *> callInputCandidateValueBeforeCall(
    llvm::CallInst &call, const NativeRecoveredPrototypeParam &input,
    llvm::Type *paramType);

llvm::StoreInst *localCallsiteInputStoreBeforeCall(
    llvm::CallInst &call, llvm::StringRef registerName, llvm::Type *valueType);

void eraseCallsiteInputStores(llvm::ArrayRef<llvm::StoreInst *> inputStores);

bool callClobbersRegister(llvm::CallBase &call, llvm::StringRef registerName);

bool isFramePointerRegisterName(llvm::StringRef registerName);

bool isKnownNoStackArgumentDeclaration(const llvm::Function &function);

bool isKnownNoReturnDeclaration(const llvm::Function &function);

std::set<std::string> stackFrameRegisterNames(const NativeAbiSpec &abi);

struct DeclarationCallInputRewrite {
  llvm::CallInst *Call = nullptr;
  std::vector<NativeRecoveredPrototypeParam> Inputs;
  uint64_t ExistingArgumentCount = 0;
  std::vector<llvm::Value *> Arguments;
  std::vector<llvm::StoreInst *> InputStores;
};

using DeclarationCallInputRewrites =
    std::map<llvm::Function *, std::vector<DeclarationCallInputRewrite>>;

std::optional<uint64_t> knownExternalPrototypeInputCount(
    llvm::StringRef functionName) {
  // Small provider for Bench2 external declarations.  It carries only fixed
  // integer/pointer input count; ABI storage still comes from NativeAbiSpec.
  return llvm::StringSwitch<std::optional<uint64_t>>(functionName)
      .Case("__tls_get_addr", 1)
      .Case("chdir", 1)
      .Case("fopen", 2)
      .Case("gnu_get_libc_version", 0)
      .Case("kill", 2)
      .Case("pthread_barrier_destroy", 1)
      .Case("pthread_barrier_wait", 1)
      .Case("pthread_cond_broadcast", 1)
      .Case("pthread_cond_destroy", 1)
      .Case("pthread_cond_signal", 1)
      .Case("pthread_cond_wait", 2)
      .Case("pthread_join", 2)
      .Case("pthread_mutex_destroy", 1)
      .Case("pthread_mutex_lock", 1)
      .Case("pthread_mutex_trylock", 1)
      .Case("pthread_mutex_unlock", 1)
      .Case("pthread_once", 2)
      .Case("pthread_rwlock_destroy", 1)
      .Case("pthread_rwlock_rdlock", 1)
      .Case("pthread_rwlock_tryrdlock", 1)
      .Case("pthread_rwlock_trywrlock", 1)
      .Case("pthread_rwlock_unlock", 1)
      .Case("pthread_rwlock_wrlock", 1)
      .Case("recv", 4)
      .Case("sched_getcpu", 0)
      .Case("sem_post", 1)
      .Case("send", 4)
      .Case("strlen", 1)
      .Case("unsetenv", 1)
      .Default(std::nullopt);
}

std::optional<std::vector<NativeRecoveredPrototypeParam>>
knownExternalPrototypeInputs(const llvm::Function &callee,
                             const NativeAbiSpec &abi,
                             const NativePrototypeModel &model) {
  if (!callee.isDeclaration() || callee.isVarArg()) {
    return std::nullopt;
  }
  std::optional<uint64_t> inputCount =
      knownExternalPrototypeInputCount(callee.getName());
  if (!inputCount) {
    return std::nullopt;
  }

  std::vector<NativeRecoveredPrototypeParam> inputs;
  inputs.reserve(*inputCount);
  for (const NativeAbiParamEntry &entry : abi.Inputs) {
    if (inputs.size() == *inputCount) {
      break;
    }
    if (entry.Storage.Kind != NativeAbiStorageKind::Register) {
      continue;
    }
    if (llvm::StringRef(entry.Storage.Name).starts_with("XMM")) {
      continue;
    }
    if (entry.MaxSize < 8) {
      continue;
    }
    std::optional<NativeStorageMatch> match =
        model.findInputRegister(entry.Storage.Name);
    if (!match) {
      return std::nullopt;
    }

    NativeRecoveredPrototypeParam param;
    param.RegisterName = entry.Storage.Name;
    param.StorageKind = "register";
    param.Size = 8;
    param.Slot = match->Slot;
    inputs.push_back(std::move(param));
  }
  if (inputs.size() != *inputCount) {
    return std::nullopt;
  }
  return inputs;
}

std::optional<NativeRecoveredPrototypeParam> declarationInputParamForStore(
    llvm::StoreInst &store, const NativePrototypeModel &model) {
  llvm::MDNode *metadata = store.getMetadata("notdec.register.access");
  if (metadata == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> registerName = metadataField(*metadata, "name");
  if (!registerName) {
    return std::nullopt;
  }
  std::optional<NativeStorageMatch> match =
      model.findInputRegister(*registerName);
  if (!match) {
    return std::nullopt;
  }
  auto *integer = llvm::dyn_cast<llvm::IntegerType>(
      store.getValueOperand()->getType());
  if (integer == nullptr || integer->getBitWidth() != 64) {
    return std::nullopt;
  }

  NativeRecoveredPrototypeParam param;
  param.RegisterName = *registerName;
  param.StorageKind = "register";
  param.Size = 8;
  param.Slot = match->Slot;
  return param;
}

std::optional<NativeRecoveredPrototypeParam>
declarationInputParamForCandidate(llvm::MDNode &candidate,
                                  const NativePrototypeModel &model) {
  std::optional<std::string> trialState =
      metadataField(candidate, "trial_state");
  if (trialState) {
    if (*trialState != "active") {
      return std::nullopt;
    }
  } else {
    std::optional<std::string> strength = metadataField(candidate, "strength");
    if (strength && *strength != "strong_local_def" &&
        *strength != "strong_phi") {
      return std::nullopt;
    }
  }
  std::optional<std::string> registerName = metadataField(candidate, "register");
  if (!registerName) {
    return std::nullopt;
  }
  std::optional<NativeStorageMatch> match =
      model.findInputRegister(*registerName);
  if (!match) {
    return std::nullopt;
  }

  NativeRecoveredPrototypeParam param;
  param.RegisterName = *registerName;
  param.StorageKind = "register";
  param.Size = 8;
  param.Slot = match->Slot;
  return param;
}

std::vector<NativeRecoveredPrototypeParam> declarationInputParamsBeforeCall(
    llvm::CallInst &call, const NativePrototypeModel &model) {
  std::map<uint64_t, NativeRecoveredPrototypeParam> paramsBySlot;
  if (llvm::MDNode *candidates =
          call.getMetadata("notdec.register.call_input_candidates")) {
    for (const llvm::MDOperand &operand : candidates->operands()) {
      auto *candidate = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
      if (candidate == nullptr) {
        continue;
      }
      std::optional<NativeRecoveredPrototypeParam> param =
          declarationInputParamForCandidate(*candidate, model);
      if (!param) {
        continue;
      }
      paramsBySlot.try_emplace(param->Slot, *param);
    }
  }
  for (auto iter = llvm::BasicBlock::reverse_iterator(call.getIterator()),
            end = call.getParent()->rend();
       iter != end; ++iter) {
    if (auto *previousCall = llvm::dyn_cast<llvm::CallBase>(&*iter)) {
      if (isNotDecRegisterHelperCall(*previousCall)) {
        continue;
      }
      llvm::Function *callee = previousCall->getCalledFunction();
      if (callee == nullptr || !callee->isIntrinsic()) {
        break;
      }
      continue;
    }
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter);
    if (store == nullptr) {
      continue;
    }
    std::optional<NativeRecoveredPrototypeParam> param =
        declarationInputParamForStore(*store, model);
    if (!param) {
      continue;
    }
    paramsBySlot.try_emplace(param->Slot, *param);
  }

  std::vector<NativeRecoveredPrototypeParam> params;
  params.reserve(paramsBySlot.size());
  for (auto &[slot, param] : paramsBySlot) {
    params.push_back(std::move(param));
  }
  return params;
}

bool sameDeclarationInputParam(const NativeRecoveredPrototypeParam &left,
                               const NativeRecoveredPrototypeParam &right) {
  return left.StorageKind == right.StorageKind &&
         left.RegisterName == right.RegisterName && left.Size == right.Size &&
         left.Slot == right.Slot;
}

std::vector<NativeRecoveredPrototypeParam> commonDeclarationInputPrefix(
    llvm::ArrayRef<std::vector<NativeRecoveredPrototypeParam>> inputSets) {
  std::vector<NativeRecoveredPrototypeParam> result;
  if (inputSets.empty()) {
    return result;
  }
  result = inputSets.front();
  for (const std::vector<NativeRecoveredPrototypeParam> &inputs : inputSets) {
    uint64_t shared = 0;
    while (shared < result.size() && shared < inputs.size() &&
           sameDeclarationInputParam(result[shared], inputs[shared])) {
      ++shared;
    }
    result.resize(shared);
    if (result.empty()) {
      return result;
    }
  }
  return result;
}

std::vector<NativeRecoveredPrototypeParam> commonDeclarationInputSlots(
    llvm::ArrayRef<std::vector<NativeRecoveredPrototypeParam>> inputSets) {
  std::vector<NativeRecoveredPrototypeParam> result;
  if (inputSets.empty()) {
    return result;
  }

  std::map<uint64_t, NativeRecoveredPrototypeParam> common;
  for (const NativeRecoveredPrototypeParam &param : inputSets.front()) {
    common.try_emplace(param.Slot, param);
  }
  for (uint64_t index = 1; index < inputSets.size(); ++index) {
    for (auto iter = common.begin(); iter != common.end();) {
      bool found = false;
      for (const NativeRecoveredPrototypeParam &param : inputSets[index]) {
        if (sameDeclarationInputParam(iter->second, param)) {
          found = true;
          break;
        }
      }
      if (!found) {
        iter = common.erase(iter);
      } else {
        ++iter;
      }
    }
    if (common.empty()) {
      return result;
    }
  }

  result.reserve(common.size());
  for (auto &[slot, param] : common) {
    result.push_back(std::move(param));
  }
  return result;
}

bool declarationInputParamMatchesAbi(
    const NativeRecoveredPrototypeParam &param, const NativePrototypeModel &model) {
  if (param.StorageKind != "register") {
    return false;
  }
  std::optional<NativeStorageMatch> match =
      model.findInputRegister(param.RegisterName);
  return match && match->Slot == param.Slot && param.Size == 8;
}

std::optional<std::vector<NativeRecoveredPrototypeParam>>
existingDeclarationInputs(const llvm::Function &callee,
                          const NativePrototypeModel &model) {
  if (callee.arg_size() == 0) {
    return std::vector<NativeRecoveredPrototypeParam>{};
  }

  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(callee);
  if (!prototype || prototype->ModelName != model.modelName() ||
      prototype->Inputs.size() != callee.arg_size() ||
      !prototype->Returns.empty()) {
    return std::nullopt;
  }
  for (uint64_t index = 0; index < prototype->Inputs.size(); ++index) {
    if (callee.getFunctionType()->getParamType(index) !=
            llvm::Type::getInt64Ty(callee.getContext()) ||
        !declarationInputParamMatchesAbi(prototype->Inputs[index], model)) {
      return std::nullopt;
    }
  }
  return prototype->Inputs;
}

std::optional<std::vector<NativeRecoveredPrototypeParam>>
declarationInputSuffix(
    llvm::ArrayRef<NativeRecoveredPrototypeParam> existingInputs,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> callInputs) {
  if (existingInputs.empty()) {
    if (callInputs.empty()) {
      return std::nullopt;
    }
    return std::vector<NativeRecoveredPrototypeParam>(callInputs.begin(),
                                                      callInputs.end());
  }

  const NativeRecoveredPrototypeParam &lastExisting = existingInputs.back();
  std::vector<NativeRecoveredPrototypeParam> suffix;
  suffix.reserve(callInputs.size());
  for (const NativeRecoveredPrototypeParam &input : callInputs) {
    if (input.Slot <= lastExisting.Slot) {
      return std::nullopt;
    }
    suffix.push_back(input);
  }
  if (suffix.empty()) {
    return std::nullopt;
  }
  return suffix;
}

bool inferredDeclarationInputsFitProvider(
    llvm::ArrayRef<NativeRecoveredPrototypeParam> inferredInputs,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> providerInputs) {
  if (inferredInputs.size() > providerInputs.size()) {
    return false;
  }
  std::set<uint64_t> providerSlots;
  for (const NativeRecoveredPrototypeParam &input : providerInputs) {
    providerSlots.insert(input.Slot);
  }
  for (const NativeRecoveredPrototypeParam &input : inferredInputs) {
    if (providerSlots.find(input.Slot) == providerSlots.end()) {
      return false;
    }
  }
  return true;
}

std::optional<std::vector<NativeRecoveredPrototypeParam>>
providerDeclarationInputSuffix(
    llvm::ArrayRef<NativeRecoveredPrototypeParam> existingInputs,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> providerInputs) {
  if (existingInputs.size() >= providerInputs.size()) {
    return std::nullopt;
  }
  for (uint64_t index = 0; index < existingInputs.size(); ++index) {
    if (!sameDeclarationInputParam(existingInputs[index],
                                   providerInputs[index])) {
      return std::nullopt;
    }
  }
  return std::vector<NativeRecoveredPrototypeParam>(
      providerInputs.begin() + existingInputs.size(), providerInputs.end());
}

bool declarationTypeMatchesProviderInputs(
    const llvm::Function &callee,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> inputs,
    const NativePrototypeModel &model) {
  if (callee.arg_size() != inputs.size()) {
    return false;
  }
  for (uint64_t index = 0; index < inputs.size(); ++index) {
    if (callee.getFunctionType()->getParamType(index) !=
            llvm::Type::getInt64Ty(callee.getContext()) ||
        !declarationInputParamMatchesAbi(inputs[index], model)) {
      return false;
    }
  }
  return true;
}

void annotateKnownExternalDeclarationPrototypes(
    llvm::Module &module, const NativeAbiSpec &abi,
    const NativePrototypeModel &model) {
  for (llvm::Function &callee : module) {
    std::optional<std::vector<NativeRecoveredPrototypeParam>> providerInputs =
        knownExternalPrototypeInputs(callee, abi, model);
    if (!providerInputs ||
        !declarationTypeMatchesProviderInputs(callee, *providerInputs, model)) {
      continue;
    }

    std::optional<NativeRecoveredPrototype> oldPrototype =
        readNativeRecoveredPrototypeMetadata(callee);
    if (oldPrototype && (!oldPrototype->Returns.empty() ||
                         oldPrototype->ModelName != model.modelName() ||
                         oldPrototype->Inputs.size() != providerInputs->size())) {
      continue;
    }
    if (oldPrototype) {
      bool sameInputs = true;
      for (uint64_t index = 0; index < providerInputs->size(); ++index) {
        if (!sameDeclarationInputParam(oldPrototype->Inputs[index],
                                       (*providerInputs)[index])) {
          sameInputs = false;
          break;
        }
      }
      if (!sameInputs) {
        continue;
      }
    }

    NativeRecoveredPrototype prototype;
    prototype.ModelName = model.modelName();
    prototype.Inputs = *providerInputs;
    callee.setMetadata(
        "notdec.prototype.recovered",
        recoveredPrototypeMetadata(module.getContext(), prototype));
  }
}

std::optional<DeclarationCallInputRewrite> declarationCallInputRewriteForCall(
    llvm::CallInst &call, uint64_t existingArgumentCount,
    llvm::ArrayRef<NativeRecoveredPrototypeParam> inputs) {
  DeclarationCallInputRewrite rewrite;
  rewrite.Call = &call;
  rewrite.Inputs.assign(inputs.begin(), inputs.end());
  rewrite.ExistingArgumentCount = existingArgumentCount;
  rewrite.Arguments.reserve(existingArgumentCount + inputs.size());
  rewrite.InputStores.reserve(existingArgumentCount + inputs.size());
  for (uint64_t index = 0; index < existingArgumentCount; ++index) {
    rewrite.Arguments.push_back(call.getArgOperand(index));
    rewrite.InputStores.push_back(nullptr);
  }
  for (const NativeRecoveredPrototypeParam &input : inputs) {
    llvm::Type *paramType = llvm::Type::getInt64Ty(call.getContext());
    std::optional<llvm::Value *> argument =
        callsiteInputValueBeforeCall(call, input, paramType);
    if (!argument) {
      argument = callInputCandidateValueBeforeCall(call, input, paramType);
    }
    if (!argument) {
      return std::nullopt;
    }
    llvm::StoreInst *inputStore =
        localCallsiteInputStoreBeforeCall(call, input.RegisterName, paramType);
    if (inputStore != nullptr &&
        (inputStore->getValueOperand() != *argument ||
         !callClobbersRegister(call, input.RegisterName))) {
      inputStore = nullptr;
    }
    rewrite.Arguments.push_back(*argument);
    rewrite.InputStores.push_back(inputStore);
  }
  return rewrite;
}

DeclarationCallInputRewrites collectDeclarationCallInputRewrites(
    llvm::Module &module, const NativeAbiSpec &abi,
    const NativePrototypeModel &model) {
  DeclarationCallInputRewrites rewrites;
  for (llvm::Function &callee : module) {
    if (!callee.isDeclaration() || callee.isIntrinsic()) {
      continue;
    }

    std::optional<std::vector<NativeRecoveredPrototypeParam>> providerInputs =
        knownExternalPrototypeInputs(callee, abi, model);
    if (providerInputs) {
      std::optional<std::vector<NativeRecoveredPrototypeParam>> existingInputs =
          existingDeclarationInputs(callee, model);
      if (!existingInputs) {
        continue;
      }
      std::optional<std::vector<NativeRecoveredPrototypeParam>> suffix =
          providerDeclarationInputSuffix(*existingInputs, *providerInputs);
      if (!suffix) {
        continue;
      }

      std::vector<DeclarationCallInputRewrite> callRewrites;
      bool safe = true;
      for (llvm::User *user : callee.users()) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(user);
        if (call == nullptr || call->getCalledFunction() != &callee ||
            call->arg_size() != callee.arg_size() ||
            call->getType() != callee.getReturnType()) {
          safe = false;
          break;
        }
        std::vector<NativeRecoveredPrototypeParam> inferredInputs =
            declarationInputParamsBeforeCall(*call, model);
        if (!inferredDeclarationInputsFitProvider(inferredInputs,
                                                  *providerInputs)) {
          safe = false;
          break;
        }
        std::optional<DeclarationCallInputRewrite> rewrite =
            declarationCallInputRewriteForCall(*call, existingInputs->size(),
                                               *suffix);
        if (!rewrite) {
          safe = false;
          break;
        }
        rewrite->Inputs = *providerInputs;
        callRewrites.push_back(std::move(*rewrite));
      }
      if (safe && !callRewrites.empty()) {
        rewrites[&callee] = std::move(callRewrites);
      }
      continue;
    }

    std::vector<std::pair<llvm::CallInst *, std::vector<NativeRecoveredPrototypeParam>>>
        callInputSets;
    std::vector<DeclarationCallInputRewrite> callRewrites;
    bool safe = true;
    for (llvm::User *user : callee.users()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (call == nullptr || call->getCalledFunction() != &callee ||
          call->arg_size() != callee.arg_size() ||
          call->getType() != callee.getReturnType()) {
        safe = false;
        break;
      }

      std::vector<NativeRecoveredPrototypeParam> inputs =
          declarationInputParamsBeforeCall(*call, model);
      if (inputs.empty()) {
        safe = false;
        break;
      }
      callInputSets.push_back({call, std::move(inputs)});
    }
    if (!safe || callInputSets.empty()) {
      continue;
    }

    std::optional<std::vector<NativeRecoveredPrototypeParam>> existingInputs =
        existingDeclarationInputs(callee, model);
    if (!existingInputs) {
      continue;
    }

    std::vector<std::vector<NativeRecoveredPrototypeParam>> inputSuffixes;
    inputSuffixes.reserve(callInputSets.size());
    safe = true;
    for (const auto &[call, inputs] : callInputSets) {
      std::optional<std::vector<NativeRecoveredPrototypeParam>> suffix =
          declarationInputSuffix(*existingInputs, inputs);
      if (!suffix || suffix->empty()) {
        safe = false;
        break;
      }
      inputSuffixes.push_back(std::move(*suffix));
    }
    if (!safe) {
      continue;
    }

    std::vector<std::vector<NativeRecoveredPrototypeParam>> inputSets;
    inputSets.reserve(callInputSets.size());
    for (const std::vector<NativeRecoveredPrototypeParam> &inputs :
         inputSuffixes) {
      inputSets.push_back(inputs);
    }
    std::vector<NativeRecoveredPrototypeParam> commonSuffix =
        existingInputs->empty() ? commonDeclarationInputPrefix(inputSets)
                                : commonDeclarationInputSlots(inputSets);
    if (commonSuffix.empty()) {
      continue;
    }
    std::vector<NativeRecoveredPrototypeParam> rewrittenInputs =
        *existingInputs;
    rewrittenInputs.insert(rewrittenInputs.end(), commonSuffix.begin(),
                           commonSuffix.end());

    for (auto &[call, inputs] : callInputSets) {
      std::optional<DeclarationCallInputRewrite> rewrite =
          declarationCallInputRewriteForCall(*call, existingInputs->size(),
                                             commonSuffix);
      if (!rewrite) {
        safe = false;
        break;
      }
      rewrite->Inputs = rewrittenInputs;
      callRewrites.push_back(std::move(*rewrite));
    }
    if (safe && !callRewrites.empty()) {
      rewrites[&callee] = std::move(callRewrites);
    }
  }
  return rewrites;
}

void rewriteDeclarationCallInputs(llvm::Module &module,
                                  const NativeAbiSpec &abi,
                                  const NativePrototypeModel &model) {
  while (true) {
    DeclarationCallInputRewrites rewrites =
        collectDeclarationCallInputRewrites(module, abi, model);
    if (rewrites.empty()) {
      break;
    }
    for (auto &[callee, callRewrites] : rewrites) {
      const std::vector<NativeRecoveredPrototypeParam> &inputs =
          callRewrites.front().Inputs;
      std::string originalName = callee->getName().str();
      callee->setName(originalName + ".old");

      std::vector<llvm::Type *> paramTypes(
          inputs.size(), llvm::Type::getInt64Ty(module.getContext()));
      auto *newType =
          llvm::FunctionType::get(callee->getReturnType(), paramTypes, false);
      llvm::Function *rewritten =
          llvm::Function::Create(newType, callee->getLinkage(), originalName,
                                 module);
      rewritten->copyMetadata(callee, 0);
      rewritten->setCallingConv(callee->getCallingConv());
      NativeRecoveredPrototype prototype;
      prototype.ModelName = model.modelName();
      prototype.Inputs = inputs;
      rewritten->setMetadata(
          "notdec.prototype.recovered",
          recoveredPrototypeMetadata(module.getContext(), prototype));

      for (const DeclarationCallInputRewrite &rewrite : callRewrites) {
        llvm::IRBuilder<> builder(rewrite.Call);
        llvm::CallInst *newCall = builder.CreateCall(
            rewritten->getFunctionType(), rewritten, rewrite.Arguments);
        newCall->setCallingConv(rewrite.Call->getCallingConv());
        if (!rewrite.Call->getType()->isVoidTy()) {
          rewrite.Call->replaceAllUsesWith(newCall);
        }
        eraseCallsiteInputStores(rewrite.InputStores);
        rewrite.Call->eraseFromParent();
      }
      callee->eraseFromParent();
    }
  }
}

void eraseRewrittenInternalCallInputStores(llvm::Module &module) {
  for (llvm::Function &callee : module) {
    if (callee.isDeclaration()) {
      continue;
    }
    std::optional<NativeRecoveredPrototype> prototype =
        readNativeRecoveredPrototypeMetadata(callee);
    if (!prototype || prototype->Inputs.empty() ||
        callee.arg_size() != prototype->Inputs.size()) {
      continue;
    }
    for (llvm::User *user : llvm::make_early_inc_range(callee.users())) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (call == nullptr || call->getCalledFunction() != &callee ||
          call->arg_size() != prototype->Inputs.size()) {
        continue;
      }
      std::vector<llvm::StoreInst *> inputStores;
      inputStores.reserve(prototype->Inputs.size());
      bool safe = true;
      for (uint64_t index = 0; index < prototype->Inputs.size(); ++index) {
        const NativeRecoveredPrototypeParam &input = prototype->Inputs[index];
        if (input.StorageKind != "register") {
          inputStores.push_back(nullptr);
          continue;
        }
        llvm::StoreInst *store = localCallsiteInputStoreBeforeCall(
            *call, input.RegisterName, call->getArgOperand(index)->getType());
        if (store == nullptr ||
            store->getValueOperand() != call->getArgOperand(index) ||
            !callClobbersRegister(*call, input.RegisterName)) {
          safe = false;
          break;
        }
        inputStores.push_back(store);
      }
      if (safe) {
        eraseCallsiteInputStores(inputStores);
      }
    }
  }
}

bool hasUnsafeReturnValueLoad(
    llvm::ArrayRef<NativePrototypeReturnBinding> returnBindings) {
  // A return value that is still a register load can also be a direct
  // callsite's old return load.  Batch rewriting another callee may erase that
  // load later, so skip it until rewrite ordering tracks that dependency.
  for (const NativePrototypeReturnBinding &binding : returnBindings) {
    auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(binding.ReturnValue);
    if (load != nullptr && load->getMetadata("notdec.register.access") != nullptr &&
        load->getMetadata("notdec.register.external_input") == nullptr &&
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
        truncLoad->getMetadata("notdec.register.external_input") == nullptr &&
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

bool callClobbersRegister(llvm::CallBase &call, llvm::StringRef registerName);

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

std::optional<llvm::Value *> callInputCandidateValueBeforeCall(
    llvm::CallInst &call, const NativeRecoveredPrototypeParam &input,
    llvm::Type *paramType) {
  for (auto iter = llvm::BasicBlock::reverse_iterator(call.getIterator()),
            end = call.getParent()->rend();
       iter != end; ++iter) {
    auto *inst = llvm::dyn_cast<llvm::Instruction>(&*iter);
    if (inst == nullptr) {
      continue;
    }
    llvm::MDNode *metadata =
        inst->getMetadata("notdec.register.call_input_candidate");
    if (metadata != nullptr &&
        metadataField(*metadata, "register") == input.RegisterName) {
      llvm::Value *value = inst;
      if (auto *candidateCall = llvm::dyn_cast<llvm::CallBase>(inst)) {
        if (isNotDecRegisterHelperCall(*candidateCall) &&
            candidateCall->arg_size() == 1) {
          value = candidateCall->getArgOperand(0);
        }
      }
      if (value->getType() != paramType) {
        return std::nullopt;
      }
      return value;
    }

    if (auto *previousCall = llvm::dyn_cast<llvm::CallBase>(inst)) {
      if (isNotDecRegisterHelperCall(*previousCall)) {
        continue;
      }
      llvm::Function *callee = previousCall->getCalledFunction();
      if (callee == nullptr || !callee->isIntrinsic()) {
        return std::nullopt;
      }
      continue;
    }
  }
  return std::nullopt;
}

struct LocalCallsiteInputStoreLookup {
  bool Blocked = false;
  llvm::StoreInst *Store = nullptr;
};

// Treat duplicate CFG edges as one edge. Some lifted branches use
// `br i1 false, label %x, label %x`; that is still a linear path for this
// cleanup.
llvm::BasicBlock *uniqueEdgeTarget(llvm::iterator_range<
                                   llvm::succ_iterator> blocks) {
  llvm::BasicBlock *result = nullptr;
  for (llvm::BasicBlock *block : blocks) {
    if (result != nullptr && result != block) {
      return nullptr;
    }
    result = block;
  }
  return result;
}

llvm::BasicBlock *uniqueEdgeSource(llvm::iterator_range<
                                   llvm::pred_iterator> blocks) {
  llvm::BasicBlock *result = nullptr;
  for (llvm::BasicBlock *block : blocks) {
    if (result != nullptr && result != block) {
      return nullptr;
    }
    result = block;
  }
  return result;
}

// The callsite cleanup needs to distinguish "no store yet" from "a load/call
// made the search unsafe"; both used to be represented as nullptr.
llvm::StoreInst *localCallsiteInputStoreBeforeCall(
    llvm::CallInst &call, llvm::StringRef registerName, llvm::Type *valueType) {
  auto findLocalStore = [&](llvm::BasicBlock::reverse_iterator iter,
                            llvm::BasicBlock::reverse_iterator end)
      -> LocalCallsiteInputStoreLookup {
    for (; iter != end; ++iter) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&*iter)) {
        llvm::MDNode *metadata = load->getMetadata("notdec.register.access");
        if (metadata != nullptr &&
            metadataField(*metadata, "name") == registerName) {
          return {true, nullptr};
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iter)) {
        llvm::MDNode *metadata = store->getMetadata("notdec.register.access");
        if (metadata == nullptr ||
            metadataField(*metadata, "name") != registerName) {
          continue;
        }
        llvm::Value *value = store->getValueOperand();
        if (value == nullptr || value->getType() != valueType) {
          return {true, nullptr};
        }
        return {false, store};
      }
      if (auto *previousCall = llvm::dyn_cast<llvm::CallBase>(&*iter)) {
        if (isNotDecRegisterHelperCall(*previousCall)) {
          continue;
        }
        llvm::Function *callee = previousCall->getCalledFunction();
        if (callee == nullptr || !callee->isIntrinsic()) {
          return {true, nullptr};
        }
      }
    }
    return {};
  };

  LocalCallsiteInputStoreLookup local =
      findLocalStore(llvm::BasicBlock::reverse_iterator(call.getIterator()),
                     call.getParent()->rend());
  if (local.Blocked || local.Store != nullptr) {
    return local.Store;
  }

  llvm::BasicBlock *current = call.getParent();
  std::set<llvm::BasicBlock *> visited;
  while (visited.insert(current).second) {
    llvm::BasicBlock *predecessor =
        uniqueEdgeSource(llvm::predecessors(current));
    if (predecessor == nullptr) {
      return nullptr;
    }

    if (uniqueEdgeTarget(llvm::successors(predecessor)) != current) {
      return nullptr;
    }

    LocalCallsiteInputStoreLookup predecessorStore =
        findLocalStore(predecessor->rbegin(), predecessor->rend());
    if (predecessorStore.Blocked || predecessorStore.Store != nullptr) {
      return predecessorStore.Store;
    }
    current = predecessor;
  }
  return nullptr;
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

llvm::MDNode *registerExternalInputMetadata(llvm::GlobalVariable &global,
                                            llvm::StringRef registerName) {
  llvm::LLVMContext &context = global.getContext();
  llvm::Metadata *metadata[] = {
      llvm::MDString::get(context, ("name=" + registerName).str()),
      llvm::ValueAsMetadata::get(&global),
  };
  return llvm::MDNode::get(context, metadata);
}

bool functionExternalInputsHasRegister(const llvm::Function &function,
                                       llvm::StringRef registerName) {
  llvm::MDNode *node = function.getMetadata("notdec.register.external_inputs");
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

void ensureFunctionExternalInputMetadata(llvm::Function &function,
                                         llvm::StringRef registerName,
                                         llvm::GlobalVariable &global) {
  if (functionExternalInputsHasRegister(function, registerName)) {
    return;
  }

  std::vector<llvm::Metadata *> entries;
  if (llvm::MDNode *old =
          function.getMetadata("notdec.register.external_inputs")) {
    for (const llvm::MDOperand &operand : old->operands()) {
      if (operand.get() != nullptr) {
        entries.push_back(operand.get());
      }
    }
  }
  entries.push_back(registerExternalInputMetadata(global, registerName));
  function.setMetadata("notdec.register.external_inputs",
                       llvm::MDNode::get(function.getContext(), entries));
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
    llvm::CallInst &call, llvm::StringRef registerName, llvm::Type *paramType,
    bool isCallerEntryValue = false) {
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
  if (isCallerEntryValue) {
    load->setMetadata("notdec.register.external_input",
                      registerExternalInputMetadata(**global, registerName));
    ensureFunctionExternalInputMetadata(*call.getFunction(), registerName,
                                        **global);
  } else if (llvm::MDNode *metadata = registerGlobalAccessMetadata(**global)) {
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
        return registerGlobalValueBeforeCall(call, registerName, paramType,
                                             true);
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

std::optional<llvm::LoadInst *> stackInputLoadInReverseRange(
    llvm::BasicBlock::reverse_iterator iter, llvm::BasicBlock::reverse_iterator end,
    llvm::StringRef space, uint64_t offset, uint32_t size,
    llvm::Type *paramType) {
  llvm::LoadInst *result = nullptr;
  for (; iter != end; ++iter) {
    if (auto *previousCall = llvm::dyn_cast<llvm::CallBase>(&*iter)) {
      if (isNotDecRegisterHelperCall(*previousCall)) {
        continue;
      }
      llvm::Function *callee = previousCall->getCalledFunction();
      if (callee == nullptr || !callee->isIntrinsic()) {
        return std::nullopt;
      }
      continue;
    }
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&*iter);
    if (load == nullptr) {
      continue;
    }
    llvm::MDNode *metadata = load->getMetadata("notdec.stack.input");
    if (metadata == nullptr ||
        !stackInputMetadataMatches(*metadata, space, offset, size)) {
      continue;
    }
    if (load->getType() != paramType || result != nullptr) {
      return std::nullopt;
    }
    result = load;
  }
  if (result == nullptr) {
    return std::nullopt;
  }
  return result;
}

std::optional<llvm::Value *> localStackInputValueBeforeCall(
    llvm::CallInst &call, llvm::StringRef space, uint64_t offset, uint32_t size,
    llvm::Type *paramType) {
  return stackInputLoadInReverseRange(
      llvm::BasicBlock::reverse_iterator(call.getIterator()),
      call.getParent()->rend(), space, offset, size, paramType);
}

std::optional<llvm::Value *> stackInputValueAtBlockExit(
    llvm::BasicBlock &block, llvm::StringRef space, uint64_t offset,
    uint32_t size, llvm::Type *paramType,
    std::set<llvm::BasicBlock *> &visited) {
  std::optional<llvm::LoadInst *> local = stackInputLoadInReverseRange(
      block.rbegin(), block.rend(), space, offset, size, paramType);
  if (local) {
    return *local;
  }
  if (hasCallInReverseRange(block.rbegin(), block.rend())) {
    return std::nullopt;
  }
  if (!visited.insert(&block).second) {
    return std::nullopt;
  }

  llvm::BasicBlock *predecessor = nullptr;
  for (llvm::BasicBlock *candidate : llvm::predecessors(&block)) {
    if (predecessor != nullptr) {
      return std::nullopt;
    }
    predecessor = candidate;
  }
  if (predecessor == nullptr) {
    return std::nullopt;
  }
  return stackInputValueAtBlockExit(*predecessor, space, offset, size,
                                    paramType, visited);
}

std::optional<llvm::Value *> equivalentStackInputValueFromPredecessors(
    llvm::BasicBlock &block, llvm::StringRef space, uint64_t offset,
    uint32_t size, llvm::Type *paramType) {
  llvm::Value *result = nullptr;
  uint64_t predecessorCount = 0;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    ++predecessorCount;
    std::set<llvm::BasicBlock *> visited;
    std::optional<llvm::Value *> value = stackInputValueAtBlockExit(
        *predecessor, space, offset, size, paramType, visited);
    if (!value) {
      return std::nullopt;
    }
    if (result == nullptr) {
      result = *value;
      continue;
    }
    if (result != *value) {
      return std::nullopt;
    }
  }
  if (predecessorCount < 2) {
    return std::nullopt;
  }
  return result;
}

std::optional<llvm::Value *> stackInputValueBeforeCall(
    llvm::CallInst &call, llvm::StringRef space, uint64_t offset, uint32_t size,
    llvm::Type *paramType) {
  if (std::optional<llvm::Value *> local =
          localStackInputValueBeforeCall(call, space, offset, size, paramType)) {
    return local;
  }
  if (hasCallInReverseRange(llvm::BasicBlock::reverse_iterator(call.getIterator()),
                            call.getParent()->rend())) {
    return std::nullopt;
  }

  std::set<llvm::BasicBlock *> visited;
  llvm::BasicBlock *current = call.getParent();
  while (visited.insert(current).second) {
    llvm::BasicBlock *predecessor = nullptr;
    for (llvm::BasicBlock *candidate : llvm::predecessors(current)) {
      if (predecessor != nullptr) {
        if (current == call.getParent()) {
          return equivalentStackInputValueFromPredecessors(
              *current, space, offset, size, paramType);
        }
        return std::nullopt;
      }
      predecessor = candidate;
    }
    if (predecessor == nullptr) {
      return std::nullopt;
    }
    std::optional<llvm::LoadInst *> load = stackInputLoadInReverseRange(
        predecessor->rbegin(), predecessor->rend(), space, offset, size,
        paramType);
    if (load) {
      return *load;
    }
    if (hasCallInReverseRange(predecessor->rbegin(), predecessor->rend())) {
      return std::nullopt;
    }
    current = predecessor;
  }
  return std::nullopt;
}

std::optional<llvm::Value *> callsiteInputValueBeforeCall(
    llvm::CallInst &call, const NativeRecoveredPrototypeParam &input,
    llvm::Type *paramType) {
  if (input.StorageKind == "register") {
    return callsiteInputValueBeforeCall(call, input.RegisterName, paramType);
  }
  if (input.StorageKind == "stack") {
    return stackInputValueBeforeCall(call, input.StackSpace, input.StackOffset,
                                     input.Size, paramType);
  }
  return std::nullopt;
}

// Multi-input form of Ghidra FuncCallSpecs::buildInputFromTrials(...): keep
// the call and ABI-ordered argument values together so the old void call can be
// replaced after every callsite has been checked.
struct MultiInputCallsiteRewrite {
  llvm::CallInst *Call = nullptr;
  std::vector<llvm::Value *> Arguments;
  // Same ABI order as Arguments.  Null means the argument did not come from a
  // local store that is safe to erase after the call is rewritten.
  std::vector<llvm::StoreInst *> InputStores;
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
    rewrite.InputStores.reserve(inputs.size());
    for (uint64_t index = 0; index < inputs.size(); ++index) {
      std::optional<llvm::Value *> argument = callsiteInputValueBeforeCall(
          *call, inputs[index], recoveredType.getParamType(index));
      if (!argument) {
        result.FailureReason = "unsafe callsite input value";
        return result;
      }
      rewrite.Arguments.push_back(*argument);
      llvm::StoreInst *inputStore = nullptr;
      if (inputs[index].StorageKind == "register") {
        inputStore = localCallsiteInputStoreBeforeCall(
            *call, inputs[index].RegisterName, recoveredType.getParamType(index));
        if (inputStore != nullptr &&
            (inputStore->getValueOperand() != *argument ||
             call->getFunction()->getMetadata(
                 "notdec.prototype.return_candidates") != nullptr ||
             !callClobbersRegister(*call, inputs[index].RegisterName))) {
          inputStore = nullptr;
        }
      }
      rewrite.InputStores.push_back(inputStore);
    }
    result.Rewrites.push_back(std::move(rewrite));
  }
  return result;
}

void eraseCallsiteInputStores(llvm::ArrayRef<llvm::StoreInst *> inputStores) {
  std::set<llvm::StoreInst *> erased;
  for (llvm::StoreInst *store : inputStores) {
    if (store != nullptr && erased.insert(store).second) {
      store->eraseFromParent();
    }
  }
}

void rewriteMultiInputDirectCallsites(
    llvm::Function &rewritten,
    llvm::ArrayRef<MultiInputCallsiteRewrite> callsites) {
  for (const MultiInputCallsiteRewrite &callsite : callsites) {
    llvm::IRBuilder<> builder(callsite.Call);
    llvm::CallInst *newCall = builder.CreateCall(
        rewritten.getFunctionType(), &rewritten, callsite.Arguments);
    newCall->setCallingConv(callsite.Call->getCallingConv());
    eraseCallsiteInputStores(callsite.InputStores);
    callsite.Call->eraseFromParent();
  }
}

bool registerNameMatchesEffect(llvm::StringRef accessName,
                               llvm::StringRef effectName) {
  std::string lanePrefix = (effectName + "_").str();
  return accessName == effectName || accessName.starts_with(lanePrefix);
}

bool accessMatchesEffectRegister(const llvm::MDNode &access,
                                 llvm::StringRef effectName) {
  if (std::optional<std::string> name = metadataField(access, "name")) {
    if (registerNameMatchesEffect(*name, effectName)) {
      return true;
    }
  }
  if (std::optional<std::string> base = metadataField(access, "base")) {
    if (registerNameMatchesEffect(*base, effectName)) {
      return true;
    }
  }
  return false;
}

bool isVectorRegisterName(llvm::StringRef name) {
  return name.starts_with("XMM") || name.starts_with("YMM") ||
         name.starts_with("ZMM");
}

bool accessIsVectorPartialStore(const llvm::MDNode &access,
                                llvm::StoreInst &store) {
  std::optional<std::string> name = metadataField(access, "name");
  std::optional<std::string> base = metadataField(access, "base");
  if ((!name || !isVectorRegisterName(*name)) &&
      (!base || !isVectorRegisterName(*base))) {
    return false;
  }

  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      store.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return false;
  }
  llvm::MDNode *globalMetadata = global->getMetadata("notdec.register");
  if (globalMetadata == nullptr) {
    return false;
  }
  std::optional<uint64_t> accessSize = parseUint64Field(access, "size");
  std::optional<uint64_t> unitSize =
      parseUint64Field(*globalMetadata, "size");
  return accessSize && unitSize && *accessSize < *unitSize;
}

bool accessMatchesRecoveredReturn(const llvm::MDNode &access,
                                  const NativeRecoveredPrototype &prototype) {
  for (const NativeRecoveredPrototypeParam &param : prototype.Returns) {
    if (param.StorageKind != "register") {
      continue;
    }
    if (accessMatchesEffectRegister(access, param.RegisterName)) {
      return true;
    }
  }
  return false;
}

bool prototypeHasRegisterInput(const NativeRecoveredPrototype &prototype,
                               llvm::StringRef registerName) {
  for (const NativeRecoveredPrototypeParam &param : prototype.Inputs) {
    if (param.StorageKind != "register") {
      continue;
    }
    if (registerNameMatchesEffect(param.RegisterName, registerName)) {
      return true;
    }
  }
  return false;
}

bool instructionReadsRegisterAccess(llvm::Instruction &instruction,
                                    const llvm::MDNode &access) {
  auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
  if (load == nullptr) {
    return false;
  }
  llvm::MDNode *loadAccess = load->getMetadata("notdec.register.access");
  if (loadAccess == nullptr) {
    return false;
  }
  if (std::optional<std::string> name = metadataField(access, "name")) {
    if (accessMatchesEffectRegister(*loadAccess, *name)) {
      return true;
    }
  }
  if (std::optional<std::string> base = metadataField(access, "base")) {
    if (accessMatchesEffectRegister(*loadAccess, *base)) {
      return true;
    }
  }
  return false;
}

bool loadReadsRegisterName(llvm::LoadInst &load, llvm::StringRef registerName) {
  if (llvm::MDNode *access = load.getMetadata("notdec.register.access")) {
    if (accessMatchesEffectRegister(*access, registerName)) {
      return true;
    }
  }
  if (llvm::MDNode *externalInput =
          load.getMetadata("notdec.register.external_input")) {
    if (accessMatchesEffectRegister(*externalInput, registerName)) {
      return true;
    }
  }
  return false;
}

bool storeWritesRegisterName(llvm::StoreInst &store,
                             llvm::StringRef registerName) {
  if (llvm::MDNode *access = store.getMetadata("notdec.register.access")) {
    if (accessMatchesEffectRegister(*access, registerName)) {
      return true;
    }
  }
  return false;
}

bool functionReadsRegisterName(llvm::Function &function,
                               llvm::StringRef registerName) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load != nullptr && loadReadsRegisterName(*load, registerName)) {
        return true;
      }
    }
  }
  return false;
}

bool functionMayTouchRegisterName(llvm::Function &function,
                                  llvm::StringRef registerName) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (!load->use_empty() && loadReadsRegisterName(*load, registerName)) {
          return true;
        }
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (storeWritesRegisterName(*store, registerName)) {
          return true;
        }
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || !callee->isIntrinsic()) {
          return true;
        }
      }
    }
  }
  return false;
}

bool callMayReadRegisterName(llvm::CallBase &call,
                             llvm::StringRef registerName) {
  if (isNotDecRegisterHelperCall(call)) {
    return false;
  }
  if (registerName != "RSP" && !isFramePointerRegisterName(registerName)) {
    return true;
  }
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr) {
    return true;
  }
  if (callee->isIntrinsic()) {
    return false;
  }
  if (callee->getName() == "notdec_stack_canary_check") {
    return false;
  }
  if (callee->getName().starts_with("notdec_caller_frame_")) {
    return false;
  }
  if (callee->isDeclaration()) {
    if (registerName == "RSP" &&
        isNoReturnDeclarationWithExplicitArguments(*callee)) {
      return false;
    }
    return !(registerName == "RSP" &&
             isKnownNoStackArgumentDeclaration(*callee));
  }
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(*callee);
  if (prototype && prototypeHasRegisterInput(*prototype, registerName)) {
    return true;
  }
  return functionReadsRegisterName(*callee, registerName);
}

bool callMayReadRegisterNameForDeadFrameStore(llvm::CallBase &call,
                                              llvm::StringRef registerName) {
  if (!isFramePointerRegisterName(registerName)) {
    return callMayReadRegisterName(call, registerName);
  }
  llvm::Function *callee = call.getCalledFunction();
  if (callee != nullptr && callee->isDeclaration()) {
    return false;
  }
  return callMayReadRegisterName(call, registerName);
}

bool callMayReadRegisterAccess(
    llvm::CallBase &call, const llvm::MDNode &access,
    llvm::function_ref<bool(llvm::CallBase &, llvm::StringRef)> mayReadName =
        callMayReadRegisterName) {
  if (std::optional<std::string> name = metadataField(access, "name")) {
    if (mayReadName(call, *name)) {
      return true;
    }
  }
  if (std::optional<std::string> base = metadataField(access, "base")) {
    if (mayReadName(call, *base)) {
      return true;
    }
  }
  return false;
}

bool callInvalidatesKnownFrameRegisterValue(llvm::CallBase &call,
                                            llvm::StringRef registerName) {
  if (isNotDecRegisterHelperCall(call)) {
    return false;
  }
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr) {
    return true;
  }
  if (callee->isIntrinsic()) {
    return false;
  }
  if (callee->isDeclaration()) {
    return true;
  }
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(*callee);
  if (prototype && prototypeHasRegisterInput(*prototype, registerName)) {
    return true;
  }
  return functionMayTouchRegisterName(*callee, registerName);
}

bool instructionWritesRegisterAccess(llvm::Instruction &instruction,
                                     const llvm::MDNode &access) {
  auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  if (store == nullptr) {
    return false;
  }
  llvm::MDNode *storeAccess = store->getMetadata("notdec.register.access");
  if (storeAccess == nullptr) {
    return false;
  }
  if (std::optional<std::string> name = metadataField(access, "name")) {
    if (accessMatchesEffectRegister(*storeAccess, *name)) {
      return true;
    }
  }
  if (std::optional<std::string> base = metadataField(access, "base")) {
    if (accessMatchesEffectRegister(*storeAccess, *base)) {
      return true;
    }
  }
  return false;
}

bool isFunctionExitInstruction(llvm::Instruction *instruction) {
  return llvm::isa<llvm::ReturnInst>(instruction) ||
         llvm::isa<llvm::UnreachableInst>(instruction);
}

bool allSuccessorsReachReturnWithoutCallOrAccessLoad(
    llvm::BasicBlock &block, const llvm::MDNode &access,
    std::set<llvm::BasicBlock *> &seen,
    llvm::function_ref<bool(llvm::CallBase &, llvm::StringRef)> mayReadName);

bool reachesReturnWithoutCallOrAccessLoad(llvm::Instruction *instruction,
                                          llvm::BasicBlock &block,
                                          const llvm::MDNode &access,
                                          std::set<llvm::BasicBlock *> &seen,
                                          llvm::function_ref<bool(
                                              llvm::CallBase &,
                                              llvm::StringRef)> mayReadName) {
  while (instruction != nullptr) {
    if (isFunctionExitInstruction(instruction)) {
      return true;
    }
    if (instructionWritesRegisterAccess(*instruction, access)) {
      return true;
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(instruction)) {
      if (callMayReadRegisterAccess(*call, access, mayReadName)) {
        return false;
      }
    }
    if (instructionReadsRegisterAccess(*instruction, access)) {
      return false;
    }
    instruction = instruction->getNextNode();
  }
  return allSuccessorsReachReturnWithoutCallOrAccessLoad(block, access, seen,
                                                        mayReadName);
}

bool allSuccessorsReachReturnWithoutCallOrAccessLoad(
    llvm::BasicBlock &block, const llvm::MDNode &access,
    std::set<llvm::BasicBlock *> &seen,
    llvm::function_ref<bool(llvm::CallBase &, llvm::StringRef)> mayReadName) {
  llvm::Instruction *terminator = block.getTerminator();
  if (terminator == nullptr) {
    return false;
  }
  if (isFunctionExitInstruction(terminator)) {
    return true;
  }
  if (auto *call = llvm::dyn_cast<llvm::CallBase>(terminator)) {
    if (callMayReadRegisterAccess(*call, access, mayReadName)) {
      return false;
    }
  }

  bool sawSuccessor = false;
  for (llvm::BasicBlock *successor : llvm::successors(&block)) {
    sawSuccessor = true;
    std::set<llvm::BasicBlock *> pathSeen = seen;
    if (!pathSeen.insert(successor).second) {
      return false;
    }
    if (!reachesReturnWithoutCallOrAccessLoad(
            successor->empty() ? nullptr : &successor->front(), *successor,
            access, pathSeen, mayReadName)) {
      return false;
    }
  }
  return sawSuccessor;
}

bool storeIsDeadOnAllReturnPaths(
    llvm::StoreInst &store, const llvm::MDNode &access,
    llvm::function_ref<bool(llvm::CallBase &, llvm::StringRef)> mayReadName =
        callMayReadRegisterName) {
  std::set<llvm::BasicBlock *> seen;
  seen.insert(store.getParent());
  llvm::Instruction *next = store.getNextNode();
  while (next != nullptr) {
    if (isFunctionExitInstruction(next)) {
      return true;
    }
    if (instructionWritesRegisterAccess(*next, access)) {
      return true;
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(next)) {
      if (callMayReadRegisterAccess(*call, access, mayReadName)) {
        return false;
      }
    }
    if (instructionReadsRegisterAccess(*next, access)) {
      return false;
    }
    next = next->getNextNode();
  }
  return allSuccessorsReachReturnWithoutCallOrAccessLoad(*store.getParent(),
                                                         access, seen,
                                                         mayReadName);
}

bool valueIsNeededByInterveningInstruction(llvm::StoreInst &store,
                                           llvm::CallInst &call,
                                           const llvm::MDNode &access) {
  if (store.getFunction() != call.getFunction()) {
    return true;
  }

  llvm::BasicBlock *block = store.getParent();
  llvm::Instruction *instruction = store.getNextNode();
  std::set<llvm::BasicBlock *> visited;
  while (block != nullptr && visited.insert(block).second) {
    while (instruction != nullptr) {
      if (instruction == &call) {
        return false;
      }
      if (llvm::isa<llvm::CallBase>(instruction) ||
          instructionReadsRegisterAccess(*instruction, access) ||
          instructionWritesRegisterAccess(*instruction, access)) {
        return true;
      }
      instruction = instruction->getNextNode();
    }

    if (block == call.getParent()) {
      return true;
    }
    block = uniqueEdgeTarget(llvm::successors(block));
    instruction =
        block != nullptr && !block->empty() ? &block->front() : nullptr;
  }
  return true;
}

bool canEraseUnusedInternalCallKilledStore(llvm::StoreInst &store,
                                           llvm::CallInst &call,
                                           llvm::Function &callee,
                                           llvm::StringRef registerName) {
  if (!callClobbersRegister(call, registerName)) {
    return false;
  }
  if (valueIsNeededByInterveningInstruction(store, call,
                                            *store.getMetadata(
                                                "notdec.register.access"))) {
    return false;
  }
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(callee);
  if (prototype && prototypeHasRegisterInput(*prototype, registerName)) {
    return false;
  }
  return !functionReadsRegisterName(callee, registerName);
}

bool reachesReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
    llvm::Instruction *instruction, llvm::BasicBlock &block,
    const llvm::MDNode &access, std::set<llvm::BasicBlock *> &seen);

bool allSuccessorsReachReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
    llvm::BasicBlock &block, const llvm::MDNode &access,
    std::set<llvm::BasicBlock *> &seen) {
  llvm::Instruction *terminator = block.getTerminator();
  if (terminator == nullptr) {
    return false;
  }
  if (isFunctionExitInstruction(terminator)) {
    return true;
  }
  if (auto *call = llvm::dyn_cast<llvm::CallBase>(terminator)) {
    if (callMayReadRegisterAccess(*call, access)) {
      return false;
    }
  }

  bool sawSuccessor = false;
  for (llvm::BasicBlock *successor : llvm::successors(&block)) {
    sawSuccessor = true;
    std::set<llvm::BasicBlock *> pathSeen = seen;
    if (!pathSeen.insert(successor).second) {
      return false;
    }
    if (!reachesReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
            successor->empty() ? nullptr : &successor->front(), *successor,
            access, pathSeen)) {
      return false;
    }
  }
  return sawSuccessor;
}

bool reachesReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
    llvm::Instruction *instruction, llvm::BasicBlock &block,
    const llvm::MDNode &access, std::set<llvm::BasicBlock *> &seen) {
  while (instruction != nullptr) {
    if (isFunctionExitInstruction(instruction)) {
      return true;
    }
    if (instructionWritesRegisterAccess(*instruction, access)) {
      return true;
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(instruction)) {
      if (callMayReadRegisterAccess(*call, access)) {
        return false;
      }
    }
    if (instructionReadsRegisterAccess(*instruction, access)) {
      return false;
    }
    instruction = instruction->getNextNode();
  }
  return allSuccessorsReachReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
      block, access, seen);
}

bool storedRegisterValueIsDeadAfterCall(llvm::CallInst &call,
                                        const llvm::MDNode &access) {
  std::set<llvm::BasicBlock *> seen;
  seen.insert(call.getParent());
  if (llvm::Instruction *next = call.getNextNode()) {
    return reachesReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
        next, *call.getParent(), access, seen);
  }
  return allSuccessorsReachReturnOrOverwriteWithoutCallOrAccessLoadRecursive(
      *call.getParent(), access, seen);
}

bool canEraseUnusedInternalCallStackFrameRegisterStore(
    llvm::StoreInst &store, llvm::CallInst &call, llvm::Function &callee,
    llvm::StringRef registerName) {
  llvm::MDNode *access = store.getMetadata("notdec.register.access");
  if (access == nullptr || !accessMatchesEffectRegister(*access, registerName)) {
    return false;
  }
  if (valueIsNeededByInterveningInstruction(store, call, *access)) {
    return false;
  }
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(callee);
  if (prototype && prototypeHasRegisterInput(*prototype, registerName)) {
    return false;
  }
  if (functionReadsRegisterName(callee, registerName)) {
    return false;
  }
  return storedRegisterValueIsDeadAfterCall(call, *access);
}

bool prototypeHasStackInput(const NativeRecoveredPrototype &prototype) {
  for (const NativeRecoveredPrototypeParam &input : prototype.Inputs) {
    if (input.StorageKind == "stack") {
      return true;
    }
  }
  return false;
}

bool isKnownNoStackArgumentDeclaration(const llvm::Function &function) {
  if (!function.isDeclaration() || function.isVarArg() ||
      function.arg_size() != 0) {
    return false;
  }
  if (isKnownNoReturnDeclaration(function)) {
    return true;
  }
  return llvm::StringSwitch<bool>(function.getName())
      .Case("__gmon_start__", true)
      .Case("__errno_location", true)
      .Case("__stack_chk_fail", true)
      .Case("if_nametoindex", true)
      .Case("notdec_plt0_resolver", true)
      .Case("php_info_print_table_start", true)
      .Case("strerror", true)
      .Case("zend_wrong_param_count", true)
      .Default(false);
}

bool isKnownNoReturnDeclaration(const llvm::Function &function) {
  if (!function.isDeclaration() || function.isVarArg() ||
      function.arg_size() != 0) {
    return false;
  }
  if (function.hasFnAttribute(llvm::Attribute::NoReturn)) {
    return true;
  }
  return llvm::StringSwitch<bool>(function.getName())
      .Case("abort", true)
      .Case("__stack_chk_fail", true)
      .Default(false);
}

void truncateKnownNoReturnDeclarationCalls(llvm::Module &module) {
  std::vector<std::pair<llvm::Instruction *, llvm::Function *>> truncatePoints;
  for (llvm::Function &function : module.functions()) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr || call->getNextNode() == nullptr) {
          continue;
        }
        auto *callee = call->getCalledFunction();
        if (callee == nullptr || !isKnownNoReturnDeclaration(*callee)) {
          continue;
        }
        truncatePoints.push_back({call->getNextNode(), &function});
        break;
      }
    }
  }
  std::set<llvm::Function *> changedFunctions;
  for (auto [truncatePoint, function] : truncatePoints) {
    llvm::changeToUnreachable(truncatePoint);
    changedFunctions.insert(function);
  }
  for (llvm::Function *function : changedFunctions) {
    llvm::removeUnreachableBlocks(*function);
  }
}

bool isNoReturnDeclarationWithExplicitArguments(const llvm::Function &function) {
  if (!function.isDeclaration() || function.isVarArg()) {
    return false;
  }
  if (function.hasFnAttribute(llvm::Attribute::NoReturn)) {
    return true;
  }
  return llvm::StringSwitch<bool>(function.getName())
      .Case("__assert_fail", true)
      .Default(false);
}

bool isRawStackFrameReturnAddressStore(llvm::StoreInst &store,
                                       const NativeAbiSpec &abi) {
  if (store.isVolatile() || store.isAtomic()) {
    return false;
  }
  auto *storedValue = llvm::dyn_cast<llvm::ConstantInt>(store.getValueOperand());
  if (storedValue == nullptr || storedValue->getBitWidth() > 64) {
    return false;
  }
  auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(
      store.getPointerOperand()->stripPointerCasts());
  if (pointer == nullptr) {
    return false;
  }
  return valueIsStackFrameExternalInputDerived(*pointer->getOperand(0), abi);
}

void eraseNoReturnDeclarationCallFrameStores(llvm::Module &module,
                                             const NativeAbiSpec &abi) {
  std::vector<llvm::StoreInst *> deadStores;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr || call->arg_empty()) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr ||
            !isNoReturnDeclarationWithExplicitArguments(*callee)) {
          continue;
        }
        llvm::Instruction *previous = call->getPrevNode();
        if (previous == nullptr) {
          continue;
        }
        auto *store = llvm::dyn_cast<llvm::StoreInst>(previous);
        if (store != nullptr && isRawStackFrameReturnAddressStore(*store, abi)) {
          deadStores.push_back(store);
        }
      }
    }
  }

  std::set<llvm::StoreInst *> uniqueStores(deadStores.begin(), deadStores.end());
  for (llvm::StoreInst *store : uniqueStores) {
    llvm::Value *storedValue = store->getValueOperand();
    llvm::Value *pointer = store->getPointerOperand();
    store->eraseFromParent();
    llvm::RecursivelyDeleteTriviallyDeadInstructions(pointer);
    llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
  }
}

bool canEraseUnusedDeclarationCallStackFrameRegisterStore(
    llvm::StoreInst &store, llvm::CallInst &call, llvm::Function &callee,
    llvm::StringRef registerName) {
  llvm::MDNode *access = store.getMetadata("notdec.register.access");
  if (access == nullptr || !accessMatchesEffectRegister(*access, registerName)) {
    return false;
  }
  if (valueIsNeededByInterveningInstruction(store, call, *access)) {
    return false;
  }
  if (callee.isVarArg() || call.arg_size() != callee.arg_size()) {
    return false;
  }
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(callee);
  if (!prototype) {
    if (isFramePointerRegisterName(registerName)) {
      return storedRegisterValueIsDeadAfterCall(call, *access);
    }
    return registerName == "RSP" &&
           isKnownNoStackArgumentDeclaration(callee) &&
           storedRegisterValueIsDeadAfterCall(call, *access);
  }
  if (prototypeHasStackInput(*prototype) ||
      prototypeHasRegisterInput(*prototype, registerName) ||
      prototype->Inputs.size() != call.arg_size()) {
    return false;
  }
  return storedRegisterValueIsDeadAfterCall(call, *access);
}

std::set<std::string> killedByCallRegisterNames(const NativeAbiSpec &abi) {
  std::set<std::string> names;
  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind != NativeAbiEffectKind::KilledByCall ||
        effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty()) {
      continue;
    }
    names.insert(effect.Storage.Name);
  }
  return names;
}

void eraseDeadKilledByCallRegisterStores(llvm::Module &module,
                                         const NativeAbiSpec &abi) {
  std::set<std::string> killedRegisters = killedByCallRegisterNames(abi);
  if (killedRegisters.empty()) {
    return;
  }

  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    std::vector<llvm::StoreInst *> deadStores;
    for (const std::string &registerName : killedRegisters) {
      std::optional<NativeRecoveredPrototype> prototype =
          readNativeRecoveredPrototypeMetadata(function);
      for (llvm::BasicBlock &block : function) {
        for (llvm::Instruction &instruction : block) {
          auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          if (store == nullptr) {
            continue;
          }
          llvm::MDNode *access = store->getMetadata("notdec.register.access");
          if (access != nullptr &&
              accessMatchesEffectRegister(*access, registerName) &&
              (!prototype || !accessMatchesRecoveredReturn(*access, *prototype)) &&
              storeIsDeadOnAllReturnPaths(*store, *access)) {
            deadStores.push_back(store);
          }
        }
      }
    }

    std::set<llvm::StoreInst *> uniqueStores(deadStores.begin(),
                                            deadStores.end());
    for (llvm::StoreInst *store : uniqueStores) {
      llvm::Value *storedValue = store->getValueOperand();
      store->eraseFromParent();
      llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
    }
  }
}

void eraseUnusedInternalCallKilledInputStores(llvm::Module &module,
                                              const NativeAbiSpec &abi) {
  std::set<std::string> killedRegisters = killedByCallRegisterNames(abi);
  if (killedRegisters.empty()) {
    return;
  }

  std::vector<llvm::StoreInst *> deadStores;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || callee->isDeclaration()) {
          continue;
        }
        for (const std::string &registerName : killedRegisters) {
          llvm::StoreInst *store = localCallsiteInputStoreBeforeCall(
              *call, registerName, llvm::Type::getInt64Ty(module.getContext()));
          if (store == nullptr) {
            continue;
          }
          llvm::MDNode *access = store->getMetadata("notdec.register.access");
          if (access == nullptr ||
              !accessMatchesEffectRegister(*access, registerName)) {
            continue;
          }
          if (canEraseUnusedInternalCallKilledStore(*store, *call, *callee,
                                                   registerName)) {
            deadStores.push_back(store);
          }
        }
      }
    }
  }

  std::set<llvm::StoreInst *> uniqueStores(deadStores.begin(),
                                          deadStores.end());
  for (llvm::StoreInst *store : uniqueStores) {
    llvm::Value *storedValue = store->getValueOperand();
    store->eraseFromParent();
    llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
  }
}

void eraseUnusedDeclarationCallStackFrameRegisterStores(llvm::Module &module,
                                                        const NativeAbiSpec &abi) {
  std::set<std::string> registerNames = stackFrameRegisterNames(abi);
  if (registerNames.empty()) {
    return;
  }

  std::vector<llvm::StoreInst *> deadStores;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || !callee->isDeclaration() ||
            callee->isIntrinsic()) {
          continue;
        }
        for (const std::string &registerName : registerNames) {
          llvm::StoreInst *store = localCallsiteInputStoreBeforeCall(
              *call, registerName, llvm::Type::getInt64Ty(module.getContext()));
          if (store != nullptr &&
              canEraseUnusedDeclarationCallStackFrameRegisterStore(
                  *store, *call, *callee, registerName)) {
            deadStores.push_back(store);
          }
        }
      }
    }
  }

  std::set<llvm::StoreInst *> uniqueStores(deadStores.begin(),
                                          deadStores.end());
  for (llvm::StoreInst *store : uniqueStores) {
    llvm::Value *storedValue = store->getValueOperand();
    store->eraseFromParent();
    llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
  }
}

void eraseUnusedInternalCallStackFrameRegisterStores(llvm::Module &module,
                                                     const NativeAbiSpec &abi) {
  std::set<std::string> registerNames = stackFrameRegisterNames(abi);
  if (registerNames.empty()) {
    return;
  }

  std::vector<llvm::StoreInst *> deadStores;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || callee->isDeclaration()) {
          continue;
        }
        for (const std::string &registerName : registerNames) {
          llvm::StoreInst *store = localCallsiteInputStoreBeforeCall(
              *call, registerName, llvm::Type::getInt64Ty(module.getContext()));
          if (store != nullptr &&
              canEraseUnusedInternalCallStackFrameRegisterStore(
                  *store, *call, *callee, registerName)) {
            deadStores.push_back(store);
          }
        }
      }
    }
  }

  std::set<llvm::StoreInst *> uniqueStores(deadStores.begin(),
                                          deadStores.end());
  for (llvm::StoreInst *store : uniqueStores) {
    llvm::Value *storedValue = store->getValueOperand();
    store->eraseFromParent();
    llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
  }
}

bool isFramePointerRegisterName(llvm::StringRef registerName) {
  return registerName == "RBP" || registerName == "EBP" ||
         registerName == "BP";
}

bool hasExistingNotDecStackAlloca(const llvm::Function &function) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &instruction : block) {
      auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
      if (alloca != nullptr && alloca->hasName() &&
          alloca->getName().starts_with("notdec_stack")) {
        return true;
      }
    }
  }
  return false;
}

std::set<std::string> stackFrameRegisterNames(const NativeAbiSpec &abi) {
  std::set<std::string> names;
  if (!abi.StackPointerRegister.empty()) {
    names.insert(abi.StackPointerRegister);
  }
  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind != NativeAbiEffectKind::Unaffected ||
        effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty()) {
      continue;
    }
    if (isFramePointerRegisterName(effect.Storage.Name)) {
      names.insert(effect.Storage.Name);
    }
  }
  return names;
}

std::set<std::string> preservedNonStackFrameRegisterNames(
    const NativeAbiSpec &abi) {
  std::set<std::string> stackFrameNames = stackFrameRegisterNames(abi);
  std::set<std::string> names;
  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind != NativeAbiEffectKind::Unaffected ||
        effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty() ||
        stackFrameNames.count(effect.Storage.Name) != 0) {
      continue;
    }
    names.insert(effect.Storage.Name);
  }
  return names;
}

llvm::LoadInst *externalInputLoadForRegister(llvm::Function &function,
                                             llvm::StringRef registerName) {
  llvm::LoadInst *result = nullptr;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      llvm::MDNode *metadata =
          load->getMetadata("notdec.register.external_input");
      if (metadata == nullptr ||
          !accessMatchesEffectRegister(*metadata, registerName)) {
        continue;
      }
      if (result != nullptr) {
        return nullptr;
      }
      result = load;
    }
  }
  return result;
}

bool registerAccessMatchesName(const llvm::Instruction &instruction,
                               llvm::StringRef metadataName,
                               llvm::StringRef registerName) {
  llvm::MDNode *access = instruction.getMetadata(metadataName);
  return access != nullptr && accessMatchesEffectRegister(*access, registerName);
}

std::optional<llvm::Value *>
mergeKnownRegisterValueAtBlockEntry(llvm::BasicBlock &block,
                                    const std::map<llvm::BasicBlock *,
                                                   llvm::Value *> &blockOut) {
  llvm::Value *merged = nullptr;
  bool sawPredecessor = false;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    sawPredecessor = true;
    auto found = blockOut.find(predecessor);
    if (found == blockOut.end() || found->second == nullptr) {
      return std::nullopt;
    }
    if (merged == nullptr) {
      merged = found->second;
      continue;
    }
    if (merged != found->second) {
      return std::nullopt;
    }
  }
  if (!sawPredecessor) {
    return std::nullopt;
  }
  return merged;
}

llvm::Value *knownFrameRegisterValueAfterBlock(
    llvm::BasicBlock &block, llvm::Value *currentValue,
    llvm::StringRef frameRegisterName, llvm::StringRef stackRegisterName) {
  for (llvm::Instruction &instruction : block) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      if (!registerAccessMatchesName(*store, "notdec.register.access",
                                     frameRegisterName)) {
        continue;
      }
      llvm::Value *storedValue = store->getValueOperand();
      currentValue =
          valueUsesExternalInputRegister(*storedValue, stackRegisterName)
              ? storedValue
              : nullptr;
      continue;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr &&
        callInvalidatesKnownFrameRegisterValue(*call, frameRegisterName)) {
      currentValue = nullptr;
    }
  }
  return currentValue;
}

void replaceStoredFramePointerRegisterLoads(llvm::Function &function,
                                            llvm::StringRef frameRegisterName,
                                            llvm::StringRef stackRegisterName) {
  // Only propagate frame registers that were just stored from the local stack
  // pointer. External RBP-based frames stay as raw loads.
  std::map<llvm::BasicBlock *, llvm::Value *> blockOut;
  bool changed = true;
  size_t iterations = 0;
  size_t maxIterations = std::max<size_t>(1, function.size() * 4);
  while (changed && iterations++ < maxIterations) {
    changed = false;
    for (llvm::BasicBlock &block : function) {
      llvm::Value *entryValue = nullptr;
      if (std::optional<llvm::Value *> merged =
              mergeKnownRegisterValueAtBlockEntry(block, blockOut)) {
        entryValue = *merged;
      }
      llvm::Value *outValue = knownFrameRegisterValueAfterBlock(
          block, entryValue, frameRegisterName, stackRegisterName);
      auto found = blockOut.find(&block);
      if (found == blockOut.end() || found->second != outValue) {
        blockOut[&block] = outValue;
        changed = true;
      }
    }
  }

  std::vector<llvm::LoadInst *> deadLoads;
  for (llvm::BasicBlock &block : function) {
    llvm::Value *currentValue = nullptr;
    if (std::optional<llvm::Value *> merged =
            mergeKnownRegisterValueAtBlockEntry(block, blockOut)) {
      currentValue = *merged;
    }
    for (llvm::Instruction &instruction : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (currentValue != nullptr &&
            registerAccessMatchesName(*load, "notdec.register.access",
                                      frameRegisterName)) {
          load->replaceAllUsesWith(currentValue);
          deadLoads.push_back(load);
          continue;
        }
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (!registerAccessMatchesName(*store, "notdec.register.access",
                                       frameRegisterName)) {
          continue;
        }
        llvm::Value *storedValue = store->getValueOperand();
        currentValue =
            valueUsesExternalInputRegister(*storedValue, stackRegisterName)
                ? storedValue
                : nullptr;
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call != nullptr &&
          callInvalidatesKnownFrameRegisterValue(*call, frameRegisterName)) {
        currentValue = nullptr;
      }
    }
  }

  for (llvm::LoadInst *load : deadLoads) {
    load->eraseFromParent();
  }
}

void replaceStoredFramePointerRegisterLoads(llvm::Module &module,
                                            const NativeAbiSpec &abi) {
  if (abi.StackPointerRegister.empty()) {
    return;
  }
  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind != NativeAbiEffectKind::Unaffected ||
        effect.Storage.Kind != NativeAbiStorageKind::Register ||
        !isFramePointerRegisterName(effect.Storage.Name)) {
      continue;
    }
    for (llvm::Function &function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      NativePrototypeRewriteEligibility eligibility =
          getNativePrototypeRewriteEligibility(function);
      if (!eligibility.Eligible || eligibility.NeedsRewrite) {
        continue;
      }
      replaceStoredFramePointerRegisterLoads(function, effect.Storage.Name,
                                             abi.StackPointerRegister);
    }
  }
}

std::optional<int64_t> signedConstantValue(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getSExtValue();
}

std::optional<int64_t> stackOffsetFromBase(llvm::Value *value,
                                           llvm::Value *base,
                                           std::set<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (value == base) {
    return 0;
  }
  if (!seen.insert(value).second) {
    return std::nullopt;
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return std::nullopt;
  }

  if (op->getOpcode() == llvm::Instruction::Add) {
    if (std::optional<int64_t> lhs =
            stackOffsetFromBase(op->getOperand(0), base, seen)) {
      if (std::optional<int64_t> rhs =
              signedConstantValue(op->getOperand(1))) {
        return *lhs + *rhs;
      }
    }
    if (std::optional<int64_t> rhs =
            stackOffsetFromBase(op->getOperand(1), base, seen)) {
      if (std::optional<int64_t> lhs =
              signedConstantValue(op->getOperand(0))) {
        return *lhs + *rhs;
      }
    }
  }

  if (op->getOpcode() == llvm::Instruction::Sub) {
    if (std::optional<int64_t> lhs =
            stackOffsetFromBase(op->getOperand(0), base, seen)) {
      if (std::optional<int64_t> rhs =
              signedConstantValue(op->getOperand(1))) {
        return *lhs - *rhs;
      }
    }
  }

  return std::nullopt;
}

std::optional<uint64_t> fixedTypeStoreSize(const llvm::DataLayout &layout,
                                           llvm::Type *type) {
  llvm::TypeSize size = layout.getTypeStoreSize(type);
  if (size.isScalable()) {
    return std::nullopt;
  }
  return size.getFixedValue();
}

std::optional<uint64_t> memoryAccessSize(const llvm::DataLayout &layout,
                                         llvm::Instruction &instruction) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    return fixedTypeStoreSize(layout, load->getType());
  }
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    return fixedTypeStoreSize(layout, store->getValueOperand()->getType());
  }
  return std::nullopt;
}

// A raw stack address that can be replaced by a byte offset in a local alloca.
// Keeping the original pointer lets the cleanup delete the now-dead inttoptr.
struct StaticStackMemoryAccess {
  llvm::Instruction *Memory = nullptr;
  llvm::IntToPtrInst *Pointer = nullptr;
  int64_t Offset = 0;
  uint64_t Size = 0;
};

bool stackRangesOverlap(int64_t leftOffset, uint64_t leftSize,
                        int64_t rightOffset, uint64_t rightSize) {
  int64_t leftEnd = leftOffset + static_cast<int64_t>(leftSize);
  int64_t rightEnd = rightOffset + static_cast<int64_t>(rightSize);
  return leftOffset < rightEnd && rightOffset < leftEnd;
}

bool staticStackStoreIsLoaded(
    const StaticStackMemoryAccess &storeAccess,
    const std::vector<StaticStackMemoryAccess> &accesses) {
  for (const StaticStackMemoryAccess &access : accesses) {
    if (!llvm::isa<llvm::LoadInst>(access.Memory)) {
      continue;
    }
    if (stackRangesOverlap(storeAccess.Offset, storeAccess.Size, access.Offset,
                           access.Size)) {
      return true;
    }
  }
  return false;
}

void eraseDeadStaticStackStores(
    const std::vector<StaticStackMemoryAccess> &accesses) {
  // The store has to be deleted before its GEP and stored value can become
  // trivially dead.
  struct DeadStore {
    llvm::StoreInst *Store = nullptr;
    llvm::Value *StoredValue = nullptr;
    llvm::Value *Pointer = nullptr;
  };

  std::vector<DeadStore> deadStores;
  for (const StaticStackMemoryAccess &access : accesses) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(access.Memory);
    if (store == nullptr || staticStackStoreIsLoaded(access, accesses)) {
      continue;
    }
    deadStores.push_back({store, store->getValueOperand(),
                          store->getPointerOperand()});
  }

  for (const DeadStore &deadStore : deadStores) {
    deadStore.Store->eraseFromParent();
    llvm::RecursivelyDeleteTriviallyDeadInstructions(deadStore.Pointer);
    llvm::RecursivelyDeleteTriviallyDeadInstructions(deadStore.StoredValue);
  }
}

// A direct load/store through the synthetic native stack alloca.  This cleanup
// only reasons about already-local stack storage; it does not reinterpret raw
// RSP/RBP-derived addresses as frame bases.
struct NativeStackAllocaAccess {
  llvm::Instruction *Memory = nullptr;
  llvm::Value *Pointer = nullptr;
  int64_t Offset = 0;
  uint64_t Size = 0;
};

std::optional<int64_t> nativeStackAllocaOffset(llvm::Value *value,
                                               llvm::AllocaInst *alloca) {
  value = value->stripPointerCasts();
  if (value == alloca) {
    return 0;
  }

  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value);
  if (gep == nullptr || gep->getPointerOperand()->stripPointerCasts() != alloca) {
    return std::nullopt;
  }
  if (gep->getSourceElementType() != llvm::Type::getInt8Ty(alloca->getContext()) ||
      gep->getNumIndices() != 1) {
    return std::nullopt;
  }
  auto *offset = llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get());
  if (offset == nullptr || offset->getBitWidth() > 64) {
    return std::nullopt;
  }
  return offset->getSExtValue();
}

std::optional<std::vector<NativeStackAllocaAccess>>
nativeStackAllocaAccesses(llvm::AllocaInst &alloca,
                          const llvm::DataLayout &layout) {
  std::vector<NativeStackAllocaAccess> accesses;
  std::vector<llvm::User *> users(alloca.user_begin(), alloca.user_end());
  for (llvm::User *user : users) {
    auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
    if (gep == nullptr) {
      return std::nullopt;
    }
    std::optional<int64_t> offset = nativeStackAllocaOffset(gep, &alloca);
    if (!offset) {
      return std::nullopt;
    }
    for (llvm::User *pointerUser : gep->users()) {
      auto *memory = llvm::dyn_cast<llvm::Instruction>(pointerUser);
      if (memory == nullptr ||
          (llvm::isa<llvm::LoadInst>(memory) &&
           memory->getOperand(0) != gep) ||
          (llvm::isa<llvm::StoreInst>(memory) &&
           memory->getOperand(1) != gep) ||
          (!llvm::isa<llvm::LoadInst>(memory) &&
           !llvm::isa<llvm::StoreInst>(memory))) {
        return std::nullopt;
      }
      std::optional<uint64_t> size = memoryAccessSize(layout, *memory);
      if (!size || *size == 0) {
        return std::nullopt;
      }
      accesses.push_back({memory, gep, *offset, *size});
    }
  }
  return accesses;
}

bool nativeStackStoreIsLoaded(
    const NativeStackAllocaAccess &storeAccess,
    const std::vector<NativeStackAllocaAccess> &accesses) {
  for (const NativeStackAllocaAccess &access : accesses) {
    if (!llvm::isa<llvm::LoadInst>(access.Memory)) {
      continue;
    }
    if (stackRangesOverlap(storeAccess.Offset, storeAccess.Size, access.Offset,
                           access.Size)) {
      return true;
    }
  }
  return false;
}

bool eraseUnusedNativeStackAllocaLoads(llvm::AllocaInst &alloca,
                                       const llvm::DataLayout &layout) {
  std::optional<std::vector<NativeStackAllocaAccess>> accesses =
      nativeStackAllocaAccesses(alloca, layout);
  if (!accesses) {
    return false;
  }

  std::vector<llvm::LoadInst *> deadLoads;
  for (const NativeStackAllocaAccess &access : *accesses) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(access.Memory);
    if (load != nullptr && load->use_empty() && !load->isVolatile() &&
        !load->isAtomic()) {
      deadLoads.push_back(load);
    }
  }

  bool changed = false;
  for (llvm::LoadInst *load : deadLoads) {
    llvm::Value *pointer = load->getPointerOperand();
    load->eraseFromParent();
    if (auto *pointerInstruction = llvm::dyn_cast<llvm::Instruction>(pointer);
        pointerInstruction != nullptr && pointerInstruction->use_empty()) {
      pointerInstruction->eraseFromParent();
    }
    changed = true;
  }
  return changed;
}

bool eraseDeadNativeStackAllocaStores(llvm::AllocaInst &alloca,
                                      const llvm::DataLayout &layout) {
  std::optional<std::vector<NativeStackAllocaAccess>> accesses =
      nativeStackAllocaAccesses(alloca, layout);
  if (!accesses) {
    return false;
  }

  struct DeadStore {
    llvm::StoreInst *Store = nullptr;
    llvm::Value *StoredValue = nullptr;
    llvm::Value *Pointer = nullptr;
  };

  std::vector<DeadStore> deadStores;
  for (const NativeStackAllocaAccess &access : *accesses) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(access.Memory);
    if (store == nullptr || store->isVolatile() || store->isAtomic() ||
        nativeStackStoreIsLoaded(access, *accesses)) {
      continue;
    }
    deadStores.push_back({store, store->getValueOperand(),
                          store->getPointerOperand()});
  }

  bool changed = false;
  for (const DeadStore &deadStore : deadStores) {
    deadStore.Store->eraseFromParent();
    if (auto *pointerInstruction =
            llvm::dyn_cast<llvm::Instruction>(deadStore.Pointer);
        pointerInstruction != nullptr && pointerInstruction->use_empty()) {
      pointerInstruction->eraseFromParent();
    }
    llvm::RecursivelyDeleteTriviallyDeadInstructions(deadStore.StoredValue);
    changed = true;
  }
  return changed;
}

void eraseDeadNativeStackAllocas(llvm::Module &module) {
  const llvm::DataLayout &layout = module.getDataLayout();
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    std::vector<llvm::AllocaInst *> allocas;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
        if (alloca != nullptr && alloca->hasName() &&
            alloca->getName().starts_with("notdec_stack.native")) {
          allocas.push_back(alloca);
        }
      }
    }

    for (llvm::AllocaInst *alloca : allocas) {
      bool changed = true;
      while (changed) {
        changed = eraseUnusedNativeStackAllocaLoads(*alloca, layout);
        changed |= eraseDeadNativeStackAllocaStores(*alloca, layout);
      }
      if (alloca->use_empty()) {
        alloca->eraseFromParent();
      }
    }
  }
}

void eraseUnusedRawStackFrameLoads(llvm::Module &module,
                                   const NativeAbiSpec &abi) {
  std::set<std::string> registerNames = stackFrameRegisterNames(abi);
  if (registerNames.empty()) {
    return;
  }

  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    std::vector<llvm::LoadInst *> deadLoads;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load == nullptr || !load->use_empty() || load->isVolatile() ||
            load->isAtomic()) {
          continue;
        }
        auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(
            load->getPointerOperand()->stripPointerCasts());
        if (pointer == nullptr) {
          continue;
        }
        for (const std::string &registerName : registerNames) {
          if (valueUsesExternalInputRegister(*pointer->getOperand(0),
                                             registerName)) {
            deadLoads.push_back(load);
            break;
          }
        }
      }
    }

    std::set<llvm::LoadInst *> uniqueLoads(deadLoads.begin(), deadLoads.end());
    for (llvm::LoadInst *load : uniqueLoads) {
      llvm::Value *pointer = load->getPointerOperand();
      load->eraseFromParent();
      llvm::RecursivelyDeleteTriviallyDeadInstructions(pointer);
    }
  }
}

void rewriteStaticStackMemoryAccesses(llvm::Module &module,
                                      const NativeAbiSpec &abi) {
  if (abi.StackPointerRegister.empty()) {
    return;
  }
  const llvm::DataLayout &layout = module.getDataLayout();

  for (llvm::Function &function : module) {
    if (function.isDeclaration() || hasExistingNotDecStackAlloca(function)) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }
    llvm::LoadInst *stackBase =
        externalInputLoadForRegister(function, abi.StackPointerRegister);
    if (stackBase == nullptr) {
      continue;
    }

    std::vector<StaticStackMemoryAccess> accesses;
    int64_t low = 0;
    int64_t high = 0;
    bool sawAccess = false;
    bool failed = false;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction);
        if (pointer == nullptr) {
          continue;
        }
        std::set<llvm::Value *> seen;
        std::optional<int64_t> offset =
            stackOffsetFromBase(pointer->getOperand(0), stackBase, seen);
        if (!offset || *offset >= 0) {
          continue;
        }

        for (llvm::User *user : pointer->users()) {
          auto *memory = llvm::dyn_cast<llvm::Instruction>(user);
          if (memory == nullptr ||
              (llvm::isa<llvm::LoadInst>(memory) &&
               memory->getOperand(0) != pointer) ||
              (llvm::isa<llvm::StoreInst>(memory) &&
               memory->getOperand(1) != pointer) ||
              (!llvm::isa<llvm::LoadInst>(memory) &&
               !llvm::isa<llvm::StoreInst>(memory))) {
            failed = true;
            break;
          }
          std::optional<uint64_t> size = memoryAccessSize(layout, *memory);
          if (!size || *size == 0) {
            failed = true;
            break;
          }
          int64_t end = *offset + static_cast<int64_t>(*size);
          if (end > 0) {
            failed = true;
            break;
          }
          accesses.push_back({memory, pointer, *offset, *size});
          low = sawAccess ? std::min(low, *offset) : *offset;
          high = sawAccess ? std::max(high, end) : end;
          sawAccess = true;
        }
        if (failed) {
          break;
        }
      }
      if (failed) {
        break;
      }
    }
    if (failed || accesses.empty() || high <= low) {
      continue;
    }

    uint64_t frameSize = static_cast<uint64_t>(high - low);
    if (frameSize >
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
      continue;
    }

    llvm::IRBuilder<> entryBuilder(&function.getEntryBlock(),
                                   function.getEntryBlock().begin());
    auto *byteType = llvm::Type::getInt8Ty(module.getContext());
    auto *arrayType = llvm::ArrayType::get(byteType, frameSize);
    llvm::AllocaInst *storage =
        entryBuilder.CreateAlloca(arrayType, nullptr, "notdec_stack.native");
    storage->setAlignment(llvm::Align(16));

    for (const StaticStackMemoryAccess &access : accesses) {
      llvm::IRBuilder<> builder(access.Memory);
      llvm::Value *byteOffset = llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(module.getContext()),
          static_cast<uint64_t>(access.Offset - low));
      llvm::Value *localPointer = builder.CreateInBoundsGEP(
          llvm::Type::getInt8Ty(module.getContext()), storage, byteOffset,
          "notdec_stack.native.ptr");
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(access.Memory)) {
        load->setOperand(0, localPointer);
      } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(access.Memory)) {
        store->setOperand(1, localPointer);
      }
    }

    std::set<llvm::IntToPtrInst *> rewrittenPointers;
    for (const StaticStackMemoryAccess &access : accesses) {
      if (rewrittenPointers.insert(access.Pointer).second &&
          access.Pointer->use_empty()) {
        llvm::RecursivelyDeleteTriviallyDeadInstructions(access.Pointer);
      }
    }
    eraseDeadStaticStackStores(accesses);
  }
}

void eraseDeadStackFrameRegisterStores(llvm::Module &module,
                                       const NativeAbiSpec &abi) {
  std::set<std::string> registerNames = stackFrameRegisterNames(abi);
  if (registerNames.empty()) {
    return;
  }

  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    std::vector<llvm::StoreInst *> deadStores;
    for (const std::string &registerName : registerNames) {
      for (llvm::BasicBlock &block : function) {
        for (llvm::Instruction &instruction : block) {
          auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          if (store == nullptr) {
            continue;
          }
          llvm::MDNode *access = store->getMetadata("notdec.register.access");
          if (access != nullptr &&
              accessMatchesEffectRegister(*access, registerName) &&
              storeIsDeadOnAllReturnPaths(
                  *store, *access,
                  isFramePointerRegisterName(registerName)
                      ? callMayReadRegisterNameForDeadFrameStore
                      : callMayReadRegisterName)) {
            deadStores.push_back(store);
          }
        }
      }
    }

    std::set<llvm::StoreInst *> uniqueStores(deadStores.begin(),
                                            deadStores.end());
    for (llvm::StoreInst *store : uniqueStores) {
      llvm::Value *storedValue = store->getValueOperand();
      store->eraseFromParent();
      llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
    }
  }
}

bool functionHasRegisterLoad(llvm::Function &function,
                             llvm::StringRef registerName) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load != nullptr && loadReadsRegisterName(*load, registerName)) {
        return true;
      }
    }
  }
  return false;
}

bool functionCallsMayReadRegister(llvm::Function &function,
                                  llvm::StringRef registerName) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call != nullptr &&
          callMayReadRegisterNameForDeadFrameStore(*call, registerName)) {
        return true;
      }
    }
  }
  return false;
}

void eraseDeadFrameBaseRegisterStores(llvm::Module &module,
                                      const NativeAbiSpec &abi) {
  if (abi.StackPointerRegister.empty()) {
    return;
  }

  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind != NativeAbiEffectKind::Unaffected ||
        effect.Storage.Kind != NativeAbiStorageKind::Register ||
        !isFramePointerRegisterName(effect.Storage.Name)) {
      continue;
    }

    for (llvm::Function &function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      NativePrototypeRewriteEligibility eligibility =
          getNativePrototypeRewriteEligibility(function);
      if (!eligibility.Eligible || eligibility.NeedsRewrite ||
          functionHasRegisterLoad(function, effect.Storage.Name) ||
          functionCallsMayReadRegister(function, effect.Storage.Name)) {
        continue;
      }

      std::vector<llvm::StoreInst *> deadStores;
      for (llvm::BasicBlock &block : function) {
        for (llvm::Instruction &instruction : block) {
          auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          if (store == nullptr) {
            continue;
          }
          llvm::MDNode *access = store->getMetadata("notdec.register.access");
          if (access != nullptr &&
              accessMatchesEffectRegister(*access, effect.Storage.Name) &&
              valueUsesExternalInputRegister(*store->getValueOperand(),
                                             abi.StackPointerRegister)) {
            deadStores.push_back(store);
          }
        }
      }

      for (llvm::StoreInst *store : deadStores) {
        llvm::Value *storedValue = store->getValueOperand();
        store->eraseFromParent();
        llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
      }
    }
  }
}

void eraseDeadNonReturnVectorStores(llvm::Module &module) {
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }
    std::optional<NativeRecoveredPrototype> prototype =
        readNativeRecoveredPrototypeMetadata(function);
    if (!prototype) {
      continue;
    }

    std::vector<llvm::StoreInst *> deadStores;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (store == nullptr) {
          continue;
        }
        llvm::MDNode *access = store->getMetadata("notdec.register.access");
        if (access == nullptr || !accessIsVectorPartialStore(*access, *store) ||
            accessMatchesRecoveredReturn(*access, *prototype) ||
            !storeIsDeadOnAllReturnPaths(*store, *access)) {
          continue;
        }
        deadStores.push_back(store);
      }
    }

    for (llvm::StoreInst *store : deadStores) {
      llvm::Value *storedValue = store->getValueOperand();
      store->eraseFromParent();
      llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
    }
  }
}

void eraseDeadStackFrameDerivedNonReturnStores(llvm::Module &module,
                                               const NativeAbiSpec &abi) {
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }
    std::optional<NativeRecoveredPrototype> prototype =
        readNativeRecoveredPrototypeMetadata(function);
    if (!prototype) {
      continue;
    }

    std::vector<llvm::StoreInst *> deadStores;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (store == nullptr) {
          continue;
        }
        llvm::MDNode *access = store->getMetadata("notdec.register.access");
        if (access == nullptr ||
            accessMatchesRecoveredReturn(*access, *prototype) ||
            !valueIsStackFrameExternalInputDerived(*store->getValueOperand(),
                                                   abi) ||
            !storeIsDeadOnAllReturnPaths(*store, *access)) {
          continue;
        }
        deadStores.push_back(store);
      }
    }

    for (llvm::StoreInst *store : deadStores) {
      llvm::Value *storedValue = store->getValueOperand();
      store->eraseFromParent();
      llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
    }
  }
}

bool valueIsNativeStackAllocaPointer(llvm::Value &value, unsigned depth = 0) {
  if (depth >= 8) {
    return false;
  }
  llvm::Value *stripped = value.stripPointerCasts();
  auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(stripped);
  if (alloca != nullptr) {
    return alloca->hasName() &&
           alloca->getName().starts_with("notdec_stack.native");
  }
  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stripped);
  return gep != nullptr &&
         valueIsNativeStackAllocaPointer(*gep->getPointerOperand(), depth + 1);
}

bool valueIsSavedRegisterRestoreLoad(llvm::Value &value,
                                     const NativeAbiSpec &abi) {
  auto *load = llvm::dyn_cast<llvm::LoadInst>(&value);
  if (load == nullptr || load->isVolatile() || load->isAtomic()) {
    return false;
  }

  llvm::Value *pointer = load->getPointerOperand()->stripPointerCasts();
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
    return valueIsStackFrameExternalInputDerived(*intToPtr->getOperand(0), abi);
  }

  return valueIsNativeStackAllocaPointer(*pointer);
}

void eraseDeadPreservedRegisterRestoreStores(llvm::Module &module,
                                             const NativeAbiSpec &abi) {
  std::set<std::string> registerNames =
      preservedNonStackFrameRegisterNames(abi);
  if (registerNames.empty()) {
    return;
  }

  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }
    std::optional<NativeRecoveredPrototype> prototype =
        readNativeRecoveredPrototypeMetadata(function);

    std::vector<llvm::StoreInst *> deadStores;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (store == nullptr) {
          continue;
        }
        llvm::MDNode *access = store->getMetadata("notdec.register.access");
        if (access == nullptr ||
            (prototype && accessMatchesRecoveredReturn(*access, *prototype)) ||
            !valueIsSavedRegisterRestoreLoad(*store->getValueOperand(), abi) ||
            !storeIsDeadOnAllReturnPaths(*store, *access)) {
          continue;
        }
        for (const std::string &registerName : registerNames) {
          if (accessMatchesEffectRegister(*access, registerName)) {
            deadStores.push_back(store);
            break;
          }
        }
      }
    }

    for (llvm::StoreInst *store : deadStores) {
      llvm::Value *storedValue = store->getValueOperand();
      store->eraseFromParent();
      llvm::RecursivelyDeleteTriviallyDeadInstructions(storedValue);
    }
  }
}

bool callsitePathReadsRegisterAfterCall(llvm::CallInst &call,
                                        llvm::StringRef registerName) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(call.getContext(),
                          ("name=" + registerName).str()),
  };
  llvm::MDNode *access = llvm::MDNode::get(call.getContext(), fields);
  std::set<llvm::BasicBlock *> seen;
  return !reachesReturnWithoutCallOrAccessLoad(
      call.getNextNode(), *call.getParent(), *access, seen,
      callMayReadRegisterName);
}

bool argumentOnlyFeedsDeadRegisterStore(llvm::Argument &argument,
                                        llvm::StoreInst *&deadStore,
                                        std::string &registerName) {
  if (argument.use_empty() || !argument.hasOneUse()) {
    return false;
  }
  auto *store = llvm::dyn_cast<llvm::StoreInst>(*argument.user_begin());
  if (store == nullptr || store->getValueOperand() != &argument ||
      store->isVolatile() || store->isAtomic()) {
    return false;
  }
  llvm::MDNode *access = store->getMetadata("notdec.register.access");
  if (access == nullptr) {
    return false;
  }
  std::optional<std::string> name = metadataField(*access, "name");
  if (!name || name->empty()) {
    return false;
  }
  std::set<llvm::BasicBlock *> seen;
  if (!reachesReturnWithoutCallOrAccessLoad(store->getNextNode(),
                                            *store->getParent(), *access,
                                            seen, callMayReadRegisterName)) {
    return false;
  }
  deadStore = store;
  registerName = *name;
  return true;
}

std::optional<std::vector<llvm::CallInst *>>
directInternalCallsites(llvm::Function &function) {
  std::vector<llvm::CallInst *> calls;
  for (llvm::User *user : function.users()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(user);
    if (call == nullptr || call->getCalledFunction() != &function ||
        call->arg_size() != function.arg_size()) {
      return std::nullopt;
    }
    calls.push_back(call);
  }
  return calls;
}

bool callsitesDoNotReadRegisterAfterCall(llvm::ArrayRef<llvm::CallInst *> calls,
                                         llvm::StringRef registerName) {
  for (llvm::CallInst *call : calls) {
    if (callsitePathReadsRegisterAfterCall(*call, registerName)) {
      return false;
    }
  }
  return true;
}

bool shrinkInternalFunctionArguments(llvm::Function &function,
                                     llvm::ArrayRef<uint8_t> keep) {
  if (keep.size() != function.arg_size() ||
      llvm::all_of(keep, [](uint8_t value) { return value != 0; })) {
    return false;
  }
  std::optional<NativeRecoveredPrototype> prototype =
      readNativeRecoveredPrototypeMetadata(function);
  if (!prototype || prototype->Inputs.size() != function.arg_size()) {
    return false;
  }
  std::optional<std::vector<llvm::CallInst *>> calls =
      directInternalCallsites(function);
  if (!calls) {
    return false;
  }

  std::vector<llvm::Type *> paramTypes;
  std::vector<NativeRecoveredPrototypeParam> inputs;
  for (uint64_t index = 0; index < keep.size(); ++index) {
    if (keep[index] == 0) {
      continue;
    }
    paramTypes.push_back(function.getFunctionType()->getParamType(index));
    inputs.push_back(prototype->Inputs[index]);
  }
  auto *newType = llvm::FunctionType::get(function.getReturnType(), paramTypes,
                                          function.isVarArg());
  llvm::Module *module = function.getParent();
  if (module == nullptr) {
    return false;
  }

  std::string originalName = function.getName().str();
  function.setName(originalName + ".old");
  llvm::Function *rewritten =
      llvm::Function::Create(newType, function.getLinkage(), originalName,
                             module);
  rewritten->copyAttributesFrom(&function);
  rewritten->copyMetadata(&function, 0);
  rewritten->setCallingConv(function.getCallingConv());
  NativeRecoveredPrototype newPrototype = *prototype;
  newPrototype.Inputs = std::move(inputs);
  rewritten->setMetadata(
      "notdec.prototype.recovered",
      recoveredPrototypeMetadata(module->getContext(), newPrototype));
  rewritten->setMetadata("notdec.register.external_inputs", nullptr);
  rewritten->setMetadata("notdec.prototype.input_candidates", nullptr);
  rewritten->setMetadata("notdec.prototype.return_candidates", nullptr);
  rewritten->splice(rewritten->end(), &function);

  auto newArg = rewritten->arg_begin();
  for (uint64_t index = 0; index < keep.size(); ++index) {
    llvm::Argument *oldArg = function.getArg(index);
    if (keep[index] == 0) {
      oldArg->replaceAllUsesWith(llvm::UndefValue::get(oldArg->getType()));
      continue;
    }
    newArg->setName(oldArg->getName());
    oldArg->replaceAllUsesWith(&*newArg);
    ++newArg;
  }

  for (llvm::CallInst *call : *calls) {
    llvm::IRBuilder<> builder(call);
    std::vector<llvm::Value *> args;
    std::vector<llvm::Value *> removedArgs;
    for (uint64_t index = 0; index < keep.size(); ++index) {
      if (keep[index] != 0) {
        args.push_back(call->getArgOperand(index));
      } else {
        removedArgs.push_back(call->getArgOperand(index));
      }
    }
    llvm::CallInst *newCall =
        builder.CreateCall(rewritten->getFunctionType(), rewritten, args);
    newCall->setCallingConv(call->getCallingConv());
    if (!call->getType()->isVoidTy()) {
      call->replaceAllUsesWith(newCall);
    }
    call->eraseFromParent();
    // Removed helper arguments often carry dead stack pointer arithmetic, such
    // as RSP.external_input - 16. Drop that chain immediately when it is unused.
    for (llvm::Value *removedArg : removedArgs) {
      llvm::RecursivelyDeleteTriviallyDeadInstructions(removedArg);
    }
  }

  function.eraseFromParent();
  return true;
}

void eraseDeadInternalCallArgumentRegisterStores(llvm::Module &module) {
  // Some internal helpers only forward register state through arguments and
  // immediately store it into another register global. If nobody reads that
  // register afterwards, shrink the helper instead of keeping dead RSP/RBP math.
  std::vector<llvm::Function *> functions;
  for (llvm::Function &function : module.functions()) {
    functions.push_back(&function);
  }
  for (llvm::Function *function : functions) {
    if (function->isDeclaration() || function->arg_empty()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(*function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }
    std::optional<std::vector<llvm::CallInst *>> calls =
        directInternalCallsites(*function);
    if (!calls) {
      continue;
    }

    std::vector<uint8_t> keep(function->arg_size(), 1);
    std::vector<llvm::StoreInst *> deadStores;
    for (llvm::Argument &argument : function->args()) {
      llvm::StoreInst *deadStore = nullptr;
      std::string registerName;
      if (!argumentOnlyFeedsDeadRegisterStore(argument, deadStore,
                                              registerName)) {
        continue;
      }
      if (!callsitesDoNotReadRegisterAfterCall(*calls, registerName)) {
        continue;
      }
      keep[argument.getArgNo()] = 0;
      deadStores.push_back(deadStore);
    }
    if (llvm::all_of(keep, [](uint8_t value) { return value != 0; })) {
      continue;
    }
    for (llvm::StoreInst *store : deadStores) {
      if (store->getParent() != nullptr) {
        store->eraseFromParent();
      }
    }
    shrinkInternalFunctionArguments(*function, keep);
  }
}

bool supportedCallerFrameAccessType(llvm::Type *type) {
  auto *integer = llvm::dyn_cast<llvm::IntegerType>(type);
  return integer != nullptr &&
         (integer->getBitWidth() == 32 || integer->getBitWidth() == 64);
}

llvm::Function *getOrCreateCallerFrameAccessDeclaration(llvm::Module &module,
                                                        bool isStore,
                                                        llvm::Type *valueType) {
  if (!supportedCallerFrameAccessType(valueType)) {
    return nullptr;
  }
  auto *integer = llvm::cast<llvm::IntegerType>(valueType);
  std::string name =
      "notdec_caller_frame_" + std::string(isStore ? "store_i" : "load_i") +
      std::to_string(integer->getBitWidth());
  if (llvm::Function *existing = module.getFunction(name)) {
    return existing;
  }

  llvm::Type *offsetType = llvm::Type::getInt64Ty(module.getContext());
  llvm::FunctionType *type = nullptr;
  if (isStore) {
    type = llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()),
                                   {offsetType, valueType}, false);
  } else {
    type = llvm::FunctionType::get(valueType, {offsetType}, false);
  }
  return llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, name,
                                module);
}

bool rewriteCallerFramePointerAccess(llvm::IntToPtrInst &pointer,
                                     llvm::LoadInst &frameBase,
                                     llvm::Module &module) {
  std::set<llvm::Value *> seen;
  std::optional<int64_t> offset =
      stackOffsetFromBase(pointer.getOperand(0), &frameBase, seen);
  if (!offset || *offset >= 0) {
    return false;
  }

  std::vector<llvm::Instruction *> memoryUsers;
  for (llvm::User *user : pointer.users()) {
    auto *memory = llvm::dyn_cast<llvm::Instruction>(user);
    if (memory == nullptr ||
        (llvm::isa<llvm::LoadInst>(memory) && memory->getOperand(0) != &pointer) ||
        (llvm::isa<llvm::StoreInst>(memory) &&
         memory->getOperand(1) != &pointer) ||
        (!llvm::isa<llvm::LoadInst>(memory) &&
         !llvm::isa<llvm::StoreInst>(memory))) {
      return false;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(memory)) {
      if (load->isVolatile() || load->isAtomic() ||
          !supportedCallerFrameAccessType(load->getType())) {
        return false;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(memory)) {
      if (store->isVolatile() || store->isAtomic() ||
          !supportedCallerFrameAccessType(store->getValueOperand()->getType())) {
        return false;
      }
    }
    memoryUsers.push_back(memory);
  }
  if (memoryUsers.empty()) {
    return false;
  }

  for (llvm::Instruction *memory : memoryUsers) {
    llvm::IRBuilder<> builder(memory);
    llvm::Value *offsetValue = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(module.getContext()), *offset, true);
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(memory)) {
      llvm::Function *callee =
          getOrCreateCallerFrameAccessDeclaration(module, false,
                                                  load->getType());
      if (callee == nullptr) {
        return false;
      }
      llvm::CallInst *call =
          builder.CreateCall(callee->getFunctionType(), callee, {offsetValue});
      load->replaceAllUsesWith(call);
      load->eraseFromParent();
      continue;
    }
    auto *store = llvm::cast<llvm::StoreInst>(memory);
    llvm::Function *callee = getOrCreateCallerFrameAccessDeclaration(
        module, true, store->getValueOperand()->getType());
    if (callee == nullptr) {
      return false;
    }
    builder.CreateCall(callee->getFunctionType(), callee,
                       {offsetValue, store->getValueOperand()});
    store->eraseFromParent();
  }

  if (pointer.use_empty()) {
    llvm::RecursivelyDeleteTriviallyDeadInstructions(&pointer);
  }
  return true;
}

void rewriteCallerFrameAccesses(llvm::Module &module,
                                const NativeAbiSpec &abi) {
  std::set<std::string> registerNames = stackFrameRegisterNames(abi);
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativePrototypeRewriteEligibility eligibility =
        getNativePrototypeRewriteEligibility(function);
    if (!eligibility.Eligible || eligibility.NeedsRewrite) {
      continue;
    }

    for (const std::string &registerName : registerNames) {
      if (registerName == abi.StackPointerRegister ||
          !isFramePointerRegisterName(registerName)) {
        continue;
      }
      llvm::LoadInst *frameBase =
          externalInputLoadForRegister(function, registerName);
      if (frameBase == nullptr) {
        continue;
      }

      std::vector<llvm::IntToPtrInst *> pointers;
      for (llvm::BasicBlock &block : function) {
        for (llvm::Instruction &instruction : block) {
          auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction);
          if (pointer != nullptr) {
            pointers.push_back(pointer);
          }
        }
      }
      for (llvm::IntToPtrInst *pointer : pointers) {
        rewriteCallerFramePointerAccess(*pointer, *frameBase, module);
      }
      if (frameBase->getParent() != nullptr && frameBase->use_empty()) {
        frameBase->eraseFromParent();
      }
    }
  }
}

llvm::Function *getOrCreateStackCanaryCheckDeclaration(llvm::Module &module) {
  llvm::Function *existing = module.getFunction("notdec_stack_canary_check");
  if (existing != nullptr) {
    return existing;
  }
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(module.getContext()), {}, false);
  return llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                "notdec_stack_canary_check", module);
}

bool loadReadsRegisterExternalInput(llvm::LoadInst &load,
                                    llvm::StringRef name) {
  llvm::MDNode *access = load.getMetadata("notdec.register.external_input");
  return access != nullptr && accessMatchesEffectRegister(*access, name);
}

bool pureInstructionOnlyFeedsBlock(llvm::Instruction &instruction,
                                   const std::set<llvm::Instruction *> &blockSet) {
  if (instruction.mayHaveSideEffects()) {
    return false;
  }
  if (!llvm::isa<llvm::BinaryOperator>(&instruction) &&
      !llvm::isa<llvm::CastInst>(&instruction) &&
      !llvm::isa<llvm::GetElementPtrInst>(&instruction)) {
    return false;
  }
  for (llvm::User *user : instruction.users()) {
    auto *userInstruction = llvm::dyn_cast<llvm::Instruction>(user);
    if (userInstruction == nullptr ||
        blockSet.find(userInstruction) == blockSet.end()) {
      return false;
    }
  }
  return true;
}

bool storeIsStackChkFailReturnAddress(llvm::StoreInst &store,
                                      llvm::ArrayRef<llvm::Value *> stackValues) {
  if (store.isVolatile() ||
      store.getMetadata("notdec.register.access") != nullptr ||
      !llvm::isa<llvm::ConstantInt>(store.getValueOperand())) {
    return false;
  }
  auto *pointer =
      llvm::dyn_cast<llvm::IntToPtrInst>(store.getPointerOperand());
  if (pointer == nullptr) {
    return false;
  }
  return llvm::is_contained(stackValues, pointer->getOperand(0));
}

bool blockOnlyCallsStackChkFail(llvm::BasicBlock &block,
                                const NativeAbiSpec &abi) {
  llvm::CallInst *failCall = nullptr;
  std::set<llvm::Instruction *> beforeFail;
  bool sawFailCall = false;
  for (llvm::Instruction &instruction : block) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
    if (!sawFailCall) {
      if (call == nullptr) {
        beforeFail.insert(&instruction);
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr || callee->getName() != "__stack_chk_fail") {
        return false;
      }
      failCall = call;
      sawFailCall = true;
      continue;
    }
    if (llvm::isa<llvm::UnreachableInst>(&instruction) ||
        llvm::isa<llvm::ReturnInst>(&instruction)) {
      continue;
    }
    return false;
  }
  if (failCall == nullptr) {
    return false;
  }

  std::vector<llvm::Value *> stackValues;
  for (llvm::Instruction *instruction : beforeFail) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
      if (load->isVolatile() ||
          !loadReadsRegisterExternalInput(*load, abi.StackPointerRegister)) {
        return false;
      }
      continue;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(instruction)) {
      if (store->isVolatile()) {
        return false;
      }
      if (registerAccessMatchesName(*store, "notdec.register.access",
                                    abi.StackPointerRegister)) {
        stackValues.push_back(store->getValueOperand());
        continue;
      }
      if (storeIsStackChkFailReturnAddress(*store, stackValues)) {
        continue;
      }
      return false;
    }
    if (!pureInstructionOnlyFeedsBlock(*instruction, beforeFail)) {
      return false;
    }
  }
  return true;
}

std::optional<bool> conditionTrueMeansCanaryEqual(llvm::Value *condition,
                                                  llvm::ICmpInst **equalCmp) {
  auto *cmp = llvm::dyn_cast_or_null<llvm::ICmpInst>(condition);
  if (cmp == nullptr) {
    return std::nullopt;
  }

  if (cmp->getPredicate() != llvm::ICmpInst::ICMP_NE &&
      cmp->getPredicate() != llvm::ICmpInst::ICMP_EQ) {
    return std::nullopt;
  }

  llvm::Value *maybeZext = nullptr;
  llvm::Value *maybeZero = nullptr;
  if (llvm::isa<llvm::ConstantInt>(cmp->getOperand(0))) {
    maybeZero = cmp->getOperand(0);
    maybeZext = cmp->getOperand(1);
  } else if (llvm::isa<llvm::ConstantInt>(cmp->getOperand(1))) {
    maybeZero = cmp->getOperand(1);
    maybeZext = cmp->getOperand(0);
  }
  auto *zero = llvm::dyn_cast_or_null<llvm::ConstantInt>(maybeZero);
  auto *zext = llvm::dyn_cast_or_null<llvm::ZExtInst>(maybeZext);
  if (zero != nullptr && zero->isZero() && zext != nullptr) {
    auto *inner = llvm::dyn_cast<llvm::ICmpInst>(zext->getOperand(0));
    if (inner == nullptr || inner->getPredicate() != llvm::ICmpInst::ICMP_EQ) {
      return std::nullopt;
    }
    *equalCmp = inner;
    return cmp->getPredicate() == llvm::ICmpInst::ICMP_NE;
  }

  *equalCmp = cmp;
  return cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ;
}

std::optional<int64_t> loadOffsetFromExternalRegister(
    llvm::LoadInst &load, llvm::Function &function, llvm::StringRef name) {
  llvm::LoadInst *base = externalInputLoadForRegister(function, name);
  if (base == nullptr) {
    return std::nullopt;
  }
  return rawStackInputOffset(load, *base);
}

bool loadIsFrameCanarySlot(llvm::LoadInst &load, const NativeAbiSpec &abi) {
  llvm::Function *function = load.getFunction();
  if (function == nullptr) {
    return false;
  }
  for (llvm::BasicBlock &block : *function) {
    for (llvm::Instruction &instruction : block) {
      auto *base = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (base == nullptr) {
        continue;
      }
      llvm::MDNode *metadata =
          base->getMetadata("notdec.register.external_input");
      std::optional<std::string> name =
          metadata == nullptr ? std::nullopt : metadataField(*metadata, "name");
      if (!name || !isFramePointerRegisterName(*name)) {
        continue;
      }
      std::optional<int64_t> offset =
          rawStackInputOffset(load, *base);
      if (offset && *offset < 0) {
        return true;
      }
    }
  }
  for (const std::string &name : stackFrameRegisterNames(abi)) {
    if (name == abi.StackPointerRegister || !isFramePointerRegisterName(name)) {
      continue;
    }
    std::optional<int64_t> offset =
        loadOffsetFromExternalRegister(load, *function, name);
    if (offset && *offset < 0) {
      return true;
    }
  }
  return false;
}

bool loadReadsRegisterAccess(llvm::LoadInst &load, llvm::StringRef name) {
  llvm::MDNode *access = load.getMetadata("notdec.register.access");
  return access != nullptr && accessMatchesEffectRegister(*access, name);
}

bool loadIsFsCanary(llvm::LoadInst &load) {
  llvm::Function *function = load.getFunction();
  if (function == nullptr) {
    return false;
  }
  for (llvm::BasicBlock &block : *function) {
    for (llvm::Instruction &instruction : block) {
      auto *base = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (base == nullptr || !loadReadsRegisterAccess(*base, "FS_OFFSET")) {
        continue;
      }
      std::set<llvm::Value *> seen;
      std::optional<int64_t> offset =
          rawStackInputOffset(load, *base);
      if (offset && *offset == 40) {
        return true;
      }
    }
  }
  return false;
}

bool canReplaceCanaryLoadUses(llvm::LoadInst &frameLoad,
                              llvm::LoadInst &fsLoad,
                              llvm::ICmpInst &equalCmp,
                              std::set<llvm::Instruction *> &diffs) {
  for (llvm::LoadInst *load : {&frameLoad, &fsLoad}) {
    for (llvm::User *user : load->users()) {
      if (user == &equalCmp) {
        continue;
      }
      auto *diff = llvm::dyn_cast<llvm::BinaryOperator>(user);
      if (diff == nullptr || diff->getOpcode() != llvm::Instruction::Sub ||
          diff->getOperand(0) != &frameLoad || diff->getOperand(1) != &fsLoad) {
        return false;
      }
      diffs.insert(diff);
    }
  }
  return true;
}

void eraseDeadLoadAndPointer(llvm::LoadInst &load) {
  if (!load.use_empty()) {
    return;
  }
  llvm::Value *pointer = load.getPointerOperand();
  load.eraseFromParent();
  llvm::RecursivelyDeleteTriviallyDeadInstructions(pointer);
}

bool eraseStackCanaryCheck(llvm::BranchInst &branch, const NativeAbiSpec &abi) {
  if (!branch.isConditional()) {
    return false;
  }

  llvm::ICmpInst *equalCmp = nullptr;
  std::optional<bool> trueMeansEqual =
      conditionTrueMeansCanaryEqual(branch.getCondition(), &equalCmp);
  if (!trueMeansEqual || equalCmp == nullptr) {
    return false;
  }

  llvm::BasicBlock *success =
      *trueMeansEqual ? branch.getSuccessor(0) : branch.getSuccessor(1);
  llvm::BasicBlock *fail =
      *trueMeansEqual ? branch.getSuccessor(1) : branch.getSuccessor(0);
  if (success == fail || !blockOnlyCallsStackChkFail(*fail, abi)) {
    return false;
  }

  auto *first = llvm::dyn_cast<llvm::LoadInst>(equalCmp->getOperand(0));
  auto *second = llvm::dyn_cast<llvm::LoadInst>(equalCmp->getOperand(1));
  if (first == nullptr || second == nullptr) {
    return false;
  }
  llvm::LoadInst *frameLoad = nullptr;
  llvm::LoadInst *fsLoad = nullptr;
  if (loadIsFrameCanarySlot(*first, abi) && loadIsFsCanary(*second)) {
    frameLoad = first;
    fsLoad = second;
  } else if (loadIsFrameCanarySlot(*second, abi) && loadIsFsCanary(*first)) {
    frameLoad = second;
    fsLoad = first;
  } else {
    return false;
  }

  std::set<llvm::Instruction *> diffs;
  if (!canReplaceCanaryLoadUses(*frameLoad, *fsLoad, *equalCmp, diffs)) {
    return false;
  }

  llvm::Function *function = branch.getFunction();
  llvm::Module *module = function == nullptr ? nullptr : function->getParent();
  if (module == nullptr) {
    return false;
  }

  llvm::IRBuilder<> builder(&branch);
  builder.CreateCall(getOrCreateStackCanaryCheckDeclaration(*module));

  llvm::Value *zero = llvm::ConstantInt::get(frameLoad->getType(), 0);
  for (llvm::Instruction *diff : diffs) {
    diff->replaceAllUsesWith(zero);
  }

  llvm::Value *oldCondition = branch.getCondition();
  builder.SetInsertPoint(&branch);
  builder.CreateBr(success);
  branch.eraseFromParent();
  for (llvm::Instruction *diff : diffs) {
    llvm::RecursivelyDeleteTriviallyDeadInstructions(diff);
  }
  eraseDeadLoadAndPointer(*frameLoad);
  eraseDeadLoadAndPointer(*fsLoad);
  llvm::RecursivelyDeleteTriviallyDeadInstructions(oldCondition);
  llvm::removeUnreachableBlocks(*function);
  return true;
}

void eraseStackCanaryChecks(llvm::Module &module, const NativeAbiSpec &abi) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (llvm::Function &function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      for (llvm::BasicBlock &block : llvm::make_early_inc_range(function)) {
        auto *branch = llvm::dyn_cast_or_null<llvm::BranchInst>(
            block.getTerminator());
        if (branch != nullptr && eraseStackCanaryCheck(*branch, abi)) {
          changed = true;
          break;
        }
      }
      if (changed) {
        break;
      }
    }
  }
}

struct ReturnLoadSearchResult {
  llvm::LoadInst *Load = nullptr;
  llvm::Value *Value = nullptr;
  llvm::BasicBlock *SharedSuccessor = nullptr;
  llvm::BasicBlock *CallPredecessor = nullptr;
  bool Blocked = false;
  bool Clobbered = false;
};

ReturnLoadSearchResult foundReturnLoad(llvm::LoadInst *load) {
  ReturnLoadSearchResult result;
  result.Load = load;
  result.Value = load;
  return result;
}

ReturnLoadSearchResult foundReturnValue(llvm::Value *value) {
  ReturnLoadSearchResult result;
  result.Value = value;
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
  std::vector<llvm::Value *> ReturnValues;
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
  // Same ABI order as Arguments.  Null means the argument store is still needed
  // as caller-visible register state.
  std::vector<llvm::StoreInst *> InputStores;
  // Same slot order as the recovered returns; null entries are unused results.
  std::vector<llvm::LoadInst *> ReturnLoads;
  std::vector<llvm::Value *> ReturnValues;
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
  if (isNotDecRegisterHelperCall(call)) {
    return false;
  }
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

bool isReturnCallEffectValue(llvm::Instruction &inst,
                             llvm::StringRef registerName) {
  llvm::MDNode *metadata = inst.getMetadata("notdec.register.call_effect");
  if (metadata == nullptr) {
    return false;
  }
  return metadataField(*metadata, "kind") == std::optional<std::string>("return") &&
         metadataField(*metadata, "register") ==
             std::optional<std::string>(registerName.str());
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
    if (isReturnCallEffectValue(*iter, returnRegisterName)) {
      return foundReturnValue(&*iter);
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
    if (blockResult.Value != nullptr || blockResult.Blocked ||
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
    if (successorResult.Value != nullptr) {
      if (load != nullptr) {
        return blockedReturnLoadSearch();
      }
      if (successorResult.Load == nullptr) {
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
    if (nestedResult.Value != nullptr) {
      if (load != nullptr) {
        return blockedReturnLoadSearch();
      }
      if (nestedResult.Load == nullptr) {
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
    if (blockResult.Value != nullptr || blockResult.Blocked) {
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
  if (localResult.Value != nullptr || localResult.Blocked ||
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
      if (successorResult.Value != nullptr) {
        if (!allowSharedSuccessorLoad) {
          return blockedReturnLoadSearch();
        }
        if (successorResult.Load == nullptr) {
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
        if (unusedResult.Blocked || unusedResult.Value != nullptr) {
          return blockedReturnLoadSearch();
        }
      }
      return {};
    }
    if (successorResult.Value != nullptr || successorResult.Blocked ||
        successorResult.Clobbered) {
      return successorResult;
    }
    current = successor;
  }
  std::set<llvm::BasicBlock *> activeBlocks;
  ReturnLoadSearchResult unusedResult =
      findSharedSuccessorUnusedReturn(*current, returnRegisterName,
                                      activeBlocks);
  if (unusedResult.Blocked || unusedResult.Value != nullptr) {
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
  llvm::Value *value = result.Value;
  if (value == nullptr) {
    return;
  }
  if (value->getType() != newCall.getType()) {
    return;
  }

  if (result.SharedSuccessor != nullptr && result.CallPredecessor != nullptr) {
    if (load == nullptr) {
      return;
    }
    replaceSharedSuccessorReturnLoad(*load, newCall, result, returnRegisterName);
    return;
  }

  value->replaceAllUsesWith(&newCall);
  if (auto *inst = llvm::dyn_cast<llvm::Instruction>(value)) {
    if (inst->use_empty()) {
      inst->eraseFromParent();
    }
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
  return result.Blocked || (result.Value != nullptr &&
                            result.Value->getType() != returnType);
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
    rewrite.ReturnValues.reserve(returns.size());
    rewrite.ReturnLoadResults.reserve(returns.size());
    rewrite.ReturnRegisterNames.reserve(returns.size());
    for (uint64_t index = 0; index < returns.size(); ++index) {
      ReturnLoadSearchResult loadResult =
          findCallsiteReturnLoad(*call, returns[index].RegisterName, true);
      if (loadResult.Blocked ||
          (loadResult.Value != nullptr &&
           loadResult.Value->getType() != returnType.getElementType(index))) {
        result.FailureReason = "unsafe callsite return load";
        return result;
      }
      rewrite.ReturnLoads.push_back(loadResult.Load);
      rewrite.ReturnValues.push_back(loadResult.Value);
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
      llvm::Value *value = index < callsite.ReturnValues.size()
                               ? callsite.ReturnValues[index]
                               : load;
      if (value == nullptr) {
        continue;
      }
      llvm::Value *field =
          builder.CreateExtractValue(newCall, {static_cast<unsigned>(index)});
      if (index < callsite.ReturnLoadResults.size() &&
          callsite.ReturnLoadResults[index].SharedSuccessor != nullptr) {
        if (load == nullptr) {
          continue;
        }
        llvm::StringRef registerName =
            index < callsite.ReturnRegisterNames.size()
                ? llvm::StringRef(callsite.ReturnRegisterNames[index])
                : llvm::StringRef();
        replaceSharedSuccessorReturnLoad(*load, *field,
                                         callsite.ReturnLoadResults[index],
                                         registerName);
        continue;
      }
      value->replaceAllUsesWith(field);
      if (auto *inst = llvm::dyn_cast<llvm::Instruction>(value)) {
        if (inst->use_empty()) {
          inst->eraseFromParent();
        }
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
    rewrite.InputStores.reserve(inputs.size());
    for (uint64_t index = 0; index < inputs.size(); ++index) {
      std::optional<llvm::Value *> argument = callsiteInputValueBeforeCall(
          *call, inputs[index], recoveredType.getParamType(index));
      if (!argument) {
        result.FailureReason = "unsafe callsite input value";
        return result;
      }
      rewrite.Arguments.push_back(*argument);
      llvm::StoreInst *inputStore = nullptr;
      if (inputs[index].StorageKind == "register") {
        inputStore = localCallsiteInputStoreBeforeCall(
            *call, inputs[index].RegisterName, recoveredType.getParamType(index));
        if (inputStore != nullptr &&
            (inputStore->getValueOperand() != *argument ||
             call->getFunction()->getMetadata(
                 "notdec.prototype.return_candidates") != nullptr ||
             !callClobbersRegister(*call, inputs[index].RegisterName))) {
          inputStore = nullptr;
        }
      }
      rewrite.InputStores.push_back(inputStore);
    }
    rewrite.ReturnLoads.reserve(returns.size());
    rewrite.ReturnValues.reserve(returns.size());
    rewrite.ReturnLoadResults.reserve(returns.size());
    rewrite.ReturnRegisterNames.reserve(returns.size());
    for (uint64_t index = 0; index < returns.size(); ++index) {
      ReturnLoadSearchResult loadResult =
          findCallsiteReturnLoad(*call, returns[index].RegisterName, true);
      if (loadResult.Blocked ||
          (loadResult.Value != nullptr &&
           loadResult.Value->getType() != returnType.getElementType(index))) {
        result.FailureReason = "unsafe callsite return load";
        return result;
      }
      rewrite.ReturnLoads.push_back(loadResult.Load);
      rewrite.ReturnValues.push_back(loadResult.Value);
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
    eraseCallsiteInputStores(callsite.InputStores);
    for (uint64_t index = 0; index < callsite.ReturnLoads.size(); ++index) {
      llvm::LoadInst *load = callsite.ReturnLoads[index];
      llvm::Value *value = index < callsite.ReturnValues.size()
                               ? callsite.ReturnValues[index]
                               : load;
      if (value == nullptr) {
        continue;
      }
      llvm::Value *field =
          builder.CreateExtractValue(newCall, {static_cast<unsigned>(index)});
      if (index < callsite.ReturnLoadResults.size() &&
          callsite.ReturnLoadResults[index].SharedSuccessor != nullptr) {
        if (load == nullptr) {
          continue;
        }
        llvm::StringRef registerName =
            index < callsite.ReturnRegisterNames.size()
                ? llvm::StringRef(callsite.ReturnRegisterNames[index])
                : llvm::StringRef();
        replaceSharedSuccessorReturnLoad(*load, *field,
                                         callsite.ReturnLoadResults[index],
                                         registerName);
        continue;
      }
      value->replaceAllUsesWith(field);
      if (auto *inst = llvm::dyn_cast<llvm::Instruction>(value)) {
        if (inst->use_empty()) {
          inst->eraseFromParent();
        }
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

std::optional<NativeParamTrial> matchingReturnTrialForSlot(
    llvm::ArrayRef<NativeParamTrial> trials, uint64_t slot) {
  for (const NativeParamTrial &trial : trials) {
    if (trial.Slot == slot) {
      return trial;
    }
  }
  return std::nullopt;
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

std::vector<NativeParamTrial> returnTrialsFromAllPredecessors(
    llvm::BasicBlock &block, const NativePrototypeModel &model) {
  std::vector<llvm::BasicBlock *> predecessors;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    predecessors.push_back(predecessor);
  }
  if (predecessors.size() < 2) {
    return {};
  }

  std::vector<NativeParamTrial> firstTrials =
      returnTrialsBeforeInstruction(*predecessors.front()->getTerminator(),
                                    model);
  if (firstTrials.empty()) {
    return {};
  }

  std::vector<NativeParamTrial> result;
  for (const NativeParamTrial &firstTrial : firstTrials) {
    bool allPredecessorsMatch = true;
    std::vector<llvm::StoreInst *> stores;
    if (firstTrial.Store != nullptr) {
      stores.push_back(firstTrial.Store);
    }

    for (llvm::BasicBlock *predecessor :
         llvm::ArrayRef<llvm::BasicBlock *>(predecessors).drop_front()) {
      std::vector<NativeParamTrial> trials =
          returnTrialsBeforeInstruction(*predecessor->getTerminator(), model);
      std::optional<NativeParamTrial> matching =
          matchingReturnTrialForSlot(trials, firstTrial.Slot);
      if (!matching || firstTrial.Value == nullptr ||
          matching->Value == nullptr ||
          !sameReturnStoreValue(*firstTrial.Value, *matching->Value)) {
        allPredecessorsMatch = false;
        break;
      }
      if (matching->Store != nullptr) {
        stores.push_back(matching->Store);
      }
    }

    if (!allPredecessorsMatch) {
      continue;
    }
    NativeParamTrial trial = firstTrial;
    trial.Store = stores.empty() ? nullptr : stores.front();
    result.push_back(std::move(trial));
  }
  return result;
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

  trials = returnTrialsFromAllPredecessors(*ret.getParent(), model);
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
  truncateKnownNoReturnDeclarationCalls(module);
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
    for (NativeParamTrial &trial : stackInputTrials(function, model, *abi)) {
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
          if (trial.Value != nullptr &&
              valueIsStackFrameExternalInputDerived(*trial.Value, *abi)) {
            continue;
          }
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
    eraseStackCanaryChecks(module, *abi);
    NativePrototypeModuleRewriteSummary rewriteSummary =
        rewriteNativeRecoveredPrototypes(module);
    truncateKnownNoReturnDeclarationCalls(module);
    eraseNoReturnDeclarationCallFrameStores(module, *abi);
    rewriteDeclarationCallOutputs(module, model);
    rewriteDeclarationCallInputs(module, *abi, model);
    annotateKnownExternalDeclarationPrototypes(module, *abi, model);
    eraseRewrittenInternalCallInputStores(module);
    eraseDeadKilledByCallRegisterStores(module, *abi);
    eraseUnusedInternalCallKilledInputStores(module, *abi);
    eraseUnusedDeclarationCallStackFrameRegisterStores(module, *abi);
    eraseUnusedInternalCallStackFrameRegisterStores(module, *abi);
    rewriteStaticStackMemoryAccesses(module, *abi);
    eraseUnusedDeclarationCallStackFrameRegisterStores(module, *abi);
    eraseUnusedRawStackFrameLoads(module, *abi);
    eraseDeadStackFrameRegisterStores(module, *abi);
    replaceStoredFramePointerRegisterLoads(module, *abi);
    eraseDeadFrameBaseRegisterStores(module, *abi);
    eraseUnusedRawStackFrameLoads(module, *abi);
    eraseDeadStackFrameRegisterStores(module, *abi);
    eraseDeadNativeStackAllocas(module);
    eraseDeadNonReturnVectorStores(module);
    eraseDeadStackFrameDerivedNonReturnStores(module, *abi);
    eraseDeadPreservedRegisterRestoreStores(module, *abi);
    rewriteCallerFrameAccesses(module, *abi);
    eraseDeadInternalCallArgumentRegisterStores(module);
    eraseDeadNativeStackAllocas(module);
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
    if (param.StorageKind != "register" && param.StorageKind != "stack") {
      return std::nullopt;
    }
    if (param.Size == 0 || param.Size > 8) {
      return std::nullopt;
    }
    paramTypes.push_back(
        llvm::IntegerType::get(context, static_cast<unsigned>(param.Size * 8)));
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

bool supportedNativeInputParamType(llvm::Type *type) {
  auto *integer = llvm::dyn_cast<llvm::IntegerType>(type);
  if (integer == nullptr) {
    return false;
  }
  unsigned width = integer->getBitWidth();
  return width != 0 && width <= 64 && width % 8 == 0;
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
    NativePrototypeInputBinding binding;
    binding.Param = param;
    if (param.StorageKind == "register") {
      std::optional<llvm::LoadInst *> load =
          uniqueExternalInputLoad(function, param.RegisterName);
      if (!load) {
        return std::nullopt;
      }
      binding.ExternalInputLoad = *load;
    } else if (param.StorageKind == "stack") {
      std::optional<llvm::LoadInst *> load =
          uniqueStackInputLoad(function, param.StackSpace, param.StackOffset,
                               param.Size);
      if (!load) {
        return std::nullopt;
      }
      binding.StackInputLoad = *load;
    } else {
      return std::nullopt;
    }
    bindings.push_back(std::move(binding));
  }
  return bindings;
}

llvm::LoadInst *inputBindingLoad(const NativePrototypeInputBinding &binding) {
  if (binding.ExternalInputLoad != nullptr) {
    return binding.ExternalInputLoad;
  }
  return binding.StackInputLoad;
}

void eraseReplacedInputLoad(llvm::LoadInst &load) {
  llvm::Value *pointer = load.getPointerOperand();
  load.eraseFromParent();
  llvm::RecursivelyDeleteTriviallyDeadInstructions(pointer);
}

bool hasStackInputBinding(
    llvm::ArrayRef<NativePrototypeInputBinding> inputBindings) {
  for (const NativePrototypeInputBinding &binding : inputBindings) {
    if (binding.StackInputLoad != nullptr ||
        binding.Param.StorageKind == "stack") {
      return true;
    }
  }
  return false;
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
    if (!supportedNativeInputParamType(paramType)) {
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
    llvm::LoadInst *inputLoad = inputBindingLoad((*inputBindings)[index]);
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
    llvm::LoadInst *inputLoad = inputBindingLoad(binding);
    argument->setName(inputLoad->getName());
    inputLoad->replaceAllUsesWith(&*argument);
    if (inputLoad->use_empty()) {
      eraseReplacedInputLoad(*inputLoad);
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
    if (!supportedNativeInputParamType(paramType)) {
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
    llvm::LoadInst *inputLoad = inputBindingLoad((*inputBindings)[index]);
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
    llvm::LoadInst *inputLoad = inputBindingLoad(binding);
    argument->setName(inputLoad->getName());
    if (returnValue == inputLoad) {
      returnValue = &*argument;
    }
    inputLoad->replaceAllUsesWith(&*argument);
    if (inputLoad->use_empty()) {
      eraseReplacedInputLoad(*inputLoad);
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
    if (!supportedNativeInputParamType(paramType)) {
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
    llvm::LoadInst *inputLoad = inputBindingLoad((*inputBindings)[index]);
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
    llvm::LoadInst *inputLoad = inputBindingLoad(binding);
    argument->setName(inputLoad->getName());
    for (NativePrototypeReturnBinding &returnBinding : *returnBindings) {
      if (returnBinding.ReturnValue == inputLoad) {
        returnBinding.ReturnValue = &*argument;
      }
    }
    inputLoad->replaceAllUsesWith(&*argument);
    if (inputLoad->use_empty()) {
      eraseReplacedInputLoad(*inputLoad);
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
