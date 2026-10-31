// =============================================================================
// session.hpp — BitTorrent download session orchestrator
//
// The Session ties all modules together:
//   - Parses the .torrent via Metainfo
//   - Announces to the tracker to obtain peers
//   - Manages a thread pool, one per peer
//   - Delegates block downloads to PieceManager
//   - Reports progress via callback
//
// Typical usage (in main):
//   bt::Session session("/mnt/usb/file.torrent", "/data/downloads");
//   session.set_progress_callback([](float pct, ...) { /* update UI */ });
//   session.start();         // Blocking until complete or failure
// =============================================================================

#pragma once

#include "metainfo.hpp"
#include "piece_manager.hpp"
#include "tracker.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

// =============================================================================
// Progress information passed to the callback
// =============================================================================

struct ProgressInfo {
    float   pct;            // 0.0 – 100.0
    int64_t bytes_done;     // Confirmed bytes (completed pieces)
    int64_t bytes_total;
    int     peers_active;   // Currently connected peers
    int     seeders;        // From last announce
    int     leechers;
    double  speed_bps;      // Estimated rate in bytes/second
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

// =============================================================================
// Session
// =============================================================================

class Session {
public:
    // Maximum number of simultaneous peers
    static constexpr int MAX_PEERS = 30;

    // Local port (0 = leeching only, no seeding)
    static constexpr uint16_t LOCAL_PORT = 0;

    /**
     * @param torrent_path   Path to the .torrent file
     * @param save_dir       Destination directory for downloaded files
     */
    Session(const std::string& torrent_path, const std::string& save_dir);

    ~Session() { stop(); }

    // Non-copyable, non-movable (contains threads and atomics)
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // -------------------------------------------------------------------------
    // Control
    // -------------------------------------------------------------------------

    /**
     * Register a callback invoked periodically with download progress.
     * Must be called before start().
     */
    void set_progress_callback(ProgressCallback cb);

    /**
     * Start the download.
     * Blocks the calling thread until the download completes, a fatal error
     * occurs, or stop() is called from another thread.
     *
     * @throws std::runtime_error  on unrecoverable failure
     */
    void start();

    /**
     * Signal to stop. Thread-safe.
     * The session sends a "stopped" announce to the tracker before shutting down.
     */
    void stop() noexcept;

    bool is_complete() const noexcept;
    bool is_running()  const noexcept { return running_.load(); }

    // -------------------------------------------------------------------------
    // Loaded torrent info
    // -------------------------------------------------------------------------
    const Metainfo& metainfo() const noexcept { return meta_; }

private:
    Metainfo                        meta_;
    std::string                     save_dir_;
    std::string                     peer_id_;    // Our 20-char ID generated at startup

    std::unique_ptr<PieceManager>   pm_;
    ProgressCallback                progress_cb_;

    std::atomic<bool>               running_{false};
    std::atomic<bool>               stop_requested_{false};

    // State from last announce
    std::mutex          tracker_mutex_;
    TrackerResponse     last_tracker_resp_;

    // Active peer count
    std::atomic<int>    peers_active_{0};

    // Speed estimation
    std::atomic<int64_t> bytes_at_last_check_{0};

    // -------------------------------------------------------------------------
    // Internal steps
    // -------------------------------------------------------------------------

    /**
     * Generate a unique peer_id in the format "-BT0001-XXXXXXXXXXXX"
     * (client prefix + 12 random bytes).
     */
    static std::string generate_peer_id();

    /**
     * Read the .torrent file from the given path.
     */
    static std::string read_file(const std::string& path);

    /**
     * Main loop of a peer worker.
     * Runs in a separate thread for each peer.
     */
    void peer_worker(PeerAddr addr);

    /**
     * Periodic tracker announce thread.
     */
    void tracker_thread();

    /**
     * Progress thread: calls progress_cb_ roughly every 500ms.
     */
    void progress_thread();

    /**
     * Spawn up to MAX_PEERS peer_worker threads.
     * Replaces finished peers with new ones as they become available.
     */
    void run_peer_pool(const std::vector<PeerAddr>& peers);
};

} // namespace bt
