#include "notdec-bin2llvm/NativeAnalysis.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool expectEqual(const std::string &actual, const std::string &expected,
                 const char *label) {
  if (actual == expected) {
    return true;
  }
  std::cerr << label << ": expected " << expected << ", got " << actual
            << '\n';
  return false;
}

bool testInstructionFlowKindStrings() {
  using notdec::bin2llvm::NativeInstructionFlowKind;
  using notdec::bin2llvm::toString;

  bool ok = true;
  ok &= expectEqual(toString(NativeInstructionFlowKind::None), "none",
                    "none flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::ConditionalBranch),
                    "conditional branch", "conditional flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::UnconditionalBranch),
                    "unconditional branch", "unconditional flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::IndirectBranch),
                    "indirect branch", "indirect flow kind");
  ok &= expectEqual(toString(NativeInstructionFlowKind::Return), "return",
                    "return flow kind");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= testInstructionFlowKindStrings();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
