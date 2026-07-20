#!/usr/bin/env bash

# Toggle between xcompmgr and picom on each key press.
# Press any key to toggle; Ctrl+C to exit.

set -euo pipefail

if ! command -v xcompmgr >/dev/null 2>&1; then
  echo "xcompmgr not found in PATH" >&2
  exit 1
fi

if ! command -v picom >/dev/null 2>&1; then
  echo "picom not found in PATH" >&2
  exit 1
fi

XCOMPMGR_CMD=(xcompmgr)
PICOM_CMD=(picom --config "$HOME/.config/picom/picom.conf")

cleanup() {
  pkill -x xcompmgr 2>/dev/null || true
  pkill -x picom 2>/dev/null || true
}

start_xcompmgr() {
  pkill -x picom 2>/dev/null || true
  sleep 0.1
  "${XCOMPMGR_CMD[@]}" >/tmp/compositor.log 2>&1 &
}

start_picom() {
  pkill -x xcompmgr 2>/dev/null || true
  sleep 0.1
  "${PICOM_CMD[@]}" >/tmp/compositor.log 2>&1 &
}

trap 'echo -e "\nStopping toggle script."; exit 0' INT

start_xcompmgr
using="xcompmgr"

echo "Compositor toggle running: currently using ${using}."
echo "Press any key to switch, Ctrl+C to quit."

while true; do
  read -rsn1 _key
  if [[ "${using}" == "xcompmgr" ]]; then
    start_picom
    using="picom"
  else
    start_xcompmgr
    using="xcompmgr"
  fi
  echo "Switched to ${using}."
  done
