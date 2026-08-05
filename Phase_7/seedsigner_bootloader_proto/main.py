import machine
import struct
import time
import sys

print("=========================================")
print("  SEEDSIGNER BOOTING...                  ")
print("=========================================")

try:
    spi = machine.SPI(1, baudrate=40000000, sck=machine.Pin(14), mosi=machine.Pin(13))
    cs = machine.Pin(19, machine.Pin.OUT)
    dc = machine.Pin(17, machine.Pin.OUT)

    def write_cmd(cmd):
        dc.value(0)
        cs.value(0)
        spi.write(bytearray([cmd]))
        cs.value(1)

    def write_data(data):
        dc.value(1)
        cs.value(0)
        spi.write(data if isinstance(data, bytearray) else bytearray([data]))
        cs.value(1)

    # ILI9341 Initialization
    write_cmd(0x01) # Software reset
    time.sleep_ms(150)
    write_cmd(0x11) # Sleep out
    time.sleep_ms(255)
    write_cmd(0x3A) # Pixel format
    write_data(0x55) # 16-bit
    write_cmd(0x36) # Memory Access Control
    write_data(0x28) # MV, BGR
    write_cmd(0x29) # Display ON
    time.sleep_ms(50)

    # Fill screen with ORANGE
    write_cmd(0x2A) # Column addr set
    write_data(bytearray([0, 0, 1, 63])) # 320 width
    write_cmd(0x2B) # Row addr set
    write_data(bytearray([0, 0, 0, 239])) # 240 height
    write_cmd(0x2C) # Memory write

    color = struct.pack(">H", 0xFD20) # Orange in RGB565
    buf = color * 320
    for i in range(240):
        write_data(buf)
        
    print("UI Display Init Success!")
except Exception as e:
    print("UI Init Error:", e)

# Spin in a tight loop to block the REPL, preventing it from dropping out
while True:
    time.sleep(1)
