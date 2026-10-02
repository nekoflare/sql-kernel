#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sql {

enum class TokenKind {
  End,
  Identifier,
  Number,
  String,
  Parameter,  // ?, $1, :name, @name

  // punctuation
  Comma,
  LeftParen,
  RightParen,
  LeftBracket,   // [
  RightBracket,  // ]
  Semicolon,
  Dot,
  Star,
  Slash,
  Percent,
  Cast,  // ::

  // operators
  Plus,
  Minus,
  Concat,      // ||
  Eq,
  Ne,          // <> or !=
  Lt,
  Le,
  Gt,
  Ge,
  Amp,         // &
  Pipe,        // |
  ShiftLeft,   // <<
  ShiftRight,  // >>
  Tilde,       // ~
  Caret,       // ^

  // keywords
  KeywordAdd,
  KeywordAll,
  KeywordAlter,
  KeywordAnd,
  KeywordAny,
  KeywordAs,
  KeywordAsc,
  KeywordAutoincrement,
  KeywordBegin,
  KeywordBetween,
  KeywordBy,
  KeywordCascade,
  KeywordCase,
  KeywordCast,
  KeywordCheck,
  KeywordCollate,
  KeywordColumn,
  KeywordCommit,
  KeywordConflict,
  KeywordConstraint,
  KeywordCreate,
  KeywordCross,
  KeywordCube,  // recognized; GROUP BY ROLLUP/CUBE unsupported
  KeywordCurrent,
  KeywordDefault,
  KeywordDelete,
  KeywordDesc,
  KeywordDistinct,
  KeywordDo,
  KeywordDrop,
  KeywordElse,
  KeywordEnd,
  KeywordEscape,
  KeywordExcept,
  KeywordExists,
  KeywordFalse,
  KeywordFirst,
  KeywordFollowing,
  KeywordFor,
  KeywordForeign,
  KeywordFrom,
  KeywordFull,
  KeywordGlob,
  KeywordGroup,
  KeywordGroups,
  KeywordHaving,
  KeywordIlike,
  KeywordIf,
  KeywordIgnore,
  KeywordIn,
  KeywordIndex,
  KeywordInner,
  KeywordInsert,
  KeywordIntersect,
  KeywordInto,
  KeywordIs,
  KeywordIsolation,
  KeywordJoin,
  KeywordKey,
  KeywordLast,
  KeywordLeft,
  KeywordLike,
  KeywordLimit,
  KeywordMaterialized,
  KeywordNatural,
  KeywordNext,
  KeywordNot,
  KeywordNo,
  KeywordNothing,
  KeywordNull,
  KeywordNulls,
  KeywordOffset,
  KeywordOn,
  KeywordOnly,
  KeywordOr,
  KeywordOrder,
  KeywordOuter,
  KeywordOver,
  KeywordPartition,
  KeywordPreceding,
  KeywordPrimary,
  KeywordRange,
  KeywordRecursive,
  KeywordReferences,
  KeywordRename,
  KeywordReplace,
  KeywordRestrict,
  KeywordReturning,
  KeywordRight,
  KeywordRollback,
  KeywordRollup,
  KeywordRow,
  KeywordRows,
  KeywordSelect,
  KeywordSet,
  KeywordSome,  // reserved for ANY/SOME subquery predicates
  KeywordTable,
  KeywordTemp,
  KeywordTemporary,
  KeywordThen,
  KeywordTies,
  KeywordTo,
  KeywordTransaction,
  KeywordTrue,
  KeywordUnbounded,
  KeywordUnion,
  KeywordUnique,
  KeywordUpdate,
  KeywordUsing,
  KeywordValues,
  KeywordView,
  KeywordWhen,
  KeywordWhere,
  KeywordWith,
  KeywordWithout,
  KeywordWork,
};

struct Token {
  TokenKind kind = TokenKind::End;
  // Lexeme text: raw digits for numbers, unescaped contents for string
  // literals and quoted identifiers, the operator spelling for operators,
  // the source spelling for keywords and identifiers.
  std::string text;
  // True when a number literal contains '.' or an exponent.
  bool is_float = false;
  std::size_t line = 1;
  std::size_t column = 1;
};

// True for keywords that may be used as ordinary identifiers (table names,
// column names, aliases, ...). Reserved words may not.
bool is_reserved_keyword(TokenKind kind);

// True when `word` (ASCII-case-insensitive) spells a reserved keyword and so
// cannot be used as a bare identifier. Used by the printer to decide when an
// identifier must be re-quoted.
bool is_reserved_keyword_word(const std::string& word);

// True for every Keyword* kind (the keyword kinds are laid out contiguously
// at the end of the TokenKind enum).
bool is_keyword_kind(TokenKind kind);

// ASCII-only upper-case conversion, used for keyword comparisons.
std::string to_upper_ascii(std::string_view word);

// Human-readable token description for error messages.
std::string describe(const Token& token);

// Scanner for the SQL subset understood by the parser. Comments (`--` line
// comments and nested `/* ... */` block comments) are treated as whitespace.
class Lexer {
 public:
  explicit Lexer(std::string_view input) : input_(input) {}

  Token next();

 private:
  bool at_end() const { return pos_ >= input_.size(); }
  char peek() const { return peek(0); }
  char peek(std::size_t offset) const;
  char advance();
  void skip_space();  // whitespace and comments

  std::string lex_string(char quote, std::size_t line, std::size_t column);
  std::string lex_dollar_string(std::size_t line, std::size_t column);

  std::string_view input_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

}  // namespace sql
