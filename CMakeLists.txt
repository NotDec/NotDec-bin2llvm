cmake_minimum_required(VERSION 3.14)
project(notdec-bin2llvm
  LANGUAGES CXX
  VERSION 0.0.1
)

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
