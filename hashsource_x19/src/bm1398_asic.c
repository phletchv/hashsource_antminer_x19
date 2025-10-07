/*
 * BM1398 ASIC Driver Implementation
 *
 * Protocol reverse-engineered from:
 * - Bitmain_Test_Fixtures S19_Pro single_board_test.c
 * - bitmaintech bmminer-mix driver-btm-c5.c
 * - Bitmain_Peek S19_Pro firmware analysis
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "../include/bm1398_asic.h"

//==============================================================================
// Linux I2C Constants
//==============================================================================

#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703  // Set slave address (from linux/i2c-dev.h)
#endif

//==============================================================================
// CRC5 Implementation
//==============================================================================

/**
 * Calculate CRC5 for BM13xx UART commands
 * Polynomial: Custom 5-bit CRC
 * Initial value: 0x1F
 *
 * Source: Bitmain single_board_test.c line 28769
 */
uint8_t bm1398_crc5(const uint8_t *data, unsigned int bits) {
    uint8_t crc = 0x1F;  // Initial value

    for (unsigned int i = 0; i < bits; i++) {
        uint8_t bit = (data[i / 8] >> (7 - (i % 8))) & 1;
        if ((crc & 0x10) != (bit << 4)) {
            crc = ((crc << 1) | bit) ^ 0x05;
        } else {
            crc = (crc << 1) | bit;
        }
        crc &= 0x1F;
    }

    return crc;
}

//==============================================================================
// Initialization and Cleanup
//==============================================================================

int bm1398_init(bm1398_context_t *ctx) {
    if (!ctx) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));

    // Open FPGA device
    int fd = open("/dev/axi_fpga_dev", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open /dev/axi_fpga_dev: %s\n", strerror(errno));
        fprintf(stderr, "Hint: Ensure bitmain_axi.ko kernel module is loaded\n");
        return -1;
    }

    // Memory map FPGA registers
    ctx->fpga_regs = mmap(NULL, FPGA_REG_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    close(fd);

    if (ctx->fpga_regs == MAP_FAILED) {
        fprintf(stderr, "Error: mmap failed: %s\n", strerror(errno));
        return -1;
    }

    ctx->initialized = true;
    ctx->num_chains = 0;

    // Detect chains
    uint32_t detected = bm1398_detect_chains(ctx);
    printf("Detected chains: 0x%08X\n", detected);

    for (int i = 0; i < MAX_CHAINS; i++) {
        if (detected & (1 << i)) {
            ctx->num_chains++;
            ctx->chips_per_chain[i] = CHIPS_PER_CHAIN_S19PRO;
            printf("  Chain %d: %d chips\n", i, ctx->chips_per_chain[i]);
        }
    }

    return 0;
}

void bm1398_cleanup(bm1398_context_t *ctx) {
    if (ctx && ctx->fpga_regs && ctx->fpga_regs != MAP_FAILED) {
        munmap((void *)ctx->fpga_regs, FPGA_REG_SIZE);
        ctx->fpga_regs = NULL;
    }
    ctx->initialized = false;
}

//==============================================================================
// Low-level UART Communication
//==============================================================================

/**
 * Send UART command to ASIC chain via FPGA BC_COMMAND_BUFFER
 *
 * Method: Write command bytes to registers 0xC4-0xCF (3 x 32-bit words)
 *         Trigger with BC_WRITE_COMMAND (0xC0)
 *         Wait for completion (bit 31 clears)
 *
 * Source: Analysis of S19 FPGA interface + bitmaintech driver
 */
int bm1398_send_uart_cmd(bm1398_context_t *ctx, int chain,
                         const uint8_t *cmd, size_t len) {
    if (!ctx || !ctx->initialized || !cmd || chain < 0 || chain >= MAX_CHAINS) {
        return -1;
    }

    if (len == 0 || len > 12) {
        fprintf(stderr, "Error: Invalid command length %zu (max 12 bytes)\n", len);
        return -1;
    }

    volatile uint32_t *regs = ctx->fpga_regs;

    // Write command bytes to BC_COMMAND_BUFFER (0xC4, 0xC8, 0xCC)
    // Up to 12 bytes = 3 x 32-bit words
    for (size_t i = 0; i < (len + 3) / 4; i++) {
        uint32_t word = 0;
        size_t bytes_to_copy = (len - i * 4);
        if (bytes_to_copy > 4) bytes_to_copy = 4;

        memcpy(&word, &cmd[i * 4], bytes_to_copy);
        regs[REG_BC_COMMAND_BUFFER + i] = word;
    }

    // Trigger command transmission
    uint32_t trigger = BC_COMMAND_BUFFER_READY | BC_CHAIN_ID(chain);
    regs[REG_BC_WRITE_COMMAND] = trigger;

    // Wait for completion (bit 31 clears)
    int timeout = 10000;  // 10ms max
    while ((regs[REG_BC_WRITE_COMMAND] & BC_COMMAND_BUFFER_READY) && timeout > 0) {
        usleep(1);
        timeout--;
    }

    if (timeout == 0) {
        fprintf(stderr, "Error: UART command timeout on chain %d\n", chain);
        return -1;
    }

    return 0;
}

//==============================================================================
// Chain Control Commands
//==============================================================================

/**
 * Send chain inactive command (stop relay)
 * Command: 0x53 0x05 0x00 0x00 [CRC5]
 */
int bm1398_chain_inactive(bm1398_context_t *ctx, int chain) {
    uint8_t cmd[5];

    cmd[0] = CMD_PREAMBLE_CHAIN_INACTIVE;
    cmd[1] = CMD_LEN_ADDRESS;
    cmd[2] = 0x00;
    cmd[3] = 0x00;
    cmd[4] = bm1398_crc5(cmd, 32);

    return bm1398_send_uart_cmd(ctx, chain, cmd, sizeof(cmd));
}

/**
 * Set chip address
 * Command: 0x40 0x05 [addr] 0x00 [CRC5]
 */
int bm1398_set_chip_address(bm1398_context_t *ctx, int chain, uint8_t addr) {
    uint8_t cmd[5];

    cmd[0] = CMD_PREAMBLE_SET_ADDRESS;
    cmd[1] = CMD_LEN_ADDRESS;
    cmd[2] = addr;
    cmd[3] = 0x00;
    cmd[4] = bm1398_crc5(cmd, 32);

    return bm1398_send_uart_cmd(ctx, chain, cmd, sizeof(cmd));
}

/**
 * Enumerate chips on chain
 * Assigns sequential addresses with specified interval
 *
 * S19 Pro: 114 chips, interval = 256/114 ≈ 2.2 → use 2
 * Addresses: 0, 2, 4, 6, ..., 226
 */
int bm1398_enumerate_chips(bm1398_context_t *ctx, int chain, int num_chips) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    printf("Enumerating %d chips on chain %d...\n", num_chips, chain);

    // Send chain inactive first to stop relay
    if (bm1398_chain_inactive(ctx, chain) < 0) {
        fprintf(stderr, "Error: Failed to send chain inactive\n");
        return -1;
    }
    usleep(10000);

    // Calculate address interval
    int interval = 256 / num_chips;
    if (interval < 1) interval = 1;

    printf("  Address interval: %d\n", interval);

    // Assign addresses sequentially
    int errors = 0;
    for (int i = 0; i < num_chips; i++) {
        uint8_t addr = i * interval;

        if (bm1398_set_chip_address(ctx, chain, addr) < 0) {
            fprintf(stderr, "Warning: Failed to set address %d for chip %d\n", addr, i);
            errors++;
        }

        // Small delay between chips
        usleep(1000);  // 1ms

        // Progress indication every 10 chips
        if ((i + 1) % 10 == 0) {
            printf("  Addressed %d/%d chips\r", i + 1, num_chips);
            fflush(stdout);
        }
    }

    printf("\n  Enumeration complete: %d chips addressed (%d errors)\n",
           num_chips, errors);

    return errors > 0 ? -1 : 0;
}

//==============================================================================
// Register Operations
//==============================================================================

/**
 * Write ASIC register
 * Command: [0x41/0x51] 0x09 [chip_addr] [reg_addr] [value_be] [CRC5]
 *
 * broadcast: true = 0x51 (all chips), false = 0x41 (single chip)
 * value: 32-bit value in BIG-ENDIAN byte order
 */
int bm1398_write_register(bm1398_context_t *ctx, int chain, bool broadcast,
                          uint8_t chip_addr, uint8_t reg_addr, uint32_t value) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    uint8_t cmd[9];

    cmd[0] = broadcast ? CMD_PREAMBLE_WRITE_BCAST : CMD_PREAMBLE_WRITE_REG;
    cmd[1] = CMD_LEN_WRITE_REG;
    cmd[2] = chip_addr;
    cmd[3] = reg_addr;
    cmd[4] = (value >> 24) & 0xFF;  // MSB first (big-endian)
    cmd[5] = (value >> 16) & 0xFF;
    cmd[6] = (value >> 8) & 0xFF;
    cmd[7] = value & 0xFF;          // LSB last
    cmd[8] = bm1398_crc5(cmd, 64);  // 8 bytes = 64 bits

    return bm1398_send_uart_cmd(ctx, chain, cmd, sizeof(cmd));
}

/**
 * Read ASIC register
 * Command: [0x42/0x52] 0x09 [chip_addr] [reg_addr] 0x00 0x00 0x00 0x00 [CRC5]
 *
 * Response comes back through FPGA RETURN_NONCE register
 * Response format: [reg_data:32bits] in bits [31:0]
 *
 * Note: This implementation uses polling of NONCE_NUMBER_IN_FIFO
 */
int bm1398_read_register(bm1398_context_t *ctx, int chain, bool broadcast,
                         uint8_t chip_addr, uint8_t reg_addr, uint32_t *value,
                         int timeout_ms) {
    if (!ctx || !ctx->initialized || !value) {
        return -1;
    }

    // Build read command (same as write but with 0x42/0x52 preamble)
    uint8_t cmd[9];
    cmd[0] = broadcast ? CMD_PREAMBLE_READ_BCAST : CMD_PREAMBLE_READ_REG;
    cmd[1] = CMD_LEN_WRITE_REG;
    cmd[2] = chip_addr;
    cmd[3] = reg_addr;
    cmd[4] = 0x00;  // Placeholder data
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x00;
    cmd[8] = bm1398_crc5(cmd, 64);

    // Send read command
    if (bm1398_send_uart_cmd(ctx, chain, cmd, sizeof(cmd)) < 0) {
        return -1;
    }

    // Wait for response in FPGA FIFO
    volatile uint32_t *regs = ctx->fpga_regs;
    int timeout = timeout_ms * 1000;  // Convert to microseconds

    while (timeout > 0) {
        // Check if response available
        int available = regs[REG_NONCE_NUMBER_IN_FIFO];
        if (available > 0) {
            // Read response from FIFO
            uint32_t response = regs[REG_RETURN_NONCE];

            // Parse response (register data in lower 32 bits)
            // TODO: Verify this is correct format based on hardware testing
            *value = response;
            return 0;
        }

        usleep(100);  // Poll every 100us
        timeout -= 100;
    }

    fprintf(stderr, "Error: Register read timeout (chain %d, reg 0x%02X)\n",
            chain, reg_addr);
    return -1;
}

/**
 * Read-modify-write register operation
 *
 * Reads register, clears bits in clear_mask, sets bits in set_mask, writes back.
 * Uses broadcast to affect all chips on chain.
 *
 * Example: To set bit 2 and clear bit 5:
 *   clear_mask = (1 << 5)
 *   set_mask = (1 << 2)
 */
int bm1398_read_modify_write_register(bm1398_context_t *ctx, int chain,
                                      uint8_t reg_addr, uint32_t clear_mask,
                                      uint32_t set_mask) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    uint32_t value;

    // Read current value (broadcast read from chip 0 as representative)
    if (bm1398_read_register(ctx, chain, false, 0, reg_addr, &value, 100) < 0) {
        fprintf(stderr, "Error: Read failed in read-modify-write (reg 0x%02X)\n",
                reg_addr);
        return -1;
    }

    printf("  Read reg 0x%02X = 0x%08X\n", reg_addr, value);

    // Modify value
    value &= ~clear_mask;  // Clear bits
    value |= set_mask;     // Set bits

    printf("  Writing reg 0x%02X = 0x%08X\n", reg_addr, value);

    // Write back (broadcast to all chips)
    if (bm1398_write_register(ctx, chain, true, 0, reg_addr, value) < 0) {
        fprintf(stderr, "Error: Write failed in read-modify-write (reg 0x%02X)\n",
                reg_addr);
        return -1;
    }

    usleep(10000);  // 10ms settle time
    return 0;
}

//==============================================================================
// Chain Initialization Sequences
//==============================================================================

/**
 * Stage 1: Hardware Reset Sequence
 *
 * Source: Bitmain single_board_test.c lines 13617-13633
 */
int bm1398_reset_chain_stage1(bm1398_context_t *ctx, int chain) {
    printf("Stage 1: Hardware reset chain %d...\n", chain);

    // Simplified hardware reset using known good values
    // Note: Register reads don't work reliably during early initialization,
    // so we use direct writes with known values from factory test code

    printf("  Performing software reset sequence...\n");
    // The factory code does a simplified reset via ticket mask only
    // Full hardware reset may not be necessary or may happen automatically

    // Set ticket mask to all cores enabled (initialization value)
    printf("  Setting ticket mask to 0xFFFFFFFF...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_TICKET_MASK,
                              TICKET_MASK_ALL_CORES) < 0) {
        fprintf(stderr, "Error: Failed to set ticket mask\n");
        return -1;
    }
    usleep(10000);

    printf("  Stage 1 complete\n");
    return 0;
}

/**
 * Stage 2: Configuration Sequence
 *
 * Source: Bitmain single_board_test.c lines 13640-13694
 */
int bm1398_configure_chain_stage2(bm1398_context_t *ctx, int chain,
                                  uint8_t diode_vdd_mux_sel) {
    printf("Stage 2: Configure chain %d...\n", chain);

    // 1. Set diode mux selector (voltage monitoring)
    printf("  Setting diode_vdd_mux_sel = %d...\n", diode_vdd_mux_sel);
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_DIODE_MUX,
                              diode_vdd_mux_sel) < 0) {
        fprintf(stderr, "Error: Failed to set diode mux\n");
        return -1;
    }
    usleep(10000);

    // 2. Chain inactive
    printf("  Chain inactive...\n");
    if (bm1398_chain_inactive(ctx, chain) < 0) {
        fprintf(stderr, "Error: Failed to send chain inactive\n");
        return -1;
    }
    usleep(10000);

    // 3. Enumerate chips
    printf("  Enumerating chips...\n");
    int num_chips = ctx->chips_per_chain[chain];
    if (bm1398_enumerate_chips(ctx, chain, num_chips) < 0) {
        fprintf(stderr, "Error: Chip enumeration failed\n");
        return -1;
    }
    usleep(10000);

    // 4. Set core configuration (pulse_mode=1, clk_sel=0)
    uint32_t core_cfg = CORE_CONFIG_BASE | ((1 & 3) << CORE_CONFIG_PULSE_MODE_SHIFT) | (0 & CORE_CONFIG_CLK_SEL_MASK);
    printf("  Setting core config = 0x%08X...\n", core_cfg);
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CORE_CONFIG,
                              core_cfg) < 0) {
        fprintf(stderr, "Error: Failed to set core config\n");
        return -1;
    }
    usleep(10000);

    // 4b. Set core timing parameters (pwth_sel=1, ccdly_sel=0, swpf_mode=0)
    // From Config.ini: Pwth_Sel=1, CCdly_Sel=0
    uint8_t pwth_sel = 1;
    uint8_t ccdly_sel = 0;
    uint8_t swpf_mode = 0;
    uint32_t core_param = ((pwth_sel & CORE_PARAM_PWTH_SEL_MASK) << CORE_PARAM_PWTH_SEL_SHIFT) |
                          ((ccdly_sel & CORE_PARAM_CCDLY_SEL_MASK) << CORE_PARAM_CCDLY_SEL_SHIFT);
    if (swpf_mode != 0) {
        core_param |= (1 << CORE_PARAM_SWPF_MODE_BIT);
    }
    printf("  Setting core timing params = 0x%08X (pwth_sel=%u, ccdly_sel=%u, swpf_mode=%u)...\n",
           core_param, pwth_sel, ccdly_sel, swpf_mode);
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CORE_PARAM,
                              core_param) < 0) {
        fprintf(stderr, "Error: Failed to set core timing parameters\n");
        return -1;
    }
    usleep(10000);

    // 4c. Set IO driver strength for clock output (clko_ds=1)
    // Register 0x58: Modify bits [7:4] to set clko_ds
    printf("  Setting IO driver clock output strength (clko_ds=1)...\n");
    uint32_t io_driver = 0x10;  // clko_ds=1 in bits [7:4]
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_IO_DRIVER,
                              io_driver) < 0) {
        fprintf(stderr, "Warning: IO driver configuration failed\n");
    }
    usleep(10000);

    // 5. Set PLL dividers to 0
    printf("  Setting PLL dividers...\n");
    bm1398_write_register(ctx, chain, true, 0, ASIC_REG_PLL_PARAM_0, 0x00000000);
    usleep(10000);
    bm1398_write_register(ctx, chain, true, 0, ASIC_REG_PLL_PARAM_1, 0x00000000);
    usleep(10000);
    bm1398_write_register(ctx, chain, true, 0, ASIC_REG_PLL_PARAM_2, 0x00000000);
    usleep(10000);
    bm1398_write_register(ctx, chain, true, 0, ASIC_REG_PLL_PARAM_3, 0x00000000);
    usleep(10000);

    // 6. Set frequency (525 MHz)
    printf("  Setting frequency to %d MHz...\n", FREQUENCY_525MHZ);
    if (bm1398_set_frequency(ctx, chain, FREQUENCY_525MHZ) < 0) {
        fprintf(stderr, "Warning: Frequency set failed (not implemented yet)\n");
    }
    usleep(10000);

    // 7. Set baud rate (12 MHz)
    printf("  Setting baud rate to %d Hz...\n", BAUD_RATE_12MHZ);
    if (bm1398_set_baud_rate(ctx, chain, BAUD_RATE_12MHZ) < 0) {
        fprintf(stderr, "Warning: Baud rate set failed (not fully implemented)\n");
    }
    usleep(50000);

    // 7a. Core reset sequence (critical for nonce reception)
    // Use broadcast writes to avoid system hang with 114 chips
    printf("  Performing core reset sequence (broadcast)...\n");

    // Step 1a: Soft reset control (register 0xA8) - broadcast
    printf("    Broadcast soft reset (reg 0xA8)...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_SOFT_RESET,
                              SOFT_RESET_MASK) < 0) {
        fprintf(stderr, "Warning: Soft reset broadcast failed\n");
    }
    usleep(100000);  // 100ms settle time

    // Step 1b: Modify CLK_CTRL (register 0x18) - broadcast
    printf("    Broadcast CLK_CTRL (reg 0x18)...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CLK_CTRL,
                              0xF0000000) < 0) {
        fprintf(stderr, "Warning: CLK_CTRL broadcast failed\n");
    }
    usleep(100000);  // 100ms settle time

    // Step 2: Re-configure clock select with clk_sel=0 - broadcast
    uint32_t core_config_reset = CORE_CONFIG_BASE | ((1 & 3) << CORE_CONFIG_PULSE_MODE_SHIFT);
    printf("    Broadcast clock select reset (clk_sel=0)...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CORE_CONFIG,
                              core_config_reset) < 0) {
        fprintf(stderr, "Warning: Clock select reset broadcast failed\n");
    }
    usleep(100000);  // 100ms settle time

    // Step 3: Re-configure timing parameters - broadcast
    printf("    Broadcast timing params...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CORE_PARAM,
                              core_param) < 0) {
        fprintf(stderr, "Warning: Timing param reset broadcast failed\n");
    }
    usleep(100000);  // 100ms settle time

    // Step 4: Core enable (register 0x3C with 0x800082AA) - broadcast
    printf("    Broadcast core enable...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CORE_CONFIG,
                              CORE_CONFIG_ENABLE) < 0) {
        fprintf(stderr, "Warning: Core enable broadcast failed\n");
    }
    usleep(100000);  // 100ms settle time

    printf("  Core reset sequence complete\n");
    usleep(500000);  // 500ms additional settle time

    // 7b. Set FPGA nonce timeout based on frequency
    // Formula from factory test: timeout = 0x1FFFF / freq_mhz
    // For 525 MHz: timeout = 0x1FFFF / 525 ≈ 251
    // CRITICAL: Factory test writes timeout to register 20 which maps to offset 0x08C, NOT 0x014!
    // Register 0x014 is not in the factory test mapping table and appears read-only
    // Register 0x08C appears to be dual-purpose (baud/clock + timeout)
    // Strategy: Read existing value, preserve upper bits, set timeout in lower 17 bits + enable bit
    uint32_t timeout_val = 0x1FFFF / FREQUENCY_525MHZ;
    if (timeout_val > 0x1FFFF) timeout_val = 0x1FFFF;  // Clamp to max

    // Read current register value to preserve other configuration
    uint32_t current_val = ctx->fpga_regs[0x08C / 4];
    printf("  Current reg[0x08C] = 0x%08X\n", current_val);

    // Merge: keep bits 17-30, set timeout in bits 0-16, set enable bit 31
    uint32_t timeout_reg = (current_val & 0x7FFE0000) | (timeout_val & 0x1FFFF) | 0x80000000;
    printf("  Setting FPGA nonce timeout = 0x%08X (timeout=%u)...\n", timeout_reg, timeout_val);
    printf("  Writing merged value to offset 0x08C...\n");
    ctx->fpga_regs[0x08C / 4] = timeout_reg;
    __sync_synchronize();
    usleep(10000);
    printf("  Verifying write: reg[0x08C] = 0x%08X\n", ctx->fpga_regs[0x08C / 4]);
    usleep(10000);

    // 8. Set final ticket mask
    printf("  Setting final ticket mask = 0xFF...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_TICKET_MASK,
                              TICKET_MASK_256_CORES) < 0) {
        fprintf(stderr, "Error: Failed to set final ticket mask\n");
        return -1;
    }
    usleep(10000);

    // 9. Set nonce overflow control (disable overflow)
    // Register 0x3C: Final configuration with nonce overflow disabled
    printf("  Setting nonce overflow control (disabled)...\n");
    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CORE_CONFIG,
                              CORE_CONFIG_NONCE_OVF_DIS) < 0) {
        fprintf(stderr, "Warning: Nonce overflow control failed\n");
    }
    usleep(10000);

    printf("  Stage 2 complete\n");
    return 0;
}

/**
 * Complete chain initialization (both stages)
 */
int bm1398_init_chain(bm1398_context_t *ctx, int chain) {
    if (!ctx || !ctx->initialized || chain < 0 || chain >= MAX_CHAINS) {
        return -1;
    }

    printf("\n====================================\n");
    printf("Initializing Chain %d\n", chain);
    printf("====================================\n\n");

    // Stage 1: Hardware reset
    if (bm1398_reset_chain_stage1(ctx, chain) < 0) {
        fprintf(stderr, "Error: Stage 1 failed\n");
        return -1;
    }

    // Stage 2: Configuration (diode_vdd_mux_sel = 3 from Config.ini)
    if (bm1398_configure_chain_stage2(ctx, chain, 3) < 0) {
        fprintf(stderr, "Error: Stage 2 failed\n");
        return -1;
    }

    printf("\n====================================\n");
    printf("Chain %d initialization complete\n", chain);
    printf("====================================\n\n");

    return 0;
}

//==============================================================================
// Baud Rate and Frequency Configuration
//==============================================================================

/**
 * Set UART baud rate
 *
 * Note: This is a simplified implementation. Full implementation requires
 * reading CLK_CTRL register, modifying specific bits, and writing back.
 *
 * Source: Bitmain single_board_test.c lines 27479-27527
 */
int bm1398_set_baud_rate(bm1398_context_t *ctx, int chain, uint32_t baud_rate) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    uint32_t baud_div;

    if (baud_rate > 3000000) {
        // High-speed mode (>3 MHz) - uses 400 MHz base clock from PLL3

        // Configure PLL3 register (0x68)
        printf("    Configuring PLL3 for high-speed UART...\n");
        bm1398_write_register(ctx, chain, true, 0, ASIC_REG_PLL_PARAM_3, 0xC0700111);
        usleep(10000);

        // Configure BAUD_CONFIG register (0x28)
        printf("    Configuring high-speed baud register...\n");
        bm1398_write_register(ctx, chain, true, 0, ASIC_REG_BAUD_CONFIG, 0x06008F00);
        usleep(10000);

        // Calculate divisor: 400MHz / (baud * 8) - 1
        baud_div = (400000000 / (baud_rate * 8)) - 1;
        printf("    Baud divisor (high-speed): %u\n", baud_div);
    } else {
        // Standard mode (<= 3 MHz) - uses 25 MHz base clock
        baud_div = (25000000 / (baud_rate * 8)) - 1;
        printf("    Baud divisor (standard): %u\n", baud_div);
    }

    // Write baud divisor to CLK_CTRL register (0x18)
    // Using known good value from factory test: includes baud div + bit 16 set
    // Bits [11:8] = upper 4 bits of divisor, bits [4:0] = lower 5 bits
    printf("    Writing CLK_CTRL register with baud configuration...\n");

    // Build CLK_CTRL value: base value + baud divisor
    // Base value preserves other critical bits
    uint32_t clk_ctrl_value = 0x00010000 |  // Bit 16 set
                              ((baud_div & 0x1E0) << 3) |  // Upper bits [11:8]
                              (baud_div & 0x1F);            // Lower bits [4:0]

    if (bm1398_write_register(ctx, chain, true, 0, ASIC_REG_CLK_CTRL,
                              clk_ctrl_value) < 0) {
        fprintf(stderr, "Error: Failed to write CLK_CTRL\n");
        return -1;
    }

    usleep(50000);  // 50ms settle time for baud rate change
    printf("    Baud rate configuration complete\n");
    return 0;
}

/**
 * Set ASIC core frequency
 *
 * Note: PLL configuration formula needs to be extracted from cgminer or bmminer.
 * This is a placeholder implementation.
 */
int bm1398_set_frequency(bm1398_context_t *ctx, int chain, uint32_t freq_mhz) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    printf("    Setting frequency to %u MHz...\n", freq_mhz);

    // PLL configuration for BM1398
    // Formula: freq = CLKI * fbdiv / (refdiv * (postdiv1+1) * (postdiv2+1))
    // Where CLKI = 25 MHz
    // VCO = CLKI / refdiv * fbdiv (must be 1600-3200 MHz)

    uint8_t refdiv, postdiv1, postdiv2;
    uint16_t fbdiv;

    // For 525 MHz (standard BM1398 frequency):
    // VCO = 25 * 84 = 2100 MHz
    // freq = 2100 / (2 * 2) = 525 MHz
    if (freq_mhz == 525) {
        refdiv = 1;
        fbdiv = 84;
        postdiv1 = 1;  // Divide by 2
        postdiv2 = 1;  // Divide by 2
    } else {
        fprintf(stderr, "    Warning: Frequency %u MHz not supported, using 525 MHz\n", freq_mhz);
        refdiv = 1;
        fbdiv = 84;
        postdiv1 = 1;
        postdiv2 = 1;
    }

    // Calculate VCO frequency for range check
    float vco = 25.0f / refdiv * fbdiv;
    printf("    PLL config: refdiv=%u, fbdiv=%u, postdiv1=%u, postdiv2=%u (VCO=%.0f MHz)\n",
           refdiv, fbdiv, postdiv1, postdiv2, vco);

    // Build PLL register value (from factory test set_pllparameter-001cacb0.c)
    // Bits: [31:30]=VCO range, [29]=reserved, [28]=VCO_mode, [27:16]=fbdiv, [13:8]=postdiv1, [6:4]=refdiv-1, [2:0]=postdiv2-1
    uint32_t pll_value = 0x40000000 |  // Base value
                         ((postdiv2 - 1) & 0x7) |
                         (((refdiv - 1) & 0x7) << 4) |
                         ((postdiv1 & 0x3f) << 8) |
                         ((fbdiv & 0xfff) << 16);

    // Set VCO range bit based on VCO frequency
    if (vco >= 2400.0f && vco <= 3200.0f) {
        pll_value |= 0x10000000;  // High VCO range
    } else if (vco < 1600.0f || vco > 3200.0f) {
        fprintf(stderr, "    Error: VCO %.0f MHz out of range (1600-3200 MHz)\n", vco);
        return -1;
    }

    printf("    Writing PLL0 register 0x08 = 0x%08X\n", pll_value);

    // Write PLL0 parameter to register 0x08 (broadcast to all chips)
    if (bm1398_write_register(ctx, chain, true, 0, 0x08, pll_value) < 0) {
        fprintf(stderr, "    Error: Failed to write PLL0 register\n");
        return -1;
    }

    usleep(10000);  // Wait for PLL to stabilize
    printf("    Frequency configuration complete\n");

    return 0;
}

//==============================================================================
// Utility Functions
//==============================================================================

/**
 * Detect which chains are present
 * Returns: Bitmask with bit N set if chain N is detected
 */
uint32_t bm1398_detect_chains(bm1398_context_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    return ctx->fpga_regs[REG_HASH_ON_PLUG];
}

/**
 * Get CRC error count from FPGA
 */
int bm1398_get_crc_error_count(bm1398_context_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    return ctx->fpga_regs[REG_CRC_ERROR_CNT_ADDR];
}

//==============================================================================
// Work Submission
//==============================================================================

/**
 * Enable work send (FPGA control register)
 * Source: Bitmain enable_work_send()
 */
int bm1398_enable_work_send(bm1398_context_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    // Register 0x2D (0xB4/4) = work send enable
    ctx->fpga_regs[0x2D] = 0xFFFFFFFF;
    return 0;
}

/**
 * Start FPGA work generation
 * Source: Bitmain start_dhash_work_gen()
 */
int bm1398_start_work_gen(bm1398_context_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    // Register 0x23 (0x8C/4) - set bit 0x40 to start
    uint32_t val = ctx->fpga_regs[0x23];
    ctx->fpga_regs[0x23] = val | 0x40;
    return 0;
}

/**
 * Check if work FIFO has space available
 * Returns: Available buffer space, or -1 on error
 *
 * Source: Bitmain single_board_test.c line 6220 (is_work_fifo_ready)
 */
int bm1398_check_work_fifo_ready(bm1398_context_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    return ctx->fpga_regs[REG_BUFFER_SPACE];
}

/**
 * Send work to ASIC chain via FPGA
 *
 * Source: Bitmain single_board_test.c software_pattern_4_midstate_send_function
 *         and set_TW_write_command
 */
int bm1398_send_work(bm1398_context_t *ctx, int chain, uint32_t work_id,
                    const uint8_t *work_data_12bytes,
                    const uint8_t midstates[4][32]) {
    if (!ctx || !ctx->initialized || !work_data_12bytes || !midstates) {
        return -1;
    }

    if (chain < 0 || chain >= MAX_CHAINS) {
        fprintf(stderr, "Error: Invalid chain %d\n", chain);
        return -1;
    }

    // Build work packet (148 bytes = 0x94)
    work_packet_t work;
    memset(&work, 0, sizeof(work));

    work.work_type = 0x01;
    work.chain_id = (uint8_t)chain | 0x80;
    work.reserved[0] = 0x00;
    work.reserved[1] = 0x00;

    // Work ID in big-endian format
    work.work_id = __builtin_bswap32(work_id);

    // Copy last 12 bytes of block header
    memcpy(work.work_data, work_data_12bytes, 12);

    // Copy 4 midstates (32 bytes each)
    for (int i = 0; i < 4; i++) {
        memcpy(work.midstate[i], midstates[i], 32);
    }

    // Byte-swap all 32-bit words in the packet (big-endian)
    // Total: 148 bytes / 4 = 37 words
    uint32_t *words = (uint32_t *)&work;
    for (int i = 0; i < sizeof(work) / 4; i++) {
        words[i] = __builtin_bswap32(words[i]);
    }

    // Write work packet to FPGA TW_WRITE_COMMAND registers
    // Starting at register 0x40, write 37 words
    volatile uint32_t *regs = ctx->fpga_regs;
    for (size_t i = 0; i < sizeof(work) / 4; i++) {
        regs[REG_TW_WRITE_COMMAND + i] = words[i];
    }

    return 0;
}

//==============================================================================
// Nonce Collection
//==============================================================================

/**
 * Get number of nonces in FPGA FIFO
 *
 * Source: Bitmain single_board_test.c get_nonce_number_in_fifo
 */
int bm1398_get_nonce_count(bm1398_context_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    uint32_t count = ctx->fpga_regs[REG_NONCE_NUMBER_IN_FIFO];
    return count & 0x7FFF;  // Mask to 15 bits
}

/**
 * Read single nonce from FPGA FIFO
 *
 * Source: Bitmain single_board_test.c get_return_nonce
 */
int bm1398_read_nonce(bm1398_context_t *ctx, nonce_response_t *nonce) {
    if (!ctx || !ctx->initialized || !nonce) {
        return -1;
    }

    volatile uint32_t *regs = ctx->fpga_regs;

    // Read nonce data (64-bit value from 2 registers)
    uint32_t nonce_low = regs[REG_RETURN_NONCE];
    uint32_t nonce_high = regs[REG_RETURN_NONCE + 1];

    // Parse nonce response
    // Format (from BM1398_PROTOCOL.md):
    // Bit 31: WORK_ID_OR_CRC indicator
    // Bit 7: NONCE_INDICATOR (valid nonce present)
    // Bits [3:0]: Chain number

    if (nonce_low & NONCE_INDICATOR) {
        nonce->chain_id = NONCE_CHAIN_NUMBER(nonce_low);
        nonce->nonce = nonce_low;  // Full 32-bit nonce value
        nonce->work_id = (nonce_high >> 16) & 0x7FFF;  // Work ID from high word

        // TODO: Parse chip_id and core_id from response
        nonce->chip_id = 0;
        nonce->core_id = 0;

        return 1;  // Successfully read nonce
    }

    return 0;  // No valid nonce
}

/**
 * Read multiple nonces from FPGA FIFO
 */
int bm1398_read_nonces(bm1398_context_t *ctx, nonce_response_t *nonces,
                      int max_count) {
    if (!ctx || !ctx->initialized || !nonces) {
        return -1;
    }

    int available = bm1398_get_nonce_count(ctx);
    if (available <= 0) {
        return 0;
    }

    int count = available < max_count ? available : max_count;
    int read_count = 0;

    for (int i = 0; i < count; i++) {
        if (bm1398_read_nonce(ctx, &nonces[read_count]) > 0) {
            read_count++;
        }
    }

    return read_count;
}

//==============================================================================
// PSU Power Control
//==============================================================================

// GPIO configuration
#define PSU_ENABLE_GPIO     907
#define GPIO_SYSFS_PATH     "/sys/class/gpio"

// I2C control bits (FPGA register 0x0C)
#define REG_I2C_CTRL        0x0C
#define I2C_READY           (1U << 31)
#define I2C_DATA_READY      (0x2U << 30)
#define I2C_READ_OP         (1U << 25)
#define I2C_READ_1BYTE      (1U << 19)
#define I2C_REGADDR_VALID   (1U << 24)

// PSU I2C addressing
#define PSU_I2C_MASTER      1
#define PSU_I2C_SLAVE_HIGH  0x02
#define PSU_I2C_SLAVE_LOW   0x00

// PSU protocol
#define PSU_REG_LEGACY      0x00
#define PSU_REG_V2          0x11
#define PSU_DETECT_MAGIC    0xF5
#define PSU_MAGIC_1         0x55
#define PSU_MAGIC_2         0xAA
#define CMD_GET_TYPE        0x02
#define CMD_SET_VOLTAGE     0x83

// Timeouts
#define I2C_TIMEOUT_MS      1000
#define PSU_SEND_DELAY_MS   400
#define PSU_READ_DELAY_MS   100
#define PSU_RETRIES         3

// PSU state (detect once per driver instance)
static uint8_t g_psu_reg = PSU_REG_V2;
static uint8_t g_psu_version = 0;

/**
 * GPIO helper functions
 */
static int gpio_write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;

    ssize_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);

    return (written == len) ? 0 : -1;
}

static int gpio_setup(int gpio, int value) {
    char path[64], buf[16];

    // Export (ignore if already exported)
    snprintf(buf, sizeof(buf), "%d", gpio);
    gpio_write_file(GPIO_SYSFS_PATH "/export", buf);

    // Set direction
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/direction", gpio);
    if (gpio_write_file(path, "out") < 0) {
        return -1;
    }

    // Set value
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/value", gpio);
    snprintf(buf, sizeof(buf), "%d", value);
    if (gpio_write_file(path, buf) < 0) {
        return -1;
    }

    return 0;
}

/**
 * I2C helper functions
 */
static inline uint32_t i2c_build_cmd(uint8_t reg, uint8_t data, bool read) {
    uint32_t cmd = (PSU_I2C_MASTER << 26) |
                   (PSU_I2C_SLAVE_HIGH << 20) |
                   ((PSU_I2C_SLAVE_LOW & 0x0E) << 15) |
                   I2C_REGADDR_VALID | (reg << 8);

    if (read) {
        cmd |= I2C_READ_OP | I2C_READ_1BYTE;
    } else {
        cmd |= data;
    }

    return cmd;
}

static int i2c_wait_ready(volatile uint32_t *regs) {
    for (int i = 0; i < I2C_TIMEOUT_MS / 5; i++) {
        if (regs[REG_I2C_CTRL] & I2C_READY)
            return 0;
        usleep(5000);
    }
    return -1;
}

static int i2c_wait_data(volatile uint32_t *regs, uint8_t *data) {
    for (int i = 0; i < I2C_TIMEOUT_MS / 5; i++) {
        uint32_t val = regs[REG_I2C_CTRL];
        if ((val >> 30) == 2) {
            *data = (uint8_t)(val & 0xFF);
            return 0;
        }
        usleep(5000);
    }
    return -1;
}

static int i2c_write_byte(volatile uint32_t *regs, uint8_t reg, uint8_t data) {
    uint8_t dummy;

    if (i2c_wait_ready(regs) < 0) return -1;

    regs[REG_I2C_CTRL] = i2c_build_cmd(reg, data, false);
    __sync_synchronize();

    return i2c_wait_data(regs, &dummy);
}

static int i2c_read_byte(volatile uint32_t *regs, uint8_t reg, uint8_t *data) {
    if (i2c_wait_ready(regs) < 0) return -1;

    regs[REG_I2C_CTRL] = i2c_build_cmd(reg, 0, true);
    __sync_synchronize();

    return i2c_wait_data(regs, data);
}

/**
 * PSU protocol detection and initialization
 */
static uint16_t calc_checksum(const uint8_t *data, size_t start, size_t end) {
    uint16_t sum = 0;
    for (size_t i = start; i < end; i++)
        sum += data[i];
    return sum;
}

static int psu_transact(volatile uint32_t *regs, const uint8_t *tx, size_t tx_len,
                       uint8_t *rx, size_t rx_len) {
    for (int retry = 0; retry < PSU_RETRIES; retry++) {
        // Send command
        bool tx_ok = true;
        for (size_t i = 0; i < tx_len && tx_ok; i++)
            tx_ok = (i2c_write_byte(regs, g_psu_reg, tx[i]) == 0);
        if (!tx_ok) continue;

        usleep(PSU_SEND_DELAY_MS * 1000);

        // Read response
        bool rx_ok = true;
        for (size_t i = 0; i < rx_len && rx_ok; i++)
            rx_ok = (i2c_read_byte(regs, g_psu_reg, &rx[i]) == 0);
        if (!rx_ok) continue;

        usleep(PSU_READ_DELAY_MS * 1000);

        // Validate magic bytes
        if (rx[0] == PSU_MAGIC_1 && rx[1] == PSU_MAGIC_2)
            return 0;
    }

    return -1;
}

static int psu_detect_protocol(volatile uint32_t *regs) {
    uint8_t test_val = PSU_DETECT_MAGIC, read_val;

    // Try V2 first
    g_psu_reg = PSU_REG_V2;
    if (i2c_write_byte(regs, g_psu_reg, test_val) == 0) {
        usleep(10000);
        if (i2c_read_byte(regs, g_psu_reg, &read_val) == 0 && read_val == test_val) {
            return 0;  // V2 protocol
        }
    }

    // Fallback to legacy
    g_psu_reg = PSU_REG_LEGACY;
    return 0;
}

static int psu_get_version(volatile uint32_t *regs) {
    uint8_t tx[8] = {PSU_MAGIC_1, PSU_MAGIC_2, 4, CMD_GET_TYPE};
    uint8_t rx[8];
    uint16_t csum = calc_checksum(tx, 2, 4);
    tx[4] = csum & 0xFF;
    tx[5] = (csum >> 8) & 0xFF;

    if (psu_transact(regs, tx, 6, rx, 8) < 0)
        return -1;

    g_psu_version = rx[4];
    return 0;
}

static uint16_t voltage_to_psu(uint32_t mv) {
    // PSU version 0x71 formula
    int64_t n = (1190935338LL - ((int64_t)mv * 78743LL)) / 1000000LL;
    if (n < 9) n = 9;
    if (n > 246) n = 246;
    return (uint16_t)n;
}

static int psu_set_voltage(volatile uint32_t *regs, uint32_t mv) {
    if (g_psu_version != 0x71) {
        fprintf(stderr, "Error: Unsupported PSU version 0x%02X\n", g_psu_version);
        return -1;
    }

    uint16_t n = voltage_to_psu(mv);
    uint8_t tx[8] = {PSU_MAGIC_1, PSU_MAGIC_2, 6, CMD_SET_VOLTAGE,
                     (uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
    uint8_t rx[8];
    uint16_t csum = calc_checksum(tx, 2, 6);
    tx[6] = csum & 0xFF;
    tx[7] = (csum >> 8) & 0xFF;

    if (psu_transact(regs, tx, 8, rx, 8) < 0)
        return -1;

    return (rx[3] == CMD_SET_VOLTAGE) ? 0 : -1;
}

//==============================================================================
// PIC Hashboard Power Control (FPGA I2C)
//==============================================================================

// PIC I2C addressing via FPGA
#define PIC_I2C_MASTER      0
#define PIC_I2C_SLAVE_HIGH  0x04

/**
 * Build FPGA I2C command for PIC communication
 *
 * Based on factory test i2c_write-001ca624.c line 46:
 * fpga_write(0xc, (slave_addr >> 4) << 0x14 | master << 0x1a |
 *                 ((slave_addr << 0x1c) >> 0x1d) << 0x10 | data)
 *
 * Where slave_addr = (chain << 1) | (0x04 << 4)
 */
static inline uint32_t pic_i2c_build_cmd(uint8_t chain, uint8_t data, bool read) {
    uint8_t slave_addr = (chain << 1) | (PIC_I2C_SLAVE_HIGH << 4);

    uint32_t cmd = (PIC_I2C_MASTER << 26) |
                   ((slave_addr >> 4) << 20) |
                   ((slave_addr & 0x0E) << 15);

    if (read) {
        cmd |= I2C_READ_OP | I2C_READ_1BYTE;
    } else {
        cmd |= data;
    }

    return cmd;
}

static int pic_i2c_write_byte(volatile uint32_t *regs, uint8_t chain, uint8_t data) {
    uint8_t dummy;

    if (i2c_wait_ready(regs) < 0) return -1;

    regs[REG_I2C_CTRL] = pic_i2c_build_cmd(chain, data, false);
    __sync_synchronize();

    return i2c_wait_data(regs, &dummy);
}

static int pic_i2c_read_byte(volatile uint32_t *regs, uint8_t chain, uint8_t *data) {
    if (i2c_wait_ready(regs) < 0) return -1;

    regs[REG_I2C_CTRL] = pic_i2c_build_cmd(chain, 0, true);
    __sync_synchronize();

    return i2c_wait_data(regs, data);
}

/**
 * Enable hashboard DC-DC converter via PIC I2C
 *
 * NOTE: This may not be necessary if DC-DC is already enabled from
 * previous run or if it auto-enables on PSU power-on.
 *
 * Based on factory test enable_dc_dc-001c5ae4.c and i2c_write-001ca624.c
 */
int bm1398_enable_dc_dc(bm1398_context_t *ctx, int chain) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    volatile uint32_t *regs = ctx->fpga_regs;
    uint8_t send_data[7] = {
        0x55,       // Magic byte 1
        0xAA,       // Magic byte 2
        0x05,       // Length
        0x15,       // Command: Enable DC-DC
        0x01,       // Data: Enable
        0x00,       // Padding
        0x1B        // Checksum
    };

    printf("Attempting to enable PIC DC-DC converter for chain %d...\n", chain);
    printf("  PIC slave address: 0x%02X\n", (chain << 1) | (PIC_I2C_SLAVE_HIGH << 4));

    // Send command
    for (int i = 0; i < 7; i++) {
        if (pic_i2c_write_byte(regs, chain, send_data[i]) < 0) {
            fprintf(stderr, "  Warning: PIC write byte %d failed (may already be enabled)\n", i);
            return -1;
        }
    }

    // Wait for PIC to process
    usleep(300000);

    // Read response
    uint8_t read_data[2] = {0};
    for (int i = 0; i < 2; i++) {
        if (pic_i2c_read_byte(regs, chain, &read_data[i]) < 0) {
            fprintf(stderr, "  Warning: PIC read byte %d failed (may already be enabled)\n", i);
            return -1;
        }
    }

    // Validate response
    if (read_data[0] != 0x15 || read_data[1] != 0x01) {
        fprintf(stderr, "  Warning: PIC DC-DC response unexpected: 0x%02X 0x%02X (may already be enabled)\n",
                read_data[0], read_data[1]);
        return -1;
    }

    printf("  PIC DC-DC converter enabled (response: 0x%02X 0x%02X)\n",
           read_data[0], read_data[1]);
    return 0;
}

/**
 * Power on PSU at specified voltage
 *
 * Sequence:
 * 1. Detect PSU protocol (V2 or legacy)
 * 2. Read PSU version
 * 3. Set voltage via I2C
 * 4. Enable PSU via GPIO 907 (write 0 to enable)
 * 5. Wait 2 seconds for power to settle
 *
 * Based on psu_test.c and factory test APW_power_on-0005e6f8.c
 */
int bm1398_psu_power_on(bm1398_context_t *ctx, uint32_t voltage_mv) {
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    // Detect PSU protocol (if not already detected)
    if (g_psu_version == 0) {
        if (psu_detect_protocol(ctx->fpga_regs) < 0) {
            fprintf(stderr, "Error: PSU protocol detection failed\n");
            return -1;
        }

        // Read PSU version
        if (psu_get_version(ctx->fpga_regs) < 0) {
            fprintf(stderr, "Warning: Could not read PSU version, assuming 0x71\n");
            g_psu_version = 0x71;
        }
    }

    // Set voltage via I2C
    if (psu_set_voltage(ctx->fpga_regs, voltage_mv) < 0) {
        fprintf(stderr, "Error: Failed to set PSU voltage to %umV\n", voltage_mv);
        return -1;
    }

    // Enable PSU via GPIO 907 (write 0 to enable)
    if (gpio_setup(PSU_ENABLE_GPIO, 0) < 0) {
        fprintf(stderr, "Error: Failed to enable PSU GPIO %d\n", PSU_ENABLE_GPIO);
        return -1;
    }

    // Wait 2 seconds for power to settle (from psu_test.c)
    sleep(2);

    return 0;
}
