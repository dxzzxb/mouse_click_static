"""Generate a geometric mouse ICO using only the Python standard library."""
import pathlib
import struct


def image(size):
    data = bytearray()
    for y in reversed(range(size)):
        for x in range(size):
            u, v = (x + 0.5) / size, (y + 0.5) / size
            dx, dy = max(abs(u - 0.5) - 0.29, 0), max(abs(v - 0.5) - 0.29, 0)
            color = (0, 139, 132, 255) if dx * dx + dy * dy <= 0.18 ** 2 else (0, 0, 0, 0)
            if ((u - 0.5) / 0.205) ** 2 + ((v - 0.51) / 0.31) ** 2 <= 1:
                color = (250, 255, 255, 255)
                if (abs(u - 0.5) < 0.015 and v < 0.45) or abs(v - 0.45) < 0.015:
                    color = (0, 139, 132, 255)
                if abs(u - 0.5) < 0.027 and 0.29 < v < 0.39:
                    color = (0, 139, 132, 255)
            r, g, b, a = color
            data += bytes((b, g, r, a))
    mask = bytes(((size + 31) // 32) * 4 * size)
    return struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, len(data), 0, 0, 0, 0) + data + mask


sizes = (16, 32, 48, 64)
payloads = [image(size) for size in sizes]
offset = 6 + len(sizes) * 16
entries = bytearray()
for size, payload in zip(sizes, payloads):
    entries += struct.pack('<BBBBHHII', size, size, 0, 0, 1, 32, len(payload), offset)
    offset += len(payload)
path = pathlib.Path(__file__).with_name('mouse.ico')
path.write_bytes(struct.pack('<HHH', 0, 1, len(sizes)) + entries + b''.join(payloads))
print(path)

