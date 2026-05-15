add_executable(notdec-bin2llvm
  notdec-bin2llvm.cpp
)

target_link_libraries(notdec-bin2llvm
  PRIVATE
    notdec-bin2llvm-core
)

add_executable(notdec-heritage-check
  notdec-heritage-check.cpp
)

target_link_libraries(notdec-heritage-check
  PRIVATE
    notdec-bin2llvm-core
)

add_executable(notdec-heritage-llvm
  notdec-heritage-llvm.cpp
)

target_link_libraries(notdec-heritage-llvm
  PRIVATE
    notdec-bin2llvm-core
)

add_executable(notdec-heritage-module-check
  notdec-heritage-module-check.cpp
)

target_link_libraries(notdec-heritage-module-check
  PRIVATE
    notdec-bin2llvm-core
)

add_executable(notdec-heritage-module-llvm
  notdec-heritage-module-llvm.cpp
)

target_link_libraries(notdec-heritage-module-llvm
  PRIVATE
    notdec-bin2llvm-core
)

if (NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  add_library(notdec-bin2llvm-sleigh-bytes STATIC
    SleighBytes.cpp
  )

  target_include_directories(notdec-bin2llvm-sleigh-bytes
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_link_libraries(notdec-bin2llvm-sleigh-bytes
    PUBLIC
      notdec-bin2llvm-core
      sleigh::sla
      sleigh::decomp
      sleigh::support
  )

  add_executable(notdec-sleigh-pcode
    notdec-sleigh-pcode.cpp
  )

  target_link_libraries(notdec-sleigh-pcode
    PRIVATE
      notdec-bin2llvm-sleigh-bytes
  )

  add_executable(notdec-sleigh-llvm
    notdec-sleigh-llvm.cpp
  )

  target_link_libraries(notdec-sleigh-llvm
    PRIVATE
      notdec-bin2llvm-sleigh-bytes
  )
endif ()
