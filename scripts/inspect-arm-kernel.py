#!/usr/bin/env python3
"""Disassemble named ARM32 functions from an ELF supplied by the researcher.

This is a generalized, publication-safe offline analysis helper.
Requires pyelftools and capstone. Does not bundle firmware, touch a device,
write to an ELF, or execute target code.
"""
import argparse
from pathlib import Path
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", required=True, type=Path, help="Legally obtained ARM32 ELF")
    p.add_argument("symbols", nargs="+", help="Function names")
    args = p.parse_args()

    try:
        from elftools.elf.elffile import ELFFile
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    except ImportError as exc:
        p.error(f"missing optional dependency: {exc}; install pyelftools and capstone")

    with args.elf.open("rb") as f:
        elf = ELFFile(f)
        if elf["e_machine"] != "EM_ARM":
            p.error("only ARM32 ELF is supported")
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            p.error("an unstripped ELF with .symtab is required")
        symbols = {sym.name: sym for sym in symtab.iter_symbols() if sym.name}
        disassembler = Cs(CS_ARCH_ARM, CS_MODE_ARM)

        failed = False
        for name in args.symbols:
            sym = symbols.get(name)
            if sym is None:
                print(f"symbol not found: {name}", file=sys.stderr)
                failed = True
                continue
            index = sym["st_shndx"]
            if not isinstance(index, int):
                print(f"symbol has no section: {name}", file=sys.stderr)
                failed = True
                continue
            section = elf.get_section(index)
            address, size = int(sym["st_value"]), int(sym["st_size"])
            offset = address - int(section["sh_addr"])
            code = section.data()[offset : offset + size]
            print(f"{name} address=0x{address:08x} size={size}")
            if not code:
                print(f"symbol has no contents: {name}", file=sys.stderr)
                failed = True
            for ins in disassembler.disasm(code, address):
                print(f"0x{ins.address:08x}: {ins.mnemonic:8} {ins.op_str}")
        return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        # A consumer such as 'head' can close stdout before disassembly ends.
        # This is not a disassembler or input-file error.
        try:
            sys.stdout.close()
        except OSError:
            pass
        sys.exit(0)
