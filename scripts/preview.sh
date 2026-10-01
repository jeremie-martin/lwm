#!/bin/bash
# Interactive preview on :100; see CONTRIBUTING.md#nested-preview.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
CONFIG_DIR="$PROJECT_DIR/config"

echo "Building LWM..."
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo "Starting Xephyr..."
Xephyr :100 -ac -screen 1920x1080 -host-cursor &
XEPHYR_PID=$!

sleep 1

TEST_CONFIG_DIR="$PROJECT_DIR/test-config"
mkdir -p "$TEST_CONFIG_DIR"

if [ ! -f "$TEST_CONFIG_DIR/config.toml" ]; then
    cp "$PROJECT_DIR/config.toml.example" "$TEST_CONFIG_DIR/config.toml"
fi

echo "Starting LWM in Xephyr..."
DISPLAY=:100 "$BUILD_DIR/src/app/lwm" --log-target stderr --config "$TEST_CONFIG_DIR/config.toml" &
WM_PID=$!

sleep 0.5

POLYBAR_PID=""
if command -v polybar &> /dev/null; then
    echo "Launching Polybar..."
    DISPLAY=:100 polybar --config="$CONFIG_DIR/polybar.ini" main &
    POLYBAR_PID=$!
fi

echo ""
echo "LWM is running in Xephyr on display :100"
echo "You can start applications with: DISPLAY=:100 <app>"
echo ""

read -p "Press Enter to exit..."

echo "Cleaning up..."
if [ -n "$POLYBAR_PID" ]; then
    kill "$POLYBAR_PID" 2>/dev/null || true
fi
kill $WM_PID 2>/dev/null || true
kill $XEPHYR_PID 2>/dev/null || true

echo "Done."
