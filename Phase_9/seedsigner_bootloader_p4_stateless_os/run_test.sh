#!/usr/bin/env bash
# ==============================================================================
# Automated Build, Flash & Serial Test Verification Script (Milestone 2)
# Target: ESP32-P4 Stateless OS Bootloader & Payload
# Safety: Enforces Virtual eFuses (CONFIG_EFUSE_VIRTUAL=y)
# ==============================================================================
set -uo pipefail
# ------------------------------------------------------------------------------
# Logging Helper Functions
# ------------------------------------------------------------------------------
log_info()    { echo -e "\033[1;34m[INFO]\033[0m $*"; }
log_success() { echo -e "\033[1;32m[PASS]\033[0m $*"; }
log_warn()    { echo -e "\033[1;33m[WARN]\033[0m $*"; }
log_error()   { echo -e "\033[1;31m[FAIL]\033[0m $*" >&2; }
log_step()    { echo -e "\n\033[1;35m=== $* ===\033[0m"; }

# ------------------------------------------------------------------------------
# Path Resolution & Environment Discovery
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -d "$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os" ]; then
    ROOT_DIR="$SCRIPT_DIR"
    BOOTLOADER_DIR="$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os"
    PAYLOAD_DIR="$SCRIPT_DIR/hello_world_esp32p4_stateless_payload"
elif [ -f "$SCRIPT_DIR/CMakeLists.txt" ] && [ -d "$SCRIPT_DIR/../hello_world_esp32p4_stateless_payload" ]; then
    BOOTLOADER_DIR="$SCRIPT_DIR"
    ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
    PAYLOAD_DIR="$ROOT_DIR/hello_world_esp32p4_stateless_payload"
else
    log_error "Unable to locate project directories from $SCRIPT_DIR"
    exit 1
fi

# Ensure ESP-IDF environment is sourced if idf.py is not in PATH
if ! command -v idf.py >/dev/null 2>&1; then
    log_info "idf.py not found in PATH, attempting to source ESP-IDF environment..."
    if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
        . "$IDF_PATH/export.sh" >/dev/null 2>&1 || true
    elif [ -f "$HOME/esp/esp-idf-v5.5/export.sh" ]; then
        . "$HOME/esp/esp-idf-v5.5/export.sh" >/dev/null 2>&1 || true
    elif [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        . "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1 || true
    fi
fi

if ! command -v idf.py >/dev/null 2>&1; then
    log_error "idf.py command not found. Please source ESP-IDF export.sh before running this script."
    exit 1
fi

# Configuration parameters with environment variable fallbacks
PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-115200}"
TIMEOUT_SECONDS="${TEST_TIMEOUT:-30}"
LOGFILE=$(mktemp /tmp/esp32p4_test_XXXXXX.log)
MONITOR_PID=""

# Cleanup handler for graceful shutdown on signals or completion
cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM
    if [ -n "$MONITOR_PID" ] && kill -0 "$MONITOR_PID" 2>/dev/null; then
        log_info "Terminating background serial monitor (PID $MONITOR_PID)..."
        kill "$MONITOR_PID" 2>/dev/null || true
        wait "$MONITOR_PID" 2>/dev/null || true
    fi
    if [ -n "$LOGFILE" ] && [ -f "$LOGFILE" ]; then
        rm -f "$LOGFILE"
    fi
    if [ $exit_code -eq 0 ]; then
        log_success "Automated Test Runner execution finished successfully (EXIT 0)."
    else
        log_error "Automated Test Runner execution failed (EXIT $exit_code)."
    fi
    exit $exit_code
}
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------------------
# Step 1: Pre-Flight Safety Verification (Virtual eFuse Protection)
# ------------------------------------------------------------------------------
log_step "Step 1: Pre-Flight Safety Verification"

check_virtual_efuse() {
    local sdk_path="$1"
    local proj_name="$2"
    if [ -f "$sdk_path" ]; then
        if grep -q "CONFIG_EFUSE_VIRTUAL=y" "$sdk_path"; then
            log_info "  - $proj_name ($sdk_path): CONFIG_EFUSE_VIRTUAL=y verified."
        else
            log_error "CRITICAL SAFETY VIOLATION: CONFIG_EFUSE_VIRTUAL=y NOT set in $sdk_path!"
            log_error "Flashing aborted to prevent potential physical eFuse modification."
            exit 1
        fi
    else
        log_warn "  - Config file $sdk_path not found, skipping check."
    fi
}

check_virtual_efuse "$BOOTLOADER_DIR/sdkconfig.defaults" "Bootloader (defaults)"
check_virtual_efuse "$PAYLOAD_DIR/sdkconfig.defaults" "Payload (defaults)"
check_virtual_efuse "$BOOTLOADER_DIR/sdkconfig" "Bootloader (sdkconfig)"
check_virtual_efuse "$PAYLOAD_DIR/sdkconfig" "Payload (sdkconfig)"

# ------------------------------------------------------------------------------
# Step 2: Build Bootloader Target
# ------------------------------------------------------------------------------
log_step "Step 2: Building Bootloader (seedsigner_bootloader_p4_stateless_os)"
log_info "Compiling bootloader in $BOOTLOADER_DIR..."
(
    cd "$BOOTLOADER_DIR"
    idf.py build
)

BOOTLOADER_BIN="$BOOTLOADER_DIR/build/bootloader/bootloader.bin"
PARTITION_BIN="$BOOTLOADER_DIR/build/partition_table/partition-table.bin"
SECURE_LOADER_BIN="$BOOTLOADER_DIR/build/seedsigner_secure_loader.bin"

for bin in "$BOOTLOADER_BIN" "$PARTITION_BIN" "$SECURE_LOADER_BIN"; do
    if [ ! -f "$bin" ] || [ ! -s "$bin" ]; then
        log_error "Missing or empty bootloader build artifact: $bin"
        exit 1
    fi
done
log_success "Bootloader build artifacts verified."

# ------------------------------------------------------------------------------
# Step 3: Build Payload Target
# ------------------------------------------------------------------------------
log_step "Step 3: Building Payload (hello_world_esp32p4)"
log_info "Compiling payload in $PAYLOAD_DIR..."
(
    cd "$PAYLOAD_DIR"
    idf.py build
)

RAW_PAYLOAD_BIN="$PAYLOAD_DIR/build/hello_world_esp32p4.bin"
if [ ! -f "$RAW_PAYLOAD_BIN" ] || [ ! -s "$RAW_PAYLOAD_BIN" ]; then
    log_error "Missing or empty payload build artifact: $RAW_PAYLOAD_BIN"
    exit 1
fi
log_success "Payload binary verified."

# ------------------------------------------------------------------------------
# Step 4: Sign Payload
# ------------------------------------------------------------------------------
log_step "Step 4: Signing Payload with generate_signed_payload.py"
SIGN_SCRIPT="$BOOTLOADER_DIR/tools/generate_signed_payload.py"
SIGNED_PAYLOAD_BIN="$PAYLOAD_DIR/build/seed.bin"

if [ ! -f "$SIGN_SCRIPT" ]; then
    log_error "Signing tool missing: $SIGN_SCRIPT"
    exit 1
fi

log_info "Generating signed payload $SIGNED_PAYLOAD_BIN..."
python3 "$SIGN_SCRIPT" "$RAW_PAYLOAD_BIN" "$SIGNED_PAYLOAD_BIN"

if [ ! -f "$SIGNED_PAYLOAD_BIN" ] || [ ! -s "$SIGNED_PAYLOAD_BIN" ]; then
    log_error "Signed payload generation failed or produced empty file."
    exit 1
fi
log_success "Signed payload seed.bin generated successfully."

# ------------------------------------------------------------------------------
# Step 5: Flash Targets to Device
# ------------------------------------------------------------------------------
log_step "Step 5: Flashing Binaries to Target Port ($PORT)"

if [ ! -e "$PORT" ]; then
    log_error "Serial port $PORT does not exist. Ensure ESP32-P4 is connected or set ESPPORT."
    exit 1
fi

# Release serial port if held by any stale monitor process
pkill -f "python3.*serial" 2>/dev/null || true
log_step "Step 5: Flashing Binaries to Target Port ($PORT)"
log_info "Writing bootloader, partition table, secure loader, and signed payload at ${BAUD} baud..."

pkill -9 -f "serial.Serial" 2>/dev/null || true
fuser -k "$PORT" 2>/dev/null || true
sleep 0.5
esptool.py --chip esp32p4 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 2MB --flash_freq 40m \
    0x2000 "$BOOTLOADER_BIN" \
    0x20000 "$PARTITION_BIN" \
    0x30000 "$SECURE_LOADER_BIN" \
    0x140000 "$SIGNED_PAYLOAD_BIN"

log_success "Flash operation completed successfully."

log_step "Step 6: Serial Log Capture & Assertion Validation"
log_info "Monitoring serial log on $PORT (timeout: ${TIMEOUT_SECONDS}s)..."
> "$LOGFILE"

python3 -u -c "
import serial, sys, time
port = '$PORT'
baud = $BAUD
try:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.5
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.2)
    while True:
        line = ser.readline()
        if line:
            sys.stdout.write(line.decode('utf-8', errors='ignore'))
            sys.stdout.flush()
except Exception as e:
    sys.stderr.write(str(e))
" > "$LOGFILE" 2>&1 &
MONITOR_PID=$!

start_time=$(date +%s)
has_sig_pass=false
has_jmp13=false
has_hello=false
has_app_main=false

# ------------------------------------------------------------------------------
# Step 6: Asynchronous Serial Capture & Automated Validation
# ------------------------------------------------------------------------------
log_step "Step 6: Serial Log Capture & Assertion Validation"
log_info "Monitoring serial log on $PORT (timeout: ${TIMEOUT_SECONDS}s)..."

while true; do
    if [ -f "$LOGFILE" ]; then
        if grep -F -q "Signature verification PASSED!" "$LOGFILE"; then
            has_sig_pass=true
        fi
        if grep -F -q "JMP[13] JUMP!" "$LOGFILE"; then
            has_jmp13=true
        fi
        if grep -qE "Hello world!|\[PAYLOAD\]" "$LOGFILE"; then
            has_hello=true
        fi
        if grep -qE "silicon revision v|Minimum free heap size:|app_main" "$LOGFILE"; then
            has_app_main=true
        fi

        # Check for catastrophic crash markers
        if grep -qE "Guru Meditation Error|Panic|Rebooting..." "$LOGFILE"; then
            log_error "Hardware crash or panic detected in serial output!"
            echo "==================== SERIAL LOG DUMP ===================="
            cat "$LOGFILE"
            echo "========================================================="
            exit 1
        fi

        # Check if all 4 required log assertions are satisfied
        if $has_sig_pass && $has_jmp13 && $has_hello && $has_app_main; then
            log_success "All serial log assertions satisfied:"
            log_info "  [X] Signature verification PASSED!"
            log_info "  [X] JMP[13] JUMP! handoff completed"
            log_info "  [X] Hello world! payload entry reached"
            log_info "  [X] app_main() executed successfully without hanging"
            break
        fi
    fi

    now=$(date +%s)
    elapsed=$((now - start_time))
    if [ $elapsed -ge "$TIMEOUT_SECONDS" ]; then
        log_error "Timeout (${TIMEOUT_SECONDS}s) reached waiting for serial log assertions!"
        log_info "Assertion state at timeout:"
        log_info "  - Signature verification PASSED: $has_sig_pass"
        log_info "  - JMP[13] JUMP!: $has_jmp13"
        log_info "  - Hello world!: $has_hello"
        log_info "  - app_main() complete: $has_app_main"
        echo "==================== SERIAL LOG DUMP ===================="
        if [ -f "$LOGFILE" ]; then
            cat "$LOGFILE"
        else
            echo "<Logfile empty or not created>"
        fi
        echo "========================================================="
        exit 1
    fi

    sleep 0.5
done

log_step "Verification Result"
log_success "Milestone 2 Automated Verification PASSED."
exit 0
