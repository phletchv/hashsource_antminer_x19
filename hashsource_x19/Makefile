# HashSource X19 Mining Software Makefile

# Cross-compilation settings for ARM
# Use buildroot toolchain if available, fallback to system arm-linux-gnueabihf-
BUILDROOT_HOST := $(realpath ../buildroot/output/host)
ifneq ($(wildcard $(BUILDROOT_HOST)/bin/arm-buildroot-linux-gnueabihf-gcc),)
	CROSS_COMPILE ?= $(BUILDROOT_HOST)/bin/arm-buildroot-linux-gnueabihf-
else
	CROSS_COMPILE ?= arm-linux-gnueabihf-
endif

CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
STRIP = $(CROSS_COMPILE)strip

# Directories
SRC_DIR = src
INC_DIR = include
DRV_DIR = drivers
OBJ_DIR = obj
BIN_DIR = bin

# Target binaries
TARGET = $(BIN_DIR)/hashsource_miner
FAN_TEST = $(BIN_DIR)/fan_test
FPGA_LOGGER = $(BIN_DIR)/fpga_logger
FPGA_DUMP = $(BIN_DIR)/fpga_dump
PSU_TEST = $(BIN_DIR)/psu_test
ID2MAC = $(BIN_DIR)/id2mac
EEPROM_DETECT = $(BIN_DIR)/eeprom_detect
CHAIN_TEST = $(BIN_DIR)/chain_test
WORK_TEST = $(BIN_DIR)/work_test
PATTERN_TEST = $(BIN_DIR)/pattern_test
FPGA_REG_TEST = $(BIN_DIR)/fpga_reg_test
ASIC_STATUS = $(BIN_DIR)/asic_status_check

# Source files for main miner
SRCS = $(SRC_DIR)/main.c

# Source files for fan test
FAN_SRCS = $(SRC_DIR)/fan_test.c

# Source files for FPGA logger
LOGGER_SRCS = $(SRC_DIR)/fpga_logger.c

# Source files for FPGA dump
DUMP_SRCS = $(SRC_DIR)/fpga_dump.c

# Source files for PSU test
PSU_SRCS = $(SRC_DIR)/psu_test.c

# Source files for id2mac
ID2MAC_SRCS = $(SRC_DIR)/id2mac.c

# Source files for eeprom_detect
EEPROM_DETECT_SRCS = $(SRC_DIR)/eeprom_detect.c

# Source files for chain_test (includes BM1398 driver)
CHAIN_TEST_SRCS = $(SRC_DIR)/chain_test.c $(SRC_DIR)/bm1398_asic.c

# Source files for work_test (includes BM1398 driver)
WORK_TEST_SRCS = $(SRC_DIR)/work_test.c $(SRC_DIR)/bm1398_asic.c

# Source files for pattern_test (includes BM1398 driver)
PATTERN_TEST_SRCS = $(SRC_DIR)/pattern_test.c $(SRC_DIR)/bm1398_asic.c

# Source files for fpga_reg_test
FPGA_REG_TEST_SRCS = $(SRC_DIR)/fpga_reg_test.c

# Source files for asic_status_check (includes BM1398 driver)
ASIC_STATUS_SRCS = $(SRC_DIR)/asic_status_check.c $(SRC_DIR)/bm1398_asic.c

# Object files
OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(SRCS)))
FAN_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(FAN_SRCS)))
LOGGER_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(LOGGER_SRCS)))
DUMP_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(DUMP_SRCS)))
PSU_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(PSU_SRCS)))
ID2MAC_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(ID2MAC_SRCS)))
EEPROM_DETECT_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(EEPROM_DETECT_SRCS)))
CHAIN_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(CHAIN_TEST_SRCS)))
WORK_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(WORK_TEST_SRCS)))
PATTERN_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(PATTERN_TEST_SRCS)))
FPGA_REG_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(FPGA_REG_TEST_SRCS)))
ASIC_STATUS_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(ASIC_STATUS_SRCS)))

# Compiler flags
CFLAGS = -Wall -Wextra -O2 -g
CFLAGS += -I$(INC_DIR)
CFLAGS += -march=armv7-a -mfpu=neon -mfloat-abi=hard
CFLAGS += -D_GNU_SOURCE

# Linker flags
LDFLAGS = -pthread -lm -lrt

# Default target
all: dirs $(TARGET) $(FAN_TEST) $(FPGA_LOGGER) $(FPGA_DUMP) $(PSU_TEST) $(ID2MAC) $(EEPROM_DETECT) $(CHAIN_TEST) $(WORK_TEST) $(PATTERN_TEST) $(FPGA_REG_TEST) $(ASIC_STATUS)

# Create directories
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

# Build main target
$(TARGET): $(OBJS)
	@echo "Linking $@"
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build fan test
$(FAN_TEST): $(FAN_OBJS)
	@echo "Linking $@"
	$(CC) $(FAN_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build FPGA logger (static for portability)
$(FPGA_LOGGER): $(LOGGER_OBJS)
	@echo "Linking $@ (static)"
	$(CC) $(LOGGER_OBJS) -o $@ $(LDFLAGS) -static
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build FPGA dump (static for portability)
$(FPGA_DUMP): $(DUMP_OBJS)
	@echo "Linking $@ (static)"
	$(CC) $(DUMP_OBJS) -o $@ $(LDFLAGS) -static
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build PSU test
$(PSU_TEST): $(PSU_OBJS)
	@echo "Linking $@"
	$(CC) $(PSU_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build id2mac
$(ID2MAC): $(ID2MAC_OBJS)
	@echo "Linking $@"
	$(CC) $(ID2MAC_OBJS) -o $@
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build eeprom_detect
$(EEPROM_DETECT): $(EEPROM_DETECT_OBJS)
	@echo "Linking $@"
	$(CC) $(EEPROM_DETECT_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build chain_test (includes BM1398 driver)
$(CHAIN_TEST): $(CHAIN_TEST_OBJS)
	@echo "Linking $@"
	$(CC) $(CHAIN_TEST_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build work_test (includes BM1398 driver)
$(WORK_TEST): $(WORK_TEST_OBJS)
	@echo "Linking $@"
	$(CC) $(WORK_TEST_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build pattern_test (includes BM1398 driver)
$(PATTERN_TEST): $(PATTERN_TEST_OBJS)
	@echo "Linking $@"
	$(CC) $(PATTERN_TEST_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build fpga_reg_test
$(FPGA_REG_TEST): $(FPGA_REG_TEST_OBJS)
	@echo "Linking $@"
	$(CC) $(FPGA_REG_TEST_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Build asic_status_check
$(ASIC_STATUS): $(ASIC_STATUS_OBJS)
	@echo "Linking $@"
	$(CC) $(ASIC_STATUS_OBJS) -o $@ $(LDFLAGS)
	@echo "Stripping $@"
	$(STRIP) $@
	@echo "Build complete: $@"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(DRV_DIR)/%.c
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts"
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Install to target filesystem
install: $(TARGET)
	@echo "Installing to target filesystem"
	cp $(TARGET) ../buildroot/output/target/usr/bin/
	cp config/miner.conf ../buildroot/output/target/etc/

# Help target
help:
	@echo "HashSource X19 Mining Software Build System"
	@echo "=============================================="
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the mining software (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  install  - Install to target filesystem"
	@echo "  config   - Create sample configuration file"
	@echo "  startup  - Create startup script"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Cross-compilation:"
	@echo "  Set CROSS_COMPILE to your toolchain prefix"
	@echo "  Example: make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-"

.PHONY: all dirs clean install config startup help
