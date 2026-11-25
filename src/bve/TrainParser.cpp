#include "bve/Train.hpp"

#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace ots {

double AccelerationCurve::at(double v) const {
    const double kmh = v * 3.6;
    double a;
    if (kmh <= 0.0) a = a0;
    else if (kmh < v1) a = a0 + (a1 - a0) * kmh / v1;
    else if (kmh < v2 || v2 <= 0.0) a = v1 > 0 ? v1 * a1 / kmh : a1;
    else a = v1 * a1 * std::pow(v2, e - 1.0) / std::pow(kmh, e);
    return a / 3.6;
}

double TrainDef::length() const {
    double l = 0.0;
    for (std::size_t i = 0; i < cars.size(); ++i) l += cars[i].length + (i + 1 < cars.size() ? cars[i].couplerGap : 0.0);
    return l;
}

double TrainDef::totalMass() const {
    double m = 0.0;
    for (const CarDef& c : cars) m += c.mass;
    return m * 1000.0;
}

namespace {

std::vector<double> numbers(std::string_view line) {
    std::vector<double> out;
    for (std::string_view p : split(line, ',')) out.push_back(parseDoubleVb6(p).value_or(0.0));
    return out;
}

void parseTrainDat(const std::string& text, TrainSpec& s) {
    std::string section;
    int row = 0;
    for (const std::string& raw : splitLines(text)) {
        std::string_view line = raw;
        if (auto sc = line.find(';'); sc != std::string_view::npos) line = line.substr(0, sc);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '#') {
            section = lower(trim(line.substr(1)));
            row = 0;
            continue;
        }
        const auto n = numbers(line);
        const double v = n.empty() ? 0.0 : n[0];
        if (section.starts_with("motor_")) {
            const int t = section == "motor_p1" ? 0 : section == "motor_p2" ? 1 : section == "motor_b1" ? 2 : section == "motor_b2" ? 3 : -1;
            if (t >= 0) {
                MotorSoundEntry m;
                if (const long v = n.empty() ? -1 : std::lround(n[0]); v >= 0) m.sound = int(v); // -1: silent
                if (n.size() >= 2) m.pitch = float(0.01 * std::max(0.0, n[1]));
                if (n.size() >= 3) m.gain = float(std::pow(0.0078125 * std::max(0.0, n[2]), 0.25));
                s.motorSound[std::size_t(t)].push_back(m);
            }
        } else if (section == "acceleration") {
            if (n.size() >= 4) s.power.push_back({n[0], n[1], n[2], n[3], n.size() >= 5 ? n[4] : 1.0});
        } else if (section == "performance") {
            switch (row) {
            case 0: s.deceleration = v / 3.6; break;
            case 1: s.staticFriction = v; break;
            case 3: s.rollingResistance = v; break;
            case 4: s.aerodynamicDrag = v; break;
            default: break;
            }
        } else if (section == "delay") {
            switch (row) {
            case 0: s.delayPowerUp = n; break;
            case 1: s.delayPowerDown = n; break;
            case 2: s.delayBrakeUp = n; break;
            case 3: s.delayBrakeDown = n; break;
            default: break;
            }
        } else if (section == "move") {
            switch (row) {
            case 0: s.jerkPowerUp = v; break;
            case 1: s.jerkPowerDown = v; break;
            case 2: s.jerkBrakeUp = v; break;
            case 3: s.jerkBrakeDown = v; break;
            case 4: s.brakeCylinderUp = v; break;
            case 5: s.brakeCylinderDown = v; break;
            default: break;
            }
        } else if (section == "brake") {
            if (row == 0) s.brakeType = BrakeType(std::clamp(int(v), 0, 2));
        } else if (section == "pressure") {
            switch (row) {
            case 0: s.bcServiceMax = v; break;
            case 1: s.bcEmergencyMax = v; break;
            case 2: s.mainReservoirMin = v; break;
            case 3: s.mainReservoirMax = v; break;
            case 4: s.brakePipeNormal = v; break;
            default: break;
            }
        } else if (section == "handle") {
            switch (row) {
            case 0: s.handleType = HandleType(std::clamp(int(v), 0, 2)); break;
            case 1: s.powerNotches = std::max(1, int(v)); s.driverPowerNotches = s.powerNotches; break;
            case 2: s.brakeNotches = std::max(0, int(v)); s.driverBrakeNotches = s.brakeNotches; break;
            case 3: s.powerNotchReduceSteps = int(v); break;
            case 7: if (v > 0) s.driverPowerNotches = int(v); break;
            case 8: if (v > 0) s.driverBrakeNotches = int(v); break;
            default: break;
            }
        } else if (section == "cab" || section == "cockpit") {
            if (row < 3) s.cab[row] = v;
            else if (row == 3) s.driverCar = int(v);
        } else if (section == "car") {
            switch (row) {
            case 0: s.motorCarMass = v; break;
            case 1: s.motorCars = std::max(0, int(v)); break;
            case 2: s.trailerCarMass = v; break;
            case 3: s.trailerCars = std::max(0, int(v)); break;
            case 4: s.carLength = v; break;
            case 5: s.frontCarMotor = v != 0; break;
            case 6: s.carWidth = v; break;
            case 7: s.carHeight = v; break;
            case 8: s.centerOfGravity = v; break;
            case 9: s.exposedArea = v; break;
            case 10: s.unexposedArea = v; break;
            default: break;
            }
        } else if (section == "device") {
            switch (row) {
            case 0: s.ats = int(v); break;
            case 1: s.atc = int(v); break;
            case 2: s.eb = v != 0; break;
            case 3: s.constSpeed = v != 0; break;
            case 4: s.holdBrake = v != 0; break;
            case 5: s.readhesion = v != 0; break;
            case 7: s.passAlarm = v != 0; break;
            case 10: s.doorWidth = v; break;
            default: break;
            }
        }
        ++row;
    }
    // automatic air brakes have no notches in train.dat (release / lap / service)
    if (s.brakeType == BrakeType::AutomaticAir && s.brakeNotches == 0) s.brakeNotches = s.driverBrakeNotches = 2;
    if (int(s.power.size()) < s.powerNotches) s.power.resize(std::size_t(s.powerNotches), s.power.empty() ? AccelerationCurve{} : s.power.back());
}

glm::dvec3 vec3(std::string_view v) {
    glm::dvec3 r(0.0);
    auto p = split(v, ',');
    for (std::size_t i = 0; i < 3 && i < p.size(); ++i) r[int(i)] = parseDoubleVb6(p[i]).value_or(r[int(i)]);
    return r;
}

} // namespace

Expected<TrainDef> loadTrain(const std::filesystem::path& folder, ObjectLibrary& lib) {
    TrainDef out;
    out.folder = folder;
    out.name = folder.filename().string();
    const std::filesystem::path dat = resolvePath(folder, "train.dat");
    const std::optional<std::string> datText = dat.empty() ? std::nullopt : readTextFile(dat);
    if (!datText) return Unexpected("train.dat not found in " + folder.string());
    parseTrainDat(*datText, out.spec);
    TrainSpec& s = out.spec;

    // cars from train.dat
    const int count = std::max(1, s.motorCars + s.trailerCars);
    out.cars.resize(std::size_t(count));
    // openBVE distributes motor cars evenly, starting with the front car if FrontCarIsAMotorCar
    {
        std::vector<bool> motor(std::size_t(count), false);
        if (s.motorCars > 0) {
            if (s.frontCarMotor || s.trailerCars == 0) {
                motor[0] = true;
                const double t = double(count) / s.motorCars;
                for (int i = 1; i < s.motorCars; ++i) motor[std::size_t(std::clamp(int(i * t + 0.5), 0, count - 1))] = true;
            } else {
                const double t = double(count) / s.motorCars;
                for (int i = 0; i < s.motorCars; ++i) motor[std::size_t(std::clamp(int((i + 0.5) * t), 0, count - 1))] = true;
            }
        }
        for (int i = 0; i < count; ++i) {
            CarDef& c = out.cars[std::size_t(i)];
            c.motor = motor[std::size_t(i)];
            c.mass = c.motor ? s.motorCarMass : s.trailerCarMass;
            c.length = s.carLength;
            c.frontAxle = 0.4 * s.carLength;
            c.rearAxle = -0.4 * s.carLength;
            c.couplerGap = 0.3;
        }
    }

    // extensions.cfg
    const std::filesystem::path ext = resolvePath(folder, "extensions.cfg");
    if (const std::optional<std::string> extText = ext.empty() ? std::nullopt : readTextFile(ext)) {
        std::string section;
        int index = -1;
        bool axlesGiven = false;
        for (const std::string& raw : splitLines(*extText)) {
            std::string_view line = raw;
            if (auto sc = line.find(';'); sc != std::string_view::npos) line = line.substr(0, sc);
            line = trim(line);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                const std::string name = lower(trim(line.substr(1, line.size() - 2)));
                section.clear();
                index = -1;
                axlesGiven = false;
                for (std::string_view prefix : {"car", "coupler", "bogie"})
                    if (name.starts_with(prefix))
                        if (const auto i = parseIntVb6(std::string_view(name).substr(prefix.size()))) section = prefix, index = *i;
                if (name == "exterior") section = "exterior";
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string_view val = trim(line.substr(eq + 1));
            if (section == "car" && index >= 0 && index < count) {
                CarDef& c = out.cars[std::size_t(index)];
                if (key == "object") {
                    std::filesystem::path p = resolvePath(folder, val);
                    if (p.empty()) out.warnings.push_back("car object not found: " + std::string(val));
                    else c.exterior = lib.intern(p);
                } else if (key == "length") {
                    if (const auto l = parseDoubleVb6(val); l && *l > 0) {
                        c.length = *l;
                        if (!axlesGiven) c.frontAxle = 0.4 * *l, c.rearAxle = -0.4 * *l;
                    }
                } else if (key == "axles") {
                    const auto p = split(val, ',');
                    const auto r = p.size() >= 2 ? parseDoubleVb6(p[0]) : std::nullopt, f = p.size() >= 2 ? parseDoubleVb6(p[1]) : std::nullopt;
                    if (r && f && *f > *r) {
                        c.rearAxle = *r;
                        c.frontAxle = *f;
                        axlesGiven = true;
                    }
                } else if (key == "reversed") {
                    c.reversed = iequals(val, "true") || val == "1";
                } else if (key == "driver") {
                    c.hasDriver = true;
                    c.driver = vec3(val);
                }
            } else if (section == "coupler" && index >= 0 && index + 1 < count) {
                if (key == "distances") {
                    const auto p = split(val, ',');
                    if (p.size() >= 2)
                        if (const auto mn = parseDoubleVb6(p[0]), mx = parseDoubleVb6(p[1]); mn && mx) out.cars[std::size_t(index)].couplerGap = 0.5 * (*mn + *mx);
                }
            } else if (section == "exterior") {
                const auto i = key.starts_with("car") ? parseIntVb6(std::string_view(key).substr(3)) : std::nullopt;
                if (i && *i >= 0 && *i < count)
                    if (const std::filesystem::path p = resolvePath(folder, val); !p.empty()) out.cars[std::size_t(*i)].exterior = lib.intern(p);
            }
        }
    }

    // driver
    s.driverCar = std::clamp(s.driverCar, 0, count - 1);
    CarDef& d = out.cars[std::size_t(s.driverCar)];
    if (!d.hasDriver) {
        d.hasDriver = true;
        // openBVE TrainDatParser: Z is measured from the front of a train.dat car (#CAR length),
        // even when extensions.cfg gives the driver car another length.
        d.driver = glm::dvec3(0.001 * s.cab.x, 0.001 * s.cab.y, 0.5 * s.carLength + 0.001 * s.cab.z);
    }
    // cab
    const std::filesystem::path panelAnimated = resolvePath(folder, "panel.animated");
    if (!panelAnimated.empty()) out.cab = lib.intern(panelAnimated);
    out.hasPanel2D = !resolvePath(folder, "panel2.cfg").empty() || !resolvePath(folder, "panel.cfg").empty();
    return out;
}

std::filesystem::path findTrainFolder(const std::filesystem::path& routeFile, const std::filesystem::path& trainHint, const std::string& name) {
    if (name.empty()) return {};
    if (!trainHint.empty()) {
        std::filesystem::path p = resolvePath(trainHint, name);
        if (!p.empty()) return p;
    }
    std::filesystem::path t = findUpwards(routeFile.parent_path(), "Train");
    if (!t.empty()) {
        std::filesystem::path p = resolvePath(t, name);
        if (!p.empty()) return p;
    }
    return {};
}

} // namespace ots
