#include "notdec-bin2llvm/passes/NativeExternalCallSignatureRewrite.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct CallArgBinding {
  llvm::StoreInst *Store = nullptr;
  llvm::Value *Value = nullptr;
  unsigned Index = 0;
};

struct CallRewritePlan {
  llvm::CallInst *Call = nullptr;
  llvm::Function *Callee = nullptr;
  llvm::Function *Caller = nullptr;
  unsigned ArgCount = 0;
  std::vector<CallArgBinding> Args;
};

std::optional<std::string> mdField(const llvm::MDNode *node,
                                   llvm::StringRef key) {
  if (node == nullptr) {
    return std::nullopt;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.starts_with(prefix)) {
      return value.drop_front(prefix.size()).str();
    }
  }
  return std::nullopt;
}

std::optional<unsigned> parseUnsigned(llvm::StringRef text) {
  unsigned value = 0;
  if (text.getAsInteger(10, value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<unsigned> mdUnsignedField(const llvm::MDNode *node,
                                        llvm::StringRef key) {
  std::optional<std::string> value = mdField(node, key);
  if (!value) {
    return std::nullopt;
  }
  return parseUnsigned(*value);
}

bool isRewritableExternalCall(const llvm::CallInst &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->isDeclaration() &&
         !callee->isIntrinsic() &&
         call.getMetadata("notdec.register.summary_ssa.call_args") != nullptr;
}

std::vector<CallArgBinding> collectCallArgBindings(llvm::CallInst &call) {
  std::optional<unsigned> count = mdUnsignedField(
      call.getMetadata("notdec.register.summary_ssa.call_args"), "count");
  if (!count || *count == 0) {
    return {};
  }

  std::vector<CallArgBinding> args(*count);
  std::vector<bool> seen(*count, false);
  for (auto it = call.getIterator(); it != call.getParent()->begin();) {
    --it;
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*it);
    if (store == nullptr) {
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&*it)) {
        llvm::Function *callee = otherCall->getCalledFunction();
        if (callee == nullptr || !callee->isIntrinsic()) {
          break;
        }
        continue;
      }
      if (it->mayWriteToMemory()) {
        break;
      }
      continue;
    }

    llvm::MDNode *metadata =
        store->getMetadata("notdec.register.summary_ssa.call_arg_store");
    std::optional<unsigned> index = mdUnsignedField(metadata, "index");
    if (!index || *index >= *count || seen[*index]) {
      continue;
    }

    seen[*index] = true;
    args[*index] = CallArgBinding{store, store->getValueOperand(), *index};
    bool complete = true;
    for (bool item : seen) {
      complete &= item;
    }
    if (complete) {
      return args;
    }
  }
  return {};
}

using SymbolPlans = std::map<llvm::Function *, std::vector<CallRewritePlan>>;

SymbolPlans collectPlans(
    llvm::Module &module, NativeExternalCallSignatureRewriteSummary &summary) {
  SymbolPlans plans;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    NativeExternalCallSignatureRewriteFunctionSummary fn;
    fn.FunctionName = function.getName().str();
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call == nullptr || !isRewritableExternalCall(*call)) {
          continue;
        }
        ++fn.CallsSeen;
        std::vector<CallArgBinding> args = collectCallArgBindings(*call);
        if (args.empty()) {
          continue;
        }
        plans[call->getCalledFunction()].push_back(
            CallRewritePlan{call, call->getCalledFunction(), call->getFunction(),
                            static_cast<unsigned>(args.size()),
                            std::move(args)});
      }
    }
    summary.CallsSeen += fn.CallsSeen;
    summary.Functions.push_back(std::move(fn));
  }
  summary.FunctionsSeen = summary.Functions.size();
  return plans;
}

llvm::Function *createReplacementDeclaration(llvm::Function &oldFunction,
                                             const CallRewritePlan &plan) {
  std::vector<llvm::Type *> params;
  params.reserve(plan.Args.size());
  for (const CallArgBinding &arg : plan.Args) {
    params.push_back(arg.Value->getType());
  }

  llvm::FunctionType *oldType = oldFunction.getFunctionType();
  llvm::FunctionType *newType =
      llvm::FunctionType::get(oldType->getReturnType(), params,
                              oldType->isVarArg());
  if (newType == oldType) {
    return &oldFunction;
  }

  llvm::Module *module = oldFunction.getParent();
  llvm::Function *newFunction =
      llvm::Function::Create(newType, oldFunction.getLinkage(),
                             oldFunction.getAddressSpace());
  newFunction->copyAttributesFrom(&oldFunction);
  newFunction->copyMetadata(&oldFunction, 0);
  newFunction->setComdat(oldFunction.getComdat());
  newFunction->setAttributes(
      llvm::AttributeList::get(oldFunction.getContext(),
                               oldFunction.getAttributes().getFnAttrs(),
                               oldFunction.getAttributes().getRetAttrs(),
                               std::vector<llvm::AttributeSet>(params.size())));
  module->getFunctionList().insert(oldFunction.getIterator(), newFunction);
  newFunction->takeName(&oldFunction);
  return newFunction;
}

llvm::AttributeList callAttributesForNewArgs(const llvm::CallBase &call,
                                             unsigned argCount) {
  const llvm::AttributeList &oldAttrs = call.getAttributes();
  if (oldAttrs.isEmpty()) {
    return {};
  }

  std::vector<llvm::AttributeSet> argAttrs(argCount);
  return llvm::AttributeList::get(call.getContext(), oldAttrs.getFnAttrs(),
                                  oldAttrs.getRetAttrs(), argAttrs);
}

llvm::CallInst *rewriteCall(CallRewritePlan &plan, llvm::Function &callee) {
  std::vector<llvm::Value *> args;
  args.reserve(plan.Args.size());
  for (const CallArgBinding &arg : plan.Args) {
    args.push_back(arg.Value);
  }

  llvm::SmallVector<llvm::OperandBundleDef, 1> bundles;
  plan.Call->getOperandBundlesAsDefs(bundles);

  llvm::CallInst *newCall = llvm::CallInst::Create(
      callee.getFunctionType(), &callee, args, bundles, "", plan.Call->getIterator());
  newCall->setTailCallKind(plan.Call->getTailCallKind());
  newCall->setCallingConv(plan.Call->getCallingConv());
  newCall->setAttributes(callAttributesForNewArgs(*plan.Call, args.size()));
  newCall->copyMetadata(*plan.Call);

  if (!plan.Call->use_empty()) {
    plan.Call->replaceAllUsesWith(newCall);
    newCall->takeName(plan.Call);
  }
  plan.Call->eraseFromParent();
  return newCall;
}

} // namespace

NativeExternalCallSignatureRewriteSummary
runNativeExternalCallSignatureRewrite(
    llvm::Module &module,
    const NativeExternalCallSignatureRewriteOptions &options) {
  NativeExternalCallSignatureRewriteSummary summary;
  SymbolPlans plans = collectPlans(module, summary);

  std::set<llvm::Function *> skipped;
  for (auto &[callee, callPlans] : plans) {
    if (callPlans.empty()) {
      continue;
    }
    unsigned argCount = callPlans.front().ArgCount;
    bool conflict = false;
    for (const CallRewritePlan &plan : callPlans) {
      conflict |= plan.ArgCount != argCount;
      if (plan.ArgCount == argCount) {
        for (unsigned index = 0; index < argCount; ++index) {
          conflict |= plan.Args[index].Value->getType() !=
                      callPlans.front().Args[index].Value->getType();
        }
      }
    }
    if (conflict) {
      skipped.insert(callee);
      summary.SymbolsSkippedForConflict += callPlans.size();
      continue;
    }

    llvm::Function *newCallee =
        createReplacementDeclaration(*callee, callPlans.front());
    std::vector<llvm::StoreInst *> storesToErase;
    for (CallRewritePlan &plan : callPlans) {
      rewriteCall(plan, *newCallee);
      ++summary.CallsRewritten;
      for (const CallArgBinding &arg : plan.Args) {
        if (arg.Store->getMetadata("notdec.register.summary_ssa.call_arg_store") !=
            nullptr) {
          storesToErase.push_back(arg.Store);
        }
      }
      for (NativeExternalCallSignatureRewriteFunctionSummary &fn :
           summary.Functions) {
        if (fn.FunctionName == plan.Caller->getName()) {
          ++fn.CallsRewritten;
          fn.StoresRemoved += plan.Args.size();
          break;
        }
      }
    }
    for (llvm::StoreInst *store : storesToErase) {
      if (store->getParent() != nullptr && store->use_empty()) {
        store->eraseFromParent();
        ++summary.StoresRemoved;
      }
    }
    if (callee->use_empty()) {
      callee->eraseFromParent();
    }
  }

  (void)skipped;
  if (options.PrintSummary) {
    printNativeExternalCallSignatureRewriteSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeExternalCallSignatureRewriteSummary(
    const NativeExternalCallSignatureRewriteSummary &summary,
    llvm::raw_ostream &os) {
  os << "Native external call signature rewrite: functions="
     << summary.FunctionsSeen << " calls_seen=" << summary.CallsSeen
     << " calls_rewritten=" << summary.CallsRewritten
     << " stores_removed=" << summary.StoresRemoved
     << " symbols_skipped_for_conflict="
     << summary.SymbolsSkippedForConflict << "\n";
  for (const NativeExternalCallSignatureRewriteFunctionSummary &function :
       summary.Functions) {
    os << "  " << function.FunctionName << ": calls_seen="
       << function.CallsSeen << " calls_rewritten=" << function.CallsRewritten
       << " stores_removed=" << function.StoresRemoved
       << " symbols_skipped_for_conflict="
       << function.SymbolsSkippedForConflict << "\n";
  }
}

} // namespace notdec::bin2llvm
