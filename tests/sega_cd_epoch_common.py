"""Project-authored helpers for the SEG-079 epoch-recipe tests: a Kosinski encoder (the public LZ format; stdlib only) and
synthetic writer-log builders. No commercial byte is read or reproduced."""
import struct

KIND_PRG, KIND_WORK, KIND_ROM = 1, 2, 3


def kosinski_encode(data, short=True, long=True, literal_only=False):
    """Greedy Kosinski encoder. Bit stream: 16-bit little-endian words, LSB first; each operation's data bytes are consumed after its
    last bit, and the next description word is fetched the moment a word's 16th bit has been consumed."""
    ops = []  # (bit list, data bytes)
    p = 0
    while p < len(data):
        best = (0, 0)
        if not literal_only:
            for back in range(1, min(p, 8192) + 1):
                n = 0
                while p + n < len(data) and n < 256 and data[p + n] == data[p - back + n]:
                    n += 1
                if n > best[0]:
                    best = (n, back)
        n, back = best
        if short and 2 <= n and back <= 256 and n <= 5:
            c = n - 2
            ops.append(([0, 0, (c >> 1) & 1, c & 1], bytes([(256 - back) & 0xFF])))
            p += n
        elif long and n >= 3:
            v = (-back) & 0x1FFF
            lo, hi5 = v & 0xFF, v >> 8
            if n <= 9:
                ops.append(([0, 1], bytes([lo, (hi5 << 3) | (n - 2)])))
            else:
                ops.append(([0, 1], bytes([lo, hi5 << 3, n - 1])))
            p += n
        else:
            ops.append(([1], bytes([data[p]])))
            p += 1
    ops.append(([0, 1], bytes([0xF0, 0xF0 & 0xF8, 0])))  # end marker: long copy, count 0, extra 0
    # lay the stream out exactly as the decoder consumes it
    out = bytearray()
    words = []          # (position in out, bits)
    cur = {}

    def new_word():
        cur.clear()
        cur.update({"pos": len(out), "bits": []})
        out.extend(b"\0\0")
        words.append(dict(cur))
        words[-1]["bits"] = cur["bits"]

    new_word()
    word_bits = 0
    for bits, payload in ops:
        for i, b in enumerate(bits):
            cur["bits"].append(b)
            word_bits += 1
            last = i == len(bits) - 1
            if word_bits == 16:
                new_word()
                word_bits = 0
            if last:
                out.extend(payload)
    for w in words:
        value = sum(b << i for i, b in enumerate(w["bits"]))
        struct.pack_into("<H", out, w["pos"], value)
    return bytes(out)


def synthetic_image(length=4096, seed=1):
    """A compressible, project-authored byte pattern with runs, repeats and literals."""
    state = seed & 0xFFFFFFFF
    out = bytearray()
    while len(out) < length:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        kind = (state >> 16) % 4
        if kind == 0:
            out += bytes([(state >> 8) & 0xFF]) * (2 + (state >> 20) % 20)
        elif kind == 1 and len(out) > 8:
            start = (state >> 10) % (len(out) - 4)
            out += out[start:start + 3 + (state >> 20) % 12]
        else:
            out += bytes((state >> (8 * i)) & 0xFF for i in range(1 + (state >> 20) % 6))
    return bytes(out[:length])


def log_record(kind, width, cpu, offset, value, pc):
    return struct.pack("<BBBBIII", kind, width, cpu, 0, offset, value, pc)
