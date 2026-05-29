cmake_minimum_required(VERSION 3.14)
project(notdec-bin2llvm
  LANGUAGES CXX
  VERSION 0.0.1
)

include(CTest)

if (BUILD_TESTING)
  add_test(
    NAME notdec.bench2_native_discovery_debug_oracle_unit
    COMMAND
      python3
      "${PROJECT_SOURCE_DIR}/tests/bench2_native_discovery_debug_check_test.py"
  )
endif ()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(FindLLVM)

option(NOTDEC_BIN2LLVM_ENABLE_SLEIGH "Enable sleigh dependency for future lifting work." OFF)
set(
  NOTDEC_BIN2LLVM_SLEIGH_GIT_REPOSITORY
  "https://github.com/lifting-bits/sleigh.git"
  CACHE STRING
  "Git repository used when fetching sleigh."
)
set(
  NOTDEC_BIN2LLVM_SLEIGH_GIT_TAG
  "c1aec71e4090a57daea1544379c63537e5e1add7"
  CACHE STRING
  "Pinned sleigh commit used by FetchContent."
)
set(
  NOTDEC_BIN2LLVM_SLEIGH_SOURCE_DIR
  ""
  CACHE PATH
  "Optional local sleigh source override. When set, FetchContent uses this source tree."
)
set(
  NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR
  "/sn640/ghidra"
  CACHE PATH
  "Ghidra source checkout used by lifting-bits/sleigh. Keep it at the Ghidra tag pinned by sleigh."
)

option(NOTDEC_BIN2LLVM_ENABLE_LIEF "Enable LIEF dependency for native binary parsing work." OFF)
set(
  NOTDEC_BIN2LLVM_LIEF_GIT_REPOSITORY
  "https://github.com/lief-project/LIEF.git"
  CACHE STRING
  "Git repository used when fetching LIEF."
)
set(
  NOTDEC_BIN2LLVM_LIEF_GIT_TAG
  "0.17.6"
  CACHE STRING
  "Pinned LIEF release tag used by FetchContent."
)
set(
  NOTDEC_BIN2LLVM_LIEF_SOURCE_DIR
  ""
  CACHE PATH
  "Optional local LIEF source override. When set, FetchContent uses this source tree."
)

if (NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  include(FetchContent)

  # Keep the first integration narrow: libraries plus ToB support helpers.
  set(sleigh_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(sleigh_BUILD_SLEIGHSPECS OFF CACHE BOOL "" FORCE)
  set(sleigh_BUILD_SUPPORT ON CACHE BOOL "" FORCE)
  set(sleigh_BUILD_EXTRATOOLS OFF CACHE BOOL "" FORCE)

  if (NOTDEC_BIN2LLVM_SLEIGH_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_SLEIGH "${NOTDEC_BIN2LLVM_SLEIGH_SOURCE_DIR}")
  endif ()
  if (NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_GHIDRASOURCE
        "${NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR}")
  endif ()

  FetchContent_Declare(
    sleigh
    GIT_REPOSITORY "${NOTDEC_BIN2LLVM_SLEIGH_GIT_REPOSITORY}"
    GIT_TAG "${NOTDEC_BIN2LLVM_SLEIGH_GIT_TAG}"
  )
  FetchContent_MakeAvailable(sleigh)

  # FindLLVM.cmake sets -fno-rtti globally to match LLVM. Sleigh itself uses
  # dynamic_cast internally, so its own targets need RTTI turned back on.
  if (TARGET sleigh_sla)
    target_compile_options(sleigh_sla PRIVATE -frtti)
  endif ()
  if (TARGET sleigh_decomp)
    target_compile_options(sleigh_decomp PRIVATE -frtti)
  endif ()
  if (TARGET sleigh_support)
    target_compile_options(sleigh_support PRIVATE -frtti)
  endif ()
endif ()

if (NOTDEC_BIN2LLVM_ENABLE_LIEF)
  include(FetchContent)

  # Start with the upstream release layout and keep the integration small.
  set(LIEF_TESTS OFF CACHE BOOL "" FORCE)
  set(LIEF_PYTHON_API OFF CACHE BOOL "" FORCE)
  set(LIEF_EXAMPLES OFF CACHE BOOL "" FORCE)

  if (NOTDEC_BIN2LLVM_LIEF_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_LIEF "${NOTDEC_BIN2LLVM_LIEF_SOURCE_DIR}")
  endif ()

  FetchContent_Declare(
    lief
    GIT_REPOSITORY "${NOTDEC_BIN2LLVM_LIEF_GIT_REPOSITORY}"
    GIT_TAG "${NOTDEC_BIN2LLVM_LIEF_GIT_TAG}"
  )
  FetchContent_MakeAvailable(lief)
endif ()

include_directories("${PROJECT_SOURCE_DIR}/include")
include_directories("${PROJECT_SOURCE_DIR}/include/notdec-bin2llvm")

add_subdirectory(lib)
add_subdirectory(tools)

if (BUILD_TESTING)
  add_executable(native_abi_cspec_test
    tests/native_abi_cspec_test.cpp
  )

  target_link_libraries(native_abi_cspec_test
    PRIVATE
      notdec-bin2llvm-core
  )

  add_test(
    NAME notdec.native_abi.cspec
    COMMAND
      native_abi_cspec_test
      "${NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR}/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec"
  )

  add_executable(native_prototype_model_test
    tests/native_prototype_model_test.cpp
  )

  target_link_libraries(native_prototype_model_test
    PRIVATE
      notdec-bin2llvm-core
  )

  add_test(
    NAME notdec.native_prototype_model.register
    COMMAND
      native_prototype_model_test
      "${NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR}/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec"
  )

  add_executable(native_prototype_recovery_test
    tests/native_prototype_recovery_test.cpp
  )

  target_link_libraries(native_prototype_recovery_test
    PRIVATE
      notdec-bin2llvm-core
  )

  add_test(
    NAME notdec.native_prototype_recovery.input_candidates
    COMMAND
      native_prototype_recovery_test
  )

  add_executable(native_register_effects_test
    tests/native_register_effects_test.cpp
  )

  target_link_libraries(native_register_effects_test
    PRIVATE
      notdec-bin2llvm-core
  )

  add_test(
    NAME notdec.native_register_effects.preserved
    COMMAND
      native_register_effects_test
  )
endif ()
