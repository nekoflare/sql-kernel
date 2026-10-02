bewaaareee its vibecoded.
only the sql i wrote for the kernel is not

# sql-parser

A hand-written SQL parser in C++17 covering essentially all of everyday SQL:
queries, DML, DDL and transaction control. No external dependencies — just a
C++17 compiler and CMake.

```sql
WITH recent AS (SELECT id, name FROM users WHERE created > DATE '2024-01-01')
SELECT u.name, count(o.id) AS total
FROM recent u
LEFT JOIN orders o ON o.user_id = u.id
WHERE u.id IN (SELECT user_id FROM orders) AND name LIKE 'A%'
GROUP BY u.name
HAVING count(o.id) > 5
ORDER BY total DESC
LIMIT 10;
```

The design is a lexer plus a precedence-climbing recursive-descent
parser over a typed AST. `sql::format()` renders any statement back to canonical
SQL, and the test suite asserts a **round-trip invariant**: parse -> format ->
parse yields an equal tree, and formatting is idempotent.

## Build & test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The test binary can also be run directly:

```sh
./build/parser_tests
```

## CLI

`sql_parse` reads SQL from a file or stdin and prints each statement back in
canonical form, exiting non-zero with a `line:col` message on a syntax error:

```sh
echo "INSERT INTO users (id) VALUES (1);" | ./build/sql_parse
# INSERT INTO users (id) VALUES (1);

./build/sql_parse broken.sql
# error: line 1:37: expected 2 value(s) in row, got 1
```

## What is supported

### Queries

* `SELECT` / `VALUES` / parenthesized queries as expressions or set operands.
* CTEs: `WITH [RECURSIVE] name [(cols)] AS [[NOT] MATERIALIZED] (…)`,
  chained and nested.
* Set operations: `UNION [ALL]`, `EXCEPT [ALL]`, `INTERSECT [ALL]`
  (`INTERSECT` binds tighter than `UNION`/`EXCEPT`, per the precedence table).
* Joins: `INNER`/`LEFT [OUTER]`/`RIGHT [OUTER]`/`FULL [OUTER]`/`CROSS`/
  `NATURAL`, with `ON` or `USING (…)`; subqueries and table functions in `FROM`.
* `DISTINCT` and `DISTINCT ON (…)`; implicit and `AS`-style aliases for
  select items, tables and column lists.
* `GROUP BY … HAVING`, `ORDER BY … [ASC|DESC] [NULLS FIRST|LAST]`,
  `LIMIT`/`OFFSET` including `LIMIT ALL`.
* Window functions: `OVER (PARTITION BY … ORDER BY … frame)` with
  `ROWS`/`RANGE`/`GROUPS` frames, and named windows `OVER w`.

### Expressions

* Operator precedence and associativity exactly as in PostgreSQL's lexical
  precedence table (`||`, `+ -`, `* / %`, unary `+ - ~`, `^`, comparison,
  `IS [NOT] …`, `[NOT] BETWEEN/IN/LIKE/ILIKE/GLOB`, `AND`, `OR`, `NOT`).
* `CASE` (both searched and simple), `CAST(x AS t)` and `x::t`,
  typed literals (`DATE '2024-01-01'`, `TIMESTAMP '…'`, `INTERVAL '…'`).
* Quantified comparisons `= ANY/SOME/ALL (subquery | value list)` — but only
  when a `(` follows; otherwise `any`/`some` parse as ordinary columns.
* Predicates: `IS [NOT] NULL/TRUE/FALSE/UNKNOWN`, `IS [NOT] DISTINCT FROM`,
  `EXISTS (subquery)`, scalar subqueries, row constructors `(a, b)`,
  `a[1]` subscripts, `x COLLATE name`, `DEFAULT`.
* Functions with schema-qualified names, `count(DISTINCT x)`, `*` and
  `t.*`.
* Parameters: `?`, `$1`, `:name`, `@name`.

### DML

* `INSERT INTO t [(cols)] VALUES (…), … | SELECT … | DEFAULT VALUES`,
  `INSERT OR REPLACE|IGNORE INTO …` (SQLite style), `ON CONFLICT
  [(cols)] DO NOTHING | DO UPDATE SET … [WHERE …]`, `RETURNING`.
* `UPDATE t [AS a] SET c = …, (a, b) = (…), … [FROM …] [WHERE …]
  [RETURNING …]` (PostgreSQL's assignment-list form is accepted and
  flattened to single assignments in the canonical output).
* `DELETE FROM t [AS a] [WHERE …] [RETURNING …]`.
* All three accept a leading `WITH [RECURSIVE]` clause.

### DDL

* `CREATE [TEMP] TABLE [IF NOT EXISTS]` with column types (`VARCHAR(255)`,
  `DOUBLE PRECISION`, `TIMESTAMP WITH TIME ZONE`, …) and constraints:
  `PRIMARY KEY`, `NOT NULL`, `NULL`, `UNIQUE`,
  `CHECK (…)`, `DEFAULT …`, `COLLATE …`, `REFERENCES … [ON UPDATE/DELETE
  …]`, `DEFERRABLE INITIALLY DEFERRED|IMMEDIATE`, `AUTOINCREMENT`;
  table-level `PRIMARY KEY (…)`, `UNIQUE (…)`, `FOREIGN KEY (…)
  REFERENCES …`, `CHECK (…)`, plus `CONSTRAINT name` prefixes.
* `CREATE [UNIQUE] INDEX [IF NOT EXISTS]` with per-column `ASC`/`DESC`.
* `CREATE [OR REPLACE] [TEMP] VIEW [IF NOT EXISTS] v [(cols)] AS …`.
* `DROP TABLE|INDEX|VIEW [IF EXISTS] a, b [CASCADE|RESTRICT]`.
* `ALTER TABLE [IF EXISTS]` — `ADD COLUMN …`, `DROP COLUMN [IF EXISTS] …`,
  `RENAME TO …`, `RENAME COLUMN … TO …`.

### Transactions

* `BEGIN [TRANSACTION] [name]`, `COMMIT`, `ROLLBACK`,
  `START TRANSACTION` (normalized to `BEGIN`).

### Lexical

* Keywords are case-insensitive; identifiers keep their original case.
* Identifiers may be bare, `"double quoted"` (with `""` escapes) or
  `` `backticked` ``; names needing it are re-quoted by the printer so
  `t."col with space"` and a table called `"order"` round-trip.
* Strings: `'…'` with `''` escapes, E-strings (`E'a\'b'`, plus `\n`, `\t`,
  `\\`), dollar-quoted `$$ … $$` / `$tag$ … $tag$`.
* Numbers: integers, decimals, scientific notation, leading `.5`.
* Comments: `--` to end of line and `/* nested /* block */ comments */`.
* `NULL`, `TRUE`, `FALSE`, and the reserved/non-reserved keyword split follow
  PostgreSQL: non-reserved keywords (`any`, `temp`, `value`, …) remain legal
  identifiers.

## Canonical output

`sql::format()` normalizes a few input spellings; every normalization is
round-trip stable:

| Input | Canonical |
| --- | --- |
| `START TRANSACTION` | `BEGIN` |
| `CAST(x AS t)`, `x::t`, `DATE '…'` | `CAST(… AS …)` |
| `SET (a, b) = (1, 2)` | `SET a = 1, b = 2` |
| `AS NOT MATERIALIZED (…)` in CTEs | `AS (…)` |
| implicit alias (`SELECT a b`) | `SELECT a AS b` |

The test suite checks the invariant directly (`CHECK_ROUND_TRIP`), and every
statement in `tests/corpus.sql` (59 statements covering the whole grammar) is
also verified for parse → format → parse stability:

```sh
while IFS= read -r s; do
  printf '%s' "$s" | ./build/sql_parse > /tmp/a.sql &&
  printf '%s' "$(cat /tmp/a.sql)" | ./build/sql_parse | diff -q - /tmp/a.sql \
    >/dev/null || echo "unstable: $s"
done < tests/corpus.sql
```

## Grammar sources

The grammar and operator precedence were written against:

* PostgreSQL — [Lexical Key Words and Precedence](https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-PRECEDENCE),
  [SELECT](https://www.postgresql.org/docs/current/sql-select.html),
  [SQL syntax summary](https://www.postgresql.org/docs/current/sql.html).
* SQLite — [Expressions](https://www.sqlite.org/lang_expr.html),
  [Syntax diagrams](https://www.sqlite.org/syntax.html).

## Not supported

Deliberately out of scope for now:

* `MERGE`, triggers/stored procedures, `GRANT`/revoke, `EXPLAIN`, `PRAGMA`.
* Locking clauses (`FOR UPDATE [OF …] [NOWAIT]`).
* Grouping sets (`ROLLUP`/`CUBE`/`GROUPING SETS` lex as function-call-shaped
  nodes rather than grouping-set nodes) and aggregate `FILTER (WHERE …)`.
* Bracket-quoted identifiers (`[name]`).

## Usage

```cpp
#include <iostream>
#include "sql_parser/parser.hpp"

try {
  sql::Parser parser(
      "INSERT INTO users (id, name) VALUES (1, 'Ada');");
  for (const sql::Statement& statement : parser.parse_all()) {
    std::cout << sql::format(statement) << ";\n";
  }
} catch (const sql::ParseError& error) {
  std::cerr << error.what() << "\n";  // "line 1:4: expected 'INTO', got …"
}
```

Individual statement types are members of `sql::Statement`, a
`std::variant` of `SelectStatement`, `InsertStatement`, `UpdateStatement`,
`DeleteStatement`, `CreateTableStatement`, `CreateIndexStatement`,
`CreateViewStatement`, `DropStatement`, `AlterTableStatement` and
`TransactionStatement`. Expressions are `sql::Expr` nodes tagged with
`Expr::Kind`; `ExprPtr` is `std::shared_ptr<Expr>`. Equality operators are
defined for every statement type, so parsed trees can be compared directly.

## Layout

```
include/sql_parser/
  error.hpp           ParseError (message + line/column)
  lexer.hpp           TokenKind, Token, Lexer, keyword tables
  expr.hpp            Expr (kind-tagged expression node)
  statement.hpp       statement structs + format()
  ast.hpp             umbrella header
  parser.hpp          Parser (recursive descent, 2-token lookahead)
src/
  lexer.cpp           hand-written lexer (comments, all string forms)
  parser_expr.cpp     expressions (precedence climbing)
  parser_stmt.cpp     statements
  printer.cpp         canonical formatting + identifier quoting
  ast.cpp             operator== for every statement type
  main.cpp            sql_parse CLI
tests/
  test_harness.hpp    TEST/CHECK/CHECK_EQ/CHECK_ROUND_TRIP macros
  main.cpp            test runner
  test_lexer.cpp, test_expr.cpp, test_select.cpp, test_insert.cpp,
  test_dml.cpp, test_ddl.cpp
```
