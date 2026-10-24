#!/usr/bin/env python3
"""Write the HyTalk key file for urfd ([HyTalk] KeyFile) from your own copy of
the HyTalk app.

The app encrypts its signalling with keys built into its protocol library
libpoc_protocol.so (armeabi-v7a): an 80 byte table, the DES/RC4 key (8 bytes),
then the AES-128, -192 and -256 keys (16, 24 and 32 bytes). urfd-ng does not
ship them. This script reads the table from the library, by its address in
the version below, and writes it as hex text.

    adb shell pm path com.hytalkpro.ocean          # where the radio keeps the app
    adb pull <that path> hytalk.apk
    python3 hytalk-keys.py hytalk.apk /usr/local/etc/urfd/hytalk.keys

Tested with HyTalk V3.2.00.040 (P50 Pro). Another version may keep the table
elsewhere; the script checks the result and refuses a wrong one.

Copyright (C) 2026
Licensed under the GNU General Public License v3 or later
"""
import hashlib
import struct
import sys
import zipfile

LIB = "lib/armeabi-v7a/libpoc_protocol.so"
KEY_VADDR = 0xff1ab            # virtual address of the table in the library
KEY_SIZE = 80
KEY_SHA256 = "96288a3e13939e1dc142983c99b580ca4782e461f4b17c6657ad001c29531753"  # of the table, to refuse a wrong one


def load_library(path):
    if path.endswith(".so"):
        return open(path, "rb").read()
    with zipfile.ZipFile(path) as z:
        return z.read(LIB)


def vaddr_to_offset(elf, vaddr):
    if elf[:4] != b"\x7fELF" or elf[4] != 1:
        sys.exit("not a 32 bit ELF library")
    phoff, = struct.unpack_from("<I", elf, 0x1c)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2a)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", elf, phoff + i * phentsize)
        if p_type == 1 and p_vaddr <= vaddr and vaddr + KEY_SIZE <= p_vaddr + p_filesz:
            return vaddr - p_vaddr + p_offset
    sys.exit("the key table's address is not in this library (another app version?)")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    elf = load_library(sys.argv[1])
    off = vaddr_to_offset(elf, KEY_VADDR)
    keys = elf[off:off + KEY_SIZE]
    if hashlib.sha256(keys).hexdigest() != KEY_SHA256:
        sys.exit("this is not the expected key table (another app version?), nothing written")
    with open(sys.argv[2], "w") as f:
        f.write("# HyTalk keys for urfd: DES/RC4 8, AES-128 16, AES-192 24, AES-256 32 bytes\n")
        for name, a, b in (("DES/RC4", 0, 8), ("AES-128", 8, 24), ("AES-192", 24, 48), ("AES-256", 48, 80)):
            f.write(" ".join("%02x" % x for x in keys[a:b]) + "   # " + name + "\n")
    print("wrote", sys.argv[2])


if __name__ == "__main__":
    main()
