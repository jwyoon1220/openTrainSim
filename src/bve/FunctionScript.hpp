#pragma once
// openBVE function scripts (infix notation, as used by .animated files):
//   if[speed > 0, 1, 0] * pluginState[73] + sin[time]
// compiled once to a small stack program and evaluated against a context that supplies the
// train / route variables.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ots {

enum class Var : unsigned short {
    Value, Delta, Time, Hour, Minute, Second,
    CameraDistance, CameraXDistance, CameraYDistance, CameraZDistance, CameraMode,
    Cars, CarNumber, Speed, Speedometer, Acceleration, AccelerationMotor,
    Distance, TrackDistance, CurveRadius, CurveCant, Pitch,
    Doors, LeftDoors, RightDoors, LeftDoorsTarget, RightDoorsTarget, LeftDoorButton, RightDoorButton,
    ReverserNotch, PowerNotch, PowerNotches, BrakeNotch, BrakeNotches, BrakeNotchLinear, BrakeNotchesLinear,
    LocoBrakeNotch, LocoBrakeNotches,
    EmergencyBrake, HasAirBrake, HoldBrake, HasHoldBrake, ConstSpeed, HasConstSpeed, HasPlugin, PluginState,
    MainReservoir, EmergencyReservoir, BrakePipe, BrakeCylinder, StraightAirPipe, EqualizingReservoir,
    Section, Klaxon, PrimaryKlaxon, SecondaryKlaxon, MusicKlaxon, PassAlarm, PilotLamp, StationAdjustAlarm,
    Headlights, WiperPosition, RouteLimit, NextStation, NextStationStop, TerminalStation, DistanceNextStation,
    StopsNextStation, DistanceStation, TimetableVisible, Brightness, FrontAxleCurveRadius, RearAxleCurveRadius,
    PantographState, MainBreaker, Count
};

class FunctionContext {
public:
    virtual ~FunctionContext() = default;
    // index = -1 when the script used no [index]
    virtual double variable(Var v, int index) const = 0;
};

class Function {
public:
    // Returns null (and an error message) when the expression cannot be compiled.
    static std::shared_ptr<Function> compile(std::string_view infix, std::string* error = nullptr);
    double evaluate(const FunctionContext& ctx, double delta);
    double lastResult = 0.0;
    [[nodiscard]] const std::string& source() const { return source_; }
    [[nodiscard]] bool isConstant() const { return constant_; }

    enum class Op : unsigned char {
        Const, Var, VarIdx, Neg, Not, Add, Sub, Mul, Div, Eq, Ne, Lt, Gt, Le, Ge, And, Or, Xor,
        If, Min, Max, Abs, Sign, Floor, Ceiling, Round, Mod, Sin, Cos, Tan, ArcTan, Exp, Log, Sqrt,
        Reciprocal, Power, Quotient, Random, RandomInt
    };
    struct Instr {
        Op op;
        unsigned char argc = 0;
        unsigned short var = 0;
        double value = 0.0;
    };

private:
    std::vector<Instr> code_;
    std::string source_;
    bool constant_ = false;
    double delta_ = 0.0;
    friend class FunctionCompiler;
};

} // namespace ots
