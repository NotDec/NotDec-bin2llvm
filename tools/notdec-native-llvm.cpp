#include "SleighBytes.h"
#include "notdec-bin2llvm/LiefElfLoadImage.h"
#include "notdec-bin2llvm/NativeAnalysis.h"
#include "notdec-bin2llvm/PcodeToLLVM.h"
#include "notdec-bin2llvm/SleighLift.h"
#include "notdec-bin2llvm/passes/NativeRegisterSSA.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <cstdint>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

#ifndef NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR
#define NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR "/sn640/ghidra"
#endif

struct CliOptions {
  std::string ElfPath;
  notdec::bin2llvm::SleighSpecOptions SpecOptions;
  uint64_t Address = 0;
  uint64_t Length = 0;
  std::optional<uint64_t> FunctionEntry;
  std::string FunctionName;
  std::vector<std::pair<uint64_t, uint64_t>> FunctionBlockRanges;
  std::unordered_map<uint64_t, std::vector<uint64_t>>
      FunctionBlockSuccessors;
  bool AllConfirmed = false;
  std::string OutputPath;
  std::string SummaryJsonPath;
  notdec::bin2llvm::NativeSleighDecodeOptions DecodeOptions;
  notdec::bin2llvm::PcodeMemoryModel MemoryModel =
      notdec::bin2llvm::PcodeMemoryModel::IntToPtr;
  bool DisableRegisterSSAPass = false;
  bool PrintRegisterSSASummary = false;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " <elf-file> [sla-file] "
               "(-a <address> -l <length> | -f <entry> | -n <name> | "
               "--all-confirmed) "
               "-o <output.ll> [--summary-json-out <path>] "
               "[--no-register-ssa-pass] [--register-ssa-summary] "
               "[--decode-seed-limit <count>] "
               "[--memory-model inttoptr|global-array] [-p root-sla-dir] "
               "[-s pspec-file]\n";
}

bool parseUint64(const std::string &text, uint64_t &value) {
  try {
    size_t parsed = 0;
    value = std::stoull(text, &parsed, 0);
    return parsed == text.size();
  } catch (const std::exception &) {
    return false;
  }
}

bool parseMemoryModel(const std::string &text,
                      notdec::bin2llvm::PcodeMemoryModel &model) {
  if (text == "inttoptr") {
    model = notdec::bin2llvm::PcodeMemoryModel::IntToPtr;
    return true;
  }
  if (text == "global-array") {
    model = notdec::bin2llvm::PcodeMemoryModel::GlobalArray;
    return true;
  }
  return false;
}

bool hasExtension(const std::string &path, llvm::StringRef extension) {
  return llvm::StringRef(path).ends_with_insensitive(extension);
}

bool isIRInputPath(const std::string &path) {
  return hasExtension(path, ".ll") || hasExtension(path, ".bc");
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc < 4) {
    return std::nullopt;
  }

  CliOptions options;
  options.ElfPath = argv[1];
  bool hasAddress = false;
  bool hasLength = false;
  int argIndex = 2;
  if (argIndex < argc && std::string(argv[argIndex]).rfind("-", 0) != 0) {
    options.SpecOptions.SlaFileName = argv[argIndex++];
  }

  for (; argIndex < argc; ++argIndex) {
    std::string flag = argv[argIndex];
    if (flag == "--all-confirmed") {
      options.AllConfirmed = true;
      continue;
    }
    if (flag == "--no-register-ssa-pass") {
      options.DisableRegisterSSAPass = true;
      continue;
    }
    if (flag == "--register-ssa-summary") {
      options.PrintRegisterSSASummary = true;
      continue;
    }
    if (argIndex + 1 >= argc) {
      std::cerr << "flag has no value: " << flag << '\n';
      return std::nullopt;
    }

    std::string value = argv[++argIndex];
    if (flag == "-a") {
      if (!parseUint64(value, options.Address)) {
        std::cerr << "invalid address: " << value << '\n';
        return std::nullopt;
      }
      hasAddress = true;
    } else if (flag == "-f") {
      uint64_t entry = 0;
      if (!parseUint64(value, entry)) {
        std::cerr << "invalid function entry: " << value << '\n';
        return std::nullopt;
      }
      options.FunctionEntry = entry;
    } else if (flag == "-n") {
      options.FunctionName = std::move(value);
    } else if (flag == "-l") {
      if (!parseUint64(value, options.Length) || options.Length == 0) {
        std::cerr << "invalid length: " << value << '\n';
        return std::nullopt;
      }
      hasLength = true;
    } else if (flag == "-o") {
      options.OutputPath = std::move(value);
    } else if (flag == "--summary-json-out") {
      options.SummaryJsonPath = std::move(value);
    } else if (flag == "--decode-seed-limit") {
      uint64_t limit = 0;
      if (!parseUint64(value, limit)) {
        std::cerr << "invalid decode seed limit: " << value << '\n';
        return std::nullopt;
      }
      options.DecodeOptions.MaxDecodedSeeds = limit;
    } else if (flag == "--memory-model") {
      if (!parseMemoryModel(value, options.MemoryModel)) {
        std::cerr << "invalid memory model: " << value << '\n';
        return std::nullopt;
      }
    } else if (flag == "-p") {
      options.SpecOptions.RootSlaDir = std::move(value);
    } else if (flag == "-s") {
      options.SpecOptions.PspecFileName = std::move(value);
    } else {
      std::cerr << "unknown flag: " << flag << '\n';
      return std::nullopt;
    }
  }

  unsigned selectionCount = 0;
  if (options.FunctionEntry) {
    ++selectionCount;
  }
  if (!options.FunctionName.empty()) {
    ++selectionCount;
  }
  if (options.AllConfirmed) {
    ++selectionCount;
  }
  if (hasAddress || hasLength) {
    ++selectionCount;
  }
  if (selectionCount != 1) {
    if (!isIRInputPath(options.ElfPath) || selectionCount != 0) {
      std::cerr << "choose exactly one of -a/-l, -f, -n, or --all-confirmed\n";
      return std::nullopt;
    }
  }
  if ((hasAddress || hasLength) && !hasAddress) {
    std::cerr << "missing -a <address>\n";
    return std::nullopt;
  }
  if ((hasAddress || hasLength) && !hasLength) {
    std::cerr << "missing -l <length>\n";
    return std::nullopt;
  }
  if (options.OutputPath.empty()) {
    std::cerr << "missing -o <output.ll>\n";
    return std::nullopt;
  }
  return options;
}

std::filesystem::path defaultX86SpecRoot() {
  return std::filesystem::path(NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR) /
         "Ghidra/Processors/x86/data/languages";
}

bool resolveSpecOptions(const LIEF::ELF::Binary &binary,
                        notdec::bin2llvm::SleighSpecOptions &options) {
  if (!options.SlaFileName.empty()) {
    return true;
  }

  if (binary.header().machine_type() != LIEF::ELF::ARCH::X86_64) {
    std::cerr << "automatic sleigh spec selection only supports x86-64 ELF\n";
    return false;
  }

  std::filesystem::path specRoot = defaultX86SpecRoot();
  std::filesystem::path slaPath = specRoot / "x86-64.sla";
  std::filesystem::path pspecPath = specRoot / "x86-64.pspec";
  if (!std::filesystem::exists(slaPath)) {
    std::cerr << "could not find auto-selected sla file: "
              << slaPath.string() << '\n';
    return false;
  }

  options.SlaFileName = slaPath.string();
  if (!options.PspecFileName && std::filesystem::exists(pspecPath)) {
    options.PspecFileName = pspecPath.string();
  }
  return true;
}

std::string entryFunctionName(uint64_t entry) {
  std::ostringstream stream;
  stream << "notdec_native_" << std::hex << entry;
  return stream.str();
}

std::string hexAddress(uint64_t address) {
  std::ostringstream stream;
  stream << std::hex << address;
  return stream.str();
}

std::string sanitizeLlvmFunctionName(const std::string &name) {
  std::string result;
  result.reserve(name.size());
  for (char ch : name) {
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
    result.push_back(ok ? ch : '_');
  }
  if (result.empty()) {
    return "";
  }
  if (result.front() >= '0' && result.front() <= '9') {
    result.insert(result.begin(), '_');
  }
  return result;
}

std::string jsonEscape(const std::string &text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (char ch : text) {
    switch (ch) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\n':
    case '\r':
    case '\t':
      escaped += ' ';
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
}

notdec::bin2llvm::NativeProgramState
runNativeDiscovery(
    const LIEF::ELF::Binary &binary,
    notdec::bin2llvm::NativeSleighDecodeOptions decodeOptions = {}) {
  notdec::bin2llvm::NativeProgramState state(binary);
  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createElfLoadAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createRelocationPltAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createElfEntryAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createElfSymbolAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createEhFrameAnalyzer());
  manager.addAnalyzer(
      notdec::bin2llvm::createSleighSeedInstructionAnalyzer(decodeOptions));
  manager.run(state);
  return state;
}

uint64_t countBasicBlocks(const notdec::bin2llvm::NativeProgramState &state) {
  uint64_t count = 0;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    count += function.Blocks.size();
  }
  return count;
}

bool writeSummaryJson(const notdec::bin2llvm::NativeProgramState &state,
                      const std::string &path) {
  using namespace notdec::bin2llvm;

  std::ofstream output(path);
  if (!output) {
    std::cerr << "failed to open summary json: " << path << '\n';
    return false;
  }

  std::map<NativeFunctionConfidence, uint64_t> confidenceCounts;
  for (const auto &[address, seed] : state.functionSeeds()) {
    (void)address;
    ++confidenceCounts[seed.Confidence];
  }

  std::map<NativeXrefKind, uint64_t> xrefKindCounts;
  for (const NativeXref &xref : state.xrefs()) {
    ++xrefKindCounts[xref.Kind];
  }

  std::map<NativeUnresolvedFlowKind, uint64_t> unresolvedFlowCounts;
  for (const NativeUnresolvedFlow &flow : state.unresolvedFlows()) {
    ++unresolvedFlowCounts[flow.Kind];
  }

  output << "{\n";
  output << "  \"function_seeds\": " << state.functionSeeds().size() << ",\n";
  output << "  \"function_worklist\": " << state.functionWorklist().size()
         << ",\n";
  output << "  \"confirmed_functions\": " << state.functions().size() << ",\n";
  output << "  \"basic_blocks\": " << countBasicBlocks(state) << ",\n";
  output << "  \"instructions\": " << state.instructions().size() << ",\n";
  output << "  \"sources\": {\n";
  bool firstSource = true;
  for (const auto &[source, count] : state.sourceCounts()) {
    output << (firstSource ? "" : ",\n");
    output << "    \"" << jsonEscape(source) << "\": " << count;
    firstSource = false;
  }
  output << "\n  },\n";
  output << "  \"confidence\": {\n";
  output << "    \"high\": "
         << confidenceCounts[NativeFunctionConfidence::High] << ",\n";
  output << "    \"medium\": "
         << confidenceCounts[NativeFunctionConfidence::Medium] << ",\n";
  output << "    \"low\": "
         << confidenceCounts[NativeFunctionConfidence::Low] << "\n";
  output << "  },\n";
  output << "  \"xrefs\": {\n";
  output << "    \"total\": " << state.xrefs().size() << ",\n";
  output << "    \"flow\": " << xrefKindCounts[NativeXrefKind::Flow]
         << ",\n";
  output << "    \"call\": " << xrefKindCounts[NativeXrefKind::Call]
         << ",\n";
  output << "    \"data\": " << xrefKindCounts[NativeXrefKind::Data]
         << ",\n";
  output << "    \"string\": " << xrefKindCounts[NativeXrefKind::String]
         << "\n";
  output << "  },\n";
  output << "  \"unresolved_indirect_flows\": {\n";
  output << "    \"total\": " << state.unresolvedFlows().size() << ",\n";
  output << "    \"indirect call\": "
         << unresolvedFlowCounts[NativeUnresolvedFlowKind::IndirectCall]
         << ",\n";
  output << "    \"indirect branch\": "
         << unresolvedFlowCounts[NativeUnresolvedFlowKind::IndirectBranch]
         << "\n";
  output << "  },\n";
  output << "  \"notes\": " << state.notes().size() << "\n";
  output << "}\n";
  return true;
}

bool resolveFunctionRange(const notdec::bin2llvm::NativeProgramState &state,
                          CliOptions &options) {
  if (!options.FunctionEntry && options.FunctionName.empty()) {
    return true;
  }

  const notdec::bin2llvm::NativeFunction *function = nullptr;
  if (options.FunctionEntry) {
    function = state.functionAt(*options.FunctionEntry);
  } else {
    for (const auto &[entry, candidate] : state.functions()) {
      (void)entry;
      if (candidate.Name != options.FunctionName) {
        continue;
      }
      if (function != nullptr) {
        std::cerr << "native discovery found duplicate function name: "
                  << options.FunctionName << '\n';
        return false;
      }
      function = &candidate;
    }
  }
  if (function == nullptr) {
    if (options.FunctionEntry) {
      std::cerr << "native discovery did not confirm function at 0x" << std::hex
                << *options.FunctionEntry << std::dec << '\n';
    } else {
      std::cerr << "native discovery did not confirm function named "
                << options.FunctionName << '\n';
    }
    return false;
  }
  if (function->RangeEnd <= function->Entry) {
    std::cerr << "native function has empty range at 0x" << std::hex
              << function->Entry << std::dec << '\n';
    return false;
  }

  options.Address = function->Entry;
  options.Length = function->RangeEnd - function->Entry;
  options.FunctionBlockRanges.clear();
  options.FunctionBlockSuccessors.clear();
  for (const notdec::bin2llvm::NativeBasicBlock &block : function->Blocks) {
    options.FunctionBlockRanges.push_back({block.Start, block.End});
    options.FunctionBlockSuccessors.emplace(block.Start, block.Successors);
  }
  if (!options.FunctionEntry) {
    options.FunctionEntry = function->Entry;
  }
  return true;
}

std::vector<std::pair<uint64_t, uint64_t>> blockRanges(
    const notdec::bin2llvm::NativeFunction &function) {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  ranges.reserve(function.Blocks.size());
  for (const notdec::bin2llvm::NativeBasicBlock &block : function.Blocks) {
    ranges.push_back({block.Start, block.End});
  }
  return ranges;
}

std::unordered_map<uint64_t, std::vector<uint64_t>> blockSuccessors(
    const notdec::bin2llvm::NativeFunction &function) {
  std::unordered_map<uint64_t, std::vector<uint64_t>> successors;
  for (const notdec::bin2llvm::NativeBasicBlock &block : function.Blocks) {
    successors.emplace(block.Start, block.Successors);
  }
  return successors;
}

std::string uniqueFunctionName(const std::string &baseName,
                               std::set<std::string> &usedNames) {
  std::string name = baseName.empty() ? "notdec_native_function" : baseName;
  std::string unique = name;
  unsigned index = 1;
  while (!usedNames.insert(unique).second) {
    unique = name + "_" + std::to_string(index++);
  }
  return unique;
}

std::string nativeFunctionLlvmName(const notdec::bin2llvm::NativeFunction &func,
                                   std::set<std::string> &usedNames) {
  if (!func.Name.empty()) {
    return uniqueFunctionName(sanitizeLlvmFunctionName(func.Name), usedNames);
  }
  return uniqueFunctionName(entryFunctionName(func.Entry), usedNames);
}

std::string externalFunctionLlvmName(const std::string &symbolName,
                                     std::set<std::string> &usedNames) {
  std::string name = sanitizeLlvmFunctionName(symbolName);
  if (name.empty()) {
    name = "notdec_external_function";
  }
  return uniqueFunctionName(name, usedNames);
}

struct NativeCallTargets {
  std::unordered_map<uint64_t, std::string> Direct;
  std::unordered_map<uint64_t, std::string> External;
  std::unordered_map<uint64_t, std::string> IndirectExternal;
};

std::optional<notdec::bin2llvm::NativeSectionInfo>
sectionByName(const notdec::bin2llvm::NativeProgramState &state,
              const std::string &name) {
  for (const notdec::bin2llvm::NativeSectionInfo &section :
       state.sections()) {
    if (section.Name == name) {
      return section;
    }
  }
  return std::nullopt;
}

NativeCallTargets planNativeCallTargets(
    const notdec::bin2llvm::NativeProgramState &state) {
  NativeCallTargets targets;
  std::set<std::string> usedNames;
  std::unordered_map<std::string, std::string> externalNamesBySymbol;
  auto externalNameFor = [&](const std::string &symbolName) -> std::string {
    auto it = externalNamesBySymbol.find(symbolName);
    if (it != externalNamesBySymbol.end()) {
      return it->second;
    }
    std::string name = externalFunctionLlvmName(symbolName, usedNames);
    externalNamesBySymbol.emplace(symbolName, name);
    return name;
  };

  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    if (function.RangeEnd <= function.Entry) {
      continue;
    }
    targets.Direct.emplace(function.Entry,
                           nativeFunctionLlvmName(function, usedNames));
  }

  for (const notdec::bin2llvm::NativePltEntry &entry : state.pltEntries()) {
    if (entry.SymbolName.empty()) {
      continue;
    }
    std::string name = externalNameFor(entry.SymbolName);
    targets.External.emplace(entry.StubAddress, name);
    targets.IndirectExternal.emplace(entry.GotAddress, name);
  }

  std::optional<notdec::bin2llvm::NativeSectionInfo> plt =
      sectionByName(state, ".plt");
  std::optional<notdec::bin2llvm::NativeSectionInfo> got =
      sectionByName(state, ".got");
  if (plt && got) {
    targets.IndirectExternal.emplace(
        got->Address + 16, externalNameFor("notdec_plt0_resolver"));
  }

  for (const notdec::bin2llvm::NativeRelocationInfo &relocation :
       state.relocations()) {
    if (relocation.SymbolName.empty() || relocation.Status != "external") {
      continue;
    }
    if (relocation.TypeName != "X86_64_GLOB_DAT") {
      continue;
    }
    targets.IndirectExternal.emplace(relocation.Address,
                                     externalNameFor(relocation.SymbolName));
  }

  return targets;
}

bool moduleVerifies(const llvm::Module &module, std::string &message) {
  std::string buffer;
  llvm::raw_string_ostream stream(buffer);
  bool failed = llvm::verifyModule(module, &stream);
  stream.flush();
  if (failed) {
    message = buffer;
  }
  return !failed;
}

void attachMemoryMapMetadata(
    llvm::Module &module, const notdec::bin2llvm::NativeProgramState &state) {
  llvm::LLVMContext &context = module.getContext();
  std::vector<llvm::Metadata *> entries;
  for (const notdec::bin2llvm::NativeMemoryRange &range :
       state.memoryRanges()) {
    std::vector<llvm::Metadata *> fields = {
        llvm::MDString::get(context, "start=0x" + hexAddress(range.Start)),
        llvm::MDString::get(context, "end=0x" + hexAddress(range.end())),
        llvm::MDString::get(context,
                            std::string("read=") +
                                (range.Readable ? "true" : "false")),
        llvm::MDString::get(context,
                            std::string("write=") +
                                (range.Writable ? "true" : "false")),
        llvm::MDString::get(context,
                            std::string("execute=") +
                                (range.Executable ? "true" : "false")),
    };
    entries.push_back(llvm::MDNode::get(context, fields));
  }
  module.getOrInsertNamedMetadata("notdec.memory_map")->addOperand(
      llvm::MDNode::get(context, entries));
}

std::unique_ptr<llvm::Module> buildConfirmedModule(
    llvm::LLVMContext &context,
    const notdec::bin2llvm::NativeProgramState &state,
    notdec::bin2llvm::LiefElfLoadImage &loadImage,
    const notdec::bin2llvm::SleighSpecOptions &specOptions,
    notdec::bin2llvm::PcodeMemoryModel memoryModel,
    std::string &errorMessage) {
  auto module =
      std::make_unique<llvm::Module>("notdec.bin2llvm.native.confirmed", context);

  NativeCallTargets callTargets = planNativeCallTargets(state);

  unsigned appended = 0;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    if (function.RangeEnd <= function.Entry) {
      continue;
    }

    auto program = notdec::bin2llvm::collectSleighPcodeRanges(
        loadImage, specOptions, blockRanges(function), std::cerr);
    if (program.Ops.empty()) {
      std::cerr << "skip native function 0x" << std::hex << function.Entry
                << std::dec << ": empty p-code\n";
      continue;
    }

    notdec::bin2llvm::PcodeLoweringConfig config;
    config.ModuleName = "notdec.bin2llvm.native.confirmed.check";
    config.MemoryModel = memoryModel;
    auto nameIt = callTargets.Direct.find(function.Entry);
    if (nameIt == callTargets.Direct.end()) {
      continue;
    }
    config.EntryFunctionName = nameIt->second;
    config.DirectCallTargets = callTargets.Direct;
    config.ExternalCallTargets = callTargets.External;
    config.IndirectExternalCallTargets = callTargets.IndirectExternal;
    config.BlockSuccessors = blockSuccessors(function);

    llvm::LLVMContext checkContext;
    std::string checkError;
    auto checkModule = notdec::bin2llvm::buildPcodeModule(
        checkContext, program, config, checkError);
    if (!checkModule) {
      std::cerr << "skip native function 0x" << std::hex << function.Entry
                << std::dec << ": " << checkError << '\n';
      continue;
    }
    std::string verifyMessage;
    if (!moduleVerifies(*checkModule, verifyMessage)) {
      std::cerr << "skip native function 0x" << std::hex << function.Entry
                << std::dec << ": " << verifyMessage;
      continue;
    }

    if (!notdec::bin2llvm::appendPcodeFunction(context, *module, program,
                                               config, errorMessage)) {
      return nullptr;
    }
    ++appended;
  }

  if (appended == 0) {
    errorMessage = "no confirmed native functions lowered successfully";
    return nullptr;
  }
  return module;
}

int writeModule(const llvm::Module &module, const std::string &outputPath) {
  std::error_code errorCode;
  llvm::raw_fd_ostream output(outputPath, errorCode);
  if (errorCode) {
    std::cerr << "failed to open output file: " << outputPath << ": "
              << errorCode.message() << '\n';
    return 1;
  }

  module.print(output, nullptr);
  return 0;
}

std::unique_ptr<llvm::Module> readIRModule(const std::string &inputPath,
                                           llvm::LLVMContext &context,
                                           std::string &errorMessage) {
  llvm::SMDiagnostic diagnostic;
  std::unique_ptr<llvm::Module> module =
      llvm::parseIRFile(inputPath, diagnostic, context);
  if (module) {
    return module;
  }

  std::string message;
  llvm::raw_string_ostream stream(message);
  diagnostic.print("notdec-native-llvm", stream);
  stream.flush();
  errorMessage = message;
  return nullptr;
}

bool runRegisterSSAPassIfEnabled(llvm::Module &module,
                                 const CliOptions &options) {
  if (options.DisableRegisterSSAPass) {
    return true;
  }
  notdec::bin2llvm::NativeRegisterSSAOptions passOptions;
  passOptions.EnableRewrite = true;
  passOptions.PrintSummary = options.PrintRegisterSSASummary;
  notdec::bin2llvm::runNativeRegisterSSA(module, passOptions);
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after register SSA pass\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  try {
    auto options = parseArgs(argc, argv);
    if (!options) {
      printUsage(argv[0]);
      return 1;
    }

    if (isIRInputPath(options->ElfPath)) {
      llvm::LLVMContext context;
      std::string errorMessage;
      std::unique_ptr<llvm::Module> module =
          readIRModule(options->ElfPath, context, errorMessage);
      if (!module) {
        std::cerr << "failed to parse IR input: " << errorMessage << '\n';
        return 1;
      }
      if (!runRegisterSSAPassIfEnabled(*module, *options)) {
        return 1;
      }
      return writeModule(*module, options->OutputPath);
    }

    std::unique_ptr<LIEF::ELF::Binary> binary =
        LIEF::ELF::Parser::parse(options->ElfPath);
    if (!binary) {
      std::cerr << "failed to parse ELF: " << options->ElfPath << '\n';
      return 1;
    }
    if (!resolveSpecOptions(*binary, options->SpecOptions)) {
      return 1;
    }
    std::unique_ptr<notdec::bin2llvm::NativeProgramState> selectedState;
    if (options->AllConfirmed || options->FunctionEntry ||
        !options->FunctionName.empty()) {
      selectedState =
          std::make_unique<notdec::bin2llvm::NativeProgramState>(
              runNativeDiscovery(*binary, options->DecodeOptions));
    }
    if (selectedState && !options->SummaryJsonPath.empty() &&
        !writeSummaryJson(*selectedState, options->SummaryJsonPath)) {
      return 1;
    }
    if (!selectedState && !options->SummaryJsonPath.empty()) {
      std::cerr << "--summary-json-out requires -f, -n, or --all-confirmed\n";
      return 1;
    }
    if (options->FunctionEntry || !options->FunctionName.empty()) {
      if (!resolveFunctionRange(*selectedState, *options)) {
        return 1;
      }
    }

    notdec::bin2llvm::LiefElfLoadImage loadImage(*binary, std::cerr);
    if (!loadImage.hasExecutableBytes()) {
      return 1;
    }

    llvm::LLVMContext context;
    std::string errorMessage;
    std::unique_ptr<llvm::Module> module;
    if (options->AllConfirmed) {
      module = buildConfirmedModule(context, *selectedState, loadImage,
                                    options->SpecOptions, options->MemoryModel,
                                    errorMessage);
    } else {
      notdec::bin2llvm::PcodeProgram program;
      if (!options->FunctionBlockRanges.empty()) {
        program = notdec::bin2llvm::collectSleighPcodeRanges(
            loadImage, options->SpecOptions, options->FunctionBlockRanges,
            std::cerr);
      } else {
        program = notdec::bin2llvm::collectSleighPcode(
            loadImage, options->SpecOptions, options->Address, options->Length,
            std::cerr);
      }
      if (program.Ops.empty()) {
        return 1;
      }

      notdec::bin2llvm::PcodeLoweringConfig config;
      config.MemoryModel = options->MemoryModel;
      if (!options->FunctionName.empty()) {
        config.EntryFunctionName =
            sanitizeLlvmFunctionName(options->FunctionName);
      } else if (options->FunctionEntry) {
        config.EntryFunctionName = entryFunctionName(*options->FunctionEntry);
      }
      if (options->FunctionEntry) {
        NativeCallTargets callTargets =
            selectedState ? planNativeCallTargets(*selectedState)
                          : planNativeCallTargets(runNativeDiscovery(
                                *binary, options->DecodeOptions));
        callTargets.Direct[*options->FunctionEntry] = config.EntryFunctionName;
        config.DirectCallTargets = std::move(callTargets.Direct);
        config.ExternalCallTargets = std::move(callTargets.External);
        config.IndirectExternalCallTargets =
            std::move(callTargets.IndirectExternal);
        config.BlockSuccessors = options->FunctionBlockSuccessors;
      }
      module = notdec::bin2llvm::buildPcodeModule(context, program, config,
                                                  errorMessage);
    }
    if (!module) {
      std::cerr << "failed to lower p-code: " << errorMessage << '\n';
      return 1;
    }

    if (selectedState) {
      attachMemoryMapMetadata(*module, *selectedState);
    } else {
      notdec::bin2llvm::NativeProgramState memoryState(*binary);
      attachMemoryMapMetadata(*module, memoryState);
    }

    if (llvm::verifyModule(*module, &llvm::errs())) {
      std::cerr << "module verification failed\n";
      return 1;
    }
    if (!runRegisterSSAPassIfEnabled(*module, *options)) {
      return 1;
    }

    return writeModule(*module, options->OutputPath);
  } catch (const ghidra::LowlevelError &error) {
    std::cerr << "libsla error: " << error.explain << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
