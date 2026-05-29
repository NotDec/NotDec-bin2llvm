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

std::optional<NativeStorageMatch> NativePrototypeModel::findInputStack(
    const std::string &space, uint64_t offset, uint32_t size) const {
  return findStack(Abi.Inputs, space, offset, size);
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

std::optional<NativeStorageMatch> NativePrototypeModel::findStack(
    const std::vector<NativeAbiParamEntry> &entries, const std::string &space,
    uint64_t offset, uint32_t size) const {
  for (size_t index = 0; index < entries.size(); ++index) {
    const NativeAbiParamEntry &entry = entries[index];
    if (entry.Storage.Kind != NativeAbiStorageKind::Stack) {
      continue;
    }
    if (entry.Storage.Space != space) {
      continue;
    }
    if (size < entry.MinSize || size > entry.MaxSize) {
      continue;
    }
    if (offset < entry.Storage.Offset) {
      continue;
    }
    uint64_t relative = offset - entry.Storage.Offset;
    if (relative > entry.MaxSize || size > entry.MaxSize - relative) {
      continue;
    }
    if (entry.Align != 0 && relative % entry.Align != 0) {
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
