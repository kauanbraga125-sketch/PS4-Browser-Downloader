// =============================================================================
// peer_wire.cpp — BitTorrent wire protocol implementation (BEP 3)
// =============================================================================

#include "peer_wire.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace bt {

// =============================================================================
// Internal helpers
// =============================================================================

// Read uint32_t big-endian from a byte pointer
static uint32_t read_u32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
           (static_cast<uint32_t>(p[3]) <<  0);
}

void PeerConnection::write_u32(uint8_t* buf, uint32_t v) noexcept {
    buf[0] = (v >> 24) & 0xFF;
    buf[1] = (v >> 16) & 0xFF;
    buf[2] = (v >>  8) & 0xFF;
    buf[3] = (v >>  0) & 0xFF;
}

// =============================================================================
// Low-level I/O
// =============================================================================

void PeerConnection::send_raw(const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t sent = ::send(fd_, ptr, len, 0);
        if (sent <= 0)
            throw std::runtime_error("peer_wire: send() failed");
        ptr += sent;
        len -= static_cast<size_t>(sent);
    }
}

void PeerConnection::recv_raw(void* data, size_t len) {
    char* ptr = static_cast<char*>(data);
    while (len > 0) {
        ssize_t got = ::recv(fd_, ptr, len, 0);
        if (got <= 0)
            throw std::runtime_error("peer_wire: recv() failed — peer disconnected");
        ptr += got;
        len -= static_cast<size_t>(got);
    }
}

uint32_t PeerConnection::recv_u32() {
    uint8_t buf[4];
    recv_raw(buf, 4);
    return read_u32(buf);
}

// =============================================================================
// Connection
// =============================================================================

void PeerConnection::connect(uint32_t ip, uint16_t port) {
    if (fd_ >= 0) disconnect();

    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ < 0)
        throw std::runtime_error("peer_wire: socket() failed");

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ip;            // Already in network byte order
    addr.sin_port        = htons(port);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("peer_wire: connect() failed");
    }
}

void PeerConnection::disconnect() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    // Reset state
    state_   = PeerState{};
    peer_id_ = {};
}

// =============================================================================
// Handshake (BEP 3)
//
// Format:
//   1 byte   — protocol name length (always 19)
//   19 bytes — "BitTorrent protocol"
//   8 bytes  — reserved (zeros; extensions go here in BEP 10)
//   20 bytes — info_hash
//   20 bytes — peer_id
//   TOTAL: 68 bytes
// =============================================================================

bool PeerConnection::handshake(const SHA1::Digest& info_hash, const std::string& our_peer_id) {
    static constexpr char PROTO[]    = "BitTorrent protocol";
    static constexpr uint8_t PSTRLEN = 19;

    // Build outgoing handshake (68 bytes)
    uint8_t hs[68]{};
    hs[0] = PSTRLEN;
    std::memcpy(hs + 1,  PROTO,               PSTRLEN);
    // hs[20..27] = reserved (zeros — no extensions for now)
    std::memcpy(hs + 28, info_hash.data(),    20);
    std::memcpy(hs + 48, our_peer_id.data(),  std::min(our_peer_id.size(), size_t(20)));

    send_raw(hs, sizeof(hs));

    // Read the peer's handshake
    uint8_t peer_hs[68]{};
    recv_raw(peer_hs, sizeof(peer_hs));

    // Validate protocol string
    if (peer_hs[0] != PSTRLEN || std::memcmp(peer_hs + 1, PROTO, PSTRLEN) != 0)
        return false;

    // Validate info_hash — must match ours
    if (std::memcmp(peer_hs + 28, info_hash.data(), 20) != 0)
        return false;

    // Save the remote peer_id
    peer_id_.assign(reinterpret_cast<char*>(peer_hs + 48), 20);

    return true;
}

// =============================================================================
// Sending messages
// =============================================================================

void PeerConnection::send_msg(MsgType type, const uint8_t* payload, size_t payload_len) {
    // Length = 1 byte (type) + payload
    uint32_t msg_len = static_cast<uint32_t>(1 + payload_len);
    uint8_t  header[5];
    write_u32(header, msg_len);
    header[4] = static_cast<uint8_t>(type);

    send_raw(header, 5);
    if (payload && payload_len > 0) send_raw(payload, payload_len);
}

void PeerConnection::send_keepalive() {
    // Keep-alive: zero length, no type byte
    uint8_t zero[4] = {0, 0, 0, 0};
    send_raw(zero, 4);
}

void PeerConnection::send_choke() {
    send_msg(MsgType::Choke);
    state_.am_choking = true;
}

void PeerConnection::send_unchoke() {
    send_msg(MsgType::Unchoke);
    state_.am_choking = false;
}

void PeerConnection::send_interested() {
    send_msg(MsgType::Interested);
    state_.am_interested = true;
}

void PeerConnection::send_not_interested() {
    send_msg(MsgType::NotInterested);
    state_.am_interested = false;
}

void PeerConnection::send_have(uint32_t piece_index) {
    uint8_t payload[4];
    write_u32(payload, piece_index);
    send_msg(MsgType::Have, payload, 4);
}

void PeerConnection::send_bitfield(const std::vector<uint8_t>& bitfield) {
    send_msg(MsgType::Bitfield, bitfield.data(), bitfield.size());
}

void PeerConnection::send_request(uint32_t piece_index, uint32_t begin, uint32_t length) {
    uint8_t payload[12];
    write_u32(payload + 0, piece_index);
    write_u32(payload + 4, begin);
    write_u32(payload + 8, length);
    send_msg(MsgType::Request, payload, 12);
}

void PeerConnection::send_cancel(uint32_t piece_index, uint32_t begin, uint32_t length) {
    uint8_t payload[12];
    write_u32(payload + 0, piece_index);
    write_u32(payload + 4, begin);
    write_u32(payload + 8, length);
    send_msg(MsgType::Cancel, payload, 12);
}

// =============================================================================
// Reading messages
// =============================================================================

std::optional<PeerMessage> PeerConnection::read_message() {
    // Read length (4 bytes big-endian)
    uint32_t msg_len = recv_u32();

    // Keep-alive
    if (msg_len == 0) return std::nullopt;

    // Read type byte
    uint8_t type_byte;
    recv_raw(&type_byte, 1);
    uint32_t payload_len = msg_len - 1;

    PeerMessage msg;
    msg.type = static_cast<MsgType>(type_byte);

    switch (msg.type) {

    case MsgType::Choke:
        state_.peer_choking = true;
        break;

    case MsgType::Unchoke:
        state_.peer_choking = false;
        break;

    case MsgType::Interested:
        state_.peer_interested = true;
        break;

    case MsgType::NotInterested:
        state_.peer_interested = false;
        break;

    case MsgType::Have: {
        if (payload_len != 4)
            throw std::runtime_error("peer_wire: Have with invalid payload");
        uint8_t buf[4];
        recv_raw(buf, 4);
        msg.piece_index = read_u32(buf);
        break;
    }

    case MsgType::Bitfield: {
        msg.bitfield.resize(payload_len);
        recv_raw(msg.bitfield.data(), payload_len);
        break;
    }

    case MsgType::Request:
    case MsgType::Cancel: {
        if (payload_len != 12)
            throw std::runtime_error("peer_wire: Request/Cancel with invalid payload");
        uint8_t buf[12];
        recv_raw(buf, 12);
        msg.piece_index = read_u32(buf + 0);
        msg.begin       = read_u32(buf + 4);
        msg.length      = read_u32(buf + 8);
        break;
    }

    case MsgType::Piece: {
        // Payload: 4 bytes index + 4 bytes begin + N data bytes
        if (payload_len < 8)
            throw std::runtime_error("peer_wire: Piece with invalid payload");
        uint8_t header_buf[8];
        recv_raw(header_buf, 8);
        msg.piece_index = read_u32(header_buf + 0);
        msg.begin       = read_u32(header_buf + 4);

        uint32_t block_len = payload_len - 8;
        msg.block.resize(block_len);
        recv_raw(msg.block.data(), block_len);
        break;
    }

    default: {
        // Unknown message — discard payload to stay in sync
        std::vector<uint8_t> discard(payload_len);
        if (payload_len > 0) recv_raw(discard.data(), payload_len);
        // Return the message regardless (caller decides what to do)
        break;
    }
    }

    return msg;
}

} // namespace bt
