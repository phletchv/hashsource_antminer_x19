# PSU Protocol Documentation

**Last Updated:** 2025-10-02
**Hardware Tested:** Antminer S19 Pro (PSU version 0x71)

## Critical Finding: Two Protocol Variants

The APW12 PSU implements **two different I2C communication protocols** depending on firmware version. S19 Pro hardware reports version `0x71`, requiring the **Legacy Protocol**.

## Hardware Interface

| Parameter            | Value                                                        |
| -------------------- | ------------------------------------------------------------ |
| I2C Controller       | FPGA-based (NOT PS I2C)                                      |
| I2C Address          | 0x10 (7-bit), encoded as master=1, slave_high=2, slave_low=0 |
| FPGA Base Address    | 0x40000000                                                   |
| FPGA I2C Register    | Offset 0x30 (word offset 0x0C)                               |
| Protocol Detection   | Register 0x00 = 0xF5                                         |
| Enable Control       | GPIO 907 (0x38b) active-low (0=ON, 1=OFF)                    |
| OUT1 (Hashboards)    | 12-15V adjustable, 240A @ 15V, 300A @ 12V                    |
| OUT2 (Control Board) | 12.3V fixed, 15A                                             |

**Critical Architecture Detail:** PSU communication does NOT use PS I2C controller (`/dev/i2c-0`). Stock firmware communicates through FPGA memory-mapped I2C controller at register 0xC.

**FPGA I2C Sources:**

- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/i2c_write_reg-001ca944.c` - FPGA I2C write (lines 35-50: `fpga_write(0xc, val)`)
- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/i2c_read_reg-001ca78c.c` - FPGA I2C read (lines 39-61: `fpga_write(0xc, val)`)
- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/wait4i2c_ready-001ca2ec.c` - Wait for I2C ready (bit 31)
- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/wait4i2c_data-001ca324.c` - Wait for data ready (bits 31-30 = 0b10)

**GPIO Control Functions:**

- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_power_on-001c2a70.c`
- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_power_off-001c2a9c.c`

### FPGA I2C Controller Register (0x0C)

**Register Layout (32-bit word at offset 0x30):**

| Bits  | Function                            | Source               |
| ----- | ----------------------------------- | -------------------- |
| 31    | Ready (1=ready for operation)       | wait4i2c_ready.c:14  |
| 31-30 | Status (0b10 = data ready)          | wait4i2c_data.c:14   |
| 26    | Master address bit 1                | i2c_write_reg.c:35   |
| 25    | Master address bit 0 / Read flag    | i2c_read_reg.c:41-42 |
| 24    | Register address valid              | i2c_write_reg.c:38   |
| 20    | Slave address high nibble           | i2c_write_reg.c:35   |
| 19    | 1-byte read flag (for reads)        | i2c_read_reg.c:42    |
| 16    | Slave address low nibble            | i2c_write_reg.c:35   |
| 15-8  | PSU register address (0x00 or 0x11) | i2c_write_reg.c:38   |
| 7-0   | Data byte                           | i2c_write_reg.c:36   |

**I2C Address Encoding:** 7-bit address 0x10 encoded as `(master=1 << 26) | (slave_high=2 << 20) | (slave_low=0 << 16)`

Reference: `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_power_open-001c81cc.c` lines 49-52

## Protocol Version Detection

**Source:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/is_power_protocal_v2-001c8020.c`

```c
bool is_power_protocal_v2(void) {
    uint version = power_version;

    // V2: 0x62, 0x64-0x66
    if (version == 0x62 || (version >= 0x64 && version <= 0x66))
        return true;

    // Legacy: all others
    return false;
}
```

| PSU Version            | Protocol | I2C Register | Used In            |
| ---------------------- | -------- | ------------ | ------------------ |
| 0x71, 0x72, 0x75, 0x77 | Legacy   | 0x00         | S19 Pro, S19       |
| 0x73, 0x74, 0x76, 0x78 | Legacy   | 0x00         | Various            |
| 0x62, 0x64, 0x65, 0x66 | V2       | 0x11         | S17, S17 Pro, S17+ |

## Legacy Protocol (S19 Pro v0x71)

### I2C Communication Method

Commands are sent/received **byte-by-byte** to/from I2C register 0x00, not as complete packets.

**Source:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/exec_power_cmd-001c7244.c`

```c
int exec_power_cmd(int fd, uint8_t *send_data, uint send_len,
                   uint8_t *read_data, uint read_len)
{
    uint8_t reg_addr = 0x00;  // Legacy protocol register

    pthread_mutex_lock(&power_mutex);

    for (retry = 0; retry < 3; retry++) {
        // Write byte-by-byte
        for (i = 0; i < send_len; i++)
            iic_write_reg(fd, &reg_addr, 1, &send_data[i], 1, true);

        usleep(400000);  // 400ms delay

        // Read byte-by-byte
        for (i = 0; i < read_len; i++)
            iic_read_reg(fd, &reg_addr, 1, &read_data[i], 1, true);

        usleep(100000);  // 100ms settling

        if (check_read_back_data(send_data, read_data, read_len) == 0)
            break;
    }

    pthread_mutex_unlock(&power_mutex);
    return 0;
}
```

**Correct kernel implementation using FPGA I2C:**

```c
// Write command byte-by-byte via FPGA
for (i = 0; i < packet_len; i++) {
    // Wait for FPGA I2C ready (bit 31 = 1)
    while (!(readl(fpga_base + 0x30) & BIT(31)))
        usleep_range(5000, 5500);

    // Build command: master | slave_addr | reg_valid | reg | data
    cmd = (1 << 26) | (2 << 20) | (0 << 16) | BIT(24) | (0x00 << 8) | packet[i];
    writel(cmd, fpga_base + 0x30);

    // Wait for completion (bits 31-30 = 0b10)
    while (((readl(fpga_base + 0x30) >> 30) & 0x3) != 2)
        usleep_range(5000, 5500);
}

msleep(400);  // PSU processing time

// Read response byte-by-byte via FPGA
for (i = 0; i < resp_len; i++) {
    while (!(readl(fpga_base + 0x30) & BIT(31)))
        usleep_range(5000, 5500);

    // Build read command: read_op | 1byte_flag | addressing
    cmd = (1 << 26) | BIT(25) | BIT(19) | (2 << 20) | (0 << 16) | BIT(24) | (0x00 << 8);
    writel(cmd, fpga_base + 0x30);

    while (((readl(fpga_base + 0x30) >> 30) & 0x3) != 2)
        usleep_range(5000, 5500);

    response[i] = readl(fpga_base + 0x30) & 0xFF;
}

msleep(100);  // Settling time
```

**Driver Implementation:** `/home/danielsokil/Lab/HashSource/hashsource_antminer_s19pro/s19pro-firmware/packages/s19-kernel-modules/apw12_psu.c` lines 177-259

### V2 Protocol (S17 variants)

**Source:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/exec_power_cmd_v2-001c7310.c`

Identical byte-by-byte approach but uses register **0x11** and 500ms delay.

## Packet Format

All commands share this structure:

```
[0x55] [0xAA] [LEN] [CMD] [DATA...] [CRC_LSB] [CRC_MSB]
```

- Magic: 0x55 0xAA (little-endian: 0xAA55)
- LEN: Header + data length (excludes checksum)
- CRC: 16-bit sum of all bytes from LEN to end of DATA

## Commands

| Code | Function       | Request Size | Response Size                       |
| ---- | -------------- | ------------ | ----------------------------------- |
| 0x01 | Get FW Version | 6 bytes      | 8+ bytes (2-byte version or string) |
| 0x02 | Get PSU Type   | 6 bytes      | 8 bytes (2-byte type)               |
| 0x03 | Get Voltage    | 6 bytes      | 8 bytes (2-byte N value)            |
| 0x83 | Set Voltage    | 8 bytes      | 8 bytes (echo)                      |

### Example - Get PSU Type (v0x71)

**Source:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_power_version-001c806c.c`

```
Request:  55 AA 04 02 06 00
Response: 55 AA 06 02 71 00 79 00

Decoded:
  Type = 0x0071 (PSU version for S19 Pro)
  Checksum = 0x0079 (0x06 + 0x02 + 0x71 = 0x79)
```

## Voltage Conversion Formulas

Formulas are **version-specific** and extracted from stock firmware.

**Source:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_convert_V_to_N-001c92c4.c` and `bitmain_convert_N_to_V_furmula-001c7c38.c`

### Version 0x71, 0x72, 0x75, 0x77 (S19 Pro)

```c
// Voltage to N (for setting)
int N = (int)(1190.935338 - voltage_v * 78.742588);

// N to voltage (for reading)
double voltage_v = (1190.935338 - N) / 78.742588;
```

**Examples:**

- 15.00V → N = 9
- 13.00V → N = 167
- 12.00V → N = 246

**Valid range:** N ≈ 9 to 246

### Version 0x73, 0x78

```c
int N = (int)(1280.577821 - voltage_v * 73.979365);
double voltage_v = (1280.577821 - N) / 73.979365;
```

### Version 0x74, 0x76

```c
int N = (int)(1156.107585 - voltage_v * 76.090494);
double voltage_v = (1156.107585 - N) / 76.090494;
```

### Version 0xc1 (Newer PSU with FW version check)

```c
// If fw_version < 4:
int N = (int)(1275.0 - voltage_v * 85.0);
double voltage_v = (1275.0 - N) / 85.0;

// If fw_version >= 4:
int N = (int)(1083.75 - voltage_v * 70.83333333333);
double voltage_v = (1083.75 - N) / 70.83333333333;
```

### Version 0x61

```c
int N = (int)(1144.502262 - voltage_v * 52.243589);
double voltage_v = (1144.502262 - N) / 52.243589;
```

### Version 0x41, 0x42

```c
int N = (int)(765.411764 - voltage_v * 35.833333);
double voltage_v = (765.411764 - N) / 35.833333;
```

### Version 0x43

```c
int N = (int)(933.240365 - voltage_v * 59.806034);
double voltage_v = (933.240365 - N) / 59.806034;
```

### Version 0x22

```c
int N = (int)(1215.89444 - voltage_v * 59.931507);
double voltage_v = (1215.89444 - N) / 59.931507;
```

### Version 0x62, 0x64-0x66 (S17)

**Special case:** These versions send/receive voltage as **IEEE 754 float** (4 bytes), not as N value.

```c
// Set voltage: bytes 4-7 contain float
*(float*)&packet[4] = target_voltage;

// Get voltage: bytes 4-7 contain float
voltage = *(float*)&response[4];
```

### S17 Protocol Formula (from PDF)

For reference only (not used by APW12):

```c
int N = (int)((21360.0 - voltage_mv) * 253.0 / 7060.0);
double voltage_mv = 21360.0 - (N * 7060.0 / 253.0);
```

Range: 14.25V - 21.36V (N=0 to N=255)

## Initialization Sequence

**Source:** Boot log `/home/danielsokil/Lab/HashSource/hashsource_antminer_s19pro/docs/bmminer_s19pro.log`

```
20:39:05 FPGA Version = 0xB031
20:39:24 power type version: 0x0071
20:39:24 Enter sleep to make sure power release finish.
20:39:55 Slept 30 seconds, diff = 0.
20:39:55 set_voltage_by_steps to 1500.
```

**Implementation:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_power_open-001c81cc.c`

**Recommended sequence:**

1. Map FPGA memory region at 0x40000000
2. Initialize FPGA I2C controller access (register offset 0x30)
3. Configure I2C addressing: master=1, slave_high=2, slave_low=0 (creates 7-bit address 0x10)
4. Enable PSU via GPIO 907 (active-low, write 0 to enable)
5. Wait 500ms for PSU stabilization
6. Detect protocol version (write 0xF5 to register 0x00 or 0x11, read back)
7. Read PSU version (command 0x02) - determines voltage formula
8. Read PSU calibration data (command 0x20, 0x06 or 0x40) if available
9. Wait 30 seconds for capacitor discharge (first power-on only)
10. Set voltage to 15.0V (command 0x83, N=9 for v0x71)
11. Wait 500ms for voltage stabilization
12. Verify voltage (command 0x03)

## Critical Timing Requirements

| Operation           | Delay                        | Source                |
| ------------------- | ---------------------------- | --------------------- |
| After write command | 400ms (legacy) or 500ms (V2) | exec_power_cmd.c      |
| After read response | 100ms                        | exec_power_cmd.c      |
| After voltage set   | 500ms                        | bitmain_set_voltage.c |
| After PSU enable    | 500ms                        | Best practice         |
| First power-on      | 30 seconds                   | bmminer boot log      |

## Driver Implementation Status

### Fixed Issues (as of 2025-10-02)

1. **FPGA I2C Architecture** - Driver now uses FPGA memory-mapped I2C controller instead of PS I2C

   - Platform driver mapping FPGA region 0x40000000-0x40001200
   - Direct register access to FPGA I2C controller at offset 0x30
   - Implementation: `/home/danielsokil/Lab/HashSource/hashsource_antminer_s19pro/s19pro-firmware/packages/s19-kernel-modules/apw12_psu.c`

2. **Byte-by-byte Communication** - Implemented per stock firmware

   - `fpga_i2c_write_byte()` and `fpga_i2c_read_byte()` functions
   - Proper wait states for FPGA I2C ready and data ready

3. **Correct Voltage Formula for v0x71** - Fixed-point implementation

   - `N = (1190935338 - voltage_mv * 78743) / 1000000`
   - Uses `do_div()` for 32-bit ARM compatibility

4. **Protocol Version Detection** - Implemented legacy (0x00) vs V2 (0x11) detection

5. **Proper Timing** - 400ms post-send, 100ms post-read, 500ms voltage settling

### Remaining Work

1. **GPIO 907 Mapping** - Stock uses FPGA GPIO controller (not PS GPIO)

   - Current: Uses PS GPIO 54 as placeholder
   - Required: Implement FPGA GPIO controller driver or map GPIO 907
   - See device tree comments: `/home/danielsokil/Lab/HashSource/hashsource_antminer_s19pro/s19pro-firmware/board/s19pro/devicetree.dts` lines 426-430

2. **Additional PSU Versions** - Only v0x71 formula implemented

   - Need formulas for v0x62, v0x64-66, v0x73-78, v0xc1, etc.
   - Reference implementations in `bitmain_convert_V_to_N-001c92c4.c`

3. **Calibration Data Support** - Factory calibration not yet parsed
   - Commands 0x20, 0x06, 0x40 for EEPROM reading
   - Voltage offset compensation

## Safety Considerations

### Voltage Limits

- **Minimum:** 12.0V (N=246 for v0x71)
- **Maximum:** 15.0V (N=9 for v0x71)
- **Hardware default:** 15.2V when EN asserted without I2C control
- **Recommended start:** 13.0V (N=167 for v0x71)

**Never exceed 15.0V** - will damage hashboards.

### Over-current Protection

PSU has built-in protection at 291-350A. Recommended limits:

- 240A @ 15V
- 300A @ 12V

Protection triggers: PSU enters lockout, requires power cycle.

### Power Sequencing

**Power-on:**

1. Assert EN (GPIO low)
2. Wait 500ms for PSU stabilization
3. Set voltage via I2C
4. Wait 500ms for voltage settling
5. Enable hashboards

**Power-off:**

1. Disable hashboards
2. Ramp voltage to 12.0V
3. Wait 100ms
4. De-assert EN (GPIO high)

## Response Validation

**Source:** `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/check_read_back_data-001c7140.c`

The PSU validates responses using:

1. **CRC Check:** Sum bytes from LEN field through end of data, compare with 2-byte CRC
2. **Magic Validation:** Verify `0x55 0xAA` header
3. **Command Echo:** Verify response CMD byte matches request CMD byte
4. **Length Check:** Verify response length matches expected size

```c
// CRC calculation
crc = 0;
for (i = 2; i < packet_len - 2; i++)
    crc += packet[i];
// Verify crc matches last 2 bytes (little-endian)
```

**Retry Logic:** Up to 3 retries if validation fails (lines 61-76 in `exec_power_cmd-001c7244.c`)

## PSU Calibration Data

PSUs may contain factory calibration data stored in EEPROM. When calibrated:

- Multiple voltage setpoints with calibration offsets
- Used for more accurate voltage control
- Detected during `bitmain_power_open()` initialization
- Applied in `bitmain_set_voltage()` if `_g_power_state.power_Calibrated == true`
- Stores calibration date, serial number, and voltage compensation values

**Implementation:**

- Lines 228-385 in `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_power_open-001c81cc.c` - Calibration data parsing
- Lines 52-97 in `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_set_voltage-001c95f0.c` - Calibration data application
- Lines 13-37 in `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/bitmain_convert_V_to_N-001c92c4.c` - Voltage to N conversion with calibration

## References

### Decompiled Source Files

**Primary analysis source (S19 XP+hyd):**

- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-c254c833bb83d47186d8419067a0cc3c/decomps/`
  - `bitmain_power_open-001c81cc.c` - PSU initialization
  - `exec_power_cmd-001c7244.c` - Legacy protocol execution
  - `exec_power_cmd_v2-001c7310.c` - V2 protocol execution
  - `bitmain_convert_V_to_N-001c92c4.c` - Voltage encoding
  - `bitmain_convert_N_to_V-001c9800.c` - Voltage decoding
  - `bitmain_convert_N_to_V_furmula-001c7c38.c` - Version-specific formulas
  - `bitmain_set_voltage-001c95f0.c` - Set voltage command
  - `bitmain_get_voltage-001c98a0.c` - Get voltage command
  - `bitmain_power_version-001c806c.c` - Version detection
  - `is_power_protocal_v2-001c8020.c` - Protocol version detection
  - `check_read_back_data-001c7140.c` - Response validation
  - `_bitmain_set_DA_conversion_N-001c7ecc.c` - Set voltage N value (legacy)
  - `bitmain_power_on-001c2a70.c` - GPIO enable control
  - `bitmain_power_off-001c2a9c.c` - GPIO disable control
  - `iic_write_reg-001ca270.c` - I2C write wrapper
  - `iic_read_reg-001ca1fc.c` - I2C read wrapper

**Verification source (S19 Pro):**

- `/home/danielsokil/Downloads/LiLei_WeChat/_ghidra/bins/single_board_test-d97121d0fd8a790763b549bb863df409/decomps/`

### Boot Logs

- `/home/danielsokil/Lab/HashSource/hashsource_antminer_s19pro/docs/bmminer_s19pro.log` - S19 Pro hardware boot sequence showing version 0x71

### Official Documentation

- `Antminer_x17_PSU_Protocol.pdf` - S17 PSU protocol (base for APW12)
- `APW12 Manual.pdf` - Hardware specifications
- `APW12 Power Supply Maintenance Guide.pdf` - Circuit and maintenance

### Binary Sources

Decompilations from Bitmain test fixture files:

- `/home/danielsokil/Downloads/LiLei_WeChat/XP+hyd PT1/single_board_test` (MD5: c254c833...)
- `/home/danielsokil/Downloads/LiLei_WeChat/S19 治具文件/single_board_test` (MD5: d97121d0...)

All findings cross-verified between multiple firmware variants and hardware boot logs.
