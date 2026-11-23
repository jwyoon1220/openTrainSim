#include "bve/ObjectLibrary.hpp"

#include "bve/MeshBuilder.hpp"
#include "phx/core/Log.hpp"
#include "util/Text.hpp"

#include <format>

namespace ots {

ObjectKind ObjectLibrary::kindOf(const std::filesystem::path& p) {
    const std::string ext = lower(p.extension().string());
    if (ext == ".csv" || ext == ".b3d" || ext == ".x") return ObjectKind::Static;
    if (ext == ".animated") return ObjectKind::Animated;
    return ObjectKind::Unsupported;
}

ObjectLibrary::ObjectLibrary() : chunks_(std::make_unique<std::unique_ptr<ObjectRecord[]>[]>(kMaxChunks)) {}

ObjectId ObjectLibrary::intern(const std::filesystem::path& path, const ObjectVariant& v) {
    const std::string key = std::format("{}|{}|{}|{:.3f}|{:.3f}", path.string(), int(v.mirror), int(v.deform), v.nearDistance, v.farDistance);
    std::lock_guard lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) return it->second;
    const ObjectId id = count_.load(std::memory_order_relaxed);
    if ((id >> kChunkBits) >= kMaxChunks) PHX_FATAL("ObjectLibrary: too many objects");
    if (!chunks_[id >> kChunkBits]) chunks_[id >> kChunkBits] = std::make_unique<ObjectRecord[]>(kChunkSize);
    ObjectRecord& r = (*this)[id];
    r.path = path;
    r.variant = v;
    r.kind = kindOf(path);
    if (r.kind == ObjectKind::Unsupported) {
        r.state = LoadState::Failed;
        r.error = "unsupported object format";
    }
    index_.emplace(key, id);
    count_.store(id + 1, std::memory_order_release);
    return id;
}

bool ObjectLibrary::load(ObjectId id) {
    ObjectRecord& r = (*this)[id];
    LoadState expected = LoadState::Unloaded;
    if (!r.state.compare_exchange_strong(expected, LoadState::Loading)) return expected == LoadState::Ready;
    ++parseCount;
    std::string err;
    bool ok = false;
    if (r.kind == ObjectKind::Animated) {
        auto def = std::make_shared<AnimatedDef>();
        ok = loadAnimated(r.path, *def, *this, err);
        if (ok) {
            r.anim = std::move(def);
            r.radius = 0.0f;
        }
    } else {
        auto mesh = std::make_shared<StaticMesh>();
        const std::string ext = lower(r.path.extension().string());
        ok = ext == ".x" ? loadX(r.path, *mesh, err) : loadCsvB3d(r.path, *mesh, err);
        if (ok) {
            if (r.variant.deform) mesh->deformBetweenRails(r.variant.nearDistance, r.variant.farDistance);
            if (r.variant.mirror) mesh->mirrorX();
            mesh->computeBounds();
            r.bmin = mesh->bmin;
            r.bmax = mesh->bmax;
            r.radius = std::max(glm::length(mesh->bmin), glm::length(mesh->bmax));
            r.triangles = u32(mesh->triangleCount());
            r.mesh = std::move(mesh);
        }
    }
    if (!err.empty()) r.error = err;
    if (!ok) {
        ++failCount;
        phx::log::warn("Object {}: {}", r.path.string(), err);
    }
    r.state = ok ? LoadState::Ready : LoadState::Failed;
    return ok;
}

void ObjectLibrary::releaseMesh(ObjectId id, bool forget) {
    ObjectRecord& r = (*this)[id];
    if (r.kind != ObjectKind::Static) return;
    r.mesh.reset();
    if (forget && r.state == LoadState::Ready) r.state = LoadState::Unloaded;
}

} // namespace ots
