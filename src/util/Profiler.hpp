#pragma once
// Frame profiler for the main thread: time per stage of the frame (smoothed) and a history
// of whole-frame times for the graph. Stages are charged with lap(): the time since the
// previous lap (or beginFrame) goes to the named stage.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace ots {

// Pace = the frame limiter's sleep (vsync), Wait = blocked on the GPU / the swapchain: both
// idle time, not counted as CPU work.
enum class CpuStage : std::uint8_t { Input, Simulation, Sound, Hud, Pace, Wait, Streaming, Gather, Build, Record, Submit };
inline constexpr std::size_t kCpuStages = 11;
inline constexpr std::array<std::string_view, kCpuStages> kCpuStageNames{"input", "sim", "sound", "hud", "pace", "wait",
                                                                         "stream", "gather", "build", "record", "submit"};
[[nodiscard]] constexpr bool isIdle(CpuStage s) { return s == CpuStage::Pace || s == CpuStage::Wait; }

class FrameProfiler {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kHistory = 240;

    // Ends the previous frame (its total goes into the history) and starts the next one.
    void beginFrame() {
        const auto now = Clock::now();
        if (started_) {
            const float frame = ms(now - frameStart_);
            history_[head_] = frame;
            head_ = (head_ + 1) % kHistory;
            count_ = std::min(count_ + 1, kHistory);
            frameMs_ += (frame - frameMs_) * kSmoothing;
            for (std::size_t i = 0; i < kCpuStages; ++i) smoothed_[i] += (current_[i] - smoothed_[i]) * kSmoothing;
        }
        current_ = {};
        started_ = true;
        frameStart_ = last_ = now;
    }
    // Charges the time since the previous lap to `stage` (ignored before the first frame).
    void lap(CpuStage stage) {
        const auto now = Clock::now();
        if (started_) current_[std::size_t(stage)] += ms(now - last_);
        last_ = now;
    }

    [[nodiscard]] float stageMs(CpuStage s) const { return smoothed_[std::size_t(s)]; }
    [[nodiscard]] float frameMs() const { return frameMs_; }
    [[nodiscard]] std::size_t samples() const { return count_; }
    // i-th recorded frame time (ms), oldest first, i < samples().
    [[nodiscard]] float history(std::size_t i) const { return history_[(head_ + kHistory - count_ + i) % kHistory]; }
    [[nodiscard]] float worstMs() const {
        float w = 0.0f;
        for (std::size_t i = 0; i < count_; ++i) w = std::max(w, history(i));
        return w;
    }
    // The 99th-percentile frame time over the history ("1% low").
    [[nodiscard]] float p99Ms() const {
        if (count_ == 0) return 0.0f;
        std::array<float, kHistory> v{};
        for (std::size_t i = 0; i < count_; ++i) v[i] = history(i);
        const auto k = std::size_t(0.99 * double(count_ - 1));
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.begin() + std::ptrdiff_t(count_));
        return v[k];
    }

private:
    static constexpr float kSmoothing = 0.05f; // ~20 frames
    static float ms(Clock::duration d) { return std::chrono::duration<float, std::milli>(d).count(); }
    Clock::time_point frameStart_{}, last_{};
    bool started_ = false;
    std::array<float, kCpuStages> current_{}, smoothed_{};
    std::array<float, kHistory> history_{};
    std::size_t head_ = 0, count_ = 0;
    float frameMs_ = 0.0f;
};

} // namespace ots
