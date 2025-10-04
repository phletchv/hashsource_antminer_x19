# HashSource Antminer X19 Firmware

Firmware for Bitmain Antminer X19 Bitcoin ASIC miners using **stock kernel + custom ramdisk** architecture to bypass RSA signature verification.

**Status**: Functional system controller (70% complete) - hardware control working, ASIC mining protocol not yet implemented

---

## Quick Facts

| Component          | Status                                                |
| ------------------ | ----------------------------------------------------- |
| **Platform**       | Xilinx Zynq-7007S (ARM Cortex-A9 + FPGA)              |
| **Approach**       | Stock kernel 4.6.0-xilinx + Custom ramdisk            |
| **Build System**   | Buildroot BR2_EXTERNAL                                |
| **Ramdisk Size**   | ~10MB (FIT image format)                              |
| **Boot Method**    | NAND flash (mtd1 + mtd3 signature update)             |
| **Key Constraint** | RSA eFuse lock prevents bootloader/kernel replacement |

---

## Architecture

### Critical Hardware Constraint

This XILINX control boards have RSA authentication **permanently enabled** in eFuses:

```
EfusePS status bits: 0xC0013C8D
Expected RSA Key Hash: 3545B6DE1FF44EE4295270CC6D0FF730F861DB9CE32F70F2980619FAF0F34DC1
```

**Consequence**: Cannot replace BOOT.bin (bootloader) or kernel - only ramdisk can be customized.

### Solution: Stock Kernel + Custom Ramdisk

```
Boot Chain (RSA-locked components preserved):
┌─────────────────────────────────────┐
│ FSBL (ROM)           ✓ RSA Verified │
│ ├─> FPGA Bitstream   ✓ RSA Verified │
│ ├─> U-Boot           ✓ RSA Verified │
│ └─> Kernel 4.6.0     ✓ RSA Verified │
│      └─> Ramdisk     ✓ SHA256 Only  │ ← Custom firmware
└─────────────────────────────────────┘
```

**Key Insight**: U-Boot verifies ramdisk via SHA256 (not RSA) against mtd3 signature partition. By updating both ramdisk and signature, custom firmware boots successfully.

### NAND Flash Layout

```
Offset         Size    Partition  Content                     Status
──────────────────────────────────────────────────────────────────────
0x00000000     40MB    mtd0       BOOT.bin + kernel           PRESERVED
0x02800000     32MB    mtd1       ramdisk.itb                 REPLACED
0x04800000     8MB     mtd2       configs                     -
0x05000000     2MB     mtd3       signatures (SHA256 @ 1024)  PATCHED
0x05200000     171MB   mtd4       reserve                     -
```

**Installation Method**:

1. Write custom ramdisk to mtd1 (0x2800000)
2. Compute SHA256 hash of ramdisk
3. Update mtd3 bytes 1024-1279 with new SHA256 + zero padding
4. Reboot → U-Boot verifies SHA256 → boot succeeds

---

## What Works (Current Status)

### Completed Features

**Hardware Control**:

- Fan PWM control (10%-100%, stock firmware format)
- PSU voltage control (12-15V via FPGA I2C)
- FPGA initialization (2-stage sequence reverse-engineered)
- GPIO control (PSU enable, LED control)

**System Components**:

- Ethernet (DHCP, SSH access)
- Serial console (115200 baud)
- NAND flash access (mtd-utils)
- Stock kernel module loading (bitmain_axi.ko, fpga_mem_driver.ko)

**Build System**:

- Buildroot BR2_EXTERNAL configuration
- Cross-compilation toolchain
- FIT ramdisk generation
- NAND installation automation

## Quick Start

### Build Firmware

```bash
# Clone buildroot submodule
git submodule update --init --recursive

# Configure and build ramdisk
make x19_xilinx_ramdisk_defconfig
make  # Uses all CPU cores by default

# Output: buildroot/output/images/ramdisk.itb
```

**Build Time**: 15-30 minutes (16-core desktop), first build

### Install to Miner

**First-time install** (stock Bitmain firmware → HashSource):

```bash
./scripts/bitmain_ramdisk_install.sh <miner_ip>
# Default IP: 192.168.0.192
# Unlocks sudo via daemonc, installs ramdisk, updates signature, reboots
```

**Update existing** (HashSource → HashSource):

```bash
./scripts/hashsource_ramdisk_update.sh <miner_ip>
# Faster update path, assumes root access already configured
```

**After reboot**:

```bash
ssh root@<miner_ip>
# Password: root
```

---

## Development Architecture

### Directory Structure

```
hashsource_antminer_x19/
├── buildroot/                  # Mainline Buildroot (submodule)
├── br2_external_bitmain/       # BR2_EXTERNAL tree
│   ├── configs/
│   │   └── x19_xilinx_ramdisk_defconfig
│   ├── packages/
│   │   └── hashsource_x19/     # Mining software package
│   └── board/x19_xilinx/
│       ├── rootfs-overlay/     # Files copied to ramdisk
│       │   ├── lib/modules/    # Stock kernel modules (binaries)
│       │   └── etc/init.d/S10modules
│       ├── ramdisk-fit.its     # FIT image template
│       ├── post-build.sh
│       └── post-image-ramdisk.sh
├── hashsource_x19/             # Standalone mining software
│   ├── src/                    # fan_test, psu_test, fpga_logger, main
│   ├── drivers/                # s19_driver.c, gpio_pwm.c
│   ├── include/miner.h
│   └── bin/                    # Compiled binaries
├── scripts/
│   ├── bitmain_ramdisk_install.sh
│   └── hashsource_ramdisk_update.sh
└── docs/
    └── PSU_PROTOCOL.md         # Complete APW12 reverse engineering
```

### Stock Kernel Modules

**Critical**: Using stock kernel 4.6.0-xilinx-g03c746f7 (RSA-signed). Custom modules cannot be compiled due to binary format requirements.

**Modules** (extracted from stock firmware, included as binaries):

- `bitmain_axi.ko` (7.3KB) - FPGA register access via `/dev/axi_fpga_dev`
- `fpga_mem_driver.ko` (7.8KB) - FPGA memory mapping

**Init Script** (`S10modules`):

- Auto-detects RAM size
- Loads modules with correct memory offset (0x0F000000 for 256MB RAM)
- Runs early in boot sequence

### Buildroot Configuration

**Key Settings** (x19_xilinx_ramdisk_defconfig):

- **Architecture**: ARM Cortex-A9, NEON, VFPv3D16
- **Toolchain**: glibc, kernel headers 4.6.x (ABI compatibility)
- **Optimization**: `-Os`, LTO enabled (size-critical)
- **Kernel**: NOT built (using stock from NAND)
- **Bootloader**: NOT built (using stock from NAND)
- **Rootfs**: ext2, 32MB max, gzip compressed → FIT image
- **Packages**: Minimal (busybox, dropbear, i2c-tools, mtd-utils)

**Patches Applied**:

- Dropbear: Disable `getrandom()` to avoid boot blocking (kernel 4.6 has incomplete entropy)

---

## Installation Method (Signature Update)

### How It Works

U-Boot verifies ramdisk SHA256 against mtd3 signature partition:

- **Offset 0-1023**: Kernel signature (RSA, not modified)
- **Offset 1024-1279**: Ramdisk signature (SHA256, UPDATED)
- **Offset 1280+**: Additional signatures

**Installation Process**:

1. Compute SHA256 hash of custom ramdisk
2. Create 256-byte signature: SHA256 (32 bytes) + zero padding (224 bytes)
3. Upload ramdisk, signature, NAND tools to miner
4. Erase mtd1, write custom ramdisk
5. Dump mtd3, patch bytes 1024-1279, write back
6. Verify with MD5 checksums
7. Reboot

**Why This Works**:

- Stock bootloader/kernel remain intact (RSA-signed)
- U-Boot verifies ramdisk via SHA256 (not RSA)
- No private RSA key needed - only SHA256 hash update
- Other vendors use this exact method

**Script**: `scripts/bitmain_ramdisk_install.sh` (automated, includes verification)

---

## Build Commands

```bash
# Initial setup
make x19_xilinx_ramdisk_defconfig
make                              # Full build

# Incremental builds
make hashsource_x19-rebuild       # Rebuild mining software only (~1 min)
make busybox-menuconfig           # Configure Busybox
make savedefconfig                # Save config changes

# Standalone mining software (without Buildroot)
cd hashsource_x19
make                              # Build all utilities
make clean                        # Clean build artifacts
```

**Output**: `buildroot/output/images/ramdisk.itb` (install this to NAND)

---

## Serial Console Access

**Hardware**: USB-to-TTL adapter (3.3V logic, **NOT 5V**)

**Connection**:

```
Adapter  →  XILINX UART Header
GND      →  GND
RX       →  TX
TX       →  RX
```

**WARNING**: Do NOT connect VCC/5V pin

**Terminal**:

```bash
minicom -D /dev/ttyUSB0 -b 115200
# or
screen /dev/ttyUSB0 115200
```

**Settings**: 115200 baud, 8N1, no flow control

---
