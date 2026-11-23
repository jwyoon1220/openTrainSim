#pragma once
// Registry of every object file referenced by the route and the train. Objects are interned
// by (path, variant) and parsed lazily, on whichever thread asks (streaming workers): the
// route can reference thousands of files while only the ones near the camera are in memory.
#include "bve/Object.hpp"

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ots {

enum class ObjectKind : u8 { Static, Animated, Unsupported };
enum class LoadState : u8 { Unloaded, Loading, Ready, Failed };

struct ObjectVariant {
    bool mirror = false;   // Mirror() for poles placed on the right
    bool deform = false;   // platform / crack transform between two rails
    f32 nearDistance = 0, farDistance = 0;
    bool operator==(const ObjectVariant&) const = default;
};

struct ObjectRecord {
    std::filesystem::path path;
    ObjectVariant variant;
    ObjectKind kind = ObjectKind::Static;
    std::atomic<LoadState> state{LoadState::Unloaded};
    // Static: CPU mesh until the renderer uploaded it (then released to save RAM).
    std::shared_ptr<StaticMesh> mesh;
    // Animated: definition (kept; small).
    std::shared_ptr<AnimatedDef> anim;
    glm::vec3 bmin{0.0f}, bmax{0.0f};
    f32 radius = 0.0f;    // bounding sphere radius around the object origin
    u32 triangles = 0;
    std::string error;
};

class ObjectLibrary {
public:
    // Thread-safe; returns the same id for the same (resolved path, variant).
    ObjectId intern(const std::filesystem::path& path, const ObjectVariant& variant = {});
    ObjectLibrary();
    // Lock-free access: records live in fixed chunks that never move.
    ObjectRecord& operator[](ObjectId id) { return chunks_[id >> kChunkBits][id & (kChunkSize - 1)]; }
    const ObjectRecord& operator[](ObjectId id) const { return chunks_[id >> kChunkBits][id & (kChunkSize - 1)]; }
    [[nodiscard]] std::size_t size() const { return count_.load(std::memory_order_acquire); }
    // Parses the file now on the calling thread (no-op if already loaded / loading elsewhere).
    // Returns true when the record is Ready afterwards.
    bool load(ObjectId id);
    // Releases the CPU mesh (after GPU upload); the record goes back to Unloaded when
    // `forget` (so a later request re-parses it), else stays Ready with bounds only.
    void releaseMesh(ObjectId id, bool forget);
    [[nodiscard]] static ObjectKind kindOf(const std::filesystem::path& p);

    std::atomic<u64> parseCount{0}, failCount{0};

private:
    static constexpr u32 kChunkBits = 10, kChunkSize = 1u << kChunkBits, kMaxChunks = 1u << 12;
    mutable std::mutex mutex_;
    std::unique_ptr<std::unique_ptr<ObjectRecord[]>[]> chunks_;
    std::atomic<u32> count_{0};
    std::unordered_map<std::string, ObjectId> index_;
};

// .animated loader: fills `out`; state objects are interned into `lib` (not loaded).
bool loadAnimated(const std::filesystem::path& path, AnimatedDef& out, ObjectLibrary& lib, std::string& error, int depth = 0);

} // namespace ots
