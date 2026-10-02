// Lexical structure: comments, string forms, numbers, operators, parameters.

#include "test_harness.hpp"

TEST(lexer_line_comment) {
  const sql::SelectStatement s = parse_select("SELECT 1 -- comment to EOL");
  CHECK_EQ(s.columns.size(), 1u);
  CHECK_VALUE(lit(s.columns[0].expr), sql::Value{1LL});

  // A comment may sit between any two tokens.
  const sql::SelectStatement split = parse_select("SELECT 1 + -- why\n 2");
  CHECK_EQ(sql::format(split), std::string("SELECT 1 + 2"));
}

TEST(lexer_block_comment) {
  const sql::SelectStatement plain =
      parse_select("SELECT /* x */ 1 /* y */ + 2");
  CHECK_EQ(sql::format(plain), std::string("SELECT 1 + 2"));

  // Block comments nest in PostgreSQL and SQLite.
  const sql::SelectStatement nested =
      parse_select("SELECT /* outer /* inner */ done */ 1");
  CHECK_EQ(sql::format(nested), std::string("SELECT 1"));
}

TEST(lexer_unterminated_block_comment) {
  CHECK_THROWS_MSG(parse_sql("SELECT 1 /* nope"), "unterminated block comment");
}

TEST(lexer_comments_between_statements) {
  const std::vector<sql::Statement> statements =
      sql::Parser("SELECT 1 -- one\n; /* two */ SELECT 2;")
          .parse_all();
  CHECK_EQ(statements.size(), 2u);
}

TEST(lexer_string_escapes) {
  const sql::SelectStatement s =
      parse_select("SELECT 'plain', 'it''s', '', 'a' || 'b'");
  CHECK_VALUE(lit(s.columns[0].expr), sql::Value{std::string("plain")});
  CHECK_VALUE(lit(s.columns[1].expr), sql::Value{std::string("it's")});
  CHECK_VALUE(lit(s.columns[2].expr), sql::Value{std::string("")});
}

TEST(lexer_e_string) {
  // E-strings process backslash escapes; the value holds a real newline.
  const sql::SelectStatement s = parse_select("SELECT E'a\\nb'");
  CHECK_VALUE(lit(s.columns[0].expr), sql::Value{std::string("a\nb")});

  const sql::SelectStatement quote = parse_select("SELECT E'a\\'b'");
  CHECK_VALUE(lit(quote.columns[0].expr), sql::Value{std::string("a'b")});
}

TEST(lexer_dollar_quoted_string) {
  const sql::SelectStatement tagged = parse_select("SELECT $tag$hi $tag$");
  CHECK_VALUE(lit(tagged.columns[0].expr), sql::Value{std::string("hi ")});

  const sql::SelectStatement empty_tag = parse_select("SELECT $$bye$$");
  CHECK_VALUE(lit(empty_tag.columns[0].expr), sql::Value{std::string("bye")});
}

TEST(lexer_dollar_quoted_errors) {
  CHECK_THROWS_MSG(parse_sql("SELECT $tag$hi"),
                   "unterminated dollar-quoted string");
  CHECK_THROWS_MSG(parse_sql("SELECT $ab-c$x$"),
                   "invalid dollar-quote tag");
}

TEST(lexer_numbers) {
  const sql::SelectStatement s =
      parse_select("SELECT 42, 1.5e3, -2E-2, .5, 0.0");
  CHECK_VALUE(lit(s.columns[0].expr), sql::Value{42LL});
  CHECK_VALUE(lit(s.columns[1].expr), sql::Value{1500.0});
  CHECK_VALUE(lit(s.columns[2].expr), sql::Value{-0.02});
  CHECK_VALUE(lit(s.columns[3].expr), sql::Value{0.5});
  CHECK_VALUE(lit(s.columns[4].expr), sql::Value{0.0});
}

TEST(lexer_operators) {
  const std::string text =
      "SELECT 1 || 2, 1 << 2, 1 >> 2, 1 & 2, 1 | 2, 1 != 2, 1 <> 2, "
      "1 % 2, 1 ^ 2";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.columns.size(), 9u);
  for (const sql::SelectItem& item : s.columns) {
    CHECK(item.expr.kind == sql::Expr::Kind::Binary);
  }
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(lexer_bitwise_not) {
  const sql::SelectStatement s = parse_select("SELECT ~5");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Unary);
  CHECK_EQ(s.columns[0].expr.name, std::string("~"));
  CHECK_EQ(sql::format(s), std::string("SELECT ~5"));
}

TEST(lexer_parameters) {
  const sql::SelectStatement s = parse_select("SELECT ?, $1, :name, @name");
  CHECK_EQ(s.columns.size(), 4u);
  for (const sql::SelectItem& item : s.columns) {
    CHECK(item.expr.kind == sql::Expr::Kind::Parameter);
  }
  CHECK_EQ(sql::format(s), std::string("SELECT ?, $1, :name, @name"));
}

TEST(lexer_keyword_case) {
  const sql::SelectStatement lower = parse_select("select 1");
  const sql::SelectStatement mixed = parse_select("SeLeCt 1");
  CHECK(lower == mixed);
}

TEST(lexer_quoted_identifiers) {
  const sql::SelectStatement s =
      parse_select("SELECT \"Mixed Case\", `back tick` FROM t");
  CHECK_EQ(s.columns[0].expr.parts[0], std::string("Mixed Case"));
  CHECK_EQ(s.columns[1].expr.parts[0], std::string("back tick"));
}

TEST(lexer_error_unexpected_character) {
  CHECK_THROWS_MSG(parse_sql("SELECT 1 # bad"), "unexpected character '#'");
  CHECK_THROWS_MSG(parse_sql("INSERT INTO t VALUES (1 @ 2)"),
                   "unexpected character '@'");
}

TEST(lexer_error_unterminated_string) {
  CHECK_THROWS_MSG(parse_sql("SELECT 'abc"), "unterminated string literal");
}

TEST(lexer_error_reports_position) {
  try {
    parse_sql("SELECT 1\nFROM 'x");
    CHECK(false);
  } catch (const sql::ParseError& e) {
    CHECK_EQ(e.line(), 2u);
    CHECK_EQ(e.column(), 6u);
  }
}
