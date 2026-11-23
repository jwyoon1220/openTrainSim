// CSV route parser following openBVE's CsvRwRouteParser:
//   split into expressions → $Include/$Chr/$Rnd/$Sub/$If → options → stable sort by track
//   position → non-track namespaces → Track namespace into blocks → ApplyRouteData geometry.
#include "bve/Route.hpp"

#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <unordered_map>

namespace ots {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// ------------------------------------------------------------------ transformations (double)

struct Xf {
    glm::dvec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
};

glm::dvec3 rotateAround(const glm::dvec3& v, const glm::dvec3& d, double angle) {
    const double c = std::cos(angle), s = std::sin(angle), C = 1.0 - c;
    return {(c + C * d.x * d.x) * v.x + (C * d.x * d.y - s * d.z) * v.y + (C * d.x * d.z + s * d.y) * v.z,
            (C * d.x * d.y + s * d.z) * v.x + (c + C * d.y * d.y) * v.y + (C * d.y * d.z - s * d.x) * v.z,
            (C * d.x * d.z - s * d.y) * v.x + (C * d.y * d.z + s * d.x) * v.y + (c + C * d.z * d.z) * v.z};
}

Xf compose(Xf t, double yaw, double pitch, double roll) {
    if (yaw != 0.0) t.X = rotateAround(t.X, t.Y, yaw), t.Z = rotateAround(t.Z, t.Y, yaw);
    if (pitch != 0.0) t.Y = rotateAround(t.Y, t.X, -pitch), t.Z = rotateAround(t.Z, t.X, -pitch);
    if (roll != 0.0) t.X = rotateAround(t.X, t.Z, -roll), t.Y = rotateAround(t.Y, t.Z, -roll);
    return t;
}

Xf makeXf(double yaw, double pitch, double roll) {
    if (pitch == 0.0 && roll == 0.0) {
        Xf t;
        const double c = std::cos(yaw), s = std::sin(yaw);
        t.X = {c, 0, -s};
        t.Z = {s, 0, c};
        return t;
    }
    return compose(Xf{}, yaw, pitch, roll);
}

glm::dmat3 toMat(const Xf& t) { return glm::dmat3(t.X, t.Y, t.Z); }

glm::dvec2 rotate2(glm::dvec2 v, double c, double s) { return {v.x * c - v.y * s, v.x * s + v.y * c}; }

// ------------------------------------------------------------------ expressions

struct Expr {
    std::string text;
    u32 file = 0;
    u32 line = 0;
    double offset = 0.0;
    bool skip = false;
    double position = -1.0; // after sorting
    bool isPosition = false;
};

// ------------------------------------------------------------------ parse-time blocks

struct PRail {
    bool started = false, startRefreshed = false, ended = false;
    glm::dvec2 start{0.0}, end{0.0};
    double cant = 0.0;
};
struct PWallDike {
    bool exists = true;
    int type = 0;
    int dir = 0;
};
struct PPole {
    bool exists = false;
    int mode = 0;
    double location = 0, interval = 0;
    int type = 0;
};
struct PFreeObj {
    double trackPos;
    int type;
    double x, y, yaw, pitch, roll;
};
struct PForm {
    int primary, secondary, form, roof;
};
struct PCrack {
    int primary, secondary, type;
};
struct PSignal {
    double trackPos;
    int section, type;
    double x, y, yaw, pitch, roll;
};
struct PRailCycle {
    int cycle = -1, current = 0;
};
struct PBlock {
    int background = -1;
    Fog fog;
    bool fogDefined = false;
    std::vector<int> cycle;
    double height = std::numeric_limits<double>::quiet_NaN();
    std::map<int, PRail> rails;
    std::vector<int> railType;
    std::vector<PRailCycle> railCycles;
    std::map<int, PWallDike> walls, dikes;
    std::vector<PPole> poles;
    std::map<int, std::vector<PFreeObj>> railFreeObj;
    std::vector<PFreeObj> groundFreeObj;
    std::vector<PForm> forms;
    std::vector<PCrack> cracks;
    std::vector<PSignal> signals;
    double curveRadius = 0, cant = 0, pitch = 0, turn = 0;
};

constexpr int kFormL = -1, kFormR = -2, kFormStub = 0;

using Dict = std::map<int, std::filesystem::path>;

struct Parser {
    ObjectLibrary& lib;
    Route& route;
    RouteLoadStats& stats;
    std::vector<std::filesystem::path> files;
    std::vector<Expr> exprs;
    std::vector<double> unitFactors{1.0};
    double unitOfSpeed = 1.0 / 3.6;
    bool valueBasedSections = false;
    std::mt19937 rng{std::random_device{}()};

    // structure
    Dict railObj, ground, wallL, wallR, dikeL, dikeR, formL, formR, formCL, formCR, roofL, roofR, roofCL, roofCR, crackL, crackR, freeObj;
    std::map<int, Dict> poles;
    std::map<int, std::filesystem::path> signalObj;
    std::map<int, std::vector<int>> groundCycles, railCycles;

    // track state
    std::vector<PBlock> blocks;
    double trackPosition = 0.0;
    int firstUsedBlock = -1;
    int currentStation = -1, currentStop = -1, currentSection = 0;
    bool departureSignalUsed = false;
    std::vector<int> blockStation; // per block: station index started here (-1)

    struct StaDef {
        int block;
    };

    int warnBudget = 40;
    template <class... A> void warn(const Expr& e, std::format_string<A...> f, A&&... a) {
        if (warnBudget-- > 0)
            phx::log::warn("{}:{}: {}", files[e.file].filename().string(), e.line, std::format(f, std::forward<A>(a)...));
    }

    Parser(ObjectLibrary& l, Route& r, RouteLoadStats& s) : lib(l), route(r), stats(s) {}

    // -------------------------------------------------------------- splitting

    void splitIntoExpressions(u32 file, const std::string& text, double offset, std::vector<Expr>& out) {
        u32 lineNo = 0;
        for (std::string& line : splitLines(text)) {
            ++lineNo;
            line.erase(std::remove(line.begin(), line.end(), '\0'), line.end());
            int level = 0;
            std::size_t a = 0;
            auto emit = [&](std::size_t b, std::size_t e) {
                std::string_view t = trim(std::string_view(line).substr(b, e - b));
                if (!t.empty() && t.front() != ';') out.push_back({std::string(t), file, lineNo, offset});
            };
            for (std::size_t j = 0; j < line.size(); ++j) {
                if (line[j] == '(') ++level;
                else if (line[j] == ')') level = std::max(0, level - 1);
                else if (line[j] == ',' && level == 0) {
                    emit(a, j);
                    a = j + 1;
                }
            }
            if (a < line.size()) emit(a, line.size());
        }
    }

    bool readFile(const std::filesystem::path& p, std::string& text) {
        if (!readTextFile(p, text)) return false;
        ++stats.files;
        return true;
    }

    // $Include / $Chr / $Rnd / $Sub / $If (openBVE PreprocessChrRndSub).
    void preprocess() {
        std::vector<std::string> subs(16);
        int openIfs = 0;
        for (std::size_t i = 0; i < exprs.size(); ++i) {
            if (exprs[i].skip) continue;
            bool next = false;
            for (int j = int(exprs[i].text.size()) - 1; j >= 0 && !next; --j) {
                std::string& t = exprs[i].text;
                if (t[std::size_t(j)] != '$') continue;
                std::size_t k = std::size_t(j) + 1;
                for (; k < t.size(); ++k) {
                    if (t[k] == '(') break;
                    if (t[k] == '/' || t[k] == '\\') {
                        k = t.size() + 1;
                        break;
                    }
                }
                if (k > t.size()) break;
                const std::string name = lower(trim(std::string_view(t).substr(std::size_t(j) + 1, k - std::size_t(j) - 1)));
                static const char* known[] = {"if", "elseif", "else", "endif", "include", "chr", "chrascii", "rnd", "sub"};
                if (std::none_of(std::begin(known), std::end(known), [&](const char* n) { return name == n; })) break;
                int l = 1;
                std::size_t h = k + 1;
                for (; h < t.size(); ++h) {
                    if (t[h] == '(') ++l;
                    else if (t[h] == ')') --l;
                    if (l <= 0) break;
                }
                if (l != 0) {
                    warn(exprs[i], "invalid parenthesis in ${}", name);
                    break;
                }
                const std::string s(trim(std::string_view(t).substr(k + 1, h - k - 1)));
                if (name == "if" || name == "elseif") {
                    double num;
                    exprs[i].text.clear();
                    next = true;
                    if (!parseDouble(s, num)) break;
                    ++openIfs;
                    if (num == 0.0) {
                        ++i;
                        int level = 1;
                        while (i < exprs.size()) {
                            const std::string& x = exprs[i].text;
                            if (istartsWith(x, "$if")) exprs[i].skip = true, ++level;
                            else if (istartsWith(x, "$else")) {
                                if (istartsWith(x, "$elseif")) {
                                    --i;
                                    --level;
                                    break;
                                }
                                exprs[i].skip = true;
                                if (level == 1) {
                                    --level;
                                    break;
                                }
                            } else if (istartsWith(x, "$endif")) {
                                exprs[i].skip = true;
                                if (--level == 0) {
                                    --openIfs;
                                    break;
                                }
                            } else exprs[i].skip = true;
                            ++i;
                        }
                    }
                } else if (name == "else") {
                    exprs[i].skip = true;
                    next = true;
                    if (openIfs != 0) {
                        ++i;
                        int level = 1;
                        while (i < exprs.size()) {
                            const std::string& x = exprs[i].text;
                            if (istartsWith(x, "$if")) exprs[i].skip = true, ++level;
                            else if (istartsWith(x, "$else")) exprs[i].skip = true;
                            else if (istartsWith(x, "$endif")) {
                                exprs[i].skip = true;
                                if (--level == 0) {
                                    --openIfs;
                                    break;
                                }
                            } else exprs[i].skip = true;
                            ++i;
                        }
                    }
                } else if (name == "endif") {
                    exprs[i].skip = true;
                    if (openIfs) --openIfs;
                    next = true;
                } else if (name == "include") {
                    next = true;
                    if (j != 0) break;
                    auto args = split(s, ';');
                    const std::size_t count = (args.size() + 1) / 2;
                    std::vector<std::filesystem::path> incFiles;
                    std::vector<double> weights, offsets;
                    double total = 0.0;
                    bool bad = false;
                    for (std::size_t ia = 0; ia < count; ++ia) {
                        std::string_view arg = trim(args[2 * ia]);
                        double off = 0.0;
                        if (auto colon = arg.find(':'); colon != std::string_view::npos) {
                            if (!parseDouble(arg.substr(colon + 1), off)) {
                                warn(exprs[i], "invalid include offset");
                                bad = true;
                                break;
                            }
                            arg = trim(arg.substr(0, colon));
                        }
                        std::filesystem::path f = resolvePath(files[exprs[i].file].parent_path(), arg);
                        if (f.empty()) {
                            warn(exprs[i], "include not found: {}", arg);
                            bad = true;
                            break;
                        }
                        double w = 1.0;
                        if (2 * ia + 1 < args.size() && !parseDoubleVb6(args[2 * ia + 1], w)) w = 1.0;
                        incFiles.push_back(f);
                        weights.push_back(w);
                        offsets.push_back(off);
                        total += w;
                    }
                    if (bad || incFiles.empty()) {
                        exprs[i].skip = true;
                        break;
                    }
                    const double pick = std::uniform_real_distribution<double>(0.0, total)(rng);
                    std::size_t chosen = 0;
                    for (double acc = 0.0; chosen < incFiles.size(); ++chosen) {
                        acc += weights[chosen];
                        if (acc > pick) break;
                    }
                    chosen = std::min(chosen, incFiles.size() - 1);
                    std::string text;
                    exprs[i].skip = true;
                    if (!readFile(incFiles[chosen], text)) {
                        warn(exprs[i], "cannot read include {}", incFiles[chosen].string());
                        break;
                    }
                    files.push_back(incFiles[chosen]);
                    std::vector<Expr> sub;
                    splitIntoExpressions(u32(files.size() - 1), text, offsets[chosen] + exprs[i].offset, sub);
                    exprs.insert(exprs.begin() + std::ptrdiff_t(i), std::make_move_iterator(sub.begin()), std::make_move_iterator(sub.end()));
                    if (!sub.empty()) --i;
                } else if (name == "chr" || name == "chrascii") {
                    int x;
                    if (!parseIntVb6(s, x) || x < 0 || x > 0x10FFFF) {
                        next = true;
                        break;
                    }
                    std::string u;
                    if (x < 0x80) u = char(x);
                    else if (x < 0x800) u = {char(0xC0 | (x >> 6)), char(0x80 | (x & 0x3F))};
                    else if (x < 0x10000) u = {char(0xE0 | (x >> 12)), char(0x80 | ((x >> 6) & 0x3F)), char(0x80 | (x & 0x3F))};
                    else u = {char(0xF0 | (x >> 18)), char(0x80 | ((x >> 12) & 0x3F)), char(0x80 | ((x >> 6) & 0x3F)), char(0x80 | (x & 0x3F))};
                    t = t.substr(0, std::size_t(j)) + u + t.substr(h + 1);
                } else if (name == "rnd") {
                    auto parts = split(s, ';');
                    int x = 0, y = 0;
                    if (parts.size() >= 2 && parseIntVb6(parts[0], x) && parseIntVb6(parts[1], y)) {
                        const int z = x + int(std::floor(std::uniform_real_distribution<double>(0.0, 1.0)(rng) * (y - x + 1)));
                        t = t.substr(0, std::size_t(j)) + std::to_string(z) + t.substr(h + 1);
                    } else if (parseIntVb6(s, x)) {
                        t = t.substr(0, std::size_t(j)) + std::to_string(x) + t.substr(h + 1);
                    } else {
                        next = true;
                    }
                } else if (name == "sub") {
                    // $Sub(n) = value  or  $Sub(n)
                    std::size_t m = h + 1;
                    bool assign = false;
                    for (; m < t.size(); ++m) {
                        if (t[m] == '=') {
                            assign = true;
                            break;
                        }
                        if (!std::isspace(static_cast<unsigned char>(t[m]))) break;
                    }
                    int x;
                    if (!parseIntVb6(s, x) || x < 0) {
                        next = true;
                        break;
                    }
                    if (std::size_t(x) >= subs.size()) subs.resize(std::size_t(x) + 1);
                    if (assign) {
                        subs[std::size_t(x)] = std::string(trim(std::string_view(t).substr(m + 1)));
                        t = t.substr(0, std::size_t(j));
                    } else {
                        t = t.substr(0, std::size_t(j)) + subs[std::size_t(x)] + t.substr(h + 1);
                    }
                }
            }
        }
        for (Expr& e : exprs) {
            std::string_view t = trim(e.text);
            if (t.empty() || t.front() == ';') e.skip = true;
            else e.text = std::string(t);
        }
    }

    // -------------------------------------------------------------- command separation

    static void separate(const std::string& text, std::string& cmd, std::string& args) {
        int depth = 0;
        std::size_t ws = std::string::npos;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(') ++depth;
            else if (c == ')') depth = std::max(0, depth - 1);
            else if ((c == ' ' || c == '\t') && depth == 0) {
                ws = i;
                break;
            }
        }
        auto stripParens = [](std::string s) {
            if (s.size() >= 2 && s.front() == '(' && s.back() == ')') return std::string(trim(std::string_view(s).substr(1, s.size() - 2)));
            if (!s.empty() && s.front() == '(') return std::string(trim(std::string_view(s).substr(1)));
            return s;
        };
        if (ws != std::string::npos) {
            const std::string a(trim(std::string_view(text).substr(0, ws)));
            std::string rest(trim(std::string_view(text).substr(ws + 1)));
            if (a.find('(') != std::string::npos && a.find(')') != std::string::npos) {
                cmd = a;
                args = stripParens(rest);
            } else if (!rest.empty() && rest.front() == '(') {
                const std::size_t j = rest.find(')');
                if (j == std::string::npos || j == rest.size() - 1) {
                    cmd = a;
                    args = stripParens(rest);
                } else {
                    // "Command (indices).Sub args": border = first whitespace or '(' after ')'
                    std::size_t k = j + 1;
                    while (k < rest.size() && rest[k] != ' ' && rest[k] != '\t' && rest[k] != '(') ++k;
                    cmd = a + std::string(trim(std::string_view(rest).substr(0, k)));
                    args = stripParens(std::string(trim(std::string_view(rest).substr(k))));
                }
            } else {
                cmd = a;
                args = stripParens(rest);
            }
        } else if (!text.empty() && text.back() == ')') {
            const std::size_t k = text.rfind('(');
            if (k != std::string::npos) {
                cmd = std::string(trim(std::string_view(text).substr(0, k)));
                args = std::string(trim(std::string_view(text).substr(k + 1, text.size() - k - 2)));
            } else {
                cmd = text;
                args.clear();
            }
        } else if (const std::size_t k = text.find('('); k != std::string::npos) {
            cmd = std::string(trim(std::string_view(text).substr(0, k)));
            args = std::string(trim(std::string_view(text).substr(k + 1)));
        } else {
            cmd = text;
            args.clear();
        }
        while (!cmd.empty() && cmd.back() == ';') cmd.pop_back();
    }

    static std::vector<std::string> splitArgs(const std::string& s) {
        std::vector<std::string> out;
        if (s.empty()) return out;
        for (std::string_view a : split(s, ';')) out.emplace_back(trim(a));
        while (!out.empty() && out.back().empty()) out.pop_back(); // openBVE drops a trailing empty
        return out;
    }

    // Applies With-prefix, ".Load" style suffixes and indices. Returns false for no command.
    bool normalise(std::string& cmd, const std::string& section, bool alwaysPrefix, int idx[2]) {
        if (!cmd.empty() && cmd.front() == '.') cmd = section + cmd;
        else if (alwaysPrefix) cmd = section + "." + cmd;
        if (auto p = ifind(cmd, ".void"); p != std::string::npos) cmd.erase(p, 5);
        const std::string lc = lower(cmd);
        auto stripSuffix = [&](std::string_view suf) {
            if (iendsWith(cmd, suf)) cmd = std::string(trim(std::string_view(cmd).substr(0, cmd.size() - suf.size())));
        };
        if (lc.starts_with("structure") || lc.starts_with("signal") || lc.starts_with("texture.background")) stripSuffix(".load");
        if (lc.starts_with("cycle")) stripSuffix(".params");
        if (lc.starts_with("route.signal") || lc.starts_with("train.run") || lc.starts_with("train.flange")) stripSuffix(".set");
        if (lower(cmd).starts_with("texture.")) cmd = "structure." + cmd.substr(8);
        idx[0] = idx[1] = 0;
        if (!cmd.empty() && cmd.back() == ')') {
            const std::size_t k = cmd.rfind('(');
            if (k != std::string::npos) {
                const std::string inner(trim(std::string_view(cmd).substr(k + 1, cmd.size() - k - 2)));
                cmd = std::string(trim(std::string_view(cmd).substr(0, k)));
                const auto parts = split(inner, ';');
                if (!parts.empty() && !trim(parts[0]).empty() && !parseIntVb6(parts[0], idx[0])) return false;
                if (parts.size() > 1 && !trim(parts[1]).empty() && !parseIntVb6(parts[1], idx[1])) return false;
            }
        }
        return !cmd.empty();
    }

    // -------------------------------------------------------------- options & sorting

    void preprocessOptions() {
        std::string section;
        bool always = false;
        for (Expr& e : exprs) {
            if (e.skip) continue;
            std::string cmd, args;
            separate(e.text, cmd, args);
            if (iequals(cmd, "with")) {
                section = args;
                always = false;
                continue;
            }
            int idx[2];
            if (!normalise(cmd, section, always, idx)) continue;
            const std::string lc = lower(cmd);
            const auto a = splitArgs(args);
            if (lc == "options.unitoflength") {
                unitFactors.clear();
                for (const auto& s : a) {
                    double f;
                    unitFactors.push_back(parseDoubleVb6(s, f) && f > 0 ? f : 1.0);
                }
                if (unitFactors.empty()) unitFactors = {1.0};
            } else if (lc == "options.unitofspeed") {
                double f;
                if (!a.empty() && parseDoubleVb6(a[0], f) && f > 0) unitOfSpeed = f / 3.6;
            } else if (lc == "options.blocklength") {
                double f;
                if (!a.empty() && parseDoubleVb6(a[0], f) && f > 0) route.blockLength = f;
            } else if (lc == "options.sectionbehavior") {
                int v;
                if (!a.empty() && parseIntVb6(a[0], v)) valueBasedSections = v == 1;
            }
        }
    }

    void sortByPosition() {
        std::vector<Expr> out;
        out.reserve(exprs.size() + exprs.size() / 4);
        double a = -1.0, pa = -1.0;
        std::vector<std::pair<double, std::size_t>> keyed;
        for (Expr& e : exprs) {
            if (e.skip) continue;
            double x;
            if (parseLength(e.text, unitFactors, x)) {
                x += e.offset;
                if (x >= 0.0) a = x;
                continue;
            }
            if (pa != a) {
                Expr p;
                p.isPosition = true;
                p.position = a;
                p.file = e.file;
                p.line = e.line;
                keyed.push_back({a, out.size()});
                out.push_back(std::move(p));
                pa = a;
            }
            e.position = a;
            keyed.push_back({a, out.size()});
            out.push_back(std::move(e));
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        exprs.clear();
        exprs.reserve(keyed.size());
        for (auto& [pos, i] : keyed) exprs.push_back(std::move(out[i]));
        stats.expressions = exprs.size();
    }

    // -------------------------------------------------------------- non-track namespaces

    std::filesystem::path objectPath(const Expr& e, const std::string& rel) {
        if (rel.empty()) return {};
        std::filesystem::path p = resolvePath(route.objectFolder, rel);
        if (p.empty()) {
            ++stats.missingObjects;
            warn(e, "object not found: {}", rel);
        }
        return p;
    }

    static double parseTime(std::string_view s) {
        s = trim(s);
        if (s.empty()) return -1.0;
        const auto dot = s.find('.');
        int h;
        if (dot == std::string_view::npos) {
            if (!parseIntVb6(s, h)) return -1.0;
            return h * 3600.0;
        }
        if (!parseIntVb6(s.substr(0, dot), h)) return -1.0;
        std::string frac(s.substr(dot + 1));
        while (frac.size() < 4) frac.push_back('0');
        int mm = 0, ss = 0;
        parseIntVb6(frac.substr(0, 2), mm);
        parseIntVb6(frac.substr(2, 2), ss);
        return h * 3600.0 + mm * 60.0 + ss;
    }

    void nonTrackPass() {
        std::string section;
        bool always = false;
        double theta = 60.0, phi = -26.57;
        for (const Expr& e : exprs) {
            if (e.isPosition || e.skip) continue;
            if (e.text.size() > 1 && e.text.front() == '[' && e.text.back() == ']') {
                section = e.text.substr(1, e.text.size() - 2);
                if (iequals(section, "object")) section = "Structure";
                else if (iequals(section, "railway")) section = "Track";
                always = true;
                continue;
            }
            std::string cmd, args;
            separate(e.text, cmd, args);
            double tmp;
            if (parseLength(cmd, unitFactors, tmp)) continue;
            if (iequals(cmd, "with")) {
                section = args;
                always = false;
                continue;
            }
            int idx[2];
            if (!normalise(cmd, section, always, idx)) continue;
            const auto period = cmd.find('.');
            std::string ns = period == std::string::npos ? std::string() : lower(cmd.substr(0, period));
            std::string c = lower(period == std::string::npos ? cmd : cmd.substr(period + 1));
            if (ns.starts_with("signal")) {
                c = ns;
                ns.clear();
            }
            const auto a = splitArgs(args);
            auto num = [&](std::size_t i, double def) {
                double v;
                return (i < a.size() && !a[i].empty() && parseDoubleVb6(a[i], v)) ? v : def;
            };
            if (ns == "route") {
                if (c == "comment") route.comment = args;
                else if (c == "timetable") route.timetable = args;
                else if (c == "gauge") route.gauge = num(0, 1435.0) * 0.001;
                else if (c == "signal") {
                    if (idx[0] >= 0) {
                        if (route.signalSpeeds.size() <= std::size_t(idx[0]))
                            route.signalSpeeds.resize(std::size_t(idx[0]) + 1, std::numeric_limits<double>::infinity());
                        const double v = num(0, 0.0);
                        route.signalSpeeds[std::size_t(idx[0])] = v <= 0 ? std::numeric_limits<double>::infinity() : v * unitOfSpeed;
                    }
                } else if (c == "ambientlight") {
                    route.ambient = glm::vec3(float(num(0, 160)), float(num(1, 160)), float(num(2, 160))) / 255.0f;
                } else if (c == "directionallight") {
                    route.diffuse = glm::vec3(float(num(0, 160)), float(num(1, 160)), float(num(2, 160))) / 255.0f;
                } else if (c == "lightdirection") {
                    theta = num(0, 60.0), phi = num(1, -26.57);
                } else if (c == "fog") {
                    const double s = num(0, 0), en = num(1, 0);
                    if (s < en) route.initialFog = {float(s), float(en), u32(num(2, 128)) | (u32(num(3, 128)) << 8) | (u32(num(4, 128)) << 16)};
                } else if (c == "starttime") {
                    route.startTime = a.empty() ? -1.0 : parseTime(a[0]);
                }
            } else if (ns == "train") {
                if ((c == "folder" || c == "file") && !args.empty()) route.trainFolder = args;
            else if ((c == "run" || c == "rail") && idx[0] >= 0) {
                if (route.runSounds.size() <= std::size_t(idx[0])) route.runSounds.resize(std::size_t(idx[0]) + 1, 0);
                route.runSounds[std::size_t(idx[0])] = std::max(0, int(num(0, 0)));
            } else if (c == "flange" && idx[0] >= 0) {
                if (route.flangeSounds.size() <= std::size_t(idx[0])) route.flangeSounds.resize(std::size_t(idx[0]) + 1, 0);
                route.flangeSounds[std::size_t(idx[0])] = std::max(0, int(num(0, 0)));
            }
                else if (c == "gauge") route.gauge = num(0, 1435.0) * 0.001;
            } else if (ns == "structure") {
                const std::string file = a.empty() ? std::string() : a[0];
                auto put = [&](Dict& d) {
                    if (idx[0] < 0) return;
                    std::filesystem::path p = objectPath(e, file);
                    if (!p.empty()) d[idx[0]] = p;
                };
                if (c == "rail") put(railObj);
                else if (c == "ground") put(ground);
                else if (c == "walll") put(wallL);
                else if (c == "wallr") put(wallR);
                else if (c == "dikel") put(dikeL);
                else if (c == "diker") put(dikeR);
                else if (c == "forml") put(formL);
                else if (c == "formr") put(formR);
                else if (c == "formcl") put(formCL);
                else if (c == "formcr") put(formCR);
                else if (c == "roofl") put(roofL);
                else if (c == "roofr") put(roofR);
                else if (c == "roofcl") put(roofCL);
                else if (c == "roofcr") put(roofCR);
                else if (c == "crackl") put(crackL);
                else if (c == "crackr") put(crackR);
                else if (c == "freeobj") put(freeObj);
                else if (c == "pole") {
                    std::filesystem::path p = objectPath(e, file);
                    if (!p.empty() && idx[0] >= 0 && idx[1] >= 0) poles[idx[0]][idx[1]] = p;
                } else if (c == "background" || c == "back") {
                    if (idx[0] >= 0 && !file.empty()) {
                        if (route.backgrounds.size() <= std::size_t(idx[0])) route.backgrounds.resize(std::size_t(idx[0]) + 1);
                        std::filesystem::path p = resolvePath(route.objectFolder, file);
                        if (p.empty()) warn(e, "background not found: {}", file);
                        route.backgrounds[std::size_t(idx[0])].texture = p;
                    }
                } else if (c == "backgroundx" || c == "backx") {
                    if (idx[0] >= 0) {
                        if (route.backgrounds.size() <= std::size_t(idx[0])) route.backgrounds.resize(std::size_t(idx[0]) + 1);
                        route.backgrounds[std::size_t(idx[0])].repetitions = std::max(1, int(num(0, 6)));
                    }
                } else if (c == "backgroundaspect" || c == "backaspect") {
                    if (idx[0] >= 0) {
                        if (route.backgrounds.size() <= std::size_t(idx[0])) route.backgrounds.resize(std::size_t(idx[0]) + 1);
                        route.backgrounds[std::size_t(idx[0])].keepAspect = num(0, 0) == 1;
                    }
                }
            } else if (ns.empty() && c == "signal") {
                if (idx[0] >= 0 && !a.empty()) {
                    std::filesystem::path p = objectPath(e, a[0]);
                    if (!p.empty()) {
                        if (ObjectLibrary::kindOf(p) == ObjectKind::Unsupported) {
                            // BVE4 bitmap signal (no extension): not supported yet
                            warn(e, "BVE4 image signals are not supported: {}", a[0]);
                        } else {
                            signalObj[idx[0]] = p;
                        }
                    }
                }
            } else if (ns == "cycle") {
                std::vector<int> list;
                for (const auto& s : a) {
                    int v;
                    list.push_back(parseIntVb6(s, v) ? v : 0);
                }
                if (c == "ground") groundCycles[idx[0]] = list;
                else if (c == "rail") railCycles[idx[0]] = list;
            } else if (ns == "options" || ns == "track") {
                // options handled earlier; track in the next pass
            } else {
                ++stats.unknownCommands;
                ++stats.unknown[ns + "." + c];
            }
        }
        theta *= kDeg, phi *= kDeg;
        const double dx = std::cos(theta) * std::sin(phi), dy = -std::sin(theta), dz = std::cos(theta) * std::cos(phi);
        route.lightDirection = glm::normalize(glm::vec3(float(-dx), float(-dy), float(-dz)));
    }

    // -------------------------------------------------------------- track namespace

    void createMissingBlocks(int to) {
        while (int(blocks.size()) <= to) {
            const PBlock& p = blocks.back();
            PBlock b;
            b.background = -1;
            b.fog = p.fog;
            b.cycle = p.cycle;
            b.railCycles = p.railCycles;
            b.railType.resize(p.railType.size());
            for (std::size_t j = 0; j < b.railType.size(); ++j) {
                const int rc = j < b.railCycles.size() ? b.railCycles[j].cycle : -1;
                auto it = rc >= 0 ? railCycles.find(rc) : railCycles.end();
                if (it != railCycles.end() && it->second.size() > 1) {
                    int& cc = b.railCycles[j].current;
                    cc = (cc + 1) % int(it->second.size());
                    b.railType[j] = it->second[std::size_t(cc)];
                } else {
                    b.railType[j] = p.railType[j];
                }
            }
            for (const auto& [k, r] : p.rails) {
                PRail n;
                n.started = r.started;
                n.start = r.start;
                n.end = r.start;
                b.rails[k] = n;
            }
            for (const auto& [k, w] : p.walls)
                if (w.exists) b.walls[k] = w;
            for (const auto& [k, w] : p.dikes)
                if (w.exists) b.dikes[k] = w;
            b.poles = p.poles;
            b.pitch = p.pitch;
            b.curveRadius = p.curveRadius;
            b.cant = p.cant;
            blocks.push_back(std::move(b));
            blockStation.push_back(-1);
        }
    }

    void ensureRailType(PBlock& b, int idx) {
        if (int(b.railType.size()) <= idx) b.railType.resize(std::size_t(idx) + 1, 0);
        if (int(b.railCycles.size()) <= idx) b.railCycles.resize(std::size_t(idx) + 1);
    }

    void setRailType(PBlock& b, int idx, int type) {
        ensureRailType(b, idx);
        auto it = railCycles.find(type);
        if (it != railCycles.end() && !it->second.empty()) {
            b.railType[std::size_t(idx)] = it->second[0];
            b.railCycles[std::size_t(idx)] = {type, 0};
        } else {
            b.railType[std::size_t(idx)] = type;
            b.railCycles[std::size_t(idx)] = {-1, 0};
        }
    }

    static int direction(std::string_view s) {
        s = trim(s);
        if (iequals(s, "L") || s == "-1") return -1;
        if (iequals(s, "R") || s == "1") return 1;
        if (iequals(s, "B") || s == "0") return 0;
        return 2; // invalid
    }

    void trackPass() {
        std::string section;
        bool always = false;
        int blockIndex = 0;
        for (const Expr& e : exprs) {
            if (e.skip) continue;
            if (e.isPosition) {
                if (e.position < 0.0) continue;
                trackPosition = e.position;
                blockIndex = int(std::floor(trackPosition / route.blockLength + 0.001));
                if (firstUsedBlock == -1) firstUsedBlock = blockIndex;
                createMissingBlocks(blockIndex);
                continue;
            }
            if (e.text.size() > 1 && e.text.front() == '[' && e.text.back() == ']') {
                section = e.text.substr(1, e.text.size() - 2);
                if (iequals(section, "railway")) section = "Track";
                always = true;
                continue;
            }
            std::string cmd, args;
            separate(e.text, cmd, args);
            if (cmd == ".") continue;
            if (iequals(cmd, "with")) {
                section = args;
                always = false;
                continue;
            }
            int idx[2];
            if (!normalise(cmd, section, always, idx)) continue;
            const auto period = cmd.find('.');
            if (period == std::string::npos || !iequals(cmd.substr(0, period), "track")) continue;
            const std::string c = lower(cmd.substr(period + 1));
            trackCommand(e, c, splitArgs(args), blockIndex);
        }
    }

    void trackCommand(const Expr& e, const std::string& c, const std::vector<std::string>& a, int bi) {
        PBlock& b = blocks[std::size_t(bi)];
        auto num = [&](std::size_t i, double def) {
            double v;
            return (i < a.size() && !a[i].empty() && parseDoubleVb6(a[i], v)) ? v : def;
        };
        auto inum = [&](std::size_t i, int def) {
            int v;
            return (i < a.size() && !a[i].empty() && parseIntVb6(a[i], v)) ? v : def;
        };
        auto railStarted = [&](int idx) {
            auto it = b.rails.find(idx);
            return it != b.rails.end() && it->second.started;
        };

        if (c == "rail" || c == "railstart") {
            const int idx = inum(0, 0);
            if (idx < 1) return;
            PRail& r = b.rails[idx];
            if (r.startRefreshed) r.ended = true;
            r.started = true;
            r.startRefreshed = true;
            if (a.size() >= 2) {
                if (!a[1].empty()) r.start.x = num(1, 0.0);
                if (!r.ended) r.end.x = r.start.x;
            }
            if (a.size() >= 3) {
                if (!a[2].empty()) r.start.y = num(2, 0.0);
                if (!r.ended) r.end.y = r.start.y;
            }
            ensureRailType(b, idx);
            if (a.size() >= 4 && !a[3].empty()) {
                const int t = inum(3, 0);
                if (t >= 0 && railObj.count(t)) setRailType(b, idx, t);
            }
            r.cant = num(4, 0.0) * 0.001;
        } else if (c == "railend") {
            const int idx = inum(0, 0);
            if (idx <= 0 || !railStarted(idx)) return;
            PRail& r = b.rails[idx];
            r.started = false;
            r.startRefreshed = false;
            r.ended = true;
            if (a.size() >= 2 && !a[1].empty()) r.end.x = num(1, 0.0);
            if (a.size() >= 3 && !a[2].empty()) r.end.y = num(2, 0.0);
        } else if (c == "railtype") {
            const int idx = std::max(0, inum(0, 0));
            const int t = inum(1, 0);
            if (t >= 0 && railObj.count(t)) setRailType(b, idx, t);
        } else if (c == "pitch") {
            b.pitch = num(0, 0.0) * 0.001;
        } else if (c == "curve") {
            const double radius = num(0, 0.0);
            double cant = num(1, 0.0) * 0.001;
            cant = std::abs(cant) * (radius > 0 ? 1.0 : (radius < 0 ? -1.0 : 0.0));
            b.curveRadius = radius;
            b.cant = cant;
        } else if (c == "turn") {
            b.turn = num(0, 0.0);
        } else if (c == "height") {
            b.height = num(0, 0.0);
        } else if (c == "ground") {
            const int t = inum(0, 0);
            auto it = groundCycles.find(t);
            if (it != groundCycles.end() && !it->second.empty()) b.cycle = it->second;
            else if (ground.count(t)) b.cycle = {t};
        } else if (c == "brightness") {
            float v = float(num(0, 255.0) / 255.0);
            route.brightness.push_back({trackPosition, std::clamp(v, 0.0f, 1.0f)});
        } else if (c == "fog") {
            const double s = num(0, 0.0), en = num(1, 0.0);
            const u32 col = u32(std::clamp(inum(2, 128), 0, 255)) | (u32(std::clamp(inum(3, 128), 0, 255)) << 8) |
                            (u32(std::clamp(inum(4, 128), 0, 255)) << 16);
            b.fog = s < en ? Fog{float(s), float(en), col} : Fog{0.0f, 0.0f, col};
            b.fogDefined = true;
        } else if (c == "back" || c == "background") {
            const int t = inum(0, 0);
            if (t >= 0 && std::size_t(t) < route.backgrounds.size() && !route.backgrounds[std::size_t(t)].texture.empty()) b.background = t;
        } else if (c == "freeobj") {
            if (a.size() < 2) return;
            const int idx = inum(0, 0), t = inum(1, 0);
            if (idx < -1 || t < 0 || !freeObj.count(t)) return;
            PFreeObj f{trackPosition, t, num(2, 0.0), num(3, 0.0), num(4, 0.0) * kDeg, num(5, 0.0) * kDeg, num(6, 0.0) * kDeg};
            if (idx == -1) b.groundFreeObj.push_back(f);
            else b.railFreeObj[idx].push_back(f);
        } else if (c == "wall" || c == "dike") {
            const int idx = std::max(0, inum(0, 0));
            int dir = a.size() >= 2 && !a[1].empty() ? direction(a[1]) : 0;
            if (dir == 2) return;
            const int t = std::max(0, inum(2, 0));
            const Dict& L = c == "wall" ? wallL : dikeL;
            const Dict& R = c == "wall" ? wallR : dikeR;
            if (dir == 0) {
                if (!L.count(t)) dir = 1;
                if (!R.count(t)) dir = -1;
            }
            if ((dir < 0 && !L.count(t)) || (dir > 0 && !R.count(t))) return;
            (c == "wall" ? b.walls : b.dikes)[idx] = PWallDike{true, t, dir};
        } else if (c == "wallend" || c == "dikeend") {
            const int idx = inum(0, 0);
            auto& m = c == "wallend" ? b.walls : b.dikes;
            auto it = m.find(idx);
            if (it != m.end()) it->second.exists = false;
        } else if (c == "pole") {
            const int idx = inum(0, 0);
            if (idx < 0) return;
            if (int(b.poles.size()) <= idx) {
                b.poles.resize(std::size_t(idx) + 1);
                b.poles[std::size_t(idx)].interval = 2.0 * route.blockLength;
            }
            PPole& p = b.poles[std::size_t(idx)];
            int mode = a.size() >= 2 && !a[1].empty() ? inum(1, 0) : p.mode;
            if (a.size() >= 3 && !a[2].empty()) p.location = num(2, 0.0);
            if (a.size() >= 4 && !a[3].empty()) p.interval = num(3, route.blockLength);
            const int t = a.size() >= 5 && !a[4].empty() ? inum(4, 0) : p.type;
            auto pm = poles.find(mode);
            if (pm == poles.end() || !pm->second.count(t)) {
                p.mode = mode;
                p.type = t;
                p.exists = false;
                return;
            }
            p.mode = mode;
            p.type = t;
            p.exists = true;
        } else if (c == "poleend") {
            const int idx = inum(0, 0);
            if (idx >= 0 && idx < int(b.poles.size())) b.poles[std::size_t(idx)].exists = false;
        } else if (c == "form") {
            const int idx1 = inum(0, 0);
            int idx2 = 0;
            if (a.size() >= 2 && !a[1].empty()) {
                if (iequals(a[1], "L")) idx2 = kFormL;
                else if (iequals(a[1], "R")) idx2 = kFormR;
                else idx2 = inum(1, 0);
            }
            if (idx1 < 0 || (idx2 < 0 && idx2 != kFormL && idx2 != kFormR)) return;
            int roof = inum(2, 0);
            if (roof < 0) roof = 0;
            const int pf = inum(3, 0);
            if (pf < 0 || (!formL.count(pf) && !formR.count(pf))) return;
            b.forms.push_back({idx1, idx2, pf, roof});
        } else if (c == "crack") {
            const int i1 = inum(0, 0), i2 = inum(1, 0), t = inum(2, 0);
            if (i1 < 0 || i2 < 0 || i1 == i2 || t < 0) return;
            b.cracks.push_back({i1, i2, t});
        } else if (c == "section" || c == "sections" || c == "sectionp") {
            Section s;
            s.trackPos = trackPosition;
            for (const std::string& arg : a) {
                int v;
                s.aspects.push_back(parseIntVb6(arg, v) && v >= 0 ? v : -1);
            }
            if (s.aspects.empty()) s.aspects = {0};
            s.type = (valueBasedSections || c == "sections") ? SectionType::ValueBased : SectionType::IndexBased;
            if (s.type == SectionType::ValueBased) std::sort(s.aspects.begin(), s.aspects.end());
            if (currentStation >= 0 && route.stations[std::size_t(currentStation)].forcedRedSignal && currentStop >= 0 && !departureSignalUsed) {
                s.departureStation = currentStation;
                departureSignalUsed = true;
            }
            route.sections.push_back(std::move(s));
            ++currentSection;
        } else if (c == "sigf") {
            const int obj = inum(0, 0);
            if (!signalObj.count(obj)) {
                warn(e, "SigF: signal {} not defined", obj);
                return;
            }
            const int sec = inum(1, 0);
            double y = num(3, 0.0);
            b.signals.push_back({trackPosition, currentSection + sec, obj, num(2, 0.0), y < 0.0 ? 4.8 : y, num(4, 0.0) * kDeg,
                                 num(5, 0.0) * kDeg, num(6, 0.0) * kDeg});
        } else if (c == "limit") {
            const double l = num(0, 0.0);
            if (l < 0.0) return;
            route.limits.push_back({trackPosition, l == 0.0 ? std::numeric_limits<double>::infinity() : l * unitOfSpeed});
        } else if (c == "sta" || c == "station") {
            ++currentStation;
            currentStop = -1;
            departureSignalUsed = false;
            Station st;
            st.trackPos = trackPosition;
            if (!a.empty()) st.name = a[0];
            if (a.size() >= 2 && !a[1].empty()) {
                const std::string s = lower(a[1]);
                if (s == "p" || s == "l") st.mode = StopMode::AllPass;
                else if (s == "b") st.mode = StopMode::PlayerPass;
                else if (s.starts_with("b:")) st.mode = StopMode::PlayerPass, st.arrival = parseTime(s.substr(2));
                else if (s == "s") st.mode = StopMode::PlayerStop;
                else if (s.starts_with("s:")) st.mode = StopMode::PlayerStop, st.arrival = parseTime(s.substr(2));
                else if (s != ".") st.arrival = parseTime(s);
            }
            if (a.size() >= 3 && !a[2].empty()) {
                const std::string s = lower(a[2]);
                if (s == "t" || s == "=") st.terminal = true;
                else if (s.starts_with("t:")) st.terminal = true, st.departure = parseTime(s.substr(2));
                else if (s == "c") {
                } else if (s.starts_with("c:")) st.departure = parseTime(s.substr(2));
                else if (s != ".") st.departure = parseTime(s);
            }
            if (a.size() >= 5 && !a[4].empty()) {
                const int d = direction(a[4]);
                st.doors = d == 2 ? 0 : d;
            }
            st.forcedRedSignal = inum(5, 0) == 1;
            if (a.size() >= 8 && !trim(a[7]).empty()) {
                st.arrivalSound = resolvePath(route.soundFolder, trim(a[7]));
                if (st.arrivalSound.empty()) warn(e, "sound not found: {}", a[7]);
            }
            if (a.size() >= 11 && !trim(a[10]).empty()) {
                st.departureSound = resolvePath(route.soundFolder, trim(a[10]));
                if (st.departureSound.empty()) warn(e, "sound not found: {}", a[10]);
            }
            st.stopDuration = std::max(5.0, num(8, 15.0));
            route.stations.push_back(std::move(st));
            blockStation[std::size_t(bi)] = currentStation;
        } else if (c == "stop") {
            if (currentStation < 0) {
                warn(e, "Track.Stop without a station");
                return;
            }
            StopPoint sp;
            sp.trackPos = trackPosition;
            sp.backward = num(1, 5.0);
            sp.forward = num(2, 5.0);
            if (sp.backward <= 0) sp.backward = 5.0;
            if (sp.forward <= 0) sp.forward = 5.0;
            sp.cars = inum(3, 0);
            route.stations[std::size_t(currentStation)].stops.push_back(sp);
            currentStop = sp.cars;
        } else if (c == "beacon" || c == "transponder" || c == "tr") {
            Beacon bc;
            bc.trackPos = trackPosition;
            if (c == "beacon") {
                bc.type = inum(0, 0);
                const int section = inum(2, 0);
                bc.optional = inum(3, 0);
                bc.section = section == -1 ? -1 : (section < 0 ? currentSection + 1 : currentSection + section);
            } else { // Track.Transponder type;signal;switch → beacon type 0..4 (openBVE mapping)
                bc.type = inum(0, 0);
                const int sig = inum(1, 0);
                bc.optional = inum(2, 0);
                bc.section = sig < 0 ? -1 : currentSection + sig + 1;
            }
            if (bc.type >= 0) route.beacons.push_back(bc);
        } else if (c == "announce") {
            if (!a.empty() && !trim(a[0]).empty()) {
                RouteSound rs;
                rs.trackPos = trackPosition;
                rs.file = resolvePath(route.soundFolder, trim(a[0]));
                rs.speed = num(1, 0.0) * unitOfSpeed;
                if (rs.file.empty()) warn(e, "sound not found: {}", a[0]);
                else route.announcements.push_back(std::move(rs));
            }
        } else if (c == "announceall" || c == "accuracy" ||
                   c == "adhesion" || c == "signal" || c == "sig" || c == "relay" || c == "marker" || c == "textmarker" ||
                   c == "pretrain" || c == "poi" || c == "pointofinterest" || c == "doppler" || c == "buffer" || c == "destination" ||
                   c == "ambientlight" || c == "directionallight" || c == "lightdirection" || c == "dynamiclight" || c == "atssn" ||
                   c == "atsp" || c == "pattern" || c == "plimit" || c == "hornblow" || c == "rain" || c == "snow" || c == "micsound") {
            // Not needed for the visual/physical simulation yet.
        } else {
            ++stats.unknownCommands;
            ++stats.unknown["track." + c];
        }
    }

    // -------------------------------------------------------------- ApplyRouteData

    std::vector<std::vector<Placement>> blockPlacements;
    std::vector<glm::dvec3> blockOrigins;

    void place(int block, ObjectId obj, const glm::dvec3& world, const glm::dmat3& rot, double trackPos, int section = -1) {
        if (obj == kNoObject) return;
        Placement p;
        p.object = obj;
        p.pos = glm::vec3(world - blockOrigins[std::size_t(block)]);
        p.rot = glm::quat_cast(glm::mat3(rot));
        p.trackOffset = float(trackPos - block * route.blockLength);
        p.section = section;
        blockPlacements[std::size_t(block)].push_back(p);
    }
    ObjectId objectFrom(const Dict& d, int key, ObjectVariant v = {}) {
        auto it = d.find(key);
        return it == d.end() ? kNoObject : lib.intern(it->second, v);
    }

    void apply() {
        if (firstUsedBlock < 0) firstUsedBlock = 0;
        const int lastBlock = int(std::floor((trackPosition + 600.0) / route.blockLength + 0.001)) + 1;
        createMissingBlocks(lastBlock + 1);
        // interpolate height
        {
            int z = 0;
            for (int i = 0; i < int(blocks.size()); ++i) {
                if (std::isnan(blocks[std::size_t(i)].height)) continue;
                for (int j = i - 1; j >= 0; --j) {
                    if (std::isnan(blocks[std::size_t(j)].height)) continue;
                    double av = blocks[std::size_t(j)].height;
                    const double d = (blocks[std::size_t(i)].height - av) / (i - j);
                    for (int k = j + 1; k < i; ++k) blocks[std::size_t(k)].height = (av += d);
                    break;
                }
                z = i;
            }
            for (std::size_t i = std::size_t(z) + 1; i < blocks.size(); ++i) blocks[i].height = blocks[std::size_t(z)].height;
        }
        const int nBlocks = int(blocks.size()) - 1; // the last block only provides "next" data
        route.blocks.resize(std::size_t(nBlocks));
        blockPlacements.assign(std::size_t(nBlocks), {});
        blockOrigins.assign(std::size_t(nBlocks), glm::dvec3(0.0));
        const double L = route.blockLength;
        glm::dvec3 position(0.0);
        glm::dvec2 dir(0.0, 1.0);
        int lastBackground = route.backgrounds.empty() ? -1 : 0;
        Fog fog = route.initialFog;

        for (int i = 0; i < nBlocks; ++i) {
            PBlock& B = blocks[std::size_t(i)];
            PBlock& N = blocks[std::size_t(i) + 1];
            RouteBlock& rb = route.blocks[std::size_t(i)];
            const double startDist = i * L;
            dir = glm::normalize(dir);
            if (B.cycle.size() == 1 && B.cycle[0] == -1) {
                auto it = groundCycles.find(0);
                B.cycle = (it != groundCycles.end() && !it->second.empty()) ? it->second : std::vector<int>{0};
            }
            if (B.cycle.empty()) B.cycle = {0};
            // turn
            if (B.turn != 0.0) {
                const double ag = -std::atan(B.turn);
                dir = rotate2(dir, std::cos(ag), std::sin(ag));
            }
            rb.origin = position;
            rb.startDir = dir;
            {   // train sounds (openBVE ApplyRouteData "rail sounds" / "point sound", rail 0 only)
                const int rt = B.railType.empty() ? 0 : B.railType[0];
                if (rt >= 0 && std::size_t(rt) < route.runSounds.size()) rb.runSound = i32(route.runSounds[std::size_t(rt)]);
                if (rt >= 0 && std::size_t(rt) < route.flangeSounds.size()) rb.flangeSound = i32(route.flangeSounds[std::size_t(rt)]);
                auto r0 = B.rails.find(0);
                auto n0 = N.rails.find(0);
                if (r0 != B.rails.end() && r0->second.started && n0 != N.rails.end()) {
                    for (const auto& [k, rk] : B.rails) {
                        if (k == 0 || !rk.started) continue;
                        auto nk = N.rails.find(k);
                        if (nk == N.rails.end()) continue;
                        const auto sgn = [](double v) { return (v > 0) - (v < 0); };
                        const bool qx = sgn(rk.start.x - r0->second.start.x) != sgn(nk->second.end.x - n0->second.end.x);
                        const bool qy = (rk.start.y - r0->second.start.y) * (nk->second.end.y - n0->second.end.y) <= 0.0;
                        if (qx && qy) {
                            rb.pointSound = true;
                            break;
                        }
                    }
                }
            }
            rb.pitch = B.pitch;
            rb.curveRadius = B.curveRadius;
            rb.cant = B.cant;
            blockOrigins[std::size_t(i)] = position;
            if (B.background >= 0) lastBackground = B.background;
            rb.background = lastBackground;
            if (B.fogDefined) fog = B.fog;
            rb.fogStart = fog.start;
            rb.fogEnd = fog.end;
            rb.fogColor = fog.color;

            // curve: chord length c, height h, half angle a
            double a = 0.0, c = L, h = 0.0;
            const double r = B.curveRadius, p = B.pitch;
            if (r != 0.0 && p != 0.0) {
                const double s = L / std::sqrt(1.0 + p * p);
                h = s * p;
                const double bb = s / std::abs(r);
                c = std::sqrt(2.0 * r * r * (1.0 - std::cos(bb)));
                a = 0.5 * (r > 0 ? 1.0 : -1.0) * bb;
                dir = rotate2(dir, std::cos(-a), std::sin(-a));
            } else if (r != 0.0) {
                const double bb = L / std::abs(r);
                c = std::sqrt(2.0 * r * r * (1.0 - std::cos(bb)));
                a = 0.5 * (r > 0 ? 1.0 : -1.0) * bb;
                dir = rotate2(dir, std::cos(-a), std::sin(-a));
            } else if (p != 0.0) {
                c = L / std::sqrt(1.0 + p * p);
                h = c * p;
            }
            const double trackYaw = std::atan2(dir.x, dir.y);
            const double trackPitch = std::atan(p);
            const Xf groundT = makeXf(trackYaw, 0.0, 0.0);
            const Xf trackT = makeXf(trackYaw, trackPitch, 0.0);
            const glm::dmat3 groundM = toMat(groundT);

            // ground
            {
                const int ci = ((i % int(B.cycle.size())) + int(B.cycle.size())) % int(B.cycle.size());
                place(i, objectFrom(ground, B.cycle[std::size_t(ci)]), position + glm::dvec3(0.0, -B.height, 0.0), groundM, startDist);
            }
            for (const PFreeObj& f : B.groundFreeObj) {
                const double d = f.trackPos - startDist;
                const glm::dvec3 w = position + glm::dvec3(dir.x * d + dir.y * f.x, f.y - B.height, dir.y * d - dir.x * f.x);
                place(i, objectFrom(freeObj, f.type), w, groundM * toMat(makeXf(f.yaw, f.pitch, f.roll)), f.trackPos);
            }

            // rails
            for (auto& [railKey, rail] : B.rails) {
                glm::dvec3 pos;
                Xf railT;
                if (railKey == 0) {
                    railT = trackT;
                    pos = position;
                } else {
                    if (!rail.started) continue;
                    const double x = rail.start.x, y = rail.start.y;
                    pos = position + glm::dvec3(dir.y * x, y, -dir.x * x);
                    auto nit = N.rails.find(railKey);
                    if (nit != N.rails.end()) {
                        glm::dvec2 dir2 = dir;
                        glm::dvec3 pos2 = position + glm::dvec3(dir.x * c, h, dir.y * c);
                        if (a != 0.0) dir2 = rotate2(dir2, std::cos(-a), std::sin(-a));
                        if (N.turn != 0.0) {
                            const double ag = -std::atan(N.turn);
                            dir2 = rotate2(dir2, std::cos(ag), std::sin(ag));
                        }
                        if (N.curveRadius != 0.0) {
                            const double s2 = L / std::sqrt(1.0 + N.pitch * N.pitch);
                            const double b2 = (N.pitch != 0.0 ? s2 : L) / std::abs(N.curveRadius);
                            const double a2 = 0.5 * (N.curveRadius > 0 ? 1.0 : -1.0) * b2;
                            dir2 = rotate2(dir2, std::cos(-a2), std::sin(-a2));
                        }
                        const double x2 = nit->second.end.x, y2 = nit->second.end.y;
                        const glm::dvec3 p2 = pos2 + glm::dvec3(dir2.y * x2, y2, -dir2.x * x2);
                        glm::dvec3 rr = p2 - pos;
                        const double len = glm::length(rr);
                        rr = len > 0 ? rr / len : glm::dvec3(dir.x, 0, dir.y);
                        railT.Z = rr;
                        railT.X = glm::dvec3(rr.z, 0.0, -rr.x);
                        const double xl = std::sqrt(railT.X.x * railT.X.x + railT.X.z * railT.X.z);
                        if (xl > 0) railT.X /= xl;
                        railT.Y = glm::cross(railT.Z, railT.X);
                    } else {
                        railT = trackT;
                    }
                }
                const glm::dmat3 railM = toMat(railT);
                // rail object
                if (railKey >= 0 && railKey < int(B.railType.size())) place(i, objectFrom(railObj, B.railType[std::size_t(railKey)]), pos, railM, startDist);
                // poles
                if (railKey >= 0 && railKey < int(B.poles.size())) {
                    const PPole& pl = B.poles[std::size_t(railKey)];
                    if (pl.exists && pl.interval > 0) {
                        double dz = startDist / pl.interval;
                        dz -= std::floor(dz + 0.5);
                        if (dz >= -0.01 && dz <= 0.01) {
                            auto pm = poles.find(pl.mode);
                            if (pm != poles.end() && pm->second.count(pl.type)) {
                                if (pl.mode == 0) {
                                    place(i, lib.intern(pm->second[pl.type], ObjectVariant{pl.location > 0.0}), pos, railM, startDist);
                                } else {
                                    const double dx = -pl.location * 3.8;
                                    const glm::dvec3 w = pos + glm::dvec3(dir.y * dx, 0.0, -dir.x * dx);
                                    place(i, lib.intern(pm->second[pl.type]), w, railM, startDist);
                                }
                            }
                        }
                    }
                }
                // walls / dikes
                auto wallDike = [&](const std::map<int, PWallDike>& m, const Dict& L_, const Dict& R_) {
                    auto it = m.find(railKey);
                    if (it == m.end() || !it->second.exists) return;
                    if (it->second.dir <= 0) place(i, objectFrom(L_, it->second.type), pos, railM, startDist);
                    if (it->second.dir >= 0) place(i, objectFrom(R_, it->second.type), pos, railM, startDist);
                };
                wallDike(B.walls, wallL, wallR);
                wallDike(B.dikes, dikeL, dikeR);
                // forms
                for (const PForm& f : B.forms) {
                    if (f.primary == railKey) formPrimary(i, f, B, N, pos, railM, startDist);
                    if (f.secondary == railKey && f.secondary > 0) {
                        double px = 0.0;
                        if (auto pit = B.rails.find(f.primary); pit != B.rails.end() && f.primary > 0) px = pit->second.start.x;
                        const double d = px - rail.start.x;
                        if (d < 0.0) {
                            place(i, objectFrom(formL, f.form), pos, railM, startDist);
                            if (f.roof > 0) place(i, objectFrom(roofL, f.roof), pos, railM, startDist);
                        } else {
                            place(i, objectFrom(formR, f.form), pos, railM, startDist);
                            if (f.roof > 0) place(i, objectFrom(roofR, f.roof), pos, railM, startDist);
                        }
                    }
                }
                // cracks
                for (const PCrack& k : B.cracks) {
                    if (k.primary != railKey) continue;
                    const double px0 = k.primary > 0 ? rail.start.x : 0.0;
                    const double px1 = k.primary > 0 && N.rails.count(k.primary) ? N.rails[k.primary].end.x : 0.0;
                    auto sit = B.rails.find(k.secondary);
                    if (sit == B.rails.end() || !sit->second.started) continue;
                    const double d0 = sit->second.start.x - px0;
                    const double d1 = (N.rails.count(k.secondary) ? N.rails[k.secondary].end.x : sit->second.start.x) - px1;
                    const ObjectVariant v{false, true, float(d0), float(d1)};
                    if (d0 < 0.0) place(i, objectFrom(crackL, k.type, v), pos, railM, startDist);
                    else if (d0 > 0.0) place(i, objectFrom(crackR, k.type, v), pos, railM, startDist);
                }
                // free objects
                if (auto fit = B.railFreeObj.find(railKey); fit != B.railFreeObj.end()) {
                    for (const PFreeObj& f : fit->second) {
                        const double dz = f.trackPos - startDist;
                        const glm::dvec3 w = pos + f.x * railT.X + f.y * railT.Y + dz * railT.Z;
                        place(i, objectFrom(freeObj, f.type), w, railM * toMat(makeXf(f.yaw, f.pitch, f.roll)), f.trackPos);
                    }
                }
                // signals
                if (railKey == 0) {
                    for (const PSignal& s : B.signals) {
                        const double dz = s.trackPos - startDist;
                        const glm::dvec3 w = pos + s.x * railT.X + s.y * railT.Y + dz * railT.Z;
                        auto it = signalObj.find(s.type);
                        if (it == signalObj.end()) continue;
                        place(i, lib.intern(it->second), w, toMat(compose(railT, s.yaw, s.pitch, s.roll)), s.trackPos, s.section);
                    }
                }
            }
            // advance
            position += glm::dvec3(dir.x * c, h, dir.y * c);
            if (a != 0.0) dir = rotate2(dir, std::cos(-a), std::sin(-a));
        }

        // flatten placements, compute block bounds lazily (object radii unknown until loaded)
        std::size_t total = 0;
        for (const auto& v : blockPlacements) total += v.size();
        route.placements.reserve(total);
        for (int i = 0; i < nBlocks; ++i) {
            RouteBlock& rb = route.blocks[std::size_t(i)];
            rb.firstPlacement = u32(route.placements.size());
            rb.placementCount = u32(blockPlacements[std::size_t(i)].size());
            glm::vec3 mn(0.0f), mx(0.0f);
            for (const Placement& p : blockPlacements[std::size_t(i)]) {
                mn = glm::min(mn, p.pos);
                mx = glm::max(mx, p.pos);
            }
            rb.bmin = mn;
            rb.bmax = mx;
            route.placements.insert(route.placements.end(), blockPlacements[std::size_t(i)].begin(), blockPlacements[std::size_t(i)].end());
            std::vector<Placement>().swap(blockPlacements[std::size_t(i)]);
        }
        stats.placements = route.placements.size();
        route.firstUsedPosition = firstUsedBlock * L;
        std::sort(route.limits.begin(), route.limits.end(), [](const auto& x, const auto& y) { return x.trackPos < y.trackPos; });
        std::stable_sort(route.brightness.begin(), route.brightness.end(), [](const auto& x, const auto& y) { return x.trackPos < y.trackPos; });
        std::stable_sort(route.beacons.begin(), route.beacons.end(), [](const auto& x, const auto& y) { return x.trackPos < y.trackPos; });
    }

    void formPrimary(int i, const PForm& f, PBlock& B, PBlock& N, const glm::dvec3& pos, const glm::dmat3& railM, double startDist) {
        auto both = [&](const Dict& form, const Dict& formC, const Dict& roof, const Dict& roofC, ObjectVariant v) {
            place(i, objectFrom(form, f.form), pos, railM, startDist);
            place(i, objectFrom(formC, f.form, v), pos, railM, startDist);
            if (f.roof > 0) {
                place(i, objectFrom(roof, f.roof), pos, railM, startDist);
                place(i, objectFrom(roofC, f.roof, v), pos, railM, startDist);
            }
        };
        if (f.secondary == kFormStub) {
            place(i, objectFrom(formL, f.form), pos, railM, startDist);
            if (f.roof > 0) place(i, objectFrom(roofL, f.roof), pos, railM, startDist);
        } else if (f.secondary == kFormL) {
            both(formL, formCL, roofL, roofCL, {});
        } else if (f.secondary == kFormR) {
            both(formR, formCR, roofR, roofCR, {});
        } else if (f.secondary > 0) {
            const double px0 = f.primary > 0 && B.rails.count(f.primary) ? B.rails[f.primary].start.x : 0.0;
            const double px1 = f.primary > 0 && N.rails.count(f.primary) ? N.rails[f.primary].end.x : 0.0;
            auto sit = B.rails.find(f.secondary);
            if (sit == B.rails.end() || !sit->second.started) return;
            const double d0 = sit->second.start.x - px0;
            const double d1 = (N.rails.count(f.secondary) ? N.rails[f.secondary].end.x : sit->second.start.x) - px1;
            const ObjectVariant v{false, true, float(d0), float(d1)};
            if (d0 < 0.0) both(formL, formCL, roofL, roofCL, v);
            else if (d0 > 0.0) both(formR, formCR, roofR, roofCR, v);
        }
    }
};

} // namespace

// ------------------------------------------------------------------ Route queries

const StopPoint* Station::stopFor(int cars) const {
    if (stops.empty()) return nullptr;
    // openBVE: the stop whose car count is the smallest >= cars, else the "all cars" (0) one.
    const StopPoint* best = nullptr;
    for (const StopPoint& s : stops)
        if (s.cars >= cars && (!best || s.cars < best->cars)) best = &s;
    if (!best)
        for (const StopPoint& s : stops)
            if (s.cars == 0) best = &s;
    return best ? best : &stops.back();
}

int Route::blockAt(double trackPos) const {
    if (blocks.empty()) return 0;
    const int i = int(std::floor(trackPos / blockLength));
    return std::clamp(i, 0, int(blocks.size()) - 1);
}

TrackPose Route::poseAt(double trackPos) const {
    TrackPose out;
    if (blocks.empty()) return out;
    const int i = blockAt(trackPos);
    const RouteBlock& b = blocks[std::size_t(i)];
    const double delta = trackPos - i * blockLength;
    const double p = b.pitch;
    glm::dvec2 dirEnd = b.startDir;
    const double s = delta / std::sqrt(1.0 + p * p);
    const double h = s * p;
    if (b.curveRadius != 0.0) {
        const double r = b.curveRadius;
        const double bb = s / std::abs(r);
        const double f = 2.0 * r * r * (1.0 - std::cos(bb));
        const double c = (delta >= 0 ? 1.0 : -1.0) * std::sqrt(std::max(f, 0.0));
        const double a = 0.5 * (r > 0 ? 1.0 : -1.0) * bb;
        const glm::dvec2 d = rotate2(b.startDir, std::cos(-a), std::sin(-a));
        out.position = b.origin + glm::dvec3(d.x * c, h, d.y * c);
        dirEnd = rotate2(b.startDir, std::cos(-2.0 * a), std::sin(-2.0 * a));
    } else {
        out.position = b.origin + glm::dvec3(b.startDir.x * s, h, b.startDir.y * s);
    }
    out.direction = glm::normalize(glm::dvec3(dirEnd.x, p, dirEnd.y));
    out.side = glm::normalize(glm::dvec3(dirEnd.y, 0.0, -dirEnd.x));
    out.up = glm::cross(out.direction, out.side);
    out.curveRadius = b.curveRadius;
    out.pitch = p;
    out.cant = b.cant;
    return out;
}

double Route::speedLimitAt(double trackPos) const {
    double v = std::numeric_limits<double>::infinity();
    for (const SpeedLimit& l : limits) {
        if (l.trackPos > trackPos) break;
        v = l.speed;
    }
    return v;
}

f32 Route::brightnessAt(double t) const {
    // openBVE RouteData.GetBrightness: linear between the surrounding brightness points.
    if (brightness.empty()) return 1.0f;
    auto it = std::upper_bound(brightness.begin(), brightness.end(), t, [](double x, const BrightnessPoint& b) { return x < b.trackPos; });
    if (it == brightness.begin()) {
        const double tMax = it->trackPos;
        return tMax > 0 ? float((it->value - 1.0) * t / tMax + 1.0) : it->value;
    }
    const BrightnessPoint& lo = *(it - 1);
    if (it == brightness.end()) return lo.value;
    const double n = (t - lo.trackPos) / std::max(1e-9, it->trackPos - lo.trackPos);
    return float((1.0 - n) * lo.value + n * it->value);
}

Fog Route::fogAt(double t) const {
    if (blocks.empty()) return initialFog;
    const RouteBlock& b = blocks[std::size_t(blockAt(t))];
    return Fog{b.fogStart, b.fogEnd, b.fogColor};
}

int Route::backgroundAt(double t) const {
    if (blocks.empty()) return -1;
    return blocks[std::size_t(blockAt(t))].background;
}

int Route::sectionAt(double t) const {
    int s = -1;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].trackPos > t) break;
        s = int(i);
    }
    return s;
}

bool loadRoute(const std::filesystem::path& file, ObjectLibrary& lib, Route& out, RouteLoadStats& stats, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    out = Route{};
    out.file = file;
    std::filesystem::path railway = findUpwards(file.parent_path(), "Railway");
    if (railway.empty()) railway = file.parent_path().parent_path();
    out.objectFolder = resolvePath(railway, "Object");
    out.soundFolder = resolvePath(railway, "Sound");
    if (out.objectFolder.empty()) out.objectFolder = railway / "Object";
    if (out.soundFolder.empty()) out.soundFolder = railway / "Sound";
    out.trainFolderHint = findUpwards(file.parent_path(), "Train");
    // openBVE's initial section: red until the first Track.Section.
    out.sections.push_back(Section{0.0, {0, 4}, SectionType::IndexBased, -1, 4});

    Parser p(lib, out, stats);
    std::string text;
    if (!p.readFile(file, text)) {
        error = "cannot read route file " + file.string();
        return false;
    }
    p.files.push_back(file);
    p.splitIntoExpressions(0, text, 0.0, p.exprs);
    p.preprocess();
    p.preprocessOptions();
    p.sortByPosition();
    // Block 0 (openBVE RouteData constructor).
    {
        PBlock b0;
        b0.background = 0;
        b0.fog = out.initialFog;
        b0.cycle = {-1};
        b0.height = 0.0;
        b0.rails[0].started = true;
        b0.railType = {0};
        b0.railCycles = {PRailCycle{}};
        p.blocks.push_back(std::move(b0));
        p.blockStation.push_back(-1);
    }
    p.nonTrackPass();
    p.trackPass();
    p.apply();
    stats.parseSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace ots
