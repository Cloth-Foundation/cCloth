// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/runtime/runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef interface
#undef interface
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

struct ClothError {
  ClothObjectHeader header;
  void* message;
};

struct ClothArray {
  ClothObjectHeader header;
  void* data;
  std::size_t length;
  std::size_t element_size;
  const ClothArrayElementLayout* element;
};

struct ClothAllocation {
  ClothAllocation* next;
  ClothAllocation* mark_next;
  void* object;
  std::uint64_t size;
  std::uint64_t identity;
  bool marked;
};

struct ClothAllocationIndexEntry {
  const void* object;
  ClothAllocation* allocation;
};

constexpr std::uint64_t kInitialCollectionThreshold = 64 * 1024;
constexpr std::size_t kInitialAllocationIndexCapacity = 64;
constexpr char kObjectTypeName[] = "cloth.lang.Object";
constexpr char kStringTypeName[] = "string";
constexpr char kArrayTypeName[] = "Array";
constexpr char kArrayMetaTypeName[] = "array";
constexpr char kErrorTypeName[] = "Error";
constexpr char kDivisionByZeroTypeName[] = "DivisionByZero";
constexpr std::uint64_t kErrorReferenceOffsets[]{offsetof(ClothError, message)};

bool string_equals(const void* value, const void* other) noexcept;
std::uint64_t string_hash_code(const void* value) noexcept;
void* string_to_string(const void* value) noexcept;

const void* const kObjectVirtualFunctions[]{
    reinterpret_cast<const void*>(&cloth_rt_object_equals),
    reinterpret_cast<const void*>(&cloth_rt_object_hash_code),
    reinterpret_cast<const void*>(&cloth_rt_object_to_string)};
const void* const kStringVirtualFunctions[]{
    reinterpret_cast<const void*>(&string_equals),
    reinterpret_cast<const void*>(&string_hash_code),
    reinterpret_cast<const void*>(&string_to_string)};
const ClothTypeDescriptor kStringTypeDescriptor{ClothHeapObjectKind::kString,
                                                &cloth_rt_object_type,
                                                kStringTypeName,
                                                sizeof(kStringTypeName) - 1,
                                                sizeof(ClothString),
                                                alignof(ClothString),
                                                nullptr,
                                                0,
                                                kStringVirtualFunctions,
                                                3,
                                                nullptr,
                                                0,
                                                nullptr,
                                                0};
const ClothTypeDescriptor kArrayTypeDescriptor{ClothHeapObjectKind::kArray,
                                               &cloth_rt_object_type,
                                               kArrayTypeName,
                                               sizeof(kArrayTypeName) - 1,
                                               sizeof(ClothArray),
                                               alignof(ClothArray),
                                               nullptr,
                                               0,
                                               kObjectVirtualFunctions,
                                               3,
                                               nullptr,
                                               0,
                                               nullptr,
                                               0};
constexpr std::uint64_t kProgramArgumentReferenceOffsets[]{0};
constexpr ClothArrayElementLayout kProgramArgumentElementLayout{
    sizeof(void*), alignof(void*), kProgramArgumentReferenceOffsets, 1};
constexpr ClothArrayElementLayout kByteElementLayout{1, 1, nullptr, 0};
constexpr std::size_t kMaximumFileByteCount = 64U * 1024U * 1024U;

thread_local ClothGcRootFrame* current_root_frame = nullptr;
ClothAllocation* allocations = nullptr;
ClothAllocationIndexEntry* allocation_index = nullptr;
std::size_t allocation_index_capacity = 0;
std::uint64_t live_object_count = 0;
std::uint64_t live_byte_count = 0;
std::uint64_t collection_count = 0;
std::uint64_t peak_live_byte_count = 0;
std::uint64_t collection_threshold = kInitialCollectionThreshold;
std::uint64_t next_allocation_identity = 1;
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

bool is_heap_object_kind(ClothHeapObjectKind kind) noexcept {
  return kind == ClothHeapObjectKind::kFileClass ||
         kind == ClothHeapObjectKind::kString ||
         kind == ClothHeapObjectKind::kArray ||
         kind == ClothHeapObjectKind::kError ||
         kind == ClothHeapObjectKind::kValueBox;
}

std::size_t native_size(std::uint64_t value,
                        std::string_view description) noexcept {
  if (value > std::numeric_limits<std::size_t>::max()) {
    runtime_failure(description);
  }
  return static_cast<std::size_t>(value);
}

bool decode_utf8_scalar(const char* data, std::size_t size, std::size_t& index,
                        std::uint32_t& scalar) noexcept {
  const auto byte = [data](std::size_t offset) {
    return static_cast<unsigned char>(data[offset]);
  };
  const auto is_continuation = [&byte](std::size_t offset) {
    return (byte(offset) & 0xc0U) == 0x80U;
  };
  if (index >= size) {
    return false;
  }
  const unsigned char first = byte(index);
  if (first <= 0x7fU) {
    scalar = first;
    ++index;
    return true;
  }
  if (first >= 0xc2U && first <= 0xdfU && index + 1 < size &&
      is_continuation(index + 1)) {
    scalar = ((first & 0x1fU) << 6U) | (byte(index + 1) & 0x3fU);
    index += 2;
    return true;
  }
  if (index + 2 < size &&
      ((first == 0xe0U && byte(index + 1) >= 0xa0U &&
        byte(index + 1) <= 0xbfU && is_continuation(index + 2)) ||
       (((first >= 0xe1U && first <= 0xecU) ||
         (first >= 0xeeU && first <= 0xefU)) &&
        is_continuation(index + 1) && is_continuation(index + 2)) ||
       (first == 0xedU && byte(index + 1) >= 0x80U &&
        byte(index + 1) <= 0x9fU && is_continuation(index + 2)))) {
    scalar = ((first & 0x0fU) << 12U) | ((byte(index + 1) & 0x3fU) << 6U) |
             (byte(index + 2) & 0x3fU);
    index += 3;
    return true;
  }
  if (index + 3 < size &&
      ((first == 0xf0U && byte(index + 1) >= 0x90U &&
        byte(index + 1) <= 0xbfU && is_continuation(index + 2) &&
        is_continuation(index + 3)) ||
       (first >= 0xf1U && first <= 0xf3U && is_continuation(index + 1) &&
        is_continuation(index + 2) && is_continuation(index + 3)) ||
       (first == 0xf4U && byte(index + 1) >= 0x80U &&
        byte(index + 1) <= 0x8fU && is_continuation(index + 2) &&
        is_continuation(index + 3)))) {
    scalar = ((first & 0x07U) << 18U) | ((byte(index + 1) & 0x3fU) << 12U) |
             ((byte(index + 2) & 0x3fU) << 6U) | (byte(index + 3) & 0x3fU);
    index += 4;
    return true;
  }
  return false;
}

bool try_count_utf8_scalars(const char* data, std::size_t size,
                            std::size_t& count) noexcept {
  count = 0;
  std::size_t index = 0;
  while (index < size) {
    std::uint32_t scalar = 0;
    if (!decode_utf8_scalar(data, size, index, scalar)) {
      return false;
    }
    ++count;
  }
  return true;
}

std::size_t count_utf8_scalars(const char* data, std::size_t size,
                               std::string_view invalid_message =
                                   "string contains invalid UTF-8") noexcept {
  std::size_t count = 0;
  if (!try_count_utf8_scalars(data, size, count)) {
    runtime_failure(invalid_message);
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

std::uint64_t scalar_value_size(ClothValueKind kind) noexcept {
  switch (kind) {
    case ClothValueKind::kBool:
    case ClothValueKind::kByte:
    case ClothValueKind::kInt8:
    case ClothValueKind::kUint8:
      return 1;
    case ClothValueKind::kInt16:
    case ClothValueKind::kUint16:
      return 2;
    case ClothValueKind::kChar:
    case ClothValueKind::kInt32:
    case ClothValueKind::kUint32:
    case ClothValueKind::kFloat32:
    case ClothValueKind::kEnum:
      return 4;
    case ClothValueKind::kInt64:
    case ClothValueKind::kUint64:
    case ClothValueKind::kFloat64:
      return 8;
    case ClothValueKind::kString:
    case ClothValueKind::kReference:
      return sizeof(void*);
    case ClothValueKind::kStruct:
    case ClothValueKind::kNullable:
      return 0;
  }
  return 0;
}

void validate_value_layout(const ClothValueLayout* layout,
                           std::size_t depth = 0) noexcept {
  if (layout == nullptr || depth > 128 || layout->name == nullptr ||
      layout->name_size == 0 || layout->size == 0 ||
      !is_power_of_two(layout->alignment) ||
      layout->size % layout->alignment != 0 ||
      (layout->fields == nullptr) != (layout->field_count == 0) ||
      (layout->enum_cases == nullptr) != (layout->enum_case_count == 0)) {
    runtime_failure("value layout metadata is invalid");
  }
  const std::uint64_t scalar_size = scalar_value_size(layout->kind);
  if (scalar_size != 0 &&
      (layout->size != scalar_size || layout->field_count != 0)) {
    runtime_failure("scalar value layout has invalid storage");
  }
  if (layout->kind == ClothValueKind::kEnum) {
    if (layout->enum_case_count == 0) {
      runtime_failure("enum value layout has no cases");
    }
    for (std::uint64_t index = 0; index < layout->enum_case_count; ++index) {
      const ClothEnumCaseLayout& item = layout->enum_cases[index];
      if (item.name == nullptr || item.name_size == 0) {
        runtime_failure("enum case layout is invalid");
      }
    }
  } else if (layout->enum_case_count != 0) {
    runtime_failure("non-enum value layout has enum cases");
  }
  if (layout->kind == ClothValueKind::kNullable && layout->field_count != 1) {
    runtime_failure("nullable value layout has the wrong payload count");
  }
  if (layout->kind != ClothValueKind::kStruct &&
      layout->kind != ClothValueKind::kNullable && layout->field_count != 0) {
    runtime_failure("non-aggregate value layout has fields");
  }
  std::uint64_t previous_end =
      layout->kind == ClothValueKind::kNullable ? 1 : 0;
  for (std::uint64_t index = 0; index < layout->field_count; ++index) {
    const ClothValueFieldLayout& field = layout->fields[index];
    validate_value_layout(field.type, depth + 1);
    if (field.offset < previous_end ||
        field.offset % field.type->alignment != 0 ||
        field.offset > layout->size ||
        field.type->size > layout->size - field.offset ||
        (layout->kind == ClothValueKind::kStruct &&
         (field.name == nullptr || field.name_size == 0)) ||
        (layout->kind == ClothValueKind::kNullable &&
         (field.name != nullptr || field.name_size != 0))) {
      runtime_failure("aggregate value field layout is invalid");
    }
    previous_end = field.offset + field.type->size;
  }
}

bool is_object_root_descriptor(const ClothTypeDescriptor* type) noexcept {
  return type == &cloth_rt_object_type ||
         (type != nullptr && type->kind == ClothHeapObjectKind::kFileClass &&
          type->name != nullptr &&
          type->name_size == sizeof(kObjectTypeName) - 1 &&
          std::memcmp(type->name, kObjectTypeName,
                      sizeof(kObjectTypeName) - 1) == 0);
}

void validate_type_descriptor(const ClothTypeDescriptor* type) noexcept {
  if (type == nullptr) {
    runtime_failure("object type descriptor is null");
  }
  if (!is_heap_object_kind(type->kind)) {
    runtime_failure("object type descriptor has the wrong kind");
  }
  if (type->parent != nullptr &&
      (type->parent == type ||
       (type->parent->kind != type->kind &&
        !is_object_root_descriptor(type->parent)) ||
       type->parent->size > type->size ||
       type->parent->alignment > type->alignment ||
       (type->parent->virtual_function_count > type->virtual_function_count &&
        !(type->parent == &cloth_rt_error_type &&
          type->virtual_function_count < 3)))) {
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
  if ((type->boxed_value_layout == nullptr) !=
      (type->boxed_value_offset == 0)) {
    runtime_failure("object boxed-value metadata is inconsistent");
  }
  if (type->boxed_value_layout != nullptr) {
    validate_value_layout(type->boxed_value_layout);
    if (type->boxed_value_offset < sizeof(ClothObjectHeader) ||
        type->boxed_value_offset % type->boxed_value_layout->alignment != 0 ||
        type->boxed_value_offset > type->size ||
        type->boxed_value_layout->size >
            type->size - type->boxed_value_offset) {
      runtime_failure("object boxed-value payload is out of bounds");
    }
  } else if (type->kind == ClothHeapObjectKind::kValueBox) {
    runtime_failure("value-box descriptor has no payload metadata");
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
      case ClothHeapObjectKind::kError:
      case ClothHeapObjectKind::kValueBox:
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
        if (array.element->reference_count == 0) {
          break;
        }
        for (std::size_t index = 0; index < array.length; ++index) {
          for (std::uint64_t slot = 0; slot < array.element->reference_count;
               ++slot) {
            void* reference = nullptr;
            std::memcpy(&reference,
                        static_cast<const std::byte*>(array.data) +
                            index * array.element_size +
                            array.element->reference_offsets[slot],
                        sizeof(reference));
            enqueue_reference(reference, worklist);
          }
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
    case ClothHeapObjectKind::kError:
    case ClothHeapObjectKind::kValueBox:
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
  if (next_allocation_identity == std::numeric_limits<std::uint64_t>::max()) {
    free_aligned(object);
    runtime_failure("managed allocation identity overflow");
  }
  reserve_allocation_index(live_object_count + 1);
  auto* allocation =
      static_cast<ClothAllocation*>(std::malloc(sizeof(ClothAllocation)));
  if (allocation == nullptr) {
    free_aligned(object);
    runtime_failure("managed allocation registry failed");
  }
  *allocation = ClothAllocation{
      allocations, nullptr, object, size, next_allocation_identity++, false};
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

const ClothString& require_parse_text(const void* value) noexcept {
  const ClothString& text = require_string(value);
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  std::size_t scalar_count = 0;
  if (text.byte_size > kMaximumStringSize ||
      text.scalar_count > kMaximumStringSize ||
      (text.data == nullptr && text.byte_size != 0) ||
      !try_count_utf8_scalars(text.data, text.byte_size, scalar_count) ||
      scalar_count != text.scalar_count) {
    runtime_failure("primitive parse text has an invalid layout");
  }
  return text;
}

bool has_valid_string_layout(const ClothString& string) noexcept {
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  std::size_t scalar_count = 0;
  return string.byte_size <= kMaximumStringSize &&
         string.scalar_count <= kMaximumStringSize &&
         (string.data != nullptr || string.byte_size == 0) &&
         try_count_utf8_scalars(string.data, string.byte_size, scalar_count) &&
         scalar_count == string.scalar_count;
}

const ClothString& require_traversable_string(const void* value) noexcept {
  const ClothString& string = require_string(value);
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (string.byte_size > kMaximumStringSize ||
      string.scalar_count > kMaximumStringSize ||
      (string.data == nullptr && string.byte_size != 0)) {
    runtime_failure("string has an invalid layout");
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
      array.element->reference_count != 0) {
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

ClothString* allocate_owned_string(const char* data, std::size_t byte_size,
                                   std::size_t scalar_count) noexcept {
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (byte_size > kMaximumStringSize || scalar_count > kMaximumStringSize) {
    runtime_failure("program argument is too large");
  }
  const std::uint64_t managed_size = sizeof(ClothString) + byte_size;
  collect_before_allocation(managed_size);
  auto* string = static_cast<ClothString*>(allocate_aligned(
      sizeof(ClothString), alignof(ClothString), "string allocation failed"));
  auto* owned_data = static_cast<char*>(allocate_aligned(
      byte_size, alignof(char), "string payload allocation failed"));
  if (byte_size != 0) {
    std::memcpy(owned_data, data, byte_size);
  }
  *string = ClothString{{&kStringTypeDescriptor, nullptr},
                        owned_data,
                        byte_size,
                        scalar_count,
                        true};
  register_allocation(&string->header, managed_size);
  return string;
}

#if !defined(_WIN32)

std::size_t program_argument_size(const char* value) noexcept {
  if (value == nullptr) {
    runtime_failure("program argument vector contains a null value");
  }
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  std::size_t size = 0;
  while (value[size] != '\0') {
    if (size == kMaximumStringSize) {
      runtime_failure("program argument is too large");
    }
    ++size;
  }
  return size;
}

#endif

#if defined(_WIN32)

struct Utf16ArgumentSize {
  std::size_t units;
  std::size_t bytes;
  std::size_t scalars;
};

Utf16ArgumentSize measure_utf16_argument(const wchar_t* value) noexcept {
  if (value == nullptr) {
    runtime_failure("program argument vector contains a null value");
  }
  constexpr std::size_t kMaximumStringSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  Utf16ArgumentSize result{};
  while (value[result.units] != L'\0') {
    std::uint32_t scalar = static_cast<std::uint16_t>(value[result.units]);
    std::size_t consumed = 1;
    if (scalar >= 0xd800U && scalar <= 0xdbffU) {
      const std::uint32_t low =
          static_cast<std::uint16_t>(value[result.units + 1]);
      if (low < 0xdc00U || low > 0xdfffU) {
        runtime_failure("program argument is not valid Unicode");
      }
      scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
      consumed = 2;
    } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
      runtime_failure("program argument is not valid Unicode");
    }
    const std::size_t width = scalar <= 0x7fU     ? 1
                              : scalar <= 0x7ffU  ? 2
                              : scalar <= 0xffffU ? 3
                                                  : 4;
    if (result.bytes > kMaximumStringSize - width ||
        result.scalars == kMaximumStringSize ||
        result.units > kMaximumStringSize - consumed) {
      runtime_failure("program argument is too large");
    }
    result.units += consumed;
    result.bytes += width;
    ++result.scalars;
  }
  return result;
}

void encode_utf16_argument(const wchar_t* value, std::size_t units,
                           char* output) noexcept {
  std::size_t input_index = 0;
  std::size_t output_index = 0;
  while (input_index < units) {
    std::uint32_t scalar = static_cast<std::uint16_t>(value[input_index++]);
    if (scalar >= 0xd800U && scalar <= 0xdbffU) {
      const std::uint32_t low =
          static_cast<std::uint16_t>(value[input_index++]);
      scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
    }
    if (scalar <= 0x7fU) {
      output[output_index++] = static_cast<char>(scalar);
    } else if (scalar <= 0x7ffU) {
      output[output_index++] = static_cast<char>(0xc0U | (scalar >> 6U));
      output[output_index++] = static_cast<char>(0x80U | (scalar & 0x3fU));
    } else if (scalar <= 0xffffU) {
      output[output_index++] = static_cast<char>(0xe0U | (scalar >> 12U));
      output[output_index++] =
          static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
      output[output_index++] = static_cast<char>(0x80U | (scalar & 0x3fU));
    } else {
      output[output_index++] = static_cast<char>(0xf0U | (scalar >> 18U));
      output[output_index++] =
          static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU));
      output[output_index++] =
          static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
      output[output_index++] = static_cast<char>(0x80U | (scalar & 0x3fU));
    }
  }
}

#endif

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

template <typename Value>
class NativeBuffer {
 public:
  NativeBuffer() = default;
  NativeBuffer(const NativeBuffer&) = delete;
  NativeBuffer& operator=(const NativeBuffer&) = delete;
  ~NativeBuffer() { std::free(data_); }

  [[nodiscard]] bool push_back(Value value) noexcept {
    constexpr std::size_t kMaximumSize =
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    if (size_ == kMaximumSize) {
      return false;
    }
    if (size_ == capacity_) {
      std::size_t capacity = capacity_ == 0 ? 256 : capacity_;
      if (capacity > kMaximumSize / 2) {
        capacity = kMaximumSize;
      } else {
        capacity *= 2;
      }
      if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
        runtime_failure("native input buffer is too large");
      }
      void* storage = std::realloc(data_, capacity * sizeof(Value));
      if (storage == nullptr) {
        runtime_failure("native input buffer allocation failed");
      }
      data_ = static_cast<Value*>(storage);
      capacity_ = capacity;
    }
    data_[size_++] = value;
    return true;
  }

  void pop_back() noexcept {
    if (size_ != 0) {
      --size_;
    }
  }

  [[nodiscard]] Value* data() noexcept { return data_; }
  [[nodiscard]] const Value* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] Value back() const noexcept { return data_[size_ - 1]; }

 private:
  Value* data_{nullptr};
  std::size_t size_{0};
  std::size_t capacity_{0};
};

template <typename Value>
class NativeAllocation {
 public:
  NativeAllocation(std::size_t count, std::string_view failure) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
      runtime_failure(failure);
    }
    const std::size_t bytes = std::max<std::size_t>(count * sizeof(Value), 1);
    data_ = static_cast<Value*>(std::malloc(bytes));
    if (data_ == nullptr) {
      runtime_failure(failure);
    }
  }
  NativeAllocation(const NativeAllocation&) = delete;
  NativeAllocation& operator=(const NativeAllocation&) = delete;
  ~NativeAllocation() { std::free(data_); }

  [[nodiscard]] Value* data() noexcept { return data_; }
  [[nodiscard]] const Value* data() const noexcept { return data_; }

 private:
  Value* data_{nullptr};
};

void* allocate_file_bytes(const std::byte* bytes, std::size_t size) noexcept {
  auto* result = static_cast<ClothArray*>(cloth_rt_array_alloc(
      static_cast<std::int32_t>(size), &kByteElementLayout));
  if (size != 0) {
    std::memcpy(result->data, bytes, size);
  }
  return result;
}

const ClothString& require_file_path(const void* value) noexcept {
  const ClothString& path = require_string(value);
  if (!has_valid_string_layout(path)) {
    runtime_failure("file path string has an invalid layout");
  }
  return path;
}

bool has_native_path_terminator(const ClothString& path) noexcept {
  return path.byte_size != 0 &&
         std::memchr(path.data, '\0', path.byte_size) != nullptr;
}

#if defined(_WIN32)

class FileHandle {
 public:
  explicit FileHandle(HANDLE value) noexcept : value_(value) {}
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  ~FileHandle() {
    if (value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(CloseHandle(value_));
    }
  }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }

 private:
  HANDLE value_{INVALID_HANDLE_VALUE};
};

void* read_file_bytes(const ClothString& path, std::uint8_t& status) noexcept {
  const int input_size = static_cast<int>(path.byte_size);
  int wide_size = 0;
  if (input_size != 0) {
    wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data,
                                    input_size, nullptr, 0);
    if (wide_size == 0) {
      runtime_failure("file path string has an invalid layout");
    }
  }
  NativeAllocation<wchar_t> native_path(static_cast<std::size_t>(wide_size) + 1,
                                        "native file path allocation failed");
  if (wide_size != 0 &&
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, input_size,
                          native_path.data(), wide_size) != wide_size) {
    runtime_failure("file path string has an invalid layout");
  }
  native_path.data()[wide_size] = L'\0';

  FileHandle file{
      CreateFileW(native_path.data(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  if (file.get() == INVALID_HANDLE_VALUE) {
    status = kClothFileReadOpenError;
    return nullptr;
  }
  if (GetFileType(file.get()) != FILE_TYPE_DISK) {
    status = kClothFileReadNotRegular;
    return nullptr;
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (GetFileInformationByHandle(file.get(), &information) == 0) {
    status = kClothFileReadIoError;
    return nullptr;
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    status = kClothFileReadNotRegular;
    return nullptr;
  }
  LARGE_INTEGER length{};
  if (GetFileSizeEx(file.get(), &length) == 0 || length.QuadPart < 0) {
    status = kClothFileReadIoError;
    return nullptr;
  }
  if (static_cast<std::uint64_t>(length.QuadPart) > kMaximumFileByteCount) {
    status = kClothFileReadTooLarge;
    return nullptr;
  }

  const std::size_t size = static_cast<std::size_t>(length.QuadPart);
  NativeAllocation<std::byte> bytes(size,
                                    "native file buffer allocation failed");
  std::size_t offset = 0;
  while (offset < size) {
    const DWORD request =
        static_cast<DWORD>(std::min<std::size_t>(size - offset, 1024U * 1024U));
    DWORD received = 0;
    if (ReadFile(file.get(), bytes.data() + offset, request, &received,
                 nullptr) == 0 ||
        received == 0) {
      status = kClothFileReadIoError;
      return nullptr;
    }
    offset += received;
  }
  std::byte extra{};
  DWORD received = 0;
  if (ReadFile(file.get(), &extra, 1, &received, nullptr) == 0) {
    status = kClothFileReadIoError;
    return nullptr;
  }
  if (received != 0) {
    LARGE_INTEGER current_length{};
    status = GetFileSizeEx(file.get(), &current_length) != 0 &&
                     current_length.QuadPart >= 0 &&
                     static_cast<std::uint64_t>(current_length.QuadPart) >
                         kMaximumFileByteCount
                 ? kClothFileReadTooLarge
                 : kClothFileReadIoError;
    return nullptr;
  }
  status = kClothFileReadValue;
  return allocate_file_bytes(bytes.data(), size);
}

#else

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) noexcept : value_(value) {}
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(close(value_));
    }
  }

  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  int value_{-1};
};

void* read_file_bytes(const ClothString& path, std::uint8_t& status) noexcept {
  NativeAllocation<char> native_path(path.byte_size + 1,
                                     "native file path allocation failed");
  if (path.byte_size != 0) {
    std::memcpy(native_path.data(), path.data, path.byte_size);
  }
  native_path.data()[path.byte_size] = '\0';

  int flags = O_RDONLY | O_NONBLOCK;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  FileDescriptor file{open(native_path.data(), flags)};
  if (file.get() < 0) {
    status = kClothFileReadOpenError;
    return nullptr;
  }
  struct stat information{};
  if (fstat(file.get(), &information) != 0) {
    status = kClothFileReadIoError;
    return nullptr;
  }
  if (!S_ISREG(information.st_mode)) {
    status = kClothFileReadNotRegular;
    return nullptr;
  }
  if (information.st_size < 0) {
    status = kClothFileReadIoError;
    return nullptr;
  }
  if (static_cast<std::uint64_t>(information.st_size) > kMaximumFileByteCount) {
    status = kClothFileReadTooLarge;
    return nullptr;
  }

  const std::size_t size = static_cast<std::size_t>(information.st_size);
  NativeAllocation<std::byte> bytes(size,
                                    "native file buffer allocation failed");
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t received =
        read(file.get(), bytes.data() + offset,
             std::min<std::size_t>(size - offset, 1024U * 1024U));
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      status = kClothFileReadIoError;
      return nullptr;
    }
    offset += static_cast<std::size_t>(received);
  }
  std::byte extra{};
  ssize_t received = 0;
  do {
    received = read(file.get(), &extra, 1);
  } while (received < 0 && errno == EINTR);
  if (received < 0) {
    status = kClothFileReadIoError;
    return nullptr;
  }
  if (received != 0) {
    struct stat current_information{};
    status = fstat(file.get(), &current_information) == 0 &&
                     current_information.st_size >= 0 &&
                     static_cast<std::uint64_t>(current_information.st_size) >
                         kMaximumFileByteCount
                 ? kClothFileReadTooLarge
                 : kClothFileReadIoError;
    return nullptr;
  }
  status = kClothFileReadValue;
  return allocate_file_bytes(bytes.data(), size);
}

#endif

struct StreamInputState {
  std::array<char, 4096> bytes{};
  std::size_t next{0};
  std::size_t size{0};
  bool eof{false};
};

StreamInputState stream_input;

bool configure_stdin() noexcept {
#if defined(_WIN32)
  static const bool configured = _setmode(_fileno(stdin), _O_BINARY) != -1;
  return configured;
#else
  return true;
#endif
}

bool read_stream_byte(char& value, std::uint8_t& status) noexcept {
  if (stream_input.next < stream_input.size) {
    value = stream_input.bytes[stream_input.next++];
    return true;
  }
  if (stream_input.eof) {
    status = kClothConsoleInputEof;
    return false;
  }
  if (!configure_stdin()) {
    status = kClothConsoleInputIoError;
    return false;
  }

  stream_input.size = std::fread(stream_input.bytes.data(), 1,
                                 stream_input.bytes.size(), stdin);
  stream_input.next = 0;
  if (stream_input.size != 0) {
    value = stream_input.bytes[stream_input.next++];
    return true;
  }
  if (std::ferror(stdin) != 0) {
    status = kClothConsoleInputIoError;
    return false;
  }
  stream_input.eof = true;
  status = kClothConsoleInputEof;
  return false;
}

void* read_stream_line(std::uint8_t& status) noexcept {
  NativeBuffer<char> line;
  for (;;) {
    char byte = 0;
    if (!read_stream_byte(byte, status)) {
      if (status != kClothConsoleInputEof || line.empty()) {
        return nullptr;
      }
      break;
    }
    if (byte == '\n') {
      break;
    }
    if (!line.push_back(byte)) {
      status = kClothConsoleInputLineTooLarge;
      return nullptr;
    }
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  std::size_t scalar_count = 0;
  if (!try_count_utf8_scalars(line.data(), line.size(), scalar_count)) {
    status = kClothConsoleInputEncodingError;
    return nullptr;
  }
  status = kClothConsoleInputValue;
  return allocate_owned_string(line.data(), line.size(), scalar_count);
}

#if defined(_WIN32)

struct ConsoleInputState {
  std::array<wchar_t, 256> units{};
  std::size_t next{0};
  std::size_t size{0};
  bool eof{false};
};

ConsoleInputState console_input;

bool has_console_input() noexcept {
  DWORD mode = 0;
  const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
  return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
         GetConsoleMode(handle, &mode) != 0;
}

bool read_console_unit(wchar_t& value, std::uint8_t& status) noexcept {
  if (console_input.next < console_input.size) {
    value = console_input.units[console_input.next++];
    return true;
  }
  if (console_input.eof) {
    status = kClothConsoleInputEof;
    return false;
  }
  DWORD size = 0;
  const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      ReadConsoleW(handle, console_input.units.data(),
                   static_cast<DWORD>(console_input.units.size()), &size,
                   nullptr) == 0) {
    status = kClothConsoleInputIoError;
    return false;
  }
  console_input.next = 0;
  console_input.size = size;
  if (size == 0) {
    console_input.eof = true;
    status = kClothConsoleInputEof;
    return false;
  }
  value = console_input.units[console_input.next++];
  return true;
}

std::uint8_t measure_utf16_line(const wchar_t* units, std::size_t size,
                                std::size_t& bytes,
                                std::size_t& scalars) noexcept {
  constexpr std::size_t kMaximumSize =
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  bytes = 0;
  scalars = 0;
  for (std::size_t index = 0; index < size;) {
    std::uint32_t scalar = static_cast<std::uint16_t>(units[index++]);
    if (scalar >= 0xd800U && scalar <= 0xdbffU) {
      if (index == size) {
        return kClothConsoleInputEncodingError;
      }
      const std::uint32_t low = static_cast<std::uint16_t>(units[index++]);
      if (low < 0xdc00U || low > 0xdfffU) {
        return kClothConsoleInputEncodingError;
      }
      scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
    } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
      return kClothConsoleInputEncodingError;
    }
    const std::size_t width = scalar <= 0x7fU     ? 1
                              : scalar <= 0x7ffU  ? 2
                              : scalar <= 0xffffU ? 3
                                                  : 4;
    if (bytes > kMaximumSize - width || scalars == kMaximumSize) {
      return kClothConsoleInputLineTooLarge;
    }
    bytes += width;
    ++scalars;
  }
  return kClothConsoleInputValue;
}

void append_utf8_scalar(std::uint32_t scalar, char* output,
                        std::size_t& index) noexcept {
  if (scalar <= 0x7fU) {
    output[index++] = static_cast<char>(scalar);
  } else if (scalar <= 0x7ffU) {
    output[index++] = static_cast<char>(0xc0U | (scalar >> 6U));
    output[index++] = static_cast<char>(0x80U | (scalar & 0x3fU));
  } else if (scalar <= 0xffffU) {
    output[index++] = static_cast<char>(0xe0U | (scalar >> 12U));
    output[index++] = static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
    output[index++] = static_cast<char>(0x80U | (scalar & 0x3fU));
  } else {
    output[index++] = static_cast<char>(0xf0U | (scalar >> 18U));
    output[index++] = static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU));
    output[index++] = static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
    output[index++] = static_cast<char>(0x80U | (scalar & 0x3fU));
  }
}

void encode_utf16_line(const wchar_t* units, std::size_t size,
                       char* output) noexcept {
  std::size_t output_index = 0;
  for (std::size_t index = 0; index < size;) {
    std::uint32_t scalar = static_cast<std::uint16_t>(units[index++]);
    if (scalar >= 0xd800U && scalar <= 0xdbffU) {
      const std::uint32_t low = static_cast<std::uint16_t>(units[index++]);
      scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
    }
    append_utf8_scalar(scalar, output, output_index);
  }
}

void* read_console_line(std::uint8_t& status) noexcept {
  NativeBuffer<wchar_t> line;
  for (;;) {
    wchar_t unit = 0;
    if (!read_console_unit(unit, status)) {
      if (status != kClothConsoleInputEof || line.empty()) {
        return nullptr;
      }
      break;
    }
    if (unit == L'\n') {
      break;
    }
    if (!line.push_back(unit)) {
      status = kClothConsoleInputLineTooLarge;
      return nullptr;
    }
  }
  if (!line.empty() && line.back() == L'\r') {
    line.pop_back();
  }
  std::size_t byte_size = 0;
  std::size_t scalar_count = 0;
  status =
      measure_utf16_line(line.data(), line.size(), byte_size, scalar_count);
  if (status != kClothConsoleInputValue) {
    return nullptr;
  }
  auto* bytes = static_cast<char*>(allocate_aligned(
      byte_size, alignof(char), "standard input conversion failed"));
  encode_utf16_line(line.data(), line.size(), bytes);
  void* result = allocate_owned_string(bytes, byte_size, scalar_count);
  free_aligned(bytes);
  return result;
}

#endif

int digit_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

struct IntegerParseType {
  std::uint8_t width;
  bool is_signed;
};

bool integer_parse_type(std::uint8_t kind, IntegerParseType& type) noexcept {
  switch (kind) {
    case kClothParseByte:
    case kClothParseUint8:
      type = IntegerParseType{8, false};
      return true;
    case kClothParseInt8:
      type = IntegerParseType{8, true};
      return true;
    case kClothParseInt16:
      type = IntegerParseType{16, true};
      return true;
    case kClothParseInt32:
      type = IntegerParseType{32, true};
      return true;
    case kClothParseInt64:
      type = IntegerParseType{64, true};
      return true;
    case kClothParseUint16:
      type = IntegerParseType{16, false};
      return true;
    case kClothParseUint32:
      type = IntegerParseType{32, false};
      return true;
    case kClothParseUint64:
      type = IntegerParseType{64, false};
      return true;
    default:
      return false;
  }
}

std::uint8_t parse_integer(const ClothString& text, IntegerParseType type,
                           std::uint64_t& bits) noexcept {
  std::size_t index = 0;
  bool negative = false;
  if (index < text.byte_size &&
      (text.data[index] == '+' || text.data[index] == '-')) {
    negative = text.data[index] == '-';
    ++index;
  }
  if (negative && !type.is_signed) {
    return kClothParseInvalid;
  }

  unsigned radix = 10;
  if (index + 1 < text.byte_size && text.data[index] == '0') {
    switch (text.data[index + 1]) {
      case 'b':
        radix = 2;
        break;
      case 'o':
        radix = 8;
        break;
      case 'x':
        radix = 16;
        break;
      default:
        break;
    }
    if (radix != 10) {
      index += 2;
    }
  }

  const std::uint64_t unsigned_limit =
      type.width == 64 ? std::numeric_limits<std::uint64_t>::max()
                       : (UINT64_C(1) << type.width) - 1;
  const std::uint64_t signed_maximum =
      type.width == 64 ? static_cast<std::uint64_t>(INT64_MAX)
                       : (UINT64_C(1) << (type.width - 1)) - 1;
  const std::uint64_t limit =
      type.is_signed ? signed_maximum + static_cast<std::uint64_t>(negative)
                     : unsigned_limit;
  std::uint64_t magnitude = 0;
  bool has_digit = false;
  bool out_of_range = false;
  while (index < text.byte_size) {
    const int digit = digit_value(text.data[index]);
    if (digit >= 0 && static_cast<unsigned>(digit) < radix) {
      has_digit = true;
      if (!out_of_range) {
        const auto value = static_cast<std::uint64_t>(digit);
        if (magnitude > (limit - value) / radix) {
          out_of_range = true;
        } else {
          magnitude = magnitude * radix + value;
        }
      }
      ++index;
      continue;
    }
    if (text.data[index] != '_' || !has_digit || index + 1 == text.byte_size) {
      return kClothParseInvalid;
    }
    const int next = digit_value(text.data[index + 1]);
    if (next < 0 || static_cast<unsigned>(next) >= radix) {
      return kClothParseInvalid;
    }
    ++index;
  }
  if (!has_digit) {
    return kClothParseInvalid;
  }
  if (out_of_range) {
    return kClothParseOutOfRange;
  }
  bits = negative ? ((~magnitude) + 1) & unsigned_limit : magnitude;
  return kClothParseValue;
}

bool append_float_digits(const ClothString& text, std::size_t& index,
                         NativeBuffer<char>& normalized,
                         bool& nonzero) noexcept {
  bool has_digit = false;
  while (index < text.byte_size) {
    const char value = text.data[index];
    if (value >= '0' && value <= '9') {
      has_digit = true;
      nonzero = nonzero || value != '0';
      static_cast<void>(normalized.push_back(value));
      ++index;
      continue;
    }
    if (value != '_') {
      break;
    }
    if (!has_digit || index + 1 == text.byte_size ||
        text.data[index + 1] < '0' || text.data[index + 1] > '9') {
      return false;
    }
    ++index;
  }
  return has_digit;
}

template <typename Float>
std::uint8_t parse_float(const ClothString& text,
                         std::uint64_t& bits) noexcept {
  NativeBuffer<char> normalized;
  std::size_t index = 0;
  bool negative = false;
  if (index < text.byte_size &&
      (text.data[index] == '+' || text.data[index] == '-')) {
    negative = text.data[index] == '-';
    if (negative) {
      static_cast<void>(normalized.push_back('-'));
    }
    ++index;
  }
  bool nonzero = false;
  if (!append_float_digits(text, index, normalized, nonzero)) {
    return kClothParseInvalid;
  }
  if (index < text.byte_size && text.data[index] == '.') {
    static_cast<void>(normalized.push_back('.'));
    ++index;
    if (!append_float_digits(text, index, normalized, nonzero)) {
      return kClothParseInvalid;
    }
  }
  if (index < text.byte_size &&
      (text.data[index] == 'e' || text.data[index] == 'E')) {
    static_cast<void>(normalized.push_back(text.data[index++]));
    if (index < text.byte_size &&
        (text.data[index] == '+' || text.data[index] == '-')) {
      static_cast<void>(normalized.push_back(text.data[index++]));
    }
    bool exponent_nonzero = false;
    if (!append_float_digits(text, index, normalized, exponent_nonzero)) {
      return kClothParseInvalid;
    }
  }
  if (index != text.byte_size) {
    return kClothParseInvalid;
  }
  if (!nonzero) {
    if constexpr (sizeof(Float) == sizeof(std::uint32_t)) {
      bits = negative ? UINT32_C(0x80000000) : 0;
    } else {
      bits = negative ? UINT64_C(0x8000000000000000) : 0;
    }
    return kClothParseValue;
  }

  Float value = 0;
  const auto parsed =
      std::from_chars(normalized.data(), normalized.data() + normalized.size(),
                      value, std::chars_format::general);
  if (parsed.ec == std::errc::result_out_of_range) {
    return kClothParseOutOfRange;
  }
  if (parsed.ec != std::errc{} ||
      parsed.ptr != normalized.data() + normalized.size()) {
    runtime_failure("floating-point parser rejected validated input");
  }
  if (!std::isfinite(value) || value == 0) {
    return kClothParseOutOfRange;
  }
  if constexpr (sizeof(Float) == sizeof(std::uint32_t)) {
    bits = std::bit_cast<std::uint32_t>(value);
  } else {
    bits = std::bit_cast<std::uint64_t>(value);
  }
  return kClothParseValue;
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

void configure_stderr() noexcept {
#if defined(_WIN32)
  static const bool configured = _setmode(_fileno(stderr), _O_BINARY) != -1;
  if (!configured) {
    runtime_failure("standard error mode configuration failed");
  }
#endif
}

void write_stderr(std::string_view text) noexcept {
  configure_stderr();
  if (!text.empty() &&
      std::fwrite(text.data(), 1, text.size(), stderr) != text.size()) {
    runtime_failure("standard error write failed");
  }
  if (std::fflush(stderr) != 0) {
    runtime_failure("standard error flush failed");
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

std::uint64_t mix_hash(std::uint64_t value) noexcept {
  value ^= value >> 30U;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27U;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31U);
}

std::uint64_t hash_bytes(const char* data, std::size_t size) noexcept {
  std::uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= static_cast<unsigned char>(data[index]);
    hash *= UINT64_C(0x100000001b3);
  }
  return mix_hash(hash);
}

std::uint64_t combine_hash(std::uint64_t left, std::uint64_t right) noexcept {
  return mix_hash(left ^ (right + UINT64_C(0x9e3779b97f4a7c15) + (left << 6U) +
                          (left >> 2U)));
}

template <typename Value>
Value load_value(const void* storage) noexcept {
  Value result{};
  std::memcpy(&result, storage, sizeof(result));
  return result;
}

bool values_equal(const ClothValueLayout& layout, const void* left,
                  const void* right, std::size_t depth = 0) noexcept {
  if (depth > 128) {
    runtime_failure("value equality exceeds the nesting limit");
  }
  switch (layout.kind) {
    case ClothValueKind::kBool:
    case ClothValueKind::kByte:
    case ClothValueKind::kUint8:
      return load_value<std::uint8_t>(left) == load_value<std::uint8_t>(right);
    case ClothValueKind::kInt8:
      return load_value<std::int8_t>(left) == load_value<std::int8_t>(right);
    case ClothValueKind::kInt16:
      return load_value<std::int16_t>(left) == load_value<std::int16_t>(right);
    case ClothValueKind::kInt32:
      return load_value<std::int32_t>(left) == load_value<std::int32_t>(right);
    case ClothValueKind::kInt64:
      return load_value<std::int64_t>(left) == load_value<std::int64_t>(right);
    case ClothValueKind::kChar:
    case ClothValueKind::kUint32:
    case ClothValueKind::kEnum:
      return load_value<std::uint32_t>(left) ==
             load_value<std::uint32_t>(right);
    case ClothValueKind::kUint16:
      return load_value<std::uint16_t>(left) ==
             load_value<std::uint16_t>(right);
    case ClothValueKind::kUint64:
      return load_value<std::uint64_t>(left) ==
             load_value<std::uint64_t>(right);
    case ClothValueKind::kFloat32:
      return load_value<float>(left) == load_value<float>(right);
    case ClothValueKind::kFloat64:
      return load_value<double>(left) == load_value<double>(right);
    case ClothValueKind::kString:
      return cloth_rt_string_equal(load_value<const void*>(left),
                                   load_value<const void*>(right)) != 0;
    case ClothValueKind::kReference:
      return load_value<const void*>(left) == load_value<const void*>(right);
    case ClothValueKind::kStruct:
      for (std::uint64_t index = 0; index < layout.field_count; ++index) {
        const ClothValueFieldLayout& field = layout.fields[index];
        if (!values_equal(*field.type,
                          static_cast<const std::byte*>(left) + field.offset,
                          static_cast<const std::byte*>(right) + field.offset,
                          depth + 1)) {
          return false;
        }
      }
      return true;
    case ClothValueKind::kNullable: {
      const std::uint8_t left_tag = load_value<std::uint8_t>(left);
      const std::uint8_t right_tag = load_value<std::uint8_t>(right);
      if (left_tag > 1 || right_tag > 1) {
        runtime_failure("nullable value has an invalid tag");
      }
      if (left_tag != right_tag || left_tag == 0) {
        return left_tag == right_tag;
      }
      const ClothValueFieldLayout& payload = layout.fields[0];
      return values_equal(
          *payload.type, static_cast<const std::byte*>(left) + payload.offset,
          static_cast<const std::byte*>(right) + payload.offset, depth + 1);
    }
  }
  runtime_failure("value equality has an unknown kind");
}

std::uint64_t value_hash(const ClothValueLayout& layout, const void* value,
                         std::size_t depth = 0) noexcept {
  if (depth > 128) {
    runtime_failure("value hashing exceeds the nesting limit");
  }
  switch (layout.kind) {
    case ClothValueKind::kBool:
    case ClothValueKind::kByte:
    case ClothValueKind::kUint8:
      return mix_hash(load_value<std::uint8_t>(value));
    case ClothValueKind::kInt8:
      return mix_hash(static_cast<std::uint64_t>(
          static_cast<std::int64_t>(load_value<std::int8_t>(value))));
    case ClothValueKind::kInt16:
      return mix_hash(static_cast<std::uint64_t>(
          static_cast<std::int64_t>(load_value<std::int16_t>(value))));
    case ClothValueKind::kInt32:
      return mix_hash(static_cast<std::uint64_t>(
          static_cast<std::int64_t>(load_value<std::int32_t>(value))));
    case ClothValueKind::kInt64:
      return mix_hash(
          static_cast<std::uint64_t>(load_value<std::int64_t>(value)));
    case ClothValueKind::kChar:
    case ClothValueKind::kUint32:
    case ClothValueKind::kEnum:
      return mix_hash(load_value<std::uint32_t>(value));
    case ClothValueKind::kUint16:
      return mix_hash(load_value<std::uint16_t>(value));
    case ClothValueKind::kUint64:
      return mix_hash(load_value<std::uint64_t>(value));
    case ClothValueKind::kFloat32: {
      const float number = load_value<float>(value);
      return mix_hash(number == 0.0F ? 0
                                     : std::bit_cast<std::uint32_t>(number));
    }
    case ClothValueKind::kFloat64: {
      const double number = load_value<double>(value);
      return mix_hash(number == 0.0 ? 0 : std::bit_cast<std::uint64_t>(number));
    }
    case ClothValueKind::kString: {
      const void* string = load_value<const void*>(value);
      return string == nullptr ? 0 : string_hash_code(string);
    }
    case ClothValueKind::kReference: {
      const void* reference = load_value<const void*>(value);
      if (reference == nullptr) {
        return 0;
      }
      const ClothObjectHeader& object = require_object(reference);
      ClothAllocation* allocation = find_allocation(reference);
      if (allocation == nullptr || object.runtime_state != allocation) {
        runtime_failure("referenced value is not a live managed allocation");
      }
      return mix_hash(allocation->identity);
    }
    case ClothValueKind::kStruct: {
      std::uint64_t result = hash_bytes(layout.name, layout.name_size);
      for (std::uint64_t index = 0; index < layout.field_count; ++index) {
        const ClothValueFieldLayout& field = layout.fields[index];
        result = combine_hash(
            result,
            value_hash(*field.type,
                       static_cast<const std::byte*>(value) + field.offset,
                       depth + 1));
      }
      return result;
    }
    case ClothValueKind::kNullable: {
      const std::uint8_t tag = load_value<std::uint8_t>(value);
      if (tag > 1) {
        runtime_failure("nullable value has an invalid tag");
      }
      if (tag == 0) {
        return mix_hash(0);
      }
      const ClothValueFieldLayout& payload = layout.fields[0];
      return combine_hash(
          mix_hash(1),
          value_hash(*payload.type,
                     static_cast<const std::byte*>(value) + payload.offset,
                     depth + 1));
    }
  }
  runtime_failure("value hashing has an unknown kind");
}

void append_bytes(NativeBuffer<char>& output, std::string_view text) noexcept {
  for (const char byte : text) {
    if (!output.push_back(byte)) {
      runtime_failure("value string representation is too large");
    }
  }
}

template <typename Value>
void append_number(NativeBuffer<char>& output, Value value) noexcept {
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    runtime_failure("value formatting failed");
  }
  append_bytes(output, std::string_view{buffer.data(), result.ptr});
}

template <typename Value>
void append_float(NativeBuffer<char>& output, Value value) noexcept {
  if (std::isnan(value)) {
    append_bytes(output, "nan");
    return;
  }
  if (std::isinf(value)) {
    append_bytes(output, std::signbit(value) ? "-inf" : "inf");
    return;
  }
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (result.ec != std::errc{}) {
    runtime_failure("floating-point value formatting failed");
  }
  append_bytes(output, std::string_view{buffer.data(), result.ptr});
}

void append_value(NativeBuffer<char>& output, const ClothValueLayout& layout,
                  const void* value, std::size_t depth = 0) noexcept {
  if (depth > 128) {
    runtime_failure("value formatting exceeds the nesting limit");
  }
  switch (layout.kind) {
    case ClothValueKind::kBool:
      append_bytes(output,
                   load_value<std::uint8_t>(value) == 0 ? "false" : "true");
      return;
    case ClothValueKind::kChar: {
      const std::uint32_t scalar = load_value<std::uint32_t>(value);
      if (scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
        runtime_failure("character value is not a Unicode scalar");
      }
      std::array<char, 4> bytes{};
      std::size_t size = 0;
      append_utf8_scalar(scalar, bytes.data(), size);
      append_bytes(output, std::string_view{bytes.data(), size});
      return;
    }
    case ClothValueKind::kByte:
    case ClothValueKind::kUint8:
      append_number(output, load_value<std::uint8_t>(value));
      return;
    case ClothValueKind::kInt8:
      append_number(output, load_value<std::int8_t>(value));
      return;
    case ClothValueKind::kInt16:
      append_number(output, load_value<std::int16_t>(value));
      return;
    case ClothValueKind::kInt32:
      append_number(output, load_value<std::int32_t>(value));
      return;
    case ClothValueKind::kInt64:
      append_number(output, load_value<std::int64_t>(value));
      return;
    case ClothValueKind::kUint16:
      append_number(output, load_value<std::uint16_t>(value));
      return;
    case ClothValueKind::kUint32:
      append_number(output, load_value<std::uint32_t>(value));
      return;
    case ClothValueKind::kUint64:
      append_number(output, load_value<std::uint64_t>(value));
      return;
    case ClothValueKind::kFloat32:
      append_float(output, load_value<float>(value));
      return;
    case ClothValueKind::kFloat64:
      append_float(output, load_value<double>(value));
      return;
    case ClothValueKind::kString: {
      const void* reference = load_value<const void*>(value);
      if (reference == nullptr) {
        append_bytes(output, "null");
        return;
      }
      const ClothString& string = require_traversable_string(reference);
      append_bytes(output, std::string_view{string.data, string.byte_size});
      return;
    }
    case ClothValueKind::kReference: {
      const void* reference = load_value<const void*>(value);
      if (reference == nullptr) {
        append_bytes(output, "null");
        return;
      }
      const ClothObjectHeader& object = require_object(reference);
      append_bytes(output, "<");
      append_bytes(output,
                   std::string_view{object.type->name,
                                    native_size(object.type->name_size,
                                                "object type name too large")});
      append_bytes(output, ">");
      return;
    }
    case ClothValueKind::kEnum: {
      const std::uint32_t tag = load_value<std::uint32_t>(value);
      if (tag >= layout.enum_case_count) {
        runtime_failure("enum value has an invalid tag");
      }
      append_bytes(
          output,
          std::string_view{layout.name, native_size(layout.name_size,
                                                    "enum name too large")});
      append_bytes(output, ".");
      const ClothEnumCaseLayout& item = layout.enum_cases[tag];
      append_bytes(
          output,
          std::string_view{item.name, native_size(item.name_size,
                                                  "enum case name too large")});
      return;
    }
    case ClothValueKind::kStruct:
      append_bytes(output, std::string_view{layout.name, layout.name_size});
      append_bytes(output, "{");
      for (std::uint64_t index = 0; index < layout.field_count; ++index) {
        if (index != 0) {
          append_bytes(output, ", ");
        }
        const ClothValueFieldLayout& field = layout.fields[index];
        append_bytes(output, std::string_view{field.name, field.name_size});
        append_bytes(output, "=");
        append_value(output, *field.type,
                     static_cast<const std::byte*>(value) + field.offset,
                     depth + 1);
      }
      append_bytes(output, "}");
      return;
    case ClothValueKind::kNullable: {
      const std::uint8_t tag = load_value<std::uint8_t>(value);
      if (tag > 1) {
        runtime_failure("nullable value has an invalid tag");
      }
      if (tag == 0) {
        append_bytes(output, "null");
        return;
      }
      const ClothValueFieldLayout& payload = layout.fields[0];
      append_value(output, *payload.type,
                   static_cast<const std::byte*>(value) + payload.offset,
                   depth + 1);
      return;
    }
  }
  runtime_failure("value formatting has an unknown kind");
}

const ClothObjectHeader& require_value_box(const void* value) noexcept {
  const ClothObjectHeader& object = require_object(value);
  validate_type_descriptor(object.type);
  if (object.type->boxed_value_layout == nullptr) {
    runtime_failure("object has no boxed-value payload");
  }
  return object;
}

bool same_value_layout_identity(const ClothValueLayout& left,
                                const ClothValueLayout& right) noexcept {
  return left.kind == right.kind && left.size == right.size &&
         left.alignment == right.alignment &&
         left.name_size == right.name_size &&
         std::memcmp(left.name, right.name,
                     native_size(left.name_size,
                                 "value layout name is too large")) == 0;
}

bool string_equals(const void* value, const void* other) noexcept {
  const ClothString& left = require_traversable_string(value);
  if (other == nullptr) {
    return false;
  }
  const ClothObjectHeader& right_header = require_object(other);
  if (right_header.type != &kStringTypeDescriptor) {
    return false;
  }
  const ClothString& right = require_traversable_string(other);
  return left.byte_size == right.byte_size &&
         (left.byte_size == 0 ||
          std::memcmp(left.data, right.data, left.byte_size) == 0);
}

std::uint64_t string_hash_code(const void* value) noexcept {
  const ClothString& string = require_traversable_string(value);
  std::uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (std::size_t index = 0; index < string.byte_size; ++index) {
    hash ^= static_cast<unsigned char>(string.data[index]);
    hash *= UINT64_C(0x100000001b3);
  }
  return mix_hash(hash ^ UINT64_C(0x737472696e67));
}

void* string_to_string(const void* value) noexcept {
  static_cast<void>(require_traversable_string(value));
  return const_cast<void*>(value);
}

void* default_object_string(const ClothObjectHeader& object) noexcept {
  const char* name = object.type->name;
  std::size_t name_size =
      native_size(object.type->name_size, "object type name is too large");
  if (object.type->kind == ClothHeapObjectKind::kArray) {
    name = kArrayMetaTypeName;
    name_size = sizeof(kArrayMetaTypeName) - 1;
  }
  if (name == nullptr ||
      name_size > std::numeric_limits<std::size_t>::max() - 2) {
    runtime_failure("object type name has invalid storage");
  }
  const std::size_t scalar_count = count_utf8_scalars(name, name_size);
  char* display = static_cast<char*>(allocate_aligned(
      name_size + 2, alignof(char), "object string formatting failed"));
  display[0] = '<';
  if (name_size != 0) {
    std::memcpy(display + 1, name, name_size);
  }
  display[name_size + 1] = '>';
  void* result =
      allocate_owned_string(display, name_size + 2, scalar_count + 2);
  free_aligned(display);
  return result;
}

bool has_object_dispatch(const ClothTypeDescriptor* type) noexcept {
  if (type == nullptr || type->virtual_function_count < 3) {
    return false;
  }
  const ClothTypeDescriptor* current = type;
  const ClothTypeDescriptor* slow = type;
  const ClothTypeDescriptor* fast = type;
  while (current != nullptr) {
    validate_type_descriptor(current);
    if (current == &cloth_rt_object_type ||
        (current->kind == ClothHeapObjectKind::kFileClass &&
         current->name_size == sizeof(kObjectTypeName) - 1 &&
         std::memcmp(current->name, kObjectTypeName,
                     sizeof(kObjectTypeName) - 1) == 0)) {
      return true;
    }
    current = current->parent;
    if (slow != nullptr) {
      slow = slow->parent;
    }
    for (std::size_t step = 0; step < 2 && fast != nullptr; ++step) {
      fast = fast->parent;
    }
    if (slow != nullptr && slow == fast) {
      runtime_failure("object type descriptor ancestry contains a cycle");
    }
  }
  return false;
}

}  // namespace

extern "C" const ClothTypeDescriptor cloth_rt_object_type{
    ClothHeapObjectKind::kFileClass,
    nullptr,
    kObjectTypeName,
    sizeof(kObjectTypeName) - 1,
    sizeof(ClothObjectHeader),
    alignof(ClothObjectHeader),
    nullptr,
    0,
    kObjectVirtualFunctions,
    3,
    nullptr,
    0,
    nullptr,
    0};

extern "C" const ClothTypeDescriptor cloth_rt_error_type{
    ClothHeapObjectKind::kError,
    &cloth_rt_object_type,
    kErrorTypeName,
    sizeof(kErrorTypeName) - 1,
    sizeof(ClothError),
    alignof(ClothError),
    kErrorReferenceOffsets,
    1,
    kObjectVirtualFunctions,
    3,
    nullptr,
    0,
    nullptr,
    0};

extern "C" const ClothTypeDescriptor cloth_rt_division_by_zero_type{
    ClothHeapObjectKind::kError,
    &cloth_rt_error_type,
    kDivisionByZeroTypeName,
    sizeof(kDivisionByZeroTypeName) - 1,
    sizeof(ClothError),
    alignof(ClothError),
    kErrorReferenceOffsets,
    1,
    kObjectVirtualFunctions,
    3,
    nullptr,
    0,
    nullptr,
    0};

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

extern "C" void* cloth_rt_console_read_line(std::uint8_t* status) noexcept {
  if (status == nullptr) {
    runtime_failure("standard input status pointer is null");
  }
  *status = kClothConsoleInputIoError;
#if defined(_WIN32)
  if (has_console_input()) {
    return read_console_line(*status);
  }
#endif
  return read_stream_line(*status);
}

extern "C" void cloth_rt_console_write_error(const void* value) noexcept {
  const ClothString& string = require_string(value);
  if (string.data == nullptr && string.byte_size != 0) {
    runtime_failure("standard error string has an invalid layout");
  }
  write_stderr(std::string_view{string.data, string.byte_size});
}

extern "C" void* cloth_rt_file_read_bytes(const void* value,
                                          std::uint8_t* status) noexcept {
  if (status == nullptr) {
    runtime_failure("file read status pointer is null");
  }
  *status = kClothFileReadIoError;
  const ClothString& path = require_file_path(value);
  if (has_native_path_terminator(path)) {
    *status = kClothFileReadInvalidPath;
    return nullptr;
  }
  return read_file_bytes(path, *status);
}

extern "C" std::uint8_t cloth_rt_parse_primitive(std::uint8_t kind,
                                                 const void* value,
                                                 std::uint64_t* bits) noexcept {
  if (bits == nullptr) {
    runtime_failure("primitive parse result pointer is null");
  }
  *bits = 0;
  if (kind > kClothParseFloat64) {
    runtime_failure("primitive parse kind is invalid");
  }
  if (value == nullptr) {
    runtime_failure("primitive parse text is null");
  }
  const ClothString& text = require_parse_text(value);
  if (kind == kClothParseBool) {
    if (text.byte_size == 4 && std::memcmp(text.data, "true", 4) == 0) {
      *bits = 1;
      return kClothParseValue;
    }
    if (text.byte_size == 5 && std::memcmp(text.data, "false", 5) == 0) {
      return kClothParseValue;
    }
    return kClothParseInvalid;
  }
  if (kind == kClothParseChar) {
    std::size_t index = 0;
    std::uint32_t scalar = 0;
    if (!decode_utf8_scalar(text.data, text.byte_size, index, scalar) ||
        index != text.byte_size) {
      return kClothParseInvalid;
    }
    *bits = scalar;
    return kClothParseValue;
  }
  IntegerParseType integer_type{};
  if (integer_parse_type(kind, integer_type)) {
    return parse_integer(text, integer_type, *bits);
  }
  return kind == kClothParseFloat32 ? parse_float<float>(text, *bits)
                                    : parse_float<double>(text, *bits);
}

extern "C" void* cloth_rt_program_arguments(std::int32_t host_count,
                                            const void* host_values) noexcept {
  if (host_count < 0 || (host_count != 0 && host_values == nullptr)) {
    runtime_failure("program argument vector is invalid");
  }
  const std::int32_t argument_count = host_count == 0 ? 0 : host_count - 1;
  void* arguments =
      cloth_rt_array_alloc(argument_count, &kProgramArgumentElementLayout);
  void** roots[]{&arguments};
  ClothGcRootFrame frame{};
  cloth_rt_gc_push_frame(&frame, roots, 1);

#if defined(_WIN32)
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  const auto* values = static_cast<const wchar_t* const*>(host_values);
#else
  const auto* values = static_cast<const char* const*>(host_values);
#endif
  for (std::int32_t index = 0; index < argument_count; ++index) {
    void* argument = nullptr;
#if defined(_WIN32)
    const wchar_t* value = values[index + 1];
    const Utf16ArgumentSize size = measure_utf16_argument(value);
    auto* encoded = static_cast<char*>(allocate_aligned(
        size.bytes, alignof(char), "program argument conversion failed"));
    encode_utf16_argument(value, size.units, encoded);
    argument = allocate_owned_string(encoded, size.bytes, size.scalars);
    free_aligned(encoded);
#else
    const char* value = values[index + 1];
    const std::size_t size = program_argument_size(value);
    const std::size_t scalars = count_utf8_scalars(
        value, size, "program argument is not valid Unicode");
    argument = allocate_owned_string(value, size, scalars);
#endif
    std::memcpy(cloth_rt_array_element(arguments, index), &argument,
                sizeof(argument));
  }

  cloth_rt_gc_pop_frame(&frame);
  return arguments;
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

extern "C" void* cloth_rt_string_slice(const void* value, std::int32_t start,
                                       std::int32_t end) noexcept {
  const ClothString& string = require_traversable_string(value);
  const bool has_nonnegative_bounds = start >= 0 && end >= 0;
  const std::size_t requested_start =
      has_nonnegative_bounds ? static_cast<std::size_t>(start) : 0;
  const std::size_t requested_end =
      has_nonnegative_bounds ? static_cast<std::size_t>(end) : 0;

  std::size_t byte_offset = 0;
  std::size_t scalar_index = 0;
  std::size_t start_byte = 0;
  std::size_t end_byte = 0;
  bool found_start = has_nonnegative_bounds && requested_start == 0;
  bool found_end = has_nonnegative_bounds && requested_end == 0;
  while (byte_offset < string.byte_size) {
    std::uint32_t scalar = 0;
    if (!decode_utf8_scalar(string.data, string.byte_size, byte_offset,
                            scalar)) {
      runtime_failure("string has an invalid layout");
    }
    ++scalar_index;
    if (!found_start && scalar_index == requested_start) {
      start_byte = byte_offset;
      found_start = true;
    }
    if (!found_end && scalar_index == requested_end) {
      end_byte = byte_offset;
      found_end = true;
    }
  }
  if (scalar_index != string.scalar_count) {
    runtime_failure("string has an invalid layout");
  }
  if (!has_nonnegative_bounds || start > end || !found_start || !found_end) {
    runtime_failure("string slice is out of bounds");
  }

  const std::size_t result_size = end_byte - start_byte;
  const char* result_data =
      result_size == 0 ? nullptr : string.data + start_byte;
  return allocate_owned_string(result_data, result_size,
                               requested_end - requested_start);
}

extern "C" std::uint32_t cloth_rt_string_scalar_at(
    const void* value, std::int32_t index) noexcept {
  const ClothString& string = require_traversable_string(value);
  if (!has_valid_string_layout(string)) {
    runtime_failure("string has an invalid layout");
  }
  if (index < 0 || static_cast<std::size_t>(index) >= string.scalar_count) {
    runtime_failure("string index is out of bounds");
  }
  std::size_t byte_offset = 0;
  std::uint32_t scalar = 0;
  for (std::int32_t scalar_index = 0; scalar_index <= index; ++scalar_index) {
    if (!decode_utf8_scalar(string.data, string.byte_size, byte_offset,
                            scalar)) {
      runtime_failure("string has an invalid layout");
    }
  }
  return scalar;
}

extern "C" std::uint8_t cloth_rt_string_next_scalar(
    const void* value, std::int32_t* byte_offset,
    std::uint32_t* scalar) noexcept {
  if (byte_offset == nullptr || scalar == nullptr) {
    runtime_failure("string iteration output pointer is null");
  }
  const ClothString& string = require_traversable_string(value);
  if (*byte_offset < 0 ||
      static_cast<std::size_t>(*byte_offset) > string.byte_size) {
    runtime_failure("string iteration cursor is invalid");
  }
  std::size_t next = static_cast<std::size_t>(*byte_offset);
  if (next == string.byte_size) {
    if (!has_valid_string_layout(string)) {
      runtime_failure("string has an invalid layout");
    }
    *scalar = 0;
    return 0;
  }
  if (next != 0 &&
      (static_cast<unsigned char>(string.data[next]) & 0xc0U) == 0x80U) {
    runtime_failure("string iteration cursor is not at a scalar boundary");
  }
  std::uint32_t decoded = 0;
  if (!decode_utf8_scalar(string.data, string.byte_size, next, decoded) ||
      next >
          static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    runtime_failure("string has an invalid layout");
  }
  *byte_offset = static_cast<std::int32_t>(next);
  *scalar = decoded;
  return 1;
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

extern "C" bool cloth_rt_object_equals(const void* value,
                                       const void* other) noexcept {
  static_cast<void>(require_object(value));
  if (other != nullptr) {
    static_cast<void>(require_object(other));
  }
  return value == other;
}

extern "C" std::uint64_t cloth_rt_object_hash_code(const void* value) noexcept {
  const ClothObjectHeader& object = require_object(value);
  ClothAllocation* allocation = find_allocation(value);
  if (allocation == nullptr || object.runtime_state != allocation) {
    runtime_failure("object is not a live managed allocation");
  }
  return mix_hash(allocation->identity);
}

extern "C" void* cloth_rt_object_to_string(const void* value) noexcept {
  return default_object_string(require_object(value));
}

extern "C" void* cloth_rt_box_value(const ClothTypeDescriptor* type,
                                    const ClothValueLayout* value_layout,
                                    const void* value) noexcept {
  validate_type_descriptor(type);
  validate_value_layout(value_layout);
  if (value == nullptr) {
    runtime_failure("box source storage is null");
  }
  if (value_layout->kind == ClothValueKind::kNullable) {
    const std::uint8_t tag = load_value<std::uint8_t>(value);
    if (tag > 1) {
      runtime_failure("nullable box source has an invalid tag");
    }
    if (tag == 0) {
      return nullptr;
    }
    const ClothValueFieldLayout& payload = value_layout->fields[0];
    value = static_cast<const std::byte*>(value) + payload.offset;
    value_layout = payload.type;
  }
  if (!same_value_layout_identity(*type->boxed_value_layout, *value_layout)) {
    runtime_failure("box source type does not match its descriptor");
  }
  void* object = cloth_rt_alloc(type);
  std::memcpy(
      static_cast<std::byte*>(object) + type->boxed_value_offset, value,
      native_size(value_layout->size, "boxed value payload is too large"));
  return object;
}

extern "C" std::uint8_t cloth_rt_try_unbox(
    const void* value, const ClothTypeDescriptor* expected_type,
    void* output) noexcept {
  validate_type_descriptor(expected_type);
  if (expected_type->boxed_value_layout == nullptr || output == nullptr) {
    runtime_failure("unbox target metadata or storage is invalid");
  }
  if (value == nullptr) {
    return 0;
  }
  const ClothObjectHeader& object = require_object(value);
  if (object.type != expected_type) {
    return 0;
  }
  std::memcpy(
      output,
      static_cast<const std::byte*>(value) + expected_type->boxed_value_offset,
      native_size(expected_type->boxed_value_layout->size,
                  "unboxed value payload is too large"));
  return 1;
}

extern "C" bool cloth_rt_value_box_equals(const void* value,
                                          const void* other) noexcept {
  const ClothObjectHeader& left = require_value_box(value);
  if (other == nullptr) {
    return false;
  }
  const ClothObjectHeader& right = require_object(other);
  if (left.type != right.type) {
    return false;
  }
  return values_equal(
      *left.type->boxed_value_layout,
      static_cast<const std::byte*>(value) + left.type->boxed_value_offset,
      static_cast<const std::byte*>(other) + right.type->boxed_value_offset);
}

extern "C" std::uint64_t cloth_rt_value_box_hash_code(
    const void* value) noexcept {
  const ClothObjectHeader& object = require_value_box(value);
  const std::uint64_t type_hash = hash_bytes(
      object.type->name,
      native_size(object.type->name_size, "box type name is too large"));
  return combine_hash(type_hash,
                      value_hash(*object.type->boxed_value_layout,
                                 static_cast<const std::byte*>(value) +
                                     object.type->boxed_value_offset));
}

extern "C" void* cloth_rt_value_box_to_string(const void* value) noexcept {
  const ClothObjectHeader& object = require_value_box(value);
  NativeBuffer<char> buffer;
  append_value(
      buffer, *object.type->boxed_value_layout,
      static_cast<const std::byte*>(value) + object.type->boxed_value_offset);
  const std::size_t scalar_count =
      count_utf8_scalars(buffer.data(), buffer.size());
  return allocate_owned_string(buffer.data(), buffer.size(), scalar_count);
}

extern "C" std::uint8_t cloth_rt_object_is_kind(const void* value,
                                                std::uint64_t kind) noexcept {
  if (value == nullptr) {
    return 0;
  }
  if (kind > static_cast<std::uint64_t>(ClothHeapObjectKind::kValueBox)) {
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
    if (!is_heap_object_kind(current->kind)) {
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
    std::int32_t length, const ClothArrayElementLayout* element) noexcept {
  if (length < 0) {
    runtime_failure("array length is negative");
  }
  if (element == nullptr) {
    runtime_failure("array element metadata is null");
  }
  const std::uint64_t element_size = element->size;
  const std::uint64_t element_alignment = element->alignment;
  if (element_size == 0) {
    runtime_failure("array element size is zero");
  }
  if (!is_power_of_two(element_alignment) ||
      element_size % element_alignment != 0) {
    runtime_failure("invalid array element alignment");
  }
  const std::size_t native_element_size =
      native_size(element_size, "array element is too large");
  static_cast<void>(
      native_size(element_alignment, "array alignment is too large"));
  static_cast<void>(native_size(element->reference_count,
                                "array reference count is too large"));
  if ((element->reference_count == 0) !=
          (element->reference_offsets == nullptr) ||
      element->reference_count > element_size / sizeof(void*)) {
    runtime_failure("invalid array reference metadata");
  }
  if (element->reference_count != 0 && element_alignment < alignof(void*)) {
    runtime_failure("invalid reference array element layout");
  }
  for (std::uint64_t index = 0; index < element->reference_count; ++index) {
    const std::uint64_t offset = element->reference_offsets[index];
    if (offset % alignof(void*) != 0 || offset > element_size ||
        sizeof(void*) > element_size - offset ||
        (index != 0 && element->reference_offsets[index - 1] >= offset)) {
      runtime_failure("invalid array reference offset");
    }
  }

  const std::size_t native_length = static_cast<std::size_t>(length);
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
  array->element = element;
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

extern "C" void cloth_rt_require_nullable_value(std::uint8_t tag) noexcept {
  if (tag == 1) return;
  if (tag == 0) {
    runtime_failure("non-null assertion failed");
  }
  runtime_failure("nullable value has an invalid presence tag");
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

extern "C" void cloth_rt_require_integer_arithmetic(
    std::uint8_t valid, std::uint8_t reason) noexcept {
  if (valid != 0) return;
  switch (reason) {
    case kClothIntegerArithmeticOverflow:
      runtime_failure("integer arithmetic overflow");
    case kClothIntegerDivisionByZero:
      runtime_failure("integer division by zero");
    case kClothIntegerRemainderByZero:
      runtime_failure("integer remainder by zero");
    default:
      runtime_failure("invalid integer arithmetic failure code");
  }
}

extern "C" void* cloth_rt_make_division_by_zero() noexcept {
  void* message = cloth_rt_string_literal(nullptr, 0);
  ClothGcRootFrame frame{};
  void** roots[]{&message};
  cloth_rt_gc_push_frame(&frame, roots, 1);
  auto* error =
      static_cast<ClothError*>(cloth_rt_alloc(&cloth_rt_division_by_zero_type));
  error->message = message;
  cloth_rt_gc_pop_frame(&frame);
  return error;
}

extern "C" std::int32_t cloth_rt_report_error(const void* value) noexcept {
  const ClothObjectHeader& object = require_object(value);
  validate_type_descriptor(object.type);
  if (object.type->kind != ClothHeapObjectKind::kError) {
    runtime_failure("error reporter received a non-error object");
  }
  const auto& error = *static_cast<const ClothError*>(value);
  constexpr std::string_view kPrefix = "cloth error: ";
  static_cast<void>(std::fwrite(kPrefix.data(), 1, kPrefix.size(), stderr));
  static_cast<void>(std::fwrite(
      error.header.type->name, 1,
      native_size(error.header.type->name_size, "error type name is too large"),
      stderr));
  if (error.message != nullptr) {
    const ClothString& message = require_string(error.message);
    if (message.byte_size != 0) {
      constexpr std::string_view kSeparator = ": ";
      static_cast<void>(
          std::fwrite(kSeparator.data(), 1, kSeparator.size(), stderr));
      static_cast<void>(
          std::fwrite(message.data, 1, message.byte_size, stderr));
    }
  }
  static_cast<void>(std::fputc('\n', stderr));
  static_cast<void>(std::fflush(stderr));
  return 1;
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
  if (has_object_dispatch(header.type)) {
    if (header.type->virtual_function_count < 3) {
      runtime_failure("Object virtual dispatch table is incomplete");
    }
    using ToStringFunction = void* (*)(const void*);
    const auto to_string = reinterpret_cast<ToStringFunction>(
        const_cast<void*>(header.type->virtual_functions[2]));
    const ClothString& display = require_traversable_string(to_string(value));
    write_stdout(std::string_view{display.data, display.byte_size});
    return;
  }
  write_stdout("<");
  write_stdout(std::string_view{
      header.type->name,
      native_size(header.type->name_size, "object type name is too large")});
  write_stdout(">");
}

extern "C" void cloth_rt_print_newline() noexcept { write_stdout("\n"); }
