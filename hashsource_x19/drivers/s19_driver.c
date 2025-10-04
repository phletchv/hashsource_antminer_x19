#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "../include/miner.h"

// Device file paths
#define DEV_AXI_FPGA    "/dev/axi_fpga_dev"
#define DEV_FPGA_MEM    "/dev/fpga_mem"

// Hardware registers (based on S19 Pro analysis)
#define REG_VERSION     0x0000
#define REG_CONTROL     0x0004
#define REG_STATUS      0x0008
#define REG_CHAIN_EN    0x000C
#define REG_FREQUENCY   0x0010
#define REG_VOLTAGE     0x0014
#define REG_WORK_ID     0x0020
#define REG_MIDSTATE    0x0040
#define REG_DATA        0x0060
#define REG_TARGET      0x0080
#define REG_NONCE_OUT   0x00A0
#define REG_TEMP_SENSOR 0x00B0
#define REG_FAN_CTRL    0x00C0
#define REG_FAN_PWM     0x00C4
#define REG_FAN_SPEED   0x00C8
#define REG_ERROR_CNT   0x00D0

// Control bits
#define CTRL_RESET      (1 << 0)
#define CTRL_START      (1 << 1)
#define CTRL_STOP       (1 << 2)
#define CTRL_AUTO_TUNE  (1 << 3)

// Status bits
#define STATUS_READY    (1 << 0)
#define STATUS_BUSY     (1 << 1)
#define STATUS_ERROR    (1 << 2)
#define STATUS_NONCE    (1 << 3)

// File descriptors
static int fd_axi = -1;
static int fd_mem = -1;

// Memory mapped regions
static volatile uint32_t *fpga_regs = NULL;
static volatile uint32_t *fpga_mem = NULL;

// Private functions
static uint32_t read_reg(uint32_t offset) {
    if (!fpga_regs) return 0;
    return fpga_regs[offset / 4];
}

static void write_reg(uint32_t offset, uint32_t value) {
    if (!fpga_regs) return;
    fpga_regs[offset / 4] = value;
}

static void delay_ms(int ms) {
    usleep(ms * 1000);
}

// Initialize hardware
int hw_init(void) {
    log_message(1, "Initializing S19 hardware driver");

    // Open AXI device
    fd_axi = open(DEV_AXI_FPGA, O_RDWR);
    if (fd_axi < 0) {
        log_message(3, "Failed to open %s: %s", DEV_AXI_FPGA, strerror(errno));
        return -1;
    }

    // Open FPGA memory device
    fd_mem = open(DEV_FPGA_MEM, O_RDWR | O_SYNC);
    if (fd_mem < 0) {
        log_message(3, "Failed to open %s: %s", DEV_FPGA_MEM, strerror(errno));
        close(fd_axi);
        return -1;
    }

    // Map FPGA control registers
    fpga_regs = (volatile uint32_t *)mmap(NULL, AXI_CTRL_SIZE,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd_axi, AXI_CTRL_BASE);
    if (fpga_regs == MAP_FAILED) {
        log_message(3, "Failed to map FPGA registers: %s", strerror(errno));
        close(fd_axi);
        close(fd_mem);
        return -1;
    }

    // Map FPGA memory
    fpga_mem = (volatile uint32_t *)mmap(NULL, FPGA_MEM_SIZE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd_mem, 0);
    if (fpga_mem == MAP_FAILED) {
        log_message(3, "Failed to map FPGA memory: %s", strerror(errno));
        munmap((void *)fpga_regs, AXI_CTRL_SIZE);
        close(fd_axi);
        close(fd_mem);
        return -1;
    }

    // Reset hardware
    write_reg(REG_CONTROL, CTRL_RESET);
    delay_ms(100);
    write_reg(REG_CONTROL, 0);
    delay_ms(100);

    // Check version
    uint32_t version = read_reg(REG_VERSION);
    log_message(1, "FPGA version: 0x%08x", version);

    // Enable all chains by default
    write_reg(REG_CHAIN_EN, 0x07);  // Enable 3 chains

    log_message(1, "Hardware initialization complete");
    return 0;
}

// Shutdown hardware
void hw_shutdown(void) {
    log_message(1, "Shutting down hardware");

    // Stop mining
    write_reg(REG_CONTROL, CTRL_STOP);
    delay_ms(100);

    // Disable all chains
    write_reg(REG_CHAIN_EN, 0x00);

    // Unmap memory
    if (fpga_regs) {
        munmap((void *)fpga_regs, AXI_CTRL_SIZE);
        fpga_regs = NULL;
    }

    if (fpga_mem) {
        munmap((void *)fpga_mem, FPGA_MEM_SIZE);
        fpga_mem = NULL;
    }

    // Close devices
    if (fd_axi >= 0) {
        close(fd_axi);
        fd_axi = -1;
    }

    if (fd_mem >= 0) {
        close(fd_mem);
        fd_mem = -1;
    }

    log_message(1, "Hardware shutdown complete");
}

// Detect ASICs
int hw_detect_asics(void) {
    log_message(1, "Detecting ASIC chains");

    int chain_count = 0;
    uint32_t chain_status = read_reg(REG_CHAIN_EN);

    for (int i = 0; i < MAX_HASH_CHAINS; i++) {
        if (chain_status & (1 << i)) {
            log_message(1, "Chain %d detected", i);
            chain_count++;

            // TODO: Detect number of chips in chain
            // This would involve sending test patterns and checking responses
        }
    }

    log_message(1, "Detected %d chains", chain_count);
    return chain_count;
}

// Set frequency
int hw_set_frequency(int chain, float freq_mhz) {
    if (chain < 0 || chain >= MAX_HASH_CHAINS) {
        return -1;
    }

    log_message(1, "Setting chain %d frequency to %.1f MHz", chain, freq_mhz);

    // Convert frequency to register value
    // Note: This is simplified, actual implementation would need proper PLL configuration
    uint32_t freq_reg = (uint32_t)(freq_mhz * 10);

    // Write frequency register for specific chain
    write_reg(REG_FREQUENCY + (chain * 4), freq_reg);
    delay_ms(10);

    return 0;
}

// Set voltage
int hw_set_voltage(int chain, float voltage_mv) {
    if (chain < 0 || chain >= MAX_HASH_CHAINS) {
        return -1;
    }

    log_message(1, "Setting chain %d voltage to %.1f mV", chain, voltage_mv);

    // Convert voltage to register value
    // Note: This is simplified, actual implementation would need proper voltage regulator control
    uint32_t volt_reg = (uint32_t)(voltage_mv);

    // Write voltage register for specific chain
    write_reg(REG_VOLTAGE + (chain * 4), volt_reg);
    delay_ms(100);  // Allow voltage to stabilize

    return 0;
}

// Send work to chain
int hw_send_work(int chain, work_t *work) {
    if (chain < 0 || chain >= MAX_HASH_CHAINS || !work) {
        return -1;
    }

    // Wait for hardware to be ready
    int timeout = 100;
    while (timeout > 0) {
        uint32_t status = read_reg(REG_STATUS);
        if (status & STATUS_READY) {
            break;
        }
        delay_ms(1);
        timeout--;
    }

    if (timeout == 0) {
        log_message(2, "Hardware timeout waiting for ready state");
        return -1;
    }

    // Write work ID
    write_reg(REG_WORK_ID, work->job_id);

    // Write midstate (32 bytes)
    for (int i = 0; i < 8; i++) {
        uint32_t *midstate32 = (uint32_t *)work->midstate;
        write_reg(REG_MIDSTATE + (i * 4), midstate32[i]);
    }

    // Write data (12 bytes)
    for (int i = 0; i < 3; i++) {
        uint32_t *data32 = (uint32_t *)work->data;
        write_reg(REG_DATA + (i * 4), data32[i]);
    }

    // Write target (32 bytes)
    for (int i = 0; i < 8; i++) {
        uint32_t *target32 = (uint32_t *)work->target;
        write_reg(REG_TARGET + (i * 4), target32[i]);
    }

    // Start work on chain
    write_reg(REG_CONTROL, CTRL_START | (chain << 8));

    return 0;
}

// Get results from chain
int hw_get_results(int chain, uint32_t *nonces, int max_nonces) {
    if (chain < 0 || chain >= MAX_HASH_CHAINS || !nonces) {
        return -1;
    }

    int nonce_count = 0;

    // Check for available nonces
    uint32_t status = read_reg(REG_STATUS);

    while ((status & STATUS_NONCE) && nonce_count < max_nonces) {
        // Read nonce
        uint32_t nonce = read_reg(REG_NONCE_OUT);
        nonces[nonce_count++] = nonce;

        // Clear nonce flag
        write_reg(REG_STATUS, STATUS_NONCE);

        // Check for more nonces
        status = read_reg(REG_STATUS);
    }

    return nonce_count;
}

// Get temperature
float hw_get_temperature(int chain) {
    if (chain < 0 || chain >= MAX_HASH_CHAINS) {
        return -1.0;
    }

    // Read temperature sensor
    uint32_t temp_raw = read_reg(REG_TEMP_SENSOR + (chain * 4));

    // Convert to Celsius (simplified conversion)
    float temperature = (float)(temp_raw & 0xFFFF) / 100.0;

    return temperature;
}

// Set fan PWM (0-100%)
int hw_set_fan_pwm(int pwm_percent) {
    if (pwm_percent < 0) pwm_percent = 0;
    if (pwm_percent > 100) pwm_percent = 100;

    log_message(1, "Setting fan PWM to %d%%", pwm_percent);

    // Convert percentage to register value (0-255)
    uint32_t pwm_value = (pwm_percent * 255) / 100;

    // Write PWM value
    write_reg(REG_FAN_PWM, pwm_value);

    // Enable fan control
    write_reg(REG_FAN_CTRL, 0x01);

    return 0;
}

// Get fan speed (RPM)
int hw_get_fan_speed(void) {
    uint32_t speed_raw = read_reg(REG_FAN_SPEED);

    // Convert raw value to RPM (assuming tachometer pulses)
    // This is a simplified conversion - actual formula depends on fan specs
    int rpm = (speed_raw & 0xFFFF) * 30;  // 2 pulses per revolution, measured over 1 second

    return rpm;
}

// Driver operations structure
static driver_ops_t s19_driver = {
    .init = hw_init,
    .shutdown = hw_shutdown,
    .detect_chains = hw_detect_asics,
    .set_frequency = hw_set_frequency,
    .set_voltage = hw_set_voltage,
    .send_work = hw_send_work,
    .get_nonce = NULL,  // We use hw_get_results instead
    .reset_chain = NULL,  // TODO: Implement
    .get_temperature = hw_get_temperature
};

// Get driver instance
driver_ops_t *get_s19_driver(void) {
    return &s19_driver;
}
