#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class IRBuilderBase;
class LLVMContext;
class Module;
class Value;
class GlobalVariable;
} // namespace llvm

namespace notdec::bin2llvm {

struct RegisterInfo {
  std::string Space;
  uint64_t Offset = 0;
  uint32_t Size = 0;
  std::string Name;
};

struct RegisterAccess {
  std::string Space;
  uint64_t Offset = 0;
  uint32_t Size = 0;
  std::optional<std::string> Name;
};

// Registers share the same p-code address space and can overlap.  This storage
// first groups overlapping register ranges into one backing global, then lowers
// smaller register accesses as bit slices of that global.  This keeps RAX/EAX/AX
// style accesses tied to one state object instead of creating unrelated values.
class RegisterStorage {
public:
  RegisterStorage(llvm::LLVMContext &context, llvm::Module &module,
                  std::vector<RegisterInfo> registers, bool isBigEndian);

  bool hasRegister(const RegisterAccess &access) const;
  llvm::Value *read(llvm::IRBuilderBase &builder,
                    const RegisterAccess &access);
  void write(llvm::IRBuilderBase &builder, const RegisterAccess &access,
             llvm::Value *value, bool isFloatWrite = false);

private:
  struct RegisterUnit {
    std::string Space;
    uint64_t Offset = 0;
    uint32_t Size = 0;
    std::string Name;
    llvm::GlobalVariable *Global = nullptr;
  };

  RegisterUnit *unitFor(const RegisterAccess &access);
  const RegisterUnit *unitFor(const RegisterAccess &access) const;
  llvm::GlobalVariable *globalFor(RegisterUnit &unit);
  uint64_t bitOffset(const RegisterUnit &unit,
                     const RegisterAccess &access) const;
  void addAccessMetadata(llvm::Value *instruction, const RegisterUnit &unit,
                         const RegisterAccess &access, bool isFloatWrite);

  llvm::LLVMContext &Context;
  llvm::Module &Module;
  std::vector<RegisterUnit> Units;
  bool IsBigEndian = false;
};

} // namespace notdec::bin2llvm
