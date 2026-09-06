// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/runtime/runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestContext;

constexpr int kInterfaceFunctionSentinel = 0;
constexpr ClothArrayElementLayout kByteElement{1, 1, nullptr, 0};
constexpr std::uint64_t kPointerOffsets[]{0};
constexpr ClothArrayElementLayout kPointerElement{sizeof(void*), alignof(void*),
                                                  kPointerOffsets, 1};

struct TestNode {
  const ClothTypeDescriptor* type;
  void* runtime_state;
  void* first;
  void* second;
};

struct TestError {
  const ClothTypeDescriptor* type;
  void* runtime_state;
  void* message;
};

struct InlineValue {
  std::uint32_t tag;
  void* first;
  std::uint32_t count;
  void* second;
};

int runtime_failure_scenario(std::string_view scenario) {
  if (scenario == "integer_overflow") {
    cloth_rt_require_integer_arithmetic(0, kClothIntegerArithmeticOverflow);
  }
  if (scenario == "integer_division_by_zero") {
    cloth_rt_require_integer_arithmetic(0, kClothIntegerDivisionByZero);
  }
  if (scenario == "integer_remainder_by_zero") {
    cloth_rt_require_integer_arithmetic(0, kClothIntegerRemainderByZero);
  }
  if (scenario == "invalid_integer_arithmetic_reason") {
    cloth_rt_require_integer_arithmetic(0, UINT8_MAX);
  }
  if (scenario == "program_argument_count") {
    static_cast<void>(cloth_rt_program_arguments(-1, nullptr));
  }
  if (scenario == "program_argument_vector") {
    static_cast<void>(cloth_rt_program_arguments(1, nullptr));
  }
  if (scenario == "program_argument_value") {
#if defined(_WIN32)
    const wchar_t* values[]{L"program", nullptr};
#else
    const char* values[]{"program", nullptr};
#endif
    static_cast<void>(cloth_rt_program_arguments(2, values));
  }
  if (scenario == "program_argument_unicode") {
#if defined(_WIN32)
    const wchar_t invalid[]{static_cast<wchar_t>(0xd800), L'\0'};
    const wchar_t* values[]{L"program", invalid};
#else
    const char invalid[]{static_cast<char>(0xff), '\0'};
    const char* values[]{"program", invalid};
#endif
    static_cast<void>(cloth_rt_program_arguments(2, values));
  }
  constexpr std::string_view kPlainName = "Plain";
  const ClothTypeDescriptor plain_type{ClothHeapObjectKind::kFileClass,
                                       nullptr,
                                       kPlainName.data(),
                                       kPlainName.size(),
                                       sizeof(TestNode),
                                       alignof(TestNode),
                                       nullptr,
                                       0,
                                       nullptr,
                                       0,
                                       nullptr,
                                       0};
  if (scenario == "report_non_error") {
    static_cast<void>(cloth_rt_report_error(cloth_rt_alloc(&plain_type)));
  }
  constexpr std::string_view kInvalidErrorName = "InvalidError";
  const ClothTypeDescriptor invalid_error_type{ClothHeapObjectKind::kError,
                                               &plain_type,
                                               kInvalidErrorName.data(),
                                               kInvalidErrorName.size(),
                                               sizeof(TestError),
                                               alignof(TestError),
                                               nullptr,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               0};
  if (scenario == "invalid_error_parent") {
    static_cast<void>(cloth_rt_alloc(&invalid_error_type));
  }
  std::uint64_t offsets[]{0, sizeof(void*)};
  ClothArrayElementLayout layout{2 * sizeof(void*), alignof(void*), offsets, 2};
  if (scenario == "null") {
    static_cast<void>(cloth_rt_array_alloc(1, nullptr));
  } else {
    if (scenario == "zero") layout.size = 0;
    if (scenario == "alignment") layout.alignment = 3;
    if (scenario == "stride") --layout.size;
    if (scenario == "table") layout.reference_offsets = nullptr;
    if (scenario == "count") layout.reference_count = 3;
    if (scenario == "offset") offsets[1] = layout.size;
    if (scenario == "unaligned") offsets[1] = 1;
    if (scenario == "duplicate") offsets[1] = 0;
    if (scenario == "unsorted") {
      offsets[0] = sizeof(void*);
      offsets[1] = 0;
    }
    if (scenario == "reference_alignment") layout.alignment = 1;
    if (scenario == "overflow") {
      layout.size = UINT64_MAX - 7;
      layout.reference_offsets = nullptr;
      layout.reference_count = 0;
    }
    const int length = scenario == "negative"   ? -1
                       : scenario == "overflow" ? 2
                                                : 1;
    static_cast<void>(cloth_rt_array_alloc(length, &layout));
  }
  return 0;  // The harness requires a runtime failure, not just any exit.
}

void store_reference(void* object, std::size_t offset, void* reference) {
  std::memcpy(static_cast<std::byte*>(object) + offset, &reference,
              sizeof(reference));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) return runtime_failure_scenario(argv[1]);
  TestContext test{"runtime"};
  cloth_rt_require_integer_arithmetic(1, kClothIntegerArithmeticOverflow);
  cloth_rt_require_integer_arithmetic(1, kClothIntegerDivisionByZero);
  cloth_rt_require_integer_arithmetic(1, kClothIntegerRemainderByZero);
  void* first = nullptr;
  void* second = nullptr;
  void** outer_roots[]{&first, &second};
  void** inner_roots[]{&second};
  ClothGcRootFrame outer{};
  ClothGcRootFrame inner{};

  cloth_rt_gc_push_frame(&outer, outer_roots, 2);
  test.expect(outer.previous == nullptr && outer.roots == outer_roots &&
                  outer.root_count == 2,
              "outer GC root frame was not initialized");

  cloth_rt_gc_push_frame(&inner, inner_roots, 1);
  test.expect(inner.previous == &outer && inner.roots == inner_roots &&
                  inner.root_count == 1,
              "nested GC root frame did not link to its caller");

  cloth_rt_gc_pop_frame(&inner);
  test.expect(inner.previous == nullptr && inner.roots == nullptr &&
                  inner.root_count == 0,
              "popped GC root frame retained active metadata");

  cloth_rt_gc_pop_frame(&outer);
  test.expect(outer.previous == nullptr && outer.roots == nullptr &&
                  outer.root_count == 0,
              "outer GC root frame was not cleared");

  void* integer_bytes = cloth_rt_array_alloc(13, &kByteElement);
  cloth_rt_integer_write(integer_bytes, 1, UINT64_C(0x89ABCDEF), 4, 0);
  cloth_rt_integer_write(integer_bytes, 5, UINT64_C(0x0123456789ABCDEF), 8, 1);
  test.expect(
      cloth_rt_integer_read(integer_bytes, 1, 4, 0) == UINT64_C(0x89ABCDEF) &&
          cloth_rt_integer_read(integer_bytes, 5, 8, 1) ==
              UINT64_C(0x0123456789ABCDEF),
      "integer byte-order round trips changed their bit patterns");

  constexpr std::uint64_t kReferenceOffsets[]{offsetof(TestNode, first),
                                              offsetof(TestNode, second)};
  constexpr std::uint64_t kBaseReferenceOffsets[]{offsetof(TestNode, first)};
  constexpr std::uint64_t kInterfaceId = 0xC10F18U;
  const void* interface_functions[]{&kInterfaceFunctionSentinel};
  const ClothInterfaceDispatch interface_dispatch{kInterfaceId,
                                                  interface_functions, 1};
  constexpr std::string_view kBaseName = "TestBase";
  const ClothTypeDescriptor base_type{
      ClothHeapObjectKind::kFileClass,
      nullptr,
      kBaseName.data(),
      kBaseName.size(),
      offsetof(TestNode, second),
      alignof(TestNode),
      kBaseReferenceOffsets,
      1,
      nullptr,
      0,
      &interface_dispatch,
      1,
  };
  constexpr std::string_view kNodeName = "TestNode";
  const ClothTypeDescriptor node_type{
      ClothHeapObjectKind::kFileClass,
      &base_type,
      kNodeName.data(),
      kNodeName.size(),
      sizeof(TestNode),
      alignof(TestNode),
      kReferenceOffsets,
      2,
      nullptr,
      0,
      &interface_dispatch,
      1,
  };

  ClothGcRootFrame managed_frame{};
  void* managed_root = nullptr;
  void** managed_roots[]{&managed_root};
  cloth_rt_gc_push_frame(&managed_frame, managed_roots, 1);

  void* first_node = cloth_rt_alloc(&node_type);
  test.expect(
      cloth_rt_object_is_interface(first_node, kInterfaceId) == 1 &&
          cloth_rt_object_is_interface(first_node, kInterfaceId + 1) == 0,
      "runtime interface membership lookup is incorrect");
  test.expect(cloth_rt_interface_function(first_node, kInterfaceId, 0) ==
                  interface_functions[0],
              "runtime interface dispatch returned the wrong function");
  managed_root = first_node;
  void* second_node = cloth_rt_alloc(&node_type);
  store_reference(first_node, offsetof(TestNode, first), second_node);
  store_reference(second_node, offsetof(TestNode, second), first_node);

  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 2 &&
                  cloth_rt_gc_live_bytes() == 2 * sizeof(TestNode),
              "marking did not preserve a rooted object cycle");

  managed_root = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "sweeping did not reclaim an unreachable object cycle");
  cloth_rt_gc_pop_frame(&managed_frame);

  constexpr std::size_t kAutomaticAllocationCount = 10000;
  const std::uint64_t automatic_collections_before =
      cloth_rt_gc_collection_count();
  for (std::size_t index = 0; index < kAutomaticAllocationCount; ++index) {
    static_cast<void>(cloth_rt_alloc(&node_type));
  }
  test.expect(cloth_rt_gc_live_objects() < kAutomaticAllocationCount &&
                  cloth_rt_gc_collection_count() > automatic_collections_before,
              "managed allocation did not trigger a collection safepoint");
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "explicit collection left unreachable managed objects");

  ClothGcRootFrame string_frame{};
  void* empty_string = nullptr;
  void* unicode_string = nullptr;
  void* left_string = nullptr;
  void* right_string = nullptr;
  void* joined_string = nullptr;
  void* expected_string = nullptr;
  void** string_roots[]{&empty_string, &unicode_string, &left_string,
                        &right_string, &joined_string,  &expected_string};
  cloth_rt_gc_push_frame(&string_frame, string_roots, 6);

  empty_string = cloth_rt_string_literal(nullptr, 0);
  constexpr char kUnicodeBytes[] = "\xC3\xA9\xF0\x9F\x99\x82";
  unicode_string =
      cloth_rt_string_literal(kUnicodeBytes, sizeof(kUnicodeBytes) - 1);
  constexpr std::string_view kLeft = "Hello, ";
  constexpr std::string_view kRight = "Cloth";
  constexpr std::string_view kExpected = "Hello, Cloth";
  left_string = cloth_rt_string_literal(kLeft.data(), kLeft.size());
  right_string = cloth_rt_string_literal(kRight.data(), kRight.size());
  joined_string = cloth_rt_string_concat(left_string, right_string);
  expected_string = cloth_rt_string_literal(kExpected.data(), kExpected.size());

  test.expect(cloth_rt_string_length(empty_string) == 0 &&
                  cloth_rt_string_byte_length(empty_string) == 0 &&
                  cloth_rt_string_is_empty(empty_string) == 1,
              "empty string meta-query values are wrong");
  test.expect(cloth_rt_string_length(unicode_string) == 2 &&
                  cloth_rt_string_byte_length(unicode_string) == 6 &&
                  cloth_rt_string_is_empty(unicode_string) == 0,
              "UTF-8 scalar and byte lengths are wrong");
  test.expect(cloth_rt_string_length(joined_string) == 12 &&
                  cloth_rt_string_byte_length(joined_string) == 12 &&
                  cloth_rt_string_equal(joined_string, expected_string) == 1,
              "string concatenation did not produce the expected value");
  test.expect(cloth_rt_string_equal(expected_string, right_string) == 0 &&
                  cloth_rt_string_equal(nullptr, nullptr) == 1 &&
                  cloth_rt_string_equal(nullptr, expected_string) == 0,
              "string content or nullable equality is wrong");

  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 6 && cloth_rt_gc_live_bytes() != 0,
              "marking did not preserve rooted strings");
  empty_string = nullptr;
  unicode_string = nullptr;
  left_string = nullptr;
  right_string = nullptr;
  joined_string = nullptr;
  expected_string = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "sweeping did not reclaim borrowed and owned strings");
  cloth_rt_gc_pop_frame(&string_frame);

  ClothGcRootFrame argument_frame{};
  void* program_arguments = nullptr;
  void** argument_roots[]{&program_arguments};
#if defined(_WIN32)
  wchar_t empty_argument[]{L'\0'};
  wchar_t words_argument[]{L't', L'w', L'o', L' ', L'w',
                           L'o', L'r', L'd', L's', L'\0'};
  wchar_t option_argument[]{L'-', L'-', L'f', L'l', L'a', L'g', L'\0'};
  wchar_t unicode_argument[]{static_cast<wchar_t>(0x00e9),
                             static_cast<wchar_t>(0xd83d),
                             static_cast<wchar_t>(0xde42), L'\0'};
  const wchar_t* host_arguments[]{L"program", empty_argument, words_argument,
                                  option_argument, unicode_argument};
#else
  char empty_argument[]{'\0'};
  char words_argument[]{"two words"};
  char option_argument[]{"--flag"};
  char unicode_argument[]{"\xC3\xA9\xF0\x9F\x99\x82"};
  const char* host_arguments[]{"program", empty_argument, words_argument,
                               option_argument, unicode_argument};
#endif
  program_arguments = cloth_rt_program_arguments(5, host_arguments);
  cloth_rt_gc_push_frame(&argument_frame, argument_roots, 1);
  words_argument[0] = 'X';
  option_argument[0] = 'X';
  unicode_argument[0] = 'X';
  test.expect(cloth_rt_array_length(program_arguments) == 4,
              "program argument count excluded the wrong host values");
  constexpr std::array<std::string_view, 4> kExpectedArguments{
      "", "two words", "--flag", "\xC3\xA9\xF0\x9F\x99\x82"};
  for (std::int32_t index = 0; index < 4; ++index) {
    void* actual = nullptr;
    std::memcpy(&actual, cloth_rt_array_element(program_arguments, index),
                sizeof(actual));
    const std::string_view expected =
        kExpectedArguments[static_cast<std::size_t>(index)];
    void* expected_string =
        cloth_rt_string_literal(expected.data(), expected.size());
    test.expect(cloth_rt_string_equal(actual, expected_string) == 1,
                "program argument conversion changed a value");
  }
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 5,
              "owned program arguments were not traced through their array");
  program_arguments = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "owned program arguments were not reclaimed");
  cloth_rt_gc_pop_frame(&argument_frame);

  constexpr std::int32_t kStressArgumentCount = 5000;
#if defined(_WIN32)
  std::vector<const wchar_t*> stress_arguments(
      static_cast<std::size_t>(kStressArgumentCount) + 1, L"argument");
  stress_arguments.front() = L"program";
#else
  std::vector<const char*> stress_arguments(
      static_cast<std::size_t>(kStressArgumentCount) + 1, "argument");
  stress_arguments.front() = "program";
#endif
  program_arguments = cloth_rt_program_arguments(kStressArgumentCount + 1,
                                                 stress_arguments.data());
  cloth_rt_gc_push_frame(&argument_frame, argument_roots, 1);
  cloth_rt_gc_collect();
  test.expect(
      cloth_rt_array_length(program_arguments) == kStressArgumentCount &&
          cloth_rt_gc_live_objects() ==
              static_cast<std::uint64_t>(kStressArgumentCount) + 1,
      "program argument construction lost values at GC safepoints");
  program_arguments = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "stress program arguments were not reclaimed");
  cloth_rt_gc_pop_frame(&argument_frame);

  ClothGcRootFrame array_frame{};
  void* array_root = nullptr;
  void** array_roots[]{&array_root};
  cloth_rt_gc_push_frame(&array_frame, array_roots, 1);
  array_root = cloth_rt_array_alloc(1, &kPointerElement);
  void* array_node = cloth_rt_alloc(&node_type);
  std::memcpy(cloth_rt_array_element(array_root, 0), &array_node,
              sizeof(array_node));
  store_reference(array_node, offsetof(TestNode, first), array_root);

  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 2,
              "array tracing did not preserve a cross-kind object cycle");
  array_root = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "sweeping did not reclaim a cross-kind object cycle");
  cloth_rt_gc_pop_frame(&array_frame);

  constexpr std::uint64_t kInlineOffsets[]{offsetof(InlineValue, first),
                                           offsetof(InlineValue, second)};
  const ClothArrayElementLayout inline_layout{
      sizeof(InlineValue), alignof(InlineValue), kInlineOffsets, 2};
  InlineValue local{7, nullptr, 9, nullptr};
  void* aggregate_array = nullptr;
  void** aggregate_roots[]{&aggregate_array, &local.first, &local.second};
  ClothGcRootFrame aggregate_frame{};
  cloth_rt_gc_push_frame(&aggregate_frame, aggregate_roots, 3);
  local.first = cloth_rt_alloc(&node_type);
  local.second = cloth_rt_string_literal("alive", 5);
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 2,
              "interior reference slots in an inline value were not roots");
  aggregate_array = cloth_rt_array_alloc(2, &inline_layout);
  InlineValue zeroed{};
  std::memcpy(&zeroed, cloth_rt_array_element(aggregate_array, 1),
              sizeof(zeroed));
  test.expect(zeroed.tag == 0 && zeroed.count == 0 && zeroed.first == nullptr &&
                  zeroed.second == nullptr,
              "aggregate array payload was not zeroed");
  std::memcpy(cloth_rt_array_element(aggregate_array, 0), &local,
              sizeof(local));
  std::memcpy(cloth_rt_array_element(aggregate_array, 1), &local,
              sizeof(local));
  local.first = nullptr;
  local.second = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 3,
              "aggregate array did not trace every contained reference");
  std::memcpy(cloth_rt_array_element(aggregate_array, 0), &zeroed,
              sizeof(zeroed));
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 3,
              "aggregate array did not trace references in later elements");
  std::memcpy(cloth_rt_array_element(aggregate_array, 1), &zeroed,
              sizeof(zeroed));
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 1,
              "aggregate array retained overwritten references");
  aggregate_array = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "aggregate array storage was not reclaimed");
  cloth_rt_gc_pop_frame(&aggregate_frame);

  ClothGcRootFrame object_frame{};
  void* meta_node = nullptr;
  void* meta_string = nullptr;
  void* meta_array = nullptr;
  void* node_name = nullptr;
  void* string_name = nullptr;
  void* array_name = nullptr;
  void* expected_node_name = nullptr;
  void* expected_string_name = nullptr;
  void* expected_array_name = nullptr;
  void** object_roots[]{
      &meta_node,          &meta_string,          &meta_array,
      &node_name,          &string_name,          &array_name,
      &expected_node_name, &expected_string_name, &expected_array_name};
  cloth_rt_gc_push_frame(&object_frame, object_roots, 9);
  meta_node = cloth_rt_alloc(&node_type);
  constexpr std::string_view kMetaString = "value";
  meta_string = cloth_rt_string_literal(kMetaString.data(), kMetaString.size());
  meta_array = cloth_rt_array_alloc(1, &kPointerElement);
  node_name = cloth_rt_object_type_name(meta_node);
  string_name = cloth_rt_object_type_name(meta_string);
  array_name = cloth_rt_object_type_name(meta_array);
  expected_node_name =
      cloth_rt_string_literal(kNodeName.data(), kNodeName.size());
  constexpr std::string_view kStringName = "string";
  expected_string_name =
      cloth_rt_string_literal(kStringName.data(), kStringName.size());
  constexpr std::string_view kArrayName = "array";
  expected_array_name =
      cloth_rt_string_literal(kArrayName.data(), kArrayName.size());

  test.expect(cloth_rt_object_is_type(meta_node, &node_type) == 1 &&
                  cloth_rt_object_is_type(meta_node, &base_type) == 1 &&
                  cloth_rt_object_is_type(meta_string, &node_type) == 0,
              "runtime descriptor ancestry is wrong");
  test.expect(
      cloth_rt_object_is_kind(
          meta_string,
          static_cast<std::uint64_t>(ClothHeapObjectKind::kString)) == 1 &&
          cloth_rt_object_is_kind(
              meta_array,
              static_cast<std::uint64_t>(ClothHeapObjectKind::kArray)) == 1 &&
          cloth_rt_object_is_kind(
              nullptr,
              static_cast<std::uint64_t>(ClothHeapObjectKind::kString)) == 0,
      "runtime object-kind checks are wrong");
  test.expect(
      cloth_rt_string_equal(node_name, expected_node_name) == 1 &&
          cloth_rt_string_equal(string_name, expected_string_name) == 1 &&
          cloth_rt_string_equal(array_name, expected_array_name) == 1,
      "stable object type names are wrong");

  for (void** root : object_roots) {
    *root = nullptr;
  }
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "object metadata queries leaked managed strings");
  cloth_rt_gc_pop_frame(&object_frame);

  ClothGcRootFrame error_frame{};
  void* error = cloth_rt_make_division_by_zero();
  void** error_roots[]{&error};
  cloth_rt_gc_push_frame(&error_frame, error_roots, 1);
  test.expect(
      cloth_rt_object_is_kind(error, static_cast<std::uint64_t>(
                                         ClothHeapObjectKind::kError)) == 1 &&
          cloth_rt_object_is_type(error, &cloth_rt_division_by_zero_type) ==
              1 &&
          cloth_rt_object_is_type(error, &cloth_rt_error_type) == 1,
      "compiler-known error descriptors have the wrong runtime identity");
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 2,
              "error tracing did not retain its managed message");
  error = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "error and message were not reclaimed together");
  cloth_rt_gc_pop_frame(&error_frame);

  const std::uint64_t peak_before = cloth_rt_gc_peak_live_bytes();
  static_cast<void>(cloth_rt_alloc(&node_type));
  test.expect(cloth_rt_gc_peak_live_bytes() >= peak_before &&
                  cloth_rt_gc_peak_live_bytes() >= cloth_rt_gc_live_bytes(),
              "peak managed bytes did not track live heap growth");
  const std::uint64_t collections_before = cloth_rt_gc_collection_count();
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_collection_count() == collections_before + 1 &&
                  cloth_rt_gc_live_objects() == 0,
              "explicit collection diagnostics were not updated");

  if (test.failures() == 0) {
    std::cout << "9 tests passed\n";
    return 0;
  }
  return 1;
}
