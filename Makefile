# LWM - Lightweight Window Manager
# Root Makefile wrapping CMake

BUILD_DIR := build
CMAKE := cmake
NPROC := $(shell nproc)

.PHONY: all build release debug install uninstall test clean distclean help

# Default target
all: build

# Release build (default)
build:
	@mkdir -p $(BUILD_DIR)
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	@$(MAKE) -C $(BUILD_DIR) -j$(NPROC)

# Debug build
debug:
	@mkdir -p $(BUILD_DIR)
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	@$(MAKE) -C $(BUILD_DIR) -j$(NPROC)

# Build with tests
test:
	@mkdir -p $(BUILD_DIR)
	@$(CMAKE) -S . -B $(BUILD_DIR) -DBUILD_TESTS=ON
	@$(MAKE) -C $(BUILD_DIR) -j$(NPROC)
	@$(BUILD_DIR)/tests/lwm_tests

# Install to system (requires sudo)
install: build
	@$(CMAKE) --install $(BUILD_DIR)

# Uninstall from system (requires sudo)
uninstall:
	@rm -f /usr/local/bin/lwm
	@echo "Uninstalled lwm"

# Clean build artifacts
clean:
	@rm -rf $(BUILD_DIR)

# Alias for clean
distclean: clean

help:
	@echo "LWM Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make           Build release binary"
	@echo "  make debug     Build debug binary"
	@echo "  make test      Build and run tests"
	@echo "  make install   Install to /usr/local/bin (use with sudo)"
	@echo "  make uninstall Remove from /usr/local/bin (use with sudo)"
	@echo "  make clean     Remove build directory"
	@echo "  make help      Show this help"
