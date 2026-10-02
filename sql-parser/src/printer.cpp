// Canonical SQL rendering of every AST node. Parentheses are inserted only
// where operator precedence (expr.hpp) requires them, so that re-parsing the
// output yields an equal tree.

#include "sql_parser/ast.hpp"
#include "sql_parser/lexer.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace sql {
namespace {

std::string print_expr(const Expr& expr, int min_precedence);
std::string print_with(const std::vector<CommonTableExpr>& ctes,
                       bool recursive);
std::string print_items(const std::vector<SelectItem>& items);
std::string print_order_by(const std::vector<OrderByItem>& items);
std::string print_table_ref(const TableRef& ref);
std::string print_column_def(const ColumnDef& column);

std::string join_exprs(const std::vector<Expr>& exprs,
                       const std::string& separator) {
  std::string out;
  for (std::size_t i = 0; i < exprs.size(); ++i) {
    if (i > 0) out += separator;
    out += print_expr(exprs[i], 0);
  }
  return out;
}

// --- identifier rendering ---------------------------------------------------
// Identifiers are printed bare when they re-lex as the same token and quoted
// otherwise: names with spaces or unusual characters, and names that are
// reserved keywords in any letter case ("order", "Order", "select") must be
// quoted for the output to parse back into an equal tree.

bool identifier_needs_quotes(const std::string& name) {
  if (name.empty()) return true;
  const char first = name[0];
  if (std::isalpha(static_cast<unsigned char>(first)) == 0 && first != '_') {
    return true;
  }
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) == 0 && c != '_' && c != '$') return true;
  }
  return is_reserved_keyword_word(name);
}

std::string quote_identifier(const std::string& name) {
  if (!identifier_needs_quotes(name)) return name;
  std::string out = "\"";
  for (char c : name) {
    out.push_back(c);
    if (c == '"') out.push_back('"');
  }
  out.push_back('"');
  return out;
}

// Schema-qualified names are stored as one "a.b" string; quote each part.
std::string quote_dotted(const std::string& name) {
  if (name.empty()) return quote_identifier(name);
  std::string out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t dot = name.find('.', start);
    const std::string part =
        name.substr(start, dot == std::string::npos ? std::string::npos
                                                    : dot - start);
    if (!out.empty()) out += ".";
    out += quote_identifier(part);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return out;
}

std::string quote_names(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += ".";
    out += quote_identifier(parts[i]);
  }
  return out;
}

// Comma-separated identifier list: column lists in DDL/DML.
std::string quote_name_list(const std::vector<std::string>& names) {
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += ", ";
    out += quote_identifier(names[i]);
  }
  return out;
}

// Type names are raw SQL fragments ("VARCHAR(255)", "DOUBLE PRECISION").
// Quote the whole fragment only when it would not re-lex identically: a
// reserved or malformed first word, a reserved later word, or a stray
// character that cannot appear in a type at all.
std::string quote_type_name(const std::string& type) {
  if (type.empty()) return type;
  bool needs = false;
  bool first_word = true;
  std::size_t word_start = std::string::npos;
  for (std::size_t i = 0; i <= type.size(); ++i) {
    const char c = i < type.size() ? type[i] : ' ';
    const unsigned char u = static_cast<unsigned char>(c);
    const bool word_char = std::isalnum(u) != 0 || c == '_' || c == '$';
    if (i < type.size() && word_char) {
      if (word_start == std::string::npos) word_start = i;
      continue;
    }
    if (word_start != std::string::npos) {
      const std::string word = type.substr(word_start, i - word_start);
      needs = first_word ? identifier_needs_quotes(word)
                         : is_reserved_keyword_word(word);
      first_word = false;
      word_start = std::string::npos;
      if (needs) break;
    }
    if (i < type.size() && !(c == ' ' || c == '(' || c == ')' || c == ',')) {
      needs = true;  // punctuation that only a quoted type could contain
      break;
    }
  }
  if (!needs) return type;
  std::string out = "\"";
  for (char c : type) {
    out.push_back(c);
    if (c == '"') out.push_back('"');
  }
  out.push_back('"');
  return out;
}

std::string print_frame_point(const std::string& kind, const ExprPtr& expr) {
  if (expr == nullptr) return kind;
  return kind + " " + print_expr(*expr, prec::OtherOperator);
}

std::string print_window(const WindowSpec& spec) {
  std::string out = "(";
  bool need_space = false;
  if (!spec.partition_by.empty()) {
    out += "PARTITION BY " + join_exprs(spec.partition_by, ", ");
    need_space = true;
  }
  if (!spec.order_by.empty()) {
    if (need_space) out += " ";
    out += "ORDER BY " + print_order_by(spec.order_by);
    need_space = true;
  }
  if (!spec.frame_type.empty()) {
    if (need_space) out += " ";
    out += spec.frame_type;
    if (spec.frame_between) {
      out += " BETWEEN " + print_frame_point(spec.frame_start,
                                             spec.frame_start_expr);
      out += " AND " + print_frame_point(spec.frame_end, spec.frame_end_expr);
    } else {
      out += " " + print_frame_point(spec.frame_start, spec.frame_start_expr);
    }
  }
  out += ")";
  return out;
}

std::string render_expr(const Expr& expr) {
  switch (expr.kind) {
    case Expr::Kind::Literal:
      return to_string(expr.literal);

    case Expr::Kind::Parameter:
      return expr.name;

    case Expr::Kind::ColumnRef:
      return quote_names(expr.parts);

    case Expr::Kind::Star:
      return expr.parts.empty() ? "*" : quote_names(expr.parts) + ".*";

    case Expr::Kind::Unary: {
      const bool is_not = expr.name == "NOT";
      std::string child =
          print_expr(*expr.args[0], is_not ? prec::Not : prec::Unary);
      if (!is_not && !child.empty()) {
        const bool leading_sign = child[0] == '-' || child[0] == '+';
        // A leading '-' would start a "--" comment; a leading sign or digit
        // would fold into the literal on reparse ("-(1)" must not print as
        // "-1", which parses back as a literal rather than a unary node).
        const bool foldable = leading_sign ||
                              child[0] == '.' ||
                              (child[0] >= '0' && child[0] <= '9');
        if (foldable && expr.name != "~") child = "(" + child + ")";
      }
      return is_not ? "NOT " + child : expr.name + child;
    }

    case Expr::Kind::Binary: {
      const int own = expression_precedence(expr);
      return print_expr(*expr.args[0], own) + " " + expr.name + " " +
             print_expr(*expr.args[1], own + 1);
    }

    case Expr::Kind::NullTest:
      return print_expr(*expr.args[0], prec::Is) +
             (expr.negated ? " IS NOT NULL" : " IS NULL");

    case Expr::Kind::BoolTest:
      return print_expr(*expr.args[0], prec::Is) + " IS " +
             (expr.negated ? "NOT " : "") + expr.name;

    case Expr::Kind::In: {
      std::string out =
          print_expr(*expr.args[0], prec::Range) + (expr.negated ? " NOT" : "") +
          " IN (";
      if (expr.subquery != nullptr) {
        out += format(*expr.subquery);
      } else {
        for (std::size_t i = 1; i < expr.args.size(); ++i) {
          if (i > 1) out += ", ";
          out += print_expr(*expr.args[i], 0);
        }
      }
      return out + ")";
    }

    case Expr::Kind::Like: {
      std::string out = print_expr(*expr.args[0], prec::Range) +
                        (expr.negated ? " NOT" : "") + " " + expr.name + " " +
                        print_expr(*expr.args[1], prec::Range + 1);
      if (expr.args.size() > 2) {
        out += " ESCAPE " + print_expr(*expr.args[2], prec::Range + 1);
      }
      return out;
    }

    case Expr::Kind::Between:
      return print_expr(*expr.args[0], prec::Range) +
             (expr.negated ? " NOT" : "") + " BETWEEN " +
             print_expr(*expr.args[1], prec::Range + 1) + " AND " +
             print_expr(*expr.args[2], prec::Range + 1);

    case Expr::Kind::Exists:
      return std::string(expr.negated ? "NOT " : "") + "EXISTS (" +
             format(*expr.subquery) + ")";

    case Expr::Kind::Subquery:
      return "(" + format(*expr.subquery) + ")";

    case Expr::Kind::FunctionCall: {
      std::string out = quote_names(expr.parts) + "(";
      if (expr.star) {
        out += "*";
      } else {
        if (expr.distinct) out += "DISTINCT ";
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
          if (i > 0) out += ", ";
          out += print_expr(*expr.args[i], 0);
        }
      }
      out += ")";
      if (expr.over != nullptr) {
        out += " OVER " + print_window(*expr.over);
      } else if (!expr.name.empty()) {
        out += " OVER " + quote_identifier(expr.name);
      }
      return out;
    }

    case Expr::Kind::Cast:
      return "CAST(" + print_expr(*expr.args[0], 0) + " AS " + expr.type_name +
             ")";

    case Expr::Kind::Collate:
      return print_expr(*expr.args[0], prec::Collate) + " COLLATE " +
             quote_identifier(expr.name);

    case Expr::Kind::Case: {
      std::string out = "CASE";
      if (expr.operand != nullptr) out += " " + print_expr(*expr.operand, 0);
      for (std::size_t i = 0; i < expr.args.size(); ++i) {
        out += " WHEN " + print_expr(*expr.args[i], 0);
        out += " THEN " + print_expr(*expr.results[i], 0);
      }
      if (expr.else_result != nullptr) {
        out += " ELSE " + print_expr(*expr.else_result, 0);
      }
      return out + " END";
    }

    case Expr::Kind::Row: {
      std::string out = "(";
      for (std::size_t i = 0; i < expr.args.size(); ++i) {
        if (i > 0) out += ", ";
        out += print_expr(*expr.args[i], 0);
      }
      return out + ")";
    }

    case Expr::Kind::Default:
      return "DEFAULT";

    case Expr::Kind::Subscript:
      return print_expr(*expr.args[0], prec::Primary) + "[" +
             print_expr(*expr.args[1], 0) + "]";

    case Expr::Kind::Quantified:
      return print_expr(*expr.args[0], prec::Comparison) + " " + expr.name +
             " " + expr.parts[0] + " " +
             print_expr(*expr.args[1], prec::Comparison + 1);
  }
  return "";
}

std::string print_expr(const Expr& expr, int min_precedence) {
  std::string body = render_expr(expr);
  if (expression_precedence(expr) < min_precedence) {
    return "(" + body + ")";
  }
  return body;
}

std::string print_with(const std::vector<CommonTableExpr>& ctes,
                       bool recursive) {
  if (ctes.empty()) return "";
  std::string out = "WITH ";
  if (recursive) out += "RECURSIVE ";
  for (std::size_t i = 0; i < ctes.size(); ++i) {
    if (i > 0) out += ", ";
    const CommonTableExpr& cte = ctes[i];
    out += quote_identifier(cte.name);
    if (!cte.columns.empty()) out += " (" + quote_name_list(cte.columns) + ")";
    out += " AS ";
    if (cte.not_materialized) out += "NOT MATERIALIZED ";
    out += "(" + format(*cte.query) + ")";
  }
  return out + " ";
}

std::string print_items(const std::vector<SelectItem>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ", ";
    out += print_expr(items[i].expr, 0);
    if (!items[i].alias.empty()) out += " AS " + quote_identifier(items[i].alias);
  }
  return out;
}

std::string print_order_by(const std::vector<OrderByItem>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ", ";
    out += print_expr(items[i].expr, 0);
    if (items[i].descending) out += " DESC";
    if (!items[i].nulls.empty()) out += " NULLS " + items[i].nulls;
  }
  return out;
}

int set_precedence(const std::string& op) {
  return op == "INTERSECT" ? 2 : 1;
}

// One operand of a compound query: parenthesised when precedence, an
// ORDER BY/LIMIT tail or a WITH clause would otherwise attach to the wrong
// level.
std::string print_set_operand(const SelectStatement& node, int own, bool left) {
  const bool has_tail = !node.order_by.empty() || node.limit != nullptr ||
                        node.limit_all || node.offset != nullptr;
  // A WITH clause must not attach to the enclosing statement, and an
  // ORDER BY/LIMIT tail belongs to the parenthesised sub-query only.
  const bool needs_parens =
      has_tail || !node.with.empty() ||
      (!node.set_op.empty() &&
       (left ? set_precedence(node.set_op) < own
             : set_precedence(node.set_op) <= own));
  const std::string text = format(node);
  return needs_parens ? "(" + text + ")" : text;
}

std::string print_table_ref(const TableRef& ref) {
  std::string body;
  switch (ref.kind) {
    case TableRef::Kind::Table:
      body = quote_dotted(ref.name);
      break;

    case TableRef::Kind::Subquery:
      body = "(" + format(*ref.subquery) + ")";
      break;

    case TableRef::Kind::TableFunction:
      body = render_expr(*ref.function);
      break;

    case TableRef::Kind::Join: {
      body = print_table_ref(*ref.left);
      if (ref.natural) body += " NATURAL";
      if (!ref.join_type.empty()) body += " " + ref.join_type;
      body += " JOIN " + print_table_ref(*ref.right);
      if (ref.on != nullptr) {
        body += " ON " + print_expr(*ref.on, 0);
      } else if (!ref.using_columns.empty()) {
        body += " USING (" + quote_name_list(ref.using_columns) + ")";
      }
      break;
    }
  }

  if (ref.parenthesized && ref.kind != TableRef::Kind::Subquery) {
    body = "(" + body + ")";
  }
  if (!ref.alias.empty()) body += " AS " + quote_identifier(ref.alias);
  if (!ref.column_aliases.empty()) {
    body += "(" + quote_name_list(ref.column_aliases) + ")";
  }
  return body;
}

std::string print_constraint(const ColumnConstraint& constraint) {
  std::string out;
  if (!constraint.name.empty()) {
    out += "CONSTRAINT " + quote_identifier(constraint.name) + " ";
  }

  switch (constraint.kind) {
    case ColumnConstraint::Kind::NotNull:
      out += "NOT NULL";
      break;
    case ColumnConstraint::Kind::Null:
      out += "NULL";
      break;
    case ColumnConstraint::Kind::PrimaryKey:
      out += "PRIMARY KEY";
      break;
    case ColumnConstraint::Kind::Unique:
      out += "UNIQUE";
      break;
    case ColumnConstraint::Kind::Check:
      out += "CHECK (" + print_expr(*constraint.value, 0) + ")";
      break;
    case ColumnConstraint::Kind::Default:
      out += "DEFAULT " + print_expr(*constraint.value, 0);
      break;
    case ColumnConstraint::Kind::References: {
      out += "REFERENCES " + quote_dotted(constraint.ref_table);
      if (!constraint.ref_columns.empty()) {
        out += " (" + quote_name_list(constraint.ref_columns) + ")";
      }
      if (!constraint.ref_on_delete.empty()) {
        out += " ON DELETE " + constraint.ref_on_delete;
      }
      if (!constraint.ref_on_update.empty()) {
        out += " ON UPDATE " + constraint.ref_on_update;
      }
      if (constraint.deferrable && constraint.initially_deferred) {
        out += " DEFERRABLE INITIALLY DEFERRED";
      } else if (constraint.deferrable) {
        out += " DEFERRABLE";
      } else if (constraint.initially_deferred) {
        out += " INITIALLY DEFERRED";
      }
      break;
    }
    case ColumnConstraint::Kind::Collate:
      out += "COLLATE " + quote_identifier(constraint.collation);
      break;
    case ColumnConstraint::Kind::AutoIncrement:
      out += "AUTOINCREMENT";
      break;
  }
  return out;
}

std::string print_column_def(const ColumnDef& column) {
  std::string out = quote_identifier(column.name);
  if (!column.type_name.empty()) out += " " + quote_type_name(column.type_name);
  for (const ColumnConstraint& constraint : column.constraints) {
    out += " " + print_constraint(constraint);
  }
  return out;
}

std::string print_table_constraint(const TableConstraint& constraint) {
  std::string out;
  if (!constraint.name.empty()) {
    out += "CONSTRAINT " + quote_identifier(constraint.name) + " ";
  }

  switch (constraint.kind) {
    case TableConstraint::Kind::PrimaryKey:
      out += "PRIMARY KEY (" + quote_name_list(constraint.columns) + ")";
      break;
    case TableConstraint::Kind::Unique:
      out += "UNIQUE (" + quote_name_list(constraint.columns) + ")";
      break;
    case TableConstraint::Kind::Check:
      out += "CHECK (" + print_expr(*constraint.check, 0) + ")";
      break;
    case TableConstraint::Kind::Foreign: {
      out += "FOREIGN KEY (" + quote_name_list(constraint.columns) +
             ") REFERENCES " + quote_dotted(constraint.ref_table);
      if (!constraint.ref_columns.empty()) {
        out += " (" + quote_name_list(constraint.ref_columns) + ")";
      }
      if (!constraint.ref_on_delete.empty()) {
        out += " ON DELETE " + constraint.ref_on_delete;
      }
      if (!constraint.ref_on_update.empty()) {
        out += " ON UPDATE " + constraint.ref_on_update;
      }
      if (constraint.deferrable && constraint.initially_deferred) {
        out += " DEFERRABLE INITIALLY DEFERRED";
      } else if (constraint.deferrable) {
        out += " DEFERRABLE";
      }
      break;
    }
  }
  return out;
}

struct StatementVisitor {
  template <typename T>
  std::string operator()(const T& statement) const {
    return format(statement);
  }
};

}  // namespace

std::string format(const Expr& expr) { return print_expr(expr, 0); }

std::string format(const Statement& statement) {
  return std::visit(StatementVisitor{}, statement);
}

std::string format(const SelectStatement& statement) {
  std::string out = print_with(statement.with, statement.recursive);

  if (!statement.set_op.empty()) {
    const int own = set_precedence(statement.set_op);
    out += print_set_operand(*statement.set_left, own, true);
    out += " " + statement.set_op;
    if (statement.set_all) out += " ALL";
    out += " " + print_set_operand(*statement.set_right, own, false);
  } else if (statement.is_values) {
    out += "VALUES";
    for (const auto& row : statement.value_rows) {
      if (&row != &statement.value_rows.front()) out += ",";
      out += " (";
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i > 0) out += ", ";
        out += print_expr(row[i], 0);
      }
      out += ")";
    }
  } else {
    out += "SELECT";
    if (statement.distinct) {
      out += " DISTINCT";
      if (!statement.distinct_on.empty()) {
        out += " ON (" + join_exprs(statement.distinct_on, ", ") + ")";
      }
    } else if (statement.all) {
      out += " ALL";
    }
    out += " " + print_items(statement.columns);

    if (!statement.from.empty()) {
      out += " FROM ";
      for (std::size_t i = 0; i < statement.from.size(); ++i) {
        if (i > 0) out += ", ";
        out += print_table_ref(statement.from[i]);
      }
    }
    if (statement.where != nullptr) {
      out += " WHERE " + print_expr(*statement.where, 0);
    }
    if (statement.group_all) {
      out += " GROUP BY ALL";
    } else if (statement.group_distinct) {
      out += " GROUP BY DISTINCT";
    } else if (!statement.group_by.empty()) {
      out += " GROUP BY " + join_exprs(statement.group_by, ", ");
    }
    if (statement.having != nullptr) {
      out += " HAVING " + print_expr(*statement.having, 0);
    }
  }

  if (!statement.order_by.empty()) {
    out += " ORDER BY " + print_order_by(statement.order_by);
  }
  if (statement.limit != nullptr) {
    out += " LIMIT " + print_expr(*statement.limit, 0);
  } else if (statement.limit_all) {
    out += " LIMIT ALL";
  }
  if (statement.offset != nullptr) {
    out += " OFFSET " + print_expr(*statement.offset, 0);
  }
  return out;
}

std::string format(const InsertStatement& statement) {
  std::string out = print_with(statement.with, statement.recursive);
  out += "INSERT ";
  if (!statement.insert_or.empty()) {
    out += "OR " + statement.insert_or + " ";
  }
  out += "INTO " + quote_dotted(statement.table);
  if (!statement.columns.empty()) {
    out += " (" + quote_name_list(statement.columns) + ")";
  }

  if (statement.default_values) {
    out += " DEFAULT VALUES";
  } else if (statement.select != nullptr) {
    out += " " + format(*statement.select);
  } else {
    out += " VALUES";
    for (const auto& row : statement.rows) {
      if (&row != &statement.rows.front()) out += ",";
      out += " (";
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i > 0) out += ", ";
        out += print_expr(row[i], 0);
      }
      out += ")";
    }
  }

  if (statement.upsert != nullptr) {
    const UpsertClause& upsert = *statement.upsert;
    out += " ON CONFLICT";
    if (!upsert.columns.empty()) {
      out += " (" + quote_name_list(upsert.columns) + ")";
    }
    if (upsert.target_where != nullptr) {
      out += " WHERE " + print_expr(*upsert.target_where, 0);
    }
    if (upsert.do_nothing) {
      out += " DO NOTHING";
    } else {
      out += " DO UPDATE SET ";
      for (std::size_t i = 0; i < upsert.updates.size(); ++i) {
        if (i > 0) out += ", ";
        out += quote_dotted(upsert.updates[i].first) + " = " +
               print_expr(*upsert.updates[i].second, 0);
      }
      if (upsert.update_where != nullptr) {
        out += " WHERE " + print_expr(*upsert.update_where, 0);
      }
    }
  }

  if (!statement.returning.empty()) {
    out += " RETURNING " + print_items(statement.returning);
  }
  return out;
}

std::string format(const UpdateStatement& statement) {
  std::string out = print_with(statement.with, statement.recursive);
  out += "UPDATE " + quote_dotted(statement.table);
  if (!statement.alias.empty()) out += " AS " + quote_identifier(statement.alias);

  out += " SET ";
  for (std::size_t i = 0; i < statement.assignments.size(); ++i) {
    if (i > 0) out += ", ";
    out += quote_dotted(statement.assignments[i].first) + " = " +
           print_expr(*statement.assignments[i].second, 0);
  }

  if (!statement.from.empty()) {
    out += " FROM ";
    for (std::size_t i = 0; i < statement.from.size(); ++i) {
      if (i > 0) out += ", ";
      out += print_table_ref(statement.from[i]);
    }
  }
  if (statement.where != nullptr) {
    out += " WHERE " + print_expr(*statement.where, 0);
  }
  if (!statement.returning.empty()) {
    out += " RETURNING " + print_items(statement.returning);
  }
  return out;
}

std::string format(const DeleteStatement& statement) {
  std::string out = print_with(statement.with, statement.recursive);
  out += "DELETE FROM " + quote_dotted(statement.table);
  if (!statement.alias.empty()) out += " AS " + quote_identifier(statement.alias);
  if (statement.where != nullptr) {
    out += " WHERE " + print_expr(*statement.where, 0);
  }
  if (!statement.returning.empty()) {
    out += " RETURNING " + print_items(statement.returning);
  }
  return out;
}

std::string format(const CreateTableStatement& statement) {
  std::string out = "CREATE ";
  if (statement.temporary) out += "TEMP ";
  out += "TABLE ";
  if (statement.if_not_exists) out += "IF NOT EXISTS ";
  out += quote_dotted(statement.name);

  if (statement.select != nullptr) {
    return out + " AS " + format(*statement.select);
  }

  out += " (";
  bool first = true;
  for (const ColumnDef& column : statement.columns) {
    if (!first) out += ", ";
    out += print_column_def(column);
    first = false;
  }
  for (const TableConstraint& constraint : statement.constraints) {
    if (!first) out += ", ";
    out += print_table_constraint(constraint);
    first = false;
  }
  return out + ")";
}

std::string format(const CreateIndexStatement& statement) {
  std::string out = "CREATE ";
  if (statement.unique) out += "UNIQUE ";
  out += "INDEX ";
  if (statement.if_not_exists) out += "IF NOT EXISTS ";
  out += quote_identifier(statement.name) + " ON " +
         quote_dotted(statement.table) + " (";
  for (std::size_t i = 0; i < statement.columns.size(); ++i) {
    if (i > 0) out += ", ";
    out += print_expr(statement.columns[i].expr, 0);
    if (statement.columns[i].descending) out += " DESC";
  }
  return out + ")";
}

std::string format(const CreateViewStatement& statement) {
  std::string out = "CREATE ";
  if (statement.or_replace) out += "OR REPLACE ";
  if (statement.temporary) out += "TEMP ";
  out += "VIEW ";
  if (statement.if_not_exists) out += "IF NOT EXISTS ";
  out += quote_dotted(statement.name);
  if (!statement.columns.empty()) {
    out += " (" + quote_name_list(statement.columns) + ")";
  }
  return out + " AS " + format(*statement.query);
}

std::string format(const DropStatement& statement) {
  std::string out = "DROP " + statement.object_type + " ";
  if (statement.if_exists) out += "IF EXISTS ";
  for (std::size_t i = 0; i < statement.names.size(); ++i) {
    if (i > 0) out += ", ";
    out += quote_dotted(statement.names[i]);
  }
  if (!statement.option.empty()) out += " " + statement.option;
  return out;
}

std::string format(const AlterTableStatement& statement) {
  std::string out = "ALTER TABLE ";
  if (statement.if_exists) out += "IF EXISTS ";
  out += quote_dotted(statement.table);

  switch (statement.action) {
    case AlterTableStatement::Action::AddColumn:
      out += " ADD COLUMN " + print_column_def(statement.column);
      break;
    case AlterTableStatement::Action::DropColumn:
      out += " DROP COLUMN ";
      if (statement.column_if_exists) out += "IF EXISTS ";
      out += quote_identifier(statement.column_name);
      break;
    case AlterTableStatement::Action::RenameTable:
      out += " RENAME TO " + quote_dotted(statement.new_name);
      break;
    case AlterTableStatement::Action::RenameColumn:
      out += " RENAME COLUMN " + quote_identifier(statement.column_name) +
             " TO " + quote_identifier(statement.new_name);
      break;
  }
  return out;
}

std::string format(const TransactionStatement& statement) {
  return statement.action;
}

}  // namespace sql
