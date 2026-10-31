// =============================================================================
// tracker.hpp — BitTorrent tracker HTTP client (BEP 3)
//
// Implemented directly over BSD sockets (no libcurl) to minimize
// dependencies in the OpenOrbis/PS4 environment.
//
// Limitations in this version:
//   - HTTP only (no HTTPS) — PS4 has SceSsl but it adds complexity
//   - Compact peer format only (compact=1), standard in practice
//   - UDP trackers (BEP 15) not implemented yet
// =============================================================================

#pragma once

#include "metainfo.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bt {

/**
 * IPv4 address of a peer returned by the tracker.
 */
struct PeerAddr {
    uint32_t ip;    // Address in network byte order (big-endian)
    uint16_t port;  // Port in host byte order

    // Returns IP as a "x.x.x.x" string
    std::string ip_str() const;
};

/**
 * Parsed tracker response.
 */
struct TrackerResponse {
    int32_t               interval     = 1800; // Seconds until next announce
    int32_t               min_interval = 0;    // Minimum interval (optional)
    int32_t               complete     = 0;    // Seeders
    int32_t               incomplete   = 0;    // Leechers
    std::vector<PeerAddr> peers;               // Peer list

    std::string failure;  // Non-empty on tracker error

    bool ok() const noexcept { return failure.empty(); }
};

// =============================================================================
// Public API
// =============================================================================

/**
 * Perform an HTTP GET to the tracker and return the parsed response.
 *
 * @param metainfo    Torrent metainfo (for info_hash and announce URL)
 * @param peer_id     Our 20-byte client identifier (generated in the session)
 * @param port        Port we are listening on (0 = leeching only)
 * @param uploaded    Bytes uploaded so far
 * @param downloaded  Bytes downloaded so far
 * @param left        Bytes remaining until download complete
 * @param event       "started" | "stopped" | "completed" | "" (regular)
 *
 * @throws std::runtime_error  on network failure or invalid response
 */
TrackerResponse tracker_announce(
    const Metainfo&    metainfo,
    const std::string& peer_id,
    uint16_t           port,
    int64_t            uploaded,
    int64_t            downloaded,
    int64_t            left,
    const std::string& event = "started"
);

/**
 * Encode raw bytes for percent-encoding (RFC 3986).
 * All bytes outside [A-Za-z0-9\-_.~] are encoded as %XX.
 */
std::string url_encode(std::string_view raw);

} // namespace bt
