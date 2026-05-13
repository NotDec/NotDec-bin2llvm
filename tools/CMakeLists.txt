add_executable(notdec-bin2llvm
  notdec-bin2llvm.cpp
)

target_link_libraries(notdec-bin2llvm
  PRIVATE
    notdec-bin2llvm-core
)

if (NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  add_executable(notdec-sleigh-pcode
    notdec-sleigh-pcode.cpp
  )

  target_link_libraries(notdec-sleigh-pcode
    PRIVATE
      sleigh::sla
      sleigh::decomp
      sleigh::support
  )
endif ()
