#pragma once

#include <cstdint>

#include "canvas.h"
#include "link.h"
#include "rules.h"
#include "wire.h"

enum class Sound : uint8_t { Tap, Error, Paired, YourTurn, Bone, Fossil, Dirt, Win, Lose };

enum class Button : uint8_t { Up, Down, Ai };

enum class Redraw : uint8_t { None, Partial, Full };

struct Stats {
    uint16_t wins;
    uint16_t losses;
};

// Hardware services the game needs besides the radio.
class Platform {
public:
    virtual ~Platform() = default;
    virtual void play(Sound sound) = 0;
    virtual void save_stats(const Stats &stats) = 0;
    virtual int battery_percent() = 0;  // -1 when unknown
};

class Game : public LinkListener {
public:
    enum class Phase : uint8_t { Lobby, Bury, Play, Over };
    enum class Verdict : uint8_t { Waiting, Fair, Mismatch };

    static constexpr uint32_t kHelloLobbyMs = 700;
    static constexpr uint32_t kHelloPairedMs = 2000;
    static constexpr uint32_t kCandidateStaleMs = 4000;
    static constexpr uint32_t kLostContactMs = 9000;
    static constexpr uint32_t kAbandonMs = 3 * 60 * 1000;
    static constexpr uint32_t kResultGraceMs = 400;
    static constexpr uint32_t kIdleSleepLobbyMs = 5 * 60 * 1000;
    static constexpr uint32_t kIdleSleepGameMs = 15 * 60 * 1000;

    Game(Radio &radio, dig::Random &rng, Platform &platform, const Mac &self,
         uint32_t session, Stats stats);

    void start(uint32_t now);
    void tap(int x, int y, uint32_t now);
    void button(Button button, bool long_press, uint32_t now);
    void receive(const Mac &from, const uint8_t *frame, size_t len, int rssi, uint32_t now);
    void tick(uint32_t now);

    Redraw redraw_needed(uint32_t now) const;
    void render(Canvas &canvas, uint32_t now);
    bool wants_sleep(uint32_t now) const;
    void render_sleep(Canvas &canvas) const;

    // Introspection for tests.
    Phase phase() const { return phase_; }
    bool my_turn() const { return my_turn_; }
    bool won() const { return won_; }
    Verdict verdict() const { return verdict_; }
    const dig::SiteView &view() const { return view_; }
    const dig::HomeSite &home() const { return home_; }
    const dig::Layout &layout() const { return layout_; }
    uint32_t game_no() const { return game_no_; }
    const Link &link() const { return link_; }
    const char *status() const { return status_; }

private:
    struct Candidate {
        Mac mac;
        uint32_t session;
        Mac partner;
        uint32_t partner_session;
        int rssi;
        uint32_t last_seen;
        bool used;
    };

    // LinkListener
    void on_hello(const Mac &from, uint32_t session, const uint8_t *body, size_t len,
                  int rssi, uint32_t now) override;
    void on_message(const uint8_t *body, size_t len, uint32_t now) override;
    void on_peer_restarted(uint32_t now) override;

    // Pairing
    void choose_partner(uint32_t now);
    void pair_with(const Candidate &c, uint32_t now);
    void back_to_lobby(const char *notice, uint32_t now);
    void send_hello();

    // Game flow
    void new_game(uint32_t now);
    void commit(uint32_t now);
    void start_play(uint32_t now);
    void finish(bool won, uint32_t now);
    void act(bool radar, uint32_t now);

    void handle_commit(Reader &r, uint32_t now);
    void handle_dig(Reader &r, uint32_t now);
    void handle_dig_result(Reader &r, uint32_t now);
    void handle_radar(Reader &r, uint32_t now);
    void handle_radar_result(Reader &r, uint32_t now);
    void handle_reveal(Reader &r);
    void handle_rematch(uint32_t now);

    void tap_bury(int x, int y, uint32_t now);
    void tap_play(int x, int y, uint32_t now);
    void tap_over(int x, int y, uint32_t now);

    void send_message(const uint8_t *body, size_t len, uint32_t now);
    void request(Redraw kind, uint32_t not_before = 0);
    void set_status(const char *fmt, ...);
    bool lost_contact(uint32_t now) const;
    void peer_name(char *out, size_t len) const;

    // Rendering (screens.cpp)
    void draw_lobby(Canvas &c, uint32_t now);
    void draw_bury(Canvas &c, uint32_t now);
    void draw_play(Canvas &c, uint32_t now);
    void draw_over(Canvas &c, uint32_t now);
    void draw_header(Canvas &c);
    void draw_button(Canvas &c, const Rect &r, const char *label, bool enabled);
    void draw_target_grid(Canvas &c, bool reveal);
    void draw_home_grid(Canvas &c, int x0, int y0, int cell, bool show_digs, int selected);

    Link link_;
    dig::Random &rng_;
    Platform &platform_;
    Mac self_;
    Stats stats_;

    Phase phase_ = Phase::Lobby;
    Candidate candidates_[4] = {};
    bool has_choice_ = false;
    Mac choice_ = {};
    uint32_t choice_session_ = 0;
    uint32_t last_hello_ = 0;
    bool hello_now_ = true;
    char notice_[72] = {};

    uint32_t game_no_ = 0;
    bool first_game_ = true;
    bool i_start_next_ = false;

    dig::Layout layout_;
    uint8_t salt_[dig::kSaltBytes] = {};
    uint32_t nonce_ = 0;
    int selected_fossil_ = dig::kNone;
    bool committed_ = false;
    bool peer_committed_ = false;
    uint32_t peer_nonce_ = 0;
    uint8_t peer_hash_[dig::kHashBytes] = {};

    dig::HomeSite home_ = {};
    dig::SiteView view_ = {};
    bool my_turn_ = false;
    int sel_x_ = -1;
    int sel_y_ = -1;
    enum class Pending : uint8_t { None, Dig, Radar } pending_ = Pending::None;
    int last_their_x_ = -1;
    int last_their_y_ = -1;
    char status_[96] = {};

    bool won_ = false;
    Verdict verdict_ = Verdict::Waiting;
    dig::Layout peer_layout_;
    bool peer_revealed_ = false;
    bool rematch_sent_ = false;
    bool rematch_received_ = false;

    Redraw redraw_ = Redraw::Full;
    uint32_t redraw_not_before_ = 0;
    bool shown_lost_ = false;
    uint32_t last_activity_ = 0;
    bool sleep_requested_ = false;
};
