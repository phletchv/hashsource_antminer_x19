# Hashboard Mining Implementation Plan

**Goal:** Get chain 0 hashing on Antminer S19 Pro test machines

**Current Status:** 70% complete - Hardware control working (fans, PSU, GPIO, EEPROM), ASIC mining protocol not yet implemented

**Target Hardware:**

- Test Machine 1: 192.30.1.24 (root/root)
- Test Machine 2: 192.168.1.27 (root/root)

---

## Phase 1: Protocol Analysis

Reverse engineer ASIC communication protocol from reference projects and stock firmware.

### Tasks

- [ ] **Task 1:** Analyze bm1397-protocol reference project

  - Location: `/home/danielsokil/Lab/GPTechinno/bm1397-protocol`
  - Goal: Extract ASIC command format and protocol documentation
  - Output: Command structure, register maps, initialization sequences

- [ ] **Task 2:** Analyze bm13xx-hla reference project

  - Location: `/home/danielsokil/Lab/GPTechinno/bm13xx-hla`
  - Goal: Protocol insights from logic analyzer captures
  - Output: SPI timing, command sequences, response formats

- [ ] **Task 3:** Analyze bm13xx-rs reference project

  - Location: `/home/danielsokil/Lab/GPTechinno/bm13xx-rs`
  - Goal: Rust implementation patterns and protocol structure
  - Output: Work distribution, nonce handling, chip detection

- [ ] **Task 4:** Analyze Bitmain_Peek/S19_Pro stock firmware

  - Location: `/home/danielsokil/Downloads/Bitmain_Peek/S19_Pro`
  - Goal: Stock firmware protocol reference
  - Output: Official initialization sequences, register values

- [ ] **Task 5:** Analyze bmminer binary for SPI command sequences

  - Location: `/home/danielsokil/Lab/HashSource/bmminer_NBP1901`
  - Goal: Decompile SPI operations (Ghidra/IDA Pro)
  - Output: Complete command sequences, timing requirements

- [ ] **Task 6:** Analyze cgminer (kanoi) for BM13xx support

  - Location: `/home/danielsokil/Lab/kanoi/cgminer`
  - Goal: Open-source BM13xx implementation
  - Output: Work distribution, pool integration, auto-tuning

- [ ] **Task 7:** Analyze skot/BM1397 reference project
  - Location: `/home/danielsokil/Lab/skot/BM1397`
  - Goal: Additional BM1397 protocol reference
  - Output: Command validation, edge cases

**Deliverable:** Comprehensive protocol documentation (commands, registers, timing)

---

## Phase 2: FPGA & Hardware Setup

Implement low-level ASIC communication infrastructure.

### Tasks

- [ ] **Task 8:** Document FPGA SPI controller registers and protocol

  - Goal: Reverse engineer SPI controller (similar to I2C at 0x030)
  - Method: FPGA register sniffing during stock firmware boot
  - Output: Register offsets, command format, ready bits

- [ ] **Task 9:** Identify ASIC chip model on test machine hashboards

  - Method: Read EEPROM chip marking field (already implemented)
  - Expected: BM1360, BM1362, BM1398, or BM1366
  - Output: Chip model, variant, die code

- [ ] **Task 10:** Implement FPGA SPI controller driver
  - File: `hashsource_x19/src/spi_driver.c`
  - Functions: `spi_init()`, `spi_send_cmd()`, `spi_read_response()`
  - Dependencies: `/dev/axi_fpga_dev`, `bitmain_axi.ko` module

**Deliverable:** Working SPI driver for ASIC communication

---

## Phase 3: Chain Initialization

Power up and initialize chain 0 ASIC chips.

### Tasks

- [ ] **Task 11:** Implement chain 0 power-up sequence

  - Pre-requisites: PSU voltage set (from EEPROM), fans running
  - GPIO control: Chain enable pins (if separate from PSU)
  - Timing: Power stabilization delays
  - Output: Chain powered and ready for SPI

- [ ] **Task 12:** Implement ASIC chip detection on chain 0

  - Method: SPI broadcast command + address enumeration
  - Expected: 95-114 chips per chain (varies by model)
  - Output: Chip count, addresses, responses

- [ ] **Task 13:** Implement ASIC chip initialization sequence

  - Commands: Reset, wake-up, configure PLLs
  - Registers: Core voltage, clock dividers, nonce range
  - Verification: Read chip ID/version registers
  - Output: All chips initialized and idle

- [ ] **Task 14:** Implement frequency/voltage configuration for chips
  - Source: EEPROM voltage/frequency fields (already decoded)
  - Method: Write PLL config registers via SPI
  - Validation: Measure actual frequency (if possible)
  - Output: Chips running at target frequency

**Deliverable:** Chain 0 fully initialized and ready to hash

---

## Phase 4: Mining Implementation

Implement actual Bitcoin SHA256 mining operations.

### Tasks

- [ ] **Task 15:** Implement work distribution (send mining jobs to chips)

  - Format: Block header (80 bytes), nonce range per chip
  - Method: SPI write to work registers
  - Distribution: Divide nonce space across all chips
  - Output: Work sent, chips hashing

- [ ] **Task 16:** Implement nonce collection (receive results from chips)

  - Method: SPI polling or interrupt-driven (TBD from protocol analysis)
  - Format: Nonce value (32-bit), chip address
  - Timing: Check for results every N milliseconds
  - Output: Nonce values received

- [ ] **Task 17:** Implement nonce validation and submission

  - Validation: Recompute SHA256(SHA256(header + nonce))
  - Check: Result < target difficulty
  - Output: Valid nonces identified

- [ ] **Task 18:** Test chain 0 hashing with fixed mining work

  - Test vector: Known block header with valid nonce
  - Expected: Chips find the nonce and return it
  - Metrics: Time to solution, error rate
  - Output: Proof of concept working

- [ ] **Task 19:** Verify hashrate calculation and reporting
  - Formula: (nonces_checked / elapsed_time)
  - Expected: ~50-110 TH/s per chain (model-dependent)
  - Validation: Compare to stock firmware performance
  - Output: Accurate hashrate measurement

**Deliverable:** Chain 0 hashing with measurable hashrate

---

## Reference Projects

| Project                   | Location                                                     | Focus                   |
| ------------------------- | ------------------------------------------------------------ | ----------------------- |
| bm1397-protocol           | `/home/danielsokil/Lab/GPTechinno/bm1397-protocol`           | Command format          |
| bm13xx-hla                | `/home/danielsokil/Lab/GPTechinno/bm13xx-hla`                | Logic analyzer captures |
| bm13xx-rs                 | `/home/danielsokil/Lab/GPTechinno/bm13xx-rs`                 | Rust implementation     |
| Bitmain_Peek              | `/home/danielsokil/Downloads/Bitmain_Peek/S19_Pro`           | Stock firmware          |
| LiLei_WeChat              | `/home/danielsokil/Downloads/LiLei_WeChat`                   | Additional firmware     |
| bmminer_NBP1901           | `/home/danielsokil/Lab/HashSource/bmminer_NBP1901`           | Binary analysis         |
| cgminer (kanoi)           | `/home/danielsokil/Lab/kanoi/cgminer`                        | Open source             |
| skot/BM1397               | `/home/danielsokil/Lab/skot/BM1397`                          | Protocol reference      |
| bitmaintech               | `/home/danielsokil/Lab/HashSource/bitmaintech`               | Additional reference    |
| bitmain_antminer_binaries | `/home/danielsokil/Lab/HashSource/bitmain_antminer_binaries` | Binary collection       |

---

## ASIC Chip Models (S19 Family)

| Chip Model | Variants                     | Compatible Miners       | Notes        |
| ---------- | ---------------------------- | ----------------------- | ------------ |
| **BM1360** | BM1360BB                     | S19i, S19j Pro          | SHA-256 ASIC |
| **BM1362** | BM1362BD                     | S19j Pro+               | SHA-256 ASIC |
| **BM1398** | BM1398BB, BM1398AC, BM1398AD | S19, S19 Pro, T19, S19a | Most common  |
| **BM1366** | BM1366BS, BM1366BP, BM1366AH | S19K Pro, S19XP         | Newer gen    |

**Typical Configuration:**

- 3 hash chains per miner
- 95-114 chips per chain
- Total: ~285-342 chips per miner

---

## Known Hardware Components

### Working Features

- FPGA register access (`/dev/axi_fpga_dev`)
- Fan control (PWM at 0x084, 0x0A0)
- PSU control (GPIO 907 + I2C at 0x030)
- EEPROM reading (I2C at 0x030, XXTEA decryption)
- Chain detection (HASH_ON_PLUG register at 0x008)

### Not Yet Implemented

- FPGA SPI controller (unknown registers)
- ASIC chip communication protocol
- Work distribution and nonce collection
- Temperature sensors
- Pool integration (Stratum)
- Web UI

---

## Development Workflow

### 1. Protocol Analysis Phase

```bash
# Run agents in parallel to analyze each reference project
# Document findings in docs/ASIC_PROTOCOL.md
```

### 2. SPI Driver Development

```bash
# Create hashsource_x19/src/spi_driver.c
cd hashsource_x19
make
```

### 3. On-Hardware Testing

```bash
# Build and deploy to test machine
make hashsource_x19-rebuild
./scripts/hashsource_ramdisk_update.sh 192.30.1.24

# SSH to test machine
sshpass -p 'root' ssh -o StrictHostKeyChecking=no root@192.30.1.24

# Run chain test
./bin/chain_test 0  # Test chain 0 only
```

### 4. Validation

```bash
# Compare hashrate to stock firmware
# Expected: 50-110 TH/s per chain (model-dependent)
```

---

## Next Steps

1. **Start Phase 1:** Launch parallel agents to analyze all reference projects
2. **Document protocol:** Create `docs/ASIC_PROTOCOL.md` with findings
3. **Identify chip model:** Run `eeprom_detect` on test machine to get chip marking
4. **Begin SPI driver:** Create `spi_driver.c` based on protocol analysis

---

## Success Criteria

- [ ] Chain 0 detects all ASIC chips
- [ ] Chips initialize without errors
- [ ] Work can be sent to chips
- [ ] Nonces are received back
- [ ] Hashrate matches expected performance (±10%)
- [ ] System is stable for 1+ hour continuous operation

---

## Notes

- **Start with chain 0 only** - Get one chain working before scaling to 3
- **Use test vectors** - Known block headers with valid nonces for validation
- **Monitor temperatures** - Implement sensor reading early to prevent damage
- **Compare to stock** - Use stock firmware behavior as ground truth
- **Document everything** - Protocol findings will be valuable for community

---

**Last Updated:** 2025-10-06
