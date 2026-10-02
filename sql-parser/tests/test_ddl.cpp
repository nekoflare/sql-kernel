// DDL: CREATE / DROP / ALTER and transaction statements.

#include "test_harness.hpp"

// ---------------------------------------------------------------------------
// CREATE TABLE
// ---------------------------------------------------------------------------

TEST(create_table_basic) {
  const std::string text = "CREATE TABLE users (id INTEGER, name TEXT)";
  const sql::CreateTableStatement s =
      std::get<sql::CreateTableStatement>(parse_sql(text));
  CHECK_EQ(s.name, std::string("users"));
  CHECK_EQ(s.columns.size(), 2u);
  CHECK_EQ(s.columns[0].type_name, std::string("INTEGER"));
  CHECK(!s.temporary);
  CHECK(!s.if_not_exists);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(create_table_all_constraints) {
  const std::string text =
      "CREATE TABLE IF NOT EXISTS t ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "name VARCHAR(255) NOT NULL DEFAULT 'x', "
      "email TEXT UNIQUE COLLATE NOCASE, "
      "score REAL CHECK (score >= 0), "
      "mgr INT REFERENCES emp (id) ON DELETE SET NULL "
      "DEFERRABLE INITIALLY DEFERRED, "
      "UNIQUE (name, email), "
      "CHECK (id > 0), "
      "FOREIGN KEY (mgr) REFERENCES emp (id) ON UPDATE CASCADE)";
  const sql::CreateTableStatement s =
      std::get<sql::CreateTableStatement>(parse_sql(text));
  CHECK(s.if_not_exists);
  CHECK_EQ(s.columns.size(), 5u);
  CHECK_EQ(s.constraints.size(), 3u);

  const sql::ColumnDef& id = s.columns[0];
  CHECK_EQ(id.constraints.size(), 2u);  // PRIMARY KEY + AUTOINCREMENT
  CHECK(id.constraints[0].kind == sql::ColumnConstraint::Kind::PrimaryKey);
  CHECK(id.constraints[1].kind == sql::ColumnConstraint::Kind::AutoIncrement);

  const sql::ColumnDef& name = s.columns[1];
  CHECK_EQ(name.constraints.size(), 2u);  // NOT NULL + DEFAULT

  const sql::ColumnDef& email = s.columns[2];
  CHECK_EQ(email.constraints.size(), 2u);  // UNIQUE + COLLATE

  const sql::ColumnDef& mgr = s.columns[4];
  CHECK_EQ(mgr.constraints.size(), 1u);
  CHECK(mgr.constraints[0].deferrable);
  CHECK(mgr.constraints[0].initially_deferred);
  CHECK_EQ(mgr.constraints[0].ref_on_delete, std::string("SET NULL"));

  CHECK(s.constraints[0].kind == sql::TableConstraint::Kind::Unique);
  CHECK(s.constraints[1].kind == sql::TableConstraint::Kind::Check);
  CHECK(s.constraints[2].kind == sql::TableConstraint::Kind::Foreign);
  CHECK_EQ(s.constraints[2].ref_on_update, std::string("CASCADE"));

  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(create_table_variants) {
  CHECK_ROUND_TRIP("CREATE TEMP TABLE c AS SELECT a, b FROM t");
  CHECK_ROUND_TRIP("CREATE TABLE db.t (a INT, b)");  // untyped column
  CHECK_ROUND_TRIP("CREATE TABLE t (a INT, PRIMARY KEY (a))");

  const sql::CreateTableStatement temp =
      std::get<sql::CreateTableStatement>(
          parse_sql("CREATE TEMP TABLE c AS SELECT a FROM t"));
  CHECK(temp.temporary);
  CHECK(temp.select != nullptr);
  CHECK(temp.columns.empty());
}

TEST(create_table_errors) {
  CHECK_THROWS_MSG(parse_sql("CREATE TABLE t;"), "'('");
  CHECK_THROWS_MSG(parse_sql("CREATE FOO t"), "expected TABLE, INDEX or VIEW");
}

// ---------------------------------------------------------------------------
// CREATE INDEX / VIEW
// ---------------------------------------------------------------------------

TEST(create_index) {
  const std::string text =
      "CREATE UNIQUE INDEX IF NOT EXISTS idx ON t (a COLLATE NOCASE DESC, b)";
  const sql::CreateIndexStatement s =
      std::get<sql::CreateIndexStatement>(parse_sql(text));
  CHECK(s.unique);
  CHECK(s.if_not_exists);
  CHECK_EQ(s.name, std::string("idx"));
  CHECK_EQ(s.table, std::string("t"));
  CHECK_EQ(s.columns.size(), 2u);
  CHECK(s.columns[0].descending);
  CHECK(!s.columns[1].descending);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
}

TEST(create_view) {
  const std::string text = "CREATE OR REPLACE VIEW v (x, y) AS SELECT 1, 2";
  const sql::CreateViewStatement s =
      std::get<sql::CreateViewStatement>(parse_sql(text));
  CHECK(s.or_replace);
  CHECK_EQ(s.name, std::string("v"));
  CHECK_EQ(s.columns.size(), 2u);
  CHECK(s.query != nullptr);
  CHECK_EQ(sql::format(s), text);
  CHECK_ROUND_TRIP(text);
  CHECK_ROUND_TRIP("CREATE TEMP VIEW v AS SELECT 1");
}

// ---------------------------------------------------------------------------
// DROP / ALTER
// ---------------------------------------------------------------------------

TEST(drop_variants) {
  const sql::DropStatement tables =
      std::get<sql::DropStatement>(parse_sql("DROP TABLE IF EXISTS a, b CASCADE"));
  CHECK_EQ(tables.object_type, std::string("TABLE"));
  CHECK(tables.if_exists);
  CHECK_EQ(tables.names.size(), 2u);
  CHECK_EQ(tables.option, std::string("CASCADE"));

  const sql::DropStatement index =
      std::get<sql::DropStatement>(parse_sql("DROP INDEX idx"));
  CHECK_EQ(index.object_type, std::string("INDEX"));
  CHECK(!index.if_exists);

  const sql::DropStatement view =
      std::get<sql::DropStatement>(parse_sql("DROP VIEW IF EXISTS v"));
  CHECK_EQ(view.object_type, std::string("VIEW"));

  CHECK_ROUND_TRIP("DROP TABLE IF EXISTS a, b CASCADE");
  CHECK_ROUND_TRIP("DROP INDEX IF EXISTS idx");
  CHECK_ROUND_TRIP("DROP VIEW v RESTRICT");
}

TEST(alter_table) {
  const sql::AlterTableStatement add =
      std::get<sql::AlterTableStatement>(
          parse_sql("ALTER TABLE IF EXISTS t ADD COLUMN c INT NOT NULL DEFAULT 0"));
  CHECK(add.action == sql::AlterTableStatement::Action::AddColumn);
  CHECK(add.if_exists);
  CHECK_EQ(add.column.name, std::string("c"));
  CHECK_EQ(add.column.type_name, std::string("INT"));
  CHECK_EQ(add.column.constraints.size(), 2u);

  const sql::AlterTableStatement drop =
      std::get<sql::AlterTableStatement>(
          parse_sql("ALTER TABLE t DROP COLUMN IF EXISTS c"));
  CHECK(drop.action == sql::AlterTableStatement::Action::DropColumn);
  CHECK(drop.if_exists == false);
  CHECK(drop.column_if_exists);
  CHECK_EQ(drop.column_name, std::string("c"));

  const sql::AlterTableStatement rename_table =
      std::get<sql::AlterTableStatement>(parse_sql("ALTER TABLE t RENAME TO u"));
  CHECK(rename_table.action ==
        sql::AlterTableStatement::Action::RenameTable);
  CHECK_EQ(rename_table.new_name, std::string("u"));

  const sql::AlterTableStatement rename_column =
      std::get<sql::AlterTableStatement>(
          parse_sql("ALTER TABLE t RENAME COLUMN a TO b"));
  CHECK(rename_column.action ==
        sql::AlterTableStatement::Action::RenameColumn);
  CHECK_EQ(rename_column.column_name, std::string("a"));
  CHECK_EQ(rename_column.new_name, std::string("b"));

  CHECK_ROUND_TRIP("ALTER TABLE IF EXISTS t ADD COLUMN c INT DEFAULT 0");
  CHECK_ROUND_TRIP("ALTER TABLE IF EXISTS t DROP COLUMN IF EXISTS c");
  CHECK_ROUND_TRIP("ALTER TABLE t RENAME TO u");
  CHECK_ROUND_TRIP("ALTER TABLE t RENAME COLUMN a TO b");
}

TEST(alter_table_errors) {
  CHECK_THROWS_MSG(parse_sql("ALTER TABLE t;"), "expected ADD, DROP or RENAME");
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------

TEST(transaction_statements) {
  const sql::TransactionStatement begin =
      std::get<sql::TransactionStatement>(parse_sql("BEGIN;"));
  CHECK_EQ(begin.action, std::string("BEGIN"));

  // BEGIN TRANSACTION / WORK / START TRANSACTION normalise to BEGIN.
  const sql::TransactionStatement with_keyword =
      std::get<sql::TransactionStatement>(parse_sql("BEGIN TRANSACTION;"));
  CHECK_EQ(with_keyword.action, std::string("BEGIN"));
  CHECK_EQ(sql::format(with_keyword), std::string("BEGIN"));

  const sql::TransactionStatement start =
      std::get<sql::TransactionStatement>(parse_sql("START TRANSACTION;"));
  CHECK_EQ(start.action, std::string("BEGIN"));

  const sql::TransactionStatement commit =
      std::get<sql::TransactionStatement>(parse_sql("COMMIT;"));
  CHECK_EQ(commit.action, std::string("COMMIT"));

  const sql::TransactionStatement rollback =
      std::get<sql::TransactionStatement>(parse_sql("ROLLBACK WORK;"));
  CHECK_EQ(rollback.action, std::string("ROLLBACK"));

  CHECK_ROUND_TRIP("BEGIN");
  CHECK_ROUND_TRIP("START TRANSACTION");
  CHECK_ROUND_TRIP("COMMIT");
  CHECK_ROUND_TRIP("ROLLBACK");
}

TEST(transaction_errors) {
  CHECK_THROWS_MSG(parse_sql("START NOW"), "'INSERT'");
  CHECK_THROWS_MSG(parse_sql("COMMIT STUFF"), "trailing input");
}

TEST(ddl_quoted_identifiers) {
  CHECK_ROUND_TRIP(
      "CREATE TABLE \"order\" (\"select\" INT, \"col with space\" TEXT, "
      "PRIMARY KEY (\"select\"))");
  CHECK_ROUND_TRIP("CREATE OR REPLACE VIEW \"my view\" (\"x y\") AS SELECT 1");
  CHECK_ROUND_TRIP(
      "CREATE UNIQUE INDEX \"my idx\" ON \"order\" (\"col with space\" DESC)");
  CHECK_ROUND_TRIP("DROP TABLE \"my table\", \"order\" CASCADE");
  CHECK_ROUND_TRIP(
      "ALTER TABLE \"order\" RENAME COLUMN \"col with space\" TO plain");

  // Type names stay raw unless they would not re-lex.
  const sql::CreateTableStatement typed = std::get<sql::CreateTableStatement>(
      parse_sql("CREATE TABLE t (a VARCHAR(255), b DOUBLE PRECISION)"));
  CHECK_EQ(sql::format(typed),
           std::string("CREATE TABLE t (a VARCHAR(255), b DOUBLE PRECISION)"));
}
