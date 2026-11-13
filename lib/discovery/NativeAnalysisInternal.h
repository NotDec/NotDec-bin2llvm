#include "notdec-bin2llvm/discovery/NativeAnalysis.h"

#include "notdec-bin2llvm/elf/LiefElfLoadImage.h"
#include "notdec-bin2llvm/core/SleighLift.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/DynamicEntry.hpp>
#include <LIEF/ELF/EnumToString.hpp>
#include <LIEF/ELF/Relocation.hpp>
#include <LIEF/ELF/Section.hpp>
#include <LIEF/ELF/Segment.hpp>
#include <LIEF/ELF/Symbol.hpp>

#if NOTDEC_BIN2LLVM_ENABLE_GTIRB
#include <gtirb_pprinter/PrettyPrinter.hpp>
#include <gtirb/gtirb.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <functional>
#include <ostream>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace notdec::bin2llvm {

struct X86PicI32JumpDispatch {
  uint64_t BlockStart = 0;
  uint64_t TableBase = 0;
  uint64_t EntryCount = 0;
};

struct X86PicI32DispatchPattern {
  std::string TargetReg;
  std::string BaseReg;
  std::string IndexReg;
};

struct LeaBaseBlockScan {
  std::optional<uint64_t> Base;
  bool Killed = false;
};

struct X86IndirectMemoryTarget {
  std::string BaseReg;
  uint64_t Displacement = 0;
};

struct X86ExternalFunctionPointerMatch {
  uint64_t SlotAddress = 0;
  NativeXrefKind Kind = NativeXrefKind::Flow;
};

bool containsAddress(uint64_t start, uint64_t size, uint64_t address);

std::string hexAddress(uint64_t address);

bool isInstructionFallthroughTo(const NativeInstruction &instruction,
                                uint64_t target);

NativeFunctionConfidence mergeConfidence(NativeFunctionConfidence lhs,
                                         NativeFunctionConfidence rhs);

std::string relocationTypeName(const LIEF::ELF::Relocation &relocation);

std::string relocationPurposeName(const LIEF::ELF::Relocation &relocation);

bool supportsRelocationPltAnalysis(LIEF::ELF::ARCH arch);

bool isRelativeRelocation(LIEF::ELF::ARCH arch,
                          LIEF::ELF::Relocation::TYPE type);

bool isGlobDatRelocation(LIEF::ELF::ARCH arch,
                         LIEF::ELF::Relocation::TYPE type);

bool isJumpSlotRelocation(LIEF::ELF::ARCH arch,
                          LIEF::ELF::Relocation::TYPE type);

bool isAbsoluteExternalFunctionPointerRelocation(
    LIEF::ELF::ARCH arch, LIEF::ELF::Relocation::TYPE type);

bool isIRelativeRelocation(LIEF::ELF::ARCH arch,
                           LIEF::ELF::Relocation::TYPE type);

std::optional<int64_t>
relocationAddendForPointerValue(const NativeProgramState &state,
                                const LIEF::ELF::Relocation &relocation);

std::optional<NativeSectionInfo> findSection(const NativeProgramState &state,
                                             const std::string &name);

bool readBytes(const NativeProgramState &state, uint64_t address, uint64_t size,
               std::vector<uint8_t> &bytes);

std::optional<uint64_t> parseHex(const std::string &text);

std::optional<uint64_t> parseUnsignedNumber(const std::string &text);

std::string trimAsciiWhitespace(const std::string &text);

std::optional<int64_t> readSigned32(const NativeProgramState &state,
                                    uint64_t address);

void addUniqueAddress(std::vector<uint64_t> &addresses, uint64_t address);

const NativeMemoryRange *findReadableRangeContaining(
    const NativeProgramState &state, uint64_t address);

bool isLikelyCStringByte(uint8_t byte);

bool looksLikeReadOnlyCString(const NativeProgramState &state,
                              uint64_t address);

bool isMappedAddress(const NativeProgramState &state, uint64_t address);

bool executableRangeContains(const NativeProgramState &state, uint64_t start,
                             uint64_t end);

bool hasSource(const NativeFunctionSeed &seed, const std::string &source);

bool hasBytesAt(const NativeProgramState &state, uint64_t address,
                std::initializer_list<uint8_t> expected);

bool isX86GlibcStartPattern(const NativeProgramState &state,
                            uint64_t address);

void addUnsupportedEhFrameSample(NativeEhFrameStats &stats,
                                 const std::string &sample);

void addEhFrameHdrUnsupportedSample(NativeEhFrameStats &stats,
                                    const std::string &sample);

void addEhFrameHdrMismatchSample(NativeEhFrameStats &stats,
                                 const std::string &sample);

void addDynamicScalarEntry(NativeProgramState &state, uint64_t address,
                           const std::string &source);

void addDynamicArrayEntries(NativeProgramState &state, uint64_t arrayAddress,
                            uint64_t arraySize, const std::string &source);

std::optional<uint64_t>
findNearestLeaBase(const NativeProgramState &state,
                   const NativeFunction &function, uint64_t beforeAddress,
                   const std::string &reg);

bool writesRegister(const std::string &text, const std::string &reg);

const NativeBasicBlock *functionBlockContaining(const NativeFunction &function,
                                                uint64_t address);

std::vector<const NativeBasicBlock *>
functionPredecessorBlocks(const NativeFunction &function, uint64_t blockStart);

LeaBaseBlockScan findNearestLeaBaseInBlockRange(
    const NativeProgramState &state, const NativeBasicBlock &block,
    uint64_t beforeAddress, const std::string &reg);

std::optional<uint64_t> findNearestLeaBaseInFunctionCFGFromBlock(
    const NativeProgramState &state, const NativeFunction &function,
    const NativeBasicBlock &block,
    uint64_t beforeAddress, const std::string &reg,
    std::set<uint64_t> &visitingBlocks,
    std::map<uint64_t, std::optional<uint64_t>> &memo);

std::optional<uint64_t> findNearestLeaBaseInFunctionCFG(
    const NativeProgramState &state, const NativeFunction &function,
    uint64_t beforeAddress, const std::string &reg,
    std::set<uint64_t> &visitedBlocks);

std::optional<uint64_t> parseX86LeaAbsoluteBase(const std::string &text,
                                                const std::string &reg);

std::optional<uint64_t> findNearestLeaBaseInDecodedCFG(
    const std::vector<NativeInstruction> &instructions, size_t beforeIndex,
    const std::string &reg);

std::string low32RegisterName(const std::string &reg);

std::string low8RegisterName(const std::string &reg);

std::vector<std::string> compareRegisterNamesForText(const std::string &reg);

std::vector<std::string> compareRegisterNamesForIndex(const std::string &reg);

void addUniqueString(std::vector<std::string> &values, std::string value);

void addCompareRegisterNames(std::vector<std::string> &compareRegs,
                             const std::string &reg);

std::optional<std::pair<std::string, std::string>>
parseX86MovRegReg(const std::string &text, const std::string &mnemonic);

std::optional<std::pair<std::string, uint64_t>>
parseX86RegImmediate(const std::string &text, const std::string &mnemonic);

std::optional<std::pair<std::string, std::string>>
parseX86MovRegMemory(const std::string &text);

std::optional<std::pair<std::string, std::string>>
parseX86MovMemoryReg(const std::string &text);

std::optional<std::pair<std::string, uint64_t>>
parseX86MemoryImmediate(const std::string &text, const std::string &mnemonic);

std::optional<uint64_t>
findNearestUpperBound(const NativeProgramState &state,
                      const NativeFunction &function, uint64_t beforeAddress,
                      const std::string &indexReg);

std::optional<std::string> parseX86RegisterAfterPrefix(
    const std::string &text, const std::string &prefix);

std::optional<X86IndirectMemoryTarget>
parseX86RegisterMemoryTarget(const std::string &text,
                             const std::string &mnemonic,
                             const std::string &destReg);

std::optional<X86IndirectMemoryTarget>
parseX86IndirectMemoryTarget(const std::string &text,
                             const std::string &mnemonic);

std::optional<uint64_t> parseX86LeaAbsoluteBase(const std::string &text,
                                                const std::string &reg);

std::optional<uint64_t> findNearestLeaBaseInDecodedInstructions(
    const std::vector<NativeInstruction> &instructions, size_t beforeIndex,
    const std::string &reg);

std::optional<uint64_t> findNearestLeaBaseInKnownFallthroughInstructions(
    const NativeProgramState &state, uint64_t beforeAddress,
    const std::string &reg);

std::optional<X86IndirectMemoryTarget>
findNearestRegisterLoadInDecodedInstructions(
    const std::vector<NativeInstruction> &instructions, size_t beforeIndex,
    const std::string &reg);

std::optional<X86IndirectMemoryTarget>
findNearestRegisterLoadInKnownFallthroughInstructions(
    const NativeProgramState &state, uint64_t beforeAddress,
    const std::string &reg);

std::optional<std::pair<std::string, std::string>>
parseX86AddRegReg(const std::string &text);

std::optional<X86PicI32DispatchPattern>
parseX86PicI32DispatchPattern(const NativeInstruction &load,
                              const NativeInstruction &add,
                              const NativeInstruction &branch);

std::optional<X86PicI32DispatchPattern>
findX86PicI32DispatchPattern(const NativeProgramState &state,
                             const NativeFunction &function,
                             const NativeInstruction &branch,
                             const std::string &branchReg);

std::optional<X86PicI32JumpDispatch>
matchX86PicI32OffsetDispatch(const NativeProgramState &state,
                             const NativeFunction &function,
                             uint64_t branchAddress);

bool isExternalFunctionPointerRelocationAt(const NativeProgramState &state,
                                           uint64_t address);

std::optional<X86ExternalFunctionPointerMatch>
matchAnyLeaBaseInDecodedInstructions(
    const std::vector<NativeInstruction> &instructions,
    const std::string &reg, uint64_t displacement, NativeXrefKind kind,
    const NativeProgramState &state);

std::optional<X86ExternalFunctionPointerMatch>
matchX86ExternalFunctionPointerInstruction(
    const NativeProgramState &state,
    const std::vector<NativeInstruction> &instructions, size_t index);

bool functionContainsBlockStart(const NativeProgramState &state,
                                uint64_t address);

bool functionContainsAddress(const NativeFunction &function, uint64_t address);

bool readX86PicI32Targets(const NativeProgramState &state, uint64_t tableBase,
                          uint64_t entryCount, bool onlyExistingBlocks,
                          std::vector<uint64_t> &targets, bool &complete);

std::unique_ptr<NativeAnalyzer> createEhFrameAnalyzer();

std::unique_ptr<NativeAnalyzer>
createElfEntryAnalyzer(NativeRuntimeFilterOptions options);

std::unique_ptr<NativeAnalyzer> createElfLoadAnalyzer();

std::unique_ptr<NativeAnalyzer>
createElfSymbolAnalyzer(NativeRuntimeFilterOptions options);

std::unique_ptr<NativeAnalyzer> createFlowFactNormalizer();

std::unique_ptr<NativeAnalyzer> createGtirbFunctionFactsAnalyzer(
    NativeGtirbDecodeOptions options);

std::unique_ptr<NativeAnalyzer> createRelocationPltAnalyzer();

std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output);

std::unique_ptr<NativeAnalyzer> createSleighSeedInstructionAnalyzer(
    NativeSleighDecodeOptions options);

std::unique_ptr<NativeAnalyzer> createX86JumpTableAnalyzer();

std::unique_ptr<NativeAnalyzer> createX86PcThunkAnalyzer();

} // namespace notdec::bin2llvm
