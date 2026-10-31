#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Little-endian packing for radio frames, independent of struct layout.
class Writer {
public:
    Writer(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    void u8(uint8_t v)
    {
        if (len_ < capacity_) {
            buffer_[len_] = v;
        }
        ++len_;
    }
    void u16(uint16_t v)
    {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v)
    {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void bytes(const uint8_t *data, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            u8(data[i]);
        }
    }

    size_t size() const { return len_; }
    bool ok() const { return len_ <= capacity_; }

private:
    uint8_t *buffer_;
    size_t capacity_;
    size_t len_ = 0;
};

class Reader {
public:
    Reader(const uint8_t *data, size_t len) : data_(data), len_(len) {}

    uint8_t u8()
    {
        if (pos_ >= len_) {
            failed_ = true;
            return 0;
        }
        return data_[pos_++];
    }
    uint16_t u16()
    {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (u8() << 8));
    }
    uint32_t u32()
    {
        const uint32_t lo = u16();
        return lo | (static_cast<uint32_t>(u16()) << 16);
    }
    void bytes(uint8_t *out, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            out[i] = u8();
        }
    }

    bool ok() const { return !failed_; }
    size_t remaining() const { return pos_ < len_ ? len_ - pos_ : 0; }
    const uint8_t *cursor() const { return data_ + pos_; }

private:
    const uint8_t *data_;
    size_t len_;
    size_t pos_ = 0;
    bool failed_ = false;
};
