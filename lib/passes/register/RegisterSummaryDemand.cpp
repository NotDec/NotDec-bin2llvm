#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

llvm::GlobalVariable *Analyzer::stackPointerGlobal() const {
    if (Abi.StackPointer.empty()) {
      return nullptr;
    }
    auto it = UnitsByName.find(Abi.StackPointer);
    return it == UnitsByName.end() ? nullptr : it->second;
  }

bool Analyzer::canTrackSavedEntryRegister(llvm::GlobalVariable *global) const {
    auto unitIt = Units.find(global);
    if (unitIt == Units.end()) {
      return false;
    }
    const RegisterUnit &unit = unitIt->second;
    return unit.Name != Abi.StackPointer &&
           Abi.Unaffected.count(unit.Name) != 0;
  }

bool Analyzer::instructionReadsRegister(const llvm::Instruction &inst,
                                const llvm::GlobalVariable &global) const {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      return load->getPointerOperand()->stripPointerCasts() == &global;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr) {
      return false;
    }
    std::optional<NativeRegisterPartialReadInfo> partial =
        parseNativeRegisterPartialRead(*call);
    return partial && partial->Global == &global;
  }

bool Analyzer::instructionWritesRegister(const llvm::Instruction &inst,
                                 const llvm::GlobalVariable &global) const {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      return store->getPointerOperand()->stripPointerCasts() == &global;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr) {
      return false;
    }
    std::optional<NativeRegisterPartialWriteInfo> partial =
        parseNativeRegisterPartialWrite(*call);
    return partial && partial->Global == &global;
  }

bool Analyzer::callMayReadRegister(const llvm::CallBase &call,
                           const llvm::GlobalVariable &global) const {
    auto unitIt = Units.find(const_cast<llvm::GlobalVariable *>(&global));
    if (unitIt == Units.end()) {
      return true;
    }
    llvm::StringRef name = unitIt->second.Name;
    auto mentions = [&](llvm::ArrayRef<NativeRegisterCallInputSlot> slots) {
      for (const NativeRegisterCallInputSlot &input : slots) {
        if (input.Kind == NativeRegisterCallInputSlotKind::Register &&
            input.UnitName == name) {
          return true;
        }
      }
      return false;
    };
    if (const NativeExternalCallShape *shape = externalCallShape(call)) {
      return mentions(shape->Inputs) || mentions(shape->VarArgInputs);
    }
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr || callee->isDeclaration()) {
      return true;
    }
    auto effectIt = Effects.find(callee);
    if (effectIt == Effects.end()) {
      return true;
    }
    auto regIt = effectIt->second.Registers.find(
        const_cast<llvm::GlobalVariable *>(&global));
    if (regIt == effectIt->second.Registers.end()) {
      return false;
    }
    const llvm::APInt &readEntry = regIt->second.ReadEntry;
    return readEntry.getBitWidth() != 0 && !readEntry.isZero();
  }

bool Analyzer::registerValueDiesAfter(llvm::StoreInst &store,
                              const llvm::GlobalVariable &global,
                              bool ignoreCallReads ) const {
    llvm::BasicBlock *block = store.getParent();
    if (block == nullptr) {
      return false;
    }
    constexpr unsigned maxBlocks = 64;
    llvm::SmallVector<llvm::BasicBlock *, 8> worklist;
    llvm::SmallPtrSet<llvm::BasicBlock *, 32> visited;
    visited.insert(block);
    for (llvm::BasicBlock *successor : llvm::successors(block)) {
      worklist.push_back(successor);
    }
    for (auto it = std::next(store.getIterator()); it != block->end(); ++it) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&*it);
          call != nullptr && !ignoreCallReads && isAnalyzableCall(*call) &&
          callMayReadRegister(*call, global)) {
        return false;
      }
      if (instructionReadsRegister(*it, global)) {
        return false;
      }
      if (instructionWritesRegister(*it, global)) {
        return true;
      }
    }
    while (!worklist.empty()) {
      if (visited.size() >= maxBlocks) {
        return false;
      }
      llvm::BasicBlock *current = worklist.pop_back_val();
      if (!visited.insert(current).second) {
        continue;
      }
      bool killed = false;
      for (const llvm::Instruction &inst : *current) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if ((call != nullptr && !ignoreCallReads && isAnalyzableCall(*call) &&
             callMayReadRegister(*call, global)) ||
            instructionReadsRegister(inst, global)) {
          return false;
        }
        if (instructionWritesRegister(inst, global)) {
          killed = true;
          break;
        }
      }
      if (killed) {
        continue;
      }
      for (llvm::BasicBlock *successor : llvm::successors(current)) {
        worklist.push_back(successor);
      }
    }
    return true;
  }

const std::set<StackSlotKey> &
  Analyzer::stackAdjustmentSlots(llvm::Function &function) const {
    auto cached = StackAdjustmentSlots.find(&function);
    if (cached != StackAdjustmentSlots.end()) {
      return cached->second;
    }
    std::set<StackSlotKey> slots;
    collectStackAdjustmentSlots(function, slots);
    return StackAdjustmentSlots.emplace(&function, std::move(slots))
        .first->second;
  }

std::optional<StackSlotKey>
  Analyzer::comparableSlotKey(llvm::Function &function,
                    const NativeStackAddress &address) const {
    std::optional<StackSlotKey> key = stackSlotKey(address);
    if (!key) {
      return std::nullopt;
    }
    if (address.Kind != NativeStackAddressKind::EntryStackPointer) {
      return key;
    }
    if (std::optional<StackSlotKey> converted =
            entrySlotToNativeFrameKey(function, *key)) {
      return converted;
    }
    return key;
  }

bool Analyzer::isStackAdjustmentAccess(llvm::Instruction &inst, llvm::Value *pointer,
                               uint64_t sizeBytes, bool push) const {
    llvm::Function *function = inst.getFunction();
    llvm::BasicBlock *block = inst.getParent();
    llvm::GlobalVariable *stackPointer = stackPointerGlobal();
    if (function == nullptr || block == nullptr || stackPointer == nullptr ||
        sizeBytes == 0) {
      return false;
    }
    auto isStackPointerStore = [stackPointer](llvm::Instruction &candidate) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&candidate);
      return store != nullptr &&
             store->getPointerOperand()->stripPointerCasts() == stackPointer;
    };
    NativeStackAddressAnalysis &analysis = stackAddressAnalysis(*function);
    std::optional<NativeStackAddress> slot =
        analysis.addressForPointer(pointer);
    std::optional<NativeStackAddress> current =
        analysis.stackPointerBefore(inst);
    if (!slot || !current) {
      return false;
    }
    std::optional<StackSlotKey> slotKey = comparableSlotKey(*function, *slot);
    std::optional<StackSlotKey> currentKey =
        comparableSlotKey(*function, *current);
    if (!slotKey || !currentKey || !(*slotKey == *currentKey)) {
      return false;
    }
    std::optional<NativeStackAddress> other;
    if (push) {
      for (auto it = inst.getIterator(); it != block->begin();) {
        --it;
        if (isStackPointerStore(*it)) {
          other = analysis.stackPointerBefore(*it);
          break;
        }
      }
    } else {
      for (auto it = std::next(inst.getIterator()); it != block->end(); ++it) {
        if (isStackPointerStore(*it)) {
          other = analysis.addressForIntegerValue(
              llvm::cast<llvm::StoreInst>(&*it)->getValueOperand());
          break;
        }
      }
    }
    if (!other) {
      return false;
    }
    std::optional<NativeStackAddress> expected = nativeStackAddressWithOffset(
        *slot, static_cast<int64_t>(sizeBytes));
    if (!expected) {
      return false;
    }
    std::optional<StackSlotKey> otherKey = comparableSlotKey(*function, *other);
    std::optional<StackSlotKey> expectedKey =
        comparableSlotKey(*function, *expected);
    return otherKey && expectedKey && *otherKey == *expectedKey;
  }

uint64_t Analyzer::valueSizeInBytes(llvm::Value *value) {
    llvm::Type *type = value != nullptr ? value->getType() : nullptr;
    if (type == nullptr || !type->isSized() ||
        !(type->isIntegerTy() || type->isFloatingPointTy() ||
          type->isPointerTy())) {
      return 0;
    }
    return type->getPrimitiveSizeInBits() / 8;
  }

void Analyzer::collectStackAdjustmentSlots(llvm::Function &function,
                                   std::set<StackSlotKey> &result) const {
    struct Candidate {
      StackSlotKey Slot;
      llvm::GlobalVariable *Register = nullptr;
      // store 正好写在当前栈顶（push 那一侧，判据 4）。
      bool StackTop = false;
    };
    std::vector<Candidate> candidates;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &inst : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        auto *loaded = llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand());
        if (loaded == nullptr) {
          continue;
        }
        RegisterAccess access = registerLoad(*loaded, Units);
        if (access.Unit == nullptr || isIgnored(*access.Unit, Options) ||
            access.Unit->Name == Abi.StackPointer ||
            Abi.Unaffected.count(access.Unit->Name) != 0) {
          continue;
        }
        std::optional<StackSlotKey> slot =
            fixedStackSlot(store->getPointerOperand(), *store);
        if (!slot) {
          continue;
        }
        const bool stackTop = isStackAdjustmentAccess(
            inst, store->getPointerOperand(),
            valueSizeInBytes(store->getValueOperand()), /*push=*/true);
        candidates.push_back({*slot, access.Unit->Global, stackTop});
      }
    }
    for (const Candidate &candidate : candidates) {
      if (result.count(candidate.Slot) != 0) {
        continue;
      }
      bool padding = true;
      bool sawLoad = false;
      for (llvm::BasicBlock &block : function) {
        for (llvm::Instruction &inst : block) {
          auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
          if (load == nullptr) {
            continue;
          }
          std::optional<StackSlotKey> slot =
              fixedStackSlot(load->getPointerOperand(), *load);
          if (!slot || !(*slot == candidate.Slot)) {
            continue;
          }
          sawLoad = true;
          const bool stackTopLoad = isStackAdjustmentAccess(
              *load, load->getPointerOperand(), valueSizeInBytes(load),
              /*push=*/false);
          for (llvm::User *user : load->users()) {
            auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
            if (store == nullptr) {
              padding = false;
              break;
            }
            RegisterAccess access = registerStore(*store, Units);
            if (access.Unit == nullptr ||
                access.Unit->Global == candidate.Register) {
              padding = false;
              break;
            }
            // 判据 3：普通保存槽，目的寄存器在下次写之前不能被显式读、也不能被
            // 调用消费。
            if (registerValueDiesAfter(*store, *access.Unit->Global)) {
              continue;
            }
            // 判据 4：push/pop 对（store 写在栈顶、load 也从栈顶读）里目的寄存器
            // 由编译器保证是死的，调用对它的消费不算真实使用。
            if (candidate.StackTop && stackTopLoad &&
                registerValueDiesAfter(*store, *access.Unit->Global,
                                       /*ignoreCallReads=*/true)) {
              continue;
            }
            padding = false;
            break;
          }
          if (!padding) {
            break;
          }
        }
        if (!padding) {
          break;
        }
      }
      if (padding && sawLoad) {
        result.insert(candidate.Slot);
      }
    }
  }

} // namespace notdec::bin2llvm
