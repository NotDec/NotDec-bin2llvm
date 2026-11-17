add_library(notdec-bin2llvm-core STATIC
  abi/NativeAbi.cpp
  abi/NativeExternalPrototype.cpp
  core/NativePipeline.cpp
  core/NotDecIntrinsics.cpp
  core/Pcode.cpp
  core/PcodeToLLVM.cpp
  core/PcodeToLLVMSupport.cpp
  core/PcodeLowererCore.cpp
  core/PcodeLowererX87.cpp
  core/PcodeLowererCfg.cpp
  core/PcodeLowererOps.cpp
  core/RegisterStorage.cpp
  elf/NativeRelocationMetadata.cpp
  intrinsics/NativeRegisterPartialRead.cpp
  intrinsics/NativeRegisterPartialWrite.cpp
  intrinsics/NativeRegisterValueRange.cpp
  intrinsics/NativeX87Intrinsic.cpp
  passes/function-pointer/NativeFunctionPointerPromotion.cpp
  passes/register/FunctionBuilderCore.cpp
  passes/register/FunctionBuilderRanges.cpp
  passes/register/FunctionBuilderSignature.cpp
  passes/register/FunctionBuilderSsa.cpp
  passes/register/NativeRegisterElimination.cpp
  passes/register/NativeRegisterFinalCleanup.cpp
  passes/register/NativeRegisterPostRewritePeephole.cpp
  passes/register/NativeRegisterPreSummaryPeephole.cpp
  passes/register/NativeRegisterSummary.cpp
  passes/register/RegisterSummarySupport.cpp
  passes/register/RegisterSummaryCore.cpp
  passes/register/RegisterSummaryFlow.cpp
  passes/register/RegisterSummaryDemand.cpp
  passes/register/RegisterSummaryRewrite.cpp
  passes/register/RegisterSummarySignature.cpp
  passes/register/RegisterEliminationModel.cpp
  passes/register/RegisterExternalShapes.cpp
  passes/register/RegisterModel.cpp
  passes/register/RegisterSignatureRewrite.cpp
  passes/register/RegisterSignatureSlots.cpp
  passes/stack/NativeStackAddress.cpp
  passes/stack/NativeStackCanaryCleanup.cpp
  passes/stack/NativeStackFrame.cpp
  passes/vararg/NativeVarArgRewrite.cpp
)

target_include_directories(notdec-bin2llvm-core
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(notdec-bin2llvm-core
  PUBLIC
    notdec_bin2llvm_llvm_deps
)

if (NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  add_library(notdec-bin2llvm-sleigh STATIC
    core/SleighLift.cpp
  )

  target_include_directories(notdec-bin2llvm-sleigh
    PUBLIC
      "${PROJECT_SOURCE_DIR}/include"
  )

  target_link_libraries(notdec-bin2llvm-sleigh
    PUBLIC
      notdec-bin2llvm-core
      sleigh::sla
      sleigh::decomp
      sleigh::support
  )
endif ()

if (NOTDEC_BIN2LLVM_ENABLE_LIEF AND NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  add_library(notdec-bin2llvm-native STATIC
    discovery/NativeAnalysis.cpp
    discovery/NativeAnalysisSupport.cpp
    discovery/NativeX86Patterns.cpp
    discovery/NativeElfFacts.cpp
    discovery/NativeEhFrame.cpp
    discovery/NativeGtirbFacts.cpp
    discovery/NativeSleighSeeds.cpp
    discovery/NativeX86Facts.cpp
    discovery/NativeReport.cpp
    elf/LiefElfLoadImage.cpp
  )

  target_include_directories(notdec-bin2llvm-native
    PUBLIC
      "${PROJECT_SOURCE_DIR}/include"
  )

  target_link_libraries(notdec-bin2llvm-native
    PUBLIC
      notdec-bin2llvm-sleigh
      LIEF::LIEF
  )

  if (NOTDEC_BIN2LLVM_ENABLE_GTIRB AND gtirb_FOUND AND gtirb_pprinter_FOUND)
    target_link_libraries(notdec-bin2llvm-native
      PUBLIC
        gtirb
        gtirb_pprinter
    )
    target_compile_options(notdec-bin2llvm-native
      PRIVATE
        -frtti
    )
    target_compile_definitions(notdec-bin2llvm-native
      PRIVATE
        NOTDEC_BIN2LLVM_ENABLE_GTIRB=1
    )
  endif ()

  target_compile_definitions(notdec-bin2llvm-native
    PRIVATE
      NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR="${NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR}"
  )
endif ()
