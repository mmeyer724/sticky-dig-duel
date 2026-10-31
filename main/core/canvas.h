#pragma once

#include <cstddef>
#include <cstdint>

// One proportional glyph inside a Font bitmap. Rows are packed MSB-first,
// (width + 7) / 8 bytes per row, Font::height rows per glyph.
struct Glyph {
    uint32_t offset;
    uint8_t width;
    uint8_t advance;
    int8_t x_offset;
};

struct Font {
    uint8_t height;
    uint8_t ascent;
    const Glyph *glyphs;  // printable ASCII, 0x20 through 0x7E
    const uint8_t *bits;
};

// Inter at 20, 28, 44 and 72 px (see tools/gen_fonts.py).
const Font &font_small();
const Font &font_medium();
const Font &font_large();
const Font &font_huge();

enum class Color : uint8_t { Black = 0, White = 1 };

// Ordered-dither fills that read as gray on a black-and-white panel.
enum class Shade : uint8_t { Light, Medium, Dark };

enum class Align : uint8_t { Left, Center, Right };

struct Rect {
    int x;
    int y;
    int w;
    int h;

    bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

// 800 x 480, 1 bit per pixel, 1 = white. Coordinates are the physical
// landscape screen with the origin at the top-left corner.
class Canvas {
public:
    static constexpr int kWidth = 800;
    static constexpr int kHeight = 480;
    static constexpr int kStride = kWidth / 8;
    static constexpr size_t kBytes = static_cast<size_t>(kStride) * kHeight;

    explicit Canvas(uint8_t *buffer) : buffer_(buffer) {}

    uint8_t *data() { return buffer_; }
    const uint8_t *data() const { return buffer_; }

    void clear(Color color = Color::White);
    void pixel(int x, int y, Color color);
    bool is_black(int x, int y) const;

    void fill(const Rect &r, Color color);
    void shade(const Rect &r, Shade shade);
    void frame(const Rect &r, int thickness, Color color);
    void dashed_frame(const Rect &r, int dash, Color color);
    void line(int x0, int y0, int x1, int y1, int thickness, Color color);
    void disc(int cx, int cy, int radius, Color color);
    void ring(int cx, int cy, int radius, int thickness, Color color);
    void round_rect(const Rect &r, int radius, Color color);

    // Draws text with its top edge at y. Returns the advance width.
    int text(int x, int y, const char *text, const Font &font, Color color,
             Align align = Align::Left);
    static int text_width(const char *text, const Font &font);

    // Word-wraps text into max_width. Returns the height used.
    int paragraph(int x, int y, int max_width, const char *text,
                  const Font &font, Color color, int line_gap = 2);

private:
    void glyph(int x, int y, const Font &font, char ch, Color color);

    uint8_t *buffer_;
};
