#include "RegisterTestHarness.h"

namespace {

bool testRegisterValueSourceAuditClassifiesReads() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-value-source-audit", context);
  attachTestAbi(module);
  auto *i64Type = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  auto *type = llvm::FunctionType::get(i64Type, {}, false);

  llvm::Function *internal = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "audit_internal", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", internal);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *unknownHelper = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "notdec.unknown.i64", module);
  llvm::CallInst *unknown = builder.CreateCall(unknownHelper);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "reason=entry_range_rejected")};
  unknown->setMetadata("notdec.unknown.source", llvm::MDNode::get(context, fields));
  builder.CreateRet(loadRegister(builder, rax, "RAX", "entry_rax"));

  llvm::Function *exposed = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "audit_exposed", module);
  llvm::BasicBlock *exposedEntry =
      llvm::BasicBlock::Create(context, "entry", exposed);
  llvm::IRBuilder<> exposedBuilder(exposedEntry);
  exposedBuilder.CreateRet(loadRegister(exposedBuilder, rdx, "RDX", "entry_rdx"));

  notdec::bin2llvm::NativeRegisterValueSourceAudit audit =
      notdec::bin2llvm::auditNativeRegisterValueSources(module);
  return expect(audit.LocalRegisterReads == 1,
                "internal register read was not audited as local") &&
         expect(audit.BoundaryRegisterReads == 1,
                "exposed register read was not audited as boundary") &&
         expect(audit.UnknownValuesByReason.size() == 1 &&
                    audit.UnknownValuesByReason.begin()->first ==
                        "entry_range_rejected" &&
                    audit.UnknownValuesByReason.begin()->second == 1,
                "unknown value reason was not audited");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testRegisterValueSourceAuditClassifiesReads();
  return ok ? 0 : 1;
}
