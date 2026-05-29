#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativePrototypeModel.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

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

  notdec::bin2llvm::NativePrototypeModel model(*abi);
  std::optional<notdec::bin2llvm::NativeStorageMatch> rdi =
      model.findInputRegister("RDI");
  std::optional<notdec::bin2llvm::NativeStorageMatch> rax =
      model.findOutputRegister("RAX");

  bool ok = true;
  ok &= expect(rdi.has_value(), "RDI did not match input storage");
  ok &= expect(rdi && rdi->Slot == 8, "RDI input slot changed");
  ok &= expect(rdi && rdi->Entry->MinSize == 1 && rdi->Entry->MaxSize == 8,
               "RDI size constraints changed");
  ok &= expect(rax.has_value(), "RAX did not match output storage");
  ok &= expect(rax && rax->Slot == 2, "RAX output slot changed");
  ok &= expect(!model.findInputRegister("RBX").has_value(),
               "RBX unexpectedly matched input storage");
  ok &= expect(!model.findOutputRegister("RBX").has_value(),
               "RBX unexpectedly matched output storage");
  ok &= expect(notdec::bin2llvm::nativeAbiHasEffectRegister(
                   *abi, notdec::bin2llvm::NativeAbiEffectKind::Unaffected,
                   "RBX"),
               "RBX is not marked unaffected");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
