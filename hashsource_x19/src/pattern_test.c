/*
 * BM1398 Pattern Test - Verify ASIC Hashing
 *
 * Uses pre-generated test patterns with known nonces to verify ASICs
 * can find correct solutions without needing pool connection.
 *
 * Based on Bitmain factory test fixture pattern test methodology.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "../include/bm1398_asic.h"

// Configuration
#define TEST_CHAIN 0
#define CORES_PER_ASIC 80    // BM1398: 80 cores (16 small cores × 5 big cores)
#define PATTERNS_PER_CORE 8  // Standard pattern test uses 8 patterns per core
#define TEST_ASIC_ID 0       // Test first ASIC only
#define TEST_PATTERNS 80     // Test all patterns for first core
#define NONCE_TIMEOUT_SEC 60 // Longer timeout for first test
#define PATTERN_OFFSET 0x34  // Offset to pattern data within each 112-byte row

// Pattern file structure (52 bytes = 0x34)
// Based on analysis of btc-asic-000.bin
typedef struct __attribute__((packed)) {
    uint8_t  midstate[32];   // SHA256 midstate
    uint8_t  reserved1[4];   // Padding (0x00000000)
    uint32_t nonce;          // Expected nonce (little-endian)
    uint8_t  work_data[12];  // Last 12 bytes of block header
} test_pattern_t;

// Work entry with pattern and tracking
typedef struct {
    test_pattern_t pattern;
    uint16_t work_id;
    uint8_t  nonce_returned;
} pattern_work_t;

/**
 * Load pattern file for one ASIC
 */
int load_asic_patterns(const char *pattern_dir, int asic_id,
                      pattern_work_t *works, int max_works) {
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/btc-asic-%03d.bin",
             pattern_dir, asic_id);

    printf("Loading pattern file: %s\n", filename);

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open pattern file %s\n", filename);
        return -1;
    }

    int loaded = 0;
    for (int core = 0; core < CORES_PER_ASIC && loaded < max_works; core++) {
        // Skip to pattern data offset for this core's row
        if (fseek(fp, PATTERN_OFFSET, SEEK_CUR) != 0) {
            fprintf(stderr, "Error: Seek failed at core %d\n", core);
            fclose(fp);
            return -1;
        }

        for (int pat = 0; pat < PATTERNS_PER_CORE && loaded < max_works; pat++) {
            size_t read_bytes = fread(&works[loaded].pattern, 1, 0x34, fp);
            if (read_bytes != 0x34) {
                fprintf(stderr, "Error: Short read at core %d pattern %d\n",
                       core, pat);
                fclose(fp);
                return -1;
            }

            works[loaded].work_id = pat;
            works[loaded].nonce_returned = 0;
            loaded++;
        }

        // Each core has 8 pattern slots, skip remaining if we read fewer
        if (PATTERNS_PER_CORE < 8) {
            fseek(fp, (8 - PATTERNS_PER_CORE) * 0x34, SEEK_CUR);
        }
    }

    fclose(fp);
    printf("Loaded %d test patterns\n\n", loaded);
    return loaded;
}

/**
 * Send pattern test work to chain
 */
int send_pattern_work(bm1398_context_t *ctx, int chain,
                     pattern_work_t *works, int num_works) {
    printf("====================================\n");
    printf("Sending %d Test Patterns\n", num_works);
    printf("====================================\n\n");

    for (int i = 0; i < num_works; i++) {
        // Check FIFO space
        while (bm1398_check_work_fifo_ready(ctx) < 1) {
            usleep(1000);
        }

        // Build midstates array (use same midstate for all 4 slots)
        uint8_t midstates[4][32];
        for (int m = 0; m < 4; m++) {
            memcpy(midstates[m], works[i].pattern.midstate, 32);
        }

        // Send work packet
        if (bm1398_send_work(ctx, chain, works[i].work_id,
                            works[i].pattern.work_data, midstates) < 0) {
            fprintf(stderr, "Error: Failed to send pattern %d\n", i);
            return -1;
        }

        if ((i + 1) % 10 == 0) {
            printf("  Sent %d/%d patterns\n", i + 1, num_works);
        }

        usleep(5000);  // 5ms delay between work packets
    }

    printf("All %d patterns sent successfully!\n\n", num_works);
    return 0;
}

/**
 * Extract ASIC index and core ID from nonce
 * Based on Bitmain get_asic_index_by_nonce() and get_coreid_by_nonce()
 */
void parse_nonce_info(uint32_t nonce, int address_interval,
                     int *asic_id, int *core_id) {
    // ASIC index from upper bits
    *asic_id = (nonce >> 24) / address_interval;

    // Core ID encoding (from factory test analysis)
    // Upper nibble = big core (0-4), lower nibble = small core (0-15)
    uint8_t core_byte = (nonce >> 16) & 0xFF;
    int big_core = (core_byte >> 4) & 0xF;
    int small_core = core_byte & 0xF;
    *core_id = big_core * 16 + small_core;
}

/**
 * Main test function
 */
int main(int argc, char *argv[]) {
    const char *pattern_dir = "/tmp/BM1398-pattern";
    int chain = TEST_CHAIN;

    if (argc > 1) {
        chain = atoi(argv[1]);
    }
    if (argc > 2) {
        pattern_dir = argv[2];
    }

    printf("\n");
    printf("====================================\n");
    printf("BM1398 Pattern Test\n");
    printf("====================================\n");
    printf("Chain: %d\n", chain);
    printf("ASIC: %d\n", TEST_ASIC_ID);
    printf("Test patterns: %d\n", TEST_PATTERNS);
    printf("Pattern dir: %s\n", pattern_dir);
    printf("\n");

    // Allocate pattern storage
    pattern_work_t *works = calloc(CORES_PER_ASIC * PATTERNS_PER_CORE,
                                   sizeof(pattern_work_t));
    if (!works) {
        fprintf(stderr, "Error: Failed to allocate pattern storage\n");
        return 1;
    }

    // Load patterns
    int num_loaded = load_asic_patterns(pattern_dir, TEST_ASIC_ID,
                                       works, CORES_PER_ASIC * PATTERNS_PER_CORE);
    if (num_loaded < TEST_PATTERNS) {
        fprintf(stderr, "Error: Failed to load enough patterns (%d < %d)\n",
               num_loaded, TEST_PATTERNS);
        free(works);
        return 1;
    }

    // Initialize driver
    bm1398_context_t ctx;
    if (bm1398_init(&ctx) < 0) {
        fprintf(stderr, "Error: Failed to initialize driver\n");
        free(works);
        return 1;
    }

    // Initialize chain
    printf("Initializing chain %d...\n\n", chain);
    if (bm1398_init_chain(&ctx, chain) < 0) {
        fprintf(stderr, "Warning: Chain initialization failed\n");
    }

    // Power on PSU
    printf("====================================\n");
    printf("Powering On PSU\n");
    printf("====================================\n");
    printf("Voltage: 15.0V\n");
    if (bm1398_psu_power_on(&ctx, 15000) < 0) {
        fprintf(stderr, "Error: Failed to power on PSU\n");
        bm1398_cleanup(&ctx);
        free(works);
        return 1;
    }
    printf("PSU powered on\n\n");

    // Enable hashboard DC-DC converter (may already be enabled)
    printf("====================================\n");
    printf("Enabling Hashboard DC-DC Converter\n");
    printf("====================================\n");
    if (bm1398_enable_dc_dc(&ctx, chain) < 0) {
        printf("Note: DC-DC enable failed (may already be enabled from previous run)\n");
        printf("Continuing with test...\n");
    }
    sleep(1);  // Allow power to stabilize
    printf("\n");

    // Reduce voltage to operational level (like stock firmware does)
    printf("====================================\n");
    printf("Reducing Voltage to Operational Level\n");
    printf("====================================\n");
    printf("Reducing from 15.0V to 12.8V...\n");
    if (bm1398_psu_set_voltage(&ctx, 12800) < 0) {
        fprintf(stderr, "Warning: Failed to reduce voltage to 12.8V\n");
        fprintf(stderr, "Continuing with test at 15.0V...\n");
    } else {
        printf("Voltage reduced to 12.8V\n");
    }
    sleep(2);  // Allow voltage to stabilize
    printf("\n");

    // Enable FPGA work distribution
    printf("Enabling FPGA work distribution...\n");
    bm1398_enable_work_send(&ctx);
    bm1398_start_work_gen(&ctx);
    usleep(100000);  // 100ms settle time
    printf("\n");

    // Send test patterns
    if (send_pattern_work(&ctx, chain, works, TEST_PATTERNS) < 0) {
        bm1398_cleanup(&ctx);
        free(works);
        return 1;
    }

    // Monitor for nonces
    printf("====================================\n");
    printf("Monitoring for Nonces (%d seconds)\n", NONCE_TIMEOUT_SEC);
    printf("====================================\n\n");

    time_t start_time = time(NULL);
    int total_nonces = 0;
    int valid_nonces = 0;
    nonce_response_t nonces[100];

    while (time(NULL) - start_time < NONCE_TIMEOUT_SEC) {
        int count = bm1398_get_nonce_count(&ctx);

        if (count > 0) {
            int read = bm1398_read_nonces(&ctx, nonces, 100);

            for (int i = 0; i < read; i++) {
                total_nonces++;

                // Parse nonce info
                int asic_id, core_id;
                parse_nonce_info(nonces[i].nonce, CHIP_ADDRESS_INTERVAL,
                               &asic_id, &core_id);
                int pattern_id = nonces[i].work_id;

                printf("Nonce #%d: 0x%08x (asic=%d, core=%d, pattern=%d)\n",
                       total_nonces, nonces[i].nonce, asic_id, core_id, pattern_id);

                // Validate against expected
                if (asic_id == TEST_ASIC_ID && core_id < CORES_PER_ASIC &&
                    pattern_id < PATTERNS_PER_CORE) {
                    int idx = core_id * PATTERNS_PER_CORE + pattern_id;
                    if (idx < num_loaded) {
                        uint32_t expected = works[idx].pattern.nonce;
                        if (nonces[i].nonce == expected) {
                            printf("  ✓ VALID! Matches expected nonce\n");
                            works[idx].nonce_returned++;
                            valid_nonces++;
                        } else {
                            printf("  ✗ MISMATCH! Expected 0x%08x\n", expected);
                        }
                    }
                }
            }
        }

        usleep(100000);  // 100ms polling interval
    }

    // Results
    printf("\n");
    printf("====================================\n");
    printf("Test Results\n");
    printf("====================================\n");
    printf("Patterns sent: %d\n", TEST_PATTERNS);
    printf("Total nonces received: %d\n", total_nonces);
    printf("Valid nonces: %d\n", valid_nonces);
    if (TEST_PATTERNS > 0) {
        printf("Success rate: %.1f%%\n",
               (valid_nonces * 100.0) / TEST_PATTERNS);
    }
    printf("\n");

    // Cleanup
    bm1398_cleanup(&ctx);
    free(works);

    return (valid_nonces > 0) ? 0 : 1;
}
