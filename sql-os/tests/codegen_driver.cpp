// Hosted driver for the codegen execution tests: compiles together with a
// generated translation unit and prints every emitted row as "cell|cell|..."
// (one line per row). With SQLOS_SHOW_HEADERS set, the column-header
// callback is installed too and each result set announces itself as a
// "H col|col" line before its rows. Port reads are preset from
// SQLOS_IN_VALUE (decimal or 0x-prefixed); recorded port writes are
// reported as a final "OUT <port>=<value> (x<count>)" line. The boot
// tables are filled with a small fake boot environment (see below).

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
  if (ncols == 0) return;  // end-of-result-set signal, not a header
  std::fputs("H ", stdout);
  for (sqlos::u32 i = 0; i < ncols; ++i) {
    if (i) std::fputc('|', stdout);
    std::fputs(cols[i], stdout);
  }
  std::fputc('\n', stdout);
}

// A small fake boot environment for the boot tables (boot_info,
// memory_map): they read like kernel-filled data, and only boot tests
// select from them.
void fill_boot_tables() {
  using sqlos::Value;
  sqlos::MemoryMapStore& map = sqlos::g_memory_map;
  map.count = 3;
  map.c0[0] = Value::i(0);
  map.c1[0] = Value::i(1048576);
  map.c2[0] = Value::t("reserved", 8);
  map.c0[1] = Value::i(1048576);
  map.c1[1] = Value::i(2146435072);
  map.c2[1] = Value::t("usable", 6);
  map.c0[2] = Value::i(4244434944);
  map.c1[2] = Value::i(16777216);
  map.c2[2] = Value::t("framebuffer", 11);
  sqlos::BootInfoStore& info = sqlos::g_boot_info;
  info.count = 1;
  info.c0[0] = Value::t("0xffff800000000000", 18);
  info.c1[0] = Value::t("0x100000", 8);
  info.c2[0] = Value::t("0xffffffff81000000", 18);
  info.c3[0] = Value::t("Limine", 6);
  info.c4[0] = Value::t("test", 4);
  info.c5[0] = Value::t("/vmlinuz root=/dev/sda1", 23);
  info.c6[0] = Value::t("x86bios", 7);
  info.c7[0] = Value::t("0xfd000000", 10);
  info.c8[0] = Value::i(1024);
  info.c9[0] = Value::i(768);
  info.c10[0] = Value::i(32);
  info.c11[0] = Value::i(1);
  info.c12[0] = Value::i(1767225600);
  info.c13[0] = Value::i(4096);  // fake CR3: a zeroed "PML4" at byte 4096
  sqlos::g_hosted_cr3 = 4096;   // the emulated walker's root
}

}  // namespace

int main() {
  if (const char* in = std::getenv("SQLOS_IN_VALUE")) {
    sqlos::g_hosted_in_value = std::strtoull(in, nullptr, 0);
  }
  fill_boot_tables();
  const bool show_headers = std::getenv("SQLOS_SHOW_HEADERS") != nullptr;
  sqlos_program(&on_row, show_headers ? &on_header : nullptr, nullptr);
  if (sqlos::g_hosted_out_count > 0) {
    std::printf("OUT %u=%llu (x%u)\n",
                static_cast<unsigned>(sqlos::g_hosted_out_port),
                static_cast<unsigned long long>(sqlos::g_hosted_out_value),
                static_cast<unsigned>(sqlos::g_hosted_out_count));
  }
  if (sqlos::g_hosted_cr3_count > 0) {
    std::printf("CR3 %lld (x%u)\n",
                static_cast<long long>(sqlos::g_hosted_cr3_write),
                static_cast<unsigned>(sqlos::g_hosted_cr3_count));
  }
  return 0;
}
