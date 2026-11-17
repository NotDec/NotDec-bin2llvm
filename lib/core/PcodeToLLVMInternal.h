#include "notdec-bin2llvm/core/PcodeToLLVM.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/core/RegisterStorage.h"

#include "core/NotDecIntrinsics.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>


namespace notdec::bin2llvm {

  enum class X87WindowKind {
    // Library-managed instruction: FPU environment (fnstsw/fnstcw/fldcw/
    // fstenv/fldenv) and fprem.  The library owns the FPU environment and the
    // ST2..ST7 state; see lowerX87Library.
    Library,
    // fld/fild/fldz/fld1/fld %st(i): push a value onto the x87 stack.
    Push,
    // fstp/fistp to memory: store ST0 (with conversion), then pop.
    PopStore,
    // fstp %st(i): store ST0 into slot i, then pop.
    PopSlot,
    // fst %st(i): store ST0 into slot i without popping.
    StoreSlot,
    // faddp/fsubp/fmulp/fdivp/fsubrp/fdivrp on st(1).
    PopArith,
    // Same with an explicit st(i) slot (i>=2).
    PopArithSlot,
    // fadd/fsub/fmul/fdiv/fsubr/fdivr between ST0 and st(i).
    RegArith,
    // Same between ST0 and a memory operand.
    MemArith,
    // fsqrt/fabs/fchs.
    Unary,
    // fcomi/fcomip/fucomi/fucomip: compare ST0 with st(i) into EFLAGS.
    Compare,
    // fxch %st(i): swap ST0 and st(i).
    Fxch,
  };

  struct X87WindowSpec {
    size_t Start = 0;
    size_t End = 0;
    X87WindowKind Kind = X87WindowKind::Library;

    // Library intrinsics (environment instructions, fprem).
    std::string IntrinsicName;
    std::vector<VarnodeView> Args;
    std::vector<llvm::Type *> ArgTypes;
    llvm::Type *ResultType = nullptr;
    // fnstsw/fnstcw: write the returned word to a register (AX) or memory.
    std::optional<VarnodeView> ResultRegister;
    // fldenv: the library reads the FPU environment image from the pointer
    // argument itself, so the group's memory LOADs must not be lowered.
    bool SuppressLoads = false;
    // fstenv/fldenv: the intrinsic takes a memory pointer and touches normal
    // memory, unlike the register-value intrinsics.
    bool AccessesMemory = false;
    // fprem: the library call result becomes the new ST0.
    bool StoreResultToST0 = false;

    // fcomi/fucomip: write EFLAGS CF/PF/ZF from the comparison and clear
    // AF/SF/OF; the FPU status word stays library-internal.
    bool WritesComparisonFlags = false;
    // fucomi/fucomip vs fcomi/fcomip.  Both set the same flags (PF on
    // unordered, ZF/CF include the unordered case), matching the x86.sinc
    // fcomi macro; the difference is only the invalid-operation exception.
    bool Unordered = false;

    // st(i) slot operand for Push(fld %st(i))/PopSlot/StoreSlot/PopArithSlot/
    // RegArith/Compare/Fxch.
    uint32_t SlotIndex = 0;
    // fstp/fistp memory store target.
    std::optional<VarnodeView> StoreAddress;
    std::optional<uint32_t> StoreValueSize;
    // fild/fistp integer conversion.
    bool IsInt = false;
    // Memory operand for Push/MemArith: the raw LOAD result varnode.
    std::optional<VarnodeView> MemValue;
    llvm::Type *MemValueType = nullptr;
    // fldz/fld1 constant.
    llvm::ConstantFP *PushConstant = nullptr;
    // faddp-family binary opcode (FloatAdd/FloatSub/FloatMult/FloatDiv).
    PcodeOpcode ArithOpcode = PcodeOpcode::FloatAdd;
    // Whether the p-code arithmetic op has ST0 as input0; the other input is
    // the st(i) slot or the memory operand.  The p-code operand order is the
    // semantic operand order (e.g. FSUBP is FLOAT_SUB(ST1, ST0), FSUBRP is
    // FLOAT_SUB(ST0, ST1)), so no separate reverse flag is needed.
    bool ST0IsInput0 = true;
    // Unary kind for Unary.
    enum class UnaryKind { Sqrt, Abs, Chs } Unary = UnaryKind::Sqrt;
    // fcomip/fucomip/fstp: pop after the operation.
    bool Pops = false;
  };

  struct X86CallStackSpec {
    const char *ArchName = "";
    const char *StackPointerRegister = "";
    uint32_t PointerSize = 0;
  };

  struct RegisterRead {
    llvm::Value *Value = nullptr;
    RegisterAccess Access;
  };

std::string varnodeKey(const VarnodeView &varnode);

std::string valueName(const VarnodeView &varnode);

unsigned bitWidth(uint32_t byteSize);

std::optional<uint32_t>
inferPointerByteSizeFromRegisters(const std::vector<RegisterInfo> &registers);

void setModuleDataLayoutFromPointerSize(llvm::Module &module,
                                        uint32_t pointerByteSize,
                                        bool isBigEndian);

bool isX87StackVarnode(const VarnodeView &varnode);

uint32_t x87StackIndex(const VarnodeView &varnode);

bool isFpuEnvVarnode(const VarnodeView &varnode);

bool touchesX87Stack(const PcodeProgram &program, size_t start, size_t end);

std::vector<RegisterInfo>
x87WindowRegisters(const std::vector<RegisterInfo> &registers);

VarnodeView x87StackVarnode(const std::vector<RegisterInfo> &registers,
                            uint32_t index);

bool isFloatBinaryOpcode(PcodeOpcode opcode);

bool isSseFloatWriteMnemonic(llvm::StringRef mnemonic);

std::string fpSuffix(llvm::Type *type);

std::string popArithName(PcodeOpcode opcode, const VarnodeView &input0);

std::string memArithName(PcodeOpcode opcode, bool memIsInput0);

std::string regArithName(PcodeOpcode opcode, const VarnodeView &input0);

class PcodeLowerer {
public:
  PcodeLowerer(llvm::LLVMContext &context, llvm::Module &module, llvm::Function &function, llvm::IRBuilder<> &builder, const PcodeLoweringConfig &config);
  bool lower(const PcodeProgram &program, std::string &errorMessage);

  void narrowMaskedRegisterReads();

  bool usesNativeCfg() const;

  std::vector<std::pair<uint64_t, uint64_t>> sortedNativeRanges() const;

  bool nativeRangesCoverAddress(uint64_t address) const;

  std::optional<std::pair<uint64_t, uint64_t>>
  nativeRangeForAddress(uint64_t address) const;

  llvm::BasicBlock *entryBlockForProgram(std::string &errorMessage);

  static bool isTerminator(PcodeOpcode opcode);

  static bool sameVarnode(const VarnodeView &lhs, const VarnodeView &rhs);

  static bool isConstValue(const VarnodeView &varnode, uint64_t value,
                           uint32_t size);

  static bool isRegisterNamed(const VarnodeView &varnode,
                              const char *registerName, uint32_t size);

  static std::optional<X86CallStackSpec>
  x86CallStackSpec(const PcodeProgram &program);

  static bool isX86StackPointer(const VarnodeView &varnode,
                                const X86CallStackSpec &spec);

  static bool isX86StackPointerAdjust(const PcodeOpView &op,
                                      PcodeOpcode opcode,
                                      const X86CallStackSpec &spec);

  static bool isX86ReturnAddressStore(const PcodeOpView &op,
                                      const VarnodeView &stackPointer,
                                      uint64_t fallthrough,
                                      const X86CallStackSpec &spec);

  static bool isX86ReturnAddressLoad(const PcodeOpView &op,
                                     const VarnodeView &returnTarget,
                                     const X86CallStackSpec &spec);

  bool suppressX86CallStackEffect(size_t start, size_t end,
                                  const X86CallStackSpec &spec);

  bool suppressX86ReturnStackEffect(size_t start, size_t end,
                                    const X86CallStackSpec &spec);

  void warnX86StackPatternMiss(uint64_t address, const char *kind,
                               const X86CallStackSpec &spec);

  void prepareX86CallReturnStackSuppression(const PcodeProgram &program);

  void prepareX86PcThunkSuppression(const PcodeProgram &program);

  static const PcodeOpView *findProducer(const PcodeProgram &program,
                                         size_t start, size_t end,
                                         const VarnodeView &target);

  std::optional<X87WindowSpec>
  classifyX87Intrinsic(const PcodeProgram &program, size_t start,
                       size_t end);

  void prepareSseFloatWriteMarkers(const PcodeProgram &program);

  void prepareX87Window(const PcodeProgram &program);

  static const PcodeOpView *
  findStackWrite(const PcodeProgram &program, size_t start, size_t end,
                 PcodeOpcode opcode, int outputIndex);

  static std::optional<uint32_t>
  stIndexFromSt0Copy(const PcodeProgram &program, size_t start, size_t end);

  static std::optional<uint32_t>
  stIndexFromSt0WriteCopy(const PcodeProgram &program, size_t start,
                          size_t end);

  static VarnodeView stIndexVarnode(uint32_t index);

  std::optional<X87WindowSpec>
  classifyX87ByMnemonic(const PcodeProgram &program, size_t start,
                        size_t end, const std::string &mnemonic);

  std::optional<X87WindowSpec>
  classifyX87ByShape(const PcodeProgram &program, size_t start, size_t end);

  llvm::Value *toIntrinsicArg(llvm::Value *value, llvm::Type *targetType);

  llvm::Type *x87Fp80();

  llvm::Value *readX87ST(uint32_t index, bool windowShift = false);

  void writeX87ST(uint32_t index, llvm::Value *value);

  void x87WindowPush(llvm::Value *value);

  void x87WindowPop();

  void x87PopIntoST1();

  llvm::Value *x87Peek(uint32_t index);

  void x87Poke(uint32_t index, llvm::Value *value);

  llvm::Value *readX87Slot(uint32_t index);

  llvm::Value *x87FloatArith(PcodeOpcode opcode, llvm::Value *lhs,
                             llvm::Value *rhs);

  bool lowerX87Library(const X87WindowSpec &spec, std::string &errorMessage);

  bool lowerX87Push(const X87WindowSpec &spec, std::string &errorMessage);

  bool lowerX87PopStore(const X87WindowSpec &spec,
                        std::string &errorMessage);

  bool lowerX87PopSlot(const X87WindowSpec &spec, std::string &errorMessage);

  bool lowerX87StoreSlot(const X87WindowSpec &spec,
                         std::string &errorMessage);

  bool lowerX87PopArith(const X87WindowSpec &spec,
                        std::string &errorMessage);

  bool lowerX87RegArith(const X87WindowSpec &spec,
                        std::string &errorMessage);

  bool lowerX87MemArith(const X87WindowSpec &spec,
                        std::string &errorMessage);

  bool lowerX87Unary(const X87WindowSpec &spec, std::string &errorMessage);

  bool lowerX87Compare(const X87WindowSpec &spec,
                       std::string &errorMessage);

  bool lowerX87Fxch(const X87WindowSpec &spec, std::string &errorMessage);

  std::optional<int> x87BlockDepthDelta(size_t blockIndex) const;

  bool computeX87BlockDepths(const PcodeProgram &program,
                             llvm::BasicBlock &blockForEntry);

  bool lowerX87Window(const X87WindowSpec &spec, std::string &errorMessage);

  std::string blockName(uint64_t address);

  std::string addressFunctionName(uint64_t address);

  std::optional<uint64_t> directTarget(const PcodeOpView &op,
                                       size_t inputIndex);

  std::optional<size_t> relativeTargetIndex(size_t opIndex,
                                            const PcodeOpView &op,
                                            size_t inputIndex,
                                            size_t opCount);

  int64_t relativeBranchOffset(const VarnodeView &target);

  void addBlockStart(std::set<size_t> &starts,
                     const std::map<uint64_t, size_t> &firstOpForAddress,
                     uint64_t address);

  void addNativeBlockStart(std::set<size_t> &starts, size_t start,
                           uint64_t blockAddress, uint64_t blockEnd,
                           bool internalPcodeBlock = false);

  bool buildBasicBlocks(const PcodeProgram &program,
                        std::string &errorMessage);

  std::optional<size_t> blockEndForStart(const PcodeProgram &program,
                                         size_t blockIndex,
                                         std::string &errorMessage);

  uint64_t blockAddressForStart(size_t start) const;

  uint64_t blockAddressForIndex(size_t blockIndex) const;

  uint64_t parentBlockAddressForIndex(size_t blockIndex) const;

  const std::vector<uint64_t> *
  nativeSuccessorsForBlockIndex(size_t blockIndex,
                                uint64_t &factBlockAddress) const;

  bool isInternalPcodeBlock(size_t blockIndex) const;

  llvm::BasicBlock *nextBlock(size_t blockIndex);

  llvm::BasicBlock *internalPcodeContinuation(size_t blockIndex,
                                              size_t opIndex);

  bool nativeFallthroughBlock(size_t blockIndex, llvm::BasicBlock *&result,
                              std::string &errorMessage);

  bool nativeConditionalFalseBlock(size_t blockIndex, uint64_t trueTarget,
                                   llvm::BasicBlock *fallback,
                                   llvm::BasicBlock *&result,
                                   std::string &errorMessage);

  bool nativeEmptyBlockSuccessor(uint64_t blockAddress,
                                 llvm::BasicBlock *&result,
                                 std::string &errorMessage);

  bool nativeIndirectBranchSuccessor(
      size_t blockIndex,
      std::vector<std::pair<uint64_t, llvm::BasicBlock *>> &result,
      std::string &errorMessage);

  llvm::BasicBlock *blockForTarget(uint64_t address);

  llvm::BasicBlock *blockForNativeTarget(uint64_t address,
                                         std::string &errorMessage);

  // SLEIGH 把 CMOV 一类指令的内部跳转目标写成“指令结束地址”。如果该地址
  // 后面到块尾的字节不再产生任何 p-code（典型是 nopl 填充），discovery 注册
  // 不出内部块，lowering 会误报 missing native block。这里把这类目标归一到
  // 当前 native block 唯一的块尾后继，语义就是“继续执行下一条指令”。
  std::optional<uint64_t> resolveOrphanBranchTarget(size_t opIndex,
                                                    uint64_t target) const;

  bool isInternalPcodeTarget(size_t blockIndex, llvm::BasicBlock *targetBlock);

  bool nativeDirectBranchTarget(size_t blockIndex, uint64_t target,
                                llvm::BasicBlock *targetBlock,
                                std::string &errorMessage);

  bool nativeConditionalTrueTarget(size_t blockIndex, uint64_t target,
                                   llvm::BasicBlock *targetBlock,
                                   std::string &errorMessage);

  llvm::BasicBlock *
  tailJumpBlockForKnownFunction(uint64_t address,
                                const std::string &calleeName);

  llvm::BasicBlock *blockForRelativeTarget(size_t opIndex,
                                           const PcodeOpView &op,
                                           size_t inputIndex,
                                           std::string &errorMessage);

  llvm::BasicBlock *exitBlock();

  llvm::BasicBlock *nativeIndirectSwitchDefaultBlock(uint64_t blockAddress);

  llvm::Value *asCondition(llvm::Value *value);

  bool lowerTerminator(size_t blockIndex, size_t opIndex, const PcodeOpView &op,
                       llvm::BasicBlock *fallthrough,
                       std::string &errorMessage);

  llvm::Type *intType(uint32_t byteSize);

  llvm::Type *floatType(uint32_t byteSize);

  uint32_t floatByteSize(llvm::Type *type) const;

  llvm::Value *readFloatBits(const VarnodeView &varnode, llvm::Type *floatTy);

  void writeFloatBits(const VarnodeView &varnode, llvm::Value *value);

  llvm::Value *resize(llvm::Value *value, uint32_t byteSize);

  uint32_t pointerByteSize() const;

  llvm::Value *read(const VarnodeView &varnode);

  std::optional<uint64_t> sourceRam(const VarnodeView &varnode) const;

  llvm::Function *functionForCodeAddress(uint64_t address);

  void setSourceRam(const VarnodeView &varnode, std::optional<uint64_t> source);

  void write(const VarnodeView &varnode, llvm::Value *value);

  llvm::Value *visibleCachedValue(const VarnodeView &varnode,
                                  llvm::Value *value);

  bool instructionDefBlockIsDirectPred(llvm::Instruction &instruction,
                                       llvm::BasicBlock &block);

  bool instructionDominatesBlock(llvm::Instruction &instruction,
                                 llvm::BasicBlock &block);

  llvm::Value *unknownValueAtEnd(llvm::BasicBlock &block, llvm::Type *type,
                                 llvm::Twine name);

  bool requireInputCount(const PcodeOpView &op, size_t count,
                         std::string &errorMessage);

  bool lowerBinary(const PcodeOpView &op, std::string &errorMessage);

  bool lowerCompare(const PcodeOpView &op, std::string &errorMessage);

  bool lowerCast(const PcodeOpView &op, std::string &errorMessage);

  bool lowerUnary(const PcodeOpView &op, std::string &errorMessage);

  bool lowerSubpiece(const PcodeOpView &op, std::string &errorMessage);

  bool lowerPiece(const PcodeOpView &op, std::string &errorMessage);

  bool lowerCountBits(const PcodeOpView &op, std::string &errorMessage);

  bool lowerCopyLike(const PcodeOpView &op, std::string &errorMessage);

  bool lowerPtrAdd(const PcodeOpView &op, std::string &errorMessage);

  bool lowerPtrSub(const PcodeOpView &op, std::string &errorMessage);

  bool lowerSignedBorrow(const PcodeOpView &op, std::string &errorMessage);

  bool lowerAddOverflow(const PcodeOpView &op, std::string &errorMessage);

  bool lowerBoolNegate(const PcodeOpView &op, std::string &errorMessage);

  bool lowerBoolBinary(const PcodeOpView &op, std::string &errorMessage);

  bool lowerFloatBinary(const PcodeOpView &op, std::string &errorMessage);

  bool lowerFloatCompare(const PcodeOpView &op, std::string &errorMessage);

  bool lowerFloatNan(const PcodeOpView &op, std::string &errorMessage);

  bool lowerFloatUnary(const PcodeOpView &op, std::string &errorMessage);

  bool lowerFloatCast(const PcodeOpView &op, std::string &errorMessage);

  llvm::GlobalVariable *memoryGlobal();

  llvm::Value *memoryPointer(llvm::Value *address);

  bool requireConstSpaceSelector(const PcodeOpView &op,
                                 std::string &errorMessage);

  bool lowerLoad(const PcodeOpView &op, std::string &errorMessage);

  bool lowerStore(const PcodeOpView &op, std::string &errorMessage);

  bool lowerHelperCall(const PcodeOpView &op, std::string &errorMessage);

  llvm::CallInst *lowerKnownVoidCall(const std::string &calleeName);

  llvm::CallInst *createUnknownVoidIndirectCall(const VarnodeView &target);

  bool lowerUnknownVoidIndirectCall(const VarnodeView &target);

  bool lowerUnknownVoidIndirectTailJump(const VarnodeView &target);

  bool lowerKnownVoidTailJump(const std::string &calleeName);

  bool lowerCall(const PcodeOpView &op, std::string &errorMessage);

  bool lowerCallInd(const PcodeOpView &op, std::string &errorMessage);

  bool lowerX86Movmskpd(const PcodeOpView &op, std::string &errorMessage);

  std::string userOpName(uint64_t index) const;

  bool isUserOp(const PcodeOpView &op, llvm::StringRef name,
                uint64_t index) const;

  bool lowerUnknownUserOp(const PcodeOpView &op, std::string &errorMessage);

  void warnUnsupportedUserOp(uint64_t index, const std::string &name,
                             uint64_t address, const std::string &helperName);

  bool lowerCallOther(const PcodeOpView &op, std::string &errorMessage);

  bool lowerOp(const PcodeOpView &op, std::string &errorMessage);

private:
  llvm::LLVMContext &Context;
  llvm::Module &Module;
  llvm::Function &Function;
  llvm::IRBuilder<> &Builder;
  const PcodeLoweringConfig &Config;
  std::unordered_map<std::string, llvm::Value *> Values;
  std::map<std::pair<std::string, llvm::BasicBlock *>, llvm::Value *>
      NonDominatingReadValues;
  std::unordered_map<std::string, uint64_t> SourceRamByVarnode;
  std::vector<size_t> BlockStarts;
  std::vector<size_t> BlockEnds;
  // 当前正在 lower 的 native block 地址，只用于把 CFG 失败定位到来源块。
  std::optional<uint64_t> CurrentNativeBlockAddress;
  std::unordered_map<size_t, uint64_t> NativeBlockAddressForStart;
  std::unordered_map<size_t, uint64_t> NativeParentBlockAddressForStart;
  std::unordered_map<size_t, uint64_t> NativeBlockEndForStart;
  std::set<size_t> NativeInternalPcodeStarts;
  std::unordered_map<size_t, llvm::BasicBlock *> BlockForStart;
  std::unordered_map<uint64_t, llvm::BasicBlock *> BlockForAddress;
  std::unordered_map<uint64_t, llvm::BasicBlock *> TailJumpBlockForAddress;
  std::set<size_t> SuppressedPcodeOpIndices;
  std::set<size_t> FloatWriteOpIndices;
  bool CurrentOpIsFloatWrite = false;
  std::map<size_t, std::pair<VarnodeView, uint64_t>> ThunkBaseWrites;
  std::map<size_t, X87WindowSpec> X87Groups;
  VarnodeView StVarnodes[2];
  std::vector<uint64_t> EmptyNativeBlockAddresses;
  std::vector<llvm::BasicBlock *> ExternalTargetBlocks;
  const std::vector<PcodeOpView> *CurrentProgramOps = nullptr;
  const PcodeProgram *CurrentProgram = nullptr;
  std::map<size_t, unsigned> X87BlockEntryDepth;  // blockStarts 下标 -> 入口深度
  bool PreciseX87Depth = false;
  std::optional<unsigned> X87Depth;  // 当前位置的深度；nullopt = 退回旧模型
  llvm::BasicBlock *ExitBlock = nullptr;
  llvm::GlobalVariable *Memory = nullptr;
  std::unique_ptr<RegisterStorage> Registers;
  std::vector<RegisterRead> RegisterReads;
  uint64_t MaskedRegisterReadsNarrowed = 0;
};

} // namespace notdec::bin2llvm
