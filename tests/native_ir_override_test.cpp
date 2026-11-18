#include "notdec-bin2llvm/core/NativeIROverride.h"
#include "notdec-bin2llvm/core/NativePipeline.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

using namespace notdec::bin2llvm;

bool expect(bool Condition, const std::string &Message) {
  if (!Condition) {
    std::cerr << "FAIL: " << Message << '\n';
  }
  return Condition;
}

std::string makeTempDir() {
  std::filesystem::path Dir = std::filesystem::temp_directory_path() /
                               ("notdec-override-test-" +
                                std::to_string(static_cast<long>(::getpid())));
  std::filesystem::create_directories(Dir);
  return Dir.string();
}

void writeFile(const std::string &Path, const std::string &Text) {
  std::ofstream Output(Path);
  Output << Text;
}

llvm::Function *makeI64Function(llvm::Module &Module, llvm::StringRef Name,
                                llvm::GlobalValue::LinkageTypes Linkage) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Type = llvm::FunctionType::get(llvm::Type::getInt64Ty(Context), false);
  return llvm::Function::Create(Type, Linkage, Name, Module);
}

void setReturnConstant(llvm::Function &Function, uint64_t Value) {
  llvm::LLVMContext &Context = Function.getContext();
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", &Function);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateRet(llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(Context), Value));
}

std::optional<uint64_t> returnConstant(llvm::Function &Function) {
  auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
      Function.getEntryBlock().getTerminator());
  if (Return == nullptr) {
    return std::nullopt;
  }
  auto *Value = llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
  if (Value == nullptr) {
    return std::nullopt;
  }
  return Value->getZExtValue();
}

NativeIROverrideOptions makeOptions(const std::string &Path) {
  NativeIROverrideOptions Options;
  Options.File = Path;
  Options.ResolvedFile = Path;
  Options.Sha256 = "deadbeef";
  return Options;
}

bool testReplaceByName(const std::string &Dir) {
  bool Ok = true;
  std::string OverridePath = Dir + "/replace.ll";
  writeFile(OverridePath, "define i64 @foo() {\nentry:\n  ret i64 2\n}\n");

  llvm::LLVMContext Context;
  llvm::Module Module("override-test", Context);
  llvm::Function *Foo =
      makeI64Function(Module, "foo", llvm::GlobalValue::ExternalLinkage);
  setReturnConstant(*Foo, 1);
  llvm::Function *Caller =
      makeI64Function(Module, "caller", llvm::GlobalValue::InternalLinkage);
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Caller);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateRet(Builder.CreateCall(Foo));

  std::string Error;
  std::vector<NativeFunctionIdentity> Identities;
  bool Applied = applyNativeIROverride(Module, makeOptions(OverridePath),
                                       Identities, Error);
  if (!expect(Applied, "replace applies: " + Error)) {
    return false;
  }
  Ok &= expect(returnConstant(*Foo) == 2, "foo body is replaced");
  Ok &= expect(llvm::verifyModule(Module, &llvm::errs()) == false,
               "module verifies after replace");
  auto *Call = llvm::dyn_cast<llvm::CallInst>(
      &*llvm::inst_begin(Caller));
  Ok &= expect(Call != nullptr && Call->getCalledFunction() == Foo,
               "caller still calls the same function object");
  Ok &= expect(Foo->getMetadata("notdec.override") != nullptr,
               "override metadata is attached");
  return Ok;
}

bool testReplaceWithArguments(const std::string &Dir) {
  bool Ok = true;
  std::string OverridePath = Dir + "/replace-args.ll";
  writeFile(OverridePath,
            "define i64 @add(i64 %a, i64 %b) {\n"
            "entry:\n"
            "  %s = add i64 %a, %b\n"
            "  ret i64 %s\n"
            "}\n");

  llvm::LLVMContext Context;
  llvm::Module Module("override-args-test", Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  auto *Type = llvm::FunctionType::get(I64, {I64, I64}, false);
  llvm::Function *Add = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "add", Module);
  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Add);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateRet(llvm::ConstantInt::get(I64, 0));

  std::string Error;
  std::vector<NativeFunctionIdentity> Identities;
  bool Applied = applyNativeIROverride(Module, makeOptions(OverridePath),
                                       Identities, Error);
  if (!expect(Applied, "replace with arguments applies: " + Error)) {
    return false;
  }
  // 修复前：带形参的函数克隆会在 CloneFunctionAttributesInto 里段错误（VMap
  // 缺形参映射）。这里确认克隆出来的函数体真的用了目标形参。
  bool SawAdd = false;
  for (llvm::Instruction &Inst : llvm::instructions(*Add)) {
    if (auto *Bin = llvm::dyn_cast<llvm::BinaryOperator>(&Inst)) {
      SawAdd = Bin->getOpcode() == llvm::Instruction::Add;
    }
  }
  Ok &= expect(SawAdd, "override body with arguments is cloned");
  Ok &= expect(Add->getMetadata("notdec.override") != nullptr,
               "override metadata is attached (arguments case)");
  Ok &= expect(llvm::verifyModule(Module, &llvm::errs()) == false,
               "module verifies after replace with arguments");
  return Ok;
}

bool testSelectAndEntry(const std::string &Dir) {
  bool Ok = true;
  std::string OverridePath = Dir + "/select.ll";
  writeFile(OverridePath, "define i64 @impl() {\nentry:\n  ret i64 7\n}\n");

  llvm::LLVMContext Context;
  llvm::Module Module("select-test", Context);
  llvm::Function *Bar =
      makeI64Function(Module, "bar", llvm::GlobalValue::InternalLinkage);
  setReturnConstant(*Bar, 1);

  NativeIROverrideOptions Options = makeOptions(OverridePath);
  Options.Entry = "impl";
  Options.Select.push_back(NativeFunctionSelector{});
  Options.Select.back().Name = "bar";
  std::vector<NativeFunctionIdentity> Identities;
  std::string Error;
  bool Applied = applyNativeIROverride(Module, Options, Identities, Error);
  if (!expect(Applied, "select applies: " + Error)) {
    return false;
  }
  Ok &= expect(returnConstant(*Bar) == 7, "selected function is replaced");
  return Ok;
}

bool testHelperImport(const std::string &Dir) {
  bool Ok = true;
  std::string OverridePath = Dir + "/helper.ll";
  writeFile(OverridePath, R"(define i64 @foo() {
entry:
  %v = call i64 @helper()
  ret i64 %v
}
define internal i64 @helper() {
entry:
  ret i64 5
}
)");

  llvm::LLVMContext Context;
  llvm::Module Module("helper-test", Context);
  llvm::Function *Foo =
      makeI64Function(Module, "foo", llvm::GlobalValue::ExternalLinkage);
  setReturnConstant(*Foo, 1);

  std::vector<NativeFunctionIdentity> Identities;
  std::string Error;
  bool Applied = applyNativeIROverride(Module, makeOptions(OverridePath),
                                       Identities, Error);
  if (!expect(Applied, "helper import applies: " + Error)) {
    return false;
  }
  llvm::Function *Helper = Module.getFunction("helper");
  Ok &= expect(Helper != nullptr && !Helper->isDeclaration(),
               "helper definition is imported");
  Ok &= expect(llvm::verifyModule(Module, &llvm::errs()) == false,
               "module verifies with helper");
  return Ok;
}

bool testMissingSymbolPolicy(const std::string &Dir) {
  bool Ok = true;
  std::string OverridePath = Dir + "/extern.ll";
  writeFile(OverridePath, R"(declare i64 @external()
define i64 @foo() {
entry:
  %v = call i64 @external()
  ret i64 %v
}
)");

  llvm::LLVMContext Context;
  llvm::Module Strict("strict", Context);
  llvm::Function *Foo =
      makeI64Function(Strict, "foo", llvm::GlobalValue::ExternalLinkage);
  setReturnConstant(*Foo, 1);
  std::vector<NativeFunctionIdentity> Identities;
  std::string Error;
  auto StrictOptions = makeOptions(OverridePath);
  StrictOptions.AllowUndefined = false;
  Ok &= expect(!applyNativeIROverride(Strict, StrictOptions, Identities,
                                      Error) &&
                   Error.find("missing from the module") != std::string::npos,
               "strict mode rejects missing symbols: " + Error);

  llvm::LLVMContext LooseContext;
  llvm::Module Loose("loose", LooseContext);
  llvm::Function *LooseFoo =
      makeI64Function(Loose, "foo", llvm::GlobalValue::ExternalLinkage);
  setReturnConstant(*LooseFoo, 1);
  auto LooseOptions = makeOptions(OverridePath);
  LooseOptions.AllowUndefined = true;
  Error.clear();
  bool LooseApplied =
      applyNativeIROverride(Loose, LooseOptions, Identities, Error);
  if (!expect(LooseApplied,
              "allow_undefined imports declaration: " + Error)) {
    return false;
  }
  llvm::Function *External = Loose.getFunction("external");
  Ok &= expect(External != nullptr && External->isDeclaration(),
               "missing symbol becomes a declaration");
  Ok &= expect(llvm::verifyModule(Loose, &llvm::errs()) == false,
               "module verifies with imported declaration");
  return Ok;
}

bool testTypeMismatch(const std::string &Dir) {
  std::string OverridePath = Dir + "/type.ll";
  writeFile(OverridePath, "define i32 @foo() {\nentry:\n  ret i32 0\n}\n");
  llvm::LLVMContext Context;
  llvm::Module Module("type-test", Context);
  llvm::Function *Foo =
      makeI64Function(Module, "foo", llvm::GlobalValue::ExternalLinkage);
  setReturnConstant(*Foo, 1);
  std::vector<NativeFunctionIdentity> Identities;
  std::string Error;
  bool Applied = applyNativeIROverride(Module, makeOptions(OverridePath),
                                       Identities, Error);
  return expect(!Applied && Error.find("type") != std::string::npos,
                "type mismatch is rejected: " + Error);
}

bool testPinSkipsInstCombine(const std::string &Dir) {
  (void)Dir;
  llvm::LLVMContext Context;
  llvm::Module Module("pin-test", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Type = llvm::FunctionType::get(I64, {I64}, false);
  auto makeAdd = [&](llvm::StringRef Name) {
    llvm::Function *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::InternalLinkage, Name, Module);
    llvm::BasicBlock *Entry =
        llvm::BasicBlock::Create(Context, "entry", Function);
    llvm::IRBuilder<> Builder(Entry);
    llvm::Value *Sum = Builder.CreateAdd(Function->getArg(0),
                                         Function->getArg(0));
    Builder.CreateRet(Sum);
    return Function;
  };
  llvm::Function *Pinned = makeAdd("pinned");
  llvm::Function *Other = makeAdd("other");

  NativeConfig Config;
  NativeStageConfig Stage;
  Stage.Kind = NativeStageKind::InstCombine;
  Stage.Name = "instcombine";
  NativeFunctionSelector Selector;
  Selector.Name = "pinned";
  Stage.InstCombine.Exclude.push_back(Selector);
  Config.Pipeline.push_back(Stage);

  NativePipelineContext Pipeline{Module, Config};
  std::string Error;
  if (!expect(runNativePipeline(Pipeline, Error),
              "pipeline runs: " + Error)) {
    return false;
  }
  auto countAdds = [](llvm::Function &Function) {
    unsigned Count = 0;
    for (llvm::Instruction &Instruction : llvm::instructions(Function)) {
      if (Instruction.getOpcode() == llvm::Instruction::Add) {
        ++Count;
      }
    }
    return Count;
  };
  bool Ok = true;
  Ok &= expect(countAdds(*Pinned) == 1, "excluded function keeps its add");
  Ok &= expect(countAdds(*Other) == 0, "other function is folded");
  Ok &= expect(llvm::verifyModule(Module, &llvm::errs()) == false,
               "module verifies after pipeline");
  return Ok;
}

bool testFunctionClaims() {
  llvm::LLVMContext Context;
  llvm::Module Module("claims-test", Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getInt64Ty(Context), false);
  llvm::Function *Marked = llvm::Function::Create(
      Type, llvm::GlobalValue::InternalLinkage, "marked", Module);
  setReturnConstant(*Marked, 0);

  NativeConfig Config;
  NativeFunctionConfig Claim;
  Claim.Select.Name = "marked";
  Claim.Pin = true;
  Claim.Attributes.NoReturn = true;
  Claim.Attributes.Memory = "none";
  Config.Functions.push_back(Claim);

  NativePipelineContext Pipeline{Module, Config};
  std::string Error;
  bool Ok = expect(runNativePipeline(Pipeline, Error),
                   "empty pipeline with claims runs: " + Error);
  Ok &= expect(Marked->hasFnAttribute(llvm::Attribute::NoReturn),
               "noreturn attribute is applied");
  Ok &= expect(Marked->getMemoryEffects() == llvm::MemoryEffects::none(),
               "memory(none) is applied");
  Ok &= expect(Pipeline.PinnedFunctions.count("marked") == 1,
               "pin is recorded");
  Ok &= expect(Pipeline.PreservedFunctions.count("marked") == 1,
               "pin is preserved through DCE");
  return Ok;
}

} // namespace

int main() {
  bool Ok = true;
  std::string Dir = makeTempDir();
  Ok &= testReplaceByName(Dir);
  Ok &= testReplaceWithArguments(Dir);
  Ok &= testSelectAndEntry(Dir);
  Ok &= testHelperImport(Dir);
  Ok &= testMissingSymbolPolicy(Dir);
  Ok &= testTypeMismatch(Dir);
  Ok &= testPinSkipsInstCombine(Dir);
  Ok &= testFunctionClaims();
  std::filesystem::remove_all(Dir);
  if (!Ok) {
    return EXIT_FAILURE;
  }
  llvm::outs() << "native ir override tests passed\n";
  return EXIT_SUCCESS;
}
