#include "notdec-bin2llvm/core/NativeIROverride.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

using llvm::StringRef;
using llvm::ValueToValueMapTy;

bool fail(std::string &Error, StringRef Path, const llvm::Twine &Message) {
  Error = (Path + ": " + Message).str();
  return false;
}

std::string formatType(llvm::FunctionType *Type) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Type->print(Stream);
  return Text;
}

void collectUnmappedGlobals(
    const llvm::Value *Value, const ValueToValueMapTy &Map,
    llvm::SmallPtrSetImpl<const llvm::GlobalValue *> &Missing,
    llvm::SmallPtrSetImpl<const llvm::Value *> &Visited) {
  if (!Visited.insert(Value).second) {
    return;
  }
  if (const auto *Global = llvm::dyn_cast<llvm::GlobalValue>(Value)) {
    if (Map.count(Global) == 0) {
      Missing.insert(Global);
    }
    return;
  }
  if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value)) {
    for (const llvm::Use &Operand : Constant->operands()) {
      collectUnmappedGlobals(Operand.get(), Map, Missing, Visited);
    }
  }
}

// 克隆前确认函数体里引用的每个全局符号都能映射到目标 module，否则会在目标
// module 里留下跨 module 的悬空引用（verifier 不一定会报）。
bool checkFunctionSymbolsMapped(llvm::Function &Function,
                                const ValueToValueMapTy &Map,
                                StringRef Path, std::string &Error) {
  llvm::SmallPtrSet<const llvm::GlobalValue *, 8> Missing;
  llvm::SmallPtrSet<const llvm::Value *, 32> Visited;
  for (llvm::BasicBlock &Block : Function) {
    for (llvm::Instruction &Instruction : Block) {
      for (llvm::Use &Operand : Instruction.operands()) {
        collectUnmappedGlobals(Operand.get(), Map, Missing, Visited);
      }
    }
  }
  if (Missing.empty()) {
    return true;
  }
  std::vector<std::string> Names;
  for (const llvm::GlobalValue *Global : Missing) {
    Names.push_back(Global->getName().str());
  }
  llvm::sort(Names);
  return fail(Error, Path,
              "definition '" + Function.getName() +
                  "' references symbols that are not available in the "
                  "module: " +
                  llvm::join(Names, ", ") +
                  " (helper functions must not call the override entry)");
}

// 把 override module 里除入口外的函数/全局按名字导入目标 module。
bool importOverrideSymbols(llvm::Module &Target, llvm::Module &Source,
                           bool AllowUndefined, llvm::Function &Entry,
                           ValueToValueMapTy &Map, StringRef Path,
                           std::string &Error) {
  std::vector<llvm::Function *> Functions;
  for (llvm::Function &Function : Source) {
    if (&Function != &Entry) {
      Functions.push_back(&Function);
    }
  }
  llvm::sort(Functions, [](llvm::Function *Lhs, llvm::Function *Rhs) {
    return Lhs->getName() < Rhs->getName();
  });

  std::vector<std::string> Missing;
  for (llvm::Function *SourceFunction : Functions) {
    if (llvm::Function *Existing =
            Target.getFunction(SourceFunction->getName())) {
      if (Existing->getFunctionType() !=
          SourceFunction->getFunctionType()) {
        return fail(Error, Path,
                    "symbol '" + SourceFunction->getName() +
                        "' exists in the module with type " +
                        formatType(Existing->getFunctionType()) +
                        ", override declares " +
                        formatType(SourceFunction->getFunctionType()));
      }
      Map[SourceFunction] = Existing;
      continue;
    }
    if (SourceFunction->isIntrinsic()) {
      return fail(Error, Path,
                  "intrinsic '" + SourceFunction->getName() +
                      "' is not declared in the module");
    }
    if (!AllowUndefined) {
      Missing.push_back(SourceFunction->getName().str());
      continue;
    }
    llvm::Function *Created = llvm::Function::Create(
        SourceFunction->getFunctionType(), SourceFunction->getLinkage(),
        SourceFunction->getAddressSpace(), SourceFunction->getName(),
        &Target);
    Created->copyAttributesFrom(SourceFunction);
    Map[SourceFunction] = Created;
  }

  std::vector<llvm::GlobalVariable *> Globals;
  for (llvm::GlobalVariable &Global : Source.globals()) {
    Globals.push_back(&Global);
  }
  llvm::sort(Globals, [](llvm::GlobalVariable *Lhs,
                         llvm::GlobalVariable *Rhs) {
    return Lhs->getName() < Rhs->getName();
  });

  for (llvm::GlobalVariable *SourceGlobal : Globals) {
    if (llvm::GlobalVariable *Existing =
            Target.getNamedGlobal(SourceGlobal->getName())) {
      if (Existing->getValueType() != SourceGlobal->getValueType()) {
        return fail(Error, Path,
                    "global '" + SourceGlobal->getName() +
                        "' exists in the module with a different type");
      }
      Map[SourceGlobal] = Existing;
      continue;
    }
    if (!AllowUndefined) {
      Missing.push_back(SourceGlobal->getName().str());
      continue;
    }
    auto *Created = new llvm::GlobalVariable(
        Target, SourceGlobal->getValueType(), SourceGlobal->isConstant(),
        SourceGlobal->getLinkage(), /*Initializer=*/nullptr,
        SourceGlobal->getName(), /*InsertBefore=*/nullptr,
        SourceGlobal->getThreadLocalMode(), SourceGlobal->getAddressSpace());
    Created->copyAttributesFrom(SourceGlobal);
    Map[SourceGlobal] = Created;
  }

  if (!Missing.empty()) {
    llvm::sort(Missing);
    return fail(Error, Path,
                "override file references symbols missing from the module: " +
                    llvm::join(Missing, ", ") +
                    " (set allow_undefined=true to import them)");
  }

  for (llvm::GlobalVariable *SourceGlobal : Globals) {
    auto *Global = llvm::cast<llvm::GlobalVariable>(Map[SourceGlobal]);
    if (!SourceGlobal->hasInitializer() || Global->hasInitializer()) {
      continue;
    }
    Global->setInitializer(llvm::MapValue(SourceGlobal->getInitializer(), Map,
                                          llvm::RF_None, nullptr, nullptr));
  }
  return true;
}

// 克隆 override 文件里的额外函数定义（入口函数单独按目标克隆）。
bool importExtraDefinitions(llvm::Module &Target, llvm::Module &Source,
                            llvm::Function &Entry,
                            const ValueToValueMapTy &SymbolMap,
                            StringRef Path, std::string &Error) {
  std::vector<llvm::Function *> Functions;
  for (llvm::Function &Function : Source) {
    if (&Function != &Entry && !Function.isDeclaration()) {
      Functions.push_back(&Function);
    }
  }
  llvm::sort(Functions, [](llvm::Function *Lhs, llvm::Function *Rhs) {
    return Lhs->getName() < Rhs->getName();
  });

  for (llvm::Function *SourceFunction : Functions) {
    auto MapIt = SymbolMap.find(SourceFunction);
    if (MapIt == SymbolMap.end()) {
      return fail(Error, Path,
                  "internal error: missing symbol mapping for '" +
                      SourceFunction->getName() + "'");
    }
    auto *Destination = llvm::cast<llvm::Function>(MapIt->second);
    if (!Destination->isDeclaration()) {
      // 目标里已有同名定义：保留目标的实现，不覆盖。
      continue;
    }
    if (!checkFunctionSymbolsMapped(*SourceFunction, SymbolMap, Path,
                                    Error)) {
      return false;
    }
    ValueToValueMapTy Map;
    for (const auto &Entry : SymbolMap) {
      Map[Entry.first] = Entry.second;
    }
    llvm::SmallVector<llvm::ReturnInst *, 8> Returns;
    llvm::CloneFunctionInto(Destination, SourceFunction, Map,
                            llvm::CloneFunctionChangeType::DifferentModule,
                            Returns);
  }
  return true;
}

void clearFunctionDebugInfo(llvm::Function &Function) {
  Function.setSubprogram(nullptr);
  for (llvm::BasicBlock &Block : Function) {
    for (llvm::Instruction &Instruction : Block) {
      Instruction.setDebugLoc(llvm::DebugLoc());
    }
  }
}

void attachOverrideMetadata(llvm::Function &Function,
                            const NativeIROverrideOptions &Options) {
  llvm::LLVMContext &Context = Function.getContext();
  llvm::Metadata *Fields[] = {
      llvm::MDString::get(Context, "replace"),
      llvm::MDString::get(Context, Options.ResolvedFile),
      llvm::MDString::get(Context, Options.Sha256),
  };
  Function.setMetadata("notdec.override", llvm::MDNode::get(Context, Fields));
}

} // namespace

bool applyNativeIROverride(llvm::Module &Module,
                           const NativeIROverrideOptions &Options,
                           llvm::ArrayRef<NativeFunctionIdentity> Identities,
                           std::string &Error) {
  std::string Path = "ir-override(" + Options.File + ")";
  llvm::SMDiagnostic Diagnostic;
  std::unique_ptr<llvm::Module> Source =
      llvm::parseIRFile(Options.ResolvedFile, Diagnostic, Module.getContext());
  if (!Source) {
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    Diagnostic.print("ir-override", Stream);
    Stream.flush();
    return fail(Error, Path, "failed to parse override file: " + Text);
  }

  llvm::Function *Entry = nullptr;
  if (!Options.Entry.empty()) {
    Entry = Source->getFunction(Options.Entry);
    if (Entry == nullptr || Entry->isDeclaration()) {
      return fail(Error, Path,
                  "entry function '" + Options.Entry +
                      "' is not defined in the override file");
    }
  } else {
    std::vector<llvm::Function *> Definitions;
    for (llvm::Function &Function : *Source) {
      if (!Function.isDeclaration()) {
        Definitions.push_back(&Function);
      }
    }
    if (Definitions.size() == 1) {
      Entry = Definitions[0];
    } else {
      // 多个定义时优先选"名字在目标 module 里也存在"的那个；仍有歧义就报错。
      for (llvm::Function *Candidate : Definitions) {
        if (Module.getFunction(Candidate->getName()) == nullptr) {
          continue;
        }
        if (Entry != nullptr) {
          Entry = nullptr;
          break;
        }
        Entry = Candidate;
      }
    }
    if (Entry == nullptr) {
      return fail(Error, Path,
                  Definitions.empty()
                      ? "override file defines no function"
                      : "override file defines multiple functions; set "
                        "options.entry");
    }
  }

  std::set<std::string> Targets;
  if (!Options.Select.empty()) {
    if (!resolveNativeFunctionSelectors(
            Module, Identities, Options.Select, Path + ".select",
            /*IncludeDeclarations=*/false, Targets, Error)) {
      return false;
    }
  } else {
    llvm::Function *Target = Module.getFunction(Entry->getName());
    if (Target == nullptr || Target->isDeclaration()) {
      return fail(Error, Path,
                  "no target for entry '" + Entry->getName() +
                      "'; add options.select");
    }
    Targets.insert(Target->getName().str());
  }

  ValueToValueMapTy SymbolMap;
  if (!importOverrideSymbols(Module, *Source, Options.AllowUndefined, *Entry,
                             SymbolMap, Path, Error)) {
    return false;
  }
  if (!importExtraDefinitions(Module, *Source, *Entry, SymbolMap, Path,
                              Error)) {
    return false;
  }

  for (const std::string &Name : Targets) {
    llvm::Function *Target = Module.getFunction(Name);
    if (Target == nullptr || Target->isDeclaration()) {
      return fail(Error, Path, "target '" + Name + "' is not a definition");
    }
    if (Target->getFunctionType() != Entry->getFunctionType()) {
      return fail(Error, Path,
                  "target '" + Name + "' has type " +
                      formatType(Target->getFunctionType()) +
                      ", override entry has type " +
                      formatType(Entry->getFunctionType()));
    }
    ValueToValueMapTy Map;
    for (const auto &Symbol : SymbolMap) {
      Map[Symbol.first] = Symbol.second;
    }
    Map[Entry] = Target;

    Target->dropAllReferences();
    while (!Target->empty()) {
      Target->begin()->eraseFromParent();
    }
    llvm::SmallVector<llvm::ReturnInst *, 8> Returns;
    llvm::CloneFunctionInto(Target, Entry, Map,
                            llvm::CloneFunctionChangeType::DifferentModule,
                            Returns);
    if (Options.AttributesFromOverride) {
      Target->setAttributes(Entry->getAttributes());
      Target->setCallingConv(Entry->getCallingConv());
    }
    if (Options.StripDebug) {
      clearFunctionDebugInfo(*Target);
    }
    attachOverrideMetadata(*Target, Options);
  }

  // CloneFunctionInto(DifferentModule) 会补一个 !llvm.dbg.cu；没有实际编译
  // 单元时留一个空列表会让 llvm-as 报 "invalid version (0)"，直接删掉。
  if (llvm::NamedMDNode *CompileUnits =
          Module.getNamedMetadata("llvm.dbg.cu")) {
    if (CompileUnits->getNumOperands() == 0) {
      Module.eraseNamedMetadata(CompileUnits);
    }
  }
  return true;
}

} // namespace notdec::bin2llvm
