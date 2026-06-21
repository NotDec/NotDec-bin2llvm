#include "notdec-bin2llvm/PcodeToLLVM.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

notdec::bin2llvm::VarnodeView uniqueVarnode(uint64_t offset, uint32_t size) {
  notdec::bin2llvm::VarnodeView varnode;
  varnode.Space = "unique";
  varnode.Offset = offset;
  varnode.Size = size;
  return varnode;
}

notdec::bin2llvm::PcodeOpView copyOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = uniqueVarnode(0x100, 8);
  op.Inputs.push_back(constVarnode(0, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView returnOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Return;
  op.OpcodeName = "RETURN";
  op.Inputs.push_back(constVarnode(0, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView branchOp(uint64_t address, uint64_t target) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Branch;
  op.OpcodeName = "BRANCH";
  notdec::bin2llvm::VarnodeView targetVarnode;
  targetVarnode.Space = "ram";
  targetVarnode.Offset = target;
  targetVarnode.Size = 8;
  op.Inputs.push_back(std::move(targetVarnode));
  return op;
}

notdec::bin2llvm::PcodeOpView cbranchOp(uint64_t address, uint64_t target) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::CBranch;
  op.OpcodeName = "CBRANCH";
  notdec::bin2llvm::VarnodeView targetVarnode;
  targetVarnode.Space = "ram";
  targetVarnode.Offset = target;
  targetVarnode.Size = 8;
  op.Inputs.push_back(std::move(targetVarnode));
  op.Inputs.push_back(constVarnode(1, 1));
  return op;
}

notdec::bin2llvm::PcodeOpView branchIndOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::BranchInd;
  op.OpcodeName = "BRANCHIND";
  op.Inputs.push_back(uniqueVarnode(0x200, 8));
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

bool testExternalTailBranchWithoutLocalBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchOp(0x1000, 0x5450));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "external_tail_branch";
  config.ExternalCallTargets.emplace(0x5450, "free");

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  llvm::Function *freeDecl = module->getFunction("free");
  if (!expect(function != nullptr, "tail branch function is missing") ||
      !expect(freeDecl != nullptr, "external tail callee is missing")) {
    return false;
  }

  bool hasTailCall = false;
  bool hasTargetBlock = false;
  for (llvm::BasicBlock &block : *function) {
    hasTargetBlock |= block.getName() == "bb_5450";
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasTailCall |= call != nullptr && call->getCalledFunction() == freeDecl &&
                     call->isTailCall();
    }
  }

  return expect(hasTailCall, "external tail branch did not emit tail call") &&
         expect(!hasTargetBlock, "external tail branch created target block") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after external tail branch lowering");
}

bool testInternalTailBranchWithoutLocalBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "internal_tail_branch";
  config.DirectCallTargets.emplace(0x2000, "notdec_native_2000");
  config.BlockRanges.emplace(0x1000, 0x1001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  llvm::Function *callee = module->getFunction("notdec_native_2000");
  if (!expect(function != nullptr, "tail branch function is missing") ||
      !expect(callee != nullptr, "internal tail callee is missing")) {
    return false;
  }

  bool hasTailCall = false;
  bool hasTargetBlock = false;
  for (llvm::BasicBlock &block : *function) {
    hasTargetBlock |= block.getName() == "bb_2000";
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasTailCall |= call != nullptr && call->getCalledFunction() == callee &&
                     call->isTailCall();
    }
  }

  return expect(hasTailCall, "internal tail branch did not emit tail call") &&
         expect(!hasTargetBlock, "internal tail branch created target block") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after internal tail branch lowering");
}

bool testInternalConditionalTailBranchWithoutLocalBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "internal_conditional_tail_branch";
  config.EntryAddress = 0x1000;
  config.DirectCallTargets.emplace(0x2000, "notdec_native_2000");
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  llvm::Function *callee = module->getFunction("notdec_native_2000");
  if (!expect(function != nullptr,
              "conditional tail branch function is missing") ||
      !expect(callee != nullptr, "conditional tail callee is missing")) {
    return false;
  }

  bool hasTailCall = false;
  bool hasTargetBlock = false;
  for (llvm::BasicBlock &block : *function) {
    hasTargetBlock |= block.getName() == "bb_2000";
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasTailCall |= call != nullptr && call->getCalledFunction() == callee &&
                     call->isTailCall();
    }
  }

  return expect(hasTailCall,
                "internal conditional tail branch did not emit tail call") &&
         expect(!hasTargetBlock,
                "internal conditional tail branch created target block") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after conditional tail branch lowering");
}

bool testNativeBlockRangeIsRequired() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(returnOp(0x1000));
  program.Ops.push_back(returnOp(0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "missing_native_block_range";
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2000});
  config.BlockRanges.emplace(0x1000, 0x1001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr, "module should fail without full block ranges") &&
         expect(errorMessage.find("outside native block ranges") !=
                    std::string::npos,
                "missing block range error was not reported");
}

bool testNativeEntryAddressChoosesEntryBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(returnOp(0x2000));
  program.Ops.push_back(returnOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_entry_address";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockRanges.emplace(0x1000, 0x1001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  if (!expect(function != nullptr, "native entry function is missing")) {
    return false;
  }

  llvm::BasicBlock &entry = function->getEntryBlock();
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(entry.getTerminator());
  if (!expect(branch != nullptr && branch->isUnconditional(),
              "lowered entry block is not an unconditional branch")) {
    return false;
  }

  return expect(branch->getSuccessor(0)->getName() == "bb_1000",
                "native entry address did not select the entry block") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after native entry selection");
}

bool testNativeDirectBranchOutsideRangesBecomesTailCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_outside_direct_branch";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  llvm::Function *callee = module->getFunction("notdec_native_2000");
  if (!expect(function != nullptr, "outside branch function is missing") ||
      !expect(callee != nullptr, "outside branch callee is missing")) {
    return false;
  }

  bool hasTailCall = false;
  bool hasTargetBlock = false;
  for (llvm::BasicBlock &block : *function) {
    hasTargetBlock |= block.getName() == "bb_2000";
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasTailCall |= call != nullptr && call->getCalledFunction() == callee &&
                     call->isTailCall();
    }
  }

  return expect(hasTailCall, "outside direct branch did not emit tail call") &&
         expect(!hasTargetBlock, "outside direct branch created target block") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after outside direct branch lowering");
}

bool testNativeDirectBranchCanTargetEmptyBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_empty_target";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockSuccessors.emplace(0x2000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  if (!expect(function != nullptr, "native empty target function is missing")) {
    return false;
  }

  bool hasEmptyTarget = false;
  for (llvm::BasicBlock &block : *function) {
    hasEmptyTarget |= block.getName() == "bb_2000";
  }

  return expect(hasEmptyTarget, "native empty target block was not emitted") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after native empty target lowering");
}

bool testNativeEntryAddressCanTargetEmptyBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(returnOp(0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_empty_entry";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  if (!expect(function != nullptr, "native empty entry function is missing")) {
    return false;
  }

  llvm::BasicBlock &entry = function->getEntryBlock();
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(entry.getTerminator());
  if (!expect(branch != nullptr && branch->isUnconditional(),
              "native empty entry did not lower to an entry branch")) {
    return false;
  }

  return expect(branch->getSuccessor(0)->getName() == "bb_1000",
                "native empty entry address did not select the empty block") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after native empty entry lowering");
}

bool testNativeSuccessorRequiresKnownBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(copyOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_missing_successor";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native successor to missing block should fail") &&
         expect(errorMessage.find("missing a native block") !=
                    std::string::npos,
                "missing native successor error was not reported");
}

bool testNativeMultipleSuccessorsRequireTerminator() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(copyOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_multiple_successors_without_terminator";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockRanges.emplace(0x3000, 0x3001);
  config.BlockSuccessors.emplace(0x1000,
                                 std::vector<uint64_t>{0x2000, 0x3000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native block with multiple successors and no terminator "
                "should fail") &&
         expect(errorMessage.find("successors but no p-code terminator") !=
                    std::string::npos,
                "missing native terminator error was not reported");
}

bool testNativeFallthroughRequiresSuccessorFacts() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(copyOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_fallthrough_missing_successors";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native fallthrough without successor facts should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing fallthrough successor facts error was not reported");
}

bool testEmptyNativeBlockRequiresSuccessorFacts() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_empty_missing_successors";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "empty native block without successor facts should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing empty block successor facts error was not reported");
}

bool testNativeConditionalSuccessorsRequireTrueTarget() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_conditional_missing_true";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockRanges.emplace(0x3000, 0x3001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x3000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native conditional without true successor should fail") &&
         expect(errorMessage.find("missing true successor") !=
                    std::string::npos,
                "missing true successor error was not reported");
}

bool testNativeConditionalRequiresSuccessorFacts() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_conditional_missing_successors";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native conditional without successor facts should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing successor facts error was not reported");
}

bool testNativeConditionalOutsideTrueTargetAllowsFalseOnlySuccessor() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_conditional_outside_true";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x3000, 0x3001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x3000});
  config.BlockSuccessors.emplace(0x3000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  llvm::Function *callee = module->getFunction("notdec_native_2000");
  if (!expect(function != nullptr,
              "outside true conditional function is missing") ||
      !expect(callee != nullptr, "outside true conditional callee is missing")) {
    return false;
  }

  bool hasTailCall = false;
  bool hasFalseTarget = false;
  for (llvm::BasicBlock &block : *function) {
    hasFalseTarget |= block.getName() == "bb_3000";
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasTailCall |= call != nullptr && call->getCalledFunction() == callee &&
                     call->isTailCall();
    }
  }

  return expect(hasTailCall,
                "outside true conditional did not emit tail call") &&
         expect(hasFalseTarget,
                "outside true conditional did not keep false successor") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after outside true conditional");
}

bool testNativeConditionalSuccessorsRejectMultipleFalseTargets() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_conditional_multiple_false";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockRanges.emplace(0x3000, 0x3001);
  config.BlockRanges.emplace(0x4000, 0x4001);
  config.BlockSuccessors.emplace(
      0x1000, std::vector<uint64_t>{0x2000, 0x3000, 0x4000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native conditional with multiple false successors should "
                "fail") &&
         expect(errorMessage.find("multiple false successors") !=
                    std::string::npos,
                "multiple false successor error was not reported");
}

bool testNativeIndirectBranchCanUseSingleSuccessor() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchIndOp(0x1000));
  program.Ops.push_back(returnOp(0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_indirect_single_successor";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  if (!expect(function != nullptr,
              "native indirect branch function is missing")) {
    return false;
  }

  llvm::BasicBlock *sourceBlock = nullptr;
  for (llvm::BasicBlock &block : *function) {
    if (block.getName() == "bb_1000") {
      sourceBlock = &block;
      break;
    }
  }
  if (!expect(sourceBlock != nullptr,
              "native indirect branch source block is missing")) {
    return false;
  }
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(sourceBlock->getTerminator());
  return expect(branch != nullptr && branch->isUnconditional(),
                "native indirect branch did not lower to a direct branch") &&
         expect(branch->getSuccessor(0)->getName() == "bb_2000",
                "native indirect branch ignored successor facts") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after indirect successor lowering");
}

bool testNativeIndirectBranchRequiresSuccessorFacts() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchIndOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_indirect_missing_successors";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native indirect branch without successor facts should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing indirect successor facts error was not reported");
}

bool testNativeIndirectBranchRejectsMultipleSuccessors() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchIndOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_indirect_multiple_successors";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockRanges.emplace(0x3000, 0x3001);
  config.BlockSuccessors.emplace(0x1000,
                                 std::vector<uint64_t>{0x2000, 0x3000});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native indirect branch with multiple successors should fail") &&
         expect(errorMessage.find("multiple indirect targets") !=
                    std::string::npos,
                "multiple indirect successor error was not reported");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testUnreachablePcodeBlocksAreRemoved();
  ok &= testExternalTailBranchWithoutLocalBlock();
  ok &= testInternalTailBranchWithoutLocalBlock();
  ok &= testInternalConditionalTailBranchWithoutLocalBlock();
  ok &= testNativeBlockRangeIsRequired();
  ok &= testNativeEntryAddressChoosesEntryBlock();
  ok &= testNativeDirectBranchOutsideRangesBecomesTailCall();
  ok &= testNativeDirectBranchCanTargetEmptyBlock();
  ok &= testNativeEntryAddressCanTargetEmptyBlock();
  ok &= testNativeSuccessorRequiresKnownBlock();
  ok &= testNativeMultipleSuccessorsRequireTerminator();
  ok &= testNativeFallthroughRequiresSuccessorFacts();
  ok &= testEmptyNativeBlockRequiresSuccessorFacts();
  ok &= testNativeConditionalSuccessorsRequireTrueTarget();
  ok &= testNativeConditionalRequiresSuccessorFacts();
  ok &= testNativeConditionalOutsideTrueTargetAllowsFalseOnlySuccessor();
  ok &= testNativeConditionalSuccessorsRejectMultipleFalseTargets();
  ok &= testNativeIndirectBranchCanUseSingleSuccessor();
  ok &= testNativeIndirectBranchRequiresSuccessorFacts();
  ok &= testNativeIndirectBranchRejectsMultipleSuccessors();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
