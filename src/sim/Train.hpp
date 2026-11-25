#pragma once
// Player train dynamics with openBVE semantics: notched power / brake handles with delays
// and jerk limits, per-car motor acceleration averaged over the train mass, brake cylinder
// pressure → deceleration, rolling + aerodynamic resistance, gradient.
#include "bve/Controls.hpp"
#include "bve/Route.hpp"
#include "bve/Train.hpp"

namespace ots {

struct CarPose {
    glm::dvec3 position{0.0};               // car centre, rail level
    glm::dvec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1}; // side, up, forward
    double frontAxle = 0.0, rearAxle = 0.0;  // track positions
};

class TrainSim {
public:
    TrainSim(const TrainDef& def, const Route& route);
    void placeAt(double frontTrackPosition);
    // Moves the train without touching its speed (collision clamp).
    void setFront(double frontTrackPosition);
    void command(Command c);
    void update(double dt);

    // handles (requested)
    int reverser = 0;     // -1 back, 0 neutral, 1 forward
    int powerNotch = 0, brakeNotch = 0;
    bool emergency = false;
    bool holdBrake = false, constSpeed = false;
    // safety system (train plugin) output: overrides the driver's handles while active.
    // Brake uses the plugin convention: max + 1 = emergency.
    bool safetyActive = false;
    int safetyReverser = 0, safetyPower = 0, safetyBrake = 0;
    bool safetyHoldBrake = false;
    [[nodiscard]] int effectiveReverser() const { return safetyActive ? safetyReverser : reverser; }
    [[nodiscard]] int effectivePower() const { return safetyActive ? safetyPower : powerNotch; }
    [[nodiscard]] int effectiveBrake() const { return safetyActive ? safetyBrake : (emergency ? maxBrake() + 1 : brakeNotch); }
    [[nodiscard]] bool effectiveEmergency() const { return effectiveBrake() > maxBrake(); }
    // actual (after delays)
    int powerActual = 0, brakeActual = 0;
    // physical state
    double speed = 0.0;        // m/s, along the track (negative = backwards)
    double acceleration = 0.0; // m/s²
    double front = 0.0;        // track position of the front of the train
    double odometer = 0.0;
    double brakeCylinder = 0.0, brakePipe = 0.0, mainReservoir = 0.0, straightAirPipe = 0.0; // kPa
    double motorAcceleration = 0.0;
    bool compressor = false;   // main reservoir compressor running
    // doors (0 closed … 1 open) and targets
    double leftDoors = 0.0, rightDoors = 0.0;
    bool leftDoorsTarget = false, rightDoorsTarget = false;
    bool hornPrimary = false, hornSecondary = false, headlights = false;
    std::vector<CarPose> cars;

    [[nodiscard]] const TrainDef& def() const { return def_; }
    [[nodiscard]] double rear() const { return front - length_; }
    [[nodiscard]] double length() const { return length_; }
    [[nodiscard]] int maxPower() const { return def_.spec.driverPowerNotches; }
    [[nodiscard]] int maxBrake() const { return def_.spec.driverBrakeNotches; }
    [[nodiscard]] bool doorsClosed() const { return leftDoors == 0.0 && rightDoors == 0.0 && !leftDoorsTarget && !rightDoorsTarget; }
    // Eye position / orientation of the driver (world).
    struct DriverView {
        glm::dvec3 position, X, Y, Z; // right, up, forward
    };
    [[nodiscard]] DriverView driverPose() const;

private:
    void updateCars();
    void stepHandles(double dt);
    const TrainDef& def_;
    const Route& route_;
    double length_ = 0.0;
    double powerTimer_ = 0.0, brakeTimer_ = 0.0;
    double motorTarget_ = 0.0;
    std::vector<double> carOffsets_; // car centre distance behind the train front
};

} // namespace ots
