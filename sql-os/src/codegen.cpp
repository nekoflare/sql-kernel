// Ahead-of-time compiler: validated SQL statements -> one C++ translation
// unit. Nothing is ever interpreted: the generated file includes
// sqlos/runtime/sqlos_runtime.hpp and is compiled with g++ -ffreestanding
// into the running program. Constructs outside the compiler's current
// surface (joins, aggregates, windows, set operations, ...) are refused
// explicitly with "not yet compiled: ..." — semantics are never dropped
// silently.

#include "sqlos/codegen.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sqlos/catalog.hpp"

namespace sqlos {
namespace {

using K = sql::Expr::Kind;

// ---------------------------------------------------------------- helpers ---

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

std::string q(const std::string& s) { return "'" + s + "'"; }

std::string bool_lit(bool b) { return b ? "true" : "false"; }

std::string join_dot(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& p : parts) {
    if (!out.empty()) out += '.';
    out += p;
  }
  return out;
}

// Mirror of the validator's naming for unnamed select items.
std::string derive_name(const sql::Expr& e) {
  if (e.kind == K::ColumnRef && !e.parts.empty()) return e.parts.back();
  if (e.kind == K::FunctionCall && !e.parts.empty()) return e.parts.back();
  return "?column?";
}

// Round-trip-safe spelling of a double literal.
std::string fmt_double(double v) {
  std::ostringstream ss;
  ss << std::setprecision(17) << v;
  std::string out = ss.str();
  if (out.find('.') == std::string::npos && out.find('e') == std::string::npos &&
      out.find('E') == std::string::npos) {
    out += ".0";
  }
  return out;
}

// Escapes into a C string literal. Octal escapes are exactly three digits,
// so the literal's byte count equals the original byte count.
std::string c_string(const std::string& raw) {
  std::string out = "\"";
  for (unsigned char ch : raw) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch >= 32 && ch <= 126) {
          out += static_cast<char>(ch);
        } else {
          out += '\\';
          out += static_cast<char>('0' + ((ch >> 6) & 7));
          out += static_cast<char>('0' + ((ch >> 3) & 7));
          out += static_cast<char>('0' + (ch & 7));
        }
    }
  }
  out += '"';
  return out;
}

// Identifier-safe spelling of a lower-case key.
std::string sanitize(const std::string& s) {
  std::string out;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (ok) {
      out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    } else {
      out += '_';
    }
  }
  return out;
}

// NOT NULL / PRIMARY KEY make a column reject NULL at runtime.
bool col_def_not_null(const sql::ColumnDef& def) {
  for (const sql::ColumnConstraint& c : def.constraints) {
    if (c.kind == sql::ColumnConstraint::Kind::NotNull ||
        c.kind == sql::ColumnConstraint::Kind::PrimaryKey) {
      return true;
    }
  }
  return false;
}

// Indents every line of generated code (purely cosmetic).
std::string indent(const std::string& code, const std::string& pad) {
  if (code.empty()) return code;
  std::string out;
  std::size_t start = 0;
  while (start < code.size()) {
    const std::size_t nl = code.find('\n', start);
    if (nl == std::string::npos) {
      out += pad + code.substr(start);
      break;
    }
    out += pad + code.substr(start, nl - start + 1);
    start = nl + 1;
  }
  return out;
}

// --- WHERE-contract scanning (hardware tables) ------------------------------

// A ColumnRef naming `column` (bare or qualified: the hardware scope has
// exactly one relation, so any qualifier can only be that relation).
bool refers_to(const sql::Expr* e, const std::string& column) {
  if (e == nullptr || e->kind != K::ColumnRef || e->parts.empty()) return false;
  return lower(e->parts.back()) == column;
}

// True when the expression contains any column reference.
bool expr_has_column(const sql::Expr& e) {
  if (e.kind == K::ColumnRef || e.kind == K::Star) return true;
  for (const sql::ExprPtr& a : e.args) {
    if (a && expr_has_column(*a)) return true;
  }
  if (e.operand && expr_has_column(*e.operand)) return true;
  if (e.else_result && expr_has_column(*e.else_result)) return true;
  for (const sql::ExprPtr& r : e.results) {
    if (r && expr_has_column(*r)) return true;
  }
  return false;
}

void collect_conjuncts(const sql::Expr* e, std::vector<const sql::Expr*>* out) {
  if (e->kind == K::Binary && upper(e->name) == "AND" && e->args.size() == 2 &&
      e->args[0] && e->args[1]) {
    collect_conjuncts(e->args[0].get(), out);
    collect_conjuncts(e->args[1].get(), out);
    return;
  }
  out->push_back(e);
}

// A compilation refusal (always explicit — never a silent drop).
struct Refused {
  SourcePos pos;
  std::string message;
};

// ------------------------------------------------- compile-time schema ------

// One column of a user table: current lower-case name, stable storage field
// index (renames keep the index, drops leave the field allocated), NOT NULL.
struct ColInfo {
  std::string name;
  int idx = 0;
  bool not_null = false;
};

struct TableCG {
  std::string key;      // lower-case current name
  std::string storage;  // C++ variable (never empty)
  bool view = false;    // views have no storage of their own
  int field_count = 0;
  std::vector<ColInfo> columns;
};

// How a column reference resolves in generated code: a storage field read
// through a scan loop, or a named local Value (hardware rows).
struct ColSrc {
  bool is_field = true;
  std::string storage;  // field: struct variable
  int field = -1;       // field: cN index
  std::string local;    // local: Value variable name
  std::string loop;     // field: enclosing scan loop variable
};

std::string col_code(const ColSrc& src) {
  if (!src.is_field) return src.local;
  return src.storage + ".c" + std::to_string(src.field) + "[" + src.loop + "]";
}

struct Binding {
  std::string key;  // lower-case relation name (alias)
  std::vector<std::pair<std::string, ColSrc>> cols;
};

// A materialized CTE usable in FROM (no loop chosen yet).
struct CteInfo {
  std::string key;
  std::string storage;
  std::vector<ColInfo> cols;
};

struct Scope {
  std::vector<Binding> bindings;  // FROM relations visible to expressions
  std::vector<CteInfo> ctes;      // materialized CTEs usable in FROM
};

// One output column of an INSERT row: storage field (user table) or system
// column marker ("port" / "value" / "address").
struct EffCol {
  int field = -1;
  bool not_null = false;
  std::string sys_col;
};

// A planned select core: prologue and brackets around row emission.
// rows holds one entry per emitted row (VALUES emits several).
struct SelectParts {
  std::string head;
  std::string loop_open;
  std::string cond_open;
  std::string tail;
  std::vector<std::string> names;
  std::vector<std::vector<std::string>> rows;
};

class Generator {
 public:
  explicit Generator(const std::vector<SourcePos>& positions)
      : positions_(positions) {}

  CodegenResult run(const std::vector<sql::Statement>& statements);

 private:
  // --- refusal / identifiers ------------------------------------------------
  [[noreturn]] void refuse(const std::string& what) {
    throw Refused{pos_, "not yet compiled: " + what};
  }
  [[noreturn]] void internal(const std::string& what) {
    throw Refused{pos_, "internal codegen error: " + what};
  }
  int next_id() { return id_++; }

  // --- storage registry -----------------------------------------------------
  std::string make_storage(const std::string& want);
  std::string emit_struct(const std::string& var, int nfields) const;
  const TableCG* cg_for(const std::string& name) const;

  // --- statements -----------------------------------------------------------
  std::string gen_statement(const sql::Statement& s);
  std::string do_create_table(const sql::CreateTableStatement& s);
  std::string do_create_view(const sql::CreateViewStatement& s);
  std::string do_drop(const sql::DropStatement& s);
  std::string do_alter(const sql::AlterTableStatement& s);

  std::string gen_select_stmt(const sql::SelectStatement& s);
  std::string gen_with(const sql::SelectStatement& s,
                       std::vector<std::string>* out_names = nullptr);
  std::string gen_select_core(const sql::SelectStatement& s, const Scope& outer,
                              const std::string& sink,
                              const Binding* preset = nullptr,
                              std::vector<std::string>* out_names = nullptr);
  // Announces a result set's column names to the host sink before its rows.
  std::string announce_header(const std::vector<std::string>& names);
  SelectParts plan_select(const sql::SelectStatement& s, const Scope& outer,
                          const Binding* preset);
  static std::string render(const SelectParts& p, const std::string& sink);

  std::string gen_insert(const sql::InsertStatement& s);
  std::string gen_update(const sql::UpdateStatement& s);
  std::string gen_delete(const sql::DeleteStatement& s);

  // --- expressions ----------------------------------------------------------
  std::string gen_expr(const sql::Expr& e, const Scope& sc);
  std::string gen_binary(const sql::Expr& e, const Scope& sc);
  std::string gen_case(const sql::Expr& e, const Scope& sc);
  std::string gen_call(const sql::Expr& e, const Scope& sc);
  std::string resolve_column(const sql::Expr& e, const Scope& sc);

  // --- shared building blocks ----------------------------------------------
  // AND-folds the conjuncts and wraps them in truth(); "" for an empty list.
  std::string cond_code(const std::vector<const sql::Expr*>& conj,
                        const Scope& sc);

  // Finds `column = expr` (or address BETWEEN a AND b) among top-level AND
  // conjuncts; the contract conjunct is removed from `rest`.
  struct HwContract {
    bool found = false;
    const sql::Expr* value = nullptr;  // "=" form: the column-free side
    const sql::Expr* low = nullptr;    // BETWEEN form
    const sql::Expr* high = nullptr;
    std::vector<const sql::Expr*> rest;
  };
  HwContract extract_contract(const sql::Expr* where, const std::string& column,
                              bool allow_between);

  // Emits one appended row for a user table (cells -> fields, NOT NULL traps).
  std::string store_row_code(const std::string& storage,
                             const std::vector<EffCol>& eff,
                             const std::vector<EffCol>& rest,
                             const std::vector<std::string>& cells);

  // Emits one row for a system-table INSERT target.
  std::string sys_row_code(const Table* t, const std::vector<EffCol>& eff,
                           const std::vector<std::string>& cells);

  std::string assemble() const;

  // --- state ----------------------------------------------------------------
  std::vector<SourcePos> positions_;
  SourcePos pos_{1, 1};
  int stmt_index_ = 0;
  int id_ = 0;

  Catalog catalog_;
  std::map<std::string, std::string> table_var_;  // lower name -> storage var
  std::map<std::string, TableCG> storage_;        // var -> schema (permanent)
  std::set<std::string> storage_names_;
  std::vector<std::pair<std::string, int>> worklists_;  // var, nfields
  std::vector<std::string> bodies_;
};

// ------------------------------------------------------------- entry point ---

CodegenResult Generator::run(const std::vector<sql::Statement>& statements) {
  CodegenResult result;
  try {
    for (std::size_t i = 0; i < statements.size(); ++i) {
      stmt_index_ = static_cast<int>(i);
      id_ = 0;
      pos_ = i < positions_.size() ? positions_[i] : SourcePos{1, 1};
      bodies_.push_back(gen_statement(statements[i]));
    }
    result.code = assemble();
  } catch (const Refused& r) {
    result.errors.push_back({r.pos, r.message});
  }
  return result;
}

std::string Generator::make_storage(const std::string& want) {
  const std::string base = "st_" + sanitize(want);
  std::string name = base;
  int n = 2;
  while (!storage_names_.insert(name).second) {
    name = base + "_" + std::to_string(n++);
  }
  return name;
}

std::string Generator::emit_struct(const std::string& var, int nfields) const {
  std::string out = "struct " + var + "_t {\n  sqlos::u32 count;\n";
  for (int i = 0; i < nfields; ++i) {
    out += "  sqlos::Value c" + std::to_string(i) + "[SQLOS_TABLE_CAP];\n";
  }
  out += "};\n" + var + "_t " + var + ";\n\n";
  return out;
}

const TableCG* Generator::cg_for(const std::string& name) const {
  const auto it = table_var_.find(lower(name));
  if (it == table_var_.end()) return nullptr;
  const auto sit = storage_.find(it->second);
  return sit == storage_.end() ? nullptr : &sit->second;
}

// ------------------------------------------------------------ DDL -----------

std::string Generator::do_create_table(const sql::CreateTableStatement& s) {
  const std::string key = lower(s.name);
  const bool exists =
      catalog_.find(s.name) != nullptr || catalog_.has_index(s.name);
  if (exists) {
    if (s.if_not_exists) return "";
    internal("table " + q(s.name) + " already exists");
  }

  Table t;
  t.name = s.name;

  std::string app;
  std::string select_code;
  std::vector<std::string> names;
  if (s.select != nullptr) {
    // CREATE TABLE ... AS SELECT: column names come from the query output.
    app = "app" + std::to_string(next_id()) + "_";
    select_code = gen_select_core(*s.select, Scope{}, app, nullptr, &names);
  } else {
    for (const sql::ColumnDef& def : s.columns) names.push_back(def.name);
  }
  if (names.empty()) {
    internal("CREATE TABLE " + q(s.name) + " produces no columns");
  }

  TableCG cg;
  cg.key = key;
  cg.storage = make_storage(key);
  cg.view = false;
  for (std::size_t i = 0; i < names.size(); ++i) {
    ColInfo ci;
    ci.name = lower(names[i]);
    ci.idx = static_cast<int>(i);
    if (s.select == nullptr) ci.not_null = col_def_not_null(s.columns[i]);
    cg.columns.push_back(ci);

    Column col;
    col.name = names[i];
    if (s.select == nullptr) {
      Type ty = Type::Unknown;
      if (!s.columns[i].type_name.empty()) {
        (void)type_from_name(s.columns[i].type_name, &ty);
      }
      col.type = ty;
      col.not_null = ci.not_null;
    }
    t.columns.push_back(col);
  }
  cg.field_count = static_cast<int>(names.size());
  if (!catalog_.insert(t)) internal("table " + q(s.name) + " already exists");
  table_var_[key] = cg.storage;
  storage_[cg.storage] = cg;

  std::string body = cg.storage + ".count = 0;\n";
  if (s.select != nullptr) {
    // Rows flow through an append sink into the fresh table.
    std::vector<EffCol> eff;
    std::vector<std::string> cells;
    for (std::size_t i = 0; i < names.size(); ++i) {
      EffCol e;
      e.field = static_cast<int>(i);
      e.not_null = false;
      eff.push_back(e);
      cells.push_back("r_->cells[" + std::to_string(i) + "]");
    }
    body += "{\n";
    body += "  sqlos::Sink " + app + ";\n";
    body += "  " + app + ".fn = [](void* c_, const sqlos::Row* r_) {\n";
    body += "    (void)c_;\n";
    body += indent(store_row_code(cg.storage, eff, {}, cells), "    ");
    body += "  };\n";
    body += "  " + app + ".hdr = nullptr;\n";
    body += "  " + app + ".ctx = nullptr;\n";
    body += select_code;
    body += "}\n";
  }
  return body;
}

std::string Generator::do_create_view(const sql::CreateViewStatement& s) {
  const std::string key = lower(s.name);
  const Table* existing = catalog_.find(s.name);
  const bool index_clash = catalog_.has_index(s.name);
  if (existing != nullptr || index_clash) {
    const bool replaceable = s.or_replace && existing != nullptr &&
                             existing->kind == TableKind::View && !index_clash;
    if (!replaceable) {
      if (s.if_not_exists) return "";
      internal("view " + q(s.name) + " already exists");
    }
  }
  // The view's query is never planned here (SELECT from a view is refused
  // later); only its existence matters for name resolution.
  Table v;
  v.name = s.name;
  v.kind = TableKind::View;
  if (!catalog_.insert(v, s.or_replace)) {
    internal("view " + q(s.name) + " already exists");
  }
  TableCG cg;
  cg.key = key;
  cg.storage = make_storage(key);
  cg.view = true;
  table_var_[key] = cg.storage;
  storage_[cg.storage] = cg;
  return "";
}

std::string Generator::do_drop(const sql::DropStatement& s) {
  if (lower(s.object_type) == "index") return "";
  for (const std::string& name : s.names) {
    table_var_.erase(lower(name));
    // The storage entry stays: earlier statements still reference its struct.
    catalog_.erase(name);
  }
  return "";
}

std::string Generator::do_alter(const sql::AlterTableStatement& s) {
  const std::string key = lower(s.table);
  const auto vit = table_var_.find(key);
  if (vit == table_var_.end()) return "";  // unknown (validator errored) or IF EXISTS
  Table* t = catalog_.find_mut(s.table);
  if (t == nullptr) internal("table " + q(s.table) + " not in catalog");
  if (t->system()) internal("system table " + q(s.table) + " cannot be altered");
  TableCG* cg = &storage_[vit->second];

  switch (s.action) {
    case sql::AlterTableStatement::Action::AddColumn: {
      const std::string cname = lower(s.column.name);
      for (const ColInfo& c : cg->columns) {
        if (c.name == cname) internal("column " + q(s.column.name) + " already exists");
      }
      ColInfo ci;
      ci.name = cname;
      ci.idx = cg->field_count;
      ci.not_null = col_def_not_null(s.column);
      cg->columns.push_back(ci);
      cg->field_count++;
      Column col;
      col.name = s.column.name;
      col.not_null = ci.not_null;
      t->columns.push_back(col);
      break;
    }
    case sql::AlterTableStatement::Action::DropColumn: {
      const std::string cname = lower(s.column_name);
      const auto it = std::find_if(
          t->columns.begin(), t->columns.end(),
          [&](const Column& c) { return lower(c.name) == cname; });
      if (it != t->columns.end()) t->columns.erase(it);
      // The storage field stays allocated (indices are stable).
      cg->columns.erase(std::remove_if(cg->columns.begin(), cg->columns.end(),
                                       [&](const ColInfo& c) {
                                         return c.name == cname;
                                       }),
                        cg->columns.end());
      break;
    }
    case sql::AlterTableStatement::Action::RenameTable: {
      if (catalog_.find(s.new_name) != nullptr ||
          catalog_.has_index(s.new_name)) {
        internal("table " + q(s.new_name) + " already exists");
      }
      Table moved = std::move(*t);
      catalog_.erase(s.table);
      moved.name = s.new_name;
      if (!catalog_.insert(std::move(moved))) {
        internal("table " + q(s.new_name) + " already exists");
      }
      const std::string var = vit->second;
      table_var_.erase(key);
      table_var_[lower(s.new_name)] = var;
      storage_[var].key = lower(s.new_name);
      break;
    }
    case sql::AlterTableStatement::Action::RenameColumn: {
      const std::string from = lower(s.column_name);
      const std::string to = lower(s.new_name);
      const auto it = std::find_if(
          t->columns.begin(), t->columns.end(),
          [&](const Column& c) { return lower(c.name) == from; });
      if (it == t->columns.end()) break;  // IF EXISTS (validator allowed it)
      for (const Column& c : t->columns) {
        if (&c != &*it && lower(c.name) == to) {
          internal("column " + q(s.new_name) + " already exists");
        }
      }
      it->name = s.new_name;
      for (ColInfo& c : cg->columns) {
        if (c.name == from) {
          c.name = to;
          break;
        }
      }
      break;
    }
  }
  return "";
}

// ------------------------------------------------------------ SELECT --------

std::string Generator::gen_statement(const sql::Statement& s) {
  if (const auto* p = std::get_if<sql::SelectStatement>(&s)) {
    return gen_select_stmt(*p);
  }
  if (const auto* p = std::get_if<sql::InsertStatement>(&s)) {
    return gen_insert(*p);
  }
  if (const auto* p = std::get_if<sql::UpdateStatement>(&s)) {
    return gen_update(*p);
  }
  if (const auto* p = std::get_if<sql::DeleteStatement>(&s)) {
    return gen_delete(*p);
  }
  if (const auto* p = std::get_if<sql::CreateTableStatement>(&s)) {
    return do_create_table(*p);
  }
  if (std::get_if<sql::CreateIndexStatement>(&s)) return "";  // no runtime form
  if (const auto* p = std::get_if<sql::CreateViewStatement>(&s)) {
    return do_create_view(*p);
  }
  if (const auto* p = std::get_if<sql::DropStatement>(&s)) return do_drop(*p);
  if (const auto* p = std::get_if<sql::AlterTableStatement>(&s)) {
    return do_alter(*p);
  }
  if (std::get_if<sql::TransactionStatement>(&s)) {
    refuse("transaction control");
  }
  internal("unknown statement kind");
}

std::string Generator::gen_select_stmt(const sql::SelectStatement& s) {
  std::vector<std::string> names;
  std::string body;
  if (!s.with.empty()) {
    body = gen_with(s, &names);
  } else {
    body = gen_select_core(s, Scope{}, "sink", nullptr, &names);
  }
  return announce_header(names) + body;
}

std::string Generator::announce_header(const std::vector<std::string>& names) {
  if (names.empty()) return "";
  const std::string id = "hdr" + std::to_string(next_id()) + "_";
  std::string c;
  c += "{\n";
  // static: hosts may keep the pointers past the callback (they hand the
  // table back at the next header), so the names must outlive the call.
  c += "  static const char* " + id + "[] = {";
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i != 0) c += ", ";
    c += c_string(names[i]);
  }
  c += "};\n";
  c += "  if (sink.hdr != nullptr) sink.hdr(sink.ctx, " + id + ", " +
       std::to_string(names.size()) + ");\n";
  c += "}\n";
  return c;
}

std::string Generator::gen_select_core(const sql::SelectStatement& s,
                                       const Scope& outer,
                                       const std::string& sink,
                                       const Binding* preset,
                                       std::vector<std::string>* out_names) {
  SelectParts p = plan_select(s, outer, preset);
  if (out_names != nullptr) *out_names = p.names;
  return render(p, sink);
}

std::string Generator::render(const SelectParts& p, const std::string& sink) {
  std::string code = p.head;
  if (!p.loop_open.empty()) code += p.loop_open;
  if (!p.cond_open.empty()) code += p.cond_open;
  for (const std::vector<std::string>& row : p.rows) {
    code += "{\n";
    code += "  sqlos::Row row_;\n";
    code += "  row_.count = " + std::to_string(row.size()) + ";\n";
    for (std::size_t k = 0; k < row.size(); ++k) {
      code += "  row_.cells[" + std::to_string(k) + "] = " + row[k] + ";\n";
    }
    code += "  " + sink + ".fn(" + sink + ".ctx, &row_);\n";
    code += "}\n";
  }
  if (!p.cond_open.empty()) code += "}\n";
  if (!p.loop_open.empty()) code += "}\n";
  code += p.tail;
  return code;
}

std::string Generator::cond_code(const std::vector<const sql::Expr*>& conj,
                                 const Scope& sc) {
  if (conj.empty()) return "";
  std::string code = gen_expr(*conj.back(), sc);
  for (int i = static_cast<int>(conj.size()) - 2; i >= 0; --i) {
    code = "sqlos::v_and(" + gen_expr(*conj[static_cast<std::size_t>(i)], sc) +
           ", " + code + ")";
  }
  return "sqlos::truth(" + code + ")";
}

Generator::HwContract Generator::extract_contract(const sql::Expr* where,
                                                  const std::string& column,
                                                  bool allow_between) {
  HwContract c;
  if (where == nullptr) return c;
  std::vector<const sql::Expr*> all;
  collect_conjuncts(where, &all);

  // Pass 1: column = expr / expr = column with a column-free other side
  // (the hardware locals do not exist yet while the contract is evaluated).
  for (const sql::Expr* e : all) {
    if (e->kind != K::Binary || e->name != "=" || e->args.size() != 2) continue;
    const sql::Expr* other = nullptr;
    if (refers_to(e->args[0].get(), column) &&
        !refers_to(e->args[1].get(), column)) {
      if (!expr_has_column(*e->args[1])) other = e->args[1].get();
    } else if (refers_to(e->args[1].get(), column) &&
               !refers_to(e->args[0].get(), column)) {
      if (!expr_has_column(*e->args[0])) other = e->args[0].get();
    }
    if (other == nullptr) continue;
    c.found = true;
    c.value = other;
    for (const sql::Expr* r : all) {
      if (r != e) c.rest.push_back(r);
    }
    return c;
  }

  // Pass 2: address BETWEEN low AND high (positive form only).
  if (allow_between) {
    for (const sql::Expr* e : all) {
      if (e->kind != K::Between || e->negated || e->args.size() < 3) continue;
      if (!refers_to(e->args[0].get(), column)) continue;
      if (expr_has_column(*e->args[1]) || expr_has_column(*e->args[2])) continue;
      c.found = true;
      c.low = e->args[1].get();
      c.high = e->args[2].get();
      for (const sql::Expr* r : all) {
        if (r != e) c.rest.push_back(r);
      }
      return c;
    }
  }
  return c;
}

SelectParts Generator::plan_select(const sql::SelectStatement& s,
                                   const Scope& outer, const Binding* preset) {
  SelectParts p;

  // Statement features outside the compiler's surface.
  if (!s.with.empty()) refuse("nested WITH clause");
  if (s.distinct || !s.distinct_on.empty()) refuse("DISTINCT");
  if (s.group_all || s.group_distinct || !s.group_by.empty()) refuse("GROUP BY");
  if (s.having) refuse("HAVING");
  if (!s.set_op.empty()) refuse("set operation " + s.set_op);
  if (!s.order_by.empty()) refuse("ORDER BY");
  if (s.limit || s.limit_all) refuse("LIMIT");
  if (s.offset) refuse("OFFSET");

  Scope sc = outer;

  enum class Sk { None, Scan, IoRead, Memory };
  Sk sk = Sk::None;
  std::string scan_var;
  std::string scan_loop;
  const Table* sys_table = nullptr;
  std::string mem_img;
  std::string src_key;

  if (preset != nullptr) {
    // Recursive term: rows come from the enclosing worklist loop.
    if (s.from.size() != 1) internal("recursive term without a FROM clause");
    sc.bindings.push_back(*preset);
    src_key = preset->key;
    sk = Sk::Scan;
  } else if (!s.from.empty()) {
    if (s.from.size() > 1) refuse("multiple FROM items (joins)");
    const sql::TableRef& r = s.from[0];
    if (r.kind == sql::TableRef::Kind::Join) refuse("JOIN");
    if (r.kind == sql::TableRef::Kind::Subquery) refuse("subquery in FROM");
    if (r.kind == sql::TableRef::Kind::TableFunction) {
      refuse("table function in FROM");
    }
    src_key = lower(r.alias.empty() ? r.name : r.alias);

    bool from_cte = false;
    for (const CteInfo& c : outer.ctes) {
      if (c.key != src_key) continue;
      Binding b;
      b.key = src_key;
      const std::string loop = "i" + std::to_string(next_id()) + "_";
      for (const ColInfo& col : c.cols) {
        b.cols.push_back(
            {col.name, ColSrc{true, c.storage, col.idx, "", loop}});
      }
      sc.bindings.push_back(std::move(b));
      scan_var = c.storage;
      scan_loop = loop;
      sk = Sk::Scan;
      from_cte = true;
      break;
    }

    if (!from_cte) {
      const Table* t = catalog_.find(r.name);
      if (t == nullptr) internal("table " + q(r.name) + " not in catalog");
      if (t->kind == TableKind::View) {
        refuse("SELECT from view " + q(t->name));
      }
      if (t->sys == SystemClass::IoWrite) {
        internal(q(t->name) + " accepts INSERT only");
      }
      if (t->sys == SystemClass::IoRead) {
        sk = Sk::IoRead;
        sys_table = t;
        Binding b;
        b.key = src_key;
        b.cols.push_back({"port", ColSrc{false, "", -1, "v_port", ""}});
        b.cols.push_back({"value", ColSrc{false, "", -1, "v_value", ""}});
        sc.bindings.push_back(std::move(b));
      } else if (t->sys == SystemClass::Memory) {
        sk = Sk::Memory;
        sys_table = t;
        mem_img = lower(t->name) == "volatile_memory"
                      ? "sqlos::g_volatile_memory"
                      : "sqlos::g_memory";
        Binding b;
        b.key = src_key;
        b.cols.push_back({"address", ColSrc{false, "", -1, "v_addr", ""}});
        b.cols.push_back({"value", ColSrc{false, "", -1, "v_value", ""}});
        sc.bindings.push_back(std::move(b));
      } else {
        const TableCG* cg = cg_for(t->name);
        if (cg == nullptr) internal("no storage for " + q(t->name));
        Binding b;
        b.key = src_key;
        const std::string loop = "i" + std::to_string(next_id()) + "_";
        for (const ColInfo& col : cg->columns) {
          b.cols.push_back(
              {col.name, ColSrc{true, cg->storage, col.idx, "", loop}});
        }
        sc.bindings.push_back(std::move(b));
        scan_var = cg->storage;
        scan_loop = loop;
        sk = Sk::Scan;
      }
    }

    // FROM x AS y (a, b) renames the visible column names positionally.
    if (!r.column_aliases.empty() && !sc.bindings.empty()) {
      Binding& b = sc.bindings.back();
      for (std::size_t k = 0;
           k < r.column_aliases.size() && k < b.cols.size(); ++k) {
        b.cols[k].first = lower(r.column_aliases[k]);
      }
    }
  }

  // --- WHERE ---------------------------------------------------------------
  std::string cond;
  if (sk == Sk::IoRead) {
    const HwContract c = extract_contract(s.where.get(), "port", false);
    if (!c.found || c.value == nullptr) {
      refuse("io read tables support only WHERE port = <expr>");
    }
    if (expr_has_column(*c.value)) {
      refuse("io read tables support only WHERE port = <expr>");
    }
    const std::string pe = gen_expr(*c.value, sc);
    p.head += "{\n";
    p.head += "  sqlos::Value vp_ = " + pe + ";\n";
    p.head += "  if (!vp_.is_null()) {\n";
    p.head += "    sqlos::u16 port_ = sqlos::io_port(vp_);\n";
    p.head += "    sqlos::Value v_port = sqlos::Value::i((sqlos::i64)port_);\n";
    const int w = sys_table->io_width;
    const std::string in =
        w == 8 ? "sqlos::inb(port_)" : (w == 16 ? "sqlos::inw(port_)" : "sqlos::inl(port_)");
    p.head += "    sqlos::Value v_value = sqlos::Value::i((sqlos::i64)" + in +
              ");\n";
    p.head += "    (void)v_port; (void)v_value;\n";  // may be unread
    p.tail = "  }\n}\n";
    cond = cond_code(c.rest, sc);
  } else if (sk == Sk::Memory) {
    const HwContract c = extract_contract(s.where.get(), "address", true);
    if (!c.found) {
      refuse(
          "memory tables support only WHERE address = <expr> or address "
          "BETWEEN low AND high");
    }
    if (c.value != nullptr) {
      if (expr_has_column(*c.value)) {
        refuse(
            "memory tables support only WHERE address = <expr> or address "
            "BETWEEN low AND high");
      }
      p.head += "{\n";
      p.head += "  sqlos::Value va_ = " + gen_expr(*c.value, sc) + ";\n";
      p.head += "  if (!va_.is_null()) {\n";
      p.head += "    sqlos::i64 addr_ = sqlos::mem_addr(va_);\n";
      p.head += "    sqlos::Value v_addr = sqlos::Value::i(addr_);\n";
      p.head += "    sqlos::Value v_value = sqlos::Value::i((sqlos::i64)" +
                mem_img + "[sqlos::mem_idx(addr_)]);\n";
      p.head += "    (void)v_addr; (void)v_value;\n";  // may be unread
      p.tail = "  }\n}\n";
    } else {
      p.head += "{\n";
      p.head += "  sqlos::Value lo_ = " + gen_expr(*c.low, sc) + ";\n";
      p.head += "  sqlos::Value hi_ = " + gen_expr(*c.high, sc) + ";\n";
      p.head += "  if (!lo_.is_null() && !hi_.is_null()) {\n";
      p.head += "    sqlos::i64 lo2_ = sqlos::mem_addr(lo_);\n";
      p.head += "    sqlos::i64 hi2_ = sqlos::mem_addr(hi_);\n";
      p.head += "    for (sqlos::i64 a_ = lo2_; a_ <= hi2_; ++a_) {\n";
      p.head += "      sqlos::Value v_addr = sqlos::Value::i(a_);\n";
      p.head += "      sqlos::Value v_value = sqlos::Value::i((sqlos::i64)" +
                mem_img + "[sqlos::mem_idx(a_)]);\n";
      p.head += "      (void)v_addr; (void)v_value;\n";  // may be unread
      p.tail = "    }\n  }\n}\n";
    }
    cond = cond_code(c.rest, sc);
  } else if (s.where) {
    cond = cond_code({s.where.get()}, sc);
  }

  if (sk == Sk::Scan && preset == nullptr) {
    p.loop_open = "for (sqlos::u32 " + scan_loop + " = 0; " + scan_loop +
                  " < " + scan_var + ".count; ++" + scan_loop + ") {\n";
  }
  if (!cond.empty()) p.cond_open = "if (" + cond + ") {\n";

  // --- select list ----------------------------------------------------------
  if (s.is_values) {
    if (s.where) refuse("WHERE with VALUES");
    if (!s.from.empty()) refuse("FROM with VALUES");
    if (s.value_rows.empty()) internal("VALUES without rows");
    const std::size_t width = s.value_rows[0].size();
    p.names.assign(width, "?column?");
    for (const std::vector<sql::Expr>& row : s.value_rows) {
      if (row.size() != width) internal("VALUES rows of different widths");
      std::vector<std::string> cells;
      for (const sql::Expr& e : row) cells.push_back(gen_expr(e, sc));
      if (cells.size() > 64) refuse("rows with more than 64 columns");
      p.rows.push_back(std::move(cells));
    }
    return p;
  }

  std::vector<std::string> cells;
  for (const sql::SelectItem& item : s.columns) {
    const sql::Expr& e = item.expr;
    if (e.kind != K::Star) {
      p.names.push_back(item.alias.empty() ? derive_name(e) : item.alias);
      cells.push_back(gen_expr(e, sc));
      continue;
    }
    const Binding* b = nullptr;
    if (e.parts.empty()) {
      if (sc.bindings.empty()) internal("asterisk without a FROM clause");
      b = &sc.bindings[0];
    } else {
      const std::string qual = lower(e.parts[0]);
      for (const Binding& bb : sc.bindings) {
        if (bb.key == qual) {
          b = &bb;
          break;
        }
      }
      if (b == nullptr) internal("unknown table " + q(e.parts[0]) + " for '*'");
    }
    for (const auto& col : b->cols) {
      p.names.push_back(col.first);
      cells.push_back(col_code(col.second));
    }
  }
  if (cells.size() > 64) refuse("rows with more than 64 columns");
  p.rows.push_back(std::move(cells));
  return p;
}

std::string Generator::gen_with(const sql::SelectStatement& s,
                                std::vector<std::string>* out_names) {
  if (s.with.size() > 1) refuse("multiple CTEs");
  const sql::CommonTableExpr& cte = s.with[0];
  if (!s.recursive) refuse("non-recursive CTE " + q(cte.name));
  if (cte.query == nullptr) internal("CTE " + q(cte.name) + " without a query");

  const sql::SelectStatement& inner = *cte.query;
  const sql::SelectStatement* anchor = &inner;
  const sql::SelectStatement* term = nullptr;
  if (!inner.set_op.empty()) {
    if (inner.set_op != "UNION" || !inner.set_all || inner.set_left == nullptr ||
        inner.set_right == nullptr) {
      refuse("recursive CTE written without UNION ALL");
    }
    anchor = inner.set_left.get();
    term = inner.set_right.get();
  }

  // Materialize the anchor (or a plain body under WITH RECURSIVE).
  const std::string app = "app" + std::to_string(next_id()) + "_";
  const SelectParts ap = plan_select(*anchor, Scope{}, nullptr);
  if (ap.rows.empty()) internal("CTE " + q(cte.name) + " has an empty anchor");
  const std::size_t width = ap.rows[0].size();

  std::vector<std::string> names = cte.columns;
  if (names.empty()) names = ap.names;
  if (names.size() != width) {
    internal("CTE " + q(cte.name) + " column count mismatch");
  }
  std::vector<ColInfo> cols;
  for (std::size_t i = 0; i < names.size(); ++i) {
    ColInfo ci;
    ci.name = lower(names[i]);
    ci.idx = static_cast<int>(i);
    ci.not_null = false;
    cols.push_back(ci);
  }

  const std::string var =
      make_storage("w" + std::to_string(stmt_index_) + "_" + lower(cte.name));
  worklists_.push_back({var, static_cast<int>(width)});

  // Shared append sink: anchor and recursive term both push rows.
  std::string append;
  append += "if (" + var + ".count >= SQLOS_TABLE_CAP) sqlos::trap();\n";
  for (std::size_t k = 0; k < width; ++k) {
    append += var + ".c" + std::to_string(k) + "[" + var +
              ".count] = r_->cells[" + std::to_string(k) + "];\n";
  }
  append += "++" + var + ".count;\n";

  std::string code;
  code += "{\n";
  code += "  sqlos::Sink " + app + ";\n";
  code += "  " + app + ".fn = [](void* c_, const sqlos::Row* r_) {\n";
  code += "    (void)c_;\n";
  code += indent(append, "    ");
  code += "  };\n";
  code += "  " + app + ".hdr = nullptr;\n";
  code += "  " + app + ".ctx = nullptr;\n";
  code += render(ap, app);
  // The block stays open: the recursive term below shares the same sink.

  if (term != nullptr) {
    if (term->from.size() != 1) {
      refuse("recursive term must scan CTE " + q(cte.name));
    }
    const sql::TableRef& tref = term->from[0];
    if (tref.kind != sql::TableRef::Kind::Table) {
      refuse("JOIN or subquery in recursive term");
    }
    if (lower(tref.name) != lower(cte.name)) {
      refuse("recursive term must scan CTE " + q(cte.name));
    }

    const std::string head = "head" + std::to_string(next_id()) + "_";
    const std::string cur = "cur" + std::to_string(next_id()) + "_";
    Binding preset;
    preset.key = lower(tref.alias.empty() ? tref.name : tref.alias);
    for (const ColInfo& col : cols) {
      preset.cols.push_back({col.name, ColSrc{true, var, col.idx, "", cur}});
    }
    for (std::size_t k = 0;
         k < tref.column_aliases.size() && k < preset.cols.size(); ++k) {
      preset.cols[k].first = lower(tref.column_aliases[k]);
    }

    code += "{\n";
    code += "  sqlos::u32 " + head + " = 0;\n";
    code += "  while (" + head + " < " + var + ".count) {\n";
    code += "    sqlos::u32 " + cur + " = " + head + ";\n";
    code += "    ++" + head + ";\n";
    code += render(plan_select(*term, Scope{}, &preset), app);
    code += "  }\n";
    code += "}\n";
  }
  code += "}\n";  // end the anchor/term block: the sink dies here

  // The outer query scans the materialized worklist.
  Scope out;
  CteInfo ci;
  ci.key = lower(cte.name);
  ci.storage = var;
  ci.cols = cols;
  out.ctes.push_back(ci);
  sql::SelectStatement outer_s = s;  // copy: plan without the WITH clause
  outer_s.with.clear();
  const SelectParts outer_parts = plan_select(outer_s, out, nullptr);
  if (out_names != nullptr) *out_names = outer_parts.names;
  code += render(outer_parts, "sink");
  return code;
}

// ------------------------------------------------------------ INSERT --------

std::string Generator::store_row_code(const std::string& storage,
                                      const std::vector<EffCol>& eff,
                                      const std::vector<EffCol>& rest,
                                      const std::vector<std::string>& cells) {
  if (cells.size() != eff.size()) internal("INSERT cell/column mismatch");
  std::string c;
  c += "if (" + storage + ".count >= SQLOS_TABLE_CAP) sqlos::trap();\n";
  for (std::size_t k = 0; k < eff.size(); ++k) {
    const std::string slot = storage + ".c" + std::to_string(eff[k].field) +
                             "[" + storage + ".count]";
    c += slot + " = " + cells[k] + ";\n";
    if (eff[k].not_null) c += "if (" + slot + ".is_null()) sqlos::trap();\n";
  }
  for (const EffCol& e : rest) {
    const std::string slot =
        storage + ".c" + std::to_string(e.field) + "[" + storage + ".count]";
    c += slot + " = sqlos::Value::null();\n";
    if (e.not_null) c += "if (" + slot + ".is_null()) sqlos::trap();\n";
  }
  c += "++" + storage + ".count;\n";
  return c;
}

std::string Generator::sys_row_code(const Table* t,
                                    const std::vector<EffCol>& eff,
                                    const std::vector<std::string>& cells) {
  if (cells.size() != eff.size()) internal("INSERT cell/column mismatch");
  int pi = -1, vi = -1, ai = -1;
  for (std::size_t k = 0; k < eff.size(); ++k) {
    if (eff[k].sys_col == "port") pi = static_cast<int>(k);
    if (eff[k].sys_col == "value") vi = static_cast<int>(k);
    if (eff[k].sys_col == "address") ai = static_cast<int>(k);
  }
  if (t->sys == SystemClass::IoWrite) {
    if (pi < 0 || vi < 0) internal(q(t->name) + " INSERT without port/value");
    const int w = t->io_width;
    const std::string fn =
        w == 8 ? "sqlos::outb" : (w == 16 ? "sqlos::outw" : "sqlos::outl");
    const std::string cast =
        w == 8 ? "sqlos::u8" : (w == 16 ? "sqlos::u16" : "sqlos::u32");
    return fn + "(sqlos::io_port(" + cells[static_cast<std::size_t>(pi)] +
           "), (" + cast + ")sqlos::io_value(" +
           cells[static_cast<std::size_t>(vi)] + ", " + std::to_string(w) +
           "));\n";
  }
  if (t->sys == SystemClass::Memory) {
    if (ai < 0 || vi < 0) internal(q(t->name) + " INSERT without address/value");
    const std::string img = lower(t->name) == "volatile_memory"
                                ? "sqlos::g_volatile_memory"
                                : "sqlos::g_memory";
    return img + "[sqlos::mem_idx(sqlos::mem_addr(" +
           cells[static_cast<std::size_t>(ai)] + "))] = sqlos::mem_byte(" +
           cells[static_cast<std::size_t>(vi)] + ");\n";
  }
  internal("system row code for a user table");
}

std::string Generator::gen_insert(const sql::InsertStatement& s) {
  if (!s.with.empty()) refuse("WITH clause on INSERT");
  if (!s.insert_or.empty()) refuse("INSERT OR " + s.insert_or);
  if (s.upsert != nullptr) refuse("ON CONFLICT");
  if (!s.returning.empty()) refuse("RETURNING");

  const Table* t = catalog_.find(s.table);
  if (t == nullptr) internal("unknown table " + q(s.table));
  if (t->kind == TableKind::View) internal("INSERT into view " + q(s.table));
  if (t->sys == SystemClass::IoRead) {
    internal(q(t->name) + " accepts SELECT only");
  }
  if (s.default_values && t->system()) {
    refuse("DEFAULT VALUES on system tables");
  }
  if (!s.rows.empty() && s.select != nullptr) {
    internal("INSERT with both VALUES and SELECT");
  }

  // Effective columns (mirrors the validator's list).
  std::vector<EffCol> eff;
  std::vector<EffCol> rest;
  if (t->system()) {
    if (!s.columns.empty()) {
      for (const std::string& name : s.columns) {
        bool known = false;
        for (const Column& c : t->columns) {
          if (lower(c.name) == lower(name)) {
            known = true;
            break;
          }
        }
        if (!known) internal("unknown column " + q(name));
        EffCol e;
        e.sys_col = lower(name);
        eff.push_back(e);
      }
    } else {
      for (const Column& c : t->columns) {
        EffCol e;
        e.sys_col = lower(c.name);
        eff.push_back(e);
      }
    }
    bool has_port = false, has_value = false, has_address = false;
    for (const EffCol& e : eff) {
      if (e.sys_col == "port") has_port = true;
      if (e.sys_col == "value") has_value = true;
      if (e.sys_col == "address") has_address = true;
    }
    if (t->sys == SystemClass::IoWrite && !(has_port && has_value)) {
      internal(q(t->name) + " INSERT must specify both port and value");
    }
    if (t->sys == SystemClass::Memory && !(has_address && has_value)) {
      internal(q(t->name) + " INSERT must specify both address and value");
    }
  } else {
    const TableCG* cg = cg_for(s.table);
    if (cg == nullptr) internal("no storage for " + q(s.table));
    if (s.default_values) {
      for (const ColInfo& col : cg->columns) {
        EffCol e;
        e.field = col.idx;
        e.not_null = col.not_null;
        rest.push_back(e);
      }
    } else if (!s.columns.empty()) {
      std::set<int> listed;
      for (const std::string& name : s.columns) {
        const ColInfo* col = nullptr;
        for (const ColInfo& c : cg->columns) {
          if (c.name == lower(name)) {
            col = &c;
            break;
          }
        }
        if (col == nullptr) internal("unknown column " + q(name));
        if (listed.count(col->idx)) {
          internal("duplicate column " + q(name) + " in INSERT");
        }
        listed.insert(col->idx);
        EffCol e;
        e.field = col->idx;
        e.not_null = col->not_null;
        eff.push_back(e);
      }
      for (const ColInfo& col : cg->columns) {
        if (listed.count(col.idx)) continue;
        EffCol e;
        e.field = col.idx;
        e.not_null = col.not_null;
        rest.push_back(e);
      }
    } else {
      for (const ColInfo& col : cg->columns) {
        EffCol e;
        e.field = col.idx;
        e.not_null = col.not_null;
        eff.push_back(e);
      }
    }
  }

  const std::string storage = t->system() ? std::string()
                                          : cg_for(s.table)->storage;
  auto emit_one = [&](const std::vector<std::string>& cells) -> std::string {
    if (t->system()) return sys_row_code(t, eff, cells);
    return store_row_code(storage, eff, rest, cells);
  };

  std::string body;
  if (s.select != nullptr) {
    const std::string app = "app" + std::to_string(next_id()) + "_";
    std::vector<std::string> out_names;
    const std::string core =
        gen_select_core(*s.select, Scope{}, app, nullptr, &out_names);
    if (out_names.size() != eff.size()) {
      internal("INSERT ... SELECT returns " + std::to_string(out_names.size()) +
               " column(s) but " + std::to_string(eff.size()) + " targeted");
    }
    std::vector<std::string> cells;
    for (std::size_t k = 0; k < eff.size(); ++k) {
      cells.push_back("r_->cells[" + std::to_string(k) + "]");
    }
    body += "{\n";
    body += "  sqlos::Sink " + app + ";\n";
    body += "  " + app + ".fn = [](void* c_, const sqlos::Row* r_) {\n";
    body += "    (void)c_;\n";
    body += indent(emit_one(cells), "    ");
    body += "  };\n";
    body += "  " + app + ".hdr = nullptr;\n";
    body += "  " + app + ".ctx = nullptr;\n";
    body += core;
    body += "}\n";
  } else if (s.default_values) {
    body += emit_one({});
  } else {
    if (s.rows.empty()) internal("INSERT without rows");
    for (const std::vector<sql::Expr>& row : s.rows) {
      if (row.size() != eff.size()) {
        internal("INSERT row has " + std::to_string(row.size()) +
                 " value(s) but " + std::to_string(eff.size()) +
                 " column(s) are targeted");
      }
      std::vector<std::string> cells;
      for (const sql::Expr& cell : row) cells.push_back(gen_expr(cell, Scope{}));
      body += emit_one(cells);
    }
  }
  return body;
}

// ------------------------------------------------------------ UPDATE --------

std::string Generator::gen_update(const sql::UpdateStatement& s) {
  if (!s.with.empty()) refuse("WITH clause on UPDATE");
  if (!s.returning.empty()) refuse("RETURNING");
  if (!s.from.empty()) refuse("UPDATE ... FROM");

  const Table* t = catalog_.find(s.table);
  if (t == nullptr) internal("unknown table " + q(s.table));
  if (t->kind == TableKind::View) internal("UPDATE on view " + q(s.table));
  if (t->sys == SystemClass::IoRead || t->sys == SystemClass::IoWrite) {
    internal(q(t->name) + " cannot be updated");
  }
  const std::string rel_key = lower(s.alias.empty() ? s.table : s.alias);

  if (t->sys == SystemClass::Memory) {
    // Only `SET value = ... WHERE address = <expr>` compiles.
    const HwContract c = extract_contract(s.where.get(), "address", false);
    if (!c.found || c.value == nullptr || expr_has_column(*c.value)) {
      refuse("UPDATE on memory supports only WHERE address = <expr>");
    }
    const std::string img = lower(t->name) == "volatile_memory"
                                ? "sqlos::g_volatile_memory"
                                : "sqlos::g_memory";

    Scope sc;
    Binding b;
    b.key = rel_key;
    b.cols.push_back({"address", ColSrc{false, "", -1, "v_addr", ""}});
    b.cols.push_back({"value", ColSrc{false, "", -1, "v_value", ""}});
    sc.bindings.push_back(b);

    const std::string addr_expr = gen_expr(*c.value, sc);
    const sql::Expr* set_value = nullptr;
    for (const auto& asg : s.assignments) {
      if (lower(asg.first) == "value" && asg.second) {
        set_value = asg.second.get();
      }
    }
    if (set_value == nullptr) internal("memory UPDATE without SET value");

    std::string code;
    code += "{\n";
    code += "  sqlos::Value va_ = " + addr_expr + ";\n";
    code += "  if (!va_.is_null()) {\n";
    code += "    sqlos::i64 addr_ = sqlos::mem_addr(va_);\n";
    code += "    sqlos::Value v_addr = sqlos::Value::i(addr_);\n";
    code += "    sqlos::Value v_value = sqlos::Value::i((sqlos::i64)" + img +
            "[sqlos::mem_idx(addr_)]);\n";
    code += "    (void)v_addr; (void)v_value;\n";  // may be unread
    const std::string cond = cond_code(c.rest, sc);
    if (!cond.empty()) code += "    if (" + cond + ") {\n";
    code += "    " + img + "[sqlos::mem_idx(addr_)] = sqlos::mem_byte(" +
            gen_expr(*set_value, sc) + ");\n";
    if (!cond.empty()) code += "    }\n";
    code += "  }\n";
    code += "}\n";
    return code;
  }

  const TableCG* cg = cg_for(s.table);
  if (cg == nullptr) internal("no storage for " + q(s.table));
  if (s.assignments.empty()) internal("UPDATE without assignments");

  Scope sc;
  Binding b;
  b.key = rel_key;
  const std::string loop = "i" + std::to_string(next_id()) + "_";
  for (const ColInfo& col : cg->columns) {
    b.cols.push_back({col.name, ColSrc{true, cg->storage, col.idx, "", loop}});
  }
  sc.bindings.push_back(b);

  std::string code;
  code += "for (sqlos::u32 " + loop + " = 0; " + loop + " < " + cg->storage +
          ".count; ++" + loop + ") {\n";
  const std::string cond =
      s.where ? cond_code({s.where.get()}, sc) : std::string();
  if (!cond.empty()) code += "  if (" + cond + ") {\n";

  // Assignments are simultaneous: evaluate every right-hand side first.
  std::vector<int> fields;
  std::vector<std::string> temps;
  std::vector<bool> not_nulls;
  for (std::size_t i = 0; i < s.assignments.size(); ++i) {
    const auto& asg = s.assignments[i];
    if (!asg.second) internal("UPDATE assignment without an expression");
    if (asg.second->kind == K::Default) refuse("DEFAULT");
    const ColInfo* col = nullptr;
    for (const ColInfo& c : cg->columns) {
      if (c.name == lower(asg.first)) {
        col = &c;
        break;
      }
    }
    if (col == nullptr) internal("unknown column " + q(asg.first));
    const std::string tmp = "u" + std::to_string(i) + "_";
    code += "  sqlos::Value " + tmp + " = " + gen_expr(*asg.second, sc) + ";\n";
    fields.push_back(col->idx);
    temps.push_back(tmp);
    not_nulls.push_back(col->not_null);
  }
  for (std::size_t k = 0; k < temps.size(); ++k) {
    if (not_nulls[k]) {
      code += "  if (" + temps[k] + ".is_null()) sqlos::trap();\n";
    }
    code += "  " + cg->storage + ".c" + std::to_string(fields[k]) + "[" + loop +
            "] = " + temps[k] + ";\n";
  }
  if (!cond.empty()) code += "}\n";
  code += "}\n";
  return code;
}

// ------------------------------------------------------------ DELETE --------

std::string Generator::gen_delete(const sql::DeleteStatement& s) {
  if (!s.with.empty()) refuse("WITH clause on DELETE");
  if (!s.returning.empty()) refuse("RETURNING");

  const Table* t = catalog_.find(s.table);
  if (t == nullptr) internal("unknown table " + q(s.table));
  if (t->kind == TableKind::View) internal("DELETE from view " + q(s.table));
  if (t->system()) internal(q(t->name) + " does not support DELETE");

  const TableCG* cg = cg_for(s.table);
  if (cg == nullptr) internal("no storage for " + q(s.table));
  const std::string rel_key = lower(s.alias.empty() ? s.table : s.alias);

  if (!s.where) return cg->storage + ".count = 0;\n";

  Scope sc;
  Binding b;
  b.key = rel_key;
  const std::string loop = "i" + std::to_string(next_id()) + "_";
  for (const ColInfo& col : cg->columns) {
    b.cols.push_back({col.name, ColSrc{true, cg->storage, col.idx, "", loop}});
  }
  sc.bindings.push_back(b);
  const std::string cond = cond_code({s.where.get()}, sc);

  // Stable compaction: keep=0 counts the surviving prefix.
  std::string code;
  code += "sqlos::u32 keep_ = 0;\n";
  code += "for (sqlos::u32 " + loop + " = 0; " + loop + " < " + cg->storage +
          ".count; ++" + loop + ") {\n";
  code += "  if (" + cond + ") continue;\n";
  code += "  if (" + loop + " != keep_) {\n";
  for (const ColInfo& col : cg->columns) {
    const std::string f = cg->storage + ".c" + std::to_string(col.idx);
    code += "    " + f + "[keep_] = " + f + "[" + loop + "];\n";
  }
  code += "  }\n";
  code += "  ++keep_;\n";
  code += "}\n";
  code += cg->storage + ".count = keep_;\n";
  return code;
}

// -------------------------------------------------------- expressions --------

std::string Generator::resolve_column(const sql::Expr& e, const Scope& sc) {
  if (e.parts.empty()) internal("column reference without a name");
  const std::string name = lower(e.parts.back());
  if (e.parts.size() >= 2) {
    const std::string qual = lower(e.parts[e.parts.size() - 2]);
    for (const Binding& b : sc.bindings) {
      if (b.key != qual) continue;
      for (const auto& col : b.cols) {
        if (col.first == name) return col_code(col.second);
      }
      internal("column " + join_dot(e.parts) + " does not exist in " + q(qual));
    }
    internal("unknown qualifier " + q(qual) + " in " + join_dot(e.parts));
  }
  const ColSrc* found = nullptr;
  int matches = 0;
  for (const Binding& b : sc.bindings) {
    for (const auto& col : b.cols) {
      if (col.first != name) continue;
      found = &col.second;
      ++matches;
    }
  }
  if (matches == 0) internal("column " + name + " did not resolve");
  if (matches > 1) internal("column " + name + " is ambiguous");
  return col_code(*found);
}

std::string Generator::gen_binary(const sql::Expr& e, const Scope& sc) {
  if (e.args.size() < 2 || !e.args[0] || !e.args[1]) {
    internal("binary operator without operands");
  }
  const std::string op = e.name;
  const std::string a = gen_expr(*e.args[0], sc);
  const std::string b = gen_expr(*e.args[1], sc);
  std::string fn;
  if (op == "=") {
    fn = "v_eq";
  } else if (op == "<>" || op == "!=") {
    fn = "v_neq";
  } else if (op == "<") {
    fn = "v_lt";
  } else if (op == "<=") {
    fn = "v_le";
  } else if (op == ">") {
    fn = "v_gt";
  } else if (op == ">=") {
    fn = "v_ge";
  } else if (upper(op) == "AND") {
    fn = "v_and";
  } else if (upper(op) == "OR") {
    fn = "v_or";
  } else if (op == "||") {
    fn = "v_cat";
  } else if (op == "+") {
    fn = "v_add";
  } else if (op == "-") {
    fn = "v_sub";
  } else if (op == "*") {
    fn = "v_mul";
  } else if (op == "/") {
    fn = "v_div";
  } else if (op == "%") {
    fn = "v_mod";
  } else if (op == "^") {
    fn = "v_pow";
  } else if (op == "&") {
    fn = "v_band";
  } else if (op == "|") {
    fn = "v_bor";
  } else if (op == "<<") {
    fn = "v_shl";
  } else if (op == ">>") {
    fn = "v_shr";
  } else {
    refuse("operator " + q(e.name));
  }
  return "sqlos::" + fn + "(" + a + ", " + b + ")";
}

std::string Generator::gen_case(const sql::Expr& e, const Scope& sc) {
  if (e.args.size() != e.results.size()) internal("CASE branch mismatch");
  std::string chain =
      e.else_result ? gen_expr(*e.else_result, sc) : "sqlos::Value::null()";

  if (e.operand) {
    // Simple CASE: evaluate the subject exactly once.
    const int id = next_id();
    const std::string op = "cs" + std::to_string(id) + "_";
    for (int i = static_cast<int>(e.args.size()) - 1; i >= 0; --i) {
      const std::size_t k = static_cast<std::size_t>(i);
      const std::string w = gen_expr(*e.args[k], sc);
      const std::string r = gen_expr(*e.results[k], sc);
      chain = "((sqlos::truth(sqlos::v_eq(" + op + ", " + w + "))) ? (" + r +
              ") : (" + chain + "))";
    }
    return "[&]() { sqlos::Value " + op + " = " + gen_expr(*e.operand, sc) +
           "; return " + chain + "; }()";
  }
  for (int i = static_cast<int>(e.args.size()) - 1; i >= 0; --i) {
    const std::size_t k = static_cast<std::size_t>(i);
    const std::string w = gen_expr(*e.args[k], sc);
    const std::string r = gen_expr(*e.results[k], sc);
    chain = "((sqlos::truth(" + w + ")) ? (" + r + ") : (" + chain + "))";
  }
  return chain;
}

std::string Generator::gen_call(const sql::Expr& e, const Scope& sc) {
  if (e.parts.empty()) internal("unnamed function call");
  const std::string fn = lower(e.parts.back());
  if (e.over != nullptr) refuse("window function");
  if (fn == "count" || fn == "sum" || fn == "avg" || fn == "min" ||
      fn == "max") {
    refuse("aggregate function " + q(fn));
  }
  if (e.star) refuse("star in expression");

  std::vector<std::string> args;
  for (const sql::ExprPtr& a : e.args) {
    if (a) args.push_back(gen_expr(*a, sc));
  }
  auto arity = [&](std::size_t want) {
    if (args.size() != want) {
      internal("function " + q(fn) + " with " + std::to_string(args.size()) +
               " argument(s)");
    }
  };

  if (fn == "length") {
    arity(1);
    return "sqlos::fn_length(" + args[0] + ")";
  }
  if (fn == "lower") {
    arity(1);
    return "sqlos::fn_lower(" + args[0] + ")";
  }
  if (fn == "upper") {
    arity(1);
    return "sqlos::fn_upper(" + args[0] + ")";
  }
  if (fn == "substr" || fn == "substring") {
    if (args.size() == 2) return "sqlos::fn_substr(" + args[0] + ", " + args[1] + ")";
    if (args.size() == 3) {
      return "sqlos::fn_substr(" + args[0] + ", " + args[1] + ", " + args[2] +
             ")";
    }
    internal("function " + q(fn) + " with " + std::to_string(args.size()) +
             " argument(s)");
  }
  if (fn == "trim") {
    arity(1);
    return "sqlos::fn_trim(" + args[0] + ")";
  }
  if (fn == "ltrim") {
    arity(1);
    return "sqlos::fn_ltrim(" + args[0] + ")";
  }
  if (fn == "rtrim") {
    arity(1);
    return "sqlos::fn_rtrim(" + args[0] + ")";
  }
  if (fn == "replace") {
    arity(3);
    return "sqlos::fn_replace(" + args[0] + ", " + args[1] + ", " + args[2] +
           ")";
  }
  if (fn == "round") {
    if (args.size() == 1) return "sqlos::fn_round(" + args[0] + ")";
    if (args.size() == 2) {
      return "sqlos::fn_round(" + args[0] + ", " + args[1] + ")";
    }
    internal("function " + q(fn) + " with " + std::to_string(args.size()) +
             " argument(s)");
  }
  if (fn == "floor") {
    arity(1);
    return "sqlos::fn_floor(" + args[0] + ")";
  }
  if (fn == "ceil" || fn == "ceiling") {
    arity(1);
    return "sqlos::fn_ceil(" + args[0] + ")";
  }
  if (fn == "abs") {
    arity(1);
    return "sqlos::fn_abs(" + args[0] + ")";
  }
  if (fn == "nullif") {
    arity(2);
    return "sqlos::fn_nullif(" + args[0] + ", " + args[1] + ")";
  }
  if (fn == "hex") {
    arity(1);
    return "sqlos::fn_hex(" + args[0] + ")";
  }
  if (fn == "coalesce") {
    if (args.size() < 2) {
      internal("function 'coalesce' with " + std::to_string(args.size()) +
               " argument(s)");
    }
    std::string code = args.back();
    for (int i = static_cast<int>(args.size()) - 2; i >= 0; --i) {
      code = "sqlos::v_coalesce2(" + args[static_cast<std::size_t>(i)] + ", " +
             code + ")";
    }
    return code;
  }
  refuse("function " + q(join_dot(e.parts)));
}

std::string Generator::gen_expr(const sql::Expr& e, const Scope& sc) {
  switch (e.kind) {
    case K::Literal: {
      if (std::holds_alternative<std::nullptr_t>(e.literal)) {
        return "sqlos::Value::null()";
      }
      if (const auto* n = std::get_if<long long>(&e.literal)) {
        return "sqlos::Value::i(" + std::to_string(*n) + "LL)";
      }
      if (const auto* d = std::get_if<double>(&e.literal)) {
        return "sqlos::Value::r(" + fmt_double(*d) + ")";
      }
      if (const auto* t = std::get_if<std::string>(&e.literal)) {
        return "sqlos::Value::t(" + c_string(*t) + ", " +
               std::to_string(t->size()) + ")";
      }
      if (const auto* b = std::get_if<bool>(&e.literal)) {
        return "sqlos::Value::b(" + bool_lit(*b) + ")";
      }
      internal("unknown literal");
    }
    case K::Parameter:
      refuse("bind parameter");
    case K::ColumnRef:
      return resolve_column(e, sc);
    case K::Star:
      refuse("star in expression");
    case K::Unary: {
      if (e.args.empty() || !e.args[0]) internal("unary without an operand");
      const std::string a = gen_expr(*e.args[0], sc);
      if (e.name == "NOT") return "sqlos::v_not(" + a + ")";
      if (e.name == "-") return "sqlos::v_neg(" + a + ")";
      if (e.name == "+") return "(" + a + ")";
      if (e.name == "~") return "sqlos::v_bitnot(" + a + ")";
      refuse("unary operator " + q(e.name));
    }
    case K::Binary:
      return gen_binary(e, sc);
    case K::NullTest: {
      if (e.args.empty() || !e.args[0]) internal("IS NULL without an operand");
      const std::string a = gen_expr(*e.args[0], sc);
      std::string test = "sqlos::v_is_null(" + a + ")";
      if (e.negated) test = "!(" + test + ")";
      return "sqlos::Value::b(" + test + ")";
    }
    case K::BoolTest: {
      if (e.args.empty() || !e.args[0]) internal("IS test without an operand");
      const std::string a = gen_expr(*e.args[0], sc);
      const std::string kw = upper(e.name);
      std::string test;
      if (kw == "TRUE") {
        test = "sqlos::v_is_true(" + a + ")";
      } else if (kw == "FALSE") {
        test = "sqlos::v_is_false(" + a + ")";
      } else if (kw == "UNKNOWN") {
        test = "sqlos::v_is_null(" + a + ")";
      } else {
        internal("unknown IS test " + q(e.name));
      }
      if (e.negated) test = "!(" + test + ")";
      return "sqlos::Value::b(" + test + ")";
    }
    case K::In: {
      if (e.subquery != nullptr) refuse("IN (subquery)");
      if (e.args.empty() || !e.args[0]) internal("IN without an operand");
      const std::string x = gen_expr(*e.args[0], sc);
      std::vector<std::string> items;
      for (std::size_t i = 1; i < e.args.size(); ++i) {
        const sql::Expr& m = *e.args[i];
        if (m.kind == K::Subquery || m.subquery) refuse("IN (subquery)");
        items.push_back(gen_expr(m, sc));
      }
      if (items.empty()) internal("IN without choices");
      const std::string arr = "vs" + std::to_string(next_id()) + "_";
      std::string code = "[&]() { const sqlos::Value " + arr + "[] = {";
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) code += ", ";
        code += items[i];
      }
      code += "}; return sqlos::v_in(" + x + ", " + arr + ", " +
              std::to_string(items.size()) + "); }()";
      if (e.negated) code = "sqlos::v_not(" + code + ")";
      return code;
    }
    case K::Like: {
      if (e.args.size() < 2 || !e.args[0] || !e.args[1]) {
        internal("LIKE without operands");
      }
      const std::string kw = upper(e.name);
      if (kw == "GLOB") refuse("GLOB");
      if (kw != "LIKE" && kw != "ILIKE") refuse("operator " + q(e.name));
      const bool icase = kw == "ILIKE";
      const std::string s0 = gen_expr(*e.args[0], sc);
      const std::string p0 = gen_expr(*e.args[1], sc);
      std::string code;
      if (e.args.size() > 2 && e.args[2]) {
        code = "sqlos::v_like_esc(" + s0 + ", " + p0 + ", " +
               gen_expr(*e.args[2], sc) + ", " + bool_lit(icase) + ")";
      } else {
        code = "sqlos::v_like(" + s0 + ", " + p0 + ", " + bool_lit(icase) + ")";
      }
      if (e.negated) code = "sqlos::v_not(" + code + ")";
      return code;
    }
    case K::Between: {
      if (e.args.size() < 3 || !e.args[0] || !e.args[1] || !e.args[2]) {
        internal("BETWEEN without bounds");
      }
      // Evaluate the operand exactly once.
      const std::string t = "bt" + std::to_string(next_id()) + "_";
      std::string code = "[&]() { sqlos::Value " + t + " = " +
                         gen_expr(*e.args[0], sc) + "; return sqlos::v_and(" +
                         "sqlos::v_ge(" + t + ", " + gen_expr(*e.args[1], sc) +
                         "), sqlos::v_le(" + t + ", " +
                         gen_expr(*e.args[2], sc) + ")); }()";
      if (e.negated) code = "sqlos::v_not(" + code + ")";
      return code;
    }
    case K::Exists:
      refuse("EXISTS");
    case K::Subquery:
      refuse("subquery in expression");
    case K::FunctionCall:
      return gen_call(e, sc);
    case K::Cast: {
      if (e.args.empty() || !e.args[0]) internal("CAST without an operand");
      const std::string a = gen_expr(*e.args[0], sc);
      Type t = Type::Unknown;
      if (!type_from_name(e.type_name, &t)) {
        refuse("CAST to unknown type " + q(e.type_name));
      }
      switch (t) {
        case Type::Int:
          return "sqlos::v_cast(" + a + ", sqlos::CType::Int)";
        case Type::Real:
          return "sqlos::v_cast(" + a + ", sqlos::CType::Real)";
        case Type::Text:
          return "sqlos::v_cast(" + a + ", sqlos::CType::Text)";
        case Type::Bool:
          return "sqlos::v_cast(" + a + ", sqlos::CType::Bool)";
        default:
          refuse("CAST to " + type_name(t));
      }
    }
    case K::Collate:
      refuse("COLLATE");
    case K::Case:
      return gen_case(e, sc);
    case K::Row:
      refuse("row constructor");
    case K::Subscript:
      refuse("subscript");
    case K::Quantified:
      refuse("quantified comparison");
    case K::Default:
      refuse("DEFAULT");
  }
  internal("unknown expression kind");
}

// ------------------------------------------------------------ assembly -------

std::string Generator::assemble() const {
  std::string out;
  out += "// Generated by sqlos --emit. Do not edit.\n";
  out += "// SQL-OS: SQL compiled ahead of time to C++; nothing interprets it.\n\n";
  out += "#include \"sqlos/runtime/sqlos_runtime.hpp\"\n\n";
  out += "namespace {\n\n";
  for (const auto& entry : storage_) {
    const TableCG& cg = entry.second;
    if (cg.view) continue;
    out += emit_struct(cg.storage, cg.field_count);
  }
  for (const auto& wl : worklists_) {
    out += emit_struct(wl.first, wl.second);
  }
  out += "\n";
  for (std::size_t i = 0; i < bodies_.size(); ++i) {
    out += "static void s" + std::to_string(i) + "(sqlos::Sink sink) {\n";
    out += "  (void)sink;\n";
    out += bodies_[i];
    out += "}\n\n";
  }
  out += "}  // namespace\n\n";
  out += "extern \"C\" void sqlos_program(sqlos::SinkFn emit, "
         "sqlos::HeaderFn header, void* user) {\n";
  out += "  sqlos::arena_reset();\n";
  out += "  sqlos::Sink sink;\n";
  out += "  sink.fn = emit;\n";
  out += "  sink.hdr = header;\n";
  out += "  sink.ctx = user;\n";
  if (bodies_.empty()) out += "  (void)sink;\n";
  for (std::size_t i = 0; i < bodies_.size(); ++i) {
    out += "  s" + std::to_string(i) + "(sink);\n";
  }
  out += "}\n";
  return out;
}

}  // namespace

CodegenResult generate(const std::vector<sql::Statement>& statements,
                       const std::vector<SourcePos>& positions) {
  Generator gen(positions);
  return gen.run(statements);
}

}  // namespace sqlos
