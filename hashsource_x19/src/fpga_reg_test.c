#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FPGA_DEVICE "/dev/axi_fpga_dev"
#define FPGA_SIZE 0x1200

int main(void) {
    int fd;
    volatile uint32_t *fpga_regs;

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

    printf("FPGA Register 0x014 Write Test\n");
    printf("===============================\n\n");

    // Read initial value
    uint32_t initial = fpga_regs[0x014 / 4];
    printf("Initial value at 0x014: 0x%08X\n", initial);

    // Test write using array index
    printf("\nTest 1: Write using array index [0x014/4] = [5]\n");
    fpga_regs[5] = 0x800000F9;
    __sync_synchronize();
    usleep(1000);
    uint32_t readback1 = fpga_regs[5];
    printf("  Wrote: 0x800000F9\n");
    printf("  Read:  0x%08X %s\n", readback1,
           (readback1 == 0x800000F9) ? "[OK]" : "[FAIL]");

    // Test write using direct pointer arithmetic
    printf("\nTest 2: Write using pointer arithmetic\n");
    volatile uint32_t *reg_ptr = (volatile uint32_t *)((uint8_t *)fpga_regs + 0x014);
    *reg_ptr = 0x800000AA;
    __sync_synchronize();
    usleep(1000);
    uint32_t readback2 = *reg_ptr;
    printf("  Wrote: 0x800000AA\n");
    printf("  Read:  0x%08X %s\n", readback2,
           (readback2 == 0x800000AA) ? "[OK]" : "[FAIL]");

    // Test write with just the timeout value (no 0x80000000 bit)
    printf("\nTest 3: Write without 0x80000000 bit\n");
    fpga_regs[5] = 0x000000F9;
    __sync_synchronize();
    usleep(1000);
    uint32_t readback3 = fpga_regs[5];
    printf("  Wrote: 0x000000F9\n");
    printf("  Read:  0x%08X %s\n", readback3,
           (readback3 == 0x000000F9) ? "[OK]" : "[FAIL]");

    // Test if register is read-only by trying different values
    printf("\nTest 4: Multiple test values\n");
    uint32_t test_values[] = {0x00000001, 0x12345678, 0xFFFFFFFF, 0x80000000};
    for (int i = 0; i < 4; i++) {
        fpga_regs[5] = test_values[i];
        __sync_synchronize();
        usleep(1000);
        uint32_t read = fpga_regs[5];
        printf("  0x%08X -> 0x%08X %s\n", test_values[i], read,
               (read == test_values[i]) ? "[OK]" : "[FAIL]");
    }

    // Restore initial value
    printf("\nRestoring initial value: 0x%08X\n", initial);
    fpga_regs[5] = initial;
    __sync_synchronize();

    // Cleanup
    munmap((void *)fpga_regs, FPGA_SIZE);
    close(fd);

    return 0;
}
