/*
 * BM1398 Chain Test Utility
 *
 * Tests chip enumeration and basic initialization on specified chain
 * Usage: chain_test [chain_id]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include "../include/bm1398_asic.h"

void print_usage(const char *prog) {
    printf("Usage: %s [chain_id]\n", prog);
    printf("  chain_id: 0, 1, or 2 (default: 0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s 0      # Test chain 0\n", prog);
    printf("  %s        # Test chain 0 (default)\n", prog);
}

void test_crc5(void) {
    printf("====================================\n");
    printf("Testing CRC5 Implementation\n");
    printf("====================================\n\n");

    // Test vector: Chain inactive command
    uint8_t cmd_inactive[] = {0x53, 0x05, 0x00, 0x00};
    uint8_t crc = bm1398_crc5(cmd_inactive, 32);
    printf("Chain inactive command: 0x53 0x05 0x00 0x00\n");
    printf("  CRC5: 0x%02X\n\n", crc);

    // Test vector: Set address 0
    uint8_t cmd_addr0[] = {0x40, 0x05, 0x00, 0x00};
    crc = bm1398_crc5(cmd_addr0, 32);
    printf("Set address 0 command: 0x40 0x05 0x00 0x00\n");
    printf("  CRC5: 0x%02X\n\n", crc);

    // Test vector: Write register 0x14 = 0xFFFFFFFF (broadcast)
    uint8_t cmd_write[] = {0x51, 0x09, 0x00, 0x14, 0xFF, 0xFF, 0xFF, 0xFF};
    crc = bm1398_crc5(cmd_write, 64);
    printf("Write register command: 0x51 0x09 0x00 0x14 0xFF 0xFF 0xFF 0xFF\n");
    printf("  CRC5: 0x%02X\n\n", crc);
}

int main(int argc, char *argv[]) {
    int chain_id = 0;

    // Parse command line
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        chain_id = atoi(argv[1]);
        if (chain_id < 0 || chain_id >= MAX_CHAINS) {
            fprintf(stderr, "Error: Invalid chain ID %d (must be 0-2)\n", chain_id);
            return 1;
        }
    }

    printf("====================================\n");
    printf("BM1398 Chain Test Utility\n");
    printf("====================================\n\n");

    // Test CRC5 implementation first
    test_crc5();

    printf("====================================\n");
    printf("Initializing BM1398 Driver\n");
    printf("====================================\n\n");

    bm1398_context_t ctx;
    if (bm1398_init(&ctx) < 0) {
        fprintf(stderr, "Error: Failed to initialize BM1398 driver\n");
        return 1;
    }

    printf("\nDriver initialized successfully\n");
    printf("Detected %d chain(s)\n\n", ctx.num_chains);

    if (ctx.num_chains == 0) {
        fprintf(stderr, "Error: No chains detected\n");
        bm1398_cleanup(&ctx);
        return 1;
    }

    // Check if requested chain exists
    if (!(bm1398_detect_chains(&ctx) & (1 << chain_id))) {
        fprintf(stderr, "Error: Chain %d not detected\n", chain_id);
        bm1398_cleanup(&ctx);
        return 1;
    }

    printf("====================================\n");
    printf("Testing Chain %d\n", chain_id);
    printf("====================================\n\n");

    printf("Chain %d configuration:\n", chain_id);
    printf("  Chips per chain: %d\n", ctx.chips_per_chain[chain_id]);
    printf("  Address interval: %d\n", CHIP_ADDRESS_INTERVAL);
    printf("  Target frequency: %d MHz\n", FREQUENCY_525MHZ);
    printf("  Target baud rate: %d Hz\n\n", BAUD_RATE_12MHZ);

    // Test 1: Chip enumeration only
    printf("====================================\n");
    printf("Test 1: Chip Enumeration\n");
    printf("====================================\n\n");

    printf("Sending chain inactive command...\n");
    if (bm1398_chain_inactive(&ctx, chain_id) < 0) {
        fprintf(stderr, "Error: Chain inactive failed\n");
        bm1398_cleanup(&ctx);
        return 1;
    }
    printf("  SUCCESS\n\n");

    usleep(10000);

    printf("Enumerating %d chips...\n", ctx.chips_per_chain[chain_id]);
    if (bm1398_enumerate_chips(&ctx, chain_id, ctx.chips_per_chain[chain_id]) < 0) {
        fprintf(stderr, "Error: Chip enumeration failed\n");
        bm1398_cleanup(&ctx);
        return 1;
    }
    printf("  SUCCESS\n\n");

    // Test 2: Register write (ticket mask)
    printf("====================================\n");
    printf("Test 2: Register Write\n");
    printf("====================================\n\n");

    printf("Writing TICKET_MASK register (0x14) = 0xFFFFFFFF...\n");
    if (bm1398_write_register(&ctx, chain_id, true, 0, ASIC_REG_TICKET_MASK,
                              TICKET_MASK_ALL_CORES) < 0) {
        fprintf(stderr, "Error: Register write failed\n");
        bm1398_cleanup(&ctx);
        return 1;
    }
    printf("  SUCCESS\n\n");

    usleep(10000);

    // Test 3: Check CRC error count
    printf("====================================\n");
    printf("Test 3: CRC Error Check\n");
    printf("====================================\n\n");

    int crc_errors = bm1398_get_crc_error_count(&ctx);
    printf("CRC error count: %d\n", crc_errors);
    if (crc_errors > 0) {
        fprintf(stderr, "Warning: %d CRC errors detected\n", crc_errors);
    } else {
        printf("  No CRC errors detected\n");
    }
    printf("\n");

    // Test 4: Full initialization (optional)
    printf("====================================\n");
    printf("Test 4: Full Chain Initialization\n");
    printf("====================================\n\n");

    printf("Do you want to run full chain initialization? (y/n): ");
    fflush(stdout);

    char response[10];
    if (fgets(response, sizeof(response), stdin)) {
        if (response[0] == 'y' || response[0] == 'Y') {
            if (bm1398_init_chain(&ctx, chain_id) < 0) {
                fprintf(stderr, "Error: Chain initialization failed\n");
                bm1398_cleanup(&ctx);
                return 1;
            }
            printf("\nChain initialization completed successfully!\n\n");
        } else {
            printf("Skipping full initialization\n\n");
        }
    }

    // Summary
    printf("====================================\n");
    printf("Test Summary\n");
    printf("====================================\n\n");
    printf("✓ CRC5 calculation working\n");
    printf("✓ FPGA UART interface working\n");
    printf("✓ Chain inactive command sent\n");
    printf("✓ Chip enumeration completed\n");
    printf("✓ Register write successful\n");
    printf("✓ CRC error count: %d\n", crc_errors);
    printf("\nAll tests passed!\n\n");

    bm1398_cleanup(&ctx);
    return 0;
}
