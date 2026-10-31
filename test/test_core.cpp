// Host tests for the portable game core: rules, SHA-256, the reliable link,
// and complete two-device games over a simulated lossy radio.
//
//   make -C test            # build and run
//   ./test/test_core --screens out/   # also write a PBM of every screen

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game.h"
#include "rules.h"
#include "sha256.h"
#include "ui_layout.h"

using namespace dig;

namespace {

int g_failures = 0;
const char *g_screens_dir = nullptr;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

class XorShift : public Random {
public:
    explicit XorShift(uint32_t seed) : state_(seed ? seed : 1) {}
    uint32_t next() override
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

private:
    uint32_t state_;
};

std::string hex(const uint8_t *d, size_t n)
{
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", d[i]);
        s += b;
    }
    return s;
}

// ------------------------------------------------------------------ unit tests

void test_sha256()
{
    uint8_t out[32];
    Sha256 empty;
    empty.finish(out);
    CHECK(hex(out, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    Sha256 abc;
    abc.update(reinterpret_cast<const uint8_t *>("abc"), 3);
    abc.finish(out);
    CHECK(hex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256 longer;
    for (const char *p = two_blocks; *p; ++p) {
        longer.update(reinterpret_cast<const uint8_t *>(p), 1);  // byte at a time
    }
    longer.finish(out);
    CHECK(hex(out, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void test_rules()
{
    int total = 0;
    for (int id = 0; id < kFossils; ++id) {
        total += fossil_kind(id).size;
    }
    CHECK(total == kBoneCells);

    XorShift rng(7);
    for (int round = 0; round < 2000; ++round) {
        Layout l;
        l.randomize(rng);
        CHECK(l.valid());
        int bones = 0;
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                bones += l.fossil_at(x, y) != kNone;
            }
        }
        CHECK(bones == kBoneCells);

        uint8_t bytes[kLayoutBytes];
        l.encode(bytes);
        Layout back;
        CHECK(back.decode(bytes));
        CHECK(std::memcmp(&back, &l, sizeof(l)) == 0);

        // Random edits never produce an invalid layout.
        for (int k = 0; k < 10; ++k) {
            const int id = static_cast<int>(rng.below(kFossils));
            if (rng.below(2)) {
                const int x = static_cast<int>(rng.below(kGrid));
                const int y = static_cast<int>(rng.below(kGrid));
                if (l.move(id, x, y)) {
                    CHECK(l.fossil_at(x, y) == id);
                }
            } else {
                l.rotate(id);
            }
            CHECK(l.valid());
        }
    }

    // Every rotation of every fossil fits somewhere on an empty board.
    for (int id = 0; id < kFossils; ++id) {
        for (int rot = 0; rot < 4; ++rot) {
            Cell cells[4];
            const int n = Layout::cells_for(id, {0, 0, static_cast<uint8_t>(rot)}, cells);
            CHECK(n == fossil_kind(id).size);
            for (int i = 0; i < n; ++i) {
                CHECK(cells[i].x >= 0 && cells[i].y >= 0);
            }
        }
    }

    // A known layout: dig everything and check fossils complete exactly once.
    Layout l;
    l.fossils[0] = {0, 0, 0};  // SKULL at A1-B2
    l.fossils[1] = {3, 0, 0};  // FEMUR along the top
    l.fossils[2] = {0, 4, 0};  // CLAW
    l.fossils[3] = {5, 6, 0};  // TOOTH bottom right
    CHECK(l.valid());
    CHECK(l.bones_around(0, 0) == 4);
    CHECK(l.bones_around(6, 6) == 2);
    CHECK(l.bones_around(3, 3) == 0);
    CHECK(!l.move(3, 0, 0));  // occupied by the skull

    HomeSite home;
    home.reset(l);
    int completions = 0;
    bool finished = false;
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            const HomeSite::DigOutcome out = home.dig(x, y);
            CHECK(out.bone == (l.fossil_at(x, y) != kNone));
            completions += out.completed != kNone;
            CHECK(!finished);
            finished = out.all_found;
        }
    }
    CHECK(completions == kFossils);
    CHECK(home.found == kBoneCells);

    SiteView view;
    view.reset();
    view.marks[0] = Mark::Bone;
    view.marks[3 * kGrid + 3] = Mark::Dirt;
    view.radar[6 * kGrid + 6] = 2;
    CHECK(view_matches(view, l));
    view.radar[6 * kGrid + 6] = 1;
    CHECK(!view_matches(view, l));

    uint8_t salt[kSaltBytes] = {1, 2, 3};
    uint8_t a[kHashBytes];
    uint8_t b[kHashBytes];
    commitment(1, l, salt, a);
    commitment(2, l, salt, b);
    CHECK(std::memcmp(a, b, kHashBytes) != 0);
}

// ------------------------------------------------------------------ simulated radio

struct Frame {
    uint32_t at;
    int to;
    Mac from;
    std::vector<uint8_t> data;
};

struct Network;

struct NodeRadio : Radio {
    Network *net;
    int index;
    void send(const Mac &to, const uint8_t *data, size_t len) override;
};

struct Network {
    struct Config {
        int drop_percent = 0;
        int dup_percent = 0;
        uint32_t max_delay_ms = 5;
    } config;
    XorShift rng{12345};
    std::vector<Mac> macs;
    std::vector<bool> online;
    std::deque<Frame> in_flight;
    uint32_t now = 0;
    uint64_t sent = 0;

    void send(int from, const Mac &to, const uint8_t *data, size_t len)
    {
        ++sent;
        for (size_t i = 0; i < macs.size(); ++i) {
            if (static_cast<int>(i) == from || !online[i]) {
                continue;
            }
            if (to != Mac::broadcast() && to != macs[i]) {
                continue;
            }
            const int copies = rng.below(100) < static_cast<uint32_t>(config.dup_percent) ? 2 : 1;
            for (int k = 0; k < copies; ++k) {
                if (rng.below(100) < static_cast<uint32_t>(config.drop_percent)) {
                    continue;
                }
                const uint32_t delay = 1 + rng.below(config.max_delay_ms);
                in_flight.push_back({now + delay, static_cast<int>(i), macs[from],
                                     std::vector<uint8_t>(data, data + len)});
            }
        }
    }
};

void NodeRadio::send(const Mac &to, const uint8_t *data, size_t len)
{
    net->send(index, to, data, len);
}

struct SimPlatform : Platform {
    Stats saved = {};
    int sounds = 0;
    void play(Sound) override { ++sounds; }
    void save_stats(const Stats &s) override { saved = s; }
    int battery_percent() override { return 83; }
};

// One simulated Sticky: a Game, its radio, a canvas, and the "busy while the
// e-paper refreshes" behaviour of the real main loop.
struct Device {
    std::string label;
    Mac mac;
    NodeRadio radio;
    SimPlatform platform;
    XorShift rng;
    std::unique_ptr<Game> game;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(Canvas::kBytes);
    Canvas canvas{pixels.data()};
    uint32_t busy_until = 0;
    std::deque<Frame> rx;  // frames waiting while the main loop is busy
    int refreshes = 0;
    int full_refreshes = 0;
    int shots = 0;

    Device(Network &net, int index, const std::string &name, uint32_t seed)
        : label(name), rng(seed)
    {
        mac = Mac{{0x24, 0x58, 0x7C, 0x10, static_cast<uint8_t>(0x30 + index),
                   static_cast<uint8_t>(0xA0 + index)}};
        radio.net = &net;
        radio.index = index;
        boot(net.now, seed);
    }

    void boot(uint32_t now, uint32_t session)
    {
        game.reset(new Game(radio, rng, platform, mac, session | 1, platform.saved));
        game->start(now);
        rx.clear();
        busy_until = 0;
    }

    void screenshot(const char *tag)
    {
        if (g_screens_dir == nullptr) {
            return;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s-%02d-%s.pbm", g_screens_dir, label.c_str(),
                      shots++, tag);
        FILE *f = std::fopen(path, "wb");
        if (f == nullptr) {
            return;
        }
        std::fprintf(f, "P4\n%d %d\n", Canvas::kWidth, Canvas::kHeight);
        for (uint8_t b : pixels) {
            std::fputc(static_cast<uint8_t>(~b), f);  // PBM uses 1 = black
        }
        std::fclose(f);
    }
};

const char *phase_name(Game::Phase p)
{
    switch (p) {
    case Game::Phase::Lobby:
        return "lobby";
    case Game::Phase::Bury:
        return "bury";
    case Game::Phase::Play:
        return "play";
    case Game::Phase::Over:
        return "over";
    }
    return "?";
}

struct World {
    Network net;
    std::vector<std::unique_ptr<Device>> devices;
    bool simulate_refresh_time = true;

    Device &add(const std::string &name, uint32_t seed)
    {
        const int index = static_cast<int>(devices.size());
        devices.emplace_back(new Device(net, index, name, seed));
        net.macs.push_back(devices.back()->mac);
        net.online.push_back(true);
        return *devices.back();
    }

    // Advances time by 10 ms, the main loop's granularity on the device.
    void step()
    {
        net.now += 10;
        for (auto it = net.in_flight.begin(); it != net.in_flight.end();) {
            if (static_cast<int32_t>(net.now - it->at) >= 0) {
                devices[it->to]->rx.push_back(*it);
                it = net.in_flight.erase(it);
            } else {
                ++it;
            }
        }
        for (size_t i = 0; i < devices.size(); ++i) {
            Device &d = *devices[i];
            if (!net.online[i] || static_cast<int32_t>(net.now - d.busy_until) < 0) {
                continue;
            }
            size_t budget = 32;  // the device's event queue depth
            while (d.rx.size() > budget) {
                d.rx.pop_front();  // overflowed while busy
            }
            while (!d.rx.empty()) {
                Frame f = d.rx.front();
                d.rx.pop_front();
                d.game->receive(f.from, f.data.data(), f.data.size(), -50, net.now);
            }
            d.game->tick(net.now);
            const Redraw r = d.game->redraw_needed(net.now);
            if (r != Redraw::None) {
                d.game->render(d.canvas, net.now);
                ++d.refreshes;
                d.full_refreshes += r == Redraw::Full;
                if (simulate_refresh_time) {
                    d.busy_until = net.now + (r == Redraw::Full ? 2500 : 900);
                }
                d.screenshot(phase_name(d.game->phase()));
            }
        }
    }

    bool run_until(uint32_t limit_ms, const std::function<bool()> &done)
    {
        const uint32_t end = net.now + limit_ms;
        while (static_cast<int32_t>(net.now - end) < 0) {
            if (done()) {
                return true;
            }
            step();
        }
        return done();
    }
};

void tap_rect(Device &d, const Rect &r, uint32_t now)
{
    d.game->tap(r.x + r.w / 2, r.y + r.h / 2, now);
}

void tap_cell(Device &d, int x, int y, uint32_t now)
{
    d.game->tap(ui::kBoardX + x * ui::kCell + ui::kCell / 2,
                ui::kBoardY + y * ui::kCell + ui::kCell / 2, now);
}

// A simple player: radar once early, otherwise dig unknown squares, preferring
// neighbours of found bones.
bool play_turn(Device &d, uint32_t now)
{
    Game &g = *d.game;
    if (g.phase() != Game::Phase::Play || !g.my_turn() ||
        std::strstr(g.status(), "...") != nullptr) {
        return false;
    }
    const SiteView &v = g.view();
    int best_x = -1;
    int best_y = -1;
    int best_score = -1;
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            if (v.at(x, y) != Mark::Unknown) {
                continue;
            }
            int score = static_cast<int>(d.rng.below(10));
            const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &dir : dirs) {
                const int nx = x + dir[0];
                const int ny = y + dir[1];
                if (on_board(nx, ny) && v.at(nx, ny) == Mark::Bone) {
                    score += 50;
                }
            }
            if (score > best_score) {
                best_score = score;
                best_x = x;
                best_y = y;
            }
        }
    }
    if (best_x < 0) {
        return false;
    }
    tap_cell(d, best_x, best_y, now);
    static bool showed_selection = false;
    if (g_screens_dir != nullptr && !showed_selection) {
        showed_selection = true;  // leave one selection on screen for the screenshots
        return true;
    }
    const bool radar = v.radars_left > 0 && v.digs == 2 + v.radars_left;
    tap_rect(d, radar ? ui::kRadar : ui::kDig, now);
    return true;
}

bool phases_are(World &w, Game::Phase p)
{
    for (auto &d : w.devices) {
        if (d->game->phase() != p) {
            return false;
        }
    }
    return true;
}

void play_one_game(World &w, bool first_game)
{
    Device &a = *w.devices[0];
    Device &b = *w.devices[1];
    CHECK(w.run_until(30000, [&] { return phases_are(w, Game::Phase::Bury); }));

    // A rearranges a fossil, both declare ready.
    tap_cell(a, 3, 3, w.net.now);
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            if (a.game->layout().fossil_at(x, y) == 0) {
                tap_cell(a, x, y, w.net.now);  // pick up the skull
                y = kGrid;
                break;
            }
        }
    }
    tap_rect(a, ui::kRotate, w.net.now);
    w.run_until(3000, [] { return false; });
    tap_rect(a, ui::kReady, w.net.now);
    w.run_until(4000, [] { return false; });
    tap_rect(b, ui::kShuffle, w.net.now);
    tap_rect(b, ui::kReady, w.net.now);
    CHECK(w.run_until(30000, [&] { return phases_are(w, Game::Phase::Play); }));
    CHECK(a.game->my_turn() != b.game->my_turn());
    CHECK(a.game->game_no() == b.game->game_no());
    const bool a_first = a.game->my_turn();

    int turns = 0;
    const bool finished = w.run_until(30 * 60 * 1000, [&] {
        play_turn(a, w.net.now) || play_turn(b, w.net.now);
        if (a.game->phase() == Game::Phase::Play && b.game->phase() == Game::Phase::Play) {
            CHECK(!(a.game->my_turn() && b.game->my_turn()) ||
                  a.game->link().sending() || b.game->link().sending());
        }
        ++turns;
        return phases_are(w, Game::Phase::Over) &&
               a.game->verdict() != Game::Verdict::Waiting &&
               b.game->verdict() != Game::Verdict::Waiting;
    });
    CHECK(finished);
    CHECK(a.game->won() != b.game->won());
    CHECK(a.game->verdict() == Game::Verdict::Fair);
    CHECK(b.game->verdict() == Game::Verdict::Fair);
    Device &winner = a.game->won() ? a : b;
    Device &loser = a.game->won() ? b : a;
    CHECK(winner.game->view().found == kBoneCells);
    CHECK(loser.game->home().found == kBoneCells);
    CHECK(loser.game->view().found < kBoneCells);
    std::printf("  game %u: %s won (%s dug first), %u digs, %d/%d refreshes (full %d/%d)\n",
                a.game->game_no(), winner.label.c_str(), a_first ? "A" : "B",
                winner.game->view().digs, a.refreshes, b.refreshes, a.full_refreshes,
                b.full_refreshes);
    (void)first_game;
}

void test_full_games(int drop, int dup, const char *title)
{
    std::printf("%s\n", title);
    World w;
    w.net.config.drop_percent = drop;
    w.net.config.dup_percent = dup;
    Device &a = w.add("A", 0x1111);
    Device &b = w.add("B", 0x2222);

    // Lobby screens before the devices can hear each other.
    w.net.online[1] = false;
    w.run_until(1500, [] { return false; });
    w.net.online[1] = true;

    play_one_game(w, true);
    const bool a_won_first = a.game->won();
    CHECK(a.platform.saved.wins + a.platform.saved.losses == 1);

    // Rematch: the loser digs first.
    tap_rect(a, ui::kRematch, w.net.now);
    w.run_until(3000, [] { return false; });
    tap_rect(b, ui::kRematch, w.net.now);
    CHECK(w.run_until(30000, [&] { return phases_are(w, Game::Phase::Bury); }));
    tap_rect(a, ui::kReady, w.net.now);
    tap_rect(b, ui::kReady, w.net.now);
    CHECK(w.run_until(30000, [&] { return phases_are(w, Game::Phase::Play); }));
    CHECK(a.game->my_turn() == !a_won_first);
    CHECK(a.game->game_no() == 2 && b.game->game_no() == 2);
    w.run_until(60 * 60 * 1000, [&] {
        play_turn(a, w.net.now) || play_turn(b, w.net.now);
        return phases_are(w, Game::Phase::Over) &&
               a.game->verdict() != Game::Verdict::Waiting &&
               b.game->verdict() != Game::Verdict::Waiting;
    });
    CHECK(phases_are(w, Game::Phase::Over));
    CHECK(a.game->won() != b.game->won());
    CHECK(a.game->verdict() == Game::Verdict::Fair && b.game->verdict() == Game::Verdict::Fair);
    CHECK(a.platform.saved.wins + a.platform.saved.losses == 2);
    CHECK(a.platform.saved.wins == b.platform.saved.losses);
    std::printf("  game 2: %s won, network frames sent %llu\n", a.game->won() ? "A" : "B",
                static_cast<unsigned long long>(w.net.sent));
}

void test_restart_and_lost_contact()
{
    std::printf("restart and lost contact\n");
    World w;
    w.net.config.drop_percent = 10;
    Device &a = w.add("R", 0x3333);
    Device &b = w.add("S", 0x4444);
    CHECK(w.run_until(30000, [&] { return phases_are(w, Game::Phase::Bury); }));
    tap_rect(a, ui::kReady, w.net.now);
    tap_rect(b, ui::kReady, w.net.now);
    CHECK(w.run_until(30000, [&] { return phases_are(w, Game::Phase::Play); }));
    for (int i = 0; i < 4; ++i) {
        w.run_until(5000, [&] { return play_turn(a, w.net.now) || play_turn(b, w.net.now); });
        w.run_until(3000, [] { return false; });
    }

    // B walks out of range: A shows lost contact, then recovers.
    w.net.online[1] = false;
    w.run_until(Game::kLostContactMs + 4000, [] { return false; });
    CHECK(a.game->phase() == Game::Phase::Play);
    w.net.online[1] = true;
    w.run_until(8000, [] { return false; });
    CHECK(a.game->phase() == Game::Phase::Play && b.game->phase() == Game::Phase::Play);
    CHECK(a.game->link().bound() && b.game->link().bound());

    // B reboots mid-game: A notices, both re-pair into a fresh game.
    b.boot(w.net.now, 0xBEEF);
    CHECK(w.run_until(30000, [&] {
        return phases_are(w, Game::Phase::Bury) && a.game->link().bound() &&
               b.game->link().bound() && a.game->link().peer_session() == b.game->link().session();
    }));
    CHECK(a.game->game_no() == 1);
}

void test_three_devices()
{
    std::printf("three devices pair off without deadlock\n");
    World w;
    w.simulate_refresh_time = false;
    w.add("X", 0x5555);
    w.add("Y", 0x6666);
    w.add("Z", 0x7777);
    w.run_until(20000, [] { return false; });
    int paired = 0;
    for (auto &d : w.devices) {
        paired += d->game->phase() == Game::Phase::Bury;
    }
    CHECK(paired == 2);
    for (auto &d : w.devices) {
        if (d->game->link().bound()) {
            const Mac peer = d->game->link().peer();
            bool mutual = false;
            for (auto &e : w.devices) {
                mutual |= e->mac == peer && e->game->link().bound() && e->game->link().peer() == d->mac;
            }
            CHECK(mutual);
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--screens") == 0) {
        g_screens_dir = argv[2];
    }
    test_sha256();
    test_rules();
    test_full_games(0, 0, "clean radio");
    const char *saved = g_screens_dir;
    g_screens_dir = nullptr;  // screenshots from the first run only
    test_full_games(30, 10, "30% loss, 10% duplicates");
    test_restart_and_lost_contact();
    test_three_devices();
    g_screens_dir = saved;
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
