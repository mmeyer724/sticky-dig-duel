#include "game.h"

#include <cstdarg>
#include <cstdio>

#include "ui_layout.h"

using namespace dig;

namespace {

enum Msg : uint8_t {
    kCommit = 1,
    kDig = 2,
    kDigResult = 3,
    kRadar = 4,
    kRadarResult = 5,
    kReveal = 6,
    kRematch = 7,
};

constexpr uint8_t kNoFossil = 0xFF;

void mac_name(const Mac &mac, char *out, size_t len)
{
    std::snprintf(out, len, "STICKY-%02X%02X", mac.b[4], mac.b[5]);
}

bool before(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) < 0;
}

}  // namespace

Game::Game(Radio &radio, Random &rng, Platform &platform, const Mac &self,
           uint32_t session, Stats stats)
    : link_(radio, *this, session), rng_(rng), platform_(platform), self_(self), stats_(stats)
{
    layout_.randomize(rng_);
}

void Game::start(uint32_t now)
{
    last_activity_ = now;
    last_hello_ = now;
    hello_now_ = true;
    request(Redraw::Full);
}

// ---------------------------------------------------------------- input

void Game::tap(int x, int y, uint32_t now)
{
    last_activity_ = now;
    switch (phase_) {
    case Phase::Lobby:
        break;
    case Phase::Bury:
        tap_bury(x, y, now);
        break;
    case Phase::Play:
        tap_play(x, y, now);
        break;
    case Phase::Over:
        tap_over(x, y, now);
        break;
    }
}

void Game::button(Button which, bool long_press, uint32_t now)
{
    last_activity_ = now;
    if (which != Button::Ai) {
        return;
    }
    if (long_press) {
        sleep_requested_ = true;
    } else {
        request(Redraw::Full);  // a clean full refresh clears e-paper ghosting
    }
}

void Game::tap_bury(int px, int py, uint32_t now)
{
    if (committed_) {
        return;
    }
    if (ui::hit(ui::kReady, px, py)) {
        platform_.play(Sound::Tap);
        commit(now);
        return;
    }
    if (ui::hit(ui::kShuffle, px, py)) {
        layout_.randomize(rng_);
        selected_fossil_ = kNone;
        platform_.play(Sound::Tap);
        request(Redraw::Partial);
        return;
    }
    if (ui::hit(ui::kRotate, px, py)) {
        if (selected_fossil_ != kNone && layout_.rotate(selected_fossil_)) {
            platform_.play(Sound::Tap);
            request(Redraw::Partial);
        } else {
            platform_.play(Sound::Error);
        }
        return;
    }

    int x;
    int y;
    if (!ui::board_cell(px, py, x, y)) {
        return;
    }
    const int id = layout_.fossil_at(x, y);
    if (id != kNone && id == selected_fossil_) {
        if (layout_.rotate(id)) {
            platform_.play(Sound::Tap);
            request(Redraw::Partial);
        } else {
            platform_.play(Sound::Error);
        }
    } else if (id != kNone) {
        selected_fossil_ = id;
        platform_.play(Sound::Tap);
        request(Redraw::Partial);
    } else if (selected_fossil_ != kNone) {
        if (layout_.move(selected_fossil_, x, y)) {
            platform_.play(Sound::Tap);
            request(Redraw::Partial);
        } else {
            platform_.play(Sound::Error);
        }
    }
}

void Game::tap_play(int px, int py, uint32_t now)
{
    if (!my_turn_ || pending_ != Pending::None) {
        return;
    }
    const bool has_selection = sel_x_ >= 0;
    if (ui::hit(ui::kDig, px, py)) {
        if (has_selection) {
            act(false, now);
        } else {
            platform_.play(Sound::Error);
        }
        return;
    }
    if (ui::hit(ui::kRadar, px, py)) {
        if (has_selection && view_.radars_left > 0) {
            act(true, now);
        } else {
            platform_.play(Sound::Error);
        }
        return;
    }
    int x;
    int y;
    if (!ui::board_cell(px, py, x, y)) {
        return;
    }
    if (view_.at(x, y) != Mark::Unknown) {
        platform_.play(Sound::Error);
        return;
    }
    sel_x_ = x;
    sel_y_ = y;
    platform_.play(Sound::Tap);
    request(Redraw::Partial);
}

void Game::tap_over(int px, int py, uint32_t now)
{
    if (rematch_sent_ || !ui::hit(ui::kRematch, px, py)) {
        return;
    }
    uint8_t body[8];
    Writer w(body, sizeof(body));
    w.u8(kRematch);
    w.u32(game_no_);
    send_message(body, w.size(), now);
    rematch_sent_ = true;
    platform_.play(Sound::Tap);
    if (rematch_received_) {
        new_game(now);
    } else {
        request(Redraw::Partial);
    }
}

void Game::act(bool radar, uint32_t now)
{
    uint8_t body[8];
    Writer w(body, sizeof(body));
    w.u8(radar ? kRadar : kDig);
    w.u32(game_no_);
    w.u8(static_cast<uint8_t>(sel_x_));
    w.u8(static_cast<uint8_t>(sel_y_));
    send_message(body, w.size(), now);
    pending_ = radar ? Pending::Radar : Pending::Dig;
    set_status(radar ? "Pinging the radar..." : "Digging...");
    platform_.play(Sound::Tap);
    // The answer usually arrives within a few milliseconds; wait briefly so
    // the screen goes straight to the result instead of refreshing twice.
    request(Redraw::Partial, now + kResultGraceMs);
}

// ---------------------------------------------------------------- radio

void Game::receive(const Mac &from, const uint8_t *frame, size_t len, int rssi, uint32_t now)
{
    link_.receive(from, frame, len, rssi, now);
}

void Game::send_hello()
{
    Mac partner = {};
    uint32_t partner_session = 0;
    if (link_.bound()) {
        partner = link_.peer();
        partner_session = link_.peer_session();
    } else if (has_choice_) {
        partner = choice_;
        partner_session = choice_session_;
    }
    uint8_t body[16];
    Writer w(body, sizeof(body));
    w.u8(static_cast<uint8_t>(phase_));
    w.bytes(partner.b, 6);
    w.u32(partner_session);
    link_.hello(body, w.size());
}

void Game::on_hello(const Mac &from, uint32_t session, const uint8_t *body, size_t len,
                    int rssi, uint32_t now)
{
    if (from == self_) {
        return;
    }
    Reader r(body, len);
    r.u8();  // sender's phase, informational
    Mac partner;
    r.bytes(partner.b, 6);
    const uint32_t partner_session = r.u32();
    if (!r.ok()) {
        return;
    }

    Candidate *slot = nullptr;
    for (Candidate &c : candidates_) {
        if (c.used && c.mac == from) {
            slot = &c;
        }
    }
    for (Candidate &c : candidates_) {
        if (slot == nullptr && !c.used) {
            slot = &c;
        }
    }
    if (slot == nullptr) {
        slot = &candidates_[0];
        for (Candidate &c : candidates_) {
            if (before(c.last_seen, slot->last_seen)) {
                slot = &c;
            }
        }
    }
    *slot = {from, session, partner, partner_session, rssi, now, true};

    if (link_.bound()) {
        const bool still_ours = partner == self_ && partner_session == link_.session();
        if (from == link_.peer() && session == link_.peer_session() && !still_ours) {
            back_to_lobby("%s left the game.", now);
            choose_partner(now);
        }
        return;
    }
    choose_partner(now);
}

void Game::choose_partner(uint32_t now)
{
    if (link_.bound() || phase_ != Phase::Lobby) {
        return;
    }
    for (Candidate &c : candidates_) {
        if (c.used && now - c.last_seen > kCandidateStaleMs) {
            c.used = false;
        }
    }
    auto open_to_us = [&](const Candidate &c) {
        return c.partner.is_zero() || (c.partner == self_ && c.partner_session == link_.session());
    };

    // Keep the current choice while it stays open to us so that both sides
    // converge; otherwise propose to the strongest available signal.
    const Candidate *pick = nullptr;
    for (const Candidate &c : candidates_) {
        if (has_choice_ && c.used && c.mac == choice_ && open_to_us(c)) {
            pick = &c;
        }
    }
    if (pick == nullptr) {
        for (const Candidate &c : candidates_) {
            if (c.used && open_to_us(c) && (pick == nullptr || c.rssi > pick->rssi)) {
                pick = &c;
            }
        }
    }

    const bool changed = (pick != nullptr) != has_choice_ ||
                         (pick != nullptr && (pick->mac != choice_ || pick->session != choice_session_));
    has_choice_ = pick != nullptr;
    if (pick != nullptr) {
        choice_ = pick->mac;
        choice_session_ = pick->session;
    }
    if (changed) {
        hello_now_ = true;
        request(Redraw::Partial);
    }
    if (pick != nullptr && pick->partner == self_ && pick->partner_session == link_.session()) {
        pair_with(*pick, now);
    }
}

void Game::pair_with(const Candidate &c, uint32_t now)
{
    link_.bind(c.mac, c.session, now);
    first_game_ = true;
    game_no_ = 0;
    hello_now_ = true;
    last_activity_ = now;
    shown_lost_ = false;
    platform_.play(Sound::Paired);
    new_game(now);
}

void Game::back_to_lobby(const char *notice, uint32_t now)
{
    char name[16];
    peer_name(name, sizeof(name));
    std::snprintf(notice_, sizeof(notice_), notice, name);
    link_.unbind();
    phase_ = Phase::Lobby;
    has_choice_ = false;
    pending_ = Pending::None;
    shown_lost_ = false;
    hello_now_ = true;
    last_activity_ = now;
    request(Redraw::Full);
}

void Game::on_peer_restarted(uint32_t now)
{
    back_to_lobby("%s restarted, so that game ended.", now);
}

void Game::on_message(const uint8_t *body, size_t len, uint32_t now)
{
    Reader r(body, len);
    const uint8_t type = r.u8();
    switch (type) {
    case kCommit:
        handle_commit(r, now);
        break;
    case kDig:
        handle_dig(r, now);
        break;
    case kDigResult:
        handle_dig_result(r, now);
        break;
    case kRadar:
        handle_radar(r, now);
        break;
    case kRadarResult:
        handle_radar_result(r, now);
        break;
    case kReveal:
        handle_reveal(r);
        break;
    case kRematch:
        if (r.u32() == game_no_ && r.ok()) {
            handle_rematch(now);
        }
        break;
    default:
        break;
    }
}

void Game::send_message(const uint8_t *body, size_t len, uint32_t now)
{
    link_.send(body, len, now);
}

// ---------------------------------------------------------------- game flow

void Game::new_game(uint32_t now)
{
    (void)now;
    ++game_no_;
    phase_ = Phase::Bury;
    layout_.randomize(rng_);
    selected_fossil_ = kNone;
    committed_ = false;
    peer_committed_ = false;
    peer_revealed_ = false;
    rematch_sent_ = false;
    rematch_received_ = false;
    verdict_ = Verdict::Waiting;
    pending_ = Pending::None;
    status_[0] = '\0';
    request(Redraw::Full);
}

void Game::commit(uint32_t now)
{
    for (uint8_t &b : salt_) {
        b = static_cast<uint8_t>(rng_.next());
    }
    nonce_ = rng_.next();
    uint8_t hash[kHashBytes];
    commitment(game_no_, layout_, salt_, hash);

    uint8_t body[48];
    Writer w(body, sizeof(body));
    w.u8(kCommit);
    w.u32(game_no_);
    w.u32(nonce_);
    w.bytes(hash, sizeof(hash));
    send_message(body, w.size(), now);

    committed_ = true;
    selected_fossil_ = kNone;
    notice_[0] = '\0';
    if (peer_committed_) {
        start_play(now);
    } else {
        request(Redraw::Partial);
    }
}

void Game::handle_commit(Reader &r, uint32_t now)
{
    const uint32_t game = r.u32();
    const uint32_t nonce = r.u32();
    uint8_t hash[kHashBytes];
    r.bytes(hash, sizeof(hash));
    if (!r.ok() || game != game_no_ || phase_ != Phase::Bury || peer_committed_) {
        return;
    }
    peer_nonce_ = nonce;
    std::memcpy(peer_hash_, hash, sizeof(hash));
    peer_committed_ = true;
    last_activity_ = now;
    if (committed_) {
        start_play(now);
    } else {
        request(Redraw::Partial);
    }
}

void Game::start_play(uint32_t now)
{
    (void)now;
    home_.reset(layout_);
    view_.reset();
    bool me_first;
    if (first_game_) {
        me_first = nonce_ != peer_nonce_ ? nonce_ > peer_nonce_ : self_ < link_.peer();
    } else {
        me_first = i_start_next_;
    }
    first_game_ = false;
    my_turn_ = me_first;
    pending_ = Pending::None;
    sel_x_ = sel_y_ = -1;
    last_their_x_ = last_their_y_ = -1;
    phase_ = Phase::Play;

    char name[16];
    peer_name(name, sizeof(name));
    if (me_first) {
        set_status("You dig first. Tap a square, then DIG.");
        platform_.play(Sound::YourTurn);
    } else {
        set_status("%s digs first.", name);
    }
    request(Redraw::Full);
}

void Game::handle_dig(Reader &r, uint32_t now)
{
    const uint32_t game = r.u32();
    const int x = r.u8();
    const int y = r.u8();
    if (!r.ok() || game != game_no_ || phase_ != Phase::Play || my_turn_ || !on_board(x, y)) {
        return;
    }
    const HomeSite::DigOutcome out = home_.dig(x, y);
    uint8_t body[12];
    Writer w(body, sizeof(body));
    w.u8(kDigResult);
    w.u32(game_no_);
    w.u8(static_cast<uint8_t>(x));
    w.u8(static_cast<uint8_t>(y));
    w.u8(out.bone ? 1 : 0);
    w.u8(out.completed == kNone ? kNoFossil : static_cast<uint8_t>(out.completed));
    w.u8(out.all_found ? 1 : 0);
    send_message(body, w.size(), now);

    last_their_x_ = x;
    last_their_y_ = y;
    last_activity_ = now;
    char name[16];
    peer_name(name, sizeof(name));
    if (out.all_found) {
        set_status("%s dug up your last bone.", name);
        finish(false, now);
        return;
    }
    if (out.completed != kNone) {
        set_status("%s dug up your %s and digs again.", name, fossil_kind(out.completed).name);
    } else if (out.bone) {
        set_status("%s hit one of your bones and digs again.", name);
    } else {
        my_turn_ = true;
        set_status("%s hit dirt. Your turn!", name);
        platform_.play(Sound::YourTurn);
    }
    request(Redraw::Partial);
}

void Game::handle_dig_result(Reader &r, uint32_t now)
{
    const uint32_t game = r.u32();
    const int x = r.u8();
    const int y = r.u8();
    const bool bone = r.u8() != 0;
    const uint8_t completed = r.u8();
    const bool all_found = r.u8() != 0;
    if (!r.ok() || game != game_no_ || phase_ != Phase::Play || !my_turn_ ||
        pending_ != Pending::Dig || x != sel_x_ || y != sel_y_) {
        return;
    }
    const int i = y * kGrid + x;
    if (view_.marks[i] == Mark::Unknown) {
        view_.marks[i] = bone ? Mark::Bone : Mark::Dirt;
        ++view_.digs;
        if (bone) {
            ++view_.found;
        }
    }
    if (completed < kFossils) {
        view_.done_mask = static_cast<uint8_t>(view_.done_mask | (1U << completed));
    }
    pending_ = Pending::None;
    sel_x_ = sel_y_ = -1;
    last_activity_ = now;
    request(Redraw::Partial);

    char name[16];
    peer_name(name, sizeof(name));
    if (all_found) {
        set_status("You dug up their last bone!");
        finish(true, now);
    } else if (completed < kFossils) {
        set_status("You unearthed their %s! Dig again.", fossil_kind(completed).name);
        platform_.play(Sound::Fossil);
    } else if (bone) {
        set_status("Bone! Dig again.");
        platform_.play(Sound::Bone);
    } else {
        my_turn_ = false;
        set_status("Just dirt. %s is digging...", name);
        platform_.play(Sound::Dirt);
    }
}

void Game::handle_radar(Reader &r, uint32_t now)
{
    const uint32_t game = r.u32();
    const int x = r.u8();
    const int y = r.u8();
    if (!r.ok() || game != game_no_ || phase_ != Phase::Play || my_turn_ || !on_board(x, y)) {
        return;
    }
    const int count = home_.ping(x, y);
    uint8_t body[12];
    Writer w(body, sizeof(body));
    w.u8(kRadarResult);
    w.u32(game_no_);
    w.u8(static_cast<uint8_t>(x));
    w.u8(static_cast<uint8_t>(y));
    w.u8(static_cast<uint8_t>(count));
    send_message(body, w.size(), now);

    my_turn_ = true;
    last_activity_ = now;
    char name[16];
    peer_name(name, sizeof(name));
    set_status("%s used the radar. Your turn!", name);
    platform_.play(Sound::YourTurn);
    request(Redraw::Partial);
}

void Game::handle_radar_result(Reader &r, uint32_t now)
{
    const uint32_t game = r.u32();
    const int x = r.u8();
    const int y = r.u8();
    const int count = r.u8();
    if (!r.ok() || game != game_no_ || phase_ != Phase::Play || !my_turn_ ||
        pending_ != Pending::Radar || x != sel_x_ || y != sel_y_ || count > 9) {
        return;
    }
    view_.radar[y * kGrid + x] = static_cast<int8_t>(count);
    if (view_.radars_left > 0) {
        --view_.radars_left;
    }
    pending_ = Pending::None;
    sel_x_ = sel_y_ = -1;
    my_turn_ = false;
    last_activity_ = now;
    char name[16];
    peer_name(name, sizeof(name));
    set_status("Radar: %d bone%s in that 3x3 area. %s is digging...", count,
               count == 1 ? "" : "s", name);
    platform_.play(Sound::Dirt);
    request(Redraw::Partial);
}

void Game::finish(bool won, uint32_t now)
{
    phase_ = Phase::Over;
    won_ = won;
    if (won) {
        ++stats_.wins;
    } else {
        ++stats_.losses;
    }
    platform_.save_stats(stats_);
    i_start_next_ = !won;  // the loser digs first in the rematch

    uint8_t body[1 + 4 + kLayoutBytes + kSaltBytes];
    Writer w(body, sizeof(body));
    w.u8(kReveal);
    w.u32(game_no_);
    uint8_t layout[kLayoutBytes];
    layout_.encode(layout);
    w.bytes(layout, sizeof(layout));
    w.bytes(salt_, sizeof(salt_));
    send_message(body, w.size(), now);

    platform_.play(won ? Sound::Win : Sound::Lose);
    request(Redraw::Full);
}

void Game::handle_reveal(Reader &r)
{
    const uint32_t game = r.u32();
    uint8_t layout[kLayoutBytes];
    uint8_t salt[kSaltBytes];
    r.bytes(layout, sizeof(layout));
    r.bytes(salt, sizeof(salt));
    if (!r.ok() || game != game_no_ || !peer_committed_ || peer_revealed_) {
        return;
    }
    Layout revealed;
    const bool decoded = revealed.decode(layout);
    uint8_t hash[kHashBytes];
    commitment(game, revealed, salt, hash);
    const bool honest = decoded && std::memcmp(hash, peer_hash_, sizeof(hash)) == 0 &&
                        view_matches(view_, revealed);
    peer_layout_ = revealed;
    peer_revealed_ = decoded;
    verdict_ = honest ? Verdict::Fair : Verdict::Mismatch;
    request(Redraw::Partial);
}

void Game::handle_rematch(uint32_t now)
{
    if (phase_ != Phase::Over) {
        return;
    }
    rematch_received_ = true;
    last_activity_ = now;
    if (rematch_sent_) {
        new_game(now);
    } else {
        request(Redraw::Partial);
    }
}

// ---------------------------------------------------------------- timing

void Game::tick(uint32_t now)
{
    link_.tick(now);
    if (!link_.bound()) {
        choose_partner(now);
    }

    const uint32_t interval = link_.bound() ? kHelloPairedMs : kHelloLobbyMs;
    if (hello_now_ || now - last_hello_ >= interval) {
        send_hello();
        last_hello_ = now;
        hello_now_ = false;
    }

    const bool lost = lost_contact(now);
    if (lost != shown_lost_) {
        shown_lost_ = lost;
        request(Redraw::Partial);
    }
    if (link_.bound() && now - link_.last_heard() > kAbandonMs) {
        back_to_lobby("Lost contact with %s.", now);
    }
}

bool Game::lost_contact(uint32_t now) const
{
    return link_.bound() && now - link_.last_heard() > kLostContactMs;
}

void Game::request(Redraw kind, uint32_t not_before)
{
    if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(redraw_)) {
        redraw_ = kind;
    }
    redraw_not_before_ = not_before;
}

Redraw Game::redraw_needed(uint32_t now) const
{
    if (redraw_ != Redraw::None && redraw_not_before_ != 0 && before(now, redraw_not_before_)) {
        return Redraw::None;
    }
    return redraw_;
}

bool Game::wants_sleep(uint32_t now) const
{
    if (sleep_requested_) {
        return true;
    }
    const uint32_t limit =
        (phase_ == Phase::Lobby && !link_.bound()) ? kIdleSleepLobbyMs : kIdleSleepGameMs;
    return now - last_activity_ > limit;
}

void Game::set_status(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_, sizeof(status_), fmt, args);
    va_end(args);
}

void Game::peer_name(char *out, size_t len) const
{
    if (link_.bound()) {
        mac_name(link_.peer(), out, len);
    } else if (has_choice_) {
        mac_name(choice_, out, len);
    } else {
        std::snprintf(out, len, "the other Sticky");
    }
}
