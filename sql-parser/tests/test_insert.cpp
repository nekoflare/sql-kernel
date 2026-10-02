// INSERT parsing: literals, quoting, case handling, round trip and errors.

#include "test_harness.hpp"

// ---------------------------------------------------------------------------
// Happy paths
// ---------------------------------------------------------------------------

TEST(insert_simple) {
  const sql::InsertStatement s =
      parse_insert("INSERT INTO users (id, name) VALUES (1, 'Ada');");

  CHECK_EQ(s.table, std::string("users"));
  CHECK_EQ(s.columns.size(), 2u);
  CHECK_EQ(s.columns[0], std::string("id"));
  CHECK_EQ(s.columns[1], std::string("name"));
  CHECK_EQ(s.rows.size(), 1u);
  CHECK_EQ(s.rows[0].size(), 2u);
  CHECK_VALUE(lit(s.rows[0][0]), sql::Value{1LL});
  CHECK_VALUE(lit(s.rows[0][1]), sql::Value{std::string("Ada")});
}

TEST(insert_semicolon_is_optional) {
  const sql::InsertStatement with_semi =
      parse_insert("INSERT INTO users (id) VALUES (1);");
  const sql::InsertStatement without_semi =
      parse_insert("INSERT INTO users (id) VALUES (1)");
  CHECK(with_semi == without_semi);
}

TEST(insert_multiple_rows) {
  const sql::InsertStatement s = parse_insert(
      "INSERT INTO users (id, name) VALUES (1, 'Ada'), (2, 'Bob'), (3, 'Cy');");

  CHECK_EQ(s.rows.size(), 3u);
  CHECK_VALUE(lit(s.rows[1][0]), sql::Value{2LL});
  CHECK_VALUE(lit(s.rows[1][1]), sql::Value{std::string("Bob")});
  CHECK_VALUE(lit(s.rows[2][1]), sql::Value{std::string("Cy")});
}

TEST(insert_column_list_is_optional) {
  const sql::InsertStatement s = parse_insert("INSERT INTO logs VALUES ('started')");
  CHECK_EQ(s.table, std::string("logs"));
  CHECK(s.columns.empty());
  CHECK_EQ(s.rows.size(), 1u);
  CHECK_VALUE(lit(s.rows[0][0]), sql::Value{std::string("started")});
}

TEST(insert_keywords_are_case_insensitive) {
  const sql::InsertStatement upper =
      parse_insert("INSERT INTO USERS (ID) VALUES (1)");
  const sql::InsertStatement lower =
      parse_insert("insert into users (id) values (1)");
  const sql::InsertStatement mixed =
      parse_insert("InSeRt InTo UsErS (Id) VaLuEs (1)");

  // Keywords may vary in case; identifiers keep the case they were written
  // with, so only the rows are expected to be equal.
  CHECK(upper.rows == lower.rows);
  CHECK(lower.rows == mixed.rows);
  CHECK_VALUE(lit(upper.rows[0][0]), sql::Value{1LL});
  CHECK_EQ(upper.table, std::string("USERS"));
  CHECK_EQ(upper.columns[0], std::string("ID"));
  CHECK_EQ(mixed.table, std::string("UsErS"));
  CHECK_EQ(mixed.columns[0], std::string("Id"));
}

TEST(insert_literals) {
  const sql::InsertStatement s = parse_insert(
      "INSERT INTO t (a, b, c, d, e, f) VALUES "
      "(3.14, -42, 'it''s', NULL, TRUE, FALSE);");

  CHECK_VALUE(lit(s.rows[0][0]), sql::Value{3.14});
  CHECK_VALUE(lit(s.rows[0][1]), sql::Value{-42LL});
  CHECK_VALUE(lit(s.rows[0][2]), sql::Value{std::string("it's")});
  CHECK_VALUE(lit(s.rows[0][3]), sql::Value{nullptr});
  CHECK_VALUE(lit(s.rows[0][4]), sql::Value{true});
  CHECK_VALUE(lit(s.rows[0][5]), sql::Value{false});
}

TEST(insert_scientific_notation) {
  const sql::InsertStatement s = parse_insert("INSERT INTO t VALUES (1.5e3, -2E-2)");
  CHECK_VALUE(lit(s.rows[0][0]), sql::Value{1500.0});
  CHECK_VALUE(lit(s.rows[0][1]), sql::Value{-0.02});
}

TEST(insert_quoted_identifiers) {
  const sql::InsertStatement s =
      parse_insert("INSERT INTO \"my table\" (`full name`) VALUES ('Ada')");
  CHECK_EQ(s.table, std::string("my table"));
  CHECK_EQ(s.columns.size(), 1u);
  CHECK_EQ(s.columns[0], std::string("full name"));
}

TEST(insert_whitespace_and_newlines) {
  const sql::InsertStatement s = parse_insert(
      "\n  INSERT   INTO\n"
      "    users (id, name)\n"
      "  VALUES\n"
      "    ( 1 , 'Ada' ) ;\n");
  CHECK_EQ(s.table, std::string("users"));
  CHECK_VALUE(lit(s.rows[0][0]), sql::Value{1LL});
  CHECK_VALUE(lit(s.rows[0][1]), sql::Value{std::string("Ada")});
}

TEST(insert_parse_all_multiple_statements) {
  const std::vector<sql::Statement> statements =
      sql::Parser(
          "INSERT INTO a (x) VALUES (1);\n"
          "INSERT INTO b (y) VALUES ('two');\n"
          "INSERT INTO c VALUES (3);")
          .parse_all();

  CHECK_EQ(statements.size(), 3u);
  CHECK_EQ(std::get<sql::InsertStatement>(statements[0]).table,
           std::string("a"));
  CHECK_EQ(std::get<sql::InsertStatement>(statements[1]).table,
           std::string("b"));
  CHECK_VALUE(lit(std::get<sql::InsertStatement>(statements[2]).rows[0][0]),
              sql::Value{3LL});
}

TEST(insert_format_round_trip) {
  const std::string sql_text =
      "INSERT INTO users (id, name, active) VALUES (1, 'Ada', TRUE), "
      "(2, 'Bob''s', FALSE), (3, NULL, TRUE)";
  const sql::InsertStatement parsed = parse_insert(sql_text);
  const sql::InsertStatement reparsed = parse_insert(sql::format(parsed) + ";");
  CHECK(parsed == reparsed);
  CHECK_EQ(sql::format(parsed),
           std::string("INSERT INTO users (id, name, active) VALUES "
                       "(1, 'Ada', TRUE), (2, 'Bob''s', FALSE), (3, NULL, TRUE)"));
  CHECK_ROUND_TRIP(sql_text);
}

// Expressions (not just literals) are accepted in VALUES rows.
TEST(insert_values_accept_expressions) {
  const sql::InsertStatement s =
      parse_insert("INSERT INTO users VALUES (id, 1 + 2 * 3, UPPER(name), ?)");
  CHECK_EQ(s.rows[0].size(), 4u);
  CHECK(s.rows[0][0].kind == sql::Expr::Kind::ColumnRef);
  CHECK(s.rows[0][1].kind == sql::Expr::Kind::Binary);
  CHECK_EQ(sql::format(s.rows[0][1]), std::string("1 + 2 * 3"));
  CHECK(s.rows[0][2].kind == sql::Expr::Kind::FunctionCall);
  CHECK(s.rows[0][3].kind == sql::Expr::Kind::Parameter);
}

TEST(insert_default_keyword) {
  const sql::InsertStatement s =
      parse_insert("INSERT INTO t (a, b) VALUES (DEFAULT, 1)");
  CHECK(s.rows[0][0].kind == sql::Expr::Kind::Default);
  CHECK_VALUE(lit(s.rows[0][1]), sql::Value{1LL});
  CHECK_EQ(sql::format(s),
           std::string("INSERT INTO t (a, b) VALUES (DEFAULT, 1)"));
}

TEST(insert_default_values) {
  const sql::InsertStatement s = parse_insert("INSERT INTO t DEFAULT VALUES");
  CHECK(s.default_values);
  CHECK(s.rows.empty());
}

TEST(insert_parameters) {
  const sql::InsertStatement s =
      parse_insert("INSERT INTO t (a, b, c, d) VALUES (?, $1, :id, @uid)");
  for (const std::vector<sql::Expr>& row : s.rows) {
    for (const sql::Expr& expr : row) {
      CHECK(expr.kind == sql::Expr::Kind::Parameter);
    }
  }
}

TEST(insert_or_replace) {
  const sql::InsertStatement s =
      parse_insert("INSERT OR REPLACE INTO t (a) VALUES (1)");
  CHECK_EQ(s.insert_or, std::string("REPLACE"));
  const sql::InsertStatement ignored =
      parse_insert("insert or ignore into t (a) values (1)");
  CHECK_EQ(ignored.insert_or, std::string("IGNORE"));
}

TEST(insert_returning) {
  const sql::InsertStatement s =
      parse_insert("INSERT INTO t (a) VALUES (1) RETURNING a, b AS c");
  CHECK_EQ(s.returning.size(), 2u);
  CHECK_EQ(s.returning[1].alias, std::string("c"));
}

TEST(insert_on_conflict_do_update) {
  const sql::InsertStatement s = parse_insert(
      "INSERT INTO t (a, b) VALUES (1, 2) ON CONFLICT (a) WHERE a > 0 "
      "DO UPDATE SET b = excluded.b WHERE t.b < 10");
  CHECK(s.upsert != nullptr);
  CHECK(!s.upsert->do_nothing);
  CHECK_EQ(s.upsert->columns.size(), 1u);
  CHECK(s.upsert->target_where != nullptr);
  CHECK(s.upsert->update_where != nullptr);
  CHECK_EQ(s.upsert->updates.size(), 1u);
  CHECK_EQ(s.upsert->updates[0].first, std::string("b"));
}

TEST(insert_on_conflict_do_nothing) {
  const sql::InsertStatement s = parse_insert(
      "INSERT INTO t (a) VALUES (1) ON CONFLICT DO NOTHING");
  CHECK(s.upsert != nullptr);
  CHECK(s.upsert->do_nothing);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST(insert_error_missing_into) {
  CHECK_THROWS_MSG(parse_insert("INSERT users (id) VALUES (1)"), "'INTO'");
}

TEST(insert_error_unsupported_statement) {
  // Anything that is not one of the supported statements, and the message
  // still lists 'INSERT' among the expected starts.
  CHECK_THROWS_MSG(parse_insert("TRUNCATE users"), "'INSERT'");
}

TEST(insert_error_missing_values) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users (id) (1)"), "'VALUES'");
}

TEST(insert_error_unclosed_row) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users (id) VALUES (1"), "')'");
}

TEST(insert_error_empty_row) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users (id) VALUES ()"),
                   "expected a value");
}

TEST(insert_error_row_size_mismatch) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users (id, name) VALUES (1)"),
                   "expected 2 value(s) in row, got 1");
}

TEST(insert_error_row_size_mismatch_too_many) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users (id) VALUES (1, 'Ada')"),
                   "expected 1 value(s) in row, got 2");
}

TEST(insert_error_sign_before_string) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users VALUES (-'Ada')"),
                   "unexpected sign before string literal");
}

TEST(insert_error_sign_without_number) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users VALUES (-)"),
                   "expected an expression");
}

TEST(insert_error_number_out_of_range) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users VALUES (99999999999999999999)"),
                   "out of range");
}

TEST(insert_error_unterminated_string) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users VALUES ('abc)"),
                   "unterminated string literal");
}

TEST(insert_error_unexpected_character) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users VALUES (1 @ 2)"),
                   "unexpected character '@'");
}

TEST(insert_error_trailing_input) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users VALUES (1) oops"),
                   "trailing input");
}

TEST(insert_error_two_statements_with_parse) {
  CHECK_THROWS_MSG(
      parse_insert("INSERT INTO a VALUES (1); INSERT INTO b VALUES (2)"),
      "trailing input");
}

TEST(insert_error_empty_input) {
  CHECK_THROWS_MSG(parse_sql(""), "'INSERT'");
  CHECK_THROWS_MSG(parse_sql("   \n  "), "'INSERT'");
}

TEST(insert_error_reports_position) {
  try {
    parse_insert("INSERT INTO users\nVALUES (1");
    CHECK(false);
  } catch (const sql::ParseError& e) {
    // "VALUES (1" on line 2: the ")" is missing, so the error points at the
    // end of input, just past the '1' (column 10).
    CHECK_EQ(e.line(), 2u);
    CHECK_EQ(e.column(), 10u);
  }
}

TEST(insert_error_missing_column_name) {
  CHECK_THROWS_MSG(parse_insert("INSERT INTO users (id,) VALUES (1)"),
                   "a column name");
}
