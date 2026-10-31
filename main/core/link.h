#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

struct Mac {
    uint8_t b[6];

    bool operator==(const Mac &o) const { return std::memcmp(b, o.b, 6) == 0; }
    bool operator!=(const Mac &o) const { return !(*this == o); }
    bool operator<(const Mac &o) const { return std::memcmp(b, o.b, 6) < 0; }
    bool is_zero() const { return *this == Mac{}; }
    static Mac broadcast() { return Mac{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}; }
};

// Datagram transport underneath the link (ESP-NOW on the device).
class Radio {
public:
    virtual ~Radio() = default;
    virtual void send(const Mac &to, const uint8_t *data, size_t len) = 0;
};

class LinkListener {
public:
    virtual ~LinkListener() = default;
    // A broadcast presence beacon from any device running this game.
    virtual void on_hello(const Mac &from, uint32_t session, const uint8_t *body,
                          size_t len, int rssi, uint32_t now) = 0;
    // A reliable message from the bound peer, delivered once and in order.
    virtual void on_message(const uint8_t *body, size_t len, uint32_t now) = 0;
    // The bound peer is sending with a new session id: it rebooted.
    virtual void on_peer_restarted(uint32_t now) = 0;
};

// Reliable, ordered messaging with one bound peer over a lossy datagram radio,
// plus unreliable broadcast beacons. Stop-and-wait: one message in flight,
// retransmitted until acknowledged. Every frame carries the sender's random
// per-boot session id so that a rebooted device's stale frames are ignored.
class Link {
public:
    static constexpr size_t kHeaderBytes = 14;
    static constexpr size_t kMaxBody = 200;
    static constexpr size_t kQueueDepth = 8;
    static constexpr uint32_t kRetryMs = 150;
    static constexpr uint32_t kSlowRetryMs = 1000;
    static constexpr int kFastRetries = 20;

    Link(Radio &radio, LinkListener &listener, uint32_t session);

    void bind(const Mac &peer, uint32_t peer_session, uint32_t now);
    void unbind();
    bool bound() const { return bound_; }
    const Mac &peer() const { return peer_; }
    uint32_t peer_session() const { return peer_session_; }
    uint32_t session() const { return session_; }
    uint32_t last_heard() const { return last_heard_; }
    bool sending() const { return count_ > 0; }

    // Queues a message for the bound peer. False if unbound or the queue is full.
    bool send(const uint8_t *body, size_t len, uint32_t now);
    void hello(const uint8_t *body, size_t len);

    void receive(const Mac &from, const uint8_t *frame, size_t len, int rssi, uint32_t now);
    void tick(uint32_t now);

private:
    enum Kind : uint8_t { kHello = 1, kData = 2, kAck = 3 };

    struct Pending {
        uint16_t seq;
        uint8_t len;
        uint8_t body[kMaxBody];
    };

    void transmit_head(uint32_t now);
    void send_frame(const Mac &to, Kind kind, uint32_t to_session, uint16_t seq,
                    const uint8_t *body, size_t len);

    Radio &radio_;
    LinkListener &listener_;
    uint32_t session_;

    bool bound_ = false;
    Mac peer_ = {};
    uint32_t peer_session_ = 0;
    uint32_t last_heard_ = 0;

    uint16_t tx_next_ = 0;
    uint16_t rx_next_ = 0;
    Pending queue_[kQueueDepth] = {};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t last_tx_ = 0;
    int retries_ = 0;
};
