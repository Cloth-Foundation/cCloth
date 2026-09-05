// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_RUNTIME_RUNTIME_H_
#define CLOTH_RUNTIME_RUNTIME_H_

#include <cstdint>

enum class ClothHeapObjectKind : std::uint64_t {
  kFileClass = 0,
  kString = 1,
  kArray = 2,
  kError = 3,
};

struct ClothInterfaceDispatch {
  std::uint64_t interface_id;
  const void* const* functions;
  std::uint64_t function_count;
};

// Immutable compiler-emitted metadata. All offsets are object-relative bytes.
struct ClothTypeDescriptor {
  ClothHeapObjectKind kind;
  const ClothTypeDescriptor* parent;
  const char* name;
  std::uint64_t name_size;
  std::uint64_t size;
  std::uint64_t alignment;
  const std::uint64_t* reference_offsets;
  std::uint64_t reference_count;
  const void* const* virtual_functions;
  std::uint64_t virtual_function_count;
  const ClothInterfaceDispatch* interfaces;
  std::uint64_t interface_count;
};

// One frame in the per-thread precise-root stack. Roots point to pointer-sized
// stack slots containing managed references.
struct ClothGcRootFrame {
  ClothGcRootFrame* previous;
  void*** roots;
  std::uint64_t root_count;
};

// Immutable compiler-emitted, program-lifetime metadata for one array element.
// References identify complete pointer slots within the padded element stride.
struct ClothArrayElementLayout {
  std::uint64_t size;
  std::uint64_t alignment;
  const std::uint64_t* reference_offsets;
  std::uint64_t reference_count;
};

inline constexpr std::uint8_t kClothIntegerArithmeticOverflow = 0;
inline constexpr std::uint8_t kClothIntegerDivisionByZero = 1;
inline constexpr std::uint8_t kClothIntegerRemainderByZero = 2;

extern "C" {

extern const ClothTypeDescriptor cloth_rt_error_type;
extern const ClothTypeDescriptor cloth_rt_division_by_zero_type;

[[nodiscard]] void* cloth_rt_alloc(const ClothTypeDescriptor* type) noexcept;
void cloth_rt_gc_push_frame(ClothGcRootFrame* frame, void*** roots,
                            std::uint64_t root_count) noexcept;
void cloth_rt_gc_pop_frame(ClothGcRootFrame* frame) noexcept;
void cloth_rt_gc_collect() noexcept;
[[nodiscard]] std::uint64_t cloth_rt_gc_live_objects() noexcept;
[[nodiscard]] std::uint64_t cloth_rt_gc_live_bytes() noexcept;
[[nodiscard]] std::uint64_t cloth_rt_gc_collection_count() noexcept;
[[nodiscard]] std::uint64_t cloth_rt_gc_peak_live_bytes() noexcept;
[[nodiscard]] void* cloth_rt_string_literal(const void* data,
                                            std::uint64_t size) noexcept;
[[nodiscard]] void* cloth_rt_string_concat(const void* left,
                                           const void* right) noexcept;
[[nodiscard]] std::uint8_t cloth_rt_string_equal(const void* left,
                                                 const void* right) noexcept;
[[nodiscard]] std::int32_t cloth_rt_string_length(const void* value) noexcept;
[[nodiscard]] std::int32_t cloth_rt_string_byte_length(
    const void* value) noexcept;
[[nodiscard]] std::uint8_t cloth_rt_string_is_empty(const void* value) noexcept;
[[nodiscard]] void* cloth_rt_object_type_name(const void* value) noexcept;
[[nodiscard]] std::uint8_t cloth_rt_object_is_kind(const void* value,
                                                   std::uint64_t kind) noexcept;
[[nodiscard]] std::uint8_t cloth_rt_object_is_type(
    const void* value, const ClothTypeDescriptor* type) noexcept;
[[nodiscard]] std::uint8_t cloth_rt_object_is_interface(
    const void* value, std::uint64_t interface_id) noexcept;
[[nodiscard]] const void* cloth_rt_interface_function(
    const void* value, std::uint64_t interface_id,
    std::uint64_t function_slot) noexcept;
[[nodiscard]] void* cloth_rt_array_alloc(
    std::int32_t length, const ClothArrayElementLayout* element) noexcept;
[[nodiscard]] std::int32_t cloth_rt_array_length(const void* array) noexcept;
[[nodiscard]] void* cloth_rt_array_element(void* array,
                                           std::int32_t index) noexcept;
void cloth_rt_integer_write(void* destination, std::int32_t offset,
                            std::uint64_t bits, std::uint8_t byte_width,
                            std::uint8_t byte_order) noexcept;
[[nodiscard]] std::uint64_t cloth_rt_integer_read(
    const void* source, std::int32_t offset, std::uint8_t byte_width,
    std::uint8_t byte_order) noexcept;
void cloth_rt_require_receiver(const void* receiver) noexcept;
void cloth_rt_require_non_null(const void* value) noexcept;
void cloth_rt_require_numeric_conversion(std::uint8_t valid) noexcept;
void cloth_rt_require_shift_count(std::uint8_t valid) noexcept;
void cloth_rt_require_integer_arithmetic(std::uint8_t valid,
                                         std::uint8_t reason) noexcept;
[[nodiscard]] void* cloth_rt_make_division_by_zero() noexcept;
[[nodiscard]] std::int32_t cloth_rt_report_error(const void* error) noexcept;
void cloth_rt_print(const void* string) noexcept;
void cloth_rt_print_char(std::uint32_t value) noexcept;
void cloth_rt_print_i8(std::int8_t value) noexcept;
void cloth_rt_print_i16(std::int16_t value) noexcept;
void cloth_rt_print_i32(std::int32_t value) noexcept;
void cloth_rt_print_i64(std::int64_t value) noexcept;
void cloth_rt_print_u8(std::uint8_t value) noexcept;
void cloth_rt_print_u16(std::uint16_t value) noexcept;
void cloth_rt_print_u32(std::uint32_t value) noexcept;
void cloth_rt_print_u64(std::uint64_t value) noexcept;
void cloth_rt_print_f32(float value) noexcept;
void cloth_rt_print_f64(double value) noexcept;
void cloth_rt_print_bool(std::uint8_t value) noexcept;
void cloth_rt_print_object(const void* value) noexcept;
void cloth_rt_print_newline() noexcept;

}  // extern "C"

#endif  // CLOTH_RUNTIME_RUNTIME_H_
