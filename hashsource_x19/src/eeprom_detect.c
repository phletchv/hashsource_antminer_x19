/*
 * X19 EEPROM Detection and Information Display (MAXIMUM SPEED)
 *
 * Reads EEPROM data from hashboard chains via FPGA I2C controller.
 * ZERO DELAYS - Pure busy-wait polling for maximum throughput.
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
 *
 * Performance optimizations:
 * - ZERO delays in I2C polling (busy-wait on FPGA register)
 * - ZERO delays between byte reads
 * - ZERO delays between chains
 * - Pure FPGA hardware-limited throughput
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

// Timing (maximum speed - no delays)
#define I2C_POLL_TIMEOUT    1000000       // High iteration count for busy-wait polling

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
//   Chain 0: byte addresses 0x000-0x0FF (EEPROM at 0x001-0x0FF, data starts at 0x001)
//   Chain 1: byte addresses 0x100-0x1FF (EEPROM at 0x101-0x1FF, data starts at 0x101)
//   Chain 2: byte addresses 0x200-0x2FF (EEPROM at 0x201-0x2FF, data starts at 0x201)
// Note: Only 12 bits of addressing (0x000-0xFFF) are used in the I2C command
static const uint16_t CHAIN_EEPROM_OFFSET[] = {
    0x0000,  // Chain 0: EEPROM starts at byte 0x000
    0x0100,  // Chain 1: EEPROM starts at byte 0x100
    0x0200   // Chain 2: EEPROM starts at byte 0x200
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

    // Poll I2C register for response (busy-wait, no delays)
    for (int i = 0; i < I2C_POLL_TIMEOUT; i++) {
        response = g_fpga_regs[REG_I2C_CTRL];

        // Check bit 31 for data ready (from bmminer: if (v4 < 0))
        if (response & 0x80000000) {
            // Data is ready, extract byte from bits 0-7
            *data = (uint8_t)(response & 0xFF);
            return 0;
        }
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

        // No delay needed - FPGA I2C controller handles timing internally
    }

    return 0;
}

//==============================================================================
// XXTEA Decryption (from bmminer FUN_00018d98)
//==============================================================================

// XOR decryption keys (extracted from stock and HulkOS firmware)
// Key set 1: "uohzoahzuhidkgna" (Bitmain stock)
static const uint32_t XOR_KEY1[4] = {
    0x7A686F75, 0x7A68616F, 0x64696875, 0x616E676B
};

// Key set 2: "iewgoahznehzwstg" (HulkOS firmware)
static const uint32_t XOR_KEY2[4] = {
    0x67776569, 0x7A68616F, 0x7A68656E, 0x67747377
};

// XOR-based decryption (simpler than XXTEA, mode 3 in bmminer)
static void xor_decrypt(uint32_t *data, uint32_t len) {
    uint32_t n = len >> 2;  // Number of 32-bit words

    for (uint32_t i = 0; i < n; i++) {
        data[i] ^= XOR_KEY2[i % 4];  // Try HulkOS key
    }
}

// XXTEA decryption keys (extracted from S19 Pro bmminer at address 0x7E2AC)
// CRITICAL: Key index 1 is used for S19 Pro EEPROM decryption (not key 0!)
// Discovered via S19 XP single_board_test binary analysis
// Key 1: "uileynimggnagnau" (0x75 0x69 0x6c 0x65 0x79 0x6e 0x69 0x6d 0x67 0x67 0x6e 0x61 0x67 0x6e 0x61 0x75)
static const uint32_t XXTEA_KEY_S19PRO[4] = {
    0x656C6975,  // "uile" (little-endian)
    0x6D696E79,  // "ynim" (little-endian)
    0x616E6767,  // "ggna" (little-endian)
    0x75616E67   // "gnau" (little-endian)
};

#define XXTEA_DELTA 0x9E3779B9

// XXTEA decrypt - exact implementation from S19 XP single_board_test (xxtea_decode-000879b4)
static void xxtea_decrypt(uint32_t *data, uint32_t len) {
    uint32_t n = len >> 2;  // Number of 32-bit words

    if (n < 2) {
        return;  // Need at least 2 words for XXTEA
    }

    uint32_t rounds = 6 + 52 / n;
    uint32_t sum = rounds * XXTEA_DELTA;
    uint32_t y = data[0];
    uint32_t z, mx;

    for (uint32_t r = 0; r < rounds; r++) {
        uint32_t e = (sum >> 2) & 3;

        // Decrypt from end to start
        for (uint32_t p = n - 1; p > 0; p--) {
            z = data[p - 1];
            mx = ((z ^ XXTEA_KEY_S19PRO[e ^ (p & 3)]) + (sum ^ y)) ^
                 ((z >> 5 ^ y << 2) + (z << 4 ^ y >> 3));
            data[p] -= mx;
            y = data[p];
        }

        // Decrypt first element
        z = data[n - 1];
        mx = ((z ^ XXTEA_KEY_S19PRO[e]) + (sum ^ y)) ^
             ((z >> 5 ^ y << 2) + (z << 4 ^ y >> 3));
        data[0] -= mx;
        y = data[0];

        sum += 0x61c88647;  // Add DELTA inverse (-DELTA in unsigned)
    }
}

//==============================================================================
// EEPROM Data Structures
//==============================================================================

typedef struct {
    uint16_t pcb_version;
    uint16_t bom_version;
    uint16_t freq_level0;  // Minimum frequency
    uint16_t freq_level1;  // Maximum frequency
    uint8_t format;        // EEPROM format version (1-4)
    uint8_t valid;         // Parsing success flag
} eeprom_info_t;

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
// EEPROM Parsing (based on bmminer FUN_0001740c)
//==============================================================================

static int parse_eeprom(const uint8_t *raw_data, eeprom_info_t *info) {
    uint8_t decrypted[256];
    uint32_t temp_buf[64];  // Temp buffer for decryption
    uint32_t *data_words;

    memset(info, 0, sizeof(eeprom_info_t));
    memcpy(decrypted, raw_data, 256);

    // Check header byte (must be 0x11)
    if ((decrypted[0] & 0xF0) != 0x10 || (decrypted[0] & 0x0F) != 0x01) {
        fprintf(stderr, "Invalid EEPROM header: 0x%02X (expected 0x11)\n", decrypted[0]);
        return -1;
    }

    // Get data length
    uint8_t data_len = decrypted[1];
    if (data_len < 2 || data_len > 250) {
        fprintf(stderr, "Invalid EEPROM data length: %d\n", data_len);
        return -1;
    }

    // Calculate encryption length (must be 8-byte aligned, add 5 for alignment)
    uint32_t enc_len = (data_len + 5) & ~7;

    // Copy encrypted data to temp buffer for decryption
    memcpy(temp_buf, &decrypted[2], enc_len);

    // Use XXTEA decryption (verified working with Key 1 from S19 XP analysis)
    xxtea_decrypt(temp_buf, enc_len);

    // Copy decrypted data back (only data_len - 2 bytes, excluding header bytes already copied)
    memcpy(&decrypted[2], temp_buf, data_len - 2);

    // Debug output
    printf("Data length: %d, Enc length: %d\n", data_len, enc_len);
    printf("First 32 decrypted bytes (XXTEA): ");
    for (int i = 0; i < 32 && i < data_len; i++) {
        printf("%02X ", decrypted[2 + i]);
    }
    printf("\n");

    // Get format type from first decrypted byte (byte 2 after header, offset 0 in decrypted payload)
    info->format = decrypted[2];
    printf("Format byte: 0x%02X (%d)\n", info->format, info->format);

    // Parse based on format
    // Format 3: S19 Pro standard format (discovered via successful decryption)
    // Decrypted structure (offset relative to decrypted[2], i.e., after 0x11 0x4A header):
    //   Offset 0x00: Format (0x03)
    //   Offset 0x01-0x1E: Serial number (30 bytes ASCII)
    //   Offset 0x22: Additional data
    //   Offset 0x33-0x34: PCB version (little-endian uint16)
    //   Offset 0x35-0x36: BOM version (little-endian uint16)
    if (info->format == 3) {
        // Format 3 offsets (verified with real EEPROM data)
        // PCB at decrypted[2 + 0x33] = raw[0x35-0x36] (after adding 2-byte header)
        info->pcb_version = decrypted[2 + 0x33] | (decrypted[2 + 0x34] << 8);  // Little-endian
        info->bom_version = decrypted[2 + 0x35] | (decrypted[2 + 0x36] << 8);  // Little-endian
        // TODO: Find frequency offset in format 3 (not yet discovered)
        info->freq_level0 = 525;  // Default S19 Pro frequency
        info->freq_level1 = 525;
    } else if (info->format >= 1 && info->format <= 2) {
        // Format 1-2 offsets (from bmminer decompilation, not yet tested)
        info->pcb_version = (decrypted[0x2F] << 8) | decrypted[0x30];
        info->bom_version = (decrypted[0x31] << 8) | decrypted[0x32];
        info->freq_level0 = (decrypted[0x35] << 8) | decrypted[0x36];
        info->freq_level1 = (decrypted[0x37] << 8) | decrypted[0x38];
    } else if (info->format == 4) {
        // Format 4 (not yet tested)
        info->pcb_version = (decrypted[0x33] << 8) | decrypted[0x35];
        info->bom_version = (decrypted[0x36] << 8) | decrypted[0x37];
        info->freq_level0 = (decrypted[0x3A] << 8) | decrypted[0x3B];
        info->freq_level1 = (decrypted[0x3C] << 8) | decrypted[0x3D];
    } else {
        fprintf(stderr, "Unsupported EEPROM format: %d (0x%02X)\n", info->format, info->format);
        return -1;
    }

    info->valid = 1;
    return 0;
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

    // Read EEPROM from each detected chain (maximum speed - no delays)
    for (int chain = 0; chain < MAX_CHAINS; chain++) {
        if (!(hash_on_plug & (1 << chain))) {
            continue;  // Skip undetected chains
        }

        memset(eeprom_data, 0xFF, sizeof(eeprom_data));

        if (eeprom_read(chain, eeprom_data, EEPROM_SIZE) == 0) {
            eeprom_info_t info;

            // Display raw hex dump
            display_eeprom(chain, eeprom_data);

            // Parse and display decoded information
            if (parse_eeprom(eeprom_data, &info) == 0) {
                printf("Chain [%d] PCB Version: 0x%04x\n", chain, info.pcb_version);
                printf("Chain [%d] BOM Version: 0x%04x\n", chain, info.bom_version);
                printf("Chain [%d] Format: %d\n", chain, info.format);
                printf("Chain [%d] Min Frequency: %d MHz\n", chain, info.freq_level0);
                printf("Chain [%d] Max Frequency: %d MHz\n", chain, info.freq_level1);
                printf("\n");
            } else {
                fprintf(stderr, "Failed to parse EEPROM data from chain %d\n", chain);
            }
        } else {
            fprintf(stderr, "Failed to read EEPROM from chain %d\n", chain);
        }
    }

    fpga_cleanup();
    return 0;
}
