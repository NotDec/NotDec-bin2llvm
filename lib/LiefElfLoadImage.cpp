#include "notdec-bin2llvm/LiefElfLoadImage.h"

#include <LIEF/ELF/Binary.hpp>
#include <LIEF/ELF/Segment.hpp>

#include <algorithm>
#include <iostream>
#include <utility>

namespace notdec::bin2llvm {

LiefElfLoadImage::LiefElfLoadImage(const LIEF::ELF::Binary &binary,
                                   std::ostream &errorStream)
    : ghidra::LoadImage("notdec-lief-elf") {
  for (const LIEF::ELF::Segment &segment : binary.segments()) {
    if (segment.type() != LIEF::ELF::Segment::TYPE::LOAD ||
        !segment.has(LIEF::ELF::Segment::FLAGS::X)) {
      continue;
    }

    SegmentBytes bytes;
    bytes.VirtualAddress = segment.virtual_address();
    bytes.VirtualSize = segment.virtual_size();
    auto content = segment.content();
    bytes.Bytes.assign(content.begin(), content.end());
    Segments.push_back(std::move(bytes));
  }

  std::sort(Segments.begin(), Segments.end(),
            [](const SegmentBytes &lhs, const SegmentBytes &rhs) {
              return lhs.VirtualAddress < rhs.VirtualAddress;
            });

  if (Segments.empty()) {
    errorStream << "ELF has no executable PT_LOAD segment\n";
  }
}

void LiefElfLoadImage::loadFill(unsigned char *ptr, int size,
                                const ghidra::Address &addr) {
  uint64_t start = addr.getOffset();
  for (int i = 0; i < size; ++i) {
    uint64_t byteAddress = start + static_cast<uint64_t>(i);
    ptr[i] = 0;
    for (const SegmentBytes &segment : Segments) {
      if (byteAddress < segment.VirtualAddress) {
        break;
      }
      uint64_t offset = byteAddress - segment.VirtualAddress;
      if (offset >= segment.VirtualSize) {
        continue;
      }
      if (offset < segment.Bytes.size()) {
        ptr[i] = segment.Bytes[offset];
      }
      break;
    }
  }
}

std::string LiefElfLoadImage::getArchType(void) const { return "elf"; }

void LiefElfLoadImage::adjustVma(long) {}

} // namespace notdec::bin2llvm
