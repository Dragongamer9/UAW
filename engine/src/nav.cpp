#include "alamo/nav.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace alamo {

NavGrid::NavGrid(const Map& map) : w_(map.width), h_(map.height), spacing_(map.spacing) {
    pass_.assign(size_t(w_) * h_, 0);
    if (map.vertexFlags.size() == pass_.size()) {
        for (size_t i = 0; i < pass_.size(); ++i) pass_[i] = (map.vertexFlags[i] & kBlockingFlags) == 0;
    } else {
        const float maxRise = std::tan(40.f * 3.14159265f / 180.f) * spacing_;
        for (uint32_t y = 0; y < h_; ++y)
            for (uint32_t x = 0; x < w_; ++x) {
                const float hc = map.vertexHeight(x, y);
                float rise = 0;
                if (x + 1 < w_) rise = std::max(rise, std::fabs(map.vertexHeight(x + 1, y) - hc));
                if (y + 1 < h_) rise = std::max(rise, std::fabs(map.vertexHeight(x, y + 1) - hc));
                pass_[size_t(y) * w_ + x] = rise <= maxRise;
            }
    }
    computeClearance();
}

void NavGrid::computeClearance() {
    // Chebyshev distance to the nearest blocked cell (the map edge counts as blocked), capped at 4.
    clear_.assign(pass_.size(), 4);
    std::vector<uint32_t> frontier;
    for (uint32_t y = 0; y < h_; ++y)
        for (uint32_t x = 0; x < w_; ++x) {
            const size_t i = size_t(y) * w_ + x;
            if (!pass_[i]) { clear_[i] = 0; frontier.push_back(uint32_t(i)); }
            else if (x == 0 || y == 0 || x + 1 == w_ || y + 1 == h_) clear_[i] = std::min<uint8_t>(clear_[i], 1);
        }
    for (uint8_t level = 1; level < 4 && !frontier.empty(); ++level) {
        std::vector<uint32_t> next;
        for (uint32_t i : frontier) {
            const int x = int(i % w_), y = int(i / w_);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= int(w_) || ny >= int(h_)) continue;
                    const size_t ni = size_t(ny) * w_ + size_t(nx);
                    if (clear_[ni] > level) { clear_[ni] = level; next.push_back(uint32_t(ni)); }
                }
        }
        frontier.swap(next);
    }
}

size_t NavGrid::passableCount() const { return size_t(std::count(pass_.begin(), pass_.end(), uint8_t(1))); }

bool NavGrid::lineClear(Vec3 a, Vec3 b, int margin) const {
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const float len = std::hypot(dx, dy);
    const int steps = std::max(1, int(len / (spacing_ * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const int cx = cellX(a[0] + dx * t), cy = cellY(a[1] + dy * t);
        if (!passable(cx, cy)) return false;
        // The endpoints may sit near walls (that's where the unit is / wants to be).
        if (margin > 0 && i > 0 && i < steps && clearance(cx, cy) <= margin) return false;
    }
    return true;
}

Vec3 NavGrid::nearestPassable(Vec3 p, int maxRadius) const {
    const int cx = cellX(p[0]), cy = cellY(p[1]);
    if (passable(cx, cy)) return p;
    for (int r = 1; r <= maxRadius; ++r) {
        float best = 1e30f;
        int bx = 0, by = 0;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (std::max(std::abs(x - cx), std::abs(y - cy)) != r || !passable(x, y)) continue;
                const float d = std::hypot(x * spacing_ - p[0], y * spacing_ - p[1]);
                if (d < best) { best = d; bx = x; by = y; }
            }
        if (best < 1e30f) return {bx * spacing_, by * spacing_, 0};
    }
    return p;
}

std::vector<Vec3> NavGrid::findPath(Vec3 from, Vec3 to, size_t maxExpanded) const {
    if (pass_.empty()) return {};
    const Vec3 start = nearestPassable(from);
    int sx = std::clamp(cellX(start[0]), 0, int(w_) - 1), sy = std::clamp(cellY(start[1]), 0, int(h_) - 1);
    int gx = std::clamp(cellX(to[0]), 0, int(w_) - 1), gy = std::clamp(cellY(to[1]), 0, int(h_) - 1);
    if (!passable(sx, sy)) return {};

    const size_t n = size_t(w_) * h_;
    std::vector<float> g(n, 1e30f);
    std::vector<int32_t> parent(n, -1);
    std::vector<uint8_t> closed(n, 0);
    auto idx = [&](int x, int y) { return size_t(y) * w_ + size_t(x); };
    auto heur = [&](int x, int y) {
        const float dx = float(std::abs(x - gx)), dy = float(std::abs(y - gy));
        return (dx + dy) + (1.41421356f - 2.f) * std::min(dx, dy); // octile
    };
    struct Node {
        float f;
        int32_t i;
        bool operator<(const Node& o) const { return f > o.f || (f == o.f && i > o.i); }
    };
    std::priority_queue<Node> open;
    const size_t s = idx(sx, sy);
    g[s] = 0;
    open.push({heur(sx, sy), int32_t(s)});

    size_t best = s;
    float bestH = heur(sx, sy);
    size_t expanded = 0;
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!open.empty() && expanded < maxExpanded) {
        const Node cur = open.top();
        open.pop();
        const size_t ci = size_t(cur.i);
        if (closed[ci]) continue;
        closed[ci] = 1;
        ++expanded;
        const int cx = int(ci % w_), cy = int(ci / w_);
        const float h = heur(cx, cy);
        if (h < bestH) { bestH = h; best = ci; }
        if (cx == gx && cy == gy) { best = ci; break; }
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + dxs[k], ny = cy + dys[k];
            if (!passable(nx, ny)) continue;
            // No corner cutting through blocked cells.
            if (k >= 4 && (!passable(cx + dxs[k], cy) || !passable(cx, cy + dys[k]))) continue;
            const size_t ni = idx(nx, ny);
            if (closed[ni]) continue;
            // Prefer a cell of space from walls so groups don't jam on corners.
            const int c = clearance(nx, ny);
            const float wallCost = c <= 1 ? 2.5f : c == 2 ? 0.6f : 0.f;
            const float ng = g[ci] + (k >= 4 ? 1.41421356f : 1.f) + wallCost;
            if (ng < g[ni]) {
                g[ni] = ng;
                parent[ni] = int32_t(ci);
                open.push({ng + heur(nx, ny), int32_t(ni)});
            }
        }
    }

    // Walk back from the goal (or the closest reachable cell).
    std::vector<Vec3> cells;
    for (int32_t i = int32_t(best); i >= 0; i = parent[size_t(i)])
        cells.push_back({float(size_t(i) % w_) * spacing_, float(size_t(i) / w_) * spacing_, 0});
    std::reverse(cells.begin(), cells.end());
    if (best == size_t(gx + gy * int(w_)) && passableAt(to[0], to[1])) cells.back() = {to[0], to[1], 0};
    if (cells.size() <= 1 && best == s) {
        if (best == idx(gx, gy)) return {{to[0], to[1], 0}};
        return {};
    }

    // String pulling: keep only the points needed to keep every segment walkable.
    std::vector<Vec3> out;
    Vec3 anchor = start;
    size_t i = 0;
    while (i < cells.size()) {
        size_t far = i;
        for (size_t j = cells.size(); j-- > i;) {
            if (lineClear(anchor, cells[j], 1)) { far = j; break; }
        }
        out.push_back(cells[far]);
        anchor = cells[far];
        i = far + 1;
    }
    return out;
}

} // namespace alamo
