#include "notdec-bin2llvm/core/NativePipelineSpec.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace notdec::bin2llvm {
namespace {

namespace fs = std::filesystem;
using llvm::StringRef;
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

bool fail(std::string &Error, const llvm::Twine &Path,
          const llvm::Twine &Message) {
  Error = (Path + ": " + Message).str();
  return false;
}

std::string joinPath(StringRef Prefix, StringRef Key) {
  if (Prefix.empty()) {
    return Key.str();
  }
  return (Prefix + "." + Key).str();
}

bool checkKeys(const Object &Obj, llvm::ArrayRef<StringRef> Allowed,
               StringRef Path, std::string &Error) {
  for (const auto &Entry : Obj) {
    StringRef Key = Entry.first;
    if (llvm::find(Allowed, Key) == Allowed.end()) {
      return fail(Error, Path,
                  "unknown key '" + Key + "' (allowed: " +
                      llvm::join(Allowed, ", ") + ")");
    }
  }
  return true;
}

bool readBool(const Object &Obj, StringRef Key, bool &Out, StringRef Path,
              std::string &Error) {
  const Value *V = Obj.get(Key);
  if (V == nullptr) {
    return true;
  }
  std::optional<bool> Parsed = V->getAsBoolean();
  if (!Parsed) {
    return fail(Error, joinPath(Path, Key), "expected bool");
  }
  Out = *Parsed;
  return true;
}

bool readString(const Object &Obj, StringRef Key, std::string &Out,
                StringRef Path, std::string &Error) {
  const Value *V = Obj.get(Key);
  if (V == nullptr) {
    return true;
  }
  std::optional<StringRef> Parsed = V->getAsString();
  if (!Parsed) {
    return fail(Error, joinPath(Path, Key), "expected string");
  }
  Out = Parsed->str();
  return true;
}

bool readStringArray(const Object &Obj, StringRef Key,
                     std::vector<std::string> &Out, StringRef Path,
                     std::string &Error) {
  const Value *V = Obj.get(Key);
  if (V == nullptr) {
    return true;
  }
  const Array *Items = V->getAsArray();
  if (Items == nullptr) {
    return fail(Error, joinPath(Path, Key), "expected array of strings");
  }
  Out.clear();
  for (size_t I = 0; I < Items->size(); ++I) {
    std::optional<StringRef> Item = (*Items)[I].getAsString();
    if (!Item) {
      return fail(Error, joinPath(Path, Key) + "[" + llvm::Twine(I) + "]",
                  "expected string");
    }
    Out.push_back(Item->str());
  }
  return true;
}

bool parseUint64Text(StringRef Text, uint64_t &Out) {
  if (Text.empty()) {
    return false;
  }
  std::string Buffer = Text.str();
  size_t End = 0;
  unsigned long long Parsed = 0;
  try {
    Parsed = std::stoull(Buffer, &End, 0);
  } catch (const std::exception &) {
    return false;
  }
  if (End != Buffer.size()) {
    return false;
  }
  Out = static_cast<uint64_t>(Parsed);
  return true;
}

// --- 选择器 ----------------------------------------------------------------

bool parseSelector(const Value &V, StringRef Path, NativeFunctionSelector &Out,
                   std::string &Error) {
  const Object *Obj = V.getAsObject();
  if (Obj == nullptr) {
    return fail(Error, Path, "expected object");
  }
  if (!checkKeys(*Obj, {"name", "glob", "symbol", "address", "optional"},
                 Path, Error)) {
    return false;
  }
  std::string Name;
  std::string Glob;
  std::string Symbol;
  if (!readString(*Obj, "name", Name, Path, Error) ||
      !readString(*Obj, "glob", Glob, Path, Error) ||
      !readString(*Obj, "symbol", Symbol, Path, Error) ||
      !readBool(*Obj, "optional", Out.Optional, Path, Error)) {
    return false;
  }
  if (!Name.empty()) {
    Out.Name = std::move(Name);
  }
  if (!Glob.empty()) {
    Out.Glob = std::move(Glob);
  }
  if (!Symbol.empty()) {
    Out.Symbol = std::move(Symbol);
  }
  if (const Value *Addr = Obj->get("address")) {
    if (std::optional<StringRef> Text = Addr->getAsString()) {
      uint64_t Parsed = 0;
      if (!parseUint64Text(*Text, Parsed)) {
        return fail(Error, joinPath(Path, "address"),
                    "expected integer or numeric string");
      }
      Out.Address = Parsed;
    } else if (std::optional<int64_t> Parsed = Addr->getAsInteger()) {
      if (*Parsed < 0) {
        return fail(Error, joinPath(Path, "address"),
                    "expected non-negative address");
      }
      Out.Address = static_cast<uint64_t>(*Parsed);
    } else {
      return fail(Error, joinPath(Path, "address"),
                  "expected integer or numeric string");
    }
  }
  if (Out.empty()) {
    return fail(Error, Path, "select needs at least one of name/glob/symbol/address");
  }
  return true;
}

bool parseSelectors(const Array &Items, StringRef Path,
                    std::vector<NativeFunctionSelector> &Out,
                    std::string &Error) {
  Out.clear();
  for (size_t I = 0; I < Items.size(); ++I) {
    NativeFunctionSelector Selector;
    std::string ItemPath =
        (Path + "[" + llvm::Twine(I) + "]").str();
    if (!parseSelector(Items[I], ItemPath, Selector, Error)) {
      return false;
    }
    Out.push_back(std::move(Selector));
  }
  return true;
}

bool readSelectors(const Object &Obj, StringRef Key,
                   std::vector<NativeFunctionSelector> &Out, StringRef Path,
                   std::string &Error) {
  const Value *V = Obj.get(Key);
  if (V == nullptr) {
    return true;
  }
  const Array *Items = V->getAsArray();
  if (Items == nullptr) {
    return fail(Error, joinPath(Path, Key), "expected array of selectors");
  }
  return parseSelectors(*Items, joinPath(Path, Key), Out, Error);
}

// --- 属性 ------------------------------------------------------------------

bool parseAttributes(const Value &V, StringRef Path,
                     NativeFunctionAttributes &Out, std::string &Error) {
  const Object *Obj = V.getAsObject();
  if (Obj == nullptr) {
    return fail(Error, Path, "expected object");
  }
  if (!checkKeys(*Obj,
                 {"noreturn", "nounwind", "willreturn", "noinline",
                  "alwaysinline", "cold", "memory"},
                 Path, Error)) {
    return false;
  }
  // readBool 只在 key 存在时写值；这里需要区分"没写"和"写了 false"。
  auto readOptionalBool = [&](StringRef Key,
                              std::optional<bool> &Slot) -> bool {
    const Value *Item = Obj->get(Key);
    if (Item == nullptr) {
      return true;
    }
    std::optional<bool> Parsed = Item->getAsBoolean();
    if (!Parsed) {
      return fail(Error, joinPath(Path, Key), "expected bool");
    }
    Slot = *Parsed;
    return true;
  };
  Out = NativeFunctionAttributes{};
  if (!readOptionalBool("noreturn", Out.NoReturn) ||
      !readOptionalBool("nounwind", Out.NoUnwind) ||
      !readOptionalBool("willreturn", Out.WillReturn) ||
      !readOptionalBool("noinline", Out.NoInline) ||
      !readOptionalBool("alwaysinline", Out.AlwaysInline) ||
      !readOptionalBool("cold", Out.Cold)) {
    return false;
  }
  if (const Value *Memory = Obj->get("memory")) {
    std::optional<StringRef> Text = Memory->getAsString();
    if (!Text) {
      return fail(Error, joinPath(Path, "memory"), "expected string");
    }
    std::string Value = Text->str();
    if (Value != "none" && Value != "read" && Value != "write" &&
        Value != "readwrite" && Value != "argmem" &&
        Value != "inaccessiblemem") {
      return fail(Error, joinPath(Path, "memory"),
                  "expected one of none/read/write/readwrite/argmem/"
                  "inaccessiblemem");
    }
    Out.Memory = std::move(Value);
  }
  if (Out.empty()) {
    return fail(Error, Path, "attributes object is empty");
  }
  return true;
}

// --- 路径解析 --------------------------------------------------------------

std::string normalizePath(const fs::path &Path) {
  std::string Result = Path.lexically_normal().string();
  while (Result.size() > 1 &&
         Result.back() == static_cast<char>(fs::path::preferred_separator)) {
    Result.pop_back();
  }
  return Result;
}

std::string resolveAgainstRoot(const std::string &Root,
                               const std::string &Path) {
  fs::path Candidate(Path);
  if (Candidate.is_absolute()) {
    return normalizePath(Candidate);
  }
  return normalizePath(fs::path(Root) / Candidate);
}

bool hashFile(const std::string &Path, std::string &Sha256,
              std::string &Error) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(Path);
  if (!Buffer) {
    Error = "failed to read " + Path + ": " + Buffer.getError().message();
    return false;
  }
  llvm::SHA256 Hash;
  Hash.update((*Buffer)->getBuffer());
  std::array<uint8_t, 32> Digest = Hash.final();
  Sha256 = llvm::toHex(Digest, /*LowerCase=*/true);
  return true;
}

// --- 各 stage 的 options ----------------------------------------------------

bool parseInstCombineOptions(const Object &Options, StringRef Path,
                             NativeStageConfig &Stage, std::string &Error) {
  if (!checkKeys(Options, {"early_cse", "exclude"}, Path, Error)) {
    return false;
  }
  if (!readBool(Options, "early_cse", Stage.InstCombine.EarlyCSE, Path,
                Error)) {
    return false;
  }
  return readSelectors(Options, "exclude", Stage.InstCombine.Exclude, Path,
                       Error);
}

bool parseRegisterEliminationOptions(const Object &Options, StringRef Path,
                                     NativeStageConfig &Stage,
                                     std::string &Error) {
  if (!checkKeys(Options,
                 {"rewrite", "residue_removal", "post_rewrite_instcombine",
                  "vararg_recovery", "abi_for_all_functions",
                  "attach_metadata", "external_prototypes",
                  "ignored_registers"},
                 Path, Error)) {
    return false;
  }
  NativeRegisterEliminationConfig &Elimination = Stage.RegisterElimination;
  if (!readBool(Options, "rewrite", Elimination.Rewrite, Path, Error) ||
      !readBool(Options, "residue_removal", Elimination.ResidueRemoval, Path,
                Error) ||
      !readBool(Options, "post_rewrite_instcombine",
                Elimination.PostRewriteInstCombine, Path, Error) ||
      !readBool(Options, "vararg_recovery", Elimination.VarArgRecovery, Path,
                Error) ||
      !readBool(Options, "abi_for_all_functions",
                Elimination.AbiForAllFunctions, Path, Error) ||
      !readBool(Options, "attach_metadata", Elimination.AttachMetadata, Path,
                Error) ||
      !readString(Options, "external_prototypes",
                  Elimination.ExternalPrototypes, Path, Error)) {
    return false;
  }
  return readStringArray(Options, "ignored_registers",
                         Elimination.IgnoredRegisters, Path, Error);
}

bool parseIROverrideOptions(const Object &Options, StringRef Path,
                            NativeStageConfig &Stage, std::string &Error) {
  if (!checkKeys(Options,
                 {"file", "select", "entry", "allow_undefined", "strip_debug",
                  "attributes_from"},
                 Path, Error)) {
    return false;
  }
  NativeIROverrideOptions &Override = Stage.IROverride;
  if (!readString(Options, "file", Override.File, Path, Error)) {
    return false;
  }
  if (Override.File.empty()) {
    return fail(Error, joinPath(Path, "file"), "missing required key");
  }
  if (!readSelectors(Options, "select", Override.Select, Path, Error) ||
      !readString(Options, "entry", Override.Entry, Path, Error) ||
      !readBool(Options, "allow_undefined", Override.AllowUndefined, Path,
                Error) ||
      !readBool(Options, "strip_debug", Override.StripDebug, Path, Error)) {
    return false;
  }
  std::string AttributesFrom = "target";
  if (!readString(Options, "attributes_from", AttributesFrom, Path, Error)) {
    return false;
  }
  if (AttributesFrom != "target" && AttributesFrom != "override") {
    return fail(Error, joinPath(Path, "attributes_from"),
                "expected target or override");
  }
  Override.AttributesFromOverride = AttributesFrom == "override";
  return true;
}

bool parseFinalCleanupOptions(const Object &Options, StringRef Path,
                              NativeStageConfig &Stage, std::string &Error) {
  if (!checkKeys(Options, {"global_dce", "preserve_address_taken"}, Path,
                 Error)) {
    return false;
  }
  NativeFinalCleanupOptions &Cleanup = Stage.FinalCleanup;
  if (const Value *GlobalDce = Options.get("global_dce")) {
    if (std::optional<bool> AsBool = GlobalDce->getAsBoolean()) {
      Cleanup.GlobalDce = *AsBool
                              ? NativeFinalCleanupOptions::GlobalDceMode::Enabled
                              : NativeFinalCleanupOptions::GlobalDceMode::Disabled;
    } else if (std::optional<StringRef> AsString = GlobalDce->getAsString()) {
      if (*AsString != "auto") {
        return fail(Error, joinPath(Path, "global_dce"),
                    "expected \"auto\", true or false");
      }
      Cleanup.GlobalDce = NativeFinalCleanupOptions::GlobalDceMode::Auto;
    } else {
      return fail(Error, joinPath(Path, "global_dce"),
                  "expected \"auto\", true or false");
    }
  }
  return readBool(Options, "preserve_address_taken",
                  Cleanup.PreserveAddressTaken, Path, Error);
}

bool parseNoOptions(const Object &Options, StringRef Path,
                    NativeStageConfig &Stage, std::string &Error) {
  (void)Stage;
  return checkKeys(Options, llvm::ArrayRef<StringRef>(), Path, Error);
}

const NativeStageDescriptor StageDescriptors[] = {
    {"instcombine", NativeStageKind::InstCombine, /*Repeatable=*/true,
     /*Filterable=*/true, &parseInstCombineOptions},
    {"register-elimination", NativeStageKind::RegisterElimination,
     /*Repeatable=*/false, /*Filterable=*/false,
     &parseRegisterEliminationOptions},
    {"function-pointer-promotion", NativeStageKind::FunctionPointerPromotion,
     /*Repeatable=*/false, /*Filterable=*/false, &parseNoOptions},
    {"post-rewrite-peephole", NativeStageKind::PostRewritePeephole,
     /*Repeatable=*/false, /*Filterable=*/false, &parseNoOptions},
    {"ir-override", NativeStageKind::IROverride, /*Repeatable=*/true,
     /*Filterable=*/false, &parseIROverrideOptions},
    {"final-cleanup", NativeStageKind::FinalCleanup, /*Repeatable=*/false,
     /*Filterable=*/false, &parseFinalCleanupOptions},
    {"erase-dead-helpers", NativeStageKind::EraseDeadHelpers,
     /*Repeatable=*/true, /*Filterable=*/false, &parseNoOptions},
};

} // namespace

llvm::ArrayRef<NativeStageDescriptor> nativeStageDescriptors() {
  return StageDescriptors;
}

const NativeStageDescriptor *findNativeStageDescriptor(StringRef Name) {
  for (const NativeStageDescriptor &Descriptor : StageDescriptors) {
    if (Descriptor.Name == Name) {
      return &Descriptor;
    }
  }
  return nullptr;
}

const NativeStageDescriptor *
findNativeStageDescriptor(NativeStageKind Kind) {
  for (const NativeStageDescriptor &Descriptor : StageDescriptors) {
    if (Descriptor.Kind == Kind) {
      return &Descriptor;
    }
  }
  return nullptr;
}

NativeConfig defaultNativeConfig(NativePipelineInput Input) {
  NativeConfig Config;
  auto append = [&Config](StringRef Name) {
    const NativeStageDescriptor *Descriptor = findNativeStageDescriptor(Name);
    if (Descriptor == nullptr) {
      return;
    }
    NativeStageConfig Stage;
    Stage.Kind = Descriptor->Kind;
    Stage.Name = Descriptor->Name.str();
    Config.Pipeline.push_back(std::move(Stage));
  };

  append("instcombine");
  append("register-elimination");
  append("instcombine");
  if (Input == NativePipelineInput::Binary) {
    append("function-pointer-promotion");
  }
  append("post-rewrite-peephole");
  append("final-cleanup");
  append("instcombine");
  append("erase-dead-helpers");

  // 默认序列里只有第一次 InstCombine 不开 EarlyCSE：后续几次都在寄存器
  // 重写之后，需要先折叠重复的 lane 链。
  bool FirstInstCombine = true;
  for (NativeStageConfig &Stage : Config.Pipeline) {
    if (Stage.Kind != NativeStageKind::InstCombine) {
      continue;
    }
    if (FirstInstCombine) {
      FirstInstCombine = false;
      continue;
    }
    Stage.InstCombine.EarlyCSE = true;
  }
  return Config;
}

std::optional<NativeConfig> parseNativeConfig(StringRef Text,
                                              StringRef ConfigPath,
                                              NativePipelineInput Input,
                                              std::string &Error) {
  (void)Input;
  llvm::Expected<Value> Parsed = llvm::json::parse(Text);
  if (!Parsed) {
    Error = llvm::toString(Parsed.takeError());
    return std::nullopt;
  }
  const Object *Root = Parsed->getAsObject();
  if (Root == nullptr) {
    Error = "root: expected object";
    return std::nullopt;
  }
  if (!checkKeys(*Root, {"schema_version", "root", "pipeline", "functions"},
                 "root", Error)) {
    return std::nullopt;
  }

  std::string AbsoluteConfigPath = normalizePath(fs::absolute(fs::path(ConfigPath.str())));
  fs::path ConfigDir = fs::path(AbsoluteConfigPath).parent_path();
  if (ConfigDir.empty()) {
    ConfigDir = ".";
  }

  NativeConfig Config;
  Config.SourcePath = AbsoluteConfigPath;

  if (const Value *Version = Root->get("schema_version")) {
    std::optional<int64_t> ParsedVersion = Version->getAsInteger();
    if (!ParsedVersion) {
      Error = "schema_version: expected integer";
      return std::nullopt;
    }
    if (*ParsedVersion != 1) {
      Error = ("schema_version: unsupported version " +
               llvm::Twine(*ParsedVersion) + " (expected 1)")
                  .str();
      return std::nullopt;
    }
    Config.SchemaVersion = static_cast<unsigned>(*ParsedVersion);
  } else {
    Error = "schema_version: missing required key";
    return std::nullopt;
  }

  std::string RootText = ".";
  if (!readString(*Root, "root", RootText, "root", Error)) {
    return std::nullopt;
  }
  Config.RootPath =
      normalizePath(fs::absolute(ConfigDir / fs::path(RootText)));

  const Value *Pipeline = Root->get("pipeline");
  if (Pipeline == nullptr) {
    Error = "pipeline: missing required key";
    return std::nullopt;
  }
  const Array *Stages = Pipeline->getAsArray();
  if (Stages == nullptr) {
    Error = "pipeline: expected array";
    return std::nullopt;
  }

  std::vector<NativeStageKind> SeenKinds;
  for (size_t I = 0; I < Stages->size(); ++I) {
    std::string Path = "pipeline[" + std::to_string(I) + "]";
    const Object *StageObject = (*Stages)[I].getAsObject();
    if (StageObject == nullptr) {
      Error = Path + ": expected object";
      return std::nullopt;
    }
    if (!checkKeys(*StageObject, {"stage", "options"}, Path, Error)) {
      return std::nullopt;
    }
    std::string StageName;
    if (!readString(*StageObject, "stage", StageName, Path, Error)) {
      return std::nullopt;
    }
    if (StageName.empty()) {
      Error = Path + ".stage: missing required key";
      return std::nullopt;
    }
    const NativeStageDescriptor *Descriptor =
        findNativeStageDescriptor(StageName);
    if (Descriptor == nullptr) {
      Error = Path + ".stage: unknown stage '" + StageName + "'";
      return std::nullopt;
    }
    if (!Descriptor->Repeatable &&
        llvm::is_contained(SeenKinds, Descriptor->Kind)) {
      Error = Path + ".stage: stage '" + StageName +
              "' is not repeatable and already appears earlier";
      return std::nullopt;
    }
    SeenKinds.push_back(Descriptor->Kind);

    NativeStageConfig Stage;
    Stage.Kind = Descriptor->Kind;
    Stage.Name = Descriptor->Name.str();
    const Value *Options = StageObject->get("options");
    if (Options != nullptr) {
      const Object *OptionsObject = Options->getAsObject();
      if (OptionsObject == nullptr) {
        Error = Path + ".options: expected object";
        return std::nullopt;
      }
      std::string OptionsPath = Path + ".options";
      if (!Descriptor->ParseOptions(*OptionsObject, OptionsPath, Stage,
                                    Error)) {
        return std::nullopt;
      }
    }
    if (Stage.Kind == NativeStageKind::IROverride) {
      NativeIROverrideOptions &Override = Stage.IROverride;
      if (Override.File.empty()) {
        Error = Path + ".options.file: missing required key";
        return std::nullopt;
      }
      Override.ResolvedFile =
          resolveAgainstRoot(Config.RootPath, Override.File);
      if (!fs::exists(Override.ResolvedFile)) {
        Error = Path + ".options.file: file not found: " +
                Override.ResolvedFile;
        return std::nullopt;
      }
      if (!hashFile(Override.ResolvedFile, Override.Sha256, Error)) {
        return std::nullopt;
      }
    }
    if (Stage.Kind == NativeStageKind::RegisterElimination &&
        !Stage.RegisterElimination.ExternalPrototypes.empty()) {
      NativeRegisterEliminationConfig &Elimination =
          Stage.RegisterElimination;
      Elimination.ResolvedExternalPrototypes = resolveAgainstRoot(
          Config.RootPath, Elimination.ExternalPrototypes);
      if (!fs::exists(Elimination.ResolvedExternalPrototypes)) {
        Error = Path +
                ".options.external_prototypes: file not found: " +
                Elimination.ResolvedExternalPrototypes;
        return std::nullopt;
      }
    }
    Config.Pipeline.push_back(std::move(Stage));
  }

  if (const Value *Functions = Root->get("functions")) {
    const Array *Items = Functions->getAsArray();
    if (Items == nullptr) {
      Error = "functions: expected array";
      return std::nullopt;
    }
    for (size_t I = 0; I < Items->size(); ++I) {
      std::string Path = "functions[" + std::to_string(I) + "]";
      const Object *Item = (*Items)[I].getAsObject();
      if (Item == nullptr) {
        Error = Path + ": expected object";
        return std::nullopt;
      }
      if (!checkKeys(*Item, {"select", "pin", "attributes", "abi"}, Path,
                     Error)) {
        return std::nullopt;
      }
      NativeFunctionConfig Function;
      const Value *Select = Item->get("select");
      if (Select == nullptr) {
        Error = Path + ".select: missing required key";
        return std::nullopt;
      }
      if (!parseSelector(*Select, Path + ".select", Function.Select, Error)) {
        return std::nullopt;
      }
      if (!readBool(*Item, "pin", Function.Pin, Path, Error)) {
        return std::nullopt;
      }
      if (const Value *Attributes = Item->get("attributes")) {
        if (!parseAttributes(*Attributes, Path + ".attributes",
                             Function.Attributes, Error)) {
          return std::nullopt;
        }
      }
      if (const Value *Abi = Item->get("abi")) {
        std::optional<StringRef> Text = Abi->getAsString();
        if (!Text) {
          Error = Path + ".abi: expected string";
          return std::nullopt;
        }
        if (*Text == "external") {
          Function.AbiConvention = NativeFunctionConfig::Abi::External;
        } else if (*Text == "internal") {
          Function.AbiConvention = NativeFunctionConfig::Abi::Internal;
        } else {
          Error = Path + ".abi: expected \"external\" or \"internal\"";
          return std::nullopt;
        }
      }
      if (!Function.Pin && Function.Attributes.empty() &&
          Function.AbiConvention == NativeFunctionConfig::Abi::Unset) {
        Error = Path + ": needs at least one of pin/attributes/abi";
        return std::nullopt;
      }
      Config.Functions.push_back(std::move(Function));
    }
  }

  return Config;
}

std::optional<NativeConfig> loadNativeConfig(StringRef Path,
                                             NativePipelineInput Input,
                                             std::string &Error) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(Path);
  if (!Buffer) {
    Error = "failed to read config " + Path.str() + ": " +
            Buffer.getError().message();
    return std::nullopt;
  }
  return parseNativeConfig((*Buffer)->getBuffer(), Path, Input, Error);
}

namespace {

void addSelectorToJson(Object &Out, const NativeFunctionSelector &Selector) {
  if (Selector.Name) {
    Out["name"] = *Selector.Name;
  }
  if (Selector.Glob) {
    Out["glob"] = *Selector.Glob;
  }
  if (Selector.Symbol) {
    Out["symbol"] = *Selector.Symbol;
  }
  if (Selector.Address) {
    Out["address"] = *Selector.Address;
  }
  if (Selector.Optional) {
    Out["optional"] = true;
  }
}

void addSelectorsToJson(Object &Out, StringRef Key,
                        const std::vector<NativeFunctionSelector> &Selectors) {
  Array Items;
  for (const NativeFunctionSelector &Selector : Selectors) {
    Object Item;
    addSelectorToJson(Item, Selector);
    Items.push_back(std::move(Item));
  }
  Out[Key] = std::move(Items);
}

Object selectorArrayToJson(
    const std::vector<NativeFunctionSelector> &Selectors) {
  Array Items;
  for (const NativeFunctionSelector &Selector : Selectors) {
    Object Item;
    addSelectorToJson(Item, Selector);
    Items.push_back(std::move(Item));
  }
  return Object{{"select", std::move(Items)}};
}

} // namespace

Object nativeStageOptionsToJson(const NativeStageConfig &Stage) {
  Object Options;
  switch (Stage.Kind) {
  case NativeStageKind::InstCombine:
    Options["early_cse"] = Stage.InstCombine.EarlyCSE;
    addSelectorsToJson(Options, "exclude", Stage.InstCombine.Exclude);
    break;
  case NativeStageKind::RegisterElimination: {
    const NativeRegisterEliminationConfig &Elimination =
        Stage.RegisterElimination;
    Options["rewrite"] = Elimination.Rewrite;
    Options["residue_removal"] = Elimination.ResidueRemoval;
    Options["post_rewrite_instcombine"] =
        Elimination.PostRewriteInstCombine;
    Options["vararg_recovery"] = Elimination.VarArgRecovery;
    Options["abi_for_all_functions"] = Elimination.AbiForAllFunctions;
    Options["attach_metadata"] = Elimination.AttachMetadata;
    Options["external_prototypes"] = Elimination.ExternalPrototypes;
    Array Registers;
    for (const std::string &Register : Elimination.IgnoredRegisters) {
      Registers.push_back(Register);
    }
    Options["ignored_registers"] = std::move(Registers);
    break;
  }
  case NativeStageKind::FunctionPointerPromotion:
  case NativeStageKind::PostRewritePeephole:
    break;
  case NativeStageKind::IROverride: {
    const NativeIROverrideOptions &Override = Stage.IROverride;
    Options["file"] = Override.File;
    addSelectorsToJson(Options, "select", Override.Select);
    Options["entry"] = Override.Entry;
    Options["allow_undefined"] = Override.AllowUndefined;
    Options["strip_debug"] = Override.StripDebug;
    Options["attributes_from"] =
        Override.AttributesFromOverride ? "override" : "target";
    break;
  }
  case NativeStageKind::FinalCleanup: {
    const NativeFinalCleanupOptions &Cleanup = Stage.FinalCleanup;
    switch (Cleanup.GlobalDce) {
    case NativeFinalCleanupOptions::GlobalDceMode::Auto:
      Options["global_dce"] = "auto";
      break;
    case NativeFinalCleanupOptions::GlobalDceMode::Enabled:
      Options["global_dce"] = true;
      break;
    case NativeFinalCleanupOptions::GlobalDceMode::Disabled:
      Options["global_dce"] = false;
      break;
    }
    Options["preserve_address_taken"] = Cleanup.PreserveAddressTaken;
    break;
  }
  case NativeStageKind::EraseDeadHelpers:
    break;
  }
  return Options;
}

void printNativeConfig(const NativeConfig &Config, llvm::raw_ostream &OS) {
  llvm::json::OStream J(OS, /*IndentSize=*/2);
  J.object([&] {
    J.attribute("schema_version", static_cast<int64_t>(Config.SchemaVersion));
    // 打印解析后的绝对 root：这样 --print-config 的产物存到任何目录都能复现
    // 同一次运行。手写配置通常不写 root，默认就是配置文件所在目录。
    if (!Config.RootPath.empty() && !Config.SourcePath.empty()) {
      J.attribute("root", Config.RootPath);
    }
    J.attributeBegin("pipeline");
    J.array([&] {
      for (const NativeStageConfig &Stage : Config.Pipeline) {
        J.object([&] {
          J.attribute("stage", Stage.Name);
          Object Options = nativeStageOptionsToJson(Stage);
          if (!Options.empty()) {
            J.attribute("options", Value(std::move(Options)));
          }
        });
      }
    });
    J.attributeEnd();
    if (!Config.Functions.empty()) {
      J.attributeBegin("functions");
      J.array([&] {
        for (const NativeFunctionConfig &Function : Config.Functions) {
          J.object([&] {
            Object Select;
            addSelectorToJson(Select, Function.Select);
            J.attribute("select", Value(std::move(Select)));
            if (Function.Pin) {
              J.attribute("pin", true);
            }
            if (Function.AbiConvention != NativeFunctionConfig::Abi::Unset) {
              J.attribute("abi",
                          Function.AbiConvention ==
                                  NativeFunctionConfig::Abi::External
                              ? "external"
                              : "internal");
            }
            const NativeFunctionAttributes &Attributes = Function.Attributes;
            if (!Attributes.empty()) {
              Object Attrs;
              if (Attributes.NoReturn) {
                Attrs["noreturn"] = *Attributes.NoReturn;
              }
              if (Attributes.NoUnwind) {
                Attrs["nounwind"] = *Attributes.NoUnwind;
              }
              if (Attributes.WillReturn) {
                Attrs["willreturn"] = *Attributes.WillReturn;
              }
              if (Attributes.NoInline) {
                Attrs["noinline"] = *Attributes.NoInline;
              }
              if (Attributes.AlwaysInline) {
                Attrs["alwaysinline"] = *Attributes.AlwaysInline;
              }
              if (Attributes.Cold) {
                Attrs["cold"] = *Attributes.Cold;
              }
              if (Attributes.Memory) {
                Attrs["memory"] = *Attributes.Memory;
              }
              J.attribute("attributes", Value(std::move(Attrs)));
            }
          });
        }
      });
      J.attributeEnd();
    }
  });
  OS << "\n";
}

void printNativeStageOptions(const NativeStageConfig &Stage,
                             llvm::raw_ostream &OS) {
  Object Options = nativeStageOptionsToJson(Stage);
  OS << Value(std::move(Options));
}


// --- 选择器匹配 -------------------------------------------------------------

namespace {

bool globMatch(StringRef Pattern, StringRef Text) {
  size_t PatternIndex = 0;
  size_t TextIndex = 0;
  size_t StarIndex = StringRef::npos;
  size_t StarTextIndex = 0;
  while (TextIndex < Text.size()) {
    if (PatternIndex < Pattern.size() &&
        (Pattern[PatternIndex] == '?' ||
         Pattern[PatternIndex] == Text[TextIndex])) {
      ++PatternIndex;
      ++TextIndex;
    } else if (PatternIndex < Pattern.size() &&
               Pattern[PatternIndex] == '*') {
      StarIndex = PatternIndex++;
      StarTextIndex = TextIndex;
    } else if (StarIndex != StringRef::npos) {
      PatternIndex = StarIndex + 1;
      TextIndex = ++StarTextIndex;
    } else {
      return false;
    }
  }
  while (PatternIndex < Pattern.size() && Pattern[PatternIndex] == '*') {
    ++PatternIndex;
  }
  return PatternIndex == Pattern.size();
}

const NativeFunctionIdentity *
lookupIdentity(llvm::ArrayRef<NativeFunctionIdentity> Identities,
               StringRef Name) {
  for (const NativeFunctionIdentity &Identity : Identities) {
    if (Identity.Name == Name) {
      return &Identity;
    }
  }
  return nullptr;
}

} // namespace

bool nativeSelectorMatches(const NativeFunctionSelector &Selector,
                           StringRef FunctionName,
                           const NativeFunctionIdentity *Identity) {
  if (Selector.Name && *Selector.Name != FunctionName) {
    return false;
  }
  if (Selector.Glob && !globMatch(*Selector.Glob, FunctionName)) {
    return false;
  }
  if (Selector.Symbol &&
      (Identity == nullptr || Identity->Symbol != *Selector.Symbol)) {
    return false;
  }
  if (Selector.Address &&
      (Identity == nullptr || Identity->Address != *Selector.Address)) {
    return false;
  }
  return true;
}

bool resolveNativeFunctionSelectors(
    llvm::Module &Module, llvm::ArrayRef<NativeFunctionIdentity> Identities,
    llvm::ArrayRef<NativeFunctionSelector> Selectors, StringRef Path,
    bool IncludeDeclarations, std::set<std::string> &Out, std::string &Error) {
  for (size_t I = 0; I < Selectors.size(); ++I) {
    const NativeFunctionSelector &Selector = Selectors[I];
    bool Matched = false;
    for (llvm::Function &Function : Module) {
      if (Function.isDeclaration() && !IncludeDeclarations) {
        continue;
      }
      const NativeFunctionIdentity *Identity =
          lookupIdentity(Identities, Function.getName());
      if (!nativeSelectorMatches(Selector, Function.getName(), Identity)) {
        continue;
      }
      Matched = true;
      Out.insert(Function.getName().str());
    }
    if (!Matched) {
      if (Selector.Optional) {
        llvm::errs() << "warning: " << Path << "[" << I
                     << "]: select matched no function\n";
        continue;
      }
      Error =
          (Path + "[" + llvm::Twine(I) + "]: select matched no function").str();
      return false;
    }
  }
  return true;
}


std::string nativeConfigSha256(const NativeConfig &Config) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  printNativeConfig(Config, Stream);
  Stream.flush();
  llvm::SHA256 Hash;
  Hash.update(Text);
  return llvm::toHex(Hash.final(), /*LowerCase=*/true);
}

llvm::json::Object nativeConfigManifest(const NativeConfig &Config) {
  llvm::json::Object Manifest;
  Manifest["schema_version"] = static_cast<int64_t>(Config.SchemaVersion);
  Manifest["sha256"] = nativeConfigSha256(Config);
  if (!Config.SourcePath.empty()) {
    Manifest["config"] = Config.SourcePath;
  }
  llvm::json::Array Files;
  for (const NativeStageConfig &Stage : Config.Pipeline) {
    if (Stage.Kind == NativeStageKind::RegisterElimination &&
        !Stage.RegisterElimination.ResolvedExternalPrototypes.empty()) {
      llvm::json::Object File;
      File["role"] = "external_prototypes";
      File["path"] = Stage.RegisterElimination.ResolvedExternalPrototypes;
      File["sha256"] = Stage.RegisterElimination.ExternalPrototypesSha256;
      Files.push_back(std::move(File));
    }
    if (Stage.Kind == NativeStageKind::IROverride) {
      llvm::json::Object File;
      File["role"] = "ir_override";
      File["path"] = Stage.IROverride.ResolvedFile;
      File["sha256"] = Stage.IROverride.Sha256;
      Files.push_back(std::move(File));
    }
  }
  Manifest["files"] = std::move(Files);
  return Manifest;
}

void attachNativeConfigMetadata(llvm::Module &Module,
                                const NativeConfig &Config) {
  if (Config.SourcePath.empty()) {
    return;
  }
  llvm::LLVMContext &Context = Module.getContext();
  llvm::NamedMDNode *Node =
      Module.getOrInsertNamedMetadata("notdec.config");
  llvm::Metadata *Header[] = {
      llvm::MDString::get(Context, "schema_version"),
      llvm::MDString::get(
          Context, llvm::Twine(Config.SchemaVersion).str()),
      llvm::MDString::get(Context, "sha256"),
      llvm::MDString::get(Context, nativeConfigSha256(Config)),
      llvm::MDString::get(Context, "source"),
      llvm::MDString::get(Context, Config.SourcePath),
  };
  Node->addOperand(llvm::MDNode::get(Context, Header));
  for (const NativeStageConfig &Stage : Config.Pipeline) {
    if (Stage.Kind == NativeStageKind::RegisterElimination &&
        !Stage.RegisterElimination.ResolvedExternalPrototypes.empty()) {
      llvm::Metadata *Fields[] = {
          llvm::MDString::get(Context, "file"),
          llvm::MDString::get(Context, "external_prototypes"),
          llvm::MDString::get(
              Context, Stage.RegisterElimination.ResolvedExternalPrototypes),
          llvm::MDString::get(
              Context, Stage.RegisterElimination.ExternalPrototypesSha256),
      };
      Node->addOperand(llvm::MDNode::get(Context, Fields));
    }
    if (Stage.Kind == NativeStageKind::IROverride) {
      llvm::Metadata *Fields[] = {
          llvm::MDString::get(Context, "file"),
          llvm::MDString::get(Context, "ir_override"),
          llvm::MDString::get(Context, Stage.IROverride.ResolvedFile),
          llvm::MDString::get(Context, Stage.IROverride.Sha256),
      };
      Node->addOperand(llvm::MDNode::get(Context, Fields));
    }
  }
}

} // namespace notdec::bin2llvm
