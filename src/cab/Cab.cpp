#include "cab/Cab.hpp"

#include "bve/Gltf.hpp"
#include "phx/core/Log.hpp"
#include "sim/World.hpp"
#include "util/Json.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace ots {
namespace {

enum class Bind : u8 { None, Power, Brake, OneHandle, Reverser, Emergency, Horn, Doors, Headlights, HoldBrake, ConstSpeed, Key, Command };

constexpr double kPi = 3.14159265358979323846;

struct Element {
    std::string name, tooltip;
    CabKind kind = CabKind::Part;
    glm::dvec3 pos{0.0};
    glm::dmat3 base{1.0};
    std::vector<ObjectId> states;
    glm::dvec3 axis{1.0, 0.0, 0.0};
    std::vector<double> angles;  // degrees, spread evenly over [min, max]
    glm::dvec3 dir{0.0, 0.0, -1.0};
    std::vector<double> offsets; // metres, same
    double min = 0.0, max = 1.0;
    bool continuous = false;     // value not rounded to whole steps
    double value = 0.0, shown = 0.0, damping = 0.0;
    std::optional<double> spring;
    bool toggle = false, wrap = false;
    glm::dvec3 hitMin{0.0}, hitMax{0.0};
    bool hitSet = false;
    Bind bind = Bind::None;
    int bindArg = 0;
    Command command = Command::Count;
    std::shared_ptr<Function> expr; // value from an openBVE function script
    std::filesystem::path sound, soundRelease;
    double dragPixels = 30.0;
    glm::dvec2 dragDir{0.0, -1.0}; // screen direction that increases the value (default: up)
    double dragAccum = 0.0;
    bool pressed = false;
    // parts of a 3D model: baked once every element is known
    std::optional<std::size_t> model;
    std::vector<std::optional<std::size_t>> nodes; // one per state, none = nothing shown
    std::optional<std::size_t> hitNode;
    [[nodiscard]] bool interactive() const { return kind == CabKind::Button || kind == CabKind::Switch || kind == CabKind::Lever; }
};

// "when" rule: message / sound on the rising edge of a condition.
struct Alert {
    std::shared_ptr<Function> when;
    std::string message;
    double seconds = 3.0;
    std::filesystem::path sound;
    double repeat = 0.0; // > 0: again every `repeat` seconds while the condition holds
    bool active = false;
    double timer = 0.0;
};

double spread(const std::vector<double>& v, double x, double lo, double hi) {
    if (v.empty()) return 0.0;
    const double t = hi > lo ? std::clamp((x - lo) / (hi - lo), 0.0, 1.0) : 0.0;
    if (v.size() == 1) return v[0] * t; // single value: reached at max, 0 at min
    const double f = t * double(v.size() - 1);
    const std::size_t i = std::min(std::size_t(f), v.size() - 2);
    return v[i] + (v[i + 1] - v[i]) * (f - double(i));
}

glm::dmat3 axisRotation(const glm::dvec3& axis, double degrees) {
    const double len = glm::length(axis);
    if (len < 1e-9 || degrees == 0.0) return glm::dmat3(1.0);
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), degrees * kPi / 180.0, axis / len));
}

glm::dmat3 eulerRotation(const glm::dvec3& r) { // degrees, applied x, then y, then z
    return axisRotation({0, 0, 1}, r.z) * axisRotation({0, 1, 0}, r.y) * axisRotation({1, 0, 0}, r.x);
}

// ---------------------------------------------------------------------- JSON helpers
struct LoadError {
    std::string what;
};
[[noreturn]] void fail(std::string what) { throw LoadError{std::move(what)}; }

glm::dvec3 vec3(const Json& v, glm::dvec3 def) {
    if (v.isString()) { // axis names
        const std::string& s = v.string();
        if (s == "x") return {1, 0, 0};
        if (s == "y") return {0, 1, 0};
        if (s == "z") return {0, 0, 1};
        if (s == "-x") return {-1, 0, 0};
        if (s == "-y") return {0, -1, 0};
        if (s == "-z") return {0, 0, -1};
        fail(std::format("'{}' is not an axis (x, y, z, -x, -y, -z)", s));
    }
    if (v.isNumber()) return glm::dvec3(v.number());
    if (!v.isArray()) return def;
    glm::dvec3 out = def;
    for (std::size_t k = 0; k < 3 && k < v.size(); ++k) out[int(k)] = v[k].number(def[int(k)]);
    return out;
}

std::vector<double> numbers(const Json& v) {
    std::vector<double> out;
    if (v.isNumber()) out.push_back(v.number());
    for (const Json& x : v.items()) out.push_back(x.number());
    return out;
}

std::shared_ptr<Function> expression(const Json& v, const std::string& where) {
    if (!v.isString()) return nullptr;
    std::string err;
    auto f = Function::compile(v.string(), &err);
    if (!f) fail(std::format("{}: {}", where, err));
    return f;
}

} // namespace

struct Cab::Impl {
    ObjectLibrary& lib;
    Simulation& sim;
    std::filesystem::path dir;
    std::string name = "cab";
    std::vector<ObjectId> bodies;
    std::vector<Element> elements;
    std::vector<Alert> alerts;
    std::optional<glm::dvec3> eye;
    int hovered = -1, active = -1, activeButton = -1;
    int meshCounter = 0;
    Cab* owner = nullptr;

    // A 3D cab model (glTF) placed in the car. Its nodes become elements by name; what no
    // element takes is drawn as the cab body.
    struct Model {
        std::shared_ptr<GltfScene> scene;
        glm::dmat4 place{1.0};     // model -> car
        std::vector<bool> claimed; // node taken by an element (or a hit volume / hidden)
        std::string file, key;
        bool body = true;
    };
    std::vector<Model> models;

    Impl(ObjectLibrary& l, Simulation& s) : lib(l), sim(s) {}

    std::filesystem::path resolve(const std::string& rel) const {
        std::filesystem::path p = resolvePath(dir, rel);
        return p.empty() ? combinePath(dir, rel) : p;
    }
    glm::dvec3 driverEye() const {
        const TrainDef& d = sim.train->def();
        return d.cars[std::size_t(std::clamp(d.spec.driverCar, 0, int(d.cars.size()) - 1))].driver;
    }

    // ------------------------------------------------------------------ models
    RigidPose nodeFrame(const Model& m, std::size_t node) const { return gltfFrame(m.place * m.scene->nodes[node].world); }
    static glm::dmat4 toFrame(const glm::dmat3& rot, const glm::dvec3& pos) {
        glm::dmat4 f(glm::transpose(rot));
        f[3] = glm::dvec4(-(glm::transpose(rot) * pos), 1.0);
        return f;
    }
    // Mesh of a node's subtree (claimed descendants left out) in the frame (rot, pos).
    ObjectId bakeNode(const Model& m, std::size_t node, const glm::dmat3& rot, const glm::dvec3& pos) {
        std::vector<bool> skip = m.claimed;
        skip[node] = false;
        auto mesh = std::make_shared<StaticMesh>();
        gltfBake(*m.scene, std::span(&node, 1), &skip, toFrame(rot, pos) * m.place, *mesh);
        if (mesh->empty()) return kNoObject;
        return lib.addMesh(std::format("{}#{}:{}", m.key, meshCounter++, m.scene->nodes[node].name), mesh);
    }
    std::size_t findNode(const Model& m, const std::string& node) const {
        const auto n = m.scene->find(node);
        if (!n) fail(std::format("{} has no node named '{}'", m.file, node));
        return *n;
    }

    void loadModel(const Json& j) {
        Model m;
        const std::string file = j.isString() ? j.string() : j.str("file");
        if (file.empty()) fail("model: \"file\" is missing");
        const std::filesystem::path p = resolve(file);
        auto scene = loadGltf(p);
        if (!scene) fail(std::format("cannot load model {}: {}", pathString(p), scene.error()));
        m.scene = std::make_shared<GltfScene>(std::move(*scene));
        m.file = file;
        m.key = pathString(p);
        if (j.isObject()) {
            // origin "driver": the model's origin goes to the driver's eye (plus "at");
            // "car" (default): the model is in car coordinates
            glm::dvec3 at = vec3(j["at"], glm::dvec3(0.0));
            const std::string origin = lower(j.str("origin", "car"));
            if (origin == "driver") at += driverEye();
            else if (origin != "car") fail(std::format("model {}: origin must be \"car\" or \"driver\"", file));
            const Json& sc = j["scale"];
            const glm::dvec3 s = sc.isNumber() ? glm::dvec3(sc.number()) : vec3(sc, glm::dvec3(1.0));
            m.place = glm::dmat4(eulerRotation(vec3(j["rotation"], glm::dvec3(0.0))) * glm::dmat3(glm::scale(glm::dmat4(1.0), s)));
            m.place[3] = glm::dvec4(at, 1.0);
            m.body = j.flag("body", true);
        }
        m.claimed.assign(m.scene->nodes.size(), false);
        for (const Json& h : j["hide"].items())
            if (h.isString()) m.claimed[findNode(m, h.string())] = true;
        phx::log::info("cab: model {} ({} nodes)", m.key, m.scene->nodes.size());
        models.push_back(std::move(m));
    }

    ObjectId loadObject(const std::string& rel) {
        const std::filesystem::path p = resolve(rel);
        const ObjectId id = lib.intern(p);
        if (!lib.load(id)) fail(std::format("cannot load {}: {}", pathString(p), lib[id].error));
        return id;
    }

    // ------------------------------------------------------------------ elements
    Bind parseBind(const std::string& s, Element& e) {
        const TrainSim& t = *sim.train;
        auto range = [&](double lo, double hi) { e.min = lo, e.max = hi; };
        if (s.empty()) return Bind::None;
        if (s == "power") return range(0, t.maxPower()), Bind::Power;
        if (s == "brake") return range(0, t.maxBrake() + 1), Bind::Brake;
        if (s == "onehandle" || s == "master") return range(-(t.maxBrake() + 1), t.maxPower()), Bind::OneHandle;
        if (s == "reverser") return range(-1, 1), Bind::Reverser;
        if (s == "emergency") return range(0, 1), Bind::Emergency;
        if (s == "horn1" || s == "horn") return e.bindArg = 1, range(0, 1), Bind::Horn;
        if (s == "horn2") return e.bindArg = 2, range(0, 1), Bind::Horn;
        if (s == "horn3" || s == "music") return e.bindArg = 3, range(0, 1), Bind::Horn;
        if (s == "doors_left") return e.bindArg = 0, range(0, 1), Bind::Doors;
        if (s == "doors_right") return e.bindArg = 1, range(0, 1), Bind::Doors;
        if (s == "headlights") return range(0, 1), Bind::Headlights;
        if (s == "hold_brake") return range(0, 1), Bind::HoldBrake;
        if (s == "const_speed") return range(0, 1), Bind::ConstSpeed;
        if (s.starts_with("key:")) { // safety-system key: key:a1, key:s ... key:l
            const auto c = commandFromName("SECURITY_" + s.substr(4));
            if (!c || securityKey(*c) < 0) fail(std::format("unknown safety key '{}'", s));
            return e.bindArg = securityKey(*c), range(0, 1), Bind::Key;
        }
        if (s.starts_with("command:")) { // any openBVE command: command:doors_left
            const auto c = commandFromName(s.substr(8));
            if (!c) fail(std::format("unknown command '{}'", s));
            e.command = *c;
            return range(0, 1), Bind::Command;
        }
        fail(std::format("unknown bind '{}'", s));
    }

    // node / nodes / hit naming parts of a model; "from" (index or file) picks the model,
    // default: the first one that has the node.
    void elementNodes(const Json& j, Element& e) {
        std::vector<std::string> names;
        if (j["node"].isString()) names.push_back(j["node"].string());
        for (const Json& n : j["nodes"].items()) names.push_back(n.isString() ? n.string() : std::string()); // null = nothing
        const std::string hit = j["hit"].isString() ? j["hit"].string() : "";
        if (names.empty() && hit.empty()) return;
        std::optional<std::size_t> mi;
        if (const Json& from = j["from"]; from.isNumber()) {
            if (from.number() < 0 || std::size_t(from.number()) >= models.size()) fail(std::format("{}: no model #{}", e.name, from.number()));
            mi = std::size_t(from.number());
        } else if (from.isString()) {
            const auto it = std::ranges::find(models, from.string(), &Model::file);
            if (it == models.end()) fail(std::format("{}: no model '{}'", e.name, from.string()));
            mi = std::size_t(it - models.begin());
        }
        std::string probe = hit;
        for (const std::string& n : names)
            if (!n.empty()) {
                probe = n;
                break;
            }
        if (!mi) {
            const auto it = std::ranges::find_if(models, [&](const Model& m) { return m.scene->find(probe).has_value(); });
            if (it == models.end()) fail(std::format("{}: no model has a node named '{}'", e.name, probe));
            mi = std::size_t(it - models.begin());
        }
        Model& m = models[*mi];
        e.model = mi;
        for (const std::string& n : names) {
            const auto k = n.empty() ? std::nullopt : std::optional(findNode(m, n));
            if (k) m.claimed[*k] = true;
            e.nodes.push_back(k);
        }
        if (!hit.empty()) {
            e.hitNode = findNode(m, hit);
            m.claimed[*e.hitNode] = true; // hit volumes are invisible
        }
        // the element turns about its first node's origin, as modelled
        if (const auto first = std::ranges::find_if(e.nodes, [](const auto& k) { return k.has_value(); }); first != e.nodes.end()) {
            const RigidPose f = nodeFrame(m, **first);
            e.base = f.rotation, e.pos = f.position;
        }
    }

    void addElement(const Json& j, std::size_t index) {
        static constexpr auto kKinds = std::to_array<std::pair<std::string_view, CabKind>>({{"button", CabKind::Button}, {"switch", CabKind::Switch},
                                                                  {"lever", CabKind::Lever},   {"gauge", CabKind::Gauge},
                                                                  {"lamp", CabKind::Lamp},     {"part", CabKind::Part}});
        if (!j.isObject()) fail(std::format("elements[{}] is not an object", index));
        Element e;
        const std::string type = lower(j.str("type"));
        bool known = false;
        for (const auto& [n, k] : kKinds)
            if (type == n) e.kind = k, known = true;
        e.name = j.str("name", j.str("node", std::format("element{}", index)));
        if (!known) fail(std::format("{}: \"type\" must be button, switch, lever, gauge, lamp or part", e.name));
        e.tooltip = j.str("tooltip", e.name);
        // placement: from the model node, or pos / rotation (car coordinates) for file models
        e.pos = vec3(j["pos"], glm::dvec3(0.0));
        e.base = eulerRotation(vec3(j["rotation"], glm::dvec3(0.0)));
        elementNodes(j, e);
        if (j.has("pivot")) e.pos = vec3(j["pivot"], e.pos);
        // separate model files (csv / b3d / x / glb): one, or one per state (null = nothing)
        if (const Json& mdl = j["model"]; mdl.isString()) {
            e.states.push_back(loadObject(mdl.string()));
        } else if (mdl.isArray()) {
            for (const Json& s : mdl.items()) e.states.push_back(s.isString() ? loadObject(s.string()) : kNoObject);
        }
        switch (e.kind) {
        case CabKind::Button:
            e.offsets = {0.004};
            e.dir = {0, -1, 0};
            break;
        case CabKind::Gauge:
            e.continuous = true;
            e.axis = {0, 0, 1};
            e.damping = 0.1;
            break;
        case CabKind::Lever: e.damping = 0.04; break;
        default: break;
        }
        e.bind = parseBind(lower(j.str("bind")), e);
        if (e.kind == CabKind::Lamp || e.kind == CabKind::Button) e.max = std::max(e.max, 1.0);
        e.min = j.num("min", e.min);
        e.max = j.num("max", e.max);
        if (j.has("positions") && !j.has("max")) e.max = e.min + j.num("positions", 2) - 1;
        if (e.kind == CabKind::Lamp && !j.has("max") && e.nodes.size() > 2) e.max = e.min + double(e.nodes.size()) - 1;
        e.continuous = j.flag("continuous", e.continuous);
        if (j.has("angles")) e.angles = numbers(j["angles"]);
        if (j.has("offsets")) e.offsets = numbers(j["offsets"]);
        if (j.has("travel")) e.offsets = {j.num("travel", 0.004)};
        e.axis = vec3(j["axis"], e.axis);
        e.dir = vec3(j["direction"], e.dir);
        e.damping = j.num("damping", e.damping);
        e.toggle = j.flag("toggle", false);
        e.wrap = j.flag("wrap", false);
        if (j.has("spring")) e.spring = j.num("spring", 0.0);
        e.dragPixels = std::max(4.0, j.num("drag_pixels", e.dragPixels));
        const std::string dd = j.str("drag", "up");
        e.dragDir = dd == "down" ? glm::dvec2(0, 1) : dd == "left" ? glm::dvec2(-1, 0) : dd == "right" ? glm::dvec2(1, 0) : glm::dvec2(0, -1);
        if (const Json& h = j["hit"]; h.isObject()) {
            const glm::dvec3 size = vec3(h["size"], glm::dvec3(0.04)), at = vec3(h["at"], glm::dvec3(0.0));
            e.hitMin = at - 0.5 * size, e.hitMax = at + 0.5 * size, e.hitSet = true;
        }
        if (const std::string s = j.str("sound"); !s.empty()) e.sound = resolve(s);
        if (const std::string s = j.str("sound_release"); !s.empty()) e.soundRelease = resolve(s);
        // value: openBVE function script ("abs[speedometer] * 3.6", "pluginState[73]") or a number
        if (const Json& v = j["value"]; v.isString()) e.expr = expression(v, e.name + ": value");
        else e.value = v.number(e.min);
        e.value = clampValue(e, e.bind != Bind::None && e.bind != Bind::Key && e.bind != Bind::Command ? readBind(e) : e.value);
        e.shown = e.value;
        elements.push_back(std::move(e));
    }

    // After every element is known: element meshes from their nodes, hit volumes, bodies.
    void finalizeModels() {
        for (Element& e : elements) {
            if (!e.model) continue;
            const Model& m = models[*e.model];
            for (const auto& n : e.nodes) e.states.push_back(n ? bakeNode(m, *n, e.base, e.pos) : kNoObject);
            if (e.hitNode) {
                StaticMesh hm;
                gltfBake(*m.scene, std::span(&*e.hitNode, 1), nullptr, toFrame(e.base, e.pos) * m.place, hm);
                hm.computeBounds();
                if (!hm.empty()) e.hitMin = glm::dvec3(hm.bmin), e.hitMax = glm::dvec3(hm.bmax), e.hitSet = true;
            }
        }
        for (Model& m : models) {
            if (!m.body) continue;
            auto mesh = std::make_shared<StaticMesh>();
            gltfBake(*m.scene, {}, &m.claimed, m.place, *mesh);
            if (!mesh->empty()) bodies.push_back(lib.addMesh(std::format("{}#body{}", m.key, meshCounter++), mesh));
        }
    }

    void build(const Json& root) {
        if (!root.isObject()) fail("the file must contain a JSON object");
        name = root.str("name", name);
        // "model": one model, "models": several
        if (const Json& m = root["model"]; !m.isNull()) loadModel(m);
        for (const Json& m : root["models"].items()) loadModel(m);
        // extra openBVE objects drawn with the body (.animated ones animate as in panel.animated)
        for (const Json& b : root["body"].items())
            if (b.isString()) bodies.push_back(loadObject(b.string()));
        if (const Json& ey = root["eye"]; !ey.isNull()) {
            // [x, y, z] in car coordinates, or {"offset": [..]} from train.dat's driver position
            eye = ey.isObject() ? driverEye() + vec3(ey["offset"], glm::dvec3(0.0)) : vec3(ey, driverEye());
        }
        const Json& els = root["elements"];
        for (std::size_t i = 0; i < els.size(); ++i) addElement(els[i], i);
        for (const Json& a : root["alerts"].items()) {
            Alert al;
            al.when = expression(a["when"], "alert when");
            if (!al.when) fail("alert: \"when\" (a function script) is missing");
            al.message = a.str("message");
            al.seconds = a.num("seconds", 3.0);
            al.repeat = a.num("repeat", 0.0);
            if (const std::string s = a.str("sound"); !s.empty()) al.sound = resolve(s);
            alerts.push_back(std::move(al));
        }
        finalizeModels();
    }

    // ------------------------------------------------------------------ values
    double readBind(const Element& e) const {
        const TrainSim& t = *sim.train;
        switch (e.bind) {
        case Bind::Power: return t.powerNotch;
        case Bind::Brake: return t.emergency ? t.maxBrake() + 1 : t.brakeNotch;
        case Bind::OneHandle:
            if (t.emergency) return -(t.maxBrake() + 1);
            return t.brakeNotch > 0 ? -t.brakeNotch : t.powerNotch;
        case Bind::Reverser: return t.reverser;
        case Bind::Emergency: return t.emergency ? 1 : 0;
        case Bind::Horn: return e.bindArg == 2 ? t.hornSecondary : e.bindArg == 1 ? t.hornPrimary : e.value;
        case Bind::Doors: return e.kind == CabKind::Button ? e.value : (e.bindArg ? t.rightDoorsTarget : t.leftDoorsTarget) ? 1 : 0;
        case Bind::Headlights: return t.headlights ? 1 : 0;
        case Bind::HoldBrake: return t.holdBrake ? 1 : 0;
        case Bind::ConstSpeed: return t.constSpeed ? 1 : 0;
        default: return e.value;
        }
    }
    void writeBind(Element& e, double v, double old) {
        TrainSim& t = *sim.train;
        const int n = int(std::lround(v));
        const bool on = v >= 0.5, wasOn = old >= 0.5;
        switch (e.bind) {
        case Bind::Power: t.powerNotch = std::clamp(n, 0, t.maxPower()); break;
        case Bind::Brake:
            t.emergency = n > t.maxBrake();
            t.brakeNotch = std::clamp(n, 0, t.maxBrake());
            break;
        case Bind::OneHandle:
            t.emergency = -n > t.maxBrake();
            t.powerNotch = std::clamp(n, 0, t.maxPower());
            t.brakeNotch = std::clamp(-n, 0, t.maxBrake());
            break;
        case Bind::Reverser: t.reverser = std::clamp(n, -1, 1); break;
        case Bind::Emergency:
            t.emergency = on;
            if (on) t.brakeNotch = t.maxBrake();
            break;
        case Bind::Horn:
            if (e.bindArg == 1) t.hornPrimary = on;
            else if (e.bindArg == 2) t.hornSecondary = on;
            if (on && !wasOn) sim.hornBlow(e.bindArg);
            break;
        case Bind::Doors:
            if (e.kind == CabKind::Button) {
                if (on && !wasOn) t.command(e.bindArg ? Command::DoorsRight : Command::DoorsLeft);
            } else if (on != (e.bindArg ? t.rightDoorsTarget : t.leftDoorsTarget)) {
                t.command(e.bindArg ? Command::DoorsRight : Command::DoorsLeft);
            }
            break;
        case Bind::Headlights: t.headlights = on; break;
        case Bind::HoldBrake: t.holdBrake = on; break;
        case Bind::ConstSpeed:
            if (on != t.constSpeed) t.command(Command::ConstSpeed);
            break;
        case Bind::Key:
            if (on != wasOn) sim.pluginKey(e.bindArg, on);
            break;
        case Bind::Command:
            if (on && !wasOn) t.command(e.command);
            break;
        default: break;
        }
    }

    double clampValue(const Element& e, double v) const {
        if (!e.continuous) v = std::round(v);
        if (e.wrap && !e.continuous) {
            const double span = e.max - e.min + 1.0;
            while (v > e.max) v -= span;
            while (v < e.min) v += span;
        }
        return std::clamp(v, e.min, e.max);
    }

    // A change by the driver: clamps, writes the binding, plays the click sound.
    void setValue(int i, double v) {
        Element& e = elements[std::size_t(i)];
        v = clampValue(e, v);
        const double old = e.value;
        if (v == old) return;
        e.value = v;
        writeBind(e, v, old);
        const std::filesystem::path& s = v < old && !e.soundRelease.empty() ? e.soundRelease : e.sound;
        if (!s.empty()) owner->sounds.push_back({s, e.pos, 1.0, 1.0});
    }

    void update(double dt) {
        // horn buttons held with the mouse keep the horn on (the game clears it when no key holds it)
        for (const Element& e : elements)
            if (e.bind == Bind::Horn && e.value >= 0.5) {
                if (e.bindArg == 1) sim.train->hornPrimary = true;
                if (e.bindArg == 2) sim.train->hornSecondary = true;
            }
        ScriptContext ctx(sim);
        ctx.trainObject = true;
        ctx.car = sim.train->def().spec.driverCar;
        for (std::size_t i = 0; i < elements.size(); ++i) {
            Element& e = elements[i];
            if (e.expr) e.value = clampValue(e, e.expr->evaluate(ctx, dt));
            else if (e.bind != Bind::None && e.bind != Bind::Key && e.bind != Bind::Command && int(i) != active)
                e.value = clampValue(e, readBind(e));
            // displayed value: first-order lag (needles, handles)
            if (e.damping > 0.0) e.shown += (e.value - e.shown) * std::min(1.0, dt / e.damping);
            else e.shown = e.value;
        }
        for (Alert& a : alerts) {
            const bool on = a.when->evaluate(ctx, dt) >= 0.5;
            bool fire = on && !a.active;
            if (on && a.repeat > 0.0 && (a.timer += dt) >= a.repeat) fire = true;
            if (fire) {
                a.timer = 0.0;
                if (!a.message.empty()) owner->messages.push_back({a.message, a.seconds});
                if (!a.sound.empty()) owner->sounds.push_back({a.sound, eye.value_or(driverEye()), 1.0, 1.0});
            }
            a.active = on;
        }
    }

    // ------------------------------------------------------------------ picking
    RigidPose pose(const Element& e) const {
        return {e.base * glm::dmat3(axisRotation(e.axis, float(spread(e.angles, e.shown, e.min, e.max)))),
                e.pos + e.base * (e.dir * spread(e.offsets, e.shown, e.min, e.max))};
    }
    ObjectId object(const Element& e) const {
        if (e.states.empty()) return kNoObject;
        if (e.states.size() == 1) return e.states[0];
        const long k = std::lround(e.shown - e.min);
        return e.states[std::size_t(std::clamp<long>(k, 0, long(e.states.size()) - 1))];
    }
    struct Box {
        glm::dvec3 lo, hi;
    };
    Box bounds(const Element& e) const {
        glm::dvec3 lo, hi;
        if (e.hitSet) {
            lo = e.hitMin, hi = e.hitMax;
        } else {
            lo = glm::dvec3(1e30), hi = glm::dvec3(-1e30);
            for (ObjectId id : e.states) {
                if (id == kNoObject) continue;
                const ObjectRecord& r = lib[id];
                if (r.state != LoadState::Ready || r.kind != ObjectKind::Static) continue;
                lo = glm::min(lo, glm::dvec3(r.bmin)), hi = glm::max(hi, glm::dvec3(r.bmax));
            }
            if (lo.x > hi.x) lo = glm::dvec3(-0.02), hi = glm::dvec3(0.02);
        }
        // tiny parts stay clickable
        const glm::dvec3 c = 0.5 * (lo + hi), h = glm::max(0.5 * (hi - lo), glm::dvec3(0.008));
        return {c - h, c + h};
    }
    int pick(const glm::dvec3& o, const glm::dvec3& d) const {
        int best = -1;
        double bestT = 3.0; // reach
        for (std::size_t i = 0; i < elements.size(); ++i) {
            const Element& e = elements[i];
            if (!e.interactive()) continue;
            const auto [R, t] = pose(e);
            const auto [lo, hi] = bounds(e);
            const glm::dmat3 Rt = glm::transpose(R);
            const glm::dvec3 lo2 = Rt * (o - t), ld = Rt * d;
            double t0 = 0.0, t1 = bestT;
            bool hit = true;
            for (int k = 0; k < 3 && hit; ++k) {
                if (std::abs(ld[k]) < 1e-12) {
                    if (lo2[k] < lo[k] || lo2[k] > hi[k]) hit = false;
                    continue;
                }
                double a = (lo[k] - lo2[k]) / ld[k], b = (hi[k] - lo2[k]) / ld[k];
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a), t1 = std::min(t1, b);
                if (t0 > t1) hit = false;
            }
            if (hit && t0 < bestT) bestT = t0, best = int(i);
        }
        return best;
    }

    void pressElement(int i, int button) {
        Element& e = elements[std::size_t(i)];
        e.pressed = true;
        e.dragAccum = 0.0;
        switch (e.kind) {
        case CabKind::Button: setValue(i, e.toggle ? (e.value >= 0.5 ? 0.0 : 1.0) : 1.0); break;
        case CabKind::Switch: setValue(i, e.value + (button == 1 ? -1.0 : 1.0)); break;
        default: break;
        }
    }
    void releaseElement(int i) {
        Element& e = elements[std::size_t(i)];
        e.pressed = false;
        if (e.kind == CabKind::Button && !e.toggle) setValue(i, 0.0);
        if (e.spring) setValue(i, *e.spring);
    }
};

Cab::Cab() = default;
Cab::~Cab() = default;

Expected<std::unique_ptr<Cab>> Cab::load(const std::filesystem::path& file, ObjectLibrary& lib, Simulation& sim) {
    const auto root = Json::parseFile(file);
    if (!root) return Unexpected(pathString(file) + ": " + root.error());
    std::unique_ptr<Cab> cab(new Cab());
    cab->impl_ = std::make_unique<Impl>(lib, sim);
    Impl& I = *cab->impl_;
    I.owner = cab.get();
    I.dir = file.parent_path();
    I.name = file.stem().string();
    try {
        I.build(*root);
    } catch (const LoadError& e) {
        return Unexpected(pathString(file) + ": " + e.what);
    }
    return cab;
}

void Cab::update(double dt) { impl_->update(dt); }

void Cab::hover(const glm::dvec3& o, const glm::dvec3& d) { impl_->hovered = impl_->active >= 0 ? impl_->active : impl_->pick(o, d); }

bool Cab::press(int button, const glm::dvec3& o, const glm::dvec3& d) {
    Impl& I = *impl_;
    if (I.active >= 0) return true;
    const int i = I.pick(o, d);
    if (i < 0) return false;
    I.active = i, I.activeButton = button;
    I.pressElement(i, button);
    return true;
}

void Cab::drag(double dx, double dy) {
    Impl& I = *impl_;
    if (I.active < 0) return;
    Element& e = I.elements[std::size_t(I.active)];
    if (e.kind != CabKind::Lever && e.kind != CabKind::Switch) return;
    e.dragAccum += dx * e.dragDir.x + dy * e.dragDir.y;
    const double step = e.continuous ? (e.max - e.min) / 10.0 : 1.0;
    while (e.dragAccum >= e.dragPixels) e.dragAccum -= e.dragPixels, I.setValue(I.active, e.value + step);
    while (e.dragAccum <= -e.dragPixels) e.dragAccum += e.dragPixels, I.setValue(I.active, e.value - step);
}

void Cab::release(int button) {
    Impl& I = *impl_;
    if (I.active < 0 || button != I.activeButton) return;
    I.releaseElement(I.active);
    I.active = -1;
}

bool Cab::scroll(const glm::dvec3& o, const glm::dvec3& d, double steps) {
    Impl& I = *impl_;
    const int i = I.pick(o, d);
    if (i < 0) return false;
    Element& e = I.elements[std::size_t(i)];
    if (e.kind != CabKind::Lever && e.kind != CabKind::Switch) return false;
    const double step = e.continuous ? (e.max - e.min) / 10.0 : 1.0;
    I.setValue(i, e.value + (steps > 0 ? step : -step));
    return true;
}

bool Cab::capturing() const { return impl_->active >= 0; }
int Cab::pick(const glm::dvec3& o, const glm::dvec3& d) const { return impl_->pick(o, d); }
const std::vector<ObjectId>& Cab::bodies() const { return impl_->bodies; }
std::size_t Cab::elementCount() const { return impl_->elements.size(); }
ObjectId Cab::elementObject(std::size_t i) const { return impl_->object(impl_->elements[i]); }
const std::vector<ObjectId>& Cab::elementStates(std::size_t i) const { return impl_->elements[i].states; }
RigidPose Cab::elementPose(std::size_t i) const { return impl_->pose(impl_->elements[i]); }
CabElementView Cab::view(std::size_t i) const {
    const Element& e = impl_->elements[i];
    return {e.name, e.tooltip, e.kind, e.interactive()};
}
int Cab::hovered() const { return impl_->hovered; }
std::optional<glm::dvec3> Cab::eye() const { return impl_->eye; }
const std::string& Cab::name() const { return impl_->name; }

} // namespace ots
