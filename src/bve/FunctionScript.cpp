#include "bve/FunctionScript.hpp"

#include "util/Text.hpp"

#include <array>
#include <cmath>
#include <random>
#include <unordered_map>

namespace ots {
namespace {

const std::unordered_map<std::string, Var>& variableNames() {
    static const std::unordered_map<std::string, Var> m = {
        {"value", Var::Value}, {"delta", Var::Delta}, {"time", Var::Time}, {"hour", Var::Hour}, {"minute", Var::Minute},
        {"second", Var::Second}, {"cameradistance", Var::CameraDistance}, {"cameraxdistance", Var::CameraXDistance},
        {"cameraydistance", Var::CameraYDistance}, {"camerazdistance", Var::CameraZDistance}, {"cameramode", Var::CameraMode},
        {"cars", Var::Cars}, {"carnumber", Var::CarNumber}, {"speed", Var::Speed}, {"speedometer", Var::Speedometer},
        {"acceleration", Var::Acceleration}, {"accelerationmotor", Var::AccelerationMotor}, {"distance", Var::Distance},
        {"trackdistance", Var::TrackDistance}, {"curveradius", Var::CurveRadius}, {"curvecant", Var::CurveCant},
        {"pitch", Var::Pitch}, {"doors", Var::Doors}, {"leftdoors", Var::LeftDoors}, {"rightdoors", Var::RightDoors},
        {"leftdoorstarget", Var::LeftDoorsTarget}, {"rightdoorstarget", Var::RightDoorsTarget},
        {"leftdoorbutton", Var::LeftDoorButton}, {"rightdoorbutton", Var::RightDoorButton},
        {"reversernotch", Var::ReverserNotch}, {"powernotch", Var::PowerNotch}, {"powernotches", Var::PowerNotches},
        {"brakenotch", Var::BrakeNotch}, {"brakenotches", Var::BrakeNotches}, {"brakenotchlinear", Var::BrakeNotchLinear},
        {"brakenotcheslinear", Var::BrakeNotchesLinear}, {"locobrakenotch", Var::LocoBrakeNotch},
        {"locobrakenotches", Var::LocoBrakeNotches}, {"emergencybrake", Var::EmergencyBrake},
        {"hasairbrake", Var::HasAirBrake}, {"holdbrake", Var::HoldBrake}, {"hasholdbrake", Var::HasHoldBrake},
        {"constspeed", Var::ConstSpeed}, {"hasconstspeed", Var::HasConstSpeed}, {"hasplugin", Var::HasPlugin},
        {"pluginstate", Var::PluginState}, {"safety", Var::Safety}, {"mainreservoir", Var::MainReservoir}, {"emergencyreservoir", Var::EmergencyReservoir},
        {"brakepipe", Var::BrakePipe}, {"brakecylinder", Var::BrakeCylinder}, {"straightairpipe", Var::StraightAirPipe},
        {"equalizingreservoir", Var::EqualizingReservoir}, {"section", Var::Section}, {"klaxon", Var::Klaxon},
        {"primaryklaxon", Var::PrimaryKlaxon}, {"secondaryklaxon", Var::SecondaryKlaxon}, {"musicklaxon", Var::MusicKlaxon},
        {"passalarm", Var::PassAlarm}, {"pilotlamp", Var::PilotLamp}, {"stationadjustalarm", Var::StationAdjustAlarm},
        {"headlights", Var::Headlights}, {"wiperposition", Var::WiperPosition}, {"routelimit", Var::RouteLimit},
        {"nextstation", Var::NextStation}, {"nextstationstop", Var::NextStationStop}, {"terminalstation", Var::TerminalStation},
        {"distancenextstation", Var::DistanceNextStation}, {"stopsnextstation", Var::StopsNextStation},
        {"distancestation", Var::DistanceStation}, {"timetablevisible", Var::TimetableVisible}, {"brightness", Var::Brightness},
        {"frontaxlecurveradius", Var::FrontAxleCurveRadius}, {"rearaxlecurveradius", Var::RearAxleCurveRadius},
        {"pantographstate", Var::PantographState}, {"mainbreaker", Var::MainBreaker},
        // openBVE aliases
        {"leftdoorstate", Var::LeftDoors}, {"rightdoorstate", Var::RightDoors}, {"ats", Var::PluginState},
    };
    return m;
}

struct FnInfo {
    Function::Op op;
    int minArgs, maxArgs;
};
const std::unordered_map<std::string, FnInfo>& functionNames() {
    using O = Function::Op;
    static const std::unordered_map<std::string, FnInfo> m = {
        {"if", {O::If, 3, 3}}, {"min", {O::Min, 1, 64}}, {"max", {O::Max, 1, 64}}, {"abs", {O::Abs, 1, 1}},
        {"sign", {O::Sign, 1, 1}}, {"floor", {O::Floor, 1, 1}}, {"ceiling", {O::Ceiling, 1, 1}}, {"round", {O::Round, 1, 1}},
        {"mod", {O::Mod, 2, 2}}, {"sin", {O::Sin, 1, 1}}, {"cos", {O::Cos, 1, 1}}, {"tan", {O::Tan, 1, 1}},
        {"arctan", {O::ArcTan, 1, 1}}, {"exp", {O::Exp, 1, 1}}, {"log", {O::Log, 1, 1}}, {"sqrt", {O::Sqrt, 1, 1}},
        {"reciprocal", {O::Reciprocal, 1, 1}}, {"power", {O::Power, 2, 2}}, {"quotient", {O::Quotient, 2, 2}},
        {"random", {O::Random, 2, 2}}, {"randomint", {O::RandomInt, 2, 2}}, {"not", {O::Not, 1, 1}},
        {"plus", {O::Add, 2, 2}}, {"subtract", {O::Sub, 2, 2}}, {"times", {O::Mul, 2, 2}}, {"divide", {O::Div, 2, 2}},
        {"minus", {O::Neg, 1, 1}}, {"equal", {O::Eq, 2, 2}}, {"unequal", {O::Ne, 2, 2}}, {"less", {O::Lt, 2, 2}},
        {"greater", {O::Gt, 2, 2}}, {"lessequal", {O::Le, 2, 2}}, {"greaterequal", {O::Ge, 2, 2}}, {"and", {O::And, 2, 2}},
        {"or", {O::Or, 2, 2}}, {"xor", {O::Xor, 2, 2}},
    };
    return m;
}

} // namespace

class FunctionCompiler {
public:
    FunctionCompiler(std::string_view src, Function& fn) : s_(src), fn_(fn) {}

    bool run(std::string& err) {
        if (!parseOr()) {
            err = error_.empty() ? "syntax error" : error_;
            return false;
        }
        skip();
        if (p_ != s_.size()) {
            err = "unexpected '" + std::string(s_.substr(p_, 1)) + "'";
            return false;
        }
        return true;
    }

private:
    std::string_view s_;
    std::size_t p_ = 0;
    Function& fn_;
    std::string error_;

    void emit(Function::Op op, unsigned char argc = 0, unsigned short var = 0, double v = 0.0) { fn_.code_.push_back({op, argc, var, v}); }
    void skip() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t')) ++p_;
    }
    bool accept(std::string_view tok) {
        skip();
        if (s_.substr(p_, tok.size()) == tok) {
            p_ += tok.size();
            return true;
        }
        return false;
    }
    bool parseOr() {
        if (!parseXor()) return false;
        for (;;) {
            skip();
            if (p_ < s_.size() && s_[p_] == '|') {
                ++p_;
                if (!parseXor()) return false;
                emit(Function::Op::Or);
            } else return true;
        }
    }
    bool parseXor() {
        if (!parseAnd()) return false;
        for (;;) {
            if (accept("^")) {
                if (!parseAnd()) return false;
                emit(Function::Op::Xor);
            } else return true;
        }
    }
    bool parseAnd() {
        if (!parseNot()) return false;
        for (;;) {
            if (accept("&")) {
                if (!parseNot()) return false;
                emit(Function::Op::And);
            } else return true;
        }
    }
    bool parseNot() {
        skip();
        if (p_ < s_.size() && s_[p_] == '!' && (p_ + 1 >= s_.size() || s_[p_ + 1] != '=')) {
            ++p_;
            if (!parseNot()) return false;
            emit(Function::Op::Not);
            return true;
        }
        return parseCmp();
    }
    bool parseCmp() {
        // left-associative, as openBVE: "a == 0 < 1.3" is "(a == 0) < 1.3"
        if (!parseAdd()) return false;
        for (;;) {
            skip();
            Function::Op op;
            if (accept("==")) op = Function::Op::Eq;
            else if (accept("!=") || accept("<>")) op = Function::Op::Ne;
            else if (accept("<=")) op = Function::Op::Le;
            else if (accept(">=")) op = Function::Op::Ge;
            else if (accept("<")) op = Function::Op::Lt;
            else if (accept(">")) op = Function::Op::Gt;
            else if (accept("=")) op = Function::Op::Eq;
            else return true;
            if (!parseAdd()) return false;
            emit(op);
        }
    }
    bool parseAdd() {
        if (!parseMul()) return false;
        for (;;) {
            if (accept("+")) {
                if (!parseMul()) return false;
                emit(Function::Op::Add);
            } else if (accept("-")) {
                if (!parseMul()) return false;
                emit(Function::Op::Sub);
            } else return true;
        }
    }
    bool parseMul() {
        if (!parseUnary()) return false;
        for (;;) {
            if (accept("*")) {
                if (!parseUnary()) return false;
                emit(Function::Op::Mul);
            } else if (accept("/")) {
                if (!parseUnary()) return false;
                emit(Function::Op::Div);
            } else return true;
        }
    }
    bool parseUnary() {
        if (accept("-")) {
            if (!parseUnary()) return false;
            emit(Function::Op::Neg);
            return true;
        }
        if (accept("+")) return parseUnary();
        return parsePrimary();
    }
    bool parsePrimary() {
        skip();
        if (p_ >= s_.size()) {
            error_ = "unexpected end of expression";
            return false;
        }
        const char c = s_[p_];
        if (c == '(') {
            ++p_;
            if (!parseOr()) return false;
            if (!accept(")")) {
                error_ = "missing ')'";
                return false;
            }
            return true;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            std::size_t e = p_;
            while (e < s_.size() && ((s_[e] >= '0' && s_[e] <= '9') || s_[e] == '.' || s_[e] == 'e' || s_[e] == 'E' ||
                                     ((s_[e] == '-' || s_[e] == '+') && e > p_ && (s_[e - 1] == 'e' || s_[e - 1] == 'E'))))
                ++e;
            const auto v = parseDouble(s_.substr(p_, e - p_));
            if (!v) {
                error_ = "invalid number " + std::string(s_.substr(p_, e - p_));
                return false;
            }
            p_ = e;
            emit(Function::Op::Const, 0, 0, *v);
            return true;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t e = p_;
            while (e < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[e])) || s_[e] == '_')) ++e;
            const std::string name = lower(s_.substr(p_, e - p_));
            p_ = e;
            skip();
            const bool bracket = p_ < s_.size() && s_[p_] == '[';
            auto fit = functionNames().find(name);
            if (bracket && fit != functionNames().end()) {
                ++p_;
                int argc = 0;
                skip();
                if (!(p_ < s_.size() && s_[p_] == ']')) {
                    for (;;) {
                        if (!parseOr()) return false;
                        ++argc;
                        if (accept(",")) continue;
                        break;
                    }
                }
                if (!accept("]")) {
                    error_ = "missing ']' after " + name;
                    return false;
                }
                if (argc < fit->second.minArgs || argc > fit->second.maxArgs) {
                    error_ = "wrong argument count for " + name;
                    return false;
                }
                emit(fit->second.op, static_cast<unsigned char>(argc));
                return true;
            }
            auto vit = variableNames().find(name);
            if (vit == variableNames().end()) {
                // Unknown variables evaluate to 0 (openBVE reports an error and disables the
                // function; staying permissive keeps the rest of the object working).
                if (bracket) {
                    ++p_;
                    if (!parseOr()) return false;
                    if (!accept("]")) return false;
                    emit(Function::Op::Const, 0, 0, 0.0); // index * 0
                    emit(Function::Op::Mul);
                    return true;
                }
                emit(Function::Op::Const, 0, 0, 0.0);
                return true;
            }
            if (bracket) {
                ++p_;
                if (!parseOr()) return false;
                if (!accept("]")) {
                    error_ = "missing ']'";
                    return false;
                }
                emit(Function::Op::VarIdx, 0, static_cast<unsigned short>(vit->second));
            } else {
                emit(Function::Op::Var, 0, static_cast<unsigned short>(vit->second));
            }
            return true;
        }
        error_ = std::string("unexpected '") + c + "'";
        return false;
    }
};

std::shared_ptr<Function> Function::compile(std::string_view infix, std::string* error) {
    auto fn = std::make_shared<Function>();
    fn->source_ = std::string(trim(infix));
    std::string err;
    FunctionCompiler c(fn->source_, *fn);
    if (fn->source_.empty() || !c.run(err)) {
        if (error) *error = err.empty() ? "empty expression" : err;
        return nullptr;
    }
    fn->constant_ = fn->code_.size() == 1 && fn->code_[0].op == Op::Const;
    return fn;
}

double Function::evaluate(const FunctionContext& ctx, double delta) {
    std::array<double, 64> stack; // a malformed program reads 0 instead of underflowing
    std::size_t sp = 0;
    auto pop = [&]() -> double { return sp > 0 ? stack[--sp] : 0.0; };
    auto push = [&](double v) {
        if (sp < stack.size()) stack[sp++] = v;
    };
    static thread_local std::mt19937 rng{12345u};
    delta_ = delta;
    for (const Instr& in : code_) {
        switch (in.op) {
        case Op::Const: push(in.value); break;
        case Op::Var:
            if (Var(in.var) == Var::Value) push(lastResult);
            else if (Var(in.var) == Var::Delta) push(delta_);
            else push(ctx.variable(Var(in.var), -1));
            break;
        case Op::VarIdx: {
            const int idx = int(std::lround(pop()));
            push(ctx.variable(Var(in.var), idx));
            break;
        }
        case Op::Neg: push(-pop()); break;
        case Op::Not: push(pop() == 0.0 ? 1.0 : 0.0); break;
        case Op::Add: { double b = pop(), a = pop(); push(a + b); break; }
        case Op::Sub: { double b = pop(), a = pop(); push(a - b); break; }
        case Op::Mul: { double b = pop(), a = pop(); push(a * b); break; }
        case Op::Div: { double b = pop(), a = pop(); push(b == 0.0 ? 0.0 : a / b); break; }
        case Op::Eq: { double b = pop(), a = pop(); push(a == b ? 1.0 : 0.0); break; }
        case Op::Ne: { double b = pop(), a = pop(); push(a != b ? 1.0 : 0.0); break; }
        case Op::Lt: { double b = pop(), a = pop(); push(a < b ? 1.0 : 0.0); break; }
        case Op::Gt: { double b = pop(), a = pop(); push(a > b ? 1.0 : 0.0); break; }
        case Op::Le: { double b = pop(), a = pop(); push(a <= b ? 1.0 : 0.0); break; }
        case Op::Ge: { double b = pop(), a = pop(); push(a >= b ? 1.0 : 0.0); break; }
        case Op::And: { double b = pop(), a = pop(); push(a != 0.0 && b != 0.0 ? 1.0 : 0.0); break; }
        case Op::Or: { double b = pop(), a = pop(); push(a != 0.0 || b != 0.0 ? 1.0 : 0.0); break; }
        case Op::Xor: { double b = pop(), a = pop(); push((a != 0.0) != (b != 0.0) ? 1.0 : 0.0); break; }
        case Op::If: { double c = pop(), b = pop(), a = pop(); push(a != 0.0 ? b : c); break; }
        case Op::Min:
        case Op::Max: {
            double r = pop();
            for (int i = 1; i < in.argc; ++i) {
                const double v = pop();
                r = in.op == Op::Min ? std::min(r, v) : std::max(r, v);
            }
            push(r);
            break;
        }
        case Op::Abs: push(std::abs(pop())); break;
        case Op::Sign: { double a = pop(); push(a > 0 ? 1.0 : (a < 0 ? -1.0 : 0.0)); break; }
        case Op::Floor: push(std::floor(pop())); break;
        case Op::Ceiling: push(std::ceil(pop())); break;
        case Op::Round: push(std::nearbyint(pop())); break;
        case Op::Mod: { double b = pop(), a = pop(); push(b == 0.0 ? 0.0 : a - b * std::floor(a / b)); break; }
        case Op::Sin: push(std::sin(pop())); break;
        case Op::Cos: push(std::cos(pop())); break;
        case Op::Tan: push(std::tan(pop())); break;
        case Op::ArcTan: push(std::atan(pop())); break;
        case Op::Exp: push(std::exp(pop())); break;
        case Op::Log: { double a = pop(); push(a > 0 ? std::log(a) : 0.0); break; }
        case Op::Sqrt: { double a = pop(); push(a > 0 ? std::sqrt(a) : 0.0); break; }
        case Op::Reciprocal: { double a = pop(); push(a == 0 ? 0.0 : 1.0 / a); break; }
        case Op::Power: { double b = pop(), a = pop(); push(a == 0 && b < 0 ? 0.0 : std::pow(a, b)); break; }
        case Op::Quotient: { double b = pop(), a = pop(); push(b == 0 ? 0.0 : std::floor(a / b)); break; }
        case Op::Random: {
            double b = pop(), a = pop();
            push(a + (b - a) * std::uniform_real_distribution<double>(0.0, 1.0)(rng));
            break;
        }
        case Op::RandomInt: {
            int b = int(pop()), a = int(pop());
            if (b < a) std::swap(a, b);
            push(double(std::uniform_int_distribution<int>(a, b)(rng)));
            break;
        }
        }
    }
    double r = sp > 0 ? stack[sp - 1] : 0.0;
    if (!std::isfinite(r)) r = 0.0;
    lastResult = r;
    return r;
}

} // namespace ots
