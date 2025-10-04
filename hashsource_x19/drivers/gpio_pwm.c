#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "../include/miner.h"

// GPIO pins for fan control (from S19 Pro analysis)
#define GPIO_FAN1_PWM   943  // Fan 1 PWM control
#define GPIO_FAN2_PWM   944  // Fan 2 PWM control
#define GPIO_FAN3_PWM   945  // Fan 3 PWM control
#define GPIO_FAN4_PWM   946  // Fan 4 PWM control

// PWM parameters
#define PWM_FREQUENCY   25000  // 25kHz PWM frequency
#define PWM_PERIOD_US   40     // 1/25000 = 40 microseconds

static pthread_t pwm_thread;
static volatile int pwm_duty_cycle = 50;  // Default 50%
static volatile bool pwm_running = false;

// Export GPIO pin
static int gpio_export(int pin) {
    char buffer[64];
    int fd;

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%d", pin);
    write(fd, buffer, strlen(buffer));
    close(fd);

    return 0;
}

// Set GPIO direction
static int gpio_set_direction(int pin, const char *direction) {
    char path[64];
    char buffer[8];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    write(fd, direction, strlen(direction));
    close(fd);

    return 0;
}

// Set GPIO value
static int gpio_set_value(int pin, int value) {
    char path[64];
    char buffer[2];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    buffer[0] = value ? '1' : '0';
    buffer[1] = '\0';
    write(fd, buffer, 1);
    close(fd);

    return 0;
}

// PWM thread function
static void *pwm_worker(void *arg) {
    (void)arg;

    int high_time_us;
    int low_time_us;

    log_message(0, "GPIO PWM thread started");

    while (pwm_running) {
        // Calculate high and low times based on duty cycle
        high_time_us = (PWM_PERIOD_US * pwm_duty_cycle) / 100;
        low_time_us = PWM_PERIOD_US - high_time_us;

        if (high_time_us > 0) {
            // Set all fan GPIOs high
            gpio_set_value(GPIO_FAN1_PWM, 1);
            gpio_set_value(GPIO_FAN2_PWM, 1);
            gpio_set_value(GPIO_FAN3_PWM, 1);
            gpio_set_value(GPIO_FAN4_PWM, 1);
            usleep(high_time_us);
        }

        if (low_time_us > 0) {
            // Set all fan GPIOs low
            gpio_set_value(GPIO_FAN1_PWM, 0);
            gpio_set_value(GPIO_FAN2_PWM, 0);
            gpio_set_value(GPIO_FAN3_PWM, 0);
            gpio_set_value(GPIO_FAN4_PWM, 0);
            usleep(low_time_us);
        }
    }

    // Set fans to safe default before exiting
    gpio_set_value(GPIO_FAN1_PWM, 1);
    gpio_set_value(GPIO_FAN2_PWM, 1);
    gpio_set_value(GPIO_FAN3_PWM, 1);
    gpio_set_value(GPIO_FAN4_PWM, 1);

    log_message(0, "GPIO PWM thread stopped");
    return NULL;
}

// Initialize GPIO PWM
int gpio_pwm_init(void) {
    log_message(1, "Initializing GPIO PWM for fan control");

    // Export and configure fan GPIO pins
    int fan_gpios[] = {GPIO_FAN1_PWM, GPIO_FAN2_PWM, GPIO_FAN3_PWM, GPIO_FAN4_PWM};

    for (int i = 0; i < 4; i++) {
        if (gpio_export(fan_gpios[i]) < 0) {
            // Pin might already be exported, continue
        }

        if (gpio_set_direction(fan_gpios[i], "out") < 0) {
            log_message(2, "Failed to set GPIO %d direction", fan_gpios[i]);
            return -1;
        }

        // Start with fans at 50%
        gpio_set_value(fan_gpios[i], 1);
    }

    // Start PWM thread
    pwm_running = true;
    if (pthread_create(&pwm_thread, NULL, pwm_worker, NULL) != 0) {
        log_message(3, "Failed to create PWM thread");
        pwm_running = false;
        return -1;
    }

    log_message(1, "GPIO PWM initialized");
    return 0;
}

// Shutdown GPIO PWM
void gpio_pwm_shutdown(void) {
    log_message(1, "Shutting down GPIO PWM");

    // Stop PWM thread
    pwm_running = false;
    pthread_join(pwm_thread, NULL);

    // Set fans to safe default (100%)
    gpio_set_value(GPIO_FAN1_PWM, 1);
    gpio_set_value(GPIO_FAN2_PWM, 1);
    gpio_set_value(GPIO_FAN3_PWM, 1);
    gpio_set_value(GPIO_FAN4_PWM, 1);
}

// Set GPIO PWM duty cycle (0-100%)
int gpio_pwm_set_duty(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    pwm_duty_cycle = percent;
    log_message(0, "GPIO PWM duty cycle set to %d%%", percent);

    return 0;
}
