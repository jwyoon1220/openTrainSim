#pragma once
// TVM430 continuous cab signalling (Korean high-speed lines). The track circuits transmit,
// for the block the train is in, a speed step that depends on how many free blocks lie ahead
// (300 → 270 → 230 → 170 → 80 → 000) and announce the next block's step. Over the step plus a
// margin the emergency brake applies until the train is back under the step. In a 000 block
// the train must stop before its end (supervised with a braking curve); entering an occupied
// block is an emergency, released at a standstill with the acknowledge button.
#include "safety/Safety.hpp"

namespace ots::safety {

class Tvm430 final : public Device {
public:
    explicit Tvm430(const TvmConfig& config) : Device(System::Tvm430), config_(config) {}
    void activate(const Context& ctx) override;
    void update(const Context& ctx) override;
    void acknowledge(bool down) override;

private:
    [[nodiscard]] double stepFor(int freeBlocks) const;
    const TvmConfig& config_;
    bool emergency_ = false, overspeed_ = false, tripped_ = false;
    double speed_ = 0.0;
    std::string reason_;
};

} // namespace ots::safety
