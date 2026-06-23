#include "notdec-bin2llvm/HeritagePcode.h"
#include "notdec-bin2llvm/HeritageToLLVM.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

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

notdec::bin2llvm::HeritageVarnode varnode(const std::string &id,
                                          const std::string &space,
                                          uint64_t offset, uint32_t size,
                                          bool isConstant = false) {
  notdec::bin2llvm::HeritageVarnode value;
  value.Id = id;
  value.Space = space;
  value.Address = space + ":" + id;
  value.Offset = offset;
  value.Size = size;
  value.IsConstant = isConstant;
  return value;
}

notdec::bin2llvm::HeritageOp op(const std::string &id,
                                const std::string &mnemonic,
                                const std::optional<std::string> &output,
                                std::vector<std::string> inputs) {
  notdec::bin2llvm::HeritageOp value;
  value.Id = id;
  value.Parent = "bb:0";
  value.SeqTarget = "ram:1000";
  value.Mnemonic = mnemonic;
  value.Output = output;
  value.Inputs = std::move(inputs);
  return value;
}

bool testForwardUniqueDefIsMaterialized() {
  notdec::bin2llvm::HeritageProgram program;
  program.Schema = "notdec.heritage-pcode.v0";
  program.Program.Language = "x86/little/64/default";
  program.Function.Name = "forward_unique_def";
  program.Function.Entry = "ram:1000";
  program.Function.ReturnType = "undefined8";

  program.Varnodes.push_back(varnode("c1", "const", 7, 16, true));
  program.Varnodes.push_back(varnode("c2", "const", 11, 16, true));
  program.Varnodes.push_back(varnode("offset", "const", 0, 4, true));
  program.Varnodes.push_back(varnode("retaddr", "const", 0, 8, true));
  program.Varnodes.push_back(varnode("wide", "unique", 0x100, 16));
  program.Varnodes.push_back(varnode("low", "unique", 0x110, 8));

  program.Ops.push_back(op("subpiece", "SUBPIECE", "low", {"wide", "offset"}));
  program.Ops.push_back(op("add", "INT_ADD", "wide", {"c1", "c2"}));
  program.Ops.push_back(op("ret", "RETURN", std::nullopt, {"retaddr", "low"}));

  notdec::bin2llvm::HeritageBlock block;
  block.Id = "bb:0";
  block.Start = "ram:1000";
  block.Ops = {"subpiece", "add", "ret"};
  program.Blocks.push_back(std::move(block));
  notdec::bin2llvm::indexHeritageProgram(program);

  llvm::LLVMContext context;
  notdec::bin2llvm::HeritageLoweringConfig config;
  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildHeritageModule(context, program, config,
                                            errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }

  bool hasFreeze = false;
  if (llvm::Function *function = module->getFunction("forward_unique_def")) {
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      hasFreeze |= llvm::isa<llvm::FreezeInst>(&inst);
    }
  }

  return expect(!hasFreeze, "forward unique def used poison fallback") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after forward unique def lowering");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testForwardUniqueDefIsMaterialized();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
