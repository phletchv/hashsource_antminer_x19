/*
 * FPGA Register Dump Tool
 * Captures complete snapshot of FPGA register state for comparison
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#define FPGA_DEVICE "/dev/axi_fpga_dev"
#define FPGA_SIZE 0x1200
#define NUM_REGS (FPGA_SIZE / 4)

// Register names for key registers (from bitmaintech and analysis)
struct reg_info {
    uint32_t offset;
    const char *name;
    const char *description;
};

static const struct reg_info known_regs[] = {
    {0x000, "HARDWARE_VERSION", "FPGA firmware version"},
    {0x004, "FAN_SPEED", "Fan tachometer readings"},
    {0x008, "HASH_ON_PLUG", "Chain detection register"},
    {0x00C, "BUFFER_SPACE", "Work FIFO buffer space"},
    {0x010, "RETURN_NONCE", "Nonce return FIFO read"},
    {0x014, "NONCE_TIMEOUT", "Nonce return timeout config"},
    {0x018, "NONCE_NUMBER_IN_FIFO", "Nonce FIFO count"},
    {0x01C, "NONCE_FIFO_INTERRUPT", "Nonce FIFO interrupt control"},
    {0x020, "TEMPERATURE_0_3", "Chip temperature sensors 0-3"},
    {0x024, "TEMPERATURE_4_7", "Chip temperature sensors 4-7"},
    {0x028, "TEMPERATURE_8_11", "Chip temperature sensors 8-11"},
    {0x02C, "TEMPERATURE_12_15", "Chip temperature sensors 12-15"},
    {0x030, "IIC_COMMAND", "I2C command (PSU/PIC control)"},
    {0x034, "RESET_HASHBOARD_COMMAND", "Hashboard reset control"},
    {0x040, "TW_WRITE_COMMAND_0", "Work data bytes 0-3"},
    {0x044, "TW_WRITE_COMMAND_1", "Work data bytes 4-7"},
    {0x048, "TW_WRITE_COMMAND_2", "Work data bytes 8-11"},
    {0x04C, "TW_WRITE_COMMAND_3", "Work data bytes 12-15"},
    {0x050, "TW_WRITE_COMMAND_4", "Work data bytes 16-19"},
    {0x080, "QN_WRITE_COMMAND", "Quick nonce write command"},
    {0x084, "FAN_CONTROL", "PWM fan control"},
    {0x088, "TIME_OUT_CONTROL", "Timeout configuration"},
    {0x08C, "BAUD_CLOCK_SEL", "Baud rate clock select"},
    {0x0A0, "PIC_COMMAND_0", "PIC communication register 0"},
    {0x0A4, "PIC_COMMAND_1", "PIC communication register 1"},
    {0x0A8, "PIC_COMMAND_2", "PIC communication register 2"},
    {0x0AC, "PIC_COMMAND_3", "PIC communication register 3"},
    {0x0C0, "BC_WRITE_COMMAND", "Broadcast command trigger"},
    {0x0C4, "BC_COMMAND_BUFFER_0", "Broadcast buffer bytes 0-3"},
    {0x0C8, "BC_COMMAND_BUFFER_1", "Broadcast buffer bytes 4-7"},
    {0x0CC, "BC_COMMAND_BUFFER_2", "Broadcast buffer bytes 8-11"},
    {0, NULL, NULL}
};

const char* get_reg_name(uint32_t offset) {
    for (int i = 0; known_regs[i].name != NULL; i++) {
        if (known_regs[i].offset == offset) {
            return known_regs[i].name;
        }
    }
    return NULL;
}

const char* get_reg_desc(uint32_t offset) {
    for (int i = 0; known_regs[i].name != NULL; i++) {
        if (known_regs[i].offset == offset) {
            return known_regs[i].description;
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int show_all = 0;
    int show_desc = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "-a") == 0) {
            show_all = 1;
        } else if (strcmp(argv[i], "--desc") == 0 || strcmp(argv[i], "-d") == 0) {
            show_desc = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("FPGA Register Dump Tool\n\n");
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -a, --all   Show all registers (default: only non-zero)\n");
            printf("  -d, --desc  Show register descriptions\n");
            printf("  -h, --help  Show this help\n\n");
            return 0;
        }
    }

    // Open FPGA device
    int fd = open(FPGA_DEVICE, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open %s: %s\n", FPGA_DEVICE, strerror(errno));
        fprintf(stderr, "Are you running as root?\n");
        return 1;
    }

    // Memory map FPGA registers
    volatile uint32_t *regs = mmap(NULL, FPGA_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
    if (regs == MAP_FAILED) {
        fprintf(stderr, "Error: Failed to mmap: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("# FPGA Register Dump\n");
    printf("# Device: %s\n", FPGA_DEVICE);
    printf("# Size: 0x%03X (%d registers)\n", FPGA_SIZE, NUM_REGS);
    printf("# Format: OFFSET VALUE [NAME] [DESCRIPTION]\n");
    printf("#\n\n");

    // Dump all registers
    int count = 0;
    for (uint32_t i = 0; i < NUM_REGS; i++) {
        uint32_t offset = i * 4;
        uint32_t value = regs[i];

        // Skip zero registers unless --all
        if (!show_all && value == 0) {
            continue;
        }

        const char *name = get_reg_name(offset);
        const char *desc = get_reg_desc(offset);

        printf("0x%03X: 0x%08X", offset, value);

        if (name) {
            printf("  # %s", name);
            if (show_desc && desc) {
                printf(" - %s", desc);
            }
        }

        printf("\n");
        count++;
    }

    printf("\n# Total: %d registers displayed\n", count);

    // Cleanup
    munmap((void*)regs, FPGA_SIZE);
    close(fd);

    return 0;
}
