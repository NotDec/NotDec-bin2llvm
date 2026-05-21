#include "notdec-bin2llvm/NativeAnalysis.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace {

struct CliOptions {
  std::string ElfPath;
  bool SummaryJson = false;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " <elf-file>\n";
  std::cerr << "       " << argv0 << " --summary-json <elf-file>\n";
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

  if (std::string(argv[1]) != "--summary-json") {
    return std::nullopt;
  }
  options.SummaryJson = true;
  options.ElfPath = argv[2];
  return options;
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
    if (!options->SummaryJson) {
      manager.addAnalyzer(notdec::bin2llvm::createReportAnalyzer(std::cout));
    }
    manager.run(state);
    if (options->SummaryJson) {
      printSummaryJson(std::cout, state);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
