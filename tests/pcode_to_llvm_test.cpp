#include "notdec-bin2llvm/PcodeToLLVM.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

bool expect(bool condition, const std::string &message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

notdec::bin2llvm::VarnodeView constVarnode(uint64_t value, uint32_t size) {
  notdec::bin2llvm::VarnodeView varnode;
  varnode.Space = "const";
  varnode.Offset = value;
  varnode.Size = size;
  return varnode;
}

notdec::bin2llvm::PcodeOpView returnOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Return;
  op.OpcodeName = "RETURN";
  op.Inputs.push_back(constVarnode(0, 8));
  return op;
}

bool testUnreachablePcodeBlocksAreRemoved() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(returnOp(0x1000));
  program.Ops.push_back(returnOp(0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "unreachable_blocks";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  if (!expect(function != nullptr, "lowered function is missing")) {
    return false;
  }

  bool hasEntry = false;
  bool hasReachableBlock = false;
  bool hasUnreachableBlock = false;
  for (llvm::BasicBlock &block : *function) {
    hasEntry |= block.getName() == "entry";
    hasReachableBlock |= block.getName() == "bb_1000";
    hasUnreachableBlock |= block.getName() == "bb_2000";
  }

  return expect(hasEntry, "entry block was removed") &&
         expect(hasReachableBlock, "reachable p-code block was removed") &&
         expect(!hasUnreachableBlock, "unreachable p-code block remains") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after p-code lowering");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testUnreachablePcodeBlocksAreRemoved();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
