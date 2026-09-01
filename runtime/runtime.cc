// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
  std::size_t byte_size;
  std::size_t scalar_count;
  bool owns_data;
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
constexpr char kStringTypeName[] = "string";
constexpr char kArrayTypeName[] = "Array";
constexpr char kArrayMetaTypeName[] = "array";
constexpr ClothTypeDescriptor kStringTypeDescriptor{
    ClothHeapObjectKind::kString,
    nullptr,
    kStringTypeName,
    sizeof(kStringTypeName) - 1,
    sizeof(ClothString),
    alignof(ClothString),
    nullptr,
    0,
    nullptr,
    0,
    nullptr,
    0};
constexpr ClothTypeDescriptor kArrayTypeDescriptor{ClothHeapObjectKind::kArray,
                                                   nullptr,
                                                   kArrayTypeName,
                                                   sizeof(kArrayTypeName) - 1,
                                                   sizeof(ClothArray),
                                                   alignof(ClothArray),
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   0,
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

std::size_t count_utf8_scalars(const char* data, std::size_t size) noexcept {
  std::size_t count = 0;
  std::size_t index = 0;
  const auto byte = [data](std::size_t offset) {
    return static_cast<unsigned char>(data[offset]);
  };
  const auto is_continuation = [&byte](std::size_t offset) {
    return (byte(offset) & 0xc0U) == 0x80U;
  };
  while (index < size) {
    const unsigned char first = byte(index);
    std::size_t width = 0;
    if (first <= 0x7fU) {
      width = 1;
    } else if (first >= 0xc2U && first <= 0xdfU && index + 1 < size &&
               is_continuation(index + 1)) {
      width = 2;
    } else if (first == 0xe0U && index + 2 < size && byte(index + 1) >= 0xa0U &&
               byte(index + 1) <= 0xbfU && is_continuation(index + 2)) {
      width = 3;
    } else if (((first >= 0xe1U && first <= 0xecU) ||
                (first >= 0xeeU && first <= 0xefU)) &&
               index + 2 < size && is_continuation(index + 1) &&
               is_continuation(index + 2)) {
      width = 3;
    } else if (first == 0xedU && index + 2 < size && byte(index + 1) >= 0x80U &&
               byte(index + 1) <= 0x9fU && is_continuation(index + 2)) {
      width = 3;
    } else if (first == 0xf0U && index + 3 < size && byte(index + 1) >= 0x90U &&
               byte(index + 1) <= 0xbfU && is_continuation(index + 2) &&
               is_continuation(index + 3)) {
      width = 4;
    } else if (first >= 0xf1U && first <= 0xf3U && index + 3 < size &&
               is_continuation(index + 1) && is_continuation(index + 2) &&
               is_continuation(index + 3)) {
      width = 4;
    } else if (first == 0xf4U && index + 3 < size && byte(index + 1) >= 0x80U &&
               byte(index + 1) <= 0x8fU && is_continuation(index + 2) &&
               is_continuation(index + 3)) {
      width = 4;
    } else {
      runtime_failure("string contains invalid UTF-8");
    }
    index += width;
    ++count;
  }
  return count;
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
  if (type->parent != nullptr &&
      (type->parent == type ||
       type->parent->kind != ClothHeapObjectKind::kFileClass ||
       type->parent->size > type->size ||
       type->parent->alignment > type->alignment ||
       type->parent->virtual_function_count > type->virtual_function_count)) {
    runtime_failure("object type descriptor has an invalid parent");
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
  if ((type->virtual_functions == nullptr) !=
      (type->virtual_function_count == 0)) {
    runtime_failure("object virtual-function metadata is inconsistent");
  }
  for (std::uint64_t index = 0; index < type->virtual_function_count; ++index) {
    if (type->virtual_functions[index] == nullptr) {
      runtime_failure("object virtual-function slot is null");
    }
  }
  if ((type->interfaces == nullptr) != (type->interface_count == 0)) {
    runtime_failure("object interface metadata is inconsistent");
  }
  std::uint64_t previous_interface_id = 0;
  bool has_previous_interface = false;
  for (std::uint64_t index = 0; index < type->interface_count; ++index) {
    const ClothInterfaceDispatch& interface = type->interfaces[index];
    if ((interface.functions == nullptr) != (interface.function_count == 0) ||
        (has_previous_interface &&
         interface.interface_id <= previous_interface_id)) {
      runtime_failure("object interface dispatch metadata is invalid");
    }
    for (std::uint64_t slot = 0; slot < interface.function_count; ++slot) {
      if (interface.functions[slot] == nullptr) {
        runtime_failure("object interface function slot is null");
      }
    }
    previous_interface_id = interface.interface_id;
    has_previous_interface = true;
  }
  if (type->parent != nullptr) {
    for (std::uint64_t parent_index = 0;
         parent_index < type->parent->interface_count; ++parent_index) {
      const ClothInterfaceDispatch& parent_interface =
          type->parent->interfaces[parent_index];
      bool found = false;
      for (std::uint64_t index = 0; index < type->interface_count; ++index) {
        const ClothInterfaceDispatch& interface = type->interfaces[index];
        if (interface.interface_id == parent_interface.interface_id) {
          found = interface.function_count == parent_interface.function_count;
          break;
        }
      }
      if (!found) {
        runtime_failure("derived object lost inherited interface metadata");
      }
    }
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
      break;
    case ClothHeapObjectKind::kString: {
      auto& string = *static_cast<ClothString*>(allocation->object);
      if (string.owns_data) {
        free_aligned(const_cast<char*>(string.data));
      }
      break;
    }
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

const ClothString& require_string(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("null string");
  }
  const auto& string = *static_cast<const ClothString*>(value);
  if (string.header.type != &kStringTypeDescriptor) {
    runtime_failure("invalid string object");
  }
  return string;
}

const ClothObjectHeader& require_object(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("null object");
  }
  const auto& object = *static_cast<const ClothObjectHeader*>(value);
  if (object.type == nullptr) {
    runtime_failure("object has no type descriptor");
  }
  return object;
}

const ClothArray& require_byte_array(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("null byte array");
  }
  const auto& array = *static_cast<const ClothArray*>(value);
  if (array.header.type != &kArrayTypeDescriptor || array.element_size != 1 ||
      array.contains_references) {
    runtime_failure("invalid byte array");
  }
  return array;
}

std::size_t integer_byte_offset(const ClothArray& array, std::int32_t offset,
                                std::uint8_t byte_width) noexcept {
  if (byte_width != 1 && byte_width != 2 && byte_width != 4 &&
      byte_width != 8) {
    runtime_failure("invalid integer byte width");
  }
  if (offset < 0) {
    runtime_failure("integer byte range is out of bounds");
  }
  const std::size_t native_offset = static_cast<std::size_t>(offset);
  if (native_offset > array.length ||
      byte_width > array.length - native_offset) {
    runtime_failure("integer byte range is out of bounds");
  }
  return native_offset;
}

ClothString* allocate_borrowed_string(const char* data, std::size_t byte_size,
                                      std::size_t scalar_count) noexcept {
  collect_before_allocation(sizeof(ClothString));
  auto* string = static_cast<ClothString*>(allocate_aligned(
      sizeof(ClothString), alignof(ClothString), "string allocation failed"));
  *string = ClothString{
      {&kStringTypeDescriptor, nullptr}, data, byte_size, scalar_count, false};
  register_allocation(&string->header, sizeof(ClothString));
  return string;
}

ClothString* allocate_concatenated_string(const ClothString& left,
                                          const ClothString& right) noexcept {
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (left.byte_size > kMaximumStringSize - right.byte_size ||
      left.scalar_count > kMaximumStringSize - right.scalar_count) {
    runtime_failure("string concatenation is too large");
  }
  const std::size_t byte_size = left.byte_size + right.byte_size;
  const std::size_t scalar_count = left.scalar_count + right.scalar_count;
  const std::uint64_t managed_size = sizeof(ClothString) + byte_size;
  collect_before_allocation(managed_size);
  auto* string = static_cast<ClothString*>(allocate_aligned(
      sizeof(ClothString), alignof(ClothString), "string allocation failed"));
  auto* data = static_cast<char*>(allocate_aligned(
      byte_size, alignof(char), "string payload allocation failed"));
  if (left.byte_size != 0) {
    std::memcpy(data, left.data, left.byte_size);
  }
  if (right.byte_size != 0) {
    std::memcpy(data + left.byte_size, right.data, right.byte_size);
  }
  *string = ClothString{
      {&kStringTypeDescriptor, nullptr}, data, byte_size, scalar_count, true};
  register_allocation(&string->header, managed_size);
  return string;
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
  const auto* bytes = static_cast<const char*>(data);
  const std::size_t scalar_count = count_utf8_scalars(bytes, string_size);
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (string_size > kMaximumStringSize || scalar_count > kMaximumStringSize) {
    runtime_failure("string literal is too large");
  }
  return allocate_borrowed_string(bytes, string_size, scalar_count);
}

extern "C" void* cloth_rt_string_concat(const void* left,
                                        const void* right) noexcept {
  return allocate_concatenated_string(require_string(left),
                                      require_string(right));
}

extern "C" std::uint8_t cloth_rt_string_equal(const void* left,
                                              const void* right) noexcept {
  if (left == right) {
    return 1;
  }
  if (left == nullptr || right == nullptr) {
    return 0;
  }
  const ClothString& left_string = require_string(left);
  const ClothString& right_string = require_string(right);
  return left_string.byte_size == right_string.byte_size &&
                 (left_string.byte_size == 0 ||
                  std::memcmp(left_string.data, right_string.data,
                              left_string.byte_size) == 0)
             ? 1
             : 0;
}

extern "C" std::int32_t cloth_rt_string_length(const void* value) noexcept {
  return static_cast<std::int32_t>(require_string(value).scalar_count);
}

extern "C" std::int32_t cloth_rt_string_byte_length(
    const void* value) noexcept {
  return static_cast<std::int32_t>(require_string(value).byte_size);
}

extern "C" std::uint8_t cloth_rt_string_is_empty(const void* value) noexcept {
  return require_string(value).byte_size == 0 ? 1 : 0;
}

extern "C" void* cloth_rt_object_type_name(const void* value) noexcept {
  const ClothObjectHeader& object = require_object(value);
  const char* name = object.type->name;
  std::size_t name_size =
      native_size(object.type->name_size, "object type name is too large");
  if (object.type->kind == ClothHeapObjectKind::kArray) {
    name = kArrayMetaTypeName;
    name_size = sizeof(kArrayMetaTypeName) - 1;
  }
  if (name == nullptr) {
    runtime_failure("object type name has null storage");
  }
  const std::size_t scalar_count = count_utf8_scalars(name, name_size);
  return allocate_borrowed_string(name, name_size, scalar_count);
}

extern "C" std::uint8_t cloth_rt_object_is_kind(const void* value,
                                                std::uint64_t kind) noexcept {
  if (value == nullptr) {
    return 0;
  }
  if (kind > static_cast<std::uint64_t>(ClothHeapObjectKind::kArray)) {
    runtime_failure("invalid heap object kind");
  }
  const ClothObjectHeader& object = require_object(value);
  return object.type->kind == static_cast<ClothHeapObjectKind>(kind) ? 1 : 0;
}

extern "C" std::uint8_t cloth_rt_object_is_type(
    const void* value, const ClothTypeDescriptor* type) noexcept {
  validate_type_descriptor(type);
  if (value == nullptr) {
    return 0;
  }
  const ClothTypeDescriptor* current = require_object(value).type;
  const ClothTypeDescriptor* slow = current;
  const ClothTypeDescriptor* fast = current;
  while (current != nullptr) {
    if (current->kind != ClothHeapObjectKind::kFileClass) {
      return 0;
    }
    validate_type_descriptor(current);
    if (current == type) {
      return 1;
    }
    current = current->parent;

    if (slow != nullptr) {
      validate_type_descriptor(slow);
      slow = slow->parent;
    }
    for (std::size_t step = 0; step < 2 && fast != nullptr; ++step) {
      validate_type_descriptor(fast);
      fast = fast->parent;
    }
    if (slow != nullptr && slow == fast) {
      runtime_failure("object type descriptor ancestry contains a cycle");
    }
  }
  return 0;
}

namespace {

const ClothInterfaceDispatch* find_interface_dispatch(
    const ClothTypeDescriptor* type, std::uint64_t interface_id) noexcept {
  std::uint64_t begin = 0;
  std::uint64_t end = type->interface_count;
  while (begin < end) {
    const std::uint64_t middle = begin + (end - begin) / 2;
    const ClothInterfaceDispatch& candidate = type->interfaces[middle];
    if (candidate.interface_id < interface_id) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  if (begin == type->interface_count ||
      type->interfaces[begin].interface_id != interface_id) {
    return nullptr;
  }
  return &type->interfaces[begin];
}

}  // namespace

extern "C" std::uint8_t cloth_rt_object_is_interface(
    const void* value, std::uint64_t interface_id) noexcept {
  if (value == nullptr) {
    return 0;
  }
  const ClothTypeDescriptor* type = require_object(value).type;
  if (type->kind != ClothHeapObjectKind::kFileClass) {
    return 0;
  }
  validate_type_descriptor(type);
  return find_interface_dispatch(type, interface_id) != nullptr ? 1 : 0;
}

extern "C" const void* cloth_rt_interface_function(
    const void* value, std::uint64_t interface_id,
    std::uint64_t function_slot) noexcept {
  const ClothTypeDescriptor* type = require_object(value).type;
  validate_type_descriptor(type);
  const ClothInterfaceDispatch* interface =
      find_interface_dispatch(type, interface_id);
  if (interface == nullptr) {
    runtime_failure("object does not implement the requested interface");
  }
  if (function_slot >= interface->function_count) {
    runtime_failure("interface function slot is out of bounds");
  }
  return interface->functions[function_slot];
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

extern "C" void cloth_rt_integer_write(void* destination, std::int32_t offset,
                                       std::uint64_t bits,
                                       std::uint8_t byte_width,
                                       std::uint8_t byte_order) noexcept {
  const ClothArray& array = require_byte_array(destination);
  const std::size_t native_offset =
      integer_byte_offset(array, offset, byte_width);
  if (byte_order > 1) {
    runtime_failure("invalid integer byte order");
  }
  auto* bytes = static_cast<std::uint8_t*>(array.data);
  for (std::size_t index = 0; index < byte_width; ++index) {
    const std::size_t destination_index =
        byte_order == 0 ? index : byte_width - index - 1;
    bytes[native_offset + destination_index] =
        static_cast<std::uint8_t>(bits >> (index * 8));
  }
}

extern "C" std::uint64_t cloth_rt_integer_read(
    const void* source, std::int32_t offset, std::uint8_t byte_width,
    std::uint8_t byte_order) noexcept {
  const ClothArray& array = require_byte_array(source);
  const std::size_t native_offset =
      integer_byte_offset(array, offset, byte_width);
  if (byte_order > 1) {
    runtime_failure("invalid integer byte order");
  }
  const auto* bytes = static_cast<const std::uint8_t*>(array.data);
  std::uint64_t bits = 0;
  for (std::size_t index = 0; index < byte_width; ++index) {
    const std::size_t source_index =
        byte_order == 0 ? index : byte_width - index - 1;
    bits |= static_cast<std::uint64_t>(bytes[native_offset + source_index])
            << (index * 8);
  }
  return bits;
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

extern "C" void cloth_rt_require_numeric_conversion(
    std::uint8_t valid) noexcept {
  if (valid == 0) {
    runtime_failure("numeric conversion is out of range");
  }
}

extern "C" void cloth_rt_require_shift_count(std::uint8_t valid) noexcept {
  if (valid == 0) {
    runtime_failure("shift count is out of range");
  }
}

extern "C" void cloth_rt_print(const void* value) noexcept {
  if (value == nullptr) {
    runtime_failure("print received a null string");
  }
  const auto& string = *static_cast<const ClothString*>(value);
  write_stdout(std::string_view{string.data, string.byte_size});
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
