#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>
#include "../include/miner.h"

// Global miner state
static miner_state_t g_miner;
static volatile bool g_shutdown = false;

// External driver
extern driver_ops_t *get_s19_driver(void);

// Signal handler
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        log_message(1, "Received shutdown signal");
        g_shutdown = true;
        g_miner.running = false;
    }
}

// Work generation thread
static void *work_thread(void *arg) {
    log_message(1, "Work thread started");

    work_t work = {0};
    uint32_t job_id = 0;

    while (g_miner.running) {
        // Generate test work (in production, this would come from a pool)
        // For now, we'll use static test data

        // Example Bitcoin block header data
        memset(&work, 0, sizeof(work_t));

        // Test midstate
        uint8_t test_midstate[32] = {
            0x6a, 0x09, 0xe6, 0x67, 0xf3, 0xbc, 0xc9, 0x08,
            0x44, 0x8a, 0x42, 0xdc, 0x20, 0xbb, 0xe1, 0x1e,
            0x7f, 0x43, 0xac, 0xca, 0x9b, 0xd1, 0xde, 0x44,
            0x67, 0x9e, 0x1c, 0x36, 0x7e, 0xaf, 0xfa, 0x37
        };
        memcpy(work.midstate, test_midstate, 32);

        // Test data (last 12 bytes of block header)
        uint8_t test_data[12] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x80
        };
        memcpy(work.data, test_data, 12);

        // Test target (difficulty 1)
        memset(work.target, 0xFF, 32);
        work.target[29] = 0xFF;
        work.target[30] = 0x00;
        work.target[31] = 0x00;

        work.job_id = job_id++;
        work.timestamp = time(NULL);
        work.nonce_start = 0;
        work.nonce_end = 0xFFFFFFFF;
        work.difficulty = 1;

        // Send work to all enabled chains
        for (int chain = 0; chain < MAX_HASH_CHAINS; chain++) {
            if (g_miner.chains[chain].enabled) {
                int ret = hw_send_work(chain, &work);
                if (ret == 0) {
                    log_message(0, "Sent work %u to chain %d", work.job_id, chain);
                }
            }
        }

        // Wait before generating next work
        sleep(1);
    }

    log_message(1, "Work thread stopped");
    return NULL;
}

// Result collection thread
static void *result_thread(void *arg) {
    log_message(1, "Result thread started");

    uint32_t nonces[16];

    while (g_miner.running) {
        // Check all chains for results
        for (int chain = 0; chain < MAX_HASH_CHAINS; chain++) {
            if (!g_miner.chains[chain].enabled) {
                continue;
            }

            int nonce_count = hw_get_results(chain, nonces, 16);

            for (int i = 0; i < nonce_count; i++) {
                log_message(0, "Chain %d found nonce: 0x%08x", chain, nonces[i]);

                // Update statistics
                pthread_rwlock_wrlock(&g_miner.stats.lock);
                g_miner.stats.total_shares++;
                g_miner.stats.last_share_time = time(NULL);
                pthread_rwlock_unlock(&g_miner.stats.lock);

                pthread_mutex_lock(&g_miner.chains[chain].lock);
                g_miner.chains[chain].accepted_shares++;
                pthread_mutex_unlock(&g_miner.chains[chain].lock);
            }
        }

        usleep(10000);  // 10ms
    }

    log_message(1, "Result thread stopped");
    return NULL;
}

// Monitoring thread
static void *monitor_thread(void *arg) {
    log_message(1, "Monitor thread started");

    time_t last_stats_time = time(NULL);

    while (g_miner.running) {
        time_t now = time(NULL);

        // Update temperatures
        for (int chain = 0; chain < MAX_HASH_CHAINS; chain++) {
            if (g_miner.chains[chain].enabled) {
                float temp = hw_get_temperature(chain);
                pthread_mutex_lock(&g_miner.chains[chain].lock);
                g_miner.chains[chain].temperature_c = temp;
                pthread_mutex_unlock(&g_miner.chains[chain].lock);

                // Check for overheating
                if (temp > 85.0) {
                    log_message(2, "Chain %d overheating: %.1f°C", chain, temp);
                    // TODO: Implement thermal throttling
                }
            }
        }

        // Print statistics every 60 seconds
        if (now - last_stats_time >= 60) {
            pthread_rwlock_rdlock(&g_miner.stats.lock);
            uint64_t total_shares = g_miner.stats.total_shares;
            uint64_t accepted = g_miner.stats.accepted_shares;
            pthread_rwlock_unlock(&g_miner.stats.lock);

            log_message(1, "Stats: Shares: %llu, Accepted: %llu",
                       (unsigned long long)total_shares,
                       (unsigned long long)accepted);

            for (int chain = 0; chain < MAX_HASH_CHAINS; chain++) {
                if (g_miner.chains[chain].enabled) {
                    pthread_mutex_lock(&g_miner.chains[chain].lock);
                    log_message(1, "Chain %d: Temp: %.1f°C, Shares: %llu, Errors: %llu",
                               chain,
                               g_miner.chains[chain].temperature_c,
                               (unsigned long long)g_miner.chains[chain].accepted_shares,
                               (unsigned long long)g_miner.chains[chain].hw_errors);
                    pthread_mutex_unlock(&g_miner.chains[chain].lock);
                }
            }

            last_stats_time = now;
        }

        sleep(5);
    }

    log_message(1, "Monitor thread stopped");
    return NULL;
}

// Initialize miner
int miner_init(const char *config_file) {
    memset(&g_miner, 0, sizeof(miner_state_t));

    // Initialize locks
    pthread_rwlock_init(&g_miner.stats.lock, NULL);
    for (int i = 0; i < MAX_HASH_CHAINS; i++) {
        pthread_mutex_init(&g_miner.chains[i].lock, NULL);
    }
    sem_init(&g_miner.work_ready, 0, 0);

    // Set default configuration
    g_miner.config.target_frequency = 500.0;  // 500 MHz
    g_miner.config.target_voltage = 1280.0;   // 1280 mV
    g_miner.config.fan_speed = 100;           // 100%
    g_miner.config.log_level = 1;
    strcpy(g_miner.config.log_file, "/var/log/miner.log");

    // Load configuration if provided
    if (config_file) {
        if (parse_config(config_file, &g_miner.config) < 0) {
            log_message(2, "Failed to load config file: %s", config_file);
        }
    }

    // Get driver
    g_miner.driver = get_s19_driver();
    if (!g_miner.driver) {
        log_message(3, "Failed to get driver");
        return -1;
    }

    // Initialize hardware
    if (g_miner.driver->init() < 0) {
        log_message(3, "Failed to initialize hardware");
        return -1;
    }

    // Detect chains
    int chain_count = g_miner.driver->detect_chains();
    if (chain_count <= 0) {
        log_message(3, "No hash chains detected");
        g_miner.driver->shutdown();
        return -1;
    }

    // Configure chains
    for (int i = 0; i < chain_count && i < MAX_HASH_CHAINS; i++) {
        g_miner.chains[i].chain_id = i;
        g_miner.chains[i].enabled = true;
        g_miner.chains[i].frequency_mhz = g_miner.config.target_frequency;
        g_miner.chains[i].voltage_mv = g_miner.config.target_voltage;

        // Set initial frequency and voltage
        g_miner.driver->set_frequency(i, g_miner.config.target_frequency);
        g_miner.driver->set_voltage(i, g_miner.config.target_voltage);
    }

    g_miner.stats.start_time = time(NULL);

    log_message(1, "Miner initialized with %d chains", chain_count);
    return 0;
}

// Start mining
int miner_start(void) {
    if (g_miner.running) {
        return 0;  // Already running
    }

    g_miner.running = true;

    // Start threads
    if (pthread_create(&g_miner.work_thread, NULL, work_thread, NULL) != 0) {
        log_message(3, "Failed to create work thread");
        g_miner.running = false;
        return -1;
    }

    if (pthread_create(&g_miner.result_thread, NULL, result_thread, NULL) != 0) {
        log_message(3, "Failed to create result thread");
        g_miner.running = false;
        pthread_join(g_miner.work_thread, NULL);
        return -1;
    }

    if (pthread_create(&g_miner.monitor_thread, NULL, monitor_thread, NULL) != 0) {
        log_message(3, "Failed to create monitor thread");
        g_miner.running = false;
        pthread_join(g_miner.work_thread, NULL);
        pthread_join(g_miner.result_thread, NULL);
        return -1;
    }

    log_message(1, "Mining started");
    return 0;
}

// Stop mining
void miner_stop(void) {
    if (!g_miner.running) {
        return;
    }

    log_message(1, "Stopping miner");
    g_miner.running = false;

    // Signal threads to stop
    sem_post(&g_miner.work_ready);

    // Wait for threads to finish
    pthread_join(g_miner.work_thread, NULL);
    pthread_join(g_miner.result_thread, NULL);
    pthread_join(g_miner.monitor_thread, NULL);

    log_message(1, "Miner stopped");
}

// Shutdown miner
void miner_shutdown(void) {
    miner_stop();

    if (g_miner.driver) {
        g_miner.driver->shutdown();
    }

    // Destroy locks
    pthread_rwlock_destroy(&g_miner.stats.lock);
    for (int i = 0; i < MAX_HASH_CHAINS; i++) {
        pthread_mutex_destroy(&g_miner.chains[i].lock);
    }
    sem_destroy(&g_miner.work_ready);

    log_message(1, "Miner shutdown complete");
}

// Fan test function
static int fan_test_mode(void) {
    log_message(1, "Starting fan PWM test mode");
    log_message(1, "Fan will ramp from 0%% to 100%% and back");

    // Initialize hardware only
    if (hw_init() < 0) {
        log_message(3, "Failed to initialize hardware");
        return -1;
    }

    int cycles = 3;  // Number of ramp cycles
    int step_delay_ms = 100;  // Delay between PWM steps

    for (int cycle = 0; cycle < cycles && !g_shutdown; cycle++) {
        log_message(1, "Fan test cycle %d/%d", cycle + 1, cycles);

        // Ramp up from 0% to 100%
        log_message(1, "Ramping up...");
        for (int pwm = 0; pwm <= 100 && !g_shutdown; pwm += 2) {
            hw_set_fan_pwm(pwm);

            // Get and display fan speed every 10%
            if (pwm % 10 == 0) {
                usleep(500000);  // Wait for fan to stabilize
                int rpm = hw_get_fan_speed();
                log_message(1, "PWM: %d%%, Fan Speed: %d RPM", pwm, rpm);
            }

            usleep(step_delay_ms * 1000);
        }

        // Hold at 100% for 2 seconds
        log_message(1, "Holding at 100%%...");
        sleep(2);

        // Ramp down from 100% to 0%
        log_message(1, "Ramping down...");
        for (int pwm = 100; pwm >= 0 && !g_shutdown; pwm -= 2) {
            hw_set_fan_pwm(pwm);

            // Get and display fan speed every 10%
            if (pwm % 10 == 0) {
                usleep(500000);  // Wait for fan to stabilize
                int rpm = hw_get_fan_speed();
                log_message(1, "PWM: %d%%, Fan Speed: %d RPM", pwm, rpm);
            }

            usleep(step_delay_ms * 1000);
        }

        // Hold at 0% for 2 seconds
        log_message(1, "Holding at 0%%...");
        sleep(2);
    }

    // Set fan to safe default (50%) before exiting
    log_message(1, "Test complete. Setting fan to 50%%");
    hw_set_fan_pwm(50);

    hw_shutdown();
    return 0;
}

// Main function
int main(int argc, char *argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Open syslog
    openlog("hashsource_miner", LOG_PID | LOG_CONS, LOG_DAEMON);

    log_message(1, "HashSource X19 Miner starting");

    // Check for test mode
    if (argc > 1 && strcmp(argv[1], "--fan-test") == 0) {
        log_message(1, "Running in fan test mode");
        int ret = fan_test_mode();
        closelog();
        return ret;
    }

    // Initialize miner
    const char *config_file = argc > 1 ? argv[1] : NULL;
    if (miner_init(config_file) < 0) {
        log_message(3, "Failed to initialize miner");
        closelog();
        return 1;
    }

    // Start mining
    if (miner_start() < 0) {
        log_message(3, "Failed to start mining");
        miner_shutdown();
        closelog();
        return 1;
    }

    // Main loop
    while (!g_shutdown) {
        sleep(1);
    }

    // Cleanup
    miner_shutdown();
    closelog();

    log_message(1, "HashSource X19 Miner exited");
    return 0;
}
