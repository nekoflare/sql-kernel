#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sql_parser/ast.hpp"
#include "sql_parser/lexer.hpp"

namespace sql {

// Recursive-descent parser with precedence climbing for expressions.
//
// Statements: SELECT / VALUES (with CTEs, joins, set operations), INSERT,
// UPDATE, DELETE, CREATE TABLE|INDEX|VIEW, DROP, ALTER TABLE and transaction
// control. See README.md for the grammar and its sources.
//
// The parser keeps two tokens of lookahead, so lexical errors in the token
// stream are reported while the statement is being parsed.
class Parser {
 public:
  explicit Parser(std::string_view sql);

  // Parses exactly one statement, then requires end of input.
  Statement parse();

  // Parses every statement in the input; empty statements (';;') are skipped.
  std::vector<Statement> parse_all();

 private:
  // --- statements ---------------------------------------------------------
  Statement parse_statement();
  std::vector<CommonTableExpr> parse_with_clause(bool* recursive);
  SelectStatement parse_select_statement();
  SelectStatement parse_compound();
  static SelectStatement combine_set(SelectStatement left,
                                     const std::string& op, bool all,
                                     SelectStatement right);
  SelectStatement parse_select_core();
  void parse_order_limit(SelectStatement* statement);
  std::vector<SelectItem> parse_select_item_list();
  TableRef parse_from_item();
  TableRef parse_table_or_subquery();
  InsertStatement parse_insert(std::vector<CommonTableExpr> with);
  UpdateStatement parse_update(std::vector<CommonTableExpr> with);
  DeleteStatement parse_delete(std::vector<CommonTableExpr> with);
  Statement parse_create();
  CreateTableStatement parse_create_table(bool temporary);
  CreateIndexStatement parse_create_index(bool unique);
  CreateViewStatement parse_create_view(bool or_replace, bool temporary);
  ColumnDef parse_column_def();
  TableConstraint parse_table_constraint();
  std::string parse_referential_action();
  Statement parse_drop();
  Statement parse_alter();
  TransactionStatement parse_transaction();

  // --- expressions --------------------------------------------------------
  Expr parse_expr(int min_precedence);
  Expr parse_prefix();
  Expr parse_range_op(Expr left, bool negated);
  Expr parse_case();
  Expr parse_cast();
  Expr parse_function_call(std::vector<std::string> parts);
  WindowSpec parse_window_spec();
  std::pair<std::string, ExprPtr> parse_frame_point();
  OrderByItem parse_order_by_item();
  std::string parse_type_name();
  std::vector<std::string> parse_name_parts();

  // --- tokens -------------------------------------------------------------
  void advance();
  bool check(TokenKind kind) const { return current_.kind == kind; }
  bool match(TokenKind kind);
  Token expect(TokenKind kind, const char* what);
  std::string expect_identifier(const char* what);
  std::string parse_optional_alias();
  [[noreturn]] void fail(const std::string& message) const;
  [[noreturn]] void fail_at(const Token& token, const std::string& message) const;

  static bool starts_query(TokenKind kind);
  // Identifiers plus keywords that are legal identifiers in name positions.
  static bool is_identifier_like(TokenKind kind);

  Lexer lexer_;
  Token current_;
  Token next_;
};

}  // namespace sql
