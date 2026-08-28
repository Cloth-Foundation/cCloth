#ifndef CLOTH_RUNTIME_RUNTIME_H_
#define CLOTH_RUNTIME_RUNTIME_H_

#include <cstdint>

extern "C" {

[[nodiscard]] void* cloth_rt_alloc(std::uint64_t size, std::uint64_t alignment,
                                   const void* type_name,
                                   std::uint64_t type_name_size) noexcept;
[[nodiscard]] void* cloth_rt_string_literal(const void* data,
                                            std::uint64_t size) noexcept;
[[nodiscard]] void* cloth_rt_array_alloc(
    std::int32_t length, std::uint64_t element_size,
    std::uint64_t element_alignment, std::uint8_t contains_references) noexcept;
[[nodiscard]] std::int32_t cloth_rt_array_length(const void* array) noexcept;
[[nodiscard]] void* cloth_rt_array_element(void* array,
                                           std::int32_t index) noexcept;
void cloth_rt_require_receiver(const void* receiver) noexcept;
void cloth_rt_require_non_null(const void* value) noexcept;
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
