// Ahead-of-time compiler: validated SQL -> a single C++ translation unit.
//
// `generate()` never interprets anything: it either emits compilable C++ or
// refuses with "not yet compiled: ..." for constructs the compiler does not
// support yet (joins, aggregates, windows, set operations, ...). A refusal is
// always explicit — the compiler never silently drops semantics.

#pragma once

#include <string>
#include <vector>

#include "sql_parser/ast.hpp"
#include "sqlos/positions.hpp"

namespace sqlos {

struct CodegenError {
  SourcePos pos;
  std::string message;
};

struct CodegenResult {
  std::string code;                 // generated C++ (only when ok())
  std::vector<CodegenError> errors; // currently stops at the first problem

  bool ok() const { return errors.empty(); }
};

// Compiles a *validated* program (run validate()/validate_source() first and
// check ok()) into one C++ file that includes
// "sqlos/runtime/sqlos_runtime.hpp" and defines
//
//     extern "C" void sqlos_program(sqlos::SinkFn emit,
//                                   sqlos::HeaderFn header, void* user);
//
// which executes every statement: each result set announces its column
// names through `header` (when not null), then each SELECT row is handed
// to `emit`.
// `positions` comes from statement_positions() (one entry per statement);
// missing entries fall back to 1:1. Schema evolves across statements exactly
// as the validator saw it: DDL mutates the compile-time catalog, later
// statements see earlier DDL.
CodegenResult generate(const std::vector<sql::Statement>& statements,
                       const std::vector<SourcePos>& positions);

}  // namespace sqlos
