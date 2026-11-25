#include "sim/Train.hpp"

#include <algorithm>
#include <cmath>

namespace ots {
namespace {
constexpr double kG = 9.80665;
constexpr double kAirDensity = 1.225;

double delayFor(const std::vector<double>& d, int notch) {
    if (d.empty()) return 0.0;
    return d[std::size_t(std::clamp(notch, 0, int(d.size()) - 1))];
}
} // namespace

TrainSim::TrainSim(const TrainDef& def, const Route& route) : def_(def), route_(route) {
    double offset = 0.0;
    carOffsets_.resize(def.cars.size());
    for (std::size_t i = 0; i < def.cars.size(); ++i) {
        carOffsets_[i] = offset + 0.5 * def.cars[i].length;
        offset += def.cars[i].length + (i + 1 < def.cars.size() ? def.cars[i].couplerGap : 0.0);
    }
    length_ = offset;
    cars.resize(def.cars.size());
    mainReservoir = def.spec.mainReservoirMax;
    brakePipe = def.spec.brakePipeNormal;
    // openBVE starts with the brake applied (full service) and the reverser in neutral.
    brakeNotch = brakeActual = maxBrake();
    brakeCylinder = def.spec.bcServiceMax;
}

void TrainSim::placeAt(double frontTrackPosition) {
    front = frontTrackPosition;
    speed = 0.0;
    updateCars();
}

void TrainSim::setFront(double f) {
    front = f;
    updateCars();
}

void TrainSim::command(Command c) {
    const bool oneHandle = def_.spec.handleType == HandleType::OneHandle;
    auto toPower = [&] { // single handle towards power
        if (emergency) {
            emergency = false;
            brakeNotch = maxBrake();
        } else if (brakeNotch > 0) --brakeNotch;
        else if (powerNotch < maxPower()) ++powerNotch;
    };
    auto toBrake = [&] {
        if (powerNotch > 0) --powerNotch;
        else if (brakeNotch < maxBrake()) ++brakeNotch;
        else emergency = true;
    };
    // openBVE: POWER_* / BRAKE_* drive separate handles, SINGLE_* a one-handle (combined)
    // controller. The default bindings share keys (Z = POWER_INCREASE and SINGLE_POWER), so
    // each family only acts on its own handle type, or one key press would move two notches.
    const bool singleCmd = c == Command::SinglePower || c == Command::SingleNeutral || c == Command::SingleBrake;
    const bool separateCmd = c == Command::PowerIncrease || c == Command::PowerDecrease || c == Command::BrakeIncrease ||
                             c == Command::BrakeDecrease || c == Command::PowerFull || c == Command::BrakeFull ||
                             c == Command::PowerNeutral || c == Command::BrakeNeutral;
    if ((singleCmd && !oneHandle) || (separateCmd && oneHandle)) return;
    switch (c) {
    case Command::PowerIncrease:
        if (powerNotch < maxPower()) ++powerNotch;
        break;
    case Command::PowerDecrease:
        if (powerNotch > 0) --powerNotch;
        break;
    case Command::PowerFull: powerNotch = maxPower(); break;
    case Command::PowerNeutral: powerNotch = 0; break;
    case Command::BrakeIncrease:
        if (brakeNotch < maxBrake()) ++brakeNotch;
        else emergency = true;
        break;
    case Command::BrakeDecrease:
        if (emergency) {
            emergency = false;
            brakeNotch = maxBrake();
        } else if (brakeNotch > 0) --brakeNotch;
        break;
    case Command::BrakeFull:
        brakeNotch = maxBrake();
        if (oneHandle) powerNotch = 0;
        break;
    case Command::BrakeNeutral:
        brakeNotch = 0;
        emergency = false;
        break;
    case Command::BrakeEmergency:
    case Command::SingleEmergency:
        emergency = true;
        if (oneHandle || c == Command::SingleEmergency) powerNotch = 0;
        break;
    case Command::SinglePower: toPower(); break;
    case Command::SingleBrake: toBrake(); break;
    case Command::SingleNeutral:
        powerNotch = brakeNotch = 0;
        emergency = false;
        break;
    case Command::ReverserForward: reverser = std::min(1, reverser + 1); break;
    case Command::ReverserBackward: reverser = std::max(-1, reverser - 1); break;
    case Command::HoldBrake: holdBrake = !holdBrake; break;
    case Command::ConstSpeed: constSpeed = !constSpeed && def_.spec.constSpeed; break;
    case Command::DoorsLeft:
        if (std::abs(speed) < 0.1) leftDoorsTarget = !leftDoorsTarget;
        break;
    case Command::DoorsRight:
        if (std::abs(speed) < 0.1) rightDoorsTarget = !rightDoorsTarget;
        break;
    case Command::Headlights: headlights = !headlights; break;
    default: break;
    }
}

void TrainSim::stepHandles(double dt) {
    const TrainSpec& s = def_.spec;
    // power notch follows the (driver or safety system) handle with per-notch delays
    const int powerTarget = std::clamp(effectivePower(), 0, maxPower());
    if (powerTarget != powerActual) {
        powerTimer_ += dt;
        const bool up = powerTarget > powerActual;
        const double d = up ? delayFor(s.delayPowerUp, powerActual) : delayFor(s.delayPowerDown, powerActual - 1);
        if (powerTimer_ >= d) {
            powerActual += up ? 1 : -1;
            powerTimer_ = 0.0;
        }
    } else {
        powerTimer_ = 0.0;
    }
    const int brakeTarget = std::clamp(effectiveBrake(), 0, maxBrake() + 1);
    const bool eb = effectiveEmergency();
    if (brakeTarget != brakeActual) {
        brakeTimer_ += dt;
        const bool up = brakeTarget > brakeActual;
        const double d = up ? delayFor(s.delayBrakeUp, brakeActual) : delayFor(s.delayBrakeDown, brakeActual - 1);
        if (brakeTimer_ >= d || eb) {
            brakeActual = up ? (eb ? brakeTarget : brakeActual + 1) : brakeActual - 1;
            brakeTimer_ = 0.0;
        }
    } else {
        brakeTimer_ = 0.0;
    }
}

void TrainSim::update(double dt) {
    const TrainSpec& s = def_.spec;
    stepHandles(dt);

    // brake cylinder
    double bcTarget = 0.0;
    const bool eb = effectiveEmergency();
    const bool hold = holdBrake || (safetyActive && safetyHoldBrake);
    if (brakeActual > maxBrake() || eb) bcTarget = s.bcEmergencyMax;
    else if (maxBrake() > 0) bcTarget = s.bcServiceMax * double(brakeActual) / double(maxBrake());
    if (hold && brakeActual == 0 && powerActual == 0 && std::abs(speed) < 1.0) bcTarget = std::max(bcTarget, 0.2 * s.bcServiceMax);
    if (brakeCylinder < bcTarget) brakeCylinder = std::min(bcTarget, brakeCylinder + s.brakeCylinderUp * dt);
    else brakeCylinder = std::max(bcTarget, brakeCylinder - s.brakeCylinderDown * dt);
    brakePipe = s.brakePipeNormal - (s.brakePipeNormal * 0.3) * (bcTarget / std::max(1.0, s.bcEmergencyMax));
    straightAirPipe = bcTarget;
    // simple compressor model
    if (mainReservoir <= s.mainReservoirMin) compressor = true;
    else if (mainReservoir >= s.mainReservoirMax) compressor = false;
    mainReservoir = std::clamp(mainReservoir + (compressor ? 20.0 : -1.0) * dt, s.mainReservoirMin, s.mainReservoirMax);

    // motor: target from the power curve, limited by jerk
    const int rev = effectiveReverser();
    const bool powerAllowed = rev != 0 && powerActual > 0 && brakeActual == 0 && !eb && !hold;
    motorTarget_ = 0.0;
    if (powerAllowed && powerActual <= int(s.power.size())) motorTarget_ = s.power[std::size_t(powerActual - 1)].at(std::abs(speed));
    const double jerkUp = s.jerkPowerUp * 0.01 / 3.6, jerkDown = s.jerkPowerDown * 0.01 / 3.6;
    if (motorAcceleration < motorTarget_) motorAcceleration = std::min(motorTarget_, motorAcceleration + jerkUp * dt);
    else motorAcceleration = std::max(motorTarget_, motorAcceleration - jerkDown * dt);

    // forces (as accelerations of the whole train). openBVE scales each motor car's curve by
    // (1 + trailer mass / motor mass), so the train as a whole accelerates as train.dat says.
    double a = double(rev) * motorAcceleration;
    const TrackPose mid = route_.poseAt(front - 0.5 * length_);
    a -= mid.direction.y * kG; // gradient
    const double massKg = std::max(1.0, def_.totalMass());
    const double area = s.exposedArea + s.unexposedArea * std::max(0, int(def_.cars.size()) - 1);
    const double resist = s.rollingResistance * kG + s.aerodynamicDrag * area * kAirDensity * speed * speed / (2.0 * massKg);
    const double brake = s.deceleration * brakeCylinder / std::max(1.0, s.bcServiceMax);
    const double friction = resist + brake; // always opposes motion

    // integrate (friction cannot reverse the direction of motion)
    const double v0 = speed;
    if (std::abs(a) < friction && v0 == 0.0) {
        speed = 0.0;
    } else {
        double v = v0 + a * dt;
        const double f = friction * dt;
        if (v > 0) v = std::max(0.0, v - f);
        else if (v < 0) v = std::min(0.0, v + f);
        speed = v;
    }
    acceleration = dt > 0 ? (speed - v0) / dt : 0.0;
    const double ds = 0.5 * (v0 + speed) * dt;
    front += ds;
    odometer += std::abs(ds);
    front = std::clamp(front, length_ + 1.0, route_.endPosition() - 1.0);

    // doors open / close over ~3 s; opening is only possible while stopped
    auto door = [&](double& st, bool target) {
        const double rate = dt / 3.0;
        st = target ? std::min(1.0, st + rate) : std::max(0.0, st - rate);
    };
    if (std::abs(speed) > 0.1) leftDoorsTarget = rightDoorsTarget = false;
    door(leftDoors, leftDoorsTarget);
    door(rightDoors, rightDoorsTarget);
    updateCars();
}

void TrainSim::updateCars() {
    for (std::size_t i = 0; i < cars.size(); ++i) {
        const CarDef& d = def_.cars[i];
        const double centre = front - carOffsets_[i];
        CarPose& c = cars[i];
        c.frontAxle = centre + d.frontAxle;
        c.rearAxle = centre + d.rearAxle;
        const TrackPose pf = route_.poseAt(c.frontAxle), pr = route_.poseAt(c.rearAxle);
        const double span = d.frontAxle - d.rearAxle;
        glm::dvec3 z = pf.position - pr.position;
        const double len = glm::length(z);
        z = len > 1e-6 ? z / len : pf.direction;
        // car centre: between the axles, shifted for asymmetric axles
        const double t = span > 1e-6 ? (0.0 - d.rearAxle) / span : 0.5;
        c.position = pr.position + (pf.position - pr.position) * t;
        glm::dvec3 up = glm::normalize(pf.up + pr.up);
        // cant: roll around the direction of travel
        const double cant = 0.5 * (pf.cant + pr.cant);
        if (cant != 0.0 && route_.gauge > 0) {
            const double roll = std::asin(std::clamp(cant / route_.gauge, -0.5, 0.5));
            const glm::dvec3 side = glm::normalize(glm::cross(up, z));
            up = glm::normalize(up * std::cos(roll) + side * std::sin(roll));
        }
        c.X = glm::normalize(glm::cross(up, z));
        c.Y = glm::cross(z, c.X);
        c.Z = z;
    }
}

TrainSim::DriverView TrainSim::driverPose() const {
    const int dc = std::clamp(def_.spec.driverCar, 0, int(cars.size()) - 1);
    const CarPose& c = cars[std::size_t(dc)];
    const glm::dvec3 d = def_.cars[std::size_t(dc)].driver;
    return {c.position + c.X * d.x + c.Y * d.y + c.Z * d.z, c.X, c.Y, c.Z};
}

} // namespace ots
