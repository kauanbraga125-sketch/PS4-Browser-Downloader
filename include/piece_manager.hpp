// =============================================================================
// piece_manager.hpp — Torrent piece manager
//
// Responsibilities:
//   - Track which pieces have been downloaded and verified
//   - Verify the integrity of each piece via SHA1
//   - Build the bitfield to announce to peers
//   - Select the next piece/block to request (simple rarest-first strategy)
//   - Write received blocks to the destination file
//
// Thread-safety:
//   All public methods are protected by an internal mutex.
//   Safe to call from multiple peer threads simultaneously.
// =============================================================================

#pragma once

#include "metainfo.hpp"
#include "sha1.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// =============================================================================
// Individual piece state
// =============================================================================

enum class PieceStatus : uint8_t {
    Missing    = 0,   // Not owned and not being downloaded
    Pending    = 1,   // Blocks currently being downloaded
    Complete   = 2,   // All blocks received and SHA1 verified
    HashFail   = 3,   // Received but SHA1 mismatch — will be re-downloaded
};

/**
 * Represents a piece being downloaded.
 * Each piece is divided into 16 KiB blocks (BLOCK_SIZE).
 */
struct Piece {
    uint32_t              index;
    PieceStatus           status;
    std::vector<uint8_t>  data;              // Buffer accumulating blocks
    std::vector<bool>     blocks_received;   // Bitset: has block i arrived?
    uint32_t              blocks_done;       // Counter to avoid verifying every loop

    explicit Piece(uint32_t idx, int64_t piece_len, size_t num_blocks)
        : index(idx)
        , status(PieceStatus::Missing)
        , data(static_cast<size_t>(piece_len), 0)
        , blocks_received(num_blocks, false)
        , blocks_done(0)
    {}
};

// =============================================================================
// Block request — smallest download unit
// =============================================================================

struct BlockRequest {
    uint32_t piece_index;
    uint32_t begin;    // Byte offset within the piece
    uint32_t length;   // Block size (≤ 16 KiB)
};

// =============================================================================
// PieceManager
// =============================================================================

class PieceManager {
public:
    /**
     * @param meta      Torrent metainfo
     * @param save_dir  Directory where files will be written
     *                  (e.g. "/data/pkg" on a jailbroken PS4)
     */
    PieceManager(const Metainfo& meta, const std::string& save_dir);

    // -------------------------------------------------------------------------
    // Main interface (thread-safe)
    // -------------------------------------------------------------------------

    /**
     * Receive a data block from a peer.
     * If the piece completes, verify SHA1 and flush to disk.
     *
     * @return true   — piece complete and verified successfully
     * @return false  — block stored but piece still incomplete, or SHA1 failed
     */
    bool receive_block(uint32_t piece_index, uint32_t begin,
                       const std::vector<uint8_t>& data);

    /**
     * Select the next block to request for a specific peer.
     * Uses a simple strategy: first Missing piece, first missing block.
     *
     * @param peer_bitfield   The peer's bitfield (which pieces it has)
     * @return BlockRequest   — block to request
     * @return std::nullopt   — nothing to request (download complete or peer has nothing useful)
     */
    std::optional<BlockRequest> next_request(const std::vector<uint8_t>& peer_bitfield) const;

    /**
     * Return our current bitfield (to send to the peer during handshake).
     * 1 bit per piece, MSB-first order.
     */
    std::vector<uint8_t> our_bitfield() const;

    // -------------------------------------------------------------------------
    // Progress
    // -------------------------------------------------------------------------

    size_t  pieces_total()    const noexcept { return pieces_.size(); }
    size_t  pieces_done()     const noexcept { return done_count_.load(); }
    bool    is_complete()     const noexcept { return done_count_ == pieces_.size(); }

    int64_t bytes_downloaded() const noexcept;
    int64_t bytes_total()      const noexcept { return meta_.total_length; }

    /** Percentage from 0.0 to 100.0 */
    float   progress_pct()     const noexcept;

private:
    const Metainfo&           meta_;
    std::string               save_dir_;
    std::vector<Piece>        pieces_;
    std::atomic<size_t>       done_count_{0};

    mutable std::mutex        mutex_;   // Protects pieces_

    // -------------------------------------------------------------------------
    // Internal helpers (called with mutex_ locked)
    // -------------------------------------------------------------------------

    /**
     * Verify SHA1 of a complete piece and, if OK, flush to disk.
     * On SHA1 failure, marks the piece as HashFail for re-download.
     */
    bool verify_and_flush(Piece& p);

    /**
     * Write the bytes of a piece to the correct files.
     * Handles pieces that cross file boundaries (multi-file mode).
     */
    void write_piece(uint32_t piece_index, const std::vector<uint8_t>& data);

    /**
     * Create the necessary directories and pre-allocate files.
     * Called from the constructor.
     */
    void allocate_files();

    /**
     * Check whether the peer has the piece at `index` based on its bitfield.
     */
    static bool peer_has_piece(const std::vector<uint8_t>& bitfield, uint32_t index) noexcept;
};

} // namespace bt
