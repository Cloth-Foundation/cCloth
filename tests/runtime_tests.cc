#include "cloth/runtime/runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

class TestContext {
 public:
  void expect(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << message << '\n';
    }
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  int failures_{0};
};

struct TestNode {
  const ClothTypeDescriptor* type;
  void* runtime_state;
  void* first;
  void* second;
};

void store_reference(void* object, std::size_t offset, void* reference) {
  std::memcpy(static_cast<std::byte*>(object) + offset, &reference,
              sizeof(reference));
}

}  // namespace

int main() {
  TestContext test;
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

  constexpr std::uint64_t kReferenceOffsets[]{offsetof(TestNode, first),
                                              offsetof(TestNode, second)};
  constexpr std::uint64_t kBaseReferenceOffsets[]{offsetof(TestNode, first)};
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
  };

  ClothGcRootFrame managed_frame{};
  void* managed_root = nullptr;
  void** managed_roots[]{&managed_root};
  cloth_rt_gc_push_frame(&managed_frame, managed_roots, 1);

  void* first_node = cloth_rt_alloc(&node_type);
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

  ClothGcRootFrame array_frame{};
  void* array_root = nullptr;
  void** array_roots[]{&array_root};
  cloth_rt_gc_push_frame(&array_frame, array_roots, 1);
  array_root = cloth_rt_array_alloc(1, sizeof(void*), alignof(void*), 1);
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
  meta_array = cloth_rt_array_alloc(1, sizeof(void*), alignof(void*), 1);
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
    std::cout << "8 tests passed\n";
    return 0;
  }
  return 1;
}
