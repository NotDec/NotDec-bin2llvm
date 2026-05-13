# Keep this helper local so the subproject can build on its own.
set(NOTDEC_LLVM_MAJOR_VERSION "22" CACHE STRING "Expected LLVM major version")

if (NOT NOTDEC_LLVM_INSTALL_DIR)
  set(
    NOTDEC_LLVM_INSTALL_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../llvm-22.1.0.obj"
    CACHE PATH
    "LLVM installation directory"
  )
endif ()

set(NOTDEC_LLVM_INCLUDE_DIR "${NOTDEC_LLVM_INSTALL_DIR}/include/llvm")
if (NOT EXISTS "${NOTDEC_LLVM_INCLUDE_DIR}")
  message(FATAL_ERROR "NOTDEC_LLVM_INSTALL_DIR (${NOTDEC_LLVM_INCLUDE_DIR}) is invalid.")
endif ()

set(NOTDEC_LLVM_VALID_INSTALLATION FALSE)
set(NOTDEC_LLVM_CMAKE_DIR "")

if (EXISTS "${NOTDEC_LLVM_INSTALL_DIR}/lib/cmake/llvm/LLVMConfig.cmake")
  set(NOTDEC_LLVM_VALID_INSTALLATION TRUE)
  set(NOTDEC_LLVM_CMAKE_DIR "${NOTDEC_LLVM_INSTALL_DIR}/lib/cmake/llvm")
endif ()

if (EXISTS "${NOTDEC_LLVM_INSTALL_DIR}/lib64/cmake/llvm/LLVMConfig.cmake")
  set(NOTDEC_LLVM_VALID_INSTALLATION TRUE)
  set(NOTDEC_LLVM_CMAKE_DIR "${NOTDEC_LLVM_INSTALL_DIR}/lib64/cmake/llvm")
endif ()

if (NOT NOTDEC_LLVM_VALID_INSTALLATION)
  message(
    FATAL_ERROR
    "LLVM installation directory (${NOTDEC_LLVM_INSTALL_DIR}) is invalid. Couldn't find LLVMConfig.cmake."
  )
endif ()

set(LLVM_DIR "${NOTDEC_LLVM_CMAKE_DIR}" CACHE PATH "LLVM CMake package directory" FORCE)
list(PREPEND CMAKE_PREFIX_PATH "${NOTDEC_LLVM_CMAKE_DIR}")

find_package(LLVM REQUIRED CONFIG)

if (NOT "${NOTDEC_LLVM_MAJOR_VERSION}" VERSION_EQUAL "${LLVM_VERSION_MAJOR}")
  message(FATAL_ERROR "Found LLVM ${LLVM_VERSION_MAJOR}, but need LLVM ${NOTDEC_LLVM_MAJOR_VERSION}")
endif ()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${NOTDEC_LLVM_INSTALL_DIR}")

unset(NOTDEC_LLVM_DYLIB CACHE)
find_library(
  NOTDEC_LLVM_DYLIB
  NAMES LLVM LLVM-${NOTDEC_LLVM_MAJOR_VERSION}
  HINTS ${LLVM_LIBRARY_DIRS} "${NOTDEC_LLVM_INSTALL_DIR}/lib" "${NOTDEC_LLVM_INSTALL_DIR}/lib64"
  NO_DEFAULT_PATH
)

add_library(notdec_bin2llvm_llvm_deps INTERFACE)

if (NOTDEC_LLVM_DYLIB)
  target_link_libraries(notdec_bin2llvm_llvm_deps INTERFACE "${NOTDEC_LLVM_DYLIB}")
  message(STATUS "notdec-bin2llvm will use shared LLVM: ${NOTDEC_LLVM_DYLIB}")
else ()
  message(FATAL_ERROR "Failed to locate shared LLVM library under ${NOTDEC_LLVM_INSTALL_DIR}")
endif ()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_STANDARD 17 CACHE STRING "")

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug)." FORCE)
endif ()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fdiagnostics-color=always")

if (NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif ()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fvisibility-inlines-hidden" SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
if (${SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG} EQUAL "1")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility-inlines-hidden")
endif ()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
