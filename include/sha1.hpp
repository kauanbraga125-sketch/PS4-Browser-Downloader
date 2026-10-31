// =============================================================================
// sha1.hpp — SHA1 header-only, based on RFC 3174
//
// Zero external dependencies. Works with the OpenOrbis musl libc.
// Usage:
//   auto digest = bt::SHA1::hash(data, len);
//   // or incremental:
//   bt::SHA1 ctx;
//   ctx.update(ptr, size);
//   auto d = ctx.finalize();
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace bt {

class SHA1 {
public:
    // SHA1 produces 160 bits = 20 bytes
    static constexpr size_t DIGEST_SIZE = 20;
    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    SHA1() noexcept { reset(); }

    // -------------------------------------------------------------------------
    // Reset context for reuse
    // -------------------------------------------------------------------------
    void reset() noexcept {
        count_    = 0;
        buf_used_ = 0;
        // Initial values defined by the SHA1 standard
        state_[0] = 0x67452301u;
        state_[1] = 0xEFCDAB89u;
        state_[2] = 0x98BADCFEu;
        state_[3] = 0x10325476u;
        state_[4] = 0xC3D2E1F0u;
    }

    // -------------------------------------------------------------------------
    // Feed bytes into the context (may be called multiple times)
    // -------------------------------------------------------------------------
    void update(const void* data, size_t len) noexcept {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        count_ += len;

        // Complete a partial block if there are bytes in the internal buffer
        if (buf_used_ > 0) {
            size_t space = BLOCK_SIZE - buf_used_;
            size_t fill  = (len < space) ? len : space;
            std::memcpy(buf_ + buf_used_, ptr, fill);
            buf_used_ += fill;
            ptr        += fill;
            len        -= fill;

            if (buf_used_ == BLOCK_SIZE) {
                compress(buf_);
                buf_used_ = 0;
            }
        }

        // Process full blocks directly from input (zero-copy)
        while (len >= BLOCK_SIZE) {
            compress(ptr);
            ptr += BLOCK_SIZE;
            len -= BLOCK_SIZE;
        }

        // Save remaining bytes in the buffer
        if (len > 0) {
            std::memcpy(buf_, ptr, len);
            buf_used_ = len;
        }
    }

    // -------------------------------------------------------------------------
    // Finalize and return the digest. Do NOT reuse the context after this call
    // without calling reset() first.
    // -------------------------------------------------------------------------
    Digest finalize() noexcept {
        // Padding: bit 1 followed by zeros up to 56 mod 64 bytes, then bit length
        uint64_t bit_count_be = to_big_endian_64(count_ * 8);

        uint8_t pad = 0x80;
        update(&pad, 1);

        // Pad until 8 bytes remain in the block (for the bit count)
        uint8_t zero = 0;
        while (buf_used_ != 56) update(&zero, 1);

        // Append bit count (big-endian, 8 bytes)
        update(&bit_count_be, 8);

        // Serialize state in big-endian
        Digest d;
        for (int i = 0; i < 5; ++i) {
            d[i * 4 + 0] = (state_[i] >> 24) & 0xFF;
            d[i * 4 + 1] = (state_[i] >> 16) & 0xFF;
            d[i * 4 + 2] = (state_[i] >>  8) & 0xFF;
            d[i * 4 + 3] = (state_[i] >>  0) & 0xFF;
        }
        return d;
    }

    // -------------------------------------------------------------------------
    // One-shot: hash in-memory data
    // -------------------------------------------------------------------------
    static Digest hash(const void* data, size_t len) noexcept {
        SHA1 ctx;
        ctx.update(data, len);
        return ctx.finalize();
    }

    static Digest hash(const std::string& s) noexcept {
        return hash(s.data(), s.size());
    }

    // Constant-time digest comparison (prevents timing attacks)
    static bool equal(const Digest& a, const Digest& b) noexcept {
        uint8_t diff = 0;
        for (size_t i = 0; i < DIGEST_SIZE; ++i) diff |= a[i] ^ b[i];
        return diff == 0;
    }

private:
    static constexpr size_t BLOCK_SIZE = 64;

    uint32_t state_[5];
    uint64_t count_;        // Total bytes processed
    uint8_t  buf_[BLOCK_SIZE];
    size_t   buf_used_;

    // 32-bit left circular rotation
    static uint32_t rotl(uint32_t v, int n) noexcept {
        return (v << n) | (v >> (32 - n));
    }

    static uint64_t to_big_endian_64(uint64_t v) noexcept {
        // Byte swap if needed (PS4 is little-endian x86_64)
        return ((v & 0x00000000000000FFull) << 56) |
               ((v & 0x000000000000FF00ull) << 40) |
               ((v & 0x0000000000FF0000ull) << 24) |
               ((v & 0x00000000FF000000ull) <<  8) |
               ((v & 0x000000FF00000000ull) >>  8) |
               ((v & 0x0000FF0000000000ull) >> 24) |
               ((v & 0x00FF000000000000ull) >> 40) |
               ((v & 0xFF00000000000000ull) >> 56);
    }

    // -------------------------------------------------------------------------
    // SHA1 block compression (64 bytes)
    // -------------------------------------------------------------------------
    void compress(const uint8_t* block) noexcept {
        // Expand the 16 block words to 80
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) <<  8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]) <<  0);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2],
                 d = state_[3], e = state_[4];

        // Four rounds of 20 operations each
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }

            uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = tmp;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }
};

} // namespace bt
