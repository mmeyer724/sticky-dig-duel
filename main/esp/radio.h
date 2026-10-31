#pragma once

#include "esp_err.h"
#include "link.h"

using FrameHandler = void (*)(const Mac &from, const uint8_t *data, size_t len, int rssi);

// ESP-NOW over a fixed Wi-Fi channel. No access point or router is involved.
// Received frames are handed to on_frame from the Wi-Fi task.
class EspNowRadio : public Radio {
public:
    static constexpr uint8_t kChannel = 6;

    esp_err_t init(FrameHandler on_frame);
    void send(const Mac &to, const uint8_t *data, size_t len) override;
    const Mac &self() const { return self_; }

private:
    Mac self_ = {};
};
