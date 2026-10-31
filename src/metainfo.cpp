// =============================================================================
// metainfo.cpp — .torrent file parser implementation
// =============================================================================

#include "metainfo.hpp"

#include <cstring>
#include <stdexcept>

namespace bt {

// =============================================================================
// FileEntry::path_str
// =============================================================================

std::string FileEntry::path_str() const {
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0) result += '/';
        result += path[i];
    }
    return result;
}

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/**
 * Locate the offset and size of the info dict within the raw bencoded buffer.
 *
 * The info_hash is the SHA1 of the raw info dict bytes (not the decoded value),
 * so we need to find the exact position in the original buffer.
 *
 * Strategy: search for the "4:info" key in the root dict, then scan forward
 * one full bencode value to determine the end of the info dict.
 */
std::string_view find_raw_info(std::string_view raw) {
    // Search for the "4:info" key in the buffer.
    // Note: false positives are possible, but in a well-formed .torrent file
    // the "4:info" key appears only once at the root level.
    const std::string KEY = "4:info";
    auto key_pos = raw.find(KEY);
    if (key_pos == std::string_view::npos)
        throw std::runtime_error("metainfo: 'info' key not found");

    size_t value_start = key_pos + KEY.size();
    if (value_start >= raw.size())
        throw std::runtime_error("metainfo: truncated info dict");

    // Advance one full bencode value from value_start
    // using a mini-scanner that tracks d...e / l...e nesting depth
    size_t pos   = value_start;
    int    depth = 0;

    while (pos < raw.size()) {
        char c = raw[pos];

        if (c == 'i') {
            // Integer: i<num>e
            ++pos;
            while (pos < raw.size() && raw[pos] != 'e') ++pos;
            ++pos; // consume 'e'
            if (depth == 0) break;

        } else if (c == 'l' || c == 'd') {
            ++pos;
            ++depth;

        } else if (c == 'e') {
            ++pos;
            --depth;
            if (depth <= 0) break;

        } else if (c >= '0' && c <= '9') {
            // String: <len>:<bytes>
            size_t colon = raw.find(':', pos);
            if (colon == std::string_view::npos)
                throw std::runtime_error("metainfo: malformed string in info dict");

            size_t len = 0;
            for (size_t i = pos; i < colon; ++i) {
                len = len * 10 + (raw[i] - '0');
            }
            pos = colon + 1 + len;
            if (depth == 0) break;

        } else {
            throw std::runtime_error(
                std::string("metainfo: unexpected character '") + c + "' while scanning info dict");
        }
    }

    return raw.substr(value_start, pos - value_start);
}

// -------------------------------------------------------------------------
// Safe accessors for bencode dictionaries
// -------------------------------------------------------------------------

const BString& require_string(const BDict& d, const std::string& key) {
    auto it = d.find(key);
    if (it == d.end())
        throw std::runtime_error("metainfo: required field missing: '" + key + "'");
    if (!it->second.is_string())
        throw std::runtime_error("metainfo: field '" + key + "' is not a string");
    return it->second.as_string();
}

int64_t require_int(const BDict& d, const std::string& key) {
    auto it = d.find(key);
    if (it == d.end())
        throw std::runtime_error("metainfo: required field missing: '" + key + "'");
    if (!it->second.is_int())
        throw std::runtime_error("metainfo: field '" + key + "' is not an integer");
    return it->second.as_int();
}

// -------------------------------------------------------------------------
// Parse piece hashes
// -------------------------------------------------------------------------

std::vector<SHA1::Digest> parse_piece_hashes(const std::string& pieces_raw) {
    if (pieces_raw.size() % 20 != 0)
        throw std::runtime_error(
            "metainfo: 'pieces' field has invalid length: " +
            std::to_string(pieces_raw.size()) + " (not a multiple of 20)");

    size_t count = pieces_raw.size() / 20;
    std::vector<SHA1::Digest> hashes(count);

    for (size_t i = 0; i < count; ++i) {
        std::memcpy(hashes[i].data(), pieces_raw.data() + i * 20, 20);
    }

    return hashes;
}

// -------------------------------------------------------------------------
// Parse file list (multi-file mode)
// -------------------------------------------------------------------------

std::vector<FileEntry> parse_files(const BList& files_list) {
    std::vector<FileEntry> result;
    result.reserve(files_list.size());
    int64_t offset = 0;

    for (const BValue& file_bv : files_list) {
        if (!file_bv.is_dict())
            throw std::runtime_error("metainfo: file entry is not a dict");

        const BDict& fd = file_bv.as_dict();
        FileEntry f;

        f.length = require_int(fd, "length");
        f.offset = offset;
        offset  += f.length;

        // path is a list of strings (path components)
        auto path_it = fd.find("path");
        if (path_it == fd.end() || !path_it->second.is_list())
            throw std::runtime_error("metainfo: file missing 'path'");

        for (const BValue& comp : path_it->second.as_list()) {
            if (!comp.is_string())
                throw std::runtime_error("metainfo: path component is not a string");
            f.path.push_back(comp.as_string());
        }

        result.push_back(std::move(f));
    }

    return result;
}

} // anonymous namespace

// =============================================================================
// Metainfo::parse — public entry point
// =============================================================================

Metainfo Metainfo::parse(const std::string& raw) {
    BValue root = decode(raw);
    if (!root.is_dict())
        throw std::runtime_error("metainfo: root is not a dictionary");

    const BDict& top = root.as_dict();
    Metainfo m;

    // -------------------------------------------------------------------------
    // Root-level fields
    // -------------------------------------------------------------------------
    m.announce = require_string(top, "announce");

    // announce-list (BEP 12) — optional
    auto al_it = top.find("announce-list");
    if (al_it != top.end() && al_it->second.is_list()) {
        for (const BValue& tier_bv : al_it->second.as_list()) {
            if (!tier_bv.is_list()) continue;
            std::vector<std::string> tier;
            for (const BValue& url_bv : tier_bv.as_list()) {
                if (url_bv.is_string()) tier.push_back(url_bv.as_string());
            }
            if (!tier.empty()) m.announce_list.push_back(std::move(tier));
        }
    }

    // -------------------------------------------------------------------------
    // info_hash: SHA1 of the raw info dict bytes
    // -------------------------------------------------------------------------
    std::string_view raw_info = find_raw_info(raw);
    m.info_hash = SHA1::hash(raw_info.data(), raw_info.size());

    // -------------------------------------------------------------------------
    // Parse the info dict
    // -------------------------------------------------------------------------
    auto info_it = top.find("info");
    if (info_it == top.end() || !info_it->second.is_dict())
        throw std::runtime_error("metainfo: 'info' missing or invalid");

    const BDict& info = info_it->second.as_dict();

    m.name         = require_string(info, "name");
    m.piece_length = require_int(info, "piece length");

    m.piece_hashes = parse_piece_hashes(require_string(info, "pieces"));

    // -------------------------------------------------------------------------
    // Single-file vs multi-file
    // -------------------------------------------------------------------------
    auto length_it = info.find("length");
    auto files_it  = info.find("files");

    if (length_it != info.end() && length_it->second.is_int()) {
        // Single-file mode: one file with empty path (use `name` as filename)
        FileEntry f;
        f.length = length_it->second.as_int();
        f.offset = 0;
        // Empty path — signals single-file to consuming code
        m.total_length = f.length;
        m.files.push_back(std::move(f));

    } else if (files_it != info.end() && files_it->second.is_list()) {
        // Multi-file mode
        m.files = parse_files(files_it->second.as_list());
        m.total_length = 0;
        for (const FileEntry& f : m.files) m.total_length += f.length;

    } else {
        throw std::runtime_error(
            "metainfo: info dict has neither 'length' (single-file) nor 'files' (multi-file)");
    }

    // Sanity check: piece count must match total size
    size_t expected_pieces =
        static_cast<size_t>((m.total_length + m.piece_length - 1) / m.piece_length);
    if (m.piece_hashes.size() != expected_pieces) {
        throw std::runtime_error(
            "metainfo: inconsistent piece count: expected " +
            std::to_string(expected_pieces) + ", got " +
            std::to_string(m.piece_hashes.size()));
    }

    return m;
}

} // namespace bt
