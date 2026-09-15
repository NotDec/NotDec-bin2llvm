#include "notdec-bin2llvm/NativeDataImage.h"

#include "notdec-bin2llvm/NativeRelocationData.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

constexpr unsigned MaxAddressChainDepth = 8;

struct Region {
  uint64_t Start = 0;
  uint64_t Size = 0;
  llvm::GlobalVariable *Global = nullptr;

  bool contains(uint64_t address) const {
    return address >= Start && address - Start < Size;
  }
};

std::optional<uint64_t> constantUint64(const llvm::Value *value) {
  auto *constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getZExtValue();
}

// Fold an integer chain made only of constants.  This is used to recognize
// inttoptr operands that are statically known addresses; anything with a
// register/load input is left alone.
std::optional<uint64_t> foldConstantChain(const llvm::Value *value,
                                          unsigned depth = 0) {
  if (value == nullptr || depth > MaxAddressChainDepth) {
    return std::nullopt;
  }
  if (auto address = constantUint64(value)) {
    return address;
  }
  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr || op->getNumOperands() != 2) {
    return std::nullopt;
  }
  auto lhs = foldConstantChain(op->getOperand(0), depth + 1);
  auto rhs = foldConstantChain(op->getOperand(1), depth + 1);
  if (!lhs || !rhs) {
    return std::nullopt;
  }
  switch (op->getOpcode()) {
  case llvm::Instruction::Add:
    return *lhs + *rhs;
  case llvm::Instruction::Sub:
    return *lhs - *rhs;
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    return *lhs | *rhs;
  default:
    return std::nullopt;
  }
}

std::string segmentGlobalName(uint64_t address) {
  std::ostringstream os;
  os << "notdec.image.0x" << std::hex << address;
  return os.str();
}

std::string slotGlobalName(uint64_t address) {
  std::ostringstream os;
  os << "notdec.reloc.0x" << std::hex << address;
  return os.str();
}

// A dynamic addend must be an index, not another address space base.  TLS and
// the native stack frame reach inttoptr chains as register globals or stack
// addresses; combining one of those with a data address would silently
// redirect TLS/stack accesses into the image (for example the x86-64 stack
// guard access "add 40, FS_OFFSET" where 40 is also a genuine inttoptr target
// inside the ELF header segment).
bool dynamicAddendIsAddressBase(llvm::Value *value, unsigned depth = 0) {
  if (value == nullptr || depth > MaxAddressChainDepth) {
    return true;
  }
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
    llvm::Value *pointer = load->getPointerOperand()->stripPointerCasts();
    auto *global = llvm::dyn_cast<llvm::GlobalVariable>(pointer);
    return global != nullptr && global->hasMetadata("notdec.register");
  }
  if (auto *argument = llvm::dyn_cast<llvm::Argument>(value)) {
    llvm::StringRef name = argument->getName();
    for (llvm::StringRef prefix : {"RSP", "ESP", "RBP", "EBP"}) {
      if (name.starts_with(prefix)) {
        return true;
      }
    }
    return false;
  }
  if (llvm::isa<llvm::AllocaInst>(value)) {
    return true;
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (dynamicAddendIsAddressBase(incoming, depth + 1)) {
        return true;
      }
    }
    return false;
  }
  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return false;
  }
  switch (op->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::And:
  case llvm::Instruction::Select:
    for (const llvm::Use &operand : op->operands()) {
      if (dynamicAddendIsAddressBase(operand.get(), depth + 1)) {
        return true;
      }
    }
    return false;
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::PtrToInt:
    return dynamicAddendIsAddressBase(op->getOperand(0), depth + 1);
  case llvm::Instruction::GetElementPtr:
    // The addend is itself an address instead of an index.
    return true;
  default:
    return false;
  }
}

struct ResolvedAddress {
  const Region *Target = nullptr;
  int64_t Offset = 0;
  llvm::Value *Dynamic = nullptr;
  // True when the constant anchor of the chain is a frontend-confirmed data
  // address.  A dynamic access is only rewritten for confirmed anchors: small
  // integer constants (masks, stack offsets) can fall inside a segment by
  // accident, and treating those as bases would redirect real memory accesses.
  bool BaseKnown = false;
  bool Valid = false;
};

// Rewrites inttoptr address chains that provably point into a modeled segment.
class DataImageRewriter {
public:
  DataImageRewriter(llvm::Module &module, std::vector<Region> regions,
                    std::set<uint64_t> knownBases)
      : Module(module), Context(module.getContext()),
        PointerBits(module.getDataLayout().getPointerSizeInBits(0)),
        IntPtrType(llvm::IntegerType::get(Context, PointerBits)),
        ByteType(llvm::Type::getInt8Ty(Context)),
        Regions(std::move(regions)), KnownBases(std::move(knownBases)) {}

  void addFallbackSlot(uint64_t address, llvm::GlobalVariable *global) {
    FallbackSlots.emplace(address, global);
  }

  NativeDataImageSummary rewrite() {
    NativeDataImageSummary summary;
    rewriteInstructions(summary);
    rewriteGlobalInitializers(summary);
    return summary;
  }

private:
  const Region *findRegion(uint64_t address) const {
    for (const Region &region : Regions) {
      if (region.contains(address)) {
        return &region;
      }
    }
    return nullptr;
  }

  ResolvedAddress resolve(llvm::Value *value, unsigned depth) const {
    ResolvedAddress result;
    if (value == nullptr || depth > MaxAddressChainDepth) {
      return result;
    }
    if (auto address = constantUint64(value)) {
      const Region *region = findRegion(*address);
      if (region == nullptr) {
        return result;
      }
      result.Target = region;
      result.Offset = static_cast<int64_t>(*address - region->Start);
      result.BaseKnown = KnownBases.count(*address) != 0;
      result.Valid = true;
      return result;
    }

    auto *op = llvm::dyn_cast<llvm::Operator>(value);
    if (op == nullptr) {
      return result;
    }
    switch (op->getOpcode()) {
    case llvm::Instruction::IntToPtr:
      return resolve(op->getOperand(0), depth + 1);
    case llvm::Instruction::Add: {
      ResolvedAddress lhs = resolve(op->getOperand(0), depth + 1);
      ResolvedAddress rhs = resolve(op->getOperand(1), depth + 1);
      if (!lhs.Valid && rhs.Valid && !rhs.Dynamic) {
        rhs.Dynamic = op->getOperand(0);
        return rhs;
      }
      if (lhs.Valid && !rhs.Valid && !lhs.Dynamic) {
        lhs.Dynamic = op->getOperand(1);
        return lhs;
      }
      if (!lhs.Valid || !rhs.Valid || lhs.Target != rhs.Target ||
          (lhs.Dynamic != nullptr && rhs.Dynamic != nullptr)) {
        return result;
      }
      result.Target = lhs.Target;
      result.Offset = lhs.Offset + rhs.Offset;
      result.Dynamic = lhs.Dynamic != nullptr ? lhs.Dynamic : rhs.Dynamic;
      result.BaseKnown = lhs.BaseKnown || rhs.BaseKnown;
      result.Valid = true;
      return result;
    }
    case llvm::Instruction::Sub: {
      ResolvedAddress lhs = resolve(op->getOperand(0), depth + 1);
      ResolvedAddress rhs = resolve(op->getOperand(1), depth + 1);
      if (lhs.Valid && rhs.Valid && lhs.Target == rhs.Target &&
          lhs.Dynamic == nullptr && rhs.Dynamic == nullptr) {
        result.Target = lhs.Target;
        result.Offset = lhs.Offset - rhs.Offset;
        result.BaseKnown = lhs.BaseKnown || rhs.BaseKnown;
        result.Valid = true;
      }
      return result;
    }
    case llvm::Instruction::Or: {
      for (unsigned index = 0; index < 2; ++index) {
        auto zero = constantUint64(op->getOperand(index));
        if (zero && *zero == 0) {
          return resolve(op->getOperand(1 - index), depth + 1);
        }
      }
      return result;
    }
    case llvm::Instruction::ZExt:
      return resolve(op->getOperand(0), depth + 1);
    case llvm::Instruction::SExt: {
      // A dynamic sign-extended index would need signed scaling in the GEP;
      // keep those raw instead of guessing.
      ResolvedAddress inner = resolve(op->getOperand(0), depth + 1);
      if (inner.Valid && inner.Dynamic == nullptr) {
        return inner;
      }
      return result;
    }
    default:
      return result;
    }
  }

  llvm::Value *pointerFor(const ResolvedAddress &resolved,
                          llvm::Instruction *insertBefore) const {
    auto *offset = llvm::ConstantInt::get(
        IntPtrType, static_cast<uint64_t>(resolved.Offset));
    if (resolved.Dynamic == nullptr) {
      return llvm::ConstantExpr::getGetElementPtr(
          ByteType, resolved.Target->Global, {offset});
    }
    // A two-index GEP would need an aggregate source element type, so keep the
    // constant segment offset in its own GEP and add the dynamic index on top.
    llvm::Constant *base = llvm::ConstantExpr::getGetElementPtr(
        ByteType, resolved.Target->Global, {offset});
    llvm::IRBuilder<> builder(insertBefore);
    llvm::Value *index = resolved.Dynamic;
    if (index->getType() != IntPtrType) {
      auto *indexType = llvm::dyn_cast<llvm::IntegerType>(index->getType());
      if (indexType == nullptr) {
        return base;
      }
      if (indexType->getBitWidth() < PointerBits) {
        index = builder.CreateZExt(index, IntPtrType, "notdec.image.index");
      } else {
        index = builder.CreateTrunc(index, IntPtrType, "notdec.image.index");
      }
    }
    return builder.CreateGEP(ByteType, base, {index}, "notdec.image.ptr");
  }

  llvm::Constant *staticPointerFor(uint64_t address,
                                   NativeDataImageSummary &summary) {
    if (const Region *region = findRegion(address)) {
      ++summary.StaticAccesses;
      return llvm::ConstantExpr::getGetElementPtr(
          ByteType, region->Global,
          {llvm::ConstantInt::get(IntPtrType, address - region->Start)});
    }
    auto slot = FallbackSlots.find(address);
    if (slot != FallbackSlots.end()) {
      ++summary.StaticAccesses;
      return slot->second;
    }
    return nullptr;
  }

  void rewriteIntToPtr(llvm::IntToPtrInst &instruction,
                       NativeDataImageSummary &summary) {
    ResolvedAddress resolved = resolve(instruction.getOperand(0), 0);
    if (!resolved.Valid ||
        (resolved.Dynamic != nullptr &&
         (!resolved.BaseKnown ||
          dynamicAddendIsAddressBase(resolved.Dynamic)))) {
      ++summary.UnresolvedIntToPtrs;
      return;
    }
    llvm::Value *pointer = pointerFor(resolved, &instruction);
    if (resolved.Dynamic != nullptr) {
      ++summary.BaseOffsetAccesses;
    } else {
      ++summary.StaticAccesses;
    }
    instruction.replaceAllUsesWith(pointer);
    instruction.eraseFromParent();
  }

  llvm::Constant *rewriteConstant(llvm::Constant *constant,
                                  NativeDataImageSummary &summary) {
    auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(constant);
    if (expression == nullptr) {
      return constant;
    }
    if (expression->getOpcode() == llvm::Instruction::IntToPtr) {
      auto address = foldConstantChain(expression->getOperand(0));
      if (!address) {
        return constant;
      }
      if (llvm::Constant *pointer = staticPointerFor(*address, summary)) {
        return pointer;
      }
      return constant;
    }
    llvm::SmallVector<llvm::Constant *, 4> operands;
    bool changed = false;
    for (const llvm::Use &operand : expression->operands()) {
      auto *constantOperand = llvm::dyn_cast<llvm::Constant>(operand.get());
      if (constantOperand == nullptr) {
        return constant;
      }
      llvm::Constant *rewritten = rewriteConstant(constantOperand, summary);
      operands.push_back(rewritten);
      changed |= rewritten != constantOperand;
    }
    if (!changed) {
      return constant;
    }
    return expression->getWithOperands(operands);
  }

  void rewriteInstructions(NativeDataImageSummary &summary) {
    std::vector<llvm::IntToPtrInst *> casts;
    for (llvm::Function &function : Module) {
      if (function.isDeclaration()) {
        continue;
      }
      for (llvm::Instruction &instruction : llvm::instructions(function)) {
        if (auto *cast = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction)) {
          casts.push_back(cast);
          continue;
        }
        for (unsigned index = 0; index < instruction.getNumOperands();
             ++index) {
          auto *constant =
              llvm::dyn_cast<llvm::Constant>(instruction.getOperand(index));
          if (constant == nullptr) {
            continue;
          }
          llvm::Constant *rewritten = rewriteConstant(constant, summary);
          if (rewritten != constant) {
            instruction.setOperand(index, rewritten);
          }
        }
      }
    }
    for (llvm::IntToPtrInst *cast : casts) {
      rewriteIntToPtr(*cast, summary);
    }
  }

  void rewriteGlobalInitializers(NativeDataImageSummary &summary) {
    for (llvm::GlobalVariable &global : Module.globals()) {
      if (!global.hasInitializer()) {
        continue;
      }
      llvm::Constant *initializer = global.getInitializer();
      llvm::Constant *rewritten = rewriteConstant(initializer, summary);
      if (rewritten != initializer) {
        global.setInitializer(rewritten);
      }
    }
  }

  llvm::Module &Module;
  llvm::LLVMContext &Context;
  unsigned PointerBits;
  llvm::IntegerType *IntPtrType;
  llvm::Type *ByteType;
  std::vector<Region> Regions;
  std::set<uint64_t> KnownBases;
  std::map<uint64_t, llvm::GlobalVariable *> FallbackSlots;
};

std::vector<NativeDataImageFunctionSlot>
normalizeSlots(llvm::ArrayRef<NativeDataImageFunctionSlot> slots) {
  std::vector<NativeDataImageFunctionSlot> normalized;
  for (const NativeDataImageFunctionSlot &slot : slots) {
    if (slot.FunctionName.empty()) {
      continue;
    }
    if (slot.Width != 1 && slot.Width != 2 && slot.Width != 4 &&
        slot.Width != 8) {
      continue;
    }
    normalized.push_back(slot);
  }
  std::sort(normalized.begin(), normalized.end(),
            [](const NativeDataImageFunctionSlot &lhs,
               const NativeDataImageFunctionSlot &rhs) {
              return lhs.Address < rhs.Address;
            });
  normalized.erase(
      std::unique(normalized.begin(), normalized.end(),
                  [](const NativeDataImageFunctionSlot &lhs,
                     const NativeDataImageFunctionSlot &rhs) {
                    return lhs.Address == rhs.Address;
                  }),
      normalized.end());
  return normalized;
}

// Collect statically known inttoptr targets in the module.  A direct
// inttoptr(C) is by construction an address use, so C can anchor base+offset
// accesses of the same object.
void collectModuleAddresses(llvm::Module &module, std::set<uint64_t> &out) {
  auto addFromOperand = [&](const llvm::Value *operand) {
    if (auto address = foldConstantChain(operand)) {
      out.insert(*address);
    }
  };
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      if (auto *cast = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction)) {
        addFromOperand(cast->getOperand(0));
      }
      for (const llvm::Use &operand : instruction.operands()) {
        auto *constant = llvm::dyn_cast<llvm::Constant>(operand.get());
        auto *expression =
            llvm::dyn_cast_or_null<llvm::ConstantExpr>(constant);
        if (expression != nullptr &&
            expression->getOpcode() == llvm::Instruction::IntToPtr) {
          addFromOperand(expression->getOperand(0));
        }
      }
    }
  }
  for (llvm::GlobalVariable &global : module.globals()) {
    if (!global.hasInitializer()) {
      continue;
    }
    auto *expression =
        llvm::dyn_cast<llvm::ConstantExpr>(global.getInitializer());
    if (expression != nullptr &&
        expression->getOpcode() == llvm::Instruction::IntToPtr) {
      addFromOperand(expression->getOperand(0));
    }
  }
}

// Build <{ [k x i8], iW, ... }> for a segment that has symbolic fields.  Packed
// layout keeps every field at its virtual-address offset, so the byte image and
// the field accesses see the same memory.
llvm::Constant *buildSegmentInitializer(
    llvm::Module &module, const NativeDataImageSegment &segment,
    llvm::ArrayRef<NativeDataImageFunctionSlot> slots, llvm::Type *&type,
    uint64_t &symbolized) {
  llvm::LLVMContext &context = module.getContext();
  std::vector<uint8_t> bytes(segment.Size, 0);
  size_t copied = std::min<size_t>(segment.Bytes.size(), bytes.size());
  std::copy_n(segment.Bytes.begin(), copied, bytes.begin());
  if (slots.empty()) {
    type = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), bytes.size());
    return llvm::ConstantDataArray::get(context, bytes);
  }

  std::vector<llvm::Type *> fields;
  std::vector<llvm::Constant *> values;
  uint64_t cursor = 0;

  auto appendBytes = [&](uint64_t end) {
    if (end <= cursor) {
      return;
    }
    auto *arrayType =
        llvm::ArrayType::get(llvm::Type::getInt8Ty(context), end - cursor);
    fields.push_back(arrayType);
    values.push_back(llvm::ConstantDataArray::get(
        context, llvm::ArrayRef<uint8_t>(bytes).slice(cursor, end - cursor)));
    cursor = end;
  };

  for (const NativeDataImageFunctionSlot &slot : slots) {
    uint64_t offset = slot.Address - segment.Address;
    if (offset < cursor || offset + slot.Width > bytes.size()) {
      continue;
    }
    llvm::Function *function = module.getFunction(slot.FunctionName);
    if (function == nullptr) {
      continue;
    }
    appendBytes(offset);
    auto *fieldType =
        llvm::IntegerType::get(context, static_cast<unsigned>(slot.Width * 8));
    fields.push_back(fieldType);
    values.push_back(llvm::ConstantExpr::getPtrToInt(function, fieldType));
    cursor = offset + slot.Width;
    ++symbolized;
  }
  appendBytes(bytes.size());

  if (fields.empty()) {
    type = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), bytes.size());
    return llvm::ConstantDataArray::get(context, bytes);
  }
  auto *structType = llvm::StructType::get(context, fields, /*isPacked=*/true);
  type = structType;
  return llvm::ConstantStruct::get(structType, values);
}

} // namespace

NativeDataImageSummary materializeNativeDataImage(
    llvm::Module &module, llvm::ArrayRef<NativeDataImageSegment> segments,
    llvm::ArrayRef<NativeDataImageFunctionSlot> slots,
    llvm::ArrayRef<uint64_t> knownAddressBases,
    const NativeDataImageOptions &options) {
  NativeDataImageSummary summary;
  if (segments.empty()) {
    return summary;
  }

  std::vector<NativeDataImageFunctionSlot> normalized = normalizeSlots(slots);
  std::set<uint64_t> knownBases(knownAddressBases.begin(),
                                knownAddressBases.end());
  collectModuleAddresses(module, knownBases);
  for (const NativeDataImageFunctionSlot &slot : normalized) {
    knownBases.insert(slot.Address);
  }

  auto segmentOf = [&](uint64_t address) -> const NativeDataImageSegment * {
    for (const NativeDataImageSegment &segment : segments) {
      if (!segment.Executable && address >= segment.Address &&
          address - segment.Address < segment.Size) {
        return &segment;
      }
    }
    return nullptr;
  };

  // Only segments that hold a confirmed address need a global; this keeps
  // unreferenced header ranges out of the module.
  std::set<const NativeDataImageSegment *> referencedSegments;
  for (uint64_t base : knownBases) {
    if (const NativeDataImageSegment *segment = segmentOf(base)) {
      referencedSegments.insert(segment);
    }
  }

  std::vector<Region> regions;
  for (const NativeDataImageSegment &segment : segments) {
    if (segment.Executable || segment.Size == 0) {
      continue;
    }
    if (options.MaxSegmentSize != 0 &&
        segment.Size > options.MaxSegmentSize) {
      ++summary.SegmentsSkipped;
      continue;
    }
    if (referencedSegments.count(&segment) == 0) {
      continue;
    }
    if (std::any_of(regions.begin(), regions.end(),
                    [&](const Region &region) {
                      return segment.Address < region.Start + region.Size &&
                             region.Start < segment.Address + segment.Size;
                    })) {
      ++summary.SegmentsSkipped;
      continue;
    }
    std::string name = segmentGlobalName(segment.Address);
    if (module.getNamedGlobal(name) != nullptr) {
      ++summary.SegmentsSkipped;
      continue;
    }

    std::vector<NativeDataImageFunctionSlot> segmentSlots;
    for (const NativeDataImageFunctionSlot &slot : normalized) {
      if (slot.Address >= segment.Address &&
          slot.Address - segment.Address + slot.Width <= segment.Size) {
        segmentSlots.push_back(slot);
      }
    }

    llvm::Type *type = nullptr;
    uint64_t symbolized = 0;
    llvm::Constant *initializer = buildSegmentInitializer(
        module, segment, segmentSlots, type, symbolized);
    if (type == nullptr || initializer == nullptr) {
      ++summary.SegmentsSkipped;
      continue;
    }
    auto *global = new llvm::GlobalVariable(
        module, type, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
        initializer, name);
    global->setAlignment(llvm::Align(1));
    summary.FunctionSlots += symbolized;
    ++summary.SegmentsCreated;

    Region region;
    region.Start = segment.Address;
    region.Size = segment.Size;
    region.Global = global;
    regions.push_back(region);
  }

  // Slots outside every modeled segment keep the previous standalone global so
  // the earlier promotion capability does not regress.
  std::vector<NativeFunctionPointerSlot> fallbackSlots;
  for (const NativeDataImageFunctionSlot &slot : normalized) {
    bool covered = std::any_of(
        regions.begin(), regions.end(),
        [&](const Region &region) { return region.contains(slot.Address); });
    if (!covered) {
      fallbackSlots.push_back({slot.Address, slot.Width, slot.FunctionName});
    }
  }
  if (!fallbackSlots.empty()) {
    materializeNativeFunctionPointerSlots(module, fallbackSlots);
    summary.FallbackSlots = fallbackSlots.size();
  }

  if (regions.empty()) {
    return summary;
  }

  DataImageRewriter rewriter(module, std::move(regions), knownBases);
  for (const NativeFunctionPointerSlot &slot : fallbackSlots) {
    if (llvm::GlobalVariable *fallback =
            module.getNamedGlobal(slotGlobalName(slot.Address))) {
      rewriter.addFallbackSlot(slot.Address, fallback);
    }
  }
  NativeDataImageSummary rewriteSummary = rewriter.rewrite();
  summary.StaticAccesses += rewriteSummary.StaticAccesses;
  summary.BaseOffsetAccesses += rewriteSummary.BaseOffsetAccesses;
  summary.UnresolvedIntToPtrs += rewriteSummary.UnresolvedIntToPtrs;
  return summary;
}

} // namespace notdec::bin2llvm
