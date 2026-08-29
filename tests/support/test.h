#ifndef CLOTH_TESTS_SUPPORT_TEST_H_
#define CLOTH_TESTS_SUPPORT_TEST_H_

#include <iostream>
#include <span>
#include <string_view>

namespace cloth::test {

class TestContext {
 public:
  explicit TestContext(std::string_view name = {}) : name_(name) {}

  void expect(bool condition, std::string_view message) {
    if (condition) {
      return;
    }
    ++failures_;
    if (!name_.empty()) {
      std::cerr << "  " << name_ << ": ";
    }
    std::cerr << message << '\n';
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  std::string_view name_;
  int failures_{0};
};

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

inline int run_tests(std::span<const TestCase> tests) {
  int failures = 0;
  for (const TestCase& test_case : tests) {
    TestContext context{test_case.name};
    test_case.function(context);
    if (context.failures() == 0) {
      std::cout << "[pass] " << test_case.name << '\n';
    } else {
      std::cout << "[fail] " << test_case.name << '\n';
      failures += context.failures();
    }
  }

  if (failures == 0) {
    std::cout << tests.size() << " tests passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}

}  // namespace cloth::test

#endif  // CLOTH_TESTS_SUPPORT_TEST_H_
