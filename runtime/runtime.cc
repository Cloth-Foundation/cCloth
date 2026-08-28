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
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#endif

namespace {

struct ClothObjectHeader {
  const ClothTypeDescriptor* type;
  void* runtime_state;
};

struct ClothString {
  ClothObjectHeader header;
  const char* data;
  std::size_t size;
};

struct ClothArray {
  ClothObjectHeader header;
  void* data;
  std::size_t length;
  std::size_t element_size;
  bool contains_references;
};

struct ClothAllocation {
  ClothAllocation* next;
  ClothAllocation* mark_next;
  void* object;
  std::uint64_t size;
  bool marked;
};

struct ClothAllocationIndexEntry {
  const void* object;
  ClothAllocation* allocation;
};

constexpr std::uint64_t kInitialCollectionThreshold = 64 * 1024;
constexpr std::size_t kInitialAllocationIndexCapacity = 64;
constexpr char kStringTypeName[] = "String";
constexpr char kArrayTypeName[] = "Array";
constexpr ClothTypeDescriptor kStringTypeDescriptor{
    ClothHeapObjectKind::kString,
    kStringTypeName,
    sizeof(kStringTypeName) - 1,
    sizeof(ClothString),
    alignof(ClothString),
    nullptr,
    0};
constexpr ClothTypeDescriptor kArrayTypeDescriptor{ClothHeapObjectKind::kArray,
                                                   kArrayTypeName,
                                                   sizeof(kArrayTypeName) - 1,
                                                   sizeof(ClothArray),
                                                   alignof(ClothArray),
                                                   nullptr,
                                                   0};

thread_local ClothGcRootFrame* current_root_frame = nullptr;
ClothAllocation* allocations = nullptr;
ClothAllocationIndexEntry* allocation_index = nullptr;
std::size_t allocation_index_capacity = 0;
std::uint64_t live_object_count = 0;
std::uint64_t live_byte_count = 0;
std::uint64_t collection_count = 0;
std::uint64_t peak_live_byte_count = 0;
std::uint64_t collection_threshold = kInitialCollectionThreshold;
bool collection_in_progress = false;

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

void free_aligned(void* storage) noexcept {
#if defined(_WIN32)
  _aligned_free(storage);
#else
  std::free(storage);
#endif
}

std::uint64_t saturating_double(std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() / 2) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return value * 2;
}

std::size_t hash_object(const void* object) noexcept {
  std::uintptr_t value = reinterpret_cast<std::uintptr_t>(object);
  value ^= value >> 4U;
  value *= static_cast<std::uintptr_t>(0x9e3779b1U);
  value ^= value >> 16U;
  return static_cast<std::size_t>(value);
}

void insert_allocation_index(ClothAllocationIndexEntry* index,
                             std::size_t capacity,
                             ClothAllocation* allocation) noexcept {
  std::size_t slot = hash_object(allocation->object) & (capacity - 1);
  while (index[slot].object != nullptr) {
    slot = (slot + 1) & (capacity - 1);
  }
  index[slot] = ClothAllocationIndexEntry{allocation->object, allocation};
}

void reserve_allocation_index(std::uint64_t required_size) noexcept {
  if (required_size <= allocation_index_capacity / 2) {
    return;
  }
  std::size_t capacity =
      std::max(kInitialAllocationIndexCapacity, allocation_index_capacity);
  const std::uint64_t maximum_capacity =
      std::numeric_limits<std::size_t>::max() /
      sizeof(ClothAllocationIndexEntry);
  while (required_size > capacity / 2) {
    if (capacity > maximum_capacity / 2) {
      runtime_failure("managed allocation index is too large");
    }
    capacity *= 2;
  }

  auto* index = static_cast<ClothAllocationIndexEntry*>(
      std::calloc(capacity, sizeof(ClothAllocationIndexEntry)));
  if (index == nullptr) {
    runtime_failure("managed allocation index failed");
  }
  for (ClothAllocation* allocation = allocations; allocation != nullptr;
       allocation = allocation->next) {
    insert_allocation_index(index, capacity, allocation);
  }
  std::free(allocation_index);
  allocation_index = index;
  allocation_index_capacity = capacity;
}

void rebuild_allocation_index() noexcept {
  if (allocation_index == nullptr) {
    return;
  }
  std::memset(allocation_index, 0,
              allocation_index_capacity * sizeof(ClothAllocationIndexEntry));
  for (ClothAllocation* allocation = allocations; allocation != nullptr;
       allocation = allocation->next) {
    insert_allocation_index(allocation_index, allocation_index_capacity,
                            allocation);
  }
}

void validate_type_descriptor(const ClothTypeDescriptor* type) noexcept {
  if (type == nullptr) {
    runtime_failure("object type descriptor is null");
  }
  if (type->kind != ClothHeapObjectKind::kFileClass) {
    runtime_failure("object type descriptor has the wrong kind");
  }
  if (type->name == nullptr && type->name_size != 0) {
    runtime_failure("object type name has null storage");
  }
  if (type->size < sizeof(ClothObjectHeader)) {
    runtime_failure("object is smaller than its runtime header");
  }
  if (!is_power_of_two(type->alignment) || type->alignment < alignof(void*)) {
    runtime_failure("invalid object alignment");
  }
  if ((type->reference_offsets == nullptr) != (type->reference_count == 0)) {
    runtime_failure("object reference metadata is inconsistent");
  }

  std::uint64_t previous_offset = 0;
  bool has_previous_offset = false;
  for (std::uint64_t index = 0; index < type->reference_count; ++index) {
    const std::uint64_t offset = type->reference_offsets[index];
    if (offset < sizeof(ClothObjectHeader) || offset % alignof(void*) != 0 ||
        offset > type->size || sizeof(void*) > type->size - offset ||
        (has_previous_offset && offset <= previous_offset)) {
      runtime_failure("object reference metadata has an invalid offset");
    }
    previous_offset = offset;
    has_previous_offset = true;
  }
}

ClothAllocation* find_allocation(const void* object) noexcept {
  if (object == nullptr || allocation_index_capacity == 0) {
    return nullptr;
  }
  std::size_t slot = hash_object(object) & (allocation_index_capacity - 1);
  while (allocation_index[slot].object != nullptr) {
    if (allocation_index[slot].object == object) {
      return allocation_index[slot].allocation;
    }
    slot = (slot + 1) & (allocation_index_capacity - 1);
  }
  return nullptr;
}

void enqueue_reference(void* object, ClothAllocation*& worklist) noexcept {
  ClothAllocation* allocation = find_allocation(object);
  if (allocation == nullptr || allocation->marked) {
    return;
  }
  allocation->marked = true;
  allocation->mark_next = worklist;
  worklist = allocation;
}

void mark_reachable_objects() noexcept {
  ClothAllocation* worklist = nullptr;
  for (ClothGcRootFrame* frame = current_root_frame; frame != nullptr;
       frame = frame->previous) {
    const std::size_t root_count =
        native_size(frame->root_count, "GC root count is too large");
    for (std::size_t index = 0; index < root_count; ++index) {
      enqueue_reference(*frame->roots[index], worklist);
    }
  }

  while (worklist != nullptr) {
    ClothAllocation* allocation = worklist;
    worklist = allocation->mark_next;
    allocation->mark_next = nullptr;
    const auto& header =
        *static_cast<const ClothObjectHeader*>(allocation->object);
    if (header.type == nullptr || header.runtime_state != allocation) {
      runtime_failure("managed object header is corrupt");
    }
    switch (header.type->kind) {
      case ClothHeapObjectKind::kFileClass:
        for (std::uint64_t index = 0; index < header.type->reference_count;
             ++index) {
          void* reference = nullptr;
          const std::uint64_t offset = header.type->reference_offsets[index];
          std::memcpy(
              &reference,
              static_cast<const std::byte*>(allocation->object) + offset,
              sizeof(reference));
          enqueue_reference(reference, worklist);
        }
        break;
      case ClothHeapObjectKind::kString:
        break;
      case ClothHeapObjectKind::kArray: {
        const auto& array = *static_cast<const ClothArray*>(allocation->object);
        if (!array.contains_references) {
          break;
        }
        for (std::size_t index = 0; index < array.length; ++index) {
          void* reference = nullptr;
          std::memcpy(&reference,
                      static_cast<const std::byte*>(array.data) +
                          index * array.element_size,
                      sizeof(reference));
          enqueue_reference(reference, worklist);
        }
        break;
      }
      default:
        runtime_failure("managed object has an invalid kind");
    }
  }
}

void destroy_managed_object(ClothAllocation* allocation) noexcept {
  const auto& header =
      *static_cast<const ClothObjectHeader*>(allocation->object);
  if (header.type == nullptr || header.runtime_state != allocation) {
    runtime_failure("managed object header is corrupt");
  }
  switch (header.type->kind) {
    case ClothHeapObjectKind::kFileClass:
    case ClothHeapObjectKind::kString:
      break;
    case ClothHeapObjectKind::kArray: {
      auto& array = *static_cast<ClothArray*>(allocation->object);
      free_aligned(array.data);
      break;
    }
    default:
      runtime_failure("managed object has an invalid kind");
  }
  free_aligned(allocation->object);
}

void sweep_unreachable_objects() noexcept {
  ClothAllocation** link = &allocations;
  while (*link != nullptr) {
    ClothAllocation* allocation = *link;
    if (allocation->marked) {
      allocation->marked = false;
      link = &allocation->next;
      continue;
    }

    *link = allocation->next;
    --live_object_count;
    live_byte_count -= allocation->size;
    destroy_managed_object(allocation);
    std::free(allocation);
  }
  rebuild_allocation_index();
}

void collect_heap() noexcept {
  if (collection_in_progress) {
    runtime_failure("garbage collection is already active");
  }
  collection_in_progress = true;
  mark_reachable_objects();
  sweep_unreachable_objects();
  if (collection_count != std::numeric_limits<std::uint64_t>::max()) {
    ++collection_count;
  }
  collection_threshold =
      std::max(kInitialCollectionThreshold, saturating_double(live_byte_count));
  collection_in_progress = false;
}

void collect_before_allocation(std::uint64_t size) noexcept {
  if (size > std::numeric_limits<std::uint64_t>::max() - live_byte_count) {
    runtime_failure("managed heap size overflow");
  }
  const std::uint64_t projected_size = live_byte_count + size;
  if (projected_size <= collection_threshold) {
    return;
  }

  collect_heap();
  if (size > std::numeric_limits<std::uint64_t>::max() - live_byte_count) {
    runtime_failure("managed heap size overflow");
  }
  const std::uint64_t post_collection_size = live_byte_count + size;
  if (post_collection_size > collection_threshold) {
    collection_threshold = std::max(kInitialCollectionThreshold,
                                    saturating_double(post_collection_size));
  }
}

void register_allocation(ClothObjectHeader* object,
                         std::uint64_t size) noexcept {
  if (live_object_count == std::numeric_limits<std::uint64_t>::max()) {
    free_aligned(object);
    runtime_failure("managed object count overflow");
  }
  reserve_allocation_index(live_object_count + 1);
  auto* allocation =
      static_cast<ClothAllocation*>(std::malloc(sizeof(ClothAllocation)));
  if (allocation == nullptr) {
    free_aligned(object);
    runtime_failure("managed allocation registry failed");
  }
  *allocation = ClothAllocation{allocations, nullptr, object, size, false};
  allocations = allocation;
  insert_allocation_index(allocation_index, allocation_index_capacity,
                          allocation);
  ++live_object_count;
  live_byte_count += size;
  peak_live_byte_count = std::max(peak_live_byte_count, live_byte_count);
  object->runtime_state = allocation;
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

extern "C" void* cloth_rt_alloc(const ClothTypeDescriptor* type) noexcept {
  validate_type_descriptor(type);
  collect_before_allocation(type->size);
  auto* header = static_cast<ClothObjectHeader*>(allocate_aligned(
      type->size, type->alignment, "object allocation failed"));
  header->type = type;
  register_allocation(header, type->size);
  return header;
}

extern "C" void cloth_rt_gc_push_frame(ClothGcRootFrame* frame, void*** roots,
                                       std::uint64_t root_count) noexcept {
  if (frame == nullptr) {
    runtime_failure("GC root frame is null");
  }
  if ((roots == nullptr) != (root_count == 0)) {
    runtime_failure("GC root frame metadata is inconsistent");
  }
  if (frame == current_root_frame) {
    runtime_failure("GC root frame is already active");
  }
  const std::size_t count =
      native_size(root_count, "GC root count is too large");
  for (std::size_t index = 0; index < count; ++index) {
    if (roots[index] == nullptr) {
      runtime_failure("GC root slot is null");
    }
  }

  frame->previous = current_root_frame;
  frame->roots = roots;
  frame->root_count = root_count;
  current_root_frame = frame;
}

extern "C" void cloth_rt_gc_pop_frame(ClothGcRootFrame* frame) noexcept {
  if (frame == nullptr || current_root_frame != frame) {
    runtime_failure("GC root frames were popped out of order");
  }
  current_root_frame = frame->previous;
  frame->previous = nullptr;
  frame->roots = nullptr;
  frame->root_count = 0;
}

extern "C" void cloth_rt_gc_collect() noexcept { collect_heap(); }

extern "C" std::uint64_t cloth_rt_gc_live_objects() noexcept {
  return live_object_count;
}

extern "C" std::uint64_t cloth_rt_gc_live_bytes() noexcept {
  return live_byte_count;
}

extern "C" std::uint64_t cloth_rt_gc_collection_count() noexcept {
  return collection_count;
}

extern "C" std::uint64_t cloth_rt_gc_peak_live_bytes() noexcept {
  return peak_live_byte_count;
}

extern "C" void* cloth_rt_string_literal(const void* data,
                                         std::uint64_t size) noexcept {
  const std::size_t string_size =
      native_size(size, "string literal is too large");
  if (data == nullptr && string_size != 0) {
    runtime_failure("string literal has null storage");
  }
  collect_before_allocation(sizeof(ClothString));
  auto* string = static_cast<ClothString*>(allocate_aligned(
      sizeof(ClothString), alignof(ClothString), "string allocation failed"));
  *string = ClothString{{&kStringTypeDescriptor, nullptr},
                        static_cast<const char*>(data),
                        string_size};
  register_allocation(&string->header, sizeof(ClothString));
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
  if (contains_references != 0 &&
      (element_size != sizeof(void*) || element_alignment < alignof(void*))) {
    runtime_failure("invalid reference array element layout");
  }

  const std::size_t native_length = static_cast<std::size_t>(length);
  const std::size_t native_element_size =
      native_size(element_size, "array element is too large");
  if (native_length >
      std::numeric_limits<std::size_t>::max() / native_element_size) {
    runtime_failure("array allocation is too large");
  }
  const std::uint64_t payload_size =
      static_cast<std::uint64_t>(native_length * native_element_size);
  if (payload_size >
      std::numeric_limits<std::uint64_t>::max() - sizeof(ClothArray)) {
    runtime_failure("managed array size overflow");
  }
  const std::uint64_t managed_size = sizeof(ClothArray) + payload_size;
  collect_before_allocation(managed_size);
  auto* array = static_cast<ClothArray*>(
      allocate_aligned(sizeof(ClothArray), alignof(ClothArray),
                       "array header allocation failed"));
  array->header = ClothObjectHeader{&kArrayTypeDescriptor, nullptr};
  array->data = allocate_aligned(payload_size, element_alignment,
                                 "array payload allocation failed");
  array->length = native_length;
  array->element_size = native_element_size;
  array->contains_references = contains_references != 0;
  register_allocation(&array->header, managed_size);
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

extern "C" void cloth_rt_require_non_null(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("non-null assertion failed");
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
  write_stdout(std::string_view{
      header.type->name,
      native_size(header.type->name_size, "object type name is too large")});
  write_stdout(">");
}

extern "C" void cloth_rt_print_newline() noexcept { write_stdout("\n"); }
