#include "rules.h"

#include <cstring>

#include "sha256.h"

namespace dig {

namespace {

const FossilKind kKinds[kFossils] = {
    {"SKULL", 4, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
    {"FEMUR", 3, {{0, 0}, {1, 0}, {2, 0}, {0, 0}}},
    {"CLAW", 3, {{0, 0}, {0, 1}, {1, 1}, {0, 0}}},
    {"TOOTH", 2, {{0, 0}, {1, 0}, {0, 0}, {0, 0}}},
};

// Rotated, normalised offsets of a fossil (top-left of its bounds at 0,0).
int rotated_shape(int id, int rot, Cell out[4])
{
    const FossilKind &kind = kKinds[id];
    int min_x = 99;
    int min_y = 99;
    for (int i = 0; i < kind.size; ++i) {
        int x = kind.shape[i].x;
        int y = kind.shape[i].y;
        for (int r = 0; r < (rot & 3); ++r) {
            const int t = x;
            x = -y;  // a clockwise quarter turn with y pointing down
            y = t;
        }
        out[i] = {static_cast<int8_t>(x), static_cast<int8_t>(y)};
        min_x = x < min_x ? x : min_x;
        min_y = y < min_y ? y : min_y;
    }
    for (int i = 0; i < kind.size; ++i) {
        out[i].x = static_cast<int8_t>(out[i].x - min_x);
        out[i].y = static_cast<int8_t>(out[i].y - min_y);
    }
    return kind.size;
}

}  // namespace

const FossilKind &fossil_kind(int id)
{
    return kKinds[id];
}

int Layout::cells_for(int id, const Placement &p, Cell out[4])
{
    const int n = rotated_shape(id, p.rot, out);
    for (int i = 0; i < n; ++i) {
        out[i].x = static_cast<int8_t>(out[i].x + p.x);
        out[i].y = static_cast<int8_t>(out[i].y + p.y);
    }
    return n;
}

bool Layout::valid() const
{
    bool used[kCells] = {};
    for (int id = 0; id < kFossils; ++id) {
        if (fossils[id].rot > 3) {
            return false;
        }
        Cell cells[4];
        const int n = cells_of(id, cells);
        for (int i = 0; i < n; ++i) {
            if (!on_board(cells[i].x, cells[i].y)) {
                return false;
            }
            bool &slot = used[cells[i].y * kGrid + cells[i].x];
            if (slot) {
                return false;
            }
            slot = true;
        }
    }
    return true;
}

int Layout::fossil_at(int x, int y) const
{
    for (int id = 0; id < kFossils; ++id) {
        Cell cells[4];
        const int n = cells_of(id, cells);
        for (int i = 0; i < n; ++i) {
            if (cells[i].x == x && cells[i].y == y) {
                return id;
            }
        }
    }
    return kNone;
}

int Layout::bones_around(int cx, int cy) const
{
    int count = 0;
    for (int y = cy - 1; y <= cy + 1; ++y) {
        for (int x = cx - 1; x <= cx + 1; ++x) {
            if (on_board(x, y) && fossil_at(x, y) != kNone) {
                ++count;
            }
        }
    }
    return count;
}

void Layout::randomize(Random &rng)
{
    while (true) {
        bool used[kCells] = {};
        bool placed = true;
        for (int id = 0; id < kFossils && placed; ++id) {
            placed = false;
            for (int attempt = 0; attempt < 100 && !placed; ++attempt) {
                const Placement p = {static_cast<uint8_t>(rng.below(kGrid)),
                                     static_cast<uint8_t>(rng.below(kGrid)),
                                     static_cast<uint8_t>(rng.below(4))};
                Cell cells[4];
                const int n = cells_for(id, p, cells);
                bool fits = true;
                for (int i = 0; i < n && fits; ++i) {
                    fits = on_board(cells[i].x, cells[i].y) &&
                           !used[cells[i].y * kGrid + cells[i].x];
                }
                if (fits) {
                    for (int i = 0; i < n; ++i) {
                        used[cells[i].y * kGrid + cells[i].x] = true;
                    }
                    fossils[id] = p;
                    placed = true;
                }
            }
        }
        if (placed) {
            return;
        }
    }
}

bool Layout::move(int id, int x, int y)
{
    const Placement original = fossils[id];
    Cell shape[4];
    const int n = rotated_shape(id, original.rot, shape);
    // Try each anchoring that puts one of the fossil's squares under (x, y).
    for (int i = 0; i < n; ++i) {
        const int ax = x - shape[i].x;
        const int ay = y - shape[i].y;
        if (ax < 0 || ay < 0) {
            continue;
        }
        fossils[id] = {static_cast<uint8_t>(ax), static_cast<uint8_t>(ay), original.rot};
        if (valid()) {
            return true;
        }
    }
    fossils[id] = original;
    return false;
}

bool Layout::rotate(int id)
{
    const Placement original = fossils[id];
    const uint8_t rot = static_cast<uint8_t>((original.rot + 1) & 3);
    // Nudge offsets ordered by distance from the original position.
    constexpr int8_t kNudges[][2] = {
        {0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
        {-2, 0}, {0, -2}, {2, 0}, {0, 2},
    };
    for (const auto &d : kNudges) {
        const int x = original.x + d[0];
        const int y = original.y + d[1];
        if (x < 0 || y < 0) {
            continue;
        }
        fossils[id] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y), rot};
        if (valid()) {
            return true;
        }
    }
    fossils[id] = original;
    return false;
}

void Layout::encode(uint8_t out[kLayoutBytes]) const
{
    for (int id = 0; id < kFossils; ++id) {
        out[id * 3] = fossils[id].x;
        out[id * 3 + 1] = fossils[id].y;
        out[id * 3 + 2] = fossils[id].rot;
    }
}

bool Layout::decode(const uint8_t in[kLayoutBytes])
{
    for (int id = 0; id < kFossils; ++id) {
        fossils[id] = {in[id * 3], in[id * 3 + 1], in[id * 3 + 2]};
        if (fossils[id].x >= kGrid || fossils[id].y >= kGrid) {
            return false;
        }
    }
    return valid();
}

void commitment(uint32_t game_no, const Layout &layout,
                const uint8_t salt[kSaltBytes], uint8_t out[kHashBytes])
{
    uint8_t bytes[4 + kLayoutBytes];
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(game_no >> (8 * i));
    }
    layout.encode(bytes + 4);
    Sha256 sha;
    static const uint8_t kDomain[] = {'d', 'i', 'g', 'd', 'u', 'e', 'l', '1'};
    sha.update(kDomain, sizeof(kDomain));
    sha.update(bytes, sizeof(bytes));
    sha.update(salt, kSaltBytes);
    sha.finish(out);
}

void SiteView::reset()
{
    for (int i = 0; i < kCells; ++i) {
        marks[i] = Mark::Unknown;
        radar[i] = -1;
    }
    found = 0;
    done_mask = 0;
    digs = 0;
    radars_left = kRadarCharges;
}

void HomeSite::reset(const Layout &l)
{
    layout = l;
    for (int i = 0; i < kCells; ++i) {
        dug[i] = false;
        radar[i] = -1;
    }
    found = 0;
    done_mask = 0;
}

HomeSite::DigOutcome HomeSite::dig(int x, int y)
{
    DigOutcome outcome = {false, kNone, false};
    const int id = layout.fossil_at(x, y);
    const int index = y * kGrid + x;
    const bool fresh = !dug[index];
    dug[index] = true;
    if (id == kNone) {
        return outcome;
    }
    outcome.bone = true;
    if (fresh) {
        ++found;
    }
    Cell cells[4];
    const int n = layout.cells_of(id, cells);
    bool complete = true;
    for (int i = 0; i < n; ++i) {
        complete = complete && dug[cells[i].y * kGrid + cells[i].x];
    }
    if (complete && (done_mask & (1U << id)) == 0) {
        done_mask = static_cast<uint8_t>(done_mask | (1U << id));
        outcome.completed = id;
    }
    outcome.all_found = found >= kBoneCells;
    return outcome;
}

int HomeSite::ping(int x, int y)
{
    const int count = layout.bones_around(x, y);
    radar[y * kGrid + x] = static_cast<int8_t>(count);
    return count;
}

bool view_matches(const SiteView &view, const Layout &layout)
{
    if (!layout.valid()) {
        return false;
    }
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            const int i = y * kGrid + x;
            const bool bone = layout.fossil_at(x, y) != kNone;
            if ((view.marks[i] == Mark::Bone && !bone) || (view.marks[i] == Mark::Dirt && bone)) {
                return false;
            }
            if (view.radar[i] >= 0 && view.radar[i] != layout.bones_around(x, y)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace dig
