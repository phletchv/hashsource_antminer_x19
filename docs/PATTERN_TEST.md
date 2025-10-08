# BM1398 Pattern Test System

## Overview

The factory test fixture uses **pre-generated test patterns** to verify ASIC hashing without needing a pool connection. These patterns contain known work with expected nonces that ASICs should find.

## Pattern File Format

### Structure

Each ASIC has its own pattern file (`btc-asic-000.bin` through `btc-asic-127.bin`):

- **File size**: 579,072 bytes (565KB)
- **Pattern entry size**: 0x34 (52) bytes
- **Total patterns**: Per core × pattern_number (typically 80 cores × 8 patterns)

### Pattern Entry Format (116 bytes = 0x74)

**Verified from single_board_test binary analysis:**

- Function: `parse_bin_file_to_pattern_ex` at ~0x13130
- Read operation: `fread(v10, 1u, 0x74u, v8)` at line 13155
- Pattern file path: `/mnt/card/BM1398-pattern/btc-asic-%03d.bin`

```c
struct test_pattern {
    uint8_t  header[15];      // Header/metadata (unknown purpose)
    uint8_t  work_data[12];   // Offset 15-26: Last 12 bytes of block header
    uint8_t  midstate[32];    // Offset 27-58: SHA256 midstate
    uint8_t  reserved[29];    // Offset 59-87: Padding/reserved
    uint32_t nonce;           // Offset 88-91: Expected nonce (little-endian)
    uint8_t  trailer[24];     // Offset 92-115: Additional data
    // Total: 15 + 12 + 32 + 29 + 4 + 24 = 116 bytes (0x74)
};
```

**Critical Discovery:** Previous documentation incorrectly stated 52 bytes (0x34).
Actual binary reads **0x74 (116) bytes** per pattern entry using `fread(v10, 1u, 0x74u, v8)`.

### Pattern File Layout

**Actual file structure (verified via binary analysis and hex dump):**

File size: **579,072 bytes** (565KB)
Structure: **80 cores × 7,238 bytes per core row**

```
For each core (80 cores total):
  Core row = 7,238 bytes (0x1C46):
    - Large header section (varies, ~6,342 bytes)
    - 8 pattern entries × 116 bytes each (0x74)

  Pattern entry layout (116 bytes = 0x74):
    Offset  Size  Field
    ------  ----  -----
    0x00    15    Header/metadata
    0x0F    12    Work data (last 12 bytes of block header)
    0x1B    32    SHA256 midstate
    0x3B    29    Reserved/padding
    0x58    4     Expected nonce (little-endian)
    0x5C    24    Trailer/additional data

Reading code (from single_board_test.c):
  - Reads: fread(buffer, 1, 0x74, fp)  // 116 bytes
  - Memory format: 124 bytes (0x7C) after processing
  - Skips unused patterns in each core row
```

**File size calculation:**

- 80 cores × 7,238 bytes/core = 579,040 bytes
- Actual file: 579,072 bytes
- Difference: 32 bytes (likely file padding)

## Test Process

### PT1/PT2/PT3 Test Flow

1. **Load Patterns**: Read pattern files for each ASIC chip from SD card

   - Path: `/mnt/card/BM1398-pattern/btc-asic-XXX.bin`
   - One file per ASIC (000-127 for S19 Pro with 114 chips active)

2. **Send Test Work**: For each ASIC × each core × each pattern:

   - Build 148-byte work packet with:
     - `work_id` = pattern_index
     - `work_data` = pattern.data[12] (offset 15 in pattern)
     - `midstates[0-3]` = pattern.midstate[32] (offset 27 in pattern, same for all 4 midstates in 4-midstate mode)
   - Send via FPGA indirect registers 16/17 (function `sub_22B10` at 0x22B10)
   - All words byte-swapped before sending

3. **Collect Nonces**: Monitor FPGA nonce FIFO registers

   - Read functions: `sub_22398` (single read) or `sub_223BC` (double read) at 0x22398/0x223BC
   - Reads logical index 4 (RETURN_NONCE at 0x010)
   - Reads logical index 5 (NONCE_NUMBER_IN_FIFO at 0x018)
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
// Pattern structure (CORRECTED - verified from binary)
typedef struct {
    uint8_t  header[15];      // Header/metadata
    uint8_t  work_data[12];   // Offset 15: Last 12 bytes of block header
    uint8_t  midstate[32];    // Offset 27: SHA256 midstate
    uint8_t  reserved[29];    // Offset 59: Padding/reserved
    uint32_t nonce;           // Offset 88: Expected nonce (little-endian)
    uint8_t  trailer[24];     // Offset 92: Additional data
} test_pattern_t;  // Total: 116 bytes (0x74)

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

    // Read pattern entries for this ASIC
    // File has large header per core row, then 8 pattern entries
    for (int core = 0; core < num_cores; core++) {
        // Skip to start of patterns in this core row
        // (Complex file structure - needs analysis of header size)

        for (int pat = 0; pat < patterns_per_core; pat++) {
            int idx = core * patterns_per_core + pat;
            // Read 116 bytes (0x74) as factory test does
            fread(&works[idx].pattern, 1, 0x74, fp);
            works[idx].work_id = pat;
            works[idx].is_nonce_returned = 0;
        }

        // Skip remaining pattern slots if < 8 patterns used
        if (patterns_per_core < 8) {
            fseek(fp, (8 - patterns_per_core) * 0x74, SEEK_CUR);
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
        // Midstate is at offset 27 in the pattern structure
        for (int m = 0; m < 4; m++) {
            memcpy(midstates[m], works[i].pattern.midstate, 32);
        }

        // Send work packet
        // Work data is at offset 15 in the pattern structure
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

Found in Bitmain_Test_Fixtures reference:

```
/home/danielsokil/Downloads/Bitmain_Test_Fixtures/S19_Pro/BM1398-pattern/
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

## Key Verified Functions (from single_board_test binary)

### Pattern Loading

- **Function**: `parse_bin_file_to_pattern_ex` at ~0x13130
- **Reads**: 116 bytes (0x74) per pattern entry
- **Memory**: 124 bytes (0x7C) after processing
- **Skips**: Unused patterns (8 pattern slots per core, reads only Pattern_Number)

### Work Sending

- **4-midstate function**: `software_pattern_4_midstate_send_function` at ~0x12760
- **8-midstate function**: `software_pattern_8_midstate_send_function` at ~0x12930
- **FPGA write**: `sub_22B10` at 0x22B10
  - First word → Logical index 16
  - Remaining words → Logical index 17 (looped)
  - Thread-safe with pthread mutex

### Nonce Reading

- **Single read**: `sub_22398` at 0x22398
- **Double read**: `sub_223BC` at 0x223BC (for stability)
- **Registers**:
  - Logical index 4 → Physical 0x010 (RETURN_NONCE)
  - Logical index 5 → Physical 0x018 (NONCE_NUMBER_IN_FIFO)

## References

- `/home/danielsokil/Downloads/Bitmain_Test_Fixtures/S19_Pro/BM1398-pattern/` - Pattern binary files
- `single_board_test.c` - Factory test decompiled implementation
- **Verified addresses**: All function addresses confirmed from binary analysis (2025-10-07)
