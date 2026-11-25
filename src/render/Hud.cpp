#include "render/Hud.hpp"

#include "util/Text.hpp"

#include "phx/ui/Font.hpp"

#include <algorithm>
#include <array>
#include <numbers>
#include <cmath>
#include <format>

namespace ots {
namespace {

std::string clock(std::optional<double> time) {
    if (!time || *time < 0) return "--:--:--";
    const double t = std::fmod(*time + 86400.0, 86400.0);
    const int s = int(t);
    return std::format("{:02}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
}

std::string notch(const TrainSim& t) {
    std::string p = t.powerNotch > 0 ? std::format("P{}", t.powerNotch) : "N";
    std::string b = t.emergency ? "EB" : (t.brakeNotch > 0 ? std::format("B{}", t.brakeNotch) : "N");
    if (t.def().spec.handleType == HandleType::OneHandle) return t.emergency ? "EB" : (t.brakeNotch > 0 ? b : p);
    return p + " / " + b;
}

// Box in the top-right corner: frame summary, frame-time graph, stacked CPU / GPU bars with
// their legends. Drawn in a smaller font than the driving HUD. Returns its bottom edge.
f32 drawProfiler(std::vector<phx::ui::Vertex>& out, const phx::ui::Theme& baseTheme, const FrameProfiler& fp, const RenderStats& st,
                 f32 fps, f32 w) {
    using phx::ui::rgba;
    phx::ui::Theme theme = baseTheme;
    theme.textScale = std::max(1.0f, baseTheme.textScale * 0.6f);
    const std::size_t panelAt = out.size(); // the background goes under the content, sized afterwards
    phx::ui::Painter p(out, theme, {0, 0, w, 10000.0f});
    const f32 lh = p.lineHeight() + 4.0f, pad = 8.0f;
    const u32 panel = rgba(10, 12, 16, 170), dim = rgba(150, 158, 170), text = rgba(225, 230, 236);
    const u32 good = rgba(110, 220, 120), mid = rgba(240, 200, 80), bad = rgba(250, 90, 70);
    // one colour per CPU stage (game stages warm, renderer stages cool, pace / wait grey)
    constexpr std::array<u32, kCpuStages> cpuColors{rgba(255, 170, 90), rgba(250, 120, 110), rgba(230, 120, 200), rgba(200, 160, 255),
                                                    rgba(70, 74, 82), rgba(110, 115, 125), rgba(120, 200, 255), rgba(90, 220, 190), rgba(150, 230, 110),
                                                    rgba(240, 220, 100), rgba(120, 150, 255)};
    constexpr std::array<u32, kGpuPassNames.size()> gpuColors{rgba(140, 140, 220), rgba(90, 200, 255), rgba(120, 220, 160), rgba(240, 210, 100),
                                                               rgba(250, 150, 90), rgba(220, 130, 220), rgba(170, 175, 185)};
    const f32 boxW = std::clamp(w * 0.3f, 360.0f, 560.0f);
    const f32 x0 = w - boxW - 8.0f, y0 = 8.0f, left = x0 + pad, right = x0 + boxW - pad, inner = right - left;
    auto frameColour = [&](float ms) { return ms <= 1000.0f / 60.0f + 0.5f ? good : ms <= 33.4f ? mid : bad; };

    // summary
    float cpuSum = 0.0f, gpuSum = 0.0f;
    for (std::size_t i = 0; i < kCpuStages; ++i)
        if (!isIdle(CpuStage(i))) cpuSum += fp.stageMs(CpuStage(i));
    for (const f32 v : st.gpuMs) gpuSum += v;
    const float frame = fp.frameMs(), worst = fp.worstMs();
    f32 y = y0 + pad;
    // with frame generation: rendered fps × presented per frame = what the display shows
    const std::string rate = st.presentedPerFrame > 1 ? std::format("{:.0f} fps (FG {}x = {:.0f})", fps, st.presentedPerFrame, fps * f32(st.presentedPerFrame))
                                                      : std::format("{:.0f} fps", fps);
    // frame colour judged per presented frame: 12 ms rendered at FG 2x is 165 shown
    p.drawText(left, y, std::format("{}   {:.2f} ms   p99 {:.1f}   max {:.1f}", rate, frame, fp.p99Ms(), worst),
               frameColour(frame / f32(st.presentedPerFrame)));
    y += lh;
    std::string jobs;
    for (std::size_t q : st.jobs.queued) jobs += std::format("{}{}", jobs.empty() ? "" : "/", q);
    p.drawText(left, y, std::format("CPU {:.2f}  GPU {}  jobs {}  run {}/{}", cpuSum, st.gpuTimed ? std::format("{:.2f}", gpuSum) : std::string("-"), jobs,
                                    st.jobs.running, st.jobs.workers),
               dim);
    y += lh + 2.0f;

    // frame-time graph, newest on the right
    const float scale = worst > 33.4f ? 66.7f : 33.4f; // ms at the top
    const f32 gh = 3.0f * lh;
    p.fillRect({left, y, inner, gh}, rgba(0, 0, 0, 90));
    const f32 colW = inner / f32(FrameProfiler::kHistory);
    for (std::size_t i = 0; i < fp.samples(); ++i) {
        const float v = fp.history(i), hgt = std::min(1.0f, v / scale) * gh;
        p.fillRect({left + inner - f32(fp.samples() - i) * colW, y + gh - hgt, std::max(1.0f, colW), hgt}, frameColour(v));
    }
    for (const float line : {1000.0f / 60.0f, 33.3f}) // 60 / 30 fps
        if (line < scale) p.fillRect({left, y + gh - line / scale * gh, inner, 1.0f}, rgba(255, 255, 255, 70));
    y += gh + 6.0f;

    // stacked bars (full width = the graph scale) with wrapped legends
    const f32 barX = left + p.textWidth("GPU "), barW = right - barX;
    auto bar = [&](std::string_view label, auto&& values, auto&& names, auto&& colors, std::size_t n) {
        p.drawText(left, y, label, text);
        p.fillRect({barX, y + 2.0f, barW, lh - 6.0f}, rgba(0, 0, 0, 90));
        f32 bx = barX;
        for (std::size_t i = 0; i < n; ++i) {
            const f32 sw = std::min(barX + barW - bx, values(i) / scale * barW);
            if (sw <= 0.0f) continue;
            p.fillRect({bx, y + 2.0f, sw, lh - 6.0f}, colors[i]);
            bx += sw;
        }
        p.fillRect({barX + 1000.0f / 60.0f / scale * barW, y, 1.0f, lh - 2.0f}, rgba(255, 255, 255, 120)); // 60 fps budget
        y += lh;
        f32 lx = barX;
        bool any = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (values(i) < 0.01f) continue;
            const std::string item = std::format("{} {:.2f}", names[i], values(i));
            const f32 iw = p.textWidth(item);
            if (lx + iw > right && lx > barX) lx = barX, y += lh; // wrap
            p.drawText(lx, y, item, colors[i]);
            lx += iw + p.textWidth("  ");
            any = true;
        }
        if (any) y += lh;
    };
    bar("CPU", [&](std::size_t i) { return fp.stageMs(CpuStage(i)); }, kCpuStageNames, cpuColors, kCpuStages);
    if (st.gpuTimed) bar("GPU", [&](std::size_t i) { return st.gpuMs[i]; }, kGpuPassNames, gpuColors, kGpuPassNames.size());
    else p.drawText(left, y, "GPU  timestamps not supported", dim), y += lh;

    const f32 bottom = y + pad - 4.0f;
    std::vector<phx::ui::Vertex> bg;
    phx::ui::Painter(bg, theme, {0, 0, w, 10000.0f}).fillRoundedRect({x0, y0, boxW, bottom - y0}, 8.0f, panel);
    out.insert(out.begin() + std::ptrdiff_t(panelAt), bg.begin(), bg.end());
    return bottom;
}

// Colour of an ATS code / TVM step lamp.
u32 codeColour(std::string_view code) {
    using phx::ui::rgba;
    if (code.starts_with("R") || code == "000" || code == "TR") return rgba(240, 60, 55);
    if (code == "SR") return rgba(250, 190, 50);
    if (code == "FS") return rgba(110, 170, 255);
    if (code == "YG") return rgba(190, 220, 60);
    if (code.starts_with("Y")) return rgba(250, 200, 50);
    if (code.starts_with("G")) return rgba(70, 220, 120);
    return rgba(225, 230, 236);
}

// Driver–machine interface of the built-in train protection, bottom left: circular
// speedometer with the permitted-speed band, the received code, target speed and distance,
// alarms and the acknowledge prompt. Vector-drawn (Painter arcs / sectors).
void drawDmi(std::vector<phx::ui::Vertex>& out, const Simulation& sim, f32 w, f32 h, const phx::ui::Theme& baseTheme) {
    using phx::ui::rgba;
    using phx::ui::Point;
    const safety::Onboard& ob = *sim.onboard;
    const safety::Display& d = ob.display();
    phx::ui::Theme theme = baseTheme;
    theme.textScale = std::max(1.0f, baseTheme.textScale * 0.75f);
    phx::ui::Painter p(out, theme, {0, 0, w, h});
    const f32 lh = p.lineHeight();
    const f32 size = std::clamp(h * 0.30f, 220.0f, 340.0f), x0 = 12.0f, y0 = h - size - lh - 40.0f;
    const bool blink = std::fmod(sim.time, 0.8) < 0.4;
    const u32 bg = rgba(14, 16, 20, 215), dim = rgba(150, 158, 170), white = rgba(235, 238, 242);
    const u32 red = rgba(240, 60, 55), amber = rgba(250, 190, 50);
    p.fillRoundedRect({x0, y0, size, size + lh + 8}, 10.0f, bg);

    // scale: the route's top speed rounded up (120 / 200 / 350 km/h)
    double top = 0.0;
    for (const SpeedLimit& l : sim.route.limits)
        if (std::isfinite(l.speed)) top = std::max(top, l.speed * 3.6);
    const f32 vmax = top <= 120.0 ? 120.0f : top <= 200.0 ? 200.0f : 350.0f;
    const Point c{x0 + size * 0.5f, y0 + size * 0.52f};
    const f32 r = size * 0.40f;
    constexpr f32 pi = std::numbers::pi_v<f32>;
    const f32 a0 = 0.75f * pi, sweep = 1.5f * pi; // 0 at lower left, clockwise to lower right
    auto angle = [&](f32 kmh) { return a0 + sweep * std::clamp(kmh / vmax, 0.0f, 1.0f); };
    // dial: ticks every 10 / 50 km/h
    for (f32 v = 0; v <= vmax + 0.1f; v += 10.0f) {
        const bool major = std::fmod(v, vmax > 200 ? 50.0f : 20.0f) < 0.1f;
        const f32 a = angle(v), ri = r - (major ? 12.0f : 6.0f);
        p.drawLine({c.x + std::cos(a) * ri, c.y + std::sin(a) * ri}, {c.x + std::cos(a) * r, c.y + std::sin(a) * r}, major ? 2.0f : 1.0f, dim);
        if (major) {
            const std::string label = std::format("{:.0f}", v);
            const f32 rl = r - 12.0f - lh * 0.9f;
            p.drawText(c.x + std::cos(a) * rl - p.textWidth(label) * 0.5f, c.y + std::sin(a) * rl - lh * 0.5f, label, dim);
        }
    }
    // permitted speed: grey band up to it, red beyond; intervention (ATP) in orange
    const f32 kmh = f32(std::abs(sim.train->speed) * 3.6);
    if (d.permitted) {
        const f32 perm = f32(*d.permitted * 3.6);
        p.fillSector(c, r + 3.0f, r + 11.0f, a0, angle(perm), rgba(190, 195, 205));
        if (perm < vmax) p.fillSector(c, r + 3.0f, r + 11.0f, angle(perm), a0 + sweep, rgba(150, 40, 38));
        if (d.intervention) p.fillSector(c, r + 3.0f, r + 11.0f, angle(perm), angle(f32(*d.intervention * 3.6)), amber);
    } else {
        p.fillSector(c, r + 3.0f, r + 11.0f, a0, a0 + sweep, rgba(70, 76, 86));
    }
    if (d.target) { // target speed marker
        const f32 a = angle(f32(*d.target * 3.6));
        p.drawLine({c.x + std::cos(a) * (r + 2), c.y + std::sin(a) * (r + 2)}, {c.x + std::cos(a) * (r + 16), c.y + std::sin(a) * (r + 16)}, 3.0f, amber);
    }
    // needle
    const bool over = d.permitted && kmh > *d.permitted * 3.6 + 0.5;
    const u32 needle = d.braking ? red : over || d.warning ? amber : white;
    const f32 an = angle(kmh);
    p.drawLine(c, {c.x + std::cos(an) * (r - 8), c.y + std::sin(an) * (r - 8)}, 3.0f, needle);
    p.fillCircle(c, size * 0.085f, rgba(30, 33, 40));
    p.strokeCircle(c, size * 0.085f, 2.0f, needle);
    const std::string speed = std::format("{:.0f}", kmh);
    p.drawText(c.x - p.textWidth(speed) * 0.5f, c.y - lh * 0.5f, speed, white);

    // system + code (top left), permitted / target (top right)
    if (const auto sys = ob.active()) p.drawText(x0 + 10, y0 + 8, std::string(safety::label(*sys)), dim);
    if (!d.code.empty()) {
        const f32 cw = std::max(p.textWidth(d.code) + 16.0f, lh * 1.6f);
        const phx::ui::Rect chip{x0 + 10, y0 + 10 + lh, cw, lh + 6};
        p.fillRoundedRect(chip, 5.0f, codeColour(d.code));
        p.drawText(chip.x + (cw - p.textWidth(d.code)) * 0.5f, chip.y + 3, d.code, rgba(15, 15, 18));
    }
    {
        const std::string perm = d.permitted ? std::format("{:.0f}", *d.permitted * 3.6) : std::string("—");
        p.drawText(x0 + size - 10 - p.textWidth(perm), y0 + 8, perm, white);
        if (d.target && d.targetDistance) {
            const std::string t = std::format("{:.0f} / {:.0f} m", *d.target * 3.6, *d.targetDistance);
            p.drawText(x0 + size - 10 - p.textWidth(t), y0 + 8 + lh, t, amber);
        }
    }
    // distance to the target (log scale, 10 m … 4 km), right edge: fills as the target nears
    if (d.target && d.targetDistance) {
        const f32 bx = x0 + size - 16, by = y0 + 2.2f * lh + 14, bh = size * 0.62f;
        p.fillRoundedRect({bx, by, 6, bh}, 3.0f, rgba(50, 54, 62));
        const f32 frac = std::clamp(f32(1.0 - std::log10(std::max(*d.targetDistance, 10.0) / 10.0) / std::log10(400.0)), 0.0f, 1.0f);
        p.fillRoundedRect({bx, by + bh * (1.0f - frac), 6, bh * frac}, 3.0f, amber);
    }
    // status line: message (blinking while the alarm sounds), acknowledge prompt
    const f32 sy = y0 + size + 2;
    if (!d.message.empty() && (!d.warning || blink))
        p.drawText(x0 + 10, sy, d.message, d.braking ? red : d.warning ? amber : white);
    if (d.ackRequired) {
        const std::string ack = "확인 S";
        const f32 aw = p.textWidth(ack) + 16;
        const phx::ui::Rect btn{x0 + size - aw - 8, sy - 3, aw, lh + 6};
        p.fillRoundedRect(btn, 5.0f, blink ? amber : rgba(90, 70, 20));
        p.drawText(btn.x + 8, btn.y + 3, ack, rgba(15, 15, 18));
    }
}

} // namespace

std::string displayName(const std::string& name) {
    if (phx::ui::fonts().covers(name)) { // TrueType font with Hangul: "seoul[서울역]" → the local name
        const auto l = name.find('['), r = name.rfind(']');
        return l != std::string::npos && r != std::string::npos && r > l + 1 ? name.substr(l + 1, r - l - 1) : name;
    }
    // bitmap font (ASCII only)
    std::string a = asciiOnly(name);
    // "name[]" left over from a non-ASCII bracketed part
    while (!a.empty() && (a.back() == ']' || a.back() == '[' || a.back() == ' ')) a.pop_back();
    if (const auto p = a.find("[]"); p != std::string::npos) a.erase(p, 2);
    return a.empty() ? name : a;
}

void buildHud(std::vector<phx::ui::Vertex>& out, const Simulation& sim, const RenderStats& st, const HudOptions& opt, f32 w, f32 h,
              const phx::ui::Theme& theme) {
    using phx::ui::rgba;
    phx::ui::Painter p(out, theme, {0, 0, w, h});
    const f32 lh = p.lineHeight() + 6.0f;
    const u32 white = rgba(240, 242, 245), dim = rgba(170, 178, 190), warn = rgba(255, 90, 70), ok = rgba(120, 230, 140);
    const u32 panel = rgba(10, 12, 16, 150);
    if (!opt.visible) {
        if (!opt.message.empty()) {
            const f32 tw = p.textWidth(opt.message);
            p.fillRoundedRect({0.5f * (w - tw) - 12, 0.4f * h - 8, tw + 24, lh + 10}, 8.0f, panel);
            p.drawText(0.5f * (w - tw), 0.4f * h, opt.message, white);
        }
        return;
    }
    // top-right column: profiler box, then the statistics panel below it
    const f32 rightTop = opt.profiler && opt.frames ? drawProfiler(out, theme, *opt.frames, st, opt.fps, w) + 8.0f : 8.0f;
    const TrainSim& t = *sim.train;
    std::vector<std::pair<std::string, u32>> lines;
    lines.push_back({std::format("{}   x{:g}{}", clock(sim.time), sim.timeFactor, sim.paused ? "   PAUSED" : ""), white});
    const double kmh = std::abs(t.speed) * 3.6;
    const double lim = sim.speedLimit();
    lines.push_back({std::format("{:5.1f} km/h   limit {}", kmh, std::isfinite(lim) ? std::format("{:.0f}", lim * 3.6) : std::string("-")),
                     sim.overspeed ? warn : white});
    const std::string_view rev = t.reverser > 0 ? "F" : (t.reverser < 0 ? "B" : "N");
    lines.push_back({std::format("{}   reverser {}   BC {:.0f} kPa", notch(t), rev, t.brakeCylinder), t.emergency ? warn : white});
    if (t.leftDoors > 0 || t.rightDoors > 0 || t.leftDoorsTarget || t.rightDoorsTarget)
        lines.push_back({std::format("doors  L {:.0f}%  R {:.0f}%", t.leftDoors * 100, t.rightDoors * 100), rgba(255, 210, 90)});
    if (sim.nextStation) {
        const Station& s = sim.route.stations[*sim.nextStation];
        const double d = sim.distanceToStop(*sim.nextStation);
        std::string line = std::format("next {}  {}", displayName(s.name),
                                       std::abs(d) >= 1000 ? std::format("{:.2f} km", d / 1000.0) : std::format("{:.0f} m", d));
        if (s.stopsHere()) line += std::format("   arr {}  dep {}", clock(s.arrival), clock(s.departure));
        else line += "   (pass)";
        const StationProgress& pr = sim.stations[*sim.nextStation];
        u32 col = s.stopsHere() ? white : dim;
        if (pr.arrived) {
            line += std::format("   stopped {:+.1f} m", pr.stopError);
            col = ok;
        }
        lines.push_back({line, col});
    }
    if (const auto sig = sim.signalAhead(); sig && sig->distance < 5000.0)
        lines.push_back({std::format("signal ahead {:.0f} m   aspect {}", sig->distance, sig->aspect), sig->aspect == 0 ? warn : dim});
    lines.push_back({std::format("km {:.3f}   camera {}", t.front / 1000.0, opt.cameraName), dim});
    if (sim.plugin && sim.plugin->running()) {
        std::string l = sim.plugin->name;
        if (t.safetyActive && (t.safetyPower != t.powerNotch || t.safetyBrake != (t.emergency ? t.maxBrake() + 1 : t.brakeNotch))) {
            const int b = t.safetyBrake;
            l += std::format("   override P{} {}", t.safetyPower, b > t.maxBrake() ? std::string("EB") : std::format("B{}", b));
        }
        lines.push_back({l, t.safetyActive && t.safetyBrake > 0 && t.safetyBrake != t.brakeNotch ? warn : ok});
        if (!sim.plugin->debug.empty() && opt.stats) lines.push_back({sim.plugin->debug, dim});
    } else if (!sim.onboard && !sim.pluginStatus.empty() && sim.pluginStatus != "no plugin (ats.cfg)") {
        lines.push_back({"plugin: " + sim.pluginStatus, warn});
    }

    f32 maxW = 0;
    for (auto& l : lines) maxW = std::max(maxW, p.textWidth(l.first));
    p.fillRoundedRect({8, 8, maxW + 20, lh * f32(lines.size()) + 10}, 8.0f, panel);
    f32 y = 14;
    for (auto& [text, col] : lines) {
        p.drawText(18, y, text, col);
        y += lh;
    }

    if (opt.stats) {
        auto mb = [](std::size_t b) { return double(b) / (1024.0 * 1024.0); };
        std::vector<std::string> s = {
            std::format("{:.0f} fps   cpu {:.2f} ms", opt.fps, st.cpuMs),
            std::format("draws {}   instances {}   anim {}", st.drawCalls, st.instances, st.animatedEvaluated),
            std::format("textures {} = {:.0f} MB (raw {:.0f} MB)  pend {}", st.textureCount, mb(st.textureBytes), mb(st.textureRawBytes),
                        st.texturesPending),
            std::format("geometry {:.1f}/{:.0f} MB vtx  {:.1f}/{:.0f} MB idx", mb(st.vertexBytes), mb(st.vertexCapacity), mb(st.indexBytes),
                        mb(st.indexCapacity)),
            std::format("meshes {}  loading {}", st.meshesResident, st.meshesPending),
            std::format("VRAM {:.0f} / {:.0f} MB", mb(st.vramUsage), mb(st.vramBudget)),
        };
        if (!opt.audio.empty()) s.push_back(opt.audio);
        f32 sw = 0;
        for (auto& l : s) sw = std::max(sw, p.textWidth(l));
        p.fillRoundedRect({w - sw - 28, rightTop, sw + 20, lh * f32(s.size()) + 10}, 8.0f, panel);
        f32 yy = rightTop + 6;
        for (auto& l : s) {
            p.drawText(w - sw - 18, yy, l, dim);
            yy += lh;
        }
    }
    if (sim.onboard) drawDmi(out, sim, w, h, theme);
    if (opt.help) {
        const std::string help = "Z/A power  ,/. brake  / EB  F/V reverser  F5/F6 doors  F1-F4 camera  Esc settings  F10 HUD  Ctrl+X profiler";
        const f32 tw = p.textWidth(help);
        p.fillRoundedRect({0.5f * (w - tw) - 10, h - lh - 12, tw + 20, lh + 6}, 8.0f, panel);
        p.drawText(0.5f * (w - tw), h - lh - 8, help, dim);
    }
    if (!opt.message.empty()) {
        const f32 tw = p.textWidth(opt.message);
        p.fillRoundedRect({0.5f * (w - tw) - 12, 0.4f * h - 8, tw + 24, lh + 10}, 8.0f, panel);
        p.drawText(0.5f * (w - tw), 0.4f * h, opt.message, white);
    }
    if (!opt.tooltip.empty()) {
        const f32 tw = p.textWidth(opt.tooltip);
        const f32 tx = std::min(opt.tooltipX + 16.0f, w - tw - 8.0f), ty = opt.tooltipY + 20.0f;
        p.fillRoundedRect({tx - 6, ty - 3, tw + 12, lh + 6}, 8.0f, panel);
        p.drawText(tx, ty, opt.tooltip, white);
    }
    // plugin interface messages (openBVE MessageColor), newest at the bottom
    if (!sim.messages.empty()) {
        const std::array<u32, 9> colors{white, rgba(0, 0, 0), rgba(160, 160, 160), white, rgba(255, 90, 80), rgba(255, 170, 60),
                                        rgba(110, 230, 110), rgba(110, 170, 255), rgba(240, 110, 240)};
        f32 my = 0.25f * h;
        for (const PluginMessage& m : sim.messages) {
            const f32 tw = p.textWidth(m.text);
            p.fillRoundedRect({0.5f * (w - tw) - 10, my - 4, tw + 20, lh + 6}, 8.0f, panel);
            p.drawText(0.5f * (w - tw), my, m.text, colors[m.color >= 0 && std::size_t(m.color) < colors.size() ? std::size_t(m.color) : 0]);
            my += lh + 8;
        }
    }
}

} // namespace ots
