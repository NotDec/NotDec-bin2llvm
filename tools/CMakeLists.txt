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
      notdec-bin2llvm-sleigh
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

if (NOTDEC_BIN2LLVM_ENABLE_LIEF)
  add_executable(notdec-lief-elf
    notdec-lief-elf.cpp
  )

  target_link_libraries(notdec-lief-elf
    PRIVATE
      LIEF::LIEF
  )
endif ()

if (NOTDEC_BIN2LLVM_ENABLE_LIEF AND NOTDEC_BIN2LLVM_ENABLE_SLEIGH)
  add_executable(notdec-native-discover
    notdec-native-discover.cpp
  )

  target_link_libraries(notdec-native-discover
    PRIVATE
      notdec-bin2llvm-native
  )

  if (BUILD_TESTING AND EXISTS "/bin/ls")
    add_test(
      NAME notdec.native_discover.x86_64_smoke
      COMMAND
        /bin/bash -c
        "$<TARGET_FILE:notdec-native-discover> --decode-seed-limit 20 /bin/ls > ${PROJECT_BINARY_DIR}/notdec-native-discover-smoke.txt"
    )
  endif ()

  add_executable(notdec-native-pcode
    notdec-native-pcode.cpp
  )

  target_link_libraries(notdec-native-pcode
    PRIVATE
      notdec-bin2llvm-native
      notdec-bin2llvm-sleigh-bytes
  )

  add_executable(notdec-native-llvm
    notdec-native-llvm.cpp
  )

  target_link_libraries(notdec-native-llvm
    PRIVATE
      notdec-bin2llvm-native
      notdec-bin2llvm-sleigh-bytes
  )

  target_compile_definitions(notdec-native-llvm
    PRIVATE
      NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR="${NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR}"
  )

  if (BUILD_TESTING AND EXISTS "/bin/ls")
    add_test(
      NAME notdec.native_llvm.x86_64_smoke
      COMMAND
        /bin/bash -c
        "$<TARGET_FILE:notdec-native-llvm> /bin/ls -a 0x6aa0 -l 1024 -o ${PROJECT_BINARY_DIR}/notdec-native-llvm-smoke.ll && ${NOTDEC_LLVM_INSTALL_DIR}/bin/llvm-as ${PROJECT_BINARY_DIR}/notdec-native-llvm-smoke.ll -o ${PROJECT_BINARY_DIR}/notdec-native-llvm-smoke.bc && ${NOTDEC_LLVM_INSTALL_DIR}/bin/opt -passes=verify ${PROJECT_BINARY_DIR}/notdec-native-llvm-smoke.bc -o ${PROJECT_BINARY_DIR}/notdec-native-llvm-smoke.opt.bc"
    )
  endif ()

  if (BUILD_TESTING)
    add_test(
      NAME notdec.native_llvm.cli_signature_rewrite_ll_smoke
      COMMAND
        /bin/bash -c
        "$<TARGET_FILE:notdec-native-llvm> ${PROJECT_SOURCE_DIR}/tests/ir/native-prototype/cli-signature-rewrite.ll --no-instcombine-pass --no-register-ssa-pass --prototype-recovery-summary --rewrite-prototype-signatures -o ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll 2> ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite-summary.txt && grep -Fq 'define void @cli_input_rdi(i64 %' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && grep -Fq 'define i64 @cli_return_rax()' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && grep -Fq 'define i64 @cli_input_rdi_return_rax(i64 %' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && grep -Fq 'define i64 @cli_input_rdi_rsi_return_rax(i64 %' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && grep -Fq ', i64 %' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && ! grep -Fq 'notdec.register.external_inputs' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && ! grep -Fq 'notdec.prototype.input_candidates' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && ! grep -Fq 'notdec.prototype.return_candidates' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll && grep -Fq 'signature rewrite seen functions: 4' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite-summary.txt && grep -Fq 'signature rewrite rewritten functions: 4' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite-summary.txt && grep -Fq 'signature rewrite skipped functions: 0' ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite-summary.txt && ${NOTDEC_LLVM_INSTALL_DIR}/bin/llvm-as ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.ll -o ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.bc && ${NOTDEC_LLVM_INSTALL_DIR}/bin/opt -passes=verify ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.bc -o ${PROJECT_BINARY_DIR}/notdec-native-llvm-cli-signature-rewrite.opt.bc"
    )
  endif ()
endif ()
