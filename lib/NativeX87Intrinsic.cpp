#include "notdec-bin2llvm/NativeX87Intrinsic.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <string>

namespace notdec::bin2llvm {

constexpr llvm::StringLiteral Prefix("notdec.x87.");

bool isNativeX87IntrinsicName(llvm::StringRef name) {
  return name.starts_with(Prefix);
}

llvm::Function *getOrInsertNativeX87Intrinsic(
    llvm::Module &module, llvm::StringRef name, llvm::Type *resultType,
    llvm::ArrayRef<llvm::Type *> argTypes, bool accessesMemory) {
  if (resultType == nullptr) {
    resultType = llvm::Type::getVoidTy(module.getContext());
  }
  llvm::FunctionType *type =
      llvm::FunctionType::get(resultType, argTypes, false);
  llvm::Function *function = llvm::cast<llvm::Function>(
      module.getOrInsertFunction(name, type).getCallee());
  if (accessesMemory) {
    // fstenv/fldenv pass a memory pointer to the library, which writes/reads
    // the whole FPU environment image there.  The call must be able to touch
    // normal memory, unlike the register-value intrinsics below.
    function->setMemoryEffects(llvm::MemoryEffects::unknown());
  } else {
    // The ST2..ST7 slots and the FPU environment live in hidden library
    // state, so an x87 intrinsic may read and write that state but must never
    // touch normal memory.  Marking memory effects as inaccessiblemem keeps
    // the calls from being dropped while load/store of the visible operands
    // stay analyzable; successive push/pop/peek/poke calls still alias the
    // same hidden state and are not reordered across each other.
    function->setMemoryEffects(
        llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::ModRef));
  }
  function->setDoesNotThrow();
  return function;
}

} // namespace notdec::bin2llvm
