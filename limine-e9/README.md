# SQL-OS on Limine — SQL runs, `outb` hits port 0xE9

This is [limine-bootloader/limine-cpp-template](https://github.com/limine-bootloader/limine-cpp-template)
glued to the SQL-OS compiler (`../sql-os`): Limine boots a kernel whose entry
point runs an **SQL program compiled ahead of time to C++** — no interpreter
anywhere. The program's `io_8_write` rows execute real `out` instructions to
port `0xE9` (233), the QEMU/Bochs debug console.

## What runs

`kernel/src/sql/program.sql`:

1. stages a message byte-by-byte in RAM (`INSERT INTO memory`)
2. patches one byte in place (`UPDATE`: `'0'` → `'O'`)
3. streams every byte from RAM to the debug port
   (`INSERT INTO io_8_write (port, value) SELECT 233, value FROM memory ...`)
4. reports memory rows and a recursive CTE (`fib`) to the kernel's row sink
   — each result set's column names arrive first, then its rows
5. dumps the bootloader's data as tables: `SELECT * FROM boot_info` (HHDM
   offset, kernel bases, framebuffer, bootloader version, boot time) and
   the full Limine memory map (`SELECT base, length, type FROM memory_map`)
6. runs a page allocator written in SQL alone: claims frames 256/257 from
   the `volatile_memory` bitmap into the `pages` registry (scan cursor in
   the one-row `meta` table), prints registry + bitmap byte, frees
   `frame-a` and claims again — frame 256 comes back as `frame-c`
7. writes one more byte straight to the port (`VALUES (233, 69)` → `E`)

`kernel/src/main.cpp` — `kmain()`:

* enables SSE first (see below), runs global constructors
* fills the SQL boot tables from the Limine responses (`init_boot_tables`)
* calls `sqlos_program(print_row, print_header, nullptr)`
* `print_header` opens each result set (column names, then an underline of
  matching width) and closes it when the statement ends; rows are buffered
  so every column can be sized to its widest entry and centered — the `|`
  bars line up — before `print_row` prints the table to port 0xE9

## How the glue fits together

```text
kernel/src/sql/program.sql          -- the SQL program (source of truth)
        |  sqlos --emit             -- validate + compile to C++ (make rule)
kernel/src/sql/program.cpp          -- generated; includes only
        |                              sqlos/runtime/sqlos_runtime.hpp
        |  c++ (freestanding flags, minus -mno-sse/-mno-80387 for this file)
kernel/bin-x86_64/kernel            -- linked with kmain, memory.cpp, cc-runtime
        |  Limine (BIOS or UEFI ISO)
template-x86_64.iso                 -- boots; SQL writes port 0xE9
```

Notes:

* **SSE:** the SQL-OS value model has IEEE doubles, and the SysV x86-64 ABI
  returns doubles in `xmm` registers — which the template's
  `-mno-sse -mno-80387` flags forbid. The generated translation unit is
  therefore the *one* file compiled with SSE allowed (target-specific flag in
  `kernel/GNUmakefile`), and `kmain()` turns SSE on in hardware (CR0/CR4)
  before anything from that unit — or any constructor — executes. All other
  template files keep the stock flags.
* **Regeneration:** `kernel/GNUmakefile` rebuilds `src/sql/program.cpp`
  whenever `program.sql` or the SQL-OS compiler sources change, and builds
  `../sql-os` with CMake if the `sqlos` binary is missing.
* **Sink vs. port writes:** the message text on 0xE9 comes from SQL itself
  (the `io_8_write` inserts); the table afterwards comes from SELECTed
  rows handed to the kernel's row sink — each result set announces its
  column names through the `HeaderFn` callback before its rows arrive.

## Build

Requires GNU make, git, curl, a C/C++ toolchain, `nasm`, `xorriso` (ISO) and
`qemu-system-x86_64` (run). The first `make` fetches the kernel dependencies
(`kernel/get-deps`) and the Limine release tarball.

```sh
make -j            # → template-x86_64.iso
```

## Run

```sh
make run-bios      # interactive BIOS boot (no OVMF download)
```

For the port-0xE9 output, boot headless with QEMU's isa-debugcon device
(it defaults to iobase 233 = 0xE9):

```sh
timeout 45 qemu-system-x86_64 -M q35 -m 2G -cdrom template-x86_64.iso \
    -boot d -display none -no-reboot \
    -device isa-debugcon,chardev=e9 -chardev file,id=e9,path=e9.log
cat e9.log
```

Expected (verified on a real boot):

```text
WELCOME TO SQL-OS VIA PORT E9
 address  |  value 
-------------------
    4     |   79   
    5     |   77   
    6     |   69   
    7     |   32   
 a  
----
 0  
 1  
 1  
 2  
 3  
 5  
 8  
 13 
 21 
 34 
 55 
    hhdm_offset      |  kernel_phys_base  |   kernel_virt_base   |  bootloader  |  bootloader_version  |  cmdline  |  firmware  |   framebuffer_addr   |  framebuffer_width  |  framebuffer_height  |  framebuffer_bpp  |  module_count  |  boot_time  
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 0xffff800000000000  |     0x7feba000     |  0xffffffff80000000  |    Limine    |        12.9.1        |           |  x86bios   |  0xffff8000fd000000  |        1280         |         800          |        32         |       -        |  1790939189 
     base       |    length     |       type       
---------------------------------------------------
     4096       |    466944     |    bootloader    
    471040      |    180224     |      usable      
    654336      |     1024      |     reserved     
    983040      |     24576     |     reserved     
    1007616     |     4096      |  reserved_mapped 
    1011712     |     36864     |     reserved     
    1048576     |  2145071104   |      usable      
  2146119680    |     28672     |    bootloader    
  2146148352    |    385024     |      kernel      
  2146533376    |    196608     |    bootloader    
  2146729984    |     98304     |      usable      
  2146828288    |    520192     |    bootloader    
  2147348480    |     4096      |     reserved     
  2147352576    |     12288     |  reserved_mapped 
  2147364864    |    118784     |     reserved     
  2952790016    |   268435456   |     reserved     
  4244635648    |    4096000    |    framebuffer   
  4275159040    |     16384     |     reserved     
  4294705152    |    262144     |     reserved     
 1086626725888  |  12884901888  |     reserved     
 frame  |   owner  
-------------------
  256   |  frame-a 
  257   |  frame-b 
 value 
-------
   3   
 frame  |   owner  
-------------------
  257   |  frame-b 
  256   |  frame-c 
 value 
-------
   3   
E
```

The first line is the RAM message — `WELCOME` proves the `UPDATE` landed —
pushed out byte-by-byte by SQL. Every table is printed with centered,
space-padded cells so the `|` bars line up: the columns are sized to their
widest entry (header name or cell), and each result set prints when its
statement ends. `4 | 79` is the patched memory cell itself, `a` holds the
recursive `fib` CTE, then the two boot tables arrive — `boot_info` (filled
from the Limine responses: HHDM offset, kernel bases, bootloader
`Limine 12.9.1`, framebuffer, boot time) and the full 20-entry memory map.
Then the page-allocator result sets arrive: `pages` shows frames 256 and
257 handed out, the `value` row is bitmap byte 32 (`3` = both bits set),
and after `frame-a` is freed the next claim brings frame 256 back as
`frame-c` — allocation, free and reuse with no allocator C++, plain SQL
over `volatile_memory`. The trailing `E` is the standalone
`INSERT INTO io_8_write ... VALUES (233, 69)` at the end of
`program.sql` (no newline after it). `boot_time`, the kernel's placement
and the memory map's split points come from the boot itself, so they
differ between runs; with this exact QEMU command the allocator tables
are stable. Bochs writes port 0xE9 to its log natively.

## Layout

```text
limine-e9/
├── GNUmakefile           # ISO/run targets (from the template)
├── limine.conf           # Limine boot entry
└── kernel/
    ├── GNUmakefile       # + SQL-OS rules: sqlos regeneration, SSE TU flags
    ├── get-deps          # freestanding headers, cc-runtime, limine-protocol
    ├── linker-scripts/
    └── src/
        ├── main.cpp      # kmain: SSE enable, sqlos_program(), E9 row sink
        ├── memory.cpp    # memcpy/memset/memmove/memcmp (from the template)
        └── sql/
            ├── program.sql   # the SQL program
            └── program.cpp   # generated by `sqlos --emit` (do not edit)
```

Based on the Limine C++ template (see `LICENSE`); the framebuffer demo was
replaced with the SQL runner.
