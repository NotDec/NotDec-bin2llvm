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
  SeedsJson,
  FunctionsJson,
  BlocksJson,
  XrefsJson,
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
  std::optional<uint64_t> QueryAddress;
  std::optional<uint64_t> QueryStart;
  std::optional<uint64_t> QueryEnd;
  std::optional<uint64_t> QueryFunctionEntry;
  std::optional<notdec::bin2llvm::NativeXrefKind> QueryXrefKind;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " <elf-file>\n";
  std::cerr << "       " << argv0 << " --summary-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --memory-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --seeds-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --functions-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --blocks-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --xrefs-json <elf-file>\n";
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
  if (argc != 2 && argc != 3 && argc != 4 && argc != 5) {
    return std::nullopt;
  }

  CliOptions options;
  if (argc == 2) {
    options.ElfPath = argv[1];
    return options;
  }

  std::string mode = argv[1];
  if (argc == 5) {
    if (mode != "--instructions-range-json") {
      return std::nullopt;
    }
    options.Mode = OutputMode::InstructionsRangeJson;
    options.QueryStart = parseAddress(argv[2]);
    options.QueryEnd = parseAddress(argv[3]);
    if (!options.QueryStart || !options.QueryEnd) {
      return std::nullopt;
    }
    options.ElfPath = argv[4];
    return options;
  }

  if (argc == 4) {
    if (mode == "--xrefs-from-json") {
      options.Mode = OutputMode::XrefsFromJson;
    } else if (mode == "--xrefs-to-json") {
      options.Mode = OutputMode::XrefsToJson;
    } else if (mode == "--xrefs-kind-json") {
      options.Mode = OutputMode::XrefsKindJson;
    } else if (mode == "--instructions-function-json") {
      options.Mode = OutputMode::InstructionsFunctionJson;
    } else {
      return std::nullopt;
    }
    if (options.Mode == OutputMode::XrefsKindJson) {
      options.QueryXrefKind = parseXrefKind(argv[2]);
      if (!options.QueryXrefKind) {
        return std::nullopt;
      }
      options.ElfPath = argv[3];
      return options;
    }
    std::optional<uint64_t> address = parseAddress(argv[2]);
    if (!address) {
      return std::nullopt;
    }
    if (options.Mode == OutputMode::InstructionsFunctionJson) {
      options.QueryFunctionEntry = *address;
    } else {
      options.QueryAddress = *address;
    }
    options.ElfPath = argv[3];
    return options;
  }

  if (mode == "--summary-json") {
    options.Mode = OutputMode::SummaryJson;
  } else if (mode == "--memory-json") {
    options.Mode = OutputMode::MemoryJson;
  } else if (mode == "--seeds-json") {
    options.Mode = OutputMode::SeedsJson;
  } else if (mode == "--functions-json") {
    options.Mode = OutputMode::FunctionsJson;
  } else if (mode == "--blocks-json") {
    options.Mode = OutputMode::BlocksJson;
  } else if (mode == "--xrefs-json") {
    options.Mode = OutputMode::XrefsJson;
  } else if (mode == "--instructions-json") {
    options.Mode = OutputMode::InstructionsJson;
  } else if (mode == "--plt-json") {
    options.Mode = OutputMode::PltJson;
  } else if (mode == "--unresolved-json") {
    options.Mode = OutputMode::UnresolvedJson;
  } else {
    return std::nullopt;
  }
  options.ElfPath = argv[2];
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

uint64_t countBasicBlocks(const notdec::bin2llvm::NativeProgramState &state) {
  uint64_t count = 0;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    count += function.Blocks.size();
  }
  return count;
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

void printSeedsJson(std::ostream &output,
                    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"seeds\": [";
  bool firstSeed = true;
  for (const auto &[address, seed] : state.functionSeeds()) {
    (void)address;
    output << (firstSeed ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"address\": \"" << hexString(seed.Address) << "\",\n";
    output << "      \"size\": " << seed.Size << ",\n";
    output << "      \"range_start\": \"" << hexString(seed.RangeStart)
           << "\",\n";
    output << "      \"range_end\": \"" << hexString(seed.RangeEnd)
           << "\",\n";
    output << "      \"range_source\": \"" << jsonEscape(seed.RangeSource)
           << "\",\n";
    output << "      \"primary_name\": \"" << jsonEscape(seed.PrimaryName)
           << "\",\n";
    output << "      \"aliases\": ";
    printStringArray(output, seed.Aliases);
    output << ",\n";
    output << "      \"sources\": ";
    printStringArray(output, seed.Sources);
    output << ",\n";
    output << "      \"confidence\": \""
           << notdec::bin2llvm::toString(seed.Confidence) << "\"\n";
    output << "    }";
    firstSeed = false;
  }
  output << (firstSeed ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.functionSeeds().size() << "\n";
  output << "}\n";
}

void printFunctionsJson(std::ostream &output,
                        const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"functions\": [";
  bool firstFunction = true;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    output << (firstFunction ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"entry\": \"" << hexString(function.Entry) << "\",\n";
    output << "      \"range_start\": \"" << hexString(function.RangeStart)
           << "\",\n";
    output << "      \"range_end\": \"" << hexString(function.RangeEnd)
           << "\",\n";
    output << "      \"name\": \"" << jsonEscape(function.Name) << "\",\n";
    output << "      \"source\": \"" << jsonEscape(function.Source) << "\",\n";
    output << "      \"block_count\": " << function.Blocks.size() << "\n";
    output << "    }";
    firstFunction = false;
  }
  output << (firstFunction ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.functions().size() << "\n";
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
         << "\"\n";
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

  instructions = state.instructionsInRange(function->RangeStart,
                                           function->RangeEnd);
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

    notdec::bin2llvm::NativeProgramState state(*binary);
    notdec::bin2llvm::NativeAnalysisManager manager;
    manager.addAnalyzer(notdec::bin2llvm::createElfLoadAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createRelocationPltAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createElfEntryAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createElfSymbolAnalyzer());
    manager.addAnalyzer(notdec::bin2llvm::createEhFrameAnalyzer());
    manager.addAnalyzer(
        notdec::bin2llvm::createSleighSeedInstructionAnalyzer());
    if (options->Mode == OutputMode::TextReport) {
      manager.addAnalyzer(notdec::bin2llvm::createReportAnalyzer(std::cout));
    }
    manager.run(state);
    if (options->Mode == OutputMode::SummaryJson) {
      printSummaryJson(std::cout, state);
    } else if (options->Mode == OutputMode::MemoryJson) {
      printMemoryJson(std::cout, state);
    } else if (options->Mode == OutputMode::SeedsJson) {
      printSeedsJson(std::cout, state);
    } else if (options->Mode == OutputMode::FunctionsJson) {
      printFunctionsJson(std::cout, state);
    } else if (options->Mode == OutputMode::BlocksJson) {
      printBlocksJson(std::cout, state);
    } else if (options->Mode == OutputMode::XrefsJson) {
      printXrefsJson(std::cout, state);
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
