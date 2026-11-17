#include "notdec-bin2llvm/core/NativePipelineSpec.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

using namespace notdec::bin2llvm;

bool expect(bool Condition, const std::string &Message) {
  if (!Condition) {
    std::cerr << "FAIL: " << Message << '\n';
  }
  return Condition;
}

std::string makeTempDir() {
  std::filesystem::path Dir = std::filesystem::temp_directory_path() /
                               ("notdec-config-test-" +
                                std::to_string(static_cast<long>(::getpid())));
  std::filesystem::create_directories(Dir);
  return Dir.string();
}

void writeFile(const std::string &Path, const std::string &Text) {
  std::ofstream Output(Path);
  Output << Text;
}

std::optional<NativeConfig> parseText(const std::string &Text,
                                      const std::string &ConfigPath,
                                      std::string &Error) {
  return parseNativeConfig(Text, ConfigPath, NativePipelineInput::Binary,
                           Error);
}

std::string printConfig(const NativeConfig &Config) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  printNativeConfig(Config, Stream);
  Stream.flush();
  return Text;
}

bool contains(const std::string &Text, const std::string &Needle) {
  return Text.find(Needle) != std::string::npos;
}

bool testDefaultPipeline() {
  bool Ok = true;
  NativeConfig Binary = defaultNativeConfig(NativePipelineInput::Binary);
  Ok &= expect(Binary.Pipeline.size() == 8, "binary default pipeline size");
  Ok &= expect(Binary.Pipeline[0].Kind == NativeStageKind::InstCombine,
               "binary stage 0 is instcombine");
  Ok &= expect(Binary.Pipeline[1].Kind ==
                   NativeStageKind::RegisterElimination,
               "binary stage 1 is register-elimination");
  Ok &= expect(Binary.Pipeline[2].Kind == NativeStageKind::InstCombine,
               "binary stage 2 is instcombine");
  Ok &= expect(Binary.Pipeline[3].Kind ==
                   NativeStageKind::FunctionPointerPromotion,
               "binary stage 3 is function-pointer-promotion");
  Ok &= expect(Binary.Pipeline[7].Kind == NativeStageKind::EraseDeadHelpers,
               "binary last stage is erase-dead-helpers");
  Ok &= expect(!Binary.Pipeline[0].InstCombine.EarlyCSE,
               "first instcombine keeps EarlyCSE off");
  Ok &= expect(Binary.Pipeline[2].InstCombine.EarlyCSE,
               "second instcombine enables EarlyCSE");
  Ok &= expect(Binary.Pipeline[6].InstCombine.EarlyCSE,
               "last instcombine enables EarlyCSE");

  NativeConfig IR = defaultNativeConfig(NativePipelineInput::IR);
  Ok &= expect(IR.Pipeline.size() == 7, "ir default pipeline size");
  bool HasPromotion = false;
  for (const NativeStageConfig &Stage : IR.Pipeline) {
    HasPromotion |= Stage.Kind == NativeStageKind::FunctionPointerPromotion;
  }
  Ok &= expect(!HasPromotion, "ir default pipeline has no promotion stage");
  return Ok;
}

bool testRoundTrip(const std::string &Dir) {
  bool Ok = true;
  std::string Path = Dir + "/roundtrip.json";
  std::string Text = R"({
    "schema_version": 1,
    "pipeline": [
      {"stage": "instcombine", "options": {"early_cse": true}},
      {"stage": "register-elimination", "options": {
        "residue_removal": false, "ignored_registers": ["RSP"]}}
    ],
    "functions": [
      {"select": {"name": "foo"}, "pin": true,
       "attributes": {"noreturn": true, "memory": "read"}}
    ]
  })";
  std::string Error;
  auto First = parseText(Text, Path, Error);
  if (!expect(First.has_value(), "roundtrip config parses: " + Error)) {
    return false;
  }
  std::string Printed = printConfig(*First);
  auto Second = parseText(Printed, Path, Error);
  if (!expect(Second.has_value(), "printed config re-parses: " + Error)) {
    return false;
  }
  Ok &= expect(printConfig(*Second) == Printed,
               "print/parse/print is stable");
  Ok &= expect(Second->Functions.size() == 1, "functions survive roundtrip");
  Ok &= expect(Second->Functions[0].Pin, "pin survives roundtrip");
  Ok &= expect(Second->Functions[0].Attributes.Memory &&
                   *Second->Functions[0].Attributes.Memory == "read",
               "memory attribute survives roundtrip");
  return Ok;
}

bool testErrors(const std::string &Dir) {
  bool Ok = true;
  std::string Path = Dir + "/bad.json";
  std::string Error;
  auto expectError = [&](const std::string &Text,
                         const std::string &Needle,
                         const std::string &Label) {
    Error.clear();
    auto Parsed = parseText(Text, Path, Error);
    return expect(!Parsed.has_value() && contains(Error, Needle),
                  Label + " (got: " + Error + ")");
  };

  Ok &= expectError(R"({"pipeline": []})", "schema_version",
                    "missing schema_version");
  Ok &= expectError(R"({"schema_version": 2, "pipeline": []})",
                    "unsupported version", "unsupported schema_version");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [{"stage": "nope"}]})",
      "unknown stage", "unknown stage");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [{"stage": "instcombine", "options": {"early_cse": 1}}]})",
      "early_cse", "type error is attributed to the option");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [{"stage": "instcombine", "options": {"early_cse_typo": true}}]})",
      "unknown key", "unknown option key");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [{"stage": "final-cleanup"}, {"stage": "final-cleanup"}]})",
      "not repeatable", "duplicate non-repeatable stage");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [{"stage": "ir-override", "options": {"file": "missing.ll"}}]})",
      "file not found", "missing override file");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [], "functions": [{"select": {}}]})",
      "at least one of", "empty selector");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [], "functions": [{"select": {"name": "f"}}]})",
      "needs at least one", "function entry without pin/attributes");
  Ok &= expectError(
      R"({"schema_version": 1, "pipeline": [{"stage": "final-cleanup", "options": {"global_dce": "yes"}}]})",
      "global_dce", "bad global_dce value");
  Ok &= expectError(R"({"schema_version": 1, "pipeline": [], "bogus": 1})",
                    "unknown key", "unknown top-level key");
  return Ok;
}

bool testPaths(const std::string &Dir) {
  bool Ok = true;
  writeFile(Dir + "/protos.json", "{\"prototypes\": []}\n");
  std::string Path = Dir + "/paths.json";
  std::string Text = R"({
    "schema_version": 1,
    "pipeline": [
      {"stage": "register-elimination", "options": {
        "external_prototypes": "protos.json"}}
    ]
  })";
  std::string Error;
  auto Config = parseText(Text, Path, Error);
  if (!expect(Config.has_value(), "path config parses: " + Error)) {
    return false;
  }
  const NativeRegisterEliminationConfig &Elimination =
      Config->Pipeline[0].RegisterElimination;
  Ok &= expect(Elimination.ExternalPrototypes == "protos.json",
               "raw path is preserved for printing");
  Ok &= expect(Elimination.ResolvedExternalPrototypes ==
                   (std::filesystem::path(Dir) / "protos.json").string(),
               "external prototype path resolves next to the config");
  return Ok;
}

bool testSelectorRequiresAddressFormat(const std::string &Dir) {
  std::string Path = Dir + "/selector.json";
  std::string Error;
  auto Parsed = parseText(
      R"({"schema_version": 1, "pipeline": [],
          "functions": [{"select": {"address": "zzz"}, "pin": true}]})",
      Path, Error);
  return expect(!Parsed.has_value() && contains(Error, "address"),
                "bad address string is rejected");
}

} // namespace

int main() {
  bool Ok = true;
  std::string Dir = makeTempDir();
  Ok &= testDefaultPipeline();
  Ok &= testRoundTrip(Dir);
  Ok &= testErrors(Dir);
  Ok &= testPaths(Dir);
  Ok &= testSelectorRequiresAddressFormat(Dir);
  std::filesystem::remove_all(Dir);
  if (!Ok) {
    return EXIT_FAILURE;
  }
  llvm::outs() << "native pipeline config tests passed\n";
  return EXIT_SUCCESS;
}
