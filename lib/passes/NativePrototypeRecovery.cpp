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
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <exception>
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

llvm::MDNode *inputCandidateMetadata(llvm::LLVMContext &context,
                                     const NativeParamActive &active) {
  if (active.Trials.empty()) {
    return nullptr;
  }

  std::vector<llvm::Metadata *> entries;
  for (const NativeParamTrial &trial : active.Trials) {
    std::vector<llvm::Metadata *> fields = {
        llvm::MDString::get(context, "name=" + trial.RegisterName),
        llvm::MDString::get(context, "slot=" + std::to_string(trial.Slot)),
    };
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
    std::vector<llvm::Metadata *> fields = {
        llvm::MDString::get(context, "name=" + param.RegisterName),
        llvm::MDString::get(context, "slot=" + std::to_string(param.Slot)),
    };
    entries.push_back(llvm::MDNode::get(context, fields));
  }
  return llvm::MDNode::get(context, entries);
}

llvm::MDNode *recoveredPrototypeMetadata(
    llvm::LLVMContext &context, const NativeRecoveredPrototype &prototype) {
  if (prototype.Inputs.empty() && prototype.Returns.empty()) {
    return nullptr;
  }

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

std::optional<llvm::StoreInst *> uniqueRegisterAccessStore(
    llvm::Function &function, llvm::StringRef registerName) {
  llvm::StoreInst *result = nullptr;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
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
      if (result != nullptr) {
        return std::nullopt;
      }
      result = store;
    }
  }
  if (result == nullptr) {
    return std::nullopt;
  }
  return result;
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

std::optional<llvm::Value *> callsiteInputValueBeforeCall(
    llvm::CallInst &call, llvm::StringRef registerName, llvm::Type *paramType) {
  std::optional<llvm::Value *> localValue = registerStoreValueInReverseRange(
      llvm::BasicBlock::reverse_iterator(call.getIterator()),
      call.getParent()->rend(), registerName, paramType);
  if (localValue) {
    return localValue;
  }

  llvm::BasicBlock *predecessor = nullptr;
  for (llvm::BasicBlock *candidate : llvm::predecessors(call.getParent())) {
    if (predecessor != nullptr) {
      return std::nullopt;
    }
    predecessor = candidate;
  }
  if (predecessor == nullptr) {
    return std::nullopt;
  }
  return registerStoreValueInReverseRange(predecessor->rbegin(),
                                          predecessor->rend(), registerName,
                                          paramType);
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
  bool Blocked = false;
};

struct ReturnOnlyCallsiteCollectionResult {
  std::vector<llvm::CallInst *> Callsites;
  std::string FailureReason;
};

struct MultiReturnCallsiteRewrite {
  llvm::CallInst *Call = nullptr;
  std::vector<llvm::LoadInst *> ReturnLoads;
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
  std::vector<llvm::LoadInst *> ReturnLoads;
};

struct InputMultiReturnCallsiteCollectionResult {
  std::vector<InputMultiReturnCallsiteRewrite> Rewrites;
  std::string FailureReason;
};

ReturnLoadSearchResult findReturnLoadBeforeStoreInRange(
    llvm::BasicBlock::iterator iter, llvm::BasicBlock::iterator end,
    llvm::StringRef returnRegisterName) {
  for (; iter != end; ++iter) {
    llvm::MDNode *metadata = iter->getMetadata("notdec.register.access");
    if (metadata == nullptr) {
      continue;
    }
    std::optional<std::string> name = metadataField(*metadata, "name");
    if (!name || *name != returnRegisterName) {
      continue;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&*iter)) {
      return {load, false};
    }
    if (llvm::isa<llvm::StoreInst>(&*iter)) {
      return {nullptr, true};
    }
  }
  return {};
}

ReturnLoadSearchResult findCallsiteReturnLoad(llvm::CallInst &oldCall,
                                              llvm::StringRef returnRegisterName) {
  llvm::BasicBlock::iterator localIter(oldCall.getIterator());
  ReturnLoadSearchResult localResult = findReturnLoadBeforeStoreInRange(
      ++localIter, oldCall.getParent()->end(), returnRegisterName);
  if (localResult.Load != nullptr || localResult.Blocked) {
    return localResult;
  }

  std::set<llvm::BasicBlock *> visited;
  llvm::BasicBlock *current = oldCall.getParent();
  while (visited.insert(current).second) {
    llvm::BasicBlock *successor = nullptr;
    for (llvm::BasicBlock *candidate : llvm::successors(current)) {
      if (successor != nullptr) {
        return {nullptr, true};
      }
      successor = candidate;
    }
    if (successor == nullptr) {
      return {};
    }

    llvm::BasicBlock *predecessor = nullptr;
    for (llvm::BasicBlock *candidate : llvm::predecessors(successor)) {
      if (predecessor != nullptr) {
        return {nullptr, true};
      }
      predecessor = candidate;
    }
    if (predecessor != current) {
      return {nullptr, true};
    }

    ReturnLoadSearchResult successorResult = findReturnLoadBeforeStoreInRange(
        successor->begin(), successor->end(), returnRegisterName);
    if (successorResult.Load != nullptr || successorResult.Blocked) {
      return successorResult;
    }
    current = successor;
  }
  return {nullptr, true};
}

void rewriteCallsiteReturnLoad(llvm::CallInst &oldCall, llvm::CallInst &newCall,
                               llvm::StringRef returnRegisterName) {
  llvm::LoadInst *load =
      findCallsiteReturnLoad(oldCall, returnRegisterName).Load;
  if (load == nullptr) {
    return;
  }
  if (load->getType() == newCall.getType()) {
    load->replaceAllUsesWith(&newCall);
    if (load->use_empty()) {
      load->eraseFromParent();
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
    rewriteCallsiteReturnLoad(*callsite.Call, *newCall, returnRegisterName);
    callsite.Call->eraseFromParent();
  }
}

bool callsiteHasMismatchedReturnLoad(llvm::CallInst &callsite,
                                     llvm::StringRef returnRegisterName,
                                     llvm::Type *returnType) {
  ReturnLoadSearchResult result =
      findCallsiteReturnLoad(callsite, returnRegisterName);
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
    if (callsiteHasMismatchedReturnLoad(*call, returnRegisterName, returnType)) {
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
    rewriteCallsiteReturnLoad(*callsite, *newCall, returnRegisterName);
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
    for (uint64_t index = 0; index < returns.size(); ++index) {
      ReturnLoadSearchResult loadResult =
          findCallsiteReturnLoad(*call, returns[index].RegisterName);
      if (loadResult.Blocked || loadResult.Load == nullptr ||
          loadResult.Load->getType() != returnType.getElementType(index)) {
        result.FailureReason = "unsafe callsite return load";
        return result;
      }
      rewrite.ReturnLoads.push_back(loadResult.Load);
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
      llvm::Value *field =
          builder.CreateExtractValue(newCall, {static_cast<unsigned>(index)});
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
    for (uint64_t index = 0; index < returns.size(); ++index) {
      ReturnLoadSearchResult loadResult =
          findCallsiteReturnLoad(*call, returns[index].RegisterName);
      if (loadResult.Blocked || loadResult.Load == nullptr ||
          loadResult.Load->getType() != returnType.getElementType(index)) {
        result.FailureReason = "unsafe callsite return load";
        return result;
      }
      rewrite.ReturnLoads.push_back(loadResult.Load);
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
      llvm::Value *field =
          builder.CreateExtractValue(newCall, {static_cast<unsigned>(index)});
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
    if (!name) {
      continue;
    }
    std::optional<NativeStorageMatch> match = model.findOutputRegister(*name);
    if (!match) {
      continue;
    }
    if (!seenSlots.insert(match->Slot).second) {
      continue;
    }

    NativeParamTrial trial;
    trial.RegisterName = *name;
    trial.Slot = match->Slot;
    trial.ValueKey = returnValueKey(*store->getValueOperand());
    trial.Active = true;
    trials.push_back(std::move(trial));
  }
  return trials;
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

std::optional<std::vector<NativeRecoveredPrototypeParam>>
readRecoveredParamList(const llvm::MDNode &node) {
  std::vector<NativeRecoveredPrototypeParam> params;
  std::optional<uint64_t> previousSlot;
  for (const llvm::MDOperand &operand : node.operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      return std::nullopt;
    }
    std::optional<std::string> name = metadataField(*entry, "name");
    std::optional<uint64_t> slot = parseUint64Field(*entry, "slot");
    if (!name || name->empty() || !slot) {
      return std::nullopt;
    }
    if (previousSlot && *slot <= *previousSlot) {
      return std::nullopt;
    }
    NativeRecoveredPrototypeParam param;
    param.RegisterName = *name;
    param.Slot = *slot;
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
        trial.Slot = match->Slot;
        trial.Active = true;
        active.Trials.push_back(std::move(trial));
      }
    }
    sortTrialsBySlot(active);

    NativeParamActive returns;
    std::map<uint64_t, NativeParamTrial> returnTrialsBySlot;
    std::map<uint64_t, uint64_t> returnSlotCounts;
    std::map<uint64_t, uint64_t> returnSlotKeyCounts;
    std::map<uint64_t, std::string> returnSlotFirstKey;
    std::set<uint64_t> returnSlotKeyConflicts;
    uint64_t returnCount = 0;
    for (llvm::BasicBlock &block : function) {
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
        ++returnCount;
        for (NativeParamTrial &trial : returnTrialsBefore(*ret, model)) {
          uint64_t slot = trial.Slot;
          ++returnSlotCounts[slot];
          if (trial.ValueKey) {
            ++returnSlotKeyCounts[slot];
            auto [iter, inserted] =
                returnSlotFirstKey.try_emplace(slot, *trial.ValueKey);
            if (!inserted && iter->second != *trial.ValueKey) {
              returnSlotKeyConflicts.insert(slot);
            }
          }
          returnTrialsBySlot.try_emplace(slot, std::move(trial));
        }
      }
    }
    std::set<uint64_t> returnSlots;
    for (const auto &[slot, count] : returnSlotCounts) {
      if (count != returnCount) {
        continue;
      }
      if (returnSlotKeyCounts[slot] == returnCount &&
          returnSlotKeyConflicts.find(slot) != returnSlotKeyConflicts.end()) {
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
    if (llvm::MDNode *node =
            recoveredPrototypeMetadata(module.getContext(), recovered)) {
      function.setMetadata("notdec.prototype.recovered", node);
    } else if (previousRecovered && previousRecoveredMetadata != nullptr &&
               previousRecovered->ModelName == abi->PrototypeName) {
      std::optional<llvm::FunctionType *> previousType =
          buildNativeRecoveredPrototypeFunctionType(function.getContext(),
                                                   *previousRecovered);
      if (previousType && function.getFunctionType() == *previousType) {
        function.setMetadata("notdec.prototype.recovered",
                             previousRecoveredMetadata);
      } else {
        function.setMetadata("notdec.prototype.recovered", nullptr);
      }
    } else {
      function.setMetadata("notdec.prototype.recovered", nullptr);
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
  if (inputs->empty() && returns->empty()) {
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
    (void)param;
    paramTypes.push_back(registerType);
  }

  llvm::Type *returnType = llvm::Type::getVoidTy(context);
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
    result.Reason = "missing recovered prototype";
    return result;
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
  if (!prototype) {
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
  if (!prototype) {
    return std::nullopt;
  }

  std::vector<NativePrototypeReturnBinding> bindings;
  bindings.reserve(prototype->Returns.size());
  for (const NativeRecoveredPrototypeParam &param : prototype->Returns) {
    std::optional<llvm::StoreInst *> store =
        uniqueRegisterAccessStore(function, param.RegisterName);
    if (!store) {
      return std::nullopt;
    }

    NativePrototypeReturnBinding binding;
    binding.Param = param;
    binding.ReturnStore = *store;
    binding.ReturnValue = (*store)->getValueOperand();
    bindings.push_back(std::move(binding));
  }
  return bindings;
}

void eraseReturnBindingStores(
    llvm::ArrayRef<NativePrototypeReturnBinding> returnBindings) {
  for (const NativePrototypeReturnBinding &binding : returnBindings) {
    if (binding.ReturnStore != nullptr) {
      binding.ReturnStore->eraseFromParent();
    }
  }
}

void clearTransientPrototypeRecoveryMetadata(llvm::Function &function) {
  function.setMetadata("notdec.register.external_inputs", nullptr);
  function.setMetadata("notdec.prototype.input_candidates", nullptr);
  function.setMetadata("notdec.prototype.return_candidates", nullptr);
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
              (*recoveredType)->getReturnType())) {
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

  std::vector<llvm::Function *> functions;
  for (llvm::Function &function : module) {
    functions.push_back(&function);
  }

  for (llvm::Function *function : functions) {
    ++summary.FunctionsSeen;
    NativePrototypeRewriteResult result =
        rewriteNativeRecoveredPrototype(*function);
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
