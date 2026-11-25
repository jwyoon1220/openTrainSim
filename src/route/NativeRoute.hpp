#pragma once
// openTrainSim's own route format (<name>.route.json): the line described as data —
// alignment, grades, stations, block signals, speed limits, train-protection zones — and
// built into the same Route the openBVE parser produces, with procedural track, signals and
// platforms (no external objects needed). Format: docs/route-format.md.
#include "bve/Route.hpp"

namespace ots {

[[nodiscard]] bool isNativeRoute(const std::filesystem::path& file);
[[nodiscard]] Expected<LoadedRoute> loadNativeRoute(const std::filesystem::path& file, ObjectLibrary& lib);
// Either format, by file name.
[[nodiscard]] Expected<LoadedRoute> loadAnyRoute(const std::filesystem::path& file, ObjectLibrary& lib);

} // namespace ots
