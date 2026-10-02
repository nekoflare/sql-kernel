#pragma once

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "sql_parser/expr.hpp"

namespace sql {

struct SelectStatement;

// WITH name [(cols)] AS [(NOT MATERIALIZED)] (query)
struct CommonTableExpr {
  std::string name;
  std::vector<std::string> columns;
  bool not_materialized = false;
  std::shared_ptr<SelectStatement> query;
};

// One item of a FROM clause (or an operand of a join).
struct TableRef {
  enum class Kind { Table, Subquery, TableFunction, Join };
  Kind kind = Kind::Table;

  std::string name;       // Table: possibly schema-qualified
  std::string alias;
  std::vector<std::string> column_aliases;

  std::shared_ptr<SelectStatement> subquery;  // Subquery
  ExprPtr function;                           // TableFunction

  // Join
  std::string join_type;  // "", "INNER", "LEFT [OUTER]", "RIGHT [OUTER]",
                          // "FULL [OUTER]", "CROSS"
  bool natural = false;
  std::shared_ptr<TableRef> left;
  std::shared_ptr<TableRef> right;
  ExprPtr on;
  std::vector<std::string> using_columns;

  bool parenthesized = false;  // written as (item) in the source
};

struct SelectItem {
  Expr expr;
  std::string alias;
};

struct SelectStatement {
  std::vector<CommonTableExpr> with;
  bool recursive = false;

  // Simple select core
  bool distinct = false;
  bool all = false;                       // SELECT ALL
  std::vector<Expr> distinct_on;          // DISTINCT ON (...)
  std::vector<SelectItem> columns;
  bool is_values = false;                 // VALUES (...) statement
  std::vector<std::vector<Expr>> value_rows;
  std::vector<TableRef> from;
  ExprPtr where;
  bool group_all = false;                 // GROUP BY ALL
  bool group_distinct = false;            // GROUP BY DISTINCT
  std::vector<Expr> group_by;
  ExprPtr having;

  // Compound queries: INTERSECT binds tighter than UNION/EXCEPT.
  std::string set_op;  // "", "UNION", "INTERSECT", "EXCEPT"
  bool set_all = false;
  std::shared_ptr<SelectStatement> set_left;
  std::shared_ptr<SelectStatement> set_right;

  // Tail
  std::vector<OrderByItem> order_by;
  ExprPtr limit;
  bool limit_all = false;
  ExprPtr offset;
};

// ON CONFLICT ... DO NOTHING | DO UPDATE SET ...
struct UpsertClause {
  std::vector<std::string> columns;  // conflict target columns
  ExprPtr target_where;              // conflict target WHERE
  bool do_nothing = false;
  std::vector<std::pair<std::string, ExprPtr>> updates;
  ExprPtr update_where;
};

struct InsertStatement {
  std::vector<CommonTableExpr> with;
  bool recursive = false;
  std::string insert_or;  // "", "REPLACE", "IGNORE" (SQLite conflict action)
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<Expr>> rows;  // VALUES rows
  bool default_values = false;
  std::shared_ptr<SelectStatement> select;  // INSERT ... SELECT
  std::shared_ptr<UpsertClause> upsert;
  std::vector<SelectItem> returning;
};

struct UpdateStatement {
  std::vector<CommonTableExpr> with;
  bool recursive = false;
  std::string table;
  std::string alias;
  std::vector<std::pair<std::string, ExprPtr>> assignments;
  std::vector<TableRef> from;
  ExprPtr where;
  std::vector<SelectItem> returning;
};

struct DeleteStatement {
  std::vector<CommonTableExpr> with;
  bool recursive = false;
  std::string table;
  std::string alias;
  ExprPtr where;
  std::vector<SelectItem> returning;
};

// ---------------------------------------------------------------- DDL ----

struct ColumnConstraint {
  enum class Kind {
    NotNull, Null, PrimaryKey, Unique, Check, Default, References, Collate,
    AutoIncrement,
  };
  Kind kind = Kind::NotNull;
  std::string name;      // optional CONSTRAINT name
  ExprPtr value;         // CHECK expression / DEFAULT expression
  std::string collation;  // COLLATE
  std::string ref_table;  // REFERENCES
  std::vector<std::string> ref_columns;
  std::string ref_on_delete;  // "", "CASCADE", "SET NULL", "RESTRICT", ...
  std::string ref_on_update;
  bool deferrable = false;
  bool initially_deferred = false;
};

struct ColumnDef {
  std::string name;
  std::string type_name;  // may be empty (SQLite allows untyped columns)
  std::vector<ColumnConstraint> constraints;
};

struct TableConstraint {
  enum class Kind { PrimaryKey, Unique, Check, Foreign };
  Kind kind = Kind::PrimaryKey;
  std::string name;
  std::vector<std::string> columns;
  ExprPtr check;
  std::string ref_table;
  std::vector<std::string> ref_columns;
  std::string ref_on_delete;
  std::string ref_on_update;
  bool deferrable = false;
  bool initially_deferred = false;
};

struct CreateTableStatement {
  bool temporary = false;
  bool if_not_exists = false;
  std::string name;
  std::vector<ColumnDef> columns;
  std::vector<TableConstraint> constraints;
  std::shared_ptr<SelectStatement> select;  // CREATE TABLE ... AS SELECT
};

struct IndexColumn {
  Expr expr;   // normally a ColumnRef, optionally wrapped in COLLATE
  bool descending = false;
};

struct CreateIndexStatement {
  bool unique = false;
  bool if_not_exists = false;
  std::string name;
  std::string table;
  std::vector<IndexColumn> columns;
};

struct CreateViewStatement {
  bool or_replace = false;
  bool temporary = false;
  bool if_not_exists = false;
  std::string name;
  std::vector<std::string> columns;
  std::shared_ptr<SelectStatement> query;
};

struct DropStatement {
  std::string object_type;  // "TABLE" | "INDEX" | "VIEW"
  bool if_exists = false;
  std::vector<std::string> names;
  std::string option;  // "", "CASCADE", "RESTRICT"
};

struct AlterTableStatement {
  enum class Action { AddColumn, DropColumn, RenameTable, RenameColumn };
  enum class ColumnChange { SetNotNull, DropNotNull, SetDefault, DropDefault };

  Action action = Action::AddColumn;
  bool if_exists = false;        // ALTER TABLE IF EXISTS
  bool column_if_exists = false; // DROP COLUMN IF EXISTS
  std::string table;
  ColumnDef column;          // AddColumn
  std::string column_name;   // DropColumn / RenameColumn
  std::string new_name;      // RenameTable / RenameColumn
  ColumnChange change = ColumnChange::SetNotNull;  // reserved for future use
  ExprPtr default_value;     // reserved for future use
};

struct TransactionStatement {
  std::string action;  // "BEGIN" | "COMMIT" | "ROLLBACK"
};

using Statement = std::variant<SelectStatement, InsertStatement,
                               UpdateStatement, DeleteStatement,
                               CreateTableStatement, CreateIndexStatement,
                               CreateViewStatement, DropStatement,
                               AlterTableStatement, TransactionStatement>;

// Canonical SQL rendering of a statement (no trailing semicolon).
std::string format(const Statement& statement);
std::string format(const SelectStatement& statement);
std::string format(const InsertStatement& statement);
std::string format(const UpdateStatement& statement);
std::string format(const DeleteStatement& statement);
std::string format(const CreateTableStatement& statement);
std::string format(const CreateIndexStatement& statement);
std::string format(const CreateViewStatement& statement);
std::string format(const DropStatement& statement);
std::string format(const AlterTableStatement& statement);
std::string format(const TransactionStatement& statement);

bool operator==(const InsertStatement& a, const InsertStatement& b);
bool operator!=(const InsertStatement& a, const InsertStatement& b);
bool operator==(const SelectStatement& a, const SelectStatement& b);
bool operator!=(const SelectStatement& a, const SelectStatement& b);
bool operator==(const UpdateStatement& a, const UpdateStatement& b);
bool operator!=(const UpdateStatement& a, const UpdateStatement& b);
bool operator==(const DeleteStatement& a, const DeleteStatement& b);
bool operator!=(const DeleteStatement& a, const DeleteStatement& b);
bool operator==(const CreateTableStatement& a, const CreateTableStatement& b);
bool operator!=(const CreateTableStatement& a, const CreateTableStatement& b);
bool operator==(const CreateIndexStatement& a, const CreateIndexStatement& b);
bool operator!=(const CreateIndexStatement& a, const CreateIndexStatement& b);
bool operator==(const CreateViewStatement& a, const CreateViewStatement& b);
bool operator!=(const CreateViewStatement& a, const CreateViewStatement& b);
bool operator==(const DropStatement& a, const DropStatement& b);
bool operator!=(const DropStatement& a, const DropStatement& b);
bool operator==(const AlterTableStatement& a, const AlterTableStatement& b);
bool operator!=(const AlterTableStatement& a, const AlterTableStatement& b);
bool operator==(const TransactionStatement& a, const TransactionStatement& b);
bool operator!=(const TransactionStatement& a, const TransactionStatement& b);

}  // namespace sql
