#include "notdec-bin2llvm/passes/summary/NativeExternalCallSignatureRewrite.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

constexpr llvm::StringLiteral CallArgValuesBundleTag =
    "notdec.register.summary_ssa.call_arg_values";

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

struct KnownExternalPrototype {
  unsigned FixedArgs = 0;
  bool VarArg = false;
};

struct ResolvedSymbolPlan {
  unsigned FixedArgs = 0;
  bool VarArg = false;
  bool ReplaceDeclaration = false;
  std::vector<CallRewritePlan *> Calls;
};

// This table is deliberately about ABI shape only.  Types stay as integer
// register values here; real C type recovery is a later pass.
const std::map<llvm::StringRef, KnownExternalPrototype> &
knownExternalPrototypes() {
  static const std::map<llvm::StringRef, KnownExternalPrototype> prototypes = {
      {"__assert_fail", {4, false}},
      {"__errno_location", {0, false}},
      {"__explicit_bzero_chk", {3, false}},
      {"__fdelt_chk", {1, false}},
      {"__fprintf_chk", {3, true}},
      {"__isoc23_strtol", {3, false}},
      {"__memcpy_chk", {4, false}},
      {"__memset_chk", {4, false}},
      {"__printf_chk", {2, true}},
      {"__snprintf_chk", {4, true}},
      {"__sprintf_chk", {3, true}},
      {"__strcat_chk", {3, false}},
      {"__stack_chk_fail", {0, false}},
      {"__tls_get_addr", {1, false}},
      {"__vasprintf_chk", {3, true}},
      {"abort", {0, false}},
      {"alarm", {1, false}},
      {"arc4random_buf", {2, false}},
      {"bind", {3, false}},
      {"calloc", {2, false}},
      {"chdir", {1, false}},
      {"clock_gettime", {2, false}},
      {"close", {1, false}},
      {"connect", {3, false}},
      {"dcgettext", {3, false}},
      {"dlsym", {2, false}},
      {"event_add", {2, false}},
      {"event_base_set", {2, false}},
      {"event_del", {1, false}},
      {"event_initialized", {1, false}},
      {"event_once", {5, false}},
      {"event_set", {5, false}},
      {"exit", {1, false}},
      {"fclose", {1, false}},
      {"fcntl", {2, true}},
      {"fcntl64", {2, true}},
      {"fflush", {1, false}},
      {"fgets", {3, false}},
      {"fopen", {2, false}},
      {"fprintf", {2, true}},
      {"fread", {4, false}},
      {"free", {1, false}},
      {"freeaddrinfo", {1, false}},
      {"fseek", {3, false}},
      {"fstat64", {2, false}},
      {"ftell", {1, false}},
      {"fwrite", {4, false}},
      {"getenv", {1, false}},
      {"getpwnam", {1, false}},
      {"getsockname", {3, false}},
      {"gettimeofday", {2, false}},
      {"gmtime_r", {2, false}},
      {"inet_ntop", {4, false}},
      {"ioctl", {2, true}},
      {"listen", {2, false}},
      {"malloc", {1, false}},
      {"malloc_usable_size", {1, false}},
      {"memcmp", {3, false}},
      {"memcpy", {3, false}},
      {"memmove", {3, false}},
      {"memset", {3, false}},
      {"mprotect", {3, false}},
      {"munmap", {2, false}},
      {"open", {2, true}},
      {"open64", {2, true}},
      {"perror", {1, false}},
      {"printf", {1, true}},
      {"pthread_cond_signal", {1, false}},
      {"pthread_join", {2, false}},
      {"pthread_key_create", {2, false}},
      {"pthread_key_delete", {1, false}},
      {"pthread_mutex_lock", {1, false}},
      {"pthread_mutex_unlock", {1, false}},
      {"pthread_setspecific", {2, false}},
      {"pthread_sigmask", {3, false}},
      {"puts", {1, false}},
      {"raise", {1, false}},
      {"read", {3, false}},
      {"realloc", {2, false}},
      {"shutdown", {2, false}},
      {"sigaction", {3, false}},
      {"sigdelset", {2, false}},
      {"signal", {2, false}},
      {"sigprocmask", {3, false}},
      {"snprintf", {3, true}},
      {"socket", {3, false}},
      {"sscanf", {2, true}},
      {"stat", {2, false}},
      {"strcasecmp", {2, false}},
      {"strcat", {2, false}},
      {"strchr", {2, false}},
      {"strcmp", {2, false}},
      {"strcpy", {2, false}},
      {"strcspn", {2, false}},
      {"strdup", {1, false}},
      {"strerror", {1, false}},
      {"strftime", {4, false}},
      {"strlen", {1, false}},
      {"strlcat", {3, false}},
      {"strlcpy", {3, false}},
      {"strncasecmp", {3, false}},
      {"strncmp", {3, false}},
      {"strncpy", {3, false}},
      {"strrchr", {2, false}},
      {"strsep", {2, false}},
      {"strstr", {2, false}},
      {"strtol", {3, false}},
      {"syscall", {1, true}},
      {"sysinfo", {1, false}},
      {"time", {1, false}},
      {"uname", {1, false}},
      {"unlink", {1, false}},
      {"waitpid", {3, false}},
      {"write", {3, false}},
  };
  return prototypes;
}

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
  std::optional<llvm::OperandBundleUse> values =
      call.getOperandBundle(CallArgValuesBundleTag);
  if (!values || values->Inputs.size() < *count) {
    return {};
  }
  for (unsigned index = 0; index < *count; ++index) {
    llvm::Value *value = values->Inputs[index].get();
    if (value == nullptr) {
      return {};
    }
    seen[index] = true;
    args[index] = CallArgBinding{nullptr, value, index};
  }

  bool complete = true;
  for (bool item : seen) {
    complete &= item;
  }
  if (complete) {
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
      if (!index || *index >= *count || args[*index].Store != nullptr) {
        continue;
      }
      args[*index].Store = store;
    }
    return args;
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

std::string countsForWarning(const std::vector<CallRewritePlan> &callPlans) {
  std::set<unsigned> counts;
  for (const CallRewritePlan &plan : callPlans) {
    counts.insert(plan.ArgCount);
  }
  std::string result;
  llvm::raw_string_ostream os(result);
  bool first = true;
  for (unsigned count : counts) {
    if (!first) {
      os << ",";
    }
    first = false;
    os << count;
  }
  os.flush();
  return result;
}

bool allUsesCovered(llvm::Function &callee,
                    const std::vector<CallRewritePlan> &callPlans) {
  std::set<const llvm::CallInst *> planned;
  for (const CallRewritePlan &plan : callPlans) {
    planned.insert(plan.Call);
  }
  for (const llvm::Use &use : callee.uses()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(use.getUser());
    if (call == nullptr || planned.count(call) == 0) {
      return false;
    }
  }
  return true;
}

ResolvedSymbolPlan resolveSymbolPlan(
    llvm::Function &callee, std::vector<CallRewritePlan> &callPlans,
    NativeExternalCallSignatureRewriteSummary &summary) {
  ResolvedSymbolPlan resolved;
  if (callPlans.empty()) {
    return resolved;
  }

  auto knownIt = knownExternalPrototypes().find(callee.getName());
  if (knownIt != knownExternalPrototypes().end()) {
    const KnownExternalPrototype &prototype = knownIt->second;
    resolved.FixedArgs = prototype.FixedArgs;
    resolved.VarArg = prototype.VarArg;
    for (CallRewritePlan &plan : callPlans) {
      if (plan.ArgCount < prototype.FixedArgs) {
        ++summary.CallsSkippedForMissingKnownArgs;
        continue;
      }
      if (!prototype.VarArg && plan.Args.size() > prototype.FixedArgs) {
        plan.Args.resize(prototype.FixedArgs);
        plan.ArgCount = prototype.FixedArgs;
      }
      resolved.Calls.push_back(&plan);
    }
    if (!resolved.Calls.empty()) {
      ++summary.SymbolsResolvedWithKnownPrototype;
    }
    resolved.ReplaceDeclaration =
        allUsesCovered(callee, callPlans) && resolved.Calls.size() == callPlans.size();
    return resolved;
  }

  unsigned minArgs = callPlans.front().ArgCount;
  bool conflict = false;
  for (const CallRewritePlan &plan : callPlans) {
    minArgs = std::min(minArgs, plan.ArgCount);
    conflict |= plan.ArgCount != callPlans.front().ArgCount;
  }
  std::string originalCounts = countsForWarning(callPlans);

  resolved.FixedArgs = minArgs;
  resolved.VarArg = false;
  for (CallRewritePlan &plan : callPlans) {
    if (plan.Args.size() > minArgs) {
      plan.Args.resize(minArgs);
      plan.ArgCount = minArgs;
    }
    resolved.Calls.push_back(&plan);
  }
  resolved.ReplaceDeclaration = !conflict && allUsesCovered(callee, callPlans);
  if (conflict) {
    ++summary.SymbolsResolvedWithMinimumArgs;
    llvm::errs() << "warning: external call signature conflict for @"
                 << callee.getName() << " counts={"
                 << originalCounts << "}; using minimum "
                 << minArgs << "\n";
  }
  return resolved;
}

llvm::FunctionType *callTypeForPlan(llvm::LLVMContext &context,
                                    llvm::Type *returnType,
                                    const CallRewritePlan &plan,
                                    const ResolvedSymbolPlan &resolved) {
  std::vector<llvm::Type *> params;
  params.reserve(resolved.FixedArgs);
  for (unsigned index = 0; index < resolved.FixedArgs; ++index) {
    params.push_back(plan.Args[index].Value->getType());
  }
  return llvm::FunctionType::get(returnType, params, resolved.VarArg);
}

llvm::Function *createReplacementDeclaration(llvm::Function &oldFunction,
                                             const CallRewritePlan &plan,
                                             const ResolvedSymbolPlan &resolved) {
  std::vector<llvm::Type *> params;
  params.reserve(resolved.FixedArgs);
  for (unsigned index = 0; index < resolved.FixedArgs; ++index) {
    params.push_back(plan.Args[index].Value->getType());
  }

  llvm::FunctionType *oldType = oldFunction.getFunctionType();
  llvm::FunctionType *newType =
      llvm::FunctionType::get(oldType->getReturnType(), params, resolved.VarArg);
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

llvm::CallInst *rewriteCall(CallRewritePlan &plan, llvm::Value &callee,
                            llvm::FunctionType &callType) {
  std::vector<llvm::Value *> args;
  args.reserve(plan.Args.size());
  for (const CallArgBinding &arg : plan.Args) {
    args.push_back(arg.Value);
  }

  llvm::SmallVector<llvm::OperandBundleDef, 1> bundles;
  plan.Call->getOperandBundlesAsDefs(bundles);
  llvm::SmallVector<llvm::OperandBundleDef, 1> keptBundles;
  for (const llvm::OperandBundleDef &bundle : bundles) {
    if (bundle.getTag() != CallArgValuesBundleTag) {
      keptBundles.push_back(bundle);
    }
  }

  llvm::CallInst *newCall = llvm::CallInst::Create(
      &callType, &callee, args, keptBundles, "", plan.Call->getIterator());
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

void stripCallArgValueBundles(llvm::Module &module) {
  llvm::SmallVector<llvm::StringRef, 16> tags;
  module.getOperandBundleTags(tags);
  std::optional<uint32_t> tagId;
  for (llvm::StringRef tag : tags) {
    if (tag == CallArgValuesBundleTag) {
      tagId = module.getContext().getOperandBundleTagID(tag);
      break;
    }
  }
  if (!tagId) {
    return;
  }

  std::vector<llvm::CallBase *> calls;
  for (llvm::Function &function : module) {
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && call->getOperandBundle(*tagId).has_value()) {
        calls.push_back(call);
      }
    }
  }
  for (llvm::CallBase *call : calls) {
    if (call->getParent() == nullptr) {
      continue;
    }
    llvm::CallBase *newCall =
        llvm::CallBase::removeOperandBundle(call, *tagId, call->getIterator());
    if (!call->use_empty()) {
      call->replaceAllUsesWith(newCall);
      newCall->takeName(call);
    }
    call->eraseFromParent();
  }
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

    ResolvedSymbolPlan resolved =
        resolveSymbolPlan(*callee, callPlans, summary);
    if (resolved.Calls.empty()) {
      skipped.insert(callee);
      summary.SymbolsSkippedForConflict += callPlans.size();
      continue;
    }

    llvm::Value *rewriteCallee = callee;
    if (resolved.ReplaceDeclaration) {
      rewriteCallee =
          createReplacementDeclaration(*callee, *resolved.Calls.front(), resolved);
    }

    std::vector<llvm::StoreInst *> storesToErase;
    for (CallRewritePlan *plan : resolved.Calls) {
      llvm::FunctionType *callType =
          callTypeForPlan(module.getContext(),
                          plan->Callee->getFunctionType()->getReturnType(), *plan,
                          resolved);
      rewriteCall(*plan, *rewriteCallee, *callType);
      ++summary.CallsRewritten;
      uint64_t storesSelected = 0;
      for (const CallArgBinding &arg : plan->Args) {
        if (arg.Store != nullptr &&
            arg.Store->getMetadata("notdec.register.summary_ssa.call_arg_store") !=
            nullptr) {
          storesToErase.push_back(arg.Store);
          ++storesSelected;
        }
      }
      for (NativeExternalCallSignatureRewriteFunctionSummary &fn :
           summary.Functions) {
        if (fn.FunctionName == plan->Caller->getName()) {
          ++fn.CallsRewritten;
          fn.StoresRemoved += storesSelected;
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
    if (resolved.ReplaceDeclaration && callee->use_empty()) {
      callee->eraseFromParent();
    }
  }

  stripCallArgValueBundles(module);

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
     << summary.SymbolsSkippedForConflict
     << " symbols_resolved_with_known_prototype="
     << summary.SymbolsResolvedWithKnownPrototype
     << " symbols_resolved_with_minimum_args="
     << summary.SymbolsResolvedWithMinimumArgs
     << " calls_skipped_for_missing_known_args="
     << summary.CallsSkippedForMissingKnownArgs << "\n";
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
