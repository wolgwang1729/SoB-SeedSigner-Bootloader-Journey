# First Test: Hello World on ESP32-S3

This is a clean, unmodified standard ESP-IDF `hello_world` project created for testing your new ESP32-S3 board.

## Steps Taken

1. **Copied Example**: Copied the standard `hello_world` example from the local ESP-IDF installation:
   ```bash
   cp -r ~/esp/esp-idf/examples/get-started/hello_world <repo>/Phase_08/hello_world_esp32s3
   ```
2. **Environment Setup**: Sourced the ESP-IDF environment variables:
   ```bash
   source ~/esp/esp-idf/export.sh
   ```
3. **Set Target**: Configured the build system for the ESP32-S3 architecture:
   ```bash
   idf.py set-target esp32s3
   ```
4. **Built Project**: Compiled the code, partition table, and bootloader:
   ```bash
   idf.py build
   ```

## Security & eFuse Verification

To ensure that your hardware remains completely unaltered, I explicitly checked the generated `sdkconfig` file.

- **Secure Boot**: `grep_search` confirmed that no `CONFIG_SECURE_...` or `CONFIG_SECURE_BOOT_V2_ENABLED` variables exist in the configuration.
- **eFuses**: No eFuse burning configurations or flash encryption settings are active.

This is a plain, unprotected firmware that executes from flash and loops indefinitely, making it 100% safe to run without permanently altering the board's secure state.

## How to Flash and Monitor

Once your board is connected via USB, run:
```bash
cd <repo>/Phase_08/hello_world_esp32s3
source ~/esp/esp-idf/export.sh
idf.py flash monitor
```
*(Use `Ctrl + ]` to exit the monitor)*
