#include "sql_parser/lexer.hpp"

#include <cctype>
#include <string>
#include <unordered_map>

#include "sql_parser/error.hpp"

namespace sql {

std::string to_upper_ascii(std::string_view word) {
  std::string out;
  out.reserve(word.size());
  for (char c : word) {
    out.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

namespace {

bool is_ident_start(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_part(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
         c == '$';
}

bool is_digit(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

const std::unordered_map<std::string, TokenKind>& keywords() {
  static const std::unordered_map<std::string, TokenKind> k = {
      {"ADD", TokenKind::KeywordAdd},
      {"ALL", TokenKind::KeywordAll},
      {"ALTER", TokenKind::KeywordAlter},
      {"AND", TokenKind::KeywordAnd},
      {"ANY", TokenKind::KeywordAny},
      {"AS", TokenKind::KeywordAs},
      {"ASC", TokenKind::KeywordAsc},
      {"AUTOINCREMENT", TokenKind::KeywordAutoincrement},
      {"BEGIN", TokenKind::KeywordBegin},
      {"BETWEEN", TokenKind::KeywordBetween},
      {"BY", TokenKind::KeywordBy},
      {"CASCADE", TokenKind::KeywordCascade},
      {"CASE", TokenKind::KeywordCase},
      {"CAST", TokenKind::KeywordCast},
      {"CHECK", TokenKind::KeywordCheck},
      {"COLLATE", TokenKind::KeywordCollate},
      {"COLUMN", TokenKind::KeywordColumn},
      {"COMMIT", TokenKind::KeywordCommit},
      {"CONFLICT", TokenKind::KeywordConflict},
      {"CONSTRAINT", TokenKind::KeywordConstraint},
      {"CREATE", TokenKind::KeywordCreate},
      {"CROSS", TokenKind::KeywordCross},
      {"CUBE", TokenKind::KeywordCube},
      {"CURRENT", TokenKind::KeywordCurrent},
      {"DEFAULT", TokenKind::KeywordDefault},
      {"DELETE", TokenKind::KeywordDelete},
      {"DESC", TokenKind::KeywordDesc},
      {"DISTINCT", TokenKind::KeywordDistinct},
      {"DO", TokenKind::KeywordDo},
      {"DROP", TokenKind::KeywordDrop},
      {"ELSE", TokenKind::KeywordElse},
      {"END", TokenKind::KeywordEnd},
      {"ESCAPE", TokenKind::KeywordEscape},
      {"EXCEPT", TokenKind::KeywordExcept},
      {"EXISTS", TokenKind::KeywordExists},
      {"FALSE", TokenKind::KeywordFalse},
      {"FIRST", TokenKind::KeywordFirst},
      {"FOLLOWING", TokenKind::KeywordFollowing},
      {"FOR", TokenKind::KeywordFor},
      {"FOREIGN", TokenKind::KeywordForeign},
      {"FROM", TokenKind::KeywordFrom},
      {"FULL", TokenKind::KeywordFull},
      {"GLOB", TokenKind::KeywordGlob},
      {"GROUP", TokenKind::KeywordGroup},
      {"GROUPS", TokenKind::KeywordGroups},
      {"HAVING", TokenKind::KeywordHaving},
      {"ILIKE", TokenKind::KeywordIlike},
      {"IF", TokenKind::KeywordIf},
      {"IGNORE", TokenKind::KeywordIgnore},
      {"IN", TokenKind::KeywordIn},
      {"INDEX", TokenKind::KeywordIndex},
      {"INNER", TokenKind::KeywordInner},
      {"INSERT", TokenKind::KeywordInsert},
      {"INTERSECT", TokenKind::KeywordIntersect},
      {"INTO", TokenKind::KeywordInto},
      {"IS", TokenKind::KeywordIs},
      {"ISOLATION", TokenKind::KeywordIsolation},
      {"JOIN", TokenKind::KeywordJoin},
      {"KEY", TokenKind::KeywordKey},
      {"LAST", TokenKind::KeywordLast},
      {"LEFT", TokenKind::KeywordLeft},
      {"LIKE", TokenKind::KeywordLike},
      {"LIMIT", TokenKind::KeywordLimit},
      {"MATERIALIZED", TokenKind::KeywordMaterialized},
      {"NATURAL", TokenKind::KeywordNatural},
      {"NEXT", TokenKind::KeywordNext},
      {"NOT", TokenKind::KeywordNot},
      {"NO", TokenKind::KeywordNo},
      {"NOTHING", TokenKind::KeywordNothing},
      {"NULL", TokenKind::KeywordNull},
      {"NULLS", TokenKind::KeywordNulls},
      {"OFFSET", TokenKind::KeywordOffset},
      {"ON", TokenKind::KeywordOn},
      {"ONLY", TokenKind::KeywordOnly},
      {"OR", TokenKind::KeywordOr},
      {"ORDER", TokenKind::KeywordOrder},
      {"OUTER", TokenKind::KeywordOuter},
      {"OVER", TokenKind::KeywordOver},
      {"PARTITION", TokenKind::KeywordPartition},
      {"PRECEDING", TokenKind::KeywordPreceding},
      {"PRIMARY", TokenKind::KeywordPrimary},
      {"RANGE", TokenKind::KeywordRange},
      {"RECURSIVE", TokenKind::KeywordRecursive},
      {"REFERENCES", TokenKind::KeywordReferences},
      {"RENAME", TokenKind::KeywordRename},
      {"REPLACE", TokenKind::KeywordReplace},
      {"RESTRICT", TokenKind::KeywordRestrict},
      {"RETURNING", TokenKind::KeywordReturning},
      {"RIGHT", TokenKind::KeywordRight},
      {"ROLLBACK", TokenKind::KeywordRollback},
      {"ROLLUP", TokenKind::KeywordRollup},
      {"ROW", TokenKind::KeywordRow},
      {"ROWS", TokenKind::KeywordRows},
      {"SELECT", TokenKind::KeywordSelect},
      {"SET", TokenKind::KeywordSet},
      {"SOME", TokenKind::KeywordSome},
      {"TABLE", TokenKind::KeywordTable},
      {"TEMP", TokenKind::KeywordTemp},
      {"TEMPORARY", TokenKind::KeywordTemporary},
      {"THEN", TokenKind::KeywordThen},
      {"TIES", TokenKind::KeywordTies},
      {"TO", TokenKind::KeywordTo},
      {"TRANSACTION", TokenKind::KeywordTransaction},
      {"TRUE", TokenKind::KeywordTrue},
      {"UNBOUNDED", TokenKind::KeywordUnbounded},
      {"UNION", TokenKind::KeywordUnion},
      {"UNIQUE", TokenKind::KeywordUnique},
      {"UPDATE", TokenKind::KeywordUpdate},
      {"USING", TokenKind::KeywordUsing},
      {"VALUES", TokenKind::KeywordValues},
      {"VIEW", TokenKind::KeywordView},
      {"WHEN", TokenKind::KeywordWhen},
      {"WHERE", TokenKind::KeywordWhere},
      {"WITH", TokenKind::KeywordWith},
      {"WITHOUT", TokenKind::KeywordWithout},
      {"WORK", TokenKind::KeywordWork},
  };
  return k;
}

}  // namespace

bool is_keyword_kind(TokenKind kind) {
  // Keyword kinds are laid out contiguously between these two sentinels.
  return kind >= TokenKind::KeywordAdd && kind <= TokenKind::KeywordWork;
}

bool is_reserved_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KeywordSelect:
    case TokenKind::KeywordFrom:
    case TokenKind::KeywordWhere:
    case TokenKind::KeywordGroup:
    case TokenKind::KeywordHaving:
    case TokenKind::KeywordOrder:
    case TokenKind::KeywordBy:
    case TokenKind::KeywordLimit:
    case TokenKind::KeywordOffset:
    case TokenKind::KeywordUnion:
    case TokenKind::KeywordIntersect:
    case TokenKind::KeywordExcept:
    case TokenKind::KeywordValues:
    case TokenKind::KeywordAnd:
    case TokenKind::KeywordOr:
    case TokenKind::KeywordNot:
    case TokenKind::KeywordIs:
    case TokenKind::KeywordIn:
    case TokenKind::KeywordBetween:
    case TokenKind::KeywordLike:
    case TokenKind::KeywordIlike:
    case TokenKind::KeywordGlob:
    case TokenKind::KeywordOn:
    case TokenKind::KeywordUsing:
    case TokenKind::KeywordJoin:
    case TokenKind::KeywordInner:
    case TokenKind::KeywordLeft:
    case TokenKind::KeywordRight:
    case TokenKind::KeywordFull:
    case TokenKind::KeywordCross:
    case TokenKind::KeywordNatural:
    case TokenKind::KeywordSet:
    case TokenKind::KeywordInto:
    case TokenKind::KeywordReturning:
    case TokenKind::KeywordWhen:
    case TokenKind::KeywordThen:
    case TokenKind::KeywordElse:
    case TokenKind::KeywordCase:
    case TokenKind::KeywordCast:
    case TokenKind::KeywordExists:
    case TokenKind::KeywordNull:
    case TokenKind::KeywordTrue:
    case TokenKind::KeywordFalse:
    case TokenKind::KeywordDistinct:
    case TokenKind::KeywordAll:
      return true;
    default:
      return false;
  }
}

bool is_reserved_keyword_word(const std::string& word) {
  const auto& map = keywords();
  const auto it = map.find(to_upper_ascii(word));
  return it != map.end() && is_reserved_keyword(it->second);
}

std::string describe(const Token& token) {
  switch (token.kind) {
    case TokenKind::End:
      return "end of input";
    case TokenKind::Identifier:
      return "identifier '" + token.text + "'";
    case TokenKind::Number:
      return "number '" + token.text + "'";
    case TokenKind::String:
      return "string literal";
    case TokenKind::Parameter:
      return "parameter '" + token.text + "'";
    default:
      return "'" + token.text + "'";
  }
}

char Lexer::peek(std::size_t offset) const {
  const std::size_t index = pos_ + offset;
  return index < input_.size() ? input_[index] : '\0';
}

char Lexer::advance() {
  const char c = input_[pos_++];
  if (c == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return c;
}

void Lexer::skip_space() {
  for (;;) {
    while (!at_end() &&
           std::isspace(static_cast<unsigned char>(peek())) != 0) {
      advance();
    }
    if (peek() == '-' && peek(1) == '-') {  // line comment
      while (!at_end() && peek() != '\n') advance();
      continue;
    }
    if (peek() == '/' && peek(1) == '*') {  // block comment (nests)
      const std::size_t start_line = line_;
      const std::size_t start_column = column_;
      advance();
      advance();
      int depth = 1;
      while (depth > 0) {
        if (at_end()) {
          throw ParseError("unterminated block comment", start_line,
                           start_column);
        }
        if (peek() == '/' && peek(1) == '*') {
          advance();
          advance();
          ++depth;
        } else if (peek() == '*' && peek(1) == '/') {
          advance();
          advance();
          --depth;
        } else {
          advance();
        }
      }
      continue;
    }
    break;
  }
}

std::string Lexer::lex_string(char quote, std::size_t line,
                              std::size_t column) {
  // pos_ is just after the opening quote.
  std::string value;
  for (;;) {
    if (at_end()) {
      throw ParseError("unterminated string literal", line, column);
    }
    const char ch = advance();
    if (ch == quote) {
      if (peek() == quote) {  // doubled quote escapes the quote character
        advance();
        value.push_back(quote);
        continue;
      }
      break;
    }
    value.push_back(ch);
  }
  return value;
}

std::string Lexer::lex_dollar_string(std::size_t line, std::size_t column) {
  // pos_ is just after the opening '$'.
  std::string tag;
  while (!at_end() && peek() != '$') {
    const char c = peek();
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
      throw ParseError("invalid dollar-quote tag", line, column);
    }
    tag.push_back(advance());
  }
  if (at_end()) {
    throw ParseError("unterminated dollar-quoted string", line, column);
  }
  advance();  // opening '$' that ends the tag

  std::string value;
  for (;;) {
    if (at_end()) {
      throw ParseError("unterminated dollar-quoted string", line, column);
    }
    if (peek() == '$') {
      // Terminator is '$' + tag + '$'; check the whole thing.
      const std::size_t needed = 1 + tag.size() + 1;
      if (input_.size() - pos_ >= needed &&
          input_.compare(pos_ + 1, tag.size(), tag) == 0 &&
          input_[pos_ + 1 + tag.size()] == '$') {
        for (std::size_t k = 0; k < needed; ++k) advance();
        break;
      }
    }
    value.push_back(advance());
  }
  return value;
}

Token Lexer::next() {
  skip_space();

  const std::size_t start_line = line_;
  const std::size_t start_column = column_;
  if (at_end()) {
    return Token{TokenKind::End, "", false, start_line, start_column};
  }

  const char c = peek();
  auto single = [&](TokenKind kind) {
    advance();
    return Token{kind, std::string(1, c), false, start_line, start_column};
  };
  auto word = [&](TokenKind kind, std::string text) {
    return Token{kind, std::move(text), false, start_line, start_column};
  };

  switch (c) {
    case ',':
      return single(TokenKind::Comma);
    case '(':
      return single(TokenKind::LeftParen);
    case ')':
      return single(TokenKind::RightParen);
    case '[':
      return single(TokenKind::LeftBracket);
    case ']':
      return single(TokenKind::RightBracket);
    case ';':
      return single(TokenKind::Semicolon);
    case '+':
      return single(TokenKind::Plus);
    case '-':  // '--' comments are consumed in skip_space()
      return single(TokenKind::Minus);
    case '*':
      return single(TokenKind::Star);
    case '/':  // '/*' comments are consumed in skip_space()
      return single(TokenKind::Slash);
    case '%':
      return single(TokenKind::Percent);
    case '=':
      return single(TokenKind::Eq);
    case '~':
      return single(TokenKind::Tilde);
    case '^':
      return single(TokenKind::Caret);
    case '&':
      return single(TokenKind::Amp);
    case '<': {
      advance();
      if (peek() == '<') {
        advance();
        return word(TokenKind::ShiftLeft, "<<");
      }
      if (peek() == '=') {
        advance();
        return word(TokenKind::Le, "<=");
      }
      if (peek() == '>') {
        advance();
        return word(TokenKind::Ne, "<>");
      }
      return word(TokenKind::Lt, "<");
    }
    case '>': {
      advance();
      if (peek() == '>') {
        advance();
        return word(TokenKind::ShiftRight, ">>");
      }
      if (peek() == '=') {
        advance();
        return word(TokenKind::Ge, ">=");
      }
      return word(TokenKind::Gt, ">");
    }
    case '|': {
      advance();
      if (peek() == '|') {
        advance();
        return word(TokenKind::Concat, "||");
      }
      return word(TokenKind::Pipe, "|");
    }
    case '!': {
      advance();
      if (peek() == '=') {
        advance();
        return word(TokenKind::Ne, "!=");
      }
      throw ParseError("unexpected character '!'", start_line, start_column);
    }
    case '?': {
      advance();
      std::string text = "?";
      while (!at_end() && is_digit(peek())) text.push_back(advance());
      return word(TokenKind::Parameter, std::move(text));
    }
    case ':': {
      advance();
      if (peek() == ':') {
        advance();
        return word(TokenKind::Cast, "::");
      }
      if (is_ident_start(peek())) {
        std::string text = ":";
        while (!at_end() && is_ident_part(peek())) text.push_back(advance());
        return word(TokenKind::Parameter, std::move(text));
      }
      throw ParseError("unexpected character ':'", start_line, start_column);
    }
    case '@': {
      advance();
      if (is_ident_start(peek())) {
        std::string text = "@";
        while (!at_end() && is_ident_part(peek())) text.push_back(advance());
        return word(TokenKind::Parameter, std::move(text));
      }
      throw ParseError("unexpected character '@'", start_line, start_column);
    }
    case '$': {
      advance();
      if (is_digit(peek())) {
        std::string text = "$";
        while (!at_end() && is_digit(peek())) text.push_back(advance());
        return word(TokenKind::Parameter, std::move(text));
      }
      if (peek() == '$' || is_ident_start(peek())) {
        std::string value = lex_dollar_string(start_line, start_column);
        return Token{TokenKind::String, value, false, start_line, start_column};
      }
      throw ParseError("unexpected character '$'", start_line, start_column);
    }
    case '\'': {
      advance();
      std::string value = lex_string('\'', start_line, start_column);
      return Token{TokenKind::String, value, false, start_line, start_column};
    }
    case '"':
    case '`': {
      advance();
      std::string value = lex_string(c, start_line, start_column);
      return Token{TokenKind::Identifier, value, false, start_line,
                   start_column};
    }
    case '.': {
      if (!is_digit(peek(1))) {
        advance();
        return word(TokenKind::Dot, ".");
      }
      break;  // leading-dot number, handled below
    }
    default:
      break;
  }

  if (is_digit(c)) {
    std::string text;
    bool is_float = false;
    while (!at_end() && is_digit(peek())) text.push_back(advance());
    if (peek() == '.' && is_digit(peek(1))) {
      is_float = true;
      text.push_back(advance());  // '.'
      while (!at_end() && is_digit(peek())) text.push_back(advance());
    }
    if (peek() == 'e' || peek() == 'E') {
      std::size_t offset = 1;
      if (peek(offset) == '+' || peek(offset) == '-') ++offset;
      if (is_digit(peek(offset))) {  // only an exponent if digits follow
        is_float = true;
        text.push_back(advance());  // 'e' / 'E'
        if (peek() == '+' || peek() == '-') text.push_back(advance());
        while (!at_end() && is_digit(peek())) text.push_back(advance());
      }
    }
    return Token{TokenKind::Number, text, is_float, start_line, start_column};
  }

  if (c == '.') {  // e.g. .001
    std::string text;
    bool is_float = true;
    text.push_back(advance());
    while (!at_end() && is_digit(peek())) text.push_back(advance());
    return Token{TokenKind::Number, text, is_float, start_line, start_column};
  }

  if (is_ident_start(c)) {
    std::string word_text;
    while (!at_end() && is_ident_part(peek())) word_text.push_back(advance());

    // E'...' escape string (no space between E and the quote).
    if ((word_text == "E" || word_text == "e") && peek() == '\'') {
      advance();  // opening quote
      std::string value;
      for (;;) {
        if (at_end()) {
          throw ParseError("unterminated string literal", start_line,
                           start_column);
        }
        char ch = advance();
        if (ch == '\'') {
          if (peek() == '\'') {
            advance();
            value.push_back('\'');
            continue;
          }
          break;
        }
        if (ch == '\\') {
          if (at_end()) {
            throw ParseError("unterminated string literal", start_line,
                             start_column);
          }
          const char esc = advance();
          switch (esc) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case '0': value.push_back('\0'); break;
            case '\'': value.push_back('\''); break;
            case '\\': value.push_back('\\'); break;
            default: value.push_back('\\'); value.push_back(esc); break;
          }
          continue;
        }
        value.push_back(ch);
      }
      return Token{TokenKind::String, value, false, start_line, start_column};
    }

    const auto it = keywords().find(to_upper_ascii(word_text));
    if (it != keywords().end()) {
      return Token{it->second, word_text, false, start_line, start_column};
    }
    return Token{TokenKind::Identifier, word_text, false, start_line,
                 start_column};
  }

  // The offending character was not consumed, so pos_ still points at it.
  throw ParseError(std::string("unexpected character '") + c + "'", start_line,
                   start_column);
}

}  // namespace sql
