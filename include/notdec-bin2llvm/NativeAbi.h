#pragma once

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace notdec::bin2llvm {

enum class NativeAbiStorageKind {
  Register,
  Stack,
};

enum class NativeAbiEffectKind {
  Unaffected,
  KilledByCall,
};

// A small storage descriptor copied from Ghidra cspec pentry/effect records.
// Later prototype recovery code needs a uniform view of register and stack
// locations, but this first step intentionally keeps only the XML facts.
struct NativeAbiStorage {
  NativeAbiStorageKind Kind = NativeAbiStorageKind::Register;
  std::string Name;
  std::string Space;
  uint64_t Offset = 0;
};

// This mirrors the cspec <pentry> shape used by Ghidra ParamEntry.  It stores
// the ABI slot constraints without trying to run Ghidra's full rule engine yet.
struct NativeAbiParamEntry {
  uint32_t MinSize = 0;
  uint32_t MaxSize = 0;
  uint32_t Align = 0;
  std::string MetaType;
  NativeAbiStorage Storage;
};

// Ghidra stores unaffected/killedbycall as EffectRecord entries.  We keep the
// same split so callsite handling can later ask the same question as hasEffect.
struct NativeAbiEffect {
  NativeAbiEffectKind Kind = NativeAbiEffectKind::Unaffected;
  NativeAbiStorage Storage;
};

// NativeAbiSpec is the native-side minimum copy of Ghidra's default ProtoModel.
// It is intentionally data-only; matching and recovery rules live in later
// stages of the prototype pass.
struct NativeAbiSpec {
  std::string PrototypeName;
  std::string StackPointerRegister;
  std::string StackPointerSpace;
  int64_t ExtraPop = 0;
  uint64_t StackShift = 0;
  std::vector<NativeAbiParamEntry> Inputs;
  std::vector<NativeAbiParamEntry> Outputs;
  std::vector<NativeAbiEffect> Effects;
};

std::optional<NativeAbiSpec> parseGhidraCspecDefaultAbi(
    llvm::StringRef cspecPath, std::string &errorMessage);

void attachNativeAbiMetadata(llvm::Module &module, const NativeAbiSpec &abi);

std::optional<NativeAbiSpec> readNativeAbiMetadata(const llvm::Module &module);

} // namespace notdec::bin2llvm
