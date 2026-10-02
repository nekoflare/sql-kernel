// Codegen tests: the refusal messages are the compiler's contract, every
// emitted unit must compile freestanding with -Werror, and hosted binaries
// must produce the expected rows — the SQL runs as compiled code, never
// through an interpreter.
//
// Execution tests shell out to g++ (the project's stated toolchain) and run
// the result; generated sources, objects and binaries live in $TMPDIR.

#include "test_harness.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sqlos/codegen.hpp"
#include "sqlos/validate.hpp"

#ifndef SQLOS_TEST_ROOT
#define SQLOS_TEST_ROOT "."
#endif

namespace {

// --- pipeline ---------------------------------------------------------------

struct EmitOutcome {
  bool validated = false;
  bool generated = false;
  std::string validation_errors;  // non-empty when the validator rejected
  std::string message;            // first codegen refusal
  sqlos::SourcePos pos;
  std::string code;
};

EmitOutcome emit_program(const std::string& src) {
  EmitOutcome out;
  std::vector<sql::Statement> statements;
  try {
    statements = sql::Parser(src).parse_all();
  } catch (const sql::ParseError& e) {
    out.validation_errors = std::string("parse error: ") + e.what();
    return out;
  }
  const std::vector<sqlos::SourcePos> positions =
      sqlos::statement_positions(src);
  const sqlos::ValidationResult v = sqlos::validate(statements, positions);
  if (!v.ok()) {
    for (const sqlos::Issue& issue : v.issues) {
      if (!out.validation_errors.empty()) out.validation_errors += "; ";
      out.validation_errors += sqlos::format_issue(issue);
    }
    return out;
  }
  out.validated = true;
  const sqlos::CodegenResult c = sqlos::generate(statements, positions);
  if (!c.ok()) {
    out.message = c.errors.empty() ? "<no error>" : c.errors[0].message;
    out.pos = c.errors.empty() ? sqlos::SourcePos{} : c.errors[0].pos;
    return out;
  }
  out.generated = true;
  out.code = c.code;
  return out;
}

// --- toolchain --------------------------------------------------------------

std::string temp_dir() {
  static const std::string dir = [] {
    const char* tmp = std::getenv("TMPDIR");
    const std::string base = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
    const std::string d = base + "/sqlos_codegen_tests";
    (void)::mkdir(d.c_str(), 0755);
    return d;
  }();
  return dir;
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << content;
  return static_cast<bool>(f);
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Runs `cmd`, capturing all output into the returned log; true when the
// command exits 0.
bool run_cmd(const std::string& cmd, const std::string& log_path) {
  const std::string full =
      "{ " + cmd + "; } > " + shell_quote(log_path) + " 2>&1";
  const int status = std::system(full.c_str());
  return status == 0;
}

std::string include_dir() { return std::string(SQLOS_TEST_ROOT) + "/include"; }
std::string driver_path() {
  return std::string(SQLOS_TEST_ROOT) + "/tests/codegen_driver.cpp";
}

// --- shared failure reporting ----------------------------------------------

void report_validation_failure(const char* file, int line, const std::string& src,
                               const EmitOutcome& o) {
  ::testing::report_failure(
      file, line, std::string("expected valid input, got: ") +
                      o.validation_errors + "\n  in: " + src);
}

void report_refusal_failure(const char* file, int line, const std::string& src) {
  ::testing::report_failure(
      file, line,
      std::string("expected refusal, but codegen succeeded\n  in: ") + src);
}

std::string compile_log(const std::string& name, const std::string& cmd) {
  const std::string log = temp_dir() + "/" + name + ".build.log";
  if (run_cmd(cmd, log)) return std::string();
  const std::string text = read_file(log);
  return text.empty() ? std::string("<no compiler output>") : text;
}

// Freestanding object build of a generated unit. Returns the compiler log
// ("" on success): generated code must be warning-free under -Werror.
std::string build_freestanding(const std::string& gen_path,
                               const std::string& name) {
  const std::string cmd =
      "g++ -std=c++17 -ffreestanding -Wall -Wextra -Werror -I " +
      shell_quote(include_dir()) + " -c " + shell_quote(gen_path) + " -o " +
      shell_quote(temp_dir() + "/" + name + ".o");
  return compile_log(name, cmd);
}

// Hosted build: generated unit + test driver, runnable on this machine.
// Returns the compiler log ("" on success).
std::string build_hosted(const std::string& gen_path,
                         const std::string& name) {
  const std::string cmd =
      "g++ -std=c++17 -DSQLOS_HOSTED -Wall -Wextra -Werror -I " +
      shell_quote(include_dir()) + " " + shell_quote(gen_path) + " " +
      shell_quote(driver_path()) + " -o " +
      shell_quote(temp_dir() + "/" + name + ".prog");
  return compile_log(name, cmd);
}

// Runs a hosted binary; returns its stdout with trailing newlines removed.
// `in_value` seeds the hosted port-read hook (SQLOS_IN_VALUE); with
// `show_headers` the driver installs the column-header callback too.
std::string run_program(const std::string& name,
                        const std::string& in_value = "",
                        bool show_headers = false) {
  const std::string prog = temp_dir() + "/" + name + ".prog";
  const std::string out = temp_dir() + "/" + name + ".out";
  std::string cmd;
  if (show_headers) {
    cmd += "SQLOS_SHOW_HEADERS=1 ";
  }
  if (!in_value.empty()) {
    cmd += "SQLOS_IN_VALUE=" + shell_quote(in_value) + " ";
  }
  cmd += shell_quote(prog);
  (void)run_cmd(cmd + " > " + shell_quote(out), temp_dir() + "/" + name + ".run.log");
  std::string text = read_file(out);
  while (!text.empty() && text.back() == '\n') text.pop_back();
  return text;
}

const char* kSchema =
    "CREATE TABLE users (id INT, name TEXT, age INT, active BOOL);"
    "CREATE TABLE orders (id INT, user_id INT, total REAL);"
    "CREATE TABLE nodes (id INT, parent INT, depth INT);";

}  // namespace

// ---------------------------------------------------------------------------
// Macros: refusal contract, freestanding compilation, hosted execution.
// ---------------------------------------------------------------------------

// Input must validate (a rejection is a parity bug) and be refused with a
// message containing `needle`.
#define CHECK_REFUSED(src, needle)                                             \
  do {                                                                         \
    ++::testing::check_count();                                                \
    const EmitOutcome o_ = emit_program(src);                                  \
    if (!o_.validated) {                                                       \
      report_validation_failure(__FILE__, __LINE__, (src), o_);                \
    } else if (o_.generated) {                                                 \
      report_refusal_failure(__FILE__, __LINE__, (src));                       \
    } else if (o_.message.find(needle) == std::string::npos) {                 \
      ::testing::report_failure(                                               \
          __FILE__, __LINE__,                                                  \
          std::string("expected refusal containing \"") + (needle) +           \
              "\", got \"" + o_.message + "\"\n  in: " + (src));               \
    }                                                                          \
  } while (0)

// Input must generate and compile as a freestanding unit without warnings.
#define CHECK_COMPILES(src, name)                                              \
  do {                                                                         \
    ++::testing::check_count();                                                \
    const EmitOutcome o_ = emit_program(src);                                  \
    if (!o_.validated) {                                                       \
      report_validation_failure(__FILE__, __LINE__, (src), o_);                \
    } else if (!o_.generated) {                                                \
      ::testing::report_failure(__FILE__, __LINE__,                           \
                                std::string("unexpected refusal: ") +          \
                                    o_.message + "\n  in: " + (src));          \
    } else {                                                                   \
      const std::string gen_ = temp_dir() + "/" + (name) + ".gen.cpp";         \
      if (!write_file(gen_, o_.code)) {                                        \
        ::testing::report_failure(__FILE__, __LINE__,                          \
                                  "cannot write " + gen_);                     \
      } else {                                                                 \
        const std::string log_ = build_freestanding(gen_, name);               \
        if (!log_.empty()) {                                                   \
          ::testing::report_failure(__FILE__, __LINE__,                        \
                                    std::string("freestanding compile failed "  \
                                                "for " #name ":\n") +          \
                                        log_);                                 \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

// Full path: generate, build hosted, run, compare stdout (trailing newlines
// stripped on both sides). `in_value` may be "" to leave the port-read hook
// at its default. `show_headers` makes the driver install the column-header
// callback, so each result set announces itself as "H col|col".
void check_runs(const char* file, int line, const std::string& src,
                const std::string& name, const std::string& expected,
                const std::string& in_value, bool show_headers) {
  const EmitOutcome o_ = emit_program(src);
  if (!o_.validated) {
    report_validation_failure(file, line, src, o_);
    return;
  }
  if (!o_.generated) {
    ::testing::report_failure(file, line,
                              std::string("unexpected refusal: ") + o_.message +
                                  "\n  in: " + src);
    return;
  }
  const std::string gen_ = temp_dir() + "/" + name + ".gen.cpp";
  if (!write_file(gen_, o_.code)) {
    ::testing::report_failure(file, line, "cannot write " + gen_);
    return;
  }
  const std::string log_ = build_hosted(gen_, name);
  if (!log_.empty()) {
    ::testing::report_failure(file, line,
                              "hosted compile failed for " + name + ":\n" + log_);
    return;
  }
  const std::string got_ = run_program(name, in_value, show_headers);
  std::string want_ = expected;
  while (!want_.empty() && want_.back() == '\n') want_.pop_back();
  if (got_ != want_) {
    ::testing::report_failure(
        file, line,
        "program " + name + " output mismatch:\n  expected \"" + want_ +
            "\"\n  got: \"" + got_ + "\"");
  }
}

#define CHECK_RUNS(src, name, expected, in_value)                              \
  do {                                                                         \
    ++::testing::check_count();                                                \
    check_runs(__FILE__, __LINE__, (src), (name), (expected), (in_value),      \
               false);                                                         \
  } while (0)

// ---------------------------------------------------------------------------
// Generation structure
// ---------------------------------------------------------------------------

TEST(generate_structure) {
  const EmitOutcome o =
      emit_program(std::string(kSchema) + "SELECT id FROM users WHERE id = 1;");
  CHECK(o.validated);
  CHECK(o.generated);
  if (o.generated) {
    CHECK(o.code.find("#include \"sqlos/runtime/sqlos_runtime.hpp\"") !=
          std::string::npos);
    CHECK(o.code.find("extern \"C\" void sqlos_program") != std::string::npos);
    CHECK(o.code.find("sqlos::v_eq") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Refusal contract
// ---------------------------------------------------------------------------

TEST(refusals_select) {
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT * FROM users u JOIN orders o ON u.id = o.user_id;",
                "not yet compiled: JOIN");
  CHECK_REFUSED(std::string(kSchema) + "SELECT * FROM users, orders;",
                "not yet compiled: multiple FROM items");
  CHECK_REFUSED(std::string(kSchema) + "SELECT count(id) FROM users;",
                "not yet compiled: aggregate function 'count'");
  CHECK_REFUSED(std::string(kSchema) + "SELECT sum(age) FROM users;",
                "not yet compiled: aggregate function 'sum'");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT age, count(*) FROM users GROUP BY age;",
                "not yet compiled: GROUP BY");
  CHECK_REFUSED(std::string(kSchema) + "SELECT age FROM users HAVING age > 1;",
                "not yet compiled: HAVING");
  CHECK_REFUSED(std::string(kSchema) + "SELECT DISTINCT age FROM users;",
                "not yet compiled: DISTINCT");
  CHECK_REFUSED(std::string(kSchema) + "SELECT id FROM users ORDER BY id;",
                "not yet compiled: ORDER BY");
  CHECK_REFUSED(std::string(kSchema) + "SELECT id FROM users LIMIT 3;",
                "not yet compiled: LIMIT");
  CHECK_REFUSED(std::string(kSchema) + "SELECT id FROM users OFFSET 3;",
                "not yet compiled: OFFSET");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT id FROM users UNION SELECT id FROM orders;",
                "not yet compiled: set operation UNION");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT count(id) OVER (ORDER BY id) FROM users;",
                "not yet compiled: window function");
}

TEST(refusals_expression) {
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT (SELECT id FROM users) FROM users;",
                "not yet compiled: subquery in expression");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT id FROM users WHERE EXISTS (SELECT 1 FROM orders);",
                "not yet compiled: EXISTS");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT id FROM users WHERE id IN (SELECT id FROM orders);",
                "not yet compiled: IN (subquery)");
  CHECK_REFUSED(std::string(kSchema) + "SELECT id FROM users WHERE id = ?;",
                "not yet compiled: bind parameter");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT id FROM users WHERE name GLOB 'a*';",
                "not yet compiled: GLOB");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT id FROM users WHERE name COLLATE NOCASE = 'x';",
                "not yet compiled: COLLATE");
  CHECK_REFUSED(std::string(kSchema) + "INSERT INTO users (id) VALUES (DEFAULT);",
                "not yet compiled: DEFAULT");
}

TEST(refusals_dml) {
  CHECK_REFUSED(
      std::string(kSchema) + "INSERT INTO users (id) VALUES (1) RETURNING id;",
      "not yet compiled: RETURNING");
  CHECK_REFUSED(std::string(kSchema) +
                    "DELETE FROM users WHERE id = 1 RETURNING id;",
                "not yet compiled: RETURNING");
  CHECK_REFUSED(std::string(kSchema) +
                    "INSERT INTO users (id) VALUES (1) "
                    "ON CONFLICT (id) DO NOTHING;",
                "not yet compiled: ON CONFLICT");
  CHECK_REFUSED(std::string(kSchema) + "INSERT OR IGNORE INTO users (id) VALUES (1);",
                "not yet compiled: INSERT OR IGNORE");
  CHECK_REFUSED(std::string(kSchema) + "UPDATE users SET age = 1 FROM orders;",
                "not yet compiled: UPDATE ... FROM");
  CHECK_REFUSED(std::string(kSchema) +
                    "WITH c AS (SELECT 1) INSERT INTO users (id) SELECT 1;",
                "not yet compiled: WITH clause on INSERT");
  CHECK_REFUSED(std::string(kSchema) + "INSERT INTO memory DEFAULT VALUES;",
                "not yet compiled: DEFAULT VALUES on system tables");
}

TEST(refusals_ddl) {
  CHECK_REFUSED(std::string(kSchema) +
                    "CREATE TABLE tt AS SELECT age, count(*) FROM users "
                    "GROUP BY age;",
                "not yet compiled: GROUP BY");
  CHECK_REFUSED(std::string(kSchema) +
                    "CREATE TABLE tt AS WITH d AS (SELECT 1) SELECT 1;",
                "not yet compiled: nested WITH clause");
  CHECK_REFUSED(
      std::string(kSchema) + "CREATE VIEW v AS SELECT id FROM users;"
                             "SELECT * FROM v;",
      "not yet compiled: SELECT from view 'v'");
}

TEST(refusals_cte_hardware) {
  CHECK_REFUSED(std::string(kSchema) +
                    "WITH c AS (SELECT id FROM users) SELECT * FROM c;",
                "not yet compiled: non-recursive CTE 'c'");
  CHECK_REFUSED(
      std::string(kSchema) +
          "WITH RECURSIVE c1(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c1 "
          "WHERE n < 3), c2(m) AS (SELECT 1) SELECT * FROM c1;",
      "not yet compiled: multiple CTEs");
  CHECK_REFUSED(
      std::string(kSchema) +
          "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL "
          "SELECT o.id FROM c JOIN orders o ON c.n = o.id) SELECT * FROM c;",
      "not yet compiled: JOIN or subquery in recursive term");
  CHECK_REFUSED(std::string(kSchema) +
                    "SELECT y FROM (SELECT id AS y FROM users);",
                "not yet compiled: subquery in FROM");
  CHECK_REFUSED("SELECT value FROM io_8_read WHERE port = value;",
                "not yet compiled: io read tables support only WHERE port");
  CHECK_REFUSED("SELECT value FROM memory WHERE address IN (1, 2);",
                "not yet compiled: memory tables support only WHERE address");
  CHECK_REFUSED("UPDATE memory SET value = 1 WHERE address BETWEEN 0 AND 9;",
                "not yet compiled: UPDATE on memory supports only WHERE "
                "address");
  CHECK_REFUSED("BEGIN;", "not yet compiled: transaction control");
}

TEST(refusal_position) {
  // The refusal is reported at the statement that could not be compiled.
  const EmitOutcome o = emit_program(
      "SELECT 1;\nCREATE TABLE t1 (a INT);\nSELECT count(a) FROM t1;");
  CHECK(o.validated);
  CHECK(!o.generated);
  CHECK_EQ(o.message, std::string("not yet compiled: aggregate function 'count'"));
  CHECK_EQ(o.pos.line, std::size_t{3});
  CHECK_EQ(o.pos.column, std::size_t{1});
}

// ---------------------------------------------------------------------------
// Freestanding compilation (-ffreestanding -Werror)
// ---------------------------------------------------------------------------

TEST(compile_empty) { CHECK_COMPILES("", "empty"); }

TEST(compile_crud) {
  CHECK_COMPILES(
      std::string(kSchema) +
          "INSERT INTO users VALUES (1, 'ada', 36, TRUE);"
          "UPDATE users SET age = 37 WHERE id = 1;"
          "DELETE FROM users WHERE id = 9;"
          "SELECT id, name, age FROM users WHERE age >= 18;",
      "crud");
}

TEST(compile_recursion) {
  CHECK_COMPILES(
      "WITH RECURSIVE walk(n) AS ("
      "  SELECT 1 UNION ALL SELECT n + 1 FROM walk WHERE n < 100"
      ") SELECT n FROM walk WHERE n >= 98;",
      "recursion");
}

TEST(compile_hardware) {
  CHECK_COMPILES(
      "INSERT INTO io_8_write (port, value) VALUES (96, 7);"
      "INSERT INTO io_16_write (port, value) VALUES (97, 65535);"
      "INSERT INTO io_32_write (port, value) VALUES (98, 4000000000);"
      "INSERT INTO memory (address, value) VALUES (0, 1);"
      "UPDATE memory SET value = 2 WHERE address = 0;"
      "INSERT INTO volatile_memory (address, value) VALUES (10, 3);"
      "SELECT port, value FROM io_8_read WHERE port = 96;"
      "SELECT value FROM memory WHERE address BETWEEN 0 AND 100;",
      "hardware");
}

TEST(compile_ddl) {
  CHECK_COMPILES(
      "CREATE TABLE t (a INT, b TEXT);"
      "CREATE INDEX ix_t ON t (a);"
      "CREATE VIEW v AS SELECT a FROM t;"
      "CREATE TABLE s AS SELECT a FROM t;"
      "ALTER TABLE t RENAME TO t2;"
      "ALTER TABLE s ADD COLUMN c REAL;"
      "ALTER TABLE s DROP COLUMN c;"
      "ALTER TABLE s RENAME COLUMN a TO z;"
      "DROP INDEX ix_t;"
      "DROP TABLE t2;"
      "DROP VIEW v;"
      "DROP TABLE s;",
      "ddl");
}

// ---------------------------------------------------------------------------
// Hosted execution: the compiled binary's output is the specification.
// ---------------------------------------------------------------------------

TEST(run_crud) {
  CHECK_RUNS(
      std::string(kSchema) +
          "INSERT INTO users VALUES (1, 'ada', 36, TRUE);"
          "INSERT INTO users VALUES (2, 'bob', 25, FALSE);"
          "INSERT INTO users VALUES (3, 'cy', 41, TRUE);"
          "UPDATE users SET age = 26 WHERE id = 2;"
          "DELETE FROM users WHERE id = 3;"
          "SELECT id, name, age FROM users;",
      "crud", "1|ada|36\n2|bob|26", "");
}

TEST(run_where_filters) {
  CHECK_RUNS(
      std::string(kSchema) +
          "INSERT INTO users VALUES (1, 'ada', 36, TRUE);"
          "INSERT INTO users VALUES (2, 'bob', 25, FALSE);"
          "INSERT INTO users VALUES (3, 'cy', 41, TRUE);"
          "SELECT id FROM users WHERE active;"
          "SELECT id FROM users WHERE age BETWEEN 30 AND 40;"
          "SELECT id FROM users WHERE id IN (2, 3);",
      "filters", "1\n3\n1\n2\n3", "");
}

TEST(run_recursion) {
  CHECK_RUNS(
      "WITH RECURSIVE walk(n) AS ("
      "  SELECT 1 UNION ALL SELECT n + 1 FROM walk WHERE n < 5"
      ") SELECT n, n * n FROM walk;",
      "squares", "1|1\n2|4\n3|9\n4|16\n5|25", "");
}

TEST(run_recursion_values_anchor) {
  CHECK_RUNS(
      "WITH RECURSIVE c(n) AS ("
      "  VALUES (1) UNION ALL SELECT n + 1 FROM c WHERE n < 4"
      ") SELECT * FROM c;",
      "values_anchor", "1\n2\n3\n4", "");
}

TEST(run_recursion_outer_where) {
  CHECK_RUNS(
      "WITH RECURSIVE c(n) AS ("
      "  SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 10"
      ") SELECT n FROM c WHERE n >= 8;",
      "outer_where", "8\n9\n10", "");
}

TEST(run_hardware) {
  CHECK_RUNS(
      "INSERT INTO io_8_write (port, value) VALUES (96, 7);"
      "INSERT INTO memory (address, value) VALUES (0, 42);"
      "UPDATE memory SET value = 43 WHERE address = 0;"
      "SELECT address, value FROM memory WHERE address = 0;"
      "SELECT value FROM memory WHERE address BETWEEN 0 AND 1;"
      "SELECT port, value FROM io_8_read WHERE port = 96;"
      "SELECT port FROM io_8_read WHERE port = 96 AND value = 170;",
      "hardware", "0|43\n43\n0\n96|170\n96\nOUT 96=7 (x1)", "0xAA");
}

TEST(run_io_widths) {
  CHECK_RUNS(
      "INSERT INTO io_16_write (port, value) VALUES (97, 30057);"
      "INSERT INTO io_32_write (port, value) VALUES (98, 4000000000);"
      "INSERT INTO volatile_memory (address, value) VALUES (5, 9);"
      "UPDATE volatile_memory SET value = 7 WHERE address = 5 AND value = 9;"
      "SELECT value FROM volatile_memory WHERE address = 5;",
      "widths", "7\nOUT 98=4000000000 (x2)", "");
}

TEST(run_expr_arith) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT, r REAL);"
      "INSERT INTO t VALUES (10, 2.5);"
      "INSERT INTO t VALUES (-3, -1.5);"
      "SELECT a + 1, a * 2, abs(a), -a FROM t WHERE a = 10;"
      "SELECT a / 3, a % 7, r * 4 FROM t WHERE a = 10;"
      "SELECT a & 3, a | 4, a << 1 FROM t WHERE a = 10;",
      "arith", "11|20|10|-10\n3|3|10\n2|14|20", "");
}

TEST(run_expr_text) {
  CHECK_RUNS(
      "CREATE TABLE t (b TEXT);"
      "INSERT INTO t VALUES ('Hello');"
      "SELECT length(b), lower(b), upper(b), substr(b, 1, 3) FROM t;"
      "SELECT b || '!', replace(b, 'l', 'L'), trim('  pad  ') FROM t;"
      "SELECT hex(255), coalesce(NULL, 'x'), nullif('same', 'same') FROM t;",
      "text",
      "5|hello|HELLO|Hel\n"
      "Hello!|HeLLo|pad\n"
      "FF|x|NULL",
      "");
}

TEST(run_expr_case_cast) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT);"
      "INSERT INTO t VALUES (10);"
      "INSERT INTO t VALUES (-3);"
      "SELECT CASE WHEN a > 0 THEN 'pos' ELSE 'neg' END FROM t;"
      "SELECT CASE a WHEN 10 THEN 'ten' ELSE 'other' END FROM t WHERE a = 10;"
      "SELECT CAST('42' AS INT), CAST(3.9 AS INT), CAST(7 AS TEXT), "
      "CAST(1 AS BOOL) FROM t WHERE a = 10;",
      "case_cast",
      "pos\n"
      "neg\n"
      "ten\n"
      "42|3|7|true",
      "");
}

TEST(run_expr_logic) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT, b TEXT);"
      "INSERT INTO t VALUES (10, 'Hello');"
      "SELECT 'Hello' LIKE 'H%', 'x' NOT LIKE 'y', 'A' ILIKE 'a' FROM t;"
      "SELECT a IS NULL, b IS NOT NULL FROM t;"
      "SELECT a BETWEEN 8 AND 12, a NOT BETWEEN 1 AND 5 FROM t;"
      "SELECT a IN (10, 20), a NOT IN (1, 2), NOT (a > 0) FROM t;",
      "logic",
      "true|true|true\n"
      "false|true\n"
      "true|true\n"
      "true|true|false",
      "");
}

TEST(run_swap_update) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT, b INT);"
      "INSERT INTO t VALUES (1, 2);"
      "UPDATE t SET a = b, b = a;"
      "SELECT a, b FROM t;",
      "swap", "2|1", "");
}

TEST(run_alter_chain) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT, b TEXT);"
      "INSERT INTO t VALUES (1, 'x');"
      "ALTER TABLE t RENAME COLUMN a TO z;"
      "ALTER TABLE t ADD COLUMN c REAL;"
      "ALTER TABLE t DROP COLUMN b;"
      "UPDATE t SET c = 2.5;"
      "ALTER TABLE t RENAME TO t2;"
      "SELECT z, c FROM t2;",
      "alter", "1|2.5", "");
}

TEST(run_delete_variants) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT);"
      "INSERT INTO t VALUES (1), (2), (3), (4);"
      "DELETE FROM t WHERE a % 2 = 0;"
      "SELECT a FROM t;"
      "DELETE FROM t;"
      "SELECT a FROM t;",
      "deletes", "1\n3", "");
}

TEST(run_ctas_and_insert_select) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT, b TEXT);"
      "INSERT INTO t VALUES (7, 'q'), (8, 'r');"
      "CREATE TABLE s AS SELECT * FROM t;"
      "CREATE TABLE u (a INT);"
      "INSERT INTO u SELECT a * 10 FROM t;"
      "SELECT * FROM s;"
      "SELECT a FROM u;",
      "ctas", "7|q\n8|r\n70\n80", "");
}

TEST(run_insert_subset_columns) {
  CHECK_RUNS(
      "CREATE TABLE t (a INT, b TEXT, c INT);"
      "INSERT INTO t (b) VALUES ('only');"
      "SELECT a, b, c FROM t;",
      "subset", "NULL|only|NULL", "");
}

TEST(run_star_forms) {
  CHECK_RUNS(
      std::string(kSchema) +
          "INSERT INTO users VALUES (1, 'ada', 36, TRUE);"
          "INSERT INTO users VALUES (2, 'bob', 25, FALSE);"
          "SELECT id, u.* FROM users u WHERE id = 1;"
          "SELECT * FROM users WHERE id = 2;",
      "stars", "1|1|ada|36|true\n2|bob|25|false", "");
}

TEST(run_cte_alias_and_shadow) {
  CHECK_RUNS(
      std::string(kSchema) +
          "INSERT INTO nodes VALUES (1, NULL, 0);"
          "WITH RECURSIVE walk(d) AS ("
          "  SELECT depth FROM nodes"
          "  UNION ALL"
          "  SELECT d + 1 FROM walk WHERE d < 3"
          ") SELECT d, walk.* FROM walk WHERE d >= 2;",
      "cte_alias", "2|2\n3|3", "");
}

TEST(run_values_statement) {
  CHECK_RUNS("VALUES (1, 'a'), (2, 'bb');", "values", "1|a\n2|bb", "");
}

TEST(run_empty_program) { CHECK_RUNS("", "empty", "", ""); }

// Column names reach the host: each result set announces itself with an
// "H col|col" line before its rows when the header callback is installed
// (SQLOS_SHOW_HEADERS). Every run_* test above passes a null callback,
// covering the other branch of the generated `if (sink.hdr != nullptr)`.
TEST(run_column_headers) {
  ++::testing::check_count();
  check_runs(__FILE__, __LINE__,
             std::string(kSchema) +
                 "INSERT INTO users VALUES (1, 'ada', 36, TRUE);"
                 "INSERT INTO users VALUES (2, 'bob', 25, FALSE);"
                 "SELECT * FROM users;"
                 "SELECT id AS user_id, name FROM users WHERE id = 2;"
                 "SELECT 7 AS seven;",
             "headers",
             "H id|name|age|active\n"
             "1|ada|36|true\n"
             "2|bob|25|false\n"
             "H user_id|name\n"
             "2|bob\n"
             "H seven\n"
             "7",
             "", true);
}
