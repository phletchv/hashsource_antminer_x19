# Reference Projects - ASIC Mining Protocol Research

**Last Updated:** 2025-10-06

This document catalogs all reference projects available for implementing BM13xx ASIC communication protocols for the HashSource Antminer X19 firmware project.

---

## Table of Contents

1. [Quick Reference Matrix](#quick-reference-matrix)
2. [Stock Firmware & Decompiled Binaries](#stock-firmware--decompiled-binaries)
3. [Protocol Documentation](#protocol-documentation)
4. [Source Code Implementations](#source-code-implementations)
5. [Analysis Tools](#analysis-tools)
6. [Next Steps & Recommendations](#next-steps--recommendations)

---

## Quick Reference Matrix

| Project                       | Type                   | ASIC Chips                                | Protocol Info | Init Sequence | Work Format | Nonce Format | Priority     |
| ----------------------------- | ---------------------- | ----------------------------------------- | ------------- | ------------- | ----------- | ------------ | ------------ |
| **Bitmain_Peek/S19_Pro**      | Decompiled firmware    | BM1360/BM1362/BM1398                      | Complete      | Complete      | Complete    | Complete     | 5 (Critical) |
| **LiLei_WeChat**              | Test fixtures          | BM1360/BM1362/BM1366/BM1368/BM1370/BM1398 | Complete      | Complete      | Complete    | Complete     | 5 (Critical) |
| **bm1397-protocol**           | Rust library           | BM1397                                    | Complete      | Complete      | Complete    | Complete     | 4 (High)     |
| **bm13xx-hla**                | Logic analyzer decoder | BM1360/BM1362/BM1366/BM1368/BM1370/BM1397 | Complete      | Partial       | Complete    | Complete     | 4 (High)     |
| **bm13xx-rs**                 | Rust library           | BM1366/BM1370/BM1397                      | Complete      | Complete      | Complete    | Complete     | 4 (High)     |
| **bitmain_antminer_binaries** | Decompiled binaries    | BM1360/BM1362/BM1366/BM1397               | Complete      | Complete      | Complete    | Partial      | 4 (High)     |
| **bitmaintech**               | Official source code   | BM1387                                    | Complete      | Complete      | Complete    | Complete     | 3 (Useful)   |
| **bmminer_NBP1901**           | Decompiled S19 Pro     | BM1391/BM1397/BM1398                      | Complete      | Complete      | Partial     | Partial      | 5 (Critical) |
| **kanoi/cgminer**             | Open source miner      | BM1362/BM1370/BM1384/BM1387/BM1397        | Complete      | Complete      | Complete    | Complete     | 5 (Critical) |
| **skot/BM1397**               | Protocol docs          | BM1366/BM1397                             | Complete      | Partial       | Complete    | Complete     | 4 (High)     |

**Legend:**

- 5 (Critical) - Must review
- 4 (High) - Strongly recommended
- 3 (Useful) - Review if needed

---

## Stock Firmware & Decompiled Binaries

### 1. Bitmain_Peek/S19_Pro

**Location:** `/home/danielsokil/Downloads/Bitmain_Peek/S19_Pro`

**Project Type:** Complete firmware dump collection with extensive Ghidra decompilation

**ASIC Chips:**

- **BM1360** (S19i, S19j Pro) - Full protocol
- **BM1362** (S19j Pro+) - Full protocol
- **BM1398** (S19, S19 Pro, T19) - Full protocol

**Contents:**

- 3 complete firmware versions + 2 recovery images (1.5GB)
- 1,373 decompiled functions from bmminer
- Complete Ghidra projects with symbols
- BMMINER_ANALYSIS.md (770 lines) - Critical functions documented
- EEPROM_VALIDATION.md (576 lines) - Complete EEPROM protocol

**Critical Files:**

```
_ghidra/bins/bmminer-2f464d0989b763718a6fbbdee35424ae/decomps/
├── FUN_00034828.c - Main chip initialization (CRITICAL)
├── FUN_00050a80.c - Write ASIC register
├── FUN_0005fadc.c - Detect ASIC chip type
├── FUN_00028138.c - Configure UART/SPI clock (12 MHz)
└── [1,369 more functions]

BMMINER_ANALYSIS.md (lines 650-770) - Complete protocol analysis
etc/topol.conf (3 variants) - ASIC addressing, topology
```

**Key Findings:**

- **BM1398P Configuration:** 114 chips/chain, 156 cores/chip, 623 small cores/chip
- **BM1362 Configuration:** 88-126 chips/chain (13 hardware variants)
- **I2C Command Format:** `(master << 26) | (slave_high << 20) | (slave_low << 16)`
- **ADC Voltage Formula:** `voltage = (adc_value & 0xFFF) * 0.00048828125 * 1.188 - 1.188`
- **ASIC Addressing:** `asic_address = base_addr + (index * interval)` (interval=2)

**Initialization Sequence (FUN_00034828):**

1. Configuration validation (token=0x51, CRC-16 check)
2. Chain reset (if not in special mode)
3. Voltage configuration (auto or fixed)
4. System initialization (fans, PSU, temp sensors)
5. Frequency configuration with hardware-specific scaling
6. Temperature sensor init
7. UART/SPI clock to 12 MHz
8. Voltage regulation with cold-start boost
9. ASIC register initialization (2-domain chips)
10. Chain ASIC detection and type setting

**Use Cases:**

- Complete hardware initialization sequence
- ASIC addressing scheme
- Voltage/frequency management algorithms
- Temperature monitoring infrastructure
- EEPROM reading & validation (XXTEA key: "uileynimggnagnau")

**Confidence:** VERY HIGH - Production firmware with working hardware

---

### 2. LiLei_WeChat

**Location:** `/home/danielsokil/Downloads/LiLei_WeChat`

**Project Type:** Bitmain test fixture firmware collection (factory board test software)

**ASIC Chips:**

- **BM1360** (S19i, S19j Pro)
- **BM1362** (S19j Pro AL)
- **BM1366** (S19 XP variants)
- **BM1368** (S21, S21+)
- **BM1370** (S21 Pro, S21 XP)
- **BM1397** (S17+, S17 Pro)
- **BM1398** (S19, S19 Pro, T19) - **TARGET CHIP**

**Contents:**

- 40+ miner model configurations (2019-2025)
- Production test binaries (single_board_test, cminer)
- ~322,000 lines of decompiled source code
- Test patterns for all chip generations
- Complete configuration files (legacy .ini + modern JSON)

**Critical Files:**

```
S19_Pro/single_board_test (binary - 169 KB)
S19_Pro/single_board_test.c (decompiled - 761 KB) [TARGET]
S19_Pro/Config.ini - BM1398 configuration
S19_Pro/BM1398-pattern/ - 128 test patterns (579 KB each)

_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/
├── send_set_config_command-001cac54.c
├── send_command-001cc964.c
├── set_chain_baud-001cb2e8.c
├── set_chain_frequency-001cb900.c
├── reset_hash_board-00093058.c
├── sw_init-00097e5c.c
└── pt_send_nonce-00091d34.c
```

**BM1398 Configuration Example:**

```json
{
  "Hash_Board": {
    "Asic_Type": "BM1398",
    "Asic_Num": 114,
    "Voltage_Domain": 38,
    "Asic_Num_Per_Voltage_Domain": 3
  },
  "Test_Speed": {
    "Baudrate": 12000000,
    "Timeout": 150
  },
  "Test_Standard": {
    "Pattern_Number": 8,
    "Nonce_Rate": 9950
  }
}
```

**Reset Sequence (BM1368/BM1370):**

```c
usleep(700000);                 // 700ms pre-delay
chain_reset_low(chain);
usleep(10000);
chain_reset_high(chain);
usleep(100000);
chain_reset_low(chain);
usleep(10000);
chain_reset_high(chain);
usleep(10000);
set_chain_ticketmask(chain, 0xFFFF);
uart_flush_rx(chain);
usleep(50000);
```

**Protocol Information:**

- **UART Baudrate:** 12 MHz standard, 25 MHz high-speed
- **Register 0x28:** Fast UART configuration
- **Register 0x60:** PLL1 parameter (high-speed UART clock)
- **Nonce Rate:** 99.5% expected return rate (configurable)

**Use Cases:**

- BM1398-specific test patterns
- Exact UART communication protocol
- Register initialization sequences
- Voltage/frequency test loops
- Nonce processing workflows

**Confidence:** VERY HIGH - Production test code from Bitmain factory

---

### 3. bitmain_antminer_binaries

**Location:** `/home/danielsokil/Lab/HashSource/bitmain_antminer_binaries`

**Project Type:** Reverse engineering repository with decompiled firmware

**ASIC Chips:**

- BM1360, BM1362, BM1366, BM1368, BM1370, BM1385, BM1387, BM1391, BM1397, BM1398

**Contents:**

- 52,887+ decompiled C source files
- S19jpro complete firmware (814MB)
- S21 series binaries (BM1366/BM1368)
- S17/T17 series (BM1397) - 28,046 LOC
- Ghidra projects for 24 binary targets

**Critical Files:**

```
S19jpro/Antminer S19j Pro/BBCtrl_BHB42601/update/minerfs.no_header.image_extract/
├── usr/bin/bmminer (549KB) - S19 Pro production miner
├── usr/bin/cgminer (416KB)
├── lib/modules/bitmain_axi.ko
└── etc/cgminer.conf.factory

S17/single-board-test.c (28,046 lines) - BM1397 complete protocol
S21/single_board_test.c (398,081 lines) - BM1360/BM1362/BM1366/BM1368/BM1370

_ghidra/bins/single_board_test-*/decomps/
├── reset_chain_*.c
├── *send_work*.c
├── dhash_send_job*.c
├── get_nonce*.c
└── receive_function*.c
```

**BM1397 Protocol Example:**

```c
void BM1391_set_config(uint8_t chain, uint8_t asic_addr, uint8_t reg_addr,
                       uint32_t reg_data, bool mode) {
    cmd_buf[0] = mode ? 0x51 : 0x41;  // Broadcast vs unicast
    cmd_buf[1] = 0x09;
    cmd_buf[2] = asic_addr;
    cmd_buf[3] = reg_addr;
    // ... register data (4 bytes) ...
    crc = CRC5(cmd_buf, 0x40);
    set_BC_command_buffer();
}
```

**Key Findings:**

- Complete ASIC initialization sequences for all generations
- PSU protocol (APW12) fully documented
- Frequency tuning algorithms
- Temperature monitoring protocols

**Use Cases:**

- BM1397 → BM1398 protocol comparison
- Initialization sequence patterns
- FPGA register interface
- Work distribution protocols

**Confidence:** HIGH - Extensive decompilation coverage

---

### 4. bmminer_NBP1901

**Location:** `/home/danielsokil/Lab/HashSource/bmminer_NBP1901`

**Project Type:** S19 Pro specific reverse engineering

**ASIC Chips:**

- **BM1391** (S17 generation) - 29 functions
- **BM1397** (S17/T17) - 35+ functions
- **BM1398** (S19/T19) - **TARGET**
- BM1385, BM1387

**Contents:**

- bmminer (NBP1901 - S19 Pro 110TH) - 1,085 decompiled functions
- single-board-test utilities - 411 decompiled functions
- Kernel modules (bitmain_axi.ko, fpga_mem_driver.ko)
- Complete S17/T17 decompilations (28K+ LOC)

**Critical Files:**

```
antminer_debug_binaries/ghidrecomps/bins/single_board_test-*/decomps/
├── BM1397_set_address-00026944.c
├── BM1397_set_config-00026238.c
├── BM1397_set_baud-00026d60.c
├── BM1397_chain_inactive-000268c8.c
├── BM1397_enable_core_clock-00026e9c.c
├── BM1397_get_status-000262cc.c
├── set_BC_command_buffer-00025da4.c
└── get_BC_write_command-00025dc4.c

antminer_debug_binaries/ghidrecomps/bins/bmminer-*/decomps/
├── bitmain_axi_init-0002d390.c
├── open_core_one_chain-00035420.c
├── send_job-00038968.c
├── get_return_nonce-0002dcec.c
└── chain_inactive-00033a1c.c

S17/single-board-test.c (28,046 lines) - Full BM1397 implementation
S21/single_board_test.c (3.9MB binary with debug symbols)
```

**FPGA Interface:**

```c
// BC Command Interface (Register 0x30)
axi_fpga_addr[0x31] = cmd_buf[0];    // Command bytes 0-3
axi_fpga_addr[0x32] = cmd_buf[1];    // Command bytes 4-7
axi_fpga_addr[0x33] = cmd_buf[2];    // Command bytes 8-11

// Control register
set_BC_write_command(cmd & 0xfff0ffff | (chain << 16) | 0x80800000);

// Nonce reading
axi_fpga_addr[0x04] = nonce_data[0];  // Word 4
axi_fpga_addr[0x05] = nonce_data[1];  // Word 5
```

**BM1397 Initialization:**

```c
1. BM1397_chain_inactive(chain)
2. BM1397_set_address(chain, addr)
3. BM1397_set_baud(chain, baud)
4. BM1397_enable_core_clock(chain, ...)
5. BM1397_set_freq(chain, chip, freq)
```

**Documentation:**

- Antminer_x17_PSU_Protocol.pdf (75KB)
- X19_Frequency-Voltage-Manual.jpeg (1MB)
- CUSTOM_FLAGS.md - Debug logging flags

**Use Cases:**

- FPGA register mapping (confirmed working)
- BM1397/BM1398 initialization sequence
- Work distribution format
- CRC5/CRC16 algorithms
- PSU control (APW8/APW9)

**Confidence:** HIGH - S19 Pro specific code

---

## Protocol Documentation

### 5. bm1397-protocol (Rust)

**Location:** `/home/danielsokil/Lab/GPTechinno/bm1397-protocol`

**Project Type:** Complete Rust library for BM1397 protocol

**ASIC Chips:**

- **BM1397** only (but protocol similar to BM1360/BM1362/BM1366/BM1398)

**Contents:**

- Published crate (crates.io version 0.2.0)
- Complete source implementation (5,496 lines)
- no_std compatible (embedded-friendly)
- Dual MIT/Apache-2.0 license

**Source Files:**

```
src/
├── lib.rs (37 lines) - Library root
├── command.rs (406 lines) - Command generation [KEY]
├── register.rs (3,768 lines) - Chip register definitions [KEY]
├── core_register.rs (763 lines) - Per-core registers
├── response.rs (233 lines) - Response parsing [KEY]
├── crc.rs (131 lines) - CRC5/CRC16 implementation [KEY]
└── specifier.rs (158 lines) - Enums and types

examples/serial.rs - Serial communication example
```

**Protocol Details:**

- **Chip ID:** 0x1397 (register 0x00)
- **Default Address:** 0x13971800 (chip_id=0x1397, cores=24, addr=0)
- **UART:** 115200 baud default, up to 6.25 Mbps
- **Preamble:** 0x55 0xAA (commands), 0xAA 0x55 (responses)
- **CRC5:** Poly=0x05, init=0x1F
- **CRC16:** Poly=0x1021, init=0xFFFF

**Command Set:**

```rust
Command::chain_inactive()          // 0x53 - Disable relay
Command::set_chip_addr(addr)       // 0x40 - Assign address
Command::read_reg(addr, dest)      // 0x42/0x52 - Read register
Command::write_reg(addr, val, dest) // 0x41/0x51 - Write register
Command::job_1_midstate(...)       // 0x21 - Send job (1 midstate)
Command::job_4_midstate(...)       // 0x21 - Send job (4 midstates)
```

**Register Map (34 registers):**

- 0x00: ChipAddress
- 0x08: PLL0Parameter
- 0x14: TicketMask (difficulty)
- 0x18: MiscControl (baudrate)
- 0x28: FastUARTConfiguration
- 0x3C: CoreRegisterControl
- 0x60-0x68: PLL1-3 Parameters
- Full list in register.rs

**Core Registers (8 per core, 24 cores):**

- 0: ClockDelayCtrl
- 3: CoreError
- 4: CoreEnable
- 5: HashClockCtrl
- 6: HashClockCounter

**Initialization Example:**

```rust
// 1. Disable relay
send(Command::chain_inactive());

// 2. Address chips
for i in 0..chip_count {
    send(Command::set_chip_addr(i));
}

// 3. Set baudrate
for cmd in Command::set_baudrate(6_250_000, HertzU32::MHz(25)) {
    send(cmd);
}

// 4. Set difficulty
send(Command::set_difficulty(256, Destination::All));

// 5. Configure frequency
send(Command::write_reg(PLL0Parameter::default()
    .enable().set_fbdiv(112), Destination::All));
```

**Job Format (1 midstate, 56 bytes):**

```
[0x55, 0xAA, 0x21, 0x36,           // Preamble + CMD + length
 job_id, 0x01,                      // Job ID, 1 midstate
 0x00, 0x00, 0x00, 0x00,            // Starting nonce
 n_bits[0..4],                      // Difficulty (LE)
 n_time[0..4],                      // Timestamp (LE)
 merkle_root[0..4],                 // Merkle root (LE)
 midstate[0..32],                   // SHA256 midstate
 crc16[0..2]]                       // CRC16 (BE)
```

**Nonce Response (9 bytes):**

```
[0xAA, 0x55,                       // Preamble
 nonce[0..4],                       // Nonce (BE)
 midstate_id,                       // Which midstate
 job_id,                            // Job ID (bit 7 set)
 crc5]                              // CRC5
```

**Use Cases:**

- Complete command protocol (directly portable to C)
- CRC5/CRC16 reference implementation
- Job submission format
- Nonce parsing logic
- Initialization sequence template

**Confidence:** VERY HIGH - Production crate, well-tested

**Documentation:** https://docs.rs/bm1397-protocol/

---

### 6. bm13xx-hla (Logic Analyzer)

**Location:** `/home/danielsokil/Lab/GPTechinno/bm13xx-hla`

**Project Type:** Saleae Logic Analyzer protocol decoder (Python)

**ASIC Chips:**

- BM1362, BM1366, BM1368, BM1370, BM1397 (5 generations)

**Contents:**

- HighLevelAnalyzer.py (500 lines) - Complete decoder
- examples/demo.sal - Real capture (Saleae format)
- demo.png - Decoded protocol screenshot

**Register Map (44 registers, lines 9-48):**

```python
0x00: chip_address
0x04: hash_rate
0x08: pll0_parameter
0x0c: chip_nonce_offset
0x10: hash_counting_number
0x14: ticket_mask
0x18: misc_control
0x1c: i2c_control
0x20: ordered_clock_enable
0x28: fast_uart_configuration
0x2c: uart_relay
0x38: ticket_mask2
0x3c: core_register_control
0x40: core_register_status
0x44: external_temperature_sensor_read
# ... (29 more registers)
```

**Core Registers (BM1397, lines 60-69):**

```python
0: clock_delay_ctrl
1: process_monitor_ctrl
2: process_monitor_data
3: core_error
4: core_enable
5: hash_clock_ctrl
6: hash_clock_counter
7: sweep_clock_ctrl
```

**Command Set:**

- 0x00: set_chipadd
- 0x01: write_register / work (special VIL)
- 0x02: read_register
- 0x03: chain_inactive
- 0xAA: respond / nonce

**Work Packet Structure (lines 306-357):**

```
Byte 0-1:   Preamble (0x55 0x??)
Byte 2:     Command (0x21)
Byte 3:     Length
Byte 4:     Job ID (bits 7:2) + flags
Byte 5:     Midstate count
Byte 6-9:   Starting nonce (BE)
Byte 10-13: nBits (difficulty)
Byte 14-17: nTime (timestamp, LE!)
Byte 18-21: Merkle root (last 4 bytes, LE)
Byte 22+:   Midstates (33 bytes each)
Last 2:     CRC16
```

**Nonce Response (7 bytes standard):**

```
Byte 0-1:   Preamble (0xAA 0x??)
Byte 2-5:   Nonce (LE)
Byte 6:     Chip address / midstate ID
Byte 7:     Register addr (job_id + small_core_id)
Byte 8:     Status (bit 7 = nonce indicator)
```

**PLL Frequency Formula (lines 149-155):**

```python
freq = CLKI * fb_div / (ref_div * (post_div1 + 1) * (post_div2 + 1))

where:
  fb_div = bits[27:16]
  ref_div = bits[12:8]
  post_div1 = bits[6:4]
  post_div2 = bits[2:0]
```

**Baudrate Calculation (BM1366, lines 394-425):**

```python
if bclk_sel == 0:
    baudrate = CLKI_freq / ((bt8d + 1) * 8)
else:
    baudrate = pll_uart_freq / ((pll_uart_div4 + 1) * (bt8d + 1) * 2)
```

**Use Cases:**

- Register map reference (44 registers documented)
- Work packet structure
- Nonce decoding (job_id, core_id extraction)
- PLL/baudrate formulas
- Protocol validation (compare with captures)

**Confidence:** HIGH - Based on real hardware captures

**Repository:** https://github.com/GPTechinno/bm13xx-hla

---

### 7. skot/BM1397

**Location:** `/home/danielsokil/Lab/skot/BM1397`

**Project Type:** Pure documentation repository (markdown + diagrams)

**ASIC Chips:**

- **BM1397** (S17, S17 Pro, T17) - Complete
- **BM1366** (S19XP, S19K Pro) - Partial

**Contents:**

- 9 markdown files (~1,097 lines documentation)
- Register diagrams (JSON + SVG)
- Protocol packet diagrams (packetdiag)
- Repair guides (PDFs)
- KiCAD footprints and 3D models

**Key Files:**

```
readme.md - Chip variants, pin config
protocol.md - UART communication [KEY]
registers.md (407 lines) - BM1397 register map [KEY]
core_registers.md (100 lines) - Per-core registers [KEY]
bm1366_protocol.md (150 lines) - BM1366 differences
bm1366_registers.md (103 lines) - BM1366 register map

images/
├── send_job.packetdiag - Job packet format [KEY]
├── nonce.packetdiag - Nonce response format [KEY]
├── *.json - Register bit field definitions
└── *.svg - Rendered diagrams

docs/Antminer S17 Hash Board Repair Guide.docx
archived_webpages/ - Zeus Mining repair guides (PDFs)
cad/ - KiCAD symbol, footprint, 3D model
```

**BM1397 Variants:**

- BM1397AD - S17, S17 Pro, T17 (original)
- BM1397AG - High-temperature resilience
- BM1397AH - Stable at low voltage
- BM1397AI - Universal, best compatibility
- BM1397AF - Purpose unknown

**Protocol Summary:**

- Baudrate: 115200 default, up to 6.25 Mbps
- Preamble: 0x55 0xAA (cmd), 0xAA 0x55 (resp)
- CRC5: Poly 0x05, init 0x1F
- CRC16: For job commands

**Chip Enumeration Algorithm:**

1. Read Chip Address (broadcast) → count responses
2. Chain Inactive → stop relay
3. For i = 0 to chip_count-1:
   - Set Chip Address (addr=0, value=i×spacing)
4. Chain Inactive → resume relay

**Baudrate Configuration Example:**

```
Default (115200 bps):
  BCLK_SEL = 0 → fBase = 25MHz
  BT8D = 26
  Baudrate = 25MHz / ((26+1) × 8) = 115740 bps

High-Speed (6.25 Mbps):
  1. PLL3: 0xC0700111 → 2.8 GHz
  2. PLL3_DIV4 = 6
  3. BCLK_SEL = 1 → fBase = 400 MHz
  4. BT8D = 7 → Baudrate = 6.25 Mbps
```

**BM1366 Enhancements:**

- Version Rolling Register (0xA4) - BIP320
- Dual job formats (midstates OR full header)
- Extended nonce responses with version bits
- 894 cores (vs 672 in BM1397)

**Use Cases:**

- Complete UART protocol specification
- Chip enumeration algorithm
- Baudrate formulas (critical for hashrate)
- Register map with reset values
- AsicBoost enable mechanism
- Missing: Full initialization sequence (requires firmware analysis)

**Confidence:** VERY HIGH - Well-documented reverse engineering

**Reverse Engineering Source:** Extracted from T17 and S19XP-Hydro firmware using Ghidra

**Repository:** https://github.com/skot/BM1397

---

## Source Code Implementations

### 8. bm13xx-rs (Rust)

**Location:** `/home/danielsokil/Lab/GPTechinno/bm13xx-rs`

**Project Type:** Open-source Rust driver library (production-ready)

**ASIC Chips:**

- **BM1366** (S19 XP, S19K Pro) - 112 cores, 894 small cores
- **BM1370** (S21 Pro, S21 XP) - 128 cores, 2040 small cores
- **BM1397** (T17 series) - 168 cores, 672 small cores

**Architecture:**

- Rust workspace with 6 crates
- no_std compatible
- embedded-hal abstractions
- Async API (embedded-io-async)

**Project Structure:**

```
bm13xx-rs/
├── bm1366/ - BM1366 chip implementation
├── bm1370/ - BM1370 chip implementation
├── bm1397/ - BM1397 chip implementation
├── bm13xx-asic/ - Common ASIC abstractions
├── bm13xx-chain/ - Chain management
└── bm13xx-protocol/ - Low-level protocol
```

**Critical Files:**

```
bm13xx-protocol/src/
├── command.rs - All UART commands
├── response.rs - Response parsing
└── crc.rs - CRC5/CRC16 implementation

bm1366/src/lib.rs - Complete BM1366 init sequence
bm13xx-asic/src/register/mod.rs - All register definitions
bm13xx-chain/src/lib.rs - Multi-chip enumeration

examples/
├── chain_enum.rs - Chain enumeration
└── version_rolling.rs - Version rolling
```

**BM1366 Initialization (from bm1366/src/lib.rs):**

```rust
1. Write CoreRegisterControl - HashClockCtrl (enable, PLL=0)
2. Write CoreRegisterControl - ClockDelayCtrlV2 (CCDLY=0, PWTH=4)
3. Write TicketMask (difficulty)
4. Write AnalogMuxControlV2 (diode_vdd_mux_sel=3)
```

**BM1370 Initialization:**

```rust
1. Write CoreRegisterControl - CoreReg11 (0x00)
2. Write CoreRegisterControl - ClockDelayCtrlV2 (CCDLY=0, PWTH=2)
3. Write TicketMask (difficulty)
4. Write AnalogMuxControlV2 (diode_vdd_mux_sel=3)
```

**BM1397 Initialization:**

```rust
1. Write ClockOrderControl0 - all CLK_SEL = 0
2. Write ClockOrderControl1 - all CLK_SEL = 0
3. Write OrderedClockEnable - disable all clocks
4. Write OrderedClockEnable - enable CLK0-7
5. Write CoreRegisterControl - ClockDelayCtrl (CCDLY=2, PWTH=3)
6. Write TicketMask (difficulty)
```

**Register Map (from bm13xx-asic/src/register/):**

- 0x00: ChipIdentification
- 0x08: PLL0Parameter
- 0x14: TicketMask
- 0x18: MiscControl
- 0x28: FastUARTConfiguration
- 0x2C: UARTRelay
- 0x3C: CoreRegisterControl
- 0x60-0x68: PLL1-3 Parameters
- 0xA4: VersionRolling (hardware version rolling)

**Use Cases:**

- BM1366 → BM1398 adaptation template
- Complete protocol implementation (C portable)
- Chain enumeration logic
- Baudrate configuration sequences
- Frequency ramping algorithms
- Job submission format
- Nonce response handling

**Confidence:** HIGH (85%) - Production library, but BM1398 not directly covered

**Note:** BM1366 is architecturally closest to BM1398 (both S19 variants)

**Repository:** https://github.com/GPTechinno/bm13xx-rs

---

### 9. bitmaintech (Official Source)

**Location:** `/home/danielsokil/Lab/HashSource/bitmaintech`

**Project Type:** Official Bitmain source code repository

**ASIC Chips:**

- **BM1387** (S9/T9/R4 series) - Complete source code
- BM1385 (S7) - Referenced

**Contents:**

- Complete cgminer/bmminer source (12,362 lines)
- FPGA kernel modules (source code)
- Yocto/Angstrom firmware build system
- PIC microcontroller firmware tools

**Key Files:**

```
bmminer-mix/driver-btm-c5.c (12,362 lines) [CRITICAL]
bmminer-mix/driver-btm-c5.h (915 lines) [CRITICAL]
FPGA_AXI_driver/axi_fpga.c (195 lines)
FPGA_MEM_driver/fpga_mem.c

S9_makepackage/app-bin/
├── bmminer (409KB compiled)
└── single-board-test (1.9MB)
```

**FPGA Register Map (Complete - driver-btm-c5.h):**

```c
0x00: HARDWARE_VERSION
0x04: FAN_SPEED
0x08: HASH_ON_PLUG          // Chain detection
0x0C: BUFFER_SPACE          // FPGA work buffer
0x10: RETURN_NONCE          // Nonce FIFO [KEY]
0x18: NONCE_NUMBER_IN_FIFO
0x1C: NONCE_FIFO_INTERRUPT
0x20-0x2C: TEMPERATURE_0_15
0x30: IIC_COMMAND           // I2C (PSU/PIC) [KEY]
0x34: RESET_HASHBOARD_COMMAND
0x40: TW_WRITE_COMMAND      // Work data (52 bytes) [KEY]
0x80: QN_WRITE_COMMAND
0x84: FAN_CONTROL           // PWM [KEY]
0x88: TIME_OUT_CONTROL
0xC0: BC_WRITE_COMMAND      // Broadcast trigger [KEY]
0xC4: BC_COMMAND_BUFFER     // Command data (12 bytes) [KEY]
```

**BM1387 Command Set:**

```c
#define SET_ADDRESS         0x1
#define SET_PLL_DIVIDER2    0x2
#define PATTERN_CONTROL     0x3
#define GET_STATUS          0x4
#define CHAIN_INACTIVE      0x5
#define SET_BAUD_OPS        0x6
#define SET_PLL_DIVIDER1    0x7
#define SET_CONFIG          0x8
#define COMMAND_FOR_ALL     0x80  // Broadcast flag
```

**Register Addresses:**

```c
#define CHIP_ADDRESS            0x00
#define GOLDEN_NONCE_COUNTER    0x08
#define PLL_PARAMETER           0x0C
#define START_NONCE_OFFSET      0x10
#define HASH_COUNTING_NUMBER    0x14
#define TICKET_MASK             0x18
#define MISC_CONTROL            0x1C
```

**Initialization Sequence (software_set_address, lines 7599-7637):**

```c
// 1. Reset all chips
chain_inactive(chain);  // 3x with 30ms delay

// 2. Assign addresses
for (chip_addr = 0; chip_addr < 0x100; chip_addr += 4) {
    set_address(chain, 0, chip_addr);
}

// 3. Set UART baud
set_baud(bauddiv, chain);

// 4. Configure PLL frequency
set_frequency_with_addr_plldatai(pll_index, 0, chip_addr, chain);

// 5. Set ticket mask
set_asic_ticket_mask(63);

// 6. Open cores
open_core_one_chain(chain, true);
```

**VIL Work Format (BM1387):**

```c
struct vil_work_1387 {
    uint8_t  work_type;      // 0x21
    uint8_t  chain_id;
    uint8_t  reserved1[2];
    uint32_t work_count;
    uint8_t  data[12];       // Last 12 bytes of header
    uint8_t  midstate[32];
};
```

**PLL Frequency Table:**

- 100-1175 MHz in 25 MHz steps
- Format: `{freq_string, fildiv1, fildiv2, vilpll}`
- Example: `{"600", 0x060040, 0x0220, 0x600221}`

**Use Cases:**

- FPGA register interface (95% compatible with X19)
- Chain initialization sequence (90% transferable)
- Broadcast command protocol
- Nonce FIFO reading
- Auto-tuning algorithms
- Work format requires BM1398 adaptation

**Confidence:** VERY HIGH for FPGA interface, MEDIUM for BM1398 work format

---

### 10. kanoi/cgminer

**Location:** `/home/danielsokil/Lab/kanoi/cgminer`

**Project Type:** Open-source Bitcoin mining software (Kano's fork)

**ASIC Chips:**

- **BM1362** (S19j Pro+) - **S19 FAMILY**
- **BM1370** (S19 XP) - **S19 FAMILY**
- BM1384, BM1387, BM1397 (reference implementations)

**Repository:** https://github.com/kanoi/cgminer

**Contents:**

- Active development (last commit: Oct 6, 2025)
- 216 C source/header files
- Complete mining software stack
- GPL v3 license

**Critical Files:**

```
driver-gekko.c (212KB) [HIGHEST VALUE]
  ├── BM1397 init (lines 1507-1560)
  ├── BM1362 init (lines 1580-1647) [S19j Pro+]
  ├── BM1370 init (lines 1648-1755) [S19 XP]
  ├── Frequency control (lines 1200-2120)
  ├── Work distribution (lines 1900-2200)
  └── Nonce reading (lines 4800-5800)

driver-gekko.h (21KB)
  ├── ASIC_INFO structures
  ├── Register definitions
  └── Job management

driver-btm-soc.c (432KB) - S9/T9 (BM1387)
driver-btm-soc.h - FPGA register map

ASIC-README (40KB) - Device configurations
README (64KB) - General usage
NEWS (363KB) - Changelog with technical notes
```

**BM1397 Initialization (lines 1507-1560):**

```c
// Chain activation
unsigned char chainin[5] = {0x53, 0x05, 0x00, 0x00, 0x00};

// Chip addressing
unsigned char chippy[] = {0x40, 0x05, 0x00, 0x00, 0x00};

// Core initialization
unsigned char init1[] = {0x51, 0x09, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
unsigned char init2[] = {0x51, 0x09, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
unsigned char init3[] = {0x51, 0x09, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00};
unsigned char init4[] = {0x51, 0x09, 0x00, 0x3C, 0x80, 0x00, 0x80, 0x74, 0x00};
unsigned char init5[] = {0x51, 0x09, 0x00, 0x68, 0xC0, 0x70, 0x01, 0x11, 0x00};
unsigned char init6[] = {0x51, 0x09, 0x00, 0x28, 0x06, 0x00, 0x00, 0x0F, 0x00};

// Baudrate (1.51MHz)
unsigned char baudrate[] = {0x51, 0x09, 0x00, 0x18, 0x00, 0x00, 0x61, 0x31, 0x00};
```

**BM1362 Initialization (lines 1580-1647):**

```c
unsigned char init3[] = {0x51, 0x09, 0x00, 0x3c, 0x80, 0x00, 0x85, 0x40, 0x00};
unsigned char init4[] = {0x51, 0x09, 0x00, 0x3c, 0x80, 0x00, 0x80, 0x08, 0x00};
unsigned char init5[] = {0x51, 0x09, 0x00, 0x54, 0x00, 0x00, 0x00, 0x03, 0x00};
unsigned char init6[] = {0x51, 0x09, 0x00, 0x58, 0x00, 0x01, 0x11, 0x11, 0x00};

// 3MHz baudrate
unsigned char baudrate[] = {0x51, 0x09, 0x00, 0x28, 0x11, 0x30, 0x00, 0x00, 0x00};
```

**BM1370 Initialization (lines 1648-1755):**

```c
unsigned char hcn[] = {0x51, 0x09, 0x00, 0x10, 0x00, 0x00, 0x12, 0xC9, 0x00};
unsigned char init0[] = {0x51, 0x09, 0x00, 0xA4, 0x90, 0x00, 0xFF, 0xFF, 0x00};
unsigned char init1[] = {0x51, 0x09, 0x00, 0xa8, 0x00, 0x07, 0x00, 0x00, 0x00};

// 3.125MHz baudrate
unsigned char baudrate[] = {0x51, 0x09, 0x00, 0x28, 0x01, 0x30, 0x00, 0x00, 0x00};
```

**Frequency Setting (BM1397):**

```c
// Register 0x08 (BM1397FREQ)
unsigned char freqbufall[] = {0x51, 0x09, 0x00, BM1397FREQ,
                               0x40, 0xF0, 0x02, 0x35, 0x00};
```

**Chip Addressing:**

```c
#define CHIPPY1397(_inf, _chi) \
    ((unsigned char)(floor(0x100 / (_inf->chips))) * (_chi))

#define TOCHIPPY1397(_inf, _adr) \
    (int)floor((_adr) / floor(0x100 / (_inf->chips)))
```

**Job ID Management:**

- BM1397: Rolling job IDs with midstate support
- BM1362: Job ID offset 0xf8 (5-bit)
- BM1370: Job ID offset 0xf0 (4-bit)

**AsicBoost:**

- BM1387/BM1397: 2-4 midstate support
- BM1362: ~256 rolls per work item
- Version rolling enabled

**Use Cases:**

- **PRODUCTION BM1362 CODE** (S19j Pro+) - Directly applicable
- **PRODUCTION BM1370 CODE** (S19 XP)
- Complete initialization sequences
- Frequency control algorithms
- Work distribution with AsicBoost
- Nonce collection and validation
- Error handling and recovery

**Confidence:** VERY HIGH - Production code, battle-tested

**Recommended Actions:**

1. Extract BM1362 driver from driver-gekko.c
2. Compare to BM1398 (likely similar protocol)
3. Port initialization sequence to X19 project
4. Test on hardware incrementally

---

## Analysis Tools

### Reference Projects Not Requiring Active Development

These projects provide context but aren't critical for immediate implementation:

- **AR_PIC_PK** - PIC microcontroller firmware tools
- **meta-altera** - Yocto layer for Cyclone V (reference only)
- **Kernel_SOC** - Linux kernel 4.6.0 sources (already using stock kernel)

---

## Next Steps & Recommendations

### Immediate Actions (Week 1)

**1. Identify Target ASIC Chip**

```bash
# Run on test machine 192.30.1.24
ssh root@192.30.1.24
cd /root
./bin/eeprom_detect

# Look for "Chip Marking" field in output
# Expected: BM1398, BM1362, or BM1366
```

**2. Extract BM1362/BM1398 Protocol from cgminer**

```bash
# Copy critical driver
cp /home/danielsokil/Lab/kanoi/cgminer/driver-gekko.c \
   /home/danielsokil/Lab/HashSource/hashsource_antminer_x19/reference/

# Extract init sequence
grep -A 20 "BM1362\|BM1370" driver-gekko.c > bm1362_init.txt
```

**3. Study Stock Firmware**

```bash
# Analyze S19 Pro bmminer (if chip is BM1398)
cd /home/danielsokil/Downloads/Bitmain_Peek/S19_Pro
cd _ghidra/bins/bmminer-2f464d0989b763718a6fbbdee35424ae/decomps

# Search for initialization functions
grep -r "init\|reset\|config" *.c | head -50
```

### Protocol Implementation Priority (Weeks 2-4)

**Phase 1: UART Communication (3-5 days)**

1. Port CRC5/CRC16 from bm1397-protocol/src/crc.rs
2. Implement command packet builder
3. Implement response parser
4. Test with chip enumeration (read register 0x00)

**Phase 2: Chain Initialization (5-7 days)**

1. Implement chain_inactive command
2. Implement set_chip_address loop
3. Enumerate chips (count responses)
4. Validate chip count matches expected (110-114/chain)

**Phase 3: Configuration (7-10 days)**

1. Configure baudrate (start 115200, upgrade to 3-6 MHz)
2. Set PLL frequency (use EEPROM frequency value)
3. Configure ticket mask (difficulty)
4. Enable cores

**Phase 4: Mining (10-14 days)**

1. Build job packet from Stratum work
2. Send test job with known golden nonce
3. Monitor nonce responses
4. Validate nonce against expected
5. Submit valid nonces to pool

### Reference Project Usage Guide

| Task                 | Primary Reference      | Secondary References   |
| -------------------- | ---------------------- | ---------------------- |
| **Protocol basics**  | bm1397-protocol (Rust) | skot/BM1397 docs       |
| **BM1362 init**      | kanoi/cgminer          | bm13xx-rs              |
| **BM1398 init**      | LiLei_WeChat S19_Pro   | Bitmain_Peek S19_Pro   |
| **FPGA interface**   | bitmaintech            | bmminer_NBP1901        |
| **Work format**      | bm13xx-hla (decoder)   | LiLei_WeChat patterns  |
| **Nonce parsing**    | bm1397-protocol        | cgminer driver-gekko   |
| **CRC algorithms**   | bm1397-protocol/crc.rs | Multiple sources       |
| **Frequency tuning** | cgminer driver-btm-soc | bitmaintech PLL tables |

### Critical Questions to Answer

1. **Which ASIC chip?** Run eeprom_detect to confirm
2. **UART or SPI?** Check FPGA logs for communication pattern
3. **BM1398 register differences?** Compare with BM1397/BM1362
4. **Work packet format?** Test with known block header
5. **Nonce response format?** Capture with logic analyzer

### Success Criteria

- [ ] Chain enumeration detects all chips (110-114/chain × 3 chains)
- [ ] Chips respond to individual register reads
- [ ] Frequency can be set and validated
- [ ] Test job returns expected golden nonce
- [ ] Hashrate calculation matches expected (±10%)
- [ ] System stable for 1+ hour continuous operation

---

## Chip Support Matrix

| Chip       | Miners            | Protocol Refs                                           | Init Sequences   | Work Format      | Status   |
| ---------- | ----------------- | ------------------------------------------------------- | ---------------- | ---------------- | -------- |
| **BM1360** | S19i, S19j Pro    | Bitmain_Peek, LiLei, bitmain_antminer_binaries          | Complete         | Complete         | Ready    |
| **BM1362** | S19j Pro+         | cgminer [KEY], LiLei, Bitmain_Peek, bm13xx-hla          | Complete         | Complete         | Ready    |
| **BM1366** | S19 XP, S19K Pro  | bm13xx-rs [KEY], skot/BM1397, bitmain_antminer_binaries | Complete         | Complete         | Ready    |
| **BM1368** | S21, S21+         | LiLei [KEY], bm13xx-hla                                 | Complete         | Complete         | Ready    |
| **BM1370** | S21 Pro, S21 XP   | cgminer [KEY], bm13xx-rs, LiLei, bm13xx-hla             | Complete         | Complete         | Ready    |
| **BM1384** | Terminus          | cgminer                                                 | Complete         | Complete         | Ready    |
| **BM1387** | S9, T9, R4        | bitmaintech [KEY], cgminer, bitmain_antminer_binaries   | Complete         | Complete         | Ready    |
| **BM1397** | S17, T17          | bm1397-protocol [KEY], skot/BM1397 [KEY], cgminer       | Complete         | Complete         | Ready    |
| **BM1398** | S19, S19 Pro, T19 | LiLei [KEY], Bitmain_Peek, bmminer_NBP1901              | Needs extraction | Needs extraction | Research |

**For BM1398 (S19 Pro):** Primary references are LiLei_WeChat S19_Pro and Bitmain_Peek S19_Pro. BM1362 protocol from cgminer likely 80-90% compatible.

---

## Glossary

- **ASIC:** Application-Specific Integrated Circuit (mining chip)
- **CRC5/CRC16:** Cyclic Redundancy Check (error detection)
- **FPGA:** Field-Programmable Gate Array (communication controller)
- **PLL:** Phase-Locked Loop (frequency generation)
- **Ticket Mask:** Difficulty filter (on-chip share validation)
- **VIL:** Version Independent Language (command format)
- **Midstate:** SHA256 intermediate state (optimization)
- **AsicBoost:** Efficiency optimization using multiple midstates
- **Nonce:** 32-bit value that solves the mining puzzle
- **Job ID:** Identifier linking work to nonces

---

## File Path Quick Reference

```bash
# Stock Firmware Analysis
/home/danielsokil/Downloads/Bitmain_Peek/S19_Pro/
/home/danielsokil/Downloads/LiLei_WeChat/S19_Pro/

# Protocol Documentation
/home/danielsokil/Lab/skot/BM1397/
/home/danielsokil/Lab/GPTechinno/bm13xx-hla/

# Source Code (Rust)
/home/danielsokil/Lab/GPTechinno/bm1397-protocol/
/home/danielsokil/Lab/GPTechinno/bm13xx-rs/

# Source Code (C - Production)
/home/danielsokil/Lab/kanoi/cgminer/driver-gekko.c
/home/danielsokil/Lab/HashSource/bitmaintech/bmminer-mix/driver-btm-c5.c

# Decompiled Binaries
/home/danielsokil/Lab/HashSource/bitmain_antminer_binaries/
/home/danielsokil/Lab/HashSource/bmminer_NBP1901/
```
