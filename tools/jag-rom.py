#!/usr/bin/env python3
"""Makes a Jaguar cartridge image from a program linked with platform/jaguar/jaguar.ld.

    tools/jag-rom.py <program.elf> <header.bin> <out.rom>

The program runs in place from the cartridge, from $802000, so the image is the 8 KB boot
header that the Jaguar's boot ROM checks ($800000; the universal homebrew header, which is not
in this repository), then the program's ROM sections as objcopy -O binary gives them (code,
constants, then .data for startup.S to copy to RAM), padded with $FF to a power of two, or to
6 MB, the most a cartridge holds.
"""
import os
import subprocess
import sys
import tempfile

HEADER_SIZE = 0x2000
LARGEST = 0x600000


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    elf, header_path, out = sys.argv[1:]
    objcopy = os.environ.get("OBJCOPY", "m68k-elf-objcopy")
    header = open(header_path, "rb").read()
    if len(header) != HEADER_SIZE:
        sys.exit(f"{header_path}: {len(header)} bytes, not {HEADER_SIZE}")
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "program.bin")
        subprocess.run([objcopy, "-O", "binary", "-j", ".text", "-j", ".data", elf, raw],
                       check=True)
        program = open(raw, "rb").read()
    image = header + program
    if len(image) > LARGEST:
        sys.exit(f"{out}: {len(image)} bytes, over the 6 MB a cartridge holds")
    size = 1
    while size < len(image):
        size <<= 1
    image += b"\xff" * (min(size, LARGEST) - len(image))
    open(out, "wb").write(image)
    print(f"{out}: {len(program)} bytes of program, {len(image)} in all")


if __name__ == "__main__":
    main()
