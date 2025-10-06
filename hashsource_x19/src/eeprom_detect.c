/*
 * X19 EEPROM Detection and Information Display
 *
 * Reads EEPROM data from hashboard chains via FPGA I2C controller.
 * Matches stock bmminer behavior exactly.
 *
 * Based on reverse engineering:
 * - bmminer FUN_00049e8c (EEPROM I2C operations)
 * - single_board_test sub_21B1C (EEPROM open/init)
 * - Slave address formula: (2 * chain_id) | (16 * device_type)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

//==============================================================================
// FPGA Register Definitions
//==============================================================================

#define FPGA_REG_BASE       0x40000000
#define FPGA_REG_SIZE       5120

// I2C controller registers - EACH CHAIN HAS ITS OWN REGISTER!
// Discovered via full FPGA register monitoring during bmminer EEPROM reads
#define REG_I2C_CHAIN0      (0x030 / 4)   // Chain 0 I2C control register
#define REG_I2C_CHAIN1      (0x3A8 / 4)   // Chain 1 I2C control register
#define REG_I2C_CHAIN2      (0x3AC / 4)   // Chain 2 I2C control register
#define REG_HASH_ON_PLUG    (0x08 / 4)    // Chain detection register

// I2C command format (from bmminer FUN_00049e8c:40)
// bits 26-27: master/chain ID
// bits 24-25: 0x3 = read operation
// bits 20-23: slave address high nibble
// bits 15-17: slave address low bits
// bits 8-15:  register/byte address
#define I2C_READ_FLAGS      0x03000000    // Read operation flags (bits 24-25)

// Device types (from single_board_test sub_21B1C)
#define DEVICE_TYPE_EEPROM  10            // 0x0A
#define DEVICE_TYPE_PIC     4             // 0x04

// EEPROM configuration
#define EEPROM_SIZE         256
#define EEPROM_MARKER       0x5A

// Max chains
#define MAX_CHAINS          3

// Timing (from bmminer FUN_000498a0)
#define I2C_POLL_TIMEOUT    0x259         // 601 iterations
#define I2C_POLL_DELAY_US   5000          // 5ms delay between polls

//==============================================================================
// Global FPGA register mapping
//==============================================================================

static volatile uint32_t *g_fpga_regs = NULL;

//==============================================================================
// FPGA Initialization
//==============================================================================

static int fpga_init(void) {
    int fd;

    // Open FPGA device
    fd = open("/dev/axi_fpga_dev", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open /dev/axi_fpga_dev: %s\n", strerror(errno));
        fprintf(stderr, "Make sure bitmain_axi.ko kernel module is loaded\n");
        return -1;
    }

    // Memory map FPGA registers
    g_fpga_regs = (volatile uint32_t *)mmap(NULL, FPGA_REG_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, fd, 0);
    close(fd);

    if (g_fpga_regs == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap FPGA registers: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void fpga_cleanup(void) {
    if (g_fpga_regs != NULL && g_fpga_regs != MAP_FAILED) {
        munmap((void *)g_fpga_regs, FPGA_REG_SIZE);
        g_fpga_regs = NULL;
    }
}

//==============================================================================
// FPGA I2C Operations (based on bmminer FUN_00049e8c)
//==============================================================================

// Per-chain I2C register mapping
static const int i2c_regs[MAX_CHAINS] = {
    REG_I2C_CHAIN0,
    REG_I2C_CHAIN1,
    REG_I2C_CHAIN2
};

// Calculate I2C slave address for chain (from single_board_test sub_25390)
// Formula: (2 * chain_id) | (16 * device_type)
static uint8_t get_slave_address(int chain_id, int device_type) {
    return (2 * chain_id) | (16 * device_type);
}

// Read single byte from EEPROM via FPGA I2C
// Matches bmminer FUN_00049e8c and single_board_test sub_255C0 exactly
static int i2c_read_byte(int chain_id, uint8_t reg_addr, uint8_t *data) {
    uint32_t cmd;
    uint32_t response;
    int i2c_reg;

    if (chain_id < 0 || chain_id >= MAX_CHAINS) {
        fprintf(stderr, "Invalid chain_id: %d\n", chain_id);
        return -1;
    }

    // Get I2C register for this chain
    i2c_reg = i2c_regs[chain_id];

    // Calculate per-chain slave address (matching bmminer exactly)
    // Formula from single_board_test: (2 * chain_id) | (16 * device_type)
    uint8_t slave_addr = get_slave_address(chain_id, DEVICE_TYPE_EEPROM);

    // Build I2C command EXACTLY as stock firmware does:
    // (*v8 << 26) | 0x3000000 | (v8[1] >> 4 << 20) | (v8[1] << 15) & 0x70000 | ((*a2 + v7) << 8)
    // where v8[1] is the full slave address (0xA0/0xA2/0xA4)
    cmd = ((uint32_t)chain_id << 26) |                    // Master/chain ID (bits 26-27)
          I2C_READ_FLAGS |                                // Read operation (0x3 << 24)
          (((uint32_t)slave_addr >> 4) << 20) |           // High nibble to bits 20-23
          (((uint32_t)slave_addr << 15) & 0x70000) |      // Slave addr shifted, masked to bits 16-18
          ((uint32_t)reg_addr << 8);                      // Register/byte address (bits 8-15)

    // Debug: show first command for each chain
    static int debug_shown[MAX_CHAINS] = {0};
    if (!debug_shown[chain_id]) {
        printf("Chain %d: reg=0x%03X, slave_addr=0x%02X, cmd=0x%08X\n",
               chain_id, i2c_reg * 4, slave_addr, cmd);
        debug_shown[chain_id] = 1;
    }

    // Write I2C command to chain-specific register
    g_fpga_regs[i2c_reg] = cmd;

    // Poll I2C register for response (from bmminer FUN_000498a0)
    // Timeout: 0x259 iterations * 5ms = ~3 seconds
    for (int i = 0; i < I2C_POLL_TIMEOUT; i++) {
        response = g_fpga_regs[i2c_reg];

        // Check bit 31 for data ready (from bmminer: if (v4 < 0))
        if (response & 0x80000000) {
            // Data is ready, extract byte from bits 0-7
            *data = (uint8_t)(response & 0xFF);
            return 0;
        }

        usleep(I2C_POLL_DELAY_US);
    }

    // Timeout
    fprintf(stderr, "I2C timeout at chain %d, reg 0x%02X (last response: 0x%08X)\n",
            chain_id, reg_addr, response);
    return -1;
}

// Read full EEPROM from chain
static int eeprom_read(int chain_id, uint8_t *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (i2c_read_byte(chain_id, (uint8_t)i, &buffer[i]) < 0) {
            fprintf(stderr, "Failed to read EEPROM byte %zu from chain %d\n", i, chain_id);
            return -1;
        }

        // Small delay between reads (matching stock firmware timing)
        if ((i % 16) == 15) {
            usleep(500000);  // 500ms every 16 bytes (from stock firmware)
        }
    }

    return 0;
}

//==============================================================================
// EEPROM Data Display (matches bmminer log format exactly)
//==============================================================================

static void display_eeprom(int chain_id, const uint8_t *data) {
    printf("[chain %d]\n", chain_id);

    // Display in exact bmminer format: 16 bytes per line, split into 2 groups of 8
    for (size_t i = 0; i < EEPROM_SIZE; i += 16) {
        printf("0x%04zX ", i);

        // First 8 bytes
        for (size_t j = 0; j < 8; j++) {
            printf("%02X ", data[i + j]);
        }

        printf("  ");  // Extra spacing between groups

        // Second 8 bytes
        for (size_t j = 8; j < 16; j++) {
            printf("%02X ", data[i + j]);
        }

        printf("\n");
    }

    printf("\n");
}

//==============================================================================
// Main
//==============================================================================

int main(void) {
    uint8_t eeprom_data[EEPROM_SIZE];
    uint32_t hash_on_plug;

    // Initialize FPGA
    if (fpga_init() < 0) {
        fprintf(stderr, "Failed to initialize FPGA\n");
        return 1;
    }

    // Check which chains are detected
    hash_on_plug = g_fpga_regs[REG_HASH_ON_PLUG];
    printf("HASH_ON_PLUG = 0x%08X\n", hash_on_plug);
    for (int i = 0; i < MAX_CHAINS; i++) {
        if (hash_on_plug & (1 << i)) {
            printf("  Chain %d: detected\n", i);
        }
    }
    printf("\n");

    // Read EEPROM from each detected chain (matching bmminer behavior)
    for (int chain = 0; chain < MAX_CHAINS; chain++) {
        if (!(hash_on_plug & (1 << chain))) {
            continue;  // Skip undetected chains
        }

        memset(eeprom_data, 0xFF, sizeof(eeprom_data));

        // Add small delay between chains (bmminer has ~2 second gap)
        if (chain > 0) {
            usleep(2000000);  // 2 second delay between chains
        }

        if (eeprom_read(chain, eeprom_data, EEPROM_SIZE) == 0) {
            display_eeprom(chain, eeprom_data);
        } else {
            fprintf(stderr, "Failed to read EEPROM from chain %d\n", chain);
        }
    }

    fpga_cleanup();
    return 0;
}
