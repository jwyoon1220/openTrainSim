#include "safety/Tvm430.hpp"

#include "sim/Train.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace ots::safety {

double Tvm430::stepFor(int freeBlocks) const {
    const int last = int(config_.steps.size()) - 1;
    return config_.steps[std::size_t(std::clamp(last - freeBlocks, 0, last))];
}

void Tvm430::activate(const Context&) { emergency_ = overspeed_ = tripped_ = false; }

void Tvm430::update(const Context& ctx) {
    const TrainSim& t = ctx.train;
    speed_ = std::abs(t.speed);
    const int cap = int(config_.steps.size());
    const auto block = ctx.track.sectionAt(t.front);
    // entering an occupied block (the train passed the end of a 000 block)
    if (t.front > ctx.previousFront) {
        const auto [first, last] = ctx.track.signalsBetween(ctx.previousFront, t.front);
        for (std::size_t i = first; i < last; ++i)
            if (ctx.track.isStop(i) && !emergency_) emergency_ = tripped_ = true, reason_ = "폐색 진입 (000)";
    }
    // step of this block, announced step of the next one; both within the line speed
    const int free = block ? ctx.track.freeBlocksAfter(*block, cap) : cap;
    const double line = ctx.track.lineSpeed(t.front);
    double step = std::min(stepFor(free), line);
    const double next = std::min(stepFor(std::max(free - 1, 0)), block ? ctx.track.lineSpeed(ctx.track.blockEnd(*block)) : line);
    const bool final = free == 0;
    double supervised = step;
    const double toEnd = block ? ctx.track.blockEnd(*block) - t.front : 0.0;
    if (final) // 000: braking curve to the block end, never below a crawl to reach it
        supervised = std::max(std::sqrt(2.0 * config_.finalDecel * std::max(0.0, toEnd)), 15 / 3.6);
    const bool occupied = block && ctx.track.isStop(*block); // inside an occupied block: on sight (RRR)
    if (occupied) step = supervised = std::min(30 / 3.6, line);

    if (!tripped_) {
        if (speed_ > supervised + config_.emergencyMargin) emergency_ = true, reason_ = "속도초과";
        else if (emergency_ && speed_ < supervised) emergency_ = false; // released under the step
    }
    overspeed_ = !emergency_ && speed_ > supervised + config_.warningMargin;

    command = {.emergency = emergency_};
    display = {};
    display.code = occupied ? "RRR" : final ? "000" : std::format("{:.0f}", step * 3.6);
    display.permitted = supervised;
    display.intervention = supervised + config_.emergencyMargin;
    if (next < step - 0.1 || final) {
        display.target = final ? 0.0 : next;
        display.targetDistance = std::max(0.0, toEnd);
    }
    display.warning = overspeed_;
    display.braking = display.emergency = emergency_;
    display.ackRequired = tripped_ && speed_ < 0.01;
    if (emergency_) display.message = tripped_ ? (speed_ < 0.01 ? reason_ + ": 확인으로 복귀" : reason_ + ": 비상제동") : reason_ + ": 비상제동";
    else if (overspeed_) display.message = "속도초과: 감속";
    else if (display.target) display.message = std::format("다음 폐색 {}", final ? std::string("정지") : std::format("{:.0f}", next * 3.6));
}

void Tvm430::acknowledge(bool down) {
    if (down && tripped_ && speed_ < 0.01) emergency_ = tripped_ = false;
}

} // namespace ots::safety
