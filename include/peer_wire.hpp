// =============================================================================
// peer_wire.hpp — BitTorrent wire protocol (BEP 3)
//
// Manages a TCP connection with a peer, including:
//   - Initial handshake
//   - Sending and receiving typed messages
//   - Choke/unchoke/interested state tracking
//   - Block requests (request/piece)
//
// Each PeerConnection instance represents ONE connection to ONE peer.
// For multiple peers, create multiple instances (usually in threads).
// =============================================================================

#pragma once

#include "sha1.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// =============================================================================
// BitTorrent protocol message types (BEP 3)
// =============================================================================

enum class MsgType : uint8_t {
    Choke         = 0,
    Unchoke       = 1,
    Interested    = 2,
    NotInterested = 3,
    Have          = 4,
    Bitfield      = 5,
    Request       = 6,
    Piece         = 7,
    Cancel        = 8,
    // Extensions (not implemented yet):
    // Port       = 9,   // DHT (BEP 5)
};

/**
 * Message received from a peer, already deserialized.
 *
 * Valid fields depend on the type:
 *   Have         → piece_index
 *   Bitfield     → bitfield
 *   Request      → piece_index, begin, length
 *   Piece        → piece_index, begin, block
 *   Cancel       → piece_index, begin, length
 *   Others       → (no payload)
 */
struct PeerMessage {
    MsgType  type;

    uint32_t piece_index = 0;
    uint32_t begin       = 0;
    uint32_t length      = 0;

    std::vector<uint8_t> bitfield;  // For Bitfield
    std::vector<uint8_t> block;     // For Piece (actual data)
};

// =============================================================================
// Local peer state (choke/interest flags)
// =============================================================================

struct PeerState {
    bool am_choking      = true;   // We are choking the peer
    bool am_interested   = false;  // We are interested in the peer
    bool peer_choking    = true;   // The peer is choking us
    bool peer_interested = false;  // The peer is interested in us
};

// =============================================================================
// PeerConnection
// =============================================================================

/**
 * Connection to a single BitTorrent peer.
 *
 * Typical lifecycle:
 *   1. connect(ip, port)
 *   2. handshake(info_hash, peer_id)  → returns false on info_hash mismatch
 *   3. send_interested()
 *   4. Loop: read_message() → process → send_request() → read_message() ...
 *   5. disconnect()
 */
class PeerConnection {
public:
    // Default block size: 16 KiB (standard BitTorrent spec)
    static constexpr uint32_t BLOCK_SIZE = 16 * 1024;

    PeerConnection() = default;
    ~PeerConnection() { disconnect(); }

    // Non-copyable (owns socket fd)
    PeerConnection(const PeerConnection&)            = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    // Movable
    PeerConnection(PeerConnection&& o) noexcept
        : fd_(o.fd_), state_(o.state_), peer_id_(std::move(o.peer_id_))
    {
        o.fd_ = -1;
    }

    // -------------------------------------------------------------------------
    // Connection management
    // -------------------------------------------------------------------------

    /**
     * Connect to the peer (TCP).
     * ip in network byte order (as returned by the tracker).
     * Throws std::runtime_error on failure.
     */
    void connect(uint32_t ip, uint16_t port);

    /**
     * Perform the BitTorrent handshake.
     * Sends our handshake and validates the peer's.
     *
     * @return true  — handshake OK, info_hash matches
     * @return false — info_hash mismatch (discard this connection)
     */
    bool handshake(const SHA1::Digest& info_hash, const std::string& our_peer_id);

    void disconnect() noexcept;

    bool is_connected() const noexcept { return fd_ >= 0; }

    // -------------------------------------------------------------------------
    // Reading messages
    // -------------------------------------------------------------------------

    /**
     * Read the next message from the peer.
     *
     * @return std::nullopt  — keep-alive (zero-length message)
     * @return PeerMessage   — received and deserialized message
     * @throws std::runtime_error on protocol or socket error
     */
    std::optional<PeerMessage> read_message();

    // -------------------------------------------------------------------------
    // Sending messages (self-explanatory names)
    // -------------------------------------------------------------------------

    void send_keepalive();
    void send_choke();
    void send_unchoke();
    void send_interested();
    void send_not_interested();
    void send_have(uint32_t piece_index);
    void send_bitfield(const std::vector<uint8_t>& bitfield);

    /**
     * Request a block of data from the peer.
     *
     * @param piece_index  Piece index (0-based)
     * @param begin        Byte offset within the piece
     * @param length       Block size (typically BLOCK_SIZE, except the last block)
     */
    void send_request(uint32_t piece_index, uint32_t begin, uint32_t length);
    void send_cancel (uint32_t piece_index, uint32_t begin, uint32_t length);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    const PeerState& state()   const noexcept { return state_;   }
    const std::string& peer_id() const noexcept { return peer_id_; }

private:
    int         fd_      = -1;
    PeerState   state_;
    std::string peer_id_;  // 20-byte peer ID of the remote peer (from handshake)

    // -------------------------------------------------------------------------
    // Low-level I/O
    // -------------------------------------------------------------------------

    // Send exactly `len` bytes (loop over send)
    void send_raw(const void* data, size_t len);

    // Receive exactly `len` bytes (loop over recv)
    void recv_raw(void*  data, size_t len);

    // Send message with length prefix (4 bytes BE) + type byte + payload
    void send_msg(MsgType type, const uint8_t* payload = nullptr, size_t payload_len = 0);

    // Read uint32_t big-endian from socket
    uint32_t recv_u32();

    // Write uint32_t big-endian to buffer
    static void write_u32(uint8_t* buf, uint32_t v) noexcept;
};

} // namespace bt
