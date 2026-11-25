#pragma once
// Terrain internals: meshes built on loader threads (terrain tiles, tunnel linings, portals)
// and the procedural ground textures they use.
#include "bve/Object.hpp"
#include "terrain/Model.hpp"

#include <string>

namespace ots::terrain {

struct TerrainTextures {
    std::string grass, rock, soil, concrete; // file paths
    // Writes the textures into the user cache the first time (tileable value noise).
    static TerrainTextures ensure();
};

// A square of terrain, `size` metres, minimum corner at `corner`, vertices relative to `origin`.
struct TileSpec {
    glm::dvec2 corner{0.0};
    double size = 256.0, spacing = 4.0;
    glm::dvec3 origin{0.0};
};

StaticMesh buildTile(const TerrainModel& model, const TerrainTextures& tex, const TileSpec& tile);
// Lining of a generated tunnel between two track positions.
StaticMesh buildLining(const TerrainModel& model, const TerrainTextures& tex, double from, double to, const glm::dvec3& origin);
// Portal wall at a track position; `entrance` faces trains coming towards it.
StaticMesh buildPortal(const TerrainModel& model, const TerrainTextures& tex, double trackPos, bool entrance, const glm::dvec3& origin);

} // namespace ots::terrain
