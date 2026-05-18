#include "notdec-bin2llvm/RegisterStorage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace notdec::bin2llvm {
namespace {

unsigned bitWidth(uint32_t byteSize) {
  return byteSize == 0 ? 1 : byteSize * 8;
}

std::string sanitizeName(const std::string &name) {
  std::string result;
  result.reserve(name.size());
  for (char ch : name) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) || ch == '_' || ch == '$' || ch == '.') {
      result.push_back(ch);
    } else {
      result.push_back('_');
    }
  }
  if (result.empty()) {
    return "";
  }
  if (std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

std::string fallbackName(const std::string &space, uint64_t offset,
                         uint32_t size) {
  std::ostringstream os;
  os << "notdec_reg_" << sanitizeName(space) << '_' << std::hex << offset << '_'
     << std::dec << size;
  return os.str();
}

bool overlaps(uint64_t lhsOffset, uint32_t lhsSize, uint64_t rhsOffset,
              uint32_t rhsSize) {
  uint64_t lhsEnd = lhsOffset + lhsSize;
  uint64_t rhsEnd = rhsOffset + rhsSize;
  return lhsOffset < rhsEnd && rhsOffset < lhsEnd;
}

} // namespace

RegisterStorage::RegisterStorage(llvm::LLVMContext &context,
                                 llvm::Module &module,
                                 std::vector<RegisterInfo> registers,
                                 bool isBigEndian)
    : Context(context), Module(module), IsBigEndian(isBigEndian) {
  registers.erase(std::remove_if(registers.begin(), registers.end(),
                                 [](const RegisterInfo &reg) {
                                   return reg.Space.empty() || reg.Size == 0;
                                 }),
                  registers.end());
  std::sort(registers.begin(), registers.end(),
            [](const RegisterInfo &lhs, const RegisterInfo &rhs) {
              if (lhs.Space != rhs.Space) {
                return lhs.Space < rhs.Space;
              }
              if (lhs.Offset != rhs.Offset) {
                return lhs.Offset < rhs.Offset;
              }
              return lhs.Size > rhs.Size;
            });

  for (const RegisterInfo &reg : registers) {
    bool merged = false;
    for (RegisterUnit &unit : Units) {
      if (unit.Space != reg.Space ||
          !overlaps(unit.Offset, unit.Size, reg.Offset, reg.Size)) {
        continue;
      }
      uint64_t end = std::max(unit.Offset + unit.Size, reg.Offset + reg.Size);
      unit.Offset = std::min(unit.Offset, reg.Offset);
      unit.Size = static_cast<uint32_t>(end - unit.Offset);
      merged = true;
      break;
    }
    if (!merged) {
      Units.push_back(RegisterUnit{reg.Space, reg.Offset, reg.Size, reg.Name});
    }
  }

  for (RegisterUnit &unit : Units) {
    for (const RegisterInfo &reg : registers) {
      if (reg.Space == unit.Space && reg.Offset == unit.Offset &&
          reg.Size == unit.Size && !reg.Name.empty()) {
        unit.Name = reg.Name;
        break;
      }
    }
    if (unit.Name.empty()) {
      unit.Name = fallbackName(unit.Space, unit.Offset, unit.Size);
    }
  }
}

bool RegisterStorage::hasRegister(const RegisterAccess &access) const {
  return unitFor(access) != nullptr;
}

RegisterStorage::RegisterUnit *
RegisterStorage::unitFor(const RegisterAccess &access) {
  return const_cast<RegisterUnit *>(
      static_cast<const RegisterStorage *>(this)->unitFor(access));
}

const RegisterStorage::RegisterUnit *
RegisterStorage::unitFor(const RegisterAccess &access) const {
  for (const RegisterUnit &unit : Units) {
    if (unit.Space == access.Space && unit.Offset <= access.Offset &&
        access.Offset + access.Size <= unit.Offset + unit.Size) {
      return &unit;
    }
  }
  return nullptr;
}

llvm::GlobalVariable *RegisterStorage::globalFor(RegisterUnit &unit) {
  if (unit.Global != nullptr) {
    return unit.Global;
  }

  std::string baseName = sanitizeName(unit.Name);
  if (baseName.empty()) {
    baseName = fallbackName(unit.Space, unit.Offset, unit.Size);
  }
  std::string name = baseName;
  unsigned index = 1;
  while (Module.getNamedValue(name) != nullptr) {
    name = baseName + "." + std::to_string(index++);
  }

  auto *type = llvm::IntegerType::get(Context, bitWidth(unit.Size));
  unit.Global = new llvm::GlobalVariable(
      Module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);

  llvm::Metadata *metadata[] = {
      llvm::MDString::get(Context, "space=" + unit.Space),
      llvm::MDString::get(Context, "offset=" + std::to_string(unit.Offset)),
      llvm::MDString::get(Context, "size=" + std::to_string(unit.Size)),
      llvm::MDString::get(Context, "name=" + unit.Name),
  };
  unit.Global->setMetadata("notdec.register",
                           llvm::MDNode::get(Context, metadata));
  return unit.Global;
}

uint64_t RegisterStorage::bitOffset(const RegisterUnit &unit,
                                    const RegisterAccess &access) const {
  uint64_t relative = access.Offset - unit.Offset;
  if (IsBigEndian) {
    relative = unit.Size - relative - access.Size;
  }
  return relative * 8;
}

void RegisterStorage::addAccessMetadata(llvm::Value *instruction,
                                        const RegisterUnit &unit,
                                        const RegisterAccess &access) {
  auto *inst = llvm::dyn_cast<llvm::Instruction>(instruction);
  if (inst == nullptr) {
    return;
  }
  std::string accessName = access.Name.value_or("");
  llvm::Metadata *metadata[] = {
      llvm::MDString::get(Context, "base=" + unit.Name),
      llvm::MDString::get(Context, "space=" + access.Space),
      llvm::MDString::get(Context, "offset=" + std::to_string(access.Offset)),
      llvm::MDString::get(Context, "size=" + std::to_string(access.Size)),
      llvm::MDString::get(Context, "name=" + accessName),
  };
  inst->setMetadata("notdec.register.access",
                    llvm::MDNode::get(Context, metadata));
}

llvm::Value *RegisterStorage::read(llvm::IRBuilderBase &builder,
                                   const RegisterAccess &access) {
  RegisterUnit *unit = unitFor(access);
  if (unit == nullptr) {
    return nullptr;
  }

  llvm::GlobalVariable *global = globalFor(*unit);
  llvm::Value *value =
      builder.CreateLoad(global->getValueType(), global, access.Name.value_or(""));
  addAccessMetadata(value, *unit, access);
  if (unit->Offset == access.Offset && unit->Size == access.Size) {
    return value;
  }

  auto *targetType = llvm::IntegerType::get(Context, bitWidth(access.Size));
  llvm::Value *shifted = builder.CreateLShr(
      value, llvm::ConstantInt::get(value->getType(), bitOffset(*unit, access)));
  return builder.CreateTrunc(shifted, targetType);
}

void RegisterStorage::write(llvm::IRBuilderBase &builder,
                            const RegisterAccess &access, llvm::Value *value) {
  RegisterUnit *unit = unitFor(access);
  if (unit == nullptr) {
    return;
  }

  llvm::GlobalVariable *global = globalFor(*unit);
  auto *globalType = llvm::cast<llvm::IntegerType>(global->getValueType());
  llvm::Value *resized = builder.CreateZExtOrTrunc(value, globalType);
  if (unit->Offset == access.Offset && unit->Size == access.Size) {
    llvm::Value *store = builder.CreateStore(resized, global);
    addAccessMetadata(store, *unit, access);
    return;
  }

  llvm::Value *oldValue = builder.CreateLoad(globalType, global);
  uint64_t shift = bitOffset(*unit, access);
  llvm::APInt mask =
      llvm::APInt::getLowBitsSet(globalType->getBitWidth(), bitWidth(access.Size))
          .shl(shift);
  llvm::Value *cleared =
      builder.CreateAnd(oldValue, llvm::ConstantInt::get(globalType, ~mask));
  llvm::Value *shifted = builder.CreateAnd(
      builder.CreateShl(resized, llvm::ConstantInt::get(globalType, shift)),
      llvm::ConstantInt::get(globalType, mask));
  llvm::Value *store = builder.CreateStore(builder.CreateOr(cleared, shifted),
                                           global);
  addAccessMetadata(store, *unit, access);
}

} // namespace notdec::bin2llvm
