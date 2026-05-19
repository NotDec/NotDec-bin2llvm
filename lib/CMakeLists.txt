add_library(notdec-bin2llvm-core STATIC
  HeritagePcode.cpp
  HeritageToLLVM.cpp
  ModuleBuilder.cpp
  Pcode.cpp
  PcodeToLLVM.cpp
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
endif ()
