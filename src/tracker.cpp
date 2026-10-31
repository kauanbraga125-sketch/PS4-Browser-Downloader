// =============================================================================
// tracker.cpp — HTTP tracker client implementation via BSD sockets
// =============================================================================

#include "tracker.hpp"
#include "bencode.hpp"

// BSD networking headers (available on OpenOrbis via musl/libkernel)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace bt {

// =============================================================================
// PeerAddr::ip_str
// =============================================================================

std::string PeerAddr::ip_str() const {
    // ip is in network byte order — inet_ntoa expects this
    struct in_addr addr{};
    addr.s_addr = ip;
    return inet_ntoa(addr);
}

// =============================================================================
// url_encode
// =============================================================================

std::string url_encode(std::string_view raw) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(raw.size() * 3);

    for (unsigned char c : raw) {
        // RFC 3986 unreserved characters pass through unencoded
        bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';

        if (unreserved) {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += HEX[c >> 4];
            result += HEX[c & 0x0F];
        }
    }
    return result;
}

// =============================================================================
// Internal implementation
// =============================================================================

namespace {

// -------------------------------------------------------------------------
// Decomposed URL structure
// -------------------------------------------------------------------------

struct ParsedUrl {
    std::string host;
    uint16_t    port;
    std::string path;  // Includes leading '/'
};

ParsedUrl parse_url(const std::string& url) {
    if (url.size() < 7 || url.substr(0, 7) != "http://")
        throw std::runtime_error(
            "tracker: only HTTP is supported. URL received: " + url);

    std::string_view sv = url;
    sv.remove_prefix(7); // Remove "http://"

    ParsedUrl result;
    auto slash = sv.find('/');

    std::string_view host_port = (slash != std::string_view::npos)
                                     ? sv.substr(0, slash)
                                     : sv;
    result.path = (slash != std::string_view::npos)
                      ? std::string(sv.substr(slash))
                      : "/";

    auto colon = host_port.find(':');
    if (colon != std::string_view::npos) {
        result.host = std::string(host_port.substr(0, colon));
        result.port = static_cast<uint16_t>(
            std::stoul(std::string(host_port.substr(colon + 1))));
    } else {
        result.host = std::string(host_port);
        result.port = 80;
    }

    return result;
}

// -------------------------------------------------------------------------
// Build the announce query string
// -------------------------------------------------------------------------

std::string build_query_string(
    const Metainfo&    meta,
    const std::string& peer_id,
    uint16_t           port,
    int64_t            uploaded,
    int64_t            downloaded,
    int64_t            left,
    const std::string& event)
{
    // info_hash and peer_id are raw bytes — percent-encode everything
    std::string ih_raw(reinterpret_cast<const char*>(meta.info_hash.data()), 20);
    std::string pid_raw(peer_id.data(), std::min(peer_id.size(), size_t(20)));

    std::string qs;
    qs.reserve(256);
    qs += "info_hash=";   qs += url_encode(ih_raw);
    qs += "&peer_id=";    qs += url_encode(pid_raw);
    qs += "&port=";       qs += std::to_string(port);
    qs += "&uploaded=";   qs += std::to_string(uploaded);
    qs += "&downloaded="; qs += std::to_string(downloaded);
    qs += "&left=";       qs += std::to_string(left);
    qs += "&compact=1";   // Request compact format (6 bytes per peer)

    if (!event.empty()) {
        qs += "&event=";
        qs += event;
    }

    return qs;
}

// -------------------------------------------------------------------------
// TCP connect, returns the fd
// -------------------------------------------------------------------------

int tcp_connect(const std::string& host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res)
        throw std::runtime_error("tracker: DNS failure for " + host);

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        throw std::runtime_error("tracker: socket() failed");
    }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd);
        freeaddrinfo(res);
        throw std::runtime_error("tracker: connect() failed for " + host + ":" + std::to_string(port));
    }

    freeaddrinfo(res);
    return fd;
}

// -------------------------------------------------------------------------
// Send all bytes (loop over send())
// -------------------------------------------------------------------------

void send_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t sent = ::send(fd, data, len, 0);
        if (sent <= 0)
            throw std::runtime_error("tracker: send() failed");
        data += sent;
        len  -= static_cast<size_t>(sent);
    }
}

// -------------------------------------------------------------------------
// Perform HTTP GET and return the full response (headers + body)
// -------------------------------------------------------------------------

std::string http_get(const ParsedUrl& url, const std::string& qs) {
    // Build HTTP/1.0 request (no keep-alive, simpler)
    std::string request;
    request.reserve(512);
    request += "GET ";
    request += url.path;
    request += '?';
    request += qs;
    request += " HTTP/1.0\r\n";
    request += "Host: ";
    request += url.host;
    request += "\r\nAccept: */*\r\nConnection: close\r\n\r\n";

    int fd = tcp_connect(url.host, url.port);
    send_all(fd, request.data(), request.size());

    // Read the full response
    std::string response;
    response.reserve(4096);
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    return response;
}

// -------------------------------------------------------------------------
// Extract HTTP body (discard headers)
// -------------------------------------------------------------------------

std::string_view extract_body(std::string_view response) {
    auto sep = response.find("\r\n\r\n");
    if (sep == std::string_view::npos)
        throw std::runtime_error("tracker: invalid HTTP response (no header separator)");
    return response.substr(sep + 4);
}

// -------------------------------------------------------------------------
// Parse compact peer list (BEP 23)
// 6 bytes per peer: 4 bytes IP (big-endian) + 2 bytes port (big-endian)
// -------------------------------------------------------------------------

std::vector<PeerAddr> parse_compact_peers(const std::string& data) {
    if (data.size() % 6 != 0)
        throw std::runtime_error(
            "tracker: compact peer list has invalid size: " +
            std::to_string(data.size()));

    std::vector<PeerAddr> peers;
    peers.reserve(data.size() / 6);

    for (size_t i = 0; i + 6 <= data.size(); i += 6) {
        PeerAddr p;
        // IP already in network byte order — copy directly
        std::memcpy(&p.ip, data.data() + i, 4);
        // Port in big-endian → host byte order
        uint16_t port_be;
        std::memcpy(&port_be, data.data() + i + 4, 2);
        p.port = ntohs(port_be);
        peers.push_back(p);
    }

    return peers;
}

} // anonymous namespace

// =============================================================================
// tracker_announce — public entry point
// =============================================================================

TrackerResponse tracker_announce(
    const Metainfo&    metainfo,
    const std::string& peer_id,
    uint16_t           port,
    int64_t            uploaded,
    int64_t            downloaded,
    int64_t            left,
    const std::string& event)
{
    ParsedUrl   url  = parse_url(metainfo.announce);
    std::string qs   = build_query_string(metainfo, peer_id, port,
                                          uploaded, downloaded, left, event);
    std::string raw  = http_get(url, qs);
    auto        body = extract_body(raw);

    BValue bv = decode(body);
    if (!bv.is_dict())
        throw std::runtime_error("tracker: response is not a bencode dictionary");

    const BDict& d = bv.as_dict();
    TrackerResponse tr{};

    // Check failure reason first
    auto fail_it = d.find("failure reason");
    if (fail_it != d.end() && fail_it->second.is_string()) {
        tr.failure = fail_it->second.as_string();
        return tr;
    }

    // Optional fields with safe default values
    auto get_int = [&](const std::string& key, int32_t def) -> int32_t {
        auto it = d.find(key);
        return (it != d.end() && it->second.is_int())
                   ? static_cast<int32_t>(it->second.as_int())
                   : def;
    };

    tr.interval     = get_int("interval", 1800);
    tr.min_interval = get_int("min interval", 0);
    tr.complete     = get_int("complete", 0);
    tr.incomplete   = get_int("incomplete", 0);

    // Peers — only accept compact format
    auto peers_it = d.find("peers");
    if (peers_it != d.end() && peers_it->second.is_string()) {
        tr.peers = parse_compact_peers(peers_it->second.as_string());
    }

    return tr;
}

} // namespace bt
