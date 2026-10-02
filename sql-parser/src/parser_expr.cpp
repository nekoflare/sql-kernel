// Expression parsing: precedence climbing for binary/postfix operators plus
// the primary-expression forms (literals, names, CASE, CAST, subqueries,
// function calls, window specifications).
//
// Precedence levels follow the PostgreSQL lexical structure table; see
// prec:: in expr.hpp.

#include <stdexcept>
#include <string>
#include <utility>

#include "sql_parser/error.hpp"
#include "sql_parser/parser.hpp"

namespace sql {
namespace {

ExprPtr ptr(Expr expr) { return make_expr_ptr(std::move(expr)); }

Expr literal_expr(Value value) {
  Expr expr;
  expr.kind = Expr::Kind::Literal;
  expr.literal = std::move(value);
  return expr;
}

// Precedence of a binary operator token; 0 when the token is not one.
int token_precedence(TokenKind kind) {
  switch (kind) {
    case TokenKind::KeywordOr:
      return prec::Or;
    case TokenKind::KeywordAnd:
      return prec::And;
    case TokenKind::Eq:
    case TokenKind::Ne:
    case TokenKind::Lt:
    case TokenKind::Le:
    case TokenKind::Gt:
    case TokenKind::Ge:
      return prec::Comparison;
    case TokenKind::Concat:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::ShiftLeft:
    case TokenKind::ShiftRight:
      return prec::OtherOperator;
    case TokenKind::Plus:
    case TokenKind::Minus:
      return prec::Additive;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
      return prec::Multiplicative;
    case TokenKind::Caret:
      return prec::Exponent;
    default:
      return 0;
  }
}

// Canonical spelling for a binary operator token.
std::string token_spelling(TokenKind kind, const std::string& text) {
  switch (kind) {
    case TokenKind::KeywordAnd:
      return "AND";
    case TokenKind::KeywordOr:
      return "OR";
    default:
      return text;
  }
}

bool is_range_token(TokenKind kind) {
  return kind == TokenKind::KeywordIn || kind == TokenKind::KeywordBetween ||
         kind == TokenKind::KeywordLike || kind == TokenKind::KeywordIlike ||
         kind == TokenKind::KeywordGlob;
}

}  // namespace

Expr Parser::parse_expr(int min_precedence) {
  Expr left = parse_prefix();

  for (;;) {
    // expr[index]  — array / rowid subscript.
    if (check(TokenKind::LeftBracket) && prec::Primary >= min_precedence) {
      advance();
      Expr subscript;
      subscript.kind = Expr::Kind::Subscript;
      subscript.args.push_back(ptr(std::move(left)));
      subscript.args.push_back(ptr(parse_expr(0)));
      expect(TokenKind::RightBracket, "']'");
      left = std::move(subscript);
      continue;
    }

    // expr :: type  — tightest postfix operator.
    if (check(TokenKind::Cast) && prec::Cast >= min_precedence) {
      advance();
      Expr cast;
      cast.kind = Expr::Kind::Cast;
      cast.args.push_back(ptr(std::move(left)));
      cast.type_name = parse_type_name();
      left = std::move(cast);
      continue;
    }

    // expr COLLATE name
    if (check(TokenKind::KeywordCollate) && prec::Collate >= min_precedence) {
      advance();
      Expr collate;
      collate.kind = Expr::Kind::Collate;
      collate.args.push_back(ptr(std::move(left)));
      collate.name = expect_identifier("a collation name");
      left = std::move(collate);
      continue;
    }

    // expr [NOT] IN / BETWEEN / LIKE / ILIKE / GLOB
    if (check(TokenKind::KeywordNot) && is_range_token(next_.kind) &&
        prec::Range >= min_precedence) {
      advance();  // NOT
      left = parse_range_op(std::move(left), true);
      continue;
    }
    if (is_range_token(current_.kind) && prec::Range >= min_precedence) {
      left = parse_range_op(std::move(left), false);
      continue;
    }

    // expr IS [NOT] NULL | TRUE | FALSE | UNKNOWN | DISTINCT FROM expr
    if (check(TokenKind::KeywordIs) && prec::Is >= min_precedence) {
      advance();
      const bool negated = match(TokenKind::KeywordNot);

      if (match(TokenKind::KeywordNull)) {
        Expr test;
        test.kind = Expr::Kind::NullTest;
        test.negated = negated;
        test.args.push_back(ptr(std::move(left)));
        left = std::move(test);
        continue;
      }
      if (check(TokenKind::KeywordTrue) || check(TokenKind::KeywordFalse)) {
        Expr test;
        test.kind = Expr::Kind::BoolTest;
        test.negated = negated;
        test.name = check(TokenKind::KeywordTrue) ? "TRUE" : "FALSE";
        advance();
        test.args.push_back(ptr(std::move(left)));
        left = std::move(test);
        continue;
      }
      if (check(TokenKind::Identifier) &&
          to_upper_ascii(current_.text) == "UNKNOWN") {
        Expr test;
        test.kind = Expr::Kind::BoolTest;
        test.negated = negated;
        test.name = "UNKNOWN";
        advance();
        test.args.push_back(ptr(std::move(left)));
        left = std::move(test);
        continue;
      }
      if (check(TokenKind::KeywordDistinct) &&
          next_.kind == TokenKind::KeywordFrom) {
        advance();  // DISTINCT
        advance();  // FROM
        Expr binary;
        binary.kind = Expr::Kind::Binary;
        binary.name = negated ? "IS NOT DISTINCT FROM" : "IS DISTINCT FROM";
        binary.args.push_back(ptr(std::move(left)));
        binary.args.push_back(ptr(parse_expr(prec::Is + 1)));
        left = std::move(binary);
        continue;
      }
      fail(negated ? "expected NULL, TRUE, FALSE or DISTINCT after IS NOT"
                   : "expected NULL, TRUE, FALSE or DISTINCT after IS");
    }

    // Binary operators.
    const int precedence = token_precedence(current_.kind);
    if (precedence != 0 && precedence >= min_precedence) {
      const std::string op = token_spelling(current_.kind, current_.text);
      advance();

      // expr <cmp> ANY|SOME|ALL (subquery | value list)  — only when a '('
      // follows; otherwise ANY/ALL/SOME are ordinary (non-reserved) column
      // references, e.g. "WHERE a = all".
      if (precedence == prec::Comparison &&
          next_.kind == TokenKind::LeftParen &&
          (check(TokenKind::KeywordAny) || check(TokenKind::KeywordSome) ||
           check(TokenKind::KeywordAll))) {
        Expr quantified;
        quantified.kind = Expr::Kind::Quantified;
        quantified.name = op;
        quantified.parts.push_back(to_upper_ascii(current_.text));
        advance();
        quantified.args.push_back(ptr(std::move(left)));
        expect(TokenKind::LeftParen, "'('");
        Expr right;
        if (starts_query(current_.kind)) {
          right.kind = Expr::Kind::Subquery;
          right.subquery =
              std::make_shared<SelectStatement>(parse_select_statement());
        } else {
          right.kind = Expr::Kind::Row;
          do {
            right.args.push_back(ptr(parse_expr(0)));
          } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RightParen, "')'");
        quantified.args.push_back(ptr(std::move(right)));
        left = std::move(quantified);
        continue;
      }

      Expr binary;
      binary.kind = Expr::Kind::Binary;
      binary.name = op;
      binary.args.push_back(ptr(std::move(left)));
      binary.args.push_back(ptr(parse_expr(precedence + 1)));
      left = std::move(binary);
      continue;
    }

    break;
  }
  return left;
}

Expr Parser::parse_range_op(Expr left, bool negated) {
  switch (current_.kind) {
    case TokenKind::KeywordIn: {
      advance();
      Expr in;
      in.kind = Expr::Kind::In;
      in.negated = negated;
      in.args.push_back(ptr(std::move(left)));
      expect(TokenKind::LeftParen, "'('");
      if (starts_query(current_.kind)) {
        in.subquery =
            std::make_shared<SelectStatement>(parse_select_statement());
      } else {
        if (check(TokenKind::RightParen)) {
          fail("expected an expression, got ')'");
        }
        do {
          in.args.push_back(ptr(parse_expr(0)));
        } while (match(TokenKind::Comma));
      }
      expect(TokenKind::RightParen, "')'");
      return in;
    }

    case TokenKind::KeywordBetween: {
      advance();
      Expr between;
      between.kind = Expr::Kind::Between;
      between.negated = negated;
      between.args.push_back(ptr(std::move(left)));
      between.args.push_back(ptr(parse_expr(prec::Range + 1)));
      expect(TokenKind::KeywordAnd, "'AND'");
      between.args.push_back(ptr(parse_expr(prec::Range + 1)));
      return between;
    }

    case TokenKind::KeywordLike:
    case TokenKind::KeywordIlike:
    case TokenKind::KeywordGlob: {
      const TokenKind op = current_.kind;
      advance();
      Expr like;
      like.kind = Expr::Kind::Like;
      like.negated = negated;
      like.name = op == TokenKind::KeywordLike
                      ? "LIKE"
                      : (op == TokenKind::KeywordIlike ? "ILIKE" : "GLOB");
      like.args.push_back(ptr(std::move(left)));
      like.args.push_back(ptr(parse_expr(prec::Range + 1)));
      if (match(TokenKind::KeywordEscape)) {
        like.args.push_back(ptr(parse_expr(prec::Range + 1)));
      }
      return like;
    }

    default:
      fail("expected IN, BETWEEN or LIKE, got " + describe(current_));
  }
}

Expr Parser::parse_prefix() {
  const Token token = current_;

  switch (token.kind) {
    case TokenKind::Minus:
    case TokenKind::Plus: {
      const bool is_minus = token.kind == TokenKind::Minus;
      advance();

      // Fold a sign directly in front of a numeric literal into the literal
      // (this is also what makes -9223372036854775808 parseable).
      if (check(TokenKind::Number)) {
        const Token number = current_;
        advance();
        try {
          if (number.is_float) {
            const std::string text =
                is_minus ? "-" + number.text : number.text;
            return literal_expr(Value{std::stod(text)});
          }
          const std::string text =
              is_minus ? "-" + number.text : number.text;
          return literal_expr(Value{std::stoll(text)});
        } catch (const std::out_of_range&) {
          fail_at(number, "number literal out of range: '" + number.text + "'");
        } catch (const std::invalid_argument&) {
          fail_at(number, "invalid number literal: '" + number.text + "'");
        }
      }
      if (check(TokenKind::String)) {
        fail_at(current_, "unexpected sign before string literal");
      }
      if (check(TokenKind::KeywordNull)) {
        fail_at(current_, "unexpected sign before NULL");
      }
      if (check(TokenKind::KeywordTrue)) {
        fail_at(current_, "unexpected sign before TRUE");
      }
      if (check(TokenKind::KeywordFalse)) {
        fail_at(current_, "unexpected sign before FALSE");
      }

      Expr unary;
      unary.kind = Expr::Kind::Unary;
      unary.name = is_minus ? "-" : "+";
      unary.args.push_back(ptr(parse_expr(prec::Unary)));
      return unary;
    }

    case TokenKind::KeywordNot: {
      advance();
      Expr unary;
      unary.kind = Expr::Kind::Unary;
      unary.name = "NOT";
      unary.args.push_back(ptr(parse_expr(prec::Not)));
      return unary;
    }

    case TokenKind::Tilde: {
      advance();
      Expr unary;
      unary.kind = Expr::Kind::Unary;
      unary.name = "~";
      unary.args.push_back(ptr(parse_expr(prec::Unary)));
      return unary;
    }

    case TokenKind::LeftParen: {
      advance();
      if (starts_query(current_.kind)) {
        Expr subquery;
        subquery.kind = Expr::Kind::Subquery;
        subquery.subquery =
            std::make_shared<SelectStatement>(parse_select_statement());
        expect(TokenKind::RightParen, "')'");
        return subquery;
      }
      Expr first = parse_expr(0);
      if (check(TokenKind::RightParen)) {
        advance();
        return first;  // plain parenthesised expression
      }
      // Row constructor: (a, b, c)
      std::vector<ExprPtr> items;
      items.push_back(ptr(std::move(first)));
      while (match(TokenKind::Comma)) items.push_back(ptr(parse_expr(0)));
      expect(TokenKind::RightParen, "')'");
      Expr row;
      row.kind = Expr::Kind::Row;
      row.args = std::move(items);
      return row;
    }

    case TokenKind::KeywordCase:
      return parse_case();

    case TokenKind::KeywordCast:
      return parse_cast();

    case TokenKind::KeywordExists: {
      advance();
      expect(TokenKind::LeftParen, "'('");
      if (!starts_query(current_.kind)) {
        fail("expected SELECT after 'EXISTS ('");
      }
      Expr exists;
      exists.kind = Expr::Kind::Exists;
      exists.subquery =
          std::make_shared<SelectStatement>(parse_select_statement());
      expect(TokenKind::RightParen, "')'");
      return exists;
    }

    case TokenKind::KeywordNull:
      advance();
      return literal_expr(Value{nullptr});

    case TokenKind::KeywordTrue:
      advance();
      return literal_expr(Value{true});

    case TokenKind::KeywordFalse:
      advance();
      return literal_expr(Value{false});

    case TokenKind::KeywordDefault: {
      advance();
      Expr expr;
      expr.kind = Expr::Kind::Default;
      return expr;
    }

    case TokenKind::Number: {
      advance();
      try {
        if (token.is_float) return literal_expr(Value{std::stod(token.text)});
        return literal_expr(Value{std::stoll(token.text)});
      } catch (const std::out_of_range&) {
        fail_at(token, "number literal out of range: '" + token.text + "'");
      } catch (const std::invalid_argument&) {
        fail_at(token, "invalid number literal: '" + token.text + "'");
      }
    }

    case TokenKind::String: {
      advance();
      return literal_expr(Value{token.text});
    }

    case TokenKind::Parameter: {
      advance();
      Expr expr;
      expr.kind = Expr::Kind::Parameter;
      expr.name = token.text;
      return expr;
    }

    case TokenKind::Star: {
      advance();  // SELECT *
      Expr star;
      star.kind = Expr::Kind::Star;
      return star;
    }

    default:
      break;
  }

  if (!is_identifier_like(token.kind)) {
    fail("expected an expression, got " + describe(token));
  }

  // Qualified column reference, t.* or a function call.
  std::vector<std::string> parts;
  parts.push_back(token.text);
  advance();
  while (check(TokenKind::Dot)) {
    advance();
    if (check(TokenKind::Star)) {
      advance();
      Expr star;
      star.kind = Expr::Kind::Star;
      star.parts = std::move(parts);
      return star;
    }
    parts.push_back(expect_identifier("a name"));
  }
  // Typed datetime literal: DATE '2020-01-01', INTERVAL '1 day', ...
  // (parsed as an ordinary CAST so the printer normalises both spellings).
  if (parts.size() == 1 && check(TokenKind::String)) {
    const std::string word = to_upper_ascii(parts[0]);
    if (word == "DATE" || word == "TIME" || word == "TIMESTAMP" ||
        word == "INTERVAL") {
      const std::string value_text = current_.text;
      advance();
      Expr cast;
      cast.kind = Expr::Kind::Cast;
      cast.args.push_back(ptr(literal_expr(Value{value_text})));
      cast.type_name = word;
      return cast;
    }
  }

  if (check(TokenKind::LeftParen)) {
    return parse_function_call(std::move(parts));
  }

  Expr reference;
  reference.kind = Expr::Kind::ColumnRef;
  reference.parts = std::move(parts);
  return reference;
}

Expr Parser::parse_function_call(std::vector<std::string> parts) {
  Expr function;
  function.kind = Expr::Kind::FunctionCall;
  function.parts = std::move(parts);

  expect(TokenKind::LeftParen, "'('");
  if (match(TokenKind::Star)) {
    function.star = true;
  } else if (!check(TokenKind::RightParen)) {
    if (match(TokenKind::KeywordDistinct)) {
      function.distinct = true;
    } else {
      match(TokenKind::KeywordAll);
    }
    do {
      function.args.push_back(ptr(parse_expr(0)));
    } while (match(TokenKind::Comma));
  }
  expect(TokenKind::RightParen, "')'");

  if (match(TokenKind::KeywordOver)) {
    if (check(TokenKind::LeftParen)) {
      function.over = std::make_shared<WindowSpec>(parse_window_spec());
    } else {
      function.name = expect_identifier("a window name");
    }
  }
  return function;
}

WindowSpec Parser::parse_window_spec() {
  expect(TokenKind::LeftParen, "'('");
  WindowSpec spec;

  if (match(TokenKind::KeywordPartition)) {
    expect(TokenKind::KeywordBy, "'BY'");
    do {
      spec.partition_by.push_back(parse_expr(0));
    } while (match(TokenKind::Comma));
  }
  if (match(TokenKind::KeywordOrder)) {
    expect(TokenKind::KeywordBy, "'BY'");
    do {
      spec.order_by.push_back(parse_order_by_item());
    } while (match(TokenKind::Comma));
  }

  if (check(TokenKind::KeywordRows) || check(TokenKind::KeywordRange) ||
      check(TokenKind::KeywordGroups)) {
    spec.frame_type = check(TokenKind::KeywordRows)
                          ? "ROWS"
                          : (check(TokenKind::KeywordRange) ? "RANGE"
                                                            : "GROUPS");
    advance();
    if (match(TokenKind::KeywordBetween)) {
      spec.frame_between = true;
      auto start = parse_frame_point();
      spec.frame_start = std::move(start.first);
      spec.frame_start_expr = std::move(start.second);
      expect(TokenKind::KeywordAnd, "'AND'");
      auto end = parse_frame_point();
      spec.frame_end = std::move(end.first);
      spec.frame_end_expr = std::move(end.second);
    } else {
      auto start = parse_frame_point();
      spec.frame_start = std::move(start.first);
      spec.frame_start_expr = std::move(start.second);
    }
  }

  expect(TokenKind::RightParen, "')'");
  return spec;
}

std::pair<std::string, ExprPtr> Parser::parse_frame_point() {
  if (match(TokenKind::KeywordUnbounded)) {
    if (match(TokenKind::KeywordPreceding)) return {"UNBOUNDED PRECEDING", nullptr};
    if (match(TokenKind::KeywordFollowing)) return {"UNBOUNDED FOLLOWING", nullptr};
    fail("expected PRECEDING or FOLLOWING after UNBOUNDED");
  }
  if (match(TokenKind::KeywordCurrent)) {
    expect(TokenKind::KeywordRow, "'ROW'");
    return {"CURRENT ROW", nullptr};
  }
  Expr offset = parse_expr(prec::OtherOperator);
  if (match(TokenKind::KeywordPreceding)) return {"PRECEDING", ptr(std::move(offset))};
  if (match(TokenKind::KeywordFollowing)) return {"FOLLOWING", ptr(std::move(offset))};
  fail("expected PRECEDING or FOLLOWING after the frame offset");
}

OrderByItem Parser::parse_order_by_item() {
  OrderByItem item;
  item.expr = parse_expr(0);
  if (match(TokenKind::KeywordAsc)) {
    item.descending = false;
  } else if (match(TokenKind::KeywordDesc)) {
    item.descending = true;
  }
  if (match(TokenKind::KeywordNulls)) {
    if (match(TokenKind::KeywordFirst)) {
      item.nulls = "FIRST";
    } else if (match(TokenKind::KeywordLast)) {
      item.nulls = "LAST";
    } else {
      fail("expected FIRST or LAST after NULLS");
    }
  }
  return item;
}

Expr Parser::parse_case() {
  expect(TokenKind::KeywordCase, "'CASE'");
  Expr expr;
  expr.kind = Expr::Kind::Case;

  if (!check(TokenKind::KeywordWhen)) {
    expr.operand = ptr(parse_expr(0));
  }
  while (match(TokenKind::KeywordWhen)) {
    expr.args.push_back(ptr(parse_expr(0)));
    expect(TokenKind::KeywordThen, "'THEN'");
    expr.results.push_back(ptr(parse_expr(0)));
  }
  if (expr.args.empty()) {
    fail("expected WHEN after CASE");
  }
  if (match(TokenKind::KeywordElse)) {
    expr.else_result = ptr(parse_expr(0));
  }
  expect(TokenKind::KeywordEnd, "'END'");
  return expr;
}

Expr Parser::parse_cast() {
  expect(TokenKind::KeywordCast, "'CAST'");
  expect(TokenKind::LeftParen, "'('");
  Expr expr;
  expr.kind = Expr::Kind::Cast;
  expr.args.push_back(ptr(parse_expr(0)));
  expect(TokenKind::KeywordAs, "'AS'");
  expr.type_name = parse_type_name();
  expect(TokenKind::RightParen, "')'");
  return expr;
}

std::string Parser::parse_type_name() {
  if (!is_identifier_like(current_.kind)) {
    fail("expected a type name, got " + describe(current_));
  }
  std::string type = current_.text;
  advance();

  if (match(TokenKind::LeftParen)) {
    type += "(";
    bool first = true;
    while (!check(TokenKind::RightParen)) {
      if (!first) {
        expect(TokenKind::Comma, "','");
        type += ", ";
      }
      if (check(TokenKind::Number)) {
        type += current_.text;
        advance();
      } else if (is_identifier_like(current_.kind)) {
        type += current_.text;
        advance();
      } else {
        fail("expected a number in the type parameter list, got " +
             describe(current_));
      }
      first = false;
    }
    expect(TokenKind::RightParen, "')'");
    type += ")";
  }

  // Multi-word types: DOUBLE PRECISION, CHARACTER VARYING,
  // TIMESTAMP WITH TIME ZONE, INT UNSIGNED, ...
  while (check(TokenKind::Identifier) || check(TokenKind::KeywordWith) ||
         check(TokenKind::KeywordWithout)) {
    type += " " + current_.text;
    advance();
  }
  return type;
}

std::vector<std::string> Parser::parse_name_parts() {
  std::vector<std::string> parts;
  parts.push_back(expect_identifier("a name"));
  while (match(TokenKind::Dot)) parts.push_back(expect_identifier("a name"));
  return parts;
}

}  // namespace sql
