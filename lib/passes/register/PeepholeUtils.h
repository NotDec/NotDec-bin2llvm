#pragma once

#include "llvm/IR/Constants.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <optional>

namespace notdec::bin2llvm {

// Reads a constant shift amount; anything wider than 64 bits is not a shift
// amount this pass can reason about.
inline std::optional<uint64_t> constantShiftAmount(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getValue().getActiveBits() > 64) {
    return std::nullopt;
  }
  return constant->getZExtValue();
}

} // namespace notdec::bin2llvm
