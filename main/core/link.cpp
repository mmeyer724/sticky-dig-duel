#include "link.h"

#include "wire.h"

namespace {

constexpr uint8_t kMagic0 = 'D';
constexpr uint8_t kMagic1 = 'G';
constexpr uint8_t kVersion = 1;

}  // namespace

Link::Link(Radio &radio, LinkListener &listener, uint32_t session)
    : radio_(radio), listener_(listener), session_(session)
{
}

void Link::bind(const Mac &peer, uint32_t peer_session, uint32_t now)
{
    bound_ = true;
    peer_ = peer;
    peer_session_ = peer_session;
    last_heard_ = now;
    tx_next_ = 0;
    rx_next_ = 0;
    head_ = 0;
    count_ = 0;
    retries_ = 0;
}

void Link::unbind()
{
    bound_ = false;
    peer_ = {};
    peer_session_ = 0;
    count_ = 0;
}

bool Link::send(const uint8_t *body, size_t len, uint32_t now)
{
    if (!bound_ || len > kMaxBody || count_ == kQueueDepth) {
        return false;
    }
    Pending &slot = queue_[(head_ + count_) % kQueueDepth];
    slot.seq = tx_next_++;
    slot.len = static_cast<uint8_t>(len);
    std::memcpy(slot.body, body, len);
    ++count_;
    if (count_ == 1) {
        retries_ = 0;
        transmit_head(now);
    }
    return true;
}

void Link::hello(const uint8_t *body, size_t len)
{
    send_frame(Mac::broadcast(), kHello, 0, 0, body, len);
}

void Link::transmit_head(uint32_t now)
{
    const Pending &head = queue_[head_];
    send_frame(peer_, kData, peer_session_, head.seq, head.body, head.len);
    last_tx_ = now;
}

void Link::send_frame(const Mac &to, Kind kind, uint32_t to_session, uint16_t seq,
                      const uint8_t *body, size_t len)
{
    uint8_t frame[kHeaderBytes + kMaxBody];
    Writer w(frame, sizeof(frame));
    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(kVersion);
    w.u8(kind);
    w.u32(session_);
    w.u32(to_session);
    w.u16(seq);
    w.bytes(body, len);
    if (w.ok()) {
        radio_.send(to, frame, w.size());
    }
}

void Link::receive(const Mac &from, const uint8_t *frame, size_t len, int rssi, uint32_t now)
{
    Reader r(frame, len);
    if (r.u8() != kMagic0 || r.u8() != kMagic1 || r.u8() != kVersion) {
        return;
    }
    const uint8_t kind = r.u8();
    const uint32_t from_session = r.u32();
    const uint32_t to_session = r.u32();
    const uint16_t seq = r.u16();
    if (!r.ok()) {
        return;
    }
    const uint8_t *body = r.cursor();
    const size_t body_len = r.remaining();
    const bool from_peer = bound_ && from == peer_;

    if (kind == kHello) {
        if (from_peer && from_session != peer_session_) {
            listener_.on_peer_restarted(now);
        } else if (from_peer) {
            last_heard_ = now;
        }
        listener_.on_hello(from, from_session, body, body_len, rssi, now);
        return;
    }

    if (!from_peer) {
        return;
    }
    if (from_session != peer_session_) {
        listener_.on_peer_restarted(now);
        return;
    }
    if (to_session != session_) {
        return;  // addressed to this device before it rebooted
    }
    last_heard_ = now;

    if (kind == kData) {
        const int16_t ahead = static_cast<int16_t>(seq - rx_next_);
        if (ahead > 0) {
            return;  // cannot happen with stop-and-wait; drop without acking
        }
        send_frame(peer_, kAck, peer_session_, seq, nullptr, 0);
        if (ahead == 0) {
            ++rx_next_;
            listener_.on_message(body, body_len, now);
        }
    } else if (kind == kAck) {
        if (count_ > 0 && seq == queue_[head_].seq) {
            head_ = (head_ + 1) % kQueueDepth;
            --count_;
            retries_ = 0;
            if (count_ > 0) {
                transmit_head(now);
            }
        }
    }
}

void Link::tick(uint32_t now)
{
    if (!bound_ || count_ == 0) {
        return;
    }
    const uint32_t interval = retries_ < kFastRetries ? kRetryMs : kSlowRetryMs;
    if (now - last_tx_ >= interval) {
        ++retries_;
        transmit_head(now);
    }
}
