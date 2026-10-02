#include "sqlos/catalog.hpp"

#include <cctype>

namespace sqlos {
namespace {

std::string lower(std::string_view word) {
  std::string out;
  out.reserve(word.size());
  for (char c : word) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

void trim(std::string* s) {
  while (!s->empty() && s->back() == ' ') s->pop_back();
  std::size_t start = 0;
  while (start < s->size() && (*s)[start] == ' ') ++start;
  if (start > 0) *s = s->substr(start);
}

bool in_set(const std::string& word, const char* const* set, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (word == set[i]) return true;
  }
  return false;
}

Table make_io_table(int width, bool read) {
  const std::string w = std::to_string(width);
  Table table;
  table.name = "io_" + w + (read ? "_read" : "_write");
  table.sys = read ? SystemClass::IoRead : SystemClass::IoWrite;
  table.io_width = width;
  table.columns.push_back({"port", Type::Int});
  table.columns.push_back({"value", Type::Int});
  return table;
}

Table make_memory_table(const std::string& name) {
  Table table;
  table.name = name;
  table.sys = SystemClass::Memory;
  table.columns.push_back({"address", Type::Int});
  table.columns.push_back({"value", Type::Int});
  return table;
}

}  // namespace

// --- types -----------------------------------------------------------------

std::string type_name(Type type) {
  switch (type) {
    case Type::Unknown:
      return "UNKNOWN";
    case Type::Null:
      return "NULL";
    case Type::Int:
      return "INT";
    case Type::Real:
      return "REAL";
    case Type::Text:
      return "TEXT";
    case Type::Bool:
      return "BOOL";
  }
  return "UNKNOWN";
}

bool assignable(Type value, Type column) {
  if (value == Type::Unknown || value == Type::Null) return true;
  if (column == Type::Unknown || column == Type::Null) return true;
  if (value == column) return true;
  // Widening inside the numeric family only.
  return value == Type::Int && column == Type::Real;
}

bool comparable(Type left, Type right) {
  if (left == Type::Unknown || left == Type::Null) return true;
  if (right == Type::Unknown || right == Type::Null) return true;
  if (left == right) return true;
  return (left == Type::Int || left == Type::Real) &&
         (right == Type::Int || right == Type::Real);
}

bool unify_types(Type left, Type right, Type* out) {
  if (left == Type::Unknown || left == Type::Null) {
    *out = right;
    return true;
  }
  if (right == Type::Unknown || right == Type::Null) {
    *out = left;
    return true;
  }
  if (left == right) {
    *out = left;
    return true;
  }
  if ((left == Type::Int || left == Type::Real) &&
      (right == Type::Int || right == Type::Real)) {
    *out = Type::Real;
    return true;
  }
  return false;
}

bool type_from_name(std::string_view raw, Type* out) {
  std::string base = lower(raw);
  const std::size_t paren = base.find('(');
  if (paren != std::string::npos) base = base.substr(0, paren);
  trim(&base);
  if (base.empty()) {  // untyped column
    *out = Type::Unknown;
    return true;
  }

  static const char* kInt[] = {"int",     "integer", "tinyint",  "smallint",
                               "mediumint", "bigint", "int1",     "int2",
                               "int4",    "int8",    "serial",   "bigserial"};
  static const char* kReal[] = {"real",   "float",  "double", "numeric",
                                "decimal"};
  static const char* kText[] = {"text",     "varchar", "char",  "nchar",
                                "nvarchar", "blob",    "uuid",  "clob",
                                "character varying"};
  static const char* kBool[] = {"bool", "boolean"};

  if (in_set(base, kInt, sizeof(kInt) / sizeof(*kInt))) {
    *out = Type::Int;
    return true;
  }
  if (in_set(base, kReal, sizeof(kReal) / sizeof(*kReal))) {
    *out = Type::Real;
    return true;
  }
  if (in_set(base, kText, sizeof(kText) / sizeof(*kText))) {
    *out = Type::Text;
    return true;
  }
  if (in_set(base, kBool, sizeof(kBool) / sizeof(*kBool))) {
    *out = Type::Bool;
    return true;
  }
  // Temporal types are stored as text (ISO-8601 strings sort lexically).
  if (base == "date" || base == "time" || base == "datetime" ||
      base == "timestamp" || base.rfind("timestamp ", 0) == 0 ||
      base.rfind("time ", 0) == 0) {
    *out = Type::Text;
    return true;
  }
  return false;
}

// --- catalog ---------------------------------------------------------------

std::string Catalog::key(std::string_view name) { return lower(name); }

Catalog::Catalog() {
  for (int width : {8, 16, 32}) {
    const Table read = make_io_table(width, /*read=*/true);
    const Table write = make_io_table(width, /*read=*/false);
    tables_.emplace(key(read.name), read);
    tables_.emplace(key(write.name), write);
  }
  const Table memory = make_memory_table("memory");
  const Table volatile_memory = make_memory_table("volatile_memory");
  tables_.emplace(key(memory.name), memory);
  tables_.emplace(key(volatile_memory.name), volatile_memory);
}

const Table* Catalog::find(std::string_view name) const {
  const auto it = tables_.find(key(name));
  return it == tables_.end() ? nullptr : &it->second;
}

Table* Catalog::find_mut(std::string_view name) {
  const auto it = tables_.find(key(name));
  return it == tables_.end() ? nullptr : &it->second;
}

bool Catalog::insert(Table table, bool replace) {
  const std::string k = key(table.name);
  const auto it = tables_.find(k);
  if (it != tables_.end()) {
    if (!replace) return false;
    // Never allow user DDL to clobber a built-in, even with OR REPLACE.
    if (it->second.system()) return false;
  }
  if (indexes_.count(k) != 0 && !replace) return false;
  tables_[k] = std::move(table);
  return true;
}

bool Catalog::erase(std::string_view name) {
  return tables_.erase(key(name)) != 0;
}

bool Catalog::has_index(std::string_view name) const {
  return indexes_.count(key(name)) != 0;
}

bool Catalog::add_index(std::string_view name) {
  const std::string k = key(name);
  if (indexes_.count(k) != 0 || tables_.count(k) != 0) return false;
  indexes_.insert(k);
  return true;
}

bool Catalog::erase_index(std::string_view name) {
  return indexes_.erase(key(name)) != 0;
}

}  // namespace sqlos
