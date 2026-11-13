#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Metadata.h"

#include <optional>
#include <string>

namespace notdec::bin2llvm {

// Reads `key=value` out of a metadata node made of MDString operands, the shape
// used by the notdec.* metadata (register, ABI, unknown source).  Returns
// std::nullopt when the node or the key is missing.
inline std::optional<std::string> mdField(const llvm::MDNode *node,
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

} // namespace notdec::bin2llvm
