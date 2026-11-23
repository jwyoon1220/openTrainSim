#include "render/Hud.hpp"

#include "util/Text.hpp"

#include <cmath>
#include <format>

namespace ots {
namespace {

std::string clock(double t) {
    if (t < 0) return "--:--:--";
    t = std::fmod(t + 86400.0, 86400.0);
    const int s = int(t);
    return std::format("{:02}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
}

std::string notch(const TrainSim& t) {
    std::string p = t.powerNotch > 0 ? std::format("P{}", t.powerNotch) : "N";
    std::string b = t.emergency ? "EB" : (t.brakeNotch > 0 ? std::format("B{}", t.brakeNotch) : "N");
    if (t.def().spec.handleType == HandleType::OneHandle) return t.emergency ? "EB" : (t.brakeNotch > 0 ? b : p);
    return p + " / " + b;
}

} // namespace

std::string displayName(const std::string& name) {
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
            p.fillRect({0.5f * (w - tw) - 12, 0.4f * h - 8, tw + 24, lh + 10}, panel);
            p.drawText(0.5f * (w - tw), 0.4f * h, opt.message, white);
        }
        return;
    }
    const TrainSim& t = *sim.train;
    std::vector<std::pair<std::string, u32>> lines;
    lines.push_back({std::format("{}   x{:g}{}", clock(sim.time), sim.timeFactor, sim.paused ? "   PAUSED" : ""), white});
    const double kmh = std::abs(t.speed) * 3.6;
    const double lim = sim.speedLimit();
    lines.push_back({std::format("{:5.1f} km/h   limit {}", kmh, std::isfinite(lim) ? std::format("{:.0f}", lim * 3.6) : std::string("-")),
                     sim.overspeed ? warn : white});
    const char* rev = t.reverser > 0 ? "F" : (t.reverser < 0 ? "B" : "N");
    lines.push_back({std::format("{}   reverser {}   BC {:.0f} kPa", notch(t), rev, t.brakeCylinder), t.emergency ? warn : white});
    if (t.leftDoors > 0 || t.rightDoors > 0 || t.leftDoorsTarget || t.rightDoorsTarget)
        lines.push_back({std::format("doors  L {:.0f}%  R {:.0f}%", t.leftDoors * 100, t.rightDoors * 100), rgba(255, 210, 90)});
    if (sim.nextStation >= 0) {
        const Station& s = sim.route.stations[std::size_t(sim.nextStation)];
        const double d = sim.distanceToStop(sim.nextStation);
        std::string line = std::format("next {}  {}", displayName(s.name),
                                       std::abs(d) >= 1000 ? std::format("{:.2f} km", d / 1000.0) : std::format("{:.0f} m", d));
        if (s.stopsHere()) line += std::format("   arr {}  dep {}", clock(s.arrival), clock(s.departure));
        else line += "   (pass)";
        const StationProgress& pr = sim.stations[std::size_t(sim.nextStation)];
        u32 col = s.stopsHere() ? white : dim;
        if (pr.arrived) {
            line += std::format("   stopped {:+.1f} m", pr.stopError);
            col = ok;
        }
        lines.push_back({line, col});
    }
    {
        double dist;
        int aspect;
        if (sim.signalAhead(dist, aspect) >= 0 && dist < 5000.0)
            lines.push_back({std::format("signal ahead {:.0f} m   aspect {}", dist, aspect), aspect == 0 ? warn : dim});
    }
    lines.push_back({std::format("km {:.3f}   camera {}", t.front / 1000.0, opt.cameraName), dim});
    if (sim.plugin && sim.plugin->running()) {
        std::string l = sim.plugin->name;
        if (t.safetyActive && (t.safetyPower != t.powerNotch || t.safetyBrake != (t.emergency ? t.maxBrake() + 1 : t.brakeNotch))) {
            const int b = t.safetyBrake;
            l += std::format("   override P{} {}", t.safetyPower, b > t.maxBrake() ? std::string("EB") : std::format("B{}", b));
        }
        lines.push_back({l, t.safetyActive && t.safetyBrake > 0 && t.safetyBrake != t.brakeNotch ? warn : ok});
        if (!sim.plugin->debug.empty() && opt.stats) lines.push_back({sim.plugin->debug, dim});
    } else if (!sim.pluginStatus.empty() && sim.pluginStatus != "no plugin (ats.cfg)") {
        lines.push_back({"plugin: " + sim.pluginStatus, warn});
    }

    f32 maxW = 0;
    for (auto& l : lines) maxW = std::max(maxW, p.textWidth(l.first));
    p.fillRect({8, 8, maxW + 20, lh * f32(lines.size()) + 10}, panel);
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
        p.fillRect({w - sw - 28, 8, sw + 20, lh * f32(s.size()) + 10}, panel);
        f32 yy = 14;
        for (auto& l : s) {
            p.drawText(w - sw - 18, yy, l, dim);
            yy += lh;
        }
    }
    if (opt.help) {
        const std::string help = "Z/A power  ,/. brake  / EB  F/V reverser  F5/F6 doors  F1-F4 camera  Esc settings  F10 HUD  Ctrl+X stats";
        const f32 tw = p.textWidth(help);
        p.fillRect({0.5f * (w - tw) - 10, h - lh - 12, tw + 20, lh + 6}, panel);
        p.drawText(0.5f * (w - tw), h - lh - 8, help, dim);
    }
    if (!opt.message.empty()) {
        const f32 tw = p.textWidth(opt.message);
        p.fillRect({0.5f * (w - tw) - 12, 0.4f * h - 8, tw + 24, lh + 10}, panel);
        p.drawText(0.5f * (w - tw), 0.4f * h, opt.message, white);
    }
    // plugin interface messages (openBVE MessageColor), newest at the bottom
    if (!sim.messages.empty()) {
        static const u32 colors[] = {white, rgba(0, 0, 0), rgba(160, 160, 160), white, rgba(255, 90, 80), rgba(255, 170, 60),
                                     rgba(110, 230, 110), rgba(110, 170, 255), rgba(240, 110, 240)};
        f32 my = 0.25f * h;
        for (const PluginMessage& m : sim.messages) {
            const f32 tw = p.textWidth(m.text);
            p.fillRect({0.5f * (w - tw) - 10, my - 4, tw + 20, lh + 6}, panel);
            p.drawText(0.5f * (w - tw), my, m.text, colors[m.color >= 0 && m.color < 9 ? m.color : 0]);
            my += lh + 8;
        }
    }
}

} // namespace ots
