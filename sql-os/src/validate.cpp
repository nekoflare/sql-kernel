// The SQL validator. Runs over the parsed AST after syntax checking and
// reports, with source positions:
//   * catalog + arity   — unknown tables/columns, VALUES/SELECT/set-op/CTE
//                         widths, ambiguous references, DDL evolution
//   * system contracts  — io_*_read is SELECT-only, io_*_write is INSERT-only,
//                         memory allows SELECT/INSERT/UPDATE with address
//                         constraints, hardware port/address range checks
//   * types             — assignment compatibility, comparisons, predicates,
//                         recursive-CTE structure (PostgreSQL/SQLite rules)
//
// No code generation lives here: validate() never executes anything.

#include "sqlos/validate.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "sql_parser/error.hpp"
#include "sql_parser/lexer.hpp"
#include "sql_parser/parser.hpp"

namespace sqlos {
namespace {

// --- small utilities ---------------------------------------------------------

std::string lower(std::string_view word) {
  std::string out;
  out.reserve(word.size());
  for (char c : word) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool ci_eq(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

std::string q(const std::string& word) { return "'" + word + "'"; }

std::string join_dot(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += ".";
    out += parts[i];
  }
  return out;
}

// Integer constant, if the expression is one (unary signs are already folded
// into literals by the parser). Everything else — parameters, arithmetic —
// is checked at run time, not here.
std::optional<long long> const_int(const sql::Expr& e) {
  if (e.kind != sql::Expr::Kind::Literal) return std::nullopt;
  if (const auto* i = std::get_if<long long>(&e.literal)) return *i;
  return std::nullopt;
}

bool is_boolish(Type t) { return t == Type::Bool || t == Type::Unknown || t == Type::Null; }
bool is_numericish(Type t) {
  return t == Type::Int || t == Type::Real || t == Type::Unknown || t == Type::Null;
}
bool is_textish(Type t) {
  return t == Type::Text || t == Type::Unknown || t == Type::Null;
}

bool is_comparison(const std::string& op) {
  return op == "=" || op == "<>" || op == "!=" || op == "<" || op == ">" ||
         op == "<=" || op == ">=";
}

bool is_arithmetic(const std::string& op) {
  return op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
         op == "^";
}

std::string derive_name(const sql::Expr& e) {
  using K = sql::Expr::Kind;
  if (e.kind == K::ColumnRef && !e.parts.empty()) return e.parts.back();
  if (e.kind == K::FunctionCall && !e.parts.empty()) return e.parts.back();
  return "?column?";
}

// --- builtin functions -------------------------------------------------------

struct FnSpec {
  const char* name;
  int min_args;
  int max_args;  // -1 = unlimited
};

const FnSpec* find_builtin(const std::string& lower_name) {
  static const FnSpec kBuiltins[] = {
      {"count", 1, 1},   {"sum", 1, 1},     {"avg", 1, 1},
      {"min", 1, 1},     {"max", 1, 1},     {"abs", 1, 1},
      {"length", 1, 1},  {"lower", 1, 1},   {"upper", 1, 1},
      {"substr", 2, 3},  {"substring", 2, 3}, {"trim", 1, 1},
      {"ltrim", 1, 1},   {"rtrim", 1, 1},   {"replace", 3, 3},
      {"round", 1, 2},   {"floor", 1, 1},   {"ceil", 1, 1},
      {"ceiling", 1, 1}, {"coalesce", 2, -1}, {"nullif", 2, 2},
      {"hex", 1, 1},
  };
  for (const FnSpec& spec : kBuiltins) {
    if (lower_name == spec.name) return &spec;
  }
  return nullptr;
}

std::string arity_message(const std::string& display, const FnSpec& spec,
                          std::size_t got) {
  std::string expect;
  if (spec.min_args == spec.max_args) {
    expect = "exactly " + std::to_string(spec.min_args);
  } else if (spec.max_args < 0) {
    expect = "at least " + std::to_string(spec.min_args);
  } else {
    expect = std::to_string(spec.min_args) + " to " +
             std::to_string(spec.max_args);
  }
  const bool singular =
      spec.min_args == spec.max_args && spec.min_args == 1;
  return "function '" + display + "' expects " + expect +
         (singular ? " argument, got " : " arguments, got ") +
         std::to_string(got);
}

// --- recursive self-reference scan ------------------------------------------

struct SelfRefStats {
  int count = 0;
  bool in_outer = false;
  bool in_subquery = false;
};

bool join_is_outer(const std::string& join_type) {
  const std::string u = lower(join_type);
  return u.find("left") != std::string::npos ||
         u.find("right") != std::string::npos ||
         u.find("full") != std::string::npos;
}

void scan_expr_refs(const sql::Expr& e, const std::string& name,
                    bool in_outer, bool in_subquery, SelfRefStats* st);
void scan_select_refs(const sql::SelectStatement& s, const std::string& name,
                      bool in_outer, bool in_subquery, SelfRefStats* st);

void scan_from_refs(const sql::TableRef& ref, const std::string& name,
                    bool in_outer, bool in_subquery, SelfRefStats* st) {
  using K = sql::TableRef::Kind;
  switch (ref.kind) {
    case K::Table:
      if (ci_eq(ref.name, name)) {
        st->count++;
        if (in_outer) st->in_outer = true;
        if (in_subquery) st->in_subquery = true;
      }
      break;
    case K::Subquery:
      // A subquery is its own world: the outer-join context does not carry
      // in, but the reference is still inside a subquery.
      if (ref.subquery) {
        scan_select_refs(*ref.subquery, name, /*in_outer=*/false,
                         /*in_subquery=*/true, st);
      }
      break;
    case K::TableFunction:
      if (ref.function) {
        scan_expr_refs(*ref.function, name, in_outer, in_subquery, st);
      }
      break;
    case K::Join: {
      const bool outer = in_outer || join_is_outer(ref.join_type);
      if (ref.left) scan_from_refs(*ref.left, name, outer, in_subquery, st);
      if (ref.right) scan_from_refs(*ref.right, name, outer, in_subquery, st);
      if (ref.on) {
        scan_expr_refs(*ref.on, name, /*in_outer=*/false, in_subquery, st);
      }
      break;
    }
  }
}

void scan_expr_refs(const sql::Expr& e, const std::string& name,
                    bool in_outer, bool in_subquery, SelfRefStats* st) {
  if (e.subquery) {
    scan_select_refs(*e.subquery, name, /*in_outer=*/false,
                     /*in_subquery=*/true, st);
  }
  for (const sql::ExprPtr& arg : e.args) {
    if (arg) scan_expr_refs(*arg, name, in_outer, in_subquery, st);
  }
  if (e.operand) scan_expr_refs(*e.operand, name, in_outer, in_subquery, st);
  if (e.else_result) {
    scan_expr_refs(*e.else_result, name, in_outer, in_subquery, st);
  }
  for (const sql::ExprPtr& r : e.results) {
    if (r) scan_expr_refs(*r, name, in_outer, in_subquery, st);
  }
}

void scan_select_refs(const sql::SelectStatement& s, const std::string& name,
                      bool in_outer, bool in_subquery, SelfRefStats* st) {
  for (const sql::CommonTableExpr& cte : s.with) {
    if (cte.query) scan_select_refs(*cte.query, name, in_outer, in_subquery, st);
  }
  for (const sql::TableRef& ref : s.from) {
    scan_from_refs(ref, name, in_outer, in_subquery, st);
  }
  auto exprs = [&](const std::vector<sql::Expr>& list) {
    for (const sql::Expr& e : list) scan_expr_refs(e, name, in_outer, in_subquery, st);
  };
  for (const sql::SelectItem& item : s.columns) {
    scan_expr_refs(item.expr, name, in_outer, in_subquery, st);
  }
  exprs(s.distinct_on);
  exprs(s.group_by);
  for (const std::vector<sql::Expr>& row : s.value_rows) exprs(row);
  if (s.where) scan_expr_refs(*s.where, name, in_outer, in_subquery, st);
  if (s.having) scan_expr_refs(*s.having, name, in_outer, in_subquery, st);
  if (s.limit) scan_expr_refs(*s.limit, name, in_outer, in_subquery, st);
  if (s.offset) scan_expr_refs(*s.offset, name, in_outer, in_subquery, st);
  for (const sql::OrderByItem& item : s.order_by) {
    scan_expr_refs(item.expr, name, in_outer, in_subquery, st);
  }
  if (s.set_left) scan_select_refs(*s.set_left, name, in_outer, in_subquery, st);
  if (s.set_right) scan_select_refs(*s.set_right, name, in_outer, in_subquery, st);
}

bool select_references(const sql::SelectStatement& s, const std::string& name) {
  SelfRefStats st;
  scan_select_refs(s, name, /*in_outer=*/false, /*in_subquery=*/false, &st);
  return st.count > 0;
}

// --- WHERE constraint walking ------------------------------------------------

bool refers_to(const sql::Expr& e, const std::string& column) {
  return e.kind == sql::Expr::Kind::ColumnRef && !e.parts.empty() &&
         ci_eq(e.parts.back(), column);
}

// Calls fn(expr) for every expression that constrains `column` to an
// equality form:  column = expr / expr = column / column IN (...) /
// column BETWEEN a AND b. Recurses through AND/OR only.
template <typename Fn>
void walk_equalities(const sql::Expr& e, const std::string& column, Fn&& fn) {
  using K = sql::Expr::Kind;
  switch (e.kind) {
    case K::Binary:
      if (e.name == "AND" || e.name == "OR") {
        if (e.args.size() == 2) {
          walk_equalities(*e.args[0], column, fn);
          walk_equalities(*e.args[1], column, fn);
        }
      } else if (e.name == "=" && e.args.size() == 2) {
        if (refers_to(*e.args[0], column)) fn(*e.args[1]);
        else if (refers_to(*e.args[1], column)) fn(*e.args[0]);
      }
      break;
    case K::In:
      if (!e.args.empty() && refers_to(*e.args[0], column)) {
        for (std::size_t i = 1; i < e.args.size(); ++i) fn(*e.args[i]);
        if (e.subquery) fn(e);  // presence marker for IN (SELECT ...)
      }
      break;
    case K::Between:
      if (e.args.size() >= 3 && refers_to(*e.args[0], column)) {
        fn(*e.args[1]);
        fn(*e.args[2]);
      }
      break;
    default:
      break;
  }
}

bool where_constrains(const sql::Expr* where, const std::string& column) {
  if (!where) return false;
  bool found = false;
  walk_equalities(*where, column, [&](const sql::Expr&) { found = true; });
  return found;
}

std::vector<long long> where_const_equals(const sql::Expr* where,
                                          const std::string& column) {
  std::vector<long long> out;
  if (!where) return out;
  walk_equalities(*where, column, [&](const sql::Expr& e) {
    if (auto v = const_int(e)) out.push_back(*v);
  });
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------------

namespace {

class Validator {
 public:
  ValidationResult run(const std::vector<sql::Statement>& statements,
                        const std::vector<SourcePos>& positions);

 private:
  // --- issue reporting ------------------------------------------------------
  void error(const std::string& message) { issues_.push_back({pos_, message}); }

  std::vector<Issue> issues_;
  Catalog catalog_;
  SourcePos pos_{1, 1};
  bool allow_default_ = false;

  // --- scope model ----------------------------------------------------------
  struct Relation {
    std::string key;      // lower-case visible name ("" = unnamed)
    std::string display;  // source spelling, for messages
    std::vector<Column> columns;
    const Table* table = nullptr;  // backing catalog table (nullptr: CTE /
                                   // derived / output aliases)
    bool virtual_cte = false;  // CTE: usable in FROM, never directly
                               // referenceable
    bool output_alias = false; // GROUP/ORDER alias (bare names, lower
                               // precedence than real columns)
    bool excluded = false;     // upsert target row (qualified names only)
    bool opaque = false;       // table function: columns unknown
    bool broken = false;       // name failed to resolve → suppress cascades
  };

  struct Scope {
    std::vector<Relation> relations;
    bool has_broken = false;
  };

  struct Lookup {
    bool found = false;
    bool ambiguous = false;
    const Column* column = nullptr;
  };

  Lookup find_column(const Scope& scope, const std::string& qualifier,
                     const std::string& name) const;

  // --- expressions ----------------------------------------------------------
  Type expr(const sql::Expr& e, const Scope& scope);
  Type binary(const sql::Expr& e, const Scope& scope);
  Type check_function(const sql::Expr& e, const Scope& scope);
  void check_window(const sql::WindowSpec& w, const Scope& scope);
  void expect_condition(const sql::Expr* e, const Scope& scope,
                        const std::string& context);

  // --- FROM construction ----------------------------------------------------
  Relation leaf_relation(const sql::TableRef& ref, const Scope& outer);
  void apply_column_aliases(Relation* rel, const sql::TableRef& ref);
  void add_from(const sql::TableRef& ref, const Scope& outer, Scope* out);
  void add_from_list(const std::vector<sql::TableRef>& from,
                     const Scope& outer, Scope* out);
  Scope subquery_scope(const Scope& scope) const;
  static void add_output_aliases(Scope* scope, const std::vector<Column>& cols);
  const Column* find_bare_range(const std::vector<Relation>& rels,
                                std::size_t begin, std::size_t end,
                                const std::string& name) const;
  void hide_bare_range(std::vector<Relation>* rels, std::size_t begin,
                       std::size_t end, const std::string& name);

  // --- queries --------------------------------------------------------------
  void validate_with(const std::vector<sql::CommonTableExpr>& ctes,
                     bool recursive, const Scope& outer, Scope* out);
  std::vector<Column> validate_query(const sql::SelectStatement& s,
                                     const Scope& outer);
  std::vector<Column> validate_core(const sql::SelectStatement& s,
                                    const Scope& with_scope, Scope* scope_out);
  void validate_tail(const sql::SelectStatement& s, const Scope& scope,
                     std::size_t width);
  void check_read_contracts(const Scope& scope, const sql::Expr* where);

  // --- DML helpers ----------------------------------------------------------
  void check_returning(const std::vector<sql::SelectItem>& items,
                       const Scope& scope,
                       const std::vector<Column>* star_cols);
  void check_io_and_memory_ranges_insert(
      const Table& target, const std::vector<const Column*>& eff,
      const std::vector<sql::Expr>& cells);

  // --- range checks ---------------------------------------------------------
  void check_port(long long v);
  void check_io_value(int width, long long v);
  void check_address(long long v);
  void check_byte(long long v);

  // --- statements -----------------------------------------------------------
  void check_statement(const sql::Statement& statement);
  void check_select(const sql::SelectStatement& s);
  void check_insert(const sql::InsertStatement& s);
  void check_update(const sql::UpdateStatement& s);
  void check_delete(const sql::DeleteStatement& s);
  void check_create_table(const sql::CreateTableStatement& s);
  void check_create_index(const sql::CreateIndexStatement& s);
  void check_create_view(const sql::CreateViewStatement& s);
  void check_drop(const sql::DropStatement& s);
  void check_alter(const sql::AlterTableStatement& s);

  Column build_column(const sql::ColumnDef& def, const std::string& table);
  void check_table_constraint(const sql::TableConstraint& tc,
                              const std::vector<Column>& columns);
  // Scope in which a row of `columns` is evaluated: used by CHECK
  // constraints, which may reference any column of the table being defined.
  Scope row_scope(const std::vector<Column>& columns) const;
  // Validates the column-level CHECK constraints of `defs` once the full
  // column list of the table is known.
  void check_column_checks(const std::vector<sql::ColumnDef>& defs,
                           const std::vector<Column>& columns);
};

// --- scope / resolution ------------------------------------------------------

Validator::Lookup Validator::find_column(const Scope& scope,
                                         const std::string& qualifier,
                                         const std::string& name) const {
  Lookup lk;
  const std::string lname = lower(name);

  if (qualifier.empty()) {
    // Pass 1: real FROM relations (input columns win over aliases).
    const Column* found = nullptr;
    for (const Relation& rel : scope.relations) {
      if (rel.virtual_cte || rel.output_alias || rel.excluded) continue;
      for (const Column& c : rel.columns) {
        if (c.hidden || !ci_eq(c.name, lname)) continue;
        if (found != nullptr) {
          lk.found = true;
          lk.ambiguous = true;
          return lk;
        }
        found = &c;
      }
    }
    if (found != nullptr) {
      lk.found = true;
      lk.column = found;
      return lk;
    }
    // Pass 2: output aliases (GROUP BY / ORDER BY).
    for (const Relation& rel : scope.relations) {
      if (!rel.output_alias) continue;
      for (const Column& c : rel.columns) {
        if (c.hidden || !ci_eq(c.name, lname)) continue;
        lk.found = true;
        lk.column = &c;
        return lk;
      }
    }
    return lk;
  }

  // Qualified: t.col / excluded.col — only directly referenceable relations.
  const std::string lq = lower(qualifier);
  for (const Relation& rel : scope.relations) {
    if (rel.virtual_cte || rel.output_alias || rel.key != lq) continue;
    if (rel.opaque) {
      lk.found = true;  // relation known, columns unknown → type Unknown
      return lk;
    }
    for (const Column& c : rel.columns) {
      if (c.hidden || !ci_eq(c.name, lname)) continue;
      lk.found = true;
      lk.column = &c;
      return lk;
    }
    return lk;  // relation matched, column missing → unknown column
  }
  return lk;
}

Validator::Scope Validator::subquery_scope(const Scope& scope) const {
  // Subqueries see CTEs, not the enclosing FROM relations.
  Scope out;
  for (const Relation& rel : scope.relations) {
    if (rel.virtual_cte) out.relations.push_back(rel);
  }
  out.has_broken = scope.has_broken;
  return out;
}

void Validator::add_output_aliases(Scope* scope,
                                   const std::vector<Column>& cols) {
  if (cols.empty()) return;
  Relation rel;
  rel.output_alias = true;
  rel.columns = cols;
  scope->relations.push_back(std::move(rel));
}

const Column* Validator::find_bare_range(const std::vector<Relation>& rels,
                                         std::size_t begin, std::size_t end,
                                         const std::string& name) const {
  for (std::size_t i = begin; i < end && i < rels.size(); ++i) {
    if (rels[i].virtual_cte || rels[i].output_alias) continue;
    for (const Column& c : rels[i].columns) {
      if (!c.hidden && ci_eq(c.name, name)) return &c;
    }
  }
  return nullptr;
}

void Validator::hide_bare_range(std::vector<Relation>* rels,
                                std::size_t begin, std::size_t end,
                                const std::string& name) {
  for (std::size_t i = begin; i < end && i < rels->size(); ++i) {
    if ((*rels)[i].virtual_cte || (*rels)[i].output_alias) continue;
    for (Column& c : (*rels)[i].columns) {
      if (!c.hidden && ci_eq(c.name, name)) c.hidden = true;
    }
  }
}

// --- FROM --------------------------------------------------------------------

void Validator::apply_column_aliases(Relation* rel, const sql::TableRef& ref) {
  if (rel->broken || ref.column_aliases.empty()) return;
  if (rel->opaque) {
    // FROM range(1, 3) AS t(a, b): aliases make the columns concrete.
    rel->columns.clear();
    for (const std::string& name : ref.column_aliases) {
      rel->columns.push_back({name, Type::Unknown});
    }
    rel->opaque = false;
    return;
  }
  if (ref.column_aliases.size() != rel->columns.size()) {
    error("relation " + q(ref.alias.empty() ? ref.name : ref.alias) +
          " declares " + std::to_string(ref.column_aliases.size()) +
          " column(s) but has " + std::to_string(rel->columns.size()));
    return;
  }
  for (std::size_t i = 0; i < ref.column_aliases.size(); ++i) {
    rel->columns[i].name = ref.column_aliases[i];
  }
}

Validator::Relation Validator::leaf_relation(const sql::TableRef& ref,
                                             const Scope& outer) {
  using K = sql::TableRef::Kind;
  Relation rel;
  rel.display = ref.alias.empty() ? ref.name : ref.alias;
  rel.key = lower(rel.display);

  if (ref.kind == K::Subquery) {
    if (ref.subquery) {
      rel.columns = validate_query(*ref.subquery, subquery_scope(outer));
    }
  } else if (ref.kind == K::TableFunction) {
    rel.opaque = true;
    if (ref.function) (void)expr(*ref.function, outer);
  } else {  // K::Table
    const std::string lbase = lower(ref.name);
    bool cte_hit = false;
    for (const Relation& r : outer.relations) {
      if (r.virtual_cte && r.key == lbase) {
        rel.columns = r.columns;
        cte_hit = true;
        break;
      }
    }
    if (!cte_hit) {
      const Table* table = catalog_.find(ref.name);
      if (table == nullptr) {
        error("unknown table " + q(ref.name));
        rel.broken = true;
        return rel;
      }
      rel.columns = table->columns;
      rel.table = table;
    }
  }
  apply_column_aliases(&rel, ref);
  return rel;
}

void Validator::add_from(const sql::TableRef& ref, const Scope& outer,
                         Scope* out) {
  using K = sql::TableRef::Kind;

  if (ref.kind == K::Join) {
    Scope joined;
    if (ref.left) add_from(*ref.left, outer, &joined);
    const std::size_t left_count = joined.relations.size();
    if (ref.right) add_from(*ref.right, outer, &joined);

    if (ref.on) expect_condition(ref.on.get(), joined, "join condition");

    if (!ref.using_columns.empty()) {
      for (const std::string& col : ref.using_columns) {
        const Column* lc =
            find_bare_range(joined.relations, 0, left_count, col);
        const Column* rc = find_bare_range(joined.relations, left_count,
                                           joined.relations.size(), col);
        if (lc == nullptr || rc == nullptr) {
          error("USING column " + q(col) +
                " must exist in both joined tables");
          continue;
        }
        Type merged;
        if (!unify_types(lc->type, rc->type, &merged)) {
          error("USING column " + q(col) + " has incompatible types (" +
                type_name(lc->type) + " vs " + type_name(rc->type) + ")");
        }
        hide_bare_range(&joined.relations, left_count,
                        joined.relations.size(), col);
      }
    } else if (ref.natural) {
      std::vector<std::string> names;
      for (std::size_t i = left_count; i < joined.relations.size(); ++i) {
        for (const Column& c : joined.relations[i].columns) {
          names.push_back(c.name);
        }
      }
      for (const std::string& name : names) {
        if (find_bare_range(joined.relations, 0, left_count, name) != nullptr) {
          hide_bare_range(&joined.relations, left_count,
                          joined.relations.size(), name);
        }
      }
    }

    for (Relation& r : joined.relations) {
      if (!r.key.empty() && !r.virtual_cte && !r.output_alias) {
        for (const Relation& existing : out->relations) {
          if (existing.virtual_cte || existing.output_alias) continue;
          if (existing.key == r.key) {
            error("relation name " + q(r.display) +
                  " appears more than once");
            break;
          }
        }
      }
      out->relations.push_back(std::move(r));
    }
    out->has_broken = out->has_broken || joined.has_broken;
    return;
  }

  Relation rel = leaf_relation(ref, outer);
  if (rel.broken) out->has_broken = true;
  if (!rel.key.empty() && !rel.virtual_cte && !rel.output_alias) {
    for (const Relation& existing : out->relations) {
      if (existing.virtual_cte || existing.output_alias) continue;
      if (existing.key == rel.key) {
        error("relation name " + q(rel.display) + " appears more than once");
        break;
      }
    }
  }
  out->relations.push_back(std::move(rel));
}

void Validator::add_from_list(const std::vector<sql::TableRef>& from,
                              const Scope& outer, Scope* out) {
  for (const sql::TableRef& ref : from) add_from(ref, outer, out);
}

// --- system-table read contracts ---------------------------------------------

void Validator::check_read_contracts(const Scope& scope,
                                     const sql::Expr* where) {
  for (const Relation& rel : scope.relations) {
    if (rel.table == nullptr || rel.virtual_cte || rel.output_alias) continue;
    const Table& t = *rel.table;
    if (t.sys == SystemClass::IoWrite) {
      error(q(t.name) + " accepts INSERT only");
    } else if (t.sys == SystemClass::IoRead) {
      if (!where_constrains(where, "port")) {
        error(q(t.name) +
              " reads require a port constraint (WHERE port = ...)");
      } else {
        for (long long v : where_const_equals(where, "port")) check_port(v);
      }
    } else if (t.sys == SystemClass::Memory) {
      if (!where_constrains(where, "address")) {
        error(q(t.name) +
              " reads require an address constraint (WHERE address = ...)");
      } else {
        for (long long v : where_const_equals(where, "address")) {
          check_address(v);
        }
      }
    }
  }
}

// --- range checks ------------------------------------------------------------

void Validator::check_port(long long v) {
  if (v < 0 || v > 65535) {
    error("port " + std::to_string(v) + " is out of range 0..65535");
  }
}

void Validator::check_io_value(int width, long long v) {
  const long long max =
      width == 8 ? 255LL : (width == 16 ? 65535LL : 4294967295LL);
  if (v < 0 || v > max) {
    error("value " + std::to_string(v) + " is out of range 0.." +
          std::to_string(max) + " for io_" + std::to_string(width) +
          "_write");
  }
}

void Validator::check_address(long long v) {
  if (v < 0) {
    error("address must be >= 0 (got " + std::to_string(v) + ")");
  }
}

void Validator::check_byte(long long v) {
  if (v < 0 || v > 255) {
    error("value " + std::to_string(v) +
          " is out of range 0..255 for byte-addressable memory");
  }
}

// --- expressions -------------------------------------------------------------

void Validator::expect_condition(const sql::Expr* e, const Scope& scope,
                                 const std::string& context) {
  if (e == nullptr) return;
  const Type t = expr(*e, scope);
  if (!is_boolish(t)) {
    error(context + " must be boolean, got " + type_name(t));
  }
}

void Validator::check_window(const sql::WindowSpec& w, const Scope& scope) {
  for (const sql::Expr& e : w.partition_by) (void)expr(e, scope);
  for (const sql::OrderByItem& item : w.order_by) (void)expr(item.expr, scope);
  if (w.frame_start_expr) (void)expr(*w.frame_start_expr, scope);
  if (w.frame_end_expr) (void)expr(*w.frame_end_expr, scope);
}

Type Validator::check_function(const sql::Expr& e, const Scope& scope) {
  const std::string display = e.parts.empty() ? std::string("?") : join_dot(e.parts);
  const std::string fn = e.parts.empty() ? std::string() : lower(e.parts.back());
  const FnSpec* spec = find_builtin(fn);

  if (e.parts.empty()) {
    error("unnamed function call");
  } else if (spec == nullptr) {
    error("unknown function " + q(display));
  } else if (e.star) {
    if (fn != "count") {
      error("function " + q(display) + " does not accept '*'");
    }
  } else {
    const std::size_t got = e.args.size();
    const bool ok = spec->max_args < 0
                        ? got >= static_cast<std::size_t>(spec->min_args)
                        : got >= static_cast<std::size_t>(spec->min_args) &&
                              got <= static_cast<std::size_t>(spec->max_args);
    if (!ok) error(arity_message(display, *spec, got));
  }

  std::vector<Type> ts;
  for (const sql::ExprPtr& arg : e.args) {
    if (arg) ts.push_back(expr(*arg, scope));
  }
  if (e.over) check_window(*e.over, scope);
  if (spec == nullptr || e.parts.empty()) return Type::Unknown;

  const Type first = ts.empty() ? Type::Unknown : ts[0];
  const bool numeric_fn = fn == "sum" || fn == "avg" || fn == "abs" ||
                          fn == "floor" || fn == "ceil" || fn == "ceiling" ||
                          fn == "round" || fn == "hex";
  const bool text_fn = fn == "length" || fn == "lower" || fn == "upper" ||
                       fn == "substr" || fn == "substring" || fn == "trim" ||
                       fn == "ltrim" || fn == "rtrim" || fn == "replace";
  if (numeric_fn && !is_numericish(first)) {
    error("argument of " + q(display) + " must be numeric, got " +
          type_name(first));
  }
  if (text_fn && !is_textish(first)) {
    error("argument of " + q(display) + " must be text, got " +
          type_name(first));
  }

  if (fn == "count") return Type::Int;
  if (fn == "length") return Type::Int;
  if (fn == "sum") {
    if (first == Type::Int) return Type::Int;
    if (first == Type::Real) return Type::Real;
    return Type::Unknown;
  }
  if (fn == "avg") {
    return is_numericish(first) && (first == Type::Int || first == Type::Real)
               ? Type::Real
               : Type::Unknown;
  }
  if (fn == "coalesce") {
    Type acc = first;
    for (std::size_t i = 1; i < ts.size(); ++i) {
      Type merged;
      if (!unify_types(acc, ts[i], &merged)) {
        error("coalesce arguments have incompatible types (" +
              type_name(acc) + " vs " + type_name(ts[i]) + ")");
        return Type::Unknown;
      }
      acc = merged;
    }
    return acc;
  }
  if (fn == "nullif") {
    if (ts.size() == 2 && !comparable(ts[0], ts[1])) {
      error("cannot compare " + type_name(ts[0]) + " with " +
            type_name(ts[1]));
    }
    return first;
  }
  if (fn == "min" || fn == "max" || fn == "abs" || fn == "floor" ||
      fn == "ceil" || fn == "ceiling" || fn == "round") {
    return first;
  }
  // Text-producing functions.
  return Type::Text;
}

Type Validator::binary(const sql::Expr& e, const Scope& scope) {
  if (e.args.size() < 2) return Type::Unknown;
  const std::string& op = e.name;
  const Type left = expr(*e.args[0], scope);
  const Type right = expr(*e.args[1], scope);

  if (op == "AND" || op == "OR") {
    if (!is_boolish(left)) {
      error("left operand of " + q(op) + " must be boolean, got " +
            type_name(left));
    }
    if (!is_boolish(right)) {
      error("right operand of " + q(op) + " must be boolean, got " +
            type_name(right));
    }
    return Type::Bool;
  }
  if (is_comparison(op)) {
    if (!comparable(left, right)) {
      error("cannot compare " + type_name(left) + " with " + type_name(right));
    }
    return Type::Bool;
  }
  if (op == "||") {
    if (!is_textish(left)) {
      error("left operand of '||' must be text, got " + type_name(left));
    }
    if (!is_textish(right)) {
      error("right operand of '||' must be text, got " + type_name(right));
    }
    return Type::Text;
  }
  if (is_arithmetic(op)) {
    if (!is_numericish(left)) {
      error("left operand of " + q(op) + " must be numeric, got " +
            type_name(left));
    }
    if (!is_numericish(right)) {
      error("right operand of " + q(op) + " must be numeric, got " +
            type_name(right));
    }
    if (op == "^") return Type::Real;
    return (left == Type::Real || right == Type::Real) ? Type::Real
                                                       : Type::Int;
  }
  return Type::Unknown;  // defensive: parser only produces known operators
}

Type Validator::expr(const sql::Expr& e, const Scope& scope) {
  using K = sql::Expr::Kind;
  switch (e.kind) {
    case K::Literal:
      if (std::holds_alternative<std::nullptr_t>(e.literal)) return Type::Null;
      if (std::holds_alternative<long long>(e.literal)) return Type::Int;
      if (std::holds_alternative<double>(e.literal)) return Type::Real;
      if (std::holds_alternative<std::string>(e.literal)) return Type::Text;
      if (std::holds_alternative<bool>(e.literal)) return Type::Bool;
      return Type::Unknown;

    case K::Parameter:
      return Type::Unknown;

    case K::ColumnRef: {
      if (e.parts.empty()) return Type::Unknown;
      const std::string qualifier =
          e.parts.size() >= 2 ? e.parts[e.parts.size() - 2] : std::string();
      const std::string display = join_dot(e.parts);
      const Lookup lk = find_column(scope, qualifier, e.parts.back());
      if (lk.ambiguous) {
        error("column " + q(display) + " is ambiguous");
        return Type::Unknown;
      }
      if (lk.found) return lk.column != nullptr ? lk.column->type : Type::Unknown;
      if (scope.has_broken) return Type::Unknown;  // suppress cascade
      error("unknown column " + q(display));
      return Type::Unknown;
    }

    case K::Star:
      error("unexpected '*' in expression");
      return Type::Unknown;

    case K::Unary: {
      if (e.args.empty() || !e.args[0]) return Type::Unknown;
      const Type t = expr(*e.args[0], scope);
      if (e.name == "NOT") {
        if (!is_boolish(t)) {
          error("NOT requires a boolean operand, got " + type_name(t));
        }
        return Type::Bool;
      }
      if (e.name == "~") {
        if (t != Type::Int && !is_numericish(t)) {
          error("'~' requires an integer operand, got " + type_name(t));
        }
        return Type::Int;
      }
      if (!is_numericish(t)) {
        error("unary " + q(e.name) + " requires a numeric operand, got " +
              type_name(t));
      }
      return t;
    }

    case K::Binary:
      return binary(e, scope);

    case K::NullTest:
      if (!e.args.empty() && e.args[0]) (void)expr(*e.args[0], scope);
      return Type::Bool;

    case K::BoolTest: {
      if (e.args.empty() || !e.args[0]) return Type::Bool;
      const Type t = expr(*e.args[0], scope);
      const std::string kw = sql::to_upper_ascii(e.name);
      if (kw != "UNKNOWN" && !is_boolish(t)) {
        error("argument of 'IS " + kw + "' must be boolean, got " +
              type_name(t));
      }
      return Type::Bool;
    }

    case K::In: {
      const Type t =
          e.args.empty() || !e.args[0] ? Type::Unknown : expr(*e.args[0], scope);
      auto check_sub = [&](const sql::SelectStatement& sub) {
        const std::vector<Column> cols = validate_query(sub, subquery_scope(scope));
        if (cols.size() != 1) {
          error("subquery must return exactly one column (got " +
                std::to_string(cols.size()) + ")");
        } else if (!comparable(t, cols[0].type)) {
          error("cannot compare " + type_name(t) + " with " +
                type_name(cols[0].type));
        }
      };
      for (std::size_t i = 1; i < e.args.size(); ++i) {
        const sql::Expr& member = *e.args[i];
        if (member.kind == K::Subquery || member.subquery) {
          if (member.subquery) check_sub(*member.subquery);
          continue;
        }
        const Type mt = expr(member, scope);
        if (!comparable(t, mt)) {
          error("cannot compare " + type_name(t) + " with " + type_name(mt));
        }
      }
      if (e.subquery) check_sub(*e.subquery);
      return Type::Bool;
    }

    case K::Like: {
      if (e.args.empty() || !e.args[0]) return Type::Bool;
      const Type a = expr(*e.args[0], scope);
      if (!is_textish(a)) {
        error("argument of " + q(e.name) + " must be text, got " +
              type_name(a));
      }
      if (e.args.size() > 1 && e.args[1]) {
        const Type p = expr(*e.args[1], scope);
        if (!is_textish(p)) {
          error("pattern of " + q(e.name) + " must be text, got " +
                type_name(p));
        }
      }
      if (e.args.size() > 2 && e.args[2]) (void)expr(*e.args[2], scope);
      return Type::Bool;
    }

    case K::Between: {
      if (e.args.size() < 3) return Type::Bool;
      const Type a = expr(*e.args[0], scope);
      const Type low = expr(*e.args[1], scope);
      const Type high = expr(*e.args[2], scope);
      if (!comparable(a, low)) {
        error("cannot compare " + type_name(a) + " with " + type_name(low));
      }
      if (!comparable(a, high)) {
        error("cannot compare " + type_name(a) + " with " + type_name(high));
      }
      return Type::Bool;
    }

    case K::Exists:
      if (e.subquery) {
        (void)validate_query(*e.subquery, subquery_scope(scope));
      } else {
        for (const sql::ExprPtr& arg : e.args) {
          if (arg) (void)expr(*arg, scope);
        }
      }
      return Type::Bool;

    case K::Subquery: {
      if (!e.subquery) return Type::Unknown;
      const std::vector<Column> cols =
          validate_query(*e.subquery, subquery_scope(scope));
      if (cols.size() != 1) {
        error("subquery must return exactly one column (got " +
              std::to_string(cols.size()) + ")");
        return Type::Unknown;
      }
      return cols[0].type;
    }

    case K::FunctionCall:
      return check_function(e, scope);

    case K::Cast: {
      Type t = Type::Unknown;
      if (!type_from_name(e.type_name, &t)) {
        error("unknown type " + q(e.type_name));
        t = Type::Unknown;
      }
      if (!e.args.empty() && e.args[0]) (void)expr(*e.args[0], scope);
      return t;
    }

    case K::Collate: {
      if (!e.args.empty() && e.args[0]) {
        const Type t = expr(*e.args[0], scope);
        if (!is_textish(t)) {
          error("argument of COLLATE must be text, got " + type_name(t));
        }
      }
      return Type::Text;
    }

    case K::Case: {
      Type result = Type::Unknown;
      bool have = false;
      auto merge = [&](Type t) {
        if (!have) {
          result = t;
          have = true;
          return;
        }
        Type merged;
        if (!unify_types(result, t, &merged)) {
          error("CASE branches have incompatible types (" +
                type_name(result) + " vs " + type_name(t) + ")");
          return;
        }
        result = merged;
      };
      if (e.operand) {
        const Type op_t = expr(*e.operand, scope);
        for (const sql::ExprPtr& w : e.args) {
          if (!w) continue;
          const Type wt = expr(*w, scope);
          if (!comparable(op_t, wt)) {
            error("cannot compare " + type_name(op_t) + " with " +
                  type_name(wt));
          }
        }
      } else {
        for (const sql::ExprPtr& w : e.args) {
          expect_condition(w.get(), scope, "CASE WHEN condition");
        }
      }
      for (const sql::ExprPtr& r : e.results) {
        if (r) merge(expr(*r, scope));
      }
      if (e.else_result) merge(expr(*e.else_result, scope));
      return result;
    }

    case K::Row:
      for (const sql::ExprPtr& arg : e.args) {
        if (arg) (void)expr(*arg, scope);
      }
      return Type::Unknown;

    case K::Subscript:
      for (const sql::ExprPtr& arg : e.args) {
        if (arg) (void)expr(*arg, scope);
      }
      return Type::Unknown;

    case K::Quantified: {
      if (!e.args.empty() && e.args[0]) (void)expr(*e.args[0], scope);
      if (e.args.size() > 1 && e.args[1]) {
        const sql::Expr& rhs = *e.args[1];
        if (rhs.subquery) {
          (void)validate_query(*rhs.subquery, subquery_scope(scope));
        } else {
          (void)expr(rhs, scope);
        }
      }
      return Type::Bool;
    }

    case K::Default:
      if (!allow_default_) error("DEFAULT is not allowed here");
      return Type::Unknown;
  }
  return Type::Unknown;
}

// --- queries -----------------------------------------------------------------

void Validator::validate_with(const std::vector<sql::CommonTableExpr>& ctes,
                              bool recursive, const Scope& outer,
                              Scope* out) {
  *out = outer;
  if (ctes.empty()) return;

  if (recursive) {
    bool any_self = false;
    for (const sql::CommonTableExpr& cte : ctes) {
      if (cte.query && select_references(*cte.query, cte.name)) {
        any_self = true;
        break;
      }
    }
    if (!any_self) {
      error("WITH RECURSIVE specified but no CTE references itself");
    }
  }

  for (const sql::CommonTableExpr& cte : ctes) {
    if (!cte.query) continue;
    const bool self = select_references(*cte.query, cte.name);
    std::vector<Column> cols;

    if (self && !recursive) {
      error("WITH RECURSIVE is required for recursive reference to " +
            q(cte.name));
    }

    if (self && recursive) {
      const sql::SelectStatement& qy = *cte.query;
      const bool well_formed = qy.set_op == "UNION" && qy.set_all &&
                               qy.set_left != nullptr &&
                               qy.set_right != nullptr;
      if (!well_formed) {
        error("recursive CTE " + q(cte.name) +
              " must be (anchor) UNION ALL (recursive term)");
        cols = validate_query(*cte.query, *out);
      } else {
        if (select_references(*qy.set_left, cte.name)) {
          error("recursive reference to " + q(cte.name) +
                " must not appear in the anchor term");
        }
        const std::vector<Column> anchor =
            validate_query(*qy.set_left, *out);
        cols = anchor;

        // Declared columns name the anchor's output — and therefore what the
        // recursive term sees. Apply them before validating the term so a
        // literal anchor (`AS (SELECT 1 ...)`) can still be written as
        // `walk(n)` with the term reading `n`. A width mismatch is reported
        // after the term validates (both queries must be checked anyway).
        if (!cte.columns.empty() && cte.columns.size() == cols.size()) {
          for (std::size_t i = 0; i < cols.size(); ++i) {
            cols[i].name = cte.columns[i];
          }
        }

        SelfRefStats st;
        scan_select_refs(*qy.set_right, cte.name, /*in_outer=*/false,
                         /*in_subquery=*/false, &st);
        if (st.count != 1) {
          error("recursive term of " + q(cte.name) + " must reference " +
                q(cte.name) + " exactly once (found " +
                std::to_string(st.count) + ")");
        }
        if (st.in_outer) {
          error("recursive reference to " + q(cte.name) +
                " must not appear within an outer join");
        }
        if (st.in_subquery) {
          error("recursive reference to " + q(cte.name) +
                " must not appear within a subquery");
        }

        Scope rec = *out;
        Relation self_rel;
        self_rel.key = lower(cte.name);
        self_rel.display = cte.name;
        self_rel.virtual_cte = true;
        self_rel.columns = cols;
        rec.relations.push_back(std::move(self_rel));

        const std::vector<Column> term = validate_query(*qy.set_right, rec);
        if (term.size() != cols.size()) {
          error("recursive term of " + q(cte.name) + " returns " +
                std::to_string(term.size()) + " column(s) but the anchor returns " +
                std::to_string(cols.size()));
        } else {
          for (std::size_t i = 0; i < term.size(); ++i) {
            Type merged;
            if (!unify_types(cols[i].type, term[i].type, &merged)) {
              error("recursive term of " + q(cte.name) + " column " +
                    std::to_string(i + 1) + ": type mismatch (" +
                    type_name(term[i].type) + " vs " + type_name(cols[i].type) +
                    ")");
            } else {
              cols[i].type = merged;
            }
          }
        }
      }
    } else {
      cols = validate_query(*cte.query, *out);
    }

    if (!cte.columns.empty()) {
      if (cte.columns.size() != cols.size()) {
        error("CTE " + q(cte.name) + " declares " +
              std::to_string(cte.columns.size()) +
              " column(s) but its query returns " +
              std::to_string(cols.size()));
      } else {
        for (std::size_t i = 0; i < cte.columns.size(); ++i) {
          cols[i].name = cte.columns[i];
        }
      }
    }

    Relation rel;
    rel.key = lower(cte.name);
    rel.display = cte.name;
    rel.virtual_cte = true;
    rel.columns = std::move(cols);
    out->relations.push_back(std::move(rel));
  }
}

std::vector<Column> Validator::validate_query(const sql::SelectStatement& s,
                                              const Scope& outer) {
  Scope with_scope = outer;
  if (!s.with.empty()) validate_with(s.with, s.recursive, outer, &with_scope);

  if (!s.set_op.empty()) {
    if (s.set_left == nullptr || s.set_right == nullptr) {
      error("malformed set operation");
      return {};
    }
    std::vector<Column> left = validate_query(*s.set_left, with_scope);
    const std::vector<Column> right = validate_query(*s.set_right, with_scope);
    if (left.size() != right.size()) {
      error("set operation branches have different numbers of columns (" +
            std::to_string(left.size()) + " vs " +
            std::to_string(right.size()) + ")");
    } else {
      for (std::size_t i = 0; i < left.size(); ++i) {
        Type merged;
        if (!unify_types(left[i].type, right[i].type, &merged)) {
          error("set operation column " + std::to_string(i + 1) +
                " has incompatible types (" + type_name(left[i].type) +
                " vs " + type_name(right[i].type) + ")");
        } else {
          left[i].type = merged;
        }
        left[i].hidden = false;
      }
    }
    Scope tail = with_scope;
    add_output_aliases(&tail, left);
    validate_tail(s, tail, left.size());
    return left;
  }

  Scope core_scope;
  std::vector<Column> cols = validate_core(s, with_scope, &core_scope);
  Scope tail = core_scope;
  add_output_aliases(&tail, cols);
  validate_tail(s, tail, cols.size());
  return cols;
}

std::vector<Column> Validator::validate_core(const sql::SelectStatement& s,
                                             const Scope& with_scope,
                                             Scope* scope_out) {
  Scope scope = with_scope;
  std::vector<Column> cols;

  if (!s.is_values) {
    add_from_list(s.from, with_scope, &scope);
    check_read_contracts(scope, s.where.get());

    bool any_star = false;
    for (const sql::SelectItem& item : s.columns) {
      const sql::Expr& e = item.expr;
      if (e.kind != sql::Expr::Kind::Star) {
        const Type t = expr(e, scope);
        Column c;
        c.name = item.alias.empty() ? derive_name(e) : item.alias;
        c.type = t;
        cols.push_back(std::move(c));
        continue;
      }
      any_star = true;
      if (e.parts.empty()) {
        bool has_relations = false;
        for (const Relation& rel : scope.relations) {
          if (rel.virtual_cte || rel.output_alias) continue;
          has_relations = true;
          for (const Column& c : rel.columns) {
            if (c.hidden) continue;
            cols.push_back(c);
          }
        }
        if (!has_relations && !scope.has_broken) {
          error("asterisk requires a FROM clause");
        }
      } else {
        const std::string qual = lower(e.parts[0]);
        const Relation* match = nullptr;
        for (const Relation& rel : scope.relations) {
          if (rel.virtual_cte || rel.output_alias || rel.key != qual) continue;
          match = &rel;
          break;
        }
        if (match == nullptr) {
          if (!scope.has_broken) error("unknown table " + q(e.parts[0]));
        } else if (!match->opaque) {
          for (const Column& c : match->columns) {
            if (c.hidden) continue;
            cols.push_back(c);
          }
        }
      }
    }
    (void)any_star;
  } else {
    if (s.value_rows.empty()) return cols;
    const std::size_t width = s.value_rows[0].size();
    std::vector<Type> types(width, Type::Unknown);
    std::size_t row_number = 0;
    for (const std::vector<sql::Expr>& row : s.value_rows) {
      ++row_number;
      if (row.size() != width) {
        error("VALUES row " + std::to_string(row_number) + " has " +
              std::to_string(row.size()) + " value(s) but row 1 has " +
              std::to_string(width));
      }
      for (std::size_t i = 0; i < row.size() && i < width; ++i) {
        const Type t = expr(row[i], scope);
        if (row_number == 1) {
          types[i] = t;
        } else {
          Type merged;
          if (!unify_types(types[i], t, &merged)) {
            error("VALUES column " + std::to_string(i + 1) +
                  " has incompatible types (" + type_name(types[i]) + " vs " +
                  type_name(t) + ")");
          } else {
            types[i] = merged;
          }
        }
      }
    }
    for (std::size_t i = 0; i < width; ++i) {
      cols.push_back({std::string("?column?"), types[i]});
    }
  }

  if (s.where) expect_condition(s.where.get(), scope, "WHERE condition");

  Scope group_scope = scope;
  add_output_aliases(&group_scope, cols);
  for (const sql::Expr& g : s.group_by) {
    if (const auto v = const_int(g)) {
      const long long pos = *v;
      if (pos < 1 || pos > static_cast<long long>(cols.size())) {
        error("GROUP BY position " + std::to_string(pos) +
              " is not in the select list (1.." +
              std::to_string(cols.size()) + ")");
      }
    } else {
      (void)expr(g, group_scope);
    }
  }
  if (s.having) expect_condition(s.having.get(), group_scope, "HAVING condition");
  for (const sql::Expr& d : s.distinct_on) (void)expr(d, scope);

  *scope_out = std::move(scope);
  return cols;
}

void Validator::validate_tail(const sql::SelectStatement& s,
                              const Scope& scope, std::size_t width) {
  for (const sql::OrderByItem& item : s.order_by) {
    if (const auto v = const_int(item.expr)) {
      const long long pos = *v;
      if (pos < 1 || pos > static_cast<long long>(width)) {
        error("ORDER BY position " + std::to_string(pos) +
              " is not in the select list (1.." + std::to_string(width) + ")");
      }
      continue;
    }
    (void)expr(item.expr, scope);
  }
  if (s.limit && !s.limit_all) {
    const Type t = expr(*s.limit, scope);
    if (t != Type::Unknown && t != Type::Null && t != Type::Int &&
        t != Type::Real) {
      error("LIMIT must be numeric, got " + type_name(t));
    }
  }
  if (s.offset) {
    const Type t = expr(*s.offset, scope);
    if (t != Type::Unknown && t != Type::Null && t != Type::Int &&
        t != Type::Real) {
      error("OFFSET must be numeric, got " + type_name(t));
    }
  }
}

// --- DML ---------------------------------------------------------------------

void Validator::check_returning(const std::vector<sql::SelectItem>& items,
                                const Scope& scope,
                                const std::vector<Column>* star_cols) {
  for (const sql::SelectItem& item : items) {
    if (item.expr.kind == sql::Expr::Kind::Star) {
      if (star_cols == nullptr && !scope.has_broken) {
        error("asterisk requires a target table");
      }
      continue;
    }
    (void)expr(item.expr, scope);
  }
}

void Validator::check_io_and_memory_ranges_insert(
    const Table& target, const std::vector<const Column*>& eff,
    const std::vector<sql::Expr>& cells) {
  if (target.sys != SystemClass::IoWrite && target.sys != SystemClass::Memory) {
    return;
  }
  for (std::size_t i = 0; i < cells.size() && i < eff.size(); ++i) {
    if (eff[i] == nullptr) continue;
    const auto v = const_int(cells[i]);
    if (!v) continue;
    const std::string& name = eff[i]->name;
    if (ci_eq(name, "port")) {
      check_port(*v);
    } else if (ci_eq(name, "value")) {
      if (target.sys == SystemClass::IoWrite) {
        check_io_value(target.io_width, *v);
      } else {
        check_byte(*v);
      }
    } else if (ci_eq(name, "address")) {
      check_address(*v);
    }
  }
}

void Validator::check_select(const sql::SelectStatement& s) {
  Scope empty;
  (void)validate_query(s, empty);
}

void Validator::check_insert(const sql::InsertStatement& s) {
  Scope cte_scope;
  if (!s.with.empty()) validate_with(s.with, s.recursive, Scope{}, &cte_scope);

  Table* target = catalog_.find_mut(s.table);
  if (target == nullptr) {
    bool is_cte = false;
    for (const Relation& r : cte_scope.relations) {
      if (r.virtual_cte && r.key == lower(s.table)) is_cte = true;
    }
    if (is_cte) error("cannot modify CTE " + q(s.table));
    else error("unknown table " + q(s.table));
  }

  if (target != nullptr) {
    if (target->kind == TableKind::View) {
      error("cannot modify view " + q(s.table));
      target = nullptr;  // suppress downstream contract checks
    } else if (target->sys == SystemClass::IoRead) {
      error(q(target->name) + " accepts SELECT only");
      target = nullptr;
    } else if (target->sys == SystemClass::Boot) {
      error(q(target->name) + " accepts SELECT only");
      target = nullptr;
    }
  }
  if (target != nullptr && target->sys != SystemClass::None) {
    if (s.upsert != nullptr) {
      error("ON CONFLICT is not supported on system tables");
    }
    if (!s.returning.empty()) {
      error("RETURNING is not supported on system tables");
    }
  }

  // Effective column list.
  std::vector<const Column*> eff;
  std::vector<std::string> eff_names;
  if (target != nullptr) {
    if (!s.columns.empty()) {
      bool port_seen = false;
      bool value_seen = false;
      bool address_seen = false;
      for (const std::string& name : s.columns) {
        const Column* col = nullptr;
        for (const Column& c : target->columns) {
          if (ci_eq(c.name, name)) {
            col = &c;
            break;
          }
        }
        if (col == nullptr) {
          error("unknown column " + q(name));
          eff.push_back(nullptr);
          eff_names.push_back(name);
          continue;
        }
        for (const std::string& seen : eff_names) {
          if (ci_eq(seen, name)) {
            error("duplicate column " + q(name) + " in INSERT column list");
            break;
          }
        }
        if (ci_eq(name, "port")) port_seen = true;
        if (ci_eq(name, "value")) value_seen = true;
        if (ci_eq(name, "address")) address_seen = true;
        eff.push_back(col);
        eff_names.push_back(name);
      }
      if (target->sys == SystemClass::IoWrite &&
          !(port_seen && value_seen)) {
        error(q(target->name) +
              " INSERT must specify both port and value");
      }
      if (target->sys == SystemClass::Memory &&
          !(address_seen && value_seen)) {
        error(q(target->name) +
              " INSERT must specify both address and value");
      }
    } else {
      for (const Column& c : target->columns) {
        eff.push_back(&c);
        eff_names.push_back(c.name);
      }
    }

    // Row arity + types.
    std::size_t row_number = 0;
    for (const std::vector<sql::Expr>& row : s.rows) {
      ++row_number;
      if (row.size() != eff.size()) {
        error("row " + std::to_string(row_number) + " has " +
              std::to_string(row.size()) + " value(s) but " +
              std::to_string(eff.size()) + " column(s) are targeted");
      }
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (row[i].kind == sql::Expr::Kind::Default) continue;
        const Type t = expr(row[i], cte_scope);
        if (i < eff.size() && eff[i] != nullptr &&
            !assignable(t, eff[i]->type)) {
          error("type mismatch: cannot store " + type_name(t) + " in " +
                type_name(eff[i]->type) + " column " + q(eff[i]->name));
        }
      }
      check_io_and_memory_ranges_insert(*target, eff, row);
    }

    // DEFAULT VALUES: every column must be defaultable or nullable.
    if (s.default_values) {
      for (const Column& c : target->columns) {
        if (!c.has_default && c.not_null) {
          error("column " + q(c.name) + " is NOT NULL and has no DEFAULT");
        }
      }
    }
  } else {
    // Unknown target: still walk the value expressions for inner errors.
    allow_default_ = true;
    for (const std::vector<sql::Expr>& row : s.rows) {
      for (const sql::Expr& cell : row) (void)expr(cell, cte_scope);
    }
    allow_default_ = false;
  }

  // INSERT ... SELECT
  if (s.select != nullptr) {
    const std::vector<Column> cols = validate_query(*s.select, cte_scope);
    if (target != nullptr) {
      if (cols.size() != eff.size()) {
        error("SELECT returns " + std::to_string(cols.size()) +
              " column(s) but " + std::to_string(eff.size()) +
              " column(s) are targeted");
      } else {
        for (std::size_t i = 0; i < cols.size(); ++i) {
          if (eff[i] != nullptr && !assignable(cols[i].type, eff[i]->type)) {
            error("type mismatch: cannot store " + type_name(cols[i].type) +
                  " in " + type_name(eff[i]->type) + " column " +
                  q(eff[i]->name));
          }
        }
      }
    }
  }

  // ON CONFLICT
  if (s.upsert != nullptr && target != nullptr) {
    const sql::UpsertClause& up = *s.upsert;
    auto known_column = [&](const std::string& name) {
      for (const Column& c : target->columns) {
        if (ci_eq(c.name, name)) return true;
      }
      return false;
    };
    for (const std::string& name : up.columns) {
      if (!known_column(name)) error("unknown column " + q(name));
    }
    if (!up.do_nothing) {
      Scope ex = cte_scope;
      Relation t_rel;
      t_rel.key = lower(s.table);
      t_rel.display = s.table;
      t_rel.columns = target->columns;
      t_rel.table = target;
      ex.relations.push_back(std::move(t_rel));
      Relation x_rel;
      x_rel.key = "excluded";
      x_rel.display = "excluded";
      x_rel.excluded = true;
      x_rel.columns = target->columns;
      ex.relations.push_back(std::move(x_rel));

      for (const auto& update : up.updates) {
        if (!known_column(update.first)) {
          error("unknown column " + q(update.first));
          continue;
        }
        if (update.second == nullptr) continue;
        if (update.second->kind == sql::Expr::Kind::Default) continue;
        const Type t = expr(*update.second, ex);
        const Column* col = nullptr;
        for (const Column& c : target->columns) {
          if (ci_eq(c.name, update.first)) {
            col = &c;
            break;
          }
        }
        if (col != nullptr && !assignable(t, col->type)) {
          error("type mismatch: cannot store " + type_name(t) + " in " +
                type_name(col->type) + " column " + q(col->name));
        }
      }
      if (up.update_where) expect_condition(up.update_where.get(), ex, "ON CONFLICT DO UPDATE WHERE condition");
    }
    if (up.target_where) expect_condition(up.target_where.get(), cte_scope, "ON CONFLICT WHERE condition");
  }

  // RETURNING
  if (!s.returning.empty()) {
    if (target != nullptr && target->system()) {
      // already reported above; skip
    } else {
      Scope ret = cte_scope;
      if (target != nullptr) {
        Relation t_rel;
        t_rel.key = lower(s.table);
        t_rel.display = s.table;
        t_rel.columns = target->columns;
        t_rel.table = target;
        ret.relations.push_back(std::move(t_rel));
        check_returning(s.returning, ret, &target->columns);
      } else {
        check_returning(s.returning, ret, nullptr);
      }
    }
  }
}

void Validator::check_update(const sql::UpdateStatement& s) {
  Scope cte_scope;
  if (!s.with.empty()) validate_with(s.with, s.recursive, Scope{}, &cte_scope);

  Table* target = catalog_.find_mut(s.table);
  if (target == nullptr) {
    bool is_cte = false;
    for (const Relation& r : cte_scope.relations) {
      if (r.virtual_cte && r.key == lower(s.table)) is_cte = true;
    }
    if (is_cte) error("cannot modify CTE " + q(s.table));
    else error("unknown table " + q(s.table));
  }
  bool skip_contracts = false;
  if (target != nullptr) {
    if (target->kind == TableKind::View) {
      error("cannot modify view " + q(s.table));
      skip_contracts = true;
    } else if (target->sys == SystemClass::IoRead) {
      error(q(target->name) + " accepts SELECT only");
      skip_contracts = true;
    } else if (target->sys == SystemClass::IoWrite) {
      error(q(target->name) + " accepts INSERT only");
      skip_contracts = true;
    } else if (target->sys == SystemClass::Boot) {
      error(q(target->name) + " accepts SELECT only");
      skip_contracts = true;
    }
    if (target->sys != SystemClass::None && !s.returning.empty()) {
      error("RETURNING is not supported on system tables");
    }
  }

  Scope scope = cte_scope;
  if (target != nullptr) {
    Relation t_rel;
    t_rel.key = lower(s.alias.empty() ? s.table : s.alias);
    t_rel.display = s.alias.empty() ? s.table : s.alias;
    t_rel.columns = target->columns;
    t_rel.table = target;
    scope.relations.push_back(std::move(t_rel));
  }
  add_from_list(s.from, cte_scope, &scope);

  // FROM-side contracts (reading hardware inside UPDATE ... FROM).
  if (!skip_contracts) {
    for (const Relation& r : scope.relations) {
      if (r.table == nullptr || r.table == target || r.virtual_cte ||
          r.output_alias) {
        continue;
      }
      if (r.table->sys == SystemClass::IoWrite) {
        error(q(r.table->name) + " accepts INSERT only");
      } else if (r.table->sys == SystemClass::IoRead) {
        if (!where_constrains(s.where.get(), "port")) {
          error(q(r.table->name) +
                " reads require a port constraint (WHERE port = ...)");
        } else {
          for (long long v : where_const_equals(s.where.get(), "port")) {
            check_port(v);
          }
        }
      } else if (r.table->sys == SystemClass::Memory) {
        if (!where_constrains(s.where.get(), "address")) {
          error(q(r.table->name) +
                " reads require an address constraint (WHERE address = ...)");
        } else {
          for (long long v : where_const_equals(s.where.get(), "address")) {
            check_address(v);
          }
        }
      }
    }
  }

  // Assignments — always against the target table's columns.
  for (const auto& assignment : s.assignments) {
    const std::string& name = assignment.first;
    const Column* col = nullptr;
    if (target != nullptr) {
      for (const Column& c : target->columns) {
        if (ci_eq(c.name, name)) {
          col = &c;
          break;
        }
      }
      if (col == nullptr) error("unknown column " + q(name));
    }
    if (assignment.second == nullptr) continue;
    if (col != nullptr && target->sys == SystemClass::Memory &&
        ci_eq(col->name, "address")) {
      error("cannot assign to 'address' on " + q(target->name) +
            " (only 'value' may be updated)");
      (void)expr(*assignment.second, scope);
      continue;
    }
    if (assignment.second->kind == sql::Expr::Kind::Default) continue;
    allow_default_ = true;
    const Type t = expr(*assignment.second, scope);
    allow_default_ = false;
    if (col != nullptr && !assignable(t, col->type)) {
      error("type mismatch: cannot store " + type_name(t) + " in " +
            type_name(col->type) + " column " + q(col->name));
    }
    if (col != nullptr && target != nullptr &&
        target->sys == SystemClass::Memory && ci_eq(col->name, "value")) {
      if (const auto v = const_int(*assignment.second)) check_byte(*v);
    }
  }

  if (s.where) expect_condition(s.where.get(), scope, "WHERE condition");

  // Memory target: address constraint + constant range checks.
  if (target != nullptr && !skip_contracts &&
      target->sys == SystemClass::Memory) {
    if (!s.where || !where_constrains(s.where.get(), "address")) {
      error(q(target->name) +
            " UPDATE requires an address constraint (WHERE address = ...)");
    } else {
      for (long long v : where_const_equals(s.where.get(), "address")) {
        check_address(v);
      }
    }
  }

  if (!s.returning.empty() && !skip_contracts &&
      target != nullptr && !target->system()) {
    check_returning(s.returning, scope, &target->columns);
  }
}

void Validator::check_delete(const sql::DeleteStatement& s) {
  Scope cte_scope;
  if (!s.with.empty()) validate_with(s.with, s.recursive, Scope{}, &cte_scope);

  Table* target = catalog_.find_mut(s.table);
  if (target == nullptr) {
    bool is_cte = false;
    for (const Relation& r : cte_scope.relations) {
      if (r.virtual_cte && r.key == lower(s.table)) is_cte = true;
    }
    if (is_cte) error("cannot modify CTE " + q(s.table));
    else error("unknown table " + q(s.table));
  }
  bool skip_contracts = false;
  if (target != nullptr) {
    if (target->kind == TableKind::View) {
      error("cannot modify view " + q(s.table));
      skip_contracts = true;
    } else if (target->sys == SystemClass::IoRead) {
      error(q(target->name) + " accepts SELECT only");
      skip_contracts = true;
    } else if (target->sys == SystemClass::IoWrite) {
      error(q(target->name) + " accepts INSERT only");
      skip_contracts = true;
    } else if (target->sys == SystemClass::Memory) {
      error(q(target->name) + " does not support DELETE (use UPDATE instead)");
      skip_contracts = true;
    } else if (target->sys == SystemClass::Boot) {
      error(q(target->name) + " accepts SELECT only");
      skip_contracts = true;
    }
    if (target->sys != SystemClass::None && !s.returning.empty()) {
      error("RETURNING is not supported on system tables");
    }
  }

  Scope scope = cte_scope;
  if (target != nullptr) {
    Relation t_rel;
    t_rel.key = lower(s.alias.empty() ? s.table : s.alias);
    t_rel.display = s.alias.empty() ? s.table : s.alias;
    t_rel.columns = target->columns;
    t_rel.table = target;
    scope.relations.push_back(std::move(t_rel));
  }

  if (s.where) expect_condition(s.where.get(), scope, "WHERE condition");
  if (!s.returning.empty() && !skip_contracts && target != nullptr &&
      !target->system()) {
    check_returning(s.returning, scope, &target->columns);
  }
}

// --- DDL ---------------------------------------------------------------------

Column Validator::build_column(const sql::ColumnDef& def,
                               const std::string& table) {
  Column col;
  col.name = def.name;
  Type type = Type::Unknown;
  if (!def.type_name.empty() && !type_from_name(def.type_name, &type)) {
    error("unknown column type " + q(def.type_name));
    type = Type::Unknown;
  }
  col.type = type;

  Scope empty;
  for (const sql::ColumnConstraint& c : def.constraints) {
    switch (c.kind) {
      case sql::ColumnConstraint::Kind::NotNull:
        col.not_null = true;
        break;
      case sql::ColumnConstraint::Kind::PrimaryKey:
        col.not_null = true;
        break;
      case sql::ColumnConstraint::Kind::Null:
      case sql::ColumnConstraint::Kind::Unique:
      case sql::ColumnConstraint::Kind::Collate:
      case sql::ColumnConstraint::Kind::AutoIncrement:
        break;
      case sql::ColumnConstraint::Kind::Default:
        if (c.value != nullptr) {
          const Type t = expr(*c.value, empty);
          if (!assignable(t, col.type)) {
            error("type mismatch: cannot store " + type_name(t) + " in " +
                  type_name(col.type) + " column " + q(col.name));
          }
        }
        col.has_default = true;
        break;
      case sql::ColumnConstraint::Kind::Check:
        // Validated after the full column list is known (check_column_checks).
        break;
      case sql::ColumnConstraint::Kind::References:
        if (!c.ref_table.empty()) {
          const Table* ref = catalog_.find(c.ref_table);
          if (ref == nullptr) {
            error("referenced table " + q(c.ref_table) + " does not exist");
          } else {
            for (const std::string& rc : c.ref_columns) {
              bool found = false;
              for (const Column& cc : ref->columns) {
                if (ci_eq(cc.name, rc)) {
                  found = true;
                  break;
                }
              }
              if (!found) {
                error("unknown column " + q(rc) + " in referenced table " +
                      q(ref->name));
              }
            }
          }
        }
        break;
    }
  }
  (void)table;
  return col;
}

void Validator::check_table_constraint(const sql::TableConstraint& tc,
                                       const std::vector<Column>& columns) {
  auto known = [&](const std::string& name) {
    for (const Column& c : columns) {
      if (ci_eq(c.name, name)) return true;
    }
    return false;
  };
  for (const std::string& name : tc.columns) {
    if (!known(name)) error("unknown column " + q(name));
  }
  switch (tc.kind) {
    case sql::TableConstraint::Kind::Check:
      if (tc.check != nullptr) {
        expect_condition(tc.check.get(), row_scope(columns), "CHECK constraint");
      }
      break;
    case sql::TableConstraint::Kind::Foreign: {
      const Table* ref = tc.ref_table.empty() ? nullptr
                                              : catalog_.find(tc.ref_table);
      if (ref == nullptr) {
        if (!tc.ref_table.empty()) {
          error("referenced table " + q(tc.ref_table) + " does not exist");
        }
        break;
      }
      for (const std::string& rc : tc.ref_columns) {
        // Reference columns are looked up in the referenced table.
        bool found = false;
        for (const Column& cc : ref->columns) {
          if (ci_eq(cc.name, rc)) {
            found = true;
            break;
          }
        }
        if (!found) {
          error("unknown column " + q(rc) + " in referenced table " +
                q(ref->name));
        }
      }
      break;
    }
    case sql::TableConstraint::Kind::PrimaryKey:
    case sql::TableConstraint::Kind::Unique:
      break;
  }
}

Validator::Scope Validator::row_scope(const std::vector<Column>& columns) const {
  Scope s;
  Relation r;
  r.columns = columns;
  s.relations.push_back(std::move(r));
  return s;
}

void Validator::check_column_checks(const std::vector<sql::ColumnDef>& defs,
                                    const std::vector<Column>& columns) {
  const Scope row = row_scope(columns);
  for (const sql::ColumnDef& def : defs) {
    for (const sql::ColumnConstraint& c : def.constraints) {
      if (c.kind == sql::ColumnConstraint::Kind::Check && c.value != nullptr) {
        expect_condition(c.value.get(), row, "CHECK constraint");
      }
    }
  }
}

void Validator::check_create_table(const sql::CreateTableStatement& s) {
  const bool exists =
      catalog_.find(s.name) != nullptr || catalog_.has_index(s.name);
  if (exists) {
    if (s.if_not_exists) return;
    error("table " + q(s.name) + " already exists");
    return;
  }

  Table table;
  table.name = s.name;
  if (s.select != nullptr) {
    Scope empty;
    table.columns = validate_query(*s.select, empty);
    if (table.columns.empty()) {
      error("query for CREATE TABLE " + q(s.name) + " returns no columns");
      return;
    }
  } else {
    if (s.columns.empty()) {
      error("table " + q(s.name) + " must declare at least one column");
      return;
    }
    for (const sql::ColumnDef& def : s.columns) {
      for (const Column& c : table.columns) {
        if (ci_eq(c.name, def.name)) {
          error("duplicate column " + q(def.name));
          break;
        }
      }
      table.columns.push_back(build_column(def, s.name));
    }
    check_column_checks(s.columns, table.columns);
    for (const sql::TableConstraint& tc : s.constraints) {
      check_table_constraint(tc, table.columns);
    }
  }

  if (!catalog_.insert(std::move(table))) {
    error("table " + q(s.name) + " already exists");
  }
}

void Validator::check_create_index(const sql::CreateIndexStatement& s) {
  if (catalog_.has_index(s.name) || catalog_.find(s.name) != nullptr) {
    if (s.if_not_exists) return;
    error("index " + q(s.name) + " already exists");
    return;
  }
  const Table* table = catalog_.find(s.table);
  if (table == nullptr) {
    error("unknown table " + q(s.table));
    return;
  }
  if (table->system()) {
    error("cannot index system table " + q(table->name));
    return;
  }
  if (table->kind == TableKind::View) {
    error("cannot index view " + q(table->name));
    return;
  }
  for (const sql::IndexColumn& ic : s.columns) {
    const sql::Expr* e = &ic.expr;
    if (e->kind == sql::Expr::Kind::Collate && !e->args.empty()) {
      e = e->args[0].get();
    }
    if (e == nullptr || e->kind != sql::Expr::Kind::ColumnRef ||
        e->parts.size() != 1) {
      error("index column must be a simple column name");
      continue;
    }
    bool found = false;
    for (const Column& c : table->columns) {
      if (ci_eq(c.name, e->parts[0])) {
        found = true;
        break;
      }
    }
    if (!found) error("unknown column " + q(e->parts[0]));
  }
  if (!catalog_.add_index(s.name)) error("index " + q(s.name) + " already exists");
}

void Validator::check_create_view(const sql::CreateViewStatement& s) {
  const Table* existing = catalog_.find(s.name);
  const bool index_clash = catalog_.has_index(s.name);
  if (existing != nullptr || index_clash) {
    const bool replaceable = s.or_replace && existing != nullptr &&
                             existing->kind == TableKind::View && !index_clash;
    if (!replaceable) {
      if (s.if_not_exists) return;
      if (existing != nullptr && existing->kind == TableKind::Table) {
        error("cannot replace non-view " + q(s.name));
      } else {
        error("table " + q(s.name) + " already exists");
      }
      return;
    }
  }

  Scope empty;
  std::vector<Column> cols;
  if (s.query != nullptr) cols = validate_query(*s.query, empty);
  if (!s.columns.empty()) {
    if (s.columns.size() != cols.size()) {
      error("view " + q(s.name) + " declares " +
            std::to_string(s.columns.size()) +
            " column(s) but its query returns " + std::to_string(cols.size()));
    } else {
      for (std::size_t i = 0; i < cols.size(); ++i) {
        cols[i].name = s.columns[i];
      }
    }
  }

  Table view;
  view.name = s.name;
  view.kind = TableKind::View;
  view.columns = std::move(cols);
  if (!catalog_.insert(std::move(view), s.or_replace)) {
    error("table " + q(s.name) + " already exists");
  }
}

void Validator::check_drop(const sql::DropStatement& s) {
  const std::string word = lower(s.object_type);
  for (const std::string& name : s.names) {
    if (s.object_type == "INDEX") {
      if (catalog_.has_index(name)) {
        catalog_.erase_index(name);
        continue;
      }
      if (catalog_.find(name) != nullptr) {
        error(q(name) + " is not an index");
        continue;
      }
      if (!s.if_exists) error("index " + q(name) + " does not exist");
      continue;
    }

    const Table* table = catalog_.find(name);
    if (table == nullptr) {
      if (!s.if_exists) error(word + " " + q(name) + " does not exist");
      continue;
    }
    if (table->system()) {
      error("system table " + q(name) + " cannot be dropped");
      continue;
    }
    if (s.object_type == "TABLE" && table->kind == TableKind::View) {
      error(q(name) + " is a view, not a table");
      continue;
    }
    if (s.object_type == "VIEW" && table->kind == TableKind::Table) {
      error(q(name) + " is a table, not a view");
      continue;
    }
    catalog_.erase(name);
  }
}

void Validator::check_alter(const sql::AlterTableStatement& s) {
  Table* table = catalog_.find_mut(s.table);
  if (table == nullptr) {
    if (s.if_exists) return;
    error("table " + q(s.table) + " does not exist");
    return;
  }
  if (table->system()) {
    error("system table " + q(s.table) + " cannot be altered");
    return;
  }

  switch (s.action) {
    case sql::AlterTableStatement::Action::AddColumn: {
      for (const Column& c : table->columns) {
        if (ci_eq(c.name, s.column.name)) {
          error("column " + q(s.column.name) + " already exists");
          return;
        }
      }
      table->columns.push_back(build_column(s.column, table->name));
      check_column_checks({s.column}, table->columns);
      break;
    }
    case sql::AlterTableStatement::Action::DropColumn: {
      auto it = std::find_if(
          table->columns.begin(), table->columns.end(),
          [&](const Column& c) { return ci_eq(c.name, s.column_name); });
      if (it == table->columns.end()) {
        if (!s.column_if_exists) {
          error("column " + q(s.column_name) + " does not exist in table " +
                q(table->name));
        }
        return;
      }
      table->columns.erase(it);
      break;
    }
    case sql::AlterTableStatement::Action::RenameTable: {
      if (catalog_.find(s.new_name) != nullptr ||
          catalog_.has_index(s.new_name)) {
        error("table " + q(s.new_name) + " already exists");
        return;
      }
      Table moved = std::move(*table);
      catalog_.erase(s.table);
      moved.name = s.new_name;
      catalog_.insert(std::move(moved));
      break;
    }
    case sql::AlterTableStatement::Action::RenameColumn: {
      auto it = std::find_if(
          table->columns.begin(), table->columns.end(),
          [&](const Column& c) { return ci_eq(c.name, s.column_name); });
      if (it == table->columns.end()) {
        error("column " + q(s.column_name) + " does not exist in table " +
              q(table->name));
        return;
      }
      for (const Column& c : table->columns) {
        if (&c != &*it && ci_eq(c.name, s.new_name)) {
          error("column " + q(s.new_name) + " already exists");
          return;
        }
      }
      it->name = s.new_name;
      break;
    }
  }
}

// --- dispatch ----------------------------------------------------------------

void Validator::check_statement(const sql::Statement& statement) {
  if (const auto* s = std::get_if<sql::SelectStatement>(&statement)) {
    check_select(*s);
  } else if (const auto* s = std::get_if<sql::InsertStatement>(&statement)) {
    check_insert(*s);
  } else if (const auto* s = std::get_if<sql::UpdateStatement>(&statement)) {
    check_update(*s);
  } else if (const auto* s = std::get_if<sql::DeleteStatement>(&statement)) {
    check_delete(*s);
  } else if (const auto* s = std::get_if<sql::CreateTableStatement>(&statement)) {
    check_create_table(*s);
  } else if (const auto* s = std::get_if<sql::CreateIndexStatement>(&statement)) {
    check_create_index(*s);
  } else if (const auto* s = std::get_if<sql::CreateViewStatement>(&statement)) {
    check_create_view(*s);
  } else if (const auto* s = std::get_if<sql::DropStatement>(&statement)) {
    check_drop(*s);
  } else if (const auto* s = std::get_if<sql::AlterTableStatement>(&statement)) {
    check_alter(*s);
  }
  // TransactionStatement: nothing to check.
}

ValidationResult Validator::run(const std::vector<sql::Statement>& statements,
                                const std::vector<SourcePos>& positions) {
  for (std::size_t i = 0; i < statements.size(); ++i) {
    pos_ = i < positions.size() ? positions[i] : SourcePos{};
    check_statement(statements[i]);
  }
  ValidationResult result;
  result.issues = std::move(issues_);
  result.catalog = std::move(catalog_);
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

ValidationResult validate(const std::vector<sql::Statement>& statements,
                          const std::vector<SourcePos>& positions) {
  Validator validator;
  return validator.run(statements, positions);
}

ValidationResult validate_source(std::string_view source) {
  std::vector<sql::Statement> statements;
  try {
    statements = sql::Parser(source).parse_all();
  } catch (const sql::ParseError& error) {
    ValidationResult result;
    Issue issue;
    issue.pos.line = error.line();
    issue.pos.column = error.column();
    const std::string what = error.what();
    const std::size_t split = what.find(": ");
    issue.message = split == std::string::npos ? what : what.substr(split + 2);
    result.issues.push_back(std::move(issue));
    return result;
  }
  return validate(statements, statement_positions(source));
}

std::string format_issue(const Issue& issue) {
  return "line " + std::to_string(issue.pos.line) + ":" +
         std::to_string(issue.pos.column) + ": " + issue.message;
}

}  // namespace sqlos
