// =============================================================================
// metainfo.hpp — .torrent file parser (BEP 3 + BEP 12)
//
// Supports:
//   - Single-file and multi-file torrents
//   - announce and announce-list (multi-tracker)
//   - Automatic info_hash calculation (SHA1 of the raw info dict)
// =============================================================================

#pragma once

#include "bencode.hpp"
#include "sha1.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// =============================================================================
// Data structures
// =============================================================================

/**
 * A file entry within a multi-file torrent.
 *
 * Example torrent structure:
 *   MyAlbum/
 *     track01.flac → path={"MyAlbum","track01.flac"}, length=...
 *     track02.flac → path={"MyAlbum","track02.flac"}, length=...
 */
struct FileEntry {
    std::vector<std::string> path;    // Path components (no separator)
    int64_t                  length;  // Size in bytes

    // Accumulated offset from the start of the torrent (filled in by the parser)
    int64_t offset = 0;

    // Returns the full path as a string with '/' separator
    std::string path_str() const;
};

/**
 * Complete metainfo of a .torrent file.
 *
 * The info_hash is computed internally during parse() and is the unique
 * identifier of the torrent on the BitTorrent network.
 */
struct Metainfo {
    // -------------------------------------------------------------------------
    // Torrent fields
    // -------------------------------------------------------------------------
    std::string announce;                               // Main tracker URL
    std::vector<std::vector<std::string>> announce_list; // BEP 12: tracker list

    std::string name;          // Suggested file or directory name
    int64_t     piece_length;  // Size of each piece in bytes (e.g. 262144 = 256 KiB)
    int64_t     total_length;  // Sum of all file sizes

    // SHA1 hashes for each piece (20 bytes each)
    // piece_hashes[i] = SHA1 of the i-th piece's bytes
    std::vector<SHA1::Digest> piece_hashes;

    // File list. Single-file torrents have exactly one entry with an empty path
    // (the `name` field is used as the filename).
    std::vector<FileEntry> files;

    // SHA1 of the raw bencoded info dict — the torrent's identifier on the network
    SHA1::Digest info_hash;

    // -------------------------------------------------------------------------
    // Convenience queries
    // -------------------------------------------------------------------------

    bool is_single_file() const noexcept {
        return files.size() == 1 && files[0].path.empty();
    }

    size_t num_pieces() const noexcept {
        return piece_hashes.size();
    }

    /**
     * Returns the actual size of piece at `index`.
     * The last piece may be smaller than piece_length.
     */
    int64_t piece_size(size_t index) const noexcept {
        if (index + 1 < num_pieces()) return piece_length;
        return total_length - static_cast<int64_t>(index) * piece_length;
    }

    /**
     * Number of 16 KiB blocks in the piece at `index`.
     * The last piece (and its last block) may be smaller.
     */
    size_t num_blocks_in_piece(size_t index) const noexcept {
        static constexpr int64_t BLOCK_SIZE = 16 * 1024;
        int64_t ps = piece_size(index);
        return static_cast<size_t>((ps + BLOCK_SIZE - 1) / BLOCK_SIZE);
    }

    // -------------------------------------------------------------------------
    // Factory
    // -------------------------------------------------------------------------

    /**
     * Parse a .torrent file read as a byte string.
     *
     * @param raw   Full .torrent file contents (binary bytes)
     * @throws std::runtime_error   if the file is malformed
     */
    static Metainfo parse(const std::string& raw);
};

} // namespace bt
