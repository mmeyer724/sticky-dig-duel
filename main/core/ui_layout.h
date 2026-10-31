#pragma once

#include "canvas.h"
#include "rules.h"

// Screen geometry shared by drawing and touch hit-testing.
namespace ui {

constexpr int kBoardX = 20;
constexpr int kBoardY = 23;
constexpr int kCell = 62;
constexpr int kBoardSize = kCell * dig::kGrid;

constexpr int kPanelX = 478;
constexpr int kPanelW = 302;
constexpr int kPanelRight = kPanelX + kPanelW;

constexpr Rect kRotate = {kPanelX, 226, 146, 60};
constexpr Rect kShuffle = {kPanelX + 156, 226, 146, 60};
constexpr Rect kReady = {kPanelX, 380, kPanelW, 80};
constexpr Rect kDig = {kPanelX, 392, 146, 68};
constexpr Rect kRadar = {kPanelX + 156, 392, 146, 68};
constexpr Rect kRematch = {kPanelX, 380, kPanelW, 80};

constexpr int kMiniX = kPanelX;
constexpr int kMiniY = 190;
constexpr int kMiniCell = 20;

// Maps a touch point to a board square of the large grid.
inline bool board_cell(int px, int py, int &x, int &y)
{
    if (px < kBoardX || py < kBoardY || px >= kBoardX + kBoardSize ||
        py >= kBoardY + kBoardSize) {
        return false;
    }
    x = (px - kBoardX) / kCell;
    y = (py - kBoardY) / kCell;
    return true;
}

// Buttons accept touches slightly outside their drawn edge.
inline bool hit(const Rect &r, int px, int py)
{
    constexpr int kSlop = 8;
    return Rect{r.x - kSlop, r.y - kSlop, r.w + 2 * kSlop, r.h + 2 * kSlop}.contains(px, py);
}

}  // namespace ui
