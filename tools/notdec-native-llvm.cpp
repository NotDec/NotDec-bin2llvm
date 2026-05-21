#include "SleighBytes.h"
#include "notdec-bin2llvm/LiefElfLoadImage.h"
#include "notdec-bin2llvm/NativeAnalysis.h"
#include "notdec-bin2llvm/PcodeToLLVM.h"
#include "notdec-bin2llvm/SleighLift.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Parser.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

#ifndef NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR
#define NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR "/sn640/ghidra"
#endif

struct CliOptions {
  std::string ElfPath;
  notdec::bin2llvm::SleighSpecOptions SpecOptions;
  uint64_t Address = 0;
  uint64_t Length = 0;
  std::optional<uint64_t> FunctionEntry;
  std::string FunctionName;
  bool AllConfirmed = false;
  std::string OutputPath;
};

void printUsage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " <elf-file> [sla-file] "
               "(-a <address> -l <length> | -f <entry> | -n <name> | "
               "--all-confirmed) "
               "-o <output.ll> [-p root-sla-dir] [-s pspec-file]\n";
}

bool parseUint64(const std::string &text, uint64_t &value) {
  try {
    size_t parsed = 0;
    value = std::stoull(text, &parsed, 0);
    return parsed == text.size();
  } catch (const std::exception &) {
    return false;
  }
}

std::optional<CliOptions> parseArgs(int argc, char **argv) {
  if (argc < 5) {
    return std::nullopt;
  }

  CliOptions options;
  options.ElfPath = argv[1];
  bool hasAddress = false;
  bool hasLength = false;
  int argIndex = 2;
  if (argIndex < argc && std::string(argv[argIndex]).rfind("-", 0) != 0) {
    options.SpecOptions.SlaFileName = argv[argIndex++];
  }

  for (; argIndex < argc; ++argIndex) {
    std::string flag = argv[argIndex];
    if (flag == "--all-confirmed") {
      options.AllConfirmed = true;
      continue;
    }
    if (argIndex + 1 >= argc) {
      std::cerr << "flag has no value: " << flag << '\n';
      return std::nullopt;
    }

    std::string value = argv[++argIndex];
    if (flag == "-a") {
      if (!parseUint64(value, options.Address)) {
        std::cerr << "invalid address: " << value << '\n';
        return std::nullopt;
      }
      hasAddress = true;
    } else if (flag == "-f") {
      uint64_t entry = 0;
      if (!parseUint64(value, entry)) {
        std::cerr << "invalid function entry: " << value << '\n';
        return std::nullopt;
      }
      options.FunctionEntry = entry;
    } else if (flag == "-n") {
      options.FunctionName = std::move(value);
    } else if (flag == "-l") {
      if (!parseUint64(value, options.Length) || options.Length == 0) {
        std::cerr << "invalid length: " << value << '\n';
        return std::nullopt;
      }
      hasLength = true;
    } else if (flag == "-o") {
      options.OutputPath = std::move(value);
    } else if (flag == "-p") {
      options.SpecOptions.RootSlaDir = std::move(value);
    } else if (flag == "-s") {
      options.SpecOptions.PspecFileName = std::move(value);
    } else {
      std::cerr << "unknown flag: " << flag << '\n';
      return std::nullopt;
    }
  }

  unsigned selectionCount = 0;
  if (options.FunctionEntry) {
    ++selectionCount;
  }
  if (!options.FunctionName.empty()) {
    ++selectionCount;
  }
  if (options.AllConfirmed) {
    ++selectionCount;
  }
  if (hasAddress || hasLength) {
    ++selectionCount;
  }
  if (selectionCount != 1) {
    std::cerr << "choose exactly one of -a/-l, -f, -n, or --all-confirmed\n";
    return std::nullopt;
  }
  if ((hasAddress || hasLength) && !hasAddress) {
    std::cerr << "missing -a <address>\n";
    return std::nullopt;
  }
  if ((hasAddress || hasLength) && !hasLength) {
    std::cerr << "missing -l <length>\n";
    return std::nullopt;
  }
  if (options.OutputPath.empty()) {
    std::cerr << "missing -o <output.ll>\n";
    return std::nullopt;
  }
  return options;
}

std::filesystem::path defaultX86SpecRoot() {
  return std::filesystem::path(NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR) /
         "Ghidra/Processors/x86/data/languages";
}

bool resolveSpecOptions(const LIEF::ELF::Binary &binary,
                        notdec::bin2llvm::SleighSpecOptions &options) {
  if (!options.SlaFileName.empty()) {
    return true;
  }

  if (binary.header().machine_type() != LIEF::ELF::ARCH::X86_64) {
    std::cerr << "automatic sleigh spec selection only supports x86-64 ELF\n";
    return false;
  }

  std::filesystem::path specRoot = defaultX86SpecRoot();
  std::filesystem::path slaPath = specRoot / "x86-64.sla";
  std::filesystem::path pspecPath = specRoot / "x86-64.pspec";
  if (!std::filesystem::exists(slaPath)) {
    std::cerr << "could not find auto-selected sla file: "
              << slaPath.string() << '\n';
    return false;
  }

  options.SlaFileName = slaPath.string();
  if (!options.PspecFileName && std::filesystem::exists(pspecPath)) {
    options.PspecFileName = pspecPath.string();
  }
  return true;
}

std::string entryFunctionName(uint64_t entry) {
  std::ostringstream stream;
  stream << "notdec_native_" << std::hex << entry;
  return stream.str();
}

std::string sanitizeLlvmFunctionName(const std::string &name) {
  std::string result;
  result.reserve(name.size());
  for (char ch : name) {
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
    result.push_back(ok ? ch : '_');
  }
  if (result.empty()) {
    return "";
  }
  if (result.front() >= '0' && result.front() <= '9') {
    result.insert(result.begin(), '_');
  }
  return result;
}

notdec::bin2llvm::NativeProgramState
runNativeDiscovery(const LIEF::ELF::Binary &binary) {
  notdec::bin2llvm::NativeProgramState state(binary);
  notdec::bin2llvm::NativeAnalysisManager manager;
  manager.addAnalyzer(notdec::bin2llvm::createElfLoadAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createRelocationPltAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createElfEntryAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createElfSymbolAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createEhFrameAnalyzer());
  manager.addAnalyzer(notdec::bin2llvm::createSleighSeedInstructionAnalyzer());
  manager.run(state);
  return state;
}

bool resolveFunctionRange(const LIEF::ELF::Binary &binary, CliOptions &options) {
  if (!options.FunctionEntry && options.FunctionName.empty()) {
    return true;
  }

  notdec::bin2llvm::NativeProgramState state = runNativeDiscovery(binary);

  const notdec::bin2llvm::NativeFunction *function = nullptr;
  if (options.FunctionEntry) {
    function = state.functionAt(*options.FunctionEntry);
  } else {
    for (const auto &[entry, candidate] : state.functions()) {
      (void)entry;
      if (candidate.Name != options.FunctionName) {
        continue;
      }
      if (function != nullptr) {
        std::cerr << "native discovery found duplicate function name: "
                  << options.FunctionName << '\n';
        return false;
      }
      function = &candidate;
    }
  }
  if (function == nullptr) {
    if (options.FunctionEntry) {
      std::cerr << "native discovery did not confirm function at 0x" << std::hex
                << *options.FunctionEntry << std::dec << '\n';
    } else {
      std::cerr << "native discovery did not confirm function named "
                << options.FunctionName << '\n';
    }
    return false;
  }
  if (function->RangeEnd <= function->Entry) {
    std::cerr << "native function has empty range at 0x" << std::hex
              << function->Entry << std::dec << '\n';
    return false;
  }

  options.Address = function->Entry;
  options.Length = function->RangeEnd - function->Entry;
  if (!options.FunctionEntry) {
    options.FunctionEntry = function->Entry;
  }
  return true;
}

std::string uniqueFunctionName(const std::string &baseName,
                               std::set<std::string> &usedNames) {
  std::string name = baseName.empty() ? "notdec_native_function" : baseName;
  std::string unique = name;
  unsigned index = 1;
  while (!usedNames.insert(unique).second) {
    unique = name + "_" + std::to_string(index++);
  }
  return unique;
}

std::string nativeFunctionLlvmName(const notdec::bin2llvm::NativeFunction &func,
                                   std::set<std::string> &usedNames) {
  if (!func.Name.empty()) {
    return uniqueFunctionName(sanitizeLlvmFunctionName(func.Name), usedNames);
  }
  return uniqueFunctionName(entryFunctionName(func.Entry), usedNames);
}

std::string externalFunctionLlvmName(const std::string &symbolName,
                                     std::set<std::string> &usedNames) {
  std::string name = sanitizeLlvmFunctionName(symbolName);
  if (name.empty()) {
    name = "notdec_external_function";
  }
  return uniqueFunctionName(name, usedNames);
}

bool moduleVerifies(const llvm::Module &module, std::string &message) {
  std::string buffer;
  llvm::raw_string_ostream stream(buffer);
  bool failed = llvm::verifyModule(module, &stream);
  stream.flush();
  if (failed) {
    message = buffer;
  }
  return !failed;
}

std::unique_ptr<llvm::Module> buildConfirmedModule(
    llvm::LLVMContext &context, const LIEF::ELF::Binary &binary,
    notdec::bin2llvm::LiefElfLoadImage &loadImage,
    const notdec::bin2llvm::SleighSpecOptions &specOptions,
    std::string &errorMessage) {
  notdec::bin2llvm::NativeProgramState state = runNativeDiscovery(binary);
  auto module =
      std::make_unique<llvm::Module>("notdec.bin2llvm.native.confirmed", context);

  std::set<std::string> usedNames;
  std::unordered_map<uint64_t, std::string> namesByEntry;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    if (function.RangeEnd <= function.Entry) {
      continue;
    }
    namesByEntry.emplace(function.Entry,
                         nativeFunctionLlvmName(function, usedNames));
  }

  std::unordered_map<uint64_t, std::string> externalNamesByStub;
  for (const notdec::bin2llvm::NativePltEntry &entry : state.pltEntries()) {
    if (entry.SymbolName.empty()) {
      continue;
    }
    externalNamesByStub.emplace(
        entry.StubAddress,
        externalFunctionLlvmName(entry.SymbolName, usedNames));
  }

  unsigned appended = 0;
  for (const auto &[entry, function] : state.functions()) {
    (void)entry;
    if (function.RangeEnd <= function.Entry) {
      continue;
    }

    uint64_t length = function.RangeEnd - function.Entry;
    auto program = notdec::bin2llvm::collectSleighPcode(
        loadImage, specOptions, function.Entry, length, std::cerr);
    if (program.Ops.empty()) {
      std::cerr << "skip native function 0x" << std::hex << function.Entry
                << std::dec << ": empty p-code\n";
      continue;
    }

    notdec::bin2llvm::PcodeLoweringConfig config;
    config.ModuleName = "notdec.bin2llvm.native.confirmed.check";
    auto nameIt = namesByEntry.find(function.Entry);
    if (nameIt == namesByEntry.end()) {
      continue;
    }
    config.EntryFunctionName = nameIt->second;
    config.DirectCallTargets = namesByEntry;
    config.ExternalCallTargets = externalNamesByStub;

    llvm::LLVMContext checkContext;
    std::string checkError;
    auto checkModule = notdec::bin2llvm::buildPcodeModule(
        checkContext, program, config, checkError);
    if (!checkModule) {
      std::cerr << "skip native function 0x" << std::hex << function.Entry
                << std::dec << ": " << checkError << '\n';
      continue;
    }
    std::string verifyMessage;
    if (!moduleVerifies(*checkModule, verifyMessage)) {
      std::cerr << "skip native function 0x" << std::hex << function.Entry
                << std::dec << ": " << verifyMessage;
      continue;
    }

    if (!notdec::bin2llvm::appendPcodeFunction(context, *module, program,
                                               config, errorMessage)) {
      return nullptr;
    }
    ++appended;
  }

  if (appended == 0) {
    errorMessage = "no confirmed native functions lowered successfully";
    return nullptr;
  }
  return module;
}

int writeModule(const llvm::Module &module, const std::string &outputPath) {
  std::error_code errorCode;
  llvm::raw_fd_ostream output(outputPath, errorCode);
  if (errorCode) {
    std::cerr << "failed to open output file: " << outputPath << ": "
              << errorCode.message() << '\n';
    return 1;
  }

  module.print(output, nullptr);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    auto options = parseArgs(argc, argv);
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
    if (!resolveSpecOptions(*binary, options->SpecOptions)) {
      return 1;
    }
    if (!resolveFunctionRange(*binary, *options)) {
      return 1;
    }

    notdec::bin2llvm::LiefElfLoadImage loadImage(*binary, std::cerr);
    if (!loadImage.hasExecutableBytes()) {
      return 1;
    }

    llvm::LLVMContext context;
    std::string errorMessage;
    std::unique_ptr<llvm::Module> module;
    if (options->AllConfirmed) {
      module = buildConfirmedModule(context, *binary, loadImage,
                                    options->SpecOptions, errorMessage);
    } else {
      auto program = notdec::bin2llvm::collectSleighPcode(
          loadImage, options->SpecOptions, options->Address, options->Length,
          std::cerr);
      if (program.Ops.empty()) {
        return 1;
      }

      notdec::bin2llvm::PcodeLoweringConfig config;
      if (!options->FunctionName.empty()) {
        config.EntryFunctionName =
            sanitizeLlvmFunctionName(options->FunctionName);
      } else if (options->FunctionEntry) {
        config.EntryFunctionName = entryFunctionName(*options->FunctionEntry);
      }
      module = notdec::bin2llvm::buildPcodeModule(context, program, config,
                                                  errorMessage);
    }
    if (!module) {
      std::cerr << "failed to lower p-code: " << errorMessage << '\n';
      return 1;
    }

    if (llvm::verifyModule(*module, &llvm::errs())) {
      std::cerr << "module verification failed\n";
      return 1;
    }

    return writeModule(*module, options->OutputPath);
  } catch (const ghidra::LowlevelError &error) {
    std::cerr << "libsla error: " << error.explain << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
