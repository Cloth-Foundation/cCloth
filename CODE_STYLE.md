# C++ Code Style

This repository follows the Google C++ Style Guide as the baseline formatting and naming standard, with explicit adjustments for the project's use of C++23.

The goal is to keep code readable, predictable, and consistent across the codebase while making use of modern standard-library facilities and safe C++ idioms.

## 1. Language standard

- The project target is C++23.
- Do not use compiler-specific language extensions as a substitute for standard C++23 features.
- Use the standard library before introducing third-party dependencies.
- Prefer standard-library facilities such as `std::optional`, `std::variant`, `std::expected`, `std::span`, ranges, and `std::filesystem` when they improve correctness and readability.
- Keep the code portable across supported toolchains. If a feature is not available in the minimum supported compiler, do not use it.

In CMake, this is enforced with:

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

## 2. Formatting

### 2.1 Indentation and line length

- Use 2-space indentation.
- Use ASCII only.
- Prefer a maximum line length of 80 columns; allow longer lines only when readability is clearly better or when a long literal or declaration is unavoidable.
- Break long function signatures, parameter lists, and return types cleanly across lines.

### 2.2 Braces and control flow

- Opening braces stay on the same line as the declaration or control statement.
- Use a blank line between sections of a function only when it improves readability.
- Use `if`, `for`, `while`, and `switch` blocks with braces even when the body is a single statement.
- Prefer early returns over deep nesting.
- For `else` blocks, keep the `else` on the same line as the closing brace of the preceding `if`.

Example:

```cpp
if (condition) {
  DoWork();
  return;
} else {
  DoOtherWork();
}
```

### 2.3 Namespaces

- Use named namespaces, not anonymous namespaces except for implementation details that truly require local linkage.
- Keep namespace blocks compact and close to their related definitions.
- Do not use `using namespace ...` in headers or broadly in implementation files.

Example:

```cpp
namespace project {
namespace io {

class Reader {
 public:
  explicit Reader(std::string path);
};

}  // namespace io
}  // namespace project
```

### 2.4 Includes

- Include the header that declares the item being defined before other includes.
- Group includes in this order:
  1. corresponding header
  2. project headers
  3. third-party headers
  4. standard library headers
- Use angle brackets for system headers and quotes for project headers.
- Do not include unused headers.

Example:

```cpp
#include "project/io/reader.h"

#include <string>
#include <vector>
```

## 3. Naming

Follow the Google C++ naming conventions, with the following repository-specific clarifications.

### 3.1 Files

- Use lowercase file names with underscores.
- Use `.h` for headers and `.cc` for implementation files.
- Use matching names for declarations and definitions.

Examples:

- `file_reader.h`
- `file_reader.cc`
- `http_client.h`

### 3.2 Types and classes

- Use `PascalCase` for classes, structs, enums, and type aliases.
- Use `PascalCase` for template parameter names when they represent types.
- Use `kCamelCase` for enum values.

Examples:

```cpp
class FileReader {
 public:
  explicit FileReader(std::string path);
};

enum class Status {
  kOk,
  kError,
};
```

### 3.3 Functions

- Use `snake_case` for function names.
- Use `snake_case` for local variables and parameters.
- Use `snake_case` for member functions.
- Use `lowercase` acronyms in names when they are not type names: `parse_url`, not `parseURL`.

Examples:

```cpp
std::string read_file(std::string_view path);
void set_error_code(int error_code);
```

### 3.4 Constants and constexpr values

- Use `kCamelCase` for constants and compile-time constants.

Example:

```cpp
constexpr int kMaxRetries = 3;
```

### 3.5 Data members

- Private and protected member variables should end with a trailing underscore.
- Do not use Hungarian notation.
- Use names that explain the meaning, not the implementation.

Example:

```cpp
class Connection {
 private:
  std::string host_;
  int port_ = 0;
};
```

### 3.6 Macros

- Avoid macros whenever possible.
- If a macro is unavoidable, use all-caps names with underscores.
- Prefer `constexpr`, `enum class`, and `inline` functions instead of macros.

## 4. Type and ownership rules

- Prefer value semantics over pointer semantics.
- Use `std::unique_ptr` when ownership is exclusive.
- Use `std::shared_ptr` only when true shared ownership is required.
- Use `std::optional`, `std::variant`, and `std::expected` instead of sentinel values and ad hoc status flags when they make intent clearer.
- Use `std::span` and `std::string_view` for non-owning views.
- Avoid raw owning pointers and manual memory management.
- Do not use `NULL`; use `nullptr`.

## 5. Classes and interfaces

- Use classes to represent cohesive abstractions and data ownership.
- Prefer composition over inheritance.
- Mark single-argument constructors as `explicit` unless implicit conversion is intentionally required.
- Prefer defaulted special member functions and the rule-of-zero where appropriate.
- Use `final` only when the class is intentionally non-derivable.
- Keep public interfaces minimal and clear.

Example:

```cpp
class Buffer {
 public:
  explicit Buffer(std::size_t capacity);

  [[nodiscard]] std::size_t size() const noexcept;
  void append(std::string_view data);

 private:
  std::vector<char> data_;
};
```

## 6. Error handling and control flow

- Prefer returning a value or result object over throwing exceptions when the failure mode is part of normal control flow.
- Use exceptions only when they are the clearest and most maintainable mechanism for truly exceptional cases.
- When an error can be represented by the standard library, prefer `std::optional`, `std::expected`, or a domain-specific status enum.
- Validate inputs at function boundaries and fail fast.
- Do not suppress errors silently.
- Use assertions for invariants and impossible states only; do not use them to handle user input or external system errors.

## 7. Comments and documentation

- Write comments only when they explain intent, constraints, or non-obvious behavior.
- Prefer self-explanatory code over excessive inline comments.
- Public interfaces should have brief comments describing purpose and lifecycle assumptions.
- Use `//` comments for regular documentation; avoid block comments unless required for disabling code.
- Document dangerous behavior, invariants, performance guarantees, and ownership assumptions.

Example:

```cpp
// Must be called before any reads to ensure the file mapping is initialized.
void initialize();
```

## 8. Modern C++23 practices

- Prefer range-based algorithms and `std::ranges` when they simplify logic.
- Use `std::string_view` instead of `const std::string&` for read-only string inputs unless ownership is required.
- Use `std::array`, `std::vector`, `std::optional`, and `std::unordered_map` with clear ownership semantics.
- Use `std::move` only when transferring ownership or avoiding copies.
- Avoid unnecessary `auto` when the type is not obvious; prefer explicit types for public APIs and interfaces.
- Do not write code that depends on undefined behavior or implementation-specific compiler quirks.

## 9. Testing and review expectations

- Every non-trivial change should have a matching test or update to an existing test.
- New code should be deterministic, easy to reason about, and free from hidden state.
- Keep functions small and cohesive. Large functions are a code smell.
- When modifying existing code, preserve the surrounding style unless the change is specifically intended to align the file with this guide.

## 10. Enforcement summary

When in doubt, follow this order:

1. correctness
2. readability
3. Google C++ style
4. C++23 standard-library idioms
5. local convenience

This keeps the codebase consistent, modern, and maintainable while staying aligned with the Google C++ formatting conventions.
