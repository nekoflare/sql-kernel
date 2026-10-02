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
6. writes one more byte straight to the port (`VALUES (233, 69)` → `E`)

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
 0xffff800000000000  |     0x7feff000     |  0xffffffff80000000  |    Limine    |        12.9.1        |           |  x86bios   |  0xffff8000fd000000  |        1280         |         800          |        32         |       -        |  1790930647 
     base       |    length     |       type       
---------------------------------------------------
     4096       |    466944     |    bootloader    
    471040      |    180224     |      usable      
    654336      |     1024      |     reserved     
    983040      |     24576     |     reserved     
    1007616     |     4096      |  reserved_mapped 
    1011712     |     36864     |     reserved     
    1048576     |  2145353728   |      usable      
  2146402304    |     28672     |    bootloader    
  2146430976    |    212992     |      kernel      
  2146643968    |     86016     |    bootloader    
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
The trailing `E` is the standalone `INSERT INTO io_8_write ... VALUES
(233, 69)` at the end of `program.sql` (no newline after it). `boot_time`
is the machine's clock, so it differs between boots; with this exact QEMU
command the rest of the capture is stable. Bochs writes port 0xE9 to its
log natively.

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
