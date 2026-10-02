// Validator tests: catalog/arity, system-table contracts, types, recursive
// CTE structure — plus issue positions.

#include "test_harness.hpp"

#include <string>
#include <vector>

#include "sqlos/validate.hpp"

namespace {

sqlos::ValidationResult check(const std::string& src) {
  return sqlos::validate_source(src);
}

bool has_issue(const sqlos::ValidationResult& result,
               const std::string& needle) {
  for (const sqlos::Issue& issue : result.issues) {
    if (issue.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string all_issues(const sqlos::ValidationResult& result) {
  std::string out;
  for (const sqlos::Issue& issue : result.issues) {
    if (!out.empty()) out += "; ";
    out += sqlos::format_issue(issue);
  }
  return out.empty() ? std::string("<no issues>") : out;
}

const char* kSchema =
    "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT, active BOOL);"
    "CREATE TABLE orders (id INT, user_id INT, total REAL);"
    "CREATE TABLE nodes (id INT, parent INT, depth INT, payload TEXT);"
    "CREATE TABLE logs (id INT, msg TEXT, CHECK (id >= 0));";

}  // namespace

#define CHECK_VALID(src)                                                    \
  do {                                                                      \
    ++::testing::check_count();                                             \
    const sqlos::ValidationResult result_ = check(src);                     \
    if (!result_.ok()) {                                                    \
      ::testing::report_failure(__FILE__, __LINE__,                         \
                                std::string("expected valid, got: ") +      \
                                    all_issues(result_) +                   \
                                    std::string("\n  in: ") + (src));       \
    }                                                                       \
  } while (0)

#define CHECK_INVALID(src, needle)                                          \
  do {                                                                      \
    ++::testing::check_count();                                             \
    const sqlos::ValidationResult result_ = check(src);                     \
    if (result_.ok()) {                                                     \
      ::testing::report_failure(__FILE__, __LINE__,                         \
                                std::string("expected issue \"") + (needle) +\
                                    "\", but program is valid: " + (src));  \
    } else if (!has_issue(result_, (needle))) {                             \
      ::testing::report_failure(__FILE__, __LINE__,                         \
                                std::string("no issue containing \"") +     \
                                    (needle) + "\", got: " +                \
                                    all_issues(result_) +                   \
                                    std::string("\n  in: ") + (src));       \
    }                                                                       \
  } while (0)

// ---------------------------------------------------------------------------
// Catalog + arity
// ---------------------------------------------------------------------------

TEST(valid_program) { CHECK_VALID(std::string(kSchema) + "BEGIN; COMMIT;"); }

TEST(system_catalog_preexists) {
  const sqlos::ValidationResult result = check("SELECT 1");
  CHECK(result.ok());
  const sqlos::Table* io = result.catalog.find("io_8_read");
  CHECK(io != nullptr);
  if (io != nullptr) {
    CHECK(io->system());
    CHECK_EQ(io->io_width, 8);
    CHECK_EQ(io->columns.size(), 2u);
  }
  CHECK(result.catalog.find("io_32_write") != nullptr);
  const sqlos::Table* mem = result.catalog.find("volatile_memory");
  CHECK(mem != nullptr);
  if (mem != nullptr) {
    CHECK(mem->sys == sqlos::SystemClass::Memory);
    CHECK_EQ(mem->columns[0].name, std::string("address"));
  }
}

TEST(unknown_table) {
  CHECK_INVALID("SELECT * FROM nope", "unknown table 'nope'");
}

TEST(unknown_column) {
  CHECK_INVALID(std::string(kSchema) + "SELECT nam FROM users",
                "unknown column 'nam'");
}

TEST(qualified_unknown_column) {
  CHECK_INVALID(std::string(kSchema) + "SELECT users.nam FROM users",
                "unknown column 'users.nam'");
}

TEST(ambiguous_column) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT id FROM users JOIN orders ON users.id = "
                    "orders.user_id WHERE id = 1",
                "column 'id' is ambiguous");
}

TEST(duplicate_relation) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users, users",
                "relation name 'users' appears more than once");
}

TEST(duplicate_alias) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users u, orders u",
                "relation name 'u' appears more than once");
}

TEST(column_alias_arity) {
  CHECK_INVALID(
      std::string(kSchema) +
          "SELECT * FROM (SELECT id, name FROM users) AS t (a)",
      "relation 't' declares 1 column(s) but has 2");
}

TEST(asterisk_without_from) { CHECK_INVALID("SELECT *", "asterisk requires"); }

TEST(insert_arity) {
  // Column-list arity is caught by the parser; without a column list the
  // validator compares against the target's full column list.
  CHECK_INVALID(std::string(kSchema) + "INSERT INTO users (id, name) VALUES (1)",
                "expected 2 value(s) in row");
  CHECK_INVALID(std::string(kSchema) + "INSERT INTO users VALUES (1)",
                "row 1 has 1 value(s) but 4 column(s) are targeted");
  CHECK_INVALID(std::string(kSchema) +
                    "INSERT INTO users (id, name) VALUES (1, 'a'), (2, 'b', 3)",
                "expected 2 value(s) in row");
}

TEST(insert_unknown_column) {
  CHECK_INVALID(std::string(kSchema) + "INSERT INTO users (bogus) VALUES (1)",
                "unknown column 'bogus'");
}

TEST(insert_valid) {
  CHECK_VALID(std::string(kSchema) +
              "INSERT INTO users (id, name, age, active) VALUES "
              "(1, 'ann', 30, TRUE);"
              "INSERT INTO orders VALUES (1, 1, 9.99);"
              "INSERT INTO users (id) VALUES (?)");
}

TEST(insert_select_width) {
  CHECK_INVALID(std::string(kSchema) +
                    "INSERT INTO users (id, name) SELECT id FROM orders",
                "SELECT returns 1 column(s) but 2 column(s) are targeted");
}

TEST(setop_width) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT id FROM users UNION ALL SELECT id, name FROM users",
                "different numbers of columns (1 vs 2)");
}

TEST(setop_type) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT id FROM users UNION ALL SELECT name FROM users",
                "set operation column 1 has incompatible types (INT vs TEXT)");
}

TEST(setop_valid) {
  CHECK_VALID(std::string(kSchema) +
              "SELECT id FROM users UNION SELECT id FROM orders");
}

TEST(values_row_width) {
  CHECK_INVALID("VALUES (1), (1, 2)", "VALUES row 2 has 2 value(s) but row 1 has 1");
}

TEST(values_type) {
  CHECK_INVALID("VALUES (1), ('x')",
                "VALUES column 1 has incompatible types (INT vs TEXT)");
}

TEST(subquery_single_column) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT id FROM users WHERE id IN (SELECT id, name FROM users)",
                "subquery must return exactly one column (got 2)");
}

TEST(order_by_position) {
  CHECK_INVALID(std::string(kSchema) + "SELECT id FROM users ORDER BY 5",
                "ORDER BY position 5 is not in the select list (1..1)");
}

TEST(group_by_position) {
  CHECK_INVALID(std::string(kSchema) + "SELECT id FROM users GROUP BY 9",
                "GROUP BY position 9 is not in the select list (1..1)");
}

TEST(order_by_alias_and_column) {
  CHECK_VALID(std::string(kSchema) +
              "SELECT id AS x FROM users ORDER BY x;"
              "SELECT id FROM users ORDER BY name, 1");
}

TEST(using_column_missing) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT name FROM users JOIN orders USING (user_id)",
                "USING column 'user_id' must exist in both joined tables");
}

TEST(using_valid) {
  CHECK_VALID(std::string(kSchema) +
              "SELECT name FROM users JOIN orders USING (id)");
}

TEST(join_condition_bool) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT id FROM users JOIN orders ON 5",
                "join condition must be boolean, got INT");
}

TEST(limit_type) {
  CHECK_INVALID(std::string(kSchema) + "SELECT id FROM users LIMIT 'x'",
                "LIMIT must be numeric, got TEXT");
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

TEST(where_must_be_boolean) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users WHERE 5",
                "WHERE condition must be boolean, got INT");
  CHECK_VALID(std::string(kSchema) + "SELECT * FROM users WHERE active");
}

TEST(and_needs_boolean) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users WHERE active AND 1",
                "right operand of 'AND' must be boolean, got INT");
}

TEST(comparison_types) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users WHERE name = 5",
                "cannot compare TEXT with INT");
}

TEST(insert_type_mismatch) {
  CHECK_INVALID(std::string(kSchema) + "INSERT INTO users (id) VALUES ('x')",
                "type mismatch: cannot store TEXT in INT column 'id'");
  CHECK_INVALID(std::string(kSchema) + "INSERT INTO users (name) VALUES (5)",
                "type mismatch: cannot store INT in TEXT column 'name'");
}

TEST(numeric_widening_ok) {
  CHECK_VALID(std::string(kSchema) + "INSERT INTO orders (total) VALUES (1)");
}

TEST(update_type_mismatch) {
  CHECK_INVALID(std::string(kSchema) + "UPDATE users SET name = 5",
                "type mismatch: cannot store INT in TEXT column 'name'");
}

TEST(update_unknown_column) {
  CHECK_INVALID(std::string(kSchema) + "UPDATE users SET bogus = 1",
                "unknown column 'bogus'");
}

TEST(update_valid) {
  CHECK_VALID(std::string(kSchema) +
              "UPDATE users SET name = 'zed' WHERE id = 1;"
              "DELETE FROM users WHERE id = 2");
}

TEST(function_unknown) {
  CHECK_INVALID(std::string(kSchema) + "SELECT foo(1) FROM users",
                "unknown function 'foo'");
}

TEST(function_arity) {
  CHECK_INVALID(std::string(kSchema) + "SELECT count(1, 2) FROM users",
                "function 'count' expects exactly 1 argument, got 2");
  CHECK_INVALID(std::string(kSchema) + "SELECT substr('abc') FROM users",
                "function 'substr' expects 2 to 3 arguments, got 1");
}

TEST(function_types) {
  CHECK_INVALID(std::string(kSchema) + "SELECT sum(name) FROM users",
                "argument of 'sum' must be numeric, got TEXT");
  CHECK_INVALID(std::string(kSchema) + "SELECT lower(id) FROM users",
                "argument of 'lower' must be text, got INT");
  CHECK_VALID(std::string(kSchema) +
              "SELECT count(*), min(age), coalesce(name, 'x') FROM users "
              "GROUP BY age");
}

TEST(not_requires_bool) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users WHERE NOT 5",
                "NOT requires a boolean operand, got INT");
}

TEST(is_true_requires_bool) {
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM users WHERE id IS TRUE",
                "argument of 'IS TRUE' must be boolean, got INT");
}

TEST(case_branch_types) {
  CHECK_INVALID(std::string(kSchema) +
                    "SELECT CASE WHEN active THEN 1 ELSE 'x' END FROM users",
                "CASE branches have incompatible types (INT vs TEXT)");
  CHECK_INVALID(std::string(kSchema) + "SELECT CASE WHEN 5 THEN 1 END FROM users",
                "CASE WHEN condition must be boolean, got INT");
}

TEST(coalesce_types) {
  CHECK_INVALID(std::string(kSchema) + "SELECT coalesce(id, name) FROM users",
                "coalesce arguments have incompatible types (INT vs TEXT)");
}

TEST(default_value_type) {
  CHECK_INVALID("CREATE TABLE t (a INT DEFAULT 'x')",
                "type mismatch: cannot store TEXT in INT column 'a'");
}

TEST(check_constraint_bool) {
  CHECK_INVALID("CREATE TABLE t (x INT, CHECK (x + 1))",
                "CHECK constraint must be boolean, got INT");
}

TEST(default_values_rules) {
  CHECK_INVALID(std::string(kSchema) + "CREATE TABLE t (a INT NOT NULL);"
                                        "INSERT INTO t DEFAULT VALUES",
                "column 'a' is NOT NULL and has no DEFAULT");
  CHECK_VALID(std::string(kSchema) + "CREATE TABLE t (a INT);"
                                      "INSERT INTO t DEFAULT VALUES");
}

// ---------------------------------------------------------------------------
// System-table contracts
// ---------------------------------------------------------------------------

TEST(io_read_requires_port) {
  CHECK_INVALID("SELECT * FROM io_8_read",
                "'io_8_read' reads require a port constraint");
  CHECK_VALID("SELECT value FROM io_8_read WHERE port = 96");
  CHECK_VALID("SELECT value FROM io_8_read WHERE port = ?"
              /* parameters are range-checked at run time */);
}

TEST(io_port_range) {
  CHECK_INVALID("SELECT value FROM io_8_read WHERE port = 99999",
                "port 99999 is out of range 0..65535");
  CHECK_INVALID("SELECT value FROM io_16_read WHERE port IN (96, 70000)",
                "port 70000 is out of range 0..65535");
}

TEST(io_write_valid_and_ranges) {
  CHECK_VALID("INSERT INTO io_8_write (port, value) VALUES (96, 31)");
  CHECK_VALID("INSERT INTO io_8_write VALUES (96, 255)");
  CHECK_VALID("INSERT INTO io_16_write (port, value) VALUES (96, 300)");
  CHECK_INVALID("INSERT INTO io_8_write (port, value) VALUES (96, 300)",
                "value 300 is out of range 0..255 for io_8_write");
  CHECK_INVALID("INSERT INTO io_32_write (port, value) VALUES (96, 4294967296)",
                "value 4294967296 is out of range 0..4294967295 for io_32_write");
}

TEST(io_write_needs_both_columns) {
  CHECK_INVALID("INSERT INTO io_8_write (port) VALUES (96)",
                "'io_8_write' INSERT must specify both port and value");
}

TEST(io_read_is_select_only) {
  CHECK_INVALID("INSERT INTO io_8_read (port, value) VALUES (1, 2)",
                "'io_8_read' accepts SELECT only");
  CHECK_INVALID("UPDATE io_8_read SET value = 1 WHERE port = 1",
                "'io_8_read' accepts SELECT only");
  CHECK_INVALID("DELETE FROM io_8_read WHERE port = 1",
                "'io_8_read' accepts SELECT only");
}

TEST(io_write_is_insert_only) {
  CHECK_INVALID("SELECT * FROM io_8_write", "'io_8_write' accepts INSERT only");
  CHECK_INVALID("UPDATE io_8_write SET value = 1 WHERE port = 1",
                "'io_8_write' accepts INSERT only");
  CHECK_INVALID("DELETE FROM io_8_write WHERE port = 1",
                "'io_8_write' accepts INSERT only");
}

TEST(boot_tables_are_select_only) {
  CHECK_VALID("SELECT * FROM boot_info");
  CHECK_VALID("SELECT base, type FROM memory_map WHERE type = 'usable'");
  CHECK_INVALID("INSERT INTO boot_info (bootloader) VALUES ('x')",
                "'boot_info' accepts SELECT only");
  CHECK_INVALID(
      "INSERT INTO memory_map (base, length, type) VALUES (0, 1, 'usable')",
      "'memory_map' accepts SELECT only");
  CHECK_INVALID("UPDATE memory_map SET base = 0 WHERE base = 0",
                "'memory_map' accepts SELECT only");
  CHECK_INVALID("UPDATE boot_info SET cmdline = 'x'",
                "'boot_info' accepts SELECT only");
  CHECK_INVALID("DELETE FROM boot_info", "'boot_info' accepts SELECT only");
  CHECK_INVALID("DELETE FROM memory_map WHERE length = 0",
                "'memory_map' accepts SELECT only");
}

TEST(memory_reads_and_writes) {
  CHECK_VALID("SELECT value FROM memory WHERE address BETWEEN 0 AND 1023");
  CHECK_VALID("INSERT INTO memory (address, value) VALUES (4096, 65)");
  CHECK_VALID("UPDATE memory SET value = 7 WHERE address = 4096");
  CHECK_VALID("SELECT value FROM volatile_memory WHERE address = 100");
  CHECK_INVALID("SELECT value FROM memory",
                "'memory' reads require an address constraint");
  CHECK_INVALID("INSERT INTO memory (address, value) VALUES (-1, 65)",
                "address must be >= 0 (got -1)");
  CHECK_INVALID("INSERT INTO memory (address, value) VALUES (4096, 300)",
                "value 300 is out of range 0..255 for byte-addressable memory");
  CHECK_INVALID("UPDATE memory SET value = 5",
                "'memory' UPDATE requires an address constraint");
  CHECK_INVALID("UPDATE memory SET address = 1 WHERE address = 0",
                "only 'value' may be updated");
  CHECK_INVALID("DELETE FROM memory WHERE address = 5",
                "'memory' does not support DELETE");
  CHECK_INVALID("SELECT value FROM memory WHERE address = -5",
                "address must be >= 0 (got -5)");
}

TEST(memory_needs_both_columns) {
  CHECK_INVALID("INSERT INTO memory (address) VALUES (16)",
                "'memory' INSERT must specify both address and value");
}

// phys is memory's contract with qword rules: addresses must be 8-aligned
// and any i64 bit pattern is a legal value. virt_memory is byte storage
// behind the page tables — identical contracts to memory. cr3_write takes
// INSERTs only, one aligned value.
TEST(vm_table_contracts) {
  CHECK_VALID("SELECT value FROM phys WHERE address = 1048576");
  CHECK_VALID("INSERT INTO phys (address, value) VALUES (1048576, 1048579)");
  CHECK_VALID("UPDATE phys SET value = 70001 WHERE address = 1048576");
  CHECK_INVALID("SELECT value FROM phys",
                "'phys' reads require an address constraint");
  CHECK_INVALID("INSERT INTO phys (address, value) VALUES (4, 1)",
                "address 4 must be a multiple of 8 for phys (qword access)");
  CHECK_INVALID("SELECT value FROM phys WHERE address = 4",
                "address 4 must be a multiple of 8 for phys (qword access)");
  CHECK_INVALID("INSERT INTO phys (address, value) VALUES (-8, 1)",
                "address must be >= 0 (got -8)");
  CHECK_INVALID("INSERT INTO phys (value) VALUES (1)",
                "'phys' INSERT must specify both address and value");
  CHECK_INVALID("UPDATE phys SET value = 1",
                "'phys' UPDATE requires an address constraint");
  CHECK_INVALID("UPDATE phys SET address = 1 WHERE address = 0",
                "only 'value' may be updated");
  CHECK_INVALID("DELETE FROM phys WHERE address = 8",
                "'phys' does not support DELETE");

  CHECK_VALID("SELECT value FROM virt_memory WHERE address = 4096");
  CHECK_VALID("INSERT INTO virt_memory (address, value) VALUES (4096, 65)");
  CHECK_VALID("UPDATE virt_memory SET value = 7 WHERE address = 4096");
  CHECK_INVALID("SELECT value FROM virt_memory",
                "'virt_memory' reads require an address constraint");
  CHECK_INVALID("INSERT INTO virt_memory (address, value) VALUES (4096, 300)",
                "value 300 is out of range 0..255 for byte-addressable memory");

  CHECK_VALID("INSERT INTO cr3_write (value) VALUES (4096)");
  CHECK_VALID("INSERT INTO cr3_write VALUES (8192)");
  CHECK_INVALID("SELECT value FROM cr3_write",
                "'cr3_write' accepts INSERT only");
  CHECK_INVALID("UPDATE cr3_write SET value = 0 WHERE value = 1",
                "'cr3_write' accepts INSERT only");
  CHECK_INVALID("DELETE FROM cr3_write", "'cr3_write' accepts INSERT only");
  CHECK_INVALID("INSERT INTO cr3_write (value) VALUES (100)",
                "cr3 value 100 must be >= 0 and a multiple of 4096");
  CHECK_INVALID("INSERT INTO cr3_write (value) VALUES (-4096)",
                "cr3 value -4096 must be >= 0 and a multiple of 4096");
}

TEST(system_ddl_protected) {
  CHECK_INVALID("DROP TABLE memory", "system table 'memory' cannot be dropped");
  CHECK_INVALID("DROP TABLE volatile_memory, io_8_read",
                "system table 'volatile_memory' cannot be dropped");
  CHECK_INVALID("ALTER TABLE io_8_read RENAME TO ports",
                "system table 'io_8_read' cannot be altered");
  CHECK_INVALID("ALTER TABLE memory ADD COLUMN x INT",
                "system table 'memory' cannot be altered");
  CHECK_INVALID("CREATE TABLE memory (x INT)", "table 'memory' already exists");
  CHECK_INVALID("CREATE INDEX i ON memory (address)",
                "cannot index system table 'memory'");
}

TEST(returning_not_on_system) {
  CHECK_INVALID("INSERT INTO io_8_write (port, value) VALUES (96, 0) "
                "RETURNING value",
                "RETURNING is not supported on system tables");
  CHECK_VALID(std::string(kSchema) +
              "INSERT INTO users (id, name) VALUES (9, 'x') RETURNING id");
}

// ---------------------------------------------------------------------------
// DDL / views / indexes
// ---------------------------------------------------------------------------

TEST(create_table_checks) {
  CHECK_INVALID(std::string(kSchema) + "CREATE TABLE users (id INT)",
                "table 'users' already exists");
  CHECK_VALID(std::string(kSchema) + "CREATE TABLE IF NOT EXISTS users (id INT)");
  CHECK_INVALID("CREATE TABLE t (a INT, a TEXT)", "duplicate column 'a'");
  CHECK_INVALID("CREATE TABLE t (a FOOBAR)", "unknown column type 'FOOBAR'");
}

TEST(ddl_evolution) {
  const sqlos::ValidationResult result =
      check("CREATE TABLE t (a INT);"
            "INSERT INTO t (a) VALUES (1);"
            "DROP TABLE t;"
            "SELECT * FROM t");
  CHECK(!result.ok());
  CHECK(has_issue(result, "unknown table 't'"));
  CHECK(result.catalog.find("t") == nullptr);
}

TEST(drop_missing) {
  CHECK_INVALID("DROP TABLE ghost", "table 'ghost' does not exist");
  CHECK_VALID("DROP TABLE IF EXISTS ghost");
  CHECK_INVALID(std::string(kSchema) + "DROP INDEX users",
                "'users' is not an index");
  CHECK_INVALID("DROP INDEX ghost", "index 'ghost' does not exist");
}

TEST(view_checks) {
  CHECK_INVALID(std::string(kSchema) + "CREATE VIEW v AS SELECT bogus FROM users",
                "unknown column 'bogus'");
  CHECK_INVALID(std::string(kSchema) + "CREATE VIEW v (a, b) AS SELECT id FROM users",
                "view 'v' declares 2 column(s) but its query returns 1");
  CHECK_INVALID(std::string(kSchema) + "CREATE TABLE users (id INT)",
                "table 'users' already exists");
  const sqlos::ValidationResult result =
      check(std::string(kSchema) +
            "CREATE VIEW v AS SELECT id, name FROM users;"
            "INSERT INTO v (id) VALUES (1)");
  CHECK(has_issue(result, "cannot modify view 'v'"));
}

TEST(view_replace_and_drop_kind) {
  const sqlos::ValidationResult result =
      check("CREATE TABLE t (a INT);"
            "CREATE VIEW v AS SELECT a FROM t;"
            "CREATE OR REPLACE VIEW v AS SELECT a FROM t;"
            "DROP TABLE v");
  CHECK(has_issue(result, "'v' is a view, not a table"));
}

TEST(index_checks) {
  CHECK_VALID(std::string(kSchema) +
              "CREATE INDEX idx_users_name ON users (name);"
              "DROP INDEX idx_users_name");
  CHECK_INVALID(std::string(kSchema) +
                    "CREATE INDEX i ON users (name);"
                    "CREATE INDEX i ON users (name)",
                "index 'i' already exists");
  CHECK_INVALID(std::string(kSchema) + "CREATE INDEX i2 ON users (bogus)",
                "unknown column 'bogus'");
  CHECK_INVALID(std::string(kSchema) + "CREATE INDEX i3 ON users (id + 1)",
                "index column must be a simple column name");
  CHECK_INVALID(std::string(kSchema) + "CREATE INDEX i4 ON ghost (id)",
                "unknown table 'ghost'");
}

TEST(alter_checks) {
  CHECK_INVALID(std::string(kSchema) + "ALTER TABLE ghost ADD COLUMN x INT",
                "table 'ghost' does not exist");
  CHECK_VALID(std::string(kSchema) + "ALTER TABLE IF EXISTS ghost ADD COLUMN x INT");
  CHECK_INVALID(std::string(kSchema) + "ALTER TABLE users ADD COLUMN id INT",
                "column 'id' already exists");
  CHECK_INVALID(std::string(kSchema) + "ALTER TABLE users DROP COLUMN bogus",
                "column 'bogus' does not exist in table 'users'");
  CHECK_VALID(std::string(kSchema) +
              "ALTER TABLE users DROP COLUMN age;"
              "ALTER TABLE users ADD COLUMN score REAL;"
              "ALTER TABLE users RENAME COLUMN name TO nickname;"
              "ALTER TABLE users RENAME TO people;"
              "SELECT nickname FROM people");
  CHECK_INVALID(std::string(kSchema) + "ALTER TABLE users RENAME TO orders",
                "table 'orders' already exists");
}

// ---------------------------------------------------------------------------
// CTEs and recursion
// ---------------------------------------------------------------------------

TEST(cte_valid) {
  CHECK_VALID(std::string(kSchema) +
              "WITH x AS (SELECT id FROM users) SELECT * FROM x");
  CHECK_INVALID(std::string(kSchema) + "SELECT * FROM x", "unknown table 'x'");
  CHECK_INVALID(std::string(kSchema) +
                    "WITH x (a, b) AS (SELECT id FROM users) SELECT * FROM x",
                "CTE 'x' declares 2 column(s) but its query returns 1");
}

TEST(cte_modify_forbidden) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH x AS (SELECT id FROM users) "
                    "INSERT INTO x (id) VALUES (1)",
                "cannot modify CTE 'x'");
}

TEST(recursive_valid) {
  CHECK_VALID(std::string(kSchema) +
              "WITH RECURSIVE walk AS ("
              "  SELECT id, parent, depth FROM nodes WHERE parent IS NULL"
              "  UNION ALL"
              "  SELECT n.id, n.parent, n.depth + 1"
              "  FROM nodes n JOIN walk w ON n.parent = w.id"
              ") SELECT * FROM walk");
}

TEST(recursive_requires_flag) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH walk(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM walk) "
                    "SELECT * FROM walk",
                "WITH RECURSIVE is required for recursive reference to 'walk'");
}

TEST(recursive_flag_unused) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS (SELECT 1) SELECT * FROM walk",
                "WITH RECURSIVE specified but no CTE references itself");
}

TEST(recursive_must_be_union_all) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS (SELECT 1 "
                    "UNION SELECT n + 1 FROM walk) SELECT * FROM walk",
                "must be (anchor) UNION ALL (recursive term)");
}

TEST(recursive_anchor_must_not_reference) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS (SELECT n FROM walk "
                    "UNION ALL SELECT 1) SELECT * FROM walk",
                "must not appear in the anchor term");
}

TEST(recursive_single_reference) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS (SELECT 1 "
                    "UNION ALL SELECT n + 1 FROM walk, walk) SELECT * FROM walk",
                "exactly once (found 2)");
}

TEST(recursive_no_outer_join) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS ("
                    "  SELECT id FROM nodes WHERE parent IS NULL"
                    "  UNION ALL"
                    "  SELECT walk.n + 1 FROM nodes "
                    "  LEFT JOIN walk ON nodes.parent = walk.n"
                    ") SELECT * FROM walk",
                "must not appear within an outer join");
}

TEST(recursive_no_subquery) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS ("
                    "  SELECT 1"
                    "  UNION ALL"
                    "  SELECT 1 FROM nodes WHERE id IN (SELECT n FROM walk)"
                    ") SELECT * FROM walk",
                "must not appear within a subquery");
}

TEST(recursive_anchor_width) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(a, b) AS (SELECT 1 "
                    "UNION ALL SELECT 1, 2 FROM walk) SELECT * FROM walk",
                "CTE 'walk' declares 2 column(s) but its query returns 1");
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk AS (SELECT 1 "
                    "UNION ALL SELECT 1, 2 FROM walk) SELECT * FROM walk",
                "recursive term of 'walk' returns 2 column(s) but the anchor returns 1");
}

TEST(recursive_term_types) {
  CHECK_INVALID(std::string(kSchema) +
                    "WITH RECURSIVE walk(n) AS (SELECT 1 "
                    "UNION ALL SELECT 'x' FROM walk) SELECT * FROM walk",
                "recursive term of 'walk' column 1: type mismatch (TEXT vs INT)");
}

// ---------------------------------------------------------------------------
// Upsert / RETURNING / statements glue
// ---------------------------------------------------------------------------

TEST(upsert_checks) {
  CHECK_VALID(std::string(kSchema) +
              "INSERT INTO orders (id, user_id, total) VALUES (1, 2, 3.5) "
              "ON CONFLICT (id) DO UPDATE SET total = excluded.total");
  CHECK_INVALID(std::string(kSchema) +
                    "INSERT INTO orders (id) VALUES (1) "
                    "ON CONFLICT (bogus) DO NOTHING",
                "unknown column 'bogus'");
  CHECK_INVALID(std::string(kSchema) +
                    "INSERT INTO orders (id, total) VALUES (1, 1.0) "
                    "ON CONFLICT (id) DO UPDATE SET user_id = excluded.total",
                "type mismatch: cannot store REAL in INT column 'user_id'");
}

TEST(on_conflict_not_on_system) {
  CHECK_INVALID(
      "INSERT INTO memory (address, value) VALUES (1, 1) "
      "ON CONFLICT (address) DO NOTHING",
      "ON CONFLICT is not supported on system tables");
}

// ---------------------------------------------------------------------------
// Positions and parse errors
// ---------------------------------------------------------------------------

TEST(issue_positions) {
  const sqlos::ValidationResult result =
      check("SELECT 1;\nSELECT * FROM nope;");
  CHECK_EQ(result.issues.size(), 1u);
  if (!result.issues.empty()) {
    CHECK_EQ(result.issues[0].pos.line, 2u);
    CHECK_EQ(result.issues[0].pos.column, 1u);
    CHECK_EQ(sqlos::format_issue(result.issues[0]),
             std::string("line 2:1: unknown table 'nope'"));
  }
}

TEST(parse_error_becomes_issue) {
  const sqlos::ValidationResult result = check("SELECT * FORM users");
  CHECK(!result.ok());
  CHECK_EQ(result.issues.size(), 1u);
}

// ---------------------------------------------------------------------------
// The whole shape of an "OS boot" program must validate.
// ---------------------------------------------------------------------------

TEST(full_os_program) {
  CHECK_VALID(
      "BEGIN;"
      "CREATE TABLE nodes (id INT, parent INT, depth INT, payload TEXT);"
      "INSERT INTO nodes (id, parent, depth, payload) "
      "VALUES (1, NULL, 0, 'root'), (2, 1, 1, 'child');"
      "CREATE VIEW roots AS SELECT id FROM nodes WHERE parent IS NULL;"
      "CREATE INDEX idx_nodes_parent ON nodes (parent);"
      "WITH RECURSIVE walk AS ("
      "  SELECT id, parent, depth FROM nodes WHERE parent IS NULL"
      "  UNION ALL"
      "  SELECT n.id, n.parent, n.depth + 1 "
      "  FROM nodes n JOIN walk w ON n.parent = w.id"
      ") SELECT id FROM walk ORDER BY depth;"
      "SELECT value FROM io_8_read WHERE port = 96;"
      "INSERT INTO io_8_write (port, value) VALUES (96, 31);"
      "INSERT INTO io_16_write (port, value) VALUES (40, 1000);"
      "SELECT address, value FROM memory WHERE address BETWEEN 0 AND 255;"
      "INSERT INTO memory (address, value) VALUES (4096, 65);"
      "UPDATE memory SET value = 7 WHERE address = 4096;"
      "UPDATE volatile_memory SET value = 1 WHERE address = 0;"
      "SELECT value FROM volatile_memory WHERE address IN (100, 200);"
      "COMMIT;");
}
