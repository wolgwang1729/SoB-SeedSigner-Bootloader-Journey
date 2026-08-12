#!/usr/bin/env bash
# ==============================================================================
# capture_log.sh — Phase 14: reset the board and capture one boot into logs/<tag>.txt
#
#   capture_log.sh <tag> [timeout_seconds]
#
# Delegates to capture_serial.py (pyserial + esptool-style RTS reset), which
# avoids idf.py monitor's slow cmake reconfigure. Console baud is 115200 (the
# loader's CONFIG_ESP_CONSOLE_UART_BAUDRATE), NOT the 921600 esptool flash baud.
# ==============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:?usage: capture_log.sh <tag> [timeout_s]}"
TIMEOUT="${2:-20}"
PORT="${ESPPORT:-/dev/ttyACM0}"

exec python3 "$SCRIPT_DIR/capture_serial.py" "$TAG" --timeout "$TIMEOUT" --port "$PORT" --baud "${ESPBAUD:-115200}"
