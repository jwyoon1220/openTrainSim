#pragma once
// Korean conventional-line ATS (KORAIL point-type ATS): a code is picked up from the ground
// coil at each signal and held until the next one. Over the code speed the alarm sounds; the
// driver must acknowledge within a few seconds and keep braking until under the speed, or
// the emergency brake applies. Passing a signal at stop (R0) is an immediate emergency. An
// emergency is released only standing still, with the acknowledge button; after passing a stop
// signal the train may then go on at the restricted speed (15 km/h) up to the next signal.
#include "safety/Safety.hpp"

namespace ots::safety {

class Ats final : public Device {
public:
    explicit Ats(const AtsConfig& config) : Device(System::Ats), config_(config) {}
    void activate(const Context& ctx) override;
    void update(const Context& ctx) override;
    void acknowledge(bool down) override;
    [[nodiscard]] std::size_t code() const { return code_; }

private:
    const AtsConfig& config_;
    std::size_t code_ = 0;
    bool alarm_ = false, acked_ = false, emergency_ = false;
    bool restricted_ = false; // passed a stop signal and reset: afterStop speed until the next signal
    double alarmTime_ = 0.0;
    double speed_ = 0.0;
    std::string reason_;
};

} // namespace ots::safety
