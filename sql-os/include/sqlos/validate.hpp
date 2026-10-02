// The SQL validator: catalog/arity checks, system-table contracts and type
// checks, run over the parsed AST with source positions for every issue.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sql_parser/ast.hpp"
#include "sqlos/catalog.hpp"
#include "sqlos/positions.hpp"

namespace sqlos {

struct Issue {
  SourcePos pos;
  std::string message;
};

struct ValidationResult {
  std::vector<Issue> issues;
  // Catalog after all DDL in the program has been applied.
  Catalog catalog;

  bool ok() const { return issues.empty(); }
};

// Validates statements in order. The catalog starts with the system tables
// and evolves (CREATE/DROP/ALTER) as statements validate, so later
// statements see earlier DDL. `positions` comes from
// statement_positions(); when its size does not match the statement count
// every issue is reported at 1:1.
ValidationResult validate(const std::vector<sql::Statement>& statements,
                          const std::vector<SourcePos>& positions);

// Parse + validate in one step. A parse error becomes a single issue.
ValidationResult validate_source(std::string_view source);

// "line L:C: message" — the same shape as sql::ParseError::what().
std::string format_issue(const Issue& issue);

}  // namespace sqlos
