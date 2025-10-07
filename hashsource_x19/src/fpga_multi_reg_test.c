#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FPGA_DEVICE "/dev/axi_fpga_dev"
#define FPGA_SIZE 0x1200

typedef struct {
    uint32_t offset;
    const char *name;
    uint32_t test_value;
} reg_test_t;

int main(void) {
    int fd;
    volatile uint32_t *fpga_regs;

    // Test various FPGA registers
    reg_test_t tests[] = {
        {0x014, "NONCE_TIMEOUT", 0x800000F9},
        {0x01C, "NONCE_FIFO_INTERRUPT", 0x00000001},
        {0x084, "FAN_CONTROL", 0x00000050},
        {0x088, "TIME_OUT_CONTROL", 0x00000100},
        {0x0B4, "WORK_SEND_ENABLE (0x2D)", 0xFFFFFFFF},
        {0x08C, "CHAIN_START (0x23)", 0x00000040},
    };
    int num_tests = sizeof(tests) / sizeof(tests[0]);

    // Open FPGA device
    fd = open(FPGA_DEVICE, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open FPGA device");
        return 1;
    }

    // Memory map FPGA registers
    fpga_regs = (volatile uint32_t *)mmap(NULL, FPGA_SIZE,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
    if (fpga_regs == MAP_FAILED) {
        perror("Failed to mmap FPGA");
        close(fd);
        return 1;
    }

    printf("FPGA Multi-Register Write Test\n");
    printf("===============================\n\n");

    for (int i = 0; i < num_tests; i++) {
        uint32_t idx = tests[i].offset / 4;
        uint32_t initial = fpga_regs[idx];

        printf("Test %d: %s (offset 0x%03X)\n", i + 1, tests[i].name, tests[i].offset);
        printf("  Initial: 0x%08X\n", initial);

        // Try to write
        fpga_regs[idx] = tests[i].test_value;
        __sync_synchronize();
        usleep(1000);

        uint32_t readback = fpga_regs[idx];
        printf("  Wrote:   0x%08X\n", tests[i].test_value);
        printf("  Read:    0x%08X %s\n", readback,
               (readback == tests[i].test_value) ? "[OK - WRITABLE]" : "[FAIL - READ-ONLY or NEEDS INIT]");

        // Restore original value
        fpga_regs[idx] = initial;
        __sync_synchronize();
        printf("\n");
    }

    // Cleanup
    munmap((void *)fpga_regs, FPGA_SIZE);
    close(fd);

    return 0;
}
