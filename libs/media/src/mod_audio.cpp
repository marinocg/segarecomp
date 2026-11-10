// Declared audio assets (SEG-145-T002; docs/architecture/modding-audio-architecture.md sections 4 and 5). The product validates the DECLARED container (canonical RIFF/WAVE PCM s16le) against the
// manifest and applies one fixed, deterministic reference conversion; it never interprets a game's native audio format.
#include "segarecomp/mod.hpp"

namespace segarecomp::mod {

namespace {
std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8U | static_cast<std::uint32_t>(b[at + 2]) << 16U | static_cast<std::uint32_t>(b[at + 3]) << 24U;
}
std::uint32_t le16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8U; }
bool tag(std::span<const std::uint8_t> b, std::size_t at, const char (&t)[5]) { return b[at] == static_cast<std::uint8_t>(t[0]) && b[at + 1] == static_cast<std::uint8_t>(t[1]) && b[at + 2] == static_cast<std::uint8_t>(t[2]) && b[at + 3] == static_cast<std::uint8_t>(t[3]); }
}  // namespace

// Canonical layout only: "RIFF" size "WAVE", "fmt " (16 bytes, PCM), "data" with nothing after it. Extra chunks, odd sizes and every other format are refused.
Wav parse_wav_pcm_s16le(std::span<const std::uint8_t> b) {
  Wav w;
  const auto bad = [&](const char *why) { w.error = why; return w; };
  if (b.size() < 44) return bad("the file is shorter than a WAV header");
  if (!tag(b, 0, "RIFF") || !tag(b, 8, "WAVE") || !tag(b, 12, "fmt ") || !tag(b, 36, "data")) return bad("the file is not canonical RIFF/WAVE");
  if (le32(b, 16) != 16 || le16(b, 20) != 1 || le16(b, 34) != 16) return bad("the file is not 16-bit PCM");
  w.channels = le16(b, 22); w.sample_rate = le32(b, 24);
  const std::uint32_t block = le16(b, 32), data_size = le32(b, 40);
  if ((w.channels != 1 && w.channels != 2) || block != w.channels * 2U || le32(b, 28) != w.sample_rate * block) return bad("the WAV format fields are inconsistent");
  if (le32(b, 4) != b.size() - 8 || data_size != b.size() - 44 || data_size % block != 0 || data_size == 0) return bad("the WAV sizes are inconsistent");
  w.frames = data_size / block;
  w.data = b.subspan(44);
  w.ok = true;
  return w;
}

std::vector<std::uint8_t> convert_pcm_u8_mono(const Wav &wav) {
  std::vector<std::uint8_t> out;
  if (!wav.ok) return out;
  out.reserve(wav.frames);
  for (unsigned f = 0; f < wav.frames; ++f) {
    int sum = 0;
    for (unsigned c = 0; c < wav.channels; ++c) {
      const std::size_t at = (static_cast<std::size_t>(f) * wav.channels + c) * 2U;
      sum += static_cast<std::int16_t>(static_cast<std::uint16_t>(wav.data[at] | (wav.data[at + 1] << 8U)));
    }
    const int mono = wav.channels == 2 ? (sum >= 0 ? sum / 2 : -((-sum + 1) / 2)) : sum;   // floor(sum / 2): rounds toward negative infinity
    out.push_back(static_cast<std::uint8_t>((mono + 32768) >> 8));
  }
  return out;
}

}  // namespace segarecomp::mod
