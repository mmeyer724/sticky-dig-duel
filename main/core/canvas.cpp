#include "canvas.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

const Glyph *glyph_for(const Font &font, char ch)
{
    if (ch < 0x20 || ch > 0x7E) {
        ch = '?';
    }
    return &font.glyphs[ch - 0x20];
}

// 4x4 Bayer thresholds; a pixel is black when its threshold is below the
// shade's level, so the patterns nest and never form solid lines.
constexpr uint8_t kBayer[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

uint8_t shade_level(Shade shade)
{
    switch (shade) {
    case Shade::Light:
        return 3;
    case Shade::Medium:
        return 8;
    case Shade::Dark:
        return 12;
    }
    return 8;
}

}  // namespace

void Canvas::clear(Color color)
{
    std::memset(buffer_, color == Color::White ? 0xFF : 0x00, kBytes);
}

void Canvas::pixel(int x, int y, Color color)
{
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
        return;
    }
    uint8_t &byte = buffer_[y * kStride + x / 8];
    const uint8_t mask = static_cast<uint8_t>(0x80U >> (x & 7));
    if (color == Color::White) {
        byte |= mask;
    } else {
        byte &= static_cast<uint8_t>(~mask);
    }
}

bool Canvas::is_black(int x, int y) const
{
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
        return false;
    }
    return (buffer_[y * kStride + x / 8] & (0x80U >> (x & 7))) == 0;
}

void Canvas::fill(const Rect &r, Color color)
{
    const int x0 = std::max(0, r.x);
    const int y0 = std::max(0, r.y);
    const int x1 = std::min(kWidth, r.x + r.w);
    const int y1 = std::min(kHeight, r.y + r.h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            pixel(x, y, color);
        }
    }
}

void Canvas::shade(const Rect &r, Shade shade)
{
    const uint8_t level = shade_level(shade);
    const int x0 = std::max(0, r.x);
    const int y0 = std::max(0, r.y);
    const int x1 = std::min(kWidth, r.x + r.w);
    const int y1 = std::min(kHeight, r.y + r.h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            pixel(x, y, kBayer[y & 3][x & 3] < level ? Color::Black : Color::White);
        }
    }
}

void Canvas::frame(const Rect &r, int thickness, Color color)
{
    fill({r.x, r.y, r.w, thickness}, color);
    fill({r.x, r.y + r.h - thickness, r.w, thickness}, color);
    fill({r.x, r.y, thickness, r.h}, color);
    fill({r.x + r.w - thickness, r.y, thickness, r.h}, color);
}

void Canvas::dashed_frame(const Rect &r, int dash, Color color)
{
    for (int x = r.x; x < r.x + r.w; ++x) {
        if (((x - r.x) / dash) % 2 == 0) {
            fill({x, r.y, 1, 2}, color);
            fill({x, r.y + r.h - 2, 1, 2}, color);
        }
    }
    for (int y = r.y; y < r.y + r.h; ++y) {
        if (((y - r.y) / dash) % 2 == 0) {
            fill({r.x, y, 2, 1}, color);
            fill({r.x + r.w - 2, y, 2, 1}, color);
        }
    }
}

void Canvas::line(int x0, int y0, int x1, int y1, int thickness, Color color)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    const int half = thickness / 2;
    int error = dx + dy;
    while (true) {
        fill({x0 - half, y0 - half, thickness, thickness}, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void Canvas::disc(int cx, int cy, int radius, Color color)
{
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius + radius) {
                pixel(cx + x, cy + y, color);
            }
        }
    }
}

void Canvas::ring(int cx, int cy, int radius, int thickness, Color color)
{
    const int outer = radius * radius + radius;
    const int inner_r = radius - thickness;
    const int inner = inner_r * inner_r + inner_r;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const int d = x * x + y * y;
            if (d <= outer && d > inner) {
                pixel(cx + x, cy + y, color);
            }
        }
    }
}

void Canvas::round_rect(const Rect &r, int radius, Color color)
{
    fill({r.x + radius, r.y, r.w - 2 * radius, r.h}, color);
    fill({r.x, r.y + radius, radius, r.h - 2 * radius}, color);
    fill({r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius}, color);
    const int corners[4][2] = {
        {r.x + radius, r.y + radius},
        {r.x + r.w - radius - 1, r.y + radius},
        {r.x + radius, r.y + r.h - radius - 1},
        {r.x + r.w - radius - 1, r.y + r.h - radius - 1},
    };
    for (const auto &c : corners) {
        disc(c[0], c[1], radius, color);
    }
}

void Canvas::glyph(int x, int y, const Font &font, char ch, Color color)
{
    const Glyph *g = glyph_for(font, ch);
    const int stride = (g->width + 7) / 8;
    const uint8_t *bits = font.bits + g->offset;
    for (int row = 0; row < font.height; ++row) {
        for (int col = 0; col < g->width; ++col) {
            if (bits[row * stride + col / 8] & (0x80U >> (col & 7))) {
                pixel(x + g->x_offset + col, y + row, color);
            }
        }
    }
}

int Canvas::text_width(const char *text, const Font &font)
{
    int width = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        width += glyph_for(font, *p)->advance;
    }
    return width;
}

int Canvas::text(int x, int y, const char *text, const Font &font, Color color, Align align)
{
    const int width = text_width(text, font);
    if (align == Align::Center) {
        x -= width / 2;
    } else if (align == Align::Right) {
        x -= width;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        glyph(x, y, font, *p, color);
        x += glyph_for(font, *p)->advance;
    }
    return width;
}

int Canvas::paragraph(int x, int y, int max_width, const char *text,
                      const Font &font, Color color, int line_gap)
{
    char line_buf[128];
    int top = y;
    const char *p = text;
    while (*p != '\0') {
        // Take as many whole words as fit on this line.
        size_t fit = 0;
        size_t scan = 0;
        int width = 0;
        while (p[scan] != '\0' && p[scan] != '\n') {
            size_t word_end = scan;
            while (p[word_end] != '\0' && p[word_end] != ' ' && p[word_end] != '\n') {
                ++word_end;
            }
            int word_width = 0;
            for (size_t i = scan; i < word_end; ++i) {
                word_width += glyph_for(font, p[i])->advance;
            }
            const int space = scan > 0 ? glyph_for(font, ' ')->advance : 0;
            if (fit > 0 && width + space + word_width > max_width) {
                break;
            }
            width += space + word_width;
            fit = word_end;
            scan = word_end;
            if (p[scan] == ' ') {
                ++scan;
            }
        }
        if (fit == 0 && p[0] != '\n') {
            fit = 1;  // A single character wider than the box still advances.
        }
        const size_t n = std::min(fit, sizeof(line_buf) - 1);
        std::memcpy(line_buf, p, n);
        line_buf[n] = '\0';
        this->text(x, y, line_buf, font, color);
        y += font.height + line_gap;
        p += fit;
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\n') {
            ++p;
        }
    }
    return y - top;
}
