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
  FunctionsJson,
  BlocksJson,
  XrefsJson,
  InstructionsJson,
};

struct CliOptions {
  std::string ElfPath;
  OutputMode Mode = OutputMode::TextReport;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " <elf-file>\n";
  std::cerr << "       " << argv0 << " --summary-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --functions-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --blocks-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --xrefs-json <elf-file>\n";
  std::cerr << "       " << argv0 << " --instructions-json <elf-file>\n";
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    return std::nullopt;
  }

  CliOptions options;
  if (argc == 2) {
    options.ElfPath = argv[1];
    return options;
  }

  std::string mode = argv[1];
  if (mode == "--summary-json") {
    options.Mode = OutputMode::SummaryJson;
  } else if (mode == "--functions-json") {
    options.Mode = OutputMode::FunctionsJson;
  } else if (mode == "--blocks-json") {
    options.Mode = OutputMode::BlocksJson;
  } else if (mode == "--xrefs-json") {
    options.Mode = OutputMode::XrefsJson;
  } else if (mode == "--instructions-json") {
    options.Mode = OutputMode::InstructionsJson;
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

void printXrefsJson(std::ostream &output,
                    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"xrefs\": [";
  bool firstXref = true;
  for (const notdec::bin2llvm::NativeXref &xref : state.xrefs()) {
    output << (firstXref ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"from\": \"" << hexString(xref.From) << "\",\n";
    output << "      \"to\": \"" << hexString(xref.To) << "\",\n";
    output << "      \"kind\": \"" << notdec::bin2llvm::toString(xref.Kind)
           << "\",\n";
    output << "      \"source\": \"" << jsonEscape(xref.Source) << "\"\n";
    output << "    }";
    firstXref = false;
  }
  output << (firstXref ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.xrefs().size() << "\n";
  output << "}\n";
}

void printInstructionsJson(
    std::ostream &output,
    const notdec::bin2llvm::NativeProgramState &state) {
  output << "{\n";
  output << "  \"instructions\": [";
  bool firstInstruction = true;
  for (const auto &[address, instruction] : state.instructions()) {
    (void)address;
    output << (firstInstruction ? "\n" : ",\n");
    output << "    {\n";
    output << "      \"address\": \"" << hexString(instruction.Address)
           << "\",\n";
    output << "      \"size\": " << instruction.Size << ",\n";
    output << "      \"bytes\": \"" << bytesHex(instruction.Bytes) << "\",\n";
    output << "      \"text\": \"" << jsonEscape(instruction.Mnemonic)
           << "\",\n";
    output << "      \"source\": \"" << jsonEscape(instruction.Source)
           << "\"\n";
    output << "    }";
    firstInstruction = false;
  }
  output << (firstInstruction ? "],\n" : "\n  ],\n");
  output << "  \"count\": " << state.instructions().size() << "\n";
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
    } else if (options->Mode == OutputMode::FunctionsJson) {
      printFunctionsJson(std::cout, state);
    } else if (options->Mode == OutputMode::BlocksJson) {
      printBlocksJson(std::cout, state);
    } else if (options->Mode == OutputMode::XrefsJson) {
      printXrefsJson(std::cout, state);
    } else if (options->Mode == OutputMode::InstructionsJson) {
      printInstructionsJson(std::cout, state);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
