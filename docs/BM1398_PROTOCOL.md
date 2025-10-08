# BM1398 ASIC Protocol Documentation

**Target Hardware**: Antminer S19 Pro (BM1398 ASIC chips)
**Chip Configuration**: 114 chips per chain, 3 chains total
**Baudrate**: 12 MHz (12,000,000 bps)
**Frequency**: 525 MHz
**Voltage**: 1320-1360 mV

---

## Table of Contents

1. [FPGA Register Map](#fpga-register-map)
2. [UART Command Format](#uart-command-format)
3. [ASIC Register Map](#asic-register-map)
4. [Chain Initialization Sequence](#chain-initialization-sequence)
5. [CRC5 Algorithm](#crc5-algorithm)
6. [Chip Enumeration](#chip-enumeration)
7. [Frequency/Baud Configuration](#frequencybaud-configuration)
8. [Work Submission Format](#work-submission-format)
9. [Implementation Notes](#implementation-notes)

---

## FPGA Register Map

FPGA base address: `0x40000000`, size: 5120 bytes (0x1400)

**⚠️ CRITICAL: Indirect Register Mapping (2025-10-07 Discovery)**

Both `bmminer` and `single_board_test` use **indirect register access** via a 110-entry mapping table, NOT direct byte offsets!

**Key indirect mappings:**

- Logical index 0 → word 0 (0x000) - Control register
- Logical index 16 → word 16 (0x040) - Work FIFO first word
- Logical index 17 → word 16 (0x040) - Work FIFO rest (SAME register!)
- Logical index 20 → word 35 (0x08C) - **Timeout register** (NOT 0x014!)
- Logical indices 35, 36, 42 → Work control/enable registers

### Key Registers (direct access - non-mapped)

| Offset      | Name                    | Purpose                                   |
| ----------- | ----------------------- | ----------------------------------------- |
| 0x000       | HARDWARE_VERSION        | FPGA version (0xC501 for S9, TBD for S19) |
| 0x004       | FAN_SPEED               | Fan tachometer reading                    |
| 0x008       | HASH_ON_PLUG            | Chain detection (bit 0-2 for chains 0-2)  |
| 0x00C       | BUFFER_SPACE            | FPGA work buffer space available          |
| 0x010       | RETURN_NONCE            | Nonce FIFO read register                  |
| 0x018       | NONCE_NUMBER_IN_FIFO    | Number of nonces available to read        |
| 0x01C       | NONCE_FIFO_INTERRUPT    | Nonce FIFO interrupt control              |
| 0x020-0x02C | TEMPERATURE_0_15        | Temperature sensor readings               |
| 0x030       | IIC_COMMAND             | I2C controller (PSU, EEPROM)              |
| 0x034       | RESET_HASHBOARD_COMMAND | Chain reset control                       |
| 0x040       | TW_WRITE_COMMAND        | Work data write (S9 legacy)               |
| 0x080       | QN_WRITE_DATA_COMMAND   | Quick nonce write?                        |
| 0x084       | FAN_CONTROL             | PWM fan control                           |
| 0x088       | TIME_OUT_CONTROL        | Timeout configuration                     |
| 0x0C0       | BC_WRITE_COMMAND        | **Broadcast command trigger**             |
| 0x0C4       | BC_COMMAND_BUFFER       | **Command data buffer (12 bytes)**        |
| 0x0F0       | FPGA_CHIP_ID_ADDR       | FPGA chip ID                              |
| 0x0F8       | CRC_ERROR_CNT_ADDR      | CRC error counter                         |

### BC_WRITE_COMMAND Register Bits (0x0C0)

```c
#define BC_COMMAND_BUFFER_READY  (1 << 31)  // Command ready to send
#define BC_COMMAND_EN_CHAIN_ID   (1 << 23)  // Enable chain ID field
#define BC_COMMAND_EN_NULL_WORK  (1 << 22)  // Enable null work mode
#define CHAIN_ID(id)             ((id) << 16) // Chain selection (0-2)
```

### RETURN_NONCE Register Bits (0x010)

```c
#define WORK_ID_OR_CRC           (1 << 31)  // Response type indicator
#define WORK_ID_OR_CRC_VALUE(v)  (((v) >> 16) & 0x7FFF)
#define NONCE_INDICATOR          (1 << 7)   // Valid nonce present
#define CHAIN_NUMBER(v)          ((v) & 0xF) // Source chain ID
#define REGISTER_DATA_CRC(v)     (((v) >> 24) & 0x7F)
```

---

## UART Command Format

### 9-Byte Register Write Command

```c
struct uart_cmd_write {
    uint8_t preamble;    // 0x41=unicast, 0x51=broadcast
    uint8_t length;      // 0x09 (9 bytes)
    uint8_t chip_addr;   // Chip address (0-255)
    uint8_t reg_addr;    // Register address
    uint32_t value;      // Register value (big-endian)
    uint8_t crc5;        // CRC5 (lower 5 bits)
};
```

**Example**: Write 0x80008700 to register 0x3C on all chips:

```
0x51 0x09 0x00 0x3C 0x80 0x00 0x87 0x00 [CRC5]
```

### 5-Byte Address/Read Command

```c
struct uart_cmd_addr {
    uint8_t preamble;    // 0x40
    uint8_t length;      // 0x05 (5 bytes)
    uint8_t chip_addr;   // Chip address to assign
    uint8_t reserved;    // 0x00
    uint8_t crc5;        // CRC5 (lower 5 bits)
};
```

**Example**: Assign address 0 to first chip:

```
0x40 0x05 0x00 0x00 [CRC5]
```

### Command Preambles

- `0x40` - Set chip address
- `0x41` - Write register (unicast)
- `0x51` - Write register (broadcast to all chips)
- `0x42` - Read register (unicast) [rarely used]
- `0x52` - Read register (broadcast) [rarely used]
- `0x53` - Chain inactive (stop relay)

---

## ASIC Register Map

### BM1398 Registers (from Bitmain test fixture analysis)

| Register | Name                 | Purpose                                      |
| -------- | -------------------- | -------------------------------------------- |
| 0x00     | CHIP_ADDR            | Chip address (read for verification)         |
| 0x08     | PLL_PARAM_0          | PLL 0 configuration                          |
| 0x10     | HASH_COUNTING_NUMBER | Hash counter config                          |
| 0x14     | TICKET_MASK          | Core enable mask / difficulty                |
| 0x18     | CLK_CTRL             | Clock control (baud, CLK_EN, PLL_RST)        |
| 0x1C     | WORK_ROLLING         | Work rolling configuration                   |
| 0x20     | WORK_CONFIG          | Work config (midstate mode)                  |
| 0x28     | BAUD_CONFIG          | High-speed baud rate config (>3MHz)          |
| 0x34     | RESET_CTRL           | Reset control (RST_N)                        |
| 0x3C     | CORE_CONFIG          | **Core configuration (pulse_mode, clk_sel)** |
| 0x44     | CORE_PARAM           | Core timing parameters                       |
| 0x54     | DIODE_MUX            | Diode_Vdd_Mux_Sel (voltage monitoring)       |
| 0x58     | UNKNOWN_58           | Unknown register                             |
| 0x60     | PLL_PARAM_1          | PLL 1 configuration                          |
| 0x64     | PLL_PARAM_2          | PLL 2 configuration                          |
| 0x68     | PLL_PARAM_3          | PLL 3 configuration (UART clock)             |
| 0x80     | UNKNOWN_80           | Unknown register                             |
| 0xA4     | VERSION_ROLLING      | Version rolling (AsicBoost)                  |

### Core Configuration Register (0x3C)

```c
// BM1398 core config value
#define CORE_CONFIG_BASE  0x80008700
#define PULSE_MODE_SHIFT  4
#define CLK_SEL_MASK      0x7

uint32_t core_config = CORE_CONFIG_BASE |
                       ((pulse_mode & 3) << PULSE_MODE_SHIFT) |
                       (clk_sel & CLK_SEL_MASK);
```

**Configuration from Config.ini**:

- `pulse_mode = 1`
- `clk_sel = 0`
- `CCdly_Sel = 1`
- `Pwth_Sel = 1`

### Ticket Mask Register (0x14)

Controls core enable and difficulty filtering:

- `0xFFFFFFFF` = All cores enabled (initialization)
- `0x000000FF` = 256 cores enabled (normal operation)

---

## Chain Initialization Sequence

### Stage 1: Hardware Reset (from Bitmain single_board_test.c)

```c
void reset_chain_stage_1(int chain) {
    // 1. Disable CLK_EN (reg 0x18, bit 2 = 0)
    write_register(chain, broadcast, 0, 0x18, value & ~(1<<2));
    usleep(10000);

    // 2. Disable RST_N (reg 0x34, bit 3 = 0)
    write_register(chain, broadcast, 0, 0x34, value & ~(1<<3));
    usleep(10000);

    // 3. Assert PLL_RST (reg 0x18, bit 18 = 1)
    write_register(chain, broadcast, 0, 0x18, value | (1<<18));
    usleep(10000);

    // 4. Deassert PLL_RST (reg 0x18, bit 18 = 0)
    write_register(chain, broadcast, 0, 0x18, value & ~(1<<18));
    usleep(10000);

    // 5. Enable CLK_EN (reg 0x18, bit 2 = 1)
    write_register(chain, broadcast, 0, 0x18, value | (1<<2));
    usleep(10000);

    // 6. Enable RST_N (reg 0x34, bit 3 = 1)
    write_register(chain, broadcast, 0, 0x34, value | (1<<3));
    usleep(10000);

    // 7. Set ticket mask to all cores
    write_register(chain, broadcast, 0, 0x14, 0xFFFFFFFF);
    usleep(10000);
}
```

### Stage 2: Configuration

```c
void configure_chain_stage_2(int chain, uint8_t diode_vdd_mux_sel) {
    // 1. Set diode mux selector
    write_register(chain, broadcast, 0, 0x54, diode_vdd_mux_sel);
    usleep(10000);

    // 2. Chain inactive (stop relay)
    send_chain_inactive(chain);
    usleep(10000);

    // 3. Enumerate chips (assign addresses)
    enumerate_chips(chain, 114);  // 114 chips
    usleep(10000);

    // 4. Set core configuration
    uint32_t core_cfg = 0x80008700 | ((1 & 3) << 4) | (0 & 7);
    write_register(chain, broadcast, 0, 0x3C, core_cfg);
    usleep(10000);

    // 5. Set timing parameters (pwth_sel=1, ccdly_sel=1, swpf_mode=0)
    // Register unknown - need more analysis
    usleep(10000);

    // 6. Set PLL dividers to 0
    write_register(chain, broadcast, 0, 0x08, 0x00000000);
    write_register(chain, broadcast, 0, 0x60, 0x00000000);
    write_register(chain, broadcast, 0, 0x64, 0x00000000);
    write_register(chain, broadcast, 0, 0x68, 0x00000000);
    usleep(10000);

    // 7. Set frequency (525 MHz)
    set_chain_frequency(chain, 525);
    usleep(10000);

    // 8. Set baud rate (12 MHz)
    set_baud_rate(chain, 12000000);
    usleep(50000);

    // 9. Set final ticket mask
    write_register(chain, broadcast, 0, 0x14, 0x000000FF);
    usleep(10000);
}
```

---

## CRC5 Algorithm

```c
/**
 * Calculate CRC5 for BM13xx UART commands
 * Polynomial: Custom 5-bit CRC
 * Input: Command bytes (without CRC byte)
 * Input bits: Number of bits to process (usually 32 or 64)
 * Returns: 5-bit CRC value (0-31)
 */
uint8_t crc5(const uint8_t *data, unsigned int bits) {
    uint8_t crc = 0x1F;  // Initial value

    for (unsigned int i = 0; i < bits; i++) {
        uint8_t bit = (data[i / 8] >> (7 - (i % 8))) & 1;
        if ((crc & 0x10) != (bit << 4)) {
            crc = ((crc << 1) | bit) ^ 0x05;
        } else {
            crc = (crc << 1) | bit;
        }
        crc &= 0x1F;
    }

    return crc;
}
```

**Usage**:

- 5-byte address command: `crc5(cmd, 32)` (4 bytes = 32 bits)
- 9-byte write command: `crc5(cmd, 64)` (8 bytes = 64 bits)

---

## Chip Enumeration

```c
/**
 * Enumerate and address chips on chain
 * S19 Pro: 114 chips, interval = 256/114 ≈ 2.2 → use 2
 */
void enumerate_chips(int chain, int num_chips) {
    // Calculate address interval
    int interval = 256 / num_chips;  // Usually 2 for 114 chips

    // Send chain inactive first (stop relay)
    send_chain_inactive(chain);
    usleep(10000);

    // Assign addresses sequentially
    for (int i = 0; i < num_chips; i++) {
        uint8_t addr = i * interval;
        send_chip_address_cmd(chain, addr);
        usleep(1000);  // 1ms between chips
    }
}

void send_chip_address_cmd(int chain, uint8_t addr) {
    uint8_t cmd[5];
    cmd[0] = 0x40;        // Address command
    cmd[1] = 0x05;        // 5 bytes
    cmd[2] = addr;        // Chip address
    cmd[3] = 0x00;        // Reserved
    cmd[4] = crc5(cmd, 32);

    send_uart_cmd(chain, cmd, 5);
}

void send_chain_inactive(int chain) {
    uint8_t cmd[5];
    cmd[0] = 0x53;        // Chain inactive
    cmd[1] = 0x05;
    cmd[2] = 0x00;
    cmd[3] = 0x00;
    cmd[4] = crc5(cmd, 32);

    send_uart_cmd(chain, cmd, 5);
}
```

---

## Frequency/Baud Configuration

### Baud Rate Configuration (12 MHz)

```c
/**
 * Set UART baud rate
 * Standard mode: <= 3 MHz (base clock = 25 MHz)
 * High-speed mode: > 3 MHz (base clock = 400 MHz via PLL3)
 */
void set_baud_rate(int chain, uint32_t baud_rate) {
    uint32_t baud_div;

    if (baud_rate > 3000000) {
        // High-speed mode (12 MHz for BM1398)
        // Configure PLL3 register (0x68)
        write_register(chain, broadcast, 0, 0x68, 0xC0700111);
        usleep(10000);

        // Configure baud config register (0x28)
        write_register(chain, broadcast, 0, 0x28, 0x06008F00);
        usleep(10000);

        // Calculate divisor: 400MHz / (baud * 8)
        baud_div = (400000000 / (baud_rate * 8)) - 1;
    } else {
        // Standard mode
        baud_div = (25000000 / (baud_rate * 8)) - 1;
    }

    // Write divisor to CLK_CTRL register (0x18)
    // Bits [11:8] = upper 4 bits, bits [4:0] = lower 5 bits
    uint32_t clk_ctrl = read_register_cached(chain, 0, 0x18);
    clk_ctrl = (clk_ctrl & 0xF0FF) | ((baud_div >> 5) << 8);
    clk_ctrl = (clk_ctrl & 0xFFE0) | (baud_div & 0x1F);
    clk_ctrl |= (1 << 16);  // Set bit 16
    write_register(chain, broadcast, 0, 0x18, clk_ctrl);
    usleep(50000);
}
```

**For 12 MHz**:

- High-speed mode enabled
- baud_div = (400,000,000 / (12,000,000 \* 8)) - 1 = 3.16 → 3
- Actual baud = 400MHz / (4 \* 8) = 12.5 MHz (close enough)

### Frequency Configuration (525 MHz)

**IMPLEMENTED (2025-10-07)** - Complete PLL configuration formula:

```c
/**
 * Set ASIC core frequency
 * Target: 525 MHz for BM1398
 *
 * PLL Formula: freq = CLKI * fbdiv / (refdiv * (postdiv1+1) * (postdiv2+1))
 * Where: CLKI = 25 MHz (crystal oscillator)
 * VCO range: 1600-3200 MHz
 */
void set_chain_frequency(int chain, uint32_t freq_mhz) {
    uint8_t refdiv, postdiv1, postdiv2;
    uint16_t fbdiv;

    // For 525 MHz:
    // VCO = 25 * 84 = 2100 MHz (within 1600-3200 MHz range)
    // freq = 2100 / ((1+1) * (1+1)) = 525 MHz
    if (freq_mhz == 525) {
        refdiv = 1;
        fbdiv = 84;
        postdiv1 = 1;  // Actual divider = postdiv1 + 1 = 2
        postdiv2 = 1;  // Actual divider = postdiv2 + 1 = 2
    }

    // Calculate VCO for range validation
    float vco = 25.0f / refdiv * fbdiv;
    if (vco < 1600.0f || vco > 3200.0f) {
        // Error: VCO out of range
        return;
    }

    // Build PLL register value
    // Bits [31:30] = VCO range (00=low, 01=high)
    // Bit  [30] = 0x40000000 base value
    // Bits [27:16] = fbdiv (12 bits)
    // Bits [13:8] = postdiv1 (6 bits)
    // Bits [6:4] = refdiv - 1 (3 bits)
    // Bits [2:0] = postdiv2 - 1 (3 bits)
    uint32_t pll0 = 0x40000000 |
                    ((postdiv2 - 1) & 0x7) |
                    (((refdiv - 1) & 0x7) << 4) |
                    ((postdiv1 & 0x3f) << 8) |
                    ((fbdiv & 0xfff) << 16);

    // Set VCO range bit for 2400-3200 MHz
    if (vco >= 2400.0f && vco <= 3200.0f) {
        pll0 |= 0x10000000;
    }

    // For 525 MHz: pll0 = 0x40540100
    // VCO = 2100 MHz (mid-range)

    // Write PLL0 parameter to register 0x08 (broadcast to all chips)
    write_register(chain, broadcast, 0, 0x08, pll0);
    usleep(10000);  // Wait for PLL to stabilize
}
```

**Verified Values (525 MHz):**

- Register value: `0x40540100`
- refdiv: 1
- fbdiv: 84
- postdiv1: 1 (÷2)
- postdiv2: 1 (÷2)
- VCO: 2100 MHz
- Final frequency: 25 × 84 / (1 × 2 × 2) = 525 MHz

**Implementation:** `hashsource_x19/src/bm1398_asic.c:609-673`

---

## Work Submission Format

### 4-Midstate Work Packet (148 bytes = 0x94)

**VERIFIED from factory test binary analysis (sub_22B10):**

```c
struct work_packet_4mid {
    uint8_t work_type;       // [0] = 0x01
    uint8_t chain_id;        // [1] = chain | 0x80
    uint8_t reserved[2];     // [2-3] = 0x00
    uint32_t work_id;        // [4-7] = Big-endian work ID
    uint8_t work_data[12];   // [8-19] = Last 12 bytes of block header
    uint8_t midstate[4][32]; // [20-147] = 4x 32-byte SHA256 midstates
};  // Total: 148 bytes (0x94)

void send_work_4midstate(int chain, uint32_t work_id,
                         const uint8_t *work_data_12bytes,
                         const uint8_t midstates[4][32]) {
    struct work_packet_4mid work;
    memset(&work, 0, sizeof(work));

    // Build packet header
    work.work_type = 0x01;
    work.chain_id = chain | 0x80;
    work.reserved[0] = 0x00;
    work.reserved[1] = 0x00;
    work.work_id = __builtin_bswap32(work_id);  // Convert to big-endian

    // Copy work data (12 bytes from pattern offset 15)
    memcpy(work.work_data, work_data_12bytes, 12);

    // Copy 4 midstates (from pattern offset 27, 32 bytes each)
    for (int i = 0; i < 4; i++) {
        memcpy(work.midstate[i], midstates[i], 32);
    }

    // Byte-swap all 32-bit words in packet
    // Factory test swaps from offset after headers to end
    uint32_t *words = (uint32_t *)&work;
    for (int i = 0; i < sizeof(work)/4; i++) {
        words[i] = __builtin_bswap32(words[i]);
    }

    // Send via FPGA indirect register mapping
    // Logical index 16 for first word, index 17 for remaining words
    // Both map to physical register 0x040 (FIFO writes)
    send_work_via_indirect_mapping(chain, &work, sizeof(work));
}
```

**Work Packet Construction (verified):**

1. Clear 148-byte buffer
2. Set header: type=0x01, chain_id=(chain|0x80)
3. Set work_id (byte-swapped to big-endian)
4. Copy 12 bytes work_data from pattern[15:27]
5. Copy 4× 32 bytes midstates from pattern[27:59] (same midstate for all 4 slots in pattern test)
6. Byte-swap all 32-bit words in the entire packet
7. Send to FPGA using indirect register writes (indices 16/17 → register 0x040)

---

## Implementation Notes

### FPGA UART Interface

The FPGA acts as a UART bridge between ARM CPU and ASIC chips. Two methods observed:

**Method 1: BC_COMMAND_BUFFER (S19 likely uses this)**

```c
void send_uart_cmd_bc(int chain, const uint8_t *cmd, size_t len) {
    // Write command bytes to BC_COMMAND_BUFFER (0xC4-0xCF, 3 x 32-bit regs)
    uint32_t *regs = (uint32_t *)fpga_base;

    for (size_t i = 0; i < (len + 3) / 4; i++) {
        uint32_t word = 0;
        memcpy(&word, &cmd[i*4], min(4, len - i*4));
        regs[0xC4/4 + i] = word;
    }

    // Trigger command with BC_WRITE_COMMAND
    uint32_t trigger = BC_COMMAND_BUFFER_READY | CHAIN_ID(chain);
    regs[0xC0/4] = trigger;

    // Wait for completion (bit 31 clears)
    while (regs[0xC0/4] & BC_COMMAND_BUFFER_READY) {
        usleep(10);
    }
}
```

**Method 2: Direct register access (S9 legacy)**

- Uses TW_WRITE_COMMAND (0x40) for work data
- May not be used in S19

### Nonce Reading

```c
struct nonce_response {
    uint32_t nonce;          // Nonce value
    uint8_t chip_id;         // Chip that found it
    uint8_t core_id;         // Core within chip
    uint16_t nonce_count;    // Total nonces from chip
};

int read_nonces(struct nonce_response *nonces, int max_count) {
    uint32_t *regs = (uint32_t *)fpga_base;

    // Check NONCE_NUMBER_IN_FIFO (0x18)
    int available = regs[0x18/4];
    if (available == 0) return 0;

    int count = min(available, max_count);

    for (int i = 0; i < count; i++) {
        uint32_t data = regs[0x10/4];  // RETURN_NONCE

        if (data & NONCE_INDICATOR) {
            nonces[i].nonce = WORK_ID_OR_CRC_VALUE(data) << 16;
            nonces[i].nonce |= (data & 0xFFFF);
            nonces[i].chip_id = CHAIN_NUMBER(data);
            // Additional parsing needed
        }
    }

    return count;
}
```

### Implementation Status

**COMPLETED (2025-10-07):**

1. [x] Protocol analysis complete
2. [x] CRC5 function implemented and verified (0 CRC errors across 342 chips)
3. [x] FPGA UART driver (BC_COMMAND_BUFFER method) working
4. [x] Chain initialization sequence complete (114/114 chips per chain)
5. [x] Chip enumeration tested on real hardware (2 machines, 3 chains each)
6. [x] Work submission implemented (80 patterns sent successfully)
7. [x] Nonce collection infrastructure complete
8. [x] PSU power control (15V, APW12 V2 protocol)
9. [x] PIC DC-DC converter enable (FPGA I2C)
10. [x] PLL frequency configuration (525 MHz, VCO=2100 MHz)
11. [x] Baud rate configuration (12 MHz high-speed mode)

**Current Status:**

All hardware initialization verified working:

- Chain detection: 3 chains per machine
- Chip enumeration: 114/114 chips per chain, 0 CRC errors
- PSU: 15V enabled via GPIO 907 and I2C
- PIC DC-DC: Enabled via FPGA I2C (response: 0x15 0x01)
- PLL: 525 MHz configured (register: 0x40540100, VCO: 2100 MHz)
- Baud: 12 MHz high-speed UART
- Work: 80 test patterns sent and accepted by FPGA FIFO

**Completed Register Configurations (2025-10-07 Evening):**

12. **Core Timing Parameters (Register 0x44)** ✓

    - pwth_sel = 1 (bits 3-5)
    - ccdly_sel = 0 (bits 6+)
    - swpf_mode = 0 (bit 0)
    - Value: 0x00000008
    - From: Config.ini Pwth_Sel, CCdly_Sel parameters

13. **IO Driver Configuration (Register 0x58)** ✓

    - clko_ds = 1 (clock output driver strength)
    - Bits [7:4] = 0x1
    - Value: 0x00000010
    - From: Factory test set_chain_iodriver_clko_ds()

14. **Core Reset Sequence (Post-Baud Rate)** ✓

    - Per-chip sequence after baud rate configuration:
    - Step 1: Soft reset (reg 0xA8 = 0x1F0)
    - Step 2: CLK_CTRL (reg 0x18 = 0xF0000000)
    - Step 3: Re-config clock (reg 0x3C, pulse_mode=1, clk_sel=0)
    - Step 4: Re-config timing (reg 0x44)
    - Step 5: Core enable (reg 0x3C = 0x800082AA)
    - From: Factory test do_core_reset() function

15. **FPGA Nonce Timeout (FPGA Register 0x14)** ✓

    - Formula: timeout = 0x1FFFF / freq_mhz
    - For 525 MHz: timeout = 249
    - Register value: 0x800000F9
    - From: Factory test dhash_set_timeout()

16. **Nonce Overflow Control (Register 0x3C)** ✓
    - Final write after timeout configuration
    - Value: 0x80008D15 (nonce overflow disabled)
    - From: Factory test set_chain_core_nonce_overflow_control()

**Current Issue:**

Despite implementing **ALL** factory test initialization steps (16 complete configurations), ASICs still not returning nonces. Test results:

- Initialization: [OK] 114/114 chips enumerated, 0 CRC errors
- PSU Power: [OK] 15V enabled
- PIC DC-DC: [OK] Enabled (response: 0x15 0x01)
- PLL Frequency: [OK] 525 MHz (0x40540100, VCO 2100 MHz)
- Baud Rate: [OK] 12 MHz configured
- Core Timing: [OK] Register 0x44 = 0x00000008
- IO Driver: [OK] Register 0x58 = 0x00000010
- Core Reset: [OK] Sequence completed (all 114 chips)
- FPGA Timeout: [OK] Register 0x14 = 0x800000F9
- Nonce Overflow: [OK] Register 0x3C = 0x80008D15
- Work Submission: [OK] 80 patterns sent
- Nonces Received: [FAIL] 0/80 (60-second timeout)

**Remaining Investigation Areas:**

1. Voltage level (currently 15V startup; may need 12.6-12.8V operational)
2. Pattern file version compatibility (may be chip-variant-specific)
3. Undocumented FPGA registers or timing requirements
4. Work packet format subtleties not visible in decompiled code
5. PLL/core stabilization timing requirements

**Implementation Files:**

- Driver: `hashsource_x19/src/bm1398_asic.c` (lines 400-600 for new configs)
- Header: `hashsource_x19/include/bm1398_asic.h` (new register definitions)
- Pattern test: `hashsource_x19/src/pattern_test.c`
- Chain test: `hashsource_x19/src/chain_test.c`
- Work test: `hashsource_x19/src/work_test.c`

---

**References**:

- Bitmain_Test_Fixtures S19_Pro single_board_test.c (decompiled)
  - pt_before_send_nonce() - Main pattern test sequence
  - set_register_stage_1/2/3() - Initialization stages
  - do_core_reset() - Post-baud rate reset sequence
  - dhash_set_timeout() - FPGA timeout configuration
  - set_clock_delay_control() - Core timing parameters
- Bitmain_Peek S19_Pro bmminer from Stock Firmware, decompiled
- bitmaintech bmminer-mix driver-btm-c5.h/c (S9 source)
- Config.ini: BM1398 test configuration (Pwth_Sel=1, CCdly_Sel=0)

**Last Updated**: 2025-10-07 Evening - All factory test configurations implemented
