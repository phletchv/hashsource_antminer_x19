# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Custom firmware for Bitmain Antminer X19 (S19 Pro) Bitcoin ASIC miners using **stock kernel + custom ramdisk** architecture to bypass RSA signature verification.

Built using Buildroot with BR2_EXTERNAL architecture targeting Xilinx Zynq-7007S SoC (ARM Cortex-A9 dual-core + FPGA).

**Critical Hardware Constraint**: Xilinx control boards have RSA eFuse authentication permanently enabled (0xC0013C8D). Stock signed bootloader and kernel are REQUIRED and preserved. Only the ramdisk is customized.

**Current Status**: 70% complete - hardware control working (fans, PSU, GPIO), ASIC mining protocol not yet implemented (BM1360/BM1362/BM1398/BM1366).

## Build Commands

### Initial Setup

```bash
# Clone buildroot submodule (if not already done)
git submodule update --init --recursive

# Configure and build ramdisk
make x19_xilinx_ramdisk_defconfig
make  # Builds custom ramdisk only (no kernel/uboot)
```

### Incremental Builds

```bash
make hashsource_x19-rebuild           # Rebuild mining software only
make busybox-menuconfig               # Configure Busybox
make savedefconfig                    # Save current config to defconfig
```

### Mining Software (Standalone)

```bash
cd hashsource_x19
make                                  # Build fan_test, psu_test, fpga_logger, main
make clean                            # Clean build artifacts
```

### NAND Installation

```bash
# First-time install (stock Bitmain firmware → HashSource):
./scripts/bitmain_ramdisk_install.sh <miner_ip>

# Update existing HashSource installation:
./scripts/hashsource_ramdisk_update.sh <miner_ip>
```

**Signature Update Method:**

U-Boot performs SHA256 signature verification during boot. The signature partition (mtd3) contains:

- Offset 0-1023: Kernel signature (1024 bytes, verified at boot)
- Offset 1024-1279: Ramdisk signature (256 bytes, verified when loading ramdisk)
- Offset 1280+: Additional signatures

**Installation Process (automated in script):**

1. Compute SHA256 hash of custom ramdisk
2. Create 256-byte signature (SHA256 hash + zero padding)
3. Unlock sudo access via daemonc exploit
4. Upload ramdisk, signature, and NAND tools (nandwrite, nanddump, flash_erase)
5. Erase and write custom ramdisk to mtd1
6. Dump mtd3, patch bytes 1024-1279 with new signature, write back
7. Verify both ramdisk and signature with MD5 checksums

**Critical Implementation Details:**

- Uses `head -c` and `tail -c` instead of `dd` (not available in stock firmware)
- NAND tools required (stock tools have wrong parameters)
- Commands run one at a time for better error visibility
- NOPASSWD sudo configured to avoid password prompts
- Verification uses nanddump with manual signature extraction

This approach works because:

- Stock bootloader/kernel remain intact and RSA-signed
- U-Boot verifies ramdisk SHA256 against mtd3 bytes 1024-1279 → updated to match
- No RSA private key needed - only SHA256 hash update
- BitFuFu, Vnish, and other vendors use this exact method

### Clean Builds

```bash
make clean                            # Clean build artifacts (keeps downloads)
make distclean                        # Full clean (removes downloads)
```

## Architecture Overview

### Boot Sequence (Stock Kernel + Custom Ramdisk)

1. **FSBL** (ROM → BOOT.bin @ 0x0) - Xilinx bootloader with RSA verification
2. **U-Boot** (NAND @ varies) - Stock Bitmain U-Boot (RSA signed)
3. **Kernel** (NAND @ 0x2000000) - **Stock kernel 4.6.0-xilinx** (RSA signed)
4. **Ramdisk** (NAND @ 0x2800000) - **Custom ramdisk** (NOT verified!)
5. **Init** (Busybox) - Loads stock modules, runs custom mining software

**Key Insight**: U-Boot only verifies kernel signature, NOT ramdisk. By preserving the stock kernel and only replacing the ramdisk, we bypass RSA authentication.

**Critical Files**:

- `BOOT.bin` @ 0x0 - FSBL + FPGA bitstream + U-Boot (stock, signed)
- `devicetree.dtb` @ 0x1A0000 - Device tree (stock, signed)
- `uImage` @ 0x2000000 - Linux 4.6.0 kernel (stock, signed)
- `ramdisk.itb` @ 0x2800000 - Custom ramdisk FIT image (REPLACED)

### Directory Structure

```
.
├── buildroot/                       # Git submodule (mainline Buildroot)
├── br2_external_bitmain/            # BR2_EXTERNAL tree
│   ├── board/x19_xilinx/            # Board-specific files
│   │   ├── stock-modules/           # Extracted stock kernel modules
│   │   │   ├── bitmain_axi.ko       # FPGA register access (7.3KB)
│   │   │   └── fpga_mem_driver.ko   # FPGA memory mapping (7.8KB)
│   │   ├── ramdisk-fit.its          # FIT image template for ramdisk
│   │   ├── post-build.sh            # Creates init scripts, motd
│   │   ├── post-image-ramdisk.sh    # Generates ramdisk.itb
│   │   └── rootfs-overlay/          # Files copied to ramdisk
│   │       ├── etc/init.d/S10modules # Loads stock kernel modules
│   │       └── lib/modules/          # Stock .ko files
│   ├── configs/
│   │   └── x19_xilinx_ramdisk_defconfig # Main config (ramdisk-only build)
│   └── packages/
│       └── hashsource_x19/          # Mining software package
├── hashsource_x19/                  # Standalone test utilities
│   ├── src/                         # fan_test, psu_test, fpga_logger, main
│   ├── drivers/                     # s19_driver.c, gpio_pwm.c
│   └── include/miner.h              # Mining daemon header
├── docs/
│   └── PSU_PROTOCOL.md              # Complete APW12 PSU reverse engineering
├── scripts/                         # Build/flash automation
│   ├── bitmain_ramdisk_install.sh   # First-time NAND installer
│   └── hashsource_ramdisk_update.sh # Update existing installation
└── uart_dumps/                      # Serial console captures
```

### Stock Kernel Modules

**Critical**: Custom kernel modules are NOT used. Stock modules from kernel 4.6.0-xilinx-g03c746f7 are included in the ramdisk.

#### bitmain_axi.ko (7.3KB)

- Character device: `/dev/axi_fpga_dev`
- Maps FPGA registers: 0x40000000 (5120 bytes)
- **VM flags**: Must set `VM_SHARED | VM_IO | VM_DONTEXPAND`

#### fpga_mem_driver.ko (7.8KB)

- FPGA memory mapping driver
- Parameter: `fpga_mem_offset_addr` (0x0F000000 for 240MB RAM)
- Memory offset varies by system RAM:
  - > 1GB RAM: 0x3F000000
  - 400MB-1GB: 0x1F000000
  - <400MB (S19 Pro): 0x0F000000

### FPGA Initialization Sequence

**REQUIRED** before fan/PSU control works (from fan_test.c):

```c
// Stage 1: Boot-time initialization
regs[0x000/4] |= 0x40000000;  // Set bit 30 (BM1391 init)
regs[0x080/4] = 0x0080800F;   // Key control register
regs[0x088/4] = 0x800001C1;   // Initial config

// Stage 2: Bmminer startup sequence
regs[0x080/4] = 0x8080800F;   // Set bit 31
regs[0x088/4] = 0x00009C40;   // Config change
regs[0x080/4] = 0x0080800F;   // Clear bit 31
regs[0x088/4] = 0x8001FFFF;   // Final config
```

### PSU Control Architecture

GPIO 1 (MIO_1) controls PSU enable (active-low: 0=ON, 1=OFF).

**Power-On Sequence** (from psu_test.c):

1. Set GPIO 1 = HIGH (disable PSU output)
2. Wait 30 seconds (capacitor discharge) - **CRITICAL on first power-on**
3. Detect protocol (write 0xF5 to register, read back)
4. Get PSU version (command 0x02)
5. Set voltage via I2C (command 0x83) while PSU disabled
6. Set GPIO 1 = LOW (enable PSU output)
7. Wait 500ms for voltage stabilization

**Uses FPGA I2C controller**, NOT PS I2C (/dev/i2c-0):

- FPGA register 0x0C (word offset, byte offset 0x30)
- I2C address 0x10 encoded as: `(master=1 << 26) | (slave_high=2 << 20) | (slave_low=0 << 16)`

See `docs/PSU_PROTOCOL.md` for complete protocol documentation.

### Fan Control

PWM registers: 0x084 and 0x0A0
Format: `(percent << 16) | (100 - percent)`

Examples:

- 100%: 0x00640000
- 50%: 0x00320032
- 0%: 0x00000064

Must perform FPGA initialization sequence first.

## Key Implementation Details

### Device Tree (Stock, Preserved)

- 256MB RAM @ 0x00000000
- Ethernet: Cadence GEM @ 0xe000b000 (GMII mode)
- UART: ttyPS0 @ 0xe0000000 (115200 baud, console)
- NAND: PL353 SMC @ 0xe1000000 (256MB)
- GPIO: Xilinx Zynq GPIO @ 0xe000a000

### NAND Flash Layout

```
0x00000000-0x001A0000:  Stock BOOT.bin (FSBL + bitstream + U-Boot) - PRESERVED
0x001A0000-0x02000000:  Stock devicetree.dtb - PRESERVED
0x02000000-0x02800000:  Stock uImage (kernel 4.6.0) - PRESERVED
0x02800000-0x04800000:  Custom ramdisk.itb - REPLACED
0x04800000-0x05000000:  configs
0x05000000-0x05200000:  sig
0x05200000-0x10000000:  reserve
```

**Installation Method**: Write `ramdisk.itb` directly to `/dev/mtdblock1` (offset 0x2800000)

### Binary Components

**Stock Components (Preserved)**:

- BOOT.bin - From Chipless/Bitmain firmware (RSA signed)
- uImage - Linux 4.6.0-xilinx-g03c746f7 (RSA signed)
- devicetree.dtb - From stock firmware (RSA signed)
- bitmain_axi.ko - Stock kernel module
- fpga_mem_driver.ko - Stock kernel module

**Custom Components (Built by Buildroot)**:

- ramdisk.itb - FIT image containing ext2 filesystem with custom userspace
- Mining software (hashsource-miner package)

## Development Constraints

### Hardware-Specific Limitations

1. **RSA eFuse Lock**: Cannot replace FSBL, U-Boot, or kernel on this unit

   - Expected RSA Key Hash: 3545B6DE1FF44EE4295270CC6D0FF730F861DB9CE32F70F2980619FAF0F34DC1
   - Other S19 Pro units may differ (batch-dependent)

2. **Kernel Version Lock**: Must use kernel 4.6.0-xilinx-g03c746f7

   - Stock modules compiled for this specific kernel
   - Cannot upgrade to newer kernel due to RSA verification

3. **FPGA Bitstream**: Proprietary binary blob required for ASIC access
   - No source code available
   - Included in BOOT.bin

### Working Features

- ✅ NAND boot with custom ramdisk
- ✅ Serial console (115200 baud)
- ✅ Ethernet (DHCP)
- ✅ SSH access
- ✅ Fan control (PWM)
- ✅ PSU control (voltage setting)
- ✅ FPGA initialization
- ✅ GPIO control

### Not Yet Implemented

- ❌ ASIC chip communication (BM1360/BM1362/BM1398/BM1366 protocols unknown)
- ❌ Mining work distribution
- ❌ Nonce collection
- ❌ Pool integration (Stratum)
- ❌ Auto-tuning (frequency/voltage optimization)
- ❌ Temperature sensors
- ❌ Web UI

## Testing

### Hardware Test Utilities (hashsource_x19/bin/)

```bash
# Fan PWM ramp test (10%-100%, 5% increments, 10 sec each)
./bin/fan_test

# PSU voltage control test
./bin/psu_test 15000  # Set 15.0V (range: 12000-15000 mV)

# FPGA register monitoring (for reverse engineering)
./bin/fpga_logger output.log
```

All test utilities require:

- Root access (`sudo`)
- Stock kernel modules loaded (automatic via S10modules init script)

### Serial Console Access

```bash
# Hardware: USB-to-TTL adapter (3.3V logic, NOT 5V)
# Connection: GND→GND, RX→TX, TX→RX (DO NOT connect VCC)
minicom -D /dev/ttyUSB0 -b 115200
# or
screen /dev/ttyUSB0 115200
```

## Configuration Files

### Buildroot Config (br2_external_bitmain/configs/x19_xilinx_ramdisk_defconfig)

- ARM Cortex-A9, NEON, VFPv3D16
- Size optimization (`BR2_OPTIMIZE_S=y`, `BR2_ENABLE_LTO=y`)
- glibc toolchain with C++ support
- **Kernel headers 4.6** (for userspace compatibility)
- **NO kernel build** (using stock kernel)
- **NO U-Boot build** (using stock bootloader)
- ext2 ramdisk (32MB max size)
- Root password: "root" (development/testing)
- DHCP on eth0

### Ramdisk Init System

**S10modules** (loads stock kernel modules):

- Runs early in boot sequence
- Automatically determines memory offset
- Loads bitmain_axi.ko and fpga_mem_driver.ko

## Common Issues

### NAND Boot Failure

If the system fails to boot after NAND installation:

1. **Verify ramdisk was written correctly**:

   ```bash
   dd if=/dev/mtdblock1 of=/tmp/verify.itb bs=1M count=10
   md5sum /tmp/verify.itb ramdisk.itb
   ```

2. **Check UART console output** for error messages

3. **Emergency recovery**: Boot from stock SD card, re-run installer

### Kernel Module Load Failures

- Check kernel version: `uname -r` should show `4.6.0-xilinx-g03c746f7`
- Verify modules exist: `ls -la /lib/modules/`
- Check init script: `cat /etc/init.d/S10modules`
- Manual load: `insmod /lib/modules/bitmain_axi.ko`

### Fan/PSU Control Not Working

- Ensure kernel modules loaded: `lsmod | grep bitmain_axi`
- Verify `/dev/axi_fpga_dev` exists
- Check FPGA initialization in mining software

### Network Not Working

- Wait 30s for DHCP
- Check cable connection
- Serial console shows IP: `ifconfig eth0`
- Manual config: `ifconfig eth0 192.168.1.100 up && route add default gw 192.168.1.1`

## External Documentation

- **README.md**: Concise project overview and getting started guide (280 lines)

  - Quick facts and current status
  - Architecture overview (RSA constraint, NAND layout)
  - Build and installation instructions
  - Hardware specifications

- **docs/PSU_PROTOCOL.md**: Complete APW12 PSU protocol reverse engineering (500+ lines)

  - Two protocol variants (Legacy vs V2)
  - FPGA I2C controller architecture
  - Version-specific voltage formulas (11 PSU versions)
  - Decompiled source references

- **hashsource_x19/README.md**: Mining software technical documentation

  - FPGA initialization sequence details
  - PWM register format
  - Kernel module requirements
  - Hardware test utility usage

## Output Locations

After successful build:

- **Ramdisk FIT image**: `buildroot/output/images/ramdisk.itb` ← Install this to NAND
- **Root filesystem**: `buildroot/output/images/rootfs.ext2`
- **Compressed ramdisk**: `buildroot/output/images/rootfs.ext2.gz`
- **NAND installer**: `buildroot/output/images/install-ramdisk-to-nand.sh`
- **Build info**: `buildroot/output/images/BUILD_INFO.txt`

## Build Times (Reference)

- First build: 15-30 minutes (16-core desktop)
- Incremental (miner): <1 minute
- Ramdisk rebuild: 2-5 minutes

## Key Differences from Previous Architecture

**OLD (Deprecated)**:

- Custom kernel 6.16.x
- Custom U-Boot chainloading
- Custom kernel modules built from source
- SD card boot only (NAND boot failed due to RSA verification)
- 128MB SD card image

**NEW (Current)**:

- Stock kernel 4.6.0 (RSA signed, preserved)
- Stock U-Boot (RSA signed, preserved)
- Stock kernel modules (binary, extracted from Chipless firmware)
- NAND boot working (ramdisk not verified by U-Boot)
- 10MB ramdisk FIT image

## Important Reminders

1. **Never modify mtd0** - Contains RSA-signed bootloader and kernel
2. **Only write to mtd1** - Custom ramdisk partition (via installation scripts)
3. **mtd3 signature update** - Required when ramdisk changes (automated in scripts)
4. **Keep stock firmware backup** for emergency recovery
5. **Stock modules MUST match kernel 4.6.0** - No mixing kernel versions

## Critical Architecture Notes

### Why Stock Kernel + Custom Ramdisk Works

U-Boot boot verification chain:

1. ROM bootloader verifies BOOT.bin via **RSA signature** (eFuse key hash)
2. U-Boot verifies kernel via **RSA signature** (public key in BOOT.bin)
3. U-Boot verifies ramdisk via **SHA256 hash** (stored in mtd3 @ offset 1024-1279)

**Key insight**: Only ramdisk uses SHA256 (not RSA), so we can:

- Compute SHA256 of custom ramdisk
- Update mtd3 signature partition with new hash
- Reboot → U-Boot verifies SHA256 matches → boot succeeds

### ASIC Protocol (Not Yet Implemented)

**Current blocker**: Bitmain ASIC chip communication protocols are proprietary and undocumented.

**X19/S19 Family ASIC Chips** (by generation):

| Chip Model | Variants                                         | Compatible Miners                       | Notes                  |
| ---------- | ------------------------------------------------ | --------------------------------------- | ---------------------- |
| **BM1360** | BM1360BB                                         | S19i, S19jpro                           | SHA-256 ASIC           |
| **BM1362** | BM1362BD                                         | S19j Pro+                               | SHA-256 ASIC           |
| **BM1398** | BM1398BB, BM1398AC, BM1398AD                     | S19, S19 Pro, T19, S19a, S19a Pro, S19+ | SHA-256 ASIC           |
| **BM1366** | BM1366BS, BM1366BP, BM1366AH, BM1366AL, BM1366AG | S19K Pro, S19XP, S19XP Hydro            | Newer gen, lower power |

**Typical Configuration**:

- 3 hash chains per miner
- Up to 114 chips per chain
- Total: ~342 chips per miner

**Required for mining**:

- SPI command format for work distribution
- Nonce response parsing
- Frequency/voltage control registers
- Chip initialization sequence (varies by generation)
- Temperature sensor protocol
- Chain addressing and broadcast commands

**Reverse engineering approaches**:

1. SPI bus sniffing during stock firmware operation (logic analyzer)
2. Binary analysis of stock `bmminer` (decompilation with Ghidra)
3. Leverage open-source projects (cgminer, Braiins OS support)
4. Compare protocols across BM1360/BM1362/BM1398/BM1366 generations

**References**:

- Zeus BTC ASIC chip documentation (repair parts catalog)
- Braiins OS source code (if available for newer chips)
- Community reverse engineering efforts

This is the **primary development blocker** preventing actual Bitcoin mining.
