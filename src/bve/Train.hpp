#pragma once
// openBVE train definition: train.dat (performance, handles, cab), extensions.cfg (car
// objects, lengths, axles, couplers) and the 3D cab (panel.animated).
#include "bve/ObjectLibrary.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace ots {

struct AccelerationCurve {
    // openBVE: a0, a1 (km/h/s), v1, v2 (km/h), e
    double a0 = 0, a1 = 0, v1 = 0, v2 = 0, e = 1;
    // Acceleration in m/s² at speed v (m/s).
    [[nodiscard]] double at(double v) const;
};

enum class HandleType : u8 { Separate, OneHandle, InterlockedSeparate };
enum class BrakeType : u8 { ElectromagneticStraight, ElectroPneumatic, AutomaticAir };

// train.dat #MOTOR_P1/P2/B1/B2: one row per 0.2 km/h (openBVE BVEMotorSoundTableEntry).
struct MotorSoundEntry {
    int sound = -1;   // [Motor] index in sound.cfg, -1 = silent
    float pitch = 1.0f, gain = 1.0f;
};

struct TrainSpec {
    std::vector<AccelerationCurve> power; // one per notch
    double deceleration = 1.0 / 3.6;      // service max, m/s²
    double staticFriction = 0.35;
    double rollingResistance = 0.0025;
    double aerodynamicDrag = 1.1;
    std::vector<double> delayPowerUp, delayPowerDown, delayBrakeUp, delayBrakeDown;
    double jerkPowerUp = 10, jerkPowerDown = 10, jerkBrakeUp = 10, jerkBrakeDown = 10; // 1/100 km/h/s²
    double brakeCylinderUp = 300, brakeCylinderDown = 200;                            // kPa/s
    BrakeType brakeType = BrakeType::ElectromagneticStraight;
    double bcServiceMax = 440, bcEmergencyMax = 440, mainReservoirMin = 690, mainReservoirMax = 780, brakePipeNormal = 490;
    HandleType handleType = HandleType::Separate;
    int powerNotches = 8, brakeNotches = 8, driverPowerNotches = 8, driverBrakeNotches = 8;
    int powerNotchReduceSteps = 0;
    glm::dvec3 cab{0.0, 1000.0, 0.0}; // mm, from the centre of the front of the driver car
    int driverCar = 0;
    double motorCarMass = 40, trailerCarMass = 40; // t
    int motorCars = 1, trailerCars = 0;
    double carLength = 20, carWidth = 2.6, carHeight = 3.2, centerOfGravity = 1.5;
    double exposedArea = 5.0, unexposedArea = 1.6;
    bool frontCarMotor = true;
    int ats = -1, atc = 0;
    bool eb = false, constSpeed = false, holdBrake = false, readhesion = false, passAlarm = false;
    double doorWidth = 1000.0;
    std::array<std::vector<MotorSoundEntry>, 4> motorSound; // P1, P2, B1, B2
};

struct CarDef {
    ObjectId exterior = kNoObject;
    double length = 20.0;
    double frontAxle = 8.0, rearAxle = -8.0; // relative to the car centre
    bool reversed = false;
    bool motor = false;
    double mass = 40.0;                      // t
    double couplerGap = 0.0;                 // to the next car (m)
    bool hasDriver = false;
    glm::dvec3 driver{0.0};                  // eye position relative to the car centre (m)
};

struct TrainDef {
    std::filesystem::path folder;
    std::string name;
    TrainSpec spec;
    std::vector<CarDef> cars;
    ObjectId cab = kNoObject;    // panel.animated (3D cab), car coordinates of the driver car
    bool hasPanel2D = false;     // panel.cfg / panel2.cfg present (not rendered yet)
    std::vector<std::string> warnings;
    [[nodiscard]] double length() const;
    [[nodiscard]] double totalMass() const; // kg
};

bool loadTrain(const std::filesystem::path& folder, ObjectLibrary& lib, TrainDef& out, std::string& error);
// openBVE train folder lookup: Train/<name> next to the route's Railway folder.
std::filesystem::path findTrainFolder(const std::filesystem::path& routeFile, const std::filesystem::path& trainHint,
                                      const std::string& name);

} // namespace ots
