cmake_minimum_required(VERSION 3.13.4)
project(notdec-bin2llvm
  LANGUAGES CXX
  VERSION 0.0.1
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(FindLLVM)

option(NOTDEC_BIN2LLVM_ENABLE_SLEIGH "Enable sleigh package lookup for future lifting work." OFF)

if (NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  find_package(sleigh REQUIRED CONFIG)
endif ()

include_directories("${PROJECT_SOURCE_DIR}/include")
include_directories("${PROJECT_SOURCE_DIR}/include/notdec-bin2llvm")

add_subdirectory(lib)
add_subdirectory(tools)
