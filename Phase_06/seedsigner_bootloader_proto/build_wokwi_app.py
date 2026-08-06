import os

APP_PATH = 'build/seedsigner_secure_loader.bin'
SEED_PATH = 'dummy_fat_dir/seed.bin'
OUT_PATH = 'wokwi_app.bin'
PADDING_OFFSET = 0x6FF00

with open(APP_PATH, 'rb') as f:
    app_data = f.read()

with open(SEED_PATH, 'rb') as f:
    seed_data = f.read()

out_data = bytearray(b'\xFF' * PADDING_OFFSET)
out_data[0:len(app_data)] = app_data
out_data.extend(seed_data)

with open(OUT_PATH, 'wb') as f:
    f.write(out_data)

print(f"Created {OUT_PATH} with size {len(out_data)} bytes.")
