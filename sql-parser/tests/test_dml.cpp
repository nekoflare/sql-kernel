// UPDATE / DELETE, RETURNING, WITH ... INSERT/UPDATE/DELETE.

#include "test_harness.hpp"

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

TEST(update_full) {
  const std::string text =
      "UPDATE t AS a SET b = 1 + 2, c = DEFAULT FROM u WHERE a.id = u.id "
      "RETURNING a.b";
  const sql::UpdateStatement s = parse_update(text);
  CHECK_EQ(s.table, std::string("t"));
  CHECK_EQ(s.alias, std::string("a"));
  CHECK_EQ(s.assignments.size(), 2u);
  CHECK_EQ(s.from.size(), 1u);
  CHECK(s.where != nullptr);
  CHECK_EQ(s.returning.size(), 1u);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(update_alias_without_as) {
  const sql::UpdateStatement s = parse_update("UPDATE t a SET x = 1");
  CHECK_EQ(s.alias, std::string("a"));
  CHECK_EQ(sql::format(s), std::string("UPDATE t AS a SET x = 1"));
}

TEST(update_assignment_list) {
  // PostgreSQL's SET (a, b) = (1, 2) is flattened into single assignments.
  const std::string text = "UPDATE t SET (a, b) = (1, 2), c = 3";
  const sql::UpdateStatement s = parse_update(text);
  CHECK_EQ(s.assignments.size(), 3u);
  CHECK_EQ(s.assignments[0].first, std::string("a"));
  CHECK_VALUE(lit(*s.assignments[0].second), sql::Value{1LL});
  CHECK_EQ(s.assignments[1].first, std::string("b"));
  CHECK_VALUE(lit(*s.assignments[1].second), sql::Value{2LL});
  CHECK_EQ(s.assignments[2].first, std::string("c"));
  CHECK_EQ(sql::format(s), std::string("UPDATE t SET a = 1, b = 2, c = 3"));
  CHECK_ROUND_TRIP(text);
}

TEST(update_assignment_list_mismatch) {
  CHECK_THROWS_MSG(
      parse_sql("UPDATE t SET (a, b) = (1, 2, 3)"),
      "expected 2 value(s) in assignment list, got 3");
}

TEST(update_with) {
  const sql::UpdateStatement s = parse_update(
      "WITH x AS (SELECT 1 AS n) UPDATE t SET a = n FROM x");
  CHECK_EQ(s.with.size(), 1u);
  CHECK_EQ(s.with[0].name, std::string("x"));
  CHECK_EQ(sql::format(s),
           std::string(
               "WITH x AS (SELECT 1 AS n) UPDATE t SET a = n FROM x"));
}

TEST(update_errors) {
  CHECK_THROWS_MSG(parse_sql("UPDATE t a = 1"), "'SET'");
  CHECK_THROWS_MSG(parse_sql("UPDATE t SET;"), "expected a name");
  CHECK_THROWS_MSG(parse_sql("UPDATE t SET a =;"), "expected an expression");
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

TEST(delete_simple) {
  const sql::DeleteStatement s = parse_delete("DELETE FROM t;");
  CHECK_EQ(s.table, std::string("t"));
  CHECK(s.where == nullptr);
  CHECK(s.returning.empty());
}

TEST(delete_where_returning) {
  const std::string text =
      "DELETE FROM t WHERE id IN (SELECT id FROM u) RETURNING *";
  const sql::DeleteStatement s = parse_delete(text);
  CHECK(s.where != nullptr);
  CHECK_EQ(s.returning.size(), 1u);
  CHECK(s.returning[0].expr.kind == sql::Expr::Kind::Star);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(delete_with_alias) {
  const sql::DeleteStatement s = parse_delete("DELETE FROM t AS x WHERE x.a = 1");
  CHECK_EQ(s.alias, std::string("x"));
  CHECK_EQ(sql::format(s), std::string("DELETE FROM t AS x WHERE x.a = 1"));
}

TEST(delete_with_cte) {
  const sql::DeleteStatement s = parse_delete(
      "WITH x AS (SELECT 1 AS n) DELETE FROM t WHERE id IN (SELECT n FROM x)");
  CHECK_EQ(s.with.size(), 1u);
}

TEST(delete_errors) {
  CHECK_THROWS_MSG(parse_sql("DELETE t WHERE a = 1"), "'FROM'");
  CHECK_THROWS_MSG(parse_sql("DELETE FROM;"), "expected a name");
}

// ---------------------------------------------------------------------------
// WITH shared by DML statements
// ---------------------------------------------------------------------------

TEST(with_recursive_flag_on_dml) {
  const sql::UpdateStatement update =
      parse_update("WITH RECURSIVE t (n) AS (SELECT 1) UPDATE x SET a = n");
  CHECK(update.recursive);
  CHECK_EQ(update.with[0].columns.size(), 1u);
  CHECK_ROUND_TRIP(
      "WITH RECURSIVE t (n) AS (SELECT 1) UPDATE x SET a = n");

  const sql::InsertStatement insert = parse_insert(
      "WITH RECURSIVE t (n) AS (SELECT 1) INSERT INTO x SELECT n FROM t");
  CHECK(insert.recursive);
  CHECK(insert.select != nullptr);
}

TEST(with_rejects_ddl) {
  CHECK_THROWS_MSG(parse_sql("WITH x AS (SELECT 1) CREATE TABLE t (a INT)"),
                   "WITH is not allowed before CREATE");
  CHECK_THROWS_MSG(parse_sql("WITH x AS (SELECT 1) DROP TABLE t"),
                   "WITH is not allowed before DROP");
}

TEST(dml_quoted_identifiers) {
  CHECK_ROUND_TRIP(
      "INSERT INTO \"order\" (\"values\", \"select\") VALUES (1, 2) "
      "RETURNING \"values\"");
  CHECK_ROUND_TRIP("UPDATE \"my table\" SET \"set\" = 1 WHERE \"order\" = 'x'");
  CHECK_ROUND_TRIP(
      "DELETE FROM \"my table\" WHERE \"col with space\" IS NOT NULL");
  CHECK_ROUND_TRIP(
      "UPDATE t SET (\"a b\", plain) = (1, 2)");
}
