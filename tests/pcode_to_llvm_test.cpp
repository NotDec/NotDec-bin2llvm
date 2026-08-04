#include "notdec-bin2llvm/PcodeToLLVM.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Intrinsics.h"
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

notdec::bin2llvm::VarnodeView ramVarnode(uint64_t offset, uint32_t size) {
  notdec::bin2llvm::VarnodeView varnode;
  varnode.Space = "ram";
  varnode.Offset = offset;
  varnode.Size = size;
  return varnode;
}

notdec::bin2llvm::VarnodeView registerVarnode(uint64_t offset, uint32_t size,
                                              std::string name) {
  notdec::bin2llvm::VarnodeView varnode;
  varnode.Space = "register";
  varnode.Offset = offset;
  varnode.Size = size;
  varnode.IsRegister = true;
  varnode.RegisterName = std::move(name);
  return varnode;
}

void addX64Registers(notdec::bin2llvm::PcodeProgram &program) {
  program.Registers.push_back({"register", 0x20, 8, "RSP"});
  program.Registers.push_back({"register", 0x288, 8, "RIP"});
}

notdec::bin2llvm::PcodeOpView rspSub8Op(uint64_t address,
                                         uint64_t instructionSize) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::IntSub;
  op.OpcodeName = "INT_SUB";
  op.Output = registerVarnode(0x20, 8, "RSP");
  op.Inputs.push_back(registerVarnode(0x20, 8, "RSP"));
  op.Inputs.push_back(constVarnode(8, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView rspAdd8Op(uint64_t address,
                                         uint64_t instructionSize) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::IntAdd;
  op.OpcodeName = "INT_ADD";
  op.Output = registerVarnode(0x20, 8, "RSP");
  op.Inputs.push_back(registerVarnode(0x20, 8, "RSP"));
  op.Inputs.push_back(constVarnode(8, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView storeReturnAddressOp(uint64_t address,
                                                   uint64_t instructionSize) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Store;
  op.OpcodeName = "STORE";
  op.Inputs.push_back(constVarnode(0, 8));
  op.Inputs.push_back(registerVarnode(0x20, 8, "RSP"));
  op.Inputs.push_back(constVarnode(address + instructionSize, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView callOp(uint64_t address, uint64_t target,
                                     uint64_t instructionSize) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Call;
  op.OpcodeName = "CALL";
  op.Inputs.push_back(ramVarnode(target, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView loadReturnTargetOp(uint64_t address,
                                                 uint64_t instructionSize) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Load;
  op.OpcodeName = "LOAD";
  op.Output = registerVarnode(0x288, 8, "RIP");
  op.Inputs.push_back(constVarnode(0, 8));
  op.Inputs.push_back(registerVarnode(0x20, 8, "RSP"));
  return op;
}

notdec::bin2llvm::PcodeOpView x64ReturnOp(uint64_t address,
                                           uint64_t instructionSize) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Return;
  op.OpcodeName = "RETURN";
  op.Inputs.push_back(registerVarnode(0x288, 8, "RIP"));
  return op;
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

notdec::bin2llvm::PcodeOpView copyFromUnknownUniqueOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = uniqueVarnode(0x300, 8);
  op.Inputs.push_back(uniqueVarnode(0x200, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView copyUniqueToUniqueOp(uint64_t address,
                                                   uint64_t outputOffset,
                                                   uint64_t inputOffset) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = uniqueVarnode(outputOffset, 8);
  op.Inputs.push_back(uniqueVarnode(inputOffset, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView copyToPartialRegisterOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = registerVarnode(0x0, 4, "EAX");
  op.Inputs.push_back(constVarnode(0x12345678, 4));
  return op;
}

notdec::bin2llvm::PcodeOpView copyFromPartialRegisterOp(uint64_t address) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = uniqueVarnode(0x500, 4);
  op.Inputs.push_back(registerVarnode(0x0, 4, "EAX"));
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
  op.Inputs.push_back(ramVarnode(target, 8));
  return op;
}

notdec::bin2llvm::PcodeOpView relativeBranchOp(uint64_t address,
                                               int8_t offset) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Branch;
  op.OpcodeName = "BRANCH";
  op.Inputs.push_back(constVarnode(static_cast<uint8_t>(offset), 1));
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

notdec::bin2llvm::PcodeOpView relativeCbranchOp(uint64_t address,
                                                int8_t offset) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::CBranch;
  op.OpcodeName = "CBRANCH";
  op.Inputs.push_back(constVarnode(static_cast<uint8_t>(offset), 1));
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
  program.Ops.push_back(returnOp(0x3000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "internal_conditional_tail_branch";
  config.EntryAddress = 0x1000;
  config.DirectCallTargets.emplace(0x2000, "notdec_native_2000");
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
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2000});
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

bool testNativeDirectBranchRequiresSuccessorFact() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchOp(0x1000, 0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_direct_missing_successor";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockSuccessors.emplace(0x2000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native direct branch without successor fact should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing direct branch successor facts error was not reported");
}

bool testNativeRelativeBranchRequiresSuccessorFact() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(relativeBranchOp(0x1000, 1));
  program.Ops.push_back(returnOp(0x2000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_relative_missing_successor";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2001);
  config.BlockSuccessors.emplace(0x2000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native relative branch without successor fact should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing relative branch successor facts error was not reported");
}

bool testNativeRelativeBranchRequiresNativeTargetBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(relativeBranchOp(0x1000, 1));
  program.Ops.push_back(returnOp(0x2001));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_relative_missing_target_block";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2002);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2001});
  config.BlockSuccessors.emplace(0x2000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native relative branch to missing target block should fail") &&
         expect(errorMessage.find("missing a native block") !=
                    std::string::npos,
                "missing relative target block error was not reported");
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

bool testAllEmptyNativeBlockCanLower() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_all_empty_block";
  config.EntryAddress = 0x1000;
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
  if (!expect(function != nullptr, "all-empty native function is missing")) {
    return false;
  }

  llvm::BasicBlock *nativeBlock = nullptr;
  for (llvm::BasicBlock &block : *function) {
    if (block.getName() == "bb_1000") {
      nativeBlock = &block;
      break;
    }
  }
  if (!expect(nativeBlock != nullptr, "all-empty native block was not emitted")) {
    return false;
  }

  return expect(llvm::isa<llvm::ReturnInst>(nativeBlock->getTerminator()),
                "all-empty native block did not lower to ret") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after all-empty native block lowering");
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
         expect(errorMessage.find("missing successor 0x2000") !=
                    std::string::npos,
                "missing conditional true successor error was not reported");
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

bool testNativeRelativeConditionalRequiresNativeTargetBlock() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(relativeCbranchOp(0x1000, 1));
  program.Ops.push_back(returnOp(0x2001));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_relative_conditional_missing_target_block";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x2000, 0x2002);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x2001});
  config.BlockSuccessors.emplace(0x2000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native relative conditional to missing target block should "
                "fail") &&
         expect(errorMessage.find("missing a native block") !=
                    std::string::npos,
                "missing relative conditional target block error was not "
                "reported");
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

bool testNativeInternalConditionalKeepsSkippedPcode() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x1001));
  program.Ops.push_back(copyFromUnknownUniqueOp(0x1000));
  program.Ops.push_back(returnOp(0x1001));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_internal_conditional";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x1001, 0x1002);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{0x1001});
  config.BlockSuccessors.emplace(0x1001, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  if (!expect(module != nullptr, errorMessage)) {
    return false;
  }
  llvm::Function *function = module->getFunction(config.EntryFunctionName);
  if (!expect(function != nullptr,
              "internal conditional function is missing")) {
    return false;
  }

  bool hasInternalBlock = false;
  bool internalBlockHasCopyWork = false;
  bool entryBranchesToNativeSuccessor = false;
  bool entryBranchesToInternalBlock = false;
  for (llvm::BasicBlock &block : *function) {
    bool isInternalBlock =
        block.getName().starts_with("bb_1000") && block.getName() != "bb_1000";
    hasInternalBlock |= isInternalBlock;
    if (isInternalBlock) {
      for (llvm::Instruction &inst : block) {
        internalBlockHasCopyWork |= !inst.isTerminator();
      }
    }

    if (block.getName() == "bb_1000") {
      auto *branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
      if (branch != nullptr && branch->isConditional()) {
        for (unsigned index = 0; index < branch->getNumSuccessors(); ++index) {
          llvm::StringRef name = branch->getSuccessor(index)->getName();
          entryBranchesToNativeSuccessor |= name == "bb_1001";
          entryBranchesToInternalBlock |=
              name.starts_with("bb_1000") && name != "bb_1000";
        }
      }
    }
  }

  return expect(hasInternalBlock,
                "native internal conditional did not create an internal block") &&
         expect(internalBlockHasCopyWork,
                "native internal conditional dropped skipped p-code") &&
         expect(entryBranchesToNativeSuccessor,
                "native internal conditional lost true native successor") &&
         expect(entryBranchesToInternalBlock,
                "native internal conditional lost false internal continuation") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after internal conditional lowering");
}

bool testNativeInternalConditionalJoinUsesPhiForPartialUniqueDef() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(relativeCbranchOp(0x1000, 2));
  program.Ops.push_back(copyFromUnknownUniqueOp(0x1000));
  program.Ops.push_back(copyUniqueToUniqueOp(0x1000, 0x400, 0x300));
  program.Ops.push_back(returnOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_internal_conditional_unique_join";
  config.EntryAddress = 0x1000;
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
  if (!expect(function != nullptr,
              "internal conditional unique join function is missing")) {
    return false;
  }

  bool hasPhi = false;
  for (llvm::BasicBlock &block : *function) {
    for (llvm::Instruction &inst : block) {
      hasPhi |= llvm::isa<llvm::PHINode>(&inst);
    }
  }

  return expect(hasPhi,
                "partial unique definition did not create a join PHI") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after partial unique join lowering");
}

bool testNativeInternalConditionalRequiresTrueSuccessorFact() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(cbranchOp(0x1000, 0x1001));
  program.Ops.push_back(copyFromUnknownUniqueOp(0x1000));
  program.Ops.push_back(returnOp(0x1001));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_internal_conditional_missing_true";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockRanges.emplace(0x1001, 0x1002);
  config.BlockSuccessors.emplace(0x1001, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  return expect(module == nullptr,
                "native internal conditional without true successor fact "
                "should fail") &&
         expect(errorMessage.find("missing successor facts") !=
                    std::string::npos,
                "missing internal conditional true successor facts error was "
                "not reported");
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

bool testNativeIndirectBranchWithNoSuccessorsIsUnknownTailCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Ops.push_back(branchIndOp(0x1000));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "native_indirect_no_successors";
  config.EntryAddress = 0x1000;
  config.BlockRanges.emplace(0x1000, 0x1001);
  config.BlockSuccessors.emplace(0x1000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;
  bool hasTailCall = false;
  bool hasReturn = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          hasTailCall |= call->isTailCall();
        }
        hasReturn |= llvm::isa<llvm::ReturnInst>(&instruction);
      }
    }
  }

  return expect(module != nullptr,
                "native indirect branch with empty successors should lower") &&
         expect(function != nullptr,
                "empty indirect successor function is missing") &&
         expect(hasTailCall,
                "empty indirect successor did not lower as tail call") &&
         expect(hasReturn,
                "empty indirect successor did not return after tail call") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after empty indirect successor");
}

bool testNativeIndirectBranchLowersMultipleSuccessorsAsSwitch() {
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
  config.BlockSuccessors.emplace(0x2000, std::vector<uint64_t>{});
  config.BlockSuccessors.emplace(0x3000, std::vector<uint64_t>{});

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function = module ? module->getFunction(
                                          "native_indirect_multiple_successors")
                                    : nullptr;
  llvm::SwitchInst *switchInst = nullptr;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      if (auto *candidate =
              llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator())) {
        switchInst = candidate;
      }
    }
  }
  llvm::BasicBlock *defaultBlock =
      switchInst == nullptr ? nullptr : switchInst->getDefaultDest();
  bool defaultIsTrap = false;
  bool defaultIsUnreachable = false;
  if (defaultBlock != nullptr) {
    defaultIsUnreachable =
        llvm::isa<llvm::UnreachableInst>(defaultBlock->getTerminator());
    for (llvm::Instruction &instruction :
         defaultBlock->instructionsWithoutDebug()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      defaultIsTrap |= call != nullptr && call->getIntrinsicID() ==
                                             llvm::Intrinsic::trap;
    }
  }
  return expect(module != nullptr,
                "native indirect branch with multiple successors should lower") &&
         expect(function != nullptr,
                "multiple indirect successor function is missing") &&
         expect(switchInst != nullptr,
                "multiple indirect successor branch did not lower as switch") &&
         expect(defaultIsTrap,
                "multiple indirect switch default did not lower to trap") &&
         expect(defaultIsUnreachable,
                "multiple indirect switch default did not end unreachable") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after multiple indirect successor");
}

bool functionUsesGlobal(llvm::Function *function, const std::string &name) {
  if (function == nullptr) {
    return false;
  }
  for (llvm::BasicBlock &block : *function) {
    for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
      for (llvm::Value *operand : instruction.operands()) {
        if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(operand)) {
          if (global->getName() == name) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool testX64CallSuppressesReturnAddressStackEffect() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  program.Ops.push_back(rspSub8Op(0x1000, 5));
  program.Ops.push_back(storeReturnAddressOp(0x1000, 5));
  program.Ops.push_back(callOp(0x1000, 0x2000, 5));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x64_call_suppresses_return_address_stack";
  config.DirectCallTargets.emplace(0x2000, "callee");

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;
  llvm::Function *callee = module ? module->getFunction("callee") : nullptr;

  bool hasCall = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        hasCall |= call != nullptr && call->getCalledFunction() == callee;
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x64 call function is missing") &&
         expect(hasCall, "x64 call did not lower to callee call") &&
         expect(!functionUsesGlobal(function, "RSP"),
                "x64 call kept return-address RSP update") &&
         expect(!functionUsesGlobal(function, "notdec_ram"),
                "x64 call kept return-address stack store") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x64 call stack suppression");
}

bool testX64ReturnSuppressesReturnAddressStackEffect() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  program.Ops.push_back(loadReturnTargetOp(0x1000, 1));
  program.Ops.push_back(rspAdd8Op(0x1000, 1));
  program.Ops.push_back(x64ReturnOp(0x1000, 1));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x64_return_suppresses_return_address_stack";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x64 return function is missing") &&
         expect(!functionUsesGlobal(function, "RSP"),
                "x64 return kept return-address RSP update") &&
         expect(!functionUsesGlobal(function, "RIP"),
                "x64 return kept return-address target load") &&
         expect(!functionUsesGlobal(function, "notdec_ram"),
                "x64 return kept return-address stack load") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x64 return stack suppression");
}

bool testNonX64DoesNotSuppressCallStackEffect() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Registers.push_back({"register", 0x20, 8, "RSP"});
  program.Ops.push_back(rspSub8Op(0x1000, 5));
  program.Ops.push_back(storeReturnAddressOp(0x1000, 5));
  program.Ops.push_back(callOp(0x1000, 0x2000, 5));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "non_x64_keeps_call_stack_effect";
  config.DirectCallTargets.emplace(0x2000, "callee");

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "non-x64 call function is missing") &&
         expect(functionUsesGlobal(function, "RSP"),
                "non-x64 call incorrectly removed RSP update") &&
         expect(functionUsesGlobal(function, "notdec_ram"),
                "non-x64 call incorrectly removed stack store") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after non-x64 call lowering");
}

bool testX86PcThunkCallFoldsToConstantBase() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Registers.push_back({"register", 0x20, 4, "ESP"});
  program.Registers.push_back({"register", 0x28, 4, "EIP"});
  program.Registers.push_back({"register", 0x30, 4, "EBX"});

  auto op = [&](uint64_t address, uint64_t instructionSize,
                notdec::bin2llvm::PcodeOpcode opcode,
                std::optional<notdec::bin2llvm::VarnodeView> output,
                std::vector<notdec::bin2llvm::VarnodeView> inputs) {
    notdec::bin2llvm::PcodeOpView view;
    view.Address = address;
    view.InstructionSize = instructionSize;
    view.Opcode = opcode;
    view.OpcodeName = notdec::bin2llvm::pcodeOpcodeName(opcode);
    view.Output = std::move(output);
    view.Inputs = std::move(inputs);
    return view;
  };

  // call 0x1740 (get_pc_thunk.bx) at 0x1000: return address 0x1005 is pushed,
  // then `add $0x547f, %ebx` folds to EBX = 0x1005 + 0x547f = 0x6584.
  program.Ops.push_back(op(0x1000, 5, notdec::bin2llvm::PcodeOpcode::IntSub,
                           registerVarnode(0x20, 4, "ESP"),
                           {registerVarnode(0x20, 4, "ESP"),
                            constVarnode(4, 4)}));
  program.Ops.push_back(op(0x1000, 5, notdec::bin2llvm::PcodeOpcode::Store,
                           std::nullopt,
                           {constVarnode(0, 4),
                            registerVarnode(0x20, 4, "ESP"),
                            constVarnode(0x1005, 4)}));
  program.Ops.push_back(op(0x1000, 5, notdec::bin2llvm::PcodeOpcode::Call,
                           std::nullopt, {ramVarnode(0x1740, 4)}));
  program.Ops.push_back(op(0x1005, 3, notdec::bin2llvm::PcodeOpcode::IntAdd,
                           registerVarnode(0x30, 4, "EBX"),
                           {registerVarnode(0x30, 4, "EBX"),
                            constVarnode(0x547f, 4)}));
  program.Ops.push_back(op(0x1008, 1, notdec::bin2llvm::PcodeOpcode::Return,
                           std::nullopt, {registerVarnode(0x28, 4, "EIP")}));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x86_pc_thunk_folds_to_constant";
  config.ThunkCallTargets.emplace(0x1740, "EBX");

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasCall = false;
  llvm::ConstantInt *ebxStore = nullptr;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        hasCall |= llvm::isa<llvm::CallBase>(&instruction);
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (store == nullptr) {
          continue;
        }
        llvm::GlobalVariable *global =
            llvm::dyn_cast<llvm::GlobalVariable>(store->getPointerOperand());
        if (global != nullptr && global->getName() == "EBX") {
          ebxStore = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
        }
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "thunk fold function is missing") &&
         expect(!hasCall, "thunk call was not folded away") &&
         expect(ebxStore != nullptr &&
                    ebxStore->getZExtValue() == 0x1005 + 0x547f,
                "thunk base register was not folded to fallthrough + imm") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after thunk fold");
}

bool testPartialRegisterWriteUsesPartialWriteHelper() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Registers.push_back({"register", 0x0, 8, "RAX"});
  program.Registers.push_back({"register", 0x0, 4, "EAX"});
  program.Ops.push_back(copyToPartialRegisterOp(0x1000));
  program.Ops.push_back(returnOp(0x1001));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "partial_register_write_helper";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasPartialWrite = false;
  bool hasRaxLoad = false;
  bool hasRaxStore = false;
  llvm::GlobalVariable *rax =
      module ? module->getGlobalVariable("RAX") : nullptr;
  if (function != nullptr) {
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        llvm::Function *callee = call->getCalledFunction();
        hasPartialWrite |=
            callee != nullptr &&
            callee->getName() == "notdec.partial_write.i64.i32";
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        hasRaxLoad |= load->getPointerOperand()->stripPointerCasts() == rax;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        hasRaxStore |= store->getPointerOperand()->stripPointerCasts() == rax;
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "partial register function is missing") &&
         expect(hasPartialWrite, "partial register write did not use helper") &&
         expect(!hasRaxLoad, "partial register write kept old-value load") &&
         expect(!hasRaxStore, "partial register write used full register store") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after partial register helper lowering");
}

bool testPartialRegisterReadUsesPartialReadHelper() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  program.Registers.push_back({"register", 0x0, 8, "RAX"});
  program.Registers.push_back({"register", 0x0, 4, "EAX"});
  program.Ops.push_back(copyFromPartialRegisterOp(0x1000));
  program.Ops.push_back(returnOp(0x1001));

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "partial_register_read_helper";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasPartialRead = false;
  bool hasRaxLoad = false;
  llvm::GlobalVariable *rax =
      module ? module->getGlobalVariable("RAX") : nullptr;
  if (function != nullptr) {
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        llvm::Function *callee = call->getCalledFunction();
        hasPartialRead |=
            callee != nullptr &&
            callee->getName() == "notdec.partial_read.i64.i32";
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        hasRaxLoad |= load->getPointerOperand()->stripPointerCasts() == rax;
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "partial register read function is missing") &&
         expect(hasPartialRead, "partial register read did not use helper") &&
         expect(!hasRaxLoad, "partial register read kept full register load") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after partial register read lowering");
}


notdec::bin2llvm::PcodeOpView x87StackCopyOp(uint64_t address,
                                             uint64_t instructionSize,
                                             uint64_t dstOffset,
                                             uint64_t srcOffset,
                                             const std::string &dstName,
                                             const std::string &srcName) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = registerVarnode(dstOffset, 10, dstName);
  op.Inputs.push_back(registerVarnode(srcOffset, 10, srcName));
  return op;
}

notdec::bin2llvm::PcodeOpView x87UniqueCopyOp(uint64_t address,
                                              uint64_t instructionSize,
                                              uint64_t uniqueOffset,
                                              uint64_t srcOffset,
                                              const std::string &srcName) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = uniqueVarnode(uniqueOffset, 10);
  op.Inputs.push_back(registerVarnode(srcOffset, 10, srcName));
  return op;
}

notdec::bin2llvm::PcodeOpView x87CopyFromUniqueOp(uint64_t address,
                                                  uint64_t instructionSize,
                                                  uint64_t dstOffset,
                                                  uint64_t uniqueOffset,
                                                  const std::string &dstName) {
  notdec::bin2llvm::PcodeOpView op;
  op.Address = address;
  op.InstructionSize = instructionSize;
  op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  op.OpcodeName = "COPY";
  op.Output = registerVarnode(dstOffset, 10, dstName);
  op.Inputs.push_back(uniqueVarnode(uniqueOffset, 10));
  return op;
}

// fildl (%ram): rolling push prefix, load i32, ST0 = INT2FLOAT(load).
void addX87FildlOps(notdec::bin2llvm::PcodeProgram &program, uint64_t address) {
  program.Ops.push_back(x87UniqueCopyOp(address, 1, 0x27180, 0x1170, "ST7"));
  for (unsigned index = 7; index >= 1; --index) {
    uint64_t dst = 0x1100 + index * 0x10;
    uint64_t src = 0x1100 + (index - 1) * 0x10;
    program.Ops.push_back(x87StackCopyOp(
        address, 1, dst, src, "ST" + std::to_string(index),
        "ST" + std::to_string(index - 1)));
  }
  program.Ops.push_back(
      x87CopyFromUniqueOp(address, 1, 0x1100, 0x27180, "ST0"));

  notdec::bin2llvm::PcodeOpView load;
  load.Address = address;
  load.InstructionSize = 1;
  load.Opcode = notdec::bin2llvm::PcodeOpcode::Load;
  load.OpcodeName = "LOAD";
  load.Output = uniqueVarnode(0x5580, 4);
  load.Inputs.push_back(constVarnode(0, 8));
  load.Inputs.push_back(ramVarnode(0x4000, 4));
  program.Ops.push_back(load);

  notdec::bin2llvm::PcodeOpView cast;
  cast.Address = address;
  cast.InstructionSize = 1;
  cast.Opcode = notdec::bin2llvm::PcodeOpcode::FloatInt2Float;
  cast.OpcodeName = "INT2FLOAT";
  cast.Output = registerVarnode(0x1100, 10, "ST0");
  cast.Inputs.push_back(uniqueVarnode(0x5580, 4));
  program.Ops.push_back(cast);
}

// fstpl (%ram): ST0 -> double, STORE, rolling pop suffix.
void addX87FstpOps(notdec::bin2llvm::PcodeProgram &program, uint64_t address) {
  notdec::bin2llvm::PcodeOpView cast;
  cast.Address = address;
  cast.InstructionSize = 1;
  cast.Opcode = notdec::bin2llvm::PcodeOpcode::FloatFloat2Float;
  cast.OpcodeName = "FLOAT2FLOAT";
  cast.Output = uniqueVarnode(0x5600, 8);
  cast.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  program.Ops.push_back(cast);

  notdec::bin2llvm::PcodeOpView store;
  store.Address = address;
  store.InstructionSize = 1;
  store.Opcode = notdec::bin2llvm::PcodeOpcode::Store;
  store.OpcodeName = "STORE";
  store.Inputs.push_back(constVarnode(0, 8));
  store.Inputs.push_back(ramVarnode(0x5000, 4));
  store.Inputs.push_back(uniqueVarnode(0x5600, 8));
  program.Ops.push_back(store);

  for (unsigned index = 0; index < 7; ++index) {
    uint64_t dst = 0x1100 + index * 0x10;
    uint64_t src = 0x1100 + (index + 1) * 0x10;
    program.Ops.push_back(x87StackCopyOp(
        address, 1, dst, src, "ST" + std::to_string(index),
        "ST" + std::to_string(index + 1)));
  }
}

bool testX87FildlFoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  addX87FildlOps(program, 0x1000);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fildl";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFildl = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        hasFildl |=
            callee != nullptr && callee->getName() == "notdec.x87.fild.i32";
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fildl function is missing") &&
         expect(hasFildl, "x87 fildl did not fold to notdec.x87.fild.i32") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fildl kept an ST0 register global") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fildl folding");
}

bool testX87FstpFoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  addX87FstpOps(program, 0x1000);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fstp";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFstp = false;
  bool hasStore = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call != nullptr) {
          llvm::Function *callee = call->getCalledFunction();
          hasFstp |= callee != nullptr &&
                     callee->getName() == "notdec.x87.fstp.f64";
        }
        hasStore |= llvm::isa<llvm::StoreInst>(&instruction);
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fstp function is missing") &&
         expect(hasFstp, "x87 fstp did not fold to notdec.x87.fstp.f64") &&
         expect(hasStore, "x87 fstp did not keep the result store") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fstp kept an ST0 register global") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fstp folding");
}

// fldz: INT2FLOAT(const) -> unique, rolling push, ST0 = COPY(unique).
// Mnemonic-driven classification must fold it to notdec.x87.fldz instead of
// matching the rolling push as a register operand.
void addX87FldzOps(notdec::bin2llvm::PcodeProgram &program, uint64_t address) {
  notdec::bin2llvm::PcodeOpView convert;
  convert.Address = address;
  convert.InstructionSize = 1;
  convert.Mnemonic = "FLDZ";
  convert.Opcode = notdec::bin2llvm::PcodeOpcode::FloatInt2Float;
  convert.OpcodeName = "INT2FLOAT";
  convert.Output = uniqueVarnode(0x72900, 10);
  convert.Inputs.push_back(constVarnode(0, 4));
  program.Ops.push_back(convert);

  for (unsigned index = 7; index >= 1; --index) {
    notdec::bin2llvm::PcodeOpView op = x87StackCopyOp(
        address, 1, 0x1100 + index * 0x10, 0x1100 + (index - 1) * 0x10,
        "ST" + std::to_string(index), "ST" + std::to_string(index - 1));
    op.Mnemonic = "FLDZ";
    program.Ops.push_back(op);
  }

  notdec::bin2llvm::PcodeOpView write = x87CopyFromUniqueOp(
      address, 1, 0x1100, 0x72900, "ST0");
  write.Mnemonic = "FLDZ";
  program.Ops.push_back(write);
}

bool testX87FldzFoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  addX87FldzOps(program, 0x1000);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fldz";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFldz = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        hasFldz |= callee != nullptr &&
                   callee->getName() == "notdec.x87.fldz";
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fldz function is missing") &&
         expect(hasFldz, "x87 fldz did not fold to notdec.x87.fldz") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fldz kept an ST0 register global") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fldz folding");
}

// fdivrp %st,%st(2): ST2 = FLOAT_DIV(ST2, ST0) + rolling pop.  Ghidra names
// this FDIVP, so the mnemonic dispatch must recover the reverse name from the
// p-code operand order and pass the slot index as the intrinsic argument.
void addX87FdivrpSt2Ops(notdec::bin2llvm::PcodeProgram &program,
                        uint64_t address) {
  notdec::bin2llvm::PcodeOpView divide;
  divide.Address = address;
  divide.InstructionSize = 1;
  divide.Mnemonic = "FDIVP";
  divide.Opcode = notdec::bin2llvm::PcodeOpcode::FloatDiv;
  divide.OpcodeName = "FLOAT_DIV";
  divide.Output = registerVarnode(0x1120, 10, "ST2");
  divide.Inputs.push_back(registerVarnode(0x1120, 10, "ST2"));
  divide.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  program.Ops.push_back(divide);

  for (unsigned index = 0; index < 7; ++index) {
    notdec::bin2llvm::PcodeOpView op = x87StackCopyOp(
        address, 1, 0x1100 + index * 0x10, 0x1100 + (index + 1) * 0x10,
        "ST" + std::to_string(index), "ST" + std::to_string(index + 1));
    op.Mnemonic = "FDIVP";
    program.Ops.push_back(op);
  }
}

bool testX87FdivrpSt2FoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  addX87FdivrpSt2Ops(program, 0x1000);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fdivrp_st2";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFdivrpSt2 = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee != nullptr && callee->getName() == "notdec.x87.fdivrp.sti" &&
            call->arg_size() == 1) {
          if (auto *constant =
                  llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0))) {
            hasFdivrpSt2 |= constant->getZExtValue() == 2;
          }
        }
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fdivrp st(2) function is missing") &&
         expect(hasFdivrpSt2,
                "x87 fdivrp st(2) did not fold to notdec.x87.fdivrp.sti(2)") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fdivrp st(2) kept an ST0 register global") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fdivrp st(2) folding");
}

// fcomi/fucomip: compare ST0 with st(1), write EFLAGS/FPU status from the
// packed intrinsic result.  pop selects the FCOMIP/FUCOMIP rolling pop.
void addX87FcomiOps(notdec::bin2llvm::PcodeProgram &program, uint64_t address,
                    const std::string &mnemonic, bool pop) {
  auto push = [&](notdec::bin2llvm::PcodeOpView op) {
    op.Address = address;
    op.InstructionSize = 1;
    op.Mnemonic = mnemonic;
    program.Ops.push_back(op);
  };

  notdec::bin2llvm::PcodeOpView nan0;
  nan0.Opcode = notdec::bin2llvm::PcodeOpcode::FloatNan;
  nan0.OpcodeName = "FLOAT_NAN";
  nan0.Output = uniqueVarnode(0x2d300, 1);
  nan0.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  push(nan0);

  notdec::bin2llvm::PcodeOpView nan1;
  nan1.Opcode = notdec::bin2llvm::PcodeOpcode::FloatNan;
  nan1.OpcodeName = "FLOAT_NAN";
  nan1.Output = uniqueVarnode(0x2d380, 1);
  nan1.Inputs.push_back(registerVarnode(0x1110, 10, "ST1"));
  push(nan1);

  notdec::bin2llvm::PcodeOpView pf;
  pf.Opcode = notdec::bin2llvm::PcodeOpcode::BoolOr;
  pf.OpcodeName = "BOOL_OR";
  pf.Output = registerVarnode(0x202, 1, "PF");
  pf.Inputs.push_back(*nan0.Output);
  pf.Inputs.push_back(*nan1.Output);
  push(pf);

  notdec::bin2llvm::PcodeOpView equal;
  equal.Opcode = notdec::bin2llvm::PcodeOpcode::FloatEqual;
  equal.OpcodeName = "FLOAT_EQUAL";
  equal.Output = uniqueVarnode(0x2d480, 1);
  equal.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  equal.Inputs.push_back(registerVarnode(0x1110, 10, "ST1"));
  push(equal);

  notdec::bin2llvm::PcodeOpView zf;
  zf.Opcode = notdec::bin2llvm::PcodeOpcode::IntOr;
  zf.OpcodeName = "INT_OR";
  zf.Output = registerVarnode(0x206, 1, "ZF");
  zf.Inputs.push_back(*pf.Output);
  zf.Inputs.push_back(*equal.Output);
  push(zf);

  notdec::bin2llvm::PcodeOpView less;
  less.Opcode = notdec::bin2llvm::PcodeOpcode::FloatLess;
  less.OpcodeName = "FLOAT_LESS";
  less.Output = uniqueVarnode(0x2d580, 1);
  less.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  less.Inputs.push_back(registerVarnode(0x1110, 10, "ST1"));
  push(less);

  notdec::bin2llvm::PcodeOpView cf;
  cf.Opcode = notdec::bin2llvm::PcodeOpcode::IntOr;
  cf.OpcodeName = "INT_OR";
  cf.Output = registerVarnode(0x200, 1, "CF");
  cf.Inputs.push_back(*pf.Output);
  cf.Inputs.push_back(*less.Output);
  push(cf);

  auto clearReg = [&](uint64_t offset, uint32_t size,
                      const std::string &name) {
    notdec::bin2llvm::PcodeOpView op;
    op.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
    op.OpcodeName = "COPY";
    op.Output = registerVarnode(offset, size, name);
    op.Inputs.push_back(constVarnode(0, size));
    push(op);
  };
  clearReg(0x20b, 1, "OF");
  clearReg(0x204, 1, "AF");
  clearReg(0x207, 1, "SF");
  clearReg(0x1091, 1, "C1");

  notdec::bin2llvm::PcodeOpView fsw;
  fsw.Opcode = notdec::bin2llvm::PcodeOpcode::IntAnd;
  fsw.OpcodeName = "INT_AND";
  fsw.Output = registerVarnode(0x10a2, 2, "FSW");
  fsw.Inputs.push_back(registerVarnode(0x10a2, 2, "FSW"));
  fsw.Inputs.push_back(constVarnode(0xfdff, 2));
  push(fsw);

  if (pop) {
    for (unsigned index = 0; index < 7; ++index) {
      notdec::bin2llvm::PcodeOpView op = x87StackCopyOp(
          address, 1, 0x1100 + index * 0x10, 0x1100 + (index + 1) * 0x10,
          "ST" + std::to_string(index), "ST" + std::to_string(index + 1));
      op.Mnemonic = mnemonic;
      program.Ops.push_back(op);
    }
  }
}

// fprem + fnstsw %ax + test $0x4,%ah + jne: the fmod retry loop found in
// libicu/libav/libpython.  Mirrors the real p-code: FPREM is a single-shot
// remainder (FLOAT_DIV/FLOAT_MULT/FLOAT_SUB), FNSTSW copies the status word
// to AX, TEST reads AH, and the branch loops back while C2 is set.
void addX87FpremFnstswOps(notdec::bin2llvm::PcodeProgram &program,
                          uint64_t address) {
  auto push = [&](notdec::bin2llvm::PcodeOpView op, uint64_t addr,
                  const std::string &mnemonic) {
    op.Address = addr;
    op.InstructionSize = 1;
    op.Mnemonic = mnemonic;
    program.Ops.push_back(op);
  };

  notdec::bin2llvm::PcodeOpView div;
  div.Opcode = notdec::bin2llvm::PcodeOpcode::FloatDiv;
  div.OpcodeName = "FLOAT_DIV";
  div.Output = uniqueVarnode(0xeec00, 10);
  div.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  div.Inputs.push_back(registerVarnode(0x1110, 10, "ST1"));
  push(div, address, "FPREM");

  notdec::bin2llvm::PcodeOpView mul;
  mul.Opcode = notdec::bin2llvm::PcodeOpcode::FloatMult;
  mul.OpcodeName = "FLOAT_MULT";
  mul.Output = uniqueVarnode(0xeec00, 10);
  mul.Inputs.push_back(uniqueVarnode(0xeec00, 10));
  mul.Inputs.push_back(registerVarnode(0x1110, 10, "ST1"));
  push(mul, address, "FPREM");

  notdec::bin2llvm::PcodeOpView rem;
  rem.Opcode = notdec::bin2llvm::PcodeOpcode::FloatSub;
  rem.OpcodeName = "FLOAT_SUB";
  rem.Output = registerVarnode(0x1100, 10, "ST0");
  rem.Inputs.push_back(registerVarnode(0x1100, 10, "ST0"));
  rem.Inputs.push_back(uniqueVarnode(0xeec00, 10));
  push(rem, address, "FPREM");

  notdec::bin2llvm::PcodeOpView copy;
  copy.Opcode = notdec::bin2llvm::PcodeOpcode::Copy;
  copy.OpcodeName = "COPY";
  copy.Output = registerVarnode(0x0, 2, "AX");
  copy.Inputs.push_back(registerVarnode(0x10a2, 2, "FSW"));
  push(copy, address + 1, "FNSTSW");

  notdec::bin2llvm::PcodeOpView andOp;
  andOp.Opcode = notdec::bin2llvm::PcodeOpcode::IntAnd;
  andOp.OpcodeName = "INT_AND";
  andOp.Output = uniqueVarnode(0xdf700, 1);
  andOp.Inputs.push_back(registerVarnode(0x1, 1, "AH"));
  andOp.Inputs.push_back(constVarnode(0x4, 1));
  push(andOp, address + 2, "TEST");

  notdec::bin2llvm::PcodeOpView zf;
  zf.Opcode = notdec::bin2llvm::PcodeOpcode::IntEqual;
  zf.OpcodeName = "INT_EQUAL";
  zf.Output = registerVarnode(0x206, 1, "ZF");
  zf.Inputs.push_back(uniqueVarnode(0xdf700, 1));
  zf.Inputs.push_back(constVarnode(0, 1));
  push(zf, address + 2, "TEST");

  notdec::bin2llvm::PcodeOpView neg;
  neg.Opcode = notdec::bin2llvm::PcodeOpcode::BoolNegate;
  neg.OpcodeName = "BOOL_NEGATE";
  neg.Output = uniqueVarnode(0x24f00, 1);
  neg.Inputs.push_back(registerVarnode(0x206, 1, "ZF"));
  push(neg, address + 3, "JNE");

  notdec::bin2llvm::PcodeOpView cbranch;
  cbranch.Opcode = notdec::bin2llvm::PcodeOpcode::CBranch;
  cbranch.OpcodeName = "CBRANCH";
  notdec::bin2llvm::VarnodeView target;
  target.Space = "ram";
  target.Offset = address;
  target.Size = 8;
  cbranch.Inputs.push_back(std::move(target));
  cbranch.Inputs.push_back(uniqueVarnode(0x24f00, 1));
  push(cbranch, address + 3, "JNE");
}

bool testX87FcomiFoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  program.Registers.push_back({"register", 0x200, 1, "CF"});
  program.Registers.push_back({"register", 0x202, 1, "PF"});
  program.Registers.push_back({"register", 0x206, 1, "ZF"});
  addX87FcomiOps(program, 0x1000, "FCOMI", false);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fcomi";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFcomi = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee != nullptr && callee->getName() == "notdec.x87.fcomi.sti" &&
            call->arg_size() == 1) {
          if (auto *constant =
                  llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0))) {
            hasFcomi |= constant->getZExtValue() == 1;
          }
        }
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fcomi function is missing") &&
         expect(hasFcomi, "x87 fcomi did not fold to notdec.x87.fcomi.sti(1)") &&
         expect(functionUsesGlobal(function, "CF"),
                "x87 fcomi did not write the CF flag from the result") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fcomi kept an ST0 register global") &&
         expect(!functionUsesGlobal(function, "FPUStatusWord"),
                "x87 fcomi kept an FPUStatusWord global; the status word is "
                "library-internal") &&
         expect(!functionUsesGlobal(function, "C1"),
                "x87 fcomi kept a C1 global; the status word is "
                "library-internal") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fcomi folding");
}

bool testX87FucomipFoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  addX87FcomiOps(program, 0x1000, "FUCOMIP", true);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fucomip";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFucomip = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        hasFucomip |= callee != nullptr &&
                      callee->getName() == "notdec.x87.fucomip.sti";
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fucomip function is missing") &&
         expect(hasFucomip,
                "x87 fucomip did not fold to notdec.x87.fucomip.sti") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fucomip kept an ST0 register global") &&
         expect(!functionUsesGlobal(function, "FPUStatusWord"),
                "x87 fucomip kept an FPUStatusWord global; the status word is "
                "library-internal") &&
         expect(!functionUsesGlobal(function, "C1"),
                "x87 fucomip kept a C1 global; the status word is "
                "library-internal") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fucomip folding");
}

bool testX87FpremFnstswFoldsToIntrinsicCall() {
  llvm::LLVMContext context;
  notdec::bin2llvm::PcodeProgram program;
  addX64Registers(program);
  program.Registers.push_back({"register", 0x0, 2, "AX"});
  program.Registers.push_back({"register", 0x206, 1, "ZF"});
  addX87FpremFnstswOps(program, 0x1000);

  notdec::bin2llvm::PcodeLoweringConfig config;
  config.EntryFunctionName = "x87_fprem_fnstsw";

  std::string errorMessage;
  std::unique_ptr<llvm::Module> module =
      notdec::bin2llvm::buildPcodeModule(context, program, config,
                                         errorMessage);
  llvm::Function *function =
      module ? module->getFunction(config.EntryFunctionName) : nullptr;

  bool hasFprem = false;
  bool hasFnstsw = false;
  if (function != nullptr) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block.instructionsWithoutDebug()) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr) {
          continue;
        }
        if (callee->getName() == "notdec.x87.fprem") {
          hasFprem = true;
        }
        if (callee->getName() == "notdec.x87.fnstsw") {
          hasFnstsw = true;
        }
      }
    }
  }

  return expect(module != nullptr, errorMessage) &&
         expect(function != nullptr, "x87 fprem/fnstsw function is missing") &&
         expect(hasFprem, "x87 fprem did not fold to notdec.x87.fprem") &&
         expect(hasFnstsw, "x87 fnstsw did not fold to notdec.x87.fnstsw") &&
         expect(functionUsesGlobal(function, "AX"),
                "x87 fnstsw did not write the AX register") &&
         expect(!functionUsesGlobal(function, "FPUStatusWord"),
                "x87 fnstsw read the FPUStatusWord global") &&
         expect(!functionUsesGlobal(function, "ST0"),
                "x87 fprem kept an ST0 register global") &&
         expect(!llvm::verifyModule(*module, &llvm::errs()),
                "module failed verifier after x87 fprem/fnstsw folding");
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
  ok &= testNativeDirectBranchRequiresSuccessorFact();
  ok &= testNativeRelativeBranchRequiresSuccessorFact();
  ok &= testNativeRelativeBranchRequiresNativeTargetBlock();
  ok &= testNativeEntryAddressCanTargetEmptyBlock();
  ok &= testAllEmptyNativeBlockCanLower();
  ok &= testNativeSuccessorRequiresKnownBlock();
  ok &= testNativeMultipleSuccessorsRequireTerminator();
  ok &= testNativeFallthroughRequiresSuccessorFacts();
  ok &= testEmptyNativeBlockRequiresSuccessorFacts();
  ok &= testNativeConditionalSuccessorsRequireTrueTarget();
  ok &= testNativeConditionalRequiresSuccessorFacts();
  ok &= testNativeRelativeConditionalRequiresNativeTargetBlock();
  ok &= testNativeConditionalOutsideTrueTargetAllowsFalseOnlySuccessor();
  ok &= testNativeInternalConditionalKeepsSkippedPcode();
  ok &= testNativeInternalConditionalJoinUsesPhiForPartialUniqueDef();
  ok &= testNativeInternalConditionalRequiresTrueSuccessorFact();
  ok &= testNativeConditionalSuccessorsRejectMultipleFalseTargets();
  ok &= testNativeIndirectBranchCanUseSingleSuccessor();
  ok &= testNativeIndirectBranchRequiresSuccessorFacts();
  ok &= testNativeIndirectBranchWithNoSuccessorsIsUnknownTailCall();
  ok &= testNativeIndirectBranchLowersMultipleSuccessorsAsSwitch();
  ok &= testX64CallSuppressesReturnAddressStackEffect();
  ok &= testX64ReturnSuppressesReturnAddressStackEffect();
  ok &= testNonX64DoesNotSuppressCallStackEffect();
  ok &= testX86PcThunkCallFoldsToConstantBase();
  ok &= testPartialRegisterWriteUsesPartialWriteHelper();
  ok &= testPartialRegisterReadUsesPartialReadHelper();
  ok &= testX87FildlFoldsToIntrinsicCall();
  ok &= testX87FstpFoldsToIntrinsicCall();
  ok &= testX87FldzFoldsToIntrinsicCall();
  ok &= testX87FdivrpSt2FoldsToIntrinsicCall();
  ok &= testX87FcomiFoldsToIntrinsicCall();
  ok &= testX87FucomipFoldsToIntrinsicCall();
  ok &= testX87FpremFnstswFoldsToIntrinsicCall();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
