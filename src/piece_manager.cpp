// =============================================================================
// piece_manager.cpp — Piece manager implementation
// =============================================================================

#include "piece_manager.hpp"

#include <dirent.h>     // opendir, mkdir
#include <fcntl.h>      // open, O_CREAT, O_WRONLY
#include <sys/stat.h>   // stat, mkdir
#include <unistd.h>     // write, close, lseek

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bt {

// =============================================================================
// Constants
// =============================================================================

static constexpr uint32_t BLOCK_SIZE = 16 * 1024; // 16 KiB

// =============================================================================
// Constructor
// =============================================================================

PieceManager::PieceManager(const Metainfo& meta, const std::string& save_dir)
    : meta_(meta)
    , save_dir_(save_dir)
{
    // Pre-allocate the structure for each piece
    pieces_.reserve(meta.num_pieces());

    for (size_t i = 0; i < meta.num_pieces(); ++i) {
        int64_t  ps         = meta.piece_size(i);
        size_t   num_blocks = meta.num_blocks_in_piece(i);
        pieces_.emplace_back(static_cast<uint32_t>(i), ps, num_blocks);
    }

    // Create directories and pre-allocate files on the filesystem
    allocate_files();
}

// =============================================================================
// receive_block
// =============================================================================

bool PieceManager::receive_block(
    uint32_t piece_index,
    uint32_t begin,
    const std::vector<uint8_t>& data)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (piece_index >= pieces_.size())
        return false;

    Piece& p = pieces_[piece_index];

    // Ignore blocks for pieces already complete or being reprocessed after hash failure
    if (p.status == PieceStatus::Complete) return false;

    // Validate offset and size
    if (begin + data.size() > p.data.size())
        return false;

    // Determine the block index
    uint32_t block_idx = begin / BLOCK_SIZE;
    if (block_idx >= p.blocks_received.size())
        return false;

    // Copy the block into the piece buffer
    std::memcpy(p.data.data() + begin, data.data(), data.size());

    // Mark the block as received (only count once)
    if (!p.blocks_received[block_idx]) {
        p.blocks_received[block_idx] = true;
        ++p.blocks_done;
        p.status = PieceStatus::Pending;
    }

    // Check if all blocks have arrived
    if (p.blocks_done == p.blocks_received.size()) {
        return verify_and_flush(p);
    }

    return false;
}

// =============================================================================
// verify_and_flush
// =============================================================================

bool PieceManager::verify_and_flush(Piece& p) {
    // Compute SHA1 of the accumulated data
    SHA1::Digest actual = SHA1::hash(p.data.data(), p.data.size());

    if (!SHA1::equal(actual, meta_.piece_hashes[p.index])) {
        // Hash mismatch — reset the piece for re-download
        p.status       = PieceStatus::HashFail;
        p.blocks_done  = 0;
        std::fill(p.blocks_received.begin(), p.blocks_received.end(), false);
        std::fill(p.data.begin(), p.data.end(), 0);
        return false;
    }

    // Write to disk
    write_piece(p.index, p.data);

    // Free the buffer memory (data no longer needed)
    p.data.clear();
    p.data.shrink_to_fit();
    p.status = PieceStatus::Complete;

    done_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// =============================================================================
// write_piece — write a piece to the correct files (single or multi-file)
// =============================================================================

void PieceManager::write_piece(uint32_t piece_index, const std::vector<uint8_t>& data) {
    // Global offset of this piece in the concatenated byte stream
    int64_t piece_global_begin = static_cast<int64_t>(piece_index) * meta_.piece_length;
    int64_t piece_global_end   = piece_global_begin + static_cast<int64_t>(data.size());

    // Iterate over files that overlap with this piece's range
    for (const FileEntry& file : meta_.files) {
        int64_t file_begin = file.offset;
        int64_t file_end   = file.offset + file.length;

        // No overlap with this piece?
        if (file_end <= piece_global_begin) continue;
        if (file_begin >= piece_global_end) break;  // Files are sorted by offset

        // Overlap interval in global coordinates
        int64_t overlap_begin = std::max(piece_global_begin, file_begin);
        int64_t overlap_end   = std::min(piece_global_end,   file_end);

        // Offset within the piece buffer
        int64_t piece_offset = overlap_begin - piece_global_begin;
        // Offset within the file
        int64_t file_offset  = overlap_begin - file_begin;
        // Number of bytes to write
        int64_t write_len    = overlap_end - overlap_begin;

        // Build the full file path
        std::string path = save_dir_;
        if (meta_.is_single_file()) {
            path += '/' + meta_.name;
        } else {
            path += '/' + file.path_str();
        }

        // Open the file for writing (must already exist after allocate_files)
        int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0)
            throw std::runtime_error("piece_manager: failed to open for writing: " + path);

        // Seek to the correct offset
        if (::lseek(fd, static_cast<off_t>(file_offset), SEEK_SET) < 0) {
            ::close(fd);
            throw std::runtime_error("piece_manager: lseek() failed on: " + path);
        }

        // Write with loop to handle partial writes
        const uint8_t* src     = data.data() + piece_offset;
        size_t         rem     = static_cast<size_t>(write_len);
        while (rem > 0) {
            ssize_t written = ::write(fd, src, rem);
            if (written <= 0) {
                ::close(fd);
                throw std::runtime_error("piece_manager: write() failed on: " + path);
            }
            src += written;
            rem -= static_cast<size_t>(written);
        }

        ::close(fd);
    }
}

// =============================================================================
// allocate_files — create directories and sparse files
// =============================================================================

// Helper: create a directory and all parents (mkdir -p)
static void mkdir_p(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            std::string sub = path.substr(0, i);
            ::mkdir(sub.c_str(), 0755); // Ignore error if already exists
        }
    }
}

void PieceManager::allocate_files() {
    if (meta_.is_single_file()) {
        mkdir_p(save_dir_);

        std::string path = save_dir_ + '/' + meta_.name;
        // Create the file and set its size via lseek + 1-byte write
        int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            throw std::runtime_error("piece_manager: could not create: " + path);

        // Truncate: seek to the last byte and write zero
        if (meta_.total_length > 0) {
            ::lseek(fd, static_cast<off_t>(meta_.total_length - 1), SEEK_SET);
            uint8_t zero = 0;
            [[maybe_unused]] ssize_t ignored = ::write(fd, &zero, 1);
        }
        ::close(fd);

    } else {
        // Multi-file: create directory structure and one file per entry
        for (const FileEntry& file : meta_.files) {
            // Reconstruct the full file path
            std::string dir_path = save_dir_ + '/' + meta_.name;
            std::string file_path = dir_path;

            for (size_t i = 0; i < file.path.size(); ++i) {
                if (i + 1 < file.path.size()) {
                    dir_path += '/' + file.path[i];
                }
                file_path += '/' + file.path[i];
            }

            mkdir_p(dir_path);

            int fd = ::open(file_path.c_str(), O_CREAT | O_WRONLY, 0644);
            if (fd < 0)
                throw std::runtime_error(
                    "piece_manager: could not create: " + file_path);

            if (file.length > 0) {
                ::lseek(fd, static_cast<off_t>(file.length - 1), SEEK_SET);
                uint8_t zero = 0;
                [[maybe_unused]] ssize_t ignored2 = ::write(fd, &zero, 1);
            }
            ::close(fd);
        }
    }
}

// =============================================================================
// next_request — select the next block to request
// =============================================================================

std::optional<BlockRequest> PieceManager::next_request(
    const std::vector<uint8_t>& peer_bitfield) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const Piece& p : pieces_) {
        // Skip completed pieces
        if (p.status == PieceStatus::Complete) continue;

        // Does the peer have this piece?
        if (!peer_has_piece(peer_bitfield, p.index)) continue;

        // Find the first unreceived block
        for (size_t bi = 0; bi < p.blocks_received.size(); ++bi) {
            if (p.blocks_received[bi]) continue;

            uint32_t begin = static_cast<uint32_t>(bi) * BLOCK_SIZE;

            // Block size: may be smaller for the last block of the last piece
            int64_t  piece_sz    = meta_.piece_size(p.index);
            int64_t  remaining   = piece_sz - static_cast<int64_t>(begin);
            uint32_t block_len   = static_cast<uint32_t>(
                std::min(remaining, static_cast<int64_t>(BLOCK_SIZE)));

            return BlockRequest{p.index, begin, block_len};
        }
    }

    return std::nullopt; // Nothing to request
}

// =============================================================================
// our_bitfield
// =============================================================================

std::vector<uint8_t> PieceManager::our_bitfield() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1 bit per piece, rounded up to bytes
    size_t num_bytes = (pieces_.size() + 7) / 8;
    std::vector<uint8_t> bf(num_bytes, 0);

    for (size_t i = 0; i < pieces_.size(); ++i) {
        if (pieces_[i].status == PieceStatus::Complete) {
            // Bit at position i: byte = i/8, bit = 7 - (i%8) (MSB first)
            bf[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
        }
    }

    return bf;
}

// =============================================================================
// Progress
// =============================================================================

int64_t PieceManager::bytes_downloaded() const noexcept {
    return static_cast<int64_t>(done_count_.load()) * meta_.piece_length;
}

float PieceManager::progress_pct() const noexcept {
    if (meta_.total_length == 0) return 100.0f;
    float ratio = static_cast<float>(bytes_downloaded()) /
                  static_cast<float>(meta_.total_length);
    return ratio * 100.0f;
}

// =============================================================================
// peer_has_piece
// =============================================================================

bool PieceManager::peer_has_piece(
    const std::vector<uint8_t>& bitfield, uint32_t index) noexcept
{
    size_t byte_idx = index / 8;
    if (byte_idx >= bitfield.size()) return false;
    return (bitfield[byte_idx] & (0x80 >> (index % 8))) != 0;
}

} // namespace bt
