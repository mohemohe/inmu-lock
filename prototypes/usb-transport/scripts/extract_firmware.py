#!/usr/bin/env python3
# FT9348W MCU firmware を ft9201-static の FocalTech Linux blob (libfprint-2.so)
# から抽出し src/ft9201_fw.h を生成する。
# (OMGrant/ft9201-libfprint scripts/extract-firmware.py の macOS 移植)
#
# fw は FocalTech の所有物 → リポジトリにはコミットしない (ビルド時に取得)。
#
# Usage: extract_firmware.py <path-to-libfprint-2.so> [out.h]

import struct
import subprocess
import sys

SYM = "FOCALFP_9348_FW_APP"  # FT9348W variant (device 2808:93a9)

so = sys.argv[1] if len(sys.argv) > 1 else "blobs/deb/usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0"
out = sys.argv[2] if len(sys.argv) > 2 else "src/ft9201_fw.h"

data = open(so, "rb").read()

# 1. シンボルの vaddr + size (macOS の llvm-nm は ELF も読める)
vaddr = size = None
res = subprocess.run(["nm", "-S", so], capture_output=True, text=True)
for line in res.stdout.splitlines():
    p = line.split()
    if len(p) >= 4 and p[-1] == SYM:
        vaddr, size = int(p[0], 16), int(p[1], 16)
if vaddr is None:
    sys.exit(f"symbol {SYM} not found in {so} (wrong/updated blob?)")

# 2. vaddr → ファイルオフセット (ELF64 セクションヘッダで解決)
e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
e_shentsz = struct.unpack_from("<H", data, 0x3A)[0]
e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
foff = None
for i in range(e_shnum):
    sh = e_shoff + i * e_shentsz
    sh_addr = struct.unpack_from("<Q", data, sh + 16)[0]
    sh_off = struct.unpack_from("<Q", data, sh + 24)[0]
    sh_size = struct.unpack_from("<Q", data, sh + 32)[0]
    if sh_addr and sh_addr <= vaddr < sh_addr + sh_size:
        foff = sh_off + (vaddr - sh_addr)
        break
if foff is None:
    sys.exit("could not map symbol vaddr to a file offset")

fw = data[foff : foff + size]

with open(out, "w") as f:
    f.write("/* FT9348W MCU firmware — extracted from ft9201-static (%s).\n" % SYM)
    f.write(" * FocalTech proprietary; generated at build time, not committed.\n")
    f.write(" * (source: mrrbrilliant/ft9201-static via OMGrant/ft9201-libfprint)\n */\n")
    f.write("#ifndef FT9201_FW_H\n#define FT9201_FW_H\n\n")
    f.write("static const unsigned char ft9201_fw_data[] = {\n")
    for i in range(0, len(fw), 12):
        f.write("  " + ", ".join("0x%02x" % b for b in fw[i : i + 12]) + ",\n")
    f.write("};\n")
    f.write("static const unsigned long ft9201_fw_size = %dU;\n\n" % len(fw))
    f.write("#endif\n")

print(f"wrote {out}: {len(fw)} bytes from {SYM}")
