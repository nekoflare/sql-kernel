// SELECT: projection, FROM/joins, grouping, ordering, set operations, CTEs.

#include "test_harness.hpp"

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

TEST(select_projection_forms) {
  const std::string text = "SELECT a, b AS c, d e, *, t.* FROM t";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.columns.size(), 5u);
  CHECK_EQ(s.columns[1].alias, std::string("c"));
  CHECK_EQ(s.columns[2].alias, std::string("e"));  // implicit alias
  CHECK(s.columns[3].expr.kind == sql::Expr::Kind::Star);
  CHECK(s.columns[4].expr.kind == sql::Expr::Kind::Star);
  CHECK_EQ(s.columns[4].expr.parts.size(), 1u);  // t.*
  CHECK_EQ(sql::format(s), std::string("SELECT a, b AS c, d AS e, *, t.* FROM t"));
  CHECK_ROUND_TRIP(text);
}

TEST(select_distinct_variants) {
  const sql::SelectStatement distinct = parse_select("SELECT DISTINCT a FROM t");
  CHECK(distinct.distinct);
  CHECK(!distinct.all);

  const sql::SelectStatement all = parse_select("SELECT ALL a FROM t");
  CHECK(all.all);
  CHECK(!all.distinct);

  const sql::SelectStatement on =
      parse_select("SELECT DISTINCT ON (a, b) a, b, c FROM t ORDER BY a, b");
  CHECK(on.distinct);
  CHECK_EQ(on.distinct_on.size(), 2u);
  CHECK_EQ(sql::format(on),
           std::string(
               "SELECT DISTINCT ON (a, b) a, b, c FROM t ORDER BY a, b"));
  CHECK_ROUND_TRIP("SELECT DISTINCT ON (a) a FROM t");
}

// ---------------------------------------------------------------------------
// FROM and joins
// ---------------------------------------------------------------------------

TEST(select_from_simple) {
  const sql::SelectStatement s = parse_select("SELECT a FROM users");
  CHECK_EQ(s.from.size(), 1u);
  CHECK(s.from[0].kind == sql::TableRef::Kind::Table);
  CHECK_EQ(s.from[0].name, std::string("users"));

  const sql::SelectStatement qualified =
      parse_select("SELECT a FROM app.users AS u");
  CHECK_EQ(qualified.from[0].name, std::string("app.users"));
  CHECK_EQ(qualified.from[0].alias, std::string("u"));
}

TEST(select_from_joins) {
  const std::string text =
      "SELECT * FROM a JOIN b ON a.id = b.id LEFT JOIN c USING (id) "
      "CROSS JOIN d";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.from.size(), 1u);  // one left-associative join chain
  const sql::TableRef& outer = s.from[0];
  CHECK(outer.kind == sql::TableRef::Kind::Join);
  CHECK(outer.right != nullptr);
  CHECK_EQ(outer.right->name, std::string("d"));

  const sql::TableRef& middle = *outer.left;
  CHECK(middle.kind == sql::TableRef::Kind::Join);
  CHECK_EQ(middle.right->name, std::string("c"));
  CHECK_EQ(middle.using_columns.size(), 1u);

  const sql::TableRef& inner = *middle.left;
  CHECK(inner.kind == sql::TableRef::Kind::Join);
  CHECK_EQ(inner.left->name, std::string("a"));
  CHECK_EQ(inner.right->name, std::string("b"));
  CHECK(inner.on != nullptr);
  CHECK_ROUND_TRIP(text);
}

TEST(select_from_natural_and_parenthesized) {
  CHECK_ROUND_TRIP("SELECT * FROM a NATURAL JOIN b");
  CHECK_ROUND_TRIP("SELECT * FROM (SELECT 1) AS x JOIN (SELECT 2) AS y "
                   "ON TRUE");
}

TEST(select_from_subquery_columns) {
  const std::string text =
      "SELECT g.n FROM generate_series(1, 10) AS g(n)";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.from.size(), 1u);
  CHECK(s.from[0].kind == sql::TableRef::Kind::TableFunction);
  CHECK_EQ(s.from[0].alias, std::string("g"));
  CHECK_EQ(s.from[0].column_aliases.size(), 1u);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(select_natural_join_rejects_condition) {
  CHECK_THROWS_MSG(parse_sql("SELECT * FROM a NATURAL JOIN b ON 1 = 1"),
                   "NATURAL JOIN cannot have a join condition");
}

// ---------------------------------------------------------------------------
// WHERE / GROUP BY / HAVING / ORDER BY / LIMIT
// ---------------------------------------------------------------------------

TEST(select_where) {
  const sql::SelectStatement s =
      parse_select("SELECT a FROM t WHERE x > 1 AND y < 2");
  CHECK(s.where != nullptr);
  CHECK_EQ(sql::format(s), std::string("SELECT a FROM t WHERE x > 1 AND y < 2"));
}

TEST(select_group_by_having) {
  const std::string text =
      "SELECT count(*), a FROM t GROUP BY a HAVING count(*) > 1";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.group_by.size(), 1u);
  CHECK(s.having != nullptr);
  CHECK_EQ(sql::format(s), text);

  const sql::SelectStatement all = parse_select("SELECT a FROM t GROUP BY ALL");
  CHECK(all.group_all);
  CHECK_ROUND_TRIP(text);
}

TEST(select_order_by_limit) {
  const std::string text =
      "SELECT a FROM t ORDER BY 1 NULLS LAST, a DESC NULLS FIRST "
      "LIMIT 5 OFFSET 2";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.order_by.size(), 2u);
  CHECK(!s.order_by[0].descending);
  CHECK_EQ(s.order_by[0].nulls, std::string("LAST"));
  CHECK(s.order_by[1].descending);
  CHECK_EQ(s.order_by[1].nulls, std::string("FIRST"));
  CHECK(s.limit != nullptr);
  CHECK(s.offset != nullptr);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(select_limit_all_and_parameters) {
  const sql::SelectStatement limit_all =
      parse_select("SELECT * FROM t LIMIT ALL");
  CHECK(limit_all.limit_all);
  CHECK(limit_all.limit == nullptr);

  CHECK_ROUND_TRIP("SELECT a FROM t LIMIT ? OFFSET ?");
  CHECK_ROUND_TRIP("SELECT a FROM t ORDER BY a LIMIT 10");
}

// ---------------------------------------------------------------------------
// Set operations
// ---------------------------------------------------------------------------

TEST(select_union_variants) {
  const std::string text = "SELECT 1 UNION ALL SELECT 2";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.set_op, std::string("UNION"));
  CHECK(s.set_all);
  CHECK(s.set_left != nullptr);
  CHECK(s.set_right != nullptr);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(select_intersect_binds_tighter) {
  const sql::SelectStatement s =
      parse_select("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3");
  CHECK_EQ(s.set_op, std::string("UNION"));
  CHECK(s.set_right != nullptr);
  CHECK_EQ(s.set_right->set_op, std::string("INTERSECT"));
  CHECK_ROUND_TRIP("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3 EXCEPT SELECT 4");
}

TEST(select_set_op_parentheses) {
  // A tail (ORDER BY/LIMIT) forces parentheses; the tail-less right operand
  // is printed without them.
  const std::string text = "(SELECT 1 ORDER BY a LIMIT 1) UNION (SELECT 2)";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.set_op, std::string("UNION"));
  CHECK_EQ(sql::format(s),
           std::string("(SELECT 1 ORDER BY a LIMIT 1) UNION SELECT 2"));
  CHECK_ROUND_TRIP(text);
}

TEST(select_values_statement) {
  const sql::SelectStatement s = parse_select("VALUES (1, 2), (3, 4)");
  CHECK(s.is_values);
  CHECK_EQ(s.value_rows.size(), 2u);
  CHECK_EQ(s.value_rows[0].size(), 2u);
  CHECK_EQ(sql::format(s), std::string("VALUES (1, 2), (3, 4)"));
}

// ---------------------------------------------------------------------------
// Common table expressions
// ---------------------------------------------------------------------------

TEST(select_cte_basic) {
  const std::string text =
      "WITH a AS (SELECT 1 AS n), b AS (SELECT 2 AS n) SELECT * FROM a, b";
  const sql::SelectStatement s = parse_select(text);
  CHECK_EQ(s.with.size(), 2u);
  CHECK_EQ(s.with[0].name, std::string("a"));
  CHECK(!s.recursive);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(select_cte_recursive_with_columns) {
  const std::string text =
      "WITH RECURSIVE t (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t "
      "WHERE n < 10) SELECT sum(n) FROM t";
  const sql::SelectStatement s = parse_select(text);
  CHECK(s.recursive);
  CHECK_EQ(s.with.size(), 1u);
  CHECK_EQ(s.with[0].columns.size(), 1u);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(select_cte_not_materialized) {
  const sql::SelectStatement s =
      parse_select("WITH x AS NOT MATERIALIZED (SELECT 1) SELECT * FROM x");
  CHECK(s.with[0].not_materialized);
  CHECK_EQ(sql::format(s),
           std::string(
               "WITH x AS NOT MATERIALIZED (SELECT 1) SELECT * FROM x"));
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST(select_error_missing_items) {
  CHECK_THROWS_MSG(parse_sql("SELECT FROM t"), "expected an expression");
  CHECK_THROWS_MSG(parse_sql("WITH x AS SELECT 1"), "'('");
}

TEST(select_error_missing_from_target) {
  CHECK_THROWS_MSG(parse_sql("SELECT a FROM;"), "expected a name");
}

TEST(select_error_trailing) {
  CHECK_THROWS_MSG(parse_sql("SELECT a FROM t WHERE x = 1 oops"),
                   "trailing input");
}

TEST(select_error_unknown_statement) {
  CHECK_THROWS_MSG(parse_sql("TRUNCATE t"), "'SELECT'");
}

// ---------------------------------------------------------------------------  
// Identifier quoting (printer must re-quote names that need it)
// ---------------------------------------------------------------------------

TEST(select_quoted_identifiers) {
  CHECK_ROUND_TRIP("SELECT a[1], t.\"col with space\" FROM t");
  CHECK_ROUND_TRIP(
      "SELECT a AS \"my alias\", \"Order\" FROM \"order\"");
  CHECK_ROUND_TRIP(
      "SELECT * FROM \"my table\" AS \"my alias\"(\"c1\", \"c2\")");
  CHECK_ROUND_TRIP(
      "WITH \"my cte\" (\"a b\") AS (SELECT 1) SELECT \"a b\" FROM \"my cte\"");

  // Reserved words are re-quoted in every case; plain identifiers stay bare.
  const sql::SelectStatement reserved =
      parse_select("SELECT \"Order\" FROM \"order\"");
  CHECK_EQ(sql::format(reserved),
           std::string("SELECT \"Order\" FROM \"order\""));
  const sql::SelectStatement plain = parse_select("SELECT MixedCase FROM t");
  CHECK_EQ(sql::format(plain), std::string("SELECT MixedCase FROM t"));
}
