#include "sqlos/positions.hpp"

#include "sql_parser/lexer.hpp"

namespace sqlos {

std::vector<SourcePos> statement_positions(std::string_view input) {
  std::vector<SourcePos> out;
  sql::Lexer lexer(input);

  bool in_statement = false;
  SourcePos start;
  for (;;) {
    const sql::Token token = lexer.next();
    if (token.kind == sql::TokenKind::End) break;
    if (token.kind == sql::TokenKind::Semicolon) {
      if (in_statement) {
        out.push_back(start);
        in_statement = false;
      }
      continue;
    }
    if (!in_statement) {
      start.line = token.line;
      start.column = token.column;
      in_statement = true;
    }
  }
  if (in_statement) out.push_back(start);
  return out;
}

}  // namespace sqlos
