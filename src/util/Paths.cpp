#include "util/Paths.hpp"

#include "util/Text.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ots {
namespace {

struct DirListing {
    std::unordered_map<std::string, std::string> byLower; // lower-case name → real name
};

std::shared_mutex gMutex;
std::unordered_map<std::string, DirListing> gCache;

const DirListing& listing(const fs::path& dir) {
    const std::string key = dir.string();
    {
        std::shared_lock lock(gMutex);
        auto it = gCache.find(key);
        if (it != gCache.end()) return it->second;
    }
    DirListing l;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        l.byLower.emplace(lower(name), name);
    }
    std::unique_lock lock(gMutex);
    return gCache.emplace(key, std::move(l)).first->second;
}

} // namespace

fs::path resolvePath(const fs::path& base, std::string_view relative) {
    std::string rel(trim(relative));
    if (rel.size() >= 2 && rel.front() == '"' && rel.back() == '"') rel = rel.substr(1, rel.size() - 2);
    for (char& c : rel)
        if (c == '\\') c = '/';
    fs::path cur = base;
    if (!rel.empty() && rel.front() == '/') cur = "/";
    std::error_code ec;
    for (std::string_view part : split(rel, '/')) {
        part = trim(part);
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        fs::path exact = cur / std::string(part);
        if (fs::exists(exact, ec)) {
            cur = exact;
            continue;
        }
        const DirListing& l = listing(cur);
        auto it = l.byLower.find(lower(part));
        if (it == l.byLower.end()) return {};
        cur /= it->second;
    }
    return cur;
}

fs::path combinePath(const fs::path& base, std::string_view relative) {
    fs::path p = resolvePath(base, relative);
    if (!p.empty()) return p;
    std::string rel(relative);
    for (char& c : rel)
        if (c == '\\') c = '/';
    return base / rel;
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

fs::path findUpwards(const fs::path& start, std::string_view name) {
    fs::path dir = start;
    for (int i = 0; i < 16 && !dir.empty(); ++i) {
        fs::path sub = resolvePath(dir, name);
        std::error_code ec;
        if (!sub.empty() && fs::is_directory(sub, ec)) return sub;
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string pathString(const fs::path& p) { return p.string(); }

} // namespace ots
