// .animated object files (openBVE Object.Animated): [Include] and [Object] sections with state
// lists and translate / rotate / texture-shift functions.
#include "bve/ObjectLibrary.hpp"
#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

namespace ots {
namespace {

glm::vec3 parseVec3(std::string_view v, glm::vec3 def) {
    auto p = split(v, ',');
    for (std::size_t i = 0; i < 3 && i < p.size(); ++i) {
        double d;
        if (parseDoubleVb6(p[i], d)) def[int(i)] = float(d);
    }
    return def;
}

std::shared_ptr<Function> fn(std::string_view src, const std::filesystem::path& file, std::string_view key) {
    std::string err;
    auto f = Function::compile(src, &err);
    if (!f) phx::log::warn("{}: {} = {}: {}", file.string(), key, src, err);
    return f;
}

} // namespace

bool loadAnimated(const std::filesystem::path& path, AnimatedDef& out, ObjectLibrary& lib, std::string& error, int depth) {
    if (depth > 8) {
        error = "include depth exceeded";
        return false;
    }
    std::string text;
    if (!readTextFile(path, text)) {
        error = "cannot read";
        return false;
    }
    const std::filesystem::path dir = path.parent_path();
    enum class Sec { None, Include, Object, Other } sec = Sec::None;
    AnimatedPart part;
    bool partOpen = false;
    std::vector<std::filesystem::path> includes;
    glm::vec3 includePos(0.0f);

    auto closeInclude = [&] {
        for (const auto& inc : includes) {
            const ObjectKind k = ObjectLibrary::kindOf(inc);
            if (k == ObjectKind::Animated) {
                AnimatedDef sub;
                std::string e;
                if (loadAnimated(inc, sub, lib, e, depth + 1)) {
                    for (AnimatedPart& p : sub.parts) {
                        p.position += includePos;
                        out.parts.push_back(std::move(p));
                    }
                }
            } else if (k == ObjectKind::Static) {
                AnimatedPart p;
                p.states.push_back(lib.intern(inc));
                p.position = includePos;
                out.parts.push_back(std::move(p));
            }
        }
        includes.clear();
        includePos = glm::vec3(0.0f);
    };
    auto closeObject = [&] {
        if (partOpen && !part.states.empty()) out.parts.push_back(std::move(part));
        part = AnimatedPart{};
        partOpen = false;
    };
    auto closeSection = [&] {
        if (sec == Sec::Include) closeInclude();
        if (sec == Sec::Object) closeObject();
    };

    for (const std::string& raw : splitLines(text)) {
        std::string_view line = raw;
        if (auto sc = line.find(';'); sc != std::string_view::npos) line = line.substr(0, sc);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            closeSection();
            const std::string name = lower(trim(line.substr(1, line.size() - 2)));
            if (name == "include") sec = Sec::Include;
            else if (name == "object") sec = Sec::Object, partOpen = true;
            else sec = Sec::Other;
            continue;
        }
        const auto eq = line.find('=');
        if (sec == Sec::Include) {
            if (eq != std::string_view::npos && iequals(trim(line.substr(0, eq)), "position")) {
                includePos = parseVec3(trim(line.substr(eq + 1)), glm::vec3(0.0f));
            } else if (eq == std::string_view::npos) {
                for (std::string_view f : split(line, ',')) {
                    f = trim(f);
                    if (f.empty()) continue;
                    std::filesystem::path p = resolvePath(dir, f);
                    if (p.empty()) phx::log::warn("{}: include not found: {}", path.string(), f);
                    else includes.push_back(p);
                }
            }
            continue;
        }
        if (sec != Sec::Object || eq == std::string_view::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string_view val = trim(line.substr(eq + 1));
        if (key == "position") {
            part.position = parseVec3(val, glm::vec3(0.0f));
        } else if (key == "states") {
            for (std::string_view f : split(val, ',')) {
                f = trim(f);
                if (f.empty()) {
                    part.states.push_back(kNoObject);
                    continue;
                }
                std::filesystem::path p = resolvePath(dir, f);
                if (p.empty()) {
                    phx::log::warn("{}: state object not found: {}", path.string(), f);
                    part.states.push_back(kNoObject);
                } else {
                    part.states.push_back(lib.intern(p));
                }
            }
        } else if (key == "statefunction") {
            part.stateFn = fn(val, path, key);
        } else if (key.starts_with("translate") || key.starts_with("rotate") || key.starts_with("textureshift")) {
            const bool rot = key.starts_with("rotate");
            const bool shift = key.starts_with("textureshift");
            const std::size_t base = rot ? 6 : (shift ? 12 : 9);
            if (key.size() <= base) continue;
            const int axis = key[base] - 'x';
            if (axis < 0 || axis > 2 || (shift && axis > 1)) continue;
            const std::string rest = key.substr(base + 1);
            if (rest == "direction") {
                if (shift) {
                    const glm::vec3 d = parseVec3(val, glm::vec3(part.textureShiftDir[axis], 0.0f));
                    part.textureShiftDir[axis] = glm::vec2(d);
                } else if (rot) {
                    part.rotateDir[axis] = parseVec3(val, part.rotateDir[axis]);
                    if (glm::length(part.rotateDir[axis]) > 0) part.rotateDir[axis] = glm::normalize(part.rotateDir[axis]);
                } else {
                    part.translateDir[axis] = parseVec3(val, part.translateDir[axis]);
                }
            } else if (rest == "function") {
                auto f = fn(val, path, key);
                if (shift) part.textureShiftFn[axis] = f;
                else if (rot) part.rotateFn[axis] = f;
                else part.translateFn[axis] = f;
            } else if (rest == "damping" && rot) {
                auto p = split(val, ',');
                double a = 0, b = 0;
                if (!p.empty()) parseDoubleVb6(p[0], a);
                if (p.size() > 1) parseDoubleVb6(p[1], b);
                part.rotateDamping[axis] = {float(a), float(b)};
            }
        } else if (key == "refreshrate") {
            double r;
            if (parseDoubleVb6(val, r)) part.refreshRate = float(r);
        }
    }
    closeSection();
    return true;
}

} // namespace ots
