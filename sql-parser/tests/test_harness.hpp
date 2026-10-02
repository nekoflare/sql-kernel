// Minimal self-registering test harness: a TEST(...) body registers itself at
// static-initialisation time and main() (tests/main.cpp) runs everything.
// No external test framework is required.

#pragma once

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sql_parser/ast.hpp"
#include "sql_parser/error.hpp"
#include "sql_parser/parser.hpp"

namespace testing {

struct TestCase {
  std::string name;
  void (*body)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& check_count() {
  static int count = 0;
  return count;
}

inline int& failure_count() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, void (*body)()) {
    registry().push_back({name, body});
  }
};

inline void report_failure(const char* file, int line,
                           const std::string& what) {
  ++failure_count();
  std::cerr << file << ":" << line << ": " << what << "\n";
}

// Generic value rendering for CHECK_EQ diagnostics.
template <typename T>
std::string display(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string display(const std::string& value) {
  return "\"" + value + "\"";
}

inline std::string display(const char* value) {
  return "\"" + std::string(value) + "\"";
}

inline std::string display(const sql::Value& value) {
  return sql::to_string(value);
}

}  // namespace testing

// ---------------------------------------------------------------------------
// Statements helpers shared by every test file.
// ---------------------------------------------------------------------------

inline sql::Statement parse_sql(const std::string& sql_text) {
  return sql::Parser(sql_text).parse();
}

inline sql::SelectStatement parse_select(const std::string& sql_text) {
  return std::get<sql::SelectStatement>(parse_sql(sql_text));
}

inline sql::InsertStatement parse_insert(const std::string& sql_text) {
  return std::get<sql::InsertStatement>(parse_sql(sql_text));
}

inline sql::UpdateStatement parse_update(const std::string& sql_text) {
  return std::get<sql::UpdateStatement>(parse_sql(sql_text));
}

inline sql::DeleteStatement parse_delete(const std::string& sql_text) {
  return std::get<sql::DeleteStatement>(parse_sql(sql_text));
}

// Literal payload of a Literal expression; throws if the expression is not a
// literal at all (so tests fail loudly instead of comparing garbage).
inline sql::Value lit(const sql::Expr& expr) {
  if (expr.kind != sql::Expr::Kind::Literal) {
    throw std::runtime_error("lit(): expression is not a literal");
  }
  return expr.literal;
}

// ---------------------------------------------------------------------------
// Assertion macros.
// ---------------------------------------------------------------------------

#define TEST(name)                                               \
  static void name();                                            \
  static ::testing::Registrar name##_registrar(#name, &name);    \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++::testing::check_count();                                           \
    if (!(cond)) {                                                        \
      ::testing::report_failure(__FILE__, __LINE__,                       \
                                "CHECK failed: " #cond);                  \
    }                                                                     \
  } while (0)

#define CHECK_EQ(actual, expected)                                        \
  do {                                                                    \
    ++::testing::check_count();                                           \
    const auto& actual_value = (actual);                                  \
    const auto& expected_value = (expected);                              \
    if (!(actual_value == expected_value)) {                              \
      std::ostringstream detail;                                          \
      detail << "CHECK_EQ failed: " #actual " ("                          \
             << ::testing::display(actual_value) << ") != " #expected     \
             << " (" << ::testing::display(expected_value) << ")";        \
      ::testing::report_failure(__FILE__, __LINE__, detail.str());        \
    }                                                                     \
  } while (0)

// Compares two sql::Value constants and prints them canonically.
#define CHECK_VALUE(actual, expected)                                     \
  do {                                                                    \
    ++::testing::check_count();                                           \
    const sql::Value actual_value = (actual);                             \
    const sql::Value expected_value = (expected);                         \
    if (!(actual_value == expected_value)) {                              \
      std::ostringstream detail;                                          \
      detail << "CHECK_VALUE failed: " << sql::to_string(actual_value)    \
             << " != " << sql::to_string(expected_value);                 \
      ::testing::report_failure(__FILE__, __LINE__, detail.str());        \
    }                                                                     \
  } while (0)

// Expects `expr` to throw sql::ParseError whose message contains `needle`.
#define CHECK_THROWS_MSG(expr, needle)                                    \
  do {                                                                    \
    ++::testing::check_count();                                           \
    bool thrown = false;                                                  \
    try {                                                                 \
      (void)(expr);                                                       \
    } catch (const sql::ParseError& e) {                                  \
      thrown = true;                                                      \
      if (std::string(e.what()).find(needle) == std::string::npos) {      \
        std::ostringstream detail;                                        \
        detail << "message \"" << e.what() << "\" does not contain \""    \
               << (needle) << "\"";                                       \
        ::testing::report_failure(__FILE__, __LINE__, detail.str());      \
      }                                                                   \
    } catch (const std::exception& e) {                                   \
      thrown = true;                                                      \
      std::ostringstream detail;                                          \
      detail << "expected sql::ParseError, got: " << e.what();            \
      ::testing::report_failure(__FILE__, __LINE__, detail.str());        \
    }                                                                     \
    if (!thrown) {                                                        \
      ::testing::report_failure(__FILE__, __LINE__,                       \
                                "expected sql::ParseError from: " #expr); \
    }                                                                     \
  } while (0)

// parse -> format -> parse: the tree must be identical and the printer must
// be idempotent on its own output.
#define CHECK_ROUND_TRIP(sql_text)                                          \
  do {                                                                      \
    ++::testing::check_count();                                             \
    const std::string input = (sql_text);                                   \
    try {                                                                   \
      const sql::Statement first = sql::Parser(input).parse();              \
      const std::string printed = sql::format(first);                       \
      const sql::Statement second = sql::Parser(printed).parse();           \
      if (!(first == second)) {                                             \
        ::testing::report_failure(                                          \
            __FILE__, __LINE__,                                             \
            "round trip changed the tree: " + input + " -> " + printed);    \
      } else if (sql::format(second) != printed) {                          \
        ::testing::report_failure(                                          \
            __FILE__, __LINE__, "printer not idempotent for: " + printed);  \
      }                                                                     \
    } catch (const std::exception& e) {                                     \
      ::testing::report_failure(__FILE__, __LINE__,                         \
                                "round trip threw for \"" + input +         \
                                    "\": " + e.what());                     \
    }                                                                       \
  } while (0)
