// sqlos — SQL validator / ahead-of-time compiler for the SQL-OS project.
//
//   sqlos [FILE]                  validate FILE (or stdin); exit 1 on errors
//   sqlos --check [FILE]          same (validation is the default mode)
//   sqlos --emit OUT.cpp [FILE]   validate, then compile to C++ at OUT.cpp
//   sqlos --help                  usage
//
// Every issue is printed as "error: line L:C: message" — the same shape the
// parser uses for syntax errors, so tooling can consume either uniformly.
// A construct the compiler cannot emit yet is reported the same way with
// "not yet compiled: ...": the CLI never writes a partial translation unit.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sql_parser/error.hpp"
#include "sql_parser/parser.hpp"
#include "sqlos/codegen.hpp"
#include "sqlos/validate.hpp"

namespace {

void usage(std::ostream& out) {
  out << "usage: sqlos [--check] [--emit OUT.cpp] [FILE]\n"
         "\n"
         "Validates a SQL program for the SQL-OS built-in tables:\n"
         "  io_8/16/32_read (SELECT-only), io_8/16/32_write (INSERT-only),\n"
         "  memory, volatile_memory (byte-addressed, address-constrained)\n"
         "\n"
         "--check is the default mode: report issues and exit.\n"
         "--emit writes the validated program as one C++ translation unit\n"
         "(compiled later with g++ -ffreestanding; no interpreter involved).\n"
         "\n"
         "Reads stdin when FILE is omitted. Exit status: 0 = valid,\n"
         "1 = validation/compile errors, 2 = usage/IO errors.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string emit_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage(std::cout);
      return 0;
    }
    if (arg == "--check") {
      // Validation-only mode; it is also the default when --emit is absent.
      continue;
    }
    if (arg == "--emit") {
      if (i + 1 >= argc) {
        std::cerr << "error: --emit requires an output file\n";
        usage(std::cerr);
        return 2;
      }
      emit_path = argv[++i];
      continue;
    }
    if (!path.empty()) {
      std::cerr << "error: only one input file is supported\n";
      usage(std::cerr);
      return 2;
    }
    path = arg;
  }

  std::string input;
  if (path.empty() || path == "-") {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    input = buffer.str();
  } else {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::cerr << "error: cannot open " << path << "\n";
      return 2;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    input = buffer.str();
  }

  if (emit_path.empty()) {
    const sqlos::ValidationResult result = sqlos::validate_source(input);
    for (const sqlos::Issue& issue : result.issues) {
      std::cerr << "error: " << sqlos::format_issue(issue) << "\n";
    }
    if (!result.ok()) {
      std::cerr << result.issues.size() << " error(s)\n";
      return 1;
    }
    return 0;
  }

  // --emit: parse once, validate, then compile to C++.
  std::vector<sql::Statement> statements;
  try {
    statements = sql::Parser(input).parse_all();
  } catch (const sql::ParseError& error) {
    std::cerr << "error: " << error.what() << "\n1 error(s)\n";
    return 1;
  }
  const std::vector<sqlos::SourcePos> positions =
      sqlos::statement_positions(input);

  const sqlos::ValidationResult validated = sqlos::validate(statements, positions);
  for (const sqlos::Issue& issue : validated.issues) {
    std::cerr << "error: " << sqlos::format_issue(issue) << "\n";
  }
  if (!validated.ok()) {
    std::cerr << validated.issues.size() << " error(s)\n";
    return 1;
  }

  const sqlos::CodegenResult generated = sqlos::generate(statements, positions);
  for (const sqlos::CodegenError& error : generated.errors) {
    std::cerr << "error: "
              << sqlos::format_issue({error.pos, error.message}) << "\n";
  }
  if (!generated.ok()) {
    std::cerr << generated.errors.size() << " error(s)\n";
    return 1;
  }

  std::ofstream out(emit_path, std::ios::binary);
  if (!out) {
    std::cerr << "error: cannot write " << emit_path << "\n";
    return 2;
  }
  out << generated.code;
  if (!out) {
    std::cerr << "error: cannot write " << emit_path << "\n";
    return 2;
  }
  return 0;
}
