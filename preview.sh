#!/bin/bash
set -e

# Compile the window manager
g++ -std=c++20 -o wm main.cpp -lxcb -lxcb-keysyms

# Start Xephyr
Xephyr :100 -ac -screen 1920x1080 -host-cursor &
XEPHYR_PID=$!

# Wait for Xephyr to start
sleep 1

# Start the window manager in Xephyr
DISPLAY=:100 ./wm &
WM_PID=$!

# Optionally start some test applications
# DISPLAY=:100 xeyes &
# DISPLAY=:100 xterm &

# Wait for user input to exit
read -p "Press Enter to exit..."

# Clean up
kill $WM_PID
kill $XEPHYR_PID
