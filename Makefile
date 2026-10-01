# Convenience commands; CMake owns the build and CTest owns the test inventory.
BUILD_DIR ?= build
BUILD_TYPE ?= Release
TEST_BUILD_TYPE ?= Debug
CMAKE ?= cmake
NPROC ?= $(shell nproc)

.PHONY: all build release debug test install uninstall clean distclean help
all release: build

build:
	@$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_OPTIONS)
	@$(CMAKE) --build "$(BUILD_DIR)" --config $(BUILD_TYPE) --parallel $(NPROC)

debug: BUILD_TYPE = Debug
debug: build

test: BUILD_TYPE = $(TEST_BUILD_TYPE)
test: override CMAKE_OPTIONS += -DBUILD_TESTING=ON
test: build
	@LWM_TEST_REQUIRE_X11=1 ctest --test-dir "$(BUILD_DIR)" -C $(BUILD_TYPE) --output-on-failure --no-tests=error

install: build
	@$(CMAKE) --install "$(BUILD_DIR)" --config $(BUILD_TYPE)

uninstall:
	@$(CMAKE) --build "$(BUILD_DIR)" --target uninstall

clean distclean:
	@rm -rf "$(BUILD_DIR)"

help:
	@echo "make [build|release]  Release build"
	@echo "make debug            Debug build with invariant checks"
	@echo "make test             Debug build and all CTest checks"
	@echo "make install          Install under CMAKE_INSTALL_PREFIX"
	@echo "make uninstall        Remove files recorded by this build"
	@echo "make clean            Remove BUILD_DIR"
