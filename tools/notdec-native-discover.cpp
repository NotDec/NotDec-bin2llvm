#include "notdec-bin2llvm/NativeAnalysis.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Keep CLI output modes explicit.  Query commands share the same analysis
// pipeline; only the final formatter changes.
enum class OutputMode {
  TextReport,
  SummaryJson,
  MemoryJson,
  RelocationsJson,
  NotesJson,
  EhFrameJson,
  SeedsJson,
  DiscoveryJson,
  FunctionJson,
  FunctionXrefsJson,
  FunctionsJson,
  BlocksJson,
  BlockJson,
  CfgJson,
  CfgDot,
  CallgraphJson,
  CallgraphDot,
  XrefsJson,
  XrefsDot,
  InstructionsJson,
  InstructionsRangeJson,
  InstructionsFunctionJson,
  PltJson,
  UnresolvedJson,
  XrefsKindJson,
  XrefsFromJson,
  XrefsToJson,
};

struct CliOptions {
  std::string ElfPath;
  OutputMode Mode = OutputMode::TextReport;
  notdec::bin2llvm::NativeDecodeMode DecodeMode =
      notdec::bin2llvm::NativeDecodeMode::Gtirb;
  notdec::bin2llvm::NativeSleighDecodeOptions DecodeOptions;
  notdec::bin2llvm::NativeGtirbDecodeOptions GtirbOptions;
  std::optional<uint64_t> QueryAddress;
  std::optional<uint64_t> QueryStart;
  std::optional<uint64_t> QueryEnd;
  std::optional<uint64_t> QueryFunctionEntry;
  std::optional<notdec::bin2llvm::NativeXrefKind> QueryXrefKind;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " <elf-file>\n";
  std::cerr << "       " << argv0
            << " [--native-decode-mode gtirb|internal] [--gtirb <path>] "
               "[--decode-seed-limit <count>] <mode> <elf-file>\n";
  std::cerr << "       " << argv0 << " --summary-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --memory-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --relocations-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --notes-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --eh-frame-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --seeds-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --discovery-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --function-json <entry> <elf-file>\n";
  std::cerr << "       " << argv0
            << " --function-xrefs-json <entry> <elf-file>\n";
  std::cerr << "       " << argv0 << " --functions-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --blocks-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --block-json <start> <elf-file>\n";
  std::cerr << "       " << argv0 << " --cfg-json <entry> <elf-file>\n";
  std::cerr << "       " << argv0 << " --cfg-dot <entry> <elf-file>\n";
  std::cerr << "       " << argv0 << " --callgraph-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --callgraph-dot <elf-file>\n";
  std::cerr << "       " << argv0 << " --xrefs-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --xrefs-dot <elf-file>\n";
  std::cerr << "       " << argv0 << " --instructions-json <elf-file>\n";
  std::cerr << "       " << argv0
            << " --instructions-range-json <start> <end> <elf-file>\n";
  std::cerr << "       " << argv0
            << " --instructions-function-json <entry> <elf-file>\n";
  std::cerr << "       " << argv0 << " --plt-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --unresolved-json <elf-file>\n";
  std::cerr << "       " << argv0
            << " --xrefs-kind-json <flow|call|data|string> <elf-file>\n";
  std::cerr << "       " << argv0 << " --xrefs-from-json <addr> <elf-file>\n";
  std::cerr << "       " << argv0 << " --xrefs-to-json <addr> <elf-file>\n";
}

std::optional<uint64_t> parseAddress(const std::string &text) {
  try {
    size_t parsedLength = 0;
    uint64_t value = std::stoull(text, &parsedLength, 0);
    if (parsedLength != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::optional<notdec::bin2llvm::NativeXrefKind>
parseXrefKind(const std::string &text) {
  using notdec::bin2llvm::NativeXrefKind;

  if (text == "flow") {
    return NativeXrefKind::Flow;
  }
  if (text == "call") {
    return NativeXrefKind::Call;
  }
  if (text == "data") {
    return NativeXrefKind::Data;
  }
  if (text == "string") {
    return NativeXrefKind::String;
  }
  return std::nullopt;
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  CliOptions options;
  std::vector<std::string> args;
  for (int index = 1; index < argc; ++index) {
    std::string arg = argv[index];
    if (arg == "--decode-seed-limit") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      std::optional<uint64_t> limit = parseAddress(argv[++index]);
      if (!limit) {
        return std::nullopt;
      }
      options.DecodeOptions.MaxDecodedSeeds = *limit;
      continue;
    }
    if (arg == "--native-decode-mode") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      std::string mode = argv[++index];
      if (mode == "gtirb") {
        options.DecodeMode = notdec::bin2llvm::NativeDecodeMode::Gtirb;
      } else if (mode == "internal") {
        options.DecodeMode = notdec::bin2llvm::NativeDecodeMode::Internal;
      } else {
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--gtirb") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      options.GtirbOptions.GtirbPath = argv[++index];
      continue;
    }
    if (arg == "--ddisasm") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      options.GtirbOptions.DdisasmPath = argv[++index];
      continue;
    }
    args.push_back(std::move(arg));
  }

  int effectiveArgc = static_cast<int>(args.size()) + 1;
  if (effectiveArgc != 2 && effectiveArgc != 3 && effectiveArgc != 4 &&
      effectiveArgc != 5) {
    return std::nullopt;
  }

  if (effectiveArgc == 2) {
    options.ElfPath = args[0];
    return options;
  }

  std::string mode = args[0];
  if (effectiveArgc == 5) {
    if (mode != "--instructions-range-json") {
      return std::nullopt;
    }
    options.Mode = OutputMode::InstructionsRangeJson;
    options.QueryStart = parseAddress(args[1]);
    options.QueryEnd = parseAddress(args[2]);
    if (!options.QueryStart || !options.QueryEnd) {
      return std::nullopt;
    }
    options.ElfPath = args[3];
    return options;
  }

  if (effectiveArgc == 4) {
    if (mode == "--xrefs-from-json") {
      options.Mode = OutputMode::XrefsFromJson;
    } else if (mode == "--xrefs-to-json") {
      options.Mode = OutputMode::XrefsToJson;
    } else if (mode == "--xrefs-kind-json") {
      options.Mode = OutputMode::XrefsKindJson;
    } else if (mode == "--instructions-function-json") {
      options.Mode = OutputMode::InstructionsFunctionJson;
    } else if (mode == "--function-json") {
      options.Mode = OutputMode::FunctionJson;
    } else if (mode == "--function-xrefs-json") {
      options.Mode = OutputMode::FunctionXrefsJson;
    } else if (mode == "--block-json") {
      options.Mode = OutputMode::BlockJson;
    } else if (mode == "--cfg-json") {
      options.Mode = OutputMode::CfgJson;
    } else if (mode == "--cfg-dot") {
      options.Mode = OutputMode::CfgDot;
    } else {
      return std::nullopt;
    }
    if (options.Mode == OutputMode::XrefsKindJson) {
      options.QueryXrefKind = parseXrefKind(args[1]);
      if (!options.QueryXrefKind) {
        return std::nullopt;
      }
      options.ElfPath = args[2];
      return options;
    }
    std::optional<uint64_t> address = parseAddress(args[1]);
    if (!address) {
      return std::nullopt;
    }
    if (options.Mode == OutputMode::InstructionsFunctionJson) {
      options.QueryFunctionEntry = *address;
    } else if (options.Mode == OutputMode::FunctionJson) {
      options.QueryFunctionEntry = *address;
    } else if (options.Mode == OutputMode::FunctionXrefsJson) {
      options.QueryFunctionEntry = *address;
    } else if (options.Mode == OutputMode::CfgJson) {
      options.QueryFunctionEntry = *address;
    } else if (options.Mode == OutputMode::CfgDot) {
      options.QueryFunctionEntry = *address;
    } else if (options.Mode == OutputMode::BlockJson) {
      options.QueryAddress = *address;
    } else {
      options.QueryAddress = *address;
    }
    if (options.QueryFunctionEntry) {
      options.DecodeOptions.InitialFunctionEntries.push_back(
          *options.QueryFunctionEntry);
    }
    options.ElfPath = args[2];
    return options;
  }

  if (mode == "--summary-json") {
    options.Mode = OutputMode::SummaryJson;
  } else if (mode == "--memory-json") {
    options.Mode = OutputMode::MemoryJson;
  } else if (mode == "--relocations-json") {
    options.Mode = OutputMode::RelocationsJson;
  } else if (mode == "--notes-json") {
    options.Mode = OutputMode::NotesJson;
  } else if (mode == "--eh-frame-json") {
    options.Mode = OutputMode::EhFrameJson;
  } else if (mode == "--seeds-json") {
    options.Mode = OutputMode::SeedsJson;
  } else if (mode == "--discovery-json") {
    options.Mode = OutputMode::DiscoveryJson;
  } else if (mode == "--functions-json") {
    options.Mode = OutputMode::FunctionsJson;
  } else if (mode == "--blocks-json") {
    options.Mode = OutputMode::BlocksJson;
  } else if (mode == "--callgraph-json") {
    options.Mode = OutputMode::CallgraphJson;
  } else if (mode == "--callgraph-dot") {
    options.Mode = OutputMode::CallgraphDot;
  } else if (mode == "--xrefs-json") {
    options.Mode = OutputMode::XrefsJson;
  } else if (mode == "--xrefs-dot") {
    options.Mode = OutputMode::XrefsDot;
  } else if (mode == "--instructions-json") {
    options.Mode = OutputMode::InstructionsJson;
  } else if (mode == "--plt-json") {
    options.Mode = OutputMode::PltJson;
  } else if (mode == "--unresolved-json") {
    options.Mode = OutputMode::UnresolvedJson;
  } else {
    return std::nullopt;
  }
  options.ElfPath = args[1];
  return options;
}

std::string hexString(uint64_t value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << value;
  return stream.str();
}

std::string bytesHex(const std::vector<uint8_t> &bytes) {
  std::ostringstream stream;
  stream << std::hex;
  for (uint8_t byte : bytes) {
    stream.width(2);
    stream.fill('0');
    stream << static_cast<unsigned>(byte);
  }
  return stream.str();
}

std::string jsonEscape(const std::string &text) {
  std::string escaped;
  for (char ch : text) {
    switch (ch) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
}

std::string dotEscape(const std::string &text) {
  std::string escaped;
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

uint64_t countBasicBlocks(const notdec::bin2llvm::NativeProgramState &state) {
  uint64_t count = 0;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    count += function.Blocks.size();
  }
  return count;
}

std::vector<const notdec::bin2llvm::NativeInstruction *>
instructionsInBlocks(const notdec::bin2llvm::NativeProgramState &state,
                     const notdec::bin2llvm::NativeFunction &function) {
  std::vector<const notdec::bin2llvm::NativeInstruction *> instructions;
  for (const notdec::bin2llvm::NativeBasicBlock &block : function.Blocks) {
    std::vector<const notdec::bin2llvm::NativeInstruction *> blockInstructions =
        state.instructionsInRange(block.Start, block.End);
    instructions.insert(instructions.end(), blockInstructions.begin(),
                        blockInstructions.end());
  }
  return instructions;
}

void printSummaryJson(std::ostream &output,
                      const notdec::bin2llvm::NativeProgramState &state) {
  using namespace notdec::bin2llvm;

  std::map<NativeXrefKind, uint64_t> xrefKindCounts;
  for (const NativeXref &xref : state.xrefs()) {
    ++xrefKindCounts[xref.Kind];
  }

  std::map<NativeUnresolvedFlowKind, uint64_t> unresolvedFlowCounts;
  for (const NativeUnresolvedFlow &flow : state.unresolvedFlows()) {
    ++unresolvedFlowCounts[flow.Kind];
  }

  std::map<NativeFunctionConfidence, uint64_t> confidenceCounts;
  for (const auto &[address, seed] : state.functionSeeds()) {
    (void)address;
    ++confidenceCounts[seed.Confidence];
  }

  output << "{\n";
  output << "  \"function_seeds\": " << state.functionSeeds().size() << ",\n";
  output << "  \"function_worklist\": " << state.functionWorklist().size()
         << ",\n";

  output << "  \"sources\": {";
  bool firstSource = true;
  for (const auto &[source, count] : state.sourceCounts()) {
    output << (firstSource ? "\n" : ",\n");
    output << "    \"" << jsonEscape(source) << "\": " << count;
    firstSource = false;
  }
  output << (firstSource ? "},\n" : "\n  },\n");

  output << "  \"confidence\": {\n";
  for (NativeFunctionConfidence confidence :
       {NativeFunctionConfidence::High, NativeFunctionConfidence::Medium,
        NativeFunctionConfidence::Low}) {
    output << "    \"" << toString(confidence) << "\": "
           << confidenceCounts[confidence]
           << (confidence == NativeFunctionConfidence::Low ? "\n" : ",\n");
  }
  output << "  },\n";

  output << "  \"confirmed_functions\": " << state.functions().size()
         << ",\n";
  output << "  \"basic_blocks\": " << countBasicBlocks(state) << ",\n";
  output << "  \"instructions\": " << state.instructions().size() << ",\n";

  output << "  \"xrefs\": {\n";
  output << "    \"total\": " << state.xrefs().size() << ",\n";
  for (NativeXrefKind kind : {NativeXrefKind::Flow, NativeXrefKind::Call,
                              NativeXrefKind::Data, NativeXrefKind::String}) {
    output << "    \"" << toString(kind) << "\": " << xrefKindCounts[kind]
           << (kind == NativeXrefKind::String ? "\n" : ",\n");
  }
  output << "  },\n";

  output << "  \"unresolved_indirect_flows\": {\n";
  output << "    \"total\": " << state.unresolvedFlows().size() << ",\n";
  output << "    \"" << toString(NativeUnresolvedFlowKind::IndirectCall)
         << "\": "
         << unresolvedFlowCounts[NativeUnresolvedFlowKind::IndirectCall]
         << ",\n";
  output << "    \"" << toString(NativeUnresolvedFlowKind::IndirectBranch)
         << "\": "
         << unresolvedFlowCounts[NativeUnresolvedFlowKind::IndirectBranch]
         << ",\n";
  output << "    \""
         << toString(NativeUnresolvedFlowKind::IndirectTailBranch) << "\": "
         << unresolvedFlowCounts[NativeUnresolvedFlowKind::IndirectTailBranch]
         << "\n";
  output << "  }\n";
  output << "}\n";
}

void printStringArray(std::ostream &output,
                      const std::vector<std::string> &values) {
  output << "[";
  for (size_t index = 0; index < values.size(); ++index) {
    output << (index == 0 ? "" : ", ");
    output << "\"" << jsonEscape(values[index]) << "\"";
  }
  output << "]";
}

void printMemoryJson(std::ostream &output,
                     const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"pointer_size\": " << static_cast<unsigned>(state.pointerSize())
         << ",\n";
  output << "  \"ranges\": [";
  bool firstRange = true;
  for (const notdec::bin2llvm::NativeMemoryRange &range :
       state.memoryRanges()) {
    output << (firstRange ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"start\": \"" << hexString(range.Start) << "\",\n";
    output << "      \"end\": \"" << hexString(range.end()) << "\",\n";
    output << "      \"size\": " << range.Size << ",\n";
    output << "      \"loaded_size\": " << range.Bytes.size() << ",\n";
    output << "      \"readable\": " << (range.Readable ? "true" : "false")
           << ",\n";
    output << "      \"writable\": " << (range.Writable ? "true" : "false")
           << ",\n";
    output << "      \"executable\": "
           << (range.Executable ? "true" : "false") << "\n";
    output << "    }";
    firstRange = false;
  }
  output << (firstRange ? "],\n" : "\n  ],\n");

  output << "  \"sections\": [";
  bool firstSection = true;
  for (const notdec::bin2llvm::NativeSectionInfo &section :
       state.sections()) {
    output << (firstSection ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"name\": \"" << jsonEscape(section.Name) << "\",\n";
    output << "      \"address\": \"" << hexString(section.Address) << "\",\n";
    output << "      \"end\": \"" << hexString(section.Address + section.Size)
           << "\",\n";
    output << "      \"size\": " << section.Size << ",\n";
    output << "      \"executable\": "
           << (section.Executable ? "true" : "false") << "\n";
    output << "    }";
    firstSection = false;
  }
  output << (firstSection ? "],\n" : "\n  ],\n");
  output << "  \"ranges_count\": " << state.memoryRanges().size() << ",\n";
  output << "  \"sections_count\": " << state.sections().size() << "\n";
  output << "}\n";
}

void printRelocationsJson(std::ostream &output,
                          const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"relocations\": [";
  bool firstRelocation = true;
  for (const notdec::bin2llvm::NativeRelocationInfo &relocation :
       state.relocations()) {
    output << (firstRelocation ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"address\": \"" << hexString(relocation.Address)
           << "\",\n";
    output << "      \"type\": " << relocation.Type << ",\n";
    output << "      \"type_name\": \"" << jsonEscape(relocation.TypeName)
           << "\",\n";
    output << "      \"symbol\": \"" << jsonEscape(relocation.SymbolName)
           << "\",\n";
    output << "      \"symbol_value\": \""
           << hexString(relocation.SymbolValue) << "\",\n";
    output << "      \"addend\": " << relocation.Addend << ",\n";
    output << "      \"table\": \"" << jsonEscape(relocation.TableKind)
           << "\",\n";
    output << "      \"status\": \"" << jsonEscape(relocation.Status)
           << "\",\n";
    output << "      \"computed_value\": ";
    if (relocation.ComputedValue) {
      output << "\"" << hexString(*relocation.ComputedValue) << "\"\n";
    } else {
      output << "null\n";
    }
    output << "    }";
    firstRelocation = false;
  }
  output << (firstRelocation ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.relocations().size() << "\n";
  output << "}\n";
}

void printNotesJson(std::ostream &output,
                    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"notes\": ";
  printStringArray(output, state.notes());
  output << ",\n";
  output << "  \"count\": " << state.notes().size() << "\n";
  output << "}\n";
}

void printEhFrameJson(std::ostream &output,
                      const notdec::bin2llvm::NativeProgramState &state) {
  const notdec::bin2llvm::NativeEhFrameStats &stats = state.ehFrameStats();

  output << "{\n";
  output << "  \"has_eh_frame_hdr\": "
         << (stats.HasEhFrameHdr ? "true" : "false") << ",\n";
  output << "  \"has_eh_frame\": " << (stats.HasEhFrame ? "true" : "false")
         << ",\n";
  output << "  \"parsed_eh_frame_hdr\": "
         << (stats.ParsedEhFrameHdr ? "true" : "false") << ",\n";
  output << "  \"cie_count\": " << stats.CieCount << ",\n";
  output << "  \"fde_count\": " << stats.FdeCount << ",\n";
  output << "  \"parsed_fde_count\": " << stats.ParsedFdeCount << ",\n";
  output << "  \"added_seed_count\": " << stats.AddedSeedCount << ",\n";
  output << "  \"overlapped_seed_count\": " << stats.OverlappedSeedCount
         << ",\n";
  output << "  \"hdr_fde_count\": " << stats.HdrFdeCount << ",\n";
  output << "  \"hdr_table_entries\": " << stats.HdrTableEntries << ",\n";
  output << "  \"hdr_matched_starts\": " << stats.HdrMatchedStarts << ",\n";
  output << "  \"hdr_missing_in_frame\": " << stats.HdrMissingInFrame
         << ",\n";
  output << "  \"hdr_extra_frame_fdes\": " << stats.HdrExtraFrameFdes
         << ",\n";
  output << "  \"hdr_fde_address_matches\": " << stats.HdrFdeAddressMatches
         << ",\n";
  output << "  \"hdr_fde_address_mismatches\": "
         << stats.HdrFdeAddressMismatches << ",\n";
  output << "  \"hdr_invalid_count\": " << stats.HdrInvalidCount << ",\n";
  output << "  \"hdr_unsupported_count\": " << stats.HdrUnsupportedCount
         << ",\n";
  output << "  \"invalid_count\": " << stats.InvalidCount << ",\n";
  output << "  \"unsupported_count\": " << stats.UnsupportedCount << ",\n";

  output << "  \"frame_fdes\": [";
  bool firstFrameFde = true;
  for (const notdec::bin2llvm::NativeEhFrameFdeInfo &fde :
       stats.FrameFdes) {
    output << (firstFrameFde ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"pc_begin\": \"" << hexString(fde.PcBegin) << "\",\n";
    output << "      \"fde_address\": \"" << hexString(fde.FdeAddress)
           << "\"\n";
    output << "    }";
    firstFrameFde = false;
  }
  output << (firstFrameFde ? "],\n" : "\n  ],\n");
  output << "  \"frame_fdes_count\": " << stats.FrameFdes.size() << ",\n";

  output << "  \"hdr_entries\": [";
  bool firstHdrEntry = true;
  for (const notdec::bin2llvm::NativeEhFrameHdrEntry &entry :
       stats.HdrEntries) {
    output << (firstHdrEntry ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"initial_location\": \""
           << hexString(entry.InitialLocation) << "\",\n";
    output << "      \"fde_address\": \"" << hexString(entry.FdeAddress)
           << "\"\n";
    output << "    }";
    firstHdrEntry = false;
  }
  output << (firstHdrEntry ? "],\n" : "\n  ],\n");
  output << "  \"hdr_entries_count\": " << stats.HdrEntries.size() << ",\n";
  output << "  \"hdr_mismatch_samples\": ";
  printStringArray(output, stats.HdrMismatchSamples);
  output << ",\n";
  output << "  \"unsupported_samples\": ";
  printStringArray(output, stats.UnsupportedSamples);
  output << "\n";
  output << "}\n";
}

void printSeedsArrayJson(std::ostream &output,
                         const notdec::bin2llvm::NativeProgramState &state,
                         const std::string &indent) {
  output << "[";
  bool firstSeed = true;
  for (const auto &[address, seed] : state.functionSeeds()) {
    (void)address;
    output << (firstSeed ? "\n" : ",\n");
    output << indent << "{\n";
    output << indent << "  \"address\": \"" << hexString(seed.Address)
           << "\",\n";
    output << indent << "  \"size\": " << seed.Size << ",\n";
    output << indent << "  \"range_start\": \"" << hexString(seed.RangeStart)
           << "\",\n";
    output << indent << "  \"range_end\": \"" << hexString(seed.RangeEnd)
           << "\",\n";
    output << indent << "  \"range_source\": \"" << jsonEscape(seed.RangeSource)
           << "\",\n";
    output << indent << "  \"primary_name\": \"" << jsonEscape(seed.PrimaryName)
           << "\",\n";
    output << indent << "  \"aliases\": ";
    printStringArray(output, seed.Aliases);
    output << ",\n";
    output << indent << "  \"sources\": ";
    printStringArray(output, seed.Sources);
    output << ",\n";
    output << indent << "  \"confidence\": \""
           << notdec::bin2llvm::toString(seed.Confidence) << "\"\n";
    output << indent << "}";
    firstSeed = false;
  }
  output << (firstSeed ? "]" : "\n" + indent.substr(0, indent.size() - 2) + "]");
}

void printSeedsJson(std::ostream &output,
                    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"seeds\": ";
  printSeedsArrayJson(output, state, "    ");
  output << ",\n";
  output << "  \"count\": " << state.functionSeeds().size() << "\n";
  output << "}\n";
}

void printFunctionsArrayJson(
    std::ostream &output,
    const notdec::bin2llvm::NativeProgramState &state,
    const std::string &indent) {
  output << "[";
  bool firstFunction = true;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    output << (firstFunction ? "\n" : ",\n");
    output << indent << "{\n";
    output << indent << "  \"entry\": \"" << hexString(function.Entry)
           << "\",\n";
    output << indent << "  \"range_start\": \"" << hexString(function.RangeStart)
           << "\",\n";
    output << indent << "  \"range_end\": \"" << hexString(function.RangeEnd)
           << "\",\n";
    output << indent << "  \"name\": \"" << jsonEscape(function.Name)
           << "\",\n";
    output << indent << "  \"source\": \"" << jsonEscape(function.Source)
           << "\",\n";
    output << indent << "  \"block_count\": " << function.Blocks.size()
           << "\n";
    output << indent << "}";
    firstFunction = false;
  }
  output << (firstFunction ? "]" : "\n" + indent.substr(0, indent.size() - 2) + "]");
}

void printFunctionsJson(std::ostream &output,
                        const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"functions\": ";
  printFunctionsArrayJson(output, state, "    ");
  output << ",\n";
  output << "  \"count\": " << state.functions().size() << "\n";
  output << "}\n";
}

void printDiscoveryJson(std::ostream &output,
                        const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"seeds\": ";
  printSeedsArrayJson(output, state, "    ");
  output << ",\n";
  output << "  \"seed_count\": " << state.functionSeeds().size() << ",\n";
  output << "  \"functions\": ";
  printFunctionsArrayJson(output, state, "    ");
  output << ",\n";
  output << "  \"function_count\": " << state.functions().size() << "\n";
  output << "}\n";
}

void printFunctionJson(std::ostream &output,
                       const notdec::bin2llvm::NativeProgramState &state,
                       uint64_t entry) {
  using namespace notdec::bin2llvm;

  const NativeFunction *function = state.functionAt(entry);
  output << "{\n";
  output << "  \"query\": \"" << hexString(entry) << "\",\n";
  if (function == nullptr) {
    output << "  \"found\": false\n";
    output << "}\n";
    return;
  }

  uint64_t outgoingXrefs = 0;
  for (const NativeXref &xref : state.xrefs()) {
    const NativeFunction *owner = state.functionContaining(xref.From);
    if (owner != nullptr && owner->Entry == function->Entry) {
      ++outgoingXrefs;
    }
  }
  std::vector<const NativeInstruction *> instructions =
      instructionsInBlocks(state, *function);
  std::vector<const NativeXref *> incomingEntryXrefs =
      state.xrefsTo(function->Entry);

  output << "  \"found\": true,\n";
  output << "  \"function\": {\n";
  output << "    \"entry\": \"" << hexString(function->Entry) << "\",\n";
  output << "    \"range_start\": \"" << hexString(function->RangeStart)
         << "\",\n";
  output << "    \"range_end\": \"" << hexString(function->RangeEnd)
         << "\",\n";
  output << "    \"size\": " << (function->RangeEnd - function->RangeStart)
         << ",\n";
  output << "    \"name\": \"" << jsonEscape(function->Name) << "\",\n";
  output << "    \"source\": \"" << jsonEscape(function->Source) << "\",\n";
  output << "    \"block_count\": " << function->Blocks.size() << ",\n";
  output << "    \"instruction_count\": " << instructions.size() << ",\n";
  output << "    \"outgoing_xref_count\": " << outgoingXrefs << ",\n";
  output << "    \"incoming_entry_xref_count\": "
         << incomingEntryXrefs.size() << "\n";
  output << "  }\n";
  output << "}\n";
}

void printAddressArray(std::ostream &output,
                       const std::vector<uint64_t> &addresses) {
  output << "[";
  for (size_t index = 0; index < addresses.size(); ++index) {
    output << (index == 0 ? "" : ", ");
    output << "\"" << hexString(addresses[index]) << "\"";
  }
  output << "]";
}

void printBlocksJson(std::ostream &output,
                     const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"blocks\": [";
  bool firstBlock = true;
  uint64_t blockCount = 0;
  for (const auto &[entry, function] : state.functions()) {
    for (const notdec::bin2llvm::NativeBasicBlock &block : function.Blocks) {
      output << (firstBlock ? "\n" : ",\n");
      output << "    {\n";
      output << "      \"function_entry\": \"" << hexString(entry) << "\",\n";
      output << "      \"start\": \"" << hexString(block.Start) << "\",\n";
      output << "      \"end\": \"" << hexString(block.End) << "\",\n";
      output << "      \"size\": " << (block.End - block.Start) << ",\n";
      output << "      \"successors\": ";
      printAddressArray(output, block.Successors);
      output << "\n";
      output << "    }";
      firstBlock = false;
      ++blockCount;
    }
  }
  output << (firstBlock ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << blockCount << "\n";
  output << "}\n";
}

void printInstructionObject(
    std::ostream &output,
    const notdec::bin2llvm::NativeInstruction &instruction,
    const char *indent);

void printBlockJson(std::ostream &output,
                    const notdec::bin2llvm::NativeProgramState &state,
                    uint64_t start) {
  const notdec::bin2llvm::NativeFunction *matchedFunction = nullptr;
  const notdec::bin2llvm::NativeBasicBlock *matchedBlock = nullptr;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    for (const notdec::bin2llvm::NativeBasicBlock &block : function.Blocks) {
      if (block.Start == start) {
        matchedFunction = &function;
        matchedBlock = &block;
        break;
      }
    }
    if (matchedBlock != nullptr) {
      break;
    }
  }

  std::vector<const notdec::bin2llvm::NativeInstruction *> instructions;
  output << "{\n";
  output << "  \"query\": \"" << hexString(start) << "\",\n";
  if (matchedBlock == nullptr) {
    output << "  \"found\": false,\n";
    output << "  \"instructions\": [],\n";
    output << "  \"instruction_count\": 0\n";
    output << "}\n";
    return;
  }

  instructions =
      state.instructionsInRange(matchedBlock->Start, matchedBlock->End);
  output << "  \"found\": true,\n";
  output << "  \"function_entry\": \"" << hexString(matchedFunction->Entry)
         << "\",\n";
  output << "  \"block\": {\n";
  output << "    \"start\": \"" << hexString(matchedBlock->Start) << "\",\n";
  output << "    \"end\": \"" << hexString(matchedBlock->End) << "\",\n";
  output << "    \"size\": " << (matchedBlock->End - matchedBlock->Start)
         << ",\n";
  output << "    \"successors\": ";
  printAddressArray(output, matchedBlock->Successors);
  output << "\n";
  output << "  },\n";
  output << "  \"instructions\": [";
  bool firstInstruction = true;
  for (const notdec::bin2llvm::NativeInstruction *instruction : instructions) {
    output << (firstInstruction ? "\n" : ",\n");
    printInstructionObject(output, *instruction, "    ");
    firstInstruction = false;
  }
  output << (firstInstruction ? "],\n" : "\n  ],\n");
  output << "  \"instruction_count\": " << instructions.size() << "\n";
  output << "}\n";
}

void printCfgJson(std::ostream &output,
                  const notdec::bin2llvm::NativeProgramState &state,
                  uint64_t entry) {
  const notdec::bin2llvm::NativeFunction *function = state.functionAt(entry);
  output << "{\n";
  output << "  \"query\": \"" << hexString(entry) << "\",\n";
  if (function == nullptr) {
    output << "  \"found\": false,\n";
    output << "  \"blocks\": [],\n";
    output << "  \"count\": 0\n";
    output << "}\n";
    return;
  }

  output << "  \"found\": true,\n";
  output << "  \"function\": {\n";
  output << "    \"entry\": \"" << hexString(function->Entry) << "\",\n";
  output << "    \"range_start\": \"" << hexString(function->RangeStart)
         << "\",\n";
  output << "    \"range_end\": \"" << hexString(function->RangeEnd)
         << "\",\n";
  output << "    \"name\": \"" << jsonEscape(function->Name) << "\",\n";
  output << "    \"source\": \"" << jsonEscape(function->Source) << "\"\n";
  output << "  },\n";
  output << "  \"blocks\": [";
  bool firstBlock = true;
  for (const notdec::bin2llvm::NativeBasicBlock &block : function->Blocks) {
    output << (firstBlock ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"start\": \"" << hexString(block.Start) << "\",\n";
    output << "      \"end\": \"" << hexString(block.End) << "\",\n";
    output << "      \"size\": " << (block.End - block.Start) << ",\n";
    output << "      \"successors\": ";
    printAddressArray(output, block.Successors);
    output << "\n";
    output << "    }";
    firstBlock = false;
  }
  output << (firstBlock ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << function->Blocks.size() << "\n";
  output << "}\n";
}

void printCfgDot(std::ostream &output,
                 const notdec::bin2llvm::NativeProgramState &state,
                 uint64_t entry) {
  const notdec::bin2llvm::NativeFunction *function = state.functionAt(entry);
  std::string query = hexString(entry);
  output << "digraph \"notdec_cfg_" << dotEscape(query) << "\" {\n";
  output << "  graph [label=\"notdec cfg " << dotEscape(query) << "\"];\n";
  output << "  node [shape=box];\n";
  if (function == nullptr) {
    output << "  \"not_found\" [label=\"found=false query=" << dotEscape(query)
           << "\"];\n";
    output << "}\n";
    return;
  }

  output << "  \"function\" [shape=plaintext, label=\"entry="
         << dotEscape(hexString(function->Entry)) << " range="
         << dotEscape(hexString(function->RangeStart)) << ".."
         << dotEscape(hexString(function->RangeEnd)) << " name="
         << dotEscape(function->Name) << "\"];\n";
  for (const notdec::bin2llvm::NativeBasicBlock &block : function->Blocks) {
    std::string start = hexString(block.Start);
    std::string end = hexString(block.End);
    output << "  \"" << dotEscape(start) << "\" [label=\""
           << dotEscape(start) << ".." << dotEscape(end) << " size="
           << (block.End - block.Start) << "\"];\n";
  }
  for (const notdec::bin2llvm::NativeBasicBlock &block : function->Blocks) {
    std::string start = hexString(block.Start);
    for (uint64_t successor : block.Successors) {
      output << "  \"" << dotEscape(start) << "\" -> \""
             << dotEscape(hexString(successor)) << "\";\n";
    }
  }
  output << "}\n";
}

std::optional<std::string>
lookupExternalCallTarget(const notdec::bin2llvm::NativeProgramState &state,
                         uint64_t address) {
  if (std::optional<std::string> symbol = state.lookupPltExternal(address)) {
    return symbol;
  }
  for (const notdec::bin2llvm::NativePltEntry &entry : state.pltEntries()) {
    if (entry.GotAddress == address) {
      return entry.SymbolName;
    }
  }
  for (const notdec::bin2llvm::NativeRelocationInfo &relocation :
       state.relocations()) {
    if (relocation.Address == address && relocation.Status == "external" &&
        !relocation.SymbolName.empty()) {
      return relocation.SymbolName;
    }
  }
  return std::nullopt;
}

void printCallgraphJson(std::ostream &output,
                        const notdec::bin2llvm::NativeProgramState &state) {
  using namespace notdec::bin2llvm;

  output << "{\n";
  output << "  \"edges\": [";
  bool firstEdge = true;
  uint64_t count = 0;
  for (const NativeXref &xref : state.xrefs()) {
    if (xref.Kind != NativeXrefKind::Call) {
      continue;
    }

    const NativeFunction *caller = state.functionContaining(xref.From);
    const NativeFunction *callee = state.functionAt(xref.To);
    std::optional<std::string> external =
        lookupExternalCallTarget(state, xref.To);

    output << (firstEdge ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"callsite\": \"" << hexString(xref.From) << "\",\n";
    output << "      \"target\": \"" << hexString(xref.To) << "\",\n";
    output << "      \"source\": \"" << jsonEscape(xref.Source) << "\",\n";
    output << "      \"caller_found\": " << (caller != nullptr ? "true" : "false")
           << ",\n";
    output << "      \"caller_entry\": ";
    if (caller != nullptr) {
      output << "\"" << hexString(caller->Entry) << "\"";
    } else {
      output << "null";
    }
    output << ",\n";
    output << "      \"caller_name\": \""
           << jsonEscape(caller != nullptr ? caller->Name : "") << "\",\n";
    if (callee != nullptr) {
      output << "      \"callee_kind\": \"internal\",\n";
      output << "      \"callee_entry\": \"" << hexString(callee->Entry)
             << "\",\n";
      output << "      \"callee_name\": \"" << jsonEscape(callee->Name)
             << "\"\n";
    } else if (external) {
      output << "      \"callee_kind\": \"external\",\n";
      output << "      \"callee_entry\": null,\n";
      output << "      \"callee_name\": \"" << jsonEscape(*external)
             << "\"\n";
    } else {
      output << "      \"callee_kind\": \"unknown\",\n";
      output << "      \"callee_entry\": null,\n";
      output << "      \"callee_name\": \"\"\n";
    }
    output << "    }";
    firstEdge = false;
    ++count;
  }
  output << (firstEdge ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << count << "\n";
  output << "}\n";
}

void printCallgraphDot(std::ostream &output,
                       const notdec::bin2llvm::NativeProgramState &state) {
  using namespace notdec::bin2llvm;

  output << "digraph \"notdec_callgraph\" {\n";
  output << "  graph [label=\"notdec callgraph\"];\n";
  output << "  node [shape=box];\n";
  for (const NativeXref &xref : state.xrefs()) {
    if (xref.Kind != NativeXrefKind::Call) {
      continue;
    }

    const NativeFunction *caller = state.functionContaining(xref.From);
    const NativeFunction *callee = state.functionAt(xref.To);
    std::optional<std::string> external =
        lookupExternalCallTarget(state, xref.To);

    std::string callerId =
        caller != nullptr ? "func_" + hexString(caller->Entry)
                          : "unknown_caller_" + hexString(xref.From);
    std::string callerLabel =
        caller != nullptr ? hexString(caller->Entry) + " " + caller->Name
                          : "unknown caller " + hexString(xref.From);

    std::string calleeId;
    std::string calleeLabel;
    if (callee != nullptr) {
      calleeId = "func_" + hexString(callee->Entry);
      calleeLabel = hexString(callee->Entry) + " " + callee->Name;
    } else if (external) {
      calleeId = "external_" + *external;
      calleeLabel = "external " + *external;
    } else {
      calleeId = "unknown_target_" + hexString(xref.To);
      calleeLabel = "unknown target " + hexString(xref.To);
    }

    output << "  \"" << dotEscape(callerId) << "\" [label=\""
           << dotEscape(callerLabel) << "\"];\n";
    output << "  \"" << dotEscape(calleeId) << "\" [label=\""
           << dotEscape(calleeLabel) << "\"];\n";
    output << "  \"" << dotEscape(callerId) << "\" -> \""
           << dotEscape(calleeId) << "\" [label=\"callsite="
           << dotEscape(hexString(xref.From)) << " source="
           << dotEscape(xref.Source) << "\"];\n";
  }
  output << "}\n";
}

void printXrefObject(std::ostream &output,
                     const notdec::bin2llvm::NativeXref &xref,
                     const char *indent) {
  output << indent << "{\n";
  output << indent << "  \"from\": \"" << hexString(xref.From) << "\",\n";
  output << indent << "  \"to\": \"" << hexString(xref.To) << "\",\n";
  output << indent << "  \"kind\": \""
         << notdec::bin2llvm::toString(xref.Kind) << "\",\n";
  output << indent << "  \"source\": \"" << jsonEscape(xref.Source)
         << "\"\n";
  output << indent << "}";
}

void printFunctionXrefsJson(
    std::ostream &output, const notdec::bin2llvm::NativeProgramState &state,
    uint64_t entry) {
  using namespace notdec::bin2llvm;

  const NativeFunction *function = state.functionAt(entry);
  output << "{\n";
  output << "  \"query\": \"" << hexString(entry) << "\",\n";
  if (function == nullptr) {
    output << "  \"found\": false,\n";
    output << "  \"outgoing\": [],\n";
    output << "  \"outgoing_count\": 0,\n";
    output << "  \"incoming_entry\": [],\n";
    output << "  \"incoming_entry_count\": 0\n";
    output << "}\n";
    return;
  }

  std::vector<const NativeXref *> outgoing;
  for (const NativeXref &xref : state.xrefs()) {
    const NativeFunction *owner = state.functionContaining(xref.From);
    if (owner != nullptr && owner->Entry == function->Entry) {
      outgoing.push_back(&xref);
    }
  }
  std::vector<const NativeXref *> incomingEntry = state.xrefsTo(function->Entry);

  output << "  \"found\": true,\n";
  output << "  \"function_entry\": \"" << hexString(function->Entry) << "\",\n";
  output << "  \"outgoing\": [";
  bool firstXref = true;
  for (const NativeXref *xref : outgoing) {
    output << (firstXref ? "\n" : ",\n");
    printXrefObject(output, *xref, "    ");
    firstXref = false;
  }
  output << (firstXref ? "],\n" : "\n  ],\n");
  output << "  \"outgoing_count\": " << outgoing.size() << ",\n";
  output << "  \"incoming_entry\": [";
  firstXref = true;
  for (const NativeXref *xref : incomingEntry) {
    output << (firstXref ? "\n" : ",\n");
    printXrefObject(output, *xref, "    ");
    firstXref = false;
  }
  output << (firstXref ? "],\n" : "\n  ],\n");
  output << "  \"incoming_entry_count\": " << incomingEntry.size() << "\n";
  output << "}\n";
}

void printXrefsJson(std::ostream &output,
                    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"xrefs\": [";
  bool firstXref = true;
  for (const notdec::bin2llvm::NativeXref &xref : state.xrefs()) {
    output << (firstXref ? "\n" : ",\n");
    printXrefObject(output, xref, "    ");
    firstXref = false;
  }
  output << (firstXref ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.xrefs().size() << "\n";
  output << "}\n";
}

void printXrefsDot(std::ostream &output,
                   const notdec::bin2llvm::NativeProgramState &state) {
  output << "digraph \"notdec_xrefs\" {\n";
  output << "  graph [label=\"notdec xrefs\"];\n";
  output << "  node [shape=box];\n";
  for (const notdec::bin2llvm::NativeXref &xref : state.xrefs()) {
    std::string from = hexString(xref.From);
    std::string to = hexString(xref.To);
    output << "  \"" << dotEscape(from) << "\" [label=\""
           << dotEscape(from) << "\"];\n";
    output << "  \"" << dotEscape(to) << "\" [label=\"" << dotEscape(to)
           << "\"];\n";
    output << "  \"" << dotEscape(from) << "\" -> \"" << dotEscape(to)
           << "\" [label=\"kind="
           << dotEscape(notdec::bin2llvm::toString(xref.Kind)) << " source="
           << dotEscape(xref.Source) << "\"];\n";
  }
  output << "}\n";
}

void printXrefsKindJson(std::ostream &output,
                        const notdec::bin2llvm::NativeProgramState &state,
                        notdec::bin2llvm::NativeXrefKind kind) {
  output << "{\n";
  output << "  \"kind\": \"" << notdec::bin2llvm::toString(kind) << "\",\n";
  output << "  \"xrefs\": [";
  bool firstXref = true;
  uint64_t count = 0;
  for (const notdec::bin2llvm::NativeXref &xref : state.xrefs()) {
    if (xref.Kind != kind) {
      continue;
    }
    output << (firstXref ? "\n" : ",\n");
    printXrefObject(output, xref, "    ");
    firstXref = false;
    ++count;
  }
  output << (firstXref ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << count << "\n";
  output << "}\n";
}

void printXrefsQueryJson(std::ostream &output,
                         const notdec::bin2llvm::NativeProgramState &state,
                         uint64_t address, bool fromQuery) {
  std::vector<const notdec::bin2llvm::NativeXref *> xrefs =
      fromQuery ? state.xrefsFrom(address) : state.xrefsTo(address);

  output << "{\n";
  output << "  \"query\": \"" << hexString(address) << "\",\n";
  output << "  \"direction\": \"" << (fromQuery ? "from" : "to") << "\",\n";
  output << "  \"xrefs\": [";
  bool firstXref = true;
  for (const notdec::bin2llvm::NativeXref *xref : xrefs) {
    output << (firstXref ? "\n" : ",\n");
    printXrefObject(output, *xref, "    ");
    firstXref = false;
  }
  output << (firstXref ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << xrefs.size() << "\n";
  output << "}\n";
}

void printInstructionObject(
    std::ostream &output,
    const notdec::bin2llvm::NativeInstruction &instruction,
    const char *indent) {
  output << indent << "{\n";
  output << indent << "  \"address\": \"" << hexString(instruction.Address)
         << "\",\n";
  output << indent << "  \"size\": " << instruction.Size << ",\n";
  output << indent << "  \"bytes\": \"" << bytesHex(instruction.Bytes)
         << "\",\n";
  output << indent << "  \"text\": \""
         << jsonEscape(instruction.Mnemonic) << "\",\n";
  output << indent << "  \"source\": \"" << jsonEscape(instruction.Source)
         << "\",\n";
  output << indent << "  \"flow_kind\": \""
         << notdec::bin2llvm::toString(instruction.FlowKind) << "\",\n";
  output << indent << "  \"direct_flow_targets\": ";
  printAddressArray(output, instruction.DirectFlowTargets);
  output << ",\n";
  output << indent << "  \"direct_call_targets\": ";
  printAddressArray(output, instruction.DirectCallTargets);
  output << ",\n";
  output << indent << "  \"tail_flow_targets\": ";
  printAddressArray(output, instruction.TailFlowTargets);
  output << ",\n";
  output << indent << "  \"fallthrough\": ";
  if (instruction.Fallthrough) {
    output << "\"" << hexString(*instruction.Fallthrough) << "\"";
  } else {
    output << "null";
  }
  output << ",\n";
  output << indent << "  \"has_indirect_call\": "
         << (instruction.HasIndirectCall ? "true" : "false") << "\n";
  output << indent << "}";
}

void printInstructionListJson(
    std::ostream &output,
    const std::vector<const notdec::bin2llvm::NativeInstruction *> &instructions,
    const std::string &prefix) {
  output << prefix << "\"instructions\": [";
  bool firstInstruction = true;
  for (const notdec::bin2llvm::NativeInstruction *instruction : instructions) {
    output << (firstInstruction ? "\n" : ",\n");
    printInstructionObject(output, *instruction, "    ");
    firstInstruction = false;
  }
  output << (firstInstruction ? "],\n" : "\n  ],\n");
  output << prefix << "\"count\": " << instructions.size() << "\n";
}

void printInstructionsJson(
    std::ostream &output,
    const notdec::bin2llvm::NativeProgramState &state) {
  std::vector<const notdec::bin2llvm::NativeInstruction *> instructions;
  for (const auto &[address, instruction] : state.instructions()) {
    (void)address;
    instructions.push_back(&instruction);
  }
  output << "{\n";
  printInstructionListJson(output, instructions, "  ");
  output << "}\n";
}

void printInstructionsRangeJson(
    std::ostream &output,
    const notdec::bin2llvm::NativeProgramState &state, uint64_t start,
    uint64_t end) {
  std::vector<const notdec::bin2llvm::NativeInstruction *> instructions =
      state.instructionsInRange(start, end);
  output << "{\n";
  output << "  \"query\": {\n";
  output << "    \"start\": \"" << hexString(start) << "\",\n";
  output << "    \"end\": \"" << hexString(end) << "\"\n";
  output << "  },\n";
  printInstructionListJson(output, instructions, "  ");
  output << "}\n";
}

void printInstructionsFunctionJson(
    std::ostream &output,
    const notdec::bin2llvm::NativeProgramState &state, uint64_t entry) {
  const notdec::bin2llvm::NativeFunction *function = state.functionAt(entry);
  std::vector<const notdec::bin2llvm::NativeInstruction *> instructions;
  output << "{\n";
  output << "  \"query\": \"" << hexString(entry) << "\",\n";
  if (function == nullptr) {
    output << "  \"found\": false,\n";
    printInstructionListJson(output, instructions, "  ");
    output << "}\n";
    return;
  }

  instructions = instructionsInBlocks(state, *function);
  output << "  \"found\": true,\n";
  output << "  \"function\": {\n";
  output << "    \"entry\": \"" << hexString(function->Entry) << "\",\n";
  output << "    \"range_start\": \"" << hexString(function->RangeStart)
         << "\",\n";
  output << "    \"range_end\": \"" << hexString(function->RangeEnd)
         << "\",\n";
  output << "    \"name\": \"" << jsonEscape(function->Name) << "\",\n";
  output << "    \"source\": \"" << jsonEscape(function->Source) << "\"\n";
  output << "  },\n";
  printInstructionListJson(output, instructions, "  ");
  output << "}\n";
}

void printPltJson(std::ostream &output,
                  const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"plt\": [";
  bool firstEntry = true;
  for (const notdec::bin2llvm::NativePltEntry &entry : state.pltEntries()) {
    output << (firstEntry ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"stub\": \"" << hexString(entry.StubAddress) << "\",\n";
    output << "      \"got\": \"" << hexString(entry.GotAddress) << "\",\n";
    output << "      \"symbol\": \"" << jsonEscape(entry.SymbolName) << "\"\n";
    output << "    }";
    firstEntry = false;
  }
  output << (firstEntry ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.pltEntries().size() << "\n";
  output << "}\n";
}

void printUnresolvedJson(
    std::ostream &output,
    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"unresolved\": [";
  bool firstFlow = true;
  for (const notdec::bin2llvm::NativeUnresolvedFlow &flow :
       state.unresolvedFlows()) {
    output << (firstFlow ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"address\": \"" << hexString(flow.Address) << "\",\n";
    output << "      \"kind\": \""
           << notdec::bin2llvm::toString(flow.Kind) << "\",\n";
    output << "      \"source\": \"" << jsonEscape(flow.Source) << "\"\n";
    output << "    }";
    firstFlow = false;
  }
  output << (firstFlow ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.unresolvedFlows().size() << "\n";
  output << "}\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    std::optional<CliOptions> options = parseArgs(argc, argv);
    if (!options) {
      printUsage(argv[0]);
      return 1;
    }

    std::unique_ptr<LIEF::ELF::Binary> binary =
        LIEF::ELF::Parser::parse(options->ElfPath);
    if (!binary) {
      std::cerr << "failed to parse ELF: " << options->ElfPath << '\n';
      return 1;
    }
    if (!notdec::bin2llvm::isSupportedNativeElfArchitecture(*binary)) {
      std::cerr << notdec::bin2llvm::unsupportedNativeElfArchitectureMessage(
                       *binary, "native discovery")
                << '\n';
      return 1;
    }

    notdec::bin2llvm::NativeProgramState state(*binary);
    options->GtirbOptions.ElfPath = options->ElfPath;
    notdec::bin2llvm::NativeAnalysisManager manager;
    manager.addAnalyzer(notdec::bin2llvm::createElfLoadAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createRelocationPltAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createElfEntryAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createElfSymbolAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createEhFrameAnalyzer());
    if (options->DecodeMode == notdec::bin2llvm::NativeDecodeMode::Gtirb) {
      options->DecodeOptions.DecodeExistingBlocksOnly = true;
      manager.addAnalyzer(notdec::bin2llvm::createGtirbFunctionFactsAnalyzer(
          options->GtirbOptions));
      manager.addAnalyzer(notdec::bin2llvm::createSleighSeedInstructionAnalyzer(
          options->DecodeOptions));
    } else {
      state.addNote("native internal seed-linear CFG discovery is disabled; "
                    "use --native-decode-mode gtirb");
    }
    manager.addAnalyzer(notdec::bin2llvm::createFlowFactNormalizer());
    if (options->Mode == OutputMode::TextReport) {
      manager.addAnalyzer(notdec::bin2llvm::createReportAnalyzer(std::cout));
    }
    manager.run(state);
    if (options->Mode == OutputMode::SummaryJson) {
      printSummaryJson(std::cout, state);
    } else if (options->Mode == OutputMode::MemoryJson) {
      printMemoryJson(std::cout, state);
    } else if (options->Mode == OutputMode::RelocationsJson) {
      printRelocationsJson(std::cout, state);
    } else if (options->Mode == OutputMode::NotesJson) {
      printNotesJson(std::cout, state);
    } else if (options->Mode == OutputMode::EhFrameJson) {
      printEhFrameJson(std::cout, state);
    } else if (options->Mode == OutputMode::SeedsJson) {
      printSeedsJson(std::cout, state);
    } else if (options->Mode == OutputMode::DiscoveryJson) {
      printDiscoveryJson(std::cout, state);
    } else if (options->Mode == OutputMode::FunctionJson) {
      printFunctionJson(std::cout, state, *options->QueryFunctionEntry);
    } else if (options->Mode == OutputMode::FunctionXrefsJson) {
      printFunctionXrefsJson(std::cout, state, *options->QueryFunctionEntry);
    } else if (options->Mode == OutputMode::FunctionsJson) {
      printFunctionsJson(std::cout, state);
    } else if (options->Mode == OutputMode::BlocksJson) {
      printBlocksJson(std::cout, state);
    } else if (options->Mode == OutputMode::BlockJson) {
      printBlockJson(std::cout, state, *options->QueryAddress);
    } else if (options->Mode == OutputMode::CfgJson) {
      printCfgJson(std::cout, state, *options->QueryFunctionEntry);
    } else if (options->Mode == OutputMode::CfgDot) {
      printCfgDot(std::cout, state, *options->QueryFunctionEntry);
    } else if (options->Mode == OutputMode::CallgraphJson) {
      printCallgraphJson(std::cout, state);
    } else if (options->Mode == OutputMode::CallgraphDot) {
      printCallgraphDot(std::cout, state);
    } else if (options->Mode == OutputMode::XrefsJson) {
      printXrefsJson(std::cout, state);
    } else if (options->Mode == OutputMode::XrefsDot) {
      printXrefsDot(std::cout, state);
    } else if (options->Mode == OutputMode::InstructionsJson) {
      printInstructionsJson(std::cout, state);
    } else if (options->Mode == OutputMode::InstructionsRangeJson) {
      printInstructionsRangeJson(std::cout, state, *options->QueryStart,
                                 *options->QueryEnd);
    } else if (options->Mode == OutputMode::InstructionsFunctionJson) {
      printInstructionsFunctionJson(std::cout, state,
                                    *options->QueryFunctionEntry);
    } else if (options->Mode == OutputMode::PltJson) {
      printPltJson(std::cout, state);
    } else if (options->Mode == OutputMode::UnresolvedJson) {
      printUnresolvedJson(std::cout, state);
    } else if (options->Mode == OutputMode::XrefsKindJson) {
      printXrefsKindJson(std::cout, state, *options->QueryXrefKind);
    } else if (options->Mode == OutputMode::XrefsFromJson) {
      printXrefsQueryJson(std::cout, state, *options->QueryAddress, true);
    } else if (options->Mode == OutputMode::XrefsToJson) {
      printXrefsQueryJson(std::cout, state, *options->QueryAddress, false);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
