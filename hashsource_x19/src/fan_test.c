/*
 * X19 Fan Control Test
 *
 * Simple fan speed ramp test with proper initialization
 * Gradually increases fan speed from 10% to 100% with 5% increments
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#define AXI_DEVICE      "/dev/axi_fpga_dev"
#define AXI_SIZE        0x1200
#define REG_PWM_MAIN    0x084
#define REG_PWM_ALT     0x0A0

static volatile uint32_t *regs = NULL;
static int fd = -1;
static volatile int g_shutdown = 0;

void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    g_shutdown = 1;
}

// Initialize FPGA registers
int fpga_init(void) {
    printf("Opening %s...\n", AXI_DEVICE);

    fd = open(AXI_DEVICE, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", AXI_DEVICE, strerror(errno));
        return -1;
    }

    regs = mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (regs == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("FPGA registers mapped at %p\n\n", (void*)regs);
    return 0;
}

void fpga_close(void) {
    if (regs) {
        munmap((void*)regs, AXI_SIZE);
    }
    if (fd >= 0) {
        close(fd);
    }
}

// Set fan speed using stock firmware format
void set_fan_speed(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    // Stock firmware format: (percent << 16) | (100 - percent)
    uint32_t pwm_value = (percent << 16) | (100 - percent);

    regs[REG_PWM_MAIN / 4] = pwm_value;
    regs[REG_PWM_ALT / 4] = pwm_value;

    // Memory barrier for ARM-FPGA coherency
    __sync_synchronize();
}

// Perform stock firmware initialization sequence
void perform_initialization(void) {
    printf("========================================\n");
    printf("FPGA Initialization Sequence\n");
    printf("========================================\n\n");

    printf("Current register state:\n");
    printf("  0x000 = 0x%08X\n", regs[0x000/4]);
    printf("  0x080 = 0x%08X\n", regs[0x080/4]);
    printf("  0x088 = 0x%08X\n\n", regs[0x088/4]);

    // Stage 1: Boot-time initialization
    printf("Stage 1: Boot-time initialization\n");

    // Set bit 30 in register 0 (BM1391 init)
    uint32_t reg0 = regs[0x000/4];
    if (!(reg0 & 0x40000000)) {
        regs[0x000/4] = reg0 | 0x40000000;
        usleep(100000);
        printf("  Set 0x000 = 0x%08X (bit 30 set)\n", regs[0x000/4]);
    } else {
        printf("  0x000 = 0x%08X (already correct)\n", regs[0x000/4]);
    }

    // Set register 0x080
    regs[0x080/4] = 0x0080800F;
    usleep(100000);
    printf("  Set 0x080 = 0x%08X\n", regs[0x080/4]);

    // Set register 0x088
    regs[0x088/4] = 0x800001C1;
    usleep(100000);
    printf("  Set 0x088 = 0x%08X\n\n", regs[0x088/4]);

    // Stage 2: Bmminer startup sequence
    printf("Stage 2: Bmminer startup sequence\n");

    regs[0x080/4] = 0x8080800F;
    usleep(50000);
    printf("  Set 0x080 = 0x%08X (bit 31 set)\n", regs[0x080/4]);

    regs[0x088/4] = 0x00009C40;
    usleep(50000);
    printf("  Set 0x088 = 0x%08X\n", regs[0x088/4]);

    regs[0x080/4] = 0x0080800F;
    usleep(50000);
    printf("  Set 0x080 = 0x%08X (bit 31 clear)\n", regs[0x080/4]);

    regs[0x088/4] = 0x8001FFFF;
    usleep(100000);
    printf("  Set 0x088 = 0x%08X (final config)\n\n", regs[0x088/4]);

    printf("Initialization complete!\n\n");
}

int main(int argc, char *argv[]) {
    printf("=== X19 Fan Speed Ramp Test ===\n\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Check root
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Must run as root\n");
        return 1;
    }

    // Initialize hardware
    if (fpga_init() < 0) {
        return 1;
    }

    // Perform initialization sequence
    perform_initialization();

    // Run fan speed ramp test
    printf("========================================\n");
    printf("Fan Speed Ramp Test\n");
    printf("========================================\n");
    printf("Ramping from 10%% to 100%% in 5%% increments\n");
    printf("10 second hold at each speed\n");
    printf("Press Ctrl+C to stop\n\n");

    for (int speed = 10; speed <= 100 && !g_shutdown; speed += 5) {
        printf("Setting fan speed to %3d%%...", speed);
        fflush(stdout);

        set_fan_speed(speed);

        uint32_t pwm_value = (speed << 16) | (100 - speed);
        printf(" (PWM: 0x%08X)\n", pwm_value);

        // Hold for 10 seconds
        for (int i = 0; i < 10 && !g_shutdown; i++) {
            sleep(1);
        }
    }

    if (!g_shutdown) {
        printf("\n========================================\n");
        printf("Test Complete!\n");
        printf("========================================\n\n");
    }

    // Set to safe default (50%)
    printf("Setting fans to 50%% before exit...\n");
    set_fan_speed(50);

    fpga_close();
    printf("Goodbye!\n");

    return 0;
}
