#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativePrototypeModel.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>

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
  std::vector<NativeParamTrial> trials = returnTrialsBeforeInstruction(ret, model);
  if (!trials.empty()) {
    return trials;
  }

  llvm::BasicBlock *predecessor = uniquePredecessor(*ret.getParent());
  if (predecessor == nullptr) {
    return {};
  }
  llvm::Instruction *terminator = predecessor->getTerminator();
  if (terminator == nullptr) {
    return {};
  }
  return returnTrialsBeforeInstruction(*terminator, model);
}

void addFunctionSummary(NativePrototypeRecoverySummary &total,
                        const NativePrototypeRecoveryFunctionSummary &function) {
  ++total.FunctionsSeen;
  total.ExternalInputsSeen += function.ExternalInputsSeen;
  total.InputCandidates += function.InputCandidates;
  total.ReturnCandidates += function.ReturnCandidates;
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
  for (const llvm::MDOperand &operand : node.operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      return std::nullopt;
    }
    std::optional<std::string> name = metadataField(*entry, "name");
    std::optional<uint64_t> slot = parseUint64Field(*entry, "slot");
    if (!name || !slot) {
      return std::nullopt;
    }
    NativeRecoveredPrototypeParam param;
    param.RegisterName = *name;
    param.Slot = *slot;
    params.push_back(std::move(param));
  }
  return params;
}

} // namespace

NativePrototypeRecoverySummary runNativePrototypeRecovery(
    llvm::Module &module, const NativePrototypeRecoveryOptions &options) {
  NativePrototypeRecoverySummary summary;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi) {
    return summary;
  }

  NativePrototypeModel model(*abi);
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    NativePrototypeRecoveryFunctionSummary functionSummary;
    functionSummary.FunctionName = function.getName().str();

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
    } else {
      function.setMetadata("notdec.prototype.recovered", nullptr);
    }
    addFunctionSummary(summary, functionSummary);
  }

  if (options.PrintSummary) {
    printNativePrototypeRecoverySummary(summary, llvm::errs());
  }
  return summary;
}

std::optional<NativeRecoveredPrototype>
readNativeRecoveredPrototypeMetadata(const llvm::Function &function) {
  llvm::MDNode *node = function.getMetadata("notdec.prototype.recovered");
  if (node == nullptr || node->getNumOperands() < 5) {
    return std::nullopt;
  }

  std::optional<std::string> model = metadataField(*node, "model");
  if (!model) {
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

  NativeRecoveredPrototype prototype;
  prototype.ModelName = *model;
  prototype.Inputs = std::move(*inputs);
  prototype.Returns = std::move(*returns);
  return prototype;
}

void printNativePrototypeRecoverySummary(
    const NativePrototypeRecoverySummary &summary, llvm::raw_ostream &os) {
  os << "native prototype recovery summary\n";
  os << "  functions: " << summary.FunctionsSeen << '\n';
  os << "  external inputs: " << summary.ExternalInputsSeen << '\n';
  os << "  input candidates: " << summary.InputCandidates << '\n';
  os << "  return candidates: " << summary.ReturnCandidates << '\n';
  for (const NativePrototypeRecoveryFunctionSummary &function :
       summary.Functions) {
    os << "  function " << function.FunctionName
       << ": external_inputs=" << function.ExternalInputsSeen
       << " input_candidates=" << function.InputCandidates
       << " return_candidates=" << function.ReturnCandidates << '\n';
  }
}

} // namespace notdec::bin2llvm
