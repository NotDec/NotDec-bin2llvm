#pragma once

#include "notdec-bin2llvm/NativeAbi.h"

#include <cstddef>
#include <optional>
#include <string>

namespace notdec::bin2llvm {

// Result of matching a concrete storage location against a prototype pentry.
// It keeps the original ABI entry and the pentry slot so later recovery code
// can preserve calling-convention order without reparsing cspec metadata.
struct NativeStorageMatch {
  size_t Slot = 0;
  const NativeAbiParamEntry *Entry = nullptr;
};

// A small native copy of Ghidra's ParamList lookup surface.  For now it only
// matches register names from NativeAbiSpec; range/justify matching is added
// later when ABI storage carries p-code space/offset/size.
class NativePrototypeModel {
public:
  explicit NativePrototypeModel(const NativeAbiSpec &abi) : Abi(abi) {}

  std::optional<NativeStorageMatch> findInputRegister(
      const std::string &name) const;
  std::optional<NativeStorageMatch> findOutputRegister(
      const std::string &name) const;
  std::optional<NativeStorageMatch> findInputStack(const std::string &space,
                                                   uint64_t offset,
                                                   uint32_t size) const;

private:
  std::optional<NativeStorageMatch> findRegister(
      const std::vector<NativeAbiParamEntry> &entries,
      const std::string &name) const;
  std::optional<NativeStorageMatch> findStack(
      const std::vector<NativeAbiParamEntry> &entries,
      const std::string &space, uint64_t offset, uint32_t size) const;

  const NativeAbiSpec &Abi;
};

bool nativeAbiHasEffectRegister(const NativeAbiSpec &abi,
                                NativeAbiEffectKind kind,
                                const std::string &name);

} // namespace notdec::bin2llvm
