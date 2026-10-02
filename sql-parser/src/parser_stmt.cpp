// Statement parsing (SELECT/VALUES, DML and DDL) plus the token-level
// helpers shared by both parser halves.

#include <string>
#include <utility>
#include <vector>

#include "sql_parser/error.hpp"
#include "sql_parser/parser.hpp"

namespace sql {
namespace {

std::string join(const std::vector<std::string>& parts,
                 const std::string& separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += separator;
    out += parts[i];
  }
  return out;
}

}  // namespace

// --- token helpers --------------------------------------------------------

Parser::Parser(std::string_view sql) : lexer_(sql) {
  current_ = lexer_.next();
  next_ = lexer_.next();
}

void Parser::advance() {
  current_ = std::move(next_);
  next_ = lexer_.next();
}

bool Parser::match(TokenKind kind) {
  if (!check(kind)) return false;
  advance();
  return true;
}

Token Parser::expect(TokenKind kind, const char* what) {
  if (!check(kind)) {
    fail(std::string("expected ") + what + ", got " + describe(current_));
  }
  Token token = std::move(current_);
  advance();
  return token;
}

std::string Parser::expect_identifier(const char* what) {
  if (!is_identifier_like(current_.kind)) {
    fail(std::string("expected ") + what + ", got " + describe(current_));
  }
  std::string text = std::move(current_.text);
  advance();
  return text;
}

std::string Parser::parse_optional_alias() {
  if (match(TokenKind::KeywordAs)) return expect_identifier("an alias");
  if (is_identifier_like(current_.kind)) {
    std::string text = std::move(current_.text);
    advance();
    return text;
  }
  return "";
}

void Parser::fail(const std::string& message) const {
  throw ParseError(message, current_.line, current_.column);
}

void Parser::fail_at(const Token& token, const std::string& message) const {
  throw ParseError(message, token.line, token.column);
}

bool Parser::starts_query(TokenKind kind) {
  return kind == TokenKind::KeywordSelect || kind == TokenKind::KeywordWith ||
         kind == TokenKind::KeywordValues;
}

bool Parser::is_identifier_like(TokenKind kind) {
  if (kind == TokenKind::Identifier) return true;
  return is_keyword_kind(kind) && !is_reserved_keyword(kind);
}

// --- entry points ---------------------------------------------------------

Statement Parser::parse() {
  Statement statement = parse_statement();
  match(TokenKind::Semicolon);
  if (!check(TokenKind::End)) {
    fail("unexpected trailing input after statement, got " +
         describe(current_));
  }
  return statement;
}

std::vector<Statement> Parser::parse_all() {
  std::vector<Statement> statements;
  while (!check(TokenKind::End)) {
    if (match(TokenKind::Semicolon)) continue;  // empty statement
    statements.push_back(parse_statement());
    if (!check(TokenKind::End) && !match(TokenKind::Semicolon)) {
      fail("expected ';' after statement, got " + describe(current_));
    }
  }
  return statements;
}

Statement Parser::parse_statement() {
  bool recursive = false;
  std::vector<CommonTableExpr> with = parse_with_clause(&recursive);

  Statement statement;

  // "START TRANSACTION" (MySQL/PostgreSQL) is normalised to BEGIN.
  if (current_.kind == TokenKind::Identifier &&
      to_upper_ascii(current_.text) == "START" &&
      (next_.kind == TokenKind::KeywordTransaction ||
       next_.kind == TokenKind::KeywordWork)) {
    if (!with.empty()) {
      fail("WITH is not allowed before a transaction statement");
    }
    advance();
    match(TokenKind::KeywordTransaction);
    match(TokenKind::KeywordWork);
    TransactionStatement transaction;
    transaction.action = "BEGIN";
    statement = std::move(transaction);
    return statement;
  }

  switch (current_.kind) {
    case TokenKind::KeywordSelect:
    case TokenKind::KeywordValues:
    case TokenKind::LeftParen: {
      SelectStatement select = parse_select_statement();
      if (!with.empty()) {
        select.with = std::move(with);
        select.recursive = recursive;
      }
      statement = std::move(select);
      break;
    }

    case TokenKind::KeywordInsert: {
      InsertStatement insert = parse_insert(std::move(with));
      insert.recursive = recursive;
      statement = std::move(insert);
      break;
    }

    case TokenKind::KeywordUpdate: {
      UpdateStatement update = parse_update(std::move(with));
      update.recursive = recursive;
      statement = std::move(update);
      break;
    }

    case TokenKind::KeywordDelete: {
      DeleteStatement delete_stmt = parse_delete(std::move(with));
      delete_stmt.recursive = recursive;
      statement = std::move(delete_stmt);
      break;
    }

    case TokenKind::KeywordCreate:
      if (!with.empty()) {
        fail("WITH is not allowed before CREATE");
      }
      statement = parse_create();
      break;

    case TokenKind::KeywordDrop:
      if (!with.empty()) fail("WITH is not allowed before DROP");
      statement = parse_drop();
      break;

    case TokenKind::KeywordAlter:
      if (!with.empty()) fail("WITH is not allowed before ALTER");
      statement = parse_alter();
      break;

    case TokenKind::KeywordBegin:
    case TokenKind::KeywordCommit:
    case TokenKind::KeywordRollback:
      if (!with.empty()) fail("WITH is not allowed before a transaction statement");
      statement = parse_transaction();
      break;

    default:
      if (!with.empty()) {
        fail(std::string("expected SELECT, VALUES, INSERT, UPDATE or DELETE after WITH, ") +
             "got " + describe(current_));
      }
      fail(
          "unsupported statement, expected 'SELECT', 'INSERT', 'UPDATE', "
          "'DELETE', 'CREATE', 'DROP', 'ALTER', 'BEGIN', 'COMMIT' or "
          "'ROLLBACK', got " + describe(current_));
  }
  return statement;
}

// --- WITH -----------------------------------------------------------------

std::vector<CommonTableExpr> Parser::parse_with_clause(bool* recursive) {
  std::vector<CommonTableExpr> ctes;
  if (!match(TokenKind::KeywordWith)) return ctes;
  if (match(TokenKind::KeywordRecursive)) *recursive = true;

  do {
    CommonTableExpr cte;
    cte.name = expect_identifier("a CTE name");
    if (match(TokenKind::LeftParen)) {
      do {
        cte.columns.push_back(expect_identifier("a column name"));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
    }
    expect(TokenKind::KeywordAs, "'AS'");
    if (match(TokenKind::KeywordNot)) {
      expect(TokenKind::KeywordMaterialized, "'MATERIALIZED'");
      cte.not_materialized = true;
    } else {
      match(TokenKind::KeywordMaterialized);
    }
    expect(TokenKind::LeftParen, "'('");
    if (!starts_query(current_.kind)) {
      fail("expected SELECT or VALUES after '(', got " + describe(current_));
    }
    cte.query = std::make_shared<SelectStatement>(parse_select_statement());
    expect(TokenKind::RightParen, "')'");
    ctes.push_back(std::move(cte));
  } while (match(TokenKind::Comma));

  return ctes;
}

// --- SELECT ---------------------------------------------------------------

SelectStatement Parser::parse_select_statement() {
  bool recursive = false;
  std::vector<CommonTableExpr> with = parse_with_clause(&recursive);

  SelectStatement node = parse_compound();
  if (!with.empty()) {
    node.with = std::move(with);
    node.recursive = recursive;
  }
  parse_order_limit(&node);
  return node;
}

SelectStatement Parser::parse_compound() {
  SelectStatement node = parse_select_core();
  for (;;) {
    std::string op;
    if (match(TokenKind::KeywordUnion)) {
      op = "UNION";
    } else if (match(TokenKind::KeywordIntersect)) {
      op = "INTERSECT";
    } else if (match(TokenKind::KeywordExcept)) {
      op = "EXCEPT";
    } else {
      break;
    }
    bool all = false;
    if (match(TokenKind::KeywordAll)) {
      all = true;
    } else {
      match(TokenKind::KeywordDistinct);
    }
    node = combine_set(std::move(node), op, all, parse_select_core());
  }
  return node;
}

SelectStatement Parser::combine_set(SelectStatement left, const std::string& op,
                                    bool all, SelectStatement right) {
  auto precedence_of = [](const std::string& set_op) {
    // INTERSECT binds more tightly than UNION/EXCEPT (PostgreSQL, SQL:2016).
    return set_op == "INTERSECT" ? 2 : 1;
  };
  if (!left.set_op.empty() && precedence_of(op) > precedence_of(left.set_op)) {
    // Attach deeper into the right spine: A UNION B INTERSECT C
    // becomes A UNION (B INTERSECT C).
    *left.set_right =
        combine_set(std::move(*left.set_right), op, all, std::move(right));
    return left;
  }
  SelectStatement combined;
  combined.set_op = op;
  combined.set_all = all;
  combined.set_left = std::make_shared<SelectStatement>(std::move(left));
  combined.set_right = std::make_shared<SelectStatement>(std::move(right));
  return combined;
}

SelectStatement Parser::parse_select_core() {
  if (check(TokenKind::LeftParen)) {  // parenthesised query
    advance();
    SelectStatement inner = parse_select_statement();
    expect(TokenKind::RightParen, "')'");
    return inner;
  }

  SelectStatement statement;

  if (match(TokenKind::KeywordValues)) {
    statement.is_values = true;
    do {
      expect(TokenKind::LeftParen, "'('");
      if (check(TokenKind::RightParen)) fail("expected an expression, got ')'");
      std::vector<Expr> row;
      do {
        row.push_back(parse_expr(0));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
      statement.value_rows.push_back(std::move(row));
    } while (match(TokenKind::Comma));
    return statement;
  }

  expect(TokenKind::KeywordSelect, "'SELECT'");
  if (match(TokenKind::KeywordAll)) {
    statement.all = true;
  } else if (match(TokenKind::KeywordDistinct)) {
    statement.distinct = true;
    if (match(TokenKind::KeywordOn)) {  // DISTINCT ON (...)
      expect(TokenKind::LeftParen, "'('");
      do {
        statement.distinct_on.push_back(parse_expr(0));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
    }
  }

  statement.columns = parse_select_item_list();

  if (match(TokenKind::KeywordFrom)) {
    do {
      statement.from.push_back(parse_from_item());
    } while (match(TokenKind::Comma));
  }
  if (match(TokenKind::KeywordWhere)) statement.where = make_expr_ptr(parse_expr(0));
  if (match(TokenKind::KeywordGroup)) {
    expect(TokenKind::KeywordBy, "'BY'");
    if (match(TokenKind::KeywordAll)) {
      statement.group_all = true;
    } else if (match(TokenKind::KeywordDistinct)) {
      statement.group_distinct = true;
    } else {
      do {
        statement.group_by.push_back(parse_expr(0));
      } while (match(TokenKind::Comma));
    }
  }
  if (match(TokenKind::KeywordHaving)) statement.having = make_expr_ptr(parse_expr(0));

  return statement;
}

void Parser::parse_order_limit(SelectStatement* statement) {
  if (match(TokenKind::KeywordOrder)) {
    expect(TokenKind::KeywordBy, "'BY'");
    do {
      statement->order_by.push_back(parse_order_by_item());
    } while (match(TokenKind::Comma));
  }
  if (match(TokenKind::KeywordLimit)) {
    if (match(TokenKind::KeywordAll)) {
      statement->limit_all = true;
    } else {
      statement->limit = make_expr_ptr(parse_expr(0));
    }
  }
  if (match(TokenKind::KeywordOffset)) {
    statement->offset = make_expr_ptr(parse_expr(0));
    match(TokenKind::KeywordRow);
    match(TokenKind::KeywordRows);
  }
}

std::vector<SelectItem> Parser::parse_select_item_list() {
  std::vector<SelectItem> items;
  do {
    SelectItem item;
    item.expr = parse_expr(0);
    item.alias = parse_optional_alias();
    items.push_back(std::move(item));
  } while (match(TokenKind::Comma));
  return items;
}

TableRef Parser::parse_from_item() {
  TableRef left = parse_table_or_subquery();

  for (;;) {
    const bool natural = match(TokenKind::KeywordNatural);
    std::string type;
    if (check(TokenKind::KeywordJoin)) {
      type = "";
    } else if (match(TokenKind::KeywordInner)) {
      type = "INNER";
    } else if (match(TokenKind::KeywordLeft)) {
      type = "LEFT";
      if (match(TokenKind::KeywordOuter)) type += " OUTER";
    } else if (match(TokenKind::KeywordRight)) {
      type = "RIGHT";
      if (match(TokenKind::KeywordOuter)) type += " OUTER";
    } else if (match(TokenKind::KeywordFull)) {
      type = "FULL";
      if (match(TokenKind::KeywordOuter)) type += " OUTER";
    } else if (match(TokenKind::KeywordCross)) {
      type = "CROSS";
    } else if (match(TokenKind::KeywordOuter)) {
      fail("expected LEFT, RIGHT or FULL before OUTER");
    } else {
      if (natural) fail("expected JOIN after NATURAL");
      break;
    }
    expect(TokenKind::KeywordJoin, "'JOIN'");

    TableRef join;
    join.kind = TableRef::Kind::Join;
    join.natural = natural;
    join.join_type = type;
    join.left = std::make_shared<TableRef>(std::move(left));
    join.right = std::make_shared<TableRef>(parse_table_or_subquery());

    if (natural) {
      if (check(TokenKind::KeywordOn) || check(TokenKind::KeywordUsing)) {
        fail("NATURAL JOIN cannot have a join condition");
      }
    } else if (type == "CROSS") {
      if (check(TokenKind::KeywordOn) || check(TokenKind::KeywordUsing)) {
        fail("CROSS JOIN cannot have a join condition");
      }
    } else if (match(TokenKind::KeywordOn)) {
      join.on = make_expr_ptr(parse_expr(0));
    } else if (match(TokenKind::KeywordUsing)) {
      expect(TokenKind::LeftParen, "'('");
      do {
        join.using_columns.push_back(expect_identifier("a column name"));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
    } else {
      fail("expected ON or USING after JOIN, got " + describe(current_));
    }

    left = std::move(join);
  }
  return left;
}

TableRef Parser::parse_table_or_subquery() {
  auto parse_column_aliases = [this](TableRef* ref) {
    if (!check(TokenKind::LeftParen)) return;
    advance();
    do {
      ref->column_aliases.push_back(expect_identifier("a column name"));
    } while (match(TokenKind::Comma));
    expect(TokenKind::RightParen, "')'");
  };

  if (check(TokenKind::LeftParen)) {  // (subquery) or (joined tables)
    advance();
    if (starts_query(current_.kind)) {
      TableRef ref;
      ref.kind = TableRef::Kind::Subquery;
      ref.subquery =
          std::make_shared<SelectStatement>(parse_select_statement());
      expect(TokenKind::RightParen, "')'");
      ref.alias = parse_optional_alias();
      parse_column_aliases(&ref);
      return ref;
    }
    TableRef ref = parse_from_item();
    expect(TokenKind::RightParen, "')'");
    ref.parenthesized = true;
    ref.alias = parse_optional_alias();
    parse_column_aliases(&ref);
    return ref;
  }

  const std::vector<std::string> parts = parse_name_parts();

  if (check(TokenKind::LeftParen)) {  // table function
    TableRef ref;
    ref.kind = TableRef::Kind::TableFunction;
    ref.function = make_expr_ptr(parse_function_call(parts));
    ref.alias = parse_optional_alias();
    parse_column_aliases(&ref);
    return ref;
  }

  TableRef ref;
  ref.kind = TableRef::Kind::Table;
  ref.name = join(parts, ".");
  ref.alias = parse_optional_alias();
  parse_column_aliases(&ref);
  return ref;
}

// --- INSERT / UPDATE / DELETE ---------------------------------------------

InsertStatement Parser::parse_insert(std::vector<CommonTableExpr> with) {
  expect(TokenKind::KeywordInsert, "'INSERT'");
  InsertStatement statement;
  statement.with = std::move(with);

  if (check(TokenKind::KeywordOr)) {  // SQLite: INSERT OR REPLACE / OR IGNORE
    advance();
    if (match(TokenKind::KeywordReplace)) {
      statement.insert_or = "REPLACE";
    } else if (match(TokenKind::KeywordIgnore)) {
      statement.insert_or = "IGNORE";
    } else {
      fail("expected REPLACE or IGNORE after INSERT OR");
    }
  }

  expect(TokenKind::KeywordInto, "'INTO'");
  statement.table = join(parse_name_parts(), ".");

  if (match(TokenKind::LeftParen)) {
    do {
      statement.columns.push_back(expect_identifier("a column name"));
    } while (match(TokenKind::Comma));
    expect(TokenKind::RightParen, "')'");
  }

  if (match(TokenKind::KeywordValues)) {
    do {
      const Token row_start = current_;
      expect(TokenKind::LeftParen, "'('");
      if (check(TokenKind::RightParen)) fail("expected a value, got ')'");
      std::vector<Expr> row;
      do {
        row.push_back(parse_expr(0));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
      if (!statement.columns.empty() &&
          row.size() != statement.columns.size()) {
        throw ParseError(
            "expected " + std::to_string(statement.columns.size()) +
                " value(s) in row, got " + std::to_string(row.size()),
            row_start.line, row_start.column);
      }
      statement.rows.push_back(std::move(row));
    } while (match(TokenKind::Comma));
  } else if (check(TokenKind::KeywordDefault)) {
    advance();
    expect(TokenKind::KeywordValues, "'VALUES' after DEFAULT");
    statement.default_values = true;
  } else if (starts_query(current_.kind)) {
    statement.select =
        std::make_shared<SelectStatement>(parse_select_statement());
  } else {
    fail("expected 'VALUES', 'DEFAULT VALUES' or 'SELECT' after the column "
         "list, got " + describe(current_));
  }

  if (match(TokenKind::KeywordOn)) {  // upsert
    expect(TokenKind::KeywordConflict, "'CONFLICT'");
    UpsertClause upsert;
    if (match(TokenKind::LeftParen)) {
      if (check(TokenKind::KeywordWhere)) {
        upsert.target_where = make_expr_ptr(parse_expr(0));
      } else {
        do {
          upsert.columns.push_back(expect_identifier("a column name"));
        } while (match(TokenKind::Comma));
      }
      expect(TokenKind::RightParen, "')'");
    }
    if (match(TokenKind::KeywordWhere)) upsert.target_where = make_expr_ptr(parse_expr(0));
    expect(TokenKind::KeywordDo, "'DO'");
    if (match(TokenKind::KeywordNothing)) {
      upsert.do_nothing = true;
    } else if (match(TokenKind::KeywordUpdate)) {
      expect(TokenKind::KeywordSet, "'SET'");
      do {
        std::string column = join(parse_name_parts(), ".");
        expect(TokenKind::Eq, "'='");
        upsert.updates.emplace_back(std::move(column),
                                    make_expr_ptr(parse_expr(0)));
      } while (match(TokenKind::Comma));
      if (match(TokenKind::KeywordWhere)) upsert.update_where = make_expr_ptr(parse_expr(0));
    } else {
      fail("expected NOTHING or UPDATE after DO");
    }
    statement.upsert = std::make_shared<UpsertClause>(std::move(upsert));
  }

  if (match(TokenKind::KeywordReturning)) {
    statement.returning = parse_select_item_list();
  }
  return statement;
}

UpdateStatement Parser::parse_update(std::vector<CommonTableExpr> with) {
  expect(TokenKind::KeywordUpdate, "'UPDATE'");
  UpdateStatement statement;
  statement.with = std::move(with);
  statement.table = join(parse_name_parts(), ".");
  statement.alias = parse_optional_alias();

  expect(TokenKind::KeywordSet, "'SET'");
  do {
    if (check(TokenKind::LeftParen)) {
      // PostgreSQL: SET (a, b) = (expr, expr); flattened into single
      // assignments so the canonical form is "a = expr, b = expr".
      advance();
      std::vector<std::string> columns;
      do {
        columns.push_back(expect_identifier("a column name"));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
      expect(TokenKind::Eq, "'='");
      expect(TokenKind::LeftParen, "'('");
      std::vector<ExprPtr> values;
      do {
        values.push_back(make_expr_ptr(parse_expr(0)));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
      if (columns.size() != values.size()) {
        fail("expected " + std::to_string(columns.size()) +
             " value(s) in assignment list, got " +
             std::to_string(values.size()));
      }
      for (std::size_t i = 0; i < columns.size(); ++i) {
        statement.assignments.emplace_back(std::move(columns[i]),
                                           std::move(values[i]));
      }
      continue;
    }
    std::string column = join(parse_name_parts(), ".");
    expect(TokenKind::Eq, "'='");
    statement.assignments.emplace_back(std::move(column),
                                       make_expr_ptr(parse_expr(0)));
  } while (match(TokenKind::Comma));

  if (match(TokenKind::KeywordFrom)) {
    do {
      statement.from.push_back(parse_from_item());
    } while (match(TokenKind::Comma));
  }
  if (match(TokenKind::KeywordWhere)) statement.where = make_expr_ptr(parse_expr(0));
  if (match(TokenKind::KeywordReturning)) {
    statement.returning = parse_select_item_list();
  }
  return statement;
}

DeleteStatement Parser::parse_delete(std::vector<CommonTableExpr> with) {
  expect(TokenKind::KeywordDelete, "'DELETE'");
  DeleteStatement statement;
  statement.with = std::move(with);
  expect(TokenKind::KeywordFrom, "'FROM'");
  statement.table = join(parse_name_parts(), ".");
  statement.alias = parse_optional_alias();
  if (match(TokenKind::KeywordWhere)) statement.where = make_expr_ptr(parse_expr(0));
  if (match(TokenKind::KeywordReturning)) {
    statement.returning = parse_select_item_list();
  }
  return statement;
}

// --- DDL ------------------------------------------------------------------

Statement Parser::parse_create() {
  expect(TokenKind::KeywordCreate, "'CREATE'");

  bool or_replace = false;
  if (check(TokenKind::KeywordOr)) {
    advance();
    expect(TokenKind::KeywordReplace, "'REPLACE'");
    or_replace = true;
  }
  const bool unique = match(TokenKind::KeywordUnique);
  const bool temporary = match(TokenKind::KeywordTemp) ||
                         match(TokenKind::KeywordTemporary);

  if (match(TokenKind::KeywordTable)) {
    if (unique) fail("UNIQUE is only valid before INDEX");
    if (or_replace) fail("OR REPLACE is only valid before VIEW");
    return parse_create_table(temporary);
  }
  if (match(TokenKind::KeywordIndex)) return parse_create_index(unique);
  if (match(TokenKind::KeywordView)) return parse_create_view(or_replace, temporary);

  fail("expected TABLE, INDEX or VIEW after CREATE, got " +
       describe(current_));
}

CreateTableStatement Parser::parse_create_table(bool temporary) {
  CreateTableStatement statement;
  statement.temporary = temporary;
  if (check(TokenKind::KeywordIf)) {
    advance();
    expect(TokenKind::KeywordNot, "'NOT'");
    expect(TokenKind::KeywordExists, "'EXISTS'");
    statement.if_not_exists = true;
  }
  statement.name = join(parse_name_parts(), ".");

  if (match(TokenKind::KeywordAs)) {  // CREATE TABLE ... AS SELECT
    statement.select =
        std::make_shared<SelectStatement>(parse_select_statement());
    return statement;
  }

  expect(TokenKind::LeftParen, "'('");
  do {
    if (check(TokenKind::KeywordConstraint) ||
        check(TokenKind::KeywordPrimary) || check(TokenKind::KeywordUnique) ||
        check(TokenKind::KeywordCheck) || check(TokenKind::KeywordForeign)) {
      statement.constraints.push_back(parse_table_constraint());
    } else {
      statement.columns.push_back(parse_column_def());
    }
  } while (match(TokenKind::Comma));
  expect(TokenKind::RightParen, "')'");
  return statement;
}

ColumnDef Parser::parse_column_def() {
  ColumnDef column;
  column.name = expect_identifier("a column name");

  // An optional data type follows, unless the next token starts a constraint.
  const bool constraint_start =
      check(TokenKind::KeywordConstraint) ||
      check(TokenKind::KeywordPrimary) || check(TokenKind::KeywordUnique) ||
      check(TokenKind::KeywordCheck) || check(TokenKind::KeywordDefault) ||
      check(TokenKind::KeywordReferences) ||
      check(TokenKind::KeywordCollate) ||
      check(TokenKind::KeywordAutoincrement) || check(TokenKind::KeywordNot);
  if (is_identifier_like(current_.kind) && !constraint_start) {
    column.type_name = parse_type_name();
  }

  for (;;) {
    std::string constraint_name;
    if (check(TokenKind::KeywordConstraint)) {
      advance();
      constraint_name = expect_identifier("a constraint name");
    }

    ColumnConstraint constraint;
    if (match(TokenKind::KeywordNot)) {
      expect(TokenKind::KeywordNull, "'NULL' after NOT");
      constraint.kind = ColumnConstraint::Kind::NotNull;
    } else if (match(TokenKind::KeywordNull)) {
      constraint.kind = ColumnConstraint::Kind::Null;
    } else if (match(TokenKind::KeywordPrimary)) {
      expect(TokenKind::KeywordKey, "'KEY'");
      constraint.kind = ColumnConstraint::Kind::PrimaryKey;
    } else if (match(TokenKind::KeywordUnique)) {
      constraint.kind = ColumnConstraint::Kind::Unique;
    } else if (match(TokenKind::KeywordCheck)) {
      expect(TokenKind::LeftParen, "'('");
      constraint.value = make_expr_ptr(parse_expr(0));
      expect(TokenKind::RightParen, "')'");
      constraint.kind = ColumnConstraint::Kind::Check;
    } else if (match(TokenKind::KeywordDefault)) {
      constraint.value = make_expr_ptr(parse_expr(0));
      constraint.kind = ColumnConstraint::Kind::Default;
    } else if (match(TokenKind::KeywordReferences)) {
      constraint.kind = ColumnConstraint::Kind::References;
      constraint.ref_table = join(parse_name_parts(), ".");
      if (match(TokenKind::LeftParen)) {
        do {
          constraint.ref_columns.push_back(
              expect_identifier("a column name"));
        } while (match(TokenKind::Comma));
        expect(TokenKind::RightParen, "')'");
      }
      while (check(TokenKind::KeywordOn)) {
        advance();
        if (match(TokenKind::KeywordDelete)) {
          constraint.ref_on_delete = parse_referential_action();
        } else if (match(TokenKind::KeywordUpdate)) {
          constraint.ref_on_update = parse_referential_action();
        } else {
          fail("expected DELETE or UPDATE after ON");
        }
      }
    } else if (match(TokenKind::KeywordCollate)) {
      constraint.collation = expect_identifier("a collation name");
      constraint.kind = ColumnConstraint::Kind::Collate;
    } else if (match(TokenKind::KeywordAutoincrement)) {
      constraint.kind = ColumnConstraint::Kind::AutoIncrement;
    } else {
      if (!constraint_name.empty()) {
        fail("expected a constraint after CONSTRAINT " + constraint_name);
      }
      break;
    }

    // Optional referential-deferrability suffix (DEFERRABLE / INITIALLY ...),
    // which may appear in either order: "DEFERRABLE INITIALLY DEFERRED".
    for (;;) {
      if (check(TokenKind::KeywordNot) &&
          next_.kind == TokenKind::Identifier &&
          to_upper_ascii(next_.text) == "DEFERRABLE") {
        advance();  // NOT DEFERRABLE: deferrable stays false
        advance();
      } else if (check(TokenKind::Identifier) &&
                 to_upper_ascii(current_.text) == "DEFERRABLE") {
        advance();
        constraint.deferrable = true;
      } else if (check(TokenKind::Identifier) &&
                 to_upper_ascii(current_.text) == "INITIALLY") {
        advance();
        if (check(TokenKind::Identifier) &&
            to_upper_ascii(current_.text) == "DEFERRED") {
          advance();
          constraint.initially_deferred = true;
        } else if (check(TokenKind::Identifier) &&
                   to_upper_ascii(current_.text) == "IMMEDIATE") {
          advance();
        } else {
          fail("expected DEFERRED or IMMEDIATE after INITIALLY");
        }
      } else {
        break;
      }
    }

    constraint.name = std::move(constraint_name);
    column.constraints.push_back(std::move(constraint));
  }
  return column;
}

TableConstraint Parser::parse_table_constraint() {
  TableConstraint constraint;
  if (match(TokenKind::KeywordConstraint)) {
    constraint.name = expect_identifier("a constraint name");
  }

  if (match(TokenKind::KeywordPrimary)) {
    expect(TokenKind::KeywordKey, "'KEY'");
    constraint.kind = TableConstraint::Kind::PrimaryKey;
  } else if (match(TokenKind::KeywordUnique)) {
    constraint.kind = TableConstraint::Kind::Unique;
  } else if (match(TokenKind::KeywordCheck)) {
    constraint.kind = TableConstraint::Kind::Check;
    expect(TokenKind::LeftParen, "'('");
    constraint.check = make_expr_ptr(parse_expr(0));
    expect(TokenKind::RightParen, "')'");
    return constraint;
  } else if (match(TokenKind::KeywordForeign)) {
    expect(TokenKind::KeywordKey, "'KEY'");
    constraint.kind = TableConstraint::Kind::Foreign;
  } else {
    fail("expected PRIMARY, UNIQUE, CHECK or FOREIGN, got " +
         describe(current_));
  }

  expect(TokenKind::LeftParen, "'('");
  do {
    constraint.columns.push_back(expect_identifier("a column name"));
  } while (match(TokenKind::Comma));
  expect(TokenKind::RightParen, "')'");

  if (constraint.kind == TableConstraint::Kind::Foreign) {
    expect(TokenKind::KeywordReferences, "'REFERENCES'");
    constraint.ref_table = join(parse_name_parts(), ".");
    if (match(TokenKind::LeftParen)) {
      do {
        constraint.ref_columns.push_back(expect_identifier("a column name"));
      } while (match(TokenKind::Comma));
      expect(TokenKind::RightParen, "')'");
    }
    while (check(TokenKind::KeywordOn)) {
      advance();
      if (match(TokenKind::KeywordDelete)) {
        constraint.ref_on_delete = parse_referential_action();
      } else if (match(TokenKind::KeywordUpdate)) {
        constraint.ref_on_update = parse_referential_action();
      } else {
        fail("expected DELETE or UPDATE after ON");
      }
    }
  }
  return constraint;
}

std::string Parser::parse_referential_action() {
  if (match(TokenKind::KeywordCascade)) return "CASCADE";
  if (match(TokenKind::KeywordRestrict)) return "RESTRICT";
  if (match(TokenKind::KeywordSet)) {
    if (match(TokenKind::KeywordNull)) return "SET NULL";
    if (match(TokenKind::KeywordDefault)) return "SET DEFAULT";
    fail("expected NULL or DEFAULT after SET");
  }
  if (match(TokenKind::KeywordNo)) {
    if (check(TokenKind::Identifier) &&
        to_upper_ascii(current_.text) == "ACTION") {
      advance();
      return "NO ACTION";
    }
    fail("expected ACTION after NO");
  }
  fail("expected CASCADE, RESTRICT, SET NULL, SET DEFAULT or NO ACTION, got " +
       describe(current_));
}

CreateIndexStatement Parser::parse_create_index(bool unique) {
  CreateIndexStatement statement;
  statement.unique = unique;
  if (check(TokenKind::KeywordIf)) {
    advance();
    expect(TokenKind::KeywordNot, "'NOT'");
    expect(TokenKind::KeywordExists, "'EXISTS'");
    statement.if_not_exists = true;
  }
  statement.name = join(parse_name_parts(), ".");
  expect(TokenKind::KeywordOn, "'ON'");
  statement.table = join(parse_name_parts(), ".");
  expect(TokenKind::LeftParen, "'('");
  do {
    IndexColumn column;
    column.expr = parse_expr(0);
    if (match(TokenKind::KeywordAsc)) {
      column.descending = false;
    } else if (match(TokenKind::KeywordDesc)) {
      column.descending = true;
    }
    statement.columns.push_back(std::move(column));
  } while (match(TokenKind::Comma));
  expect(TokenKind::RightParen, "')'");
  return statement;
}

CreateViewStatement Parser::parse_create_view(bool or_replace,
                                              bool temporary) {
  CreateViewStatement statement;
  statement.or_replace = or_replace;
  statement.temporary = temporary;
  if (check(TokenKind::KeywordIf)) {
    advance();
    expect(TokenKind::KeywordNot, "'NOT'");
    expect(TokenKind::KeywordExists, "'EXISTS'");
    statement.if_not_exists = true;
  }
  statement.name = join(parse_name_parts(), ".");
  if (match(TokenKind::LeftParen)) {
    do {
      statement.columns.push_back(expect_identifier("a column name"));
    } while (match(TokenKind::Comma));
    expect(TokenKind::RightParen, "')'");
  }
  expect(TokenKind::KeywordAs, "'AS'");
  statement.query =
      std::make_shared<SelectStatement>(parse_select_statement());
  return statement;
}

Statement Parser::parse_drop() {
  expect(TokenKind::KeywordDrop, "'DROP'");
  DropStatement statement;
  if (match(TokenKind::KeywordTable)) {
    statement.object_type = "TABLE";
  } else if (match(TokenKind::KeywordIndex)) {
    statement.object_type = "INDEX";
  } else if (match(TokenKind::KeywordView)) {
    statement.object_type = "VIEW";
  } else {
    fail("expected TABLE, INDEX or VIEW after DROP, got " +
         describe(current_));
  }
  if (check(TokenKind::KeywordIf)) {
    advance();
    expect(TokenKind::KeywordExists, "'EXISTS'");
    statement.if_exists = true;
  }
  do {
    statement.names.push_back(join(parse_name_parts(), "."));
  } while (match(TokenKind::Comma));
  if (match(TokenKind::KeywordCascade)) {
    statement.option = "CASCADE";
  } else if (match(TokenKind::KeywordRestrict)) {
    statement.option = "RESTRICT";
  }
  return statement;
}

Statement Parser::parse_alter() {
  expect(TokenKind::KeywordAlter, "'ALTER'");
  expect(TokenKind::KeywordTable, "'TABLE'");

  AlterTableStatement statement;
  if (check(TokenKind::KeywordIf)) {
    advance();
    expect(TokenKind::KeywordExists, "'EXISTS'");
    statement.if_exists = true;
  }
  statement.table = join(parse_name_parts(), ".");

  if (match(TokenKind::KeywordAdd)) {
    statement.action = AlterTableStatement::Action::AddColumn;
    match(TokenKind::KeywordColumn);
    statement.column = parse_column_def();
  } else if (match(TokenKind::KeywordDrop)) {
    statement.action = AlterTableStatement::Action::DropColumn;
    match(TokenKind::KeywordColumn);
    if (check(TokenKind::KeywordIf)) {
      advance();
      expect(TokenKind::KeywordExists, "'EXISTS'");
      statement.column_if_exists = true;
    }
    statement.column_name = expect_identifier("a column name");
  } else if (match(TokenKind::KeywordRename)) {
    if (match(TokenKind::KeywordTo)) {
      statement.action = AlterTableStatement::Action::RenameTable;
      statement.new_name = expect_identifier("a new table name");
    } else {
      match(TokenKind::KeywordColumn);
      statement.action = AlterTableStatement::Action::RenameColumn;
      statement.column_name = expect_identifier("a column name");
      expect(TokenKind::KeywordTo, "'TO'");
      statement.new_name = expect_identifier("a new name");
    }
  } else {
    fail("expected ADD, DROP or RENAME after ALTER TABLE, got " +
         describe(current_));
  }
  return statement;
}

TransactionStatement Parser::parse_transaction() {
  TransactionStatement statement;
  if (match(TokenKind::KeywordBegin)) {
    statement.action = "BEGIN";
  } else if (match(TokenKind::KeywordCommit)) {
    statement.action = "COMMIT";
  } else if (match(TokenKind::KeywordRollback)) {
    statement.action = "ROLLBACK";
  } else {
    fail("expected BEGIN, COMMIT or ROLLBACK");
  }
  match(TokenKind::KeywordTransaction);
  match(TokenKind::KeywordWork);
  return statement;
}

}  // namespace sql
