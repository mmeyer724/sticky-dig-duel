// Screen rendering for Game. Everything is black and white (with ordered
// dither for "dirt") so the panel's fast partial refresh can be used.

#include <algorithm>
#include <cstdio>

#include "game.h"
#include "ui_layout.h"

using namespace dig;
using namespace ui;

namespace {

constexpr Color B = Color::Black;
constexpr Color W = Color::White;

Rect inset(const Rect &r, int d)
{
    return {r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d};
}

// Vertical position that centres capital letters of `font` in a box.
int caps_top(const Rect &r, const Font &font)
{
    const int cap = font.ascent * 3 / 4;
    return r.y + (r.h - cap) / 2 - (font.ascent - cap);
}

void bone(Canvas &c, int cx, int cy, int size, Color color)
{
    const int a = size * 3 / 10;
    const int shaft = std::max(3, size / 6);
    const int knob = std::max(2, size / 8);
    const int o = knob * 7 / 10 + 1;
    c.line(cx - a, cy + a, cx + a, cy - a, shaft, color);
    c.disc(cx - a - o, cy + a - o, knob, color);
    c.disc(cx - a + o, cy + a + o, knob, color);
    c.disc(cx + a - o, cy - a - o, knob, color);
    c.disc(cx + a + o, cy - a + o, knob, color);
}

void hole(Canvas &c, int cx, int cy, int size)
{
    c.ring(cx, cy, std::max(2, size / 6), std::max(1, size / 18), B);
}

void radar_badge(Canvas &c, int cx, int cy, int count, bool small)
{
    const int radius = small ? 15 : 19;
    c.disc(cx, cy, radius, W);
    c.ring(cx, cy, radius, 3, B);
    char text[4];
    std::snprintf(text, sizeof(text), "%d", count);
    const Font &font = small ? font_small() : font_medium();
    c.text(cx, caps_top({cx - radius, cy - radius, 2 * radius, 2 * radius}, font), text, font, B,
           Align::Center);
}

void battery(Canvas &c, int right, int y, int percent)
{
    if (percent < 0) {
        return;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d%%", std::min(percent, 100));
    const int w = c.text(right, y, text, font_small(), B, Align::Right);
    const Rect body = {right - w - 40, y + 5, 30, 15};
    c.frame(body, 2, B);
    c.fill({body.x + body.w, body.y + 4, 3, 7}, B);
    c.fill({body.x + 3, body.y + 3, (body.w - 6) * std::min(percent, 100) / 100, body.h - 6}, B);
}

}  // namespace

void Game::render(Canvas &c, uint32_t now)
{
    c.clear(W);
    switch (phase_) {
    case Phase::Lobby:
        draw_lobby(c, now);
        break;
    case Phase::Bury:
        draw_bury(c, now);
        break;
    case Phase::Play:
        draw_play(c, now);
        break;
    case Phase::Over:
        draw_over(c, now);
        break;
    }
    redraw_ = Redraw::None;
    redraw_not_before_ = 0;
}

void Game::draw_header(Canvas &c)
{
    char name[16];
    peer_name(name, sizeof(name));
    char text[32];
    std::snprintf(text, sizeof(text), "vs %s", name);
    c.text(kPanelX, 12, text, font_small(), B);
    battery(c, kPanelRight, 12, platform_.battery_percent());
}

void Game::draw_button(Canvas &c, const Rect &r, const char *label, bool enabled)
{
    const Font &font = font_medium();
    if (enabled) {
        c.round_rect(r, 10, B);
        c.text(r.x + r.w / 2, caps_top(r, font), label, font, W, Align::Center);
    } else {
        c.round_rect(r, 10, B);
        c.round_rect(inset(r, 3), 8, W);
        c.text(r.x + r.w / 2, caps_top(r, font), label, font, B, Align::Center);
    }
}

void Game::draw_home_grid(Canvas &c, int x0, int y0, int cell, bool show_digs, int selected)
{
    const int size = cell * kGrid;
    const int gap = cell >= 40 ? 2 : 1;
    c.fill({x0, y0, size, size}, B);
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            const Rect r = {x0 + x * cell + gap, y0 + y * cell + gap, cell - 2 * gap, cell - 2 * gap};
            const int cx = r.x + r.w / 2;
            const int cy = r.y + r.h / 2;
            const int id = layout_.fossil_at(x, y);
            const bool dug = show_digs && home_.dug[y * kGrid + x];
            if (id == kNone) {
                if (dug) {
                    c.fill(r, W);
                    hole(c, cx, cy, cell);
                } else {
                    c.shade(r, Shade::Light);
                }
                continue;
            }
            const Color fg = id == selected ? B : W;
            const Color bg = id == selected ? W : B;
            c.fill(r, bg);
            // Join squares of the same fossil so each reads as one piece.
            if (x + 1 < kGrid && layout_.fossil_at(x + 1, y) == id) {
                c.fill({r.x + r.w, r.y, 2 * gap, r.h}, bg);
            }
            if (y + 1 < kGrid && layout_.fossil_at(x, y + 1) == id) {
                c.fill({r.x, r.y + r.h, r.w, 2 * gap}, bg);
            }
            if (x + 1 < kGrid && y + 1 < kGrid && layout_.fossil_at(x + 1, y) == id &&
                layout_.fossil_at(x, y + 1) == id && layout_.fossil_at(x + 1, y + 1) == id) {
                c.fill({r.x + r.w, r.y + r.h, 2 * gap, 2 * gap}, bg);
            }
            if (dug) {
                c.line(r.x + 3, r.y + 3, r.x + r.w - 4, r.y + r.h - 4, 2, fg);
                c.line(r.x + r.w - 4, r.y + 3, r.x + 3, r.y + r.h - 4, 2, fg);
            } else {
                bone(c, cx, cy, cell, fg);
            }
        }
    }
    if (show_digs && last_their_x_ >= 0) {
        const Rect r = {x0 + last_their_x_ * cell, y0 + last_their_y_ * cell, cell, cell};
        c.frame(inset(r, -2), 2, B);
    }
}

void Game::draw_target_grid(Canvas &c, bool reveal)
{
    c.fill({kBoardX, kBoardY, kBoardSize, kBoardSize}, B);
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            const int i = y * kGrid + x;
            const Rect r = {kBoardX + x * kCell + 2, kBoardY + y * kCell + 2, kCell - 4, kCell - 4};
            const int cx = r.x + r.w / 2;
            const int cy = r.y + r.h / 2;
            const bool hidden_bone = reveal && peer_layout_.fossil_at(x, y) != kNone;
            switch (view_.marks[i]) {
            case Mark::Unknown:
                if (hidden_bone) {
                    c.fill(r, W);
                    bone(c, cx, cy, kCell, B);
                } else {
                    c.shade(r, Shade::Light);
                }
                break;
            case Mark::Dirt:
                c.fill(r, W);
                hole(c, cx, cy, kCell);
                break;
            case Mark::Bone:
                bone(c, cx, cy, kCell, W);
                break;
            }
            if (x == sel_x_ && y == sel_y_) {
                c.frame(r, 6, B);
                c.frame(inset(r, 6), 3, W);
                if (pending_ != Pending::None) {
                    c.fill(inset(r, 9), B);
                }
            }
        }
    }
    // Radar pings: a dashed outline of the scanned 3x3 area plus the count.
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            const int count = view_.radar[y * kGrid + x];
            if (count < 0) {
                continue;
            }
            const int x0 = std::max(0, x - 1);
            const int y0 = std::max(0, y - 1);
            const int x1 = std::min(kGrid - 1, x + 1);
            const int y1 = std::min(kGrid - 1, y + 1);
            c.dashed_frame({kBoardX + x0 * kCell, kBoardY + y0 * kCell, (x1 - x0 + 1) * kCell,
                            (y1 - y0 + 1) * kCell},
                           8, W);
            const int cx = kBoardX + x * kCell + kCell / 2;
            const int cy = kBoardY + y * kCell + kCell / 2;
            if (view_.marks[y * kGrid + x] == Mark::Unknown) {
                radar_badge(c, cx, cy, count, false);
            } else {
                radar_badge(c, cx + kCell / 2 - 17, cy - kCell / 2 + 17, count, true);
            }
        }
    }
}

void Game::draw_lobby(Canvas &c, uint32_t now)
{
    (void)now;
    c.text(36, 24, "DIG DUEL", font_huge(), B);
    c.text(40, 118, "A fossil hunt for two Stickies", font_medium(), B);
    c.paragraph(40, 172, 440,
                "Bury four fossils in your dig site, then take turns digging up squares of the "
                "other Sticky's site. Find a bone and you dig again. The radar counts the bones "
                "in a 3x3 area. First to unearth all 12 bones wins.",
                font_small(), B, 5);

    // A little sample dig site as decoration.
    const int x0 = 528;
    const int y0 = 40;
    const int cell = 46;
    c.fill({x0, y0, cell * 5, cell * 5}, B);
    const char *art[5] = {"..o..", ".BB.o", "..Bo.", "o..R.", "..o.."};
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            const Rect r = {x0 + x * cell + 2, y0 + y * cell + 2, cell - 4, cell - 4};
            const int cx = r.x + r.w / 2;
            const int cy = r.y + r.h / 2;
            switch (art[y][x]) {
            case 'B':
                bone(c, cx, cy, cell, W);
                break;
            case 'o':
                c.fill(r, W);
                hole(c, cx, cy, cell);
                break;
            case 'R':
                c.shade(r, Shade::Light);
                radar_badge(c, cx, cy, 2, false);
                break;
            default:
                c.shade(r, Shade::Light);
                break;
            }
        }
    }

    char me[16];
    std::snprintf(me, sizeof(me), "STICKY-%02X%02X", self_.b[4], self_.b[5]);
    char line[64];
    std::snprintf(line, sizeof(line), "You are %s", me);
    c.text(40, 330, line, font_small(), B);
    std::snprintf(line, sizeof(line), "Wins %u   Losses %u", stats_.wins, stats_.losses);
    c.text(kPanelRight - 20, 330, line, font_small(), B, Align::Right);

    const Rect box = {36, 368, 728, 96};
    c.round_rect(box, 12, B);
    c.round_rect(inset(box, 3), 10, W);
    char name[16];
    peer_name(name, sizeof(name));
    if (has_choice_) {
        std::snprintf(line, sizeof(line), "Found %s, connecting...", name);
    } else {
        std::snprintf(line, sizeof(line), "Looking for another Sticky...");
    }
    c.text(60, 382, line, font_medium(), B);
    c.text(60, 424,
           notice_[0] != '\0' ? notice_ : "Switch on the other Sticky and keep the two nearby.",
           font_small(), B);
    battery(c, box.x + box.w - 20, 388, platform_.battery_percent());
}

void Game::draw_bury(Canvas &c, uint32_t now)
{
    (void)now;
    draw_home_grid(c, kBoardX, kBoardY, kCell, false, committed_ ? kNone : selected_fossil_);
    draw_header(c);
    c.text(kPanelX, 40, committed_ ? "BURIED" : "BURY", font_large(), B);

    char name[16];
    peer_name(name, sizeof(name));
    char text[160];
    if (committed_) {
        std::snprintf(text, sizeof(text),
                      "Your fossils are hidden. The game starts when %s is ready too.", name);
    } else if (selected_fossil_ != kNone) {
        std::snprintf(text, sizeof(text),
                      "%s picked up. Tap a square to move it, or tap it again to turn it.",
                      fossil_kind(selected_fossil_).name);
    } else {
        std::snprintf(text, sizeof(text),
                      "Tap a fossil to pick it up, then tap a square to move it there.");
    }
    c.paragraph(kPanelX, 102, kPanelW, text, font_small(), B, 3);

    draw_button(c, kRotate, "ROTATE", !committed_ && selected_fossil_ != kNone);
    draw_button(c, kShuffle, "SHUFFLE", !committed_);

    if (lost_contact(now)) {
        std::snprintf(text, sizeof(text), "Lost contact with %s. Bring the Stickies closer.", name);
    } else if (notice_[0] != '\0' && !committed_) {
        std::snprintf(text, sizeof(text), "%s", notice_);
    } else {
        std::snprintf(text, sizeof(text), peer_committed_ ? "%s is ready to dig!"
                                                          : "%s is still burying.", name);
    }
    c.paragraph(kPanelX, 300, kPanelW, text, font_small(), B, 3);
    draw_button(c, kReady, committed_ ? "WAITING..." : "I'M READY", !committed_);
}

void Game::draw_play(Canvas &c, uint32_t now)
{
    draw_target_grid(c, false);
    draw_header(c);
    c.text(kPanelX, 36, my_turn_ ? "YOUR TURN" : "THEIR TURN", font_large(), B);

    char name[16];
    peer_name(name, sizeof(name));
    char text[160];
    if (lost_contact(now)) {
        std::snprintf(text, sizeof(text), "Lost contact with %s. Bring the Stickies closer.", name);
    } else {
        std::snprintf(text, sizeof(text), "%s", status_);
    }
    c.paragraph(kPanelX, 96, kPanelW, text, font_small(), B, 2);

    c.text(kMiniX, kMiniY - 28, "YOUR SITE", font_small(), B);
    draw_home_grid(c, kMiniX, kMiniY, kMiniCell, true, kNone);
    std::snprintf(text, sizeof(text), "%u/%d found", home_.found, kBoneCells);
    c.text(kMiniX, kMiniY + kMiniCell * kGrid + 8, text, font_small(), B);

    const int lx = kPanelX + 166;
    c.text(lx, kMiniY - 28, "THEIRS", font_small(), B);
    for (int id = 0; id < kFossils; ++id) {
        const int y = kMiniY + id * 30;
        const bool done = view_.done_mask & (1U << id);
        const Rect box = {lx, y + 3, 18, 18};
        c.frame(box, 2, B);
        if (done) {
            c.fill(inset(box, 4), B);
        }
        c.text(lx + 28, y, fossil_kind(id).name, font_small(), B);
        if (done) {
            const int w = Canvas::text_width(fossil_kind(id).name, font_small());
            c.fill({lx + 26, y + 12, w + 4, 2}, B);
        }
    }
    std::snprintf(text, sizeof(text), "%u/%d found", view_.found, kBoneCells);
    c.text(lx, kMiniY + kMiniCell * kGrid + 8, text, font_small(), B);

    const bool can_act = my_turn_ && pending_ == Pending::None && sel_x_ >= 0;
    draw_button(c, kDig, "DIG", can_act);
    std::snprintf(text, sizeof(text), "RADAR %u", view_.radars_left);
    draw_button(c, kRadar, text, can_act && view_.radars_left > 0);
}

void Game::draw_over(Canvas &c, uint32_t now)
{
    draw_target_grid(c, peer_revealed_);
    draw_header(c);
    c.text(kPanelX, 36, won_ ? "YOU WIN!" : "YOU LOSE", font_large(), B);

    char name[16];
    peer_name(name, sizeof(name));
    char text[160];
    if (won_) {
        std::snprintf(text, sizeof(text), "You unearthed all %d of their bones in %u digs.",
                      kBoneCells, view_.digs);
    } else {
        std::snprintf(text, sizeof(text), "%s unearthed all %d of your bones first.", name,
                      kBoneCells);
    }
    int y = 96 + c.paragraph(kPanelX, 96, kPanelW, text, font_small(), B, 2) + 12;

    switch (verdict_) {
    case Verdict::Waiting:
        std::snprintf(text, sizeof(text), "Checking their map...");
        break;
    case Verdict::Fair:
        std::snprintf(text, sizeof(text), "Fair game: their map matches every answer they gave.");
        break;
    case Verdict::Mismatch:
        std::snprintf(text, sizeof(text), "Warning: their map does not match their answers!");
        break;
    }
    y += c.paragraph(kPanelX, y, kPanelW, text, font_small(), B, 2) + 12;
    std::snprintf(text, sizeof(text), "Wins %u   Losses %u", stats_.wins, stats_.losses);
    c.text(kPanelX, y, text, font_small(), B);

    if (lost_contact(now)) {
        std::snprintf(text, sizeof(text), "Lost contact with %s.", name);
    } else if (rematch_sent_) {
        std::snprintf(text, sizeof(text), "Waiting for %s...", name);
    } else if (rematch_received_) {
        std::snprintf(text, sizeof(text), "%s wants a rematch!", name);
    } else {
        text[0] = '\0';
    }
    c.paragraph(kPanelX, 318, kPanelW, text, font_small(), B, 2);
    draw_button(c, kRematch, rematch_sent_ ? "WAITING..." : "REMATCH", !rematch_sent_);
}

void Game::render_sleep(Canvas &c) const
{
    c.clear(W);
    c.text(400, 120, "Dig Duel is asleep", font_large(), B, Align::Center);
    c.text(400, 196, "Press the AI button to wake it up.", font_medium(), B, Align::Center);
    char me[16];
    std::snprintf(me, sizeof(me), "STICKY-%02X%02X", self_.b[4], self_.b[5]);
    char line[64];
    std::snprintf(line, sizeof(line), "%s   Wins %u   Losses %u", me, stats_.wins, stats_.losses);
    c.text(400, 250, line, font_small(), B, Align::Center);
    for (int i = 0; i < 5; ++i) {
        bone(c, 240 + i * 80, 360, 56, B);
    }
}
