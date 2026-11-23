// DirectX .x (text) object loader: Frame hierarchy, Mesh, MeshMaterialList, MeshNormals,
// MeshTextureCoords. Like openBVE (BlackTransparencyMode.AllFiles), textures of X materials use
// pure black as their transparent colour.
#include "bve/MeshBuilder.hpp"

#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>

namespace ots {
namespace {

struct Token {
    enum Kind { Ident, Number, String, Open, Close, Sep, End } kind = End;
    std::string text;
    double number = 0.0;
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}
    Token next() {
        for (;;) {
            while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
            if (p_ >= s_.size()) return {};
            if (s_[p_] == '#' || (s_[p_] == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/')) {
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
                continue;
            }
            break;
        }
        const char c = s_[p_];
        if (c == '{') return ++p_, Token{Token::Open, "{"};
        if (c == '}') return ++p_, Token{Token::Close, "}"};
        if (c == ';' || c == ',') return ++p_, Token{Token::Sep, std::string(1, c)};
        if (c == '"') {
            const std::size_t e = s_.find('"', p_ + 1);
            Token t{Token::String, std::string(s_.substr(p_ + 1, (e == std::string_view::npos ? s_.size() : e) - p_ - 1))};
            p_ = e == std::string_view::npos ? s_.size() : e + 1;
            return t;
        }
        if (c == '<') { // GUID
            const std::size_t e = s_.find('>', p_);
            p_ = e == std::string_view::npos ? s_.size() : e + 1;
            return next();
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            std::size_t e = p_ + 1;
            while (e < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[e])) || s_[e] == '.' || s_[e] == 'e' || s_[e] == 'E' ||
                                     ((s_[e] == '-' || s_[e] == '+') && (s_[e - 1] == 'e' || s_[e - 1] == 'E'))))
                ++e;
            Token t{Token::Number, std::string(s_.substr(p_, e - p_))};
            parseDouble(t.text, t.number);
            p_ = e;
            return t;
        }
        std::size_t e = p_;
        while (e < s_.size() && !std::isspace(static_cast<unsigned char>(s_[e])) && s_[e] != '{' && s_[e] != '}' && s_[e] != ';' &&
               s_[e] != ',')
            ++e;
        if (e == p_) ++e;
        Token t{Token::Ident, std::string(s_.substr(p_, e - p_))};
        p_ = e;
        return t;
    }

private:
    std::string_view s_;
    std::size_t p_ = 0;
};

struct Node {
    std::string type, name;
    std::vector<double> numbers;
    std::vector<std::string> strings;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<std::string> refs;
};

class Parser {
public:
    explicit Parser(std::string_view s) : lex_(s) { tok_ = lex_.next(); }
    void parseTop(std::vector<std::unique_ptr<Node>>& out) {
        while (tok_.kind != Token::End) {
            if (tok_.kind == Token::Ident) {
                if (auto n = parseObject()) out.push_back(std::move(n));
            } else {
                advance();
            }
        }
    }

private:
    Lexer lex_;
    Token tok_;
    void advance() { tok_ = lex_.next(); }

    // tok_ is the type identifier.
    std::unique_ptr<Node> parseObject() {
        auto n = std::make_unique<Node>();
        n->type = lower(tok_.text);
        advance();
        if (tok_.kind == Token::Ident) {
            n->name = tok_.text;
            advance();
        }
        if (tok_.kind != Token::Open) return nullptr;
        advance();
        const bool isTemplate = n->type == "template";
        int depth = 0;
        while (tok_.kind != Token::End) {
            if (tok_.kind == Token::Close) {
                advance();
                if (depth == 0) break;
                --depth;
                continue;
            }
            if (isTemplate) {
                if (tok_.kind == Token::Open) ++depth;
                advance();
                continue;
            }
            switch (tok_.kind) {
            case Token::Number: n->numbers.push_back(tok_.number); advance(); break;
            case Token::String: n->strings.push_back(tok_.text); advance(); break;
            case Token::Sep: advance(); break;
            case Token::Open: { // reference { Name }
                advance();
                if (tok_.kind == Token::Ident) n->refs.push_back(tok_.text), advance();
                while (tok_.kind != Token::Close && tok_.kind != Token::End) advance();
                advance();
                break;
            }
            case Token::Ident: {
                if (auto c = parseObject()) n->children.push_back(std::move(c));
                break;
            }
            default: advance(); break;
            }
        }
        return isTemplate ? nullptr : std::move(n);
    }
};

struct Loader {
    std::filesystem::path dir;
    std::map<std::string, const Node*> namedMaterials;
    MeshBuilder builder;
    StaticMesh out;
    std::string error;

    Material material(const Node& m) {
        Material mat;
        auto c = [&](std::size_t i, double def) {
            const double v = i < m.numbers.size() ? m.numbers[i] : def;
            return u32(std::clamp(int(std::lround(v * 255.0)), 0, 255));
        };
        mat.color = c(0, 1) | (c(1, 1) << 8) | (c(2, 1) << 16) | (c(3, 1) << 24);
        mat.emissive = c(8, 0) | (c(9, 0) << 8) | (c(10, 0) << 16);
        for (const auto& ch : m.children)
            if (ch->type == "texturefilename" && !ch->strings.empty()) {
                std::filesystem::path t = resolvePath(dir, ch->strings[0]);
                if (t.empty()) error = "texture not found: " + ch->strings[0];
                mat.texture = t.string();
                if (!t.empty()) {
                    mat.colorKey = true;
                    mat.keyR = mat.keyG = mat.keyB = 0;
                }
            }
        return mat;
    }

    void mesh(const Node& m, const glm::mat4& xf) {
        const auto& num = m.numbers;
        std::size_t p = 0;
        auto take = [&]() { return p < num.size() ? num[p++] : 0.0; };
        const u32 nv = u32(take());
        MeshBuilder& b = builder;
        b = MeshBuilder{};
        b.materials.clear();
        for (u32 i = 0; i < nv; ++i) {
            const double x = take(), y = take(), z = take();
            const glm::vec4 w = xf * glm::vec4(float(x), float(y), float(z), 1.0f);
            b.vertices.push_back({glm::vec3(w), glm::vec3(0.0f), glm::vec2(0.0f)});
        }
        const u32 nf = u32(take());
        for (u32 f = 0; f < nf; ++f) {
            BuilderFace face;
            const u32 n = u32(take());
            for (u32 k = 0; k < n; ++k) face.idx.push_back(u32(take()));
            b.faces.push_back(std::move(face));
        }
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(xf)));
        for (const auto& ch : m.children) {
            if (ch->type == "meshtexturecoords") {
                std::size_t q = 0;
                const u32 n = u32(q < ch->numbers.size() ? ch->numbers[q++] : 0);
                for (u32 i = 0; i < n && i < b.vertices.size() && q + 1 < ch->numbers.size(); ++i, q += 2)
                    b.vertices[i].uv = {float(ch->numbers[q]), float(ch->numbers[q + 1])};
            } else if (ch->type == "meshnormals") {
                std::size_t q = 0;
                auto tk = [&]() { return q < ch->numbers.size() ? ch->numbers[q++] : 0.0; };
                const u32 n = u32(tk());
                std::vector<glm::vec3> normals(n);
                for (u32 i = 0; i < n; ++i) {
                    const float x = float(tk()), y = float(tk()), z = float(tk());
                    glm::vec3 v = nm * glm::vec3(x, y, z);
                    normals[i] = glm::length(v) > 0 ? glm::normalize(v) : v;
                }
                const u32 fcount = u32(tk());
                for (u32 f = 0; f < fcount && f < b.faces.size(); ++f) {
                    const u32 k = u32(tk());
                    for (u32 j = 0; j < k; ++j) {
                        const u32 ni = u32(tk());
                        if (j < b.faces[f].idx.size()) {
                            b.faces[f].normals.resize(b.faces[f].idx.size());
                            b.faces[f].normals[j] = ni < normals.size() ? normals[ni] : glm::vec3(0.0f);
                        }
                    }
                }
            } else if (ch->type == "meshmateriallist") {
                std::size_t q = 0;
                auto tk = [&]() { return q < ch->numbers.size() ? ch->numbers[q++] : 0.0; };
                tk(); // material count (declared)
                const u32 nidx = u32(tk());
                std::vector<u32> faceMat(nidx);
                for (u32 i = 0; i < nidx; ++i) faceMat[i] = u32(tk());
                for (const auto& mc : ch->children)
                    if (mc->type == "material") b.materials.push_back(material(*mc));
                for (const std::string& r : ch->refs) {
                    auto it = namedMaterials.find(r);
                    if (it != namedMaterials.end()) b.materials.push_back(material(*it->second));
                }
                for (u32 f = 0; f < b.faces.size(); ++f) {
                    const u32 mi = faceMat.empty() ? 0 : faceMat[std::min<std::size_t>(f, faceMat.size() - 1)];
                    b.faces[f].material = mi;
                }
            }
        }
        if (b.materials.empty()) b.materials.push_back(Material{});
        if (glm::determinant(glm::mat3(xf)) < 0.0f) b.reverseFaces();
        b.flush(out);
    }

    void walk(const Node& n, const glm::mat4& parent) {
        glm::mat4 xf = parent;
        if (n.type == "frame") {
            for (const auto& c : n.children)
                if (c->type == "frametransformmatrix" && c->numbers.size() >= 16) {
                    glm::mat4 local;
                    // Row-vector matrix (v · M): its rows are glm's columns.
                    for (int r = 0; r < 4; ++r)
                        for (int col = 0; col < 4; ++col) local[r][col] = float(c->numbers[std::size_t(r * 4 + col)]);
                    xf = parent * local;
                }
        }
        if (n.type == "mesh") mesh(n, xf);
        for (const auto& c : n.children)
            if (c->type == "frame" || c->type == "mesh") walk(*c, xf);
    }
};

} // namespace

bool loadX(const std::filesystem::path& path, StaticMesh& out, std::string& error) {
    std::string text;
    if (!readTextFile(path, text)) {
        error = "cannot read";
        return false;
    }
    if (text.size() < 16 || text.compare(0, 3, "xof") != 0) {
        error = "not an X file";
        return false;
    }
    if (text.compare(8, 3, "txt") != 0) {
        error = "binary/compressed X files are not supported";
        return false;
    }
    std::vector<std::unique_ptr<Node>> top;
    Parser(std::string_view(text).substr(16)).parseTop(top);
    Loader l;
    l.dir = path.parent_path();
    for (const auto& n : top)
        if (n->type == "material" && !n->name.empty()) l.namedMaterials[n->name] = n.get();
    for (const auto& n : top) l.walk(*n, glm::mat4(1.0f));
    mergeParts(l.out);
    out = std::move(l.out);
    if (!l.error.empty()) error = l.error;
    return true;
}

} // namespace ots
