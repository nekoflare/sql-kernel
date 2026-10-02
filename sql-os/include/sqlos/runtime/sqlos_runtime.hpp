// SQL-OS runtime — the single header every `sqlos --emit` translation unit
// includes.
//
// Constraints:
//   * Freestanding: compiles with `g++ -std=c++17 -ffreestanding -c`.
//     No stdlib, no libc, no libm — every operation is implemented here or
//     via compiler builtins.
//   * Hosted under test: define SQLOS_HOSTED to replace x86 port I/O with
//     recordable hooks (g_hosted_* globals).
//   * Fixed-capacity everything: tables hold SQLOS_TABLE_CAP rows, images
//     SQLOS_MEM_BYTES bytes, generated text SQLOS_ARENA_BYTES. Overflow of
//     any budget traps (ud2) instead of allocating.
//
// Error model (matches the validator's contract, see README):
//   * Hard errors — integer division by zero, out-of-range port/address/
//     byte, NOT NULL violations, failed text->number casts, arena/table
//     overflow — call trap() and abort the program.
//   * Floating-point overflow produces ±inf (C semantics); NaN propagates.

#pragma once

// --- configuration ----------------------------------------------------------

#ifndef SQLOS_TABLE_CAP
#define SQLOS_TABLE_CAP 1024  // max rows per user table or CTE worklist
#endif
#ifndef SQLOS_MEM_BYTES
#define SQLOS_MEM_BYTES 65536  // size of each memory image (bytes)
#endif
#ifndef SQLOS_ARENA_BYTES
#define SQLOS_ARENA_BYTES 65536  // budget for all generated text values
#endif
#ifndef SQLOS_MAX_COLS
#define SQLOS_MAX_COLS 64  // max columns in one emitted row
#endif
#ifndef SQLOS_MEMMAP_CAP
#define SQLOS_MEMMAP_CAP 64  // rows in the boot memory_map table
#endif
#ifndef SQLOS_BOOT_INFO_ROWS
#define SQLOS_BOOT_INFO_ROWS 1  // boot_info is a single row
#endif

namespace sqlos {

// --- scalar types -----------------------------------------------------------

using i8 = signed char;
using u8 = unsigned char;
using i16 = short;
using u16 = unsigned short;
using i32 = int;
using u32 = unsigned int;
using i64 = long long;
using u64 = unsigned long long;
using f64 = double;

static_assert(sizeof(i64) == 8 && sizeof(u64) == 8, "need 64-bit long long");
static_assert(sizeof(f64) == 8, "need 64-bit double");
static_assert(sizeof(i32) == 4 && sizeof(i16) == 2, "fixed widths required");

constexpr i64 kI64Min = -9223372036854775807LL - 1;
constexpr i64 kI64Max = 9223372036854775807LL;

// Abort the program. In hosted tests this is a SIGILL — tests only reach it
// when they deliberately probe a contract violation.
[[noreturn]] inline void trap() { __builtin_trap(); }

// --- value model ------------------------------------------------------------

enum class Kind : u8 { Null = 0, Int, Real, Text, Bool };

// The one runtime representation of an SQL value. Trivially copyable,
// default-constructs to NULL so static storage needs no dynamic init.
struct Value {
  Kind kind;
  union {
    i64 as_int;
    f64 as_real;
    bool as_bool;
    struct {
      const char* p;
      u32 len;
    } as_text;
  };

  constexpr Value() : kind(Kind::Null), as_int(0) {}

  static constexpr Value null() { return Value(); }
  static Value i(i64 v) {
    Value x;
    x.kind = Kind::Int;
    x.as_int = v;
    return x;
  }
  static Value r(f64 v) {
    Value x;
    x.kind = Kind::Real;
    x.as_real = v;
    return x;
  }
  static Value b(bool v) {
    Value x;
    x.kind = Kind::Bool;
    x.as_bool = v;
    return x;
  }
  static Value t(const char* p, u32 n) {
    Value x;
    x.kind = Kind::Text;
    x.as_text.p = p;
    x.as_text.len = n;
    return x;
  }

  bool is_null() const { return kind == Kind::Null; }
};

// 24 bytes: 8-byte-aligned payload (i64/f64/pointer pair) plus the tag.
static_assert(sizeof(Value) <= 24, "Value must stay compact");

// One emitted row. `sqlos_program` hands rows to the host through Sink.
struct Row {
  u32 count;
  Value cells[SQLOS_MAX_COLS];
};

using SinkFn = void (*)(void* ctx, const Row* row);
// Column names of one result set, announced before any of its rows. The
// names have static storage — hosts may keep the pointers until the
// program ends. Each result set then ends with its statement: a final
// call with ncols == 0 (and cols == null) tells the host to print the
// buffered table. A host that passes a null HeaderFn to sqlos_program
// never hears about either.
using HeaderFn = void (*)(void* ctx, const char* const* cols, u32 ncols);
struct Sink {
  SinkFn fn;
  HeaderFn hdr;
  void* ctx;
};

// The entry point `sqlos --emit` generates, one per compiled program. Each
// result set first announces its column names through `header` (when it is
// not null), then hands every selected row to `emit`; `user` is passed
// through to both untouched. C linkage, because that is how the generator
// emits it.
extern "C" void sqlos_program(SinkFn emit, HeaderFn header, void* user);

// --- text arena -------------------------------------------------------------

// All runtime-produced text (concatenation, lower(), ...) lives here until
// the next sqlos_program() call. Literals point into .rodata instead.
inline char g_arena[SQLOS_ARENA_BYTES] = {};
inline u32 g_arena_pos = 0;

inline void arena_reset() { g_arena_pos = 0; }

inline char* arena_alloc(u32 n) {
  if (g_arena_pos + n > (u32)SQLOS_ARENA_BYTES || g_arena_pos + n < n) trap();
  char* p = g_arena + g_arena_pos;
  g_arena_pos += n;
  return p;
}

inline Value arena_text(const char* src, u32 n) {
  char* p = arena_alloc(n);
  for (u32 k = 0; k < n; ++k) p[k] = src[k];
  return Value::t(p, n);
}

inline void copy_text(char* dst, const char* src, u32 n) {
  for (u32 k = 0; k < n; ++k) dst[k] = src[k];
}

// --- memory images ----------------------------------------------------------

inline u8 g_memory[SQLOS_MEM_BYTES] = {};
inline u8 g_volatile_memory[SQLOS_MEM_BYTES] = {};

// --- boot tables ------------------------------------------------------------

// Read-only views over the boot environment, filled by the host before
// sqlos_program() runs (the kernel copies the Limine responses here; the
// hosted test driver installs a small fake). SQL can select from them but
// never insert, update or delete. The cN fields mirror the generated
// storage layout so scans read them like any other table.
struct MemoryMapStore {
  u32 count;
  Value c0[SQLOS_MEMMAP_CAP];  // base (physical address, INT)
  Value c1[SQLOS_MEMMAP_CAP];  // length (INT)
  Value c2[SQLOS_MEMMAP_CAP];  // type (TEXT: usable, reserved, ...)
};
inline MemoryMapStore g_memory_map = {};

struct BootInfoStore {
  u32 count;
  Value c0[SQLOS_BOOT_INFO_ROWS];   // hhdm_offset (TEXT hex)
  Value c1[SQLOS_BOOT_INFO_ROWS];   // kernel_phys_base (TEXT hex)
  Value c2[SQLOS_BOOT_INFO_ROWS];   // kernel_virt_base (TEXT hex)
  Value c3[SQLOS_BOOT_INFO_ROWS];   // bootloader (TEXT)
  Value c4[SQLOS_BOOT_INFO_ROWS];   // bootloader_version (TEXT)
  Value c5[SQLOS_BOOT_INFO_ROWS];   // cmdline (TEXT)
  Value c6[SQLOS_BOOT_INFO_ROWS];   // firmware (TEXT)
  Value c7[SQLOS_BOOT_INFO_ROWS];   // framebuffer_addr (TEXT hex)
  Value c8[SQLOS_BOOT_INFO_ROWS];   // framebuffer_width (INT)
  Value c9[SQLOS_BOOT_INFO_ROWS];   // framebuffer_height (INT)
  Value c10[SQLOS_BOOT_INFO_ROWS];  // framebuffer_bpp (INT)
  Value c11[SQLOS_BOOT_INFO_ROWS];  // module_count (INT)
  Value c12[SQLOS_BOOT_INFO_ROWS];  // boot_time (INT, unix seconds)
};
inline BootInfoStore g_boot_info = {};

// --- port I/O ---------------------------------------------------------------

#if defined(SQLOS_HOSTED)

// Hosted test hooks: in* returns the preset value, out* records the write.
inline u64 g_hosted_in_value = 0;
inline u16 g_hosted_in_port = 0;
inline u64 g_hosted_out_value = 0;
inline u16 g_hosted_out_port = 0;
inline u32 g_hosted_out_count = 0;

inline u8 inb(u16 port) {
  g_hosted_in_port = port;
  return (u8)g_hosted_in_value;
}
inline u16 inw(u16 port) {
  g_hosted_in_port = port;
  return (u16)g_hosted_in_value;
}
inline u32 inl(u16 port) {
  g_hosted_in_port = port;
  return (u32)g_hosted_in_value;
}
inline void outb(u16 port, u8 v) {
  g_hosted_out_port = port;
  g_hosted_out_value = v;
  ++g_hosted_out_count;
}
inline void outw(u16 port, u16 v) {
  g_hosted_out_port = port;
  g_hosted_out_value = v;
  ++g_hosted_out_count;
}
inline void outl(u16 port, u32 v) {
  g_hosted_out_port = port;
  g_hosted_out_value = v;
  ++g_hosted_out_count;
}

#elif defined(__x86_64__) || defined(__i386__)

inline u8 inb(u16 port) {
  u8 v;
  __asm__ volatile("inb %%dx, %%al" : "=a"(v) : "d"(port));
  return v;
}
inline u16 inw(u16 port) {
  u16 v;
  __asm__ volatile("inw %%dx, %%ax" : "=a"(v) : "d"(port));
  return v;
}
inline u32 inl(u16 port) {
  u32 v;
  __asm__ volatile("inl %%dx, %%eax" : "=a"(v) : "d"(port));
  return v;
}
inline void outb(u16 port, u8 v) {
  __asm__ volatile("outb %%al, %%dx" : : "a"(v), "d"(port));
}
inline void outw(u16 port, u16 v) {
  __asm__ volatile("outw %%ax, %%dx" : : "a"(v), "d"(port));
}
inline void outl(u16 port, u32 v) {
  __asm__ volatile("outl %%eax, %%dx" : : "a"(v), "d"(port));
}

#else
#error "sqlos runtime: port I/O needs x86; build tests with -DSQLOS_HOSTED"
#endif

// --- math (no libm) ---------------------------------------------------------

inline f64 fabs_f64(f64 x) { return x < 0 ? -x : x; }

inline bool f64_integral(f64 x) {
  if (x >= -9.0e18 && x < 9.0e18) {
    i64 t = (i64)x;
    return (f64)t == x;
  }
  return true;  // |x| too large for a fraction to be representable
}

inline f64 floor_f64(f64 x) {
  if (x >= -9.0e18 && x < 9.0e18) {
    i64 t = (i64)x;  // toward zero
    if ((f64)t > x) --t;
    return (f64)t;
  }
  return x;
}

inline f64 ceil_f64(f64 x) {
  if (x >= -9.0e18 && x < 9.0e18) {
    i64 t = (i64)x;
    if ((f64)t < x) ++t;
    return (f64)t;
  }
  return x;
}

// Round half away from zero (SQL round()).
inline f64 round_f64(f64 x) {
  if (!f64_integral(x)) {
    f64 a = fabs_f64(x);
    f64 r = floor_f64(a + 0.5);
    return x < 0 ? -r : r;
  }
  return x;
}

inline f64 fmod_f64(f64 a, f64 b) {
  if (b == 0) trap();
  f64 q = a / b;
  if (q >= -9.0e18 && q < 9.0e18) {
    i64 t = (i64)q;  // toward zero
    return a - b * (f64)t;
  }
  return a - b * q;  // |q| too large: no fractional part left
}

// 2^m for |m| <= 512, by direct exponent-field adjustment.
inline f64 pow2_small(i64 m) {
  union {
    f64 f;
    u64 u;
  } v;
  v.f = 1.0;
  v.u += ((u64)m) << 52;
  return v.f;
}

// exp() via range reduction to |x| <= ln2/2-ish plus a Taylor series.
inline f64 exp_f64(f64 x) {
  if (x != x) return x;
  if (x > 709.79) return 1.0e308 * 1.7976931348623157e308;  // -> +inf
  if (x < -745.14) return 0;
  const f64 ln2 = 0.6931471805599453094;
  i64 m = (i64)(x / ln2 + (x >= 0 ? 0.5 : -0.5));
  f64 f = x - (f64)m * ln2;
  f64 term = 1;
  f64 sum = 1;
  for (u32 k = 1; k < 40; ++k) {
    term *= f / (f64)k;
    sum += term;
    if (term == 0) break;
  }
  while (m > 512) {
    sum *= pow2_small(512);
    m -= 512;
  }
  while (m < -512) {
    sum *= pow2_small(-512);
    m += 512;
  }
  return sum * pow2_small(m);
}

// ln() via mantissa normalization + atanh series. Argument must be > 0.
inline f64 ln_f64(f64 x) {
  if (!(x > 0)) trap();
  i64 e = 0;
  while (x >= 2) {
    x *= 0.5;
    ++e;
  }
  while (x < 1) {
    x *= 2;
    --e;
  }
  // ln(m) = 2 * atanh(z), z = (m-1)/(m+1), |z| <= 1/3.
  f64 z = (x - 1) / (x + 1);
  f64 z2 = z * z;
  f64 s = 0;
  f64 t = z;
  for (u32 k = 0; k < 64 && t != 0; ++k) {
    s += t / (f64)(2 * k + 1);
    t *= z2;
  }
  return 2 * s + (f64)e * 0.6931471805599453094;
}

// pow(): integer exponents are computed exactly by binary exponentiation;
// fractional exponents use exp(y*ln(x)) for x > 0 and trap for x < 0.
inline f64 pow_f64(f64 x, f64 y) {
  if (y == 0) return 1;
  if (x == 0) {
    if (y > 0) return 0;
    trap();  // 0 ^ negative
  }
  f64 ay = fabs_f64(y);
  if (f64_integral(ay) && ay <= 1024) {
    i64 n = (i64)ay;
    f64 base = x < 0 ? -x : x;
    f64 r = 1;
    while (n > 0) {
      if (n & 1) r *= base;
      base *= base;
      n >>= 1;
    }
    if ((i64)ay & 1) r = -r;
    if (y < 0) r = 1 / r;
    return r;
  }
  if (x < 0) trap();  // fractional power of a negative base
  return exp_f64(y * ln_f64(x));
}

// --- formatting (no printf) -------------------------------------------------

// i64 -> decimal into `out` (>= 21 chars). Returns the length.
inline u32 fmt_i64(i64 v, char* out) {
  char* w = out;
  u64 m;
  if (v < 0) {
    *w++ = '-';
    m = (u64)(-(v + 1)) + 1;  // safe for kI64Min
  } else {
    m = (u64)v;
  }
  char digits[20];
  u32 n = 0;
  do {
    digits[n++] = (char)('0' + (m % 10));
    m /= 10;
  } while (m != 0);
  for (u32 k = n; k > 0; --k) *w++ = digits[k - 1];
  return (u32)(w - out);
}

// f64 -> shortest reasonable decimal (15 significant digits, trailing zeros
// trimmed; scientific notation outside 1e-4..1e17). `out` >= 48 chars.
inline u32 fmt_f64(f64 v, char* out) {
  char* w = out;
  if (v != v) {
    const char* s = "NaN";
    for (u32 k = 0; s[k]; ++k) *w++ = s[k];
    return (u32)(w - out);
  }
  if (v < 0) {
    *w++ = '-';
    v = -v;
  }
  if (v > 1.7976931348623157e308) {
    const char* s = "inf";
    for (u32 k = 0; s[k]; ++k) *w++ = s[k];
    return (u32)(w - out);
  }
  if (v == 0) {
    *w++ = '0';
    return (u32)(w - out);
  }

  // Scale into [1e14, 1e15) -> exactly 15 significant digits.
  i32 exp10 = 0;
  u32 guard = 0;
  while (v >= 1.0e15 && guard < 400) {
    v /= 10;
    ++exp10;
    ++guard;
  }
  guard = 0;
  while (v < 1.0e14 && guard < 400) {
    v *= 10;
    --exp10;
    ++guard;
  }
  u64 d = (u64)(v + 0.5);
  if (d >= 1000000000000000ull) {
    d /= 10;
    ++exp10;
  }

  char dig[16];
  u64 m = d;
  for (int k = 14; k >= 0; --k) {
    dig[k] = (char)('0' + (m % 10));
    m /= 10;
  }
  i32 pos = 15 + exp10;  // digits before the decimal point (may be <= 0)

  auto emit_sci = [&]() {
    *w++ = dig[0];
    u32 last = 14;
    while (last > 1 && dig[last] == '0') --last;
    if (last >= 1) {
      *w++ = '.';
      for (u32 k = 1; k <= last; ++k) *w++ = dig[k];
    }
    *w++ = 'e';
    i32 e = pos - 1;
    if (e < 0) {
      *w++ = '-';
      e = -e;
    } else {
      *w++ = '+';
    }
    char eb[8];
    u32 n = 0;
    do {
      eb[n++] = (char)('0' + (e % 10));
      e /= 10;
    } while (e != 0);
    for (u32 k = n; k > 0; --k) *w++ = eb[k - 1];
  };

  if (pos > 16 || pos < -4) {
    emit_sci();
    return (u32)(w - out);
  }
  if (pos >= 15) {
    // Pure integer: trailing zeros are significant, never trim.
    for (u32 k = 0; k < 15; ++k) *w++ = dig[k];
    for (i32 k = 15; k < pos; ++k) *w++ = '0';
    return (u32)(w - out);
  }
  if (pos <= 0) {
    *w++ = '0';
    *w++ = '.';
    char* frac_start = w;
    for (i32 k = 0; k < -pos; ++k) *w++ = '0';
    for (u32 k = 0; k < 15; ++k) *w++ = dig[k];
    while (w > frac_start && w[-1] == '0') --w;
  } else {
    for (i32 k = 0; k < pos; ++k) *w++ = dig[k];
    *w++ = '.';
    char* frac_start = w;
    for (u32 k = (u32)pos; k < 15; ++k) *w++ = dig[k];
    while (w > frac_start && w[-1] == '0') --w;
    if (w == frac_start) --w;  // drop the dangling '.'
  }
  return (u32)(w - out);
}

inline u32 fmt_bool(bool v, char* out) {
  const char* s = v ? "true" : "false";
  u32 n = 0;
  while (s[n]) {
    out[n] = s[n];
    ++n;
  }
  return n;
}

// Uppercase, no 0x prefix, minimal digits; '-' for negatives.
inline u32 fmt_hex(i64 v, char* out) {
  char* w = out;
  u64 m;
  if (v < 0) {
    *w++ = '-';
    m = (u64)(-(v + 1)) + 1;
  } else {
    m = (u64)v;
  }
  char digits[16];
  u32 n = 0;
  const char* kHex = "0123456789ABCDEF";
  do {
    digits[n++] = kHex[m % 16];
    m /= 16;
  } while (m != 0);
  for (u32 k = n; k > 0; --k) *w++ = digits[k - 1];
  return (u32)(w - out);
}

// --- text -> number ---------------------------------------------------------

inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Strict integer: [ws] [+|-] digits. No fraction, no trailing junk.
inline bool parse_i64(const char* s, u32 n, i64* out) {
  u32 i = 0;
  while (i < n && is_space(s[i])) ++i;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  if (i >= n || !is_digit(s[i])) return false;
  u64 mag = 0;
  while (i < n && is_digit(s[i])) {
    u64 next = mag * 10 + (u64)(s[i] - '0');
    if (next < mag) return false;  // overflow
    mag = next;
    ++i;
  }
  while (i < n && is_space(s[i])) ++i;
  if (i != n) return false;
  if (neg) {
    if (mag > 9223372036854775808ull) return false;
    *out = mag == 9223372036854775808ull ? kI64Min : -(i64)mag;
  } else {
    if (mag > (u64)kI64Max) return false;
    *out = (i64)mag;
  }
  return true;
}

// [ws] [+|-] digits [. digits] [eE [+|-] digits]
inline bool parse_f64(const char* s, u32 n, f64* out) {
  u32 i = 0;
  while (i < n && is_space(s[i])) ++i;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  f64 val = 0;
  bool any = false;
  while (i < n && is_digit(s[i])) {
    val = val * 10 + (f64)(s[i] - '0');
    ++i;
    any = true;
  }
  if (i < n && s[i] == '.') {
    ++i;
    f64 scale = 0.1;
    while (i < n && is_digit(s[i])) {
      val += (f64)(s[i] - '0') * scale;
      scale /= 10;
      ++i;
      any = true;
    }
  }
  if (!any) return false;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool eneg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
      eneg = s[i] == '-';
      ++i;
    }
    if (i >= n || !is_digit(s[i])) return false;
    i64 e = 0;
    while (i < n && is_digit(s[i]) && e < 500) {
      e = e * 10 + (s[i] - '0');
      ++i;
    }
    while (i < n && is_digit(s[i])) ++i;  // saturate
    if (eneg) e = -e;
    if (e > 0) {
      while (e-- > 0) val *= 10;
    } else {
      while (e++ < 0) val /= 10;
    }
  }
  while (i < n && is_space(s[i])) ++i;
  if (i != n) return false;
  *out = neg ? -val : val;
  return true;
}

// --- text primitives --------------------------------------------------------

inline i32 cmp_bytes(const char* a, u32 an, const char* b, u32 bn) {
  u32 n = an < bn ? an : bn;
  for (u32 k = 0; k < n; ++k) {
    u8 ca = (u8)a[k];
    u8 cb = (u8)b[k];
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (an == bn) return 0;
  return an < bn ? -1 : 1;
}

inline char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}
inline char ascii_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

inline bool is_space_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

// SQL LIKE / ILIKE matcher with backtracking. `esc` < 0 disables escapes.
inline bool like_match(const char* s, u32 sl, const char* p, u32 pl, i32 esc,
                       bool icase) {
  auto eq = [&](char a, char b) {
    if (icase) return ascii_lower(a) == ascii_lower(b);
    return a == b;
  };
  u32 si = 0;
  u32 pi = 0;
  u32 star = 0xFFFFFFFFu;
  u32 ss = 0;
  while (si < sl) {
    if (pi < pl) {
      char pc = p[pi];
      if (esc >= 0 && pc == (char)esc && pi + 1 < pl) {
        if (eq(p[pi + 1], s[si])) {
          ++si;
          pi += 2;
          continue;
        }
      } else if (pc == '%') {
        star = pi++;
        ss = si;
        continue;
      } else if (pc == '_' || eq(pc, s[si])) {
        ++si;
        ++pi;
        continue;
      }
    }
    if (star != 0xFFFFFFFFu) {
      pi = star + 1;
      si = ++ss;
      continue;
    }
    return false;
  }
  while (pi < pl && p[pi] == '%') ++pi;
  return pi == pl;
}

// --- SQL operators ----------------------------------------------------------

// Numeric view of an Int/Real value; traps on anything else.
inline f64 as_f64(Value v) {
  if (v.kind == Kind::Real) return v.as_real;
  if (v.kind == Kind::Int) return (f64)v.as_int;
  trap();
}
inline bool is_num(Value v) { return v.kind == Kind::Int || v.kind == Kind::Real; }

// WHERE / CASE / truth contexts: NULL is not true; anything else traps
// (validated programs only ever put booleans here).
inline bool truth(Value v) {
  if (v.kind == Kind::Null) return false;
  if (v.kind == Kind::Bool) return v.as_bool;
  trap();
}

inline Value v_not(Value a) {
  if (a.kind == Kind::Null) return Value::null();
  if (a.kind == Kind::Bool) return Value::b(!a.as_bool);
  trap();
}

inline Value v_and(Value a, Value b) {
  if ((a.kind != Kind::Bool && a.kind != Kind::Null) ||
      (b.kind != Kind::Bool && b.kind != Kind::Null)) trap();
  if (a.kind == Kind::Bool && !a.as_bool) return Value::b(false);
  if (b.kind == Kind::Bool && !b.as_bool) return Value::b(false);
  if (a.kind == Kind::Null || b.kind == Kind::Null) return Value::null();
  return Value::b(true);
}

inline Value v_or(Value a, Value b) {
  if ((a.kind != Kind::Bool && a.kind != Kind::Null) ||
      (b.kind != Kind::Bool && b.kind != Kind::Null)) trap();
  if (a.kind == Kind::Bool && a.as_bool) return Value::b(true);
  if (b.kind == Kind::Bool && b.as_bool) return Value::b(true);
  if (a.kind == Kind::Null || b.kind == Kind::Null) return Value::null();
  return Value::b(false);
}

inline i64 wrap_add(i64 a, i64 b) { return (i64)((u64)a + (u64)b); }
inline i64 wrap_sub(i64 a, i64 b) { return (i64)((u64)a - (u64)b); }
inline i64 wrap_mul(i64 a, i64 b) { return (i64)((u64)a * (u64)b); }

inline Value v_neg(Value a) {
  if (a.kind == Kind::Null) return Value::null();
  if (a.kind == Kind::Int) return Value::i(wrap_sub(0, a.as_int));
  if (a.kind == Kind::Real) return Value::r(-a.as_real);
  trap();
}

inline Value v_bitnot(Value a) {
  if (a.kind == Kind::Null) return Value::null();
  if (a.kind == Kind::Int) return Value::i(~a.as_int);
  if (a.kind == Kind::Real) return Value::i(~(i64)a.as_real);
  trap();
}

// Shared skeleton for + - * % on numbers.
#define SQLOS_BINOP(NAME, INT_EXPR, REAL_EXPR)                             \
  inline Value NAME(Value a, Value b) {                                    \
    if (a.is_null() || b.is_null()) return Value::null();                  \
    if (a.kind == Kind::Int && b.kind == Kind::Int) {                      \
      return Value::i(INT_EXPR);                                           \
    }                                                                      \
    if (is_num(a) && is_num(b)) {                                          \
      return Value::r(REAL_EXPR);                                          \
    }                                                                      \
    trap();                                                                \
  }

SQLOS_BINOP(v_add, wrap_add(a.as_int, b.as_int), as_f64(a) + as_f64(b))
SQLOS_BINOP(v_sub, wrap_sub(a.as_int, b.as_int), as_f64(a) - as_f64(b))
SQLOS_BINOP(v_mul, wrap_mul(a.as_int, b.as_int), as_f64(a) * as_f64(b))
#undef SQLOS_BINOP

inline Value v_mod(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int) {
    if (b.as_int == 0) trap();
    if (b.as_int == -1) return Value::i(0);  // LLONG_MIN % -1 is UB
    return Value::i(a.as_int % b.as_int);
  }
  if (is_num(a) && is_num(b)) return Value::r(fmod_f64(as_f64(a), as_f64(b)));
  trap();
}

// Integer division truncates (PostgreSQL); division by zero traps.
inline Value v_div(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int) {
    if (b.as_int == 0) trap();
    if (a.as_int == kI64Min && b.as_int == -1) return Value::i(kI64Min);
    return Value::i(a.as_int / b.as_int);
  }
  if (is_num(a) && is_num(b)) {
    f64 bb = as_f64(b);
    if (bb == 0) trap();
    return Value::r(as_f64(a) / bb);
  }
  trap();
}

inline Value v_pow(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (!is_num(a) || !is_num(b)) trap();
  return Value::r(pow_f64(as_f64(a), as_f64(b)));
}

inline Value v_band(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int)
    return Value::i(a.as_int & b.as_int);
  trap();
}
inline Value v_bor(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int)
    return Value::i(a.as_int | b.as_int);
  trap();
}
inline Value v_bxor(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int)
    return Value::i(a.as_int ^ b.as_int);
  trap();
}
inline Value v_shl(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int) {
    if (b.as_int < 0 || b.as_int > 63) trap();
    return Value::i((i64)((u64)a.as_int << b.as_int));
  }
  trap();
}
inline Value v_shr(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind == Kind::Int && b.kind == Kind::Int) {
    if (b.as_int < 0 || b.as_int > 63) trap();
    if (b.as_int == 0 || a.as_int >= 0) return Value::i(a.as_int >> b.as_int);
    // Arithmetic shift with defined behaviour for negative values.
    u64 shifted = (u64)a.as_int >> b.as_int;
    u64 fill = ~(u64)0 << (64 - b.as_int);
    return Value::i((i64)(shifted | fill));
  }
  trap();
}

enum class Cmp : u8 { Lt, Eq, Gt, Null, Bad };

inline Cmp cmp_values(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Cmp::Null;
  if (a.kind == Kind::Int && b.kind == Kind::Int)
    return a.as_int < b.as_int ? Cmp::Lt
           : (a.as_int > b.as_int ? Cmp::Gt : Cmp::Eq);
  if (is_num(a) && is_num(b)) {
    f64 x = as_f64(a);
    f64 y = as_f64(b);
    return x < y ? Cmp::Lt : (x > y ? Cmp::Gt : Cmp::Eq);
  }
  if (a.kind == Kind::Bool && b.kind == Kind::Bool)
    return a.as_bool == b.as_bool ? Cmp::Eq : Cmp::Bad;  // ordered below
  if (a.kind == Kind::Text && b.kind == Kind::Text) {
    i32 c = cmp_bytes(a.as_text.p, a.as_text.len, b.as_text.p, b.as_text.len);
    return c < 0 ? Cmp::Lt : (c > 0 ? Cmp::Gt : Cmp::Eq);
  }
  return Cmp::Bad;  // bools and mixed kinds: no total order (see v_eq)
}

// Booleans compare unordered: = and <> work, < <= > >= are NULL.
inline Value v_eq(Value a, Value b) {
  Cmp c = cmp_values(a, b);
  if (c == Cmp::Null) return Value::null();
  if (c == Cmp::Bad) {
    if (a.kind == Kind::Bool && b.kind == Kind::Bool)
      return Value::b(a.as_bool == b.as_bool);
    trap();
  }
  return Value::b(c == Cmp::Eq);
}
inline Value v_neq(Value a, Value b) {
  Cmp c = cmp_values(a, b);
  if (c == Cmp::Null) return Value::null();
  if (c == Cmp::Bad) {
    if (a.kind == Kind::Bool && b.kind == Kind::Bool)
      return Value::b(a.as_bool != b.as_bool);
    trap();
  }
  return Value::b(c != Cmp::Eq);
}
inline Value v_lt(Value a, Value b) {
  Cmp c = cmp_values(a, b);
  if (c == Cmp::Null) return Value::null();
  if (c == Cmp::Bad) trap();
  return Value::b(c == Cmp::Lt);
}
inline Value v_le(Value a, Value b) {
  Cmp c = cmp_values(a, b);
  if (c == Cmp::Null) return Value::null();
  if (c == Cmp::Bad) trap();
  return Value::b(c != Cmp::Gt);
}
inline Value v_gt(Value a, Value b) {
  Cmp c = cmp_values(a, b);
  if (c == Cmp::Null) return Value::null();
  if (c == Cmp::Bad) trap();
  return Value::b(c == Cmp::Gt);
}
inline Value v_ge(Value a, Value b) {
  Cmp c = cmp_values(a, b);
  if (c == Cmp::Null) return Value::null();
  if (c == Cmp::Bad) trap();
  return Value::b(c != Cmp::Lt);
}

inline Value v_cat(Value a, Value b) {
  if (a.is_null() || b.is_null()) return Value::null();
  if (a.kind != Kind::Text || b.kind != Kind::Text) trap();
  u32 total = a.as_text.len + b.as_text.len;
  char* p = arena_alloc(total);
  copy_text(p, a.as_text.p, a.as_text.len);
  copy_text(p + a.as_text.len, b.as_text.p, b.as_text.len);
  return Value::t(p, total);
}

// x IN (items...) with correct three-valued logic.
inline Value v_in(Value x, const Value* items, u32 n) {
  bool any_null = false;
  for (u32 k = 0; k < n; ++k) {
    Value c = v_eq(x, items[k]);
    if (c.kind == Kind::Bool && c.as_bool) return Value::b(true);
    if (c.kind == Kind::Null) any_null = true;
  }
  return any_null ? Value::null() : Value::b(false);
}

// --- IS tests ---------------------------------------------------------------

inline bool v_is_null(Value a) { return a.is_null(); }
inline bool v_is_true(Value a) {
  if (a.kind == Kind::Bool) return a.as_bool;
  if (a.kind == Kind::Null) return false;
  trap();
}
inline bool v_is_false(Value a) {
  if (a.kind == Kind::Bool) return !a.as_bool;
  if (a.kind == Kind::Null) return false;
  trap();
}

// --- LIKE -------------------------------------------------------------------

inline Value v_like(Value s, Value pat, bool icase) {
  if (s.is_null() || pat.is_null()) return Value::null();
  if (s.kind != Kind::Text || pat.kind != Kind::Text) trap();
  return Value::b(like_match(s.as_text.p, s.as_text.len, pat.as_text.p,
                             pat.as_text.len, -1, icase));
}

inline Value v_like_esc(Value s, Value pat, Value esc, bool icase) {
  if (s.is_null() || pat.is_null() || esc.is_null()) return Value::null();
  if (s.kind != Kind::Text || pat.kind != Kind::Text) trap();
  if (esc.kind != Kind::Text || esc.as_text.len != 1) trap();
  return Value::b(like_match(s.as_text.p, s.as_text.len, pat.as_text.p,
                             pat.as_text.len, (i32)(u8)esc.as_text.p[0],
                             icase));
}

// --- CAST -------------------------------------------------------------------

enum class CType : u8 { Int, Real, Text, Bool };

inline Value v_cast(Value v, CType t) {
  if (v.is_null()) return Value::null();
  switch (t) {
    case CType::Int: {
      if (v.kind == Kind::Int) return v;
      if (v.kind == Kind::Real) return Value::i((i64)v.as_real);
      if (v.kind == Kind::Bool) return Value::i(v.as_bool ? 1 : 0);
      if (v.kind == Kind::Text) {
        i64 out;
        if (!parse_i64(v.as_text.p, v.as_text.len, &out)) trap();
        return Value::i(out);
      }
      trap();
    }
    case CType::Real: {
      if (v.kind == Kind::Real) return v;
      if (v.kind == Kind::Int) return Value::r((f64)v.as_int);
      if (v.kind == Kind::Bool) return Value::r(v.as_bool ? 1.0 : 0.0);
      if (v.kind == Kind::Text) {
        f64 out;
        if (!parse_f64(v.as_text.p, v.as_text.len, &out)) trap();
        return Value::r(out);
      }
      trap();
    }
    case CType::Text: {
      char buf[48];
      if (v.kind == Kind::Text) return v;
      if (v.kind == Kind::Int) return arena_text(buf, fmt_i64(v.as_int, buf));
      if (v.kind == Kind::Real) return arena_text(buf, fmt_f64(v.as_real, buf));
      if (v.kind == Kind::Bool) return arena_text(buf, fmt_bool(v.as_bool, buf));
      trap();
    }
    case CType::Bool: {
      if (v.kind == Kind::Bool) return v;
      if (v.kind == Kind::Int) return Value::b(v.as_int != 0);
      if (v.kind == Kind::Real) return Value::b(v.as_real != 0);
      trap();  // text -> bool has no portable spelling; refuse at runtime
    }
  }
  trap();
}

// --- builtin scalar functions ----------------------------------------------

inline Value fn_length(Value s) {
  if (s.is_null()) return Value::null();
  if (s.kind != Kind::Text) trap();
  return Value::i((i64)s.as_text.len);
}

inline Value fn_lower(Value s) {
  if (s.is_null()) return Value::null();
  if (s.kind != Kind::Text) trap();
  char* p = arena_alloc(s.as_text.len);
  for (u32 k = 0; k < s.as_text.len; ++k) p[k] = ascii_lower(s.as_text.p[k]);
  return Value::t(p, s.as_text.len);
}

inline Value fn_upper(Value s) {
  if (s.is_null()) return Value::null();
  if (s.kind != Kind::Text) trap();
  char* p = arena_alloc(s.as_text.len);
  for (u32 k = 0; k < s.as_text.len; ++k) p[k] = ascii_upper(s.as_text.p[k]);
  return Value::t(p, s.as_text.len);
}

// Argument conversion for integer-ish function parameters (start/len/ndigits).
inline bool arg_i64(Value v, i64* out) {
  if (v.kind == Kind::Int) {
    *out = v.as_int;
    return true;
  }
  if (v.kind == Kind::Real) {
    *out = (i64)v.as_real;
    return true;
  }
  return false;  // caller propagates NULL or traps
}

// PostgreSQL substr: 1-based, negative start counts from the end, negative
// or omitted length runs to the end.
inline Value substr_impl(Value s, Value start, const Value* len) {
  if (s.is_null() || start.is_null()) return Value::null();
  if (len != nullptr && len->is_null()) return Value::null();
  if (s.kind != Kind::Text) trap();
  i64 st;
  if (!arg_i64(start, &st)) trap();
  i64 sl = (i64)s.as_text.len;
  i64 from = st < 0 ? sl + st : st - 1;
  if (from < 0) from = 0;
  if (from > sl) from = sl;
  i64 to = sl;
  if (len != nullptr) {
    i64 ln;
    if (!arg_i64(*len, &ln)) trap();
    if (ln < 0) to = sl;
    else to = from + ln > sl ? sl : from + ln;
  }
  if (to < from) to = from;
  return Value::t(s.as_text.p + from, (u32)(to - from));
}

inline Value fn_substr(Value s, Value start) { return substr_impl(s, start, nullptr); }
inline Value fn_substr(Value s, Value start, Value len) {
  return substr_impl(s, start, &len);
}

inline Value trim_impl(Value s, bool left, bool right) {
  if (s.is_null()) return Value::null();
  if (s.kind != Kind::Text) trap();
  u32 lo = 0;
  u32 hi = s.as_text.len;
  if (left)
    while (lo < hi && is_space_char(s.as_text.p[lo])) ++lo;
  if (right)
    while (hi > lo && is_space_char(s.as_text.p[hi - 1])) --hi;
  return Value::t(s.as_text.p + lo, hi - lo);
}

inline Value fn_trim(Value s) { return trim_impl(s, true, true); }
inline Value fn_ltrim(Value s) { return trim_impl(s, true, false); }
inline Value fn_rtrim(Value s) { return trim_impl(s, false, true); }

inline Value fn_replace(Value s, Value from, Value to) {
  if (s.is_null() || from.is_null() || to.is_null()) return Value::null();
  if (s.kind != Kind::Text || from.kind != Kind::Text || to.kind != Kind::Text)
    trap();
  u32 fl = from.as_text.len;
  if (fl == 0) return s;
  // Pass 1: total length.
  u64 total = 0;
  u32 i = 0;
  while (i < s.as_text.len) {
    if (i + fl <= s.as_text.len &&
        cmp_bytes(s.as_text.p + i, fl, from.as_text.p, fl) == 0) {
      total += to.as_text.len;
      i += fl;
    } else {
      total += 1;
      ++i;
    }
    if (total > (u64)SQLOS_ARENA_BYTES) trap();
  }
  // Pass 2: fill.
  char* p = arena_alloc((u32)total);
  u32 w = 0;
  i = 0;
  while (i < s.as_text.len) {
    if (i + fl <= s.as_text.len &&
        cmp_bytes(s.as_text.p + i, fl, from.as_text.p, fl) == 0) {
      copy_text(p + w, to.as_text.p, to.as_text.len);
      w += to.as_text.len;
      i += fl;
    } else {
      p[w++] = s.as_text.p[i++];
    }
  }
  return Value::t(p, w);
}

inline Value fn_round(Value x) {
  if (x.is_null()) return Value::null();
  if (x.kind == Kind::Int) return x;  // int round(x) = x
  if (x.kind != Kind::Real) trap();
  return Value::r(round_f64(x.as_real));
}

inline Value fn_round(Value x, Value ndigits) {
  if (x.is_null() || ndigits.is_null()) return Value::null();
  if (x.kind == Kind::Int) return x;
  if (x.kind != Kind::Real) trap();
  i64 n;
  if (!arg_i64(ndigits, &n)) trap();
  if (n > 308 || n < -308) trap();
  f64 scale = 1;
  for (i64 k = 0; k < (n < 0 ? -n : n); ++k) scale *= 10;
  if (n < 0) scale = 1 / scale;
  return Value::r(round_f64(x.as_real * scale) / scale);
}

inline Value fn_floor(Value x) {
  if (x.is_null()) return Value::null();
  if (x.kind == Kind::Int) return x;
  if (x.kind != Kind::Real) trap();
  return Value::r(floor_f64(x.as_real));
}

inline Value fn_ceil(Value x) {
  if (x.is_null()) return Value::null();
  if (x.kind == Kind::Int) return x;
  if (x.kind != Kind::Real) trap();
  return Value::r(ceil_f64(x.as_real));
}

inline Value fn_abs(Value x) {
  if (x.is_null()) return Value::null();
  if (x.kind == Kind::Int) {
    if (x.as_int == kI64Min) trap();
    return Value::i(x.as_int < 0 ? -x.as_int : x.as_int);
  }
  if (x.kind == Kind::Real) return Value::r(fabs_f64(x.as_real));
  trap();
}

inline Value v_coalesce2(Value a, Value b) { return a.is_null() ? b : a; }

inline Value fn_nullif(Value a, Value b) {
  Value c = v_eq(a, b);
  if (c.kind == Kind::Bool && c.as_bool) return Value::null();
  return a;
}

inline Value fn_hex(Value x) {
  if (x.is_null()) return Value::null();
  if (x.kind != Kind::Int) trap();  // real -> hex has no exact meaning
  char buf[24];
  return arena_text(buf, fmt_hex(x.as_int, buf));
}

// --- system-table access ----------------------------------------------------

// Convert an expression to a port/number with range checks (traps outside).
inline u16 io_port(Value v) {
  if (v.is_null()) trap();
  i64 p;
  if (!arg_i64(v, &p)) trap();
  if (p < 0 || p > 65535) trap();
  return (u16)p;
}

inline u64 io_value(Value v, u32 width) {
  if (v.is_null()) trap();
  i64 n;
  if (!arg_i64(v, &n)) trap();
  if (n < 0) trap();
  u64 max = width >= 64 ? ~(u64)0 : ((u64)1 << width) - 1;
  if ((u64)n > max) trap();
  return (u64)n;
}

inline i64 mem_addr(Value v) {
  if (v.is_null()) trap();
  i64 a;
  if (!arg_i64(v, &a)) trap();
  if (a < 0 || a >= (i64)SQLOS_MEM_BYTES) trap();
  return a;
}

inline u32 mem_idx(i64 addr) {
  if (addr < 0 || addr >= (i64)SQLOS_MEM_BYTES) trap();
  return (u32)addr;
}

inline u8 mem_byte(Value v) {
  if (v.is_null()) trap();
  i64 n;
  if (!arg_i64(v, &n)) trap();
  if (n < 0 || n > 255) trap();
  return (u8)n;
}

}  // namespace sqlos
