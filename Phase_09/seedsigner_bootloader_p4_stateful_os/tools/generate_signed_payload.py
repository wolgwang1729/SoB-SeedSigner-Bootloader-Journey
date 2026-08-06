import hashlib
from ecdsa import SigningKey, SECP256k1
from ecdsa.util import sigencode_string
import struct
import bech32
import os

BL_SECT_MAGIC = 0x54434553
BL_SECT_STRUCT_REV = 1

def crc32_fast(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF

def pack_attribute(key, value):
    if isinstance(value, str):
        value = value.encode('ascii')
    elif isinstance(value, int):
        if value < 256:
            value = struct.pack('<B', value)
        elif value < 65536:
            value = struct.pack('<H', value)
        else:
            value = struct.pack('<I', value)
    return struct.pack('<BB', key, len(value)) + value

def build_section(name, version, payload, attributes=b''):
    attr_list = attributes + b'\x00' * (216 - len(attributes))
    pl_size = len(payload)
    pl_crc = crc32_fast(payload) if pl_size > 0 else 0
    fmt = '<II16sIII216s'
    header = struct.pack(fmt,
                         BL_SECT_MAGIC,
                         BL_SECT_STRUCT_REV,
                         name.encode('ascii'),
                         version,
                         pl_size,
                         pl_crc,
                         attr_list)
    struct_crc = crc32_fast(header)
    header += struct.pack('<I', struct_crc)
    return header + payload

# Fixed dummy key
priv_key = SigningKey.from_secret_exponent(123456789, curve=SECP256k1)
pub_key = priv_key.get_verifying_key()
pub_bytes = b'\x04' + pub_key.to_string("uncompressed")
print("const bl_pubkey_t vendor_keys[] = {")
print("    { .bytes = { " + ", ".join(f"0x{b:02x}" for b in pub_bytes) + " } },")
print("    BL_PUBKEY_END_OF_LIST\n};")

# Fingerprint: first 16 bytes of double sha256 of pubkey
fp = hashlib.sha256(hashlib.sha256(pub_bytes).digest()).digest()[:16]

import sys

if len(sys.argv) < 3:
    print("Usage: python generate_signed_payload.py <input.bin> <output.bin>")
    sys.exit(1)

with open(sys.argv[1], 'rb') as f:
    payload = f.read()

attrs = pack_attribute(4, "seedsigner_esp32p4") + pack_attribute(1, "secp256k1-sha256")
main_section = build_section("main", 1, payload, attrs)

main_hash = hashlib.sha256(main_section).digest()
hrp = "1-"
converted_bits = bech32.convertbits(main_hash, 8, 5, pad=True)
msg_str = bech32.bech32_encode(hrp, converted_bits)
msg_bytes = msg_str.encode('ascii')

msg_hash = hashlib.sha256(msg_bytes).digest()
signature = priv_key.sign_digest_deterministic(msg_hash, sigencode=sigencode_string)

sig_payload = fp + signature
sig_section = build_section("sign", 1, sig_payload)

with open(sys.argv[2], 'wb') as f:
    f.write(main_section)
    f.write(sig_section)
print(f"Signed {sys.argv[2]} successfully!")


