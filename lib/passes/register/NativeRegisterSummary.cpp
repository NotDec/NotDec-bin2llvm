#include "notdec-bin2llvm/passes/register/NativeRegisterSummary.h"

#include "core/NotDecMetadata.h"
#include "passes/register/RegisterModel.h"

#include "notdec-bin2llvm/abi/NativeAbi.h"
#include "notdec-bin2llvm/abi/NativeExternalPrototype.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/stack/NativeStackAddress.h"
#include "notdec-bin2llvm/passes/vararg/NativeVarArgRewrite.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

NativeRegisterSummary
runNativeRegisterSummary(llvm::Module &module,
                         const NativeRegisterSummaryOptions &options) {
  Analyzer analyzer(module, options);
  NativeRegisterSummary summary = analyzer.run();
  if (options.PrintSummary) {
    printNativeRegisterSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSummary(const NativeRegisterSummary &summary,
                                llvm::raw_ostream &os) {
  os << "Native register summary: functions=" << summary.FunctionsSeen
     << " noreturn=" << summary.NoReturnFunctions
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " calls=" << summary.CallsSeen
     << " read_entry=" << summary.ReadEntryRegisters
     << " modified=" << summary.ModifiedRegisters
     << " preserved=" << summary.PreservedRegisters
     << " demanded_returns=" << summary.DemandedReturns << "\n";
  for (const NativeRegisterSummaryFunction &function : summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen << " calls=" << function.CallsSeen
       << " noreturn=" << (function.NoReturn ? "true" : "false")
       << " read_entry=" << function.ReadEntryRegisters
       << " modified=" << function.ModifiedRegisters
       << " preserved=" << function.PreservedRegisters
       << " demanded_returns=" << function.DemandedReturns << "\n";
  }
}

} // namespace notdec::bin2llvm
