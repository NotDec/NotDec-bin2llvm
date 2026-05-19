#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace notdec::bin2llvm {

// Data exported from Ghidra HighFunction after heritage has built SSA.  Keep
// this close to the JSON schema for now; the next lowering only needs ids and
// simple metadata, not a new general-purpose IR.
struct HeritageVarnode {
  std::string Id;
  std::string Space;
  std::string Address;
  uint64_t Offset = 0;
  uint32_t Size = 0;
  bool IsConstant = false;
  bool IsRegister = false;
  bool IsInput = false;
  bool IsAddressTied = false;
  std::optional<std::string> RegisterName;
  std::optional<std::string> HighVariable;
  std::optional<std::string> HighType;
};

struct HeritageOp {
  std::string Id;
  std::string Parent;
  std::string SeqTarget;
  std::string Mnemonic;
  std::optional<std::string> Output;
  std::vector<std::string> Inputs;
  std::optional<std::string> CallTarget;
  std::optional<std::string> CallTargetName;
  std::optional<std::string> EffectOp;
};

struct HeritageBlock {
  std::string Id;
  std::string Start;
  std::vector<std::string> In;
  std::vector<std::string> Out;
  std::vector<std::string> Ops;
};

struct HeritageParam {
  uint32_t Index = 0;
  std::string Name;
  std::string Type;
  std::string Storage;
  std::optional<std::string> Varnode;
};

struct HeritageFunction {
  std::string Name;
  std::string Entry;
  std::string ReturnType;
  std::vector<HeritageParam> Params;
};

struct HeritageProgramInfo {
  std::string Name;
  std::string Language;
  std::string CompilerSpec;
  std::string SimplificationStyle;
};

struct HeritageProgram {
  std::string Schema;
  HeritageProgramInfo Program;
  HeritageFunction Function;
  std::vector<HeritageBlock> Blocks;
  std::vector<HeritageOp> Ops;
  std::vector<HeritageVarnode> Varnodes;
  std::unordered_map<std::string, const HeritageBlock *> BlockById;
  std::unordered_map<std::string, const HeritageOp *> OpById;
  std::unordered_map<std::string, const HeritageVarnode *> VarnodeById;
  std::unordered_map<std::string, std::vector<const HeritageBlock *>>
      BlockByStart;
};

// One module function owns the same per-function data as the old schema.  The
// wrapper keeps status/error near the function so module lowering can leave a
// failed function as a declaration without losing its symbol.
struct HeritageModuleFunction {
  HeritageProgram Program;
  std::string Status;
  std::string ErrorMessage;
};

struct HeritageExternalFunction {
  std::string Name;
  std::string Address;
  std::string ReturnType;
  std::vector<HeritageParam> Params;
  std::string Source;
};

struct HeritageModuleFailure {
  std::string Entry;
  std::string Name;
  std::string Stage;
  std::string Message;
};

// Module-level JSON is intentionally a thin container around function-level
// heritage data.  This keeps the old single-function path stable while adding
// enough symbol tables for a real LLVM module shell.
struct HeritageModule {
  std::string Schema;
  HeritageProgramInfo Program;
  std::vector<HeritageModuleFunction> Functions;
  std::vector<HeritageExternalFunction> Externals;
  std::vector<HeritageModuleFailure> Failures;
};

bool loadHeritageProgramFromJson(const std::string &path,
                                 HeritageProgram &program,
                                 std::string &errorMessage);

bool loadHeritageModuleFromJson(const std::string &path, HeritageModule &module,
                                std::string &errorMessage);

void indexHeritageProgram(HeritageProgram &program);

} // namespace notdec::bin2llvm
