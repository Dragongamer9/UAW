// Ground navigation over the map's vertex grid.
//
// Each vertex carries a uint32 flag word (TED chunk 0x010A). Comparing the
// flags with terrain and objects on several retail maps:
//   0x01  open ground
//   0x02  blocked: building footprints, river channels, canyon walls
//   0x08  deep water (ocean)
//   0x10  shallow/coastal water
//   0x20  blocked: pyramid faces, map border walls, lakes
//   0x40  walkable (rare; raised walkways)
//   0x80  walkable crossings: fords and bridges over rivers
// Ground units may stand on a vertex with none of 0x02/0x08/0x10/0x20 set.
#pragma once

#include "alamo/ted.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace alamo {

struct Map;

class NavGrid {
public:
    NavGrid() = default;
    // Uses the map's vertex flags; maps without them fall back to "slope under 40 degrees".
    explicit NavGrid(const Map& map);

    static constexpr uint32_t kBlockingFlags = 0x02 | 0x08 | 0x10 | 0x20;

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    float spacing() const { return spacing_; }

    bool passable(int x, int y) const {
        return x >= 0 && y >= 0 && x < int(w_) && y < int(h_) && pass_[size_t(y) * w_ + x];
    }
    // Call rebuild() after a batch of edits so clearance stays correct.
    void setPassable(int x, int y, bool p) {
        if (x >= 0 && y >= 0 && x < int(w_) && y < int(h_)) pass_[size_t(y) * w_ + x] = p;
    }
    void rebuild() { computeClearance(); computeRegions(); }
    // Connected walkable area of a cell (0 = blocked); cells of different regions can't reach each other.
    uint32_t region(int x, int y) const {
        return (x >= 0 && y >= 0 && x < int(w_) && y < int(h_) && !region_.empty()) ? region_[size_t(y) * w_ + x] : 0;
    }
    bool passableAt(float wx, float wy) const { return passable(cellX(wx), cellY(wy)); }
    int cellX(float wx) const { return int(std::lround(wx / spacing_)); }
    int cellY(float wy) const { return int(std::lround(wy / spacing_)); }

    // Straight walkable line between two world points (sampled every half cell).
    // With `margin`, every sample must also be at least that many cells from a blocked cell.
    bool lineClear(Vec3 a, Vec3 b, int margin = 0) const;

    // Cells to the nearest blocked cell (0 = blocked, 1 = touching a wall, capped at 4).
    int clearance(int x, int y) const {
        return (x >= 0 && y >= 0 && x < int(w_) && y < int(h_)) ? clear_[size_t(y) * w_ + x] : 0;
    }

    // Nearest passable cell centre to a world point (searches outward), or the point itself.
    Vec3 nearestPassable(Vec3 p, int maxRadius = 30) const;

    // A* over 8-connected cells, then string-pulled. Returns world points from
    // just after `from` to `to` (z = 0; callers sample terrain height). Empty if unreachable.
    // If `to` is blocked, the path ends at the closest reachable cell.
    std::vector<Vec3> findPath(Vec3 from, Vec3 to, size_t maxExpanded = 400000) const;

    size_t passableCount() const;

private:
    uint32_t w_ = 0, h_ = 0;
    float spacing_ = 20;
    std::vector<uint8_t> pass_;
    std::vector<uint8_t> clear_;
    std::vector<uint32_t> region_;
    void computeClearance();
    void computeRegions();
    // A* scratch, reused between searches (stamped instead of cleared).
    mutable std::vector<float> g_;
    mutable std::vector<int32_t> parent_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t generation_ = 0;
};

} // namespace alamo
