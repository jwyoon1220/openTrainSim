#pragma once
// RIFF/WAVE decoder for train and route sounds. Everything is converted to 16-bit signed mono
// (stereo / multichannel is averaged) so OpenAL can spatialise it.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ots {

struct PcmMono16 {
    std::vector<int16_t> samples;
    int sampleRate = 0;
    [[nodiscard]] double duration() const { return sampleRate > 0 ? double(samples.size()) / double(sampleRate) : 0.0; }
};

// Supports WAVE_FORMAT_PCM (8 bit unsigned, 16 / 24 / 32 bit signed), WAVE_FORMAT_IEEE_FLOAT
// (32 / 64 bit) and WAVE_FORMAT_EXTENSIBLE with those sub-formats; any channel count.
// Tolerates odd-sized chunks (pad byte), unknown chunks, a data chunk whose declared size runs
// past the end of the file (truncate), and a missing/short fmt extension. Returns false with
// `error` set for anything else (ADPCM, mp3 in WAV, ...).
bool decodeWav(const std::vector<uint8_t>& bytes, PcmMono16& out, std::string& error);
bool loadWav(const std::filesystem::path& file, PcmMono16& out, std::string& error);

} // namespace ots
