#include "notdec-bin2llvm/HeritagePcode.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct CheckState {
  std::vector<std::string> Errors;
  unsigned MissingParamVarnodes = 0;
  unsigned MultiequalCount = 0;
  unsigned RegisterVarnodeCount = 0;
  unsigned DirectCallCount = 0;
};

void addError(CheckState &state, std::string message) {
  state.Errors.push_back(std::move(message));
}

void checkSchema(const notdec::bin2llvm::HeritageProgram &program,
                 CheckState &state) {
  if (program.Schema != "notdec.heritage-pcode.v0") {
    addError(state, "unexpected schema: " + program.Schema);
  }
}

void checkFunction(const notdec::bin2llvm::HeritageProgram &program,
                   CheckState &state) {
  for (const auto &param : program.Function.Params) {
    if (!param.Varnode) {
      state.MissingParamVarnodes++;
      continue;
    }
    if (!program.VarnodeById.count(*param.Varnode)) {
      addError(state, "parameter references unknown varnode: " +
                          *param.Varnode);
    }
  }
}

void checkBlocks(const notdec::bin2llvm::HeritageProgram &program,
                 CheckState &state) {
  for (const auto &block : program.Blocks) {
    for (const std::string &input : block.In) {
      if (!program.BlockById.count(input)) {
        addError(state, "block " + block.Id +
                            " references unknown input block: " + input);
      }
    }
    for (const std::string &output : block.Out) {
      if (!program.BlockById.count(output)) {
        addError(state, "block " + block.Id +
                            " references unknown output block: " + output);
      }
    }
    for (const std::string &op : block.Ops) {
      if (!program.OpById.count(op)) {
        addError(state, "block " + block.Id + " references unknown op: " +
                            op);
      }
    }
  }
}

void checkOps(const notdec::bin2llvm::HeritageProgram &program,
              CheckState &state) {
  for (const auto &op : program.Ops) {
    if (!program.BlockById.count(op.Parent)) {
      addError(state, "op " + op.Id + " references unknown parent block");
    }

    if (op.Mnemonic == "MULTIEQUAL") {
      state.MultiequalCount++;
      auto block = program.BlockById.find(op.Parent);
      if (block != program.BlockById.end() &&
          block->second->In.size() != op.Inputs.size()) {
        addError(state, "MULTIEQUAL " + op.Id + " has " +
                            std::to_string(op.Inputs.size()) +
                            " input(s), but parent block has " +
                            std::to_string(block->second->In.size()) +
                            " predecessor(s)");
      }
    }

    if (op.CallTarget) {
      state.DirectCallCount++;
    }

    if (op.Output && !program.VarnodeById.count(*op.Output)) {
      addError(state, "op " + op.Id + " references unknown output varnode");
    }
    for (const std::string &input : op.Inputs) {
      if (!program.VarnodeById.count(input)) {
        addError(state, "op " + op.Id +
                            " references unknown input varnode: " + input);
      }
    }
  }
}

void countVarnodes(const notdec::bin2llvm::HeritageProgram &program,
                   CheckState &state) {
  for (const auto &varnode : program.Varnodes) {
    if (varnode.IsRegister) {
      state.RegisterVarnodeCount++;
    }
  }
}

int printSummary(const notdec::bin2llvm::HeritageProgram &program,
                 const CheckState &state) {
  llvm::outs() << "heritage-pcode check\n";
  llvm::outs() << "  blocks: " << program.Blocks.size() << '\n';
  llvm::outs() << "  ops: " << program.Ops.size() << '\n';
  llvm::outs() << "  varnodes: " << program.Varnodes.size() << '\n';
  llvm::outs() << "  params: " << program.Function.Params.size() << '\n';
  llvm::outs() << "  missing param varnodes: " << state.MissingParamVarnodes
               << '\n';
  llvm::outs() << "  register varnodes: " << state.RegisterVarnodeCount
               << '\n';
  llvm::outs() << "  MULTIEQUAL ops: " << state.MultiequalCount << '\n';
  llvm::outs() << "  direct calls: " << state.DirectCallCount << '\n';

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
    llvm::errs() << "usage: " << argv[0] << " <heritage-pcode.json>\n";
    return 1;
  }

  notdec::bin2llvm::HeritageProgram program;
  std::string errorMessage;
  if (!notdec::bin2llvm::loadHeritageProgramFromJson(argv[1], program,
                                                     errorMessage)) {
    llvm::errs() << errorMessage << '\n';
    return 1;
  }

  CheckState state;
  checkSchema(program, state);
  checkFunction(program, state);
  checkBlocks(program, state);
  checkOps(program, state);
  countVarnodes(program, state);
  return printSummary(program, state);
}
