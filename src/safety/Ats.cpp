#include "safety/Ats.hpp"

#include "sim/Train.hpp"

#include <cmath>
#include <format>

namespace ots::safety {

void Ats::activate(const Context& ctx) {
    // the code of the last signal passed: the one at the start of the section the train is in
    const auto s = ctx.track.sectionAt(ctx.train.front);
    code_ = s ? config_.codeFor(ctx.track.aspect(*s)) : config_.clearCode;
    alarm_ = acked_ = false;
    alarmTime_ = 0.0;
}

void Ats::update(const Context& ctx) {
    const TrainSim& t = ctx.train;
    speed_ = std::abs(t.speed);
    // ground coils at the signals passed since the last step
    if (t.front > ctx.previousFront) {
        const auto [first, last] = ctx.track.signalsBetween(ctx.previousFront, t.front);
        for (std::size_t i = first; i < last; ++i) {
            code_ = config_.codeFor(ctx.track.aspect(i));
            restricted_ = false;
            if (config_.codes[code_].stop && !emergency_) {
                emergency_ = true;
                reason_ = "정지신호 통과";
            }
        }
    }
    const AtsCode& code = config_.codes[code_];
    const double limit = restricted_ ? config_.afterStop : code.speed;
    const bool over = speed_ > limit + config_.tolerance;
    if (emergency_) {
        alarm_ = false;
    } else if (over) {
        if (!alarm_) alarm_ = true, acked_ = false, alarmTime_ = 0.0;
        alarmTime_ += ctx.dt;
        if (acked_ && t.brakeNotch < config_.minBrake && !t.emergency) acked_ = false, alarmTime_ = 0.0; // brake released while still fast
        if (!acked_ && alarmTime_ > config_.ackSeconds) {
            emergency_ = true;
            reason_ = "속도초과 미확인";
        }
    } else {
        alarm_ = acked_ = false;
    }

    command = {.emergency = emergency_};
    display = {};
    display.code = code.name;
    if (std::isfinite(limit)) display.permitted = limit;
    display.warning = alarm_ && !acked_;
    display.braking = display.emergency = emergency_;
    display.ackRequired = (alarm_ && !acked_) || (emergency_ && speed_ < 0.01);
    if (emergency_) display.message = speed_ < 0.01 ? reason_ + ": 확인으로 복귀" : reason_ + ": 비상제동";
    else if (alarm_ && !acked_) display.message = std::format("속도초과 {}: {:.0f}초 안에 확인", code.name, std::max(0.0, config_.ackSeconds - alarmTime_));
    else if (alarm_) display.message = "속도초과: 제동 유지";
}

void Ats::acknowledge(bool down) {
    if (!down) return;
    if (emergency_) {
        if (speed_ < 0.01) { // reset at a standstill
            emergency_ = alarm_ = acked_ = false;
            if (config_.codes[code_].stop) restricted_ = true;
        }
        return;
    }
    if (alarm_) acked_ = true;
}

} // namespace ots::safety
