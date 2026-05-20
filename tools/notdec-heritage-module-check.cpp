#include "notdec-bin2llvm/HeritagePcode.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct CheckState {
  std::vector<std::string> Errors;
  unsigned DuplicateFunctionNameCount = 0;
  unsigned DirectCallCount = 0;
  unsigned ResolvedInternalCalls = 0;
  unsigned ResolvedExternalCalls = 0;
  unsigned UnknownCalls = 0;
};

void addError(CheckState &state, std::string message) {
  state.Errors.push_back(std::move(message));
}

void checkFunctionRefs(const notdec::bin2llvm::HeritageProgram &program,
                       CheckState &state) {
  for (const auto &param : program.Function.Params) {
    if (param.Varnode && !program.VarnodeById.count(*param.Varnode)) {
      addError(state,
               "function " + program.Function.Name +
                   " parameter references unknown varnode: " + *param.Varnode);
    }
  }

  for (const auto &block : program.Blocks) {
    for (const std::string &input : block.In) {
      if (!program.BlockById.count(input)) {
        addError(state, "function " + program.Function.Name + " block " +
                            block.Id +
                            " references unknown input block: " + input);
      }
    }
    for (const std::string &output : block.Out) {
      if (!program.BlockById.count(output)) {
        addError(state, "function " + program.Function.Name + " block " +
                            block.Id +
                            " references unknown output block: " + output);
      }
    }
    for (const std::string &op : block.Ops) {
      if (!program.OpById.count(op)) {
        addError(state, "function " + program.Function.Name + " block " +
                            block.Id + " references unknown op: " + op);
      }
    }
  }

  for (const auto &op : program.Ops) {
    if (!program.BlockById.count(op.Parent)) {
      addError(state, "function " + program.Function.Name + " op " + op.Id +
                          " references unknown parent block");
    }
    if (op.Output && !program.VarnodeById.count(*op.Output)) {
      addError(state, "function " + program.Function.Name + " op " + op.Id +
                          " references unknown output varnode");
    }
    for (const std::string &input : op.Inputs) {
      if (!program.VarnodeById.count(input)) {
        addError(state, "function " + program.Function.Name + " op " + op.Id +
                            " references unknown input varnode: " + input);
      }
    }
  }
}

void checkModuleSymbols(const notdec::bin2llvm::HeritageModule &module,
                        CheckState &state) {
  std::unordered_map<std::string, unsigned> names;
  std::unordered_map<std::string, unsigned> entries;
  for (const auto &function : module.Functions) {
    const auto &heritageFunction = function.Program.Function;
    names[heritageFunction.Name]++;
    entries[heritageFunction.Entry]++;
  }
  for (const auto &[name, count] : names) {
    if (count > 1) {
      state.DuplicateFunctionNameCount += count - 1;
    }
  }
  for (const auto &[entry, count] : entries) {
    if (count > 1) {
      addError(state, "duplicate function entry: " + entry);
    }
  }
}

void countCalls(const notdec::bin2llvm::HeritageModule &module,
                CheckState &state) {
  std::unordered_set<std::string> internalEntries;
  std::unordered_set<std::string> internalNames;
  std::unordered_set<std::string> externalNames;
  for (const auto &function : module.Functions) {
    internalEntries.insert(function.Program.Function.Entry);
    internalNames.insert(function.Program.Function.Name);
  }
  for (const auto &external : module.Externals) {
    externalNames.insert(external.Name);
  }

  for (const auto &function : module.Functions) {
    for (const auto &op : function.Program.Ops) {
      if (!op.CallTarget && !op.CallTargetName) {
        continue;
      }
      state.DirectCallCount++;
      if (op.CallTarget && internalEntries.count(*op.CallTarget)) {
        state.ResolvedInternalCalls++;
      } else if (op.CallTargetName && internalNames.count(*op.CallTargetName)) {
        state.ResolvedInternalCalls++;
      } else if (op.CallTargetName && externalNames.count(*op.CallTargetName)) {
        state.ResolvedExternalCalls++;
      } else {
        state.UnknownCalls++;
      }
    }
  }
}

int printSummary(const notdec::bin2llvm::HeritageModule &module,
                 const CheckState &state) {
  llvm::outs() << "heritage-module check\n";
  llvm::outs() << "  functions: " << module.Functions.size() << '\n';
  llvm::outs() << "  externals: " << module.Externals.size() << '\n';
  llvm::outs() << "  failures: " << module.Failures.size() << '\n';
  llvm::outs() << "  duplicate function names: "
               << state.DuplicateFunctionNameCount << '\n';
  llvm::outs() << "  direct calls: " << state.DirectCallCount << '\n';
  llvm::outs() << "  resolved internal calls: " << state.ResolvedInternalCalls
               << '\n';
  llvm::outs() << "  resolved external calls: " << state.ResolvedExternalCalls
               << '\n';
  llvm::outs() << "  unknown calls: " << state.UnknownCalls << '\n';

  if (state.Errors.empty()) {
    llvm::outs() << "  status: ok\n";
    return 0;
  }

  llvm::errs() << "  status: failed\n";
  for (const auto &error : state.Errors) {
    llvm::errs() << "  error: " << error << '\n';
  }
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    llvm::errs() << "usage: " << argv[0] << " <heritage-module.json>\n";
    return 1;
  }

  notdec::bin2llvm::HeritageModule module;
  std::string errorMessage;
  if (!notdec::bin2llvm::loadHeritageModuleFromJson(argv[1], module,
                                                    errorMessage)) {
    llvm::errs() << errorMessage << '\n';
    return 1;
  }

  CheckState state;
  if (module.Schema != "notdec.heritage-module.v0") {
    addError(state, "unexpected schema: " + module.Schema);
  }
  checkModuleSymbols(module, state);
  for (const auto &function : module.Functions) {
    checkFunctionRefs(function.Program, state);
  }
  countCalls(module, state);
  return printSummary(module, state);
}
