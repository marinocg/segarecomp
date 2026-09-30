"""SEG-009-T007 test-local reference model of the Sega PSG and its PCM contract (machine contract section 10).

Written from SMS Power! "SN76489" and the contract text, independent of libs/device/sega/psg. It is itself checked tick
for tick against the device (tests/sms_psg_differential_test.py, hermetic part) and against the pinned ares component
(oracle part), then used as the expected-value generator of the generated-native fixture test.
"""
TABLE = [32767, 26028, 20675, 16422, 13045, 10362, 8231, 6568, 5193, 4125, 3277, 2603, 2067, 1642, 1304, 0]
RATE_NUM, RATE_DEN, FULL_SCALE = 39375000, 485100, 131068


class Psg:
    def __init__(self):
        self.period = [0, 0, 0]
        self.counter = [0, 0, 0]
        self.out = [0, 0, 0]
        self.att = [15, 15, 15, 15]
        self.noise = 0
        self.noise_counter = 0
        self.flip = 0
        self.lfsr = 0x8000
        self.latch = None

    def write(self, byte):
        if byte & 0x80:
            self.latch = (byte >> 5 & 3, byte >> 4 & 1)
            low, high = byte & 15, None
        else:
            if self.latch is None:
                raise ValueError("data byte before any latch byte")
            low, high = byte & 15, byte & 0x3F
        channel, attenuation = self.latch
        if attenuation:
            self.att[channel] = byte & 15
        elif channel < 3:
            if high is None:
                self.period[channel] = (self.period[channel] & 0x3F0) | low
            else:
                self.period[channel] = (self.period[channel] & 0x00F) | (high << 4)
        else:
            self.noise = byte & 7
            self.lfsr = 0x8000

    def tick(self):
        for i in range(3):
            self.counter[i] = max(self.counter[i] - 1, 0) if self.counter[i] else 0
            if self.counter[i] == 0:
                self.counter[i] = self.period[i]
                self.out[i] ^= 1
        self.noise_counter = self.noise_counter - 1 if self.noise_counter else 0
        if self.noise_counter == 0:
            rate = self.noise & 3
            self.noise_counter = self.period[2] if rate == 3 else 0x10 << rate
            self.flip ^= 1
            if self.flip:
                feedback = ((self.lfsr & 1) ^ (self.lfsr >> 3 & 1)) if self.noise & 4 else self.lfsr & 1
                self.lfsr = self.lfsr >> 1 | feedback << 15
        return self.level()

    def level(self):
        total = sum(TABLE[self.att[i]] for i in range(3) if self.out[i])
        return total + (TABLE[self.att[3]] if self.lfsr & 1 else 0)


def sample_start(k):
    return -(-k * RATE_NUM // RATE_DEN)


def levels_for(writes, end):
    """Tick levels for `w <clock> <byte>` writes: a write is applied after floor(clock / 16) whole ticks."""
    psg, levels, w = Psg(), [], 0
    for j in range(end // 16):
        while w < len(writes) and writes[w][0] // 16 <= j:
            psg.write(writes[w][1])
            w += 1
        levels.append(psg.tick())
    return levels


def decimate(levels):
    """Output sample k covers the chip ticks whose start 16 j lies in [T_k, T_(k+1)); complete samples only."""
    samples, k = [], 0
    while sample_start(k + 1) <= 16 * len(levels):
        lo, hi = sample_start(k), sample_start(k + 1)
        chunk = levels[(lo + 15) // 16:(hi + 15) // 16]
        samples.append(sum(chunk) // len(chunk) * 65535 // FULL_SCALE - 32768)
        k += 1
    return samples


def pcm_for(writes, end):
    return decimate(levels_for(writes, end))
