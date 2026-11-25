#include "render/TextureCache.hpp"

#include "phx/core/Log.hpp"
#include "phx/gpu/VulkanContext.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace ots {

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

TextureId TextureCache::acquire(const std::string& path, bool colorKey, u8 r, u8 g, u8 b, JobPriority priority) {
    const std::string key = colorKey ? std::format("{}|{},{},{}", path, r, g, b) : path;
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
    e.refs = 1;
    e.state = State::Decoding;
    e.priority = priority;
    index_.emplace(key, id);
    ++pendingCount_;
    const TextureSettings s = settings_;
    e.job = pool_.submit([this, id, gen, path, colorKey, r, g, b, s] {
        Done d{id, gen, false, {}, 0};
        int w = 0, h = 0, n = 0;
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
    std::ranges::stable_sort(waiting_, {}, [&](const Done& d) { return entries_[d.id].priority; });
    std::size_t keep = 0;
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        Done& d = waiting_[i];
        Entry& e = entries_[d.id];
        if (e.generation != d.generation || e.state != State::Decoding) continue; // stale
        if (!d.ok) {
            phx::log::warn("Texture unreadable: {}", e.path);
            e.state = State::Failed;
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
