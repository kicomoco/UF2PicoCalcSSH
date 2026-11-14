"""Validate a Pico 2 W UF2, including picotool's RP2350 compatibility marker."""
import argparse
import hashlib
import pathlib
import struct


def verify(path):
    data = path.read_bytes()
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    app_blocks = []
    markers = 0
    for offset in range(0, len(data), 512):
        start, magic, flags, address, size, number, total, family = struct.unpack_from("<8I", data, offset)
        end, = struct.unpack_from("<I", data, offset + 508)
        if (start, magic, end) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise ValueError(f"Bad UF2 magic at byte {offset}")
        if size != 256 or not flags & 0x2000:
            raise ValueError(f"Invalid payload or missing family at byte {offset}")
        if family == 0xE48BFF57 and flags == 0xA000 and address == 0x10FFFF00:
            markers += 1
            continue
        if family != 0xE48BFF59 or flags != 0x2000:
            raise ValueError(f"Not an RP2350 ARM-S application at byte {offset}")
        if address < 0x10000000 or address + size > 0x10400000 or address % 256:
            raise ValueError(f"Application address outside Pico 2 W flash: {address:#x}")
        app_blocks.append((number, total, address))
    count = len(app_blocks)
    if not count or markers > 1:
        raise ValueError("Missing application or duplicate compatibility marker")
    addresses = set()
    for expected, (number, total, address) in enumerate(app_blocks):
        if number != expected or total != count or address in addresses:
            raise ValueError("Missing, duplicate or out-of-order application blocks")
        addresses.add(address)
    if min(addresses) != 0x10000000:
        raise ValueError("UF2 is not linked at the uf2loader application base")
    print(f"Valid Pico 2 W UF2: {count} application blocks, {markers} RP2350 marker")
    print(f"Flash: {min(addresses):#010x}..{max(addresses) + 256:#010x}")
    print(f"SHA256: {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("uf2", type=pathlib.Path)
    args = parser.parse_args()
    verify(args.uf2)
