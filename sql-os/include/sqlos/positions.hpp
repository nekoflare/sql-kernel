// Maps statements back to their position in the source. The AST carries no
// spans, so validation issues are located by re-tokenizing the input: each
// non-empty statement starts at the first token after the previous `;`.

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace sqlos {

struct SourcePos {
  std::size_t line = 1;
  std::size_t column = 1;
};

// Start position of every non-empty statement, aligned one-to-one with
// Parser::parse_all().
std::vector<SourcePos> statement_positions(std::string_view input);

}  // namespace sqlos
