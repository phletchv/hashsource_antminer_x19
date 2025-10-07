/*
 * Bitmain Antminer S19 Pro EEPROM Reader and Decoder
 *
 * Reads and decrypts EEPROM data from hashboard chains via FPGA I2C controller.
 *
 * ARCHITECTURE:
 * - Hardware: Xilinx Zynq-7007S SoC with custom FPGA bitstream
 * - I2C: FPGA-based controller at register 0x030 (shared across all chains)
 * - Addressing: 12-bit byte addressing (0x000-0xFFF) differentiates chains
 * - Encryption: XXTEA with 128-bit key (Key 1 from bmminer at 0x7E2AC)
 *
 * DISCOVERY PROCESS:
 * 1. Reverse engineered stock bmminer binary (ARM, stripped)
 *    - Ghidra/IDA Pro decompilation of FUN_00049e8c (I2C), FUN_0001740c (EEPROM parse)
 *    - Found 4 XXTEA keys at memory address 0x7E2AC
 *    - Identified key selection logic at FUN_00018c40
 *
 * 2. Analyzed S19 XP single_board_test binary
 *    - Clearer XXTEA implementation (xxtea_decode-000879b4.c)
 *    - Confirmed algorithm: rounds = 6 + 52/n, delta = 0x9E3779B9
 *
 * 3. Tested all 4 keys - Key 1 ("uileynimggnagnau") successfully decrypted
 *    - Format byte: 0x03 (valid range: 1-4)
 *    - PCB version 0x011E found at offset 0x33 (little-endian)
 *    - Verified on real hardware: 3 chains, all format 3
 *
 * REFERENCES:
 * - Stock firmware: Antminer-S19-Pro-merge-release-20221226124238
 * - Bmminer binary: 2f464d0989b763718a6fbbdee35424ae (ARM 32-bit)
 * - S19 XP test binary: single_board_test-c254c833bb83d47186d8419067a0cc3c
 * - XXTEA spec: https://en.wikipedia.org/wiki/XXTEA (Block TEA, Corrected)
 * - I2C addressing: docs/bmminer_fpga_init_68_7C_2E_2F_A4_D9.log
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

//==============================================================================
// Hardware Configuration
//==============================================================================

#define FPGA_REG_BASE           0x40000000
#define FPGA_REG_SIZE           5120
#define REG_I2C_CTRL            (0x030 / 4)     // Shared I2C controller
#define REG_HASH_ON_PLUG        (0x008 / 4)     // Chain detection

#define I2C_SLAVE_ADDR          0xA0            // All chains use same address
#define I2C_READ_FLAGS          0x03000000      // Bits 24-25: read operation
#define I2C_READY_BIT           0x80000000      // Bit 31: data ready
#define I2C_POLL_TIMEOUT        1000000

#define EEPROM_SIZE             256
#define EEPROM_HEADER           0x11
#define EEPROM_TRAILER          0x5A
#define MAX_CHAINS              3

// Chain byte address offsets (discovered via FPGA log analysis)
// Source: docs/bmminer_fpga_init_68_7C_2E_2F_A4_D9.log
static const uint16_t CHAIN_OFFSET[MAX_CHAINS] = { 0x0000, 0x0100, 0x0200 };

//==============================================================================
// XXTEA Decryption
//==============================================================================

// XXTEA key extracted from S19 Pro bmminer binary at address 0x7E2AC
// Source: bmminer-2f464d0989b763718a6fbbdee35424ae, IDA Pro disassembly
// Key index: 1 (of 4 available keys)
// ASCII: "uileynimggnagnau"
// Discovery: Tested all 4 keys via S19 XP single_board_test analysis
static const uint32_t XXTEA_KEY[4] = {
    0x656C6975,  // "uile" (little-endian)
    0x6D696E79,  // "ynim"
    0x616E6767,  // "ggna"
    0x75616E67   // "gnau"
};

#define XXTEA_DELTA             0x9E3779B9      // Golden ratio constant
#define XXTEA_DELTA_INV         0x61C88647      // -DELTA in unsigned arithmetic

/*
 * XXTEA Block Cipher Decryption (Corrected Block TEA)
 *
 * Algorithm: XXTEA (eXtended Tiny Encryption Algorithm)
 * Key size: 128 bits (4 × 32-bit words)
 * Block size: Variable (minimum 64 bits, 2 × 32-bit words)
 * Rounds: 6 + 52/n (where n = number of 32-bit words)
 *
 * Source: S19 XP single_board_test xxtea_decode-000879b4.c
 * Reference: https://en.wikipedia.org/wiki/XXTEA
 * Paper: "Correction to XTEA" by Needham and Wheeler (1998)
 */
static void xxtea_decrypt(uint32_t * restrict data, size_t len) {
    const size_t n = len / sizeof(uint32_t);
    if (n < 2) return;  // Minimum 2 words required

    const uint32_t rounds = 6 + 52 / n;
    uint32_t sum = rounds * XXTEA_DELTA;
    uint32_t y = data[0];

    for (uint32_t r = 0; r < rounds; r++) {
        const uint32_t e = (sum >> 2) & 3;

        // Decrypt in reverse order (end to start)
        for (size_t p = n - 1; p > 0; p--) {
            const uint32_t z = data[p - 1];
            const uint32_t mx = ((z ^ XXTEA_KEY[e ^ (p & 3)]) + (sum ^ y)) ^
                                ((z >> 5 ^ y << 2) + (z << 4 ^ y >> 3));
            data[p] -= mx;
            y = data[p];
        }

        // Decrypt first element
        const uint32_t z = data[n - 1];
        const uint32_t mx = ((z ^ XXTEA_KEY[e]) + (sum ^ y)) ^
                            ((z >> 5 ^ y << 2) + (z << 4 ^ y >> 3));
        data[0] -= mx;
        y = data[0];

        sum += XXTEA_DELTA_INV;  // Equivalent to: sum -= XXTEA_DELTA
    }
}

//==============================================================================
// EEPROM Data Structure
//==============================================================================

// EEPROM info structure
typedef struct {
    uint8_t  header_version;        // Format/header version (1-4)
    char     board_serial_no[18];   // Board serial number (17 bytes + null)
    char     chip_die[3];           // Chip die code (2 bytes + null)
    char     chip_marking[11];      // Chip marking/model (10 bytes + null)
    uint8_t  chip_bin;              // Chip bin (1-9)
    uint32_t ft_version;            // FT program version (big-endian u32)
    uint16_t pcb_version;           // PCB hardware revision (big-endian)
    uint16_t bom_version;           // BOM version (big-endian)
    uint16_t default_freq;          // Default frequency in MHz (direct value, NOT lookup code)
    bool     valid;                 // Successfully parsed
} eeprom_info_t;

//==============================================================================
// FPGA I2C Interface
//==============================================================================

static volatile uint32_t *g_fpga_regs = NULL;

static int fpga_init(void) {
    const int fd = open("/dev/axi_fpga_dev", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open /dev/axi_fpga_dev: %s\n", strerror(errno));
        fprintf(stderr, "Hint: Ensure bitmain_axi.ko kernel module is loaded\n");
        return -1;
    }

    g_fpga_regs = mmap(NULL, FPGA_REG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (g_fpga_regs == MAP_FAILED) {
        fprintf(stderr, "Error: mmap failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void fpga_cleanup(void) {
    if (g_fpga_regs && g_fpga_regs != MAP_FAILED) {
        munmap((void *)g_fpga_regs, FPGA_REG_SIZE);
    }
}

/*
 * Read single byte from EEPROM via FPGA I2C controller
 *
 * I2C Command Format (32-bit register at 0x030):
 *   Bits 26-27: Master ID (always 0 for S19 Pro)
 *   Bits 24-25: Operation (0x3 = read)
 *   Bits 20-23: Slave address high nibble (0xA)
 *   Bits 16-19: Byte address bits 8-11
 *   Bits 8-15:  Byte address bits 0-7
 *   Bits 0-7:   Response data (after bit 31 set)
 *
 * Source: bmminer FUN_00049e8c (I2C operations)
 */
static int i2c_read_byte(int chain_id, uint8_t reg_addr, uint8_t *data) {
    if (chain_id < 0 || chain_id >= MAX_CHAINS || !data) {
        return -1;
    }

    const uint16_t byte_addr = CHAIN_OFFSET[chain_id] + reg_addr;
    const uint32_t cmd = I2C_READ_FLAGS |
                         ((I2C_SLAVE_ADDR >> 4) << 20) |
                         (((byte_addr >> 8) & 0xF) << 16) |
                         ((byte_addr & 0xFF) << 8);

    g_fpga_regs[REG_I2C_CTRL] = cmd;

    // Busy-wait polling (no delays - FPGA handles timing)
    for (int i = 0; i < I2C_POLL_TIMEOUT; i++) {
        const uint32_t response = g_fpga_regs[REG_I2C_CTRL];
        if (response & I2C_READY_BIT) {
            *data = response & 0xFF;
            return 0;
        }
    }

    return -1;  // Timeout
}

static int eeprom_read(int chain_id, uint8_t *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (i2c_read_byte(chain_id, i, &buffer[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

//==============================================================================
// EEPROM Parsing
//==============================================================================

/*
 * Parse and decrypt EEPROM data
 *
 * Structure (Format 3, verified on S19 Pro):
 *   Byte 0:       0x11 (magic header)
 *   Byte 1:       Data length (0x4A = 74 bytes or 0x42 = 66 bytes)
 *   Bytes 2-N:    Encrypted payload (XXTEA)
 *   Byte 255:     0x5A (trailer marker)
 *
 * Decrypted payload (Format 3):
 *   Offset 0:     Format byte (0x03)
 *   Offset 1-17:  Serial number (17 bytes ASCII)
 *   Offset 18-19: Chip die (2 bytes)
 *   Offset 20-29: Chip marking (10 bytes)
 *   Offset 33:    Chip bin
 *   Offset 34-37: FT version (big-endian uint32)
 *   Offset 45-46: PCB version (big-endian uint16)
 *   Offset 47-48: BOM version (big-endian uint16)
 *   Variable offset based on data_len:
 *     0x42 → offset=5, 0x4A → offset=0
 *   Offset 56-off: Chip tech (2 bytes)
 *   Offset 58-off: Voltage raw (big-endian uint16, multiply by 10 for mV)
 *   Offset 60-off: Frequency (big-endian uint16, MHz)
 */
static int parse_eeprom(const uint8_t *raw_data, eeprom_info_t *info) {
    memset(info, 0, sizeof(*info));

    // Validate header
    if (raw_data[0] != EEPROM_HEADER) {
        fprintf(stderr, "Error: Invalid EEPROM header: 0x%02X\n", raw_data[0]);
        return -1;
    }

    const uint8_t data_len = raw_data[1];
    if (data_len < 2 || data_len > 250) {
        fprintf(stderr, "Error: Invalid data length: %u\n", data_len);
        return -1;
    }

    // Decrypt payload (XXTEA requires 8-byte alignment)
    const size_t enc_len = (data_len + 5) & ~7;
    uint32_t decrypted[64] = {0};
    memcpy(decrypted, &raw_data[2], enc_len);
    xxtea_decrypt(decrypted, enc_len);

    // Parse fields
    const uint8_t *payload = (const uint8_t *)decrypted;

    // Variable offset for chip_tech/voltage/frequency fields
    const int var_offset = (data_len == 0x42) ? 5 : 0;

    // Header version (format byte)
    info->header_version = payload[0];

    // Board serial number: 17 bytes ASCII at offset 1-17
    memcpy(info->board_serial_no, &payload[1], 17);
    info->board_serial_no[17] = '\0';

    // Chip die: 2 bytes at offset 18-19
    memcpy(info->chip_die, &payload[18], 2);
    info->chip_die[2] = '\0';

    // Chip marking: 10 bytes at offset 20-29
    memcpy(info->chip_marking, &payload[20], 10);
    info->chip_marking[10] = '\0';

    // Chip bin: 1 byte at offset 33
    info->chip_bin = payload[33];

    // FT version: 4 bytes at offset 34-37 (big-endian u32)
    info->ft_version = ((uint32_t)payload[34] << 24) |
                       ((uint32_t)payload[35] << 16) |
                       ((uint32_t)payload[36] << 8) |
                       ((uint32_t)payload[37]);

    // PCB version: 2 bytes at offset 45-46 (big-endian u16)
    info->pcb_version = (payload[45] << 8) | payload[46];

    // BOM version: 2 bytes at offset 47-48 (big-endian u16)
    info->bom_version = (payload[47] << 8) | payload[48];

    // Default frequency: 2 bytes at offset (58 - var_offset), big-endian
    // This is a DIRECT value in MHz (e.g., 525 = 525 MHz), NOT a lookup table index
    // Bitmain's bmminer reads this at offset 0x23 and prints "min freq in eeprom = %d"
    const int freq_offset = 58 - var_offset;
    info->default_freq = (payload[freq_offset] << 8) | payload[freq_offset + 1];

    info->valid = true;
    return 0;
}

static void display_eeprom_hex(int chain_id, const uint8_t *data) {
    printf("[chain %d]\n", chain_id);
    for (size_t i = 0; i < EEPROM_SIZE; i += 16) {
        printf("0x%04zX ", i);
        for (size_t j = 0; j < 8; j++)  printf("%02X ", data[i + j]);
        printf("  ");
        for (size_t j = 8; j < 16; j++) printf("%02X ", data[i + j]);
        printf("\n");
    }
    printf("\n");
}

//==============================================================================
// Main
//==============================================================================

int main(void) {
    if (fpga_init() < 0) {
        return EXIT_FAILURE;
    }

    const uint32_t detected = g_fpga_regs[REG_HASH_ON_PLUG];
    printf("HASH_ON_PLUG = 0x%08X\n", detected);

    for (int i = 0; i < MAX_CHAINS; i++) {
        if (detected & (1 << i)) {
            printf("  Chain %d: detected\n", i);
        }
    }
    printf("\n");

    // Process each detected chain
    for (int chain = 0; chain < MAX_CHAINS; chain++) {
        if (!(detected & (1 << chain))) {
            continue;
        }

        uint8_t eeprom_data[EEPROM_SIZE];
        if (eeprom_read(chain, eeprom_data, EEPROM_SIZE) < 0) {
            fprintf(stderr, "Error: Failed to read chain %d EEPROM\n", chain);
            continue;
        }

        display_eeprom_hex(chain, eeprom_data);

        eeprom_info_t info;
        if (parse_eeprom(eeprom_data, &info) == 0) {
            printf("Chain [%d] Header Version: %u\n", chain, info.header_version);
            printf("Chain [%d] Board Serial No: %s\n", chain, info.board_serial_no);
            printf("Chain [%d] Chip Die: %s\n", chain, info.chip_die);
            printf("Chain [%d] Chip Marking: %s\n", chain, info.chip_marking);
            printf("Chain [%d] Chip Bin: %u\n", chain, info.chip_bin);
            printf("Chain [%d] FT Version: %u\n", chain, info.ft_version);
            printf("Chain [%d] PCB Version: %u\n", chain, info.pcb_version);
            printf("Chain [%d] BOM Version: %u\n", chain, info.bom_version);
            printf("Chain [%d] Default Frequency: %u MHz\n", chain, info.default_freq);
            printf("\n");
        } else {
            fprintf(stderr, "Error: Failed to parse chain %d EEPROM\n\n", chain);
        }
    }

    fpga_cleanup();
    return EXIT_SUCCESS;
}
