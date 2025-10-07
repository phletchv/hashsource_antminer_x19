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

typedef struct {
    char     serial[32];        // Serial number (hashboard_sn, null-terminated)
    char     chip_die[4];       // Chip die code (2 chars + null)
    char     chip_marking[16];  // Chip marking/model (13 chars max)
    uint8_t  chip_bin;          // Chip bin (1-9)
    char     ft_version[12];    // FT program version (9 chars max)
    uint8_t  asic_sensor;       // ASIC temperature sensor type
    uint8_t  asic_sensor_addr[4]; // ASIC sensor addresses
    uint8_t  pic_sensor;        // PIC temperature sensor type
    uint8_t  pic_sensor_addr;   // PIC sensor address
    uint16_t pcb_version;       // PCB hardware revision
    uint16_t bom_version;       // BOM version
    char     chip_tech[4];      // Chip technology (2 chars + null)
    uint16_t voltage;           // Operating voltage
    uint16_t frequency;         // Operating frequency (MHz)
    uint16_t nonce_rate;        // Nonce rate
    int8_t   pcb_temp_in;       // PCB temperature inlet
    int8_t   pcb_temp_out;      // PCB temperature outlet
    uint8_t  test_version;      // Test version
    uint8_t  test_standard;     // Test standard
    uint16_t freq_min;          // Minimum frequency (MHz, legacy formats)
    uint16_t freq_max;          // Maximum frequency (MHz, legacy formats)
    uint8_t  format;            // Format version (1-4)
    bool     valid;             // Successfully parsed
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
 *   Byte 1:       Data length (0x4A = 74 bytes)
 *   Bytes 2-73:   Encrypted payload (XXTEA, 72 bytes)
 *   Byte 255:     0x5A (trailer marker)
 *
 * Decrypted payload (Format 3):
 *   Offset 0x00:      Format byte (0x03)
 *   Offset 0x01-0x1E: Serial number (30 bytes ASCII)
 *   Offset 0x33-0x34: PCB version (little-endian uint16)
 *   Offset 0x35-0x36: BOM version (little-endian uint16)
 *
 * Source: bmminer FUN_0001740c (EEPROM parsing)
 * Discovery: Reverse engineered via successful decryption with Key 1
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

    // Extract format byte
    const uint8_t *payload = (const uint8_t *)decrypted;
    info->format = payload[0];

    // Parse format-specific fields
    switch (info->format) {
        case 3:  // S19 Pro standard format (verified on hardware)
            // Source: load_board_info_from_format_v3-0005d28c.c from S19 XP single_board_test

            // Serial number (hashboard_sn): 30 bytes ASCII at offset 0x01-0x1E
            memcpy(info->serial, &payload[0x01], 30);
            info->serial[30] = '\0';
            for (int i = 29; i >= 0 && (info->serial[i] == ' ' || info->serial[i] == '\0'); i--) {
                info->serial[i] = '\0';
            }

            // Chip die: 2 bytes at 0x1F-0x20
            memcpy(info->chip_die, &payload[0x1F], 2);
            info->chip_die[2] = '\0';

            // Chip marking: 13 bytes at 0x21-0x2D
            memcpy(info->chip_marking, &payload[0x21], 13);
            info->chip_marking[13] = '\0';
            for (int i = 12; i >= 0 && (info->chip_marking[i] == ' ' || info->chip_marking[i] == '\0'); i--) {
                info->chip_marking[i] = '\0';
            }

            // Chip bin: 1 byte at 0x2E (value 1-9)
            info->chip_bin = payload[0x2E];

            // FT program version: 9 bytes at 0x2F-0x37
            memcpy(info->ft_version, &payload[0x2F], 9);
            info->ft_version[9] = '\0';
            for (int i = 8; i >= 0 && (info->ft_version[i] == ' ' || info->ft_version[i] == '\0'); i--) {
                info->ft_version[i] = '\0';
            }

            // ASIC sensor type: 1 byte at 0x38
            info->asic_sensor = payload[0x38];

            // ASIC sensor addresses: 4 bytes at 0x39-0x3C
            memcpy(info->asic_sensor_addr, &payload[0x39], 4);

            // PIC sensor type: 1 byte at 0x3D
            info->pic_sensor = payload[0x3D];

            // PIC sensor address: 1 byte at 0x3E
            info->pic_sensor_addr = payload[0x3E];

            // PCB version: 2 bytes at 0x33-0x34 (little-endian)
            info->pcb_version = payload[0x33] | (payload[0x34] << 8);

            // BOM version: 2 bytes at 0x35-0x36 (little-endian)
            info->bom_version = payload[0x35] | (payload[0x36] << 8);

            // Chip technology: 2 bytes at 0x38-0x39
            memcpy(info->chip_tech, &payload[0x38], 2);
            info->chip_tech[2] = '\0';

            // Voltage: 2 bytes at 0x3A-0x3B (big-endian uint16)
            info->voltage = (payload[0x3A] << 8) | payload[0x3B];

            // Frequency: 2 bytes at 0x3C-0x3D (big-endian uint16)
            info->frequency = (payload[0x3C] << 8) | payload[0x3D];

            // Nonce rate: 2 bytes at 0x3E-0x3F (big-endian uint16)
            info->nonce_rate = (payload[0x3E] << 8) | payload[0x3F];

            // PCB temperature inlet: 1 byte at 0x40 (signed)
            info->pcb_temp_in = (int8_t)payload[0x40];

            // PCB temperature outlet: 1 byte at 0x41 (signed)
            info->pcb_temp_out = (int8_t)payload[0x41];

            // Test version: 1 byte at 0x42
            info->test_version = payload[0x42];

            // Test standard: 1 byte at 0x43
            info->test_standard = payload[0x43];

            // Legacy freq fields not used in Format 3
            info->freq_min = 0;
            info->freq_max = 0;
            break;

        case 1:
        case 2:  // Older formats (from decompilation, untested)
            info->serial[0] = '\0';  // Unknown serial offset for these formats
            info->pcb_version = (payload[0x2D] << 8) | payload[0x2E];
            info->bom_version = (payload[0x2F] << 8) | payload[0x30];
            info->freq_min = (payload[0x33] << 8) | payload[0x34];
            info->freq_max = (payload[0x35] << 8) | payload[0x36];
            break;

        case 4:  // Newer format (from decompilation, untested)
            info->serial[0] = '\0';  // Unknown serial offset for this format
            info->pcb_version = (payload[0x31] << 8) | payload[0x33];
            info->bom_version = (payload[0x34] << 8) | payload[0x35];
            info->freq_min = (payload[0x38] << 8) | payload[0x39];
            info->freq_max = (payload[0x3A] << 8) | payload[0x3B];
            break;

        default:
            fprintf(stderr, "Error: Unsupported format: %u\n", info->format);
            return -1;
    }

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
            printf("Chain [%d] Format: %u\n", chain, info.format);

            if (info.serial[0] != '\0') {
                printf("Chain [%d] Serial: %s\n", chain, info.serial);
            }

            printf("Chain [%d] PCB Version: 0x%04X\n", chain, info.pcb_version);
            printf("Chain [%d] BOM Version: 0x%04X\n", chain, info.bom_version);

            if (info.format == 3) {
                // Additional Format 3 fields
                if (info.chip_die[0] != '\0' && info.chip_die[0] != ' ') {
                    printf("Chain [%d] Chip Die: %s\n", chain, info.chip_die);
                }
                if (info.chip_marking[0] != '\0') {
                    printf("Chain [%d] Chip Marking: %s\n", chain, info.chip_marking);
                }
                if (info.chip_bin > 0 && info.chip_bin <= 9) {
                    printf("Chain [%d] Chip Bin: BIN%u\n", chain, info.chip_bin);
                }
                if (info.ft_version[0] != '\0') {
                    printf("Chain [%d] FT Version: %s\n", chain, info.ft_version);
                }
                if (info.chip_tech[0] != '\0' && info.chip_tech[0] != ' ') {
                    printf("Chain [%d] Chip Technology: %s\n", chain, info.chip_tech);
                }
                if (info.voltage > 0) {
                    printf("Chain [%d] Voltage: %u\n", chain, info.voltage);
                }
                if (info.frequency > 0 && info.frequency < 10000) {
                    printf("Chain [%d] Frequency: %u MHz\n", chain, info.frequency);
                }
                if (info.nonce_rate > 0) {
                    printf("Chain [%d] Nonce Rate: %u\n", chain, info.nonce_rate);
                }
                if (info.asic_sensor != 0) {
                    printf("Chain [%d] ASIC Sensor: 0x%02X\n", chain, info.asic_sensor);
                }
                if (info.pic_sensor != 0) {
                    printf("Chain [%d] PIC Sensor: 0x%02X\n", chain, info.pic_sensor);
                }
                if (info.pcb_temp_in != 0 || info.pcb_temp_out != 0) {
                    printf("Chain [%d] PCB Temp (In/Out): %d°C / %d°C\n",
                           chain, info.pcb_temp_in, info.pcb_temp_out);
                }
            } else if (info.freq_min > 0 || info.freq_max > 0) {
                printf("Chain [%d] Frequency Range: %u-%u MHz\n",
                       chain, info.freq_min, info.freq_max);
            }

            printf("\n");
        } else {
            fprintf(stderr, "Error: Failed to parse chain %d EEPROM\n\n", chain);
        }
    }

    fpga_cleanup();
    return EXIT_SUCCESS;
}
