/*
 * ASIC Register Scanner
 * Scans all BM1398 ASIC register space to find undocumented registers
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "../include/bm1398_asic.h"

// Known documented registers
static const uint8_t known_regs[] = {
    0x00, // CHIP_ADDR
    0x04, // HASH_RATE
    0x08, // PLL_PARAM_0
    0x0C, // CHIP_NONCE_OFFSET
    0x10, // HASH_COUNTING_NUMBER
    0x14, // TICKET_MASK
    0x18, // CLK_CTRL (MISC_CONTROL)
    0x1C, // I2C_CONTROL
    0x20, // ORDERED_CLOCK_ENABLE
    0x28, // FAST_UART_CONFIG
    0x2C, // UART_RELAY
    0x38, // TICKET_MASK_2
    0x3C, // CORE_CONFIG (CORE_REGISTER_CONTROL)
    0x40, // CORE_REGISTER_STATUS
    0x44, // CORE_PARAM (timing parameters)
    0x58, // IO_DRIVER
    0x60, // PLL_PARAM_1
    0x64, // PLL_PARAM_2
    0x68, // PLL_PARAM_3
    0xA8, // SOFT_RESET
    0, 0, 0  // NULL terminator
};

int is_known_register(uint8_t reg) {
    for (int i = 0; known_regs[i] != 0 || i == 0; i++) {
        if (known_regs[i] == reg) {
            return 1;
        }
        if (known_regs[i] == 0 && known_regs[i+1] == 0) {
            break;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int chain = 0;
    int scan_all = 0;
    int scan_unknown = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chain") == 0 && i + 1 < argc) {
            chain = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "-a") == 0) {
            scan_all = 1;
        } else if (strcmp(argv[i], "--unknown") == 0 || strcmp(argv[i], "-u") == 0) {
            scan_unknown = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("ASIC Register Scanner\n\n");
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --chain N   Scan chain N (default: 0)\n");
            printf("  -a, --all   Scan all registers 0x00-0xFF\n");
            printf("  -u, --unknown  Only scan undocumented registers\n");
            printf("  -h, --help  Show this help\n\n");
            return 0;
        }
    }

    if (!scan_all && !scan_unknown) {
        printf("Please specify --all or --unknown\n");
        return 1;
    }

    printf("ASIC Register Scanner\n");
    printf("=====================\n");
    printf("Chain: %d\n", chain);
    printf("Mode: %s\n\n", scan_all ? "All registers" : "Unknown registers only");

    // Initialize ASIC communication
    bm1398_context_t ctx;
    if (bm1398_init(&ctx) < 0) {
        fprintf(stderr, "Error: Failed to initialize ASIC communication\n");
        return 1;
    }

    // Initialize the chain (without full power-up sequence)
    printf("Initializing chain %d...\n", chain);
    if (bm1398_enumerate_chain(&ctx, chain) < 0) {
        fprintf(stderr, "Error: Failed to enumerate chain\n");
        bm1398_cleanup(&ctx);
        return 1;
    }

    if (ctx.num_chips[chain] == 0) {
        fprintf(stderr, "Error: No chips detected on chain %d\n", chain);
        bm1398_cleanup(&ctx);
        return 1;
    }

    printf("Found %d chips on chain %d\n\n", ctx.num_chips[chain], chain);

    // Scan first chip only (chip address 0)
    printf("Scanning chip 0 registers...\n");
    printf("Format: REG_ADDR VALUE [status]\n\n");

    int success_count = 0;
    int fail_count = 0;
    int timeout_count = 0;

    for (uint16_t reg = 0; reg <= 0xFF; reg += 4) {
        uint8_t reg_addr = (uint8_t)reg;

        // Skip known registers if scanning unknown only
        if (scan_unknown && is_known_register(reg_addr)) {
            continue;
        }

        uint32_t value = 0;
        int ret = bm1398_read_register(&ctx, chain, false, 0, reg_addr, &value, 100);

        if (ret == 0) {
            printf("0x%02X: 0x%08X", reg_addr, value);
            if (!is_known_register(reg_addr)) {
                printf("  [UNKNOWN]");
            }
            printf("\n");
            success_count++;
        } else if (ret == -2) {
            // Timeout - register might not exist
            timeout_count++;
        } else {
            fail_count++;
        }

        usleep(10000);  // 10ms delay between reads
    }

    printf("\n");
    printf("Scan complete:\n");
    printf("  Successful reads: %d\n", success_count);
    printf("  Timeouts: %d\n", timeout_count);
    printf("  Errors: %d\n", fail_count);

    bm1398_cleanup(&ctx);
    return 0;
}
