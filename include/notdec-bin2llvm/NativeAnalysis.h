#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace LIEF::ELF {
class Binary;
} // namespace LIEF::ELF

namespace notdec::bin2llvm {

enum class NativeFunctionConfidence {
  High,
  Medium,
  Low,
};

std::string toString(NativeFunctionConfidence confidence);

struct NativeMemoryRange {
  uint64_t Start = 0;
  uint64_t Size = 0;
  bool Readable = false;
  bool Writable = false;
  bool Executable = false;
  std::vector<uint8_t> Bytes;

  uint64_t end() const { return Start + Size; }
};

struct NativeFunctionSeed {
  uint64_t Address = 0;
  uint64_t Size = 0;
  // Function ranges are half-open: [RangeStart, RangeEnd).  This matches later
  // decode-boundary checks and avoids inclusive-end overflow cases.
  uint64_t RangeStart = 0;
  uint64_t RangeEnd = 0;
  std::string RangeSource;
  std::string PrimaryName;
  std::vector<std::string> Aliases;
  std::vector<std::string> Sources;
  NativeFunctionConfidence Confidence = NativeFunctionConfidence::Low;
};

// NativeFunctionWorkItem is the handoff from entry discovery to recursive
// decode.  It records only the address and the source that first queued it;
// later decode can look up merged names/ranges in FunctionSeeds.
struct NativeFunctionWorkItem {
  uint64_t Address = 0;
  std::string Source;
};

struct NativeSectionInfo {
  std::string Name;
  uint64_t Address = 0;
  uint64_t Size = 0;
  bool Executable = false;
};

struct NativeEhFrameFdeInfo {
  uint64_t PcBegin = 0;
  uint64_t FdeAddress = 0;
};

struct NativeEhFrameHdrEntry {
  uint64_t InitialLocation = 0;
  uint64_t FdeAddress = 0;
};

// EhFrameStats keeps the first native .eh_frame reader observable without
// exposing DWARF internals to the rest of native discovery.
struct NativeEhFrameStats {
  bool HasEhFrameHdr = false;
  bool HasEhFrame = false;
  bool ParsedEhFrameHdr = false;
  uint64_t CieCount = 0;
  uint64_t FdeCount = 0;
  uint64_t ParsedFdeCount = 0;
  uint64_t AddedSeedCount = 0;
  uint64_t OverlappedSeedCount = 0;
  uint64_t HdrFdeCount = 0;
  uint64_t HdrTableEntries = 0;
  uint64_t HdrMatchedStarts = 0;
  uint64_t HdrMissingInFrame = 0;
  uint64_t HdrExtraFrameFdes = 0;
  uint64_t HdrFdeAddressMatches = 0;
  uint64_t HdrFdeAddressMismatches = 0;
  uint64_t HdrInvalidCount = 0;
  uint64_t HdrUnsupportedCount = 0;
  uint64_t InvalidCount = 0;
  uint64_t UnsupportedCount = 0;
  std::vector<NativeEhFrameFdeInfo> FrameFdes;
  std::vector<NativeEhFrameHdrEntry> HdrEntries;
  std::vector<std::string> HdrMismatchSamples;
  std::vector<std::string> UnsupportedSamples;
};

// NativeRelocationInfo records the subset of ELF relocation state that native
// discovery needs.  It does not try to model every relocation side effect; it
// keeps enough data to explain pointer reads and later classify PLT calls.
struct NativeRelocationInfo {
  uint64_t Address = 0;
  uint32_t Type = 0;
  std::string TypeName;
  std::string SymbolName;
  uint64_t SymbolValue = 0;
  int64_t Addend = 0;
  std::string TableKind;
  std::string Status;
  std::optional<uint64_t> ComputedValue;
};

// NativePltEntry links an executable PLT stub to the GOT slot and external
// symbol it dispatches to.  Function decoding can later use this to report an
// external call instead of treating the stub as an internal function.
struct NativePltEntry {
  uint64_t StubAddress = 0;
  uint64_t GotAddress = 0;
  std::string SymbolName;
};

enum class NativeXrefKind {
  Flow,
  Call,
  Data,
  String,
};

std::string toString(NativeXrefKind kind);

// NativeBasicBlock is the smallest CFG unit shared by native analyzers.  It is
// address based on purpose: recursive decode owns instruction details later,
// while lowering and CLI queries mostly need stable block ranges and edges.
struct NativeBasicBlock {
  uint64_t Start = 0;
  uint64_t End = 0;
  std::vector<uint64_t> Successors;
};

// NativeFunction is separate from NativeFunctionSeed.  A seed says "try here";
// a confirmed function says decode has accepted an entry and at least a
// conservative body/range can be queried by later analyzers.
struct NativeFunction {
  uint64_t Entry = 0;
  uint64_t RangeStart = 0;
  uint64_t RangeEnd = 0;
  std::string Name;
  std::vector<NativeBasicBlock> Blocks;
  std::string Source;
};

// NativeXref keeps only the common reference shape used by CFG, callgraph, and
// CLI queries.  More detailed operand metadata can be added when decode starts
// producing it; the indexes below only depend on from/to/kind.
struct NativeXref {
  uint64_t From = 0;
  uint64_t To = 0;
  NativeXrefKind Kind = NativeXrefKind::Flow;
  std::string Source;
};

// NativeInstruction records decoded instruction facts accepted by native
// analyzers.  It deliberately stores raw bytes and a light display mnemonic,
// while operands and P-Code stay out until recursive decode has real users for
// them.
struct NativeInstruction {
  uint64_t Address = 0;
  uint64_t Size = 0;
  std::vector<uint8_t> Bytes;
  std::string Mnemonic;
  std::string Source;

  uint64_t end() const { return Address + Size; }
};

// NativeProgramState is the shared state for the native lifter's first
// AutoAnalysis pass.  It is intentionally much smaller than Ghidra's Program:
// the current goal is to let analyzers exchange memory, relocation, PLT,
// section, symbol, and function-seed facts without turning discovery into one
// large function.
class NativeProgramState {
public:
  explicit NativeProgramState(const LIEF::ELF::Binary &binary);

  const LIEF::ELF::Binary &binary() const { return Binary; }

  uint8_t pointerSize() const { return PointerSize; }
  const std::vector<NativeMemoryRange> &memoryRanges() const {
    return MemoryRanges;
  }
  const std::vector<NativeSectionInfo> &sections() const { return Sections; }
  const std::map<uint64_t, NativeFunctionSeed> &functionSeeds() const {
    return FunctionSeeds;
  }
  const std::vector<NativeFunctionWorkItem> &functionWorklist() const {
    return FunctionWorklist;
  }
  const std::map<std::string, uint64_t> &sourceCounts() const {
    return SourceCounts;
  }
  const std::vector<NativeRelocationInfo> &relocations() const {
    return Relocations;
  }
  const std::map<uint64_t, uint64_t> &relocatedPointers() const {
    return RelocatedPointers;
  }
  const std::vector<NativePltEntry> &pltEntries() const { return PltEntries; }
  const NativeEhFrameStats &ehFrameStats() const { return EhFrameStats; }
  NativeEhFrameStats &ehFrameStats() { return EhFrameStats; }
  const std::map<uint64_t, NativeFunction> &functions() const {
    return Functions;
  }
  const std::vector<NativeXref> &xrefs() const { return Xrefs; }
  const std::map<uint64_t, NativeInstruction> &instructions() const {
    return Instructions;
  }
  const std::vector<std::string> &notes() const { return Notes; }

  bool isExecutableAddress(uint64_t address) const;
  std::optional<uint64_t> readPointer(uint64_t address) const;
  std::optional<uint64_t> readRawPointer(uint64_t address) const;
  std::optional<std::string> lookupPltExternal(uint64_t address) const;
  const NativeFunction *functionAt(uint64_t entry) const;
  const NativeFunction *functionContaining(uint64_t address) const;
  std::vector<const NativeXref *> xrefsFrom(uint64_t address) const;
  std::vector<const NativeXref *> xrefsTo(uint64_t address) const;
  const NativeInstruction *instructionAt(uint64_t address) const;
  std::vector<const NativeInstruction *>
  instructionsInRange(uint64_t start, uint64_t end) const;

  bool addFunctionSeed(uint64_t address, uint64_t size, std::string name,
                       std::string source, NativeFunctionConfidence confidence);
  bool addFunction(NativeFunction function);
  bool addBasicBlock(uint64_t functionEntry, NativeBasicBlock block);
  void addXref(NativeXref xref);
  bool addInstruction(NativeInstruction instruction);
  void addFunctionRange(uint64_t address, uint64_t start, uint64_t end,
                        std::string source);
  void addRelocation(NativeRelocationInfo relocation);
  void addRelocatedPointer(uint64_t address, uint64_t value);
  void addPltEntry(NativePltEntry entry);
  void addNote(std::string note);

private:
  const LIEF::ELF::Binary &Binary;
  uint8_t PointerSize = 8;
  std::vector<NativeMemoryRange> MemoryRanges;
  std::vector<NativeSectionInfo> Sections;
  std::map<uint64_t, NativeFunctionSeed> FunctionSeeds;
  std::vector<NativeFunctionWorkItem> FunctionWorklist;
  std::map<std::string, uint64_t> SourceCounts;
  std::vector<NativeRelocationInfo> Relocations;
  std::map<uint64_t, uint64_t> RelocatedPointers;
  std::vector<NativePltEntry> PltEntries;
  NativeEhFrameStats EhFrameStats;
  std::map<uint64_t, NativeFunction> Functions;
  std::vector<NativeXref> Xrefs;
  std::map<uint64_t, std::vector<size_t>> XrefsByFrom;
  std::map<uint64_t, std::vector<size_t>> XrefsByTo;
  std::map<uint64_t, NativeInstruction> Instructions;
  std::vector<std::string> Notes;
};

class NativeAnalysisManager;

class NativeAnalyzer {
public:
  virtual ~NativeAnalyzer() = default;

  virtual std::string name() const = 0;
  virtual int priority() const = 0;
  virtual void run(NativeProgramState &state,
                   NativeAnalysisManager &manager) = 0;
};

// The first manager keeps scheduling simple: analyzers are sorted once by
// priority and run in that order.  The interface leaves room for later
// worklists, but this first patch only needs the loader, entry, symbol, and
// report analyzers from the plan.
class NativeAnalysisManager {
public:
  void addAnalyzer(std::unique_ptr<NativeAnalyzer> analyzer);
  void run(NativeProgramState &state);

private:
  std::vector<std::unique_ptr<NativeAnalyzer>> Analyzers;
};

std::unique_ptr<NativeAnalyzer> createElfLoadAnalyzer();
std::unique_ptr<NativeAnalyzer> createRelocationPltAnalyzer();
std::unique_ptr<NativeAnalyzer> createElfEntryAnalyzer();
std::unique_ptr<NativeAnalyzer> createElfSymbolAnalyzer();
std::unique_ptr<NativeAnalyzer> createEhFrameAnalyzer();
std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output);

} // namespace notdec::bin2llvm
