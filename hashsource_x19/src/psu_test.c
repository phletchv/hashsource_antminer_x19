/*
 * X19 APW12 PSU Voltage Ramp Test
 *
 * Tests PSU voltage control via FPGA I2C controller with voltage ramping
 * from 15V down to 12V and back up to 15V in VOLTAGE_STEP increments.
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
#include <signal.h>

// Device paths
#define AXI_DEVICE          "/dev/axi_fpga_dev"
#define GPIO_SYSFS_PATH     "/sys/class/gpio"

// FPGA configuration
#define AXI_SIZE            0x1200
#define REG_I2C_CTRL        0x0C

// I2C control bits
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

// GPIO configuration
#define PSU_ENABLE_GPIO     907    // Stock kernel: gpiochip906 + MIO_1

// Test parameters
#define VOLTAGE_MIN         12000  // 12.0V
#define VOLTAGE_MAX         15000  // 15.0V
#define VOLTAGE_STEP        500    // 0.5V
#define POWER_RELEASE_SECS  30
#define VOLTAGE_SETTLE_SECS 2
#define VOLTAGE_HOLD_SECS   5
#define RAMP_STEP_SECS      3

// Timeouts
#define I2C_TIMEOUT_MS      1000
#define PSU_SEND_DELAY_MS   400
#define PSU_READ_DELAY_MS   100
#define PSU_RETRIES         3

// Hardware state
static volatile uint32_t *g_fpga_regs = NULL;
static int g_fpga_fd = -1;
static uint8_t g_psu_reg = PSU_REG_V2;
static uint8_t g_psu_version = 0;
static volatile sig_atomic_t g_shutdown = 0;

// Signal handler
static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

// Error handling
#define CHECK(expr, msg, ...) \
    do { if ((expr) < 0) { fprintf(stderr, "Error: " msg "\n", ##__VA_ARGS__); goto cleanup; } } while(0)

//==============================================================================
// GPIO Operations
//==============================================================================

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
        fprintf(stderr, "Failed to set GPIO %d direction\n", gpio);
        return -1;
    }

    // Set value
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/value", gpio);
    snprintf(buf, sizeof(buf), "%d", value);
    if (gpio_write_file(path, buf) < 0) {
        fprintf(stderr, "Failed to set GPIO %d value\n", gpio);
        return -1;
    }

    return 0;
}

//==============================================================================
// FPGA I2C Operations
//==============================================================================

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

static int i2c_wait_ready(void) {
    for (int i = 0; i < I2C_TIMEOUT_MS / 5; i++) {
        if (g_fpga_regs[REG_I2C_CTRL] & I2C_READY)
            return 0;
        usleep(5000);
    }
    fprintf(stderr, "I2C timeout waiting for ready\n");
    return -1;
}

static int i2c_wait_data(uint8_t *data) {
    for (int i = 0; i < I2C_TIMEOUT_MS / 5; i++) {
        uint32_t val = g_fpga_regs[REG_I2C_CTRL];
        if ((val >> 30) == 2) {
            *data = (uint8_t)(val & 0xFF);
            return 0;
        }
        usleep(5000);
    }
    fprintf(stderr, "I2C timeout waiting for data\n");
    return -1;
}

static int i2c_write_byte(uint8_t reg, uint8_t data) {
    uint8_t dummy;

    if (i2c_wait_ready() < 0) return -1;

    g_fpga_regs[REG_I2C_CTRL] = i2c_build_cmd(reg, data, false);
    __sync_synchronize();

    return i2c_wait_data(&dummy);
}

static int i2c_read_byte(uint8_t reg, uint8_t *data) {
    if (i2c_wait_ready() < 0) return -1;

    g_fpga_regs[REG_I2C_CTRL] = i2c_build_cmd(reg, 0, true);
    __sync_synchronize();

    return i2c_wait_data(data);
}

//==============================================================================
// PSU Protocol
//==============================================================================

static uint16_t calc_checksum(const uint8_t *data, size_t start, size_t end) {
    uint16_t sum = 0;
    for (size_t i = start; i < end; i++)
        sum += data[i];
    return sum;
}

static int psu_transact(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len) {
    for (int retry = 0; retry < PSU_RETRIES; retry++) {
        // Send command
        bool tx_ok = true;
        for (size_t i = 0; i < tx_len && tx_ok; i++)
            tx_ok = (i2c_write_byte(g_psu_reg, tx[i]) == 0);
        if (!tx_ok) continue;

        usleep(PSU_SEND_DELAY_MS * 1000);

        // Read response
        bool rx_ok = true;
        for (size_t i = 0; i < rx_len && rx_ok; i++)
            rx_ok = (i2c_read_byte(g_psu_reg, &rx[i]) == 0);
        if (!rx_ok) continue;

        usleep(PSU_READ_DELAY_MS * 1000);

        // Validate magic bytes
        if (rx[0] == PSU_MAGIC_1 && rx[1] == PSU_MAGIC_2)
            return 0;
    }

    return -1;
}

static int psu_detect_protocol(void) {
    uint8_t test_val = PSU_DETECT_MAGIC, read_val;

    printf("Detecting PSU protocol...\n");

    // Try V2 first
    g_psu_reg = PSU_REG_V2;
    if (i2c_write_byte(g_psu_reg, test_val) == 0) {
        usleep(10000);
        if (i2c_read_byte(g_psu_reg, &read_val) == 0 && read_val == test_val) {
            printf("  V2 protocol (register 0x11)\n");
            return 0;
        }
    }

    // Fallback to legacy
    g_psu_reg = PSU_REG_LEGACY;
    printf("  Legacy protocol (register 0x00)\n");
    return 0;
}

static int psu_get_version(void) {
    uint8_t tx[8] = {PSU_MAGIC_1, PSU_MAGIC_2, 4, CMD_GET_TYPE};
    uint8_t rx[8];
    uint16_t csum = calc_checksum(tx, 2, 4);
    tx[4] = csum & 0xFF;
    tx[5] = (csum >> 8) & 0xFF;

    if (psu_transact(tx, 6, rx, 8) < 0)
        return -1;

    g_psu_version = rx[4];
    printf("  PSU version: 0x%02X\n", g_psu_version);
    return 0;
}

static uint16_t voltage_to_psu(uint32_t mv) {
    // Version 0x71 formula
    int64_t n = (1190935338LL - ((int64_t)mv * 78743LL)) / 1000000LL;
    if (n < 9) n = 9;
    if (n > 246) n = 246;
    return (uint16_t)n;
}

static int psu_set_voltage(uint32_t mv) {
    if (g_psu_version != 0x71) {
        fprintf(stderr, "Unsupported PSU version 0x%02X\n", g_psu_version);
        return -1;
    }

    uint16_t n = voltage_to_psu(mv);
    uint8_t tx[8] = {PSU_MAGIC_1, PSU_MAGIC_2, 6, CMD_SET_VOLTAGE,
                     (uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
    uint8_t rx[8];
    uint16_t csum = calc_checksum(tx, 2, 6);
    tx[6] = csum & 0xFF;
    tx[7] = (csum >> 8) & 0xFF;

    if (psu_transact(tx, 8, rx, 8) < 0)
        return -1;

    return (rx[3] == CMD_SET_VOLTAGE) ? 0 : -1;
}

//==============================================================================
// FPGA Operations
//==============================================================================

static int fpga_init(void) {
    g_fpga_fd = open(AXI_DEVICE, O_RDWR | O_SYNC);
    if (g_fpga_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", AXI_DEVICE, strerror(errno));
        return -1;
    }

    g_fpga_regs = mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_fpga_fd, 0);
    if (g_fpga_regs == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap FPGA: %s\n", strerror(errno));
        close(g_fpga_fd);
        g_fpga_fd = -1;
        return -1;
    }

    return 0;
}

static void fpga_cleanup(void) {
    if (g_fpga_regs != NULL && g_fpga_regs != MAP_FAILED)
        munmap((void*)g_fpga_regs, AXI_SIZE);
    if (g_fpga_fd >= 0)
        close(g_fpga_fd);
}

//==============================================================================
// Voltage Ramp Test
//==============================================================================

static int voltage_ramp(uint32_t start_mv, uint32_t end_mv, int32_t step_mv) {
    const char *dir = (step_mv > 0) ? "UP" : "DOWN";

    printf("Ramping %s: %.2fV → %.2fV\n", dir, start_mv/1000.0, end_mv/1000.0);
    printf("----------------------------------------\n");

    for (uint32_t v = start_mv;
         (step_mv > 0) ? (v <= end_mv) : (v >= end_mv);
         v += step_mv) {

        if (g_shutdown) return -1;

        printf("  %.2fV... ", v/1000.0);
        fflush(stdout);

        if (psu_set_voltage(v) < 0) {
            fprintf(stderr, "FAILED\n");
            return -1;
        }

        printf("OK\n");
        sleep(RAMP_STEP_SECS);
    }

    printf("\nReached %.2fV, holding for %ds...\n\n", end_mv/1000.0, VOLTAGE_HOLD_SECS);
    sleep(VOLTAGE_HOLD_SECS);

    return 0;
}

//==============================================================================
// Main
//==============================================================================

int main(void) {
    int ret = 1;

    printf("========================================\n");
    printf("X19 APW12 PSU Voltage Ramp Test\n");
    printf("========================================\n\n");
    printf("Sequence: 15V → 12V → 15V (%.2fV steps)\n\n", VOLTAGE_STEP/1000.0);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (geteuid() != 0) {
        fprintf(stderr, "Error: Must run as root\n");
        return 1;
    }

    // Initialize FPGA
    CHECK(fpga_init(), "FPGA initialization failed");
    printf("FPGA mapped at %p\n\n", (void*)g_fpga_regs);

    // Power release
    printf("Power Release\n");
    printf("----------------------------------------\n");
    CHECK(gpio_setup(PSU_ENABLE_GPIO, 1), "Failed to setup GPIO %d", PSU_ENABLE_GPIO);
    printf("PSU disabled (GPIO %d HIGH)\n", PSU_ENABLE_GPIO);

    printf("Waiting %ds for capacitor discharge...\n", POWER_RELEASE_SECS);
    for (int i = POWER_RELEASE_SECS; i > 0 && !g_shutdown; i--) {
        printf("\r  %ds remaining...", i);
        fflush(stdout);
        sleep(1);
    }
    printf("\rPower release complete!    \n\n");

    if (g_shutdown) goto cleanup;

    // PSU initialization
    printf("PSU Initialization\n");
    printf("----------------------------------------\n");
    CHECK(psu_detect_protocol(), "Protocol detection failed");

    if (psu_get_version() < 0)
        fprintf(stderr, "Warning: Could not read version\n");

    CHECK(psu_set_voltage(VOLTAGE_MAX), "Failed to set initial voltage");
    printf("Initial voltage: %.2fV\n", VOLTAGE_MAX/1000.0);

    CHECK(gpio_setup(PSU_ENABLE_GPIO, 0), "Failed to enable PSU");
    printf("PSU enabled (GPIO %d LOW)\n", PSU_ENABLE_GPIO);

    printf("Settling for %ds...\n\n", VOLTAGE_SETTLE_SECS);
    sleep(VOLTAGE_SETTLE_SECS);

    // Voltage ramp test
    printf("Voltage Ramp Test\n");
    printf("========================================\n\n");

    if (voltage_ramp(VOLTAGE_MAX, VOLTAGE_MIN, -VOLTAGE_STEP) < 0) goto cleanup;
    if (voltage_ramp(VOLTAGE_MIN, VOLTAGE_MAX, VOLTAGE_STEP) < 0) goto cleanup;

    // Shutdown
    printf("Shutdown\n");
    printf("========================================\n");
    gpio_setup(PSU_ENABLE_GPIO, 1);
    printf("PSU disabled\n\n");

    printf("Test complete!\n");
    ret = 0;

cleanup:
    fpga_cleanup();
    return ret;
}
