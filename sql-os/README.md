# sql-os

A SQL front end for the SQL-OS project: SQL programs over built-in hardware
tables (I/O ports, byte-addressed memory) are **validated and compiled ahead
of time to C++**. There is no interpreter, no VM, no SQL engine at runtime —
a program either compiles to machine code with `g++`, or it is rejected at
build time with a positioned error.

```text
SQL text ──parse──▶ AST ──validate──▶ checked program ──codegen──▶ out.cpp ──g++──▶ binary
              ▲                          │                             │
         sql-parser                 errors stop                   errors stop
        (sibling project)           here, exit 1                  here, exit 1
```

* **Parse** — the hand-written parser from the sibling `../sql-parser`
  project, reused as a library; covers essentially all of everyday SQL.
* **Validate** — catalog/arity checks, type checks, DDL/DML semantics, and
  the hardware contracts of the built-in tables (below). Every issue is
  reported as `error: line L:C: message`.
* **Generate** — a single self-contained C++ translation unit including one
  freestanding runtime header. Constructs the code generator does not handle
  yet are reported as `not yet compiled: ...` and **no file is written**;
  the CLI never emits a partial translation unit.

## Example

```sql
WITH RECURSIVE walk(n) AS (
  SELECT 1
  UNION ALL
  SELECT n + 1 FROM walk WHERE n < 5
)
SELECT n, n * n FROM walk;
```

```sh
./build/sqlos --emit walk.cpp walk.sql
g++ -std=c++17 -DSQLOS_HOSTED -I include walk.cpp tests/codegen_driver.cpp -o walk
./walk
# 1|1
# 2|4
# 3|9
# 4|16
# 5|25
```

The recursive CTE compiles to a static worklist plus a drain loop — the
anchor appends rows, then the term runs until the worklist stops growing:

```cpp
// emitted (trimmed)
static void s0(sqlos::Sink sink) {
  {                                        // anchor + term share the sink
    sqlos::Sink app0_;
    app0_.fn = [](void* c_, const sqlos::Row* r_) { /* append to w0_walk */ };
    app0_.ctx = nullptr;
    {                                      // anchor: SELECT 1
      sqlos::Row row_;
      row_.count = 1;
      row_.cells[0] = sqlos::Value::i(1LL);
      app0_.fn(app0_.ctx, &row_);
    }
    {                                      // term: SELECT n + 1 FROM walk ...
      sqlos::u32 head1_ = 0;
      while (head1_ < st_w0_walk.count) {  //   drain the growing worklist
        sqlos::u32 cur2_ = head1_;
        ++head1_;
        if (sqlos::truth(sqlos::v_lt(st_w0_walk.c0[cur2_], sqlos::Value::i(5LL)))) {
          /* append n + 1 */
        }
      }
    }
  }
  /* outer query scans st_w0_walk and emits rows through `sink` */
}
```

## Build & test

The build expects `sql-parser` as a sibling directory (wired up in
`CMakeLists.txt`):

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The test binary can also be run directly:

```sh
./build/sqlos_tests
# OK: 116 tests, 244 checks
```

## CLI

```sh
sqlos [--check] [--emit OUT.cpp] [FILE]
```

`--check` (the default without `--emit`) validates and reports:

```sh
echo "SELECT value FROM io_8_read WHERE port = 96;" | ./build/sqlos --check
# exit 0

echo "SELECT * FROM nope;" | ./build/sqlos --check
# error: line 1:1: unknown table 'nope'
# 1 error(s)                       (exit 1)
```

`--emit` validates first, then writes the translation unit:

```sh
./build/sqlos --emit out.cpp program.sql
# exit 0 → out.cpp is complete and self-contained

./build/sqlos --emit out.cpp bad.sql
# error: line 3:1: not yet compiled: GROUP BY
# 1 error(s)                       (exit 1; out.cpp is NOT created/updated)
```

Exit status: `0` valid, `1` parse/validation/codegen errors, `2` usage or
I/O errors.

## Built-in tables

User tables live in per-table static structs (up to `SQLOS_TABLE_CAP` rows
each); these tables are backed by hardware or by fixed memory images:

| table | columns | access | compile-time contract |
|---|---|---|---|
| `io_8_read` / `io_16_read` / `io_32_read` | `port INT, value INT` | `SELECT` only | `WHERE` must constrain `port` (e.g. `WHERE port = 96`, or with extra conjuncts); constant ports range-checked `0..65535` |
| `io_8_write` / `io_16_write` / `io_32_write` | `port INT, value INT` | `INSERT` only | constant `port` in `0..65535`, constant `value` in the width's range (`0..255` / `0..65535` / `0..4294967295`) |
| `memory` | `address INT, value INT` | `SELECT` / `INSERT` / `UPDATE` | `SELECT`/`UPDATE` need `WHERE address = <expr>` or `address BETWEEN low AND high` (no `DELETE` — use `UPDATE`); literal addresses `>= 0`, literal values `0..255` |
| `volatile_memory` | same as `memory` | same | same, separate 64 KiB image |
| `memory_map` | `base INT, length INT, type TEXT` | `SELECT` only | none — a full scan of the host-filled boot memory map (one row per region; `type` is `usable` / `reserved` / `acpi_reclaimable` / `acpi_nvs` / `bad` / `bootloader` / `kernel` / `framebuffer` / `reserved_mapped`) |
| `boot_info` | `hhdm_offset, kernel_phys_base, kernel_virt_base, bootloader, bootloader_version, cmdline, firmware, framebuffer_addr` (all `TEXT`), `framebuffer_width, framebuffer_height, framebuffer_bpp, module_count, boot_time` (`INT`) | `SELECT` only | none — one row filled by the host at boot. Address columns are `0x`-hex text because upper-half HHDM/kernel addresses don't fit SQL's signed integers; a missing response arrives as `NULL` |

Additional rules, enforced by the validator (they are errors, not
refusals): `io_*_read` is `SELECT`-only, `io_*_write` is `INSERT`-only,
`boot_info`/`memory_map` are `SELECT`-only,
`RETURNING` / `ON CONFLICT` / `INSERT OR ...` are rejected on system
tables, and `UPDATE memory ... SET address = ...` is rejected.

Range failures that survive validation as runtime values (e.g. a computed
port) are trapped by the generated program rather than silently wrapped:
port/address/byte-range violations, division by zero, `NOT NULL`
violations, failed casts and capacity overruns all end in `trap()`
(`ud2` on x86 — an immediate, defined failure, never memory corruption).

## What gets validated

The validator accepts essentially the full parser grammar and checks it
against the catalog: statement-by-statement DDL/DML semantics (column
arity, assignability, defaults, `NOT NULL`, `CHECK`, `PRIMARY KEY`,
views, indexes, `ALTER` variants), expression type checking, star
expansion, subqueries in expression/FROM/`IN`/`EXISTS`, joins, set
operations, aggregates/`GROUP BY`/`HAVING`, window expressions, `WITH`
(including `WITH RECURSIVE` structure: top must be `(anchor) UNION ALL
(term)`, the term must scan the CTE exactly once, the anchor must not
reference it), and the system-table contracts above.

## What gets compiled (codegen v1)

The code generator currently emits: expressions (arithmetic, comparison,
`AND`/`OR`/`NOT`, `IN`/`BETWEEN`/`LIKE`/`ILIKE`, `CASE`, `CAST`,
`COALESCE`/`NULLIF`, string/math/bitwise builtins), `SELECT`s over a single
table or CTE (with `WHERE`, aliases, qualified stars), `INSERT`, `UPDATE`,
`DELETE`, `CREATE TABLE` (incl. `AS SELECT`), `DROP`/`ALTER`,
`CREATE VIEW`/`CREATE INDEX` (tracked in the catalog; no runtime effect),
`VALUES`, a single recursive CTE (`WITH RECURSIVE`, anchor
`UNION ALL` term) compiled to a worklist loop, and uncorrelated scalar
subqueries `(SELECT ...)` evaluated inline — NULL when the subquery
returns no rows, `trap()` when it returns more than one (the standard SQL
error, through the project's defined-failure channel).

Everything else **validates but is refused at codegen** with a precise
message — never silently compiled wrong, never partially written. The
messages below are representative (one per line); each is an exact string
in the compiler, asserted by the tests:

```text
not yet compiled: JOIN
not yet compiled: multiple FROM items (joins)
not yet compiled: aggregate function 'count'
not yet compiled: GROUP BY
not yet compiled: HAVING
not yet compiled: DISTINCT
not yet compiled: ORDER BY
not yet compiled: LIMIT
not yet compiled: OFFSET
not yet compiled: set operation UNION
not yet compiled: window function
not yet compiled: EXISTS
not yet compiled: IN (subquery)
not yet compiled: subquery in FROM
not yet compiled: multiple CTEs
not yet compiled: non-recursive CTE 'c'
not yet compiled: SELECT from view 'v'
not yet compiled: RETURNING
not yet compiled: ON CONFLICT
not yet compiled: INSERT OR REPLACE
not yet compiled: bind parameter
not yet compiled: COLLATE
not yet compiled: GLOB
not yet compiled: DEFAULT
not yet compiled: transaction control
not yet compiled: io read tables support only WHERE port = <expr>
not yet compiled: memory tables support only WHERE address = <expr> or address BETWEEN low AND high
...
```

Refusals are reported at the offending statement's position
(`error: line L:C: not yet compiled: ...`) and the exit code is 1.

## The runtime

Generated code includes exactly one header,
`include/sqlos/runtime/sqlos_runtime.hpp` — no libraries, no exceptions,
no RTTI:

* **Freestanding by default.** `g++ -std=c++17 -ffreestanding -c out.cpp`
  must succeed; the tests enforce it with `-Wall -Wextra -Werror`.
* **Hosted test hooks** via `-DSQLOS_HOSTED`: `in*` returns a preset
  value, `out*` records the write — so hardware programs are unit-testable
  on a normal machine. Without it, port I/O uses x86 `in`/`out`
  instructions.
* **Dynamic values everywhere.** A `sqlos::Value` (24 bytes: kind +
  payload) represents `NULL`/`INT`/`REAL`/`TEXT`/`BOOL`; SQL `NULL`
  propagates through arithmetic, and `WHERE` treats `NULL` as false.
* **Fixed capacity.** `SQLOS_TABLE_CAP` 1024 rows per table/worklist,
  `SQLOS_MEM_BYTES` 65536 bytes per memory image, `SQLOS_ARENA_BYTES`
  65536 bytes of text storage, `SQLOS_MAX_COLS` 64 columns per row.
* **Results are a callback.** The program exports
  `extern "C" void sqlos_program(sqlos::SinkFn, sqlos::HeaderFn, void*)`:
  each result set first announces its column names through `header` (skip
  it with `nullptr`), then each selected row is delivered as
  `cells[0..count)` — link it against any driver you like.

## Boots on Limine (port 0xE9)

`../limine-e9/` glues this compiler to
[limine-cpp-template](https://github.com/limine-bootloader/limine-cpp-template):
Limine boots a kernel whose `kmain()` runs `sqlos_program(...)`, and the
SQL program's `INSERT INTO io_8_write (port, value) VALUES (233, ...)` rows
execute real `out` instructions to the QEMU/Bochs debug port 0xE9:

```sh
make -C ../limine-e9 -j
timeout 45 qemu-system-x86_64 -M q35 -m 2G -cdrom ../limine-e9/template-x86_64.iso \
    -boot d -display none -no-reboot \
    -device isa-debugcon,chardev=e9 -chardev file,id=e9,path=e9.log
cat e9.log
# WELCOME TO SQL-OS VIA PORT E9   <- staged in RAM, patched by UPDATE, streamed to 0xE9
# address | value / a / ...       <- each result set prints centered, padded
# 4 | 79 ... 0 1 1 ... 55            with " | " gaps and bars that line up
# hhdm_offset|kernel_phys_base|... <- the boot table: HHDM, kernel bases, ...
# base|length|type (20 regions)    <- the Limine memory map as a table
# E                               <- final byte, no newline
```

The generated translation unit is compiled with the kernel's freestanding
flags except `-mno-sse`/`-mno-80387` (the value model has doubles, which the
SysV ABI returns in `xmm`); `kmain()` enables SSE in hardware before first
use. Before the program runs, `kmain()` also copies the Limine responses
into the `boot_info` / `memory_map` built-in tables, so SQL can `SELECT`
the HHDM offset, kernel bases, framebuffer, bootloader info and the whole
memory map like any other table.
See `../limine-e9/README.md` for details.

## Tests

`tests/test_codegen.cpp` exercises three layers:

1. **Refusal contract** — every `not yet compiled: ...` message is asserted
   against a program that must first *validate* (a validation miss fails
   the test) and then refuse with the exact message at the right position.
2. **Freestanding compilation** — representative programs are generated and
   compiled with `g++ -std=c++17 -ffreestanding -Wall -Wextra -Werror`.
3. **Hosted execution** — generated code is linked with
   `tests/codegen_driver.cpp`, run, and its stdout compared cell-by-cell:
   CRUD, recursion (anchor/term/`VALUES` anchors/outer `WHERE`), hardware
   I/O (8/16/32-bit ports, both memory images), expressions, `ALTER`
   chains, star forms, simultaneous `UPDATE` assignment, deletion
   compaction, CTAS and `INSERT ... SELECT`.

`tests/test_validate.cpp` covers the validator (catalog, contracts, types,
recursive-CTE structure, issue positions).

## Layout

```text
sql-os/
├── CMakeLists.txt
├── include/sqlos/
│   ├── catalog.hpp          # tables, columns, types, system classes
│   ├── positions.hpp        # statement start positions
│   ├── validate.hpp         # Validation/Issue API
│   ├── codegen.hpp          # generate() → CodegenResult
│   └── runtime/sqlos_runtime.hpp   # the freestanding runtime (single header)
├── src/
│   ├── catalog.cpp
│   ├── positions.cpp
│   ├── validate.cpp         # the validator
│   ├── codegen.cpp          # the code generator
│   └── main.cpp             # CLI: sqlos [--check] [--emit OUT.cpp] [FILE]
└── tests/
    ├── test_validate.cpp
    ├── test_codegen.cpp
    └── codegen_driver.cpp   # hosted driver linked into execution tests
```

## Status

Validator and codegen v1 are complete and tested (116 tests, 244 checks),
and the whole pipeline boots: `../limine-e9/` compiles an SQL program into
a Limine kernel that writes port 0xE9 on real (emulated) hardware.
Next steps on the compiler side: joins and aggregation, `ORDER BY`/
`LIMIT`, correlated subqueries (`EXISTS` / `IN (SELECT ...)` still
refuse), and `RETURNING` — each one moves from
`not yet compiled:` to codegen coverage behind this same test contract.
