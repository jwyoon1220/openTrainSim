#include "render/TextureCache.hpp"

#include "phx/core/Log.hpp"
#include "util/Platform.hpp"
#include "phx/gpu/VulkanContext.hpp"

#include <stb_image.h>
#include <tinyexr.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

namespace ots {

namespace {

// ---------------------------------------------------------------- on-disk texture cache
// One file per (source file, size, mtime, colour key, settings). The source's size and mtime
// are part of the key, so editing a texture simply misses; stale files are never read.
constexpr char kTexMagic[8] = {'O', 'T', 'S', 'T', 'E', 'X', '0', '1'};

u64 fnv1a(const void* data, std::size_t n, u64 h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

std::filesystem::path textureCacheFile(const std::string& path, bool colorKey, u8 r, u8 g, u8 b, const TextureSettings& s, f32 hdrScale = 0.0f) {
    std::error_code ec;
    const std::filesystem::path src(path);
    const auto size = std::filesystem::file_size(src, ec);
    if (ec) return {};
    const auto mtime = std::filesystem::last_write_time(src, ec).time_since_epoch().count();
    if (ec) return {};
    u64 h = fnv1a(path.data(), path.size());
    const u64 sz = u64(size);
    const i64 mt = i64(mtime);
    h = fnv1a(&sz, sizeof(sz), h);
    h = fnv1a(&mt, sizeof(mt), h);
    const u8 key[8] = {u8(colorKey), r, g, b, u8(s.compress), u8(s.mipmaps), 0, 0};
    h = fnv1a(key, sizeof(key), h);
    h = fnv1a(&s.maxSize, sizeof(s.maxSize), h);
    if (hdrScale != 0.0f) h = fnv1a(&hdrScale, sizeof(hdrScale), h);
    static const std::filesystem::path dir = userCacheDir() / "textures" / "v1";
    return dir / std::format("{:02x}", u32(h & 0xFF)) / std::format("{:016x}.tex", h);
}

struct TexHeader {
    char magic[8];
    u32 format, width, height, levels;
    u64 dataBytes, raw;
    u8 hasAlpha, translucent, pad[6];
};

bool loadCachedTexture(const std::filesystem::path& file, phx::tex::PreparedTexture& tex, std::size_t& raw) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    TexHeader h{};
    if (!f.read(reinterpret_cast<char*>(&h), sizeof(h)) || std::memcmp(h.magic, kTexMagic, 8) != 0) return false;
    if (h.levels > 32 || h.dataBytes > (u64(1) << 31)) return false;
    tex.format = VkFormat(h.format);
    tex.width = h.width;
    tex.height = h.height;
    tex.levels.resize(h.levels);
    tex.data.resize(std::size_t(h.dataBytes));
    if (!f.read(reinterpret_cast<char*>(tex.levels.data()), std::streamsize(h.levels * sizeof(phx::tex::MipLevel)))) return false;
    if (!f.read(reinterpret_cast<char*>(tex.data.data()), std::streamsize(h.dataBytes))) return false;
    for (const phx::tex::MipLevel& l : tex.levels)
        if (u64(l.offset) + l.size > h.dataBytes) return false;
    tex.hasAlpha = h.hasAlpha;
    tex.translucent = h.translucent;
    raw = std::size_t(h.raw);
    return true;
}

void storeCachedTexture(const std::filesystem::path& file, const phx::tex::PreparedTexture& tex, std::size_t raw) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // write to a private temporary, then rename: readers never see a partial file
    const std::filesystem::path tmp = file.string() + std::format(".{:x}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        TexHeader h{};
        std::memcpy(h.magic, kTexMagic, 8);
        h.format = u32(tex.format);
        h.width = tex.width;
        h.height = tex.height;
        h.levels = u32(tex.levels.size());
        h.dataBytes = tex.data.size();
        h.raw = raw;
        h.hasAlpha = tex.hasAlpha;
        h.translucent = tex.translucent;
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        f.write(reinterpret_cast<const char*>(tex.levels.data()), std::streamsize(tex.levels.size() * sizeof(phx::tex::MipLevel)));
        f.write(reinterpret_cast<const char*>(tex.data.data()), std::streamsize(tex.data.size()));
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

} // namespace

TextureCache::TextureCache(phx::VulkanContext& ctx, phx::Bindless& bindless, phx::UploadQueue& upload, ThreadPool& pool, TextureSettings s)
    : ctx_(ctx), bindless_(bindless), upload_(upload), pool_(pool), settings_(s) {}

TextureCache::~TextureCache() {
    for (Entry& e : entries_) destroy(e);
}

void TextureCache::destroy(Entry& e) {
    if (e.slotRepeat) bindless_.freeSampled(e.slotRepeat);
    if (e.slotClamp) bindless_.freeSampled(e.slotClamp);
    e.slotRepeat = e.slotClamp = 0;
    if (e.image.image) {
        residentBytes_ -= e.bytes;
        rawBytes_ -= e.raw;
        --residentCount_;
        phx::destroyImage(ctx_, e.image); // caller guarantees the GPU is done with it
    }
    e.state = State::Free;
}

TextureId TextureCache::acquireRaw(const std::string& path, u32 maxSize, f32 hdrScale, JobPriority priority) {
    const std::string key = std::format("{}|raw{},{}", path, maxSize, hdrScale);
    if (auto it = index_.find(key); it != index_.end()) {
        ++entries_[it->second].refs;
        prioritize(it->second, priority);
        return it->second;
    }
    // reuse acquire()'s bookkeeping, then mark the entry raw before its job runs
    pendingRaw_ = {maxSize, hdrScale, key};
    const TextureId id = acquire(path, false, 0, 0, 0, priority);
    pendingRaw_ = {};
    return id;
}

TextureId TextureCache::acquire(const std::string& path, bool colorKey, u8 r, u8 g, u8 b, JobPriority priority) {
    const std::string key = pendingRaw_.maxSize ? pendingRaw_.key : colorKey ? std::format("{}|{},{},{}", path, r, g, b) : path;
    auto it = index_.find(key);
    if (it != index_.end()) {
        ++entries_[it->second].refs;
        prioritize(it->second, priority);
        return it->second;
    }
    TextureId id;
    if (!free_.empty()) {
        id = free_.back();
        free_.pop_back();
    } else {
        id = TextureId(entries_.size());
        entries_.emplace_back();
    }
    Entry& e = entries_[id];
    const u32 gen = e.generation + 1;
    e = Entry{};
    e.generation = gen;
    e.key = key;
    e.path = path;
    e.colorKey = colorKey;
    e.r = r, e.g = g, e.b = b;
    e.rawMaxSize = pendingRaw_.maxSize;
    e.hdrScale = pendingRaw_.hdrScale;
    e.refs = 1;
    e.state = State::Decoding;
    e.priority = priority;
    ++version_;
    index_.emplace(key, id);
    ++pendingCount_;
    TextureSettings s = settings_;
    const bool raw = e.rawMaxSize > 0;
    const f32 hdrScale = e.hdrScale;
    if (raw) {
        s.maxSize = e.rawMaxSize;
        s.compress = false;
        s.mipmaps = true;
    }
    e.job = pool_.submit([this, id, gen, path, colorKey, r, g, b, s, raw, hdrScale] {
        Done d{id, gen, false, {}, 0};
        const std::filesystem::path cacheFile =
            s.diskCache ? textureCacheFile(path, colorKey, r, g, b, s, raw ? hdrScale : 0.0f) : std::filesystem::path{};
        if (!cacheFile.empty() && loadCachedTexture(cacheFile, d.tex, d.raw)) {
            d.ok = true;
            std::lock_guard lock(doneMutex_);
            done_.push_back(std::move(d));
            return;
        }
        int w = 0, h = 0, n = 0;
        if (raw && path.size() > 4 && (path.ends_with(".exr") || path.ends_with(".EXR"))) {
            float* hdr = nullptr;
            const char* err = nullptr;
            if (LoadEXR(&hdr, &w, &h, path.c_str(), &err) == TINYEXR_SUCCESS && w > 0 && h > 0) {
                const std::size_t count = std::size_t(w) * std::size_t(h);
                std::vector<u8> rgba(count * 4);
                const float inv = 1.0f / hdrScale;
                for (std::size_t i = 0; i < count; ++i) {
                    for (int c = 0; c < 3; ++c) {
                        const float v = std::clamp(hdr[i * 4 + c] * inv, 0.0f, 1.0f);
                        rgba[i * 4 + c] = u8(std::lround(std::sqrt(std::sqrt(v)) * 255.0f));
                    }
                    rgba[i * 4 + 3] = 255;
                }
                std::free(hdr);
                phx::tex::PrepareOptions opt;
                opt.maxSize = s.maxSize;
                opt.compression = phx::tex::Compression::None;
                opt.mipmaps = true;
                d.raw = count * 4 * 4 / 3;
                d.tex = phx::tex::prepare(rgba, u32(w), u32(h), opt);
                d.ok = true;
                if (!cacheFile.empty()) storeCachedTexture(cacheFile, d.tex, d.raw);
            } else if (err) {
                phx::log::warn("EXR {}: {}", path, err);
                FreeEXRErrorMessage(err);
            }
            std::lock_guard lock(doneMutex_);
            done_.push_back(std::move(d));
            return;
        }
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (px && w > 0 && h > 0) {
            std::vector<u8> rgba(px, px + std::size_t(w) * std::size_t(h) * 4);
            stbi_image_free(px);
            if (colorKey) {
                for (std::size_t i = 0; i < rgba.size(); i += 4)
                    if (rgba[i] == r && rgba[i + 1] == g && rgba[i + 2] == b) rgba[i + 3] = 0;
            }
            bool hasAlpha, translucent;
            phx::tex::analyseAlpha(rgba.data(), u32(w), u32(h), hasAlpha, translucent);
            phx::tex::PrepareOptions opt;
            opt.maxSize = s.maxSize;
            opt.compression = s.compress ? phx::tex::Compression::BC : phx::tex::Compression::None;
            opt.mipmaps = s.mipmaps;
            opt.alphaTest = hasAlpha && !translucent;
            d.raw = std::size_t(w) * std::size_t(h) * 4 * 4 / 3;
            d.tex = phx::tex::prepare(rgba, u32(w), u32(h), opt);
            d.ok = true;
            if (!cacheFile.empty()) storeCachedTexture(cacheFile, d.tex, d.raw);
        } else if (px) {
            stbi_image_free(px);
        }
        std::lock_guard lock(doneMutex_);
        done_.push_back(std::move(d));
    }, priority);
    return id;
}

void TextureCache::prioritize(TextureId id, JobPriority priority) {
    if (id == kNone || id >= entries_.size()) return;
    Entry& e = entries_[id];
    if (e.state != State::Decoding || !(priority < e.priority)) return;
    e.priority = priority;
    pool_.raise(e.job, priority);
}

void TextureCache::release(TextureId id) {
    if (id == kNone || id >= entries_.size()) return;
    Entry& e = entries_[id];
    if (e.refs == 0) return;
    if (--e.refs == 0) zeroRef_.push_back(id);
}

void TextureCache::update() {
    {
        std::lock_guard lock(doneMutex_);
        for (Done& d : done_) waiting_.push_back(std::move(d));
        done_.clear();
    }
    // most urgent first: the upload budget goes to what is visible
    const auto texPriority = [&](const Done& d) { return entries_[d.id].priority; };
    if (!std::ranges::is_sorted(waiting_, {}, texPriority)) std::ranges::stable_sort(waiting_, {}, texPriority);
    std::size_t keep = 0;
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        Done& d = waiting_[i];
        Entry& e = entries_[d.id];
        if (e.generation != d.generation || e.state != State::Decoding) continue; // stale
        if (!d.ok) {
            phx::log::warn("Texture unreadable: {}", e.path);
            e.state = State::Failed;
            ++version_;
            e.job = {};
            --pendingCount_;
            continue;
        }
        if (!upload_.canFit(d.tex.data.size())) {
            waiting_[keep++] = std::move(d);
            continue;
        }
        e.image = phx::createTextureImage(ctx_, d.tex);
        if (!upload_.uploadImage(e.image.image, d.tex)) {
            phx::destroyImage(ctx_, e.image);
            waiting_[keep++] = std::move(d);
            continue;
        }
        e.hasAlpha = d.tex.hasAlpha;
        e.translucent = d.tex.translucent;
        e.bytes = d.tex.data.size();
        e.raw = d.raw;
        e.state = State::Resident;
        ++version_;
        e.job = {};
        residentBytes_ += e.bytes;
        rawBytes_ += e.raw;
        ++residentCount_;
        --pendingCount_;
    }
    waiting_.resize(keep);

    // Unreferenced textures: slots go back to Bindless (which delays their reuse) and the
    // image is destroyed once the frames in flight can no longer sample it.
    for (TextureId id : zeroRef_) {
        Entry& e = entries_[id];
        if (e.refs != 0 || e.state == State::Free) continue; // re-acquired meanwhile
        if (e.state == State::Decoding) {
            --pendingCount_; // its result will be stale
            pool_.cancel(e.job);
        }
        e.job = {};
        index_.erase(e.key);
        if (e.slotRepeat) bindless_.freeSampled(e.slotRepeat);
        if (e.slotClamp) bindless_.freeSampled(e.slotClamp);
        e.slotRepeat = e.slotClamp = 0;
        if (e.image.image) zombies_.push_back({e.image, 4u, e.bytes, e.raw});
        e.image = {};
        ++e.generation;
        e.state = State::Free;
        ++version_;
        free_.push_back(id);
    }
    zeroRef_.clear();
    for (std::size_t i = 0; i < zombies_.size();) {
        if (zombies_[i].frames-- == 0) {
            residentBytes_ -= zombies_[i].bytes;
            rawBytes_ -= zombies_[i].raw;
            --residentCount_;
            phx::destroyImage(ctx_, zombies_[i].image);
            zombies_[i] = zombies_.back();
            zombies_.pop_back();
        } else {
            ++i;
        }
    }
}

std::size_t TextureCache::pendingCount(JobPriority upTo) const {
    return std::size_t(std::ranges::count_if(entries_, [&](const Entry& e) { return e.state == State::Decoding && e.priority <= upTo; }));
}

bool TextureCache::settled(TextureId id) const {
    if (id == kNone) return true;
    const State s = entries_[id].state;
    return s == State::Resident || s == State::Failed;
}

u32 TextureCache::slot(TextureId id, bool clamp) {
    if (id == kNone) return 0;
    Entry& e = entries_[id];
    if (e.state != State::Resident) return 0;
    u32& s = clamp ? e.slotClamp : e.slotRepeat;
    if (!s) {
        s = bindless_.allocateSampled();
        if (clamp) bindless_.setMaterialClamped(s, e.image.view);
        else bindless_.setMaterial(s, e.image.view);
    }
    return s;
}

// ------------------------------------------------------------------ materials

MaterialId MaterialTable::acquire(const Material& m, JobPriority priority) {
    const std::string key = std::format("{}|{:08x}|{:06x}|{}{}{}{}|{}|{}|{}", m.texture, m.color, m.emissive, int(m.colorKey), int(m.keyR),
                                        int(m.keyG), int(m.keyB), int(m.blend), int(m.wrap), m.glowHalfDistance);
    auto it = index_.find(key);
    if (it != index_.end()) {
        ++entries_[it->second].refs;
        textures_.prioritize(entries_[it->second].texture, priority);
        return it->second;
    }
    MaterialId id;
    if (!free_.empty()) {
        id = free_.back();
        free_.pop_back();
    } else {
        id = MaterialId(entries_.size());
        entries_.emplace_back();
    }
    Entry& e = entries_[id];
    ++version_;
    e.m = m;
    e.refs = 1;
    e.texture = m.texture.empty() ? kNone : textures_.acquire(m.texture, m.colorKey, m.keyR, m.keyG, m.keyB, priority);
    index_.emplace(key, id);
    return id;
}

void MaterialTable::release(MaterialId id) {
    if (id == kNone || id >= entries_.size()) return;
    Entry& e = entries_[id];
    if (e.refs == 0 || --e.refs != 0) return;
    ++version_;
    textures_.release(e.texture);
    e.texture = kNone;
    const Material& m = e.m;
    index_.erase(std::format("{}|{:08x}|{:06x}|{}{}{}{}|{}|{}|{}", m.texture, m.color, m.emissive, int(m.colorKey), int(m.keyR), int(m.keyG),
                             int(m.keyB), int(m.blend), int(m.wrap), m.glowHalfDistance));
    free_.push_back(id);
}

bool MaterialTable::ready(MaterialId id) const { return textures_.settled(entries_[id].texture); }

AlphaMode MaterialTable::mode(MaterialId id) const {
    const Entry& e = entries_[id];
    if (e.m.blend == MaterialBlend::Additive) return AlphaMode::Additive;
    if ((e.m.color >> 24) < 255) return AlphaMode::Blend;
    if (e.texture != kNone && textures_.settled(e.texture)) {
        if (textures_.translucent(e.texture)) return AlphaMode::Blend;
        if (textures_.hasAlpha(e.texture)) return AlphaMode::AlphaTest;
    }
    return AlphaMode::Opaque;
}

u32 MaterialTable::write(GpuMaterial* dst, u32 capacity) {
    const u32 n = std::min<u32>(capacity, u32(entries_.size()));
    for (u32 i = 0; i < n; ++i) {
        const Entry& e = entries_[i];
        GpuMaterial g{0, e.m.color, e.m.emissive, 0};
        if (e.refs && e.texture != kNone) {
            g.texture = textures_.slot(e.texture, e.m.wrap == WrapMode::Clamp);
            if (g.texture) g.flags |= kMatTextured;
        }
        switch (e.refs ? mode(i) : AlphaMode::Opaque) {
        case AlphaMode::AlphaTest: g.flags |= kMatAlphaTest; break;
        case AlphaMode::Blend: g.flags |= kMatBlend; break;
        case AlphaMode::Additive: g.flags |= kMatAdditive | kMatBlend; break;
        default: break;
        }
        g.flags |= u32(e.m.glowHalfDistance) << kMatGlowShift;
        dst[i] = g;
    }
    return n;
}

} // namespace ots
