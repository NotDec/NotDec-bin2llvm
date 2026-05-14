add_library(notdec-bin2llvm-core STATIC
  HeritagePcode.cpp
  HeritageToLLVM.cpp
  ModuleBuilder.cpp
  Pcode.cpp
  PcodeToLLVM.cpp
)

target_include_directories(notdec-bin2llvm-core
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(notdec-bin2llvm-core
  PUBLIC
    notdec_bin2llvm_llvm_deps
)
