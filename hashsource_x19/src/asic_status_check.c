#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "bm1398_asic.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <chain_id>\n", argv[0]);
        return 1;
    }

    int chain = atoi(argv[1]);

    printf("\n====================================\n");
    printf("BM1398 ASIC Status Check\n");
    printf("====================================\n");
    printf("Chain: %d\n\n", chain);

    // Initialize context
    bm1398_context_t ctx;
    if (bm1398_init(&ctx) < 0) {
        fprintf(stderr, "Failed to initialize BM1398 context\n");
        return 1;
    }

    // Detect chains
    int chain_mask = bm1398_detect_chains(&ctx);
    printf("Detected chains: 0x%08X\n", chain_mask);

    if (!(chain_mask & (1 << chain))) {
        fprintf(stderr, "Chain %d not detected\n", chain);
        bm1398_cleanup(&ctx);
        return 1;
    }

    // Get chip count
    int chip_count = ctx.chips_per_chain[chain];
    printf("Chain %d has %d chips\n\n", chain, chip_count);

    // Read status from first few chips
    int chips_to_check = (chip_count > 5) ? 5 : chip_count;

    printf("Reading ASIC registers from first %d chips:\n\n", chips_to_check);

    for (int chip = 0; chip < chips_to_check; chip++) {
        uint8_t chip_addr = chip * CHIP_ADDRESS_INTERVAL;

        printf("Chip %d (addr 0x%02X):\n", chip, chip_addr);

        // Read critical registers
        uint32_t regs_to_check[] = {
            0x00,  // Chip address
            0x08,  // PLL0 parameter
            0x14,  // Ticket mask
            0x18,  // CLK_CTRL (misc control)
            0x3C,  // Core register control
            0x44,  // Core param (timing)
            0x58,  // IO driver config
            0xA8,  // Soft reset
        };
        const char *reg_names[] = {
            "CHIP_ADDRESS",
            "PLL0_PARAMETER",
            "TICKET_MASK",
            "CLK_CTRL",
            "CORE_REG_CTRL",
            "CORE_PARAM",
            "IO_DRIVER",
            "SOFT_RESET",
        };

        for (int i = 0; i < 8; i++) {
            uint32_t value = 0;

            // Try to read register
            if (bm1398_read_register(&ctx, chain, false, chip_addr,
                                    regs_to_check[i], &value, 1000000) == 0) {
                printf("  0x%02X %-15s = 0x%08X\n",
                       regs_to_check[i], reg_names[i], value);
            } else {
                printf("  0x%02X %-15s = [READ FAILED]\n",
                       regs_to_check[i], reg_names[i]);
            }

            usleep(10000);  // 10ms between reads
        }

        printf("\n");
    }

    // Check FPGA work FIFO status
    printf("FPGA Status:\n");
    printf("  Work FIFO space: %d\n", bm1398_check_work_fifo_ready(&ctx));
    printf("  Nonce FIFO count: %d\n", bm1398_get_nonce_count(&ctx));
    printf("  Register 0x08C: 0x%08X\n", ctx.fpga_regs[0x08C / 4]);
    printf("  Register 0x0B4: 0x%08X\n", ctx.fpga_regs[0x0B4 / 4]);

    bm1398_cleanup(&ctx);
    return 0;
}
