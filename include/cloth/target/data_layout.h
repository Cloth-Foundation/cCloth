#ifndef CLOTH_TARGET_DATA_LAYOUT_H_
#define CLOTH_TARGET_DATA_LAYOUT_H_

#include <cstdint>
#include <string>

namespace cloth {

enum class Endianness {
  kLittle,
  kBig,
};

struct SizeAlignment {
  std::uint64_t size;
  std::uint64_t alignment;

  friend bool operator==(const SizeAlignment&, const SizeAlignment&) = default;
};

// Describes the target properties needed before backend-specific lowering.
// LLVM target data can populate this contract without leaking LLVM types into
// the front end.
struct TargetDataLayout {
  std::string target_name;
  std::string llvm_data_layout;
  Endianness endianness;
  SizeAlignment pointer;
  std::uint64_t int64_alignment;
  std::uint64_t float64_alignment;
  std::uint64_t object_header_words;

  friend bool operator==(const TargetDataLayout&,
                         const TargetDataLayout&) = default;

  [[nodiscard]] static TargetDataLayout llvm_x86_64();
  [[nodiscard]] static TargetDataLayout llvm_wasm32();
};

[[nodiscard]] bool is_power_of_two(std::uint64_t value) noexcept;
[[nodiscard]] bool is_valid_data_layout(
    const TargetDataLayout& layout) noexcept;
[[nodiscard]] std::uint64_t align_to(std::uint64_t value,
                                     std::uint64_t alignment) noexcept;

}  // namespace cloth

#endif  // CLOTH_TARGET_DATA_LAYOUT_H_
