#pragma once
// Korean ATP (KTCS-1 / ETCS Level 1 based). At each signal the balise group transmits the
// movement authority — up to the next signal showing stop (EoA) — and the speed profile
// ahead. Between balises nothing changes: a signal that clears meanwhile is only learned at
// its balise, so the train keeps braking towards the old end of authority (point
// transmission). Supervision (full supervision, FS):
//   permitted  ≤ ceiling (line speed) and the service braking curve with indication time
//   warning    over permitted
//   SBI        over the service curve (or ceiling + margin): full service brake until under permitted
//   EBI        over the emergency curve (or ceiling + margin): emergency brake until standstill
// Passing the EoA trips the train (emergency); after a reset at a standstill it runs in
// staff-responsible mode (SR, 40 km/h) up to the next balise.
#include "safety/Safety.hpp"

namespace ots::safety {

class Atp final : public Device {
public:
    explicit Atp(const AtpConfig& config) : Device(System::Atp), config_(config) {}
    void activate(const Context& ctx) override;
    void update(const Context& ctx) override;
    void acknowledge(bool down) override;

    enum class Mode : u8 { FullSupervision, StaffResponsible, Trip };
    // One supervised target: reach `speed` (m/s) at `position` (track m).
    struct Target {
        double position = 0.0, speed = 0.0;
        bool endOfAuthority = false;
    };
    // Speeds a target imposes at distance d before it (pure; ots_check prints them).
    struct Curves {
        double permitted, warning, service, emergency;
    };
    [[nodiscard]] static Curves curves(const AtpConfig& c, double serviceDecel, double emergencyDecel, double speed, double distance,
                                       double targetSpeed, bool endOfAuthority);

private:
    void receive(const Context& ctx); // balise: new authority and speed profile
    const AtpConfig& config_;
    Mode mode_ = Mode::FullSupervision;
    std::vector<Target> targets_;
    std::optional<double> eoa_;
    bool serviceBrake_ = false, emergency_ = false;
    double speed_ = 0.0;
    std::string reason_;
};

} // namespace ots::safety
