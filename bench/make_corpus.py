#!/usr/bin/env python3
"""Build a mixed-type corpus for benchmarking.

Every file is generated deterministically from a fixed seed so that runs are
reproducible and nothing is tuned against data the compressor has seen.
"""
import os, random, struct, sys, zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/corpus"
os.makedirs(OUT, exist_ok=True)


def w(name, data):
    p = os.path.join(OUT, name)
    with open(p, "wb") as f:
        f.write(data)
    print(f"  {name:22s} {len(data):>10,} bytes")
    return p


def text(n, seed=1):
    r = random.Random(seed)
    words = ("the of and to in a is that for it as was with be by on not this are or "
             "from at which have has had were been their said each she do how если "
             "compression entropy model context probability arithmetic coder mixing "
             "predict adaptive stream buffer offset length literal match window").split()
    out = []
    total = 0
    while total < n:
        s = " ".join(r.choice(words) for _ in range(r.randint(6, 18)))
        s = s[0].upper() + s[1:] + r.choice([".", ".", ".", "?", "!"]) + "\n"
        b = s.encode()
        out.append(b)
        total += len(b)
    return b"".join(out)[:n]


def source_code(n, seed=2):
    r = random.Random(seed)
    tmpl = [
        "static int {f}_{i}(const uint8_t *p, size_t n)\n{{\n"
        "    size_t i;\n    int acc = {v};\n"
        "    for (i = 0; i < n; ++i) acc += p[i] * {m};\n"
        "    return acc & 0x{k:04x};\n}}\n\n",
        "void {f}_{i}(struct ctx *c)\n{{\n"
        "    if (!c || c->len < {v}) return;\n"
        "    c->pos += {m};\n    c->flags |= 0x{k:x};\n}}\n\n",
        "/* {f} {i}: helper for the {f} path */\n"
        "size_t {f}_{i}(char *dst, size_t cap)\n{{\n"
        "    return snprintf(dst, cap, \"%d/%d\", {v}, {m});\n}}\n\n",
    ]
    names = ["parse", "emit", "hash", "scan", "flush", "encode", "decode", "probe"]
    out = []
    total = 0
    i = 0
    while total < n:
        t = r.choice(tmpl)
        s = t.format(f=r.choice(names), i=i, v=r.randint(0, 999),
                     m=r.randint(1, 64), k=r.randint(0, 65535))
        out.append(s.encode())
        total += len(s)
        i += 1
    return b"".join(out)[:n]


def csv_records(n, seed=3):
    r = random.Random(seed)
    out = [b"id,user,region,status,amount,ts\n"]
    total = len(out[0])
    i = 100000
    regions = ["emea", "apac", "amer", "latam"]
    while total < n:
        line = "%d,user_%05d,%s,%s,%.2f,2026-%02d-%02dT%02d:%02d:%02dZ\n" % (
            i, r.randint(1, 20000), r.choice(regions),
            r.choice(["active", "closed", "pending"]),
            r.randint(0, 500000) / 100.0,
            r.randint(1, 12), r.randint(1, 28),
            r.randint(0, 23), r.randint(0, 59), r.randint(0, 59))
        b = line.encode()
        out.append(b)
        total += len(b)
        i += 1
    return b"".join(out)[:n]


def json_log(n, seed=4):
    r = random.Random(seed)
    out = []
    total = 0
    lvl = ["INFO", "WARN", "ERROR", "DEBUG"]
    msg = ["connection established", "cache miss", "retry scheduled",
           "request completed", "token refreshed", "queue drained"]
    while total < n:
        s = ('{"ts":"2026-07-%02dT%02d:%02d:%02d.%03dZ","level":"%s",'
             '"svc":"api-%d","trace":"%032x","msg":"%s","ms":%d}\n') % (
            r.randint(1, 28), r.randint(0, 23), r.randint(0, 59), r.randint(0, 59),
            r.randint(0, 999), r.choice(lvl), r.randint(1, 8),
            r.getrandbits(128), r.choice(msg), r.randint(0, 5000))
        b = s.encode()
        out.append(b)
        total += len(b)
    return b"".join(out)[:n]


def audio_pcm(n, seed=5):
    """16-bit stereo PCM: two correlated smooth channels + noise floor."""
    r = random.Random(seed)
    frames = n // 4
    buf = bytearray()
    import math
    ph1 = ph2 = 0.0
    f1, f2 = 0.004, 0.0031
    for k in range(frames):
        if k % 4096 == 0:
            f1 = 0.001 + r.random() * 0.01
            f2 = 0.001 + r.random() * 0.01
        ph1 += f1
        ph2 += f2
        l = int(9000 * math.sin(ph1) + 3000 * math.sin(ph1 * 2.7) + r.randint(-60, 60))
        rr = int(8500 * math.sin(ph2) + 2600 * math.sin(ph2 * 3.1) + r.randint(-60, 60))
        buf += struct.pack("<hh", max(-32768, min(32767, l)),
                           max(-32768, min(32767, rr)))
    return bytes(buf[:n])


def image_gray(n, seed=6):
    """Raw 8-bit grayscale: smooth gradients, shapes, a little grain."""
    import math
    r = random.Random(seed)
    wid = 1024
    hgt = max(1, n // wid)
    buf = bytearray(wid * hgt)
    cx, cy = wid * 0.4, hgt * 0.55
    for y in range(hgt):
        row = y * wid
        for x in range(wid):
            v = 128 + 90 * math.sin(x * 0.006) * math.cos(y * 0.008)
            d = math.hypot(x - cx, y - cy)
            if d < 220:
                v = v * 0.35 + 190 * (1 - d / 220)
            v += r.randint(-3, 3)
            buf[row + x] = max(0, min(255, int(v)))
    return bytes(buf[:n])


def binary_exe(n, seed=7):
    """Synthetic x86-64: real opcode mix with relative calls to few targets."""
    r = random.Random(seed)
    out = bytearray()
    funcs = [r.randint(0, n - 1) & ~0xF for _ in range(220)]
    while len(out) < n:
        k = r.random()
        pos = len(out)
        if k < 0.13:
            tgt = r.choice(funcs)
            rel = (tgt - (pos + 5)) & 0xFFFFFFFF
            out += b"\xe8" + struct.pack("<I", rel)
        elif k < 0.18:
            tgt = r.choice(funcs)
            rel = (tgt - (pos + 5)) & 0xFFFFFFFF
            out += b"\xe9" + struct.pack("<I", rel)
        elif k < 0.34:
            out += bytes([0x48, 0x89, 0xC0 + r.randint(0, 63)])
        elif k < 0.46:
            out += bytes([0x48, 0x8B, 0x40 + r.randint(0, 7), r.randint(0, 255)])
        elif k < 0.56:
            out += bytes([0x0F, 0x84 + r.randint(0, 5)]) + struct.pack("<i", r.randint(-4096, 4096))
        elif k < 0.66:
            out += bytes([0x83, 0xC0 + r.randint(0, 7), r.randint(0, 127)])
        elif k < 0.74:
            out += b"\x55\x48\x89\xe5"
        elif k < 0.80:
            out += b"\x5d\xc3"
        elif k < 0.86:
            out += bytes([0xB8 + r.randint(0, 7)]) + struct.pack("<I", r.randint(0, 1 << 20))
        elif k < 0.92:
            out += bytes([0x66, 0x90]) * r.randint(1, 3)
        else:
            out += bytes(r.getrandbits(8) for _ in range(r.randint(1, 6)))
    return bytes(out[:n])


def floats(n, seed=8):
    """Float64 time series: the classic 'delta helps, generic LZ does not' case."""
    import math
    r = random.Random(seed)
    cnt = n // 8
    buf = bytearray()
    v = 100.0
    for i in range(cnt):
        v += r.gauss(0, 0.4) + 0.02 * math.sin(i * 0.01)
        buf += struct.pack("<d", v)
    return bytes(buf[:n])


def db_pages(n, seed=9):
    """Fixed-width pages with headers, padding and repeated dictionary values."""
    r = random.Random(seed)
    page = 4096
    vals = [("prod_%04d" % r.randint(0, 400)).encode() for _ in range(64)]
    out = bytearray()
    pno = 0
    while len(out) < n:
        p = bytearray(page)
        p[0:8] = struct.pack("<II", 0xDEADBEEF, pno)
        off = 32
        while off < page - 64:
            rec = struct.pack("<IIH", r.randint(0, 1 << 24), pno, r.randint(0, 300))
            v = r.choice(vals)
            rec += bytes([len(v)]) + v
            rec += b"\x00" * ((8 - len(rec) % 8) % 8)
            if off + len(rec) > page - 8:
                break
            p[off:off + len(rec)] = rec
            off += len(rec)
        out += p
        pno += 1
    return bytes(out[:n])


def already_compressed(n, seed=10):
    """A zlib stream: high entropy, nothing left to find. Guards against
    any pipeline that only ever grows the output."""
    r = random.Random(seed)
    raw = text(n * 3, seed)
    return zlib.compress(raw, 9)[:n]


def mixed_archive(n, seed=11):
    """Tar-like concatenation of everything above: the realistic case."""
    parts = [
        text(n // 7, seed), source_code(n // 7, seed), csv_records(n // 7, seed),
        json_log(n // 7, seed), binary_exe(n // 7, seed), audio_pcm(n // 7, seed),
        db_pages(n // 7, seed),
    ]
    out = bytearray()
    for i, p in enumerate(parts):
        hdr = ("FILE%03d" % i).encode().ljust(32, b"\x00") + struct.pack("<Q", len(p))
        out += hdr + p
        out += b"\x00" * ((512 - len(out) % 512) % 512)
    return bytes(out[:n])


SIZE = 4 * 1024 * 1024
print(f"building corpus in {OUT} ({SIZE // 1024 // 1024} MiB per file)")
w("text.txt", text(SIZE))
w("source.c", source_code(SIZE))
w("records.csv", csv_records(SIZE))
w("logs.json", json_log(SIZE))
w("audio.pcm", audio_pcm(SIZE))
w("image.gray", image_gray(SIZE))
w("binary.exe", binary_exe(SIZE))
w("series.f64", floats(SIZE))
w("db.pages", db_pages(SIZE))
w("precompressed.z", already_compressed(SIZE))
w("archive.mixed", mixed_archive(SIZE))
print("done")
