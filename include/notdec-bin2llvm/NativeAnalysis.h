#pragma once

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
  std::string PrimaryName;
  std::vector<std::string> Aliases;
  std::vector<std::string> Sources;
  NativeFunctionConfidence Confidence = NativeFunctionConfidence::Low;
};

struct NativeSectionInfo {
  std::string Name;
  uint64_t Address = 0;
  uint64_t Size = 0;
  bool Executable = false;
};

// NativeProgramState is the shared state for the native lifter's first
// AutoAnalysis pass.  It is intentionally much smaller than Ghidra's Program:
// the current goal is to let analyzers exchange memory, section, symbol, and
// function-seed facts without turning discovery into one large function.
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
  const std::map<std::string, uint64_t> &sourceCounts() const {
    return SourceCounts;
  }
  const std::vector<std::string> &notes() const { return Notes; }

  bool isExecutableAddress(uint64_t address) const;
  std::optional<uint64_t> readPointer(uint64_t address) const;

  bool addFunctionSeed(uint64_t address, uint64_t size, std::string name,
                       std::string source, NativeFunctionConfidence confidence);
  void addNote(std::string note);

private:
  const LIEF::ELF::Binary &Binary;
  uint8_t PointerSize = 8;
  std::vector<NativeMemoryRange> MemoryRanges;
  std::vector<NativeSectionInfo> Sections;
  std::map<uint64_t, NativeFunctionSeed> FunctionSeeds;
  std::map<std::string, uint64_t> SourceCounts;
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
std::unique_ptr<NativeAnalyzer> createElfEntryAnalyzer();
std::unique_ptr<NativeAnalyzer> createElfSymbolAnalyzer();
std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output);

} // namespace notdec::bin2llvm
