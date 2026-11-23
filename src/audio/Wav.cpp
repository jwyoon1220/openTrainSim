#include "audio/Wav.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace ots {
namespace {

uint16_t read16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t read32(const uint8_t* p) {
    return uint32_t(read16(p)) | (uint32_t(read16(p + 2)) << 16);
}

int32_t pcmSample(const uint8_t* p, unsigned width) {
    switch (width) {
    case 1: return (int32_t(p[0]) - 128) * 256;
    case 2: return std::bit_cast<int16_t>(read16(p));
    case 3: {
        const int32_t value = read16(p) | (int32_t(p[2]) << 16);
        return ((value ^ 0x800000) - 0x800000) >> 8;
    }
    default: return std::bit_cast<int32_t>(read32(p)) >> 16;
    }
}

double floatSample(const uint8_t* p, unsigned width) {
    const double value = width == 4
        ? double(std::bit_cast<float>(read32(p)))
        : std::bit_cast<double>(uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32));
    return std::isnan(value) ? 0.0 : std::clamp(value, -1.0, 1.0);
}

} // namespace

bool decodeWav(const std::vector<uint8_t>& bytes, PcmMono16& out, std::string& error) {
    out = {};
    error.clear();
    const auto fail = [&](const char* message) {
        error = message;
        return false;
    };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) ||
        std::memcmp(bytes.data() + 8, "WAVE", 4))
        return fail("expected RIFF/WAVE header");

    const std::size_t end = 8 + std::min<std::size_t>(read32(bytes.data() + 4), bytes.size() - 8);
    const uint8_t* fmt = nullptr;
    const uint8_t* data = nullptr;
    std::size_t fmtSize = 0;
    std::size_t dataSize = 0;
    for (std::size_t pos = 12; pos <= end && end - pos >= 8;) {
        const uint8_t* chunk = bytes.data() + pos;
        const std::size_t declared = read32(chunk + 4);
        pos += 8;
        const std::size_t size = std::min(declared, end - pos);
        if (!data && !std::memcmp(chunk, "data", 4)) {
            data = bytes.data() + pos;
            dataSize = size;
        } else if (!fmt && !std::memcmp(chunk, "fmt ", 4)) {
            if (size != declared) return fail("truncated fmt chunk");
            fmt = bytes.data() + pos;
            fmtSize = size;
        }
        pos += size;
        if ((declared & 1) && pos < end) ++pos;
    }
    if (!fmt || fmtSize < 16) return fail("missing or short fmt chunk");
    if (!data) return fail("missing data chunk");

    unsigned format = read16(fmt);
    if (format == 0xfffe) {
        if (fmtSize < 26) return fail("missing extensible WAV sub-format");
        format = read16(fmt + 24);
    }
    const unsigned channels = read16(fmt + 2);
    const uint32_t rate = read32(fmt + 4);
    const unsigned stride = read16(fmt + 12);
    const unsigned bits = read16(fmt + 14);
    const bool floating = format == 3;
    if (floating ? (bits != 32 && bits != 64)
                 : (format != 1 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)))
        return fail("unsupported WAV sample format");

    const unsigned width = bits / 8;
    if (!channels || !rate || rate > uint32_t(std::numeric_limits<int>::max()) || stride < channels * width)
        return fail("invalid WAV channel count, sample rate or block alignment");

    out.samples.resize(dataSize / stride);
    out.sampleRate = int(rate);
    for (int16_t& sample : out.samples) {
        double average;
        if (floating) {
            double sum = 0.0;
            for (unsigned channel = 0; channel < channels; ++channel)
                sum += floatSample(data + channel * width, width);
            average = (sum / channels) * 32767.0;
        } else {
            int32_t sum = 0;
            for (unsigned channel = 0; channel < channels; ++channel)
                sum += pcmSample(data + channel * width, width);
            average = double(sum) / channels;
        }
        sample = int16_t(std::clamp(std::round(average), -32768.0, 32767.0));
        data += stride;
    }
    return true;
}

bool loadWav(const std::filesystem::path& file, PcmMono16& out, std::string& error) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    std::vector<uint8_t> bytes;
    if (size < 0 || uintmax_t(size) > bytes.max_size() ||
        size > std::numeric_limits<std::streamsize>::max()) {
        out = {};
        error = "cannot open WAV file or determine its size";
        return false;
    }
    bytes.resize(std::size_t(size));
    input.seekg(0);
    if (!input || (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)))) {
        out = {};
        error = "cannot read WAV file";
        return false;
    }
    return decodeWav(bytes, out, error);
}

} // namespace ots
