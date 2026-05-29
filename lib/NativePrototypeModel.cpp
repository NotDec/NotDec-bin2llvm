#include "notdec-bin2llvm/NativePrototypeModel.h"

#include <vector>

namespace notdec::bin2llvm {

std::optional<NativeStorageMatch> NativePrototypeModel::findInputRegister(
    const std::string &name) const {
  return findRegister(Abi.Inputs, name);
}

std::optional<NativeStorageMatch> NativePrototypeModel::findOutputRegister(
    const std::string &name) const {
  return findRegister(Abi.Outputs, name);
}

std::optional<NativeStorageMatch> NativePrototypeModel::findRegister(
    const std::vector<NativeAbiParamEntry> &entries,
    const std::string &name) const {
  for (size_t index = 0; index < entries.size(); ++index) {
    const NativeAbiParamEntry &entry = entries[index];
    if (entry.Storage.Kind != NativeAbiStorageKind::Register) {
      continue;
    }
    if (entry.Storage.Name != name) {
      continue;
    }
    return NativeStorageMatch{index, &entry};
  }
  return std::nullopt;
}

bool nativeAbiHasEffectRegister(const NativeAbiSpec &abi,
                                NativeAbiEffectKind kind,
                                const std::string &name) {
  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind != kind) {
      continue;
    }
    if (effect.Storage.Kind != NativeAbiStorageKind::Register) {
      continue;
    }
    if (effect.Storage.Name == name) {
      return true;
    }
  }
  return false;
}

} // namespace notdec::bin2llvm
