# LWM - Lightweight Window Manager
# Root Makefile wrapping CMake

BUILD_DIR ?= build
TEST_BUILD_TYPE ?= Debug
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
	@$(CMAKE) -S . -B $(BUILD_DIR) -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=$(TEST_BUILD_TYPE)
	@$(MAKE) -C $(BUILD_DIR) -j$(NPROC)
	@LWM_TEST_REQUIRE_X11=1 $(BUILD_DIR)/tests/lwm_tests
	@$(BUILD_DIR)/tests/lwm_logging_tests
	@ctest --test-dir "$(BUILD_DIR)" -R '^uninstall_manifest$$' --output-on-failure

# Install to system (requires sudo)
install: build
	@$(CMAKE) --install $(BUILD_DIR)

# Uninstall from system (requires sudo)
uninstall:
	@$(CMAKE) --build "$(BUILD_DIR)" --target uninstall

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
	@echo "  make uninstall Remove files recorded by this build (use sudo if needed)"
	@echo "  make clean     Remove build directory"
	@echo "  make help      Show this help"
