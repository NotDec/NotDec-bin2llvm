#pragma once

#include <cstdint>
#include <vector>

namespace llvm {
class Instruction;
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

// 寄存器感知 DCE 的统计：观察位为 0 的寄存器写，其写入值没人读，可以把值换成
// 常量并递归删掉现在没人用的计算链（寄存器读、算术、unknown 占位）。
struct NativeRegisterValueDceSummary {
  uint64_t DeadWrites = 0;
  uint64_t ValuesCleared = 0;
  uint64_t InstructionsRemoved = 0;
  uint64_t RegisterReadsRemoved = 0;
};

// deadWrites 是需求分析得到的"观察位为 0"的寄存器写（store @REG 或
// notdec.partial_write.*）。写本身保留（它仍把寄存器定义成 non-entry，影响
// 签名事实），只把写入值换成 0，然后删掉因此没人用的纯计算链。
NativeRegisterValueDceSummary
pruneDeadRegisterValues(llvm::Module &module,
                        const std::vector<llvm::Instruction *> &deadWrites);

void printNativeRegisterValueDceSummary(
    const NativeRegisterValueDceSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
