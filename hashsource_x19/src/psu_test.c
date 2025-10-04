/*
 * X19 APW12 PSU Test Program - FPGA Direct Access
 *
 * Tests PSU initialization and voltage control via FPGA I2C controller
 * Replicates stock firmware sequence exactly:
 *   1. GPIO 907 = HIGH (disable PSU)
 *   2. Sleep 30 seconds (power release)
 *   3. Detect protocol and get version
 *   4. Set voltage via FPGA I2C
 *   5. GPIO 907 = LOW (enable PSU output)
 *   6. Verify voltage with multimeter
 *
 * Based on stock firmware analysis: FUN_00019698, FUN_00042138, FUN_00042100
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

// FPGA Configuration
#define AXI_DEVICE      "/dev/axi_fpga_dev"
#define AXI_SIZE        0x1200

// FPGA Register Map
#define FPGA_REG_I2C_CTRL       0x0C    // I2C Controller
#define FPGA_REG_CHAIN_ENABLE   0x0D    // Chain enable bits (bit 0 = chain 0, bit 1 = chain 1, etc)

// FPGA I2C Control Register Bits
#define FPGA_I2C_READY       (1U << 31)     // Bit 31: Ready for operation
#define FPGA_I2C_DATA_READY  (0x2U << 30)   // Bits 31-30 = 0b10: Data ready
#define FPGA_I2C_READ_OP     (1U << 25)     // Bit 25: Read operation
#define FPGA_I2C_READ_1BYTE  (1U << 19)     // Bit 19: 1-byte read
#define FPGA_I2C_REGADDR_VALID (1U << 24)   // Bit 24: Register address valid

// PSU I2C Configuration (from decompiled bmminer FUN_0004a0dc)
// Stock firmware uses: slave_config = 0x20 = (slave_high=0x02) << 4 | (slave_low=0x00)
// Then splits it: (0x20 >> 4) << 20 | (0x20 & 0x0E) << 15
#define PSU_I2C_MASTER       1      // Master bus ID (bit 26)
#define PSU_I2C_SLAVE_HIGH   0x02   // Slave address high nibble (bits 23-20)
#define PSU_I2C_SLAVE_LOW    0x00   // Slave address low nibble (bits 17-15, masked with 0x0E)
// This creates 7-bit I2C address 0x10: (0x02 << 4) | 0x00 = 0x20 >> 1 = 0x10

// PSU Protocol
#define PSU_REG_LEGACY       0x00
#define PSU_REG_V2           0x11
#define PSU_PROTOCOL_DETECT  0xF5

#define PSU_MAGIC_1          0x55
#define PSU_MAGIC_2          0xAA

// PSU Commands
#define CMD_GET_VERSION      0x01
#define CMD_GET_TYPE         0x02
#define CMD_GET_VOLTAGE      0x03
#define CMD_SET_VOLTAGE      0x83

// GPIO Configuration
// Stock firmware uses gpio907 in sysfs (gpiochip base=906 + offset 1)
// This maps to hardware pin MIO_1 (Zynq GPIO 1)
// Our system uses base=0, so we use GPIO 1 to access the same hardware pin
#define PSU_ENABLE_GPIO      1      // MIO_1 - PSU enable pin (active LOW)
#define GPIO_SYSFS_PATH      "/sys/class/gpio"

// Timeouts
#define FPGA_I2C_TIMEOUT_MS  1000
#define PSU_DELAY_AFTER_SEND_MS  400
#define PSU_DELAY_AFTER_READ_MS  100

static volatile uint32_t *regs = NULL;
static int fd = -1;
static uint8_t psu_i2c_reg = PSU_REG_V2;  // Default to V2
static uint8_t psu_version = 0;

// Signal handling
static volatile int g_shutdown = 0;
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    g_shutdown = 1;
}

// GPIO Control via sysfs
int gpio_export(int gpio) {
    int fd = open(GPIO_SYSFS_PATH "/export", O_WRONLY);
    if (fd < 0) {
        if (errno == EACCES) {
            // Already exported, not an error
            return 0;
        }
        fprintf(stderr, "Failed to open gpio export: %s\n", strerror(errno));
        return -1;
    }

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", gpio);
    if (write(fd, buf, len) < 0) {
        if (errno != EBUSY) {  // EBUSY means already exported
            fprintf(stderr, "Failed to export gpio %d: %s\n", gpio, strerror(errno));
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int gpio_set_direction(int gpio, const char *direction) {
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/direction", gpio);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open gpio direction: %s\n", strerror(errno));
        return -1;
    }

    if (write(fd, direction, strlen(direction)) < 0) {
        fprintf(stderr, "Failed to set gpio direction: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int gpio_set_value(int gpio, int value) {
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/value", gpio);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open gpio value: %s\n", strerror(errno));
        return -1;
    }

    char buf[2] = { value ? '1' : '0', 0 };
    if (write(fd, buf, 1) < 0) {
        fprintf(stderr, "Failed to write gpio value: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

// FPGA I2C Wait for Ready
int fpga_i2c_wait_ready(void) {
    int timeout = FPGA_I2C_TIMEOUT_MS / 5;

    while (timeout-- > 0) {
        uint32_t val = regs[FPGA_REG_I2C_CTRL];
        if (val & FPGA_I2C_READY) {
            return 0;
        }
        usleep(5000);
    }

    fprintf(stderr, "FPGA I2C timeout waiting for ready\n");
    return -1;
}

// FPGA I2C Wait for Data
int fpga_i2c_wait_data(uint8_t *data) {
    int timeout = FPGA_I2C_TIMEOUT_MS / 5;

    while (timeout-- > 0) {
        uint32_t val = regs[FPGA_REG_I2C_CTRL];
        if ((val >> 30) == 2) {  // Bits 31-30 = 0b10
            *data = (uint8_t)(val & 0xFF);
            return 0;
        }
        usleep(5000);
    }

    fprintf(stderr, "FPGA I2C timeout waiting for data\n");
    return -1;
}

// FPGA I2C Write Byte
int fpga_i2c_write_byte(uint8_t reg_addr, uint8_t data, int reg_addr_valid) {
    uint32_t cmd;
    uint8_t dummy;

    if (fpga_i2c_wait_ready() < 0) {
        return -1;
    }

    // Build command word - EXACT match to stock firmware FUN_0004a0dc
    cmd = (PSU_I2C_MASTER << 26) |                    // master << 26
          (PSU_I2C_SLAVE_HIGH << 20) |                 // (slave >> 4) << 20
          ((PSU_I2C_SLAVE_LOW & 0x0E) << 15) |        // (slave & 0x0E) << 15  ← CRITICAL FIX!
          data;                                        // data at bits 0-7

    if (reg_addr_valid) {
        cmd |= FPGA_I2C_REGADDR_VALID | (reg_addr << 8);  // reg_addr << 8, valid bit 24
    }

    // Write command
    regs[FPGA_REG_I2C_CTRL] = cmd;
    __sync_synchronize();

    // Wait for completion
    if (fpga_i2c_wait_data(&dummy) < 0) {
        return -1;
    }

    return 0;
}

// FPGA I2C Read Byte
int fpga_i2c_read_byte(uint8_t reg_addr, uint8_t *data, int reg_addr_valid) {
    uint32_t cmd;

    if (fpga_i2c_wait_ready() < 0) {
        return -1;
    }

    // Build read command - EXACT match to stock firmware FUN_00049e8c
    cmd = (PSU_I2C_MASTER << 26) |                    // master << 26
          FPGA_I2C_READ_OP |                           // bit 25: read operation
          FPGA_I2C_READ_1BYTE |                        // bit 19: 1-byte read (0x80000)
          (PSU_I2C_SLAVE_HIGH << 20) |                 // (slave >> 4) << 20
          ((PSU_I2C_SLAVE_LOW & 0x0E) << 15);         // (slave & 0x0E) << 15  ← CRITICAL FIX!

    if (reg_addr_valid) {
        cmd |= FPGA_I2C_REGADDR_VALID | (reg_addr << 8);  // reg_addr << 8, valid bit 24
    }

    // Write command
    regs[FPGA_REG_I2C_CTRL] = cmd;
    __sync_synchronize();

    // Wait for data
    if (fpga_i2c_wait_data(data) < 0) {
        return -1;
    }

    return 0;
}

// Calculate checksum
uint16_t calc_checksum(uint8_t *data, size_t start, size_t end) {
    uint16_t sum = 0;
    for (size_t i = start; i < end; i++) {
        sum += data[i];
    }
    return sum;
}

// Detect PSU Protocol
int psu_detect_protocol(void) {
    uint8_t test_val = PSU_PROTOCOL_DETECT;
    uint8_t read_val;

    printf("Detecting PSU protocol...\n");

    // Try V2 protocol first (register 0x11)
    psu_i2c_reg = PSU_REG_V2;
    if (fpga_i2c_write_byte(psu_i2c_reg, test_val, 1) == 0) {
        usleep(10000);
        if (fpga_i2c_read_byte(psu_i2c_reg, &read_val, 1) == 0 && read_val == test_val) {
            printf("  Detected V2 protocol (register 0x11)\n\n");
            return 0;
        }
    }

    // Fall back to legacy protocol (register 0x00)
    psu_i2c_reg = PSU_REG_LEGACY;
    printf("  Using legacy protocol (register 0x00)\n\n");
    return 0;
}

// Get PSU Version
int psu_get_version(void) {
    uint8_t send_packet[16];
    uint8_t recv_packet[16];
    uint16_t checksum;
    int i, retry;

    printf("Reading PSU version...\n");

    // Build packet
    send_packet[0] = PSU_MAGIC_1;
    send_packet[1] = PSU_MAGIC_2;
    send_packet[2] = 4;  // Length
    send_packet[3] = CMD_GET_TYPE;
    checksum = calc_checksum(send_packet, 2, 4);
    send_packet[4] = checksum & 0xFF;
    send_packet[5] = (checksum >> 8) & 0xFF;

    for (retry = 0; retry < 3; retry++) {
        // Send command byte-by-byte
        for (i = 0; i < 6; i++) {
            if (fpga_i2c_write_byte(psu_i2c_reg, send_packet[i], 1) < 0) {
                break;
            }
        }
        if (i < 6) continue;

        usleep(PSU_DELAY_AFTER_SEND_MS * 1000);

        // Read response
        for (i = 0; i < 8; i++) {
            if (fpga_i2c_read_byte(psu_i2c_reg, &recv_packet[i], 1) < 0) {
                break;
            }
        }
        if (i < 8) continue;

        usleep(PSU_DELAY_AFTER_READ_MS * 1000);

        // Validate response
        if (recv_packet[0] == PSU_MAGIC_1 && recv_packet[1] == PSU_MAGIC_2) {
            psu_version = recv_packet[4];
            printf("  PSU Version: 0x%02X, Type: 0x%02X\n\n", psu_version, psu_version);
            return 0;
        }
    }

    fprintf(stderr, "Failed to read PSU version\n");
    return -1;
}

// Voltage conversion for X19 (version 0x71)
uint16_t voltage_to_n_v71(uint32_t voltage_mv) {
    int64_t n = 1190935338LL - ((int64_t)voltage_mv * 78743LL);
    n = n / 1000000LL;

    if (n < 9) n = 9;       // 15.0V max
    if (n > 246) n = 246;   // 12.0V min

    return (uint16_t)n;
}

uint32_t n_to_voltage_v71(uint16_t n) {
    int64_t voltage_mv = 1190935338LL - ((int64_t)n * 1000000LL);
    voltage_mv = voltage_mv / 78743LL;
    return (uint32_t)voltage_mv;
}

// Set PSU Voltage
int psu_set_voltage(uint32_t voltage_mv) {
    uint8_t send_packet[16];
    uint8_t recv_packet[16];
    uint16_t n_value, checksum;
    int i, retry;

    printf("Setting voltage to %u mV...\n", voltage_mv);

    if (psu_version == 0x71) {
        n_value = voltage_to_n_v71(voltage_mv);
    } else {
        fprintf(stderr, "Unsupported PSU version 0x%02X\n", psu_version);
        return -1;
    }

    printf("  N value: 0x%04X\n", n_value);

    // Build packet
    send_packet[0] = PSU_MAGIC_1;
    send_packet[1] = PSU_MAGIC_2;
    send_packet[2] = 6;  // Length (header + 2 data bytes)
    send_packet[3] = CMD_SET_VOLTAGE;
    send_packet[4] = n_value & 0xFF;
    send_packet[5] = (n_value >> 8) & 0xFF;
    checksum = calc_checksum(send_packet, 2, 6);
    send_packet[6] = checksum & 0xFF;
    send_packet[7] = (checksum >> 8) & 0xFF;

    for (retry = 0; retry < 3; retry++) {
        // Send command byte-by-byte
        for (i = 0; i < 8; i++) {
            if (fpga_i2c_write_byte(psu_i2c_reg, send_packet[i], 1) < 0) {
                break;
            }
        }
        if (i < 8) continue;

        usleep(PSU_DELAY_AFTER_SEND_MS * 1000);

        // Read response
        for (i = 0; i < 8; i++) {
            if (fpga_i2c_read_byte(psu_i2c_reg, &recv_packet[i], 1) < 0) {
                break;
            }
        }
        if (i < 8) continue;

        usleep(PSU_DELAY_AFTER_READ_MS * 1000);

        // Validate response
        if (recv_packet[0] == PSU_MAGIC_1 && recv_packet[1] == PSU_MAGIC_2 &&
            recv_packet[3] == CMD_SET_VOLTAGE) {
            printf("  Voltage command sent successfully\n\n");
            return 0;
        }
    }

    fprintf(stderr, "Failed to set voltage\n");
    return -1;
}

// Initialize FPGA
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

// Enable hashboard chain in FPGA (CRITICAL - must be called before PSU communication!)
// From stock firmware analysis: FUN_00022ba4
// This function sets a bit in register 0x0D corresponding to the chain number
int fpga_enable_chain(int chain) {
    printf("Enabling hashboard chain %d in FPGA register 0x0D...\n", chain);

    // Read current value
    uint32_t val = regs[FPGA_REG_CHAIN_ENABLE];
    printf("  Current value: 0x%08x\n", val);

    // Set the bit for this chain (bit 0 = chain 0, bit 1 = chain 1, etc)
    val |= (1U << chain);
    printf("  New value: 0x%08x (enabled bit %d)\n", val, chain);

    // Write back
    regs[FPGA_REG_CHAIN_ENABLE] = val;
    __sync_synchronize();  // Memory barrier

    // Verify
    uint32_t verify = regs[FPGA_REG_CHAIN_ENABLE];
    if (verify != val) {
        fprintf(stderr, "Error: Chain enable verification failed (wrote 0x%08x, read 0x%08x)\n",
                val, verify);
        return -1;
    }

    printf("  Chain %d enabled successfully\n\n", chain);
    return 0;
}

int main(int argc, char *argv[]) {
    uint32_t target_voltage_mv = 15000;  // Default 15V

    printf("========================================\n");
    printf("X19 APW12 PSU Test (FPGA Direct)\n");
    printf("========================================\n\n");

    // Parse voltage argument
    if (argc > 1) {
        target_voltage_mv = atoi(argv[1]);
        if (target_voltage_mv < 12000 || target_voltage_mv > 15000) {
            fprintf(stderr, "Error: Voltage must be 12000-15000 mV\n");
            return 1;
        }
    }

    printf("Target voltage: %u mV (%.2f V)\n\n", target_voltage_mv, target_voltage_mv / 1000.0);

    // Setup signals
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Check root
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Must run as root\n");
        return 1;
    }

    // Initialize FPGA
    if (fpga_init() < 0) {
        return 1;
    }

    // CRITICAL: Enable hashboard chain 0 in FPGA register 0x0D
    // This enables the I2C interface to the PSU - without this, PSU won't respond!
    if (fpga_enable_chain(0) < 0) {
        fprintf(stderr, "Failed to enable chain in FPGA\n");
        fpga_close();
        return 1;
    }

    printf("========================================\n");
    printf("PSU Power Release & Initialization\n");
    printf("========================================\n\n");

    // Step 1: GPIO setup and disable PSU (HIGH = disabled, active LOW)
    printf("Step 1: Disable PSU via GPIO %d (set HIGH)\n", PSU_ENABLE_GPIO);
    if (gpio_export(PSU_ENABLE_GPIO) < 0) {
        fprintf(stderr, "Failed to export GPIO\n");
        fpga_close();
        return 1;
    }
    if (gpio_set_direction(PSU_ENABLE_GPIO, "out") < 0) {  // Set as output
        fprintf(stderr, "Failed to set GPIO direction\n");
        fpga_close();
        return 1;
    }
    if (gpio_set_value(PSU_ENABLE_GPIO, 1) < 0) {  // 1 = HIGH = disabled
        fprintf(stderr, "Failed to disable PSU\n");
        fpga_close();
        return 1;
    }
    printf("  PSU disabled (GPIO HIGH)\n\n");

    // Step 2: 30-second power release (CRITICAL - matches stock firmware!)
    printf("Step 2: Waiting 30 seconds for PSU power release...\n");
    printf("  (This allows internal capacitors to discharge)\n");
    for (int i = 30; i > 0; i--) {
        printf("  %d seconds remaining...\r", i);
        fflush(stdout);
        sleep(1);
        if (g_shutdown) {
            fpga_close();
            return 1;
        }
    }
    printf("  Power release complete!                    \n\n");

    if (g_shutdown) {
        fpga_close();
        return 1;
    }

    printf("========================================\n");
    printf("PSU Initialization\n");
    printf("========================================\n\n");

    // Detect protocol
    if (psu_detect_protocol() < 0) {
        fpga_close();
        return 1;
    }

    // Get version
    if (psu_get_version() < 0) {
        fprintf(stderr, "Warning: Could not read version\n\n");
    }

    // Step 3: Set voltage WHILE PSU IS DISABLED
    printf("Step 3: Set voltage to %u mV via I2C (PSU still disabled)\n", target_voltage_mv);
    if (psu_set_voltage(target_voltage_mv) < 0) {
        fpga_close();
        return 1;
    }
    printf("  Voltage configured successfully\n\n");

    // Step 4: Enable PSU output (LOW = enabled, active LOW)
    printf("Step 4: Enable PSU DC output (set GPIO LOW)\n");
    if (gpio_set_value(PSU_ENABLE_GPIO, 0) < 0) {  // 0 = LOW = enabled
        fprintf(stderr, "Failed to enable PSU\n");
        fpga_close();
        return 1;
    }
    printf("  PSU enabled (GPIO LOW)\n\n");

    // Step 5: Wait for voltage stabilization
    printf("Step 5: Waiting for voltage to stabilize...\n");
    sleep(2);
    printf("  Voltage should now be present on PSU DC output!\n\n");

    printf("========================================\n");
    printf("PSU Initialization Complete!\n");
    printf("========================================\n\n");

    printf("Configuration:\n");
    printf("  - FPGA chain: 0 (enabled via register 0x0D)\n");
    printf("  - PSU I2C register: 0x%02X\n", psu_i2c_reg);
    printf("  - PSU version: 0x%02X\n", psu_version);
    printf("  - Configured voltage: %u mV (%.2f V)\n", target_voltage_mv, target_voltage_mv / 1000.0);
    printf("  - GPIO %d: LOW (PSU enabled)\n\n", PSU_ENABLE_GPIO);

    printf("MEASURE DC OUTPUT WITH MULTIMETER NOW!\n");
    printf("Expected: ~%.2f V DC\n\n", target_voltage_mv / 1000.0);

    printf("Press Ctrl+C to exit...\n");
    while (!g_shutdown) {
        sleep(1);
    }

    fpga_close();
    return 0;
}
