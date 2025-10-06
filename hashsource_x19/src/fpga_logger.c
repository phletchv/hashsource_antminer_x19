/*
 * FPGA Register Logger with Auto-Restart
 * Automatically restarts cgminer/bmminer and logs FPGA register changes
 * during initialization sequence
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#define FPGA_DEVICE "/dev/axi_fpga_dev"
#define FPGA_SIZE 0x1200
#define NUM_REGS (FPGA_SIZE / 4)
#define POLL_INTERVAL_US  1000  // 1ms for faster capture

static volatile int g_running = 1;
static FILE *g_logfile = NULL;

void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
}

void log_timestamp(FILE *f) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%ld.%06ld] ", ts.tv_sec, ts.tv_nsec / 1000);
}

int restart_cgminer(void) {
    printf("\n====================================\n");
    printf("Restarting cgminer/bmminer...\n");
    printf("====================================\n\n");

    // Kill existing processes
    system("killall -9 bmminer cgminer 2>/dev/null");
    sleep(2);

    // Restart via init script
    int ret = system("sudo /etc/init.d/S70cgminer restart");

    if (ret == 0) {
        printf("cgminer/bmminer restarted successfully\n");
        printf("Waiting 5 seconds before starting logger...\n\n");
        sleep(5);
        return 0;
    } else {
        fprintf(stderr, "Warning: Failed to restart cgminer (exit code %d)\n", ret);
        fprintf(stderr, "Continuing with logging anyway...\n\n");
        return -1;
    }
}

int main(int argc, char *argv[]) {
    const char *logfile = "/tmp/fpga_init.log";
    int auto_restart = 1;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-restart") == 0) {
            auto_restart = 0;
        } else {
            logfile = argv[i];
        }
    }

    printf("FPGA Register Change Logger with Auto-Restart\n");
    printf("==============================================\n");
    printf("Device: %s\n", FPGA_DEVICE);
    printf("Log file: %s\n", logfile);
    printf("Monitoring %d registers (0x000-0x%03X)\n", NUM_REGS, FPGA_SIZE - 4);
    printf("Poll interval: %d microseconds\n", POLL_INTERVAL_US);
    printf("Auto-restart: %s\n\n", auto_restart ? "yes" : "no");

    // Restart cgminer if requested
    if (auto_restart) {
        restart_cgminer();
    }

    printf("Press Ctrl+C to stop\n\n");

    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Open FPGA device
    int fd = open(FPGA_DEVICE, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", FPGA_DEVICE, strerror(errno));
        fprintf(stderr, "Are you running as root?\n");
        return 1;
    }

    // Memory map FPGA registers
    volatile uint32_t *regs = mmap(NULL, FPGA_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
    if (regs == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("FPGA registers mapped at %p\n", (void*)regs);

    // Open log file
    g_logfile = fopen(logfile, "w");
    if (!g_logfile) {
        fprintf(stderr, "Failed to open log file: %s\n", strerror(errno));
        munmap((void*)regs, FPGA_SIZE);
        close(fd);
        return 1;
    }

    // Write header
    fprintf(g_logfile, "# FPGA Register Change Log\n");
    fprintf(g_logfile, "# Format: [timestamp] OFFSET OLD_VALUE NEW_VALUE\n");
    fprintf(g_logfile, "# Timestamp in seconds.microseconds since start\n\n");
    fflush(g_logfile);

    // Allocate shadow copy for change detection
    uint32_t *shadow = malloc(FPGA_SIZE);
    if (!shadow) {
        fprintf(stderr, "Failed to allocate memory\n");
        fclose(g_logfile);
        munmap((void*)regs, FPGA_SIZE);
        close(fd);
        return 1;
    }

    // Initialize shadow with current values
    memcpy(shadow, (void*)regs, FPGA_SIZE);

    // Log initial state of key registers
    printf("Initial register state:\n");
    fprintf(g_logfile, "# Initial State\n");

    const uint32_t key_regs[] = {
        0x000, 0x004, 0x008, 0x00C,
        0x080, 0x084, 0x088, 0x08C,
        0x0A0, 0x0A4, 0x0A8, 0x0AC
    };

    for (int i = 0; i < sizeof(key_regs)/sizeof(key_regs[0]); i++) {
        uint32_t offset = key_regs[i];
        uint32_t value = regs[offset / 4];
        printf("  0x%03X = 0x%08X\n", offset, value);
        log_timestamp(g_logfile);
        fprintf(g_logfile, "INIT 0x%03X 0x%08X\n", offset, value);
    }
    printf("\n");
    fflush(g_logfile);

    printf("Monitoring started...\n");

    // Main monitoring loop
    uint64_t poll_count = 0;
    uint64_t change_count = 0;

    while (g_running) {
        // Check all registers for changes
        for (uint32_t i = 0; i < NUM_REGS; i++) {
            uint32_t current = regs[i];
            uint32_t old = shadow[i];

            if (current != old) {
                uint32_t offset = i * 4;

                // Log to file
                log_timestamp(g_logfile);
                fprintf(g_logfile, "0x%03X 0x%08X 0x%08X\n", offset, old, current);
                fflush(g_logfile);

                // Log to console for important registers
                if (offset <= 0x010 ||
                    (offset >= 0x080 && offset <= 0x0AC)) {
                    log_timestamp(stdout);
                    printf("0x%03X: 0x%08X -> 0x%08X\n", offset, old, current);
                }

                // Update shadow
                shadow[i] = current;
                change_count++;
            }
        }

        poll_count++;

        // Print status every 10 seconds
        if (poll_count % (10000000 / POLL_INTERVAL_US) == 0) {
            printf("Status: %lu polls, %lu changes detected\n",
                   poll_count, change_count);
        }

        usleep(POLL_INTERVAL_US);
    }

    printf("\nStopping...\n");
    printf("Total polls: %lu\n", poll_count);
    printf("Total changes: %lu\n", change_count);

    // Log final state
    fprintf(g_logfile, "\n# Final State\n");
    for (int i = 0; i < sizeof(key_regs)/sizeof(key_regs[0]); i++) {
        uint32_t offset = key_regs[i];
        uint32_t value = regs[offset / 4];
        log_timestamp(g_logfile);
        fprintf(g_logfile, "FINAL 0x%03X 0x%08X\n", offset, value);
    }

    // Cleanup
    free(shadow);
    fclose(g_logfile);
    munmap((void*)regs, FPGA_SIZE);
    close(fd);

    printf("Log saved to: %s\n", logfile);

    return 0;
}
