from pathlib import Path
import struct, zlib, sys

out = Path(sys.argv[1])
w = h = 512
raw = bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w):
        # simple blue gradient background
        r = 18 + (x * 20 // w)
        g = 70 + (y * 50 // h)
        b = 150 + (x * 60 // w)
        # centered download arrow
        if 218 <= x <= 294 and 120 <= y <= 300:
            r,g,b = 245,245,250
        if 170 <= y <= 300 and abs(x-256) <= (y-170)//2:
            r,g,b = 245,245,250
        if 175 <= x <= 337 and 330 <= y <= 370:
            r,g,b = 245,245,250
        raw += bytes((r,g,b,255))

def chunk(t, data):
    return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t+data) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', w,h,8,6,0,0,0))
png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
png += chunk(b'IEND', b'')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(png)
