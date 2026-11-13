#pragma once

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>

namespace llvm {
class BasicBlock;
class Function;
class GlobalVariable;
class Instruction;
class Module;
class Value;
} // namespace llvm

namespace notdec::bin2llvm {

// Every native stack address has one of three deliberately separate bases.
// EntryStackPointer is the only kind that can describe a callee ABI input.
// An aligned SP is not entry-equivalent, and a localized native frame is not
// an ABI input either, even when their numeric offsets happen to match.
enum class NativeStackAddressKind {
  EntryStackPointer,
  RelativeStackPointer,
  NativeFrame,
};

struct NativeStackAddress {
  NativeStackAddressKind Kind = NativeStackAddressKind::EntryStackPointer;
  // EntryStackPointer uses nullptr. RelativeStackPointer uses the value that
  // established the new base, and NativeFrame uses the notdec_stack.native
  // alloca. This makes unrelated aligned frames and allocas incomparable.
  llvm::Value *Base = nullptr;
  int64_t Offset = 0;

  bool operator==(const NativeStackAddress &other) const {
    return Kind == other.Kind && Base == other.Base && Offset == other.Offset;
  }

  bool operator!=(const NativeStackAddress &other) const {
    return !(*this == other);
  }
};

// Return nullopt instead of wrapping a machine-address offset. The supported
// lifted forms use small constants, but callers should not turn malformed IR
// into a matching stack slot.
std::optional<NativeStackAddress>
nativeStackAddressWithOffset(NativeStackAddress address, int64_t offset);

// Find the lifted global that owns the ABI stack pointer. Keeping this lookup
// here avoids each summary pass parsing notdec.register metadata differently.
llvm::GlobalVariable *findNativeStackPointerGlobal(llvm::Module &module,
                                                   llvm::StringRef name);

// Function-local SP state used by the summary chain. It intentionally covers
// only direct lifted SP loads/stores, constant add/sub, alignment masks, and
// notdec_stack.native pointer arithmetic. It is not a general memory or alias
// analysis. Facts are valid only while the function IR is not mutated.
class NativeStackAddressAnalysis {
public:
  NativeStackAddressAnalysis(llvm::Function &function,
                             llvm::GlobalVariable *stackPointer,
                             llvm::StringRef stackPointerName);

  // Current SP immediately before an instruction. A missing result means
  // predecessor paths disagree or the stack pointer was assigned an unknown
  // value.
  std::optional<NativeStackAddress>
  stackPointerBefore(const llvm::Instruction &instruction) const;

  // Classify a memory pointer. NativeFrame is checked before integer-SP
  // arithmetic so rewritten local stack slots never become entry-stack args.
  std::optional<NativeStackAddress>
  addressForPointer(llvm::Value *pointer) const;

  // Classify the integer form used before an inttoptr or when lifted code
  // passes a stack address directly to another function.
  std::optional<NativeStackAddress>
  addressForIntegerValue(llvm::Value *value) const;

private:
  struct StackPointerState {
    bool Reachable = false;
    std::optional<NativeStackAddress> Address;

    bool operator==(const StackPointerState &other) const {
      return Reachable == other.Reachable && Address == other.Address;
    }

    bool operator!=(const StackPointerState &other) const {
      return !(*this == other);
    }
  };

  llvm::Function &Function;
  llvm::GlobalVariable *StackPointer = nullptr;
  llvm::StringRef StackPointerName;
  std::map<const llvm::Instruction *, StackPointerState> Before;
  std::map<const llvm::BasicBlock *, StackPointerState> BlockOut;

  void build();
  StackPointerState blockEntryState(const llvm::BasicBlock &block) const;
  void transfer(const llvm::Instruction &instruction,
                StackPointerState &state) const;
  bool isStackPointerLoad(const llvm::Value &value) const;
  bool isStackPointerStore(const llvm::Instruction &instruction) const;
  bool isSummaryStackPointerValue(const llvm::Value &value) const;

  std::optional<NativeStackAddress> addressForInteger(llvm::Value *value) const;
  std::optional<NativeStackAddress>
  addressForNativeFramePointer(llvm::Value *value) const;
};

} // namespace notdec::bin2llvm
