#!/usr/bin/env python3
"""Offline inspection of *caller-supplied* esconfig register-operation records.

Only prints decoded scalar operations. Never emits embedded firmware data,
connects to a device, or executes any register operation.

The parser consumes 40-byte records as ten little-endian u32 fields:
operation, profile mask, selector, unused, value-low, value-high, poll-mask,
unused, delay, unused. Field names are interpretations for the studied image.
Use --offset with the ELF *file* offset.
"""
import argparse
import struct
from pathlib import Path

REC = struct.Struct("<IIIII IIIII".replace(" ", ""))
WIDTH = {0: 8, 1: 16, 2: 32, 3: 48, 4: 64}


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--elf", type=Path, required=True, help="Your own, legally obtained native ELF")
    cli.add_argument("--offset", type=lambda x: int(x, 0), required=True, help="File offset, not virtual address")
    cli.add_argument("--count", type=int, required=True, help="Number of consecutive 40-byte records")
    args = cli.parse_args()
    if not 1 <= args.count <= 200:
        cli.error("--count must be between 1 and 200")
    data = args.elf.read_bytes()
    if args.offset < 0 or args.offset + args.count * REC.size > len(data):
        cli.error("Requested range exceeds ELF file size")
    for i in range(args.count):
        p = args.offset + i * REC.size
        op, profile, selector, _, vlow, vhigh, pollmask, _, delay, _ = REC.unpack_from(data, p)
        page, reg, width_idx = selector >> 24, (selector >> 16) & 0xFF, selector & 0xF
        if width_idx not in WIDTH:
            print(f"[{i:02}] unexpected width code {width_idx} at offset {p:#x}")
            continue
        value = (vhigh << 32) | vlow
        print(f"[{i:02}] mode_mask={profile:#x} operation={op} "
              f"page={page:#04x} reg={reg:#04x} bits={WIDTH[width_idx]} "
              f"value={value:#018x} pollmask={pollmask:#x} delay={delay}")
        if page == 0x05 and reg == 0x83 and width_idx == 2:
            print(f"     VLAN entry: members={value & 0x1ff:#05x} "
                  f"untag={(value >> 9) & 0x1ff:#05x} "
                  f"additional_bits={value & ~0x3ffff:#010x}")


if __name__ == "__main__":
    main()
