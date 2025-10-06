/*
 * X19 EEPROM Detection and Information Display
 *
 * Reads EEPROM data from hashboard chains via FPGA I2C controller.
 * Matches stock bmminer behavior exactly.
 *
 * CRITICAL DISCOVERY (via FPGA log analysis + strace):
 * - ALL chains use the SAME I2C slave address (0xA0)
 * - Chain selection is done via BYTE ADDRESS OFFSET, not slave address
 * - Uses 12-bit byte addressing (bits 8-19 of I2C command)
 * - Chain 0: bytes 0x0000-0x00FF, Chain 1: bytes 0x1200-0x12FF, Chain 2: bytes 0x2500-0x25FF
 *
 * Based on reverse engineering:
 * - FPGA log analysis (docs/bmminer_fpga_init_68_7C_2E_2F_A4_D9.log)
 * - strace of stock bmminer (shows only /dev/axi_fpga_dev usage)
 * - bmminer FUN_00049e8c (EEPROM I2C operations)
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

// I2C controller register - SINGLE REGISTER FOR ALL CHAINS!
// Chain selection is done via bits 26-27 in the command word
#define REG_I2C_CTRL        (0x030 / 4)   // I2C control register (shared by all chains)
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

// EEPROM byte address offsets per chain (discovered from FPGA log analysis)
// CRITICAL DISCOVERY: Stock firmware uses the SAME slave address (0xA0) for all chains,
// but different byte address ranges to access each chain's EEPROM.
// Analysis of docs/bmminer_fpga_init_68_7C_2E_2F_A4_D9.log shows:
//   Chain 0: byte addresses 0x0000-0x11DB (EEPROM at 0x0000-0x00FF)
//   Chain 1: byte addresses 0x11EB-0x244E (EEPROM at 0x1200-0x12FF)
//   Chain 2: byte addresses 0x246C-0x2FFF (EEPROM at 0x2500-0x25FF)
static const uint16_t CHAIN_EEPROM_OFFSET[] = {
    0x0000,  // Chain 0: EEPROM starts at byte 0x0000
    0x1200,  // Chain 1: EEPROM starts at byte 0x1200
    0x2500   // Chain 2: EEPROM starts at byte 0x2500
};

// Read single byte from EEPROM via FPGA I2C
// Uses 12-bit byte addressing (bits 8-19) instead of slave address variation
static int i2c_read_byte(int chain_id, uint8_t reg_addr, uint8_t *data) {
    uint32_t cmd;
    uint32_t response;

    if (chain_id < 0 || chain_id >= MAX_CHAINS) {
        fprintf(stderr, "Invalid chain_id: %d\n", chain_id);
        return -1;
    }

    // ALL chains use the same slave address (0xA0)
    // Chain selection is done via byte address offset, not slave address!
    const uint8_t slave_addr = 0xA0;  // (16 * DEVICE_TYPE_EEPROM) = 0xA0

    // Calculate full 12-bit byte address: base_offset + byte_index
    uint16_t full_byte_addr = CHAIN_EEPROM_OFFSET[chain_id] + reg_addr;

    // Build I2C command with 12-bit byte addressing (bits 8-19):
    // - Bits 26-27: always 0 (master ID - stock firmware always uses 0)
    // - Bits 24-25: 0x3 (read operation)
    // - Bits 20-23: 0xA (slave address high nibble, always 0xA0)
    // - Bits 16-19: byte address bits 8-11 (upper nibble of 12-bit address)
    // - Bits 8-15:  byte address bits 0-7 (lower byte of 12-bit address)
    // - Bits 0-7:   response data from previous read (ignored for write)
    cmd = (0 << 26) |                                           // Master ID always 0
          I2C_READ_FLAGS |                                      // Read operation (0x3 << 24)
          (((uint32_t)slave_addr >> 4) << 20) |                 // Slave high nibble (0xA)
          ((((uint32_t)full_byte_addr >> 8) & 0xF) << 16) |     // Byte addr bits 8-11 (bits 16-19, masked to 4 bits)
          (((uint32_t)full_byte_addr & 0xFF) << 8);             // Byte addr bits 0-7 (bits 8-15)

    // Debug: show first command for each chain
    static int debug_shown[MAX_CHAINS] = {0};
    if (!debug_shown[chain_id]) {
        printf("Chain %d: slave_addr=0x%02X, byte_offset=0x%04X, first_cmd=0x%08X\n",
               chain_id, slave_addr, full_byte_addr, cmd);
        debug_shown[chain_id] = 1;
    }

    // Write I2C command to FPGA register 0x030 (shared by all chains)
    g_fpga_regs[REG_I2C_CTRL] = cmd;

    // Poll I2C register for response (from bmminer FUN_000498a0)
    // Timeout: 0x259 iterations * 5ms = ~3 seconds
    for (int i = 0; i < I2C_POLL_TIMEOUT; i++) {
        response = g_fpga_regs[REG_I2C_CTRL];

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
