#!/usr/bin/env python3
"""Pack a raw ARM64 Image payload into an Android boot.img compatible with lavender's ABL.
Layout is copied from the user's own stock boot image: header page, gzip(kernel)+appended DTB,
then the original AVB vbmeta blob and footer at their original offsets (64 MiB partition)."""
import sys, gzip, struct, hashlib
payload = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
hdr    = bytearray(open('header.bin', 'rb').read())
dtb    = open('dtb.bin', 'rb').read()
vbmeta = open('vbmeta.bin', 'rb').read()
footer = open('footer.bin', 'rb').read()
PAGE, VB_OFF, TOTAL = 4096, 0xD32000, 64 * 1024 * 1024
kernel = gzip.compress(payload, 9, mtime=0) + dtb
assert PAGE + len(kernel) <= VB_OFF, 'payload too large'
hdr[8:12] = struct.pack('<I', len(kernel))
sha = hashlib.sha1(kernel + struct.pack('<I', len(kernel)) + struct.pack('<II', 0, 0)).digest()
hdr[576:608] = sha + b'\0' * 12
img = bytearray(TOTAL)
img[:PAGE] = hdr
img[PAGE:PAGE + len(kernel)] = kernel
img[VB_OFF:VB_OFF + len(vbmeta)] = vbmeta
img[TOTAL - len(footer):] = footer
open(out, 'wb').write(img)
print(f'{out}: payload {len(payload)} B, kernel blob {len(kernel)} B')
