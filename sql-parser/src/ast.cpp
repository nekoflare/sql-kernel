// Value rendering, precedence tables and structural equality for the AST.

#include "sql_parser/ast.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace sql {
namespace {

// Renders a double with the fewest significant digits that read back to the
// same value, and always in a form that lexes as a floating-point token
// (so 1500.0 does not turn into the integer 1500 on a round trip).
std::string format_double(double value) {
  for (int precision = 1; precision <= 17; ++precision) {
    std::ostringstream out;
    out << std::setprecision(precision) << value;
    std::string text = out.str();
    try {
      if (std::stod(text) != value) continue;
    } catch (...) {
      continue;
    }
    if (text.find('.') == std::string::npos &&
        text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
      text += ".0";
    }
    return text;
  }
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return out.str();
}

struct ValuePrinter {
  std::string operator()(std::nullptr_t) const { return "NULL"; }
  std::string operator()(long long value) const { return std::to_string(value); }
  std::string operator()(double value) const { return format_double(value); }
  std::string operator()(const std::string& value) const {
    std::string out = "'";
    for (char c : value) {
      if (c == '\'') {
        out += "''";
      } else {
        out.push_back(c);
      }
    }
    out.push_back('\'');
    return out;
  }
  std::string operator()(bool value) const { return value ? "TRUE" : "FALSE"; }
};

// --- structural equality ---------------------------------------------------

bool expr_ptr_equal(const ExprPtr& a, const ExprPtr& b);
bool expr_equal(const Expr& a, const Expr& b);
bool select_equal(const SelectStatement& a, const SelectStatement& b);

template <typename T>
bool ptr_equal(const std::shared_ptr<T>& a, const std::shared_ptr<T>& b,
               bool (*compare)(const T&, const T&)) {
  if ((a == nullptr) != (b == nullptr)) return false;
  if (a == nullptr) return true;
  return compare(*a, *b);
}

bool expr_ptr_equal(const ExprPtr& a, const ExprPtr& b) {
  if ((a == nullptr) != (b == nullptr)) return false;
  if (a == nullptr) return true;
  return expr_equal(*a, *b);
}

bool expr_equal(const Expr& a, const Expr& b) {
  if (a.kind != b.kind) return false;
  if (a.literal != b.literal) return false;
  if (a.name != b.name) return false;
  if (a.parts != b.parts) return false;
  if (a.type_name != b.type_name) return false;
  if (a.negated != b.negated || a.distinct != b.distinct ||
      a.star != b.star) {
    return false;
  }
  if (a.args.size() != b.args.size()) return false;
  for (std::size_t i = 0; i < a.args.size(); ++i) {
    if (!expr_ptr_equal(a.args[i], b.args[i])) return false;
  }
  if (a.results.size() != b.results.size()) return false;
  for (std::size_t i = 0; i < a.results.size(); ++i) {
    if (!expr_ptr_equal(a.results[i], b.results[i])) return false;
  }
  if (!expr_ptr_equal(a.else_result, b.else_result)) return false;
  if (!expr_ptr_equal(a.operand, b.operand)) return false;
  if (!ptr_equal(a.subquery, b.subquery, select_equal)) return false;

  if ((a.over == nullptr) != (b.over == nullptr)) return false;
  if (a.over != nullptr) {
    const WindowSpec& x = *a.over;
    const WindowSpec& y = *b.over;
    if (x.partition_by.size() != y.partition_by.size()) return false;
    for (std::size_t i = 0; i < x.partition_by.size(); ++i) {
      if (!expr_equal(x.partition_by[i], y.partition_by[i])) return false;
    }
    if (x.order_by.size() != y.order_by.size()) return false;
    for (std::size_t i = 0; i < x.order_by.size(); ++i) {
      if (!expr_equal(x.order_by[i].expr, y.order_by[i].expr)) return false;
      if (x.order_by[i].descending != y.order_by[i].descending) return false;
      if (x.order_by[i].nulls != y.order_by[i].nulls) return false;
    }
    if (x.frame_type != y.frame_type || x.frame_between != y.frame_between ||
        x.frame_start != y.frame_start || x.frame_end != y.frame_end) {
      return false;
    }
    if (!expr_ptr_equal(x.frame_start_expr, y.frame_start_expr)) return false;
    if (!expr_ptr_equal(x.frame_end_expr, y.frame_end_expr)) return false;
  }
  return true;
}

bool order_by_equal(const OrderByItem& a, const OrderByItem& b) {
  return a.descending == b.descending && a.nulls == b.nulls &&
         expr_equal(a.expr, b.expr);
}

bool select_item_equal(const SelectItem& a, const SelectItem& b) {
  return a.alias == b.alias && expr_equal(a.expr, b.expr);
}

bool table_ref_equal(const TableRef& a, const TableRef& b);

bool cte_equal(const CommonTableExpr& a, const CommonTableExpr& b) {
  return a.name == b.name && a.columns == b.columns &&
         a.not_materialized == b.not_materialized &&
         ptr_equal(a.query, b.query, select_equal);
}

bool table_ref_equal(const TableRef& a, const TableRef& b) {
  if (a.kind != b.kind) return false;
  if (a.name != b.name || a.alias != b.alias ||
      a.column_aliases != b.column_aliases) {
    return false;
  }
  if (!ptr_equal(a.subquery, b.subquery, select_equal)) return false;
  if ((a.function == nullptr) != (b.function == nullptr)) return false;
  if (a.function != nullptr && !expr_equal(*a.function, *b.function)) {
    return false;
  }
  if (a.join_type != b.join_type || a.natural != b.natural ||
      a.parenthesized != b.parenthesized) {
    return false;
  }
  if (a.kind != TableRef::Kind::Join) return true;
  if (a.using_columns != b.using_columns) return false;
  if ((a.on == nullptr) != (b.on == nullptr)) return false;
  if (a.on != nullptr && !expr_equal(*a.on, *b.on)) return false;
  return ptr_equal(a.left, b.left, table_ref_equal) &&
         ptr_equal(a.right, b.right, table_ref_equal);
}

bool select_equal(const SelectStatement& a, const SelectStatement& b) {
  if (a.recursive != b.recursive || a.with.size() != b.with.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.with.size(); ++i) {
    if (!cte_equal(a.with[i], b.with[i])) return false;
  }

  if (a.distinct != b.distinct || a.all != b.all ||
      a.is_values != b.is_values) {
    return false;
  }
  if (a.distinct_on.size() != b.distinct_on.size()) return false;
  for (std::size_t i = 0; i < a.distinct_on.size(); ++i) {
    if (!expr_equal(a.distinct_on[i], b.distinct_on[i])) return false;
  }
  if (a.columns.size() != b.columns.size()) return false;
  for (std::size_t i = 0; i < a.columns.size(); ++i) {
    if (!select_item_equal(a.columns[i], b.columns[i])) return false;
  }
  if (a.value_rows.size() != b.value_rows.size()) return false;
  for (std::size_t i = 0; i < a.value_rows.size(); ++i) {
    if (a.value_rows[i].size() != b.value_rows[i].size()) return false;
    for (std::size_t j = 0; j < a.value_rows[i].size(); ++j) {
      if (!expr_equal(a.value_rows[i][j], b.value_rows[i][j])) return false;
    }
  }
  if (a.from.size() != b.from.size()) return false;
  for (std::size_t i = 0; i < a.from.size(); ++i) {
    if (!table_ref_equal(a.from[i], b.from[i])) return false;
  }
  if (!ptr_equal(a.where, b.where, expr_equal)) return false;
  if (a.group_all != b.group_all || a.group_distinct != b.group_distinct) {
    return false;
  }
  if (a.group_by.size() != b.group_by.size()) return false;
  for (std::size_t i = 0; i < a.group_by.size(); ++i) {
    if (!expr_equal(a.group_by[i], b.group_by[i])) return false;
  }
  if (!ptr_equal(a.having, b.having, expr_equal)) return false;

  if (a.set_op != b.set_op || a.set_all != b.set_all) return false;
  if (!ptr_equal(a.set_left, b.set_left, select_equal)) return false;
  if (!ptr_equal(a.set_right, b.set_right, select_equal)) return false;

  if (a.order_by.size() != b.order_by.size()) return false;
  for (std::size_t i = 0; i < a.order_by.size(); ++i) {
    if (!order_by_equal(a.order_by[i], b.order_by[i])) return false;
  }
  if (!ptr_equal(a.limit, b.limit, expr_equal)) return false;
  if (a.limit_all != b.limit_all) return false;
  return ptr_equal(a.offset, b.offset, expr_equal);
}

bool upsert_equal(const UpsertClause& a, const UpsertClause& b) {
  if (a.columns != b.columns || a.do_nothing != b.do_nothing) return false;
  if (!ptr_equal(a.target_where, b.target_where, expr_equal)) return false;
  if (!ptr_equal(a.update_where, b.update_where, expr_equal)) return false;
  if (a.updates.size() != b.updates.size()) return false;
  for (std::size_t i = 0; i < a.updates.size(); ++i) {
    if (a.updates[i].first != b.updates[i].first) return false;
    if (!expr_ptr_equal(a.updates[i].second, b.updates[i].second)) {
      return false;
    }
  }
  return true;
}

bool column_constraint_equal(const ColumnConstraint& a,
                             const ColumnConstraint& b) {
  if (a.kind != b.kind || a.name != b.name || a.collation != b.collation ||
      a.ref_table != b.ref_table || a.ref_columns != b.ref_columns ||
      a.ref_on_delete != b.ref_on_delete ||
      a.ref_on_update != b.ref_on_update || a.deferrable != b.deferrable ||
      a.initially_deferred != b.initially_deferred) {
    return false;
  }
  return ptr_equal(a.value, b.value, expr_equal);
}

bool column_def_equal(const ColumnDef& a, const ColumnDef& b) {
  if (a.name != b.name || a.type_name != b.type_name) return false;
  if (a.constraints.size() != b.constraints.size()) return false;
  for (std::size_t i = 0; i < a.constraints.size(); ++i) {
    if (!column_constraint_equal(a.constraints[i], b.constraints[i])) {
      return false;
    }
  }
  return true;
}

bool table_constraint_equal(const TableConstraint& a,
                            const TableConstraint& b) {
  if (a.kind != b.kind || a.name != b.name || a.columns != b.columns ||
      a.ref_table != b.ref_table || a.ref_columns != b.ref_columns ||
      a.ref_on_delete != b.ref_on_delete ||
      a.ref_on_update != b.ref_on_update || a.deferrable != b.deferrable ||
      a.initially_deferred != b.initially_deferred) {
    return false;
  }
  return ptr_equal(a.check, b.check, expr_equal);
}

bool index_column_equal(const IndexColumn& a, const IndexColumn& b) {
  return a.descending == b.descending && expr_equal(a.expr, b.expr);
}

bool pair_list_equal(
    const std::vector<std::pair<std::string, ExprPtr>>& a,
    const std::vector<std::pair<std::string, ExprPtr>>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].first != b[i].first) return false;
    if (!expr_ptr_equal(a[i].second, b[i].second)) return false;
  }
  return true;
}

bool with_equal(const std::vector<CommonTableExpr>& a,
                const std::vector<CommonTableExpr>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!cte_equal(a[i], b[i])) return false;
  }
  return true;
}

}  // namespace

std::string to_string(const Value& value) {
  return std::visit(ValuePrinter{}, value);
}

int binary_precedence(std::string_view op) {
  if (op == "OR") return prec::Or;
  if (op == "AND") return prec::And;
  if (op == "IS DISTINCT FROM" || op == "IS NOT DISTINCT FROM") {
    return prec::Is;
  }
  if (op == "=" || op == "<>" || op == "!=" || op == "<" || op == "<=" ||
      op == ">" || op == ">=") {
    return prec::Comparison;
  }
  if (op == "^") return prec::Exponent;
  if (op == "*" || op == "/" || op == "%") return prec::Multiplicative;
  if (op == "+" || op == "-") return prec::Additive;
  if (op == "||" || op == "&" || op == "|" || op == "<<" || op == ">>") {
    return prec::OtherOperator;
  }
  return 0;
}

int expression_precedence(const Expr& expr) {
  switch (expr.kind) {
    case Expr::Kind::Cast:
      return prec::Cast;
    case Expr::Kind::Collate:
      return prec::Collate;
    case Expr::Kind::Unary:
      return expr.name == "NOT" ? prec::Not : prec::Unary;
    case Expr::Kind::NullTest:
    case Expr::Kind::BoolTest:
      return prec::Is;
    case Expr::Kind::In:
    case Expr::Kind::Like:
    case Expr::Kind::Between:
      return prec::Range;
    case Expr::Kind::Quantified:
      return prec::Comparison;
    case Expr::Kind::Binary: {
      const int precedence = binary_precedence(expr.name);
      return precedence != 0 ? precedence : prec::OtherOperator;
    }
    default:
      return prec::Primary;
  }
}

bool operator==(const Expr& a, const Expr& b) { return expr_equal(a, b); }
bool operator!=(const Expr& a, const Expr& b) { return !expr_equal(a, b); }

bool operator==(const SelectStatement& a, const SelectStatement& b) {
  return select_equal(a, b);
}
bool operator!=(const SelectStatement& a, const SelectStatement& b) {
  return !select_equal(a, b);
}

bool operator==(const InsertStatement& a, const InsertStatement& b) {
  if (a.recursive != b.recursive || a.insert_or != b.insert_or ||
      a.table != b.table || a.columns != b.columns ||
      a.default_values != b.default_values) {
    return false;
  }
  if (a.with.size() != b.with.size()) return false;
  for (std::size_t i = 0; i < a.with.size(); ++i) {
    if (!cte_equal(a.with[i], b.with[i])) return false;
  }
  if (a.rows.size() != b.rows.size()) return false;
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    if (a.rows[i].size() != b.rows[i].size()) return false;
    for (std::size_t j = 0; j < a.rows[i].size(); ++j) {
      if (!expr_equal(a.rows[i][j], b.rows[i][j])) return false;
    }
  }
  if (!ptr_equal(a.select, b.select, select_equal)) return false;
  if ((a.upsert == nullptr) != (b.upsert == nullptr)) return false;
  if (a.upsert != nullptr && !upsert_equal(*a.upsert, *b.upsert)) return false;
  if (a.returning.size() != b.returning.size()) return false;
  for (std::size_t i = 0; i < a.returning.size(); ++i) {
    if (!select_item_equal(a.returning[i], b.returning[i])) return false;
  }
  return true;
}

bool operator!=(const InsertStatement& a, const InsertStatement& b) {
  return !(a == b);
}

bool operator==(const UpdateStatement& a, const UpdateStatement& b) {
  if (a.recursive != b.recursive || a.table != b.table || a.alias != b.alias) {
    return false;
  }
  if (!with_equal(a.with, b.with)) return false;
  if (!pair_list_equal(a.assignments, b.assignments)) return false;
  if (a.from.size() != b.from.size()) return false;
  for (std::size_t i = 0; i < a.from.size(); ++i) {
    if (!table_ref_equal(a.from[i], b.from[i])) return false;
  }
  if (!ptr_equal(a.where, b.where, expr_equal)) return false;
  if (a.returning.size() != b.returning.size()) return false;
  for (std::size_t i = 0; i < a.returning.size(); ++i) {
    if (!select_item_equal(a.returning[i], b.returning[i])) return false;
  }
  return true;
}
bool operator!=(const UpdateStatement& a, const UpdateStatement& b) {
  return !(a == b);
}

bool operator==(const DeleteStatement& a, const DeleteStatement& b) {
  if (a.recursive != b.recursive || a.table != b.table || a.alias != b.alias) {
    return false;
  }
  if (!with_equal(a.with, b.with)) return false;
  if (!ptr_equal(a.where, b.where, expr_equal)) return false;
  if (a.returning.size() != b.returning.size()) return false;
  for (std::size_t i = 0; i < a.returning.size(); ++i) {
    if (!select_item_equal(a.returning[i], b.returning[i])) return false;
  }
  return true;
}
bool operator!=(const DeleteStatement& a, const DeleteStatement& b) {
  return !(a == b);
}

bool operator==(const CreateTableStatement& a, const CreateTableStatement& b) {
  if (a.temporary != b.temporary || a.if_not_exists != b.if_not_exists ||
      a.name != b.name) {
    return false;
  }
  if (a.columns.size() != b.columns.size()) return false;
  for (std::size_t i = 0; i < a.columns.size(); ++i) {
    if (!column_def_equal(a.columns[i], b.columns[i])) return false;
  }
  if (a.constraints.size() != b.constraints.size()) return false;
  for (std::size_t i = 0; i < a.constraints.size(); ++i) {
    if (!table_constraint_equal(a.constraints[i], b.constraints[i])) {
      return false;
    }
  }
  return ptr_equal(a.select, b.select, select_equal);
}
bool operator!=(const CreateTableStatement& a, const CreateTableStatement& b) {
  return !(a == b);
}

bool operator==(const CreateIndexStatement& a, const CreateIndexStatement& b) {
  if (a.unique != b.unique || a.if_not_exists != b.if_not_exists ||
      a.name != b.name || a.table != b.table ||
      a.columns.size() != b.columns.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.columns.size(); ++i) {
    if (!index_column_equal(a.columns[i], b.columns[i])) return false;
  }
  return true;
}
bool operator!=(const CreateIndexStatement& a, const CreateIndexStatement& b) {
  return !(a == b);
}

bool operator==(const CreateViewStatement& a, const CreateViewStatement& b) {
  return a.or_replace == b.or_replace && a.temporary == b.temporary &&
         a.if_not_exists == b.if_not_exists && a.name == b.name &&
         a.columns == b.columns &&
         ptr_equal(a.query, b.query, select_equal);
}
bool operator!=(const CreateViewStatement& a, const CreateViewStatement& b) {
  return !(a == b);
}

bool operator==(const DropStatement& a, const DropStatement& b) {
  return a.object_type == b.object_type && a.if_exists == b.if_exists &&
         a.names == b.names && a.option == b.option;
}
bool operator!=(const DropStatement& a, const DropStatement& b) {
  return !(a == b);
}

bool operator==(const AlterTableStatement& a, const AlterTableStatement& b) {
  return a.action == b.action && a.if_exists == b.if_exists &&
         a.column_if_exists == b.column_if_exists && a.table == b.table &&
         column_def_equal(a.column, b.column) &&
         a.column_name == b.column_name && a.new_name == b.new_name &&
         a.change == b.change && ptr_equal(a.default_value, b.default_value,
                                           expr_equal);
}
bool operator!=(const AlterTableStatement& a, const AlterTableStatement& b) {
  return !(a == b);
}

bool operator==(const TransactionStatement& a, const TransactionStatement& b) {
  return a.action == b.action;
}
bool operator!=(const TransactionStatement& a, const TransactionStatement& b) {
  return !(a == b);
}

}  // namespace sql
