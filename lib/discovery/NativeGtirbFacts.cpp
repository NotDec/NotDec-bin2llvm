#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

class GtirbFunctionFactsAnalyzer final : public NativeAnalyzer {
public:
  explicit GtirbFunctionFactsAnalyzer(NativeGtirbDecodeOptions options)
      : Options(std::move(options)) {}

  std::string name() const override { return "GtirbFunctionFactsAnalyzer"; }
  int priority() const override { return 55; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
#if NOTDEC_BIN2LLVM_ENABLE_GTIRB
    auto importGtirb = [&](const std::filesystem::path &gtirbPath,
                           const char *source) -> bool {
      std::ifstream input(gtirbPath, std::ios::binary);
      if (!input) {
        state.addNote(std::string("gtirb frontend could not open ") + source +
                      ": " + gtirbPath.string());
        return false;
      }

      gtirb::Context context;
      gtirb_pprint::registerAuxDataTypes();
      gtirb::ErrorOr<gtirb::IR *> loaded = gtirb::IR::load(context, input);
      if (!loaded) {
        state.addNote(std::string("gtirb frontend load failed from ") + source +
                      ": " + loaded.getError().message());
        return false;
      }

      uint64_t functionCount = 0;
      uint64_t blockCount = 0;
      uint64_t edgeCount = 0;
      for (gtirb::Module &module : (*loaded)->modules()) {
        importFunctions(state, context, module, Options.RuntimeFilter,
                        functionCount, blockCount);
        importCfgEdges(state, module, edgeCount);
      }
      uint64_t fallbackCount = importSeedRanges(state, Options.RuntimeFilter);
      state.setControlFlowAuthority(NativeControlFlowAuthority::Gtirb);
      state.addNote("gtirb frontend imported from " + std::string(source) +
                    ": " + std::to_string(functionCount) + " functions, " +
                    std::to_string(blockCount) + " blocks, " +
                    std::to_string(edgeCount) + " cfg edges, " +
                    std::to_string(fallbackCount) +
                    " seed-range fallbacks");
      return true;
    };

    std::optional<std::filesystem::path> gtirbPath = resolveGtirbPath(state);
    if (!gtirbPath) {
      return;
    }
    (void)importGtirb(*gtirbPath, "gtirb");
#else
    (void)state;
    state.addNote("gtirb frontend unavailable: rebuild with GTIRB");
#endif
  }

private:
  NativeGtirbDecodeOptions Options;

  static std::string shellQuote(const std::string &text) {
    std::string quoted = "'";
    for (char ch : text) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += "'";
    return quoted;
  }

  std::optional<std::filesystem::path>
  resolveGtirbPath(NativeProgramState &state) const {
    if (!Options.GtirbPath.empty()) {
      return std::filesystem::path(Options.GtirbPath);
    }
    if (!Options.GenerateIfMissing || Options.ElfPath.empty()) {
      state.addNote("gtirb frontend skipped: no .gtirb path");
      return std::nullopt;
    }

    std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("notdec-" +
         std::to_string(std::hash<std::string>{}(Options.ElfPath)) +
         ".gtirb");
    std::string command = shellQuote(Options.DdisasmPath) + " " +
                          shellQuote(Options.ElfPath) + " --ir " +
                          shellQuote(output.string()) + " >/dev/null 2>/dev/null";
    int exitCode = std::system(command.c_str());
    if (exitCode == 0 && std::filesystem::exists(output)) {
      return output;
    }

    std::filesystem::path fallback =
        std::filesystem::temp_directory_path() /
        ("notdec-" +
         std::to_string(std::hash<std::string>{}(Options.ElfPath)) +
         "-no-analysis.gtirb");
    command = shellQuote(Options.DdisasmPath) + " --no-analysis " +
              shellQuote(Options.ElfPath) + " --ir " +
              shellQuote(fallback.string()) + " >/dev/null 2>/dev/null";
    exitCode = std::system(command.c_str());
    if (exitCode == 0 && std::filesystem::exists(fallback)) {
      state.addNote("gtirb frontend ddisasm fell back to no-analysis for: " +
                    Options.ElfPath);
      return fallback;
    }

    state.addNote("gtirb frontend ddisasm failed for: " + Options.ElfPath);
    return std::nullopt;
  }

#if NOTDEC_BIN2LLVM_ENABLE_GTIRB
  static const gtirb::CodeBlock *codeBlockByUuid(const gtirb::Context &context,
                                                 const gtirb::UUID &uuid) {
    const gtirb::Node *node = gtirb::Node::getByUUID(context, uuid);
    if (node == nullptr || !gtirb::CodeBlock::classof(node)) {
      return nullptr;
    }
    return static_cast<const gtirb::CodeBlock *>(node);
  }

  static uint64_t codeBlockAddress(const gtirb::CodeBlock &block) {
    return static_cast<uint64_t>(*block.getAddress());
  }

  static std::string functionNameFor(const gtirb::Context &context,
                                     const gtirb::Module &module,
                                     const gtirb::UUID &functionUuid) {
    const auto *functionNames =
        module.getAuxData<gtirb::schema::FunctionNames>();
    if (functionNames == nullptr) {
      return "";
    }
    auto iterator = functionNames->find(functionUuid);
    if (iterator == functionNames->end()) {
      return "";
    }
    const gtirb::Node *node = gtirb::Node::getByUUID(context, iterator->second);
    if (node == nullptr || !gtirb::Symbol::classof(node)) {
      return "";
    }
    return static_cast<const gtirb::Symbol *>(node)->getName();
  }

  static void importFunctions(NativeProgramState &state,
                              const gtirb::Context &context,
                              const gtirb::Module &module,
                              const NativeRuntimeFilterOptions &runtimeFilter,
                              uint64_t &functionCount, uint64_t &blockCount) {
    const auto *functionEntries =
        module.getAuxData<gtirb::schema::FunctionEntries>();
    const auto *functionBlocks =
        module.getAuxData<gtirb::schema::FunctionBlocks>();
    if (functionEntries == nullptr || functionBlocks == nullptr) {
      state.addNote("gtirb frontend missing function auxdata");
      return;
    }

    // FunctionEntries is std::map<UUID, ...>: its iteration order is the
    // random UUID order ddisasm assigns, not address order.  Collect first and
    // sort by entry address before adding seeds/functions, otherwise the
    // discovery worklist (and any first-seed-wins range decision) changes from
    // run to run even for the same binary.
    struct CollectedFunction {
      NativeFunction Function;
      size_t RawBlockCount = 0;
    };
    std::vector<CollectedFunction> collected;
    for (const auto &[functionUuid, entryUuids] : *functionEntries) {
      if (entryUuids.empty()) {
        continue;
      }
      auto blocksIterator = functionBlocks->find(functionUuid);
      if (blocksIterator == functionBlocks->end()) {
        continue;
      }

      std::vector<NativeBasicBlock> blocks;
      uint64_t rangeStart = std::numeric_limits<uint64_t>::max();
      uint64_t rangeEnd = 0;
      for (const gtirb::UUID &blockUuid : blocksIterator->second) {
        const gtirb::CodeBlock *block = codeBlockByUuid(context, blockUuid);
        if (block == nullptr || !block->getAddress() || block->getSize() == 0) {
          continue;
        }
        uint64_t start = codeBlockAddress(*block);
        uint64_t end = start + block->getSize();
        if (!state.isExecutableAddress(start)) {
          continue;
        }
        NativeBasicBlock nativeBlock;
        nativeBlock.Start = start;
        nativeBlock.End = end;
        blocks.push_back(std::move(nativeBlock));
        rangeStart = std::min(rangeStart, start);
        rangeEnd = std::max(rangeEnd, end);
      }
      if (blocks.empty()) {
        continue;
      }

      uint64_t entryAddress = 0;
      for (const gtirb::UUID &entryUuid : entryUuids) {
        const gtirb::CodeBlock *entryBlock =
            codeBlockByUuid(context, entryUuid);
        if (entryBlock != nullptr && entryBlock->getAddress()) {
          entryAddress = codeBlockAddress(*entryBlock);
          break;
        }
      }
      if (entryAddress == 0) {
        continue;
      }

      std::sort(blocks.begin(), blocks.end(),
                [](const NativeBasicBlock &lhs,
                   const NativeBasicBlock &rhs) { return lhs.Start < rhs.Start; });
      std::string name = functionNameFor(context, module, functionUuid);
      if (runtimeFilter.SkipRuntimeFunctions &&
          (isNativeRuntimeFunctionName(name) ||
           isNativeRuntimeAddress(state, entryAddress))) {
        continue;
      }
      NativeFunction function;
      function.Entry = entryAddress;
      function.RangeStart = rangeStart;
      function.RangeEnd = rangeEnd;
      function.Name = std::move(name);
      function.Blocks = std::move(blocks);
      function.Source = "gtirb-ddisasm";
      collected.push_back(
          CollectedFunction{std::move(function), blocksIterator->second.size()});
    }

    std::sort(collected.begin(), collected.end(),
              [](const CollectedFunction &lhs, const CollectedFunction &rhs) {
                if (lhs.Function.Entry != rhs.Function.Entry) {
                  return lhs.Function.Entry < rhs.Function.Entry;
                }
                if (lhs.Function.RangeStart != rhs.Function.RangeStart) {
                  return lhs.Function.RangeStart < rhs.Function.RangeStart;
                }
                if (lhs.Function.RangeEnd != rhs.Function.RangeEnd) {
                  return lhs.Function.RangeEnd < rhs.Function.RangeEnd;
                }
                if (lhs.Function.Name != rhs.Function.Name) {
                  return lhs.Function.Name < rhs.Function.Name;
                }
                return lhs.Function.Blocks.size() < rhs.Function.Blocks.size();
              });

    for (CollectedFunction &item : collected) {
      NativeFunction &function = item.Function;
      state.addFunctionSeed(function.Entry,
                            function.RangeEnd - function.RangeStart,
                            function.Name, "gtirb-ddisasm",
                            NativeFunctionConfidence::High);
      if (state.addFunction(std::move(function))) {
        ++functionCount;
        blockCount += item.RawBlockCount;
      }
    }
  }

  static void importCfgEdges(NativeProgramState &state,
                             const gtirb::Module &module,
                             uint64_t &edgeCount) {
    const gtirb::CFG &cfg = module.getIR()->getCFG();
    std::map<uint64_t, const gtirb::CodeBlock *> codeBlocksByAddress;
    for (const gtirb::CodeBlock &block : module.code_blocks()) {
      if (!block.getAddress()) {
        continue;
      }
      uint64_t address = static_cast<uint64_t>(*block.getAddress());
      auto [iterator, inserted] = codeBlocksByAddress.emplace(address, &block);
      if (!inserted && block.getSize() > iterator->second->getSize()) {
        // Duplicate code blocks at one address exist for overlapping thunks.
        // module.code_blocks() order is GTIRB/UUID order; pick the larger block
        // explicitly so the CFG import does not depend on it.
        iterator->second = &block;
      }
    }

    for (const auto &[entry, function] : state.functions()) {
      (void)entry;
      std::set<uint64_t> blockStarts;
      for (const NativeBasicBlock &block : function.Blocks) {
        blockStarts.insert(block.Start);
      }
      // 原来的 functionContains 是线性扫本函数所有块；下面把它换成"按各块覆盖的
      // 地址区间查 code block"（见 GTIRB 补边那段），这里不再需要。
      auto addFlowXref = [&](uint64_t source, uint64_t target) {
        NativeXref xref;
        xref.From = source;
        xref.To = target;
        xref.Kind = NativeXrefKind::Flow;
        xref.Source = "gtirb-ddisasm-flow";
        state.addXref(std::move(xref));
      };
      auto addCallXref = [&](uint64_t source, uint64_t target) {
        NativeXref xref;
        xref.From = source;
        xref.To = target;
        xref.Kind = NativeXrefKind::Call;
        xref.Source = "gtirb-ddisasm-call";
        state.addXref(std::move(xref));
      };
      auto importBlockEdges = [&](uint64_t source,
                                  const gtirb::CodeBlock &gtirbBlock,
                                  bool attachSuccessors) {
        std::vector<uint64_t> successors;
        for (auto [successorNode, label] :
             gtirb::cfgSuccessors(cfg, &gtirbBlock)) {
          if (!label || !gtirb::CodeBlock::classof(successorNode)) {
            continue;
          }
          const auto &[conditional, direct, type] = *label;
          (void)conditional;
          (void)direct;
          const gtirb::CodeBlock *successor =
              static_cast<const gtirb::CodeBlock *>(successorNode);
          if (!successor->getAddress()) {
            continue;
          }
          uint64_t target = static_cast<uint64_t>(*successor->getAddress());
          if (type == gtirb::EdgeType::Branch ||
              type == gtirb::EdgeType::Fallthrough) {
            bool localTarget = blockStarts.count(target) != 0;
            if (attachSuccessors && localTarget) {
              successors.push_back(target);
            } else {
              addFlowXref(source, target);
            }
            ++edgeCount;
          } else if (type == gtirb::EdgeType::Call) {
            addCallXref(source, target);
          }
        }
        if (attachSuccessors) {
          std::sort(successors.begin(), successors.end());
          successors.erase(std::unique(successors.begin(), successors.end()),
                           successors.end());
          state.addBasicBlockSuccessors(function.Entry, source, successors);
        }
      };

      for (const NativeBasicBlock &block : function.Blocks) {
        auto gtirbBlock = codeBlocksByAddress.find(block.Start);
        if (gtirbBlock == codeBlocksByAddress.end()) {
          continue;
        }
        importBlockEdges(block.Start, *gtirbBlock->second,
                         /*attachSuccessors=*/true);
      }

      // GTIRB CFG can contain code-block boundaries that FunctionBlocks did not
      // list for this function.  Keep those edges as flow xrefs so the normalizer
      // can split the containing NativeBasicBlock before lowering.
      //
      // 只查本函数各块覆盖的地址区间，而不是"对每个函数扫全模块 code block 再线性
      // 判断是否落在函数里"：后者是 函数数 × 模块块数（wolfssl 上 importCfgEdges
      // 占到全程序 14.6% 采样、其中 std::set<uint64_t>::count 一项就 12%）。
      // 判定集合不变（落在某个块内、且不是块的起点），顺序仍按地址。
      std::vector<std::pair<uint64_t, const gtirb::CodeBlock *>> strayBlocks;
      for (const NativeBasicBlock &block : function.Blocks) {
        auto it = codeBlocksByAddress.lower_bound(block.Start);
        for (; it != codeBlocksByAddress.end() && it->first < block.End; ++it) {
          if (blockStarts.count(it->first) != 0) {
            continue;
          }
          strayBlocks.emplace_back(it->first, it->second);
        }
      }
      std::sort(strayBlocks.begin(), strayBlocks.end(),
                [](const auto &lhs, const auto &rhs) {
                  return lhs.first < rhs.first;
                });
      strayBlocks.erase(
          std::unique(strayBlocks.begin(), strayBlocks.end(),
                      [](const auto &lhs, const auto &rhs) {
                        return lhs.first == rhs.first;
                      }),
          strayBlocks.end());
      for (const auto &[address, gtirbBlock] : strayBlocks) {
        importBlockEdges(address, *gtirbBlock, /*attachSuccessors=*/false);
      }
    }
  }

  static uint64_t
  importSeedRanges(NativeProgramState &state,
                   const NativeRuntimeFilterOptions &runtimeFilter) {
    uint64_t imported = 0;
    for (const auto &[entry, seed] : state.functionSeeds()) {
      if (state.functionAt(entry) != nullptr ||
          seed.Confidence != NativeFunctionConfidence::High ||
          seed.RangeStart == 0 || seed.RangeEnd <= seed.RangeStart ||
          !state.isExecutableAddress(entry) ||
          !executableRangeContains(state, seed.RangeStart, seed.RangeEnd)) {
        continue;
      }
      if (runtimeFilter.SkipRuntimeFunctions &&
          isNativeRuntimeSeed(state, seed)) {
        continue;
      }

      NativeBasicBlock block;
      block.Start = seed.RangeStart;
      block.End = seed.RangeEnd;

      NativeFunction function;
      function.Entry = entry;
      function.RangeStart = seed.RangeStart;
      function.RangeEnd = seed.RangeEnd;
      function.Name = seed.PrimaryName;
      function.Blocks.push_back(std::move(block));
      function.Source = "gtirb-seed-range-fallback";
      function.IsExternallyVisible = seed.IsExternallyVisible;
      if (state.addFunction(std::move(function))) {
        ++imported;
      }
    }
    return imported;
  }
#endif
};

std::unique_ptr<NativeAnalyzer> createGtirbFunctionFactsAnalyzer(
    NativeGtirbDecodeOptions options) {
  return std::make_unique<GtirbFunctionFactsAnalyzer>(std::move(options));
}

} // namespace notdec::bin2llvm
