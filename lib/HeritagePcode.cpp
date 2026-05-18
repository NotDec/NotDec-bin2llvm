#include "notdec-bin2llvm/HeritagePcode.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <charconv>
#include <sstream>

namespace notdec::bin2llvm {
namespace {

std::optional<std::string> getString(const llvm::json::Object &object,
                                     llvm::StringRef key) {
  if (auto value = object.getString(key)) {
    return value->str();
  }
  return std::nullopt;
}

std::optional<uint64_t> parseUnsigned(const std::string &value) {
  uint64_t result = 0;
  const char *begin = value.data();
  const char *end = value.data() + value.size();
  auto parsed = std::from_chars(begin, end, result);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return std::nullopt;
  }
  return result;
}

bool readStringArray(const llvm::json::Object &object, llvm::StringRef key,
                     std::vector<std::string> &out, std::string &errorMessage) {
  const auto *array = object.getArray(key);
  if (array == nullptr) {
    errorMessage = "missing array: " + key.str();
    return false;
  }

  for (const auto &value : *array) {
    auto string = value.getAsString();
    if (!string) {
      errorMessage = "non-string value in array: " + key.str();
      return false;
    }
    out.push_back(string->str());
  }
  return true;
}

bool readOptionalString(const llvm::json::Object &object, llvm::StringRef key,
                        std::optional<std::string> &out,
                        std::string &errorMessage) {
  const auto *value = object.get(key);
  if (value == nullptr || value->getAsNull()) {
    out.reset();
    return true;
  }
  auto string = value->getAsString();
  if (!string) {
    errorMessage = "field is not string or null: " + key.str();
    return false;
  }
  out = string->str();
  return true;
}

bool requireString(const llvm::json::Object &object, llvm::StringRef key,
                   std::string &out, std::string &errorMessage) {
  auto value = getString(object, key);
  if (!value) {
    errorMessage = "missing string: " + key.str();
    return false;
  }
  out = std::move(*value);
  return true;
}

bool readProgramInfo(const llvm::json::Object &root, HeritageProgramInfo &info,
                     std::string &errorMessage) {
  const auto *object = root.getObject("program");
  if (object == nullptr) {
    return true;
  }

  info.Name = getString(*object, "name").value_or("");
  info.Language = getString(*object, "language").value_or("");
  info.CompilerSpec = getString(*object, "compilerSpec").value_or("");
  info.SimplificationStyle =
      getString(*object, "simplificationStyle").value_or("");
  return true;
}

bool readParam(const llvm::json::Object &object, HeritageParam &param,
               std::string &errorMessage) {
  auto index = object.getInteger("index");
  if (!index || *index < 0) {
    errorMessage = "parameter missing non-negative index";
    return false;
  }
  param.Index = static_cast<uint32_t>(*index);
  return requireString(object, "name", param.Name, errorMessage) &&
         requireString(object, "type", param.Type, errorMessage) &&
         requireString(object, "storage", param.Storage, errorMessage) &&
         readOptionalString(object, "varnode", param.Varnode, errorMessage);
}

bool readFunctionObject(const llvm::json::Object &object,
                        HeritageFunction &function, std::string &errorMessage) {
  if (!requireString(object, "name", function.Name, errorMessage) ||
      !requireString(object, "entry", function.Entry, errorMessage) ||
      !requireString(object, "returnType", function.ReturnType, errorMessage)) {
    return false;
  }

  const auto *params = object.getArray("params");
  if (params == nullptr) {
    errorMessage = "function missing params array";
    return false;
  }
  for (const auto &paramValue : *params) {
    const auto *paramObject = paramValue.getAsObject();
    if (paramObject == nullptr) {
      errorMessage = "parameter is not object";
      return false;
    }
    HeritageParam param;
    if (!readParam(*paramObject, param, errorMessage)) {
      return false;
    }
    function.Params.push_back(std::move(param));
  }
  return true;
}

bool readFunction(const llvm::json::Object &root, HeritageFunction &function,
                  std::string &errorMessage) {
  const auto *object = root.getObject("function");
  if (object == nullptr) {
    errorMessage = "missing object: function";
    return false;
  }
  return readFunctionObject(*object, function, errorMessage);
}

bool readBlocks(const llvm::json::Object &root,
                std::vector<HeritageBlock> &blocks, std::string &errorMessage) {
  const auto *array = root.getArray("blocks");
  if (array == nullptr) {
    errorMessage = "missing array: blocks";
    return false;
  }
  for (const auto &blockValue : *array) {
    const auto *object = blockValue.getAsObject();
    if (object == nullptr) {
      errorMessage = "block is not object";
      return false;
    }
    HeritageBlock block;
    if (!requireString(*object, "id", block.Id, errorMessage) ||
        !requireString(*object, "start", block.Start, errorMessage) ||
        !readStringArray(*object, "in", block.In, errorMessage) ||
        !readStringArray(*object, "out", block.Out, errorMessage) ||
        !readStringArray(*object, "ops", block.Ops, errorMessage)) {
      return false;
    }
    blocks.push_back(std::move(block));
  }
  return true;
}

bool readOps(const llvm::json::Object &root, std::vector<HeritageOp> &ops,
             std::string &errorMessage) {
  const auto *array = root.getArray("ops");
  if (array == nullptr) {
    errorMessage = "missing array: ops";
    return false;
  }
  for (const auto &opValue : *array) {
    const auto *object = opValue.getAsObject();
    if (object == nullptr) {
      errorMessage = "op is not object";
      return false;
    }
    HeritageOp op;
    if (!requireString(*object, "id", op.Id, errorMessage) ||
        !requireString(*object, "parent", op.Parent, errorMessage) ||
        !requireString(*object, "seqTarget", op.SeqTarget, errorMessage) ||
        !requireString(*object, "mnemonic", op.Mnemonic, errorMessage) ||
        !readOptionalString(*object, "output", op.Output, errorMessage) ||
        !readStringArray(*object, "inputs", op.Inputs, errorMessage) ||
        !readOptionalString(*object, "callTarget", op.CallTarget,
                            errorMessage) ||
        !readOptionalString(*object, "callTargetName", op.CallTargetName,
                            errorMessage)) {
      return false;
    }
    ops.push_back(std::move(op));
  }
  return true;
}

bool readVarnodes(const llvm::json::Object &root,
                  std::vector<HeritageVarnode> &varnodes,
                  std::string &errorMessage) {
  const auto *array = root.getArray("varnodes");
  if (array == nullptr) {
    errorMessage = "missing array: varnodes";
    return false;
  }
  for (const auto &varnodeValue : *array) {
    const auto *object = varnodeValue.getAsObject();
    if (object == nullptr) {
      errorMessage = "varnode is not object";
      return false;
    }
    HeritageVarnode varnode;
    std::string offset;
    auto size = object->getInteger("size");
    if (!size || *size < 0) {
      errorMessage = "varnode missing non-negative size";
      return false;
    }
    if (!requireString(*object, "id", varnode.Id, errorMessage) ||
        !requireString(*object, "space", varnode.Space, errorMessage) ||
        !requireString(*object, "offset", offset, errorMessage) ||
        !requireString(*object, "address", varnode.Address, errorMessage) ||
        !readOptionalString(*object, "registerName", varnode.RegisterName,
                            errorMessage) ||
        !readOptionalString(*object, "highVariable", varnode.HighVariable,
                            errorMessage) ||
        !readOptionalString(*object, "highType", varnode.HighType,
                            errorMessage)) {
      return false;
    }
    auto parsedOffset = parseUnsigned(offset);
    if (!parsedOffset) {
      errorMessage = "invalid varnode offset: " + offset;
      return false;
    }
    varnode.Offset = *parsedOffset;
    varnode.Size = static_cast<uint32_t>(*size);
    varnode.IsConstant = object->getBoolean("isConstant").value_or(false);
    varnode.IsRegister = object->getBoolean("isRegister").value_or(false);
    varnode.IsInput = object->getBoolean("isInput").value_or(false);
    varnode.IsAddressTied =
        object->getBoolean("isAddressTied").value_or(false);
    varnodes.push_back(std::move(varnode));
  }
  return true;
}

bool readModuleFunction(const llvm::json::Object &root,
                        const HeritageProgramInfo &programInfo,
                        HeritageModuleFunction &function,
                        std::string &errorMessage) {
  function.Program.Schema = "notdec.heritage-pcode.v0";
  function.Program.Program = programInfo;
  if (!readFunctionObject(root, function.Program.Function, errorMessage) ||
      !readBlocks(root, function.Program.Blocks, errorMessage) ||
      !readOps(root, function.Program.Ops, errorMessage) ||
      !readVarnodes(root, function.Program.Varnodes, errorMessage)) {
    return false;
  }
  function.Status = getString(root, "status").value_or("ok");
  function.ErrorMessage = getString(root, "errorMessage").value_or("");
  indexHeritageProgram(function.Program);
  return true;
}

bool readExternalFunction(const llvm::json::Object &object,
                          HeritageExternalFunction &external,
                          std::string &errorMessage) {
  if (!requireString(object, "name", external.Name, errorMessage)) {
    return false;
  }
  external.Address = getString(object, "address").value_or("");
  external.ReturnType = getString(object, "returnType").value_or("void");
  external.Source = getString(object, "source").value_or("");

  const auto *params = object.getArray("params");
  if (params == nullptr) {
    return true;
  }
  for (const auto &paramValue : *params) {
    const auto *paramObject = paramValue.getAsObject();
    if (paramObject == nullptr) {
      errorMessage = "external parameter is not object";
      return false;
    }
    HeritageParam param;
    if (!readParam(*paramObject, param, errorMessage)) {
      return false;
    }
    external.Params.push_back(std::move(param));
  }
  return true;
}

bool readFailure(const llvm::json::Object &object,
                 HeritageModuleFailure &failure, std::string &errorMessage) {
  failure.Entry = getString(object, "entry").value_or("");
  failure.Name = getString(object, "name").value_or("");
  failure.Stage = getString(object, "stage").value_or("");
  failure.Message = getString(object, "message").value_or("");
  return true;
}

} // namespace

void indexHeritageProgram(HeritageProgram &program) {
  program.BlockById.clear();
  program.OpById.clear();
  program.VarnodeById.clear();
  program.BlockByStart.clear();

  for (const HeritageBlock &block : program.Blocks) {
    program.BlockById.emplace(block.Id, &block);
    program.BlockByStart.emplace(block.Start, &block);
  }
  for (const HeritageOp &op : program.Ops) {
    program.OpById.emplace(op.Id, &op);
  }
  for (const HeritageVarnode &varnode : program.Varnodes) {
    program.VarnodeById.emplace(varnode.Id, &varnode);
  }
}

bool loadHeritageProgramFromJson(const std::string &path,
                                 HeritageProgram &program,
                                 std::string &errorMessage) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    errorMessage =
        "failed to read " + path + ": " + buffer.getError().message();
    return false;
  }

  auto parsed = llvm::json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    errorMessage =
        "failed to parse JSON: " + llvm::toString(parsed.takeError());
    return false;
  }

  const auto *root = parsed->getAsObject();
  if (root == nullptr) {
    errorMessage = "top-level JSON value must be an object";
    return false;
  }

  HeritageProgram result;
  if (!requireString(*root, "schema", result.Schema, errorMessage) ||
      !readProgramInfo(*root, result.Program, errorMessage) ||
      !readFunction(*root, result.Function, errorMessage) ||
      !readBlocks(*root, result.Blocks, errorMessage) ||
      !readOps(*root, result.Ops, errorMessage) ||
      !readVarnodes(*root, result.Varnodes, errorMessage)) {
    return false;
  }

  indexHeritageProgram(result);
  program = std::move(result);
  indexHeritageProgram(program);
  return true;
}

bool loadHeritageModuleFromJson(const std::string &path, HeritageModule &module,
                                std::string &errorMessage) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    errorMessage =
        "failed to read " + path + ": " + buffer.getError().message();
    return false;
  }

  auto parsed = llvm::json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    errorMessage =
        "failed to parse JSON: " + llvm::toString(parsed.takeError());
    return false;
  }

  const auto *root = parsed->getAsObject();
  if (root == nullptr) {
    errorMessage = "top-level JSON value must be an object";
    return false;
  }

  HeritageModule result;
  if (!requireString(*root, "schema", result.Schema, errorMessage) ||
      !readProgramInfo(*root, result.Program, errorMessage)) {
    return false;
  }

  const auto *functions = root->getArray("functions");
  if (functions == nullptr) {
    errorMessage = "missing array: functions";
    return false;
  }
  for (const auto &functionValue : *functions) {
    const auto *functionObject = functionValue.getAsObject();
    if (functionObject == nullptr) {
      errorMessage = "module function is not object";
      return false;
    }
    HeritageModuleFunction function;
    if (!readModuleFunction(*functionObject, result.Program, function,
                            errorMessage)) {
      return false;
    }
    result.Functions.push_back(std::move(function));
  }

  if (const auto *externals = root->getArray("externals")) {
    for (const auto &externalValue : *externals) {
      const auto *externalObject = externalValue.getAsObject();
      if (externalObject == nullptr) {
        errorMessage = "external function is not object";
        return false;
      }
      HeritageExternalFunction external;
      if (!readExternalFunction(*externalObject, external, errorMessage)) {
        return false;
      }
      result.Externals.push_back(std::move(external));
    }
  }

  if (const auto *failures = root->getArray("failures")) {
    for (const auto &failureValue : *failures) {
      const auto *failureObject = failureValue.getAsObject();
      if (failureObject == nullptr) {
        errorMessage = "failure is not object";
        return false;
      }
      HeritageModuleFailure failure;
      if (!readFailure(*failureObject, failure, errorMessage)) {
        return false;
      }
      result.Failures.push_back(std::move(failure));
    }
  }

  module = std::move(result);
  return true;
}

} // namespace notdec::bin2llvm
