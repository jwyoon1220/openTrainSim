#pragma once
// Textures and materials with reference counting.
//  * TextureCache: one GPU image per (file, colour key), decoded + mip-mapped + BC-compressed on
//    worker threads, uploaded within the frame budget, bindless slots allocated per sampler
//    (repeat / clamp) only when used, freed when no material references it any more.
//  * MaterialTable: identical materials across all objects share one entry (and one GPU
//    record in the per-frame material table).
#include "bve/Object.hpp"
#include "phx/gpu/Upload.hpp"
#include "render/GpuTypes.hpp"
#include "util/ThreadPool.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ots {

using TextureId = u32;
using MaterialId = u32;
inline constexpr u32 kNone = ~0u;

struct TextureSettings {
    u32 maxSize = 2048;
    bool compress = true;
    bool mipmaps = true;
    // Keep decoded, mip-mapped, block-compressed results on disk (like a page cache): later
    // runs skip decoding and compression entirely.
    bool diskCache = true;
};

class TextureCache {
public:
    TextureCache(phx::VulkanContext& ctx, phx::Bindless& bindless, phx::UploadQueue& upload, ThreadPool& pool, TextureSettings settings);
    ~TextureCache();

    // Decodes on a worker at `priority`; an existing texture still decoding is raised to it.
    TextureId acquire(const std::string& path, bool colorKey, u8 r, u8 g, u8 b, JobPriority priority = JobPriority::View);
    // Uncompressed, full-size image (sky maps): no block compression, `maxSize` instead of the
    // global limit. OpenEXR files are encoded as (value / hdrScale)^(1/4) in RGBA8 so faint
    // stars keep their range; decode with pow(c, 4) * hdrScale.
    TextureId acquireRaw(const std::string& path, u32 maxSize, f32 hdrScale, JobPriority priority = JobPriority::Near);
    void release(TextureId id);
    // Raises a texture still decoding (or waiting for upload) to `priority`.
    void prioritize(TextureId id, JobPriority priority);
    // Uploads finished decodes (within the upload budget), frees unreferenced textures.
    void update();
    [[nodiscard]] bool settled(TextureId id) const; // resident or failed
    [[nodiscard]] u32 slot(TextureId id, bool clamp);
    [[nodiscard]] bool translucent(TextureId id) const { return entries_[id].translucent; }
    [[nodiscard]] bool hasAlpha(TextureId id) const { return entries_[id].hasAlpha; }
    [[nodiscard]] std::size_t residentBytes() const { return residentBytes_; }
    [[nodiscard]] std::size_t residentCount() const { return residentCount_; }
    [[nodiscard]] std::size_t pendingCount() const { return pendingCount_; }
    // Textures still decoding / waiting for upload at `upTo` or more urgent (a scan).
    [[nodiscard]] std::size_t pendingCount(JobPriority upTo) const;
    [[nodiscard]] std::size_t uncompressedBytes() const { return rawBytes_; }
    // Bumped whenever a texture's state (and so a material's slot / alpha mode) can change.
    [[nodiscard]] u64 version() const { return version_; }

private:
    enum class State : u8 { Free, Decoding, Ready, Resident, Failed };
    struct Entry {
        std::string key;
        std::string path;
        bool colorKey = false;
        u8 r = 0, g = 0, b = 0;
        u32 rawMaxSize = 0; // acquireRaw: > 0
        f32 hdrScale = 1.0f;
        State state = State::Free;
        u32 refs = 0;
        phx::Image image;
        u32 slotRepeat = 0, slotClamp = 0;
        bool hasAlpha = false, translucent = false;
        std::size_t bytes = 0, raw = 0;
        u32 generation = 0;
        JobPriority priority = JobPriority::Idle;
        JobTicket job;
    };
    struct Done {
        TextureId id;
        u32 generation;
        bool ok;
        phx::tex::PreparedTexture tex;
        std::size_t raw;
    };
    void destroy(Entry& e);
    phx::VulkanContext& ctx_;
    phx::Bindless& bindless_;
    phx::UploadQueue& upload_;
    ThreadPool& pool_;
    TextureSettings settings_;
    std::vector<Entry> entries_;
    std::vector<TextureId> free_;
    std::unordered_map<std::string, TextureId> index_;
    std::vector<TextureId> zeroRef_;
    std::mutex doneMutex_;
    std::vector<Done> done_;
    std::vector<Done> waiting_; // decoded but not uploaded yet (budget)
    struct Zombie {
        phx::Image image;
        u32 frames;
        std::size_t bytes, raw;
    };
    std::vector<Zombie> zombies_;
    std::size_t residentBytes_ = 0, residentCount_ = 0, pendingCount_ = 0, rawBytes_ = 0;
    u64 version_ = 1;
    struct RawRequest {
        u32 maxSize = 0;
        f32 hdrScale = 1.0f;
        std::string key;
    };
    RawRequest pendingRaw_; // set by acquireRaw() around its acquire() call
};

enum class AlphaMode : u8 { Opaque, AlphaTest, Blend, Additive };

class MaterialTable {
public:
    explicit MaterialTable(TextureCache& textures) : textures_(textures) {}
    // Its texture is decoded at `priority` (raised if already queued lower).
    MaterialId acquire(const Material& m, JobPriority priority = JobPriority::View);
    void release(MaterialId id);
    [[nodiscard]] bool ready(MaterialId id) const;
    [[nodiscard]] AlphaMode mode(MaterialId id) const;
    // Writes the GPU table (index = MaterialId); returns the entry count.
    u32 write(GpuMaterial* dst, u32 capacity);
    [[nodiscard]] u32 size() const { return u32(entries_.size()); }
    // Changes whenever write() could produce different output (dirty tracking per destination).
    [[nodiscard]] u64 version() const { return version_ + textures_.version(); }

private:
    struct Entry {
        Material m;
        TextureId texture = kNone;
        u32 refs = 0;
    };
    TextureCache& textures_;
    std::vector<Entry> entries_;
    std::vector<MaterialId> free_;
    std::unordered_map<std::string, MaterialId> index_;
    u64 version_ = 0;
};

} // namespace ots
