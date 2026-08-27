#include "cloth/runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
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

struct ClothTypeDescriptor {
  const char* name;
  std::size_t name_size;
  ClothTypeDescriptor* next;
};

struct ClothObjectHeader {
  const ClothTypeDescriptor* type;
  void* runtime_state;
};

std::mutex descriptor_mutex;
ClothTypeDescriptor* descriptors = nullptr;

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

void* allocate_aligned(std::uint64_t size, std::uint64_t alignment,
                       std::string_view failure) noexcept {
  if (!is_power_of_two(alignment)) {
    runtime_failure("invalid allocation alignment");
  }
  const std::size_t allocation_size =
      std::max<std::size_t>(native_size(size, "allocation is too large"), 1);
  const std::size_t allocation_alignment = std::max<std::size_t>(
      native_size(alignment, "allocation alignment is too large"),
      alignof(void*));

  void* storage = nullptr;
#if defined(_WIN32)
  storage = _aligned_malloc(allocation_size, allocation_alignment);
#else
  if (posix_memalign(&storage, allocation_alignment, allocation_size) != 0) {
    storage = nullptr;
  }
#endif
  if (storage == nullptr) {
    runtime_failure(failure);
  }
  std::memset(storage, 0, allocation_size);
  return storage;
}

const ClothTypeDescriptor* find_type_descriptor(
    const void* name_data, std::uint64_t name_size) noexcept {
  const std::size_t size = native_size(name_size, "type name is too large");
  if (name_data == nullptr && size != 0) {
    runtime_failure("type name has null storage");
  }
  const auto* name = static_cast<const char*>(name_data);
  const std::lock_guard lock{descriptor_mutex};
  for (ClothTypeDescriptor* descriptor = descriptors; descriptor != nullptr;
       descriptor = descriptor->next) {
    if (descriptor->name_size == size &&
        (size == 0 || std::memcmp(descriptor->name, name, size) == 0)) {
      return descriptor;
    }
  }

  auto* descriptor = static_cast<ClothTypeDescriptor*>(
      std::malloc(sizeof(ClothTypeDescriptor)));
  auto* owned_name =
      static_cast<char*>(std::malloc(std::max(size, std::size_t{1})));
  if (descriptor == nullptr || owned_name == nullptr) {
    std::free(descriptor);
    std::free(owned_name);
    runtime_failure("type descriptor allocation failed");
  }
  if (size != 0) {
    std::memcpy(owned_name, name, size);
  }
  *descriptor = ClothTypeDescriptor{owned_name, size, descriptors};
  descriptors = descriptor;
  return descriptor;
}

void configure_stdout() noexcept {
#if defined(_WIN32)
  static const bool configured = _setmode(_fileno(stdout), _O_BINARY) != -1;
  if (!configured) {
    runtime_failure("standard output mode configuration failed");
  }
#endif
}

void write_stdout(std::string_view text) noexcept {
  configure_stdout();
  if (!text.empty() &&
      std::fwrite(text.data(), 1, text.size(), stdout) != text.size()) {
    runtime_failure("standard output write failed");
  }
  if (std::fflush(stdout) != 0) {
    runtime_failure("standard output flush failed");
  }
}

template <typename Integer>
void write_integer(Integer value) noexcept {
  std::array<char, 32> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    runtime_failure("integer formatting failed");
  }
  write_stdout(std::string_view{buffer.data(), result.ptr});
}

template <typename Float>
void write_float(Float value) noexcept {
  if (std::isnan(value)) {
    write_stdout("nan");
    return;
  }
  if (std::isinf(value)) {
    write_stdout(std::signbit(value) ? "-inf" : "inf");
    return;
  }
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (result.ec != std::errc{}) {
    runtime_failure("floating-point formatting failed");
  }
  write_stdout(std::string_view{buffer.data(), result.ptr});
}

}  // namespace

extern "C" void* cloth_rt_alloc(std::uint64_t size, std::uint64_t alignment,
                                const void* type_name,
                                std::uint64_t type_name_size) noexcept {
  if (size < sizeof(ClothObjectHeader)) {
    runtime_failure("object is smaller than its runtime header");
  }
  if (!is_power_of_two(alignment) || alignment < alignof(void*)) {
    runtime_failure("invalid object alignment");
  }
  auto* header = static_cast<ClothObjectHeader*>(
      allocate_aligned(size, alignment, "object allocation failed"));
  header->type = find_type_descriptor(type_name, type_name_size);
  header->runtime_state = nullptr;
  return header;
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
  array->data = allocate_aligned(
      static_cast<std::uint64_t>(native_length * native_element_size),
      element_alignment, "array payload allocation failed");
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
  write_integer(value);
}

extern "C" void cloth_rt_print_bool(std::uint8_t value) noexcept {
  if (value > 1) {
    runtime_failure("print received an invalid bool");
  }
  write_stdout(value == 0 ? std::string_view{"false"}
                          : std::string_view{"true"});
}

extern "C" void cloth_rt_print_char(std::uint32_t value) noexcept {
  std::array<char, 4> buffer{};
  std::size_t size = 0;
  if (value <= 0x7fU) {
    buffer[0] = static_cast<char>(value);
    size = 1;
  } else if (value <= 0x7ffU) {
    buffer[0] = static_cast<char>(0xc0U | (value >> 6U));
    buffer[1] = static_cast<char>(0x80U | (value & 0x3fU));
    size = 2;
  } else if (value <= 0xffffU && (value < 0xd800U || value > 0xdfffU)) {
    buffer[0] = static_cast<char>(0xe0U | (value >> 12U));
    buffer[1] = static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
    buffer[2] = static_cast<char>(0x80U | (value & 0x3fU));
    size = 3;
  } else if (value >= 0x10000U && value <= 0x10ffffU) {
    buffer[0] = static_cast<char>(0xf0U | (value >> 18U));
    buffer[1] = static_cast<char>(0x80U | ((value >> 12U) & 0x3fU));
    buffer[2] = static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
    buffer[3] = static_cast<char>(0x80U | (value & 0x3fU));
    size = 4;
  } else {
    runtime_failure("print received an invalid char");
  }
  write_stdout(std::string_view{buffer.data(), size});
}

extern "C" void cloth_rt_print_i8(std::int8_t value) noexcept {
  write_integer(static_cast<std::int32_t>(value));
}

extern "C" void cloth_rt_print_i16(std::int16_t value) noexcept {
  write_integer(static_cast<std::int32_t>(value));
}

extern "C" void cloth_rt_print_i64(std::int64_t value) noexcept {
  write_integer(value);
}

extern "C" void cloth_rt_print_u8(std::uint8_t value) noexcept {
  write_integer(static_cast<std::uint32_t>(value));
}

extern "C" void cloth_rt_print_u16(std::uint16_t value) noexcept {
  write_integer(static_cast<std::uint32_t>(value));
}

extern "C" void cloth_rt_print_u32(std::uint32_t value) noexcept {
  write_integer(value);
}

extern "C" void cloth_rt_print_u64(std::uint64_t value) noexcept {
  write_integer(value);
}

extern "C" void cloth_rt_print_f32(float value) noexcept { write_float(value); }

extern "C" void cloth_rt_print_f64(double value) noexcept {
  write_float(value);
}

extern "C" void cloth_rt_print_object(const void* value) noexcept {
  if (value == nullptr) {
    write_stdout("null");
    return;
  }
  const auto& header = *static_cast<const ClothObjectHeader*>(value);
  if (header.type == nullptr) {
    runtime_failure("object has no type descriptor");
  }
  write_stdout("<");
  write_stdout(std::string_view{header.type->name, header.type->name_size});
  write_stdout(">");
}

extern "C" void cloth_rt_print_newline() noexcept { write_stdout("\n"); }
