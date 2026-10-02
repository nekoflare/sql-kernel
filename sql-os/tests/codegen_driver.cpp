// Hosted driver for the codegen execution tests: compiles together with a
// generated translation unit and prints every emitted row as "cell|cell|..."
// (one line per row). With SQLOS_SHOW_HEADERS set, the column-header
// callback is installed too and each result set announces itself as a
// "H col|col" line before its rows. Port reads are preset from
// SQLOS_IN_VALUE (decimal or 0x-prefixed); recorded port writes are
// reported as a final "OUT <port>=<value> (x<count>)" line.

#include <cstdio>
#include <cstdlib>

#include "sqlos/runtime/sqlos_runtime.hpp"

extern "C" void sqlos_program(sqlos::SinkFn, sqlos::HeaderFn, void*);

namespace {

void print_cell(const sqlos::Value& v) {
  using K = sqlos::Kind;
  switch (v.kind) {
    case K::Null:
      std::fputs("NULL", stdout);
      break;
    case K::Int:
      std::printf("%lld", static_cast<long long>(v.as_int));
      break;
    case K::Real:
      std::printf("%g", v.as_real);
      break;
    case K::Text:
      std::fwrite(v.as_text.p, 1, v.as_text.len, stdout);
      break;
    case K::Bool:
      std::fputs(v.as_bool ? "true" : "false", stdout);
      break;
  }
}

void on_row(void*, const sqlos::Row* row) {
  for (sqlos::u32 i = 0; i < row->count; ++i) {
    if (i) std::fputc('|', stdout);
    print_cell(row->cells[i]);
  }
  std::fputc('\n', stdout);
}

void on_header(void*, const char* const* cols, sqlos::u32 ncols) {
  std::fputs("H ", stdout);
  for (sqlos::u32 i = 0; i < ncols; ++i) {
    if (i) std::fputc('|', stdout);
    std::fputs(cols[i], stdout);
  }
  std::fputc('\n', stdout);
}

}  // namespace

int main() {
  if (const char* in = std::getenv("SQLOS_IN_VALUE")) {
    sqlos::g_hosted_in_value = std::strtoull(in, nullptr, 0);
  }
  const bool show_headers = std::getenv("SQLOS_SHOW_HEADERS") != nullptr;
  sqlos_program(&on_row, show_headers ? &on_header : nullptr, nullptr);
  if (sqlos::g_hosted_out_count > 0) {
    std::printf("OUT %u=%llu (x%u)\n",
                static_cast<unsigned>(sqlos::g_hosted_out_port),
                static_cast<unsigned long long>(sqlos::g_hosted_out_value),
                static_cast<unsigned>(sqlos::g_hosted_out_count));
  }
  return 0;
}
