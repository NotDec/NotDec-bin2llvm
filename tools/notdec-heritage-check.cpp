#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Keep this checker intentionally small. It validates the exported shape before
// we commit to a native lowering data model for heritage P-Code.
struct CheckState {
  std::unordered_set<std::string> Blocks;
  std::unordered_set<std::string> Ops;
  std::unordered_set<std::string> Varnodes;
  std::unordered_map<std::string, unsigned> BlockInputCounts;
  std::vector<std::string> Errors;
  unsigned ParamCount = 0;
  unsigned MissingParamVarnodes = 0;
  unsigned OpCount = 0;
  unsigned MultiequalCount = 0;
  unsigned RegisterVarnodeCount = 0;
  unsigned DirectCallCount = 0;
};

void addError(CheckState &state, std::string message) {
  state.Errors.push_back(std::move(message));
}

std::string asString(llvm::StringRef value) { return value.str(); }

std::optional<std::string> getString(const llvm::json::Object &object,
                                     llvm::StringRef key) {
  if (auto value = object.getString(key)) {
    return value->str();
  }
  return std::nullopt;
}

const llvm::json::Array *requireArray(const llvm::json::Object &object,
                                      llvm::StringRef key,
                                      CheckState &state) {
  const auto *array = object.getArray(key);
  if (array == nullptr) {
    addError(state, "missing array: " + key.str());
  }
  return array;
}

const llvm::json::Object *requireObject(const llvm::json::Value &value,
                                        llvm::StringRef what,
                                        CheckState &state) {
  const auto *object = value.getAsObject();
  if (object == nullptr) {
    addError(state, "expected object in " + what.str());
  }
  return object;
}

bool isNullValue(const llvm::json::Value *value) {
  return value == nullptr || value->getAsNull().has_value();
}

void collectBlocks(const llvm::json::Object &root, CheckState &state) {
  const auto *blocks = requireArray(root, "blocks", state);
  if (blocks == nullptr) {
    return;
  }

  for (const auto &blockValue : *blocks) {
    const auto *block = requireObject(blockValue, "blocks", state);
    if (block == nullptr) {
      continue;
    }
    auto id = getString(*block, "id");
    if (!id) {
      addError(state, "block missing id");
      continue;
    }
    state.Blocks.insert(*id);
    if (const auto *in = block->getArray("in")) {
      state.BlockInputCounts[*id] = in->size();
    }
  }
}

void collectVarnodes(const llvm::json::Object &root, CheckState &state) {
  const auto *varnodes = requireArray(root, "varnodes", state);
  if (varnodes == nullptr) {
    return;
  }

  for (const auto &varnodeValue : *varnodes) {
    const auto *varnode = requireObject(varnodeValue, "varnodes", state);
    if (varnode == nullptr) {
      continue;
    }
    auto id = getString(*varnode, "id");
    if (!id) {
      addError(state, "varnode missing id");
      continue;
    }
    state.Varnodes.insert(*id);
    if (auto isRegister = varnode->getBoolean("isRegister");
        isRegister && *isRegister) {
      state.RegisterVarnodeCount++;
    }
  }
}

void collectOps(const llvm::json::Object &root, CheckState &state) {
  const auto *ops = requireArray(root, "ops", state);
  if (ops == nullptr) {
    return;
  }

  for (const auto &opValue : *ops) {
    const auto *op = requireObject(opValue, "ops", state);
    if (op == nullptr) {
      continue;
    }
    auto id = getString(*op, "id");
    if (!id) {
      addError(state, "op missing id");
      continue;
    }
    state.Ops.insert(*id);
  }
}

void checkFunction(const llvm::json::Object &root, CheckState &state) {
  const auto *function = root.getObject("function");
  if (function == nullptr) {
    addError(state, "missing object: function");
    return;
  }
  const auto *params = function->getArray("params");
  if (params == nullptr) {
    addError(state, "function missing params array");
    return;
  }

  state.ParamCount = params->size();
  for (const auto &paramValue : *params) {
    const auto *param = requireObject(paramValue, "function.params", state);
    if (param == nullptr) {
      continue;
    }
    auto *varnodeValue = param->get("varnode");
    if (isNullValue(varnodeValue)) {
      state.MissingParamVarnodes++;
      continue;
    }
    auto varnode = varnodeValue->getAsString();
    if (!varnode) {
      addError(state, "parameter varnode is not a string");
      continue;
    }
    if (!state.Varnodes.count(asString(*varnode))) {
      addError(state, "parameter references unknown varnode: " +
                          asString(*varnode));
    }
  }
}

void checkBlockReferences(const llvm::json::Object &root, CheckState &state) {
  const auto *blocks = root.getArray("blocks");
  if (blocks == nullptr) {
    return;
  }

  for (const auto &blockValue : *blocks) {
    const auto *block = blockValue.getAsObject();
    if (block == nullptr) {
      continue;
    }
    auto id = getString(*block, "id").value_or("<unknown>");
    for (llvm::StringRef edgeName : {"in", "out"}) {
      const auto *edges = block->getArray(edgeName);
      if (edges == nullptr) {
        addError(state, "block " + id + " missing " + edgeName.str() +
                            " array");
        continue;
      }
      for (const auto &edgeValue : *edges) {
        auto edge = edgeValue.getAsString();
        if (!edge) {
          addError(state, "block " + id + " has non-string " +
                              edgeName.str() + " edge");
          continue;
        }
        if (!state.Blocks.count(asString(*edge))) {
          addError(state, "block " + id + " references unknown block: " +
                              asString(*edge));
        }
      }
    }

    const auto *ops = block->getArray("ops");
    if (ops == nullptr) {
      addError(state, "block " + id + " missing ops array");
      continue;
    }
    for (const auto &opValue : *ops) {
      auto op = opValue.getAsString();
      if (!op) {
        addError(state, "block " + id + " has non-string op reference");
        continue;
      }
      if (!state.Ops.count(asString(*op))) {
        addError(state, "block " + id + " references unknown op: " +
                            asString(*op));
      }
    }
  }
}

void checkOpReferences(const llvm::json::Object &root, CheckState &state) {
  const auto *ops = root.getArray("ops");
  if (ops == nullptr) {
    return;
  }

  for (const auto &opValue : *ops) {
    const auto *op = opValue.getAsObject();
    if (op == nullptr) {
      continue;
    }
    auto id = getString(*op, "id").value_or("<unknown>");
    state.OpCount++;

    auto parent = getString(*op, "parent");
    if (!parent || !state.Blocks.count(*parent)) {
      addError(state, "op " + id + " references unknown parent block");
    }

    auto mnemonic = getString(*op, "mnemonic").value_or("");
    if (mnemonic == "MULTIEQUAL") {
      state.MultiequalCount++;
    }
    if (auto callTarget = getString(*op, "callTarget"); callTarget) {
      state.DirectCallCount++;
    }

    auto *outputValue = op->get("output");
    if (!isNullValue(outputValue)) {
      auto output = outputValue->getAsString();
      if (!output || !state.Varnodes.count(asString(*output))) {
        addError(state, "op " + id + " references unknown output varnode");
      }
    }

    const auto *inputs = op->getArray("inputs");
    if (inputs == nullptr) {
      addError(state, "op " + id + " missing inputs array");
      continue;
    }
    for (const auto &inputValue : *inputs) {
      auto input = inputValue.getAsString();
      if (!input || !state.Varnodes.count(asString(*input))) {
        addError(state, "op " + id + " references unknown input varnode");
      }
    }

    if (mnemonic == "MULTIEQUAL" && parent) {
      auto blockInputs = state.BlockInputCounts.find(*parent);
      if (blockInputs != state.BlockInputCounts.end() &&
          blockInputs->second != inputs->size()) {
        addError(state, "MULTIEQUAL " + id + " has " +
                            std::to_string(inputs->size()) +
                            " input(s), but parent block has " +
                            std::to_string(blockInputs->second) +
                            " predecessor(s)");
      }
    }
  }
}

int printSummary(const CheckState &state) {
  llvm::outs() << "heritage-pcode check\n";
  llvm::outs() << "  blocks: " << state.Blocks.size() << '\n';
  llvm::outs() << "  ops: " << state.OpCount << '\n';
  llvm::outs() << "  varnodes: " << state.Varnodes.size() << '\n';
  llvm::outs() << "  params: " << state.ParamCount << '\n';
  llvm::outs() << "  missing param varnodes: " << state.MissingParamVarnodes
               << '\n';
  llvm::outs() << "  register varnodes: " << state.RegisterVarnodeCount << '\n';
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

  auto buffer = llvm::MemoryBuffer::getFile(argv[1]);
  if (!buffer) {
    llvm::errs() << "failed to read " << argv[1] << ": "
                 << buffer.getError().message() << '\n';
    return 1;
  }

  auto parsed = llvm::json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    llvm::errs() << "failed to parse JSON: "
                 << llvm::toString(parsed.takeError()) << '\n';
    return 1;
  }

  const auto *root = parsed->getAsObject();
  if (root == nullptr) {
    llvm::errs() << "top-level JSON value must be an object\n";
    return 1;
  }

  CheckState state;
  auto schema = getString(*root, "schema");
  if (!schema || *schema != "notdec.heritage-pcode.v0") {
    addError(state, "unexpected or missing schema");
  }

  collectBlocks(*root, state);
  collectVarnodes(*root, state);
  collectOps(*root, state);
  checkFunction(*root, state);
  checkBlockReferences(*root, state);
  checkOpReferences(*root, state);

  return printSummary(state);
}
