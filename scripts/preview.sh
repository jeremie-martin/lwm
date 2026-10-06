#!/bin/bash
# Interactive preview on :100; see TESTING.md#nested-preview.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
CONFIG_DIR="$PROJECT_DIR/config"
export LWM_SCRIPTS="$SCRIPT_DIR"

echo "Building LWM..."
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

WM_PID=""
POLYBAR_PID=""
XEPHYR_PID=""
# Stop everything this script started, however it exits.
cleanup() {
    for pid in $POLYBAR_PID $WM_PID $XEPHYR_PID; do kill "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT

echo "Starting Xephyr..."
Xephyr :100 -ac -screen 1920x1080 -host-cursor &
XEPHYR_PID=$!

sleep 1

TEST_CONFIG_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/lwm-preview"
mkdir -p "$TEST_CONFIG_DIR"

if [ ! -f "$TEST_CONFIG_DIR/config.toml" ]; then
    cp "$PROJECT_DIR/config.toml.example" "$TEST_CONFIG_DIR/config.toml"
fi

echo "Starting LWM in Xephyr..."
DISPLAY=:100 "$BUILD_DIR/src/app/lwm" --log-target stderr --config "$TEST_CONFIG_DIR/config.toml" &
WM_PID=$!

sleep 0.5

if command -v polybar &> /dev/null; then
    echo "Launching Polybar..."
    DISPLAY=:100 polybar --config="$CONFIG_DIR/polybar.ini" main &
    POLYBAR_PID=$!
fi

echo ""
echo "LWM is running in Xephyr on display :100"
echo "You can start applications with: DISPLAY=:100 <app>"
echo ""

read -r -p "Press Enter to exit..."
