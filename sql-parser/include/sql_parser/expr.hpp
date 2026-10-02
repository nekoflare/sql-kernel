#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sql {

// Literal values supported anywhere a constant is allowed.
using Value = std::variant<std::nullptr_t, long long, double, std::string, bool>;

struct Expr;
struct SelectStatement;
struct WindowSpec;

using ExprPtr = std::shared_ptr<Expr>;

// Operator precedence (highest binding first), following the PostgreSQL
// lexical structure table (https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-PRECEDENCE)
// plus the SQL:2016 precedence for COLLATE and unary operators.
namespace prec {
constexpr int Or = 1;            // OR
constexpr int And = 2;           // AND
constexpr int Not = 3;           // NOT
constexpr int Is = 4;            // IS [NOT] NULL / TRUE / DISTINCT FROM
constexpr int Comparison = 5;    // = <> < <= > >= !=
constexpr int Range = 6;         // [NOT] BETWEEN / IN / LIKE
constexpr int OtherOperator = 7; // || & | << >>
constexpr int Additive = 8;      // + -
constexpr int Multiplicative = 9;// * / %
constexpr int Exponent = 10;     // ^
constexpr int Collate = 11;      // COLLATE
constexpr int Unary = 12;        // unary + - ~
constexpr int Cast = 13;         // :: and CAST(...)
constexpr int Primary = 14;      // literals, names, calls, (), CASE
}  // namespace prec

// Precedence of a binary operator by its spelling ("=", "AND", "+", ...);
// 0 when the text is not a binary operator.
int binary_precedence(std::string_view op);

// Binding strength of a whole expression node, used by the printer to decide
// where parentheses are required.
int expression_precedence(const Expr& expr);

struct Expr {
  enum class Kind {
    Literal,       // value holds the constant
    Parameter,     // ?, $1, :name, @name  (name holds the spelling)
    ColumnRef,     // parts = [column] | [table, column] | [schema, table, column]
    Star,          // * or t.*  (parts holds the qualifier)
    Unary,         // name = "-" | "+" | "~" | "NOT"
    Binary,        // name = operator spelling ("=", "<>", "||", "AND", ...)
    NullTest,      // args[0] IS [NOT] NULL
    BoolTest,      // args[0] IS [NOT] TRUE|FALSE|UNKNOWN  (name holds the keyword)
    In,            // args[0] IN list, or subquery for IN (SELECT ...)
    Like,          // args[0] <op> args[1] [ESCAPE args[2]]; name = LIKE|ILIKE|GLOB
    Between,       // args = operand, low, high
    Exists,        // EXISTS subquery (NOT is a Unary wrapper)
    Subquery,      // scalar (SELECT ...) in expression position
    FunctionCall,  // parts = name, args, distinct/star flags, optional OVER
    Cast,          // CAST(args[0] AS type_name)
    Collate,       // args[0] COLLATE name
    Case,          // operand? + args (WHEN) + results (THEN) + else_result
    Row,           // (a, b, c) row constructor
    Subscript,     // args[0][args[1]]  array / rowid subscript
    Quantified,    // args[0] <name> ANY|SOME|ALL args[1]  (name = operator)
    Default,       // DEFAULT keyword
  };

  Kind kind = Kind::Literal;
  Value literal = nullptr;   // Literal
  std::string name;          // operator / collation / parameter / alias text
  std::vector<std::string> parts;
  std::vector<ExprPtr> args;
  std::vector<ExprPtr> results;      // CASE ... THEN results
  ExprPtr else_result;               // CASE ... ELSE
  ExprPtr operand;                   // simple CASE subject
  std::shared_ptr<SelectStatement> subquery;  // Exists / Subquery / In
  std::shared_ptr<WindowSpec> over;  // window function OVER clause
  std::string type_name;             // Cast target type, e.g. "VARCHAR(255)"
  bool negated = false;              // NOT variants (IS NOT NULL, NOT IN, ...)
  bool distinct = false;             // COUNT(DISTINCT x)
  bool star = false;                 // COUNT(*)
};

struct OrderByItem {
  Expr expr;
  bool descending = false;
  std::string nulls;  // "", "FIRST", "LAST"
};

// Window specification for OVER (...): partition/ordering plus an optional
// frame clause (ROWS / RANGE / GROUPS).
struct WindowSpec {
  std::vector<Expr> partition_by;
  std::vector<OrderByItem> order_by;

  std::string frame_type;       // "", "ROWS", "RANGE", "GROUPS"
  bool frame_between = false;
  std::string frame_start;      // "UNBOUNDED PRECEDING" | "CURRENT ROW" |
                                // "PRECEDING" | "FOLLOWING"
  ExprPtr frame_start_expr;
  std::string frame_end;        // as above, only with frame_between
  ExprPtr frame_end_expr;
};

std::string to_string(const Value& value);

// Canonical SQL rendering of an expression (parentheses added only where
// precedence requires them).
std::string format(const Expr& expr);

// Convenience: wrap a node in a shared pointer.
inline ExprPtr make_expr_ptr(Expr expr) {
  return std::make_shared<Expr>(std::move(expr));
}

bool operator==(const Expr& a, const Expr& b);
bool operator!=(const Expr& a, const Expr& b);

}  // namespace sql
