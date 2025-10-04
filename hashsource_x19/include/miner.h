#ifndef MINER_H
#define MINER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

// Hardware configuration
#define MAX_HASH_CHAINS     3
#define MAX_CHIPS_PER_CHAIN 114
#define MAX_CORES_PER_CHIP  100
#define NONCE_BITS          32
#define SHA256_DIGEST_SIZE  32

// Memory mapped addresses
#define FPGA_MEM_BASE       0x40000000
#define FPGA_MEM_SIZE       0x10000
#define AXI_CTRL_BASE       0x43C00000
#define AXI_CTRL_SIZE       0x10000

// GPIO definitions
#define GPIO_RED_LED        941
#define GPIO_GREEN_LED      942
#define GPIO_RESET_CHAIN    960

// Work structure
typedef struct {
    uint8_t  midstate[32];
    uint8_t  data[12];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint32_t job_id;
    uint32_t difficulty;
    time_t   timestamp;
} work_t;

// Chain status
typedef struct {
    int      chain_id;
    int      chip_count;
    float    frequency_mhz;
    float    voltage_mv;
    float    temperature_c;
    uint64_t accepted_shares;
    uint64_t rejected_shares;
    uint64_t hw_errors;
    uint64_t hashrate;
    bool     enabled;
    pthread_mutex_t lock;
} chain_status_t;

// Chip status
typedef struct {
    int      chip_id;
    int      core_count;
    float    frequency_mhz;
    float    voltage_mv;
    float    temperature_c;
    uint32_t nonce_errors;
    uint32_t last_nonce;
    bool     active;
} chip_status_t;

// Mining statistics
typedef struct {
    uint64_t total_hashes;
    uint64_t total_shares;
    uint64_t accepted_shares;
    uint64_t rejected_shares;
    uint64_t hw_errors;
    double   average_hashrate;
    time_t   start_time;
    time_t   last_share_time;
    pthread_rwlock_t lock;
} mining_stats_t;

// Pool configuration
typedef struct {
    char     url[256];
    char     user[128];
    char     pass[128];
    int      port;
    bool     enabled;
    int      priority;
} pool_config_t;

// Main miner configuration
typedef struct {
    pool_config_t   pools[3];
    int             active_pool;
    float           target_frequency;
    float           target_voltage;
    int             fan_speed;
    bool            auto_tune;
    char            log_file[256];
    int             log_level;
} miner_config_t;

// Driver interface
typedef struct {
    int  (*init)(void);
    void (*shutdown)(void);
    int  (*detect_chains)(void);
    int  (*set_frequency)(int chain, float freq_mhz);
    int  (*set_voltage)(int chain, float voltage_mv);
    int  (*send_work)(int chain, work_t *work);
    int  (*get_nonce)(int chain, uint32_t *nonce, uint32_t *job_id);
    void (*reset_chain)(int chain);
    float (*get_temperature)(int chain);
} driver_ops_t;

// Global state
typedef struct {
    miner_config_t  config;
    chain_status_t  chains[MAX_HASH_CHAINS];
    mining_stats_t  stats;
    driver_ops_t    *driver;
    bool            running;
    pthread_t       work_thread;
    pthread_t       result_thread;
    pthread_t       monitor_thread;
    sem_t           work_ready;
    void            *fpga_mem;
    void            *axi_ctrl;
} miner_state_t;

// Function prototypes
int  miner_init(const char *config_file);
void miner_shutdown(void);
int  miner_start(void);
void miner_stop(void);
void miner_get_stats(mining_stats_t *stats);
int  miner_add_pool(const pool_config_t *pool);
int  miner_switch_pool(int pool_id);

// Hardware functions
int  hw_init(void);
void hw_shutdown(void);
int  hw_detect_asics(void);
int  hw_set_frequency(int chain, float freq_mhz);
int  hw_set_voltage(int chain, float voltage_mv);
int  hw_send_work(int chain, work_t *work);
int  hw_get_results(int chain, uint32_t *nonces, int max_nonces);
int  hw_set_fan_pwm(int pwm_percent);
int  hw_get_fan_speed(void);
float hw_get_temperature(int chain);

// Utility functions
uint64_t get_timestamp_ms(void);
void hexdump(const char *prefix, const void *data, size_t len);
int  parse_config(const char *filename, miner_config_t *config);
void log_message(int level, const char *format, ...);

#endif // MINER_H