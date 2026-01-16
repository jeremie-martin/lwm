#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Build the project
echo "Building LWM..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Start Xephyr
echo "Starting Xephyr..."
Xephyr :100 -ac -screen 1920x1080 -host-cursor &
XEPHYR_PID=$!

# Wait for Xephyr to start
sleep 1

# Create test config directory if needed
TEST_CONFIG_DIR="$PROJECT_DIR/test-config"
mkdir -p "$TEST_CONFIG_DIR"

# Copy example config if no test config exists
if [ ! -f "$TEST_CONFIG_DIR/config.toml" ]; then
    cp "$PROJECT_DIR/config.toml.example" "$TEST_CONFIG_DIR/config.toml"
fi

# Start the window manager in Xephyr
echo "Starting LWM in Xephyr..."
DISPLAY=:100 "$BUILD_DIR/src/app/lwm" "$TEST_CONFIG_DIR/config.toml" &
WM_PID=$!

echo ""
echo "LWM is running in Xephyr on display :100"
echo "You can start applications with: DISPLAY=:100 <app>"
echo ""

# Wait for user input to exit
read -p "Press Enter to exit..."

# Clean up
echo "Cleaning up..."
kill $WM_PID 2>/dev/null || true
kill $XEPHYR_PID 2>/dev/null || true

echo "Done."
