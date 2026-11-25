#include "safety/Atp.hpp"

#include "sim/Train.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace ots::safety {

Atp::Curves Atp::curves(const AtpConfig& c, double as, double ae, double speed, double distance, double vt, bool eoa) {
    const double react = speed * c.brakeDelay;
    auto reach = [&](double decel, double dist) { return std::sqrt(vt * vt + 2.0 * decel * std::max(0.0, dist)); };
    Curves k{reach(as, distance - react - speed * c.indicationTime), 0.0, reach(as, distance - react), reach(ae, distance - react)};
    k.warning = 0.5 * (k.permitted + k.service);
    if (eoa) // release speed: the train may always creep up to the end of authority
        for (double* v : {&k.permitted, &k.warning, &k.service, &k.emergency}) *v = std::max(*v, c.releaseSpeed);
    return k;
}

void Atp::receive(const Context& ctx) {
    const double front = ctx.train.front;
    targets_.clear();
    eoa_.reset();
    if (const auto stop = ctx.track.nextStop(front); stop && ctx.track.signalPosition(*stop) - front < config_.lookahead) {
        eoa_ = ctx.track.signalPosition(*stop);
        targets_.push_back({*eoa_, 0.0, true});
    }
    // speed profile: every restriction ahead within the authority
    const double horizon = std::min(front + config_.lookahead, eoa_.value_or(front + config_.lookahead));
    for (const SpeedLimit& l : ctx.track.route().limits)
        if (l.trackPos > front && l.trackPos < horizon && std::isfinite(l.speed)) targets_.push_back({l.trackPos, l.speed, false});
}

void Atp::activate(const Context& ctx) {
    mode_ = Mode::FullSupervision;
    serviceBrake_ = emergency_ = false;
    receive(ctx);
}

void Atp::update(const Context& ctx) {
    const TrainSim& t = ctx.train;
    speed_ = std::abs(t.speed);
    // balise groups at the signals passed
    if (t.front > ctx.previousFront) {
        const auto [first, last] = ctx.track.signalsBetween(ctx.previousFront, t.front);
        if (first < last && mode_ != Mode::Trip) {
            if (mode_ == Mode::FullSupervision && ctx.track.isStop(last - 1)) { // a signal at stop: its own balise trips the train
                mode_ = Mode::Trip, emergency_ = true, reason_ = "정지신호 통과 (TR)";
            } else {
                receive(ctx);
                mode_ = Mode::FullSupervision;
            }
        }
    }
    if (mode_ == Mode::FullSupervision && eoa_ && t.front > *eoa_ + 1.0) mode_ = Mode::Trip, emergency_ = true, reason_ = "이동권한 초과 (TR)";

    const double serviceMax = t.def().spec.deceleration;
    const double as = config_.serviceDecel > 0 ? config_.serviceDecel : 0.8 * serviceMax;
    const double ae = config_.emergencyDecel > 0 ? config_.emergencyDecel : 1.1 * serviceMax;
    const double ceiling = mode_ == Mode::StaffResponsible ? std::min(config_.staffResponsible, ctx.track.lineSpeed(t.front)) : ctx.track.lineSpeed(t.front);
    Curves sup{ceiling, ceiling + config_.ceilingWarning, ceiling + config_.ceilingService, ceiling + config_.ceilingEmergency};
    const Target* most = nullptr; // the target that limits the permitted speed most
    if (mode_ == Mode::FullSupervision)
        for (const Target& g : targets_) {
            const double d = g.position - t.front;
            if (d < -1.0 || g.speed >= ceiling) continue;
            const Curves k = curves(config_, as, ae, speed_, d, g.speed, g.endOfAuthority);
            if (k.permitted < sup.permitted) most = &g;
            sup.permitted = std::min(sup.permitted, k.permitted), sup.warning = std::min(sup.warning, k.warning);
            sup.service = std::min(sup.service, k.service), sup.emergency = std::min(sup.emergency, k.emergency);
        }

    if (mode_ != Mode::Trip) {
        if (speed_ > sup.emergency && !emergency_) emergency_ = true, reason_ = "비상제동 곡선 초과";
        if (speed_ > sup.service) serviceBrake_ = true;
        else if (speed_ <= sup.permitted) serviceBrake_ = false;
    }
    const bool warning = !emergency_ && speed_ > sup.permitted + 0.3;

    command = {.brake = serviceBrake_ && !emergency_ ? 1000 : 0, .emergency = emergency_};
    display = {};
    display.code = mode_ == Mode::FullSupervision ? "FS" : mode_ == Mode::StaffResponsible ? "SR" : "TR";
    display.permitted = sup.permitted;
    display.intervention = sup.service;
    if (most) display.target = most->speed, display.targetDistance = std::max(0.0, most->position - t.front);
    else if (mode_ == Mode::FullSupervision && eoa_) display.target = 0.0, display.targetDistance = std::max(0.0, *eoa_ - t.front);
    display.warning = warning;
    display.braking = serviceBrake_ || emergency_;
    display.emergency = emergency_;
    display.ackRequired = emergency_ && speed_ < 0.01;
    if (emergency_) display.message = speed_ < 0.01 ? reason_ + ": 확인으로 복귀" : reason_ + ": 비상제동";
    else if (serviceBrake_) display.message = "상용제동 개입";
    else if (warning) display.message = "속도초과: 감속";
    else if (mode_ == Mode::StaffResponsible) display.message = "SR: 다음 발리스까지 제한 운전";
}

void Atp::acknowledge(bool down) {
    if (!down || !emergency_ || speed_ >= 0.01) return;
    emergency_ = serviceBrake_ = false;
    if (mode_ == Mode::Trip) mode_ = Mode::StaffResponsible, targets_.clear(), eoa_.reset();
}

} // namespace ots::safety
