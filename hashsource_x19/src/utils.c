#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <ctype.h>
#include "../include/miner.h"

// Get timestamp in milliseconds
uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Hexdump function for debugging
void hexdump(const char *prefix, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i, j;

    if (prefix) {
        printf("%s:\n", prefix);
    }

    for (i = 0; i < len; i += 16) {
        printf("%08zx: ", i);

        // Print hex bytes
        for (j = 0; j < 16; j++) {
            if (i + j < len) {
                printf("%02x ", bytes[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) printf(" ");
        }

        printf(" |");

        // Print ASCII representation
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = bytes[i + j];
            printf("%c", isprint(c) ? c : '.');
        }

        printf("|\n");
    }
}

// Parse configuration file
int parse_config(const char *filename, miner_config_t *config) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    char key[128];
    char value[128];

    while (fgets(line, sizeof(line), fp)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        // Parse key=value pairs
        if (sscanf(line, "%127[^=]=%127s", key, value) == 2) {
            // Remove whitespace
            char *p = key + strlen(key) - 1;
            while (p > key && isspace(*p)) *p-- = '\0';
            p = key;
            while (*p && isspace(*p)) p++;

            // Process configuration options
            if (strcmp(p, "pool1.url") == 0) {
                strncpy(config->pools[0].url, value, sizeof(config->pools[0].url) - 1);
            } else if (strcmp(p, "pool1.user") == 0) {
                strncpy(config->pools[0].user, value, sizeof(config->pools[0].user) - 1);
            } else if (strcmp(p, "pool1.pass") == 0) {
                strncpy(config->pools[0].pass, value, sizeof(config->pools[0].pass) - 1);
            } else if (strcmp(p, "pool2.url") == 0) {
                strncpy(config->pools[1].url, value, sizeof(config->pools[1].url) - 1);
            } else if (strcmp(p, "pool2.user") == 0) {
                strncpy(config->pools[1].user, value, sizeof(config->pools[1].user) - 1);
            } else if (strcmp(p, "pool2.pass") == 0) {
                strncpy(config->pools[1].pass, value, sizeof(config->pools[1].pass) - 1);
            } else if (strcmp(p, "pool3.url") == 0) {
                strncpy(config->pools[2].url, value, sizeof(config->pools[2].url) - 1);
            } else if (strcmp(p, "pool3.user") == 0) {
                strncpy(config->pools[2].user, value, sizeof(config->pools[2].user) - 1);
            } else if (strcmp(p, "pool3.pass") == 0) {
                strncpy(config->pools[2].pass, value, sizeof(config->pools[2].pass) - 1);
            } else if (strcmp(p, "frequency") == 0) {
                config->target_frequency = atof(value);
            } else if (strcmp(p, "voltage") == 0) {
                config->target_voltage = atof(value);
            } else if (strcmp(p, "fan_speed") == 0) {
                config->fan_speed = atoi(value);
            } else if (strcmp(p, "auto_tune") == 0) {
                config->auto_tune = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(p, "log_file") == 0) {
                strncpy(config->log_file, value, sizeof(config->log_file) - 1);
            } else if (strcmp(p, "log_level") == 0) {
                config->log_level = atoi(value);
            }
        }
    }

    fclose(fp);
    return 0;
}

// Logging function
void log_message(int level, const char *format, ...) {
    va_list args;
    char buffer[1024];
    time_t now;
    struct tm *tm_info;
    char time_str[32];

    // Format the message
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Get current time
    time(&now);
    tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    // Log levels: 0=DEBUG, 1=INFO, 2=WARNING, 3=ERROR
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    // Print to console
    if (level >= 1) {  // Only print INFO and above to console
        printf("[%s] %s: %s\n", time_str, level_str[level], buffer);
        fflush(stdout);
    }

    // Log to syslog
    int syslog_priority[] = {LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR};
    syslog(syslog_priority[level], "%s", buffer);
}
