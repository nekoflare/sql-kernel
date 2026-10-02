// Small CLI: reads SQL from a file (or stdin) and prints each parsed
// statement back out in canonical form. Exits non-zero on a parse error.
//
//   ./sql_parse input.sql
//   echo "INSERT INTO users (id) VALUES (1);" | ./sql_parse

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "sql_parser/error.hpp"
#include "sql_parser/parser.hpp"

int main(int argc, char** argv) {
  std::string input;

  if (argc > 1) {
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
      std::cerr << "error: cannot open '" << argv[1] << "'\n";
      return 1;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    input = buffer.str();
  } else {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    input = buffer.str();
  }

  try {
    sql::Parser parser(input);
    for (const sql::Statement& statement : parser.parse_all()) {
      std::cout << sql::format(statement) << ";\n";
    }
  } catch (const sql::ParseError& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
