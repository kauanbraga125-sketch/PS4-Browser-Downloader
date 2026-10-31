// =============================================================================
// bencode.hpp — Bencode format decoder and encoder (BEP 3)
//
// Bencode is the serialization format used in .torrent files and the
// tracker protocol. Supports four types:
//   - Integer:    i<number>e        → e.g. i42e
//   - String:     <len>:<bytes>     → e.g. 4:spam
//   - List:       l<items>e         → e.g. li1e4:spame
//   - Dictionary: d<key-value...>e  → e.g. d3:fooi1ee  (keys are always strings)
// =============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bt {

// Forward declaration for the recursive type
struct BValue;

using BInt    = int64_t;
using BString = std::string;
using BList   = std::vector<BValue>;
using BDict   = std::map<BString, BValue>;   // Ordered map — bencode requires sorted keys

/**
 * Tagged union representing any bencode value.
 *
 * Uses std::variant for type-safe access with no extra allocation.
 * Create: BValue v = BString("hello");
 * Read:   v.as_string()  or  std::get<BString>(v.data)
 */
struct BValue {
    std::variant<BInt, BString, BList, BDict> data;

    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------
    BValue() = default;
    explicit BValue(BInt v)    : data(v)             {}
    explicit BValue(BString v) : data(std::move(v))  {}
    explicit BValue(BList v)   : data(std::move(v))  {}
    explicit BValue(BDict v)   : data(std::move(v))  {}

    // Convenience for integer literals
    explicit BValue(int v)      : data(static_cast<BInt>(v)) {}

    // -------------------------------------------------------------------------
    // Type predicates
    // -------------------------------------------------------------------------
    bool is_int()    const noexcept { return std::holds_alternative<BInt>(data);    }
    bool is_string() const noexcept { return std::holds_alternative<BString>(data); }
    bool is_list()   const noexcept { return std::holds_alternative<BList>(data);   }
    bool is_dict()   const noexcept { return std::holds_alternative<BDict>(data);   }

    // -------------------------------------------------------------------------
    // Accessors — throw std::bad_variant_access on type mismatch
    // -------------------------------------------------------------------------
    BInt&          as_int()    { return std::get<BInt>(data);    }
    BString&       as_string() { return std::get<BString>(data); }
    BList&         as_list()   { return std::get<BList>(data);   }
    BDict&         as_dict()   { return std::get<BDict>(data);   }

    const BInt&    as_int()    const { return std::get<BInt>(data);    }
    const BString& as_string() const { return std::get<BString>(data); }
    const BList&   as_list()   const { return std::get<BList>(data);   }
    const BDict&   as_dict()   const { return std::get<BDict>(data);   }
};

// =============================================================================
// Public API
// =============================================================================

/**
 * Decode a bencoded buffer and return the root BValue.
 *
 * @param data   Input buffer (may contain null bytes — uses string_view)
 * @throws std::runtime_error   on malformed input
 */
BValue decode(std::string_view data);

/**
 * Encode a BValue to bencode bytes.
 * Dictionaries are emitted with lexicographically ordered keys (guaranteed by std::map).
 */
std::string encode(const BValue& val);

/**
 * Encode variant that writes into an existing buffer (avoids extra allocation).
 */
void encode_into(const BValue& val, std::string& out);

} // namespace bt
