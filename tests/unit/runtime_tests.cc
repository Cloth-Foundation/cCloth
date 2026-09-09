// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/runtime/runtime.h"

#include <array>
#include <bit>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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

struct TestString {
  const ClothTypeDescriptor* type;
  void* runtime_state;
  const char* data;
  std::size_t byte_size;
  std::size_t scalar_count;
  bool owns_data;
};

struct InlineValue {
  std::uint32_t tag;
  void* first;
  std::uint32_t count;
  void* second;
};

struct NullableInlineValue {
  std::uint8_t tag;
  InlineValue payload;
};

std::FILE* open_file_for_writing(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  return _wfopen_s(&file, path.c_str(), L"wb") == 0 ? file : nullptr;
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return std::string{reinterpret_cast<const char*>(encoded.data()),
                     encoded.size()};
}

bool write_file(const std::filesystem::path& path,
                std::span<const std::uint8_t> contents) {
  std::FILE* output = open_file_for_writing(path);
  if (output == nullptr) return false;
  const std::size_t written =
      std::fwrite(contents.data(), 1, contents.size(), output);
  return std::fclose(output) == 0 && written == contents.size();
}

bool file_read_matches(const std::filesystem::path& path,
                       std::span<const std::uint8_t> expected) {
  const std::string encoded_path = path_to_utf8(path);
  void* managed_path =
      cloth_rt_string_literal(encoded_path.data(), encoded_path.size());
  void* result = nullptr;
  void** roots[]{&managed_path, &result};
  ClothGcRootFrame frame{};
  cloth_rt_gc_push_frame(&frame, roots, 2);
  std::uint8_t status = UINT8_MAX;
  result = cloth_rt_file_read_bytes(managed_path, &status);
  cloth_rt_gc_collect();
  bool matches = status == kClothFileReadValue && result != nullptr &&
                 cloth_rt_array_length(result) ==
                     static_cast<std::int32_t>(expected.size());
  for (std::size_t index = 0; matches && index < expected.size(); ++index) {
    const auto* actual = static_cast<const std::uint8_t*>(
        cloth_rt_array_element(result, static_cast<std::int32_t>(index)));
    matches = *actual == expected[index];
  }
  cloth_rt_gc_pop_frame(&frame);
  return matches;
}

std::uint8_t file_read_status(const std::filesystem::path& path) {
  const std::string encoded_path = path_to_utf8(path);
  void* managed_path =
      cloth_rt_string_literal(encoded_path.data(), encoded_path.size());
  void* result = nullptr;
  void** roots[]{&managed_path, &result};
  ClothGcRootFrame frame{};
  cloth_rt_gc_push_frame(&frame, roots, 2);
  std::uint8_t status = UINT8_MAX;
  result = cloth_rt_file_read_bytes(managed_path, &status);
  cloth_rt_gc_pop_frame(&frame);
  return result == nullptr ? status : kClothFileReadValue;
}

struct ParseResult {
  std::uint8_t status;
  std::uint64_t bits;
};

ParseResult parse(std::uint8_t kind, std::string_view text) {
  void* string = cloth_rt_string_literal(text.data(), text.size());
  std::uint64_t bits = UINT64_MAX;
  const std::uint8_t status = cloth_rt_parse_primitive(kind, string, &bits);
  return ParseResult{status, bits};
}

int console_input_scenario(std::span<const std::string> expected_lines) {
  void* line = nullptr;
  void** roots[]{&line};
  ClothGcRootFrame frame{};
  cloth_rt_gc_push_frame(&frame, roots, 1);
  for (const std::string_view expected : expected_lines) {
    std::uint8_t status = UINT8_MAX;
    line = cloth_rt_console_read_line(&status);
    void* expected_string =
        cloth_rt_string_literal(expected.data(), expected.size());
    if (status != kClothConsoleInputValue || line == nullptr ||
        cloth_rt_string_equal(line, expected_string) == 0) {
      std::cerr << "console input line did not match\n";
      return 1;
    }
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::uint8_t status = UINT8_MAX;
    line = cloth_rt_console_read_line(&status);
    if (status != kClothConsoleInputEof || line != nullptr) {
      std::cerr << "console input did not report stable EOF\n";
      cloth_rt_gc_pop_frame(&frame);
      return 1;
    }
  }
  cloth_rt_gc_pop_frame(&frame);
  std::cout << "console input passed\n";
  return 0;
}

std::string console_edge_input() {
  constexpr std::size_t kStreamChunkSize = 4096;
  std::string input{
      "crlf\r\nlone\rcr\n\n \t \n\xef\xbb\xbf"
      "bom\nnul\0inside\n",
      37};
  input.append(kStreamChunkSize - 1 - input.size(), 'a');
  input.append("\xf0\x9f\x99\x82\nfinal");
  return input;
}

std::vector<std::string> console_edge_lines() {
  constexpr std::size_t kSplitPrefixSize = 4058;
  return {"crlf",
          "lone\rcr",
          "",
          " \t ",
          "\xef\xbb\xbf"
          "bom",
          std::string{"nul\0inside", 10},
          std::string(kSplitPrefixSize, 'a') + "\xf0\x9f\x99\x82",
          "final"};
}

int emit_console_edge_input() {
  const std::string input = console_edge_input();
  cloth_rt_print(cloth_rt_string_literal(input.data(), input.size()));
  return 0;
}

int console_io_failure_scenario() {
#if defined(_WIN32)
  int descriptors[2]{};
  if (_pipe(descriptors, 4096, _O_BINARY) != 0 ||
      _dup2(descriptors[1], _fileno(stdin)) != 0) {
    std::cerr << "could not prepare a failing standard-input pipe\n";
    return 1;
  }
  static_cast<void>(_close(descriptors[0]));
  static_cast<void>(_close(descriptors[1]));
  static_cast<void>(SetStdHandle(STD_INPUT_HANDLE, INVALID_HANDLE_VALUE));
#else
  int descriptors[2]{};
  if (pipe(descriptors) != 0 || dup2(descriptors[1], fileno(stdin)) == -1) {
    std::cerr << "could not prepare a failing standard-input pipe\n";
    return 1;
  }
  static_cast<void>(close(descriptors[0]));
  static_cast<void>(close(descriptors[1]));
#endif
  std::uint8_t status = UINT8_MAX;
  void* line = cloth_rt_console_read_line(&status);
  if (status != kClothConsoleInputIoError || line != nullptr) {
    std::cerr << "console input did not report an I/O failure\n";
    return 1;
  }
  std::cout << "console input passed\n";
  return 0;
}

int console_invalid_encoding_scenario() {
  std::uint8_t status = UINT8_MAX;
  void* line = cloth_rt_console_read_line(&status);
  if (status != kClothConsoleInputEncodingError || line != nullptr) {
    std::cerr << "console input accepted malformed UTF-8\n";
    return 1;
  }
  std::cout << "console input passed\n";
  return 0;
}

int file_read_scenario(std::string_view path) {
  std::array<std::uint8_t, 256> expected{};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expected[index] = static_cast<std::uint8_t>(index);
  }
  const std::filesystem::path native_path{std::string{path}};
  std::error_code file_error;
  static_cast<void>(std::filesystem::remove(native_path, file_error));
  if (!write_file(native_path, expected)) {
    std::cerr << "could not create file-read fixture\n";
    return 1;
  }

  const std::string encoded_path = path_to_utf8(native_path);
  void* managed_path =
      cloth_rt_string_literal(encoded_path.data(), encoded_path.size());
  void* first = nullptr;
  void* second = nullptr;
  void** roots[]{&managed_path, &first, &second};
  ClothGcRootFrame frame{};
  cloth_rt_gc_push_frame(&frame, roots, 3);
  std::uint8_t status = UINT8_MAX;
  first = cloth_rt_file_read_bytes(managed_path, &status);
  if (status != kClothFileReadValue || first == nullptr ||
      cloth_rt_array_length(first) !=
          static_cast<std::int32_t>(expected.size())) {
    std::cerr << "file read did not return the exact array size\n";
    return 1;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto* actual = static_cast<const std::uint8_t*>(
        cloth_rt_array_element(first, static_cast<std::int32_t>(index)));
    if (*actual != expected[index]) {
      std::cerr << "file read changed a byte\n";
      return 1;
    }
  }
  second = cloth_rt_file_read_bytes(managed_path, &status);
  *static_cast<std::uint8_t*>(cloth_rt_array_element(first, 0)) = UINT8_MAX;
  if (status != kClothFileReadValue || second == first ||
      *static_cast<const std::uint8_t*>(cloth_rt_array_element(second, 0)) !=
          0) {
    std::cerr << "file reads did not return independent storage\n";
    return 1;
  }

  if (!write_file(native_path, {})) {
    std::cerr << "could not create empty file-read fixture\n";
    return 1;
  }
  second = cloth_rt_file_read_bytes(managed_path, &status);
  if (status != kClothFileReadValue || second == nullptr ||
      cloth_rt_array_length(second) != 0) {
    std::cerr << "empty file did not produce an empty array\n";
    return 1;
  }

  constexpr std::array<std::uint8_t, 10> kBinaryText{
      0, 0xef, 0xbb, 0xbf, 0xff, '\r', '\n', '\r', 'X', '\n'};
  if (!write_file(native_path, kBinaryText) ||
      !file_read_matches(native_path, kBinaryText)) {
    std::cerr << "binary text bytes were not preserved\n";
    return 1;
  }

  constexpr std::size_t kReadChunkSize = 1024U * 1024U;
  for (const std::size_t size :
       {kReadChunkSize - 1, kReadChunkSize, kReadChunkSize + 1}) {
    std::vector<std::uint8_t> chunk_bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
      chunk_bytes[index] = static_cast<std::uint8_t>(index * 31U);
    }
    if (!write_file(native_path, chunk_bytes) ||
        !file_read_matches(native_path, chunk_bytes)) {
      std::cerr << "file read failed across a chunk boundary\n";
      return 1;
    }
  }

  if (!write_file(native_path, kBinaryText)) {
    std::cerr << "could not create repeated-read fixture\n";
    return 1;
  }
  first = nullptr;
  second = nullptr;
  for (int attempt = 0; attempt < 16; ++attempt) {
    second = cloth_rt_file_read_bytes(managed_path, &status);
    cloth_rt_gc_collect();
    if (status != kClothFileReadValue || second == nullptr ||
        cloth_rt_array_length(second) !=
            static_cast<std::int32_t>(kBinaryText.size()) ||
        *static_cast<const std::uint8_t*>(cloth_rt_array_element(second, 3)) !=
            kBinaryText[3]) {
      std::cerr << "repeated file read failed under GC pressure\n";
      return 1;
    }
  }

  constexpr std::uintmax_t kMaximumFileByteCount = 64ULL * 1024ULL * 1024ULL;
  if (!write_file(native_path, {})) {
    std::cerr << "could not create size-boundary fixture\n";
    return 1;
  }
  std::filesystem::resize_file(native_path, kMaximumFileByteCount, file_error);
  if (file_error) {
    std::cerr << "could not create size-boundary fixture\n";
    return 1;
  }
  second = nullptr;
  cloth_rt_gc_collect();
  first = cloth_rt_file_read_bytes(managed_path, &status);
  if (status != kClothFileReadValue || first == nullptr ||
      cloth_rt_array_length(first) !=
          static_cast<std::int32_t>(kMaximumFileByteCount) ||
      *static_cast<const std::uint8_t*>(cloth_rt_array_element(first, 0)) !=
          0 ||
      *static_cast<const std::uint8_t*>(cloth_rt_array_element(
          first, static_cast<std::int32_t>(kMaximumFileByteCount - 1))) != 0) {
    std::cerr << "exact size-limit file was rejected or changed\n";
    return 1;
  }
  first = nullptr;
  cloth_rt_gc_collect();
  std::filesystem::resize_file(native_path, kMaximumFileByteCount + 1,
                               file_error);
  if (file_error) {
    std::cerr << "could not create oversized file-read fixture\n";
    return 1;
  }
  first = cloth_rt_file_read_bytes(managed_path, &status);
  if (status != kClothFileReadTooLarge || first != nullptr) {
    std::cerr << "oversized file did not report the size limit\n";
    return 1;
  }

  file_error.clear();
  if (!std::filesystem::remove(native_path, file_error) || file_error) {
    std::cerr << "file read retained its native handle\n";
    return 1;
  }
  first = cloth_rt_file_read_bytes(managed_path, &status);
  if (status != kClothFileReadOpenError || first != nullptr) {
    std::cerr << "missing file did not report an open failure\n";
    return 1;
  }

  void* empty_path = cloth_rt_string_literal(nullptr, 0);
  first = cloth_rt_file_read_bytes(empty_path, &status);
  if (status != kClothFileReadOpenError || first != nullptr) {
    std::cerr << "empty path did not report an open failure\n";
    return 1;
  }

  void* directory = cloth_rt_string_literal(".", 1);
  first = cloth_rt_file_read_bytes(directory, &status);
  if (status != kClothFileReadNotRegular || first != nullptr) {
    std::cerr << "directory did not report a non-regular input\n";
    return 1;
  }
  constexpr char kEmbeddedZero[]{'b', 'a', 'd', '\0', 'p', 'a', 't', 'h'};
  void* embedded_zero =
      cloth_rt_string_literal(kEmbeddedZero, sizeof(kEmbeddedZero));
  first = cloth_rt_file_read_bytes(embedded_zero, &status);
  if (status != kClothFileReadInvalidPath || first != nullptr) {
    std::cerr << "embedded U+0000 did not report an invalid path\n";
    return 1;
  }

  const std::filesystem::path path_directory =
      native_path.parent_path() /
      std::filesystem::path{u8"runtime-file-paths-\u03bb"};
  file_error.clear();
  if (!std::filesystem::create_directory(path_directory, file_error) ||
      file_error) {
    std::cerr << "could not create path-semantics directory\n";
    return 1;
  }
  const std::filesystem::path named_file =
      path_directory / std::filesystem::path{u8"source bytes-\u03bc.bin"};
  if (!write_file(named_file, kBinaryText)) {
    std::cerr << "could not create path-semantics fixture\n";
    return 1;
  }
  const std::filesystem::path absolute_file =
      std::filesystem::absolute(named_file, file_error);
  const std::filesystem::path relative_file = std::filesystem::relative(
      named_file, std::filesystem::current_path(), file_error);
  std::filesystem::path preferred_file = absolute_file;
  preferred_file.make_preferred();
  const std::filesystem::path dotted_file =
      named_file.parent_path() / "." / named_file.filename();
  if (file_error || !file_read_matches(absolute_file, kBinaryText) ||
      !file_read_matches(relative_file, kBinaryText) ||
      !file_read_matches(preferred_file, kBinaryText) ||
      !file_read_matches(dotted_file, kBinaryText)) {
    std::cerr << "host path spellings did not resolve consistently\n";
    return 1;
  }

  const std::filesystem::path link_file = path_directory / "source-link.bin";
  std::filesystem::create_symlink(absolute_file, link_file, file_error);
  if (!file_error && !file_read_matches(link_file, kBinaryText)) {
    std::cerr << "symlink to a regular file was not followed\n";
    return 1;
  }
  file_error.clear();
  static_cast<void>(std::filesystem::remove(link_file, file_error));

  const std::filesystem::path directory_input =
      path_directory / "directory-input";
  file_error.clear();
  if (!std::filesystem::create_directory(directory_input, file_error) ||
      file_error ||
      file_read_status(directory_input) != kClothFileReadNotRegular) {
    std::cerr << "directory path did not report a non-regular input\n";
    return 1;
  }
  file_error.clear();
  if (!std::filesystem::remove(directory_input, file_error) || file_error) {
    std::cerr << "failed file read retained its native handle\n";
    return 1;
  }

#if !defined(_WIN32)
  const std::filesystem::path fifo_path = path_directory / "source.fifo";
  if (mkfifo(fifo_path.c_str(), 0600) != 0 ||
      file_read_status(fifo_path) != kClothFileReadNotRegular) {
    std::cerr << "non-regular file did not report its kind\n";
    return 1;
  }
  static_cast<void>(std::filesystem::remove(fifo_path, file_error));

  const std::filesystem::path denied_file = path_directory / "denied.bin";
  if (!write_file(denied_file, kBinaryText) ||
      chmod(denied_file.c_str(), 0000) != 0) {
    std::cerr << "could not create denied-path fixture\n";
    return 1;
  }
  const std::uint8_t denied_status = file_read_status(denied_file);
  if (chmod(denied_file.c_str(), 0600) != 0 ||
      (denied_status != kClothFileReadOpenError &&
       denied_status != kClothFileReadValue)) {
    std::cerr << "denied path produced an invalid result\n";
    return 1;
  }
  static_cast<void>(std::filesystem::remove(denied_file, file_error));
#endif

  file_error.clear();
  if (!std::filesystem::remove(named_file, file_error) || file_error ||
      !std::filesystem::remove(path_directory, file_error) || file_error) {
    std::cerr << "path-semantics fixtures could not be cleaned up\n";
    return 1;
  }

  managed_path = nullptr;
  first = nullptr;
  second = nullptr;
  cloth_rt_gc_collect();
  cloth_rt_gc_pop_frame(&frame);
  std::cout << "file read passed\n";
  return 0;
}

int runtime_failure_scenario(std::string_view scenario) {
  if (scenario == "nullable_value_absent") {
    cloth_rt_require_nullable_value(0);
  }
  if (scenario == "nullable_value_invalid") {
    cloth_rt_require_nullable_value(UINT8_MAX);
  }
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
  if (scenario == "parse_kind") {
    std::uint64_t bits = 0;
    void* text = cloth_rt_string_literal("1", 1);
    static_cast<void>(cloth_rt_parse_primitive(UINT8_MAX, text, &bits));
  }
  if (scenario == "parse_text") {
    std::uint64_t bits = 0;
    static_cast<void>(
        cloth_rt_parse_primitive(kClothParseInt32, nullptr, &bits));
  }
  if (scenario == "parse_bits") {
    void* text = cloth_rt_string_literal("1", 1);
    static_cast<void>(
        cloth_rt_parse_primitive(kClothParseInt32, text, nullptr));
  }
  if (scenario == "string_index_negative") {
    void* text = cloth_rt_string_literal("A", 1);
    static_cast<void>(cloth_rt_string_scalar_at(text, -1));
  }
  if (scenario == "string_index_length") {
    void* text = cloth_rt_string_literal("A", 1);
    static_cast<void>(cloth_rt_string_scalar_at(text, 1));
  }
  if (scenario == "string_index_empty") {
    void* text = cloth_rt_string_literal(nullptr, 0);
    static_cast<void>(cloth_rt_string_scalar_at(text, 0));
  }
  if (scenario == "string_index_after_length") {
    void* text = cloth_rt_string_literal("A", 1);
    static_cast<void>(cloth_rt_string_scalar_at(text, 2));
  }
  if (scenario == "string_iteration_offset") {
    void* text = cloth_rt_string_literal("A", 1);
    std::int32_t offset = 2;
    std::uint32_t scalar = 0;
    static_cast<void>(cloth_rt_string_next_scalar(text, &offset, &scalar));
  }
  if (scenario == "string_iteration_boundary") {
    constexpr char kText[] = "\xC3\xA9";
    void* text = cloth_rt_string_literal(kText, sizeof(kText) - 1);
    std::int32_t offset = 1;
    std::uint32_t scalar = 0;
    static_cast<void>(cloth_rt_string_next_scalar(text, &offset, &scalar));
  }
  if (scenario == "string_iteration_output") {
    void* text = cloth_rt_string_literal("A", 1);
    std::uint32_t scalar = 0;
    static_cast<void>(cloth_rt_string_next_scalar(text, nullptr, &scalar));
  }
  if (scenario.starts_with("string_slice_")) {
    constexpr char kText[] = "A\xF0\x9F\xA7\xB5Z";
    auto* text = static_cast<TestString*>(
        cloth_rt_string_literal(kText, sizeof(kText) - 1));
    if (scenario == "string_slice_null") {
      static_cast<void>(cloth_rt_string_slice(nullptr, 0, 0));
    } else if (scenario == "string_slice_negative_start") {
      static_cast<void>(cloth_rt_string_slice(text, -1, 1));
    } else if (scenario == "string_slice_negative_end") {
      static_cast<void>(cloth_rt_string_slice(text, 0, -1));
    } else if (scenario == "string_slice_reversed") {
      static_cast<void>(cloth_rt_string_slice(text, 2, 1));
    } else if (scenario == "string_slice_after_length") {
      static_cast<void>(cloth_rt_string_slice(text, 0, 4));
    } else if (scenario == "string_slice_large") {
      static_cast<void>(cloth_rt_string_slice(text, INT32_MAX, INT32_MAX));
    } else if (scenario == "string_slice_layout_size") {
      text->byte_size =
          static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) +
          1;
      static_cast<void>(cloth_rt_string_slice(text, 0, 1));
    } else if (scenario == "string_slice_layout_data") {
      text->data = nullptr;
      static_cast<void>(cloth_rt_string_slice(text, 0, 1));
    } else if (scenario == "string_slice_layout_encoding") {
      static constexpr char kInvalidUtf8[]{static_cast<char>(0xff)};
      text->data = kInvalidUtf8;
      text->byte_size = 1;
      text->scalar_count = 1;
      static_cast<void>(cloth_rt_string_slice(text, 0, 1));
    } else if (scenario == "string_slice_layout_suffix") {
      static constexpr char kInvalidSuffix[]{'A', static_cast<char>(0xc0),
                                             static_cast<char>(0xaf)};
      text->data = kInvalidSuffix;
      text->byte_size = sizeof(kInvalidSuffix);
      text->scalar_count = 2;
      static_cast<void>(cloth_rt_string_slice(text, 0, 1));
    } else if (scenario == "string_slice_layout_scalars") {
      text->scalar_count = 4;
      static_cast<void>(cloth_rt_string_slice(text, 0, 1));
    }
  }
  if (scenario == "string_layout_encoding") {
    auto* text = static_cast<TestString*>(cloth_rt_string_literal("A", 1));
    static constexpr char kInvalidUtf8[]{static_cast<char>(0xff)};
    text->data = kInvalidUtf8;
    static_cast<void>(cloth_rt_string_scalar_at(text, 0));
  }
  if (scenario == "string_layout_scalars") {
    auto* text = static_cast<TestString*>(cloth_rt_string_literal("A", 1));
    text->scalar_count = 2;
    std::int32_t offset = 1;
    std::uint32_t scalar = 0;
    static_cast<void>(cloth_rt_string_next_scalar(text, &offset, &scalar));
  }
  if (scenario.starts_with("parse_layout_")) {
    auto* text = static_cast<TestString*>(cloth_rt_string_literal("1", 1));
    if (scenario == "parse_layout_size") {
      text->byte_size =
          static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) +
          1;
    } else if (scenario == "parse_layout_data") {
      text->data = nullptr;
    } else if (scenario == "parse_layout_encoding") {
      static constexpr char kInvalidUtf8[]{static_cast<char>(0xff)};
      text->data = kInvalidUtf8;
    } else if (scenario == "parse_layout_scalars") {
      text->scalar_count = 2;
    }
    std::uint64_t bits = UINT64_MAX;
    static_cast<void>(cloth_rt_parse_primitive(kClothParseInt32, text, &bits));
  }
  if (scenario == "console_status") {
    static_cast<void>(cloth_rt_console_read_line(nullptr));
  }
  if (scenario == "file_status") {
    void* path = cloth_rt_string_literal("missing", 7);
    static_cast<void>(cloth_rt_file_read_bytes(path, nullptr));
  }
  if (scenario == "file_path") {
    std::uint8_t status = UINT8_MAX;
    static_cast<void>(cloth_rt_file_read_bytes(nullptr, &status));
  }
  if (scenario == "file_path_layout") {
    auto* path = static_cast<TestString*>(cloth_rt_string_literal("a", 1));
    path->scalar_count = 2;
    std::uint8_t status = UINT8_MAX;
    static_cast<void>(cloth_rt_file_read_bytes(path, &status));
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
  if (argc == 3 && std::string_view{argv[1]} == "file_read") {
    return file_read_scenario(argv[2]);
  }
  if (argc == 2 && std::string_view{argv[1]} == "emit_console_edges") {
    return emit_console_edge_input();
  }
  if (argc == 2 && std::string_view{argv[1]} == "console_input") {
    const std::array<std::string, 4> expected{
        "first", "", "\xC3\xA9\xF0\x9F\x99\x82", "last"};
    return console_input_scenario(expected);
  }
  if (argc == 2 && std::string_view{argv[1]} == "console_edges") {
    const std::vector<std::string> expected = console_edge_lines();
    return console_input_scenario(expected);
  }
  if (argc == 2 && std::string_view{argv[1]} == "console_invalid") {
    return console_invalid_encoding_scenario();
  }
  if (argc == 2 && std::string_view{argv[1]} == "console_io") {
    return console_io_failure_scenario();
  }
  if (argc == 2) return runtime_failure_scenario(argv[1]);
  TestContext test{"runtime"};
  cloth_rt_require_nullable_value(1);
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
  void* slice_source = nullptr;
  void* slice_result = nullptr;
  void* slice_expected = nullptr;
  void* long_slice_source = nullptr;
  void* long_slice_result = nullptr;
  void** string_roots[]{&empty_string,      &unicode_string,   &left_string,
                        &right_string,      &joined_string,    &expected_string,
                        &slice_source,      &slice_result,     &slice_expected,
                        &long_slice_source, &long_slice_result};
  cloth_rt_gc_push_frame(&string_frame, string_roots, 11);

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
  test.expect(cloth_rt_string_scalar_at(unicode_string, 0) == 0x00e9U &&
                  cloth_rt_string_scalar_at(unicode_string, 1) == 0x1f642U,
              "string scalar indexing decoded the wrong values");
  std::int32_t byte_offset = 0;
  std::uint32_t scalar = UINT32_MAX;
  test.expect(
      cloth_rt_string_next_scalar(unicode_string, &byte_offset, &scalar) == 1 &&
          byte_offset == 2 && scalar == 0x00e9U &&
          cloth_rt_string_next_scalar(unicode_string, &byte_offset, &scalar) ==
              1 &&
          byte_offset == 6 && scalar == 0x1f642U &&
          cloth_rt_string_next_scalar(unicode_string, &byte_offset, &scalar) ==
              0 &&
          byte_offset == 6 && scalar == 0,
      "string scalar iteration violated its cursor contract");

  std::string slice_text{"A\0", 2};
  slice_text.append(
      "\xC3\xA9\xF0\x9F\xA7\xB5"
      "e\xCC\x81\xF4\x8F\xBF\xBFZ");
  slice_source = cloth_rt_string_literal(slice_text.data(), slice_text.size());
  const auto expect_slice = [&](std::int32_t start, std::int32_t end,
                                const std::string& expected,
                                std::string_view description) {
    slice_result = cloth_rt_string_slice(slice_source, start, end);
    slice_expected = cloth_rt_string_literal(expected.data(), expected.size());
    test.expect(cloth_rt_string_equal(slice_result, slice_expected) == 1 &&
                    cloth_rt_string_length(slice_result) == end - start,
                description);
  };
  expect_slice(0, 0, "", "zero-width string slice at zero is wrong");
  expect_slice(2, 2, "", "zero-width string slice in the middle is wrong");
  expect_slice(8, 8, "", "zero-width string slice at length is wrong");
  expect_slice(0, 2, std::string{"A\0", 2},
               "string slice lost embedded U+0000");
  expect_slice(6, 8, "\xF4\x8F\xBF\xBFZ",
               "string slice lost U+10FFFF or its suffix");
  expect_slice(2, 6,
               "\xC3\xA9\xF0\x9F\xA7\xB5"
               "e\xCC\x81",
               "mixed-width and combining string slice is wrong");
  expect_slice(3, 4, "\xF0\x9F\xA7\xB5", "one-scalar string slice is wrong");
  expect_slice(0, 8, slice_text, "complete string slice changed content");

  cloth_rt_gc_collect();
  constexpr std::size_t kLongSliceSize = 128 * 1024;
  std::string long_slice_text(kLongSliceSize, 'x');
  long_slice_source =
      cloth_rt_string_literal(long_slice_text.data(), long_slice_text.size());
  const std::uint64_t slice_collections_before = cloth_rt_gc_collection_count();
  long_slice_result = cloth_rt_string_slice(
      long_slice_source, 1, static_cast<std::int32_t>(kLongSliceSize - 1));
  test.expect(cloth_rt_gc_collection_count() > slice_collections_before &&
                  cloth_rt_string_length(long_slice_result) ==
                      static_cast<std::int32_t>(kLongSliceSize - 2) &&
                  cloth_rt_string_scalar_at(long_slice_result, 0) == 'x' &&
                  cloth_rt_string_scalar_at(
                      long_slice_result,
                      static_cast<std::int32_t>(kLongSliceSize - 3)) == 'x',
              "allocating string slice did not preserve its rooted source");

  constexpr std::size_t kLongScalarCount = 4096;
  constexpr std::string_view kThread = "\xF0\x9F\xA7\xB5";
  std::string long_text;
  long_text.reserve(kLongScalarCount * kThread.size());
  for (std::size_t index = 0; index < kLongScalarCount; ++index) {
    long_text.append(kThread);
  }
  void* long_string =
      cloth_rt_string_literal(long_text.data(), long_text.size());
  byte_offset = 0;
  std::size_t visited = 0;
  bool cursor_is_monotonic = true;
  while (cloth_rt_string_next_scalar(long_string, &byte_offset, &scalar) != 0) {
    ++visited;
    cursor_is_monotonic =
        cursor_is_monotonic &&
        byte_offset == static_cast<std::int32_t>(visited * kThread.size()) &&
        scalar == UINT32_C(0x1f9f5);
  }
  test.expect(
      cursor_is_monotonic && visited == kLongScalarCount &&
          byte_offset == static_cast<std::int32_t>(long_text.size()) &&
          scalar == 0 &&
          cloth_rt_string_scalar_at(
              long_string, static_cast<std::int32_t>(kLongScalarCount - 1)) ==
              UINT32_C(0x1f9f5),
      "long string traversal did not advance one UTF-8 cursor");

  const ParseResult parsed_true = parse(kClothParseBool, "true");
  const ParseResult parsed_false = parse(kClothParseBool, "false");
  const ParseResult invalid_bool = parse(kClothParseBool, "True");
  test.expect(
      parsed_true.status == kClothParseValue && parsed_true.bits == 1 &&
          parsed_false.status == kClothParseValue && parsed_false.bits == 0 &&
          invalid_bool.status == kClothParseInvalid && invalid_bool.bits == 0,
      "strict bool parsing is incorrect");

  const ParseResult parsed_char = parse(kClothParseChar, "\xF0\x9F\x99\x82");
  const char nul[]{'\0'};
  const ParseResult parsed_nul =
      parse(kClothParseChar, std::string_view{nul, sizeof(nul)});
  test.expect(parsed_char.status == kClothParseValue &&
                  parsed_char.bits == UINT32_C(0x1f642) &&
                  parsed_nul.status == kClothParseValue &&
                  parsed_nul.bits == 0 &&
                  parse(kClothParseChar, "ab").status == kClothParseInvalid &&
                  parse(kClothParseChar, "").status == kClothParseInvalid,
              "Unicode scalar parsing is incorrect");

  test.expect(
      parse(kClothParseByte, "255").bits == 255 &&
          parse(kClothParseByte, "256").status == kClothParseOutOfRange &&
          parse(kClothParseInt8, "-128").bits == UINT8_C(0x80) &&
          parse(kClothParseInt8, "128").status == kClothParseOutOfRange &&
          parse(kClothParseInt16, "-32768").bits == UINT16_C(0x8000) &&
          parse(kClothParseInt32, "2_147_483_647").bits ==
              UINT32_C(2147483647) &&
          parse(kClothParseInt64, "-9223372036854775808").bits ==
              UINT64_C(0x8000000000000000) &&
          parse(kClothParseUint8, "0xff").bits == UINT8_C(0xff) &&
          parse(kClothParseUint16, "0b1111_0000").bits == UINT16_C(0xf0) &&
          parse(kClothParseUint32, "0o377").bits == UINT32_C(255) &&
          parse(kClothParseUint64, "18446744073709551615").bits == UINT64_MAX,
      "primitive integer widths, signs, bases, or separators are incorrect");
  test.expect(
      parse(kClothParseUint32, "-0").status == kClothParseInvalid &&
          parse(kClothParseInt32, "0X10").status == kClothParseInvalid &&
          parse(kClothParseInt32, "1__0").status == kClothParseInvalid &&
          parse(kClothParseInt32, " 10").status == kClothParseInvalid &&
          parse(kClothParseInt32, "10i32").status == kClothParseInvalid,
      "integer parsing accepted noncanonical text");

  struct ParseCase {
    std::uint8_t kind;
    std::string_view text;
    std::uint8_t status;
    std::uint64_t bits;
  };
  const std::vector<ParseCase> integer_cases{
      {kClothParseByte, "+0", kClothParseValue, 0},
      {kClothParseByte, "255", kClothParseValue, UINT8_MAX},
      {kClothParseByte, "256", kClothParseOutOfRange, 0},
      {kClothParseInt8, "-128", kClothParseValue, UINT8_C(0x80)},
      {kClothParseInt8, "127", kClothParseValue, UINT8_C(0x7f)},
      {kClothParseInt8, "-129", kClothParseOutOfRange, 0},
      {kClothParseInt8, "128", kClothParseOutOfRange, 0},
      {kClothParseInt16, "-32768", kClothParseValue, UINT16_C(0x8000)},
      {kClothParseInt16, "32767", kClothParseValue, UINT16_C(0x7fff)},
      {kClothParseInt16, "-32769", kClothParseOutOfRange, 0},
      {kClothParseInt16, "32768", kClothParseOutOfRange, 0},
      {kClothParseInt32, "-2147483648", kClothParseValue, UINT32_C(0x80000000)},
      {kClothParseInt32, "2147483647", kClothParseValue, UINT32_C(0x7fffffff)},
      {kClothParseInt32, "-2147483649", kClothParseOutOfRange, 0},
      {kClothParseInt32, "2147483648", kClothParseOutOfRange, 0},
      {kClothParseInt64, "-9223372036854775808", kClothParseValue,
       UINT64_C(0x8000000000000000)},
      {kClothParseInt64, "9223372036854775807", kClothParseValue,
       UINT64_C(0x7fffffffffffffff)},
      {kClothParseInt64, "-9223372036854775809", kClothParseOutOfRange, 0},
      {kClothParseInt64, "9223372036854775808", kClothParseOutOfRange, 0},
      {kClothParseUint8, "255", kClothParseValue, UINT8_MAX},
      {kClothParseUint8, "256", kClothParseOutOfRange, 0},
      {kClothParseUint16, "65535", kClothParseValue, UINT16_MAX},
      {kClothParseUint16, "65536", kClothParseOutOfRange, 0},
      {kClothParseUint32, "4294967295", kClothParseValue, UINT32_MAX},
      {kClothParseUint32, "4294967296", kClothParseOutOfRange, 0},
      {kClothParseUint64, "18446744073709551615", kClothParseValue, UINT64_MAX},
      {kClothParseUint64, "18446744073709551616", kClothParseOutOfRange, 0},
      {kClothParseInt8, "-0x80", kClothParseValue, UINT8_C(0x80)},
      {kClothParseUint32, "+0X10", kClothParseInvalid, 0},
      {kClothParseUint32, "+0xFf", kClothParseValue, UINT32_C(0xff)},
      {kClothParseUint32, "0b", kClothParseInvalid, 0},
      {kClothParseUint32, "0o8", kClothParseInvalid, 0},
      {kClothParseUint32, "0xg", kClothParseInvalid, 0},
      {kClothParseUint32, "_1", kClothParseInvalid, 0},
      {kClothParseUint32, "1_", kClothParseInvalid, 0},
      {kClothParseUint32, "1__0", kClothParseInvalid, 0},
      {kClothParseUint32, "1 0", kClothParseInvalid, 0},
      {kClothParseUint32, "10//comment", kClothParseInvalid, 0},
      {kClothParseUint32, std::string_view{"1\0", 2}, kClothParseInvalid, 0},
      {kClothParseUint32,
       "\xef\xbb\xbf"
       "1",
       kClothParseInvalid, 0},
      {kClothParseUint32, "\xd9\xa1", kClothParseInvalid, 0},
      {kClothParseUint32, "1u32", kClothParseInvalid, 0},
      {kClothParseUint32, "-0", kClothParseInvalid, 0},
  };
  for (const ParseCase& parse_case : integer_cases) {
    const ParseResult result = parse(parse_case.kind, parse_case.text);
    test.expect(
        result.status == parse_case.status && result.bits == parse_case.bits,
        "integer parse boundary matrix failed");
  }

  const ParseResult parsed_f32 = parse(kClothParseFloat32, "1.5");
  const ParseResult parsed_f64 = parse(kClothParseFloat64, "1_2.5e+1");
  const ParseResult negative_zero = parse(kClothParseFloat64, "-0e999999");
  const ParseResult f32_halfway =
      parse(kClothParseFloat32, "1.000000059604644775390625");
  const ParseResult f32_above_halfway =
      parse(kClothParseFloat32, "1.000000059604644775390626");
  const ParseResult f32_upper_even_halfway =
      parse(kClothParseFloat32, "1.000000178813934326171875");
  const ParseResult f64_halfway =
      parse(kClothParseFloat64,
            "1.00000000000000011102230246251565404236316680908203125");
  test.expect(
      parsed_f32.status == kClothParseValue &&
          parsed_f32.bits == std::bit_cast<std::uint32_t>(1.5F) &&
          parsed_f64.status == kClothParseValue &&
          parsed_f64.bits == std::bit_cast<std::uint64_t>(125.0) &&
          negative_zero.status == kClothParseValue &&
          negative_zero.bits == UINT64_C(0x8000000000000000) &&
          f32_halfway.status == kClothParseValue &&
          f32_halfway.bits == std::bit_cast<std::uint32_t>(1.0F) &&
          f32_above_halfway.status == kClothParseValue &&
          f32_above_halfway.bits == UINT32_C(0x3f800001) &&
          f32_upper_even_halfway.status == kClothParseValue &&
          f32_upper_even_halfway.bits == UINT32_C(0x3f800002) &&
          f64_halfway.status == kClothParseValue &&
          f64_halfway.bits == std::bit_cast<std::uint64_t>(1.0) &&
          parse(kClothParseFloat32, "1e-45").status == kClothParseValue &&
          parse(kClothParseFloat32, "1e-10000").status ==
              kClothParseOutOfRange &&
          parse(kClothParseFloat32, "3.5e38").status == kClothParseOutOfRange &&
          parse(kClothParseFloat64, "inf").status == kClothParseInvalid &&
          parse(kClothParseFloat64, "0x1").status == kClothParseInvalid,
      "strict floating-point parsing is incorrect");

  const std::vector<ParseCase> float_cases{
      {kClothParseFloat32, "+12", kClothParseValue,
       std::bit_cast<std::uint32_t>(12.0F)},
      {kClothParseFloat32, "1.401298464324817e-45", kClothParseValue,
       UINT32_C(0x00000001)},
      {kClothParseFloat32, "1.1754943508222875e-38", kClothParseValue,
       UINT32_C(0x00800000)},
      {kClothParseFloat32, "3.4028234663852886e38", kClothParseValue,
       UINT32_C(0x7f7fffff)},
      {kClothParseFloat64, "4.9406564584124654e-324", kClothParseValue,
       UINT64_C(0x0000000000000001)},
      {kClothParseFloat64, "2.2250738585072014e-308", kClothParseValue,
       UINT64_C(0x0010000000000000)},
      {kClothParseFloat64, "1.7976931348623157e308", kClothParseValue,
       UINT64_C(0x7fefffffffffffff)},
      {kClothParseFloat64, "0e-999999", kClothParseValue, 0},
      {kClothParseFloat64, "-0e+999999", kClothParseValue,
       UINT64_C(0x8000000000000000)},
      {kClothParseFloat32, "1e-10000", kClothParseOutOfRange, 0},
      {kClothParseFloat32, "1e+10000", kClothParseOutOfRange, 0},
      {kClothParseFloat64, ".5", kClothParseInvalid, 0},
      {kClothParseFloat64, "1.", kClothParseInvalid, 0},
      {kClothParseFloat64, "1e", kClothParseInvalid, 0},
      {kClothParseFloat64, "1e+", kClothParseInvalid, 0},
      {kClothParseFloat64, "1__0.0", kClothParseInvalid, 0},
      {kClothParseFloat64, "1.0_f64", kClothParseInvalid, 0},
      {kClothParseFloat64, " 1.0", kClothParseInvalid, 0},
      {kClothParseFloat64, "1,0", kClothParseInvalid, 0},
      {kClothParseFloat64, std::string_view{"1\0.0", 5}, kClothParseInvalid, 0},
      {kClothParseFloat64, "nan", kClothParseInvalid, 0},
      {kClothParseFloat64, "-inf", kClothParseInvalid, 0},
      {kClothParseFloat64, "0x1.0p0", kClothParseInvalid, 0},
  };
  for (const ParseCase& parse_case : float_cases) {
    const ParseResult result = parse(parse_case.kind, parse_case.text);
    test.expect(
        result.status == parse_case.status && result.bits == parse_case.bits,
        "floating-point parse boundary matrix failed");
  }

  const int initial_rounding = std::fegetround();
  test.expect(initial_rounding != -1 && std::fesetround(FE_UPWARD) == 0,
              "host rounding mode could not be changed for parsing audit");
  const ParseResult upward_f32 =
      parse(kClothParseFloat32, "1.000000059604644775390625");
  const ParseResult upward_f64 =
      parse(kClothParseFloat64,
            "1.00000000000000011102230246251565404236316680908203125");
  test.expect(upward_f32.status == kClothParseValue &&
                  upward_f32.bits == std::bit_cast<std::uint32_t>(1.0F) &&
                  upward_f64.status == kClothParseValue &&
                  upward_f64.bits == std::bit_cast<std::uint64_t>(1.0),
              "floating parsing depended on the host rounding mode");
  test.expect(std::fesetround(initial_rounding) == 0,
              "host rounding mode was not restored after parsing audit");

  const std::vector<ParseCase> bool_and_char_cases{
      {kClothParseBool, "true", kClothParseValue, 1},
      {kClothParseBool, "false", kClothParseValue, 0},
      {kClothParseBool, "TRUE", kClothParseInvalid, 0},
      {kClothParseBool, "0", kClothParseInvalid, 0},
      {kClothParseBool, " false", kClothParseInvalid, 0},
      {kClothParseBool, std::string_view{"true\0", 5}, kClothParseInvalid, 0},
      {kClothParseChar, "A", kClothParseValue, UINT32_C(0x41)},
      {kClothParseChar, "\xc3\xa9", kClothParseValue, UINT32_C(0xe9)},
      {kClothParseChar, "\xcc\x81", kClothParseValue, UINT32_C(0x301)},
      {kClothParseChar, "e\xcc\x81", kClothParseInvalid, 0},
      {kClothParseChar, "\xf0\x9f\x99\x82", kClothParseValue,
       UINT32_C(0x1f642)},
      {kClothParseChar, std::string_view{"\0", 1}, kClothParseValue, 0},
      {kClothParseChar, "", kClothParseInvalid, 0},
      {kClothParseChar, "ab", kClothParseInvalid, 0},
  };
  for (const ParseCase& parse_case : bool_and_char_cases) {
    const ParseResult result = parse(parse_case.kind, parse_case.text);
    test.expect(
        result.status == parse_case.status && result.bits == parse_case.bits,
        "boolean or character parse matrix failed");
  }

  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 11 && cloth_rt_gc_live_bytes() != 0,
              "marking did not preserve rooted strings");
  empty_string = nullptr;
  unicode_string = nullptr;
  left_string = nullptr;
  right_string = nullptr;
  joined_string = nullptr;
  expected_string = nullptr;
  slice_source = nullptr;
  slice_result = nullptr;
  slice_expected = nullptr;
  long_slice_source = nullptr;
  long_slice_result = nullptr;
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

  constexpr std::uint64_t kNullableInlineOffsets[]{
      offsetof(NullableInlineValue, payload) + offsetof(InlineValue, first),
      offsetof(NullableInlineValue, payload) + offsetof(InlineValue, second)};
  const ClothArrayElementLayout nullable_inline_layout{
      sizeof(NullableInlineValue), alignof(NullableInlineValue),
      kNullableInlineOffsets, 2};
  NullableInlineValue nullable_local{1, {7, nullptr, 9, nullptr}};
  void* nullable_array = nullptr;
  void** nullable_roots[]{&nullable_array, &nullable_local.payload.first,
                          &nullable_local.payload.second};
  ClothGcRootFrame nullable_frame{};
  cloth_rt_gc_push_frame(&nullable_frame, nullable_roots, 3);
  nullable_local.payload.first = cloth_rt_alloc(&node_type);
  nullable_local.payload.second = cloth_rt_string_literal("nullable", 8);
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 2,
              "nullable aggregate payload roots were not retained");

  nullable_array = cloth_rt_array_alloc(1, &nullable_inline_layout);
  NullableInlineValue nullable_zeroed{};
  std::memcpy(&nullable_zeroed, cloth_rt_array_element(nullable_array, 0),
              sizeof(nullable_zeroed));
  test.expect(nullable_zeroed.tag == 0 &&
                  nullable_zeroed.payload.first == nullptr &&
                  nullable_zeroed.payload.second == nullptr,
              "absent nullable aggregate payload was not zeroed");
  std::memcpy(cloth_rt_array_element(nullable_array, 0), &nullable_local,
              sizeof(nullable_local));
  nullable_local = {};
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 3,
              "shifted nullable aggregate map lost a present payload");
  std::memcpy(cloth_rt_array_element(nullable_array, 0), &nullable_zeroed,
              sizeof(nullable_zeroed));
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 1,
              "absent nullable aggregate retained stale payload roots");
  nullable_array = nullptr;
  cloth_rt_gc_collect();
  test.expect(cloth_rt_gc_live_objects() == 0 && cloth_rt_gc_live_bytes() == 0,
              "nullable aggregate array storage was not reclaimed");
  cloth_rt_gc_pop_frame(&nullable_frame);

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
    std::cout << "10 tests passed\n";
    return 0;
  }
  return 1;
}
