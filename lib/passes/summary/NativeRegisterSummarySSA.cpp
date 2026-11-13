#include "notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h"

#include "NotDecIntrinsics.h"
#include "NotDecMetadata.h"
#include "passes/summary/RegisterModel.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeExternalPrototype.h"
#include "notdec-bin2llvm/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterPreSummaryPeephole.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/summary/NativeStackAddress.h"
#include "notdec-bin2llvm/passes/summary/NativeStackCanaryCleanup.h"
#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"
#include "notdec-bin2llvm/passes/summary/NativeVarArgRewrite.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

NativeRegisterValueSourceAudit
auditNativeRegisterValueSources(llvm::Module &module,
                                bool abiForAllFunctions) {
  return auditRegisterValueSourcesImpl(module, abiForAllFunctions);
}

NativeRegisterSummarySSASummary
runNativeRegisterSummarySSA(llvm::Module &module,
                            const NativeRegisterSummarySSAOptions &options) {
  NativeExternalPrototypeMap externalPrototypes =
      defaultNativeExternalPrototypes();
  if (!options.ExternalPrototypeJsonPath.empty()) {
    std::string errorMessage;
    std::optional<NativeExternalPrototypeMap> loadedPrototypes =
        loadNativeExternalPrototypesJson(options.ExternalPrototypeJsonPath,
                                         errorMessage);
    if (loadedPrototypes) {
      for (auto &[name, prototype] : *loadedPrototypes) {
        externalPrototypes[name] = std::move(prototype);
      }
    } else {
      llvm::errs() << "warning: failed to load external prototypes: "
                   << errorMessage << '\n';
    }
  }

  truncateKnownNoReturnCalls(module, externalPrototypes);

  NativeStackFrameRewriteSummary stackFrameSummary =
      runNativeStackFrameRewrite(module);
  NativeStackCanaryCleanupSummary canarySummary =
      runNativeStackCanaryCleanup(module);
  NativeRegisterSummarySSAOptions effectiveOptions = options;
  effectiveOptions.IgnoredRegisters.insert(
      stackFrameSummary.IgnoredRegisters.begin(),
      stackFrameSummary.IgnoredRegisters.end());
  std::map<llvm::GlobalVariable *, RegisterUnit> units =
      collectRegisterUnits(module);
  (void)canonicalizeRegisterPointerPhiLoads(module, units);
  NativeRegisterPreSummaryPeepholeSummary peepholeSummary =
      runNativeRegisterPreSummaryPeephole(module);
  if (options.PrintSummary) {
    printNativeRegisterPreSummaryPeepholeSummary(peepholeSummary, llvm::errs());
  }
  AbiFacts abi = collectAbiFacts(module, units);
  (void)canonicalizeX87WindowAccesses(module, units);

  // 变参恢复：识别编译器生成的寄存器保存区 + 手写 va_list 初始化，换成
  // llvm.va_start，并挂 !notdec.vararg 让签名推断带 ...（这样调用点只传固定
  // 参数 + 真正的变参，不再为 R8/R9 凑垃圾值）。语义与依据见
  // logs/20260919-01-native-vararg-recovery-plan.md。
  NativeVarArgRewriteOptions varArgOptions;
  varArgOptions.RewriteVarArgs =
      options.EnableVarArgRecovery && options.EnableRewrite;
  varArgOptions.AttachMetadata = options.EnableVarArgRecovery;
  NativeVarArgRewriteSummary varArgSummary =
      runNativeVarArgRewrite(module, varArgOptions);
  if (options.PrintSummary) {
    printNativeVarArgRewriteSummary(varArgSummary, llvm::errs());
  }

  NativeRegisterSummaryOptions baseSummaryOptions;
  baseSummaryOptions.AttachMetadata = false;
  baseSummaryOptions.IgnoredRegisters = effectiveOptions.IgnoredRegisters;
  baseSummaryOptions.ExternalCallShapes =
      buildExternalCallShapes(module, units, abi, externalPrototypes);
  baseSummaryOptions.UnknownExternalInputPolicy =
      NativeRegisterUnknownExternalInputPolicy::NoInputs;
  baseSummaryOptions.RunTopDownDemand = false;
  baseSummaryOptions.CollectExternalCallsiteEvidence = true;
  baseSummaryOptions.ExternalEvidenceSlots =
      unknownExternalEvidenceSlots(abi, module);
  NativeRegisterSummary baseRegisterSummary =
      runNativeRegisterSummary(module, baseSummaryOptions);
  std::map<llvm::Function *, FunctionSummaryFacts> baseFacts =
      summaryFactsByFunction(baseRegisterSummary, module);
  truncateKnownNoReturnCalls(module, externalPrototypes, &baseFacts);

  std::vector<NativeRegisterSummarySSAWarning> inferenceWarnings;
  NativeExternalCallsiteShapeMap externalCallsiteShapes =
      inferExternalCallShapes(externalPrototypes, baseRegisterSummary, units,
                              inferenceWarnings);

  NativeRegisterSummaryOptions finalSummaryOptions = baseSummaryOptions;
  finalSummaryOptions.AttachMetadata = options.AttachMetadata;
  finalSummaryOptions.RunTopDownDemand = true;
  finalSummaryOptions.ExternalCallShapes =
      buildExternalCallShapes(module, units, abi, externalPrototypes);
  finalSummaryOptions.ExternalCallsiteShapes = externalCallsiteShapes;
  finalSummaryOptions.CollectExternalCallsiteEvidence = false;
  finalSummaryOptions.ExternalEvidenceSlots.clear();
  NativeRegisterSummary registerSummary =
      runNativeRegisterSummary(module, finalSummaryOptions);
  std::map<llvm::Function *, FunctionSummaryFacts> facts =
      summaryFactsByFunction(registerSummary, module);
  truncateKnownNoReturnCalls(module, externalPrototypes, &facts);
  SignatureRewriteState signatureState;
  signatureState.ExternalPrototypes = &externalPrototypes;
  signatureState.ExternalCallsiteShapes = &externalCallsiteShapes;
  inferIndirectCallInputs(baseRegisterSummary, signatureState.IndirectCallInputs);
  signatureState.Warnings = std::move(inferenceWarnings);
  if (options.EnableRewrite) {
    signatureState.Shapes = buildInitialSignatureShapes(
        module, units, facts, abi, externalPrototypes,
        options.AbiForAllFunctions);
    propagateTailCallStackParams(module, signatureState);
    // 每个函数的调用约定标记（Arch2.md 第 13 节）：external = 参数/返回都按外部 ABI；
    // internal = 接口可改，可以用自定义约定。审计和后续 pass 只读这个标记，不再重算。
    if (options.AttachMetadata) {
      for (llvm::Function &function : module) {
        if (function.isIntrinsic()) {
          continue;
        }
        AbiConvention convention =
            abiConventionFor(function, options.AbiForAllFunctions);
        llvm::Metadata *fields[] = {llvm::MDString::get(
            module.getContext(),
            convention == AbiConvention::External ? "conventions=external"
                                                  : "conventions=internal")};
        function.setMetadata("notdec.abi",
                             llvm::MDNode::get(module.getContext(), fields));
      }
    }
  }

  NativeRegisterSummarySSASummary summary;
  summary.VarArgFunctionsDetected = varArgSummary.FunctionsDetected;
  summary.VarArgFunctionsRewritten = varArgSummary.FunctionsRewritten;
  summary.VarArgFunctions = varArgSummary.RewrittenFunctions;
  summary.StackCanaryChecksRemoved = canarySummary.CanaryChecksRemoved;
  summary.StackCanaryFailBlocksRemoved = canarySummary.FailBlocksRemoved;
  summary.StackCanaryFailFunctionsRemoved = canarySummary.FailFunctionsRemoved;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativeRegisterSummarySSAFunctionSummary fn;
    FunctionBuilder builder(function, units, facts, abi, options, fn,
                            signatureState);
    builder.run();
    addFunctionSummary(summary, fn);
    summary.Functions.push_back(std::move(fn));
  }
  if (options.EnableRewrite) {
    refineIndirectCallsiteParamShapes(signatureState);
    refineInternalStackParamShapes(signatureState);
    addDemandedExternalReturns(signatureState, units, abi, externalPrototypes);
    markSignatureCallArgStores(signatureState, summary);
    rewriteSignatureShapes(module, signatureState, units, abi, summary);
    // Stage 2：签名/SSA 都稳定之后，把"自己取变参"的手写菱形折成 va_arg。
    (void)foldManualVaArgSequences(module);
    eraseUnusedSummaryHelperDeclarations(module);
    // 重写会用 takeName 替换函数对象（createReplacementFunction），按旧函数
    // 指针组织的 facts 在清理遍查不到（rangeMayComeFromEntry 等走缺省回退）。
    // summary 内部按函数名记录，重写后按名字重新解析一次，清理遍才能用上
    // 分析结果（比如 MayEntry=false 的寄存器不允许入口读）。
    std::map<llvm::Function *, FunctionSummaryFacts> cleanupFacts =
        summaryFactsByFunction(registerSummary, module);
    if (options.EnableResidueRemoval) {
      constexpr unsigned maxPostRewriteCleanupIterations = 10;
      for (unsigned iteration = 0; iteration < maxPostRewriteCleanupIterations;
           ++iteration) {
        bool instCombineChanged = false;
        if (effectiveOptions.EnablePostRewriteInstCombine) {
          instCombineChanged = runPostRewriteInstCombine(module);
          // 第一轮之后 InstCombine/SimplifyCFG 一点没改 IR：这一轮清理面对的 IR 和
          // 上一轮结束时完全相同，上一轮的"改写完再删死值"已经是同一个 IR 上的定点，
          // 再跑一轮只是重复劳动。剩在 IR 里的死 store 不影响语义。
          if (iteration > 0 && !instCombineChanged) {
            break;
          }
        }

        uint64_t deadStoresRemovedThisIteration = 0;
        for (llvm::Function &function : module) {
          if (function.isDeclaration()) {
            continue;
          }
          NativeRegisterSummarySSAFunctionSummary cleanupFn;
          FunctionBuilder cleanup(function, units, cleanupFacts, abi, options,
                                  cleanupFn, signatureState);
          cleanup.removeDeadStoresAfterSignatureRewrite(
              /*collectReturns=*/iteration == 0);
          deadStoresRemovedThisIteration += cleanupFn.DeadStoresRemoved;
        }
        summary.DeadStoresRemoved += deadStoresRemovedThisIteration;

        if (!effectiveOptions.EnablePostRewriteInstCombine ||
            deadStoresRemovedThisIteration == 0) {
          break;
        }
      }
      NativeStackFrameCleanupOptions cleanupOptions;
      cleanupOptions.StackPointerRegister =
          stackFrameSummary.StackPointerRegister;
      cleanupOptions.Registers = effectiveOptions.IgnoredRegisters;
      NativeStackFrameCleanupSummary cleanupSummary =
          runNativeStackFrameCleanup(module, cleanupOptions);
      summary.StackFrameAccessesRewritten += cleanupSummary.AccessesRewritten;
      summary.StackFramePointerLoadsReplaced +=
          cleanupSummary.FramePointerLoadsReplaced;
      summary.StackFrameRegisterLoadsRemoved +=
          cleanupSummary.RegisterLoadsRemoved;
      summary.StackFrameRegisterStoresRemoved +=
          cleanupSummary.RegisterStoresRemoved;
      summary.StackFrameAllocaLoadsRemoved +=
          cleanupSummary.StackAllocaLoadsRemoved;
      summary.StackFrameAllocaStoresRemoved +=
          cleanupSummary.StackAllocaStoresRemoved;
      summary.StackFrameAllocasRemoved += cleanupSummary.StackAllocasRemoved;
      NativeStackCanaryCleanupSummary lateCanarySummary =
          runNativeStackCanaryCleanup(module);
      summary.StackCanaryChecksRemoved += lateCanarySummary.CanaryChecksRemoved;
      summary.StackCanaryFailBlocksRemoved +=
          lateCanarySummary.FailBlocksRemoved;
      summary.StackCanaryFailFunctionsRemoved +=
          lateCanarySummary.FailFunctionsRemoved;
      eraseDeadSummaryCallValueHelpers(module);
      summary.DeadLoadsRemoved += eraseDeadSummarySSAEntryReads(module);
      eraseUnusedSummaryHelperDeclarations(module);
    }
    // Signature rewrite and late stack cleanup can remove the final real use of
    // a summary call-value helper.  Do one unconditional sweep before emitting
    // warnings/output so dead clobber helpers do not leak into final IR when no
    // residue cleanup iteration was needed for them.
    eraseDeadSummaryCallValueHelpers(module);
    eraseUnusedSummaryHelperDeclarations(module);
  }
  summary.FunctionsSeen = summary.Functions.size();
  summary.Warnings = collectRemainingCallValueWarnings(module);
  summary.Warnings.insert(summary.Warnings.end(),
                          signatureState.Warnings.begin(),
                          signatureState.Warnings.end());
  if (options.PrintSummary) {
    printNativeRegisterSummarySSASummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSummarySSASummary(
    const NativeRegisterSummarySSASummary &summary, llvm::raw_ostream &os) {
  os << "Native register summary SSA: functions=" << summary.FunctionsSeen
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " loads_replaced=" << summary.LoadsReplaced
     << " dead_loads_removed=" << summary.DeadLoadsRemoved
     << " dead_stores_removed=" << summary.DeadStoresRemoved
     << " phis_created=" << summary.PhisCreated
     << " phis_simplified=" << summary.PhisSimplified
     << " range_entry_inputs=" << summary.RangeEntryInputs
     << " entry_range_rejected_reads=" << summary.EntryRangeRejectedReads
     << " call_returns=" << summary.CallReturnValues
     << " call_clobbers=" << summary.CallClobberValues
     << " call_arg_stores_marked=" << summary.CallArgStoresMarked
     << " calls_rewritten=" << summary.CallsRewritten
     << " functions_rewritten=" << summary.FunctionsRewritten
     << " preserved_calls=" << summary.PreservedCalls
     << " unknown_call_effects=" << summary.UnknownCallEffects
     << " stack_frame_accesses_rewritten="
     << summary.StackFrameAccessesRewritten
     << " stack_frame_pointer_loads_replaced="
     << summary.StackFramePointerLoadsReplaced
     << " stack_frame_register_loads_removed="
     << summary.StackFrameRegisterLoadsRemoved
     << " stack_frame_register_stores_removed="
     << summary.StackFrameRegisterStoresRemoved
     << " stack_frame_alloca_loads_removed="
     << summary.StackFrameAllocaLoadsRemoved
     << " stack_frame_alloca_stores_removed="
     << summary.StackFrameAllocaStoresRemoved
     << " stack_frame_allocas_removed=" << summary.StackFrameAllocasRemoved
     << " vararg_functions_detected=" << summary.VarArgFunctionsDetected
     << " vararg_functions_rewritten=" << summary.VarArgFunctionsRewritten
     << " stack_canary_checks_removed=" << summary.StackCanaryChecksRemoved
     << " stack_canary_fail_blocks_removed="
     << summary.StackCanaryFailBlocksRemoved
     << " stack_canary_fail_functions_removed="
     << summary.StackCanaryFailFunctionsRemoved
     << " partial_demand_candidates=" << summary.PartialDemandCandidates
     << " partial_demand_matched=" << summary.PartialDemandMatched
     << " partial_demand_rejected=" << summary.PartialDemandRejected
     << " range_registers_planned=" << summary.RangeRegistersPlanned
     << " range_segments_planned=" << summary.RangeSegmentsPlanned
     << " range_read_events=" << summary.RangeReadEvents
     << " range_write_events=" << summary.RangeWriteEvents
     << " range_clobber_events=" << summary.RangeClobberEvents
     << " warnings=" << summary.Warnings.size() << "\n";
  for (const NativeRegisterSummarySSAWarning &warning : summary.Warnings) {
    os << "  warning"
       << " function=" << warning.FunctionName
       << " callee=" << warning.CalleeName
       << " register=" << warning.RegisterName << " kind=" << warning.Kind
       << " reason=" << warning.Reason << " uses=" << warning.Uses;
    if (warning.RangeBitWidth != 0) {
      os << " range_offset=" << warning.RangeBitOffset
         << " range_width=" << warning.RangeBitWidth;
    }
    os << "\n";
  }
  for (const NativeRegisterSummarySSAFunctionSummary &function :
       summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen
       << " loads_replaced=" << function.LoadsReplaced
       << " dead_loads_removed=" << function.DeadLoadsRemoved
       << " dead_stores_removed=" << function.DeadStoresRemoved
       << " phis_created=" << function.PhisCreated
       << " phis_simplified=" << function.PhisSimplified
       << " range_entry_inputs=" << function.RangeEntryInputs
       << " entry_range_rejected_reads=" << function.EntryRangeRejectedReads
       << " call_returns=" << function.CallReturnValues
       << " call_clobbers=" << function.CallClobberValues
       << " call_arg_stores_marked=" << function.CallArgStoresMarked
       << " calls_rewritten=" << function.CallsRewritten
       << " functions_rewritten=" << function.FunctionsRewritten
       << " preserved_calls=" << function.PreservedCalls
       << " unknown_call_effects=" << function.UnknownCallEffects
       << " partial_demand_candidates=" << function.PartialDemandCandidates
       << " partial_demand_matched=" << function.PartialDemandMatched
       << " partial_demand_rejected=" << function.PartialDemandRejected
       << " range_registers_planned=" << function.RangeRegistersPlanned
       << " range_segments_planned=" << function.RangeSegmentsPlanned
       << " range_read_events=" << function.RangeReadEvents
       << " range_write_events=" << function.RangeWriteEvents
       << " range_clobber_events=" << function.RangeClobberEvents << "\n";
  }
}

} // namespace notdec::bin2llvm
