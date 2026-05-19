#pragma once

#include <sleigh/libsleigh.hh>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace LIEF::ELF {
class Binary;
} // namespace LIEF::ELF

namespace notdec::bin2llvm {

// LiefElfLoadImage adapts ELF loadable executable segments to libsla's byte
// reader.  It deliberately keeps only a flat VA-to-byte view here: function
// discovery, relocation handling, and CFG recovery stay outside this layer.
class LiefElfLoadImage : public ghidra::LoadImage {
public:
  LiefElfLoadImage(const LIEF::ELF::Binary &binary, std::ostream &errorStream);

  void loadFill(unsigned char *ptr, int size,
                const ghidra::Address &addr) override;
  std::string getArchType(void) const override;
  void adjustVma(long) override;

  bool hasExecutableBytes() const { return !Segments.empty(); }

private:
  struct SegmentBytes {
    uint64_t VirtualAddress = 0;
    uint64_t VirtualSize = 0;
    std::vector<uint8_t> Bytes;
  };

  std::vector<SegmentBytes> Segments;
};

} // namespace notdec::bin2llvm
