# HashSource X19 Mining Software

Custom mining software and hardware test utilities for Bitmain Antminer X19/S19 family miners.

**Supported ASIC Chips**:

- BM1360 (S19i, S19jpro)
- BM1362 (S19j Pro+)
- BM1398 (S19, S19 Pro, T19, S19a, S19a Pro, S19+)
- BM1366 (S19K Pro, S19XP, S19XP Hydro)

## Directory Structure

```
mining_software/
├── src/           # Source files
│   ├── main.c           # Main miner application
│   ├── fan_test.c       # Fan PWM control test with full FPGA init
│   ├── fpga_logger.c    # FPGA register change logger
│   └── utils.c          # Utility functions
├── include/       # Header files
├── drivers/       # Hardware driver implementations
├── config/        # Configuration files
│   ├── miner.conf       # Mining pool configuration
│   └── S90hashsource    # Init script for mining service
├── bin/           # Compiled binaries (generated)
└── obj/           # Object files (generated)
```

## Building

The Makefile automatically detects and uses the buildroot cross-compilation toolchain if available. Simply run:

```bash
make
```

If buildroot is not found, it will fallback to the system `arm-linux-gnueabihf-` toolchain.

You can also manually specify the toolchain:

```bash
CROSS_COMPILE=path/to/toolchain- make
```

### Build Targets

- `make` or `make all` - Build all binaries (miner, fan_test, fpga_logger)
- `make clean` - Remove build artifacts
- `make install` - Install to target filesystem

## Binaries

### fan_test

Hardware test utility that controls fan PWM with proper FPGA initialization.

**Usage:**

```bash
./bin/fan_test
```

**Requirements:**

- Must run as root
- Requires kernel modules loaded:
  - `bitmain_axi.ko` - FPGA AXI interface driver (with VM_SHARED fix)
  - `fpga_mem_driver.ko` - FPGA memory driver

**Test behavior:**

- Performs complete 2-stage FPGA initialization (boot-time + bmminer sequence)
- Ramps from 10% to 100% in 5% increments
- 10 second hold at each speed level for clear audible feedback
- Uses stock firmware PWM format: `(percent << 16) | (100 - percent)`
- Sets fans to 50% default on exit
- Supports Ctrl+C for safe shutdown

**Example output:**

```
=== S19 Pro Fan Speed Ramp Test ===

Opening /dev/axi_fpga_dev...
FPGA registers mapped at 0xb6f4f000

========================================
FPGA Initialization Sequence
========================================
...
Setting fan speed to  10%... (PWM: 0x000A005A)
Setting fan speed to  15%... (PWM: 0x000F0055)
...
```

### fpga_logger

FPGA register change logger for debugging and analysis.

**Usage:**

```bash
./bin/fpga_logger [output_file]
```

Monitors FPGA registers in real-time and logs all changes. Useful for:

- Reverse engineering stock firmware behavior
- Debugging initialization sequences
- Analyzing bmminer startup

### hashsource_miner

Main mining application (work in progress).

## Technical Details

### FPGA Initialization Sequence

The S19 Pro FPGA requires a specific 2-stage initialization before PWM control works:

**Stage 1: Boot-time initialization**

```c
regs[0x000/4] |= 0x40000000;  // Set bit 30 (BM1391 init)
regs[0x080/4] = 0x0080800F;   // Key control register
regs[0x088/4] = 0x800001C1;   // Initial config
```

**Stage 2: Bmminer startup sequence**

```c
regs[0x080/4] = 0x8080800F;   // Set bit 31
regs[0x088/4] = 0x00009C40;   // Config change
regs[0x080/4] = 0x0080800F;   // Clear bit 31
regs[0x088/4] = 0x8001FFFF;   // Final config
```

### PWM Register Format

Fan speed is controlled via registers 0x084 and 0x0A0 using the format:

```c
pwm_value = (percent << 16) | (100 - percent)
```

**Examples:**

- 100%: `0x00640000` (100 << 16 | 0)
- 50%: `0x00320032` (50 << 16 | 50)
- 0%: `0x00000064` (0 << 16 | 100)

This format was discovered by comparing stock firmware behavior with custom implementations.

### Kernel Module Requirements

The custom kernel modules require specific VM flags for memory-mapped I/O:

```c
vm_flags_set(vma, VM_SHARED | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
```

The `VM_SHARED` flag (0x4000000) is critical - without it, writes to FPGA registers do not propagate correctly. This was discovered by decompiling stock firmware kernel modules.

## Hardware Requirements

- Bitmain Antminer S19 Pro (Zynq-7007S SoC)
- Custom kernel modules for FPGA access
- Root access for hardware control

## Development Notes

### Debugging FPGA Issues

1. Use `fpga_logger` to capture register changes during stock firmware operation
2. Compare against custom implementation behavior
3. Check kernel module VM flags match stock firmware exactly
4. Verify initialization sequence is performed in correct order

### Memory-Mapped I/O

All FPGA access uses `/dev/axi_fpga_dev` character device:

- Base address: 0x40000000 (physical)
- Size: 0x1200 (4608 bytes)
- Memory barrier required: `__sync_synchronize()` after writes

## References

- Stock firmware analysis in `/home/danielsokil/Downloads/Bitmain_Peek/S19_Pro/`
