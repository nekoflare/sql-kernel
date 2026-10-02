// Expressions: precedence, operators, predicates, subqueries, windows.

#include "test_harness.hpp"

namespace {

// Renders "SELECT <expr>" in canonical form.
std::string select_of(const std::string& expr_text) {
  return sql::format(parse_select("SELECT " + expr_text));
}

}  // namespace

// ---------------------------------------------------------------------------
// Precedence and operators
// ---------------------------------------------------------------------------

TEST(expr_arithmetic_precedence) {
  CHECK_EQ(select_of("1 + 2 * 3"), std::string("SELECT 1 + 2 * 3"));
  CHECK_EQ(select_of("(1 + 2) * 3"), std::string("SELECT (1 + 2) * 3"));
  CHECK_EQ(select_of("1 + 2 * 3 ^ 2 >= 4"),
           std::string("SELECT 1 + 2 * 3 ^ 2 >= 4"));
  CHECK_EQ(select_of("1 || 2 & 3"), std::string("SELECT 1 || 2 & 3"));
  CHECK_ROUND_TRIP("SELECT 1 + 2 * 3 ^ 2 >= 4 AND NOT 5 = 5 OR 6 <> 6");
}

TEST(expr_logical_precedence) {
  CHECK_EQ(select_of("a AND b OR c"), std::string("SELECT a AND b OR c"));
  CHECK_EQ(select_of("a OR b AND c"), std::string("SELECT a OR b AND c"));
  CHECK_EQ(select_of("NOT a AND b"), std::string("SELECT NOT a AND b"));
  CHECK_EQ(select_of("NOT (a OR b)"), std::string("SELECT NOT (a OR b)"));
  CHECK_ROUND_TRIP("SELECT (a AND b) OR (c AND d)");
}

TEST(expr_concatenation) {
  CHECK_EQ(select_of("'x' || 'y'"), std::string("SELECT 'x' || 'y'"));
}

TEST(expr_unary_signs) {
  // -42 folds into the literal...
  const sql::SelectStatement folded = parse_select("SELECT -42, +7");
  CHECK_VALUE(lit(folded.columns[0].expr), sql::Value{-42LL});
  CHECK_VALUE(lit(folded.columns[1].expr), sql::Value{7LL});

  // ...but -(1) stays a unary node and must not print as "-1".
  const sql::SelectStatement unary = parse_select("SELECT -(1)");
  CHECK(unary.columns[0].expr.kind == sql::Expr::Kind::Unary);
  CHECK_EQ(sql::format(unary), std::string("SELECT -(1)"));
  CHECK_ROUND_TRIP("SELECT -(1)");
  CHECK_ROUND_TRIP("SELECT -(1 + 2), -(-x), ~1, ~-x, NOT NOT x");
}

TEST(expr_quantified_comparison) {
  const sql::SelectStatement any =
      parse_select("SELECT a FROM t WHERE a = ANY (SELECT b FROM u)");
  const sql::Expr& test = *any.where;
  CHECK(test.kind == sql::Expr::Kind::Quantified);
  CHECK_EQ(test.name, std::string("="));
  CHECK_EQ(test.parts[0], std::string("ANY"));

  const sql::SelectStatement all =
      parse_select("SELECT a FROM t WHERE a >= ALL (1, 2, 3)");
  CHECK(all.where->kind == sql::Expr::Kind::Quantified);
  CHECK_EQ(all.where->parts[0], std::string("ALL"));
  CHECK(all.where->args[1]->kind == sql::Expr::Kind::Row);

  CHECK_EQ(
      select_of("b <> SOME (SELECT x FROM y)"),
      std::string("SELECT b <> SOME (SELECT x FROM y)"));
  CHECK_ROUND_TRIP("SELECT a FROM t WHERE a = ANY (SELECT b FROM u)");
}

TEST(expr_any_without_paren_is_column) {
  // ANY/ALL/SOME are non-reserved: without a following '(' they are ordinary
  // column references, not quantified comparisons.
  const sql::SelectStatement s = parse_select("SELECT a FROM t WHERE a = any");
  CHECK(s.where->kind == sql::Expr::Kind::Binary);
  CHECK_EQ(s.where->name, std::string("="));
  CHECK(s.where->args[1]->kind == sql::Expr::Kind::ColumnRef);
  CHECK_EQ(s.where->args[1]->parts[0], std::string("any"));
  CHECK_ROUND_TRIP("SELECT a FROM t WHERE a = any");
}

// ---------------------------------------------------------------------------
// Predicates
// ---------------------------------------------------------------------------

TEST(expr_in_predicate) {
  const sql::SelectStatement s = parse_select("SELECT a FROM t WHERE a IN (1, 2)");
  CHECK(s.where->kind == sql::Expr::Kind::In);
  CHECK(!s.where->negated);
  CHECK_EQ(s.where->args.size(), 3u);  // a IN (1, 2)

  const sql::SelectStatement not_in =
      parse_select("SELECT a FROM t WHERE a NOT IN (SELECT b FROM u)");
  CHECK(not_in.where->kind == sql::Expr::Kind::In);
  CHECK(not_in.where->negated);
  CHECK(not_in.where->subquery != nullptr);
  CHECK_ROUND_TRIP("SELECT a FROM t WHERE a NOT IN (SELECT b FROM u)");
}

TEST(expr_between_predicate) {
  const sql::SelectStatement s =
      parse_select("SELECT a FROM t WHERE b BETWEEN 1 AND 5");
  CHECK(s.where->kind == sql::Expr::Kind::Between);
  CHECK_EQ(s.where->args.size(), 3u);
  CHECK_EQ(select_of("b NOT BETWEEN 1 AND 5"),
           std::string("SELECT b NOT BETWEEN 1 AND 5"));
  // BETWEEN binds looser than comparison.
  CHECK_EQ(select_of("b BETWEEN 1 AND 5 = c"),
           std::string("SELECT b BETWEEN 1 AND 5 = c"));
  CHECK_ROUND_TRIP("SELECT a FROM t WHERE b BETWEEN 1 AND 5");
}

TEST(expr_like_predicate) {
  CHECK_EQ(select_of("b LIKE '%x%'"),
           std::string("SELECT b LIKE '%x%'"));
  CHECK_EQ(select_of("b NOT LIKE '%x%'"),
           std::string("SELECT b NOT LIKE '%x%'"));
  CHECK_EQ(select_of("b ILIKE '%x%' ESCAPE '!'"),
           std::string("SELECT b ILIKE '%x%' ESCAPE '!'"));
  CHECK_EQ(select_of("b GLOB '[a-z]*'"),
           std::string("SELECT b GLOB '[a-z]*'"));
  CHECK_ROUND_TRIP("SELECT a FROM t WHERE b LIKE 'x%' ESCAPE '!'");
}

TEST(expr_is_null_and_boolean_tests) {
  CHECK_EQ(select_of("b IS NULL"), std::string("SELECT b IS NULL"));
  CHECK_EQ(select_of("b IS NOT NULL"), std::string("SELECT b IS NOT NULL"));
  CHECK_EQ(select_of("b IS TRUE"), std::string("SELECT b IS TRUE"));
  CHECK_EQ(select_of("b IS NOT FALSE"), std::string("SELECT b IS NOT FALSE"));
  CHECK_EQ(select_of("b IS UNKNOWN"), std::string("SELECT b IS UNKNOWN"));

  const sql::SelectStatement s = parse_select("SELECT b IS NOT NULL");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::NullTest);
  CHECK(s.columns[0].expr.negated);
}

TEST(expr_is_distinct_from) {
  CHECK_EQ(select_of("a IS DISTINCT FROM b"),
           std::string("SELECT a IS DISTINCT FROM b"));
  CHECK_EQ(select_of("a IS NOT DISTINCT FROM b"),
           std::string("SELECT a IS NOT DISTINCT FROM b"));
  const sql::SelectStatement s = parse_select("SELECT a IS DISTINCT FROM b");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Binary);
  CHECK_EQ(s.columns[0].expr.name, std::string("IS DISTINCT FROM"));
}

TEST(expr_exists_and_subquery) {
  const sql::SelectStatement s =
      parse_select("SELECT EXISTS (SELECT 1), NOT EXISTS (SELECT 2)");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Exists);
  CHECK(s.columns[1].expr.kind == sql::Expr::Kind::Unary);
  CHECK_EQ(s.columns[1].expr.name, std::string("NOT"));
  CHECK(s.columns[1].expr.args[0]->kind == sql::Expr::Kind::Exists);

  const sql::SelectStatement scalar = parse_select("SELECT (SELECT 1)");
  CHECK(scalar.columns[0].expr.kind == sql::Expr::Kind::Subquery);
  CHECK_EQ(sql::format(scalar), std::string("SELECT (SELECT 1)"));
}

// ---------------------------------------------------------------------------
// General forms
// ---------------------------------------------------------------------------

TEST(expr_case) {
  CHECK_EQ(select_of("CASE WHEN a > 1 THEN 1 END"),
           std::string("SELECT CASE WHEN a > 1 THEN 1 END"));
  CHECK_EQ(select_of("CASE a WHEN 1 THEN 'x' WHEN 2 THEN 'y' ELSE 'z' END"),
           std::string("SELECT CASE a WHEN 1 THEN 'x' WHEN 2 THEN 'y' "
                       "ELSE 'z' END"));

  const sql::SelectStatement s =
      parse_select("SELECT CASE WHEN a THEN 1 ELSE 2 END");
  const sql::Expr& case_expr = s.columns[0].expr;
  CHECK(case_expr.kind == sql::Expr::Kind::Case);
  CHECK(case_expr.operand == nullptr);
  CHECK_EQ(case_expr.args.size(), 1u);
  CHECK(case_expr.else_result != nullptr);
}

TEST(expr_cast_forms) {
  // CAST(...) and :: are the same node; the printer normalises to CAST.
  CHECK_EQ(select_of("CAST(a AS INT)"), std::string("SELECT CAST(a AS INT)"));
  CHECK_EQ(select_of("a::text"), std::string("SELECT CAST(a AS text)"));
  CHECK_EQ(select_of("b::numeric(10, 2)"),
           std::string("SELECT CAST(b AS numeric(10, 2))"));

  const sql::SelectStatement s = parse_select("SELECT a::text");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Cast);
  CHECK_EQ(s.columns[0].expr.type_name, std::string("text"));
}

TEST(expr_typed_datetime_literal) {
  const sql::SelectStatement s =
      parse_select("SELECT DATE '2020-01-01', INTERVAL '1 day'");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Cast);
  CHECK_EQ(s.columns[0].expr.type_name, std::string("DATE"));
  CHECK_VALUE(lit(*s.columns[0].expr.args[0]),
              sql::Value{std::string("2020-01-01")});
  CHECK_EQ(sql::format(s),
           std::string("SELECT CAST('2020-01-01' AS DATE), "
                       "CAST('1 day' AS INTERVAL)"));
  CHECK_ROUND_TRIP("SELECT TIMESTAMP '2020-01-01 12:00:00'");
}

TEST(expr_function_calls) {
  CHECK_EQ(select_of("count(DISTINCT a), count(*), upper(b)"),
           std::string("SELECT count(DISTINCT a), count(*), upper(b)"));

  const sql::SelectStatement s = parse_select("SELECT count(DISTINCT a)");
  const sql::Expr& call = s.columns[0].expr;
  CHECK(call.kind == sql::Expr::Kind::FunctionCall);
  CHECK(call.distinct);
  CHECK(!call.star);
  CHECK_EQ(call.parts[0], std::string("count"));

  const sql::SelectStatement star = parse_select("SELECT count(*)");
  CHECK(star.columns[0].expr.star);
}

TEST(expr_window_over) {
  const std::string text =
      "SELECT SUM(x) OVER (PARTITION BY y ORDER BY z ROWS BETWEEN "
      "UNBOUNDED PRECEDING AND CURRENT ROW) FROM t";
  const sql::SelectStatement s = parse_select(text);
  const sql::Expr& call = s.columns[0].expr;
  CHECK(call.kind == sql::Expr::Kind::FunctionCall);
  CHECK(call.over != nullptr);
  CHECK_EQ(call.over->partition_by.size(), 1u);
  CHECK_EQ(call.over->order_by.size(), 1u);
  CHECK_EQ(call.over->frame_type, std::string("ROWS"));
  CHECK(call.over->frame_between);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(expr_window_name) {
  CHECK_EQ(select_of("COUNT(*) OVER w"),
           std::string("SELECT COUNT(*) OVER w"));
}

TEST(expr_row_constructor) {
  const sql::SelectStatement s = parse_select("SELECT (a, b)");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Row);
  CHECK_EQ(s.columns[0].expr.args.size(), 2u);
  CHECK_EQ(sql::format(s), std::string("SELECT (a, b)"));
}

TEST(expr_subscript) {
  const sql::SelectStatement s = parse_select("SELECT a[1]");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Subscript);
  CHECK_EQ(sql::format(s), std::string("SELECT a[1]"));
  CHECK_EQ(select_of("(a + b)[1]"), std::string("SELECT (a + b)[1]"));
  CHECK_ROUND_TRIP("SELECT a[1], t.b[2][3]");
}

TEST(expr_collate) {
  CHECK_EQ(select_of("a COLLATE NOCASE"),
           std::string("SELECT a COLLATE NOCASE"));
  const sql::SelectStatement s = parse_select("SELECT a COLLATE NOCASE");
  CHECK(s.columns[0].expr.kind == sql::Expr::Kind::Collate);
}

TEST(expr_round_trip_batch) {
  const char* expressions[] = {
      "1 + 2 * 3 - 4 / 5 % 6",
      "a = b AND c <> d OR NOT e",
      "CASE WHEN a IN (1, 2) THEN b ELSE c END",
      "CAST(a AS VARCHAR(255))::text",
      "count(DISTINCT a) OVER (PARTITION BY b ORDER BY c)",
      "(SELECT max(x) FROM t) + 1",
      "a IS NOT NULL AND b BETWEEN 1 AND 2",
      "x LIKE 'a%' ESCAPE '!' OR y GLOB 'b*'",
      "NOT EXISTS (SELECT 1 FROM t WHERE a = b)",
      "f(a, b, c)",
      "t.col::numeric(5, 2)",
      "-a[1] * 2",
  };
  for (const char* expr_text : expressions) {
    CHECK_ROUND_TRIP(std::string("SELECT ") + expr_text);
  }
}
