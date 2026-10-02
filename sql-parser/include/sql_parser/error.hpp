#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace sql {

// Thrown for any lexical or syntactic problem. The message is prefixed with
// "line L:C: " so it is useful on its own, while line()/column() allow callers
// to report the position separately.
class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& message, std::size_t line, std::size_t column)
      : std::runtime_error("line " + std::to_string(line) + ":" +
                           std::to_string(column) + ": " + message),
        line_(line),
        column_(column) {}

  std::size_t line() const noexcept { return line_; }
  std::size_t column() const noexcept { return column_; }

 private:
  std::size_t line_;
  std::size_t column_;
};

}  // namespace sql
