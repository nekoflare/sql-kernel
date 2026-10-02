// Test runner: executes every TEST(...) body registered by the test files.

#include <exception>
#include <iostream>

#include "test_harness.hpp"

int main() {
  for (const testing::TestCase& test : testing::registry()) {
    const int failures_before = testing::failure_count();
    try {
      test.body();
    } catch (const std::exception& e) {
      ++testing::failure_count();
      std::cerr << "unexpected exception escaped " << test.name << ": "
                << e.what() << "\n";
    }
    if (testing::failure_count() != failures_before) {
      std::cerr << "FAILED: " << test.name << "\n";
    }
  }

  if (testing::failure_count() == 0) {
    std::cout << "OK: " << testing::registry().size() << " tests, "
              << testing::check_count() << " checks\n";
    return 0;
  }
  std::cout << "FAILED: " << testing::failure_count() << " of "
            << testing::check_count() << " checks failed\n";
  return 1;
}
