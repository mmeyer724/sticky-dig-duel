#pragma once

#include <cstdint>

// Dig Duel rules. Each player buries four fossils in a 7x7 dig site, then the
// players take turns digging up squares of each other's site. Finding a bone
// earns another dig. A radar ping (two per game) counts the bones in a 3x3
// area instead. The first player to unearth all 12 bones of the other site wins.
namespace dig {

constexpr int kGrid = 7;
constexpr int kCells = kGrid * kGrid;
constexpr int kFossils = 4;
constexpr int kBoneCells = 12;
constexpr int kRadarCharges = 2;
constexpr int kLayoutBytes = kFossils * 3;
constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;
constexpr int kNone = -1;

struct Cell {
    int8_t x;
    int8_t y;
};

struct FossilKind {
    const char *name;
    uint8_t size;
    Cell shape[4];  // unrotated offsets
};

const FossilKind &fossil_kind(int id);

class Random {
public:
    virtual ~Random() = default;
    virtual uint32_t next() = 0;
    uint32_t below(uint32_t n) { return n == 0 ? 0 : next() % n; }
};

struct Placement {
    uint8_t x;    // column of the rotated shape's top-left corner
    uint8_t y;
    uint8_t rot;  // quarter turns clockwise
};

class Layout {
public:
    Placement fossils[kFossils] = {};

    static int cells_for(int id, const Placement &p, Cell out[4]);
    int cells_of(int id, Cell out[4]) const { return cells_for(id, fossils[id], out); }

    bool valid() const;
    int fossil_at(int x, int y) const;  // fossil id or kNone
    int bones_around(int cx, int cy) const;

    void randomize(Random &rng);
    // Moves a fossil so that it covers (x, y). Returns false if it can't fit.
    bool move(int id, int x, int y);
    // Rotates a fossil a quarter turn, nudging it to stay on the board.
    bool rotate(int id);

    void encode(uint8_t out[kLayoutBytes]) const;
    bool decode(const uint8_t in[kLayoutBytes]);
};

void commitment(uint32_t game_no, const Layout &layout,
                const uint8_t salt[kSaltBytes], uint8_t out[kHashBytes]);

enum class Mark : uint8_t { Unknown, Dirt, Bone };

// What a player has learned about the opponent's site.
struct SiteView {
    Mark marks[kCells];
    int8_t radar[kCells];  // bone count reported at each radar centre, or -1
    uint8_t found;
    uint8_t done_mask;     // bit per fossil fully unearthed
    uint8_t digs;
    uint8_t radars_left;

    void reset();
    Mark at(int x, int y) const { return marks[y * kGrid + x]; }
};

// A player's own site, as the opponent digs into it.
struct HomeSite {
    Layout layout;
    bool dug[kCells];
    int8_t radar[kCells];
    uint8_t found;
    uint8_t done_mask;

    struct DigOutcome {
        bool bone;
        int completed;  // fossil id finished by this dig, or kNone
        bool all_found;
    };

    void reset(const Layout &l);
    DigOutcome dig(int x, int y);
    int ping(int x, int y);
};

// True if every dig and radar answer recorded in `view` agrees with `layout`.
bool view_matches(const SiteView &view, const Layout &layout);

inline bool on_board(int x, int y)
{
    return x >= 0 && y >= 0 && x < kGrid && y < kGrid;
}

}  // namespace dig
