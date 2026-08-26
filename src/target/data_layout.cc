#include "cloth/target/data_layout.h"

#include <cstdint>
#include <limits>

namespace cloth {

TargetDataLayout TargetDataLayout::llvm_x86_64() {
  return TargetDataLayout{"x86_64-unknown-unknown",
                          "e-p:64:64-i64:64-f64:64-n8:16:32:64-S128",
                          Endianness::kLittle,
                          SizeAlignment{8, 8},
                          8,
                          8,
                          2};
}

TargetDataLayout TargetDataLayout::llvm_wasm32() {
  return TargetDataLayout{"wasm32-unknown-unknown",
                          "e-m:e-p:32:32-i64:64-f64:64-n32:64-S128",
                          Endianness::kLittle,
                          SizeAlignment{4, 4},
                          8,
                          8,
                          2};
}

bool is_power_of_two(std::uint64_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

bool is_valid_data_layout(const TargetDataLayout& layout) noexcept {
  const bool endianness_is_valid = layout.endianness == Endianness::kLittle ||
                                   layout.endianness == Endianness::kBig;
  return !layout.target_name.empty() && !layout.llvm_data_layout.empty() &&
         endianness_is_valid && layout.pointer.size != 0 &&
         is_power_of_two(layout.pointer.alignment) &&
         is_power_of_two(layout.int64_alignment) &&
         is_power_of_two(layout.float64_alignment) &&
         layout.object_header_words == 2 &&
         layout.object_header_words <=
             std::numeric_limits<std::uint64_t>::max() / layout.pointer.size &&
         layout.pointer.size <= std::numeric_limits<std::uint32_t>::max() / 8;
}

std::uint64_t align_to(std::uint64_t value, std::uint64_t alignment) noexcept {
  if (!is_power_of_two(alignment)) {
    return value;
  }
  const std::uint64_t mask = alignment - 1;
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return (value + mask) & ~mask;
}

}  // namespace cloth
