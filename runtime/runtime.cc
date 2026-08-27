#include "cloth/runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

struct ClothString {
  const char* data;
  std::size_t size;
};

struct ClothArray {
  void* data;
  std::size_t length;
  std::size_t element_size;
  bool contains_references;
};

[[noreturn]] void runtime_failure(std::string_view message) noexcept {
  constexpr std::string_view kPrefix = "cloth runtime error: ";
  static_cast<void>(std::fwrite(kPrefix.data(), 1, kPrefix.size(), stderr));
  static_cast<void>(std::fwrite(message.data(), 1, message.size(), stderr));
  static_cast<void>(std::fputc('\n', stderr));
  std::abort();
}

bool is_power_of_two(std::uint64_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

std::size_t native_size(std::uint64_t value,
                        std::string_view description) noexcept {
  if (value > std::numeric_limits<std::size_t>::max()) {
    runtime_failure(description);
  }
  return static_cast<std::size_t>(value);
}

void write_stdout(std::string_view text) noexcept {
  if (!text.empty() &&
      std::fwrite(text.data(), 1, text.size(), stdout) != text.size()) {
    runtime_failure("standard output write failed");
  }
  if (std::fflush(stdout) != 0) {
    runtime_failure("standard output flush failed");
  }
}

}  // namespace

extern "C" void* cloth_rt_alloc(std::uint64_t size,
                                std::uint64_t alignment) noexcept {
  if (!is_power_of_two(alignment)) {
    runtime_failure("invalid object alignment");
  }
  const std::size_t allocation_size =
      std::max<std::size_t>(native_size(size, "object is too large"), 1);
  const std::size_t allocation_alignment = std::max<std::size_t>(
      native_size(alignment, "object alignment is too large"), alignof(void*));

  void* storage = nullptr;
#if defined(_WIN32)
  storage = _aligned_malloc(allocation_size, allocation_alignment);
#else
  if (posix_memalign(&storage, allocation_alignment, allocation_size) != 0) {
    storage = nullptr;
  }
#endif
  if (storage == nullptr) {
    runtime_failure("object allocation failed");
  }
  std::memset(storage, 0, allocation_size);
  return storage;
}

extern "C" void* cloth_rt_string_literal(const void* data,
                                         std::uint64_t size) noexcept {
  const std::size_t string_size =
      native_size(size, "string literal is too large");
  if (data == nullptr && string_size != 0) {
    runtime_failure("string literal has null storage");
  }
  auto* string = static_cast<ClothString*>(std::malloc(sizeof(ClothString)));
  if (string == nullptr) {
    runtime_failure("string allocation failed");
  }
  *string = ClothString{static_cast<const char*>(data), string_size};
  return string;
}

extern "C" void* cloth_rt_array_alloc(
    std::int32_t length, std::uint64_t element_size,
    std::uint64_t element_alignment,
    std::uint8_t contains_references) noexcept {
  if (length < 0) {
    runtime_failure("array length is negative");
  }
  if (element_size == 0) {
    runtime_failure("array element size is zero");
  }
  if (!is_power_of_two(element_alignment)) {
    runtime_failure("invalid array element alignment");
  }
  if (contains_references > 1) {
    runtime_failure("invalid array reference metadata");
  }

  const std::size_t native_length = static_cast<std::size_t>(length);
  const std::size_t native_element_size =
      native_size(element_size, "array element is too large");
  if (native_length >
      std::numeric_limits<std::size_t>::max() / native_element_size) {
    runtime_failure("array allocation is too large");
  }
  auto* array = static_cast<ClothArray*>(std::malloc(sizeof(ClothArray)));
  if (array == nullptr) {
    runtime_failure("array header allocation failed");
  }
  array->data = cloth_rt_alloc(
      static_cast<std::uint64_t>(native_length * native_element_size),
      element_alignment);
  array->length = native_length;
  array->element_size = native_element_size;
  array->contains_references = contains_references != 0;
  return array;
}

extern "C" std::int32_t cloth_rt_array_length(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("null array");
  }
  const auto& array = *static_cast<const ClothArray*>(value);
  return static_cast<std::int32_t>(array.length);
}

extern "C" void* cloth_rt_array_element(void* value,
                                        std::int32_t index) noexcept {
  if (value == nullptr) {
    runtime_failure("null array");
  }
  auto& array = *static_cast<ClothArray*>(value);
  if (index < 0 || static_cast<std::size_t>(index) >= array.length) {
    runtime_failure("array index is out of bounds");
  }
  return static_cast<void*>(static_cast<std::byte*>(array.data) +
                            static_cast<std::size_t>(index) *
                                array.element_size);
}

extern "C" void cloth_rt_require_receiver(const void* receiver) noexcept {
  if (receiver == nullptr) {
    runtime_failure("null receiver");
  }
}

extern "C" void cloth_rt_print(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("print received a null String");
  }
  const auto& string = *static_cast<const ClothString*>(value);
  write_stdout(std::string_view{string.data, string.size});
}

extern "C" void cloth_rt_print_i32(std::int32_t value) noexcept {
  std::array<char, 16> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    runtime_failure("integer formatting failed");
  }
  write_stdout(std::string_view{buffer.data(), result.ptr});
}

extern "C" void cloth_rt_print_bool(std::uint8_t value) noexcept {
  write_stdout(value == 0 ? std::string_view{"false"}
                          : std::string_view{"true"});
}
