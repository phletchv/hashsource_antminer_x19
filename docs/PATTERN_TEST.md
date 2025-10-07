# BM1398 Pattern Test System

## Overview

The factory test fixture uses **pre-generated test patterns** to verify ASIC hashing without needing a pool connection. These patterns contain known work with expected nonces that ASICs should find.

## Pattern File Format

### Structure

Each ASIC has its own pattern file (`btc-asic-000.bin` through `btc-asic-127.bin`):

- **File size**: 579,072 bytes (565KB)
- **Pattern entry size**: 0x34 (52) bytes
- **Total patterns**: Per core × pattern_number (typically 80 cores × 8 patterns)

### Pattern Entry Format (52 bytes = 0x34)

**Corrected based on implementation analysis:**

```c
struct test_pattern {
    uint8_t  midstate[32];   // SHA256 midstate
    uint8_t  reserved[4];    // Padding (0x00000000)
    uint32_t nonce;          // Expected nonce (little-endian)
    uint8_t  work_data[12];  // Last 12 bytes of block header
    // Total: 32 + 4 + 4 + 12 = 52 bytes (0x34)
};
```

### Pattern File Layout

**Actual file structure (verified via hex analysis):**

```
For each core (80 cores):
  112-byte row structure:
    0x00-0x33: Header/metadata (52 bytes)
    0x34-0x67: Pattern 0 (52 bytes) <- PATTERN_OFFSET
    0x68-0x9B: Pattern 1 (52 bytes)
    ...
    0x1F4-0x227: Pattern 7 (52 bytes)
    (8 patterns × 52 bytes = 416 bytes starting at offset 0x34)

Reading strategy:
  1. Seek to offset 0x34 (skip header)
  2. Read 8 patterns × 52 bytes = 416 bytes
  3. Pattern data starts at offset 0x34 within each 112-byte core row
```

## Test Process

### PT1/PT2/PT3 Test Flow

1. **Load Patterns**: Read pattern files for each ASIC chip from SD card

   - Path: `/mnt/card/BM1398-pattern/btc-asic-XXX.bin`
   - One file per ASIC (000-127 for S19 Pro with 114 chips active)

2. **Send Test Work**: For each ASIC × each core × each pattern:

   - Build 148-byte work packet with:
     - `work_id` = pattern_index
     - `work_data` = pattern.data[12]
     - `midstates[0]` = pattern.midstate[32] (use same for all 4 midstates in 4-midstate mode)
   - Send via FPGA TW_WRITE_COMMAND

3. **Collect Nonces**: Monitor FPGA RETURN_NONCE register

   - Parse response to extract:
     - ASIC index (from nonce value using address_interval)
     - Core ID (from nonce value)
     - Pattern number (from work_id field)
     - Nonce value (32-bit)

4. **Validate Nonces**: For each returned nonce:

   - Lookup expected nonce: `pattern_info.works[asic_id][core_index * pattern_num + pattern_index].pattern.nonce`
   - Compare: `received_nonce == expected_nonce`
   - If match: Count as valid nonce, increment stats
   - If no match: Count as error

5. **Calculate Results**:
   - Valid nonces per ASIC
   - Valid nonces per core
   - Total nonce return rate
   - Pass/Fail criteria (e.g., >95% nonce return rate)

## Implementation Example

```c
// Pattern structure (corrected format)
typedef struct {
    uint8_t  midstate[32];   // SHA256 midstate
    uint8_t  reserved[4];    // Padding (0x00000000)
    uint32_t nonce;          // Expected nonce (little-endian)
    uint8_t  work_data[12];  // Last 12 bytes of block header
} test_pattern_t;

// Work entry (per core per pattern)
typedef struct {
    test_pattern_t pattern;
    uint16_t work_id;
    uint8_t  is_nonce_returned;
} pattern_work_t;

// Load pattern file for one ASIC
int load_asic_patterns(int asic_id, int num_cores, int patterns_per_core,
                      pattern_work_t *works) {
    char filename[128];
    snprintf(filename, sizeof(filename),
             "/mnt/card/BM1398-pattern/btc-asic-%03d.bin", asic_id);

    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;

    for (int core = 0; core < num_cores; core++) {
        for (int pat = 0; pat < patterns_per_core; pat++) {
            int idx = core * patterns_per_core + pat;
            fread(&works[idx].pattern, 1, 0x34, fp);
            works[idx].work_id = pat;
            works[idx].is_nonce_returned = 0;
        }
        // Skip remaining patterns if < 8
        if (patterns_per_core < 8) {
            fseek(fp, (8 - patterns_per_core) * 0x34, SEEK_CUR);
        }
    }

    fclose(fp);
    return 0;
}

// Send pattern test work
int send_pattern_work(bm1398_context_t *ctx, int chain, int asic_id,
                     pattern_work_t *works, int num_works) {
    for (int i = 0; i < num_works; i++) {
        uint8_t midstates[4][32];

        // Use same midstate for all 4 slots in 4-midstate mode
        for (int m = 0; m < 4; m++) {
            memcpy(midstates[m], works[i].pattern.midstate, 32);
        }

        // Send work packet
        bm1398_send_work(ctx, chain, works[i].work_id,
                        works[i].pattern.work_data, midstates);

        usleep(1000);  // 1ms delay between work submissions
    }

    return 0;
}

// Check returned nonce against expected
int check_pattern_nonce(uint32_t received_nonce, int asic_id,
                       int core_id, int pattern_id,
                       pattern_work_t *works, int patterns_per_core) {
    int idx = core_id * patterns_per_core + pattern_id;
    uint32_t expected = works[idx].pattern.nonce;

    if (received_nonce == expected) {
        works[idx].is_nonce_returned++;
        return 1;  // Valid
    }

    return 0;  // Invalid
}
```

## Available Pattern Files

Found in LiLei reference:

```
/home/danielsokil/Downloads/LiLei_WeChat/S19_Pro/BM1398-pattern/
  - btc-asic-000.bin through btc-asic-127.bin
  - Each file: 565KB (579,072 bytes)
```

## Configuration Parameters

From Config.ini:

- **Pattern_Number**: 8 (patterns per core)
- **Small_Core_Num_In_Big_Core**: 16
- **Big_Core_Num**: 5
- **Total cores**: 80 (16 × 5)
- **Midstate_Number**: 4 (4-midstate mode)

## Implementation Status

**COMPLETED (2025-10-07):**

1. [x] Copy pattern files to test machines (/tmp/BM1398-pattern/)
2. [x] Implement pattern loader (hashsource_x19/src/pattern_test.c:46-92)
3. [x] Implement nonce validation logic (hashsource_x19/src/pattern_test.c:137-149)
4. [x] Create pattern_test utility (hashsource_x19/src/pattern_test.c)
5. [x] PSU power control (15V working)
6. [x] PIC DC-DC converter enable (FPGA I2C, response: 0x15 0x01)
7. [x] PLL frequency configuration (525 MHz, VCO=2100 MHz)

**Test Results (Machine 1: 192.30.1.24):**

```
Test: 80 patterns from btc-asic-000.bin (first ASIC, first core)
Chain initialization: [OK] 114/114 chips, 0 CRC errors
PSU power: [OK] 15V enabled
PIC DC-DC: [OK] Enabled (0x15 0x01)
PLL frequency: [OK] 525 MHz configured (0x40540100)
Patterns sent: [OK] 80/80 accepted by FPGA FIFO
Nonces received: [FAIL] 0/80 after 60 seconds

Status: All hardware initialization successful, but ASICs not returning nonces
```

**Current Issue:**

Despite successful initialization:

- Chain enumeration: 114/114 chips addressed
- PSU: 15V power enabled
- PIC DC-DC: Converter enabled and responding
- PLL: 525 MHz frequency configured (VCO=2100 MHz)
- Work: 80 test patterns sent and accepted

ASICs are not returning nonces. Investigating:

- Work packet format/byte order
- Missing ASIC register configuration
- Core timing parameters
- Voltage adjustment needs (may need 12.6-12.8V for operation vs 15V startup)

**Next Steps:**

1. Compare FPGA register dumps with stock firmware during operation
2. Verify work packet byte order matches factory test expectations
3. Test with minimal patterns (1-10) to rule out buffer issues
4. Review factory test for missing ASIC register writes
5. Consider voltage adjustment after initialization

## References

- `LiLei_WeChat/S19_Pro/BM1398-pattern/` - Pattern binary files
- `single_board_test.c` - Factory test implementation
- `BTC_software_pattern_check_nonce()` - Nonce validation logic
- `parse_bin_file_to_pattern_ex()` - Pattern file parser
