#pragma once
// 3D cab: a train ships <train>/cab.json next to a cab model (glTF, as exported by Blender).
// The file names the model's nodes that are interactive elements — buttons, switches, levers,
// gauges, lamps and generic animated parts — and binds them to the train (handles, doors,
// horns, safety keys, any command) or to openBVE function scripts. No scripting runtime.
// The driver operates the elements with the mouse (click, drag, wheel); keyboard controls keep
// working and the bound elements follow them. See docs/cab-api.md for the file format.
// Without a cab.json the openBVE cab (panel.animated) is used as before.
#include "bve/ObjectLibrary.hpp"
#include "util/Expected.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ots {

class Simulation;

enum class CabKind : u8 { Part, Button, Switch, Lever, Gauge, Lamp };

struct CabElementView {
    std::string name, tooltip;
    CabKind kind = CabKind::Part;
    bool interactive = false;
};

struct CabSoundRequest {
    std::filesystem::path file;
    glm::dvec3 position{0.0}; // car coordinates of the driver car
    double volume = 1.0, pitch = 1.0;
};

struct CabMessage {
    std::string text;
    double seconds = 3.0;
};

class Cab {
public:
    ~Cab();
    Cab(const Cab&) = delete;
    Cab& operator=(const Cab&) = delete;

    // Reads cab.json and its models. Null (and `error`) when it cannot be loaded.
    static Expected<std::unique_ptr<Cab>> load(const std::filesystem::path& file, ObjectLibrary& lib, Simulation& sim);

    // Once per frame before the simulation step: bound / function-script values, alerts.
    void update(double dt);

    // Mouse. Rays are in the driver car's coordinates (x right, y up, z forward, from the car
    // centre). press() returns true when an element took the mouse (no camera look meanwhile).
    void hover(const glm::dvec3& origin, const glm::dvec3& dir);
    bool press(int button, const glm::dvec3& origin, const glm::dvec3& dir);
    void drag(double dxPixels, double dyPixels);
    void release(int button);
    bool scroll(const glm::dvec3& origin, const glm::dvec3& dir, double steps);
    [[nodiscard]] bool capturing() const;
    [[nodiscard]] int pick(const glm::dvec3& origin, const glm::dvec3& dir) const; // element index or -1

    // Rendering: static bodies plus one object per element in its current pose.
    [[nodiscard]] const std::vector<ObjectId>& bodies() const;
    [[nodiscard]] std::size_t elementCount() const;
    [[nodiscard]] ObjectId elementObject(std::size_t i) const;      // current state
    [[nodiscard]] const std::vector<ObjectId>& elementStates(std::size_t i) const;
    [[nodiscard]] RigidPose elementPose(std::size_t i) const;
    [[nodiscard]] CabElementView view(std::size_t i) const;
    [[nodiscard]] int hovered() const; // element under the mouse, -1 = none
    // Driver eye position (car coordinates) if cab.json sets one.
    [[nodiscard]] std::optional<glm::dvec3> eye() const;

    // Sounds / messages to show, drained by the game every frame.
    std::vector<CabSoundRequest> sounds;
    std::vector<CabMessage> messages;
    [[nodiscard]] const std::string& name() const;

    struct Impl; // Cab.cpp

private:
    Cab();
    std::unique_ptr<Impl> impl_;
};

} // namespace ots
