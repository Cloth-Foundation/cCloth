#ifndef CLOTH_RUNTIME_RUNTIME_H_
#define CLOTH_RUNTIME_RUNTIME_H_

#include <cstdint>

extern "C" {

[[nodiscard]] void* cloth_rt_alloc(std::uint64_t size,
                                   std::uint64_t alignment) noexcept;
[[nodiscard]] void* cloth_rt_string_literal(const void* data,
                                            std::uint64_t size) noexcept;
void cloth_rt_require_receiver(const void* receiver) noexcept;
void cloth_rt_print(const void* string) noexcept;
void cloth_rt_print_i32(std::int32_t value) noexcept;
void cloth_rt_print_bool(std::uint8_t value) noexcept;

}  // extern "C"

#endif  // CLOTH_RUNTIME_RUNTIME_H_
