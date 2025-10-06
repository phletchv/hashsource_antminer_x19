/*
 * X19 EEPROM Detection and Information Display
 *
 * Reads EEPROM data from hashboard chains via FPGA I2C controller.
 * Based on reverse engineering of bmminer EEPROM access (FUN_00049e8c).
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

// I2C controller register - varies by hardware variant (from bmminer lookup tables)
// Variant 1: 0x30 (index 0xc in DAT_0007f130)
// Variant 2: 0x34 (index 0xc in DAT_0007ee48)
#define REG_I2C_CTRL_V1     (0x30 / 4)    // I2C control register variant 1
#define REG_I2C_CTRL_V2     (0x34 / 4)    // I2C control register variant 2
#define REG_HASH_ON_PLUG    (0x08 / 4)    // Chain detection register

// I2C command format (from bmminer FUN_00049e8c:40)
// bits 26-27: master/chain ID
// bits 24-25: 0x3 = read operation
// bits 20-23: slave address high nibble (0xA for EEPROM 0xA0)
// bits 15-17: slave address low bits (0x0 for EEPROM 0xA0)
// bits 8-15:  register/byte address
#define I2C_READ_FLAGS      0x03000000    // Read operation flags (bits 24-25)
#define I2C_STATUS_READY    0x01          // I2C ready for next operation

// EEPROM slave address (7-bit: 0x50, 8-bit write: 0xA0, 8-bit read: 0xA1)
#define EEPROM_SLAVE_ADDR   0xA0

// EEPROM configuration
#define EEPROM_SIZE         256
#define EEPROM_MARKER_V1    0x5A
#define EEPROM_MARKER_V5    0x5A

// Max chains
#define MAX_CHAINS          3

// Timing
#define I2C_TIMEOUT_US      100000        // 100ms timeout for I2C operations
#define I2C_RETRY_DELAY_US  1000          // 1ms delay between retries

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

// Global I2C register variant (auto-detected)
static int g_i2c_reg = -1;

// Read single byte from EEPROM via FPGA I2C
// Based on bmminer FUN_000498a0 - polls I2C register for response with bit 31 as ready flag
static int i2c_read_byte(int chain_id, uint8_t reg_addr, uint8_t *data) {
    uint32_t cmd;
    uint32_t response;
    // Each chain has different I2C slave address: 0xA0 + (2 * chain_id)
    // From bmminer line 38739: (2 * chain_id) | 0xA0
    uint8_t slave_addr = EEPROM_SLAVE_ADDR + (2 * chain_id);
    uint8_t slave_high = (slave_addr >> 4) & 0xF;   // High nibble
    uint8_t slave_low = (slave_addr >> 1) & 0x7;    // Low 3 bits

    // Build I2C command (matching bmminer FUN_00049e8c:40)
    cmd = ((uint32_t)chain_id << 26) |      // Master/chain ID
          I2C_READ_FLAGS |                   // Read operation (0x3 << 24)
          ((uint32_t)slave_high << 20) |     // Slave address high nibble
          ((uint32_t)slave_low << 15) |      // Slave address low bits
          ((uint32_t)reg_addr << 8);         // Register/byte address

    // Auto-detect I2C register variant on first call
    if (g_i2c_reg == -1) {
        printf("Auto-detecting I2C controller register variant...\n");

        // Try variant 1 (0x30)
        printf("  Trying variant 1 (reg 0x%02X)...\n", REG_I2C_CTRL_V1 * 4);
        g_fpga_regs[REG_I2C_CTRL_V1] = cmd;
        usleep(10000);
        response = g_fpga_regs[REG_I2C_CTRL_V1];
        if (response & 0x80000000) {
            printf("  Variant 1 detected! Using reg 0x%02X\n", REG_I2C_CTRL_V1 * 4);
            g_i2c_reg = REG_I2C_CTRL_V1;
            *data = (uint8_t)(response & 0xFF);
            return 0;
        }

        // Try variant 2 (0x34)
        printf("  Trying variant 2 (reg 0x%02X)...\n", REG_I2C_CTRL_V2 * 4);
        g_fpga_regs[REG_I2C_CTRL_V2] = cmd;
        usleep(10000);
        response = g_fpga_regs[REG_I2C_CTRL_V2];
        if (response & 0x80000000) {
            printf("  Variant 2 detected! Using reg 0x%02X\n", REG_I2C_CTRL_V2 * 4);
            g_i2c_reg = REG_I2C_CTRL_V2;
            *data = (uint8_t)(response & 0xFF);
            return 0;
        }

        fprintf(stderr, "  Failed to detect I2C variant (V1: 0x%08X, V2: 0x%08X)\n",
                g_fpga_regs[REG_I2C_CTRL_V1], g_fpga_regs[REG_I2C_CTRL_V2]);
        return -1;
    }

    // Use detected variant
    g_fpga_regs[g_i2c_reg] = cmd;

    // Poll I2C register for response (from bmminer FUN_000498a0)
    // Timeout: 0x259 iterations * 5ms = ~3 seconds
    int max_retries = 0x259;
    for (int i = 0; i < max_retries; i++) {
        response = g_fpga_regs[g_i2c_reg];

        // Check bit 31 for data ready (from bmminer: -(local_14 >> 0x1f) != 0)
        if (response & 0x80000000) {
            // Data is ready, extract byte from bits 0-7
            *data = (uint8_t)(response & 0xFF);
            return 0;
        }

        usleep(5000);  // 5ms delay between polls
    }

    // Timeout
    fprintf(stderr, "I2C timeout at chain %d, addr 0x%02X (last response: 0x%08X)\n",
            chain_id, reg_addr, response);
    return -1;
}

// Read full EEPROM from chain
static int eeprom_read(int chain_id, uint8_t *buffer, size_t size) {
    printf("Reading EEPROM from chain %d using FPGA I2C controller...\n", chain_id);

    for (size_t i = 0; i < size; i++) {
        if (i2c_read_byte(chain_id, (uint8_t)i, &buffer[i]) < 0) {
            fprintf(stderr, "Failed to read EEPROM byte %zu from chain %d\n", i, chain_id);
            return -1;
        }

        // Progress indicator every 64 bytes
        if ((i % 64) == 63) {
            printf("  Read %zu/%zu bytes...\n", i + 1, size);
        }

        // Small delay between reads
        if ((i % 16) == 15) {
            usleep(5000);  // 5ms every 16 bytes
        }
    }

    return 0;
}

//==============================================================================
// EEPROM Data Display
//==============================================================================

static void hexdump(const char *prefix, const uint8_t *data, size_t len) {
    printf("%s\n", prefix);

    for (size_t i = 0; i < len; i += 16) {
        printf("0x%04zX ", i);

        // Print hex bytes
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                printf("%02X ", data[i + j]);
            } else {
                printf("   ");
            }

            if (j == 7) {
                printf("  ");
            }
        }

        printf("  ");

        // Print ASCII representation
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }

        printf("\n");
    }
}

static void parse_eeprom(int chain_id, const uint8_t *data) {
    printf("\n=== Chain %d EEPROM Information ===\n", chain_id);

    // Check format version
    uint8_t format = data[0];
    printf("Format Version: 0x%02X", format);

    if (format == 0xFF) {
        printf(" (BLANK/ERASED)\n");
        printf("\nNote: This EEPROM appears to be blank. This could mean:\n");
        printf("  - Test/development hashboard\n");
        printf("  - New/unprogrammed board\n");
        printf("  - Erased EEPROM\n");
        printf("  - I2C communication error\n");
        return;
    }

    if (format == 0x01) {
        printf(" (v1 - Production)\n");
    } else if (format == 0x04) {
        printf(" (v4 - Test)\n");
    } else if (format == 0x05) {
        printf(" (v5 - Test)\n");
    } else if ((format & 0xF0) == 0x10) {
        uint8_t major = (format >> 4) & 0xF;
        uint8_t minor = format & 0xF;
        printf(" (v%d.%d)\n", major, minor);
    } else {
        printf(" (unknown)\n");
    }

    // Check marker
    uint8_t marker = data[EEPROM_SIZE - 1];
    printf("Marker: 0x%02X %s\n", marker,
           (marker == EEPROM_MARKER_V1 || marker == EEPROM_MARKER_V5) ? "(valid)" : "(INVALID!)");

    // Data length (for v1 format)
    if (format == 0x01 || format == 0x11) {
        uint8_t data_len = data[1];
        printf("Data Length: %d bytes\n", data_len);

        // Try to extract board serial (bytes 1-10 in v1 format)
        if (EEPROM_SIZE >= 11) {
            char serial[11];
            memcpy(serial, &data[1], 10);
            serial[10] = '\0';

            // Check if it's printable ASCII
            bool is_printable = true;
            for (int i = 0; i < 10 && serial[i] != '\0'; i++) {
                if (serial[i] < 32 || serial[i] > 126) {
                    is_printable = false;
                    break;
                }
            }

            if (is_printable && serial[0] != '\0') {
                printf("Board Serial: %s\n", serial);
            }
        }
    }

    // Hex dump of entire EEPROM
    printf("\n");
    hexdump("EEPROM Data:", data, EEPROM_SIZE);
    printf("\n");
}

//==============================================================================
// Main
//==============================================================================

int main(void) {
    uint8_t eeprom_data[EEPROM_SIZE];
    uint32_t hash_on_plug;

    printf("===============================================\n");
    printf("  HashSource X19 - EEPROM Detection Tool\n");
    printf("===============================================\n\n");

    // Initialize FPGA
    printf("Initializing FPGA...\n");
    if (fpga_init() < 0) {
        fprintf(stderr, "Failed to initialize FPGA\n");
        return 1;
    }

    // Check which chains are detected
    hash_on_plug = g_fpga_regs[REG_HASH_ON_PLUG];
    printf("Chain detection (HASH_ON_PLUG @ 0x008): 0x%08X\n", hash_on_plug);

    int detected_chains = 0;
    for (int i = 0; i < MAX_CHAINS; i++) {
        if (hash_on_plug & (1 << i)) {
            detected_chains++;
            printf("  Chain %d: DETECTED\n", i);
        } else {
            printf("  Chain %d: not detected\n", i);
        }
    }

    if (detected_chains == 0) {
        printf("\nNo chains detected. Make sure hashboards are connected.\n");
        fpga_cleanup();
        return 1;
    }

    printf("\n");

    // Try to read EEPROM from each detected chain
    for (int chain = 0; chain < MAX_CHAINS; chain++) {
        if (!(hash_on_plug & (1 << chain))) {
            continue;  // Skip undetected chains
        }

        printf("\n==============================================\n");
        printf("  Chain %d\n", chain);
        printf("==============================================\n\n");

        memset(eeprom_data, 0xFF, sizeof(eeprom_data));

        if (eeprom_read(chain, eeprom_data, EEPROM_SIZE) == 0) {
            parse_eeprom(chain, eeprom_data);
        } else {
            fprintf(stderr, "\nFailed to read EEPROM from chain %d\n", chain);
            fprintf(stderr, "This could mean:\n");
            fprintf(stderr, "  - EEPROM chip not present on hashboard\n");
            fprintf(stderr, "  - FPGA I2C communication error\n");
            fprintf(stderr, "  - Incorrect I2C addressing\n");
        }
    }

    fpga_cleanup();
    printf("\nDone.\n");
    return 0;
}
