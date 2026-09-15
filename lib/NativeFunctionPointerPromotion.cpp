#include "notdec-bin2llvm/NativeFunctionPointerPromotion.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace notdec::bin2llvm {
namespace {

bool isNativeRelocationSlot(const llvm::GlobalVariable &global) {
  return global.hasName() &&
         global.getName().starts_with("notdec.reloc.0x");
}

bool isNativeDataImage(const llvm::GlobalVariable &global) {
  return global.hasName() && global.getName().starts_with("notdec.image.0x");
}

std::optional<uint64_t> parseHexAddress(llvm::StringRef name,
                                        llvm::StringRef prefix) {
  if (!name.starts_with(prefix)) {
    return std::nullopt;
  }
  uint64_t address = 0;
  if (name.drop_front(prefix.size()).getAsInteger(16, address)) {
    return std::nullopt;
  }
  return address;
}

std::optional<llvm::Function *>
functionFromPointerToIntConstant(const llvm::Constant *constant) {
  auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(constant);
  if (expression == nullptr ||
      expression->getOpcode() != llvm::Instruction::PtrToInt ||
      expression->getNumOperands() != 1) {
    return std::nullopt;
  }
  auto *function = llvm::dyn_cast<llvm::Function>(expression->getOperand(0));
  if (function == nullptr) {
    return std::nullopt;
  }
  return function;
}

// A reference to a pointer-sized field of a modeled data image or to a legacy
// standalone relocation slot global.
struct SlotRef {
  llvm::GlobalVariable *Global = nullptr;
  int64_t Offset = 0;
};

// Decompose a pointer into (global, constant byte offset).  A non-constant GEP
// index means the access could touch any offset, so the caller must stay
// conservative; return false here for that case.
bool decomposeGlobalPointer(llvm::Value *pointer,
                            const llvm::DataLayout &layout,
                            llvm::GlobalVariable *&global, int64_t &offset) {
  pointer = pointer->stripPointerCasts();
  if (auto *variable = llvm::dyn_cast<llvm::GlobalVariable>(pointer)) {
    global = variable;
    offset = 0;
    return true;
  }
  auto *gep = llvm::dyn_cast<llvm::GEPOperator>(pointer);
  if (gep == nullptr) {
    return false;
  }
  llvm::APInt accumulated(layout.getIndexSizeInBits(0), 0);
  if (!gep->accumulateConstantOffset(layout, accumulated)) {
    return false;
  }
  auto *variable = llvm::dyn_cast<llvm::GlobalVariable>(
      gep->getPointerOperand()->stripPointerCasts());
  if (variable == nullptr) {
    return false;
  }
  global = variable;
  offset = accumulated.getSExtValue();
  return true;
}

// Field layout of a data image global: only packed struct fields built by
// materializeNativeDataImage are understood.
struct ImageField {
  int64_t Offset = 0;
  uint64_t Width = 0;
  llvm::Constant *Initializer = nullptr;
};

std::optional<ImageField> imageFieldAt(llvm::GlobalVariable &global,
                                       const llvm::DataLayout &layout,
                                       int64_t offset) {
  auto *structType = llvm::dyn_cast<llvm::StructType>(global.getValueType());
  if (structType == nullptr || !structType->isPacked() ||
      !global.hasInitializer()) {
    return std::nullopt;
  }
  auto *initializer =
      llvm::dyn_cast<llvm::ConstantStruct>(global.getInitializer());
  if (initializer == nullptr ||
      initializer->getNumOperands() != structType->getNumElements()) {
    return std::nullopt;
  }
  int64_t cursor = 0;
  for (unsigned index = 0; index < structType->getNumElements(); ++index) {
    llvm::Type *element = structType->getElementType(index);
    uint64_t size = layout.getTypeStoreSize(element).getFixedValue();
    if (static_cast<int64_t>(cursor) == offset) {
      ImageField field;
      field.Offset = cursor;
      field.Width = size;
      field.Initializer = initializer->getOperand(index);
      return field;
    }
    cursor += size;
  }
  return std::nullopt;
}

// Enumerate the symbolic function-pointer fields of a data image.
std::vector<std::pair<int64_t, llvm::Function *>>
imageFunctionSlots(llvm::GlobalVariable &global, const llvm::DataLayout &layout) {
  std::vector<std::pair<int64_t, llvm::Function *>> slots;
  auto *structType = llvm::dyn_cast<llvm::StructType>(global.getValueType());
  if (structType == nullptr || !structType->isPacked() ||
      !global.hasInitializer()) {
    return slots;
  }
  auto *initializer =
      llvm::dyn_cast<llvm::ConstantStruct>(global.getInitializer());
  if (initializer == nullptr) {
    return slots;
  }
  int64_t cursor = 0;
  for (unsigned index = 0; index < structType->getNumElements(); ++index) {
    llvm::Type *element = structType->getElementType(index);
    uint64_t size = layout.getTypeStoreSize(element).getFixedValue();
    if (auto function =
            functionFromPointerToIntConstant(initializer->getOperand(index))) {
      slots.emplace_back(cursor, *function);
    }
    cursor += size;
  }
  return slots;
}

// Visible writes to a global.  A dynamic-offset store, an address escape, or a
// store whose value is not a known function makes the whole object unknown;
// this is the conservative side of promotion.
struct GlobalWrites {
  bool Unknown = false;
  std::map<int64_t, llvm::Function *> Stores;
};

void scanGlobalUses(llvm::Value *value, int64_t offset, bool dynamic,
                    const llvm::DataLayout &layout, GlobalWrites &writes,
                    unsigned depth) {
  if (depth > 8) {
    writes.Unknown = true;
    return;
  }
  for (llvm::User *user : value->users()) {
    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(user)) {
      llvm::APInt accumulated(layout.getIndexSizeInBits(0), 0);
      bool constant = gep->accumulateConstantOffset(layout, accumulated);
      scanGlobalUses(gep,
                     constant ? offset + accumulated.getSExtValue() : offset,
                     dynamic || !constant, layout, writes, depth + 1);
      continue;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
      if (load->getPointerOperand() != value) {
        writes.Unknown = true;
      }
      continue;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
      if (store->getPointerOperand() != value || dynamic) {
        writes.Unknown = true;
        continue;
      }
      llvm::Function *target = nullptr;
      if (auto *constant =
              llvm::dyn_cast<llvm::Constant>(store->getValueOperand())) {
        if (auto function = functionFromPointerToIntConstant(constant)) {
          target = *function;
        }
      }
      writes.Stores[offset] = target;
      continue;
    }
    // The address escapes into a call, a ptrtoint, or another computation; an
    // external write can no longer be ruled out.
    writes.Unknown = true;
  }
}

struct SlotTargets {
  std::set<llvm::Function *> Functions;
  bool Unknown = false;
};

class PromotionAnalysis {
public:
  explicit PromotionAnalysis(llvm::Module &module)
      : Layout(module.getDataLayout()) {
    for (llvm::Function &function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      for (llvm::Instruction &instruction : llvm::instructions(function)) {
        for (const llvm::Use &operand : instruction.operands()) {
          auto *constant = llvm::dyn_cast<llvm::ConstantInt>(operand.get());
          if (constant != nullptr && constant->getBitWidth() <= 64) {
            RawAddressConstants.insert(constant->getZExtValue());
          }
        }
      }
    }
  }

  const GlobalWrites &writesFor(llvm::GlobalVariable &global) {
    auto it = WriteCache.find(&global);
    if (it != WriteCache.end()) {
      return it->second;
    }
    GlobalWrites writes;
    scanGlobalUses(&global, 0, false, Layout, writes, 0);
    return WriteCache.emplace(&global, writes).first->second;
  }

  // Slot address in the input image, when the referenced object is one of the
  // slots the data image pass saw.
  std::optional<uint64_t> slotAddress(const SlotRef &ref) const {
    if (isNativeRelocationSlot(*ref.Global)) {
      if (ref.Offset != 0) {
        return std::nullopt;
      }
      return parseHexAddress(ref.Global->getName(), "notdec.reloc.0x");
    }
    if (isNativeDataImage(*ref.Global)) {
      auto base = parseHexAddress(ref.Global->getName(), "notdec.image.0x");
      if (!base) {
        return std::nullopt;
      }
      return *base + static_cast<uint64_t>(ref.Offset);
    }
    return std::nullopt;
  }

  SlotTargets targetsFor(const SlotRef &ref) {
    SlotTargets targets;
    const GlobalWrites &writes = writesFor(*ref.Global);
    if (writes.Unknown) {
      targets.Unknown = true;
      return targets;
    }
    if (isNativeRelocationSlot(*ref.Global)) {
      if (!ref.Global->hasInitializer()) {
        targets.Unknown = true;
        return targets;
      }
      auto function =
          functionFromPointerToIntConstant(ref.Global->getInitializer());
      if (!function) {
        targets.Unknown = true;
        return targets;
      }
      targets.Functions.insert(*function);
    } else if (isNativeDataImage(*ref.Global)) {
      auto field = imageFieldAt(*ref.Global, Layout, ref.Offset);
      if (!field) {
        targets.Unknown = true;
        return targets;
      }
      auto function = functionFromPointerToIntConstant(field->Initializer);
      if (!function) {
        targets.Unknown = true;
        return targets;
      }
      targets.Functions.insert(*function);
    } else {
      targets.Unknown = true;
      return targets;
    }

    uint64_t width = 0;
    if (isNativeRelocationSlot(*ref.Global)) {
      width = Layout.getTypeStoreSize(ref.Global->getValueType())
                  .getFixedValue();
    } else if (auto field = imageFieldAt(*ref.Global, Layout, ref.Offset)) {
      width = field->Width;
    }
    for (const auto &[storeOffset, function] : writes.Stores) {
      if (storeOffset + static_cast<int64_t>(width) <= ref.Offset ||
          ref.Offset + static_cast<int64_t>(width) <= storeOffset) {
        continue;
      }
      if (function == nullptr) {
        targets.Unknown = true;
      } else {
        targets.Functions.insert(function);
      }
    }
    return targets;
  }

  // A slot address that also appears as a bare integer constant in the module
  // can be written through a path this pass cannot see (a callee that receives
  // the address as an argument); do not promote those.
  bool hasBareAddressConstant(uint64_t address) const {
    return RawAddressConstants.count(address) != 0;
  }

private:
  const llvm::DataLayout &Layout;
  std::map<llvm::GlobalVariable *, GlobalWrites> WriteCache;
  std::set<uint64_t> RawAddressConstants;
};

std::optional<SlotRef> slotLoadedByCall(llvm::Value *value,
                                        const llvm::DataLayout &layout) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
    llvm::GlobalVariable *global = nullptr;
    int64_t offset = 0;
    if (decomposeGlobalPointer(load->getPointerOperand(), layout, global,
                               offset)) {
      SlotRef ref;
      ref.Global = global;
      ref.Offset = offset;
      return ref;
    }
    return std::nullopt;
  }
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(value)) {
    return slotLoadedByCall(intToPtr->getOperand(0), layout);
  }
  if (auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    if ((constant->getOpcode() == llvm::Instruction::IntToPtr ||
         constant->getOpcode() == llvm::Instruction::BitCast) &&
        constant->getNumOperands() == 1) {
      return slotLoadedByCall(constant->getOperand(0), layout);
    }
  }
  if (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    if (cast->getNumOperands() == 1) {
      return slotLoadedByCall(cast->getOperand(0), layout);
    }
  }
  return std::nullopt;
}

void promoteCall(llvm::CallInst &oldCall, llvm::Function &target) {
  llvm::SmallVector<llvm::Value *, 8> arguments(oldCall.arg_begin(),
                                                oldCall.arg_end());
  llvm::IRBuilder<> builder(&oldCall);
  llvm::CallInst *newCall = builder.CreateCall(
      oldCall.getFunctionType(), &target, arguments, oldCall.getName());
  newCall->setCallingConv(oldCall.getCallingConv());
  newCall->setTailCallKind(oldCall.getTailCallKind());
  newCall->setAttributes(oldCall.getAttributes());
  newCall->copyMetadata(oldCall);
  newCall->setDebugLoc(oldCall.getDebugLoc());
  oldCall.replaceAllUsesWith(newCall);
  oldCall.eraseFromParent();
}

} // namespace

NativeFunctionPointerPromotionSummary runNativeFunctionPointerPromotion(
    llvm::Module &module,
    const NativeFunctionPointerPromotionOptions &options) {
  (void)options;
  NativeFunctionPointerPromotionSummary summary;
  PromotionAnalysis analysis(module);

  for (llvm::GlobalVariable &global : module.globals()) {
    if (isNativeRelocationSlot(global)) {
      SlotRef ref;
      ref.Global = &global;
      ref.Offset = 0;
      SlotTargets targets = analysis.targetsFor(ref);
      if (targets.Unknown) {
        ++summary.SlotsWithUnknownWrites;
      } else if (targets.Functions.size() > 1) {
        ++summary.SlotsWithMultipleTargets;
      } else if (targets.Functions.size() == 1) {
        ++summary.SlotsWithKnownSingleTarget;
      }
      continue;
    }
    if (isNativeDataImage(global)) {
      for (const auto &[offset, function] :
           imageFunctionSlots(global, module.getDataLayout())) {
        (void)function;
        SlotRef ref;
        ref.Global = &global;
        ref.Offset = offset;
        SlotTargets targets = analysis.targetsFor(ref);
        if (targets.Unknown) {
          ++summary.SlotsWithUnknownWrites;
        } else if (targets.Functions.size() > 1) {
          ++summary.SlotsWithMultipleTargets;
        } else if (targets.Functions.size() == 1) {
          ++summary.SlotsWithKnownSingleTarget;
        }
      }
    }
  }

  std::vector<llvm::CallInst *> calls;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (call == nullptr || call->getCalledFunction() != nullptr ||
          call->isMustTailCall()) {
        continue;
      }
      ++summary.IndirectCallsSeen;
      calls.push_back(call);
    }
  }

  for (llvm::CallInst *call : calls) {
    auto ref =
        slotLoadedByCall(call->getCalledOperand(), module.getDataLayout());
    if (!ref) {
      continue;
    }
    if (auto address = analysis.slotAddress(*ref)) {
      if (analysis.hasBareAddressConstant(*address)) {
        continue;
      }
    }
    SlotTargets targets = analysis.targetsFor(*ref);
    if (targets.Unknown || targets.Functions.size() != 1) {
      continue;
    }
    llvm::Function *target = *targets.Functions.begin();
    if (target->isDeclaration()) {
      continue;
    }
    promoteCall(*call, *target);
    ++summary.IndirectCallsPromoted;
  }
  return summary;
}

} // namespace notdec::bin2llvm
