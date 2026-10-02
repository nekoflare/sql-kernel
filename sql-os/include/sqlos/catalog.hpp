// Semantic layer: types, tables and the catalog (built-in OS tables plus
// whatever DDL declares). The parser itself is untyped — this is where the
// validator gets its knowledge of what exists.

#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sqlos {

// Semantic types used by the validator. Null and Unknown are wildcards:
// compatible with every other type (NULL is assignable anywhere, and
// untyped things — parameters, untyped columns — are checked at runtime).
enum class Type { Unknown, Null, Int, Real, Text, Bool };

// Human-readable spelling for messages: "INT", "REAL", "TEXT", "BOOL",
// "NULL", "UNKNOWN".
std::string type_name(Type type);

// Can a value of type `value` be stored into a column of type `column`?
// Widening inside the numeric family (INT -> REAL) is allowed; everything
// else must match exactly. Null/Unknown pass in either position.
bool assignable(Type value, Type column);

// May the two types be compared with <, =, BETWEEN, ...?
bool comparable(Type left, Type right);

// Least upper bound of two types for set operations / CASE branches.
// Returns false when the types are incompatible.
bool unify_types(Type left, Type right, Type* out);

// Maps a DDL type name ("VARCHAR(255)", "double precision", "BOOL") onto a
// Type. An empty name maps to Unknown (untyped column, SQLite style).
// Returns false for names the OS does not understand.
bool type_from_name(std::string_view type_name, Type* out);

struct Column {
  std::string name;
  Type type = Type::Unknown;
  bool not_null = false;
  bool has_default = false;
  // Join USING / NATURAL: the column exists on both sides but `*` expands
  // it only once.
  bool hidden = false;
};

// How a table is backed by the OS.
enum class SystemClass {
  None,     // ordinary user table (persistent store)
  IoRead,   // io_{8,16,32}_read  — SELECT only
  IoWrite,  // io_{8,16,32}_write — INSERT only
  Memory,   // memory / volatile_memory — SELECT / INSERT / UPDATE
  Boot,     // boot_info / memory_map — SELECT only, host-filled at boot
};

enum class TableKind { Table, View };

struct Table {
  std::string name;
  TableKind kind = TableKind::Table;
  SystemClass sys = SystemClass::None;
  int io_width = 0;  // 8 / 16 / 32 for IoRead and IoWrite
  std::vector<Column> columns;

  bool system() const { return sys != SystemClass::None; }
};

// Case-insensitive name → table. The system tables pre-exist; user DDL
// mutates the catalog as statements validate (later statements see earlier
// CREATE/DROP/ALTER).
class Catalog {
 public:
  Catalog();  // creates the built-in OS tables

  const Table* find(std::string_view name) const;
  Table* find_mut(std::string_view name);

  // Inserts (or replaces, with `replace`) a table. Returns false when the
  // name is taken and replace is false.
  bool insert(Table table, bool replace = false);
  bool erase(std::string_view name);

  // Indexes live in the same namespace as tables.
  bool has_index(std::string_view name) const;
  bool add_index(std::string_view name);
  bool erase_index(std::string_view name);

  std::map<std::string, Table>& tables() { return tables_; }
  const std::map<std::string, Table>& tables() const { return tables_; }

 private:
  static std::string key(std::string_view name);

  std::map<std::string, Table> tables_;
  std::set<std::string> indexes_;
};

}  // namespace sqlos
