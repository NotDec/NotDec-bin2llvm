add_executable(notdec-bin2llvm
  notdec-bin2llvm.cpp
)

target_link_libraries(notdec-bin2llvm
  PRIVATE
    notdec-bin2llvm-core
)
