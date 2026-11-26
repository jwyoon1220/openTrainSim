#pragma once
// GPU residency of objects. Every object is uploaded once and drawn instanced wherever it is
// placed. Parsing and vertex packing (24-byte vertices, 16-bit indices) happen on worker
// threads; the main thread only copies into the vertex / index heaps within the frame's
// upload budget. Objects unused for a while are evicted (LRU) and re-parsed on demand.
#include "bve/ObjectLibrary.hpp"
#include "render/TextureCache.hpp"

#include <deque>
#include <mutex>
#include <vector>

namespace ots {

struct GpuDraw {
    u32 vertexOffset = 0, firstIndex = 0, indexCount = 0;
    MaterialId material = kNone;
    bool doubleSided = false;
};

struct GpuMesh {
    std::vector<GpuDraw> draws;
    std::vector<MaterialId> materials; // one reference each (released on eviction)
    phx::GpuHeap::Range vertices, indices;
    f32 radius = 0.0f;
    glm::vec3 center{0.0f};
};

struct GeometrySettings {
    u32 vertexHeapMB = 64, indexHeapMB = 16;
    f32 keepSeconds = 30.0f; // evict objects unused for this long
};

class MeshCache {
public:
    MeshCache(phx::VulkanContext& ctx, phx::UploadQueue& upload, ThreadPool& pool, ObjectLibrary& lib, MaterialTable& materials,
              GeometrySettings settings);
    ~MeshCache();

    // Starts loading (parse + pack on a worker) unless already loading / resident; a queued
    // load is raised if `priority` is more urgent than before. Its textures inherit it.
    void request(ObjectId id, JobPriority priority = JobPriority::View);
    // Static object: resident and all its textures settled → drawable. Marks it used and
    // requests it at `priority` if it is not.
    const GpuMesh* drawable(ObjectId id, u64 frame, JobPriority priority = JobPriority::View);
    // Animated object definition, once loaded (its states are requested automatically, at
    // Idle: the state on show is raised when drawn).
    const AnimatedDef* animated(ObjectId id, u64 frame, JobPriority priority = JobPriority::View);
    [[nodiscard]] bool failed(ObjectId id) const;
    void update(u64 frame, double frameSeconds);
    // Marks all objects referenced by permanent things (train, cab) as never evicted.
    void pin(ObjectId id);

    [[nodiscard]] const phx::GpuHeap& vertexHeap() const { return vertices_; }
    [[nodiscard]] const phx::GpuHeap& indexHeap() const { return indices_; }
    [[nodiscard]] std::size_t residentCount() const { return resident_; }
    [[nodiscard]] std::size_t pendingCount() const { return inFlight_; }
    // Loads still pending at `upTo` or more urgent (a scan: for the loading screen).
    [[nodiscard]] std::size_t pendingCount(JobPriority upTo) const;

private:
    enum class State : u8 { None, Loading, Resident, Failed };
    struct Entry {
        State state = State::None;
        bool pinned = false;
        bool animated = false;
        JobPriority priority = JobPriority::Idle; // most urgent request so far (this load)
        JobTicket job;
        u64 lastUsed = 0;
        u64 readyVersion = 0; // MaterialTable::version() when `ready` was computed (0 = never)
        bool ready = false;
        GpuMesh mesh;
    };
    struct PackedDraw {
        u32 vertexBase, indexBase, indexCount, material;
        bool doubleSided;
    };
    struct Packed {
        ObjectId id;
        bool ok = false, animated = false;
        std::vector<GpuVertex> vertices;
        std::vector<u16> indices;
        std::vector<PackedDraw> draws;
        std::vector<Material> materials;
        f32 radius = 0.0f;
        glm::vec3 center{0.0f};
    };
    Entry& entry(ObjectId id);
    bool upload(Packed& p);
    void evict(ObjectId id);
    static void pack(const StaticMesh& mesh, Packed& out);

    phx::VulkanContext& ctx_;
    phx::UploadQueue& upload_;
    ThreadPool& pool_;
    ObjectLibrary& lib_;
    MaterialTable& materials_;
    GeometrySettings settings_;
    phx::GpuHeap vertices_, indices_;
    std::deque<Entry> entries_; // deque: growing keeps references (draw lists hold GpuDraw*)
    std::mutex doneMutex_;
    std::vector<Packed> done_, waiting_;
    std::size_t resident_ = 0, inFlight_ = 0;
    u64 frame_ = 0;
    double framesPerSecond_ = 60.0;
};

} // namespace ots
