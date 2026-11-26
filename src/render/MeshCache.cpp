#include "render/MeshCache.hpp"

#include "phx/core/Log.hpp"
#include "phx/gpu/VulkanContext.hpp"

#include <algorithm>
#include <unordered_map>

namespace ots {

MeshCache::MeshCache(phx::VulkanContext& ctx, phx::UploadQueue& upload, ThreadPool& pool, ObjectLibrary& lib, MaterialTable& materials,
                     GeometrySettings s)
    : ctx_(ctx), upload_(upload), pool_(pool), lib_(lib), materials_(materials), settings_(s),
      vertices_(ctx, VkDeviceSize(s.vertexHeapMB) * 1024 * 1024 / sizeof(GpuVertex), sizeof(GpuVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
      indices_(ctx, VkDeviceSize(s.indexHeapMB) * 1024 * 1024 / sizeof(u16), sizeof(u16), VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {}

MeshCache::~MeshCache() {
    for (Entry& e : entries_) pool_.cancel(e.job);
    pool_.waitIdle();
    for (Entry& e : entries_)
        if (e.state == State::Resident && !e.animated) {
            vertices_.free(e.mesh.vertices, 0);
            indices_.free(e.mesh.indices, 0);
        }
}

MeshCache::Entry& MeshCache::entry(ObjectId id) {
    if (id >= entries_.size()) entries_.resize(std::max<std::size_t>(id + 1, entries_.size() * 2));
    return entries_[id];
}

void MeshCache::pin(ObjectId id) {
    if (id == kNoObject) return;
    entry(id).pinned = true;
    request(id, JobPriority::Critical);
}

bool MeshCache::failed(ObjectId id) const { return id < entries_.size() && entries_[id].state == State::Failed; }

void MeshCache::request(ObjectId id, JobPriority priority) {
    if (id == kNoObject) return;
    Entry& e = entry(id);
    e.lastUsed = frame_;
    if (e.state == State::Loading && priority < e.priority) {
        e.priority = priority;
        pool_.raise(e.job, priority); // no-op once a worker has it; the upload order still follows
    }
    if (e.state != State::None) return;
    e.state = State::Loading;
    e.priority = priority;
    ++inFlight_;
    e.job = pool_.submit(
        [this, id] {
            Packed p;
            p.id = id;
            ObjectRecord& r = lib_[id];
            if (lib_.load(id)) {
                if (r.kind == ObjectKind::Animated) {
                    p.ok = true;
                    p.animated = true;
                } else if (auto mesh = r.mesh) {
                    pack(*mesh, p);
                    p.ok = true;
                    p.radius = r.radius;
                    p.center = 0.5f * (r.bmin + r.bmax);
                    lib_.releaseMesh(id, true); // the GPU copy is the only one kept
                }
            }
            std::lock_guard lock(doneMutex_);
            done_.push_back(std::move(p));
        },
        priority);
}

void MeshCache::pack(const StaticMesh& mesh, Packed& out) {
    for (const SubMesh& part : mesh.parts) {
        const u32 matIndex = u32(out.materials.size());
        out.materials.push_back(part.material);
        auto emit = [&](const std::vector<u32>& vmap, const std::vector<u16>& idx) {
            PackedDraw d{u32(out.vertices.size()), u32(out.indices.size()), u32(idx.size()), matIndex, part.doubleSided};
            for (u32 vi : vmap) {
                const MeshVertex& v = part.vertices[vi];
                out.vertices.push_back({v.pos.x, v.pos.y, v.pos.z, packOctNormal(v.normal), v.uv.x, v.uv.y});
            }
            out.indices.insert(out.indices.end(), idx.begin(), idx.end());
            out.draws.push_back(d);
        };
        if (part.vertices.size() <= 65535) {
            std::vector<u32> vmap(part.vertices.size());
            for (u32 i = 0; i < vmap.size(); ++i) vmap[i] = i;
            std::vector<u16> idx(part.indices.begin(), part.indices.end());
            emit(vmap, idx);
            continue;
        }
        // split: chunks of triangles referencing at most 65535 distinct vertices
        std::unordered_map<u32, u16> remap;
        std::vector<u32> vmap;
        std::vector<u16> idx;
        for (std::size_t t = 0; t + 2 < part.indices.size(); t += 3) {
            if (vmap.size() + 3 > 65535) {
                emit(vmap, idx);
                remap.clear(), vmap.clear(), idx.clear();
            }
            for (int k = 0; k < 3; ++k) {
                const u32 vi = part.indices[t + std::size_t(k)];
                auto [it, inserted] = remap.try_emplace(vi, u16(vmap.size()));
                if (inserted) vmap.push_back(vi);
                idx.push_back(it->second);
            }
        }
        if (!idx.empty()) emit(vmap, idx);
    }
}

bool MeshCache::upload(Packed& p) {
    Entry& e = entry(p.id);
    if (p.animated) {
        e.animated = true;
        e.state = State::Resident;
        ++resident_;
        e.job = {};
        const bool pinned = e.pinned; // `e` may dangle once request() grows entries_
        if (const auto& def = lib_[p.id].anim)
            for (const AnimatedPart& part : def->parts)
                for (ObjectId s : part.states)
                    if (s != kNoObject) {
                        if (pinned) pin(s);
                        else request(s, JobPriority::Idle);
                    }
        return true;
    }
    const VkDeviceSize vbytes = p.vertices.size() * sizeof(GpuVertex), ibytes = p.indices.size() * sizeof(u16);
    if (!upload_.canFit(vbytes + ibytes + 64)) return false;
    GpuMesh m;
    if (!p.vertices.empty()) {
        if (!vertices_.allocate(p.vertices.size(), m.vertices) || !indices_.allocate(p.indices.size(), m.indices)) {
            vertices_.free(m.vertices, 0);
            // heap full: evict the least recently used unpinned object and retry next frame
            ObjectId victim = kNoObject;
            u64 oldest = ~0ull;
            for (ObjectId i = 0; i < entries_.size(); ++i) {
                const Entry& x = entries_[i];
                if (x.state == State::Resident && !x.pinned && !x.animated && x.lastUsed + 2 < frame_ && x.lastUsed < oldest) {
                    oldest = x.lastUsed;
                    victim = i;
                }
            }
            if (victim == kNoObject) {
                phx::log::warn("geometry heap full ({} MB vertices / {} MB indices): raise [render] vertexHeapMB / indexHeapMB",
                               settings_.vertexHeapMB, settings_.indexHeapMB);
                return false;
            }
            evict(victim);
            return false;
        }
        upload_.uploadBuffer(vertices_.buffer().buffer, m.vertices.offset * sizeof(GpuVertex), p.vertices.data(), vbytes);
        upload_.uploadBuffer(indices_.buffer().buffer, m.indices.offset * sizeof(u16), p.indices.data(), ibytes);
    }
    std::vector<MaterialId> mats(p.materials.size());
    for (std::size_t i = 0; i < mats.size(); ++i) mats[i] = materials_.acquire(p.materials[i], e.priority);
    for (const PackedDraw& d : p.draws)
        m.draws.push_back({u32(m.vertices.offset) + d.vertexBase, u32(m.indices.offset) + d.indexBase, d.indexCount, mats[d.material], d.doubleSided});
    m.materials = std::move(mats);
    m.radius = p.radius;
    m.center = p.center;
    e.mesh = std::move(m);
    e.readyVersion = 0;
    e.state = State::Resident;
    e.animated = false;
    e.job = {};
    ++resident_;
    return true;
}

void MeshCache::evict(ObjectId id) {
    Entry& e = entries_[id];
    if (e.state != State::Resident || e.animated) return;
    vertices_.free(e.mesh.vertices);
    indices_.free(e.mesh.indices);
    for (MaterialId m : e.mesh.materials) materials_.release(m);
    e.mesh = {};
    e.state = State::None;
    --resident_;
}

void MeshCache::update(u64 frame, double frameSeconds) {
    frame_ = frame;
    if (frameSeconds > 0) framesPerSecond_ = 0.95 * framesPerSecond_ + 0.05 * (1.0 / frameSeconds);
    vertices_.retireFrame();
    indices_.retireFrame();
    {
        std::lock_guard lock(doneMutex_);
        for (Packed& p : done_) waiting_.push_back(std::move(p));
        done_.clear();
    }
    // most urgent first: the upload budget goes to what is visible
    const auto meshPriority = [&](const Packed& p) { return entries_[p.id].priority; };
    if (!std::ranges::is_sorted(waiting_, {}, meshPriority)) std::ranges::stable_sort(waiting_, {}, meshPriority);
    std::size_t keep = 0;
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        Packed& p = waiting_[i];
        Entry& e = entry(p.id);
        if (!p.ok) {
            e.state = State::Failed;
            e.job = {};
            --inFlight_;
            continue;
        }
        if (upload(p)) {
            --inFlight_;
        } else {
            waiting_[keep++] = std::move(p);
        }
    }
    waiting_.resize(keep);

    // LRU eviction of objects not drawn for keepSeconds (time measured in frames)
    if (frame % 30 == 0) {
        const u64 keepFrames = u64(std::max(1.0, settings_.keepSeconds * framesPerSecond_));
        for (ObjectId id = 0; id < entries_.size(); ++id) {
            Entry& e = entries_[id];
            if (e.state == State::Resident && !e.pinned && !e.animated && e.lastUsed + keepFrames < frame) evict(id);
        }
    }
}

std::size_t MeshCache::pendingCount(JobPriority upTo) const {
    return std::size_t(std::ranges::count_if(entries_, [&](const Entry& e) { return e.state == State::Loading && e.priority <= upTo; }));
}

const GpuMesh* MeshCache::drawable(ObjectId id, u64 frame, JobPriority priority) {
    if (id == kNoObject) return nullptr;
    Entry& e = entry(id);
    e.lastUsed = frame;
    if (e.state == State::None || e.state == State::Loading) {
        request(id, priority);
        return nullptr;
    }
    if (e.state != State::Resident || e.animated) return nullptr;
    // Material readiness only changes with the material table's version: cache it per mesh
    // instead of walking every draw every frame.
    const u64 v = materials_.version();
    if (e.readyVersion != v) {
        e.readyVersion = v;
        e.ready = std::ranges::all_of(e.mesh.draws, [&](const GpuDraw& d) { return materials_.ready(d.material); });
    }
    return e.ready ? &e.mesh : nullptr;
}

const AnimatedDef* MeshCache::animated(ObjectId id, u64 frame, JobPriority priority) {
    if (id == kNoObject) return nullptr;
    Entry& e = entry(id);
    e.lastUsed = frame;
    if (e.state == State::None || e.state == State::Loading) {
        request(id, priority);
        return nullptr;
    }
    if (e.state != State::Resident || !e.animated) return nullptr;
    return lib_[id].anim.get();
}

} // namespace ots
