add_library(notdec-bin2llvm-core STATIC
  HeritagePcode.cpp
  HeritageToLLVM.cpp
  ModuleBuilder.cpp
  NativeAbi.cpp
  NativeExternalPrototype.cpp
  NativePrototypeModel.cpp
  NativeRegisterPartialRead.cpp
  NativeRegisterPartialWrite.cpp
  NativeRegisterValueRange.cpp
  Pcode.cpp
  PcodeToLLVM.cpp
  passes/heritage/NativePrototypeRecovery.cpp
  passes/summary/NativeStackFrame.cpp
  passes/summary/NativeStackCanaryCleanup.cpp
  passes/summary/NativeRegisterSummary.cpp
  passes/summary/NativeRegisterPeephole.cpp
  passes/summary/NativeRegisterSummarySSA.cpp
  passes/summary/NativeRegisterFinalCleanup.cpp
  passes/heritage/NativeHeritageSSA.cpp
  RegisterStorage.cpp
)

target_include_directories(notdec-bin2llvm-core
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(notdec-bin2llvm-core
  PUBLIC
    notdec_bin2llvm_llvm_deps
)

if (NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  add_library(notdec-bin2llvm-sleigh STATIC
    SleighLift.cpp
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
    LiefElfLoadImage.cpp
    NativeAnalysis.cpp
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
