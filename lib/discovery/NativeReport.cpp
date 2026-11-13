#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

class ReportAnalyzer final : public NativeAnalyzer {
public:
  explicit ReportAnalyzer(std::ostream &output) : Output(output) {}

  std::string name() const override { return "ReportAnalyzer"; }
  int priority() const override { return 1000; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    std::map<NativeFunctionConfidence, uint64_t> confidenceCounts;
    for (const auto &[address, seed] : state.functionSeeds()) {
      (void)address;
      ++confidenceCounts[seed.Confidence];
    }

    Output << "native discovery report\n";
    Output << "  pointer size: " << static_cast<unsigned>(state.pointerSize())
           << '\n';
    Output << "  memory ranges: " << state.memoryRanges().size() << '\n';
    Output << "  sections: " << state.sections().size() << '\n';
    Output << "  function seeds: " << state.functionSeeds().size() << '\n';
    Output << "  function worklist: " << state.functionWorklist().size()
           << '\n';

    Output << "  sources:\n";
    for (const auto &[source, count] : state.sourceCounts()) {
      Output << "    " << source << ": " << count << '\n';
    }

    Output << "  confidence:\n";
    for (NativeFunctionConfidence confidence :
         {NativeFunctionConfidence::High, NativeFunctionConfidence::Medium,
          NativeFunctionConfidence::Low}) {
      Output << "    " << toString(confidence) << ": "
             << confidenceCounts[confidence] << '\n';
    }

    Output << "  executable sections:\n";
    for (const NativeSectionInfo &section : state.sections()) {
      if (!section.Executable) {
        continue;
      }
      Output << "    " << section.Name << " " << hexAddress(section.Address)
             << " size " << section.Size << '\n';
    }

    Output << "  relocations:\n";
    Output << "    total: " << state.relocations().size() << '\n';
    Output << "    relocated pointers: " << state.relocatedPointers().size()
           << '\n';
    std::map<std::string, uint64_t> relocationStatusCounts;
    std::map<std::string, uint64_t> relocationTypeCounts;
    for (const NativeRelocationInfo &relocation : state.relocations()) {
      ++relocationStatusCounts[relocation.Status];
      ++relocationTypeCounts[relocation.TypeName];
    }
    Output << "    by status:\n";
    for (const auto &[status, count] : relocationStatusCounts) {
      Output << "      " << status << ": " << count << '\n';
    }
    Output << "    by type:\n";
    for (const auto &[typeName, count] : relocationTypeCounts) {
      Output << "      " << typeName << ": " << count << '\n';
    }

    Output << "  plt:\n";
    Output << "    external symbols: " << state.pltEntries().size() << '\n';
    size_t shownPltEntries = 0;
    for (const NativePltEntry &entry : state.pltEntries()) {
      if (shownPltEntries == 8) {
        break;
      }
      Output << "    " << hexAddress(entry.StubAddress) << " -> "
             << entry.SymbolName << " via GOT " << hexAddress(entry.GotAddress)
             << '\n';
      ++shownPltEntries;
    }

    Output << "  confirmed functions:\n";
    Output << "    total: " << state.functions().size() << '\n';
    uint64_t blockCount = 0;
    for (const auto &[entry, function] : state.functions()) {
      (void)entry;
      blockCount += function.Blocks.size();
    }
    Output << "    basic blocks: " << blockCount << '\n';
    Output << "  instructions:\n";
    Output << "    total: " << state.instructions().size() << '\n';

    std::map<NativeXrefKind, uint64_t> xrefKindCounts;
    for (const NativeXref &xref : state.xrefs()) {
      ++xrefKindCounts[xref.Kind];
    }
    Output << "  xrefs:\n";
    Output << "    total: " << state.xrefs().size() << '\n';
    for (NativeXrefKind kind : {NativeXrefKind::Flow, NativeXrefKind::Call,
                                NativeXrefKind::Data, NativeXrefKind::String}) {
      Output << "    " << toString(kind) << ": " << xrefKindCounts[kind]
             << '\n';
    }
    std::map<NativeUnresolvedFlowKind, uint64_t> unresolvedFlowCounts;
    for (const NativeUnresolvedFlow &flow : state.unresolvedFlows()) {
      ++unresolvedFlowCounts[flow.Kind];
    }
    Output << "  unresolved indirect flows:\n";
    Output << "    total: " << state.unresolvedFlows().size() << '\n';
    for (NativeUnresolvedFlowKind kind :
         {NativeUnresolvedFlowKind::IndirectCall,
          NativeUnresolvedFlowKind::IndirectBranch,
          NativeUnresolvedFlowKind::IndirectTailBranch}) {
      Output << "    " << toString(kind) << ": "
             << unresolvedFlowCounts[kind] << '\n';
    }

    const NativeEhFrameStats &ehFrame = state.ehFrameStats();
    Output << "  eh_frame:\n";
    Output << "    .eh_frame_hdr: " << (ehFrame.HasEhFrameHdr ? "yes" : "no")
           << '\n';
    Output << "    .eh_frame: " << (ehFrame.HasEhFrame ? "yes" : "no")
           << '\n';
    Output << "    parsed hdr: "
           << (ehFrame.ParsedEhFrameHdr ? "yes" : "no") << '\n';
    Output << "    CIE: " << ehFrame.CieCount << '\n';
    Output << "    FDE: " << ehFrame.FdeCount << '\n';
    Output << "    parsed FDE: " << ehFrame.ParsedFdeCount << '\n';
    Output << "    hdr FDE count: " << ehFrame.HdrFdeCount << '\n';
    Output << "    hdr table entries: " << ehFrame.HdrTableEntries << '\n';
    Output << "    hdr matched starts: " << ehFrame.HdrMatchedStarts << '\n';
    Output << "    hdr missing in frame: " << ehFrame.HdrMissingInFrame
           << '\n';
    Output << "    frame FDEs missing in hdr: " << ehFrame.HdrExtraFrameFdes
           << '\n';
    Output << "    hdr FDE address matches: "
           << ehFrame.HdrFdeAddressMatches << '\n';
    Output << "    hdr FDE address mismatches: "
           << ehFrame.HdrFdeAddressMismatches << '\n';
    Output << "    added seeds: " << ehFrame.AddedSeedCount << '\n';
    Output << "    overlapped seeds: " << ehFrame.OverlappedSeedCount << '\n';
    Output << "    invalid: " << ehFrame.InvalidCount << '\n';
    Output << "    unsupported: " << ehFrame.UnsupportedCount << '\n';
    Output << "    hdr invalid: " << ehFrame.HdrInvalidCount << '\n';
    Output << "    hdr unsupported: " << ehFrame.HdrUnsupportedCount << '\n';
    if (!ehFrame.HdrMismatchSamples.empty()) {
      Output << "    hdr mismatch samples:\n";
      for (const std::string &sample : ehFrame.HdrMismatchSamples) {
        Output << "      " << sample << '\n';
      }
    }
    if (!ehFrame.UnsupportedSamples.empty()) {
      Output << "    unsupported samples:\n";
      for (const std::string &sample : ehFrame.UnsupportedSamples) {
        Output << "      " << sample << '\n';
      }
    }

    std::map<std::string, uint64_t> rangeSourceCounts;
    for (const auto &[address, seed] : state.functionSeeds()) {
      (void)address;
      if (!seed.RangeSource.empty()) {
        ++rangeSourceCounts[seed.RangeSource];
      }
    }
    Output << "  range sources:\n";
    for (const auto &[source, count] : rangeSourceCounts) {
      Output << "    " << source << ": " << count << '\n';
    }

    if (!state.notes().empty()) {
      Output << "  notes:\n";
      for (const std::string &note : state.notes()) {
        Output << "    " << note << '\n';
      }
    }

    Output << "  seeds:\n";
    for (const auto &[address, seed] : state.functionSeeds()) {
      Output << "    " << hexAddress(address);
      if (!seed.PrimaryName.empty()) {
        Output << " " << seed.PrimaryName;
      }
      if (seed.Size != 0) {
        Output << " size " << seed.Size;
      }
      if (seed.RangeStart != 0 || seed.RangeEnd != 0) {
        Output << " range [" << hexAddress(seed.RangeStart) << ", "
               << hexAddress(seed.RangeEnd) << ") from " << seed.RangeSource;
      }
      Output << " [" << toString(seed.Confidence) << "] sources:";
      for (const std::string &source : seed.Sources) {
        Output << " " << source;
      }
      if (!seed.Aliases.empty()) {
        Output << " aliases:";
        for (const std::string &alias : seed.Aliases) {
          Output << " " << alias;
        }
      }
      Output << '\n';
    }
  }

private:
  std::ostream &Output;
};

std::unique_ptr<NativeAnalyzer> createReportAnalyzer(std::ostream &output) {
  return std::make_unique<ReportAnalyzer>(output);
}

} // namespace notdec::bin2llvm
