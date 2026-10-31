#pragma once

#include <cstddef>
#include <cstdint>

// Minimal SHA-256 (FIPS 180-4), used to commit to a hidden layout before play.
class Sha256 {
public:
    Sha256();
    void update(const uint8_t *data, size_t len);
    void finish(uint8_t digest[32]);

private:
    void block(const uint8_t *chunk);

    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    uint64_t total_ = 0;
};
