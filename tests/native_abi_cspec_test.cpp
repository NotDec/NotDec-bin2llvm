#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool hasParamRegister(
    const std::vector<notdec::bin2llvm::NativeAbiParamEntry> &entries,
    const std::string &name) {
  for (const notdec::bin2llvm::NativeAbiParamEntry &entry : entries) {
    if (entry.Storage.Kind ==
            notdec::bin2llvm::NativeAbiStorageKind::Register &&
        entry.Storage.Name == name) {
      return true;
    }
  }
  return false;
}

const notdec::bin2llvm::NativeAbiParamEntry *findParamRegister(
    const std::vector<notdec::bin2llvm::NativeAbiParamEntry> &entries,
    const std::string &name) {
  for (const notdec::bin2llvm::NativeAbiParamEntry &entry : entries) {
    if (entry.Storage.Kind ==
            notdec::bin2llvm::NativeAbiStorageKind::Register &&
        entry.Storage.Name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool hasEffectRegister(const notdec::bin2llvm::NativeAbiSpec &abi,
                       notdec::bin2llvm::NativeAbiEffectKind kind,
                       const std::string &name) {
  for (const notdec::bin2llvm::NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind == kind &&
        effect.Storage.Kind ==
            notdec::bin2llvm::NativeAbiStorageKind::Register &&
        effect.Storage.Name == name) {
      return true;
    }
  }
  return false;
}

bool expect(bool condition, const std::string &message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <x86-64-gcc.cspec>\n";
    return EXIT_FAILURE;
  }

  std::string errorMessage;
  std::optional<notdec::bin2llvm::NativeAbiSpec> abi =
      notdec::bin2llvm::parseGhidraCspecDefaultAbi(argv[1], errorMessage);
  if (!abi) {
    std::cerr << errorMessage << '\n';
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok &= expect(abi->PrototypeName == "__stdcall",
               "unexpected default prototype name");
  ok &= expect(abi->StackPointerRegister == "RSP",
               "missing stack pointer register");
  ok &= expect(hasParamRegister(abi->Inputs, "RDI"), "missing input RDI");
  ok &= expect(hasParamRegister(abi->Inputs, "RSI"), "missing input RSI");
  ok &= expect(hasParamRegister(abi->Inputs, "RDX"), "missing input RDX");
  ok &= expect(hasParamRegister(abi->Inputs, "RCX"), "missing input RCX");
  ok &= expect(hasParamRegister(abi->Inputs, "R8"), "missing input R8");
  ok &= expect(hasParamRegister(abi->Inputs, "R9"), "missing input R9");
  ok &= expect(hasParamRegister(abi->Outputs, "RAX"), "missing output RAX");
  ok &= expect(hasParamRegister(abi->Outputs, "RDX"), "missing output RDX");
  const notdec::bin2llvm::NativeAbiParamEntry *xmm0Input =
      findParamRegister(abi->Inputs, "XMM0_Qa");
  ok &= expect(xmm0Input != nullptr, "missing float input XMM0_Qa");
  if (xmm0Input != nullptr) {
    ok &= expect(xmm0Input->MetaType == "float",
                 "XMM0_Qa input is not marked float");
    ok &= expect(xmm0Input->MinSize == 4 && xmm0Input->MaxSize == 8,
                 "XMM0_Qa input has unexpected size range");
  }
  const notdec::bin2llvm::NativeAbiParamEntry *xmm0Output =
      findParamRegister(abi->Outputs, "XMM0_Qa");
  ok &= expect(xmm0Output != nullptr, "missing float output XMM0_Qa");
  if (xmm0Output != nullptr) {
    ok &= expect(xmm0Output->MetaType == "float",
                 "XMM0_Qa output is not marked float");
  }
  ok &= expect(hasEffectRegister(*abi,
                                 notdec::bin2llvm::NativeAbiEffectKind::
                                     Unaffected,
                                 "RBX"),
               "missing unaffected RBX");
  ok &= expect(hasEffectRegister(*abi,
                                 notdec::bin2llvm::NativeAbiEffectKind::
                                     Unaffected,
                                 "R15"),
               "missing unaffected R15");
  ok &= expect(hasEffectRegister(*abi,
                                 notdec::bin2llvm::NativeAbiEffectKind::
                                     KilledByCall,
                                 "RAX"),
               "missing killedbycall RAX");
  ok &= expect(hasEffectRegister(*abi,
                                 notdec::bin2llvm::NativeAbiEffectKind::
                                     KilledByCall,
                                 "XMM0"),
               "missing killedbycall XMM0");
  if (!ok) {
    return EXIT_FAILURE;
  }

  llvm::LLVMContext context;
  llvm::Module module("native-abi-cspec-test", context);
  notdec::bin2llvm::attachNativeAbiMetadata(module, *abi);
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after ABI metadata\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
